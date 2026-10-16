//! MSC4108: signing another device in from this one.
//!
//! matrix-sdk 0.18 supports this device being the new one or the signed-in
//! one, scanning or showing. Lightning implements the signed-in side of
//! both: showing a QR for a new device to scan, or reading the QR a new
//! device shows. Signing this device in by QR needs the OAuth device-code
//! grant, which `oauth.rs` deliberately does not request (a test asserts
//! it); `login_with_qr_code` takes its own `ClientRegistrationData`, so it
//! could be added without changing that.
//!
//! No camera decoder is bundled, so the scanning leg takes the QR's base64
//! text, which displaying clients also offer.
//!
//! The progress stream must be consumed: the check code and verification
//! URL arrive only through it, and the flow waits on the user acting on
//! them. Tasks always run the stream consumer alongside the future.

use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc, Mutex,
};
use std::time::Duration;

use futures_util::StreamExt;
use matrix_sdk::authentication::oauth::qrcode::{
    GeneratedQrProgress, GrantLoginProgress, QRCodeGrantLoginError, QrCodeData, QrProgress,
    SecureChannelError,
};
use matrix_sdk::ruma::api::error::ErrorKind;
use serde_json::json;

use crate::rooms::require_client;
use crate::{enqueue, RustClient};

/// How long the homeserver gets to create the new device after consent.
/// The SDK default (10 s) is too short over slow links, and giving up forces
/// the user to start over.
const DEVICE_CREATION_TIMEOUT_SECS: u64 = 30;

/// Per-flow state. Holds the one-shot `CheckCodeSender` the SDK hands over
/// through the stream, so a later FFI call can answer the generate-side
/// flow.
pub(crate) struct QrLoginState {
    /// Bumped for every started flow; answers or cancels naming an old
    /// generation are ignored.
    generation: AtomicU64,
    /// The one-shot sender, paired with its generation. `abort()` only requests
    /// cancellation, so an old pump already handling `QrScanned` can refill this
    /// slot after `cancel()` cleared it; the pairing stops `submit_check_code`
    /// from sending the digits to a dead flow.
    sender: Mutex<Option<(u64, matrix_sdk::authentication::oauth::qrcode::CheckCodeSender)>>,
    cancel: Mutex<Option<tokio::task::JoinHandle<()>>>,
}

impl QrLoginState {
    pub(crate) fn new() -> Self {
        Self {
            generation: AtomicU64::new(0),
            sender: Mutex::new(None),
            cancel: Mutex::new(None),
        }
    }
}

impl Default for QrLoginState {
    fn default() -> Self {
        Self::new()
    }
}

/// Coarse failure category for the UI. Never the error text, which quotes
/// channel state and URLs.
fn classify(err: &str) -> &'static str {
    let lower = err.to_ascii_lowercase();
    // First: `QRCodeGrantLoginError::MissingSecretsBackup`, hit by accounts
    // without cross-signing or key backup (there are no secrets to send). Its
    // fix is one button away, so it gets its own category.
    if lower.contains("secrets backup") || lower.contains("secret backup") {
        "no_secrets"
    } else if lower.contains("checkcode") || lower.contains("check code") {
        "check_code"
    } else if lower.contains("cancel") {
        "cancelled"
    } else if lower.contains("unsupported") || lower.contains("not supported")
        || lower.contains("endpoint") || lower.contains("unrecognized")
    {
        // Before "expired": a server that does not know the endpoint can say
        // "not found" too, and nothing expired.
        "unsupported"
    } else if lower.contains("expired") || lower.contains("timeout")
        || lower.contains("timed out") || lower.contains("not found")
    {
        "expired"
    } else {
        "failed"
    }
}

/// The generate flow's first request creates the rendezvous session on our
/// own homeserver. A server without MSC4108 refuses it: M_UNRECOGNIZED, or a
/// 404 / M_NOT_FOUND, which the SDK turns into `NotFound`, "the rendezvous
/// session was not found and might have expired". Before the QR code exists
/// there is no session to have expired, so that is "unsupported" (B083).
fn rendezvous_refused(err: &QRCodeGrantLoginError, qr_shown: bool) -> bool {
    match err {
        QRCodeGrantLoginError::NotFound => !qr_shown,
        QRCodeGrantLoginError::SecureChannel(SecureChannelError::RendezvousChannel(http)) => {
            let Some(api) = http.as_client_api_error() else { return false };
            matches!(api.error_kind(), Some(ErrorKind::Unrecognized))
                || (!qr_shown && matches!(api.status_code.as_u16(), 404 | 405))
        }
        _ => false,
    }
}

