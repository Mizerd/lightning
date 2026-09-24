//! User-Interactive Authentication (UIA) and device sign-out.
//!
//! The SDK owns the protocol: the call is tried without auth, the server's
//! 401 challenge comes from `Error::as_uiaa_response()`, and the retry
//! carries ruma's `AuthData::Password`. Lightning only parks the pending
//! operation between challenge and answer (like the SDK's
//! `CrossSigningResetHandle`).
//!
//! The password is used once, in the retry. Scrubbing is best effort (see
//! `scrub_string`): on success the String moves into ruma's
//! `uiaa::Password`, which is dropped without zeroing. It is never cloned,
//! logged or enqueued; only stage names and categories cross the FFI.

use std::sync::Arc;

use matrix_sdk::ruma::{
    api::client::uiaa::{self, AuthData, AuthType, UiaaInfo},
    OwnedDeviceId,
};
use serde_json::json;

use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// The one operation the UIA layer retries. Add a variant per operation,
/// never a second slot.
pub(crate) enum UiaOperation {
    DeleteDevices(Vec<OwnedDeviceId>),
}

/// One parked operation awaiting user authentication.
pub(crate) struct UiaPending {
    /// The challenge id C++ replies to: the op id of the triggering call, so
    /// stale replies are rejected by equality.
    pub uia_id: u64,
    /// The server's UIA session, echoed on the retry.
    pub session: Option<String>,
    pub op: UiaOperation,
}

/// Best-effort scrub of a secret's buffer: volatile zero writes plus a
/// compiler fence so the stores are not eliminated, then `clear()`. Covers
/// our copy on error paths only; on success the String is moved into ruma's
/// `uiaa::Password`, which drops it unzeroed.
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

/// Sanitized challenge event: stage names and flow shapes only, never the
/// server's params or anything the user typed.
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
    // Can the password stage complete a flow (remaining stages exactly
    // [password] or [password, dummy])? The only stage rendered today.
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

/// Delete one or more of the account's own devices. The first attempt has no
/// auth (some servers allow a grace window); a UIA challenge parks the
/// operation and emits `uia_required`; other failures are terminal. Result
/// event: `device_delete_result { op_id, lifecycle, ok, category }`.
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
    // One UIA-gated operation at a time; a second would have no UI and could
    // cross answers.
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

/// Answer the pending challenge with the account password and retry. A
/// wrong password re-parks with the server's refreshed session and
/// re-emits `uia_required` with `wrong_password`.
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
    // Take the pending op only if the reply matches, so a stale reply cannot
    // consume a newer challenge.
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
                    // Wrong password (or a further stage): re-park with the refreshed session.
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

/// Abandon the pending challenge. Local only; the server's session expires.
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
