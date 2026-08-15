//! Reusable User-Interactive Authentication (UIA) layer + device sign-out
//! (v0.7.x).
//!
//! The SDK owns the protocol: the privileged call is attempted WITHOUT auth
//! first, the server's 401 UIA challenge is surfaced through
//! `Error::as_uiaa_response()`, and the retry carries the ruma
//! `AuthData::Password` the SDK serializes. Lightning implements no auth
//! stage itself; it only parks the pending operation between the challenge
//! and the user's response — the same slot pattern the SDK's own
//! `CrossSigningResetHandle` uses.
//!
//! SECURITY: the password crosses this module exactly once, inside the
//! retry call. Transit scrubbing is BEST-EFFORT (see `scrub_string`): the
//! error paths zero our copy, but the success path MOVES the String into
//! ruma's `uiaa::Password`, which serializes and drops it without zeroing.
//! ruma's type holds a plain `String` (its Debug impl redacts, but there
//! is no zeroize), so nothing here may clone, log, or enqueue it — and
//! nothing does. Only stage NAMES and coarse categories cross the FFI
//! event queue.

use std::sync::Arc;

use matrix_sdk::ruma::{
    api::client::uiaa::{self, AuthData, AuthType, UiaaInfo},
    OwnedDeviceId,
};
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// The one privileged operation the UIA layer currently retries. Extend as
/// an enum variant per operation — never as a second slot.
pub(crate) enum UiaOperation {
    DeleteDevices(Vec<OwnedDeviceId>),
}

/// One parked operation awaiting user authentication.
pub(crate) struct UiaPending {
    /// Doubles as the challenge id C++ replies to; equals the op id of the
    /// call that triggered the challenge, so stale replies are rejected by
    /// simple equality.
    pub uia_id: u64,
    /// The server's UIA session cookie, echoed back on the retry.
    pub session: Option<String>,
    pub op: UiaOperation,
}

/// Best-effort scrub of a secret's transit buffer: volatile writes plus a
/// compiler fence so dead-store elimination cannot drop the zeroing; zero
/// bytes are valid UTF-8, and `clear()` then drops the length.
///
/// HONESTY (review L1): this covers OUR copy on the error paths only. On
/// the success path the String is MOVED into ruma's `uiaa::Password`,
/// which serializes it into the request body and drops both without
/// zeroing — that memory is not scrubbable from here without patching
/// ruma. "Best-effort", not a guarantee.
fn scrub_string(secret: &mut String) {
    // SAFETY: writing 0x00 into every byte keeps the buffer valid UTF-8.
    unsafe {
        for byte in secret.as_mut_vec().iter_mut() {
            std::ptr::write_volatile(byte, 0);
        }
    }
    std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
    secret.clear();
}

/// Sanitized challenge event. Stage names and flow shapes only — never the
/// server's params payload, never anything typed by the user.
fn enqueue_challenge(
    events: &crate::EventQueueRef,
    uia_id: u64,
    lifecycle: u64,
    info: &UiaaInfo,
    wrong_password: bool,
) {
    let flows: Vec<Vec<String>> = info
        .flows
        .iter()
        .map(|flow| flow.stages.iter().map(|s| s.to_string()).collect())
        .collect();
    let completed: Vec<String> =
        info.completed.iter().map(|s| s.to_string()).collect();
    // "Can the password stage complete some flow?" — the only stage this
    // layer renders today. A flow whose remaining stages are exactly
    // [password] (or [password, dummy]) is completable.
    let has_password_stage = info.flows.iter().any(|flow| {
        flow.stages
            .iter()
            .filter(|stage| !info.completed.contains(stage))
            .all(|stage| {
                matches!(stage, AuthType::Password | AuthType::Dummy)
            })
            && flow.stages.iter().any(|s| matches!(s, AuthType::Password))
    });
    enqueue(events, json!({
        "type": "uia_required",
        "op_id": uia_id,
        "lifecycle": lifecycle,
        "flows": flows,
        "completed": completed,
        "has_password_stage": has_password_stage,
        "wrong_password": wrong_password,
    }));
}

