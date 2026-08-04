//! matrix-client-rust — Matrix Rust SDK FFI bridge for Lightning.
//!
//! The C++ backend owns UI state and polls this bridge for JSON events. Rust
//! owns the Matrix SDK client, Tokio runtime, and SDK SQLite store.

use std::{
    collections::{BTreeSet, HashMap, VecDeque},
    ffi::{c_char, CStr, CString},
    fs::OpenOptions,
    io::Write,
    os::raw::{c_int, c_uint, c_void},
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
    encryption::verification::{
        SasState, SasVerification, VerificationRequest, VerificationRequestState,
    },
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
            secret::send::ToDeviceSecretSendEvent,
            typing::SyncTypingEvent,
        },
        uint, EventId, OwnedDeviceId, OwnedEventId, OwnedRoomId, OwnedTransactionId,
        OwnedUserId, RoomId, UInt, UserId,
    },
    room::Receipts,
    store::RoomLoadSettings,
    Client, LoopCtrl, Room, SessionMeta, SessionTokens, ThreadingSupport,
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

mod gifs;
mod rooms;
mod timeline;

/// Shared alias for the FFI event queue reference (used by `rooms.rs`).
pub(crate) type EventQueueRef = Arc<Mutex<VecDeque<String>>>;

/// v0.7.2: wake-up signals for the E2EE recovery coordinator (the
/// crypto-bootstrap observer). Carries NO key material — each variant is a
/// pure "something changed, re-evaluate" edge.
#[derive(Clone, Copy, Debug)]
enum RecoveryNudge {
    /// A SAS flow reached `SasState::Done` locally (either direction).
    /// Re-arms the bounded secret-request retry ladder even when the
    /// device-level `VerificationState` was already `Verified` (a repeat
    /// verification produces no state edge, but its fire-once SDK secret
    /// requests deserve the same supervised follow-up).
    VerificationDone,
    /// The user explicitly asked to re-request the encryption secrets
    /// ("Request keys again"). Runs one attempt immediately and re-arms
    /// the bounded follow-up ladder.
    ManualRequest,
    /// An `m.secret.send` to-device event was decrypted for this session.
    /// Only the arrival is signalled (never the name or value); the
    /// coordinator re-checks sanitized SDK state shortly afterwards.
    SecretEventSeen,
}

/// Slot through which SAS flows and the manual-request FFI reach the live
/// coordinator's nudge channel. `None` whenever no observer is running.
type RecoveryNudgeSlot =
    Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<RecoveryNudge>>>>;

/// Deliver a nudge to the live coordinator, if one is running. A missing
/// or closed channel is silently fine — the coordinator re-evaluates from
/// authoritative SDK state whenever it (re)starts.
fn notify_recovery_nudge(slot: &RecoveryNudgeSlot, nudge: RecoveryNudge) -> bool {
    if let Ok(guard) = slot.lock() {
        if let Some(sender) = guard.as_ref() {
            return sender.send(nudge).is_ok();
        }
    }
    false
}

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
    // the FFI simple; a second request arriving while one is live is
    // cancelled on the wire rather than silently evicting the live flow.
    // Both slots are released by `FlowSlotGuard` when the driver ends for
    // ANY reason, and only ever for the flow that owns them.
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas) — SasVerification has no flow_id() accessor on
    // matrix-sdk 0.18, so we track it externally.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
    // v0.7.3: managed SAS driver tasks. These hold an Arc<Client> for THIS
    // account, so one still polling after the handle is destroyed keeps the
    // account's SQLite crypto store open while sign-out deletes it — the
    // same hazard the room-key import task is already joined for. Joined
    // (not abandoned) by shutdown_managed_tasks.
    verification_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    // Cooperative stop for those drivers. Every poll tick checks it, so a
    // normal exit lands well inside the join budget and the abort below
    // stays a last-resort error boundary. Set only by teardown; the handle
    // is always destroyed immediately afterwards.
    verification_shutdown: Arc<AtomicBool>,
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
    // v0.7 defense-in-depth: dedicated TERMINAL event lane. Op-id-keyed
    // command results (media ready/failed, GIF responses/downloads) are
    // delivered here so a timeline-diff flood on the bulk queue can never
    // starve or drop them. C++ drains this queue fully before every bulk
    // batch. Bounded as a tripwire only — the C++ in-flight discipline
    // keeps it near-empty.
    command_events: EventQueueRef,
    // v0.7: verified-session crypto-bootstrap observer. Forwards sanitized
    // SDK verification/recovery/backup state and room-key-import counts to
    // C++ for the post-verification status UI. Lives exactly as long as the
    // sync session; stopped inside stop_sync_and_wait.
    bootstrap_task: Mutex<Option<SyncTask>>,
    // v0.7.2: live coordinator nudge channel. SAS flows push
    // VerificationDone here, the manual FFI pushes ManualRequest, and the
    // m.secret.send handler pushes SecretEventSeen. Cleared whenever the
    // observer stops so a stale sender can never reach a later session.
    recovery_nudges: RecoveryNudgeSlot,
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
            verification_tasks: Mutex::new(Vec::new()),
            verification_shutdown: Arc::new(AtomicBool::new(false)),
            import_active: Arc::new(AtomicBool::new(false)),
            media_results: Arc::new(Mutex::new(HashMap::new())),
            command_events: Arc::new(Mutex::new(VecDeque::new())),
            bootstrap_task: Mutex::new(None),
            recovery_nudges: Arc::new(Mutex::new(None)),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    fn stop_sync_and_wait(&self) -> bool {
        // The crypto-bootstrap observer shares the sync session's lifetime;
        // stop it first so no status event can be emitted for a session
        // that is going away. Dropping the nudge sender first guarantees no
        // late SAS/manual nudge can reach a coordinator that is stopping.
        if let Ok(mut nudges) = self.recovery_nudges.lock() {
            *nudges = None;
        }
        let bootstrap = self
            .bootstrap_task
            .lock()
            .ok()
            .and_then(|mut guard| guard.take());
        if let Some(mut task) = bootstrap {
            if let Some(cancel) = task.cancel.take() {
                let _ = cancel.send(());
            }
            if let Some(thread) = task.thread.take() {
                let _ = thread.join();
            }
        }
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
        // v0.7.3: SAS drivers first. They are the only managed tasks that
        // hold an Arc<Client> purely to poll verification state, so one left
        // running keeps this account's crypto store open across
        // mx_rust_destroy — and `finishSignOut` deletes that store moments
        // later. Signal, join, and only then fall back to abort.
        self.verification_shutdown.store(true, Ordering::SeqCst);
        let verifications = self
            .verification_tasks
            .lock()
            .ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
        if !verifications.is_empty() {
            self.runtime.block_on(async {
                let mut handles = verifications;
                let all = futures_util::future::join_all(handles.iter_mut());
                if tokio::time::timeout(
                    std::time::Duration::from_secs(timeline::SHUTDOWN_JOIN_TIMEOUT_SECS),
                    all,
                )
                .await
                .is_err()
                {
                    for handle in &handles {
                        handle.abort();
                    }
                    let _ = tokio::time::timeout(
                        std::time::Duration::from_secs(2),
                        futures_util::future::join_all(handles.iter_mut()),
                    )
                    .await;
                }
            });
        }
        // Every driver has stopped, so whatever is still parked in the slots
        // has nobody left to cancel it. That includes the case with no driver
        // at all: an incoming request the user never answered sits here from
        // the moment it arrives. Tell the peer before the session goes away —
        // abandoning a flow silently leaves it waiting out matrix-sdk-crypto's
        // 10-minute VERIFICATION_TIMEOUT.
        //
        // This is the ONLY place sign-out cancellation can live. Every
        // teardown path runs `mx_rust_shutdown_tasks` first (see
        // RustSdkMatrixClient::logout, which calls it before mx_rust_logout),
        // so a cancel placed in the logout FFI would find both slots already
        // empty and never run.
        let (pending_sas, pending_request) =
            take_pending_flows(&self.active_request, &self.active_sas);
        if pending_sas.is_some() || pending_request.is_some() {
            self.runtime.block_on(cancel_flow_best_effort(
                pending_sas.as_ref(),
                pending_request.as_ref(),
            ));
        }

        self.timelines.shutdown(&self.runtime);

        let actions = self.room_action_tasks.lock().ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
        // v0.7 defense-in-depth: ONE overall join budget for every pending
        // room-action task (previously 15s EACH, sequentially — a handful
        // of hung media fetches could block an account switch for minutes).
        // Whatever misses the budget is aborted and briefly drained.
        self.runtime.block_on(async {
            let mut handles = actions;
            let all = futures_util::future::join_all(handles.iter_mut());
            if tokio::time::timeout(
                std::time::Duration::from_secs(timeline::SHUTDOWN_JOIN_TIMEOUT_SECS),
                all,
            )
            .await
            .is_err()
            {
                for handle in &handles {
                    handle.abort();
                }
                let _ = tokio::time::timeout(
                    std::time::Duration::from_secs(2),
                    futures_util::future::join_all(handles.iter_mut()),
                )
                .await;
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

    /// v0.7.3: run a SAS driver on the SHARED runtime as a joinable task.
    ///
    /// These used to be raw `std::thread::spawn` + a throwaway per-call
    /// runtime, which nothing tracked and nothing joined: a driver survived
    /// `mx_rust_destroy` by up to seven minutes (a 300 s peer wait plus a
    /// 120 s completion poll) still holding this account's `Client`.
    fn spawn_verification_task<F>(&self, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.verification_tasks.lock() {
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
        let active_sas = Arc::clone(&bridge.active_sas);
        // Shared runtime: the SDK's post-login E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "login", async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
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
        let active_sas = Arc::clone(&bridge.active_sas);
        // Shared runtime: the SDK's post-restore E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(
                shared_runtime, runtime_events, "restore_from_file",
                async move {
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
                            Arc::clone(&active_sas),
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
        let active_sas = Arc::clone(&bridge.active_sas);
        // Shared runtime: the SDK's post-restore E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "restore", async move {
                match restore_client(&homeserver, &store_path, &user_id, &device_id, access_token)
                    .await
                {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
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
        // A pending verification is cancelled and released in
        // `shutdown_managed_tasks`, which EVERY teardown path runs before
        // this FFI (RustSdkMatrixClient::logout calls mx_rust_shutdown_tasks
        // first). Doing it here as well would be dead code: both slots are
        // already empty by the time this runs, so the cancel would silently
        // never fire and the peer would still wait out the SDK's 10-minute
        // timeout — exactly the bug it would appear to fix.
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
        let sync_client = client.clone();
        let thread = std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                run_authoritative_sync(sync_client, events, sync_mode, cancel_rx).await;
            });
        });
        *task_slot = Some(SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
        });
        drop(task_slot);

        // v0.7: verified-session crypto-bootstrap observer. Watches the
        // SDK's verification/recovery/backup state streams and the
        // room-keys-received stream for the ACTIVE session and forwards
        // sanitized state names + counts (never key material, session ids,
        // or secrets) stamped with the session lifecycle so a stale
        // observer can never update a later account. Stopped inside
        // stop_sync_and_wait, so it lives exactly as long as the sync.
        if let Ok(mut slot) = bridge.bootstrap_task.lock() {
            if slot.is_none() {
                let observer_events = Arc::clone(&bridge.events);
                let timelines = Arc::clone(&bridge.timelines);
                let lifecycle = bridge.timelines.lifecycle();
                let nudges = Arc::clone(&bridge.recovery_nudges);
                let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
                let thread = std::thread::spawn(move || {
                    let runtime_events = Arc::clone(&observer_events);
                    run_async(runtime_events, "crypto_bootstrap", async move {
                        run_crypto_bootstrap_observer(
                            client, observer_events, timelines, lifecycle,
                            nudges, cancel_rx,
                        )
                        .await;
                    });
                });
                *slot = Some(SyncTask {
                    cancel: Some(cancel),
                    thread: Some(thread),
                });
            }
        }
    }));
}

fn verification_state_name(
    state: matrix_sdk::encryption::VerificationState,
) -> &'static str {
    use matrix_sdk::encryption::VerificationState;
    match state {
        VerificationState::Unknown => "unknown",
        VerificationState::Verified => "verified",
        VerificationState::Unverified => "unverified",
    }
}

fn recovery_state_name(
    state: matrix_sdk::encryption::recovery::RecoveryState,
) -> &'static str {
    use matrix_sdk::encryption::recovery::RecoveryState;
    match state {
        RecoveryState::Unknown => "unknown",
        RecoveryState::Enabled => "enabled",
        RecoveryState::Disabled => "disabled",
        RecoveryState::Incomplete => "incomplete",
    }
}

fn backup_state_name(state: matrix_sdk::encryption::backups::BackupState) -> &'static str {
    use matrix_sdk::encryption::backups::BackupState;
    match state {
        BackupState::Unknown => "unknown",
        BackupState::Creating => "creating",
        BackupState::Enabling => "enabling",
        BackupState::Resuming => "resuming",
        BackupState::Enabled => "enabled",
        BackupState::Downloading => "downloading",
        BackupState::Disabling => "disabling",
    }
}

fn emit_crypto_bootstrap(
    events: &Arc<Mutex<VecDeque<String>>>,
    lifecycle: u64,
    kind: &str,
    state: &str,
    count: u64,
) {
    enqueue(
        events,
        json!({
            "type": "crypto_bootstrap",
            "kind": kind,
            "state": state,
            "count": count,
            "lifecycle": lifecycle,
        }),
    );
}