/// `classify` for the flow that shows the QR code.
fn classify_generate(err: &QRCodeGrantLoginError, qr_shown: bool) -> &'static str {
    if rendezvous_refused(err, qr_shown) {
        "unsupported"
    } else {
        classify(&err.to_string())
    }
}

fn emit(bridge: &Arc<Mutex<std::collections::VecDeque<String>>>, gen: u64, value: serde_json::Value) {
    let mut v = value;
    v["type"] = json!("qr_login_progress");
    v["generation"] = json!(gen);
    enqueue(bridge, v);
}

/// Start the flow where this device displays a QR code. The new device
/// scans it and shows two digits, which `submit_check_code` answers with.
pub(crate) fn grant_generate(bridge: &RustClient) -> Result<u64, String> {
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let state = Arc::clone(&bridge.qr_login);
    let gen = state.generation.fetch_add(1, Ordering::SeqCst) + 1;
    // A previous flow's sender must not answer this one.
    if let Ok(mut guard) = state.sender.lock() {
        *guard = None;
    }

    let task = bridge.runtime.spawn(async move {
        let oauth = client.oauth();
        let grant = oauth
            .grant_login_with_qr_code()
            .device_creation_timeout(Duration::from_secs(DEVICE_CREATION_TIMEOUT_SECS))
            .generate();
        let mut progress = grant.subscribe_to_progress();
        // Whether the rendezvous session existed (its QR code was shown): only
        // then can a "not found" mean it expired.
        let qr_shown = Arc::new(AtomicBool::new(false));

        // Consume the stream concurrently with the future: the QR payload and the
        // check-code sender arrive through it, and the future waits on both.
        let stream_events = Arc::clone(&events);
        let stream_state = Arc::clone(&state);
        let stream_qr_shown = Arc::clone(&qr_shown);
        let pump = tokio::spawn(async move {
            while let Some(step) = progress.next().await {
                match step {
                    GrantLoginProgress::Starting => {
                        emit(&stream_events, gen, json!({ "step": "starting" }));
                    }
                    GrantLoginProgress::EstablishingSecureChannel(
                        GeneratedQrProgress::QrReady(data),
                    ) => {
                        stream_qr_shown.store(true, Ordering::SeqCst);
                        // Rendered to modules here: the encoder is already on this side
                        // (verification), and C++ has none.
                        match crate::render_qr_bytes(&data.to_bytes()) {
                            Some((size, bits)) => emit(
                                &stream_events,
                                gen,
                                json!({
                                    "step": "qr_ready",
                                    "qr_size": size,
                                    "qr_bits": bits,
                                    // The payload as text too, for devices that can only paste.
                                    "qr_text": data.to_base64(),
                                }),
                            ),
                            None => emit(
                                &stream_events,
                                gen,
                                json!({ "step": "failed", "category": "failed" }),
                            ),
                        }
                    }
                    GrantLoginProgress::EstablishingSecureChannel(
                        GeneratedQrProgress::QrScanned(sender),
                    ) => {
                        // Paired with this flow's generation; refused if already superseded.
                        if let Ok(mut guard) = stream_state.sender.lock() {
                            if stream_state.generation.load(Ordering::SeqCst) == gen {
                                *guard = Some((gen, sender));
                            }
                        }
                        emit(&stream_events, gen, json!({ "step": "check_code_needed" }));
                    }
                    GrantLoginProgress::WaitingForAuth { verification_uri } => {
                        // An https URL from the user's own homeserver's authorization server; C++
                        // opens it via UrlLauncher, which allows only http/https.
                        emit(
                            &stream_events,
                            gen,
                            json!({
                                "step": "waiting_for_auth",
                                "verification_uri": verification_uri.to_string(),
                            }),
                        );
                    }
                    GrantLoginProgress::SyncingSecrets => {
                        emit(&stream_events, gen, json!({ "step": "syncing_secrets" }));
                    }
                    GrantLoginProgress::Done => break,
                }
            }
        });

        let outcome = grant.await;
        pump.abort();
        if let Ok(mut guard) = state.sender.lock() {
            *guard = None;
        }
        match outcome {
            Ok(()) => emit(&events, gen, json!({ "step": "done" })),
            // Only the category crosses (the text quotes channel state and URLs); C++
            // logs it in QrLoginController::fail.
            Err(err) => emit(
                &events,
                gen,
                json!({
                    "step": "failed",
                    "category": classify_generate(&err, qr_shown.load(Ordering::SeqCst)),
                }),
            ),
        }
    });
    if let Ok(mut guard) = bridge.qr_login.cancel.lock() {
        if let Some(old) = guard.replace(task) {
            old.abort();
        }
    }
    Ok(gen)
}