/// Delete one or more of the account's OWN devices.
///
/// First attempt runs without auth (some servers allow a grace window). A
/// UIA challenge parks the operation and emits `uia_required`; any other
/// failure is terminal. Result event: `device_delete_result { op_id,
/// lifecycle, ok, category }`.
pub(crate) fn delete_devices(
    bridge: &RustClient,
    device_ids: Vec<String>,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let ids: Vec<OwnedDeviceId> = device_ids
        .iter()
        .map(|id| OwnedDeviceId::from(id.as_str()))
        .collect();
    if ids.is_empty() {
        return Err("no devices given".to_owned());
    }
    // One UIA-gated operation at a time: a second challenge would have no
    // UI surface and could cross answers between operations.
    if bridge
        .uia_pending
        .lock()
        .ok()
        .map(|guard| guard.is_some())
        .unwrap_or(false)
    {
        return Err("another operation is awaiting authentication".to_owned());
    }
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = client.delete_devices(&ids, None).await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(_) => {
                enqueue(&events, json!({
                    "type": "device_delete_result",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                }));
            }
            Err(err) => {
                if let Some(info) = err.as_uiaa_response() {
                    if let Ok(mut guard) = pending_slot.lock() {
                        *guard = Some(UiaPending {
                            uia_id: op_id,
                            session: info.session.clone(),
                            op: UiaOperation::DeleteDevices(ids),
                        });
                    }
                    enqueue_challenge(&events, op_id, lifecycle, info, false);
                } else {
                    enqueue(&events, json!({
                        "type": "device_delete_result",
                        "op_id": op_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
            }
        }
    });
    Ok(())
}

/// Answer the pending UIA challenge with the account password and retry the
/// parked operation. A wrong password re-parks the operation with the
/// server's refreshed session and re-emits `uia_required` with
/// `wrong_password`, so the dialog can offer another attempt.
pub(crate) fn uia_submit_password(
    bridge: &RustClient,
    uia_id: u64,
    mut password: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let Some(own_user) = client.user_id().map(ToOwned::to_owned) else {
        scrub_string(&mut password);
        return Err("no authenticated user".to_owned());
    };
    // Take the pending op only if the reply matches it; a stale reply must
    // not consume (or answer) a newer challenge.
    let pending = {
        let mut guard = bridge
            .uia_pending
            .lock()
            .map_err(|_| "internal state unavailable".to_owned())?;
        match guard.as_ref() {
            Some(p) if p.uia_id == uia_id => guard.take(),
            _ => None,
        }
    };
    let Some(pending) = pending else {
        scrub_string(&mut password);
        return Err("no matching authentication challenge".to_owned());
    };

    let mut auth = uiaa::Password::new(
        uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(
            own_user.to_string(),
        )),
        std::mem::take(&mut password),
    );
    auth.session = pending.session.clone();

    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let pending_slot = Arc::clone(&bridge.uia_pending);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let auth_data = AuthData::Password(auth);
        let (result, op) = match pending.op {
            UiaOperation::DeleteDevices(ids) => (
                client.delete_devices(&ids, Some(auth_data)).await,
                UiaOperation::DeleteDevices(ids),
            ),
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        match result {
            Ok(_) => {
                enqueue(&events, json!({
                    "type": "device_delete_result",
                    "op_id": uia_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                }));
            }
            Err(err) => {
                if let Some(info) = err.as_uiaa_response() {
                    // Wrong password (or a further stage): re-park with the
                    // server's refreshed session so the user can retry.
                    if let Ok(mut guard) = pending_slot.lock() {
                        *guard = Some(UiaPending {
                            uia_id,
                            session: info.session.clone(),
                            op,
                        });
                    }
                    enqueue_challenge(&events, uia_id, lifecycle, info, true);
                } else {
                    enqueue(&events, json!({
                        "type": "device_delete_result",
                        "op_id": uia_id,
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": classify_room_error(&err.to_string()),
                    }));
                }
            }
        }
    });
    Ok(())
}

/// Abandon the pending challenge (dialog cancelled). Local only — the
/// server's UIA session simply expires.
pub(crate) fn uia_cancel(bridge: &RustClient, uia_id: u64) {
    if let Ok(mut guard) = bridge.uia_pending.lock() {
        match guard.as_ref() {
            Some(p) if p.uia_id == uia_id => {
                guard.take();
            }
            _ => {}
        }
    }
}