/// v0.7.2: the per-account E2EE RECOVERY COORDINATOR (formerly the
/// crypto-bootstrap observer). Forwards the SDK's post-verification
/// progress (verification/recovery/backup state streams, received-room-key
/// counts) exactly like the original observer, and actively drives the
/// standards-based recovery steps matrix-sdk 0.18 leaves undone:
///
///   * once this session is Verified, one `fetch_exists_on_server` probe
///     reports whether a key backup actually exists (`backup_exists`), so
///     the UI can distinguish "no backup to restore" from "waiting for the
///     other device" honestly;
///   * whenever backups become (or already are) usable while verified, the
///     open room gets one deduplicated `download_room_keys_for_room` pass —
///     the deterministic replacement for the OneShot bulk download, which
///     never re-runs once a backup key is stored and swallows failures;
///   * a bounded secret-request RETRY LADDER replaces the old one-shot
///     watchdog. matrix-sdk queues the m.secret.request set exactly once at
///     SAS completion (matrix-sdk-crypto verification/mod.rs
///     `mark_as_done` → gossiping) and a request that reaches the peer
///     before the peer finished its own completion is refused and never
///     replayed. The ladder re-issues genuinely new requests through
///     `OlmMachine::query_missing_secrets_from_other_sessions` (reached via
///     the audited `testing` feature accessor — see `Cargo.toml`), which
///     creates fresh request IDs for every still-missing secret and
///     deduplicates against queued-but-unsent ones. Attempts fire at
///     bounded delays after every arming edge (startup-verified, Verified
///     edge, SAS Done, manual request) and stop as soon as nothing is
///     missing, the own identity turns out unverified (a gossiped answer
///     could not be accepted — a fresh verification is the honest remedy),
///     or the ladder is exhausted (manual recovery remains);
///   * an `m.secret.send` arrival handler and nudge channel let SAS
///     completions, the user's "Request keys again" action, and secret
///     arrivals re-drive evaluation even when the device-level
///     `VerificationState` shows no edge (a repeat verification while
///     already Verified previously produced NO follow-up at all);
///   * received room keys additionally trigger the active room's in-place
///     decryption retry so recovered history appears without reopening.
///
/// Everything stays on public matrix-sdk APIs. No custom crypto and no
/// hand-built gossip: requests are created, validated, stored, and matched
/// by the SDK's own GossipMachine; incoming secrets keep the SDK's full
/// trust checks (same user + verified sending device). Never touches or
/// forwards key material — kinds, fixed state strings, and counts only.
async fn run_crypto_bootstrap_observer(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    timelines: Arc<timeline::TimelineRegistry>,
    lifecycle: u64,
    nudges: RecoveryNudgeSlot,
    mut cancel_rx: tokio::sync::oneshot::Receiver<()>,
) {
    let encryption = client.encryption();
    let mut verification = encryption.verification_state();
    let recovery = encryption.recovery();
    let backups = encryption.backups();
    let mut recovery_states = recovery.state_stream();
    let mut backup_states = backups.state_stream();
    let mut room_keys = encryption.room_keys_received_stream().await;

    // v0.7.2 coordinator wiring: the nudge channel (SAS Done, manual
    // request, secret arrival) and the m.secret.send arrival observer. The
    // handler forwards ONLY the fact that a secret event was decrypted —
    // never its name, value, or sender.
    let (nudge_tx, mut nudge_rx) =
        tokio::sync::mpsc::unbounded_channel::<RecoveryNudge>();
    if let Ok(mut slot) = nudges.lock() {
        *slot = Some(nudge_tx.clone());
    }
    let secret_send_handle = client.add_event_handler({
        let tx = nudge_tx.clone();
        move |_ev: ToDeviceSecretSendEvent| {
            let tx = tx.clone();
            async move {
                let _ = tx.send(RecoveryNudge::SecretEventSeen);
            }
        }
    });

    let mut verified = matches!(
        verification.get(),
        matrix_sdk::encryption::VerificationState::Verified
    );
    // Homeserver truth from the one-shot probe: None = not yet known.
    let mut backup_exists: Option<bool> = None;
    // v0.7.2 bounded secret-request retry ladder. `attempt` indexes
    // SECRET_RETRY_DELAYS_SECS; `retry_deadline` is the next evaluation
    // instant (None = disarmed). A short `recheck_deadline` follows each
    // observed m.secret.send so freshly imported state is re-read after
    // the SDK finished processing it.
    let mut retry_attempt: usize = 0;
    let mut retry_deadline: Option<tokio::time::Instant> = None;
    let mut recheck_deadline: Option<tokio::time::Instant> = None;
    // Cap on secret-ARRIVAL-driven re-arms of an exhausted ladder (review
    // finding: a hostile device spamming decryptable m.secret.send events
    // must not keep the ladder alive forever). Reset by every genuine
    // arming edge (startup-verified, Verified edge, SAS Done, manual).
    const MAX_ARRIVAL_REARMS: u32 = 2;
    let mut arrival_rearms: u32 = 0;

    // Baseline snapshot so C++ has a coherent starting state.
    emit_crypto_bootstrap(
        &events, lifecycle, "verification_state",
        verification_state_name(verification.get()), 0,
    );
    emit_crypto_bootstrap(
        &events, lifecycle, "recovery_state",
        recovery_state_name(recovery.state()), 0,
    );
    emit_crypto_bootstrap(
        &events, lifecycle, "backup_state",
        backup_state_name(backups.state()), 0,
    );

    // Server-truth probe + download pass for the already-steady state (the
    // F2 stored-key case: verified, backup Enabled at startup, and the SDK
    // will never download on its own). Arming the ladder here is what
    // heals a session that is ALREADY stuck in "verified but secretless"
    // from a previous run: the first attempt fires shortly after startup
    // and issues fresh, standards-based secret requests.
    if verified {
        probe_backup_exists(&client, &events, lifecycle, &mut backup_exists)
            .await;
        retry_attempt = 0;
        retry_deadline = secret_retry_deadline(retry_attempt);
        arrival_rearms = 0;
    }
    if verified
        && matches!(
            backups.state(),
            matrix_sdk::encryption::backups::BackupState::Enabled
        )
    {
        run_backup_download_pass(&client, &timelines).await;
    }

    loop {
        tokio::select! {
            _ = &mut cancel_rx => break,
            state = verification.next() => {
                let Some(state) = state else { break };
                let was_verified = verified;
                verified = matches!(
                    state,
                    matrix_sdk::encryption::VerificationState::Verified
                );
                emit_crypto_bootstrap(
                    &events, lifecycle, "verification_state",
                    verification_state_name(state), 0,
                );
                if verified {
                    probe_backup_exists(
                        &client, &events, lifecycle, &mut backup_exists,
                    )
                    .await;
                    if !was_verified {
                        // (Re)arm the retry ladder on every Verified edge —
                        // a repeat verification gets its own bounded,
                        // actively re-requesting wait.
                        retry_attempt = 0;
                        retry_deadline = secret_retry_deadline(retry_attempt);
                        arrival_rearms = 0;
                        // v0.7.1 idempotency fix: verification completing
                        // AFTER the backup key is already usable (manual
                        // recovery first, verification second) previously
                        // ran no download pass — the Enabled edge fired
                        // while this session was still unverified, and no
                        // later edge would come. The pass is deduplicated
                        // per lifecycle, so this is safe to repeat.
                        if matches!(
                            backups.state(),
                            matrix_sdk::encryption::backups::BackupState::Enabled
                        ) {
                            run_backup_download_pass(&client, &timelines)
                                .await;
                        }
                    }
                } else {
                    retry_deadline = None;
                }
            }
            _ = watchdog_sleep(retry_deadline) => {
                retry_deadline = None;
                if !verified {
                    continue;
                }
                let outcome = attempt_secret_recovery(
                    &client, &events, lifecycle, backup_exists,
                )
                .await;
                match outcome {
                    SecretAttemptOutcome::Complete
                    | SecretAttemptOutcome::IdentityUnverified
                    | SecretAttemptOutcome::Unavailable => {
                        // Terminal for this ladder: either nothing is
                        // missing, or a gossiped answer could not be
                        // accepted / created. A later arming edge (fresh
                        // verification, manual request) starts a new one.
                    }
                    SecretAttemptOutcome::Requested
                    | SecretAttemptOutcome::AlreadyPending
                    | SecretAttemptOutcome::NoEligibleDevices => {
                        retry_attempt += 1;
                        retry_deadline = secret_retry_deadline(retry_attempt);
                        emit_crypto_bootstrap(
                            &events, lifecycle, "secrets_pending",
                            if retry_deadline.is_some() {
                                "waiting"
                            } else {
                                // Ladder exhausted with secrets still
                                // missing — the UI escalates to manual
                                // recovery honestly.
                                "exhausted"
                            },
                            0,
                        );
                    }
                }
            }
            _ = watchdog_sleep(recheck_deadline) => {
                recheck_deadline = None;
                // Re-read sanitized SDK state after the secret event was
                // fully processed; stop the ladder when nothing is missing.
                if secret_recheck_complete(
                    &client, &events, lifecycle, backup_exists,
                )
                .await
                {
                    retry_deadline = None;
                } else if verified
                    && retry_deadline.is_none()
                    && arrival_rearms < MAX_ARRIVAL_REARMS
                {
                    // A secret arrived but recovery is still incomplete
                    // (e.g. the SDK refused it, or the gossiped backup key
                    // did not match the active version and was discarded).
                    // Re-arm one bounded follow-up round instead of
                    // stalling forever on an exhausted ladder — capped so
                    // hostile unrequested m.secret.send spam cannot keep
                    // the ladder alive indefinitely.
                    arrival_rearms += 1;
                    retry_attempt = 1;
                    retry_deadline = secret_retry_deadline(retry_attempt);
                }
            }
            nudge = nudge_rx.recv() => {
                let Some(nudge) = nudge else { continue };
                match nudge {
                    RecoveryNudge::VerificationDone => {
                        // A SAS flow completed locally. The SDK queued its
                        // own fire-once request set; give the peer a
                        // bounded window before supervising with fresh
                        // requests. Runs even when VerificationState shows
                        // no edge (repeat verification).
                        probe_backup_exists(
                            &client, &events, lifecycle, &mut backup_exists,
                        )
                        .await;
                        retry_attempt = 0;
                        retry_deadline = secret_retry_deadline(retry_attempt);
                        arrival_rearms = 0;
                    }
                    RecoveryNudge::ManualRequest => {
                        probe_backup_exists(
                            &client, &events, lifecycle, &mut backup_exists,
                        )
                        .await;
                        if !verified {
                            emit_crypto_bootstrap(
                                &events, lifecycle, "secret_request",
                                "identity_unverified", 0,
                            );
                            continue;
                        }
                        arrival_rearms = 0;
                        let outcome = attempt_secret_recovery(
                            &client, &events, lifecycle, backup_exists,
                        )
                        .await;
                        match outcome {
                            SecretAttemptOutcome::Requested
                            | SecretAttemptOutcome::AlreadyPending
                            | SecretAttemptOutcome::NoEligibleDevices => {
                                // The manual attempt consumed slot 0; keep
                                // the bounded follow-ups armed.
                                retry_attempt = 1;
                                retry_deadline =
                                    secret_retry_deadline(retry_attempt);
                            }
                            _ => {
                                retry_deadline = None;
                            }
                        }
                    }
                    RecoveryNudge::SecretEventSeen => {
                        // Arrival only — the SDK validated, matched, and
                        // imported (or refused) the secret itself.
                        emit_crypto_bootstrap(
                            &events, lifecycle, "secret_response",
                            "received", 1,
                        );
                        recheck_deadline = Some(
                            tokio::time::Instant::now()
                                + std::time::Duration::from_secs(3),
                        );
                    }
                }
            }
            state = recovery_states.next() => {
                let Some(state) = state else { break };
                emit_crypto_bootstrap(
                    &events, lifecycle, "recovery_state",
                    recovery_state_name(state), 0,
                );
            }
            state = backup_states.next() => {
                let Some(state) = state else { break };
                let Ok(state) = state else { continue }; // lagged stream
                emit_crypto_bootstrap(
                    &events, lifecycle, "backup_state",
                    backup_state_name(state), 0,
                );
                // The gossiped (or manually recovered) backup key became
                // usable — run the deterministic download pass the OneShot
                // strategy only attempts on the very first key store.
                if verified
                    && matches!(
                        state,
                        matrix_sdk::encryption::backups::BackupState::Enabled
                    )
                {
                    run_backup_download_pass(&client, &timelines).await;
                }
            }
            keys = async {
                match room_keys.as_mut() {
                    Some(stream) => stream.next().await,
                    // No crypto machine (should not happen with E2EE wired):
                    // park forever instead of spinning the loop.
                    None => std::future::pending().await,
                }
            } => {
                let Some(keys) = keys else { break };
                let Ok(infos) = keys else { continue }; // lagged stream
                if !infos.is_empty() {
                    // Counts only — never room ids, session ids, or key
                    // material.
                    emit_crypto_bootstrap(
                        &events, lifecycle, "room_keys_received", "",
                        infos.len() as u64,
                    );
                    // v0.7.2: freshly imported keys retry the ACTIVE room's
                    // undecryptable rows in place (the registry filters to
                    // the open room and deduplicates; identifiers stay
                    // inside the Rust boundary).
                    let mut by_room: HashMap<String, Vec<String>> =
                        HashMap::new();
                    for info in infos.iter() {
                        by_room
                            .entry(info.room_id.to_string())
                            .or_default()
                            .push(info.session_id.clone());
                    }
                    let sessions: Vec<(String, Vec<String>)> =
                        by_room.into_iter().collect();
                    timelines
                        .retry_decryption_after_import(&sessions)
                        .await;
                }
            }
        }
    }

    client.remove_event_handler(secret_send_handle);
    if let Ok(mut slot) = nudges.lock() {
        *slot = None;
    }
}

/// One-time (per supervisor) network probe: does a key backup actually
/// exist on the homeserver? Emits the sanitized boolean as
/// `backup_exists` true/false; a failed probe emits nothing (the UI keeps
/// its cautious unknown state) and may be retried on the next
/// verification-state edge.
async fn probe_backup_exists(
    client: &Client,
    events: &Arc<Mutex<VecDeque<String>>>,
    lifecycle: u64,
    exists: &mut Option<bool>,
) {
    if exists.is_some() {
        return;
    }
    match client.encryption().backups().fetch_exists_on_server().await {
        Ok(found) => {
            *exists = Some(found);
            emit_crypto_bootstrap(
                events,
                lifecycle,
                "backup_exists",
                if found { "true" } else { "false" },
                0,
            );
        }
        Err(_) => { /* transient network failure — stay unknown */ }
    }
}

/// v0.7.1: sleep until the armed deadline, or forever when it is disarmed.
/// Takes the deadline BY VALUE (Copy) so the select! arm never borrows the
/// coordinator's mutable state.
async fn watchdog_sleep(deadline: Option<tokio::time::Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending::<()>().await,
    }
}

/// v0.7.2 retry-ladder delays, in seconds AFTER each arming edge. The
/// first attempt waits long enough for the SDK's own fire-once request set
/// (queued at SAS Done) or an in-flight answer to land; the later attempts
/// cover the documented peer-side race (a request that arrives before the
/// peer finished its own completion is refused and never replayed) with
/// bounded backoff. Past the last entry the coordinator emits an honest
/// `secrets_pending exhausted` and stops until a new arming edge.
const SECRET_RETRY_DELAYS_SECS: [u64; 3] = [20, 90, 240];

/// Deadline for the given ladder attempt, or None when the ladder is
/// exhausted. Pure delay lookup kept separate for unit tests.
fn next_secret_retry_delay(attempt: usize) -> Option<u64> {
    SECRET_RETRY_DELAYS_SECS.get(attempt).copied()
}

fn secret_retry_deadline(attempt: usize) -> Option<tokio::time::Instant> {
    next_secret_retry_delay(attempt).map(|secs| {
        tokio::time::Instant::now() + std::time::Duration::from_secs(secs)
    })
}

/// v0.7.2 missing-secret decision: is post-verification recovery still
/// incomplete? Cross-signing private keys are always required; the backup
/// decryption key only when the homeserver-truth probe said a backup
/// EXISTS (an account without any backup has nothing to enable, and an
/// unknown probe result stays conservative on the cross-signing half
/// only). Pure and synchronous so the decision table is unit-testable.
fn secret_recovery_missing(
    cross_signing_complete: bool,
    backup_exists: Option<bool>,
    backup_enabled: bool,
) -> bool {
    !cross_signing_complete
        || (backup_exists == Some(true) && !backup_enabled)
}

/// One bounded secret-recovery attempt outcome. Carried counts are device
/// counts only — never identifiers or key material.
enum SecretAttemptOutcome {
    /// Nothing is missing anymore; the ladder stops.
    Complete,
    /// The own identity is not verified on this session, so the SDK would
    /// reject any gossiped answer (m.secret.send is only accepted from a
    /// device this session considers verified, which requires local
    /// own-identity trust). A fresh interactive verification is the
    /// standards-based remedy; requesting again would mislead.
    IdentityUnverified,
    /// New m.secret.request gossip was queued for every missing secret.
    Requested,
    /// Requests for the missing secrets are already queued and unsent.
    AlreadyPending,
    /// No other verified own device exists to answer a request right now.
    NoEligibleDevices,
    /// The crypto machine was unavailable or the store query failed.
    Unavailable,
}

