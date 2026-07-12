//! matrix-client-rust — Matrix Rust SDK FFI bridge for Lightning.
//!
//! The C++ backend owns UI state and polls this bridge for JSON events. Rust
//! owns the Matrix SDK client, Tokio runtime, and SDK SQLite store.

use std::{
    collections::{BTreeSet, HashMap, VecDeque},
    ffi::{c_char, CStr, CString},
    fs::OpenOptions,
    io::Write,
    os::raw::{c_int, c_void},
    panic::{catch_unwind, AssertUnwindSafe},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use matrix_sdk::{
    authentication::matrix::MatrixSession,
    config::SyncSettings,
    encryption::verification::{SasState, SasVerification, VerificationRequest},
    room::MessagesOptions,
    ruma::{
        api::{error::ErrorKind, FeatureFlag},
        events::{
            key::verification::{
                request::ToDeviceKeyVerificationRequestEvent, VerificationMethod,
            },
            room::{
                encrypted::OriginalSyncRoomEncryptedEvent,
                message::{MessageType, OriginalSyncRoomMessageEvent, RoomMessageEventContent},
            },
            typing::SyncTypingEvent,
        },
        uint, EventId, OwnedDeviceId, OwnedEventId, OwnedTransactionId, OwnedUserId, RoomId,
        UInt, UserId,
    },
    room::Receipts,
    store::RoomLoadSettings,
    Client, LoopCtrl, Room, SessionMeta, SessionTokens,
};
use futures_util::StreamExt;
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    room_list_service::{filters, RoomListItem},
    spaces::SpaceService,
    sync_service::{Error as UnifiedSyncError, State as UnifiedSyncState, SyncService},
};
use serde::{Deserialize, Serialize};
use serde_json::json;

mod rooms;
mod timeline;

/// Shared alias for the FFI event queue reference (used by `rooms.rs`).
pub(crate) type EventQueueRef = Arc<Mutex<VecDeque<String>>>;

struct RustClient {
    store_path: PathBuf,
    session_file: Arc<Mutex<Option<PathBuf>>>,
    client: Arc<Mutex<Option<Client>>>,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_task: Mutex<Option<SyncTask>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    // v0.5.7: shared multi-thread runtime for everything with a lifetime
    // longer than one FFI call — SDK timelines, their subscription
    // forwarders, the send queue, the event cache, and room-key import.
    // The per-call `run_async` runtimes cannot host those: tasks spawned
    // on them die when the call's runtime is dropped.
    runtime: Arc<tokio::runtime::Runtime>,
    // v0.5.7: live timeline registry (single active room timeline).
    timelines: Arc<timeline::TimelineRegistry>,
    // v0.5.7: managed room-key import task so sign-out can join it
    // deterministically instead of polling a flag with a timeout.
    import_task: Mutex<Option<tokio::task::JoinHandle<()>>>,
    // Short room-state commands (typing, receipts, invite membership and
    // marked-unread) are owned and joined during shutdown. Nothing spawned by
    // the 0.5.8 room-state layer is detached.
    room_action_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    active_typing_room: Arc<Mutex<Option<String>>>,
    receipt_targets: Arc<Mutex<HashMap<String, OwnedEventId>>>,
    receipt_serial: Arc<tokio::sync::Mutex<()>>,
    invite_actions: Arc<Mutex<BTreeSet<String>>>,
    // v0.5.0: SAS verification state. Single active flow at a time keeps
    // the FFI simple; if a second request arrives we cancel the first.
    // Both slots are cleared when the flow reaches Done or Cancelled.
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas) — SasVerification has no flow_id() accessor on
    // matrix-sdk 0.18, so we track it externally.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
    // v0.5.6: encrypted room-key import is serialized per client — a
    // second attempt while one is in flight is rejected synchronously.
    // The flag is also read by C++ before sign-out to wait for a live
    // import to finish before dropping the store.
    import_active: Arc<AtomicBool>,
    // v0.5.9: parked media payloads for the binary take/free bridge, keyed
    // by operation id. Bytes wait here between a `media_ready` event and
    // the matching `mx_rust_media_take` call; the map is cleared on
    // shutdown so decrypted media never outlives the session in memory.
    media_results: Arc<Mutex<HashMap<u64, Vec<u8>>>>,
}

