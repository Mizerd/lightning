//! MSC4108 — signing ANOTHER device in from this one.
//!
//! # Which direction this is, and why only this one
//!
//! matrix-sdk 0.18 supports four combinations: this device can be the new one
//! (scanning or showing), or the already-signed-in one (scanning or showing).
//! Lightning implements the SIGNED-IN side of both — we show a QR that a new
//! device scans, or we scan the QR a new device shows.
//!
//! The other direction, signing THIS device in from a QR, is deliberately not
//! here. It requires the OAuth device-code grant in the client metadata, and
//! `rust/src/oauth.rs` does not request it — with a test asserting so and a
//! comment saying the omission is on purpose. Reversing a recorded security
//! decision is not a mechanical edit. When it is taken, the route is clean:
//! `login_with_qr_code` accepts its OWN `ClientRegistrationData`, so a
//! separate metadata can request device_code while the ordinary
//! authorization-code flow keeps not to, and that test stays true.
//!
//! # There is no camera here
//!
//! Lightning bundles no camera-frame decoder, and adding one is a dependency
//! decision rather than a detail. So the scanning leg takes the QR's own
//! base64 TEXT, which every client that displays a code also offers. The UI
//! says that plainly instead of implying a camera we do not have.
//!
//! # The progress stream is not optional
//!
//! Each of these futures only completes if something is consuming its
//! progress: the check code and the verification URL arrive ONLY through the
//! stream, and the flow blocks on the user acting on them. So the task is
//! always spawned as stream-consumer plus future, never the future alone.

use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Mutex,
};
use std::time::Duration;

use futures_util::StreamExt;
use matrix_sdk::authentication::oauth::qrcode::{
    GeneratedQrProgress, GrantLoginProgress, QrCodeData, QrProgress,
};
use serde_json::json;

use crate::rooms::require_client;
use crate::{enqueue, RustClient};

/// How long the homeserver is given to create the new device after the user
/// consents. The SDK's own default is 10 s; a real sign-in over a slow link
/// can take longer than that, and the cost of waiting is a spinner while the
/// cost of giving up is a flow the user has to start over.
const DEVICE_CREATION_TIMEOUT_SECS: u64 = 30;

/// The check code the user must relay, once it exists. Held so the
/// generate-side flow can be answered by a later FFI call — the SDK hands us
/// a one-shot `CheckCodeSender` through the stream, and there is nowhere else
/// to keep it.
pub(crate) struct QrLoginState {
    /// Bumped for every started flow. An answer or a cancel naming an old
    /// generation is from a flow the user has already left, and applying it
    /// would drive the current one with the previous one's input.
    generation: AtomicU64,
    /// The one-shot sender, WITH the generation it belongs to.
    ///
    /// The generation is stored beside it because `abort()` only REQUESTS
    /// cancellation (§16). An old pump that has already been handed a
    /// `QrScanned` step runs its arm to completion — the arm has no `.await`
    /// — so it can re-populate this slot AFTER `cancel()` cleared it. Without
    /// the pairing, `submit_check_code` would pass its own generation check
    /// and then take the DEAD flow's sender: the digits go nowhere and the
    /// live flow hangs until its device-creation timeout.
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

/// Classify a failure into something the UI can say. Deliberately COARSE and
/// deliberately not the error's own text: these errors quote channel state
/// and URLs, and a category is what a message can be written from.
fn classify(err: &str) -> &'static str {
    let lower = err.to_ascii_lowercase();
    if lower.contains("expired") || lower.contains("timeout") || lower.contains("timed out") {
        "expired"
    } else if lower.contains("cancel") {
        "cancelled"
    } else if lower.contains("checkcode") || lower.contains("check code") {
        "check_code"
    } else if lower.contains("unsupported") || lower.contains("not supported")
        || lower.contains("endpoint")
    {
        "unsupported"
    } else {
        "failed"
    }
}

fn emit(bridge: &Arc<Mutex<std::collections::VecDeque<String>>>, gen: u64, value: serde_json::Value) {
    let mut v = value;
    v["type"] = json!("qr_login_progress");
    v["generation"] = json!(gen);
    enqueue(bridge, v);
}