/// Evaluate the sanitized trust/secret state and, when appropriate, queue
/// standards-based m.secret.request gossip through the SDK. Emits the
/// matching `own_identity`, `cross_signing_secrets`, and `secret_request`
/// bootstrap events (fixed state strings + counts only).
async fn attempt_secret_recovery(
    client: &Client,
    events: &Arc<Mutex<VecDeque<String>>>,
    lifecycle: u64,
    backup_exists: Option<bool>,
) -> SecretAttemptOutcome {
    let encryption = client.encryption();

    let own_identity_verified = match client.user_id() {
        Some(uid) => matches!(
            encryption.get_user_identity(uid).await,
            Ok(Some(identity)) if identity.is_verified()
        ),
        None => false,
    };
    emit_crypto_bootstrap(
        events, lifecycle, "own_identity",
        if own_identity_verified { "verified" } else { "unverified" }, 0,
    );

    let cross_signing_complete = encryption
        .cross_signing_status()
        .await
        .map(|status| {
            status.has_master && status.has_self_signing && status.has_user_signing
        })
        .unwrap_or(false);
    emit_crypto_bootstrap(
        events, lifecycle, "cross_signing_secrets",
        if cross_signing_complete { "complete" } else { "incomplete" }, 0,
    );

    let backup_enabled = encryption.backups().are_enabled().await;
    if !secret_recovery_missing(cross_signing_complete, backup_exists, backup_enabled) {
        emit_crypto_bootstrap(
            events, lifecycle, "secret_request", "none_missing", 0,
        );
        return SecretAttemptOutcome::Complete;
    }

    if !own_identity_verified {
        emit_crypto_bootstrap(
            events, lifecycle, "secret_request", "identity_unverified", 0,
        );
        return SecretAttemptOutcome::IdentityUnverified;
    }

    let eligible = count_eligible_verified_devices(client).await;
    if eligible == 0 {
        emit_crypto_bootstrap(
            events, lifecycle, "secret_request", "no_eligible_devices", 0,
        );
        return SecretAttemptOutcome::NoEligibleDevices;
    }

    match queue_missing_secret_requests(client).await {
        Ok(true) => {
            emit_crypto_bootstrap(
                events, lifecycle, "secret_request", "requested", eligible,
            );
            SecretAttemptOutcome::Requested
        }
        Ok(false) => {
            emit_crypto_bootstrap(
                events, lifecycle, "secret_request", "already_pending",
                eligible,
            );
            SecretAttemptOutcome::AlreadyPending
        }
        Err(()) => {
            emit_crypto_bootstrap(
                events, lifecycle, "secret_request", "unavailable", 0,
            );
            SecretAttemptOutcome::Unavailable
        }
    }
}

/// Post-arrival re-read: emit the refreshed sanitized identity/secret
/// state and report whether recovery is now complete (nothing missing).
async fn secret_recheck_complete(
    client: &Client,
    events: &Arc<Mutex<VecDeque<String>>>,
    lifecycle: u64,
    backup_exists: Option<bool>,
) -> bool {
    let encryption = client.encryption();
    let cross_signing_complete = encryption
        .cross_signing_status()
        .await
        .map(|status| {
            status.has_master && status.has_self_signing && status.has_user_signing
        })
        .unwrap_or(false);
    emit_crypto_bootstrap(
        events, lifecycle, "cross_signing_secrets",
        if cross_signing_complete { "complete" } else { "incomplete" }, 0,
    );
    let backup_enabled = encryption.backups().are_enabled().await;
    !secret_recovery_missing(cross_signing_complete, backup_exists, backup_enabled)
}

/// Count the OTHER verified devices of this account (the sessions a
/// standards-based secret request could be answered by). Count only —
/// device identifiers never leave this function.
async fn count_eligible_verified_devices(client: &Client) -> u64 {
    let Some(uid) = client.user_id() else { return 0 };
    let own_device = client.device_id();
    match client.encryption().get_user_devices(uid).await {
        Ok(devices) => devices
            .devices()
            .filter(|device| {
                Some(device.device_id()) != own_device && device.is_verified()
            })
            .count() as u64,
        Err(_) => 0,
    }
}

/// Queue m.secret.request gossip for every still-missing secret through
/// `OlmMachine::query_missing_secrets_from_other_sessions` — the SDK's own
/// re-request entry point (fresh request IDs, deduplicated against
/// queued-but-unsent requests, sent by the normal outgoing-request sync
/// machinery, answered only via the SDK's full trust validation). The
/// accessor is gated behind matrix-sdk's `testing` feature because 0.18
/// exposes no other public route to the machine; the feature's code sites
/// were audited as strictly additive (see rust/Cargo.toml). Returns
/// whether any new request was queued.
async fn queue_missing_secret_requests(client: &Client) -> Result<bool, ()> {
    let machine = client.olm_machine_for_testing().await;
    let Some(machine) = machine.as_ref() else {
        return Err(());
    };
    machine
        .query_missing_secrets_from_other_sessions()
        .await
        .map_err(|_| ())
}

/// Run the deduplicated whole-room backup download pass for the currently
/// open room, if any. Rooms opened later get their own pass from
/// `open_room_task`.
async fn run_backup_download_pass(
    client: &Client,
    timelines: &Arc<timeline::TimelineRegistry>,
) {
    if let Some(room_id) = timelines.active_room_id() {
        timelines
            .download_backup_keys_for_room(client, &room_id)
            .await;
    }
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

/// v0.7 defense-in-depth: drain one event from the TERMINAL command lane
/// (media ready/failed, GIF results). C++ empties this queue completely
/// before each bulk mx_rust_poll_event batch so terminal results can never
/// be starved by timeline-diff floods.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_poll_command_event(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event = bridge
            .command_events
            .lock()
            .map_err(|_| "Rust SDK command queue lock poisoned.".to_owned())?
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
                let content = RoomMessageEventContent::text_markdown(body);
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

/// Manual key-backup recovery (v0.5.0-prep+7, upgraded by the v0.7
/// recovery supervisor). Calls
/// `client.encryption().recovery().recover(input)` on matrix-sdk 0.18,
/// which accepts a Base58 recovery KEY **or a recovery PASSPHRASE** (the
/// SDK tries the passphrase KDF first when the server's key event carries
/// passphrase info, then Base58 — verified in the pinned sources). The
/// secret must arrive here as a plain string; C++ sanitises and zeroes its
/// buffer after the call. This FFI **never** logs the input or any
/// imported key material. Result events on the poll queue:
///   { "type": "key_backup_status", "state": "attempted" }
///   { "type": "key_backup_status", "state": "ok" }
///   { "type": "key_backup_status", "state": "failed", "message": "…" }
///
/// v0.7: after a successful recover, the SDK's `maybe_enable_backups`
/// short-circuits with NO download whenever the backup key was already
/// stored (and the fire-once OneShot bulk download does not re-run), so a
/// deterministic download pass for the open room plus a visible-UTD
/// decryption retry run here explicitly. Manual recovery deliberately
/// clears the room's dedup mark first — an explicit user action gets one
/// fresh pass.
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
        let timelines = Arc::clone(&bridge.timelines);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "recover_backup", async move {
                enqueue(
                    &events,
                    json!({ "type": "key_backup_status", "state": "attempted" }),
                );
                let recovery = client.encryption().recovery();
                match recovery.recover(&recovery_key).await {
                    Ok(_) => {
                        // Deterministic post-recover download for the open
                        // room (forced once), then re-run decryption for its
                        // visible undecryptable rows. Uses only public SDK
                        // APIs; imported keys propagate to timelines through
                        // the SDK's own redecryption path.
                        if let Some(room_id) = timelines.active_room_id() {
                            timelines.clear_backup_attempt(&room_id);
                            timelines
                                .download_backup_keys_for_room(
                                    &client, &room_id,
                                )
                                .await;
                        }
                        enqueue(
                            &events,
                            json!({ "type": "key_backup_status", "state": "ok" }),
                        );
                    }
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

/// v0.7.1: sanitized local-confirmation event. Emitted (once per flow)
/// when the SDK reports `SasState::Confirmed` — "the verification process
/// has been confirmed from our side, we're waiting for the other side to
/// confirm as well" — so the UI can acknowledge the "They match" press
/// instead of freezing on the emoji screen until the PEER also confirms.
/// Flow id only; never emoji values, decimals, or key material.
fn verification_sas_confirmed_event(flow_id: &str) -> serde_json::Value {
    json!({
        "type": "verification_sas_confirmed",
        "flow_id": flow_id,
    })
}

/// SAS state-poll cadence. matrix-sdk exposes both `state()` snapshots and
/// `changes()` streams; polling the REQUEST keeps one code path for the
/// request and the SAS, because the request is what owns which `Sas` the
/// flow is currently using.
const VERIFICATION_POLL_MS: u64 = 500;
/// Bounded wait for an `m.key.verification.start` to land when neither
/// side has produced a SAS yet (60 s).
const SAS_HANDSHAKE_TICKS: u32 = 120;
/// Bounded wait for a started SAS to reach a terminal state (120 s).
const SAS_COMPLETION_TICKS: u32 = 240;
/// Bounded wait for the peer to answer our request (5 minutes). Shorter
/// than matrix-sdk-crypto's own 10-minute VERIFICATION_TIMEOUT so the user
/// gets an answer rather than a hang.
const VERIFICATION_PEER_TICKS: u32 = 600;
/// How long a teardown-time cancellation may spend on the wire. Kept well
/// under `timeline::SHUTDOWN_JOIN_TIMEOUT_SECS` so cancelling and joining
/// together stay inside the existing shutdown budget: telling the peer is
/// worth a short wait, never a stalled sign-out.
const VERIFICATION_CANCEL_TIMEOUT_SECS: u64 = 3;

/// Best-effort, bounded cancellation for a session that is going away.
///
/// A flow abandoned without a cancel leaves the peer waiting out
/// matrix-sdk-crypto's 10-minute `VERIFICATION_TIMEOUT` with no signal at
/// all, so shutdown still owes the peer this message. It is best-effort by
/// design: teardown may not block on a network round trip, and a cancel
/// that cannot complete in the budget is dropped rather than allowed to
/// stall sign-out. Both levels are attempted because each SDK cancel is
/// idempotent and either one may be the live half of the flow.
async fn cancel_flow_best_effort(
    sas: Option<&SasVerification>,
    request: Option<&VerificationRequest>,
) {
    let budget = std::time::Duration::from_secs(VERIFICATION_CANCEL_TIMEOUT_SECS);
    if let Some(sas) = sas {
        if !sas.is_cancelled() && !sas.is_done() {
            let _ = tokio::time::timeout(budget, sas.cancel()).await;
        }
    }
    if let Some(request) = request {
        if !request.is_cancelled() && !request.is_done() {
            let _ = tokio::time::timeout(budget, request.cancel()).await;
        }
    }
}

/// The SAS this request is currently using, straight from SDK state.
///
/// This — not a cached handle — is authoritative. When both peers send
/// `m.key.verification.start` at once, matrix-sdk-crypto applies the spec
/// tie-break and REPLACES the losing `Sas` in its verification cache
/// (`receive_start` -> `replace_sas`, verification/requests.rs). A handle
/// captured before that point shares no state with the survivor: it never
/// changes state again, so polling it reports a timeout while the real
/// flow is alive, and confirming it does nothing.
fn sas_from_request(request: &VerificationRequest) -> Option<SasVerification> {
    match request.state() {
        VerificationRequestState::Transitioned { verification } => verification.sas(),
        _ => None,
    }
}

/// Can this flow handle still make progress?
///
/// Abstracted purely so the single-flow slot rules below can be unit
/// tested: matrix-sdk's `VerificationRequest` and `SasVerification` have
/// crate-private constructors, so a test can never build a real one, and
/// these rules are exactly where the "verification is already in progress"
/// brick and the newer-flow clobber lived.
trait FlowLiveness {
    fn is_finished(&self) -> bool;
}

/// Which flow a slot occupant belongs to, for compare-and-clear.
trait FlowIdentity {
    fn flow_key(&self) -> &str;
}

impl FlowLiveness for VerificationRequest {
    fn is_finished(&self) -> bool {
        // `is_passive` means another of our own sessions answered this
        // request; it can never progress here either.
        self.is_cancelled() || self.is_done() || self.is_passive()
    }
}

impl FlowIdentity for VerificationRequest {
    fn flow_key(&self) -> &str {
        self.flow_id()
    }
}

impl FlowLiveness for SasVerification {
    fn is_finished(&self) -> bool {
        self.is_cancelled() || self.is_done()
    }
}

/// True when the single-flow slots still hold a LIVE flow.
///
/// Dead occupants (cancelled, done, or answered by another of our
/// sessions) are cleared in passing, so the slots are self-healing instead
/// of sticky: an abandoned request can never refuse every later attempt
/// for the rest of the process lifetime.
fn flow_slots_are_live<R, S>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &Arc<Mutex<Option<(String, S)>>>,
) -> bool
where
    R: FlowLiveness,
    S: FlowLiveness,
{
    if let Ok(mut guard) = request_slot.lock() {
        if guard.as_ref().is_some_and(|request| request.is_finished()) {
            *guard = None;
        }
    }
    if let Ok(mut guard) = sas_slot.lock() {
        if guard.as_ref().is_some_and(|(_, sas)| sas.is_finished()) {
            *guard = None;
        }
    }
    let request_live = request_slot.lock().map(|g| g.is_some()).unwrap_or(false);
    let sas_live = sas_slot.lock().map(|g| g.is_some()).unwrap_or(false);
    request_live || sas_live
}

/// Release the single-flow slots, but ONLY where they still hold this flow.
///
/// The unconditional `*g = None` this replaces meant a terminating flow
/// wiped whatever occupied the slot — including a NEWER request that had
/// arrived in the meantime, whose Accept then failed with "no active
/// verification request".
fn release_flow_slots<R, S>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &Arc<Mutex<Option<(String, S)>>>,
    flow_id: &str,
) where
    R: FlowIdentity,
{
    if let Ok(mut guard) = sas_slot.lock() {
        if guard.as_ref().is_some_and(|(stored, _)| stored == flow_id) {
            *guard = None;
        }
    }
    if let Ok(mut guard) = request_slot.lock() {
        if guard.as_ref().is_some_and(|r| r.flow_key() == flow_id) {
            *guard = None;
        }
    }
}

/// Take whatever is parked in the single-flow slots, leaving them empty.
///
/// Teardown must TAKE before it clears. The slots are the only handle on a
/// flow the peer is still waiting for, so clearing them first throws away
/// the ability to cancel it — which is exactly how an earlier version of
/// this change ended up with a cancel that could never fire, because
/// `shutdown_managed_tasks` emptied the slots before `mx_rust_logout` (the
/// function that held the cancel) ever ran.
fn take_pending_flows<R, S>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &Arc<Mutex<Option<(String, S)>>>,
) -> (Option<S>, Option<R>) {
    let sas = sas_slot
        .lock()
        .ok()
        .and_then(|mut guard| guard.take())
        .map(|(_, sas)| sas);
    let request = request_slot.lock().ok().and_then(|mut guard| guard.take());
    (sas, request)
}

/// Releases this flow's slots when its driver ends for ANY reason —
/// normal exit, early return, cooperative shutdown, or a panic.
///
/// Releasing by hand at every exit is what previously leaked: several
/// early returns kept `active_request` occupied and
/// `mx_rust_start_own_verification` then refused outright, so one failed
/// attempt bricked verification until the app restarted. Cleanup on drop
/// makes future exits safe by construction rather than by remembering.
struct FlowSlotGuard<R: FlowIdentity, S> {
    request_slot: Arc<Mutex<Option<R>>>,
    sas_slot: Arc<Mutex<Option<(String, S)>>>,
    flow_id: String,
}

impl<R: FlowIdentity, S> FlowSlotGuard<R, S> {
    fn new(
        request_slot: Arc<Mutex<Option<R>>>,
        sas_slot: Arc<Mutex<Option<(String, S)>>>,
        flow_id: String,
    ) -> Self {
        Self { request_slot, sas_slot, flow_id }
    }
}

impl<R: FlowIdentity, S> Drop for FlowSlotGuard<R, S> {
    fn drop(&mut self) {
        release_flow_slots(&self.request_slot, &self.sas_slot, &self.flow_id);
    }
}