impl RustClient {
    fn new(store_path: PathBuf) -> Result<Self, String> {
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("lightning-sdk")
            .enable_all()
            .build()
            .map_err(|err| format!("failed to create shared Tokio runtime: {err}"))?;
        Ok(Self {
            store_path,
            session_file: Arc::new(Mutex::new(None)),
            client: Arc::new(Mutex::new(None)),
            events: Arc::clone(&events),
            sync_task: Mutex::new(None),
            sync_mode: Arc::new(Mutex::new(SyncMode::Stopped)),
            runtime: Arc::new(runtime),
            timelines: Arc::new(timeline::TimelineRegistry::new(events)),
            import_task: Mutex::new(None),
            room_action_tasks: Mutex::new(Vec::new()),
            active_typing_room: Arc::new(Mutex::new(None)),
            receipt_targets: Arc::new(Mutex::new(HashMap::new())),
            receipt_serial: Arc::new(tokio::sync::Mutex::new(())),
            invite_actions: Arc::new(Mutex::new(BTreeSet::new())),
            active_request: Arc::new(Mutex::new(None)),
            active_sas: Arc::new(Mutex::new(None)),
            import_active: Arc::new(AtomicBool::new(false)),
            media_results: Arc::new(Mutex::new(HashMap::new())),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    fn stop_sync_and_wait(&self) -> bool {
        let task = self.sync_task.lock().ok().and_then(|mut guard| guard.take());
        if let Some(mut task) = task {
            if let Some(cancel) = task.cancel.take() {
                let _ = cancel.send(());
            }
            if let Some(thread) = task.thread.take() {
                let _ = thread.join();
            }
            true
        } else {
            false
        }
    }

    /// v0.5.7: deterministic teardown of everything the shared runtime
    /// hosts. Order matters: timelines first (they hold event-cache /
    /// send-queue references), then the managed room-key import (joined,
    /// never abandoned mid-write — the crypto store must not be deleted
    /// under it), then the sync loop. A bounded timeout remains only as a
    /// last-resort error boundary after the deterministic join.
    fn shutdown_managed_tasks(&self) -> (bool, bool) {
        self.timelines.shutdown(&self.runtime);

        let actions = self.room_action_tasks.lock().ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
        self.runtime.block_on(async {
            for mut handle in actions {
                if tokio::time::timeout(
                    std::time::Duration::from_secs(timeline::SHUTDOWN_JOIN_TIMEOUT_SECS),
                    &mut handle,
                ).await.is_err() {
                    handle.abort();
                    let _ = handle.await;
                }
            }
        });

        let import = self.import_task.lock().ok().and_then(|mut guard| guard.take());
        let mut import_joined = true;
        if let Some(handle) = import {
            if !handle.is_finished() {
                let joined = self.runtime.block_on(async {
                    tokio::time::timeout(
                        std::time::Duration::from_secs(
                            timeline::SHUTDOWN_JOIN_TIMEOUT_SECS,
                        ),
                        handle,
                    )
                    .await
                    .is_ok()
                });
                import_joined = joined;
            }
        }

        let sync_stopped = self.stop_sync_and_wait();

        // v0.5.9: drop any parked (possibly decrypted) media bytes with the
        // session; nothing may hand them out after sign-out.
        if let Ok(mut guard) = self.media_results.lock() {
            guard.clear();
        }
        (import_joined, sync_stopped)
    }

    fn reap_finished_sync(&self) {
        let finished = self
            .sync_task
            .lock()
            .ok()
            .and_then(|guard| guard.as_ref().map(|task| {
                task.thread.as_ref().is_some_and(std::thread::JoinHandle::is_finished)
            }))
            .unwrap_or(false);
        if finished {
            self.stop_sync_and_wait();
        }
    }

    fn spawn_room_action<F>(&self, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.room_action_tasks.lock() {
            tasks.retain(|task| !task.is_finished());
            tasks.push(self.runtime.spawn(future));
        }
    }
}

struct SyncTask {
    cancel: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

/// The authoritative sync path selection. This is deliberately distinct from
/// transient connectivity: a temporary network loss keeps the selected mode
/// (SlidingSync / ClassicSyncFallback) and is reported only through the
/// connection state, so the mode label does not flicker. Only a fatal
/// authentication failure moves to `Failed`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SyncMode {
    Probing,
    SlidingSync,
    ClassicSyncFallback,
    Failed,
    Stopped,
}

impl SyncMode {
    fn as_str(self) -> &'static str {
        match self {
            Self::Probing => "probing",
            Self::SlidingSync => "sliding_sync",
            Self::ClassicSyncFallback => "classic_fallback",
            Self::Failed => "failed",
            Self::Stopped => "stopped",
        }
    }
}

/// Set the authoritative sync mode and announce it — but only when it
/// actually changes. Deduping at the source keeps the mode label from
/// flickering when the state machine re-affirms the same mode (e.g. the
/// modern loop re-entering `SlidingSync` after a transient retry).
fn set_sync_mode(
    slot: &Arc<Mutex<SyncMode>>,
    events: &Arc<Mutex<VecDeque<String>>>,
    mode: SyncMode,
    reason: Option<&str>,
) {
    let changed = match slot.lock() {
        Ok(mut guard) => {
            let changed = *guard != mode;
            *guard = mode;
            changed
        }
        Err(_) => true,
    };
    if changed {
        enqueue(events, json!({
            "type": "room_list_mode", "mode": mode.as_str(), "reason": reason
        }));
    }
}

#[derive(Clone, Serialize, Deserialize)]
struct PersistentSessionFile {
    version: u32,
    homeserver: String,
    session: MatrixSession,
}

impl Drop for RustClient {
    fn drop(&mut self) {
        let _ = self.shutdown_managed_tasks();
    }
}

#[no_mangle]
pub extern "C" fn mx_rust_backend_name() -> *mut c_char {
    ffi_string(|| Ok("matrix-rust-sdk".to_owned()))
}

#[no_mangle]
pub extern "C" fn mx_rust_status_string() -> *mut c_char {
    ffi_string(|| {
        Ok("Matrix Rust SDK backend linked. Lightning uses matrix-sdk 0.18 and matrix-sdk-ui 0.18 with SDK-owned E2EE sync, timeline pagination, user lookup, encrypted media and protected client-side link previews. No manual crypto in C++.".to_owned())
    })
}

// v0.5.0-prep+9: initial E2EE support gate. The FFI reports 1 because
// matrix-sdk 0.18 is compiled in with the `e2e-encryption` feature and
// the encrypted receive + encrypted send paths were both verified live
// against `matrix.smetonis.net` (Element Classic ↔ Lightning marker
// round-trip). The C++ CryptoManager gate remains the source of truth
// for the UI; this only unblocks the `sendTextMessage` refusal inside
// RustSdkMatrixClient when the interactive user is sending into an
// encrypted room.
#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee(_client: *mut c_void) -> c_int {
    1
}

#[no_mangle]
pub extern "C" fn mx_rust_version() -> *mut c_char {
    ffi_string(|| Ok(env!("CARGO_PKG_VERSION").to_owned()))
}

#[no_mangle]
pub extern "C" fn mx_rust_create(store_path: *const c_char) -> *mut c_void {
    match catch_unwind(AssertUnwindSafe(|| {
        let store_path = unsafe { cstr_arg(store_path) }?;
        let path = PathBuf::from(store_path);
        std::fs::create_dir_all(&path)
            .map_err(|err| format!("failed to create Rust SDK store directory: {err}"))?;
        let client = RustClient::new(path)?;
        Ok::<*mut c_void, String>(Box::into_raw(Box::new(client)) as *mut c_void)
    })) {
        Ok(Ok(ptr)) => ptr,
        Ok(Err(_)) | Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_session_file(
    ptr: *mut c_void,
    session_file_path: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let path = unsafe { cstr_arg(session_file_path) }?;
        let next = if path.trim().is_empty() {
            None
        } else {
            Some(PathBuf::from(path))
        };
        let mut guard = bridge
            .session_file
            .lock()
            .map_err(|_| "Rust SDK session-file lock poisoned.".to_owned())?;
        *guard = next;
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_destroy(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if ptr.is_null() {
            return;
        }
        drop(unsafe { Box::from_raw(ptr as *mut RustClient) });
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_login(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user: *const c_char,
    password: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user = unsafe { cstr_arg(user) }?;
        let password = unsafe { cstr_arg(password) }?;

        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let session_file = Arc::clone(&bridge.session_file);
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "login", async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
                        let login = client
                            .matrix_auth()
                            .login_username(&user, &password)
                            .initial_device_display_name("Lightning")
                            .send()
                            .await;
                        match login {
                            Ok(response) => {
                                let session = MatrixSession::from(&response);
                                if let Some(path) = configured_session_file(&session_file) {
                                    if let Err(err) =
                                        save_persistent_session(&path, &homeserver, &session)
                                    {
                                        enqueue(
                                            &events,
                                            json!({
                                                "type": "error",
                                                "message": format!(
                                                    "Failed to save Rust SDK smoke session: {err}"
                                                ),
                                            }),
                                        );
                                    }
                                }
                                if let Ok(mut guard) = client_slot.lock() {
                                    *guard = Some(client);
                                }
                                enqueue(
                                    &events,
                                    json!({
                                        "type": "login_ok",
                                        "homeserver": homeserver,
                                        "user_id": response.user_id.to_string(),
                                        "device_id": response.device_id.to_string(),
                                        "access_token": response.access_token,
                                    }),
                                );
                            }
                            Err(err) => {
                                // Release SDK/store ownership before C++ can
                                // react to login_failed with a local reset.
                                drop(client);
                                enqueue(
                                    &events,
                                    json!({
                                        "type": "login_failed",
                                        "message": format_matrix_error("Matrix Rust SDK login failed", err),
                                    }),
                                );
                            }
                        }
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({ "type": "login_failed", "message": err }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_restore_from_file(
    ptr: *mut c_void,
    homeserver: *const c_char,
    expected_user_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let expected_user_id = unsafe { cstr_arg(expected_user_id) }?;
        let Some(session_file) = configured_session_file(&bridge.session_file) else {
            return Ok("error: Rust SDK smoke session file is not configured.".to_owned());
        };

        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "restore_from_file", async move {
                let stored = match read_persistent_session(&session_file) {
                    Ok(stored) => stored,
                    Err(err) => {
                        enqueue(
                            &events,
                            json!({
                                "type": "login_failed",
                                "message": format!("Rust SDK smoke session restore unavailable: {err}"),
                            }),
                        );
                        return;
                    }
                };

                if stored.homeserver != homeserver {
                    enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session homeserver does not match this run.",
                        }),
                    );
                    return;
                }

                let stored_user = stored.session.meta.user_id.to_string();
                if !expected_user_id.is_empty() && stored_user != expected_user_id {
                    enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session account does not match this run.",
                        }),
                    );
                    return;
                }

                match restore_client_with_session(&homeserver, &store_path, stored.session.clone())
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
                        if let Err(err) =
                            save_persistent_session(&session_file, &homeserver, &stored.session)
                        {
                            enqueue(
                                &events,
                                json!({
                                    "type": "error",
                                    "message": format!(
                                        "Failed to refresh Rust SDK smoke session: {err}"
                                    ),
                                }),
                            );
                        }
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": stored_user,
                                "device_id": stored.session.meta.device_id.to_string(),
                            }),
                        );
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": err,
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_restore(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user_id: *const c_char,
    device_id: *const c_char,
    access_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;

        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "restore", async move {
                match restore_client(&homeserver, &store_path, &user_id, &device_id, access_token)
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                        );
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": user_id,
                                "device_id": device_id,
                            }),
                        );
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "message": err,
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_logout(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        bridge.stop_sync_and_wait();
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone());
        let events = Arc::clone(&bridge.events);
        if let Some(client) = client {
            std::thread::spawn(move || {
                let runtime_events = Arc::clone(&events);
                let logout_events = Arc::clone(&events);
                let completed = Arc::new(AtomicBool::new(false));
                let completion_flag = Arc::clone(&completed);
                run_async(runtime_events, "logout", async move {
                    let event = match client.matrix_auth().logout().await {
                        Ok(_) => json!({ "type": "logged_out", "result": "ok" }),
                        Err(err) if matches!(
                            err.client_api_error_kind(),
                            Some(ErrorKind::UnknownToken { .. })
                        ) => json!({
                            "type": "logged_out",
                            "result": "already_invalid",
                        }),
                        Err(err) => json!({
                            "type": "logged_out",
                            "result": "failed",
                            "message": format_matrix_error(
                                "Matrix Rust SDK logout failed", err),
                        }),
                    };
                    completion_flag.store(true, Ordering::SeqCst);
                    enqueue(&logout_events, event);
                });
                if !completed.load(Ordering::SeqCst) {
                    enqueue(
                        &events,
                        json!({
                            "type": "logged_out",
                            "result": "failed",
                            "message": "Matrix Rust SDK logout task could not complete.",
                        }),
                    );
                }
            });
        } else {
            bridge.enqueue(json!({
                "type": "logged_out",
                "result": "no_active_session",
            }));
        }
        if let Ok(mut guard) = bridge.client.lock() {
            *guard = None;
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_sync(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            bridge.enqueue(json!({
                "type": "error",
                "message": "Rust SDK sync requested before a session was available.",
            }));
            return;
        };

        bridge.reap_finished_sync();
        let mut task_slot = match bridge.sync_task.lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        if task_slot.is_some() {
            return;
        }

        let events = Arc::clone(&bridge.events);
        let sync_mode = Arc::clone(&bridge.sync_mode);
        bridge.enqueue(json!({ "type": "status", "state": "syncing" }));

        let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
        let thread = std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                run_authoritative_sync(client, events, sync_mode, cancel_rx).await;
            });
        });
        *task_slot = Some(SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
        });
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stop_sync(ptr: *mut c_void) -> c_int {
    match catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return false;
        };
        let stopped = bridge.stop_sync_and_wait();
        set_sync_mode(&bridge.sync_mode, &bridge.events, SyncMode::Stopped, None);
        bridge.enqueue(json!({ "type": "status", "state": "disconnected" }));
        stopped
    })) {
        Ok(true) => 1,
        Ok(false) | Err(_) => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_send_typing(
    ptr: *mut c_void,
    room_id: *const c_char,
    typing: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let active = Arc::clone(&bridge.active_typing_room);
        let events = Arc::clone(&bridge.events);
        let is_typing = typing != 0;
        let previous = if let Ok(mut guard) = active.lock() {
            if is_typing {
                let previous = guard.clone().filter(|old| old != &room_id);
                *guard = Some(room_id.clone());
                previous
            } else {
                if guard.as_deref() == Some(room_id.as_str()) { *guard = None; }
                None
            }
        } else { None };
        bridge.spawn_room_action(async move {
            if let Some(previous) = previous {
                if let Ok(id) = RoomId::parse(previous) {
                    if let Some(old_room) = client.get_room(&id) {
                        let _ = old_room.typing_notice(false).await;
                    }
                }
            }
            if is_typing && active.lock().ok().and_then(|g| g.clone()).as_deref()
                != Some(room_id.as_str()) {
                return;
            }
            if room.typing_notice(is_typing).await.is_ok() {
                enqueue(&events, json!({
                    "type": "typing_sent", "active": is_typing
                }));
            }
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_send_read_receipt(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let event_id: OwnedEventId = EventId::parse(event_id)
            .map_err(|_| "invalid event id".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let targets = Arc::clone(&bridge.receipt_targets);
        let serial = Arc::clone(&bridge.receipt_serial);
        if let Ok(mut guard) = targets.lock() {
            guard.insert(room_id.clone(), event_id.clone());
        }
        bridge.spawn_room_action(async move {
            let _serial = serial.lock().await;
            if targets.lock().ok().and_then(|guard| guard.get(&room_id).cloned())
                .as_ref() != Some(&event_id) {
                return;
            }
            let receipts = Receipts::new()
                .fully_read_marker(event_id.clone())
                .public_read_receipt(event_id);
            if room.send_multiple_receipts(receipts).await.is_ok() {
                enqueue(&events, json!({ "type": "read_marker_advanced", "room_id": room_id }));
            } else {
                enqueue(&events, json!({
                    "type": "room_action_error", "action": "read_receipt",
                    "room_id": room_id
                }));
            }
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_marked_unread(
    ptr: *mut c_void,
    room_id: *const c_char,
    unread: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            match room.set_unread_flag(unread != 0).await {
                Ok(()) => enqueue_rooms(&events, &client).await,
                Err(_) => enqueue(&events, json!({
                    "type": "room_action_error", "action": "marked_unread"
                })),
            }
        });
        Ok(String::new())
    })
}

/// RAII cleanup for a pending invite action. Removing the room id from the
/// pending set on `Drop` guarantees cleanup on *every* task exit — success,
/// failure, cancellation (task abort), or panic — so a room can never be
/// left permanently stuck in the pending state.
struct InviteActionGuard {
    pending: Arc<Mutex<BTreeSet<String>>>,
    room_id: String,
}

impl Drop for InviteActionGuard {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.pending.lock() {
            guard.remove(&self.room_id);
        }
    }
}

fn invite_action(ptr: *mut c_void, room_id: *const c_char, accept: bool) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        // Joined (or any non-invited) rooms can never invoke accept/reject.
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .filter(|room| room.state() == matrix_sdk::RoomState::Invited)
            .ok_or_else(|| "room is not an invitation".to_owned())?;
        let pending = Arc::clone(&bridge.invite_actions);
        if !pending.lock().map_err(|_| "invite action state unavailable".to_owned())?
            .insert(room_id.clone()) {
            return Err("invite action already pending".to_owned());
        }
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            // Guard owns the pending-set entry for the whole task lifetime.
            let _guard = InviteActionGuard {
                pending: Arc::clone(&pending),
                room_id: room_id.clone(),
            };
            enqueue(&events, json!({
                "type": "invite_state_update", "room_id": room_id,
                "action": if accept { "accept" } else { "reject" }, "state": "pending"
            }));
            let result = if accept { room.join().await } else { room.leave().await };
            enqueue(&events, json!({
                "type": "invite_state_update", "room_id": room_id,
                "action": if accept { "accept" } else { "reject" },
                "state": if result.is_ok() { "done" } else { "failed" }
            }));
            if result.is_ok() { enqueue_rooms(&events, &client).await; }
            // `_guard` drops here, clearing the pending entry.
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_accept_invite(
    ptr: *mut c_void, room_id: *const c_char,
) -> *mut c_char { invite_action(ptr, room_id, true) }

#[no_mangle]
pub unsafe extern "C" fn mx_rust_reject_invite(
    ptr: *mut c_void, room_id: *const c_char,
) -> *mut c_char { invite_action(ptr, room_id, false) }

/// Controlled fresh room-list reset. Called by C++ if it ever rejects a
/// malformed/out-of-range room-list diff, so the model recovers to a
/// complete, correct snapshot from the SDK's current room set instead of
/// staying stale. Well-formed diffs from the dynamic adapter should never
/// trigger this; it is a safety net, not a routine path.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_resync_rooms(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Err("no active Matrix session".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            enqueue_rooms(&events, &client).await;
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_poll_event(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event = bridge
            .events
            .lock()
            .map_err(|_| "Rust SDK event queue lock poisoned.".to_owned())?
            .pop_front()
            .unwrap_or_default();
        Ok(event)
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_send_text(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    transaction_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let transaction_id = unsafe { cstr_arg(transaction_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "send_text", async move {
                let Some(room_id_ref) = RoomId::parse(&room_id).ok() else {
                    enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Invalid Matrix room id.",
                        }),
                    );
                    return;
                };
                let Some(room) = client.get_room(&room_id_ref) else {
                    enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Rust SDK does not know that room yet.",
                        }),
                    );
                    return;
                };

                // v0.5.0-prep+9: encrypted rooms are now allowed on the
                // interactive UI send path. matrix-sdk auto-encrypts via
                // its `e2e-encryption` feature when the room's
                // encryption state says so; if it can't (missing keys,
                // untrusted target device, etc.) the SDK returns an
                // error that flows back through send_failed. The C++
                // side still refuses when CryptoManager::supportsE2ee()
                // is false — see RustSdkMatrixClient::sendTextMessage.
                let content = RoomMessageEventContent::text_plain(body);
                let txn: OwnedTransactionId = transaction_id.clone().into();
                match room.send(content).with_transaction_id(txn).await {
                    Ok(result) => enqueue(
                        &events,
                        json!({
                            "type": "send_ok",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "event_id": result.response.event_id.to_string(),
                        }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": format_matrix_error("Matrix Rust SDK send failed", err),
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

/// Encrypted-room test probe (v0.5.0-prep+6). Structurally similar to
/// mx_rust_send_text but REFUSES non-encrypted rooms — the mirror-image of
/// the safety in mx_rust_send_text. Neither entry point can be misused: the
/// UI-facing send goes through mx_rust_send_text and only reaches
/// unencrypted rooms; the smoke-only probe goes through this function and
/// only reaches encrypted rooms.
///
/// matrix-sdk performs the encryption end-to-end via its e2e-encryption +
/// sqlite features. This FFI never sees ciphertext, keys, or session
/// material. On success the SDK returns a server event id (safe to log).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_probe_encrypted_send(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    transaction_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let transaction_id = unsafe { cstr_arg(transaction_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "probe_encrypted_send", async move {
                let Some(room_id_ref) = RoomId::parse(&room_id).ok() else {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Invalid Matrix room id.",
                        }),
                    );
                    return;
                };
                let Some(room) = client.get_room(&room_id_ref) else {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Rust SDK does not know that room yet.",
                        }),
                    );
                    return;
                };
                if !room.encryption_state().is_encrypted() {
                    enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": "Probe refused: target room is not encrypted.",
                        }),
                    );
                    return;
                }

                let content = RoomMessageEventContent::text_plain(body);
                let txn: OwnedTransactionId = transaction_id.clone().into();
                match room.send(content).with_transaction_id(txn).await {
                    Ok(result) => enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_ok",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "event_id": result.response.event_id.to_string(),
                        }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "encrypted_send_failed",
                            "room_id": room_id,
                            "transaction_id": transaction_id,
                            "message": format_matrix_error(
                                "Matrix Rust SDK encrypted send failed", err),
                        }),
                    ),
                }
            });
        });

        Ok(String::new())
    })
}

