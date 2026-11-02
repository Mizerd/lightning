
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

const DEVICE_CREATION_TIMEOUT_SECS: u64 = 30;

pub(crate) struct QrLoginState {
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

fn classify(err: &str) -> &'static str {
    let lower = err.to_ascii_lowercase();
    if lower.contains("secrets backup") || lower.contains("secret backup") {
        "no_secrets"
    } else if lower.contains("checkcode") || lower.contains("check code") {
        "check_code"
    } else if lower.contains("cancel") {
        "cancelled"
    } else if lower.contains("unsupported") || lower.contains("not supported")
        || lower.contains("endpoint") || lower.contains("unrecognized")
    {
        "unsupported"
    } else if lower.contains("expired") || lower.contains("timeout")
        || lower.contains("timed out") || lower.contains("not found")
    {
        "expired"
    } else {
        "failed"
    }
}

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

pub(crate) fn grant_generate(bridge: &RustClient) -> Result<u64, String> {
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
            .generate();
        let mut progress = grant.subscribe_to_progress();
        let qr_shown = Arc::new(AtomicBool::new(false));

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
                        match crate::render_qr_bytes(&data.to_bytes()) {
                            Some((size, bits)) => emit(
                                &stream_events,
                                gen,
                                json!({
                                    "step": "qr_ready",
                                    "qr_size": size,
                                    "qr_bits": bits,
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

pub(crate) fn submit_check_code(
    bridge: &RustClient,
    generation: u64,
    code: u8,
) -> Result<(), String> {
    let state = Arc::clone(&bridge.qr_login);
    if state.generation.load(Ordering::SeqCst) != generation {
        return Err("that sign-in is no longer running".to_owned());
    }
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

pub(crate) fn cancel(bridge: &RustClient) {
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
        assert_eq!(classify("Secrets backup not set up"), "no_secrets");
        assert_eq!(
            classify("Secrets backup not set up: session not found"),
            "no_secrets"
        );
        assert_eq!(
            classify("The rendezvous session was not found and might have expired"),
            "expired"
        );
        assert_eq!(classify("something nobody has seen before"), "failed");
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

    #[test]
    fn a_server_without_the_rendezvous_endpoint_is_unsupported_not_expired() {
        assert_eq!(classify_generate(&QRCodeGrantLoginError::NotFound, false), "unsupported");
        assert_eq!(classify_generate(&QRCodeGrantLoginError::NotFound, true), "expired");
        let unrecognized = rendezvous_error(404, ErrorKind::Unrecognized);
        assert_eq!(classify_generate(&unrecognized, false), "unsupported");
        assert_eq!(classify_generate(&unrecognized, true), "unsupported");
        assert_eq!(
            classify_generate(&rendezvous_error(405, ErrorKind::Unknown), false),
            "unsupported"
        );
        assert_eq!(
            classify_generate(&QRCodeGrantLoginError::InvalidCheckCode, false),
            "check_code"
        );
        assert_eq!(
            classify_generate(&QRCodeGrantLoginError::MissingSecretsBackup(None), false),
            "no_secrets"
        );
    }

    #[test]
    fn a_payload_that_is_not_a_sign_in_code_is_refused_up_front() {
        assert!(QrCodeData::from_base64("not a qr code at all").is_err());
        assert!(QrCodeData::from_base64("").is_err());
    }
}