/// Start the flow where this device reads the QR (as text) the new device
/// shows. The check code is shown here, and the user types it on the new
/// device.
pub(crate) fn grant_scan(bridge: &RustClient, payload: String) -> Result<u64, String> {
    let data = QrCodeData::from_base64(payload.trim())
        .map_err(|_| "that does not look like a sign-in code".to_owned())?;
    let client = require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let state = Arc::clone(&bridge.qr_login);
    let gen = state.generation.fetch_add(1, Ordering::SeqCst) + 1;
    if let Ok(mut guard) = state.sender.lock() {
        *guard = None;
    }

    let task = bridge.runtime.spawn(async move {
        let oauth = client.oauth();
        let grant = oauth
            .grant_login_with_qr_code()
            .device_creation_timeout(Duration::from_secs(DEVICE_CREATION_TIMEOUT_SECS))
            .scan(&data);
        let mut progress = grant.subscribe_to_progress();

        let stream_events = Arc::clone(&events);
        let pump = tokio::spawn(async move {
            while let Some(step) = progress.next().await {
                match step {
                    GrantLoginProgress::Starting => {
                        emit(&stream_events, gen, json!({ "step": "starting" }));
                    }
                    GrantLoginProgress::EstablishingSecureChannel(QrProgress {
                        check_code,
                    }) => {
                        emit(
                            &stream_events,
                            gen,
                            json!({
                                "step": "check_code_shown",
                                // Two digits: not secret, short-lived, specific to this channel.
                                "check_code": check_code.to_digit(),
                            }),
                        );
                    }
                    GrantLoginProgress::WaitingForAuth { verification_uri } => {
                        emit(
                            &stream_events,
                            gen,
                            json!({
                                "step": "waiting_for_auth",
                                "verification_uri": verification_uri.to_string(),
                            }),
                        );
                    }
                    GrantLoginProgress::SyncingSecrets => {
                        emit(&stream_events, gen, json!({ "step": "syncing_secrets" }));
                    }
                    GrantLoginProgress::Done => break,
                }
            }
        });

        let outcome = grant.await;
        pump.abort();
        match outcome {
            Ok(()) => emit(&events, gen, json!({ "step": "done" })),
            Err(err) => emit(
                &events,
                gen,
                json!({ "step": "failed", "category": classify(&err.to_string()) }),
            ),
        }
    });
    if let Ok(mut guard) = bridge.qr_login.cancel.lock() {
        if let Some(old) = guard.replace(task) {
            old.abort();
        }
    }
    Ok(gen)
}

/// Answer the generate-side flow with the two digits the new device showed.
/// The sender is one-shot, so it is taken from the slot.
pub(crate) fn submit_check_code(
    bridge: &RustClient,
    generation: u64,
    code: u8,
) -> Result<(), String> {
    let state = Arc::clone(&bridge.qr_login);
    if state.generation.load(Ordering::SeqCst) != generation {
        return Err("that sign-in is no longer running".to_owned());
    }
    // Taken under the same lock as the generation check, so a pump cannot slip
    // an old sender in between.
    let sender = state
        .sender
        .lock()
        .ok()
        .and_then(|mut guard| match guard.as_ref() {
            Some((slot_gen, _)) if *slot_gen == generation => {
                guard.take().map(|(_, sender)| sender)
            }
            _ => None,
        })
        .ok_or_else(|| "no code is being waited for".to_owned())?;
    // `send` is async and one-shot; spawned because this runs on the GUI thread.
    let events = Arc::clone(&bridge.events);
    bridge.runtime.spawn(async move {
        if sender.send(code).await.is_err() {
            emit(
                &events,
                generation,
                json!({ "step": "failed", "category": "check_code" }),
            );
        }
    });
    Ok(())
}