/// Key-backup recovery probe (v0.5.0-prep+7). Calls
/// `client.encryption().recovery().recover(recovery_key)` on matrix-sdk
/// v0.18 to import backed-up room keys from the homeserver's secret
/// storage. The recovery key must arrive here as a plain string; the C++
/// side is expected to sanitise before calling. This FFI **never** logs
/// the recovery key or the imported key material. Result events on the
/// poll queue:
///   { "type": "key_backup_status", "state": "attempted" }
///   { "type": "key_backup_status", "state": "ok" }
///   { "type": "key_backup_status", "state": "failed", "message": "…" }
///
/// Recovery-passphrase support is intentionally NOT wired here — the
/// matrix-sdk API takes a recovery *key* (BASE58) on the fast path. The
/// smoke harness reports `key_backup=failed reason=passphrase_not_supported`
/// if a passphrase is provided but no key.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_recover_from_backup(
    ptr: *mut c_void,
    recovery_key: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let recovery_key = unsafe { cstr_arg(recovery_key) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "recover_backup", async move {
                enqueue(
                    &events,
                    json!({ "type": "key_backup_status", "state": "attempted" }),
                );
                let recovery = client.encryption().recovery();
                match recovery.recover(&recovery_key).await {
                    Ok(_) => enqueue(
                        &events,
                        json!({ "type": "key_backup_status", "state": "ok" }),
                    ),
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "key_backup_status",
                            "state": "failed",
                            "message": format_matrix_error(
                                "Matrix Rust SDK key backup recover failed", err),
                        }),
                    ),
                }
            });
        });
        Ok(String::new())
    })
}

/// Timeline reload for a specific room via matrix-sdk 0.18 Room::messages.
/// Emits the same `timeline_event` shape the live event handlers use, so the
/// C++ side dedupes by event_id automatically. Body plaintext is only
/// forwarded when the SDK decrypted the event (or when it was never
/// encrypted); ciphertext is never forwarded — undecryptable rows emit an
/// empty body + undecryptable=true, exactly like the live path.
///
/// Also emits a summary event on the poll queue:
///   { "type": "reload_timeline_done",
///     "room_id": "...", "events": N, "decrypted": N, "undecryptable": N }
///   { "type": "reload_timeline_failed", "room_id": "...", "message": "..." }
#[no_mangle]
pub unsafe extern "C" fn mx_rust_reload_room_timeline(
    ptr: *mut c_void,
    room_id: *const c_char,
    limit: u32,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };

        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "reload_timeline", async move {
                let Ok(room_ref) = RoomId::parse(&room_id) else {
                    enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": "Invalid Matrix room id.",
                    }));
                    return;
                };
                let Some(room) = client.get_room(&room_ref) else {
                    enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": "Rust SDK does not know that room yet.",
                    }));
                    return;
                };

                let mut opts = MessagesOptions::backward();
                // Clamp limit to a sane range; matrix-sdk's default is 10.
                let clamped: u64 = if limit == 0 { 30 } else {
                    std::cmp::min(limit as u64, 200)
                };
                opts.limit = UInt::new(clamped).unwrap_or(uint!(30));

                match room.messages(opts).await {
                    Ok(messages) => {
                        let mut total = 0u32;
                        let mut decrypted = 0u32;
                        let mut undecryptable_count = 0u32;
                        // messages.chunk is newest-first for backward();
                        // reverse so C++ inserts oldest-first, matching
                        // the live sync ordering.
                        for ev in messages.chunk.into_iter().rev() {
                            total += 1;
                            let event_id = ev
                                .event_id()
                                .map(|i| i.to_string())
                                .unwrap_or_default();
                            let sender = ev
                                .sender()
                                .map(|i| i.to_string())
                                .unwrap_or_default();
                            if event_id.is_empty() {
                                continue;
                            }
                            let is_decrypted = ev.encryption_info().is_some();

                            let raw = ev.raw();
                            let value: serde_json::Value = match serde_json::from_str(
                                raw.json().get(),
                            ) {
                                Ok(v) => v,
                                Err(_) => continue,
                            };
                            let ts_ms = value
                                .get("origin_server_ts")
                                .and_then(|v| v.as_u64())
                                .unwrap_or(0);
                            let type_str = value
                                .get("type")
                                .and_then(|v| v.as_str())
                                .unwrap_or("");
                            let content = value.get("content");

                            let (is_encrypted, undecryptable, msgtype, body) =
                                if type_str == "m.room.encrypted" {
                                    undecryptable_count += 1;
                                    (true, true, "encrypted".to_owned(), String::new())
                                } else if type_str == "m.room.message" {
                                    let mt = content
                                        .and_then(|c| c.get("msgtype"))
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("m.text");
                                    let bd = content
                                        .and_then(|c| c.get("body"))
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("")
                                        .to_owned();
                                    let kind = match mt {
                                        "m.notice" => "notice",
                                        "m.emote" => "emote",
                                        _ => "text",
                                    }
                                    .to_owned();
                                    if is_decrypted {
                                        decrypted += 1;
                                    }
                                    (is_decrypted, false, kind, bd)
                                } else {
                                    // Skip state / other event types on
                                    // this path — the live sync handles
                                    // room state separately.
                                    continue;
                                };

                            enqueue(&events, json!({
                                "type": "timeline_event",
                                "room_id": room_id.clone(),
                                "event": {
                                    "event_id": event_id,
                                    "sender": sender,
                                    "body": body,
                                    "msgtype": msgtype,
                                    "timestamp_ms": ts_ms,
                                    "is_encrypted": is_encrypted,
                                    "is_decrypted": is_decrypted,
                                    "undecryptable": undecryptable,
                                    "decrypted": is_decrypted,
                                }
                            }));
                        }
                        enqueue(&events, json!({
                            "type": "reload_timeline_done",
                            "room_id": room_id,
                            "events": total,
                            "decrypted": decrypted,
                            "undecryptable": undecryptable_count,
                        }));
                    }
                    Err(err) => enqueue(&events, json!({
                        "type": "reload_timeline_failed",
                        "room_id": room_id,
                        "message": format_matrix_error(
                            "Matrix Rust SDK Room::messages failed", err),
                    })),
                }
            });
        });
        Ok(String::new())
    })
}