/// Drive a ready verification request through the SAS flow to a terminal
/// state. Shared by BOTH directions, because both need exactly the same
/// sequence once `m.key.verification.ready` has been exchanged.
///
/// Emits `verification_sas_started`, `verification_sas_ready` (emoji +
/// decimals), `verification_sas_confirmed`, and finally `verification_done`
/// / `verification_cancelled` / `verification_failed`. Never reports
/// success unless the SDK reached `SasState::Done`.
#[allow(clippy::too_many_arguments)]
async fn drive_sas_flow(
    client: &Client,
    request: &VerificationRequest,
    flow_id: &str,
    events: &Arc<Mutex<VecDeque<String>>>,
    sas_slot: &Arc<Mutex<Option<(String, SasVerification)>>>,
    nudges: &RecoveryNudgeSlot,
    shutdown: &Arc<AtomicBool>,
) {
    let poll = std::time::Duration::from_millis(VERIFICATION_POLL_MS);
    let fail = |message: String| {
        enqueue(events, json!({
            "type": "verification_failed",
            "flow_id": flow_id,
            "message": message,
        }));
    };

    // Someone has to send `m.key.verification.start`; matrix-sdk does NOT
    // do it for us, and a peer that also waits leaves both sides parked in
    // Ready. `start_sas()` is the only emitter — but it is NOT idempotent:
    // in Transitioned it builds a SECOND Sas and emits a competing start
    // (verification/requests.rs `start_sas` -> `start_sas_helper`). So
    // adopt whatever the SDK already has before starting anything.
    let sas: Option<SasVerification> = match sas_from_request(request) {
        Some(sas) => Some(sas),
        None => match request.start_sas().await {
            Ok(Some(sas)) => Some(sas),
            // `Ok(None)` means the peer never advertised `m.sas.v1` or the
            // request left Ready. Give a start that is already in flight a
            // bounded chance to land, then fail visibly rather than hang.
            Ok(None) => {
                let mut found: Option<SasVerification> = None;
                for _ in 0..SAS_HANDSHAKE_TICKS {
                    if shutdown.load(Ordering::SeqCst) {
                        // No SAS exists yet, but the request does, and the
                        // peer is waiting on it.
                        cancel_flow_best_effort(None, Some(request)).await;
                        return;
                    }
                    tokio::time::sleep(poll).await;
                    if let Some(sas) = sas_from_request(request) {
                        found = Some(sas);
                        break;
                    }
                    if let Some(verification) = client
                        .encryption()
                        .get_verification(request.other_user_id(), request.flow_id())
                        .await
                    {
                        if let Some(sas) = verification.sas() {
                            found = Some(sas);
                            break;
                        }
                    }
                    if request.is_cancelled() || request.is_done() {
                        break;
                    }
                }
                found
            }
            Err(err) => {
                fail(format_matrix_error("SAS start failed", err));
                return;
            }
        },
    };
    let Some(mut sas) = sas else {
        fail("Timed out waiting for SAS handshake.".to_owned());
        return;
    };

    // A peer-started SAS arrives in `SasState::Started` and needs OUR
    // `m.key.verification.accept` before either side can send a key.
    // `Sas::accept()` returns None outside Started (verification/sas/mod.rs)
    // so this is a no-op on the branch where WE started the flow. The
    // outgoing path used to skip it entirely: it adopted the peer's SAS and
    // then polled a flow it had never accepted, which stalled to the
    // completion timeout — the same "both peers parked" symptom as before,
    // one handshake step later.
    if let Err(err) = sas.accept().await {
        fail(format_matrix_error("SAS accept failed", err));
        return;
    }
    if let Ok(mut guard) = sas_slot.lock() {
        *guard = Some((flow_id.to_owned(), sas.clone()));
    }
    enqueue(events, json!({
        "type": "verification_sas_started",
        "flow_id": flow_id,
    }));

    let mut emitted_emojis = false;
    let mut emitted_confirmed = false;
    for _ in 0..SAS_COMPLETION_TICKS {
        if shutdown.load(Ordering::SeqCst) {
            // The session is going away mid-flow. Cancelling both levels is
            // what stops the peer sitting on an emoji screen for ten
            // minutes; the slot sweep in shutdown_managed_tasks only covers
            // flows this driver no longer owns.
            cancel_flow_best_effort(Some(&sas), Some(request)).await;
            return;
        }
        tokio::time::sleep(poll).await;

        // Re-derive every tick: the SDK, not this snapshot, owns which Sas
        // the flow uses, and a tie-break replacement would otherwise leave
        // us polling a dead object. Keeping the slot in step also means
        // confirm/mismatch always act on the live one.
        if let Some(current) = sas_from_request(request) {
            sas = current;
            if let Ok(mut guard) = sas_slot.lock() {
                *guard = Some((flow_id.to_owned(), sas.clone()));
            }
        }

        // Classify inside a block so the SasState snapshot is dropped
        // before any await — nothing SDK-owned is held across a suspend
        // point.
        let needs_accept = {
            match sas.state() {
                // Reachable when the peer's start lands after ours was sent
                // and the SDK substitutes theirs. Accepting moves it out of
                // Started, so this cannot spin.
                SasState::Started { .. } => true,
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
                    enqueue(events, json!({
                        "type": "verification_sas_ready",
                        "flow_id": flow_id,
                        "emojis": emoji_list,
                        "decimals": dec,
                    }));
                    false
                }
                SasState::Done { .. } => {
                    enqueue(events, json!({
                        "type": "verification_done",
                        "flow_id": flow_id,
                    }));
                    notify_recovery_nudge(nudges, RecoveryNudge::VerificationDone);
                    return;
                }
                SasState::Cancelled(info) => {
                    enqueue(events, json!({
                        "type": "verification_cancelled",
                        "flow_id": flow_id,
                        "message": format!("{:?}", info.reason()),
                    }));
                    return;
                }
                // v0.7.1: OUR confirmation registered; Done still requires
                // the peer's MAC. Surface it once so the UI can leave the
                // frozen emoji screen honestly.
                SasState::Confirmed if !emitted_confirmed => {
                    emitted_confirmed = true;
                    enqueue(events, verification_sas_confirmed_event(flow_id));
                    false
                }
                _ => false,
            }
        };
        if needs_accept {
            if let Err(err) = sas.accept().await {
                fail(format_matrix_error("SAS accept failed", err));
                return;
            }
        }
    }
    fail("Timed out waiting for SAS completion.".to_owned());
}

/// SAS emoji verification — accept an incoming request and drive it to a
/// terminal state (v0.5.0). Emits `verification_ready` once both sides have
/// exchanged `m.key.verification.ready`, then hands off to the shared
/// `drive_sas_flow` driver for the start/accept/key/mac sequence.
///
/// The flow's single-flow slots are released by `FlowSlotGuard` on every
/// exit path, including a panic, so a failed attempt can never refuse the
/// next one.
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
            // Nothing can drive this flow any more. Release the slot rather
            // than parking a dead request that would refuse every later
            // attempt for the rest of the process lifetime.
            release_flow_slots(&bridge.active_request, &bridge.active_sas, &flow_id);
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        let nudges = Arc::clone(&bridge.recovery_nudges);
        let shutdown = Arc::clone(&bridge.verification_shutdown);
        bridge.spawn_verification_task(async move {
            let _slots = FlowSlotGuard::new(
                Arc::clone(&request_slot),
                Arc::clone(&sas_slot),
                flow_id.clone(),
            );

            // Advertise ONLY what this client can actually perform.
            // `accept()` would use the SDK's SUPPORTED_METHODS, which
            // includes `m.reciprocate.v1` (matrix-sdk-crypto
            // verification/requests.rs) — telling the peer we can scan a QR
            // code it shows us. Lightning builds without the `qrcode`
            // feature and has no scanner, so that invites a reciprocate
            // start we can never answer. Worse, without that feature the
            // SDK's `receive_start` has no ReciprocateV1 arm at all: it
            // warns and returns, sending no cancel, so the peer waits out
            // the full 10-minute timeout. Both directions advertise SAS
            // only.
            if let Err(err) = request
                .accept_with_methods(vec![VerificationMethod::SasV1])
                .await
            {
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": flow_id,
                    "message": format_matrix_error(
                        "Matrix Rust SDK verification accept failed", err),
                }));
                return;
            }
            enqueue(&events, json!({
                "type": "verification_ready",
                "flow_id": flow_id,
            }));

            drive_sas_flow(
                &client, &request, &flow_id, &events, &sas_slot, &nudges,
                &shutdown,
            )
            .await;
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
        // v0.7.3: joinable like the drivers. `confirm()` uploads a signature
        // and writes to the crypto store, so it must not still be running
        // when sign-out deletes that store.
        bridge.spawn_verification_task(async move {
            if let Err(err) = action(sas.clone()).await {
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": stored_flow,
                    "message": format_matrix_error(label, err),
                }));
            }
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
    // Cancel at BOTH levels. Each SDK cancel is idempotent (it returns no
    // outgoing request once the flow is already cancelled or done), so
    // running both is safe — and necessary. These used to be `if` /
    // `else if`, which meant an `active_sas` belonging to some OTHER flow
    // suppressed the request-level cancel entirely: nothing reached the
    // wire, the peer waited out matrix-sdk-crypto's 10-minute
    // VERIFICATION_TIMEOUT, and both slots were cleared regardless.
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let sas_entry = bridge.active_sas.lock().ok().and_then(|g| g.clone());
        let request = bridge.active_request.lock().ok().and_then(|g| g.clone());
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let request_slot = Arc::clone(&bridge.active_request);
        bridge.spawn_verification_task(async move {
            let mut cancelled = false;
            if let Some((stored_flow, sas)) = sas_entry {
                if stored_flow == flow_id {
                    if !sas.is_cancelled() && !sas.is_done() {
                        let _ = sas.cancel().await;
                    }
                    cancelled = true;
                }
            }
            if let Some(request) = request {
                if request.flow_id() == flow_id {
                    if !request.is_cancelled() && !request.is_done() {
                        let _ = request.cancel().await;
                    }
                    cancelled = true;
                }
            }
            // Report only a cancellation that actually applied to this
            // flow, and release only this flow's slots — clearing them
            // unconditionally used to evict a newer request's handle.
            if cancelled {
                enqueue(&events, json!({
                    "type": "verification_cancelled",
                    "flow_id": flow_id,
                    "message": "cancelled",
                }));
            }
            release_flow_slots(&request_slot, &sas_slot, &flow_id);
        });
        Ok(String::new())
    })
}

/// Lightning-initiated (outbound) SAS verification of the current
/// session against another session belonging to the same Matrix
/// account. Advertises SAS as the only method so the SDK does not
/// send an m.qr_code.* request Lightning cannot follow through.
///
/// Emits `verification_request_started` as soon as the SDK has sent the
/// request, then `verification_ready` once the peer answers, and hands
/// off to the shared `drive_sas_flow` driver — the same one the
/// receive-first path uses, so both directions perform the identical
/// start/accept/key/mac sequence.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_own_verification(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        // Reject a duplicate start only if a flow is genuinely LIVE. A
        // presence-only check bricked verification for the rest of the process
        // whenever a dead request stayed parked — an incoming request occupies
        // the slot with no user action at all. Testing liveness (and clearing
        // what is dead) keeps the slot self-healing.
        if flow_slots_are_live(&bridge.active_request, &bridge.active_sas) {
            return Ok("error: A verification is already in progress.".to_owned());
        }

        let events = Arc::clone(&bridge.events);
        let request_slot = Arc::clone(&bridge.active_request);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let nudges = Arc::clone(&bridge.recovery_nudges);
        let shutdown = Arc::clone(&bridge.verification_shutdown);
        bridge.spawn_verification_task(async move {
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

            // Claim the slot only if nothing live took it while our request
            // was in flight — an incoming request can land in exactly that
            // window, and the liveness gate above does not reserve anything.
            // Losing the race must cancel OUR request, never evict theirs.
            let claimed = match request_slot.lock() {
                Ok(mut guard) => {
                    let occupied = guard.as_ref().is_some_and(|r| {
                        !(r.is_cancelled() || r.is_done() || r.is_passive())
                    });
                    if occupied {
                        false
                    } else {
                        *guard = Some(request.clone());
                        true
                    }
                }
                Err(_) => false,
            };
            if !claimed {
                let _ = request.cancel().await;
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": "",
                    "message": "Another verification started first. Finish or cancel it, then try again.",
                }));
                return;
            }
            let _slots = FlowSlotGuard::new(
                Arc::clone(&request_slot),
                Arc::clone(&sas_slot),
                flow_id.clone(),
            );

            enqueue(&events, json!({
                "type": "verification_request_started",
                "flow_id": flow_id.clone(),
                "other_user_id": other_user,
                "is_self_verification": is_self,
            }));

            // Wait for the peer to answer. Poll for up to 5 minutes so the
            // user can pick the request up in Element without racing our
            // timer.
            //
            // Watch the STATE, not `is_ready()`. That accessor is a bare
            // `matches!(*self.inner.read(), InnerRequest::Ready(_))`
            // (matrix-sdk-crypto verification/requests.rs), so it is true
            // only while the request sits in Ready. The moment the peer's
            // `m.key.verification.start` lands, the request moves to
            // Transitioned and `is_ready()` is false FOREVER. A peer that
            // sends `.ready` and `.start` inside one sample window would
            // otherwise leave this loop polling a predicate that can never
            // become true again.
            let mut ready = false;
            for _ in 0..VERIFICATION_PEER_TICKS {
                if shutdown.load(Ordering::SeqCst) {
                    // We asked the peer to verify and are now walking away;
                    // withdraw the request instead of leaving it pending.
                    cancel_flow_best_effort(None, Some(&request)).await;
                    return;
                }
                tokio::time::sleep(
                    std::time::Duration::from_millis(VERIFICATION_POLL_MS),
                ).await;
                match request.state() {
                    // Both are "the peer answered". The driver adopts the
                    // peer's SAS from the request itself, so Transitioned
                    // needs no separate handling here.
                    VerificationRequestState::Ready { .. }
                    | VerificationRequestState::Transitioned { .. } => {
                        ready = true;
                        break;
                    }
                    // Passive (answered by another of our sessions) maps to
                    // Cancelled(Accepted) in matrix-sdk-crypto, so it lands
                    // here rather than spinning out the full five minutes.
                    VerificationRequestState::Done
                    | VerificationRequestState::Cancelled(_) => break,
                    VerificationRequestState::Created { .. }
                    | VerificationRequestState::Requested { .. } => {}
                }
            }
            if !ready {
                // Report what actually happened. Calling every exit here a
                // timeout mislabelled a peer-side or user-side cancellation.
                let was_cancelled = request.is_cancelled();
                if !was_cancelled && !request.is_done() {
                    let _ = request.cancel().await;
                }
                enqueue(&events, json!({
                    "type": "verification_cancelled",
                    "flow_id": flow_id,
                    "message": if was_cancelled {
                        "cancelled"
                    } else {
                        "timed_out_waiting_for_peer"
                    },
                }));
                return;
            }

            enqueue(&events, json!({
                "type": "verification_ready",
                "flow_id": flow_id.clone(),
            }));

            drive_sas_flow(
                &client, &request, &flow_id, &events, &sas_slot, &nudges,
                &shutdown,
            )
            .await;
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