/// Abandon whatever is running; safe when nothing is.
pub(crate) fn cancel(bridge: &RustClient) {
    // Bump first, so a late answer for this flow is rejected by generation.
    bridge.qr_login.generation.fetch_add(1, Ordering::SeqCst);
    if let Ok(mut guard) = bridge.qr_login.sender.lock() {
        *guard = None;
    }
    if let Ok(mut guard) = bridge.qr_login.cancel.lock() {
        if let Some(task) = guard.take() {
            task.abort();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Distinct failures must stay distinct; the exact strings do not matter.
    #[test]
    fn failures_are_classified_into_things_a_message_can_be_written_from() {
        assert_eq!(classify("the secure channel expired"), "expired");
        assert_eq!(classify("Request timed out"), "expired");
        assert_eq!(classify("the user cancelled the login"), "cancelled");
        assert_eq!(classify("CheckCode mismatch"), "check_code");
        assert_eq!(
            classify("NoDeviceAuthorizationEndpoint"),
            "unsupported"
        );
        // An account with no cross-signing or key backup has no secrets to send.
        assert_eq!(classify("Secrets backup not set up"), "no_secrets");
        // It must win over the others: the SDK's message may also mention a
        // rendezvous session.
        assert_eq!(
            classify("Secrets backup not set up: session not found"),
            "no_secrets"
        );
        // A missing rendezvous session is an expiry.
        assert_eq!(
            classify("The rendezvous session was not found and might have expired"),
            "expired"
        );
        // Unrecognised errors stay generic; a wrong specific reason misleads.
        assert_eq!(classify("something nobody has seen before"), "failed");
        // A server that does not know the endpoint is not an expiry, even
        // when its words say "not found".
        assert_eq!(
            classify("Error in the rendezvous channel: M_UNRECOGNIZED: Not found"),
            "unsupported"
        );
    }

    fn rendezvous_error(status: u16, kind: ErrorKind) -> QRCodeGrantLoginError {
        use matrix_sdk::ruma::api::error::{
            Error as ApiError, ErrorBody, FromHttpResponseError, StandardErrorBody,
        };
        let api = ApiError::new(
            matrix_sdk::reqwest::StatusCode::from_u16(status).unwrap(),
            ErrorBody::Standard(StandardErrorBody::new(kind, "Not found".to_owned())),
        );
        let http = matrix_sdk::HttpError::from(FromHttpResponseError::Server(
            matrix_sdk::RumaApiError::MatrixError(api),
        ));
        QRCodeGrantLoginError::SecureChannel(SecureChannelError::RendezvousChannel(http))
    }

    // B083: on a server without MSC4108 the generate flow failed at once with
    // "the code expired". Before the QR code exists, a refused rendezvous is
    // "unsupported"; once it was shown, a missing session did expire.
    #[test]
    fn a_server_without_the_rendezvous_endpoint_is_unsupported_not_expired() {
        // The SDK's own NotFound, from a 404 / M_NOT_FOUND on creation.
        assert_eq!(classify_generate(&QRCodeGrantLoginError::NotFound, false), "unsupported");
        assert_eq!(classify_generate(&QRCodeGrantLoginError::NotFound, true), "expired");
        // M_UNRECOGNIZED: the server does not know the endpoint at any stage.
        let unrecognized = rendezvous_error(404, ErrorKind::Unrecognized);
        assert_eq!(classify_generate(&unrecognized, false), "unsupported");
        assert_eq!(classify_generate(&unrecognized, true), "unsupported");
        // A bare 404/405 on creation, whatever the errcode.
        assert_eq!(
            classify_generate(&rendezvous_error(405, ErrorKind::Unknown), false),
            "unsupported"
        );
        // Other failures keep their categories.
        assert_eq!(
            classify_generate(&QRCodeGrantLoginError::InvalidCheckCode, false),
            "check_code"
        );
        assert_eq!(
            classify_generate(&QRCodeGrantLoginError::MissingSecretsBackup(None), false),
            "no_secrets"
        );
    }

    // A payload that is not a QR code is refused before any task starts.
    #[test]
    fn a_payload_that_is_not_a_sign_in_code_is_refused_up_front() {
        assert!(QrCodeData::from_base64("not a qr code at all").is_err());
        assert!(QrCodeData::from_base64("").is_err());
    }
}