/// SAS emoji verification — accept an incoming request and drive it to
/// the KeysExchanged state (v0.5.0). Emits `verification_sas_ready` with
/// the emoji list when the SDK finishes key exchange; `verification_done`
/// / `verification_cancelled` when the flow reaches a terminal state.
///
/// Poll-based state watching (500 ms cadence, 120 s cap) is used
/// deliberately instead of a futures Stream so we don't introduce a
/// direct futures-util dep — matrix-sdk exposes state() as a snapshot.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_accept_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;

        let request = match bridge.active_request.lock() {
            Ok(g) => g.clone(),
            Err(_) => None,
        };
        let Some(request) = request else {
            return Ok("error: no active verification request.".to_owned());
        };
        if request.flow_id() != flow_id {
            return Ok("error: verification flow id mismatch.".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "verification_accept", async move {
                if let Err(err) = request.accept().await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": request.flow_id().to_string(),
                        "message": format_matrix_error(
                            "Matrix Rust SDK verification accept failed", err),
                    }));
                    return;
                }
                enqueue(&events, json!({
                    "type": "verification_ready",
                    "flow_id": request.flow_id().to_string(),
                }));

                // Wait for the request to transition into SasV1; poll
                // for up to 60s. matrix-sdk drives the handshake
                // automatically after accept().
                let sas: Option<SasVerification> = {
                    let mut sas: Option<SasVerification> = None;
                    for _ in 0..120 {
                        tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                        if let Some(v) = client
                            .encryption()
                            .get_verification(request.other_user_id(),
                                              request.flow_id())
                            .await
                        {
                            if let matrix_sdk::encryption::verification::Verification::SasV1(s) = v {
                                sas = Some(s);
                                break;
                            }
                        }
                        if request.is_cancelled() {
                            break;
                        }
                    }
                    sas
                };
                let Some(sas) = sas else {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": request.flow_id().to_string(),
                        "message": "Timed out waiting for SAS handshake.",
                    }));
                    return;
                };
                // Start the SAS flow with the SDK's default settings.
                let sas_flow_id = request.flow_id().to_string();
                if let Err(err) = sas.accept().await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": sas_flow_id,
                        "message": format_matrix_error(
                            "SAS accept failed", err),
                    }));
                    return;
                }
                if let Ok(mut g) = sas_slot.lock() {
                    *g = Some((sas_flow_id.clone(), sas.clone()));
                }
                enqueue(&events, json!({
                    "type": "verification_sas_started",
                    "flow_id": sas_flow_id,
                }));

                // Poll state for up to 120 s waiting for KeysExchanged /
                // Done / Cancelled. matrix-sdk emits emojis at
                // KeysExchanged.
                let mut emitted_emojis = false;
                for _ in 0..240 {
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    let state = sas.state();
                    match state {
                        SasState::KeysExchanged { .. } if !emitted_emojis => {
                            emitted_emojis = true;
                            let mut emoji_list: Vec<serde_json::Value> = Vec::new();
                            if let Some(emojis) = sas.emoji() {
                                for e in emojis.iter() {
                                    emoji_list.push(json!({
                                        "symbol": e.symbol,
                                        "description": e.description,
                                    }));
                                }
                            }
                            let dec = sas.decimals().map(|(a, b, c)| json!([a, b, c]))
                                .unwrap_or(json!([]));
                            enqueue(&events, json!({
                                "type": "verification_sas_ready",
                                "flow_id": sas_flow_id,
                                "emojis": emoji_list,
                                "decimals": dec,
                            }));
                        }
                        SasState::Done { .. } => {
                            enqueue(&events, json!({
                                "type": "verification_done",
                                "flow_id": sas_flow_id,
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        SasState::Cancelled(info) => {
                            enqueue(&events, json!({
                                "type": "verification_cancelled",
                                "flow_id": sas_flow_id,
                                "message": format!("{:?}", info.reason()),
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        _ => {}
                    }
                }
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": sas_flow_id,
                    "message": "Timed out waiting for SAS completion.",
                }));
                if let Ok(mut g) = sas_slot.lock() { *g = None; }
                if let Ok(mut g) = request_slot.lock() { *g = None; }
            });
        });
        Ok(String::new())
    })
}

fn ffi_sas_action(
    ptr: *mut c_void,
    flow_id: *const c_char,
    label: &'static str,
    action: fn(SasVerification)
        -> std::pin::Pin<Box<dyn std::future::Future<Output = matrix_sdk::Result<()>> + Send>>,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let entry = match bridge.active_sas.lock() {
            Ok(g) => g.clone(),
            Err(_) => None,
        };
        let Some((stored_flow, sas)) = entry else {
            return Ok("error: no active SAS verification.".to_owned());
        };
        if stored_flow != flow_id {
            return Ok("error: SAS verification flow id mismatch.".to_owned());
        }
        let events = Arc::clone(&bridge.events);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, label, async move {
                if let Err(err) = action(sas.clone()).await {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": stored_flow,
                        "message": format_matrix_error(label, err),
                    }));
                }
            });
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_confirm_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_sas_action(ptr, flow_id, "verification_confirm", |sas| Box::pin(async move {
        sas.confirm().await
    }))
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_mismatch_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_sas_action(ptr, flow_id, "verification_mismatch", |sas| Box::pin(async move {
        sas.mismatch().await
    }))
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_cancel_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    // Try SAS-level cancel first; if no active SAS, fall back to request-level.
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let sas_entry = bridge.active_sas.lock().ok().and_then(|g| g.clone());
        let request = bridge.active_request.lock().ok().and_then(|g| g.clone());
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "verification_cancel", async move {
                if let Some((stored_flow, sas)) = sas_entry {
                    if stored_flow == flow_id {
                        let _ = sas.cancel().await;
                        enqueue(&events, json!({
                            "type": "verification_cancelled",
                            "flow_id": flow_id,
                            "message": "cancelled",
                        }));
                    }
                } else if let Some(request) = request {
                    if request.flow_id() == flow_id {
                        let _ = request.cancel().await;
                        enqueue(&events, json!({
                            "type": "verification_cancelled",
                            "flow_id": flow_id,
                            "message": "cancelled",
                        }));
                    }
                }
                if let Ok(mut g) = sas_slot.lock() { *g = None; }
                if let Ok(mut g) = request_slot.lock() { *g = None; }
            });
        });
        Ok(String::new())
    })
}

/// Lightning-initiated (outbound) SAS verification of the current
/// session against another session belonging to the same Matrix
/// account. Advertises SAS as the only method so the SDK does not
/// send an m.qr_code.* request Lightning cannot follow through.
///
/// Emits `verification_request_started` synchronously (before the SDK
/// finishes sending) so the QML side can flip to "Waiting…"
/// immediately. Subsequent `verification_ready` / `verification_sas_ready`
/// / `verification_done` / `verification_cancelled` events follow the
/// same shape as the receive-first path — the C++ SAS state machine
/// is shared.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_own_verification(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        // Reject a duplicate start if a flow is already active.
        let has_active = bridge.active_request.lock().ok().and_then(|g| g.clone()).is_some()
            || bridge.active_sas.lock().ok().and_then(|g| g.clone()).is_some();
        if has_active {
            return Ok("error: A verification is already in progress.".to_owned());
        }

        let events = Arc::clone(&bridge.events);
        let request_slot = Arc::clone(&bridge.active_request);
        let sas_slot = Arc::clone(&bridge.active_sas);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "verification_start_own", async move {
                let Some(user_id) = client.user_id().map(|u| u.to_owned()) else {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": "",
                        "message": "Rust SDK client has no user id yet.",
                    }));
                    return;
                };

                // Look up own identity locally first; if missing, force a
                // /keys/query to refresh it (matches Element's flow).
                let identity_res = client.encryption().get_user_identity(&user_id).await;
                let identity = match identity_res {
                    Ok(Some(id)) => Some(id),
                    Ok(None) => match client.encryption().request_user_identity(&user_id).await {
                        Ok(id) => id,
                        Err(err) => {
                            enqueue(&events, json!({
                                "type": "verification_failed",
                                "flow_id": "",
                                "message": format_matrix_error(
                                    "own identity lookup failed", err),
                            }));
                            return;
                        }
                    },
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "verification_failed",
                            "flow_id": "",
                            "message": format_matrix_error(
                                "own identity lookup failed", err),
                        }));
                        return;
                    }
                };
                let Some(identity) = identity else {
                    enqueue(&events, json!({
                        "type": "verification_failed",
                        "flow_id": "",
                        "message": "This Matrix account has no cross-signing identity. Sign in with a session that has cross-signing set up first.",
                    }));
                    return;
                };

                let request = match identity
                    .request_verification_with_methods(vec![VerificationMethod::SasV1])
                    .await
                {
                    Ok(r) => r,
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "verification_failed",
                            "flow_id": "",
                            "message": format_matrix_error(
                                "verification request send failed", err),
                        }));
                        return;
                    }
                };

                let flow_id = request.flow_id().to_string();
                let other_user = request.other_user_id().to_string();
                let is_self = request.is_self_verification();

                if let Ok(mut g) = request_slot.lock() {
                    *g = Some(request.clone());
                }
                enqueue(&events, json!({
                    "type": "verification_request_started",
                    "flow_id": flow_id.clone(),
                    "other_user_id": other_user,
                    "is_self_verification": is_self,
                }));

                // Wait for the request to become ready (the peer accepts).
                // Poll for up to 5 minutes so the user can pick up the
                // request in Element without racing our timer.
                let mut ready = false;
                for _ in 0..600 {
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    if request.is_cancelled() || request.is_done() {
                        break;
                    }
                    if request.is_ready() {
                        ready = true;
                        break;
                    }
                }
                if !ready {
                    if !request.is_cancelled() && !request.is_done() {
                        let _ = request.cancel().await;
                    }
                    enqueue(&events, json!({
                        "type": "verification_cancelled",
                        "flow_id": flow_id,
                        "message": "timed_out_waiting_for_peer",
                    }));
                    if let Ok(mut g) = request_slot.lock() { *g = None; }
                    return;
                }

                enqueue(&events, json!({
                    "type": "verification_ready",
                    "flow_id": flow_id.clone(),
                }));

                // Start SAS exactly once. `start_sas` returns None when the
                // peer has already started; in that case fetch the current
                // Verification via get_verification and continue.
                let sas = match request.start_sas().await {
                    Ok(Some(sas)) => sas,
                    Ok(None) => {
                        let mut found: Option<SasVerification> = None;
                        for _ in 0..20 {
                            tokio::time::sleep(std::time::Duration::from_millis(250)).await;
                            if let Some(v) = client
                                .encryption()
                                .get_verification(request.other_user_id(), request.flow_id())
                                .await
                            {
                                if let matrix_sdk::encryption::verification::Verification::SasV1(s) = v {
                                    found = Some(s);
                                    break;
                                }
                            }
                        }
                        match found {
                            Some(s) => s,
                            None => {
                                enqueue(&events, json!({
                                    "type": "verification_failed",
                                    "flow_id": flow_id,
                                    "message": "SAS did not become available.",
                                }));
                                if let Ok(mut g) = request_slot.lock() { *g = None; }
                                return;
                            }
                        }
                    }
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "verification_failed",
                            "flow_id": flow_id,
                            "message": format_matrix_error(
                                "SAS start failed", err),
                        }));
                        if let Ok(mut g) = request_slot.lock() { *g = None; }
                        return;
                    }
                };

                if let Ok(mut g) = sas_slot.lock() {
                    *g = Some((flow_id.clone(), sas.clone()));
                }
                enqueue(&events, json!({
                    "type": "verification_sas_started",
                    "flow_id": flow_id.clone(),
                }));

                let mut emitted_emojis = false;
                for _ in 0..240 {
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    let state = sas.state();
                    match state {
                        SasState::KeysExchanged { .. } if !emitted_emojis => {
                            emitted_emojis = true;
                            let mut emoji_list: Vec<serde_json::Value> = Vec::new();
                            if let Some(emojis) = sas.emoji() {
                                for e in emojis.iter() {
                                    emoji_list.push(json!({
                                        "symbol": e.symbol,
                                        "description": e.description,
                                    }));
                                }
                            }
                            let dec = sas.decimals().map(|(a, b, c)| json!([a, b, c]))
                                .unwrap_or(json!([]));
                            enqueue(&events, json!({
                                "type": "verification_sas_ready",
                                "flow_id": flow_id.clone(),
                                "emojis": emoji_list,
                                "decimals": dec,
                            }));
                        }
                        SasState::Done { .. } => {
                            enqueue(&events, json!({
                                "type": "verification_done",
                                "flow_id": flow_id,
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        SasState::Cancelled(info) => {
                            enqueue(&events, json!({
                                "type": "verification_cancelled",
                                "flow_id": flow_id,
                                "message": format!("{:?}", info.reason()),
                            }));
                            if let Ok(mut g) = sas_slot.lock() { *g = None; }
                            if let Ok(mut g) = request_slot.lock() { *g = None; }
                            return;
                        }
                        _ => {}
                    }
                }
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": flow_id,
                    "message": "Timed out waiting for SAS completion.",
                }));
                if let Ok(mut g) = sas_slot.lock() { *g = None; }
                if let Ok(mut g) = request_slot.lock() { *g = None; }
            });
        });
        Ok(String::new())
    })
}