/// v0.6.0 checkpoint 7: one async E2EE health snapshot, entirely from
/// official SDK state APIs. Emits `crypto_health` on the poll queue:
///   { device_id, device_verified, device_cross_signed,
///     own_identity_available, own_identity_verified,
///     has_master, has_self_signing, has_user_signing,
///     backup_exists_on_server, backup_state, recovery_state,
///     secret_storage_enabled, lifecycle }
/// Only booleans, enum names, and the (public) device id — never keys,
/// signatures, secrets, or store paths.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_query_crypto_health(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        bridge.spawn_room_action(async move {
            use matrix_sdk::encryption::{backups::BackupState, recovery::RecoveryState};

            let user_id = client.user_id().map(|u| u.to_owned());
            let device_id = client.device_id().map(|d| d.to_string()).unwrap_or_default();

            let mut own_identity_available = false;
            let mut own_identity_verified = false;
            if let Some(uid) = &user_id {
                if let Ok(Some(identity)) = client.encryption().get_user_identity(uid).await {
                    own_identity_available = true;
                    own_identity_verified = identity.is_verified();
                }
            }
            let mut device_verified = false;
            let mut device_cross_signed = false;
            if let Ok(Some(device)) = client.encryption().get_own_device().await {
                device_verified = device.is_verified();
                device_cross_signed = device.is_cross_signed_by_owner();
            }
            let (mut has_master, mut has_self_signing, mut has_user_signing) =
                (false, false, false);
            if let Some(status) = client.encryption().cross_signing_status().await {
                has_master = status.has_master;
                has_self_signing = status.has_self_signing;
                has_user_signing = status.has_user_signing;
            }

            let backups = client.encryption().backups();
            let backup_state = match backups.state() {
                BackupState::Unknown => "unknown",
                BackupState::Creating => "creating",
                BackupState::Enabling => "enabling",
                BackupState::Resuming => "resuming",
                BackupState::Enabled => "enabled",
                BackupState::Downloading => "downloading",
                BackupState::Disabling => "disabling",
            };
            let backup_exists = backups.exists_on_server().await.unwrap_or(false);

            let recovery_state = match client.encryption().recovery().state() {
                RecoveryState::Unknown => "unknown",
                RecoveryState::Enabled => "enabled",
                RecoveryState::Disabled => "disabled",
                RecoveryState::Incomplete => "incomplete",
            };
            let secret_storage_enabled = client
                .encryption()
                .secret_storage()
                .is_enabled()
                .await
                .unwrap_or(false);

            enqueue(&events, json!({
                "type": "crypto_health",
                "lifecycle": lifecycle,
                "device_id": device_id,
                "device_verified": device_verified,
                "device_cross_signed": device_cross_signed,
                "own_identity_available": own_identity_available,
                "own_identity_verified": own_identity_verified,
                "has_master": has_master,
                "has_self_signing": has_self_signing,
                "has_user_signing": has_user_signing,
                "backup_exists_on_server": backup_exists,
                "backup_state": backup_state,
                "recovery_state": recovery_state,
                "secret_storage_enabled": secret_storage_enabled,
            }));
        });
        Ok(String::new())
    })
}

/// v0.7.2: user-initiated "Request keys again". Nudges the live recovery
/// coordinator to run one immediate standards-based secret-request attempt
/// (fresh m.secret.request IDs through the SDK's own gossip machinery) and
/// re-arm its bounded follow-up ladder. Progress arrives as sanitized
/// `crypto_bootstrap` events; no key material crosses the FFI.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_missing_secrets(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if bridge.client.lock().ok().and_then(|g| g.clone()).is_none() {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        }
        if notify_recovery_nudge(
            &bridge.recovery_nudges,
            RecoveryNudge::ManualRequest,
        ) {
            Ok(String::new())
        } else {
            Ok("error: encryption sync is not running yet.".to_owned())
        }
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
    mention_user_ids: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        bridge
            .timelines
            .send_text(&bridge.runtime, room_id, body, mentions)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_send_reply(
    ptr: *mut c_void,
    room_id: *const c_char,
    in_reply_to_event_id: *const c_char,
    body: *const c_char,
    mention_user_ids: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let reply_to = unsafe { cstr_arg(in_reply_to_event_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        bridge
            .timelines
            .send_reply(&bridge.runtime, room_id, reply_to, body, mentions)
            .map(|_| String::new())
    })
}

// ── v0.6.0: SDK-backed thread timelines ─────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_open(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        if room_id.trim().is_empty() || root.trim().is_empty() {
            return Err("empty room or thread root id".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge.timelines.open_thread(&bridge.runtime, client, room_id, root);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close_thread();
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_paginate_back(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    count: u16,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        bridge
            .timelines
            .paginate_thread_back(&bridge.runtime, room_id, root, count)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_send_text(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    body: *const c_char,
    in_reply_to: *const c_char,
    mention_user_ids: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        // Optional rich-reply target within the thread; NULL/empty = plain
        // thread message.
        let reply_to = if in_reply_to.is_null() {
            None
        } else {
            let value = unsafe { cstr_arg(in_reply_to) }?;
            if value.trim().is_empty() { None } else { Some(value) }
        };
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .send_thread_text(
                &bridge.runtime, client, room_id, root, body, reply_to, mentions,
            )
            .map(|_| String::new())
    })
}

/// v0.6.0 checkpoint 9: list the account's Matrix devices/sessions. Merges
/// the server device list (display name, last-seen metadata) with the SDK
/// crypto store's per-device trust (is_verified / is_cross_signed_by_owner).
/// Emits a `device_list` poll event with presentation-safe fields only —
/// device ids, names, timestamps, last-seen IP as reported by the
/// homeserver — never device keys, signatures, or tokens.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_list_devices(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        bridge.spawn_room_action(async move {
            let own_device_id =
                client.device_id().map(|d| d.to_string()).unwrap_or_default();
            let user_id = client.user_id().map(|u| u.to_owned());
            let response = match client.devices().await {
                Ok(response) => response,
                Err(_err) => {
                    enqueue(&events, json!({
                        "type": "device_list",
                        "lifecycle": lifecycle,
                        "ok": false,
                        "category": "network",
                        "devices": [],
                    }));
                    return;
                }
            };
            let mut devices = Vec::new();
            for device in response.devices {
                let device_id = device.device_id.to_string();
                let mut verified = false;
                let mut cross_signed = false;
                let mut has_crypto_identity = false;
                if let Some(uid) = &user_id {
                    if let Ok(Some(crypto_device)) = client
                        .encryption()
                        .get_device(uid, &device.device_id)
                        .await
                    {
                        has_crypto_identity = true;
                        verified = crypto_device.is_verified();
                        cross_signed = crypto_device.is_cross_signed_by_owner();
                    }
                }
                devices.push(json!({
                    "device_id": device_id,
                    "display_name": device.display_name.unwrap_or_default(),
                    "last_seen_ts": device
                        .last_seen_ts
                        .map(|ts| u64::from(ts.get()))
                        .unwrap_or(0),
                    "last_seen_ip": device.last_seen_ip.unwrap_or_default(),
                    "is_current": device.device_id == own_device_id,
                    "has_crypto_identity": has_crypto_identity,
                    "verified": verified,
                    "cross_signed": cross_signed,
                }));
            }
            enqueue(&events, json!({
                "type": "device_list",
                "lifecycle": lifecycle,
                "ok": true,
                "devices": devices,
            }));
        });
        Ok(String::new())
    })
}

/// v0.6.0 checkpoint 8: manual "Retry decryption" for the open room (and
/// its open thread panel). One bounded pass per call; the SDK's own
/// AfterDecryptionFailure strategy performs any backup key download.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_retry_decryption(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .retry_visible_decryption(&bridge.runtime, client, room_id)
            .map(|_| String::new())
    })
}

// ── v0.6.0 checkpoint 5: thread list, follow state, threaded read ───────

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_list_open(
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
        bridge.timelines.open_thread_list(&bridge.runtime, client, room_id);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_list_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close_thread_list();
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_list_paginate(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        bridge
            .timelines
            .paginate_thread_list(&bridge.runtime, room_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_mark_read(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        bridge
            .timelines
            .mark_thread_read(&bridge.runtime, room_id, root)
            .map(|_| String::new())
    })
}

/// Query MSC4306 thread-subscription (follow) state. Result event:
///   thread_subscription_state { room_id, thread_root_id, supported,
///                               subscribed, automatic }
#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_subscription_query(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let root_ref = matrix_sdk::ruma::EventId::parse(&root)
            .map_err(|_| "invalid thread root id".to_owned())?;
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            match room.load_or_fetch_thread_subscription(&root_ref).await {
                Ok(subscription) => enqueue(&events, json!({
                    "type": "thread_subscription_state",
                    "room_id": room_id,
                    "thread_root_id": root,
                    "supported": true,
                    "subscribed": subscription.is_some(),
                    "automatic": subscription.map(|s| s.automatic).unwrap_or(false),
                })),
                // Coarse category only: homeservers without MSC4306 (or a
                // network failure) both surface as unsupported/unavailable —
                // the UI hides the control rather than lying about state.
                Err(_) => enqueue(&events, json!({
                    "type": "thread_subscription_state",
                    "room_id": room_id,
                    "thread_root_id": root,
                    "supported": false,
                    "subscribed": false,
                    "automatic": false,
                })),
            }
        });
        Ok(String::new())
    })
}

/// Manually follow/unfollow a thread (MSC4306). Result events: a
/// thread_subscription_result acknowledgement followed by a fresh
/// thread_subscription_state on success.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_set_subscribed(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    subscribed: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let root_ref = matrix_sdk::ruma::EventId::parse(&root)
            .map_err(|_| "invalid thread root id".to_owned())?;
        let subscribe = subscribed != 0;
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            let result = if subscribe {
                // Manual subscription (no `automatic` event id) by design:
                // this is the user's explicit Follow action.
                room.subscribe_thread(root_ref.to_owned(), None).await
            } else {
                room.unsubscribe_thread(root_ref.to_owned()).await
            };
            enqueue(&events, json!({
                "type": "thread_subscription_result",
                "room_id": room_id,
                "thread_root_id": root,
                "ok": result.is_ok(),
                "subscribed": subscribe,
            }));
            if result.is_ok() {
                enqueue(&events, json!({
                    "type": "thread_subscription_state",
                    "room_id": room_id,
                    "thread_root_id": root,
                    "supported": true,
                    "subscribed": subscribe,
                    "automatic": false,
                }));
            }
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_edit(
    ptr: *mut c_void,
    room_id: *const c_char,
    target_event_id: *const c_char,
    new_body: *const c_char,
    mention_user_ids: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let new_body = unsafe { cstr_arg(new_body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        bridge
            .timelines
            .edit(&bridge.runtime, room_id, target, new_body, mentions)
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

/// Parse an optional newline-separated FFI list argument. NULL or an empty
/// string yields an empty list; entries are trimmed and blanks dropped.
unsafe fn cstr_list_arg(value: *const c_char) -> Result<Vec<String>, String> {
    if value.is_null() {
        return Ok(Vec::new());
    }
    let raw = unsafe { cstr_arg(value) }?;
    Ok(raw
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(str::to_owned)
        .collect())
}

/// Parse the optional thread-root FFI argument. NULL/empty means the room's
/// live timeline; anything else targets that thread's timeline.
unsafe fn thread_root_arg(value: *const c_char) -> Result<String, String> {
    if value.is_null() {
        return Ok(String::new());
    }
    Ok(unsafe { cstr_arg(value) }?.trim().to_owned())
}

/// v0.7 polls: vote on an MSC3381 poll. `answer_ids` is a newline-separated
/// list of the chosen stable answer ids; an empty list retracts the vote.
/// `thread_root_id` NULL/empty targets the room timeline, else the thread.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_poll_response(
    ptr: *mut c_void,
    room_id: *const c_char,
    thread_root_id: *const c_char,
    poll_start_event_id: *const c_char,
    answer_ids: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root = unsafe { thread_root_arg(thread_root_id) }?;
        let poll_start = unsafe { cstr_arg(poll_start_event_id) }?;
        let answers = unsafe { cstr_list_arg(answer_ids) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .send_poll_response(
                &bridge.runtime, client, room_id, thread_root, poll_start, answers,
            )
            .map(|_| String::new())
    })
}

/// v0.7 polls: end an MSC3381 poll (UI offers this for own polls only; the
/// homeserver/receivers enforce the actual permission rules).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_poll_end(
    ptr: *mut c_void,
    room_id: *const c_char,
    thread_root_id: *const c_char,
    poll_start_event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root = unsafe { thread_root_arg(thread_root_id) }?;
        let poll_start = unsafe { cstr_arg(poll_start_event_id) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .end_poll(&bridge.runtime, client, room_id, thread_root, poll_start)
            .map(|_| String::new())
    })
}

/// v0.7 polls: create an MSC3381 poll in a room or thread. `answers` is a
/// newline-separated list (2..=20 after trimming); `undisclosed` != 0 hides
/// tallies until the poll ends; `max_selections` is clamped to >= 1.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_poll_create(
    ptr: *mut c_void,
    room_id: *const c_char,
    thread_root_id: *const c_char,
    question: *const c_char,
    answers: *const c_char,
    undisclosed: c_int,
    max_selections: c_uint,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root = unsafe { thread_root_arg(thread_root_id) }?;
        let question = unsafe { cstr_arg(question) }?;
        let answers = unsafe { cstr_list_arg(answers) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .send_poll_start(
                &bridge.runtime,
                client,
                room_id,
                thread_root,
                question,
                answers,
                undisclosed != 0,
                u64::from(max_selections),
            )
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

/// v0.6.1: bounded, redirect-validated HTTPS GET for an external GIF provider.
/// `url` is built C++-side and carries the provider API key; it is treated as
/// secret and never logged. The result arrives as a `gif_response` poll event
/// (ok / status / coarse category / bounded JSON body). No Matrix identifiers
/// are ever sent to the provider.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_gif_get(
    ptr: *mut c_void,
    url: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let url = unsafe { cstr_arg(url) }?;
        gifs::gif_get(bridge, url, op_id).map(|_| String::new())
    })
}

/// v0.6.1: download + validate a provider GIF, parking the bytes for
/// mx_rust_media_take (op_id key). Only https provider-CDN URLs are accepted;
/// the bytes must be a real GIF (magic + bounded canvas) or the result is a
/// rejection. Result: `gif_download_result` poll event.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_gif_download(
    ptr: *mut c_void,
    url: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let url = unsafe { cstr_arg(url) }?;
        gifs::gif_download(bridge, url, op_id).map(|_| String::new())
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