/// Start the flow where THIS device displays a QR code.
///
/// The new device scans it and then shows the user two digits, which they
/// type here — that is what `submit_check_code` answers.
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

        // The stream is consumed CONCURRENTLY with the future, never after
        // it: the QR payload and the check-code sender arrive through the
        // stream, and the future does not complete until the user has acted
        // on both.
        let stream_events = Arc::clone(&events);
        let stream_state = Arc::clone(&state);
        let pump = tokio::spawn(async move {
            while let Some(step) = progress.next().await {
                match step {
                    GrantLoginProgress::Starting => {
                        emit(&stream_events, gen, json!({ "step": "starting" }));
                    }
                    GrantLoginProgress::EstablishingSecureChannel(
                        GeneratedQrProgress::QrReady(data),
                    ) => {
                        // The payload is rendered to modules HERE rather than
                        // handed across the FFI as bytes: the encoder lives on
                        // this side already (verification uses it), and the C++
                        // side has no QR encoder at all.
                        match crate::render_qr_bytes(&data.to_bytes()) {
                            Some((size, bits)) => emit(
                                &stream_events,
                                gen,
                                json!({
                                    "step": "qr_ready",
                                    "qr_size": size,
                                    "qr_bits": bits,
                                    // The same payload as text, because a
                                    // camera is not the only way to move it
                                    // and some devices only offer paste.
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
                        // Paired with THIS flow's generation, and refused
                        // outright if the flow is already superseded.
                        if let Ok(mut guard) = stream_state.sender.lock() {
                            if stream_state.generation.load(Ordering::SeqCst) == gen {
                                *guard = Some((gen, sender));
                            }
                        }
                        emit(&stream_events, gen, json!({ "step": "check_code_needed" }));
                    }
                    GrantLoginProgress::WaitingForAuth { verification_uri } => {
                        // An https URL from the user's OWN homeserver's
                        // authorization server. It crosses as a string and the
                        // C++ side opens it through UrlLauncher, whose existing
                        // allowlist already refuses anything but http/https.
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

/// Start the flow where THIS device scans (well, is given the text of) the
/// QR the new device is displaying.
///
/// Here the check code is ours to SHOW: the user reads it off this screen and
/// types it on the new device.
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
                                // Two digits. It is not a secret — it exists so
                                // the two devices can prove they are talking to
                                // each other — but it is short-lived and only
                                // meaningful for this channel.
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
///
/// The sender is ONE-SHOT: a second call has nothing to send, which is why it
/// is taken out of the slot rather than borrowed.
pub(crate) fn submit_check_code(
    bridge: &RustClient,
    generation: u64,
    code: u8,
) -> Result<(), String> {
    let state = Arc::clone(&bridge.qr_login);
    if state.generation.load(Ordering::SeqCst) != generation {
        return Err("that sign-in is no longer running".to_owned());
    }
    // Taken under the SAME lock that checks the generation, so a pump
    // writing between the check and the take cannot slip an old sender in.
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
    // `send` is ASYNC and one-shot. It is spawned rather than blocked on
    // because this is called from the GUI thread across the FFI, and the
    // whole point of the flow is that it is waiting on a person.
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

/// Abandon whatever is running. Safe when nothing is.
pub(crate) fn cancel(bridge: &RustClient) {
    // Bumping FIRST means a late answer for the flow being cancelled is
    // rejected by generation rather than racing the abort.
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

    // The classifier is what the UI's wording is built on, so what matters is
    // that distinct failures stay distinct — not the exact strings.
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
        // Anything unrecognised must NOT be reported as one of the specific
        // causes — a wrong specific reason is worse than an honest generic
        // one, because the user acts on it.
        assert_eq!(classify("something nobody has seen before"), "failed");
    }

    // A payload that is not a QR code must be refused BEFORE any task is
    // spawned, so a paste of the wrong thing is an immediate message rather
    // than a flow that hangs.
    #[test]
    fn a_payload_that_is_not_a_sign_in_code_is_refused_up_front() {
        assert!(QrCodeData::from_base64("not a qr code at all").is_err());
        assert!(QrCodeData::from_base64("").is_err());
    }
}