/// Report the cross-signing state of the current session. Only aggregate,
/// non-secret metadata crosses the FFI — device keys and signatures never
/// do. `device_cross_signed = true` is the source of truth for the
/// "Verified" label in the UI.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_query_own_device_status(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        // Run the async query on a short-lived Tokio runtime so we can
        // return the JSON synchronously — this is a status query, not a
        // long-running action.
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|err| format!("failed to build status runtime: {err}"))?;

        let result: serde_json::Value = runtime.block_on(async move {
            let user_id = client.user_id().map(|u| u.to_owned());
            let device_id_opt = client.device_id().map(|d| d.to_string());
            let mut own_identity_available = false;
            let mut own_identity_verified = false;
            let mut device_cross_signed = false;
            let mut has_master = false;
            let mut has_self_signing = false;
            let mut has_user_signing = false;
            if let Some(uid) = &user_id {
                if let Ok(Some(identity)) = client.encryption().get_user_identity(uid).await {
                    own_identity_available = true;
                    own_identity_verified = identity.is_verified();
                }
            }
            if let Ok(Some(device)) = client.encryption().get_own_device().await {
                device_cross_signed = device.is_cross_signed_by_owner();
            }
            if let Some(status) = client.encryption().cross_signing_status().await {
                has_master = status.has_master;
                has_self_signing = status.has_self_signing;
                has_user_signing = status.has_user_signing;
            }
            json!({
                "device_id": device_id_opt.unwrap_or_default(),
                "own_identity_available": own_identity_available,
                "own_identity_verified": own_identity_verified,
                "device_cross_signed": device_cross_signed,
                "has_master": has_master,
                "has_self_signing": has_self_signing,
                "has_user_signing": has_user_signing,
            })
        });
        Ok(serde_json::to_string(&result).unwrap_or_else(|_| "{}".to_owned()))
    })
}

/// Encrypted Megolm room-key import (v0.5.6). Delegates to
/// `Encryption::import_room_keys`, which internally uses
/// `matrix_sdk_base::crypto::decrypt_room_key_export` and imports the
/// resulting inbound room sessions into the active crypto store. The
/// SDK wraps the passphrase in `zeroize::Zeroizing` so it is scrubbed
/// after decrypt regardless of the return path.
///
/// Decrypted key material is NEVER returned to C++ — this FFI only
/// forwards aggregate result counts and (already-public) affected room
/// IDs so the UI can reprocess timelines.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_import_room_keys(
    ptr: *mut c_void,
    file_path: *const c_char,
    passphrase: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let file_path = unsafe { cstr_arg(file_path) }?;
        // Read passphrase into an owned String and immediately drop the
        // C string reference; matrix-sdk wraps it in Zeroizing before
        // decrypt so this buffer is the only copy we ever keep.
        let passphrase = unsafe { cstr_arg(passphrase) }?;

        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        if file_path.trim().is_empty() {
            return Ok("error: The selected file path is empty.".to_owned());
        }

        // Serialize imports through an atomic flag.
        if bridge
            .import_active
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            let events = Arc::clone(&bridge.events);
            enqueue(&events, json!({
                "type": "room_key_import_failed",
                "category": "already_running",
                "message": "A room-key import is already in progress.",
            }));
            return Ok(String::new());
        }

        let events = Arc::clone(&bridge.events);
        let import_flag = Arc::clone(&bridge.import_active);
        let registry = Arc::clone(&bridge.timelines);
        // v0.5.7: the import is a managed task on the shared runtime.
        // Sign-out joins this handle deterministically instead of polling
        // the import_active flag against a wall clock.
        let handle = bridge.runtime.spawn(async move {
                enqueue(&events, json!({
                    "type": "room_key_import_started",
                }));
                let path_buf = PathBuf::from(&file_path);
                // Quick pre-flight so we can emit clearer errors than the
                // SDK does when e.g. the user picks a directory.
                match std::fs::metadata(&path_buf) {
                    Ok(md) if md.is_dir() => {
                        enqueue(&events, json!({
                            "type": "room_key_import_failed",
                            "category": "invalid_file",
                            "message": "Selected path is a directory, not a file.",
                        }));
                        import_flag.store(false, Ordering::SeqCst);
                        return;
                    }
                    Ok(_) => {}
                    Err(err) => {
                        enqueue(&events, json!({
                            "type": "room_key_import_failed",
                            "category": "read_failed",
                            "message": format!("Failed to read the selected file: {err}"),
                        }));
                        import_flag.store(false, Ordering::SeqCst);
                        return;
                    }
                }

                let result = client
                    .encryption()
                    .import_room_keys(path_buf, &passphrase)
                    .await;
                // `passphrase` goes out of scope here — no explicit
                // zeroize on this side (matrix-sdk holds the Zeroizing
                // copy). We deliberately never route this value into a
                // log.
                match result {
                    Ok(import) => {
                        let mut room_ids: Vec<String> = Vec::new();
                        for room in import.keys.keys() {
                            room_ids.push(room.to_string());
                        }
                        enqueue(&events, json!({
                            "type": "room_key_import_progress",
                            "imported": import.imported_count,
                            "total": import.total_count,
                        }));
                        enqueue(&events, json!({
                            "type": "room_key_import_done",
                            "imported": import.imported_count,
                            "total": import.total_count,
                            "affected_rooms": room_ids.len(),
                            "room_ids": room_ids,
                        }));
                        // v0.5.7: immediate decryption retry. The imported
                        // Megolm session IDs stay inside Rust; only counts
                        // and (public) room ids ever cross the FFI. The SDK
                        // timeline emits in-place Set diffs for every item
                        // it can now decrypt — no restart, no manual
                        // refresh, no room switch.
                        let sessions_by_room =
                            timeline::sessions_by_room_from_import(&import.keys);
                        registry
                            .retry_decryption_after_import(&sessions_by_room)
                            .await;
                    }
                    Err(err) => {
                        let display = err.to_string();
                        let category = classify_import_error(&display);
                        enqueue(&events, json!({
                            "type": "room_key_import_failed",
                            "category": category,
                            "message": display,
                        }));
                    }
                }
                import_flag.store(false, Ordering::SeqCst);
        });
        if let Ok(mut guard) = bridge.import_task.lock() {
            *guard = Some(handle);
        }
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_key_import_active(
    ptr: *mut c_void,
) -> c_int {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return 0;
        };
        if bridge.import_active.load(Ordering::SeqCst) { 1 } else { 0 }
    }));
    result.unwrap_or(0)
}