/// v0.7 Space management: MSC1772 child removal (empty-via m.space.child).
/// Never leaves or deletes the child room itself. Result event:
/// space_child_removed_result { op_id, space_id, room_id, ok }.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_remove_room_from_space(
    ptr: *mut c_void,
    space_id: *const c_char,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let space_id = unsafe { cstr_arg(space_id) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::remove_room_from_space(bridge, space_id, room_id, op_id)
            .map(|_| String::new())
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
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_thread_send_attachment(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
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
        let root = unsafe { cstr_arg(root_event_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        rooms::send_thread_attachment_path(
            bridge, room_id, root, local_path, mime, caption, width, height,
            animated != 0, op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_thread_send_attachment_bytes(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
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
        let root = unsafe { cstr_arg(root_event_id) }?;
        let filename = unsafe { cstr_arg(filename) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if data.is_null() || len == 0 {
            return Err("attachment data is empty".to_owned());
        }
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        rooms::send_thread_attachment_bytes(
            bridge, room_id, root, bytes, filename, mime, width, height, op_id,
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
    timeout_class: c_uint,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let key = unsafe { cstr_arg(key) }?;
        rooms::media_fetch(bridge, key, kind, op_id, timeout_class)
            .map(|_| String::new())
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
    // v0.7: OneShot backup download — the Element-like verified-session
    // bootstrap. When this device completes SAS verification, the SDK's
    // crypto layer automatically gossips m.secret.request for the missing
    // cross-signing secrets AND the backup recovery key; when the already
    // trusted session answers, the SDK enables backups and (with OneShot)
    // downloads EVERY backed-up room key at once, so historical encrypted
    // rooms decrypt in place without the user typing the recovery key.
    // The previous AfterDecryptionFailure strategy only fetched one key per
    // freshly-failing event, which left already-rendered history encrypted
    // after verification. Manual recovery-key entry keeps working: the 4S
    // import path funnels into the same maybe_enable_backups + OneShot
    // download. Trust policy is unchanged — no auto cross-signing creation
    // and no auto backup creation (both default false); keys only flow
    // after SDK-confirmed verification or an explicit recovery-key entry.
    let encryption_settings = matrix_sdk::encryption::EncryptionSettings {
        backup_download_strategy:
            matrix_sdk::encryption::BackupDownloadStrategy::OneShot,
        ..Default::default()
    };
    // Threading support routes m.thread events into the event cache's per-thread
    // linked chunks so TimelineFocus::Thread panels (and their live updates) can
    // populate — without it subscribe_to_thread() returns nothing and a thread
    // panel opens empty even though the root's bundled thread_summary reports
    // replies. It also makes room unread/read-receipt computation thread-aware.
    // with_subscriptions stays false: Lightning drives thread follow state
    // through the direct Room subscribe/unsubscribe/query API, not the MSC4308
    // sliding-sync extension.
    Client::builder()
        .homeserver_url(homeserver)
        .sqlite_store(store_path, None)
        .user_agent("Lightning/0.6.5")
        .with_encryption_settings(encryption_settings)
        .with_threading_support(ThreadingSupport::Enabled { with_subscriptions: false })
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
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
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
    let verif_sas_slot = Arc::clone(&active_sas);
    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: ToDeviceKeyVerificationRequestEvent| {
            let events = Arc::clone(&verif_events);
            let slot = Arc::clone(&verif_slot);
            let sas_slot = Arc::clone(&verif_sas_slot);
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

                // Never evict a live flow. This slot used to be overwritten
                // unconditionally, so a second request silently orphaned
                // whatever was in progress: its driver kept polling a flow
                // the FFI could no longer reach, and that driver's terminal
                // cleanup then cleared the NEWCOMER's handle, leaving Accept
                // with "no active verification request". A dead occupant is
                // cleared in passing. Tell the peer rather than leaving it
                // to the SDK's 10-minute timeout; the flow already on screen
                // owns the single-flow UI, so this one is not surfaced.
                if flow_slots_are_live(&slot, &sas_slot) {
                    let _ = request.cancel().await;
                    return;
                }

                // The peer's device id is real, public metadata and is what
                // tells the user WHICH session is asking. It is carried by
                // the request state, not by `their_supported_methods()` —
                // that accessor only reports methods, and mapping it to a
                // string produced an always-empty field.
                let (other_device_id, other_device_name) = match request.state() {
                    VerificationRequestState::Requested { other_device_data, .. }
                    | VerificationRequestState::Ready { other_device_data, .. } => (
                        other_device_data.device_id().to_string(),
                        other_device_data.display_name().unwrap_or_default().to_owned(),
                    ),
                    _ => (String::new(), String::new()),
                };
                enqueue(
                    &events,
                    json!({
                        "type": "verification_request_received",
                        "flow_id": request.flow_id().to_string(),
                        "other_user_id": request.other_user_id().to_string(),
                        "other_device_id": other_device_id,
                        "other_device_name": other_device_name,
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
                // v0.6.0 checkpoint 12: notification-relevant metadata for
                // rooms WITHOUT a live timeline — authoritative m.mentions
                // and the m.thread root, matching the live-timeline payload.
                let (mentions_me, mentions_room) = match &ev.content.mentions {
                    Some(mentions) => (
                        mentions.user_ids.contains(room.own_user_id()),
                        mentions.room,
                    ),
                    None => (false, false),
                };
                let thread_root_id = match &ev.content.relates_to {
                    Some(matrix_sdk::ruma::events::room::message::Relation::Thread(
                        thread,
                    )) => thread.event_id.to_string(),
                    _ => String::new(),
                };
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
                            "mentions_me": mentions_me,
                            "mentions_room": mentions_room,
                            "thread_root_id": thread_root_id,
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
        // Bounded latest-event registration state for this sync session.
        let latest_events = client.latest_events().await;
        let mut watched_latest: BTreeSet<OwnedRoomId> = BTreeSet::new();

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
                    forward_room_list_diffs(&events, batch, latest_events,
                                            &mut watched_latest).await;
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
    latest_events: &matrix_sdk::latest_events::LatestEvents,
    watched: &mut BTreeSet<OwnedRoomId>,
) {
    // Serialize the room AND register it with the lazy Latest Events API so
    // its preview/activity keep updating without the room ever being opened.
    async fn payload(
        room: Room,
        latest_events: &matrix_sdk::latest_events::LatestEvents,
        watched: &mut BTreeSet<OwnedRoomId>,
    ) -> serde_json::Value {
        watch_latest_event(latest_events, watched, room.room_id()).await;
        room_payload(&room).await
    }

    for diff in batches {
        let value = match diff {
            VectorDiff::Reset { values } => {
                let mut rooms = Vec::with_capacity(values.len());
                for item in values {
                    rooms.push(payload(item.into_inner(), latest_events, watched).await);
                }
                json!({ "type": "room_list_reset", "rooms": rooms })
            }
            VectorDiff::Append { values } => {
                let mut rooms = Vec::with_capacity(values.len());
                for item in values {
                    rooms.push(payload(item.into_inner(), latest_events, watched).await);
                }
                json!({ "type": "room_list_append", "rooms": rooms })
            }
            VectorDiff::PushFront { value } => json!({
                "type": "room_list_push_front",
                "room": payload(value.into_inner(), latest_events, watched).await
            }),
            VectorDiff::PushBack { value } => json!({
                "type": "room_list_push_back",
                "room": payload(value.into_inner(), latest_events, watched).await
            }),
            VectorDiff::PopFront => json!({ "type": "room_list_pop_front" }),
            VectorDiff::PopBack => json!({ "type": "room_list_pop_back" }),
            VectorDiff::Insert { index, value } => json!({
                "type": "room_list_insert", "index": index,
                "room": payload(value.into_inner(), latest_events, watched).await
            }),
            VectorDiff::Set { index, value } => json!({
                "type": "room_list_set", "index": index,
                "room": payload(value.into_inner(), latest_events, watched).await
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
        "last_message_preview": latest_event_preview_text(&room.latest_event()),
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

/// Presentation-safe room-list preview text for a room's cached latest
/// event. Pure so it is unit-testable.
///
/// Text-family messages surface their body (for decrypted events this is the
/// decrypted body — it travels in memory only; the C++ room list never
/// persists encrypted-room previews). Media messages surface their
/// filename-style body, matching the C++ open-room preview. Anything else —
/// still-encrypted events, state events, invites, unsent local echoes —
/// yields an empty string so the C++ side keeps its placeholder behaviour.
pub(crate) fn latest_event_preview_text(
    value: &matrix_sdk_base::latest_event::LatestEventValue,
) -> String {
    use matrix_sdk::ruma::events::{
        AnySyncMessageLikeEvent, AnySyncTimelineEvent, SyncMessageLikeEvent,
    };
    use matrix_sdk_base::latest_event::LatestEventValue;

    fn body_or(body: String, fallback: &str) -> String {
        if body.is_empty() { fallback.to_owned() } else { body }
    }

    // Room-list previews are one visual line: bodies are free-form (a poll
    // fallback carries one line per answer) and must never define room-row
    // geometry on the C++ side.
    fn one_line(text: &str) -> String {
        text.split_whitespace().collect::<Vec<_>>().join(" ")
    }

    let LatestEventValue::Remote(event) = value else {
        return String::new();
    };
    let Ok(deserialized) = event.raw().deserialize() else {
        return String::new();
    };
    let AnySyncTimelineEvent::MessageLike(message_like) = deserialized else {
        return String::new();
    };
    match message_like {
        AnySyncMessageLikeEvent::RoomMessage(SyncMessageLikeEvent::Original(message)) => {
            match message.content.msgtype {
                MessageType::Text(content) => one_line(&content.body),
                MessageType::Notice(content) => one_line(&content.body),
                MessageType::Emote(content) => one_line(&content.body),
                MessageType::Image(content) => body_or(one_line(&content.body), "Image"),
                MessageType::File(content) => body_or(one_line(&content.body), "File"),
                MessageType::Video(content) => body_or(one_line(&content.body), "Video"),
                MessageType::Audio(content) => body_or(one_line(&content.body), "Audio"),
                _ => String::new(),
            }
        }
        AnySyncMessageLikeEvent::Sticker(SyncMessageLikeEvent::Original(_)) => {
            "Sticker".to_owned()
        }
        // MSC3381 poll starts previously fell through to the empty arm, so a
        // room whose latest event was a poll showed no preview after a cold
        // start (and the live path showed the multi-line MSC1767 fallback).
        AnySyncMessageLikeEvent::UnstablePollStart(SyncMessageLikeEvent::Original(poll)) => {
            format!("Poll: {}", one_line(&poll.content.poll_start().question.text))
        }
        _ => String::new(),
    }
}

/// Rooms whose latest event the SDK actively computes. The Latest Events API
/// is lazy — without `listen_to_room` a room's latest event (and therefore
/// its room-list preview and fresh activity timestamp) is never populated;
/// this was exactly the "room information only appears after opening the
/// room" behaviour. Watching is bounded so an account with thousands of
/// rooms cannot trigger unbounded computation; the room list is
/// recency-sorted, so the first rooms delivered are the ones on screen.
const LATEST_EVENT_WATCH_CAP: usize = 200;

async fn watch_latest_event(
    latest_events: &matrix_sdk::latest_events::LatestEvents,
    watched: &mut BTreeSet<OwnedRoomId>,
    room_id: &RoomId,
) {
    if watched.contains(room_id) || watched.len() >= LATEST_EVENT_WATCH_CAP {
        return;
    }
    // Failure is non-fatal: the room simply keeps an empty preview until a
    // live event or an explicit open provides one.
    if latest_events.listen_to_room(room_id).await.unwrap_or(false) {
        watched.insert(room_id.to_owned());
    }
}

async fn room_name(room: &Room) -> String {
    if let Some(name) = room.name() {
        if !name.is_empty() {
            return name.to_owned();
        }
    }

    // The SDK's Display impl is the full Matrix room-naming algorithm
    // (explicit name -> canonical alias -> heroes -> member summary) and it
    // renders an unnamed lone-member room as "Empty Room" rather than a bare
    // MXID. Handling the variants by hand previously dropped `Empty`/`Err`
    // into the raw-room-id arm, so a legitimately unnamed room (e.g. a fresh
    // room with no name, alias, or other members) showed "!id:server" as its
    // display name across the room list and header. Render through the SDK
    // instead, and only fall back to the raw id when the SDK truly yields
    // nothing (a defensive last-resort diagnostic, never the normal path).
    match room.display_name().await {
        Ok(display_name) => {
            let rendered = display_name.to_string();
            if rendered.is_empty() {
                room.room_id().to_string()
            } else {
                rendered
            }
        }
        Err(_) => room.room_id().to_string(),
    }
}

/// Upper bound on the FFI event queue so we never grow without limit if the
/// C++ poll timer stalls (e.g. UI thread stuck under load). When we hit the
/// cap we drop the OLDEST events and inject a single `queue_overflow`
/// notice so the C++ side can log/surface the loss. The cap is generous —
/// well above the burst of `rooms` + `timeline_event` we produce on
/// initial sync of a real account.
const EVENT_QUEUE_CAP: usize = 4096;

/// v0.7 defense-in-depth: capacity of the terminal-event lane. Natural
/// population is bounded by the C++ concurrency discipline (8 media slots
/// plus a handful of commands), so this is a tripwire, not a working limit.
pub(crate) const COMMAND_QUEUE_CAP: usize = 512;

/// Enqueue an op-id-terminal result on the dedicated command lane. If the
/// tripwire cap is ever hit, the OLDEST terminal event is dropped — and its
/// parked media payload (if any) is freed first, so an overflow can never
/// leak decrypted bytes in `media_results`.
pub(crate) fn enqueue_terminal(
    queue: &EventQueueRef,
    parked: &Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    value: serde_json::Value,
) {
    let Ok(mut guard) = queue.lock() else { return };
    while guard.len() >= COMMAND_QUEUE_CAP {
        if let Some(dropped) = guard.pop_front() {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&dropped) {
                if let Some(op) = v.get("op_id").and_then(|o| o.as_u64()) {
                    if let Ok(mut results) = parked.lock() {
                        results.remove(&op);
                    }
                }
            }
        }
    }
    guard.push_back(value.to_string());
}

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

/// Like `run_async`, but executes the future on the SHARED bridge runtime
/// so tasks the SDK spawns from within it survive after the call returns.
///
/// This is MANDATORY for every path that establishes the session
/// (login/restore): matrix-sdk's `restore_session`/`login` spawn the
/// client's E2EE initialization task on the ambient runtime — the
/// verification-state updater, `Backups::setup_and_resume` (which
/// registers the `m.secret.send` listener and resumes/enables key backup),
/// `Recovery::setup`, and the backup upload/download tasks. Running those
/// paths on a throwaway per-call runtime silently KILLED all of that the
/// moment the call finished, which left every session with "backup exists
/// but this session cannot use it": a gossiped or stored backup key was
/// parked in the secret inbox with nothing alive to consume it. (v0.7.2
/// root-cause fix.)
fn run_async_on<F>(
    runtime: Arc<tokio::runtime::Runtime>,
    events: Arc<Mutex<VecDeque<String>>>,
    label: &'static str,
    future: F,
) where
    F: std::future::Future<Output = ()> + Send + 'static,
{
    let result = catch_unwind(AssertUnwindSafe(|| {
        runtime.block_on(future);
    }));
    if result.is_err() {
        enqueue(
            &events,
            json!({ "type": "error", "message": format!("Rust SDK {label} task panicked.") }),
        );
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

    // v0.7 media defense-in-depth: the terminal lane's tripwire overflow
    // frees the parked payload of the dropped event, so decrypted bytes can
    // never leak in media_results when a terminal event is discarded.
    #[test]
    fn terminal_queue_overflow_frees_parked_bytes() {
        use std::collections::{HashMap, VecDeque};
        use std::sync::{Arc, Mutex};
        let queue: super::EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let parked: Arc<Mutex<HashMap<u64, Vec<u8>>>> =
            Arc::new(Mutex::new(HashMap::new()));

        // Park a payload for the op whose terminal event will be dropped.
        parked.lock().unwrap().insert(1, vec![0u8; 64]);
        parked.lock().unwrap().insert(2, vec![0u8; 64]);

        for op in 1..=(super::COMMAND_QUEUE_CAP as u64) {
            super::enqueue_terminal(
                &queue,
                &parked,
                serde_json::json!({ "type": "media_ready", "op_id": op }),
            );
        }
        // Queue is at cap; both payloads still parked.
        assert_eq!(queue.lock().unwrap().len(), super::COMMAND_QUEUE_CAP);
        assert_eq!(parked.lock().unwrap().len(), 2);

        // One more terminal event drops the OLDEST (op 1) and frees its
        // parked bytes; op 2's payload survives.
        super::enqueue_terminal(
            &queue,
            &parked,
            serde_json::json!({ "type": "media_ready", "op_id": 9999u64 }),
        );
        assert_eq!(queue.lock().unwrap().len(), super::COMMAND_QUEUE_CAP);
        assert!(!parked.lock().unwrap().contains_key(&1));
        assert!(parked.lock().unwrap().contains_key(&2));
        // FIFO order is preserved for the survivors.
        let front = queue.lock().unwrap().front().cloned().unwrap();
        assert!(front.contains("\"op_id\":2"));
    }

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

    // v0.7.1 verification UX: the local-confirmation event carries the flow
    // id and nothing else — never emoji symbols, descriptions, decimals, or
    // key material.
    #[test]
    fn sas_confirmed_event_shape_is_flow_id_only() {
        let event = super::verification_sas_confirmed_event("flow-abc123");
        let obj = event.as_object().expect("json object");
        assert_eq!(obj.len(), 2);
        assert_eq!(obj["type"], "verification_sas_confirmed");
        assert_eq!(obj["flow_id"], "flow-abc123");
    }

    // ── Single-flow slot discipline ────────────────────────────────────
    //
    // These pin the rules that made verification look permanently dead:
    // a slot that stayed occupied refused every later attempt, and a
    // terminating flow's unconditional clear evicted a NEWER request's
    // handle. matrix-sdk's VerificationRequest/SasVerification cannot be
    // constructed outside that crate, so the rules are exercised through
    // the same FlowLiveness/FlowIdentity traits production uses.
    mod flow_slots {
        use std::sync::{Arc, Mutex};

        use crate::{FlowIdentity, FlowLiveness, FlowSlotGuard};

        #[derive(Debug)]
        struct FakeFlow {
            flow_id: String,
            finished: bool,
        }

        impl FakeFlow {
            fn live(flow_id: &str) -> Self {
                Self { flow_id: flow_id.to_owned(), finished: false }
            }
            fn finished(flow_id: &str) -> Self {
                Self { flow_id: flow_id.to_owned(), finished: true }
            }
        }

        impl FlowLiveness for FakeFlow {
            fn is_finished(&self) -> bool {
                self.finished
            }
        }

        impl FlowIdentity for FakeFlow {
            fn flow_key(&self) -> &str {
                &self.flow_id
            }
        }

        type RequestSlot = Arc<Mutex<Option<FakeFlow>>>;
        type SasSlot = Arc<Mutex<Option<(String, FakeFlow)>>>;

        fn slots() -> (RequestSlot, SasSlot) {
            (Arc::new(Mutex::new(None)), Arc::new(Mutex::new(None)))
        }

        fn request_flow(slot: &RequestSlot) -> Option<String> {
            slot.lock().unwrap().as_ref().map(|f| f.flow_id.clone())
        }

        fn sas_flow(slot: &SasSlot) -> Option<String> {
            slot.lock().unwrap().as_ref().map(|(id, _)| id.clone())
        }

        #[test]
        fn empty_slots_are_not_live() {
            let (request, sas) = slots();
            assert!(!crate::flow_slots_are_live(&request, &sas));
        }

        #[test]
        fn a_live_request_blocks_a_second_start() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-a"));
            assert!(crate::flow_slots_are_live(&request, &sas));
            // Still there: a live flow must not be silently evicted.
            assert_eq!(request_flow(&request).as_deref(), Some("flow-a"));
        }

        // The brick: an incoming request occupies the slot with no user
        // action at all, and nothing released it when the flow died. A
        // presence-only gate then refused every later attempt for the rest
        // of the process lifetime.
        #[test]
        fn a_finished_occupant_is_cleared_and_does_not_block() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::finished("flow-dead"));
            *sas.lock().unwrap() =
                Some(("flow-dead".to_owned(), FakeFlow::finished("flow-dead")));

            assert!(!crate::flow_slots_are_live(&request, &sas));
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
        }

        #[test]
        fn a_live_sas_alone_still_counts_as_live() {
            let (request, sas) = slots();
            *sas.lock().unwrap() =
                Some(("flow-b".to_owned(), FakeFlow::live("flow-b")));
            assert!(crate::flow_slots_are_live(&request, &sas));
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-b"));
        }

        #[test]
        fn releasing_clears_only_the_owning_flow() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-mine"));
            *sas.lock().unwrap() =
                Some(("flow-mine".to_owned(), FakeFlow::live("flow-mine")));

            crate::release_flow_slots(&request, &sas, "flow-mine");
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
        }

        // The clobber: a terminating driver used to run `*g = None`
        // unconditionally, wiping whichever flow happened to occupy the
        // slot — including a request that arrived after it. Accept then
        // failed with "no active verification request".
        #[test]
        fn releasing_never_evicts_a_newer_flow() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-new"));
            *sas.lock().unwrap() =
                Some(("flow-new".to_owned(), FakeFlow::live("flow-new")));

            // The OLD flow terminates and releases.
            crate::release_flow_slots(&request, &sas, "flow-old");

            assert_eq!(request_flow(&request).as_deref(), Some("flow-new"));
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-new"));
        }

        #[test]
        fn the_guard_releases_on_a_normal_exit() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-guarded"));
            {
                let _guard = FlowSlotGuard::new(
                    Arc::clone(&request),
                    Arc::clone(&sas),
                    "flow-guarded".to_owned(),
                );
            }
            assert_eq!(request_flow(&request), None);
        }

        // A driver that panicked used to leak both slots: `run_async`
        // reported the panic and nothing cleaned up, so a request parked in
        // Ready blocked verification until the app restarted.
        #[test]
        fn the_guard_releases_on_a_panic() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-panic"));
            let req = Arc::clone(&request);
            let sasc = Arc::clone(&sas);

            let result = std::panic::catch_unwind(move || {
                let _guard =
                    FlowSlotGuard::new(req, sasc, "flow-panic".to_owned());
                panic!("driver blew up");
            });

            assert!(result.is_err());
            assert_eq!(request_flow(&request), None);
        }

        // Teardown's cancellation can only work if the flow is still in the
        // slots when it runs. The first attempt at this fix put the cancel in
        // mx_rust_logout, which the C++ side calls AFTER
        // mx_rust_shutdown_tasks had already emptied both slots — so it took
        // None every time and the peer still waited out the SDK's 10-minute
        // timeout. This pins take-then-clear as one step.
        //
        // What it does NOT prove: that the cancel reaches the peer. That is a
        // network round trip through SDK types no test can construct.
        #[test]
        fn teardown_takes_the_parked_flow_instead_of_discarding_it() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-teardown"));
            *sas.lock().unwrap() =
                Some(("flow-teardown".to_owned(), FakeFlow::live("flow-teardown")));

            let (taken_sas, taken_request) =
                crate::take_pending_flows(&request, &sas);

            // Handed to the caller so it still has something to cancel...
            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            assert_eq!(
                taken_sas.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            // ...and the slots are empty, so nothing can act on it again.
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
        }

        #[test]
        fn teardown_has_nothing_to_cancel_when_no_flow_is_parked() {
            let (request, sas) = slots();
            let (taken_sas, taken_request) =
                crate::take_pending_flows(&request, &sas);
            assert!(taken_sas.is_none());
            assert!(taken_request.is_none());
        }

        // An incoming request the user never answered has no driver at all,
        // so the slot sweep is the only thing that can tell that peer we are
        // gone.
        #[test]
        fn teardown_takes_a_request_that_never_had_a_driver() {
            let (request, sas) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-unanswered"));

            let (taken_sas, taken_request) =
                crate::take_pending_flows(&request, &sas);

            assert!(taken_sas.is_none());
            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-unanswered")
            );
            assert_eq!(request_flow(&request), None);
        }

        #[test]
        fn the_guard_leaves_a_newer_flow_alone_on_a_panic() {
            let (request, sas) = slots();
            let req = Arc::clone(&request);
            let sasc = Arc::clone(&sas);

            let result = std::panic::catch_unwind(move || {
                let _guard = FlowSlotGuard::new(
                    Arc::clone(&req),
                    Arc::clone(&sasc),
                    "flow-old".to_owned(),
                );
                // A newer request claims the slot before we die.
                *req.lock().unwrap() = Some(FakeFlow::live("flow-new"));
                panic!("driver blew up");
            });

            assert!(result.is_err());
            assert_eq!(request_flow(&request).as_deref(), Some("flow-new"));
        }
    }

    // v0.7.1 secrets watchdog: the sanitized event matches the shared
    // crypto_bootstrap shape exactly (kind + fixed state string + zero
    // count + lifecycle stamp — no extra fields).
    #[test]
    fn secrets_pending_event_shape_matches_bootstrap_events() {
        use std::collections::VecDeque;
        use std::sync::{Arc, Mutex};
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));
        super::emit_crypto_bootstrap(&events, 7, "secrets_pending", "waiting", 0);
        let raw = events.lock().unwrap().pop_front().expect("one event");
        let event: serde_json::Value = serde_json::from_str(&raw).unwrap();
        let obj = event.as_object().expect("json object");
        assert_eq!(obj.len(), 5);
        assert_eq!(obj["type"], "crypto_bootstrap");
        assert_eq!(obj["kind"], "secrets_pending");
        assert_eq!(obj["state"], "waiting");
        assert_eq!(obj["count"], 0);
        assert_eq!(obj["lifecycle"], 7);
    }

    // v0.7.2 retry ladder: bounded, ordered delays; exhausted past the end.
    #[test]
    fn secret_retry_ladder_is_bounded_and_ordered() {
        let d0 = super::next_secret_retry_delay(0).expect("first attempt");
        let d1 = super::next_secret_retry_delay(1).expect("second attempt");
        let d2 = super::next_secret_retry_delay(2).expect("third attempt");
        assert!(d0 < d1 && d1 < d2, "delays must back off");
        assert_eq!(super::next_secret_retry_delay(3), None);
        assert_eq!(super::next_secret_retry_delay(usize::MAX), None);
    }

    // v0.7.2 missing-secret decision table: cross-signing private keys are
    // always required; the backup key only when the server-truth probe said
    // a backup exists.
    #[test]
    fn secret_recovery_missing_decision_table() {
        let missing = super::secret_recovery_missing;
        // The live-failure shape: nothing arrived yet.
        assert!(missing(false, Some(true), false));
        // Cross-signing incomplete alone is enough, whatever the backup.
        assert!(missing(false, Some(false), false));
        assert!(missing(false, None, true));
        // Cross-signing complete but a server-side backup is not usable.
        assert!(missing(true, Some(true), false));
        // Fully recovered.
        assert!(!missing(true, Some(true), true));
        // No backup on the server (or unknown probe): the cross-signing
        // half decides alone.
        assert!(!missing(true, Some(false), false));
        assert!(!missing(true, None, false));
    }

    // v0.7.2: every coordinator emission goes through the shared
    // crypto_bootstrap shape (kind + fixed state string + count +
    // lifecycle stamp — no extra fields, no identifiers).
    #[test]
    fn secret_request_event_shape_matches_bootstrap_events() {
        use std::collections::VecDeque;
        use std::sync::{Arc, Mutex};
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));
        super::emit_crypto_bootstrap(&events, 9, "secret_request", "requested", 2);
        let raw = events.lock().unwrap().pop_front().expect("one event");
        let event: serde_json::Value = serde_json::from_str(&raw).unwrap();
        let obj = event.as_object().expect("json object");
        assert_eq!(obj.len(), 5);
        assert_eq!(obj["type"], "crypto_bootstrap");
        assert_eq!(obj["kind"], "secret_request");
        assert_eq!(obj["state"], "requested");
        assert_eq!(obj["count"], 2);
        assert_eq!(obj["lifecycle"], 9);
    }
}

#[cfg(test)]
mod latest_event_preview_tests {
    use matrix_sdk::ruma::serde::Raw;
    use matrix_sdk_base::latest_event::{LatestEventValue, RemoteLatestEventValue};
    use serde_json::json;

    use super::latest_event_preview_text;

    fn remote(content: serde_json::Value, event_type: &str) -> LatestEventValue {
        LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(
            Raw::from_json_string(
                json!({
                    "content": content,
                    "type": event_type,
                    "event_id": "$preview0",
                    "origin_server_ts": 42,
                    "sender": "@alice:example.org",
                })
                .to_string(),
            )
            .expect("valid fixture event"),
        ))
    }

    #[test]
    fn none_value_yields_empty_preview() {
        assert_eq!(latest_event_preview_text(&LatestEventValue::None), "");
    }

    #[test]
    fn text_message_surfaces_body() {
        let value = remote(
            json!({ "msgtype": "m.text", "body": "hello rooms" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&value), "hello rooms");
    }

    #[test]
    fn media_messages_surface_filename_or_kind() {
        let named = remote(
            json!({ "msgtype": "m.image", "body": "cat.png",
                    "url": "mxc://example.org/cat" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&named), "cat.png");
        let unnamed = remote(
            json!({ "msgtype": "m.file", "body": "",
                    "url": "mxc://example.org/blob" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&unnamed), "File");
    }

    #[test]
    fn multiline_body_flattens_to_single_line_preview() {
        let value = remote(
            json!({ "msgtype": "m.text", "body": "first\nsecond\n\nthird" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&value), "first second third");
    }

    #[test]
    fn poll_start_previews_question_on_one_line() {
        let value = remote(
            json!({
                "org.matrix.msc3381.poll.start": {
                    "question": { "org.matrix.msc1767.text": "Best\nanswer?" },
                    "kind": "org.matrix.msc3381.poll.disclosed",
                    "max_selections": 1,
                    "answers": [
                        { "id": "a", "org.matrix.msc1767.text": "Yes" },
                        { "id": "b", "org.matrix.msc1767.text": "No" },
                    ],
                },
                "org.matrix.msc1767.text": "Best\nanswer?\n1. Yes\n2. No",
            }),
            "org.matrix.msc3381.poll.start",
        );
        assert_eq!(latest_event_preview_text(&value), "Poll: Best answer?");
    }

    #[test]
    fn still_encrypted_event_yields_empty_preview() {
        // An undecryptable latest event must never leak ciphertext or
        // invent a body; the C++ side renders its own placeholder.
        let value = remote(
            json!({
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "opaque",
                "device_id": "DEV",
                "sender_key": "key",
                "session_id": "session",
            }),
            "m.room.encrypted",
        );
        assert_eq!(latest_event_preview_text(&value), "");
    }

    #[test]
    fn state_event_yields_empty_preview() {
        let value = LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(
            Raw::from_json_string(
                json!({
                    "content": { "name": "Renamed room" },
                    "type": "m.room.name",
                    "state_key": "",
                    "event_id": "$preview1",
                    "origin_server_ts": 42,
                    "sender": "@alice:example.org",
                })
                .to_string(),
            )
            .expect("valid fixture event"),
        ));
        assert_eq!(latest_event_preview_text(&value), "");
    }
}

/// Live two-device E2EE interoperability harness (v0.7.2).
///
/// Runs ONLY when explicitly requested (`--ignored` plus the
/// LIGHTNING_LIVE_E2EE=1 gate) against a real homeserver with a dedicated
/// test account supplied through LIGHTNING_TEST_HOMESERVER /
/// LIGHTNING_TEST_USER / LIGHTNING_TEST_PASSWORD environment variables.
///
/// Device A ("peer") is a plain matrix-sdk 0.18 client — the same
/// verification and secret-gossip responder engine Element X ships. It
/// bootstraps cross-signing + recovery/backup, seeds encrypted history,
/// then answers the SAS flow and secret requests exactly as a trusted
/// Element session would. Device B is the REAL Lightning bridge (the C
/// FFI surface plus the recovery coordinator under test).
///
/// The peer's sync is paused right after its SAS confirmation, so
/// Lightning completes verification against a temporarily unresponsive
/// trusted session — the live failure shape — and the coordinator's
/// supervision (Verified-edge ladder arming, request rounds, secret
/// acceptance, backup enablement, OneShot restore, restart persistence)
/// must recover once the peer resumes.
///
/// The test NEVER prints event payloads, tokens, keys, or identifiers —
/// progress markers are fixed kind/state tokens only.
#[cfg(test)]
mod live_e2ee_interop_tests {
    use std::ffi::{c_void, CStr, CString};
    use std::time::{Duration, Instant};

    fn env_nonempty(name: &str) -> Option<String> {
        std::env::var(name).ok().filter(|v| !v.trim().is_empty())
    }

    unsafe fn take(ptr: *mut std::ffi::c_char) -> String {
        if ptr.is_null() {
            return String::new();
        }
        let s = CStr::from_ptr(ptr).to_string_lossy().into_owned();
        super::mx_rust_free_cstring(ptr);
        s
    }

    unsafe fn poll_all(handle: *mut c_void) -> Vec<serde_json::Value> {
        let mut out = Vec::new();
        loop {
            let raw = take(super::mx_rust_poll_event(handle));
            if raw.is_empty() {
                break;
            }
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&raw) {
                out.push(v);
            }
        }
        out
    }

    /// Drain bridge events until `pred` matches or the timeout elapses.
    /// Returns the matching event. Never logs payloads.
    unsafe fn wait_for(
        handle: *mut c_void,
        what: &str,
        timeout: Duration,
        mut pred: impl FnMut(&serde_json::Value) -> bool,
    ) -> serde_json::Value {
        let deadline = Instant::now() + timeout;
        loop {
            for ev in poll_all(handle) {
                if pred(&ev) {
                    eprintln!("[live] {what}: ok");
                    return ev;
                }
            }
            assert!(
                Instant::now() < deadline,
                "timed out waiting for {what}"
            );
            std::thread::sleep(Duration::from_millis(300));
        }
    }

    fn is_bootstrap(ev: &serde_json::Value, kind: &str) -> bool {
        ev["type"] == "crypto_bootstrap" && ev["kind"] == kind
    }

    #[test]
    #[ignore = "live homeserver E2EE interop; set LIGHTNING_LIVE_E2EE=1 and credentials env"]
    fn live_verified_session_recovers_secrets_and_backup() {
        if env_nonempty("LIGHTNING_LIVE_E2EE").as_deref() != Some("1") {
            eprintln!("[live] gate off; skipping");
            return;
        }
        // Surface the SDK's own sanitized crypto tracing (RUST_LOG
        // controlled; off unless the runner sets it).
        let _ = tracing_subscriber::fmt()
            .with_env_filter(tracing_subscriber::EnvFilter::from_default_env())
            .with_writer(std::io::stderr)
            .try_init();
        let homeserver = env_nonempty("LIGHTNING_TEST_HOMESERVER")
            .expect("LIGHTNING_TEST_HOMESERVER");
        let user = env_nonempty("LIGHTNING_TEST_USER")
            .expect("LIGHTNING_TEST_USER");
        let password = env_nonempty("LIGHTNING_TEST_PASSWORD")
            .expect("LIGHTNING_TEST_PASSWORD");

        let peer_dir = tempfile_dir("lightning-live-peer");
        let bridge_dir = tempfile_dir("lightning-live-bridge");
        let session_file = bridge_dir.join("session.json");

        // ── Peer (device A): trusted session with recovery + history. ──
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("peer runtime");
        let peer = runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(&homeserver)
                .sqlite_store(&peer_dir, None)
                .build()
                .await
                .expect("peer client");
            client
                .matrix_auth()
                .login_username(&user, &password)
                .initial_device_display_name("Lightning interop peer")
                .send()
                .await
                .expect("peer login");
            client
        });
        eprintln!("[live] peer: logged in");

        // Pauseable peer sync loop.
        let (pause_tx, pause_rx) = tokio::sync::watch::channel(false);
        {
            let client = peer.clone();
            runtime.spawn(async move {
                let mut settings = matrix_sdk::config::SyncSettings::default()
                    .timeout(Duration::from_secs(5));
                loop {
                    if *pause_rx.borrow() {
                        tokio::time::sleep(Duration::from_millis(250)).await;
                        continue;
                    }
                    match client.sync_once(settings.clone()).await {
                        Ok(resp) => {
                            settings = settings.token(resp.next_batch);
                        }
                        Err(_) => {
                            tokio::time::sleep(Duration::from_millis(750))
                                .await;
                        }
                    }
                }
            });
        }

        // Cross-signing bootstrap (fresh identity per run is fine for the
        // dedicated test account) + recovery/backup enablement.
        runtime.block_on(async {
            use matrix_sdk::ruma::api::client::uiaa;
            let encryption = peer.encryption();
            let needs_bootstrap = encryption
                .cross_signing_status()
                .await
                .map(|s| !(s.has_master && s.has_self_signing && s.has_user_signing))
                .unwrap_or(true);
            if needs_bootstrap {
                if let Err(err) = encryption.bootstrap_cross_signing(None).await {
                    let response = err
                        .as_uiaa_response()
                        .expect("bootstrap needs UIA")
                        .clone();
                    let mut auth = uiaa::Password::new(
                        uiaa::UserIdentifier::Matrix(
                            uiaa::MatrixUserIdentifier::new(user.clone()),
                        ),
                        password.clone(),
                    );
                    auth.session = response.session;
                    encryption
                        .bootstrap_cross_signing(Some(
                            uiaa::AuthData::Password(auth),
                        ))
                        .await
                        .expect("bootstrap cross-signing with UIA");
                }
            }
            eprintln!("[live] peer: cross-signing ready");
            match encryption.recovery().state() {
                matrix_sdk::encryption::recovery::RecoveryState::Enabled => {}
                _ => {
                    // Creates 4S + key backup and uploads the secrets. The
                    // returned recovery key stays in this process memory
                    // only and is dropped immediately. A stale backup from
                    // an earlier run of the DEDICATED test account (whose
                    // key is lost by design) is deleted and recreated.
                    let recovery = encryption.recovery();
                    if let Err(err) = recovery.enable().await {
                        use matrix_sdk::encryption::recovery::RecoveryError;
                        match err {
                            RecoveryError::BackupExistsOnServer => {
                                encryption
                                    .backups()
                                    .disable_and_delete()
                                    .await
                                    .expect("delete stale test backup");
                                let _recovery_key = recovery
                                    .enable()
                                    .await
                                    .expect("enable recovery after reset");
                            }
                            other => panic!("enable recovery: {other:?}"),
                        }
                    }
                }
            }
            eprintln!("[live] peer: recovery enabled");
        });

        // Seed encrypted history and wait until its keys are in backup. The
        // room carries a clearly recognizable name (and a per-process run
        // marker) so a leftover on the dedicated test account is never a bare
        // MXID, and is cleaned up at the end (see the teardown below).
        let test_room_id = runtime.block_on(async {
            use matrix_sdk::ruma::events::room::message::RoomMessageEventContent;
            let mut request =
                matrix_sdk::ruma::api::client::room::create_room::v3::Request::new();
            request.name = Some(format!(
                "Lightning E2EE Interop Test {}",
                std::process::id()
            ));
            let room = peer.create_room(request).await.expect("create room");
            room.enable_encryption().await.expect("enable encryption");
            for i in 0..3 {
                room.send(RoomMessageEventContent::text_plain(format!(
                    "interop history {i}"
                )))
                .await
                .expect("send history");
            }
            let backups = peer.encryption().backups();
            backups
                .wait_for_steady_state()
                .await
                .expect("backup steady state");
            eprintln!("[live] peer: history seeded and backed up");
            room.room_id().to_owned()
        });

        // ── Lightning (device B): the real bridge under test. ──
        let store = CString::new(bridge_dir.to_string_lossy().as_bytes())
            .unwrap();
        let handle = super::mx_rust_create(store.as_ptr());
        assert!(!handle.is_null(), "bridge handle");
        unsafe {
            let sf = CString::new(session_file.to_string_lossy().as_bytes())
                .unwrap();
            let r = take(super::mx_rust_set_session_file(handle, sf.as_ptr()));
            assert!(r.is_empty(), "set_session_file");
            let hs = CString::new(homeserver.as_bytes()).unwrap();
            let us = CString::new(user.as_bytes()).unwrap();
            let pw = CString::new(password.as_bytes()).unwrap();
            let r = take(super::mx_rust_login(
                handle, hs.as_ptr(), us.as_ptr(), pw.as_ptr(),
            ));
            assert!(r.is_empty(), "login dispatch");
            wait_for(handle, "bridge login", Duration::from_secs(45), |ev| {
                assert!(
                    ev["type"] != "login_failed",
                    "bridge login failed"
                );
                ev["type"] == "login_ok"
            });
            super::mx_rust_start_sync(handle);

            // ── SAS verification, with the peer pausing after ITS
            //    confirmation (the unresponsive-trusted-session shape). ──
            let r = take(super::mx_rust_start_own_verification(handle));
            assert!(r.is_empty(), "start verification dispatch");
            let started = wait_for(
                handle,
                "verification request started",
                Duration::from_secs(30),
                |ev| ev["type"] == "verification_request_started",
            );
            let flow_id = started["flow_id"].as_str().unwrap().to_owned();

            // Peer accepts the request and drives SAS to its own
            // confirmation.
            let peer_confirm = {
                let client = peer.clone();
                let flow = flow_id.clone();
                let uid = client.user_id().unwrap().to_owned();
                runtime.spawn(async move {
                    use matrix_sdk::encryption::verification::Verification;
                    let request = loop {
                        if let Some(r) = client
                            .encryption()
                            .get_verification_request(&uid, &flow)
                            .await
                        {
                            break r;
                        }
                        tokio::time::sleep(Duration::from_millis(400)).await;
                    };
                    request.accept().await.expect("peer accept request");
                    let sas = loop {
                        if let Some(Verification::SasV1(s)) = client
                            .encryption()
                            .get_verification(&uid, &flow)
                            .await
                        {
                            break s;
                        }
                        tokio::time::sleep(Duration::from_millis(400)).await;
                    };
                    sas.accept().await.expect("peer accept sas");
                    loop {
                        if matches!(
                            sas.state(),
                            matrix_sdk::encryption::verification::SasState::KeysExchanged { .. }
                        ) {
                            break;
                        }
                        tokio::time::sleep(Duration::from_millis(400)).await;
                    }
                    sas.confirm().await.expect("peer confirm");
                })
            };

            wait_for(
                handle,
                "sas emojis on bridge",
                Duration::from_secs(90),
                |ev| ev["type"] == "verification_sas_ready",
            );
            runtime
                .block_on(async { peer_confirm.await })
                .expect("peer confirmation task");
            // Pause the trusted session NOW: it has confirmed (its MAC is
            // out) but will not see the bridge's MAC, will not sign the
            // new device, and will not answer secret requests until
            // resumed.
            pause_tx.send(true).ok();
            eprintln!("[live] peer: paused after confirmation");

            let fid = CString::new(flow_id.as_bytes()).unwrap();
            let r = take(super::mx_rust_confirm_verification(
                handle, fid.as_ptr(),
            ));
            assert!(r.is_empty(), "bridge confirm dispatch");
            wait_for(
                handle,
                "bridge verification done",
                Duration::from_secs(60),
                |ev| ev["type"] == "verification_done",
            );

            // Give the coordinator's first ladder window a chance to run
            // against the unresponsive peer, then resume it.
            std::thread::sleep(Duration::from_secs(30));
            pause_tx.send(false).ok();
            eprintln!("[live] peer: resumed");

            // The coordinator must observe the answers and reach the
            // recovered state: cross-signing secrets present and the
            // backup enabled (OneShot bulk download included).
            wait_for(
                handle,
                "secret answer observed",
                Duration::from_secs(180),
                |ev| {
                    is_bootstrap(ev, "secret_response")
                        || (is_bootstrap(ev, "cross_signing_secrets")
                            && ev["state"] == "complete")
                },
            );
            let deadline = Instant::now() + Duration::from_secs(180);
            let health = loop {
                let r = take(super::mx_rust_query_crypto_health(handle));
                assert!(r.is_empty(), "health dispatch");
                let h = wait_for(
                    handle,
                    "crypto health snapshot",
                    Duration::from_secs(20),
                    |ev| {
                        // Surface coordinator progress markers while
                        // waiting (fixed kind/state tokens only).
                        if ev["type"] == "crypto_bootstrap" {
                            eprintln!(
                                "[live] bootstrap {} {} {}",
                                ev["kind"].as_str().unwrap_or("?"),
                                ev["state"].as_str().unwrap_or(""),
                                ev["count"].as_u64().unwrap_or(0),
                            );
                        }
                        ev["type"] == "crypto_health"
                    },
                );
                // Sanitized booleans and enum names only.
                eprintln!(
                    "[live] health master={} self={} user={} backup={} \
                     xdev={} ownid={} recovery={}",
                    h["has_master"], h["has_self_signing"],
                    h["has_user_signing"], h["backup_state"],
                    h["device_cross_signed"], h["own_identity_verified"],
                    h["recovery_state"],
                );
                let complete = h["has_master"] == true
                    && h["has_self_signing"] == true
                    && h["has_user_signing"] == true
                    && h["backup_state"] == "enabled"
                    && h["device_cross_signed"] == true
                    && h["own_identity_verified"] == true;
                if complete {
                    break h;
                }
                assert!(
                    Instant::now() < deadline,
                    "recovery never completed: cross-signing/backup state \
                     did not converge"
                );
                std::thread::sleep(Duration::from_secs(5));
            };
            assert_eq!(health["recovery_state"], "enabled");
            eprintln!("[live] bridge: secrets + backup recovered");

            // A manual re-request after completion must honestly report
            // that nothing is missing (proves the user action end to end).
            let r = take(super::mx_rust_request_missing_secrets(handle));
            assert!(r.is_empty(), "manual request dispatch");
            wait_for(
                handle,
                "manual request reports none missing",
                Duration::from_secs(30),
                |ev| {
                    is_bootstrap(ev, "secret_request")
                        && ev["state"] == "none_missing"
                },
            );

            // ── Restart persistence: same store, restored session. ──
            let stopped = super::mx_rust_stop_sync(handle);
            assert!(stopped >= 0);
            {
                // The sqlite pool releases connections via spawn_blocking;
                // give the drop an ambient runtime (harness-only concern —
                // the application tears down inside its own runtime).
                let _guard = runtime.enter();
                super::mx_rust_destroy(handle);
            }

            let handle2 = super::mx_rust_create(store.as_ptr());
            assert!(!handle2.is_null());
            let sf = CString::new(session_file.to_string_lossy().as_bytes())
                .unwrap();
            let r = take(super::mx_rust_set_session_file(handle2, sf.as_ptr()));
            assert!(r.is_empty());
            let hs = CString::new(homeserver.as_bytes()).unwrap();
            let expected = {
                let uid = peer.user_id().unwrap().to_string();
                CString::new(uid.as_bytes()).unwrap()
            };
            let r = take(super::mx_rust_restore_from_file(
                handle2, hs.as_ptr(), expected.as_ptr(),
            ));
            assert!(r.is_empty(), "restore dispatch");
            wait_for(handle2, "bridge restore", Duration::from_secs(45), |ev| {
                assert!(ev["type"] != "login_failed", "restore failed");
                ev["type"] == "login_ok"
            });
            super::mx_rust_start_sync(handle2);
            let deadline = Instant::now() + Duration::from_secs(120);
            loop {
                let r = take(super::mx_rust_query_crypto_health(handle2));
                assert!(r.is_empty());
                let h = wait_for(
                    handle2,
                    "post-restart crypto health",
                    Duration::from_secs(20),
                    |ev| ev["type"] == "crypto_health",
                );
                if h["has_master"] == true
                    && h["has_self_signing"] == true
                    && h["has_user_signing"] == true
                    && h["backup_state"] == "enabled"
                {
                    break;
                }
                assert!(
                    Instant::now() < deadline,
                    "restart lost recovered secrets or backup state"
                );
                std::thread::sleep(Duration::from_secs(5));
            }
            eprintln!("[live] bridge: restart kept secrets and backup");

            // Cleanup: sign the test device out (temp store, dedicated
            // test account) and wait for the round trip before dropping.
            super::mx_rust_logout(handle2);
            let deadline = Instant::now() + Duration::from_secs(20);
            'logout: while Instant::now() < deadline {
                for ev in poll_all(handle2) {
                    if ev["type"] == "logged_out" {
                        break 'logout;
                    }
                }
                std::thread::sleep(Duration::from_millis(300));
            }
            {
                let _guard = runtime.enter();
                super::mx_rust_destroy(handle2);
            }
        }

        // Cleanup the temporary test room so repeated runs do not litter the
        // dedicated test account with unnamed encrypted rooms (six such rooms
        // had accumulated before this). Bounded to EXACTLY the room this run
        // created (tracked by id) and performed by the peer, which — being the
        // same account — leaves it for every device. Failure is reported, not
        // hidden, and never flips the test result. Set
        // LIGHTNING_LIVE_E2EE_KEEP_ROOMS=1 to preserve the room for debugging.
        if env_nonempty("LIGHTNING_LIVE_E2EE_KEEP_ROOMS").is_none() {
            runtime.block_on(async {
                match peer.get_room(&test_room_id) {
                    Some(room) => match room.leave().await {
                        Ok(()) => {
                            let _ = room.forget().await;
                            eprintln!("[live] peer: test room left and forgotten");
                        }
                        Err(err) => eprintln!(
                            "[live] peer: test room leave FAILED, left for \
                             manual cleanup: {err:?}"
                        ),
                    },
                    None => eprintln!(
                        "[live] peer: test room not resolvable for cleanup, \
                         left for manual cleanup"
                    ),
                }
            });
        } else {
            eprintln!("[live] peer: KEEP_ROOMS set; test room preserved");
        }

        {
            let _guard = runtime.enter();
            drop(peer);
        }
        runtime.shutdown_timeout(Duration::from_secs(5));
    }

    fn tempfile_dir(prefix: &str) -> std::path::PathBuf {
        let base = std::env::temp_dir().join(format!(
            "{prefix}-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&base).expect("temp dir");
        base
    }
}