// ---------------------------------------------------------------------------
// v0.5.7: live SDK timeline FFI. All functions return "" when the command was
// accepted for async execution or "error: …" synchronously. Results arrive on
// the poll queue as timeline_reset / timeline_diff / timeline_pagination /
// timeline_send_failed / timeline_retry_decryption / timeline_error events,
// each stamped with room_generation + lifecycle for stale-callback rejection.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_open(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        if room_id.trim().is_empty() {
            return Err("empty room id".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge.timelines.open_room(&bridge.runtime, client, room_id);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close();
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_paginate_back(
    ptr: *mut c_void,
    room_id: *const c_char,
    count: u16,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        bridge
            .timelines
            .paginate_back(&bridge.runtime, room_id, count)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_send_text(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        bridge
            .timelines
            .send_text(&bridge.runtime, room_id, body)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_send_reply(
    ptr: *mut c_void,
    room_id: *const c_char,
    in_reply_to_event_id: *const c_char,
    body: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let reply_to = unsafe { cstr_arg(in_reply_to_event_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        bridge
            .timelines
            .send_reply(&bridge.runtime, room_id, reply_to, body)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_edit(
    ptr: *mut c_void,
    room_id: *const c_char,
    target_event_id: *const c_char,
    new_body: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let new_body = unsafe { cstr_arg(new_body) }?;
        bridge
            .timelines
            .edit(&bridge.runtime, room_id, target, new_body)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_toggle_reaction(
    ptr: *mut c_void,
    room_id: *const c_char,
    target_event_id: *const c_char,
    key: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let key = unsafe { cstr_arg(key) }?;
        bridge
            .timelines
            .toggle_reaction(&bridge.runtime, room_id, target, key)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_redact(
    ptr: *mut c_void,
    room_id: *const c_char,
    target_event_id: *const c_char,
    reason: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let reason = unsafe { cstr_arg(reason) }?;
        bridge
            .timelines
            .redact(&bridge.runtime, room_id, target, reason)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_retry_send(
    ptr: *mut c_void,
    room_id: *const c_char,
    transaction_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let txn = unsafe { cstr_arg(transaction_id) }?;
        bridge
            .timelines
            .retry_send(&bridge.runtime, room_id, txn)
            .map(|_| String::new())
    })
}

// ---------------------------------------------------------------------------
// v0.5.9: room management, user search and the media bridge. Wrappers only —
// all logic (validation, spawning, event emission) lives in `rooms.rs`.
// `op_id` values are generated by C++ and echoed back on every async result.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_users(
    ptr: *mut c_void,
    query: *const c_char,
    limit: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let query = unsafe { cstr_arg(query) }?;
        rooms::search_users(bridge, query, limit, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_get_user_profile(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        rooms::fetch_user_profile(bridge, user_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_get_url_preview(
    ptr: *mut c_void,
    url: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let url = unsafe { cstr_arg(url) }?;
        rooms::fetch_url_preview(bridge, url, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_get_dm_rooms(
    ptr: *mut c_void,
    user_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        rooms::get_dm_rooms(bridge, &user_id)
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_create_dm(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        rooms::create_dm(bridge, user_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_create_room(
    ptr: *mut c_void,
    options_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let options = unsafe { cstr_arg(options_json) }?;
        rooms::create_room(bridge, options, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_invite_users(
    ptr: *mut c_void,
    room_id: *const c_char,
    users_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let users = unsafe { cstr_arg(users_json) }?;
        rooms::invite_users(bridge, room_id, users, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_members(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::room_members(bridge, room_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_name(
    ptr: *mut c_void,
    room_id: *const c_char,
    name: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let name = unsafe { cstr_arg(name) }?;
        rooms::set_room_name(bridge, room_id, name, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_topic(
    ptr: *mut c_void,
    room_id: *const c_char,
    topic: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let topic = unsafe { cstr_arg(topic) }?;
        rooms::set_room_topic(bridge, room_id, topic, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_avatar(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        rooms::set_room_avatar(bridge, room_id, local_path, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_remove_room_avatar(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::remove_room_avatar(bridge, room_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_leave_room(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::leave_room(bridge, room_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_add_room_to_space(
    ptr: *mut c_void,
    space_id: *const c_char,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let space_id = unsafe { cstr_arg(space_id) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::add_room_to_space(bridge, space_id, room_id, op_id).map(|_| String::new())
    })
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_timeline_send_attachment(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    animated: c_int,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        rooms::send_attachment_path(
            bridge,
            room_id,
            local_path,
            mime,
            caption,
            width,
            height,
            animated != 0,
            op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_timeline_send_attachment_bytes(
    ptr: *mut c_void,
    room_id: *const c_char,
    data: *const u8,
    len: usize,
    filename: *const c_char,
    mime: *const c_char,
    width: u64,
    height: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let filename = unsafe { cstr_arg(filename) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if data.is_null() || len == 0 {
            return Err("attachment data is empty".to_owned());
        }
        // One bounded copy into Rust-owned memory; C++ frees its buffer
        // immediately after this call returns.
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        rooms::send_attachment_bytes(
            bridge, room_id, bytes, filename, mime, width, height, op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_fetch(
    ptr: *mut c_void,
    key: *const c_char,
    kind: u32,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let key = unsafe { cstr_arg(key) }?;
        rooms::media_fetch(bridge, key, kind, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_fetch_mxc(
    ptr: *mut c_void,
    mxc: *const c_char,
    width: u64,
    height: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let mxc = unsafe { cstr_arg(mxc) }?;
        rooms::media_fetch_mxc(bridge, mxc, width, height, op_id).map(|_| String::new())
    })
}

/// Move a parked media payload out of the bridge. Returns a heap buffer the
/// caller MUST release with `mx_rust_media_free`, or null when the op id is
/// unknown (stale/duplicate take). This is the only path media bytes take
/// across the FFI — they never enter the JSON event queue.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_take(
    ptr: *mut c_void,
    op_id: u64,
    out_len: *mut usize,
) -> *mut u8 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if out_len.is_null() {
            return std::ptr::null_mut();
        }
        unsafe { *out_len = 0 };
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return std::ptr::null_mut();
        };
        let Some(bytes) = bridge
            .media_results
            .lock()
            .ok()
            .and_then(|mut guard| guard.remove(&op_id))
        else {
            return std::ptr::null_mut();
        };
        let boxed: Box<[u8]> = bytes.into_boxed_slice();
        unsafe { *out_len = boxed.len() };
        Box::into_raw(boxed) as *mut u8
    }));
    result.unwrap_or(std::ptr::null_mut())
}

/// Release a buffer returned by `mx_rust_media_take`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_free(data: *mut u8, len: usize) {
    if data.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let slice = std::ptr::slice_from_raw_parts_mut(data, len);
        unsafe { drop(Box::from_raw(slice)) };
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_upload_limit(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        rooms::fetch_upload_limit(bridge).map(|_| String::new())
    })
}

/// Deterministic shutdown of all managed async work (timeline
/// subscriptions, room-key import, sync). Called by C++ before server
/// logout and store cleanup so nothing still owns the crypto store when it
/// is deleted. Returns a short safe status string for logging.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_shutdown_tasks(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let (import_joined, sync_stopped) = bridge.shutdown_managed_tasks();
        Ok(format!(
            "import_joined={import_joined} sync_stopped={sync_stopped}"
        ))
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_free_cstring(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        unsafe {
            drop(CString::from_raw(ptr));
        }
    }));
}

async fn build_client(homeserver: &str, store_path: &Path) -> Result<Client, String> {
    Client::builder()
        .homeserver_url(homeserver)
        .sqlite_store(store_path, None)
        .user_agent("Lightning/0.5.14")
        .build()
        .await
        .map_err(|err| format_matrix_error("failed to build Matrix Rust SDK client", err))
}

async fn restore_client(
    homeserver: &str,
    store_path: &Path,
    user_id: &str,
    device_id: &str,
    access_token: String,
) -> Result<Client, String> {
    let user_id: OwnedUserId = UserId::parse(user_id)
        .map_err(|err| format!("invalid stored Matrix user id: {err}"))?
        .to_owned();
    let device_id: OwnedDeviceId = device_id.to_owned().into();
    let session = MatrixSession {
        meta: SessionMeta { user_id, device_id },
        tokens: SessionTokens { access_token, refresh_token: None },
    };
    restore_client_with_session(homeserver, store_path, session).await
}

async fn restore_client_with_session(
    homeserver: &str,
    store_path: &Path,
    session: MatrixSession,
) -> Result<Client, String> {
    let client = build_client(homeserver, store_path).await?;
    client
        .matrix_auth()
        .restore_session(session, RoomLoadSettings::default())
        .await
        .map_err(|err| format_matrix_error("Matrix Rust SDK session restore failed", err))?;
    Ok(client)
}

fn configured_session_file(slot: &Arc<Mutex<Option<PathBuf>>>) -> Option<PathBuf> {
    slot.lock().ok().and_then(|guard| guard.clone())
}

fn read_persistent_session(path: &Path) -> Result<PersistentSessionFile, String> {
    let bytes = std::fs::read(path)
        .map_err(|err| format!("failed to read session file: {err}"))?;
    serde_json::from_slice(&bytes)
        .map_err(|err| format!("failed to parse session file: {err}"))
}

fn save_persistent_session(
    path: &Path,
    homeserver: &str,
    session: &MatrixSession,
) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|err| format!("failed to create session directory: {err}"))?;
    }

    let data = PersistentSessionFile {
        version: 1,
        homeserver: homeserver.to_owned(),
        session: session.clone(),
    };
    let bytes = serde_json::to_vec(&data)
        .map_err(|err| format!("failed to serialize session: {err}"))?;
    let tmp = path.with_extension("json.tmp");

    let mut options = OpenOptions::new();
    options.create(true).write(true).truncate(true);
    #[cfg(unix)]
    {
        options.mode(0o600);
    }
    let mut file = options
        .open(&tmp)
        .map_err(|err| format!("failed to open session file: {err}"))?;
    file.write_all(&bytes)
        .map_err(|err| format!("failed to write session file: {err}"))?;
    file.write_all(b"\n")
        .map_err(|err| format!("failed to finish session file: {err}"))?;
    file.sync_all()
        .map_err(|err| format!("failed to flush session file: {err}"))?;
    drop(file);

    std::fs::rename(&tmp, path)
        .map_err(|err| format!("failed to install session file: {err}"))?;
    #[cfg(unix)]
    {
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))
            .map_err(|err| format!("failed to restrict session file permissions: {err}"))?;
    }
    Ok(())
}

fn install_event_handlers(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
) {
    // v0.5.0: SAS emoji verification, receive-first. matrix-sdk 0.18 does
    // NOT expose a public `recv_verification_requests` stream, so we
    // observe incoming requests via a to-device event handler and then
    // hydrate the `VerificationRequest` via
    // `client.encryption().get_verification_request(user, flow_id)`.
    //
    // The active flow (single-flow policy) is stored in
    // `active_request`; the FFI accept path drives `accept()` +
    // `start_sas()` from that stored handle. No secret material is
    // ever forwarded through the FFI — only flow id, mxid, device id,
    // is_self_verification, and SAS emojis (which are safe to display
    // by SAS design).
    let verif_events = Arc::clone(&events);
    let verif_slot = Arc::clone(&active_request);
    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: ToDeviceKeyVerificationRequestEvent| {
            let events = Arc::clone(&verif_events);
            let slot = Arc::clone(&verif_slot);
            let client = client_clone.clone();
            async move {
                let flow_id = ev.content.transaction_id.to_string();
                let Some(request) = client
                    .encryption()
                    .get_verification_request(&ev.sender, &flow_id)
                    .await
                else {
                    return;
                };
                let other_device = request
                    .their_supported_methods()
                    .map(|_| String::new())
                    .unwrap_or_default();
                enqueue(
                    &events,
                    json!({
                        "type": "verification_request_received",
                        "flow_id": request.flow_id().to_string(),
                        "other_user_id": request.other_user_id().to_string(),
                        "other_device_id": other_device,
                        "is_self_verification": request.is_self_verification(),
                        "we_started": request.we_started(),
                    }),
                );
                if let Ok(mut g) = slot.lock() {
                    *g = Some(request);
                }
            }
        },
    );

    // m.typing is a replacement event: every payload completely replaces the
    // room's previous typing set. Bound display metadata resolution while
    // preserving enough IDs for useful one/two/many UI formatting.
    let typing_events = Arc::clone(&events);
    let own_user = client.user_id().map(ToOwned::to_owned);
    client.add_event_handler(move |ev: SyncTypingEvent, room: Room| {
        let events = Arc::clone(&typing_events);
        let own_user = own_user.clone();
        async move {
            let mut users = Vec::new();
            for user_id in ev.content.user_ids.into_iter()
                .filter(|user_id| Some(user_id) != own_user.as_ref())
                .take(32) {
                let display_name = room.get_member_no_sync(&user_id).await.ok().flatten()
                    .map(|member| member.name().to_owned()).unwrap_or_default();
                users.push(json!({
                    "user_id": user_id.to_string(), "display_name": display_name
                }));
            }
            enqueue(&events, json!({
                "type": "typing_update", "room_id": room.room_id().to_string(),
                "users": users,
            }));
        }
    });
    // Decrypted (or plaintext) room messages — the SDK dispatches this handler
    // for both. `encryption_info` is Some(...) only when the SDK decrypted the
    // payload; when Some, the event on the wire was m.room.encrypted and the
    // SDK produced usable plaintext. That distinction becomes the
    // (is_encrypted, is_decrypted) pair on the C++ side.
    //
    // The body / ciphertext boundary is enforced on the Rust side: we only
    // ever forward plaintext bodies here (decrypted or already-plaintext), and
    // we never forward ciphertext at all — the encrypted handler below emits
    // an empty body + undecryptable flag instead.
    let plaintext_events = Arc::clone(&events);
    client.add_event_handler(
        move |ev: OriginalSyncRoomMessageEvent,
              room: Room,
              encryption_info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            let events = Arc::clone(&plaintext_events);
            async move {
                let (kind, body) = match &ev.content.msgtype {
                    MessageType::Text(content) => ("text", content.body.clone()),
                    MessageType::Notice(content) => ("notice", content.body.clone()),
                    MessageType::Emote(content) => ("emote", content.body.clone()),
                    _ => return,
                };

                let is_encrypted = encryption_info.is_some();
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_event",
                        "room_id": room.room_id().to_string(),
                        "event": {
                            "event_id": ev.event_id.to_string(),
                            "sender": ev.sender.to_string(),
                            "body": body,
                            "msgtype": kind,
                            "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                            "is_encrypted": is_encrypted,
                            "is_decrypted": is_encrypted,
                            "undecryptable": false,
                            // Kept for backward compat with C++ builds that
                            // still read `decrypted` — remove after prep+6.
                            "decrypted": is_encrypted,
                        },
                    }),
                );
            }
        },
    );

    // Encrypted room messages the SDK could NOT decrypt. Without this handler
    // undecryptable events silently disappear and the room looks empty even
    // though messages are arriving. Emit a placeholder timeline event tagged
    // `undecryptable = true` so C++ can render an honest
    // "[unable to decrypt yet]" bubble. We deliberately do NOT include the
    // ciphertext in the payload — the C++ side never needs it. The
    // `error_kind` is a coarse hint the UI can display later; today we always
    // emit "no_key" because the SDK doesn't expose a finer reason on this
    // path in v0.18 without more work.
    let encrypted_events = Arc::clone(&events);
    client.add_event_handler(move |ev: OriginalSyncRoomEncryptedEvent, room: Room| {
        let events = Arc::clone(&encrypted_events);
        async move {
            enqueue(
                &events,
                json!({
                    "type": "timeline_event",
                    "room_id": room.room_id().to_string(),
                    "event": {
                        "event_id": ev.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        // Empty body triggers the placeholder in C++.
                        "body": "",
                        "msgtype": "encrypted",
                        "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                        "is_encrypted": true,
                        "is_decrypted": false,
                        "undecryptable": true,
                        "error_kind": "no_key",
                        // Backward compat with prep+5 C++ builds.
                        "decrypted": false,
                    },
                }),
            );
        }
    });
}

async fn run_authoritative_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) {
    set_sync_mode(&sync_mode, &events, SyncMode::Probing, None);

    // Probe the exact capability consumed by matrix-sdk 0.18's native
    // Sliding Sync v5 endpoint. A failed /versions request is connectivity,
    // not proof of incompatibility: the mode stays `Probing` (no downgrade,
    // no flicker) and only the connection indicator reports offline while
    // we retry. Cancellation exits the probe immediately.
    let modern_supported = loop {
        let probe = tokio::select! {
            _ = &mut cancel => return,
            result = client.supported_versions() => result,
        };
        match probe {
            Ok(versions) => break versions.features.contains(&FeatureFlag::Msc4186),
            Err(_) => {
                enqueue(&events, json!({
                    "type": "room_list_sync_state", "state": "offline"
                }));
                tokio::select! {
                    _ = &mut cancel => return,
                    _ = tokio::time::sleep(std::time::Duration::from_secs(3)) => {}
                }
            }
        }
    };

    if modern_supported {
        if let Some(cancel) = run_modern_sync(
            client.clone(), Arc::clone(&events), Arc::clone(&sync_mode), cancel
        ).await {
            set_sync_mode(
                &sync_mode, &events, SyncMode::ClassicSyncFallback, Some("unsupported")
            );
            run_classic_sync(client, events, cancel).await;
        }
    } else {
        set_sync_mode(
            &sync_mode, &events, SyncMode::ClassicSyncFallback, Some("unsupported")
        );
        run_classic_sync(client, events, cancel).await;
    }
}

fn unified_error_kind(error: &UnifiedSyncError) -> Option<&ErrorKind> {
    use matrix_sdk_ui::{encryption_sync_service, room_list_service};
    match error {
        UnifiedSyncError::RoomList(room_list_service::Error::SlidingSync(error)) =>
            error.client_api_error_kind(),
        UnifiedSyncError::EncryptionSync(encryption_sync_service::Error::SlidingSync(error))
        | UnifiedSyncError::EncryptionSync(encryption_sync_service::Error::LockError(error))
        | UnifiedSyncError::EncryptionSync(encryption_sync_service::Error::ClientError(error)) =>
            error.client_api_error_kind(),
        _ => None,
    }
}

fn unsupported_modern_error(error: &UnifiedSyncError) -> bool {
    matches!(unified_error_kind(error), Some(ErrorKind::Unrecognized | ErrorKind::NotFound))
}

fn authentication_error(error: &UnifiedSyncError) -> bool {
    matches!(unified_error_kind(error), Some(ErrorKind::UnknownToken { .. } | ErrorKind::Forbidden))
}

/// Runs matrix-sdk-ui's unified supervisor. Its `EncryptionSyncPermit`
/// guarantees exactly one encryption Sliding Sync while the room-list sync is
/// active. Returning `Some(cancel)` is the only path allowed to start classic
/// sync and happens solely for a verified unsupported endpoint error.
async fn run_modern_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) -> Option<tokio::sync::oneshot::Receiver<()>> {
    loop {
        let service = match SyncService::builder(client.clone()).build().await {
            Ok(service) => service,
            Err(error) if unsupported_modern_error(&error) => return Some(cancel),
            Err(error) => {
                enqueue(&events, json!({
                    "type": "room_list_error", "category": if authentication_error(&error) {
                        "authentication"
                    } else { "temporary" }
                }));
                if authentication_error(&error) {
                    set_sync_mode(&sync_mode, &events, SyncMode::Failed, None);
                    let _ = (&mut cancel).await;
                    return None;
                }
                tokio::select! {
                    _ = &mut cancel => return None,
                    _ = tokio::time::sleep(std::time::Duration::from_secs(3)) => continue,
                }
            }
        };

        let room_list_service = service.room_list_service();
        let room_list = match room_list_service.all_rooms().await {
            Ok(list) => list,
            Err(_) => {
                enqueue(&events, json!({ "type": "room_list_error", "category": "setup" }));
                let _ = (&mut cancel).await;
                return None;
            }
        };
        let (entries, controller) = room_list.entries_with_dynamic_adapters(10_000);
        controller.set_filter(Box::new(filters::new_filter_non_left()));
        tokio::pin!(entries);

        let space_service = SpaceService::new(client.clone()).await;
        let mut unified_state = service.state();
        let mut list_state = room_list_service.state();

        set_sync_mode(&sync_mode, &events, SyncMode::SlidingSync, None);
        enqueue(&events, json!({ "type": "room_list_sync_state", "state": "starting" }));
        service.start().await;
        let mut first_sync = true;
        loop {
            tokio::select! {
                _ = &mut cancel => {
                    service.stop().await;
                    return None;
                }
                batch = entries.next() => {
                    let Some(batch) = batch else { break; };
                    forward_room_list_diffs(&events, batch).await;
                    enqueue_spaces(&events, &space_service, &client).await;
                }
                state = list_state.next() => {
                    if let Some(matrix_sdk_ui::room_list_service::State::Running) = state {
                        enqueue(&events, json!({
                            "type": "room_list_sync_state", "state": "running"
                        }));
                        if first_sync {
                            first_sync = false;
                            enqueue(&events, json!({ "type": "initial_sync_done" }));
                        }
                    }
                }
                state = unified_state.next() => {
                    match state {
                        // Transient connectivity loss keeps the selected mode
                        // (SlidingSync) so the label does not flicker; only the
                        // connection indicator reports offline. The supervisor
                        // reconnects on its own.
                        Some(UnifiedSyncState::Offline) => {
                            enqueue(&events, json!({
                                "type": "room_list_sync_state", "state": "offline"
                            }));
                        }
                        Some(UnifiedSyncState::Error(error)) => {
                            service.stop().await;
                            // The ONLY path allowed to start classic sync: a
                            // positively-classified unsupported endpoint.
                            if unsupported_modern_error(&error) { return Some(cancel); }
                            // Authentication failure is fatal — never downgrade,
                            // never sleep-retry; wait for an explicit stop.
                            if authentication_error(&error) {
                                set_sync_mode(&sync_mode, &events, SyncMode::Failed, None);
                                enqueue(&events, json!({
                                    "type": "room_list_error", "category": "authentication"
                                }));
                                let _ = (&mut cancel).await;
                                return None;
                            }
                            // Any other error is treated as transient: keep the
                            // SlidingSync mode, report offline, and rebuild.
                            enqueue(&events, json!({
                                "type": "room_list_sync_state", "state": "offline"
                            }));
                            break;
                        }
                        Some(UnifiedSyncState::Terminated) => break,
                        _ => {}
                    }
                }
            }
        }

        service.stop().await;
        // Bounded backoff before rebuilding the supervisor. Cancellation exits
        // immediately; there is no busy loop. The mode stays SlidingSync (the
        // next iteration re-affirms it, deduped) so only the connection state
        // reflects the transient retry.
        tokio::select! {
            _ = &mut cancel => return None,
            _ = tokio::time::sleep(std::time::Duration::from_secs(3)) => {
                enqueue(&events, json!({
                    "type": "room_list_sync_state", "state": "retrying"
                }));
            }
        }
    }
}

async fn run_classic_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) {
    enqueue(&events, json!({ "type": "room_list_sync_state", "state": "starting" }));
    let first_response = Arc::new(AtomicBool::new(true));
    let settings = SyncSettings::default().ignore_timeout_on_first_sync(true).full_state(true);
    let callback_client = client.clone();
    let callback_events = Arc::clone(&events);
    let callback_first = Arc::clone(&first_response);
    let sync = client.sync_with_callback(settings, move |_response| {
        let client = callback_client.clone();
        let events = Arc::clone(&callback_events);
        let first_response = Arc::clone(&callback_first);
        async move {
            enqueue_rooms(&events, &client).await;
            let spaces = SpaceService::new(client.clone()).await;
            enqueue_spaces(&events, &spaces, &client).await;
            enqueue(&events, json!({ "type": "room_list_sync_state", "state": "running" }));
            if first_response.swap(false, Ordering::SeqCst) {
                enqueue(&events, json!({ "type": "initial_sync_done" }));
            }
            LoopCtrl::Continue
        }
    });
    tokio::pin!(sync);
    tokio::select! {
        result = &mut sync => if let Err(err) = result {
            enqueue(&events, json!({
                "type": "sync_error",
                "message": format_matrix_error("Matrix Rust SDK sync failed", err),
            }));
        },
        _ = &mut cancel => {}
    }
}

async fn forward_room_list_diffs(
    events: &Arc<Mutex<VecDeque<String>>>,
    batches: Vec<VectorDiff<RoomListItem>>,
) {
    for diff in batches {
        let value = match diff {
            VectorDiff::Reset { values } => {
                let mut rooms = Vec::with_capacity(values.len());
                for item in values { rooms.push(room_payload(&item.into_inner()).await); }
                json!({ "type": "room_list_reset", "rooms": rooms })
            }
            VectorDiff::Append { values } => {
                let mut rooms = Vec::with_capacity(values.len());
                for item in values { rooms.push(room_payload(&item.into_inner()).await); }
                json!({ "type": "room_list_append", "rooms": rooms })
            }
            VectorDiff::PushFront { value } => json!({
                "type": "room_list_push_front", "room": room_payload(&value.into_inner()).await
            }),
            VectorDiff::PushBack { value } => json!({
                "type": "room_list_push_back", "room": room_payload(&value.into_inner()).await
            }),
            VectorDiff::PopFront => json!({ "type": "room_list_pop_front" }),
            VectorDiff::PopBack => json!({ "type": "room_list_pop_back" }),
            VectorDiff::Insert { index, value } => json!({
                "type": "room_list_insert", "index": index,
                "room": room_payload(&value.into_inner()).await
            }),
            VectorDiff::Set { index, value } => json!({
                "type": "room_list_set", "index": index,
                "room": room_payload(&value.into_inner()).await
            }),
            VectorDiff::Remove { index } => json!({ "type": "room_list_remove", "index": index }),
            VectorDiff::Truncate { length } => json!({
                "type": "room_list_truncate", "length": length
            }),
            VectorDiff::Clear => json!({ "type": "room_list_clear" }),
        };
        enqueue(events, value);
    }
}

async fn enqueue_spaces(
    events: &Arc<Mutex<VecDeque<String>>>,
    service: &SpaceService,
    client: &Client,
) {
    let filters = service.space_filters().await;
    let joined_spaces = client.joined_space_rooms();
    let space_ids: BTreeSet<String> = joined_spaces.iter()
        .map(|room| room.room_id().to_string()).collect();
    let mut parents_by_child = HashMap::<String, Vec<String>>::new();
    let mut children_by_parent = HashMap::<String, BTreeSet<String>>::new();

    // Ask SpaceService's cycle-pruned graph for every known joined room's
    // parents. This extends its two presentation-level filters into a full
    // selectable hierarchy without reparsing raw state in Lightning.
    for room in client.joined_rooms() {
        let child_id = room.room_id().to_string();
        let parents: Vec<String> = service.joined_parents_of_child(room.room_id()).await
            .into_iter().map(|parent| parent.room_id.to_string()).collect();
        for parent in &parents {
            children_by_parent.entry(parent.clone()).or_default().insert(child_id.clone());
        }
        parents_by_child.insert(child_id, parents);
    }
    // Preserve inaccessible/unjoined identifiers exposed by the pinned
    // SpaceFilter even though they cannot become visible room rows.
    for filter in &filters {
        let parent = filter.space_room.room_id.to_string();
        children_by_parent.entry(parent).or_default()
            .extend(filter.descendants.iter().map(ToString::to_string));
    }

    let mut ordered_ids: Vec<String> = filters.iter()
        .map(|filter| filter.space_room.room_id.to_string()).collect();
    for room in &joined_spaces {
        let id = room.room_id().to_string();
        if !ordered_ids.contains(&id) { ordered_ids.push(id); }
    }

    let mut spaces = Vec::with_capacity(ordered_ids.len());
    for id in ordered_ids {
        let Some(room) = joined_spaces.iter().find(|room| room.room_id().as_str() == id) else {
            continue;
        };
        let parents = parents_by_child.get(&id).cloned().unwrap_or_default();
        let mut descendants = BTreeSet::new();
        let mut pending: Vec<(String, usize)> = children_by_parent.get(&id)
            .into_iter().flat_map(|children| children.iter().cloned())
            .map(|child| (child, 1)).collect();
        while let Some((child, depth)) = pending.pop() {
            if depth > 64 || child == id || !descendants.insert(child.clone()) { continue; }
            if let Some(nested) = children_by_parent.get(&child) {
                pending.extend(nested.iter().cloned().map(|value| (value, depth + 1)));
            }
        }
        let child_spaces: Vec<String> = children_by_parent.get(&id).into_iter()
            .flat_map(|children| children.iter())
            .filter(|child| space_ids.contains(*child)).cloned().collect();
        let level = filters.iter().find(|filter| filter.space_room.room_id.as_str() == id)
            .map(|filter| filter.level).unwrap_or(2);
        spaces.push(json!({
            "id": id,
            "name": room_name(room).await,
            "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
            "parents": parents,
            "child_spaces": child_spaces,
            "descendants": descendants,
            "level": level,
        }));
    }
    enqueue(events, json!({ "type": "space_list_reset", "spaces": spaces }));
}

async fn room_payload(room: &Room) -> serde_json::Value {
    let membership = match room.state() {
        matrix_sdk::RoomState::Joined => "joined",
        matrix_sdk::RoomState::Invited => "invited",
        matrix_sdk::RoomState::Knocked => "knocked",
        matrix_sdk::RoomState::Left | matrix_sdk::RoomState::Banned => "left",
    };
    let direct_targets: Vec<String> = room.direct_targets().iter().map(ToString::to_string).collect();
    let notifications = room.unread_notification_counts();
    let (inviter_user_id, inviter_display_name) = if membership == "invited" {
        match room.invite_details().await {
            Ok(invite) => (
                invite.inviter_id.to_string(),
                invite.inviter.map(|member| member.name().to_owned()).unwrap_or_default(),
            ),
            Err(_) => (String::new(), String::new()),
        }
    } else { (String::new(), String::new()) };

    json!({
        "id": room.room_id().to_string(),
        "membership": membership,
        "name": room_name(room).await,
        "canonical_alias": room.canonical_alias().map(|alias| alias.to_string()).unwrap_or_default(),
        "topic": room.topic().unwrap_or_default(),
        "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
        "last_message_preview": "",
        "last_activity_ms": room.latest_event_timestamp().map(|ts| u64::from(ts.get())).unwrap_or(0),
        "unread_count": room.num_unread_notifications().max(notifications.notification_count),
        "highlight_count": room.num_unread_mentions().max(notifications.highlight_count),
        "marked_unread": room.is_marked_unread(),
        "has_unread_messages": room.num_unread_messages() > 0,
        "encrypted": room.encryption_state().is_encrypted(),
        "is_space": room.is_space(),
        "is_direct": !direct_targets.is_empty(),
        "direct_user_id": direct_targets.first().cloned().unwrap_or_default(),
        "direct_user_ids": direct_targets,
        "member_count": room.joined_members_count(),
        "room_type": room.room_type().map(|kind| kind.to_string()).unwrap_or_default(),
        "prev_batch": room.last_prev_batch().unwrap_or_default(),
        "inviter_user_id": inviter_user_id,
        "inviter_display_name": inviter_display_name,
    })
}

async fn enqueue_rooms(events: &Arc<Mutex<VecDeque<String>>>, client: &Client) {
    let mut out = Vec::new();
    for room in client.rooms() {
        if matches!(room.state(), matrix_sdk::RoomState::Joined
            | matrix_sdk::RoomState::Invited | matrix_sdk::RoomState::Knocked) {
            out.push(room_payload(&room).await);
        }
    }
    enqueue(events, json!({ "type": "room_list_reset", "rooms": out }));
}

async fn room_name(room: &Room) -> String {
    if let Some(name) = room.name() {
        if !name.is_empty() {
            return name.to_owned();
        }
    }

    match room.display_name().await {
        Ok(matrix_sdk::RoomDisplayName::Named(name))
        | Ok(matrix_sdk::RoomDisplayName::Aliased(name))
        | Ok(matrix_sdk::RoomDisplayName::Calculated(name))
        | Ok(matrix_sdk::RoomDisplayName::EmptyWas(name)) if !name.is_empty() => name,
        _ => room.room_id().to_string(),
    }
}

/// Upper bound on the FFI event queue so we never grow without limit if the
/// C++ poll timer stalls (e.g. UI thread stuck under load). When we hit the
/// cap we drop the OLDEST events and inject a single `queue_overflow`
/// notice so the C++ side can log/surface the loss. The cap is generous —
/// well above the burst of `rooms` + `timeline_event` we produce on
/// initial sync of a real account.
const EVENT_QUEUE_CAP: usize = 4096;

pub(crate) fn enqueue(events: &Arc<Mutex<VecDeque<String>>>, value: serde_json::Value) {
    let Ok(serialized) = serde_json::to_string(&value) else {
        return;
    };
    if let Ok(mut guard) = events.lock() {
        if guard.len() >= EVENT_QUEUE_CAP {
            // Drop oldest to make room. If the previous entry we just
            // dropped was itself the overflow marker, don't spam another.
            let dropped_marker = matches!(
                guard.front().map(|s| s.contains("\"queue_overflow\"")),
                Some(true),
            );
            guard.pop_front();
            if !dropped_marker {
                // Reserve one slot for the marker + the new event.
                if guard.len() >= EVENT_QUEUE_CAP {
                    guard.pop_front();
                }
                if let Ok(marker) = serde_json::to_string(&json!({
                    "type": "queue_overflow",
                    "message": "Rust SDK event queue dropped oldest events; C++ poll may have stalled.",
                })) {
                    guard.push_back(marker);
                }
            }
        }
        guard.push_back(serialized);
    }
}

fn run_async<F>(events: Arc<Mutex<VecDeque<String>>>, label: &'static str, future: F)
where
    F: std::future::Future<Output = ()>,
{
    let result = catch_unwind(AssertUnwindSafe(|| {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|err| format!("failed to create Tokio runtime for {label}: {err}"))?;
        runtime.block_on(future);
        Ok::<(), String>(())
    }));

    match result {
        Ok(Ok(())) => {}
        Ok(Err(message)) => enqueue(
            &events,
            json!({ "type": "error", "message": message }),
        ),
        Err(_) => enqueue(
            &events,
            json!({ "type": "error", "message": format!("Rust SDK {label} task panicked.") }),
        ),
    }
}

unsafe fn bridge<'a>(ptr: *mut c_void) -> Result<&'a RustClient, String> {
    if ptr.is_null() {
        return Err("Rust SDK backend handle is null.".to_owned());
    }
    Ok(unsafe { &*(ptr as *mut RustClient) })
}

unsafe fn cstr_arg(ptr: *const c_char) -> Result<String, String> {
    if ptr.is_null() {
        return Err("null string argument passed to Rust SDK FFI.".to_owned());
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map(str::to_owned)
        .map_err(|err| format!("invalid UTF-8 string passed to Rust SDK FFI: {err}"))
}

fn ffi_string(body: impl FnOnce() -> Result<String, String>) -> *mut c_char {
    let value = match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(err)) => format!("error: {err}"),
        Err(_) => "error: Rust SDK FFI panic was caught.".to_owned(),
    };
    to_c_string(&value)
}

fn to_c_string(s: &str) -> *mut c_char {
    let sanitized = s.replace('\0', "");
    match CString::new(sanitized) {
        Ok(cs) => cs.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

fn format_matrix_error(context: &str, err: impl std::fmt::Display) -> String {
    format!("{context}: {err}")
}

/// Categorize an `import_room_keys` error message into a safe UI code.
///
/// Kept as a free function so it can be unit-tested without a live
/// SDK. The heuristics look at coarse substrings only — the raw
/// message (which comes from matrix-sdk / matrix-sdk-base and is
/// therefore already safe) is passed through separately; this
/// classifier decides which localized string the UI shows.
fn classify_import_error(message: &str) -> &'static str {
    let lc = message.to_lowercase();
    if lc.contains("mac")
        || lc.contains("passphrase")
        || lc.contains("password")
        || lc.contains("hmac")
        || lc.contains("decrypt")
    {
        "bad_passphrase"
    } else if lc.contains("header")
        || lc.contains("version")
        || lc.contains("base64")
        || lc.contains("invalid")
    {
        "invalid_file"
    } else if lc.contains("io") || lc.contains("read") {
        "read_failed"
    } else {
        "import_failed"
    }
}

#[cfg(test)]
mod tests {
    use super::classify_import_error;

    #[test]
    fn wrong_passphrase_becomes_bad_passphrase() {
        assert_eq!(classify_import_error("MAC verification failed"), "bad_passphrase");
        assert_eq!(classify_import_error("Wrong passphrase supplied"), "bad_passphrase");
        assert_eq!(classify_import_error("failed to decrypt export"), "bad_passphrase");
    }

    #[test]
    fn corrupt_file_becomes_invalid_file() {
        assert_eq!(classify_import_error("Invalid header line"), "invalid_file");
        assert_eq!(classify_import_error("Unsupported version 7"), "invalid_file");
        assert_eq!(classify_import_error("base64 decode error"), "invalid_file");
    }

    #[test]
    fn io_becomes_read_failed() {
        assert_eq!(classify_import_error("io error: file not found"), "read_failed");
    }

    #[test]
    fn other_errors_default_to_import_failed() {
        assert_eq!(classify_import_error("crypto store unavailable"), "import_failed");
    }
}
