//! matrix-client-rust — Matrix Rust SDK FFI bridge for Lightning.
//!
//! The C++ backend owns UI state and polls this bridge for JSON events. Rust
//! owns the Matrix SDK client, Tokio runtime, and SDK SQLite store.

use std::{
    collections::{BTreeSet, HashMap, VecDeque},
    ffi::{c_char, c_uchar, CStr, CString},
    fs::OpenOptions,
    io::Write,
    os::raw::{c_int, c_uint, c_void},
    panic::{catch_unwind, AssertUnwindSafe},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicI32, Ordering},
        Arc, Mutex,
    },
};

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

use matrix_sdk::{
    authentication::matrix::MatrixSession,
    config::SyncSettings,
    encryption::verification::{
        QrVerification, QrVerificationState, SasState, SasVerification, VerificationRequest,
        VerificationRequestState,
    },
    notification_settings::{
        IsEncrypted, IsOneToOne, NotificationSettings, RoomNotificationMode,
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
                member::SyncRoomMemberEvent,
                message::{MessageType, OriginalSyncRoomMessageEvent, RoomMessageEventContent},
                pinned_events::SyncRoomPinnedEventsEvent,
                power_levels::SyncRoomPowerLevelsEvent,
                tombstone::SyncRoomTombstoneEvent,
            },
            secret::send::ToDeviceSecretSendEvent,
            space::child::SpaceChildEventContent,
            typing::SyncTypingEvent,
            SyncStateEvent,
        },
        uint, EventId, OwnedDeviceId, OwnedEventId, OwnedRoomId, OwnedTransactionId,
        OwnedUserId, RoomId, UInt, UserId,
    },
    room::Receipts,
    store::RoomLoadSettings,
    Client, LoopCtrl, Room, SessionMeta, SessionTokens, ThreadingSupport,
};
use matrix_sdk::deserialized_responses::SyncOrStrippedState;
use futures_util::StreamExt;
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    room_list_service::{filters, RoomListItem, RoomListService},
    spaces::SpaceService,
    sync_service::{Error as UnifiedSyncError, State as UnifiedSyncState, SyncService},
};
use serde::{Deserialize, Serialize};
use serde_json::json;

mod banner;
mod bio;
mod calls;
mod discover;
mod gifs;
mod ignore;
mod localsearch;
mod location;
mod mediahistory;
mod namecolor;
mod widgets;
mod oauth;
mod pinned;
mod search;
mod sso;
mod stickers;
mod uia;
mod policy;
mod presence;
mod qrlogin;
mod profile;
mod rooms;
mod rtc;
mod sfu;
mod timeline;

/// The single HTTP user agent Lightning presents, to the homeserver and to any
/// user-invoked third-party request alike. Derived from `rust/Cargo.toml` so
/// the released version can never disagree with what the binary sends; the
/// CMake project version is checked against Cargo's by the `signpath-compliance`
/// test, which makes this one canonical release version end to end.
pub(crate) const USER_AGENT: &str = concat!("Lightning/", env!("CARGO_PKG_VERSION"));

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
    /// The local message index (SQLite FTS5), opened lazily inside the
    /// account's own store directory so it is deleted with the account and
    /// inherits the same 0700 protection as the SDK store beside it.
    ///
    /// A `rusqlite::Connection` is `Send` but not `Sync`, so the mutex is what
    /// makes it shareable — and it is a `std` mutex, not tokio's, because
    /// every hold is a synchronous query measured in microseconds and an
    /// async-aware lock across an `.await` is how a deadlock gets written.
    search_index: Arc<Mutex<Option<localsearch::SearchIndex>>>,
    /// Cooperative stop for the background indexer, checked between rooms.
    /// Teardown sets it, so a sweep in flight does not keep the account store
    /// open while sign-out tries to delete it.
    index_shutdown: Arc<AtomicBool>,
    /// Where each room's INDEPENDENT media-history walk has reached.
    ///
    /// Keyed by room id, because the Room Information panel browses one room
    /// at a time and reopening it should continue rather than restart. Holds
    /// only an opaque `/messages` token and counters — no event content.
    media_history: Arc<Mutex<HashMap<String, mediahistory::Cursor>>>,
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
    // Media-capable mode (2026-08-18 round 2): only when the C++ side has
    // a registered media backend do inbound call handlers include the
    // remote SDP in their poll payloads. Default OFF — production carries
    // no SDP across the FFI at all.
    call_media_capable: Arc<std::sync::atomic::AtomicBool>,
    // MatrixRTC phase 2: the one live SFU signalling session, and the
    // generation its task checks before every enqueue. The generation is a
    // separate Arc so teardown can invalidate a running task without waiting
    // on the session mutex it may be nowhere near.
    sfu: sfu::SfuState,
    sfu_generation: Arc<std::sync::atomic::AtomicU64>,
    // v0.7.x: the sliding-sync RoomListService, published by the running
    // modern sync loop and withdrawn on every exit path (RAII guard in
    // `run_modern_sync`). Room opens use it to carry exactly ONE room
    // subscription — the active room — because subscription-only required
    // state (`m.room.pinned_events`) never reaches the store otherwise:
    // a pin made this session, or by another client, would stay invisible
    // until the once-per-room `/state` probe ran again after a restart.
    room_list_service: Arc<Mutex<Option<Arc<RoomListService>>>>,
    // The room the user currently has open — the single subscription the
    // sliding sync should carry. Remembered separately from the service so
    // a room opened before the sync loop is up is subscribed as soon as
    // the service appears, and cleared in `stop_sync_and_wait` so a stale
    // room id can never be subscribed under a later account's sync.
    active_room_subscription: Arc<Mutex<Option<OwnedRoomId>>>,
    // v0.7.x UIA: at most ONE privileged operation parked between a UIA
    // challenge and the user's answer (uia.rs). Cleared on teardown so a
    // stale challenge can never be answered under a later account.
    uia_pending: Arc<Mutex<Option<uia::UiaPending>>>,
    // v0.5.7: managed room-key import task so sign-out can join it
    // deterministically instead of polling a flag with a timeout.
    import_task: Mutex<Option<tokio::task::JoinHandle<()>>>,
    // Short room-state commands (typing, receipts, invite membership and
    // marked-unread) are owned and joined during shutdown. Nothing spawned by
    // the 0.5.8 room-state layer is detached.
    room_action_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    active_typing_room: Arc<Mutex<Option<String>>>,
    receipt_targets: Arc<Mutex<HashMap<String, OwnedEventId>>>,
    // v0.9.0 receipt privacy: 0 public, 1 private (m.read.private), 2 none.
    //
    // Stored ONCE on the bridge rather than passed per call, because there
    // are three paths that send a receipt — the in-room one, mark-a-room-read
    // without opening it, and the thread panel's — and a privacy setting
    // honoured by two of three is not a privacy setting. One value, read by
    // all three.
    receipt_privacy: Arc<AtomicI32>,
    /// MSC4108 sign-in-another-device state: the generation, the one-shot
    /// check-code sender and the running task. See rust/src/qrlogin.rs.
    qr_login: Arc<qrlogin::QrLoginState>,
    receipt_serial: Arc<tokio::sync::Mutex<()>>,
    invite_actions: Arc<Mutex<BTreeSet<String>>>,
    // Server-synchronized per-room notification mode (SDK push rules).
    // Writes are serialized behind one async mutex — the SDK's
    // set_room_notification_mode is a rules read/modify/write, and two
    // interleaved tasks could otherwise each insert a rule — and coalesced
    // per room to the LATEST requested mode, mirroring the receipt pattern.
    // A room's entry lives until the task that will report for it consumes
    // it, so it doubles as the "write in flight" marker the read path
    // checks (see mx_rust_get_room_notification_mode).
    notification_mode_targets: Arc<Mutex<HashMap<String, u8>>>,
    notification_mode_serial: Arc<tokio::sync::Mutex<()>>,
    // ONE session-long NotificationSettings, created lazily on first use.
    // A fresh Client::notification_settings() per call would discard the
    // rule state the SDK applies locally after each successful write, so a
    // second write (or a read) issued before the next push-rules sync
    // would act on stale rules. Cleared with the session on sign-out /
    // detach, exactly like media_results.
    notification_settings: Arc<Mutex<Option<NotificationSettings>>>,
    // v0.5.0: SAS verification state. Single active flow at a time keeps
    // the FFI simple; a second request arriving while one is live is
    // cancelled on the wire rather than silently evicting the live flow.
    // Both slots are released by `FlowSlotGuard` when the driver ends for
    // ANY reason, and only ever for the flow that owns them.
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas) — SasVerification has no flow_id() accessor on
    // matrix-sdk 0.18, so we track it externally.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
    // The CSRF `state` of the OAuth authorization request currently in
    // flight, kept ONLY so a cancelled sign-in can call
    // `OAuth::abort_login(state)` and drop the SDK's stored validation data.
    // Scoped to this handle — and an OAuth sign-in always runs on its own
    // short-lived bootstrap handle — so two accounts can never share it.
    // Never logged: it is the anti-CSRF value for one login attempt.
    oauth_state: Arc<Mutex<Option<matrix_sdk::authentication::oauth::CsrfToken>>>,
    // Managed handle for the session-token persistence watcher (see
    // oauth::spawn_token_persistence). Held so shutdown can abort it; without
    // this it is an unowned task holding a strong Client forever.
    token_task: Arc<Mutex<Option<tokio::task::JoinHandle<()>>>>,
    // (flow_id, qr) — the SHOW-QR half of the same single-flow policy.
    // QrVerification likewise has no flow_id() accessor, so the id is
    // tracked alongside it exactly like the SAS slot. At most one of
    // active_sas / active_qr is ever occupied for a given request: the SDK
    // replaces the request's `Verification` when the flow switches method,
    // and the drivers release their own slot on the way out.
    active_qr: Arc<Mutex<Option<(String, QrVerification)>>>,
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
    // Abort handles for in-flight media fetches, keyed by op id. A fetch
    // whose requester is gone (video card closed mid-download, room left)
    // used to run to completion anyway — an uncancellable multi-hundred-MB
    // download saturating the link while every newer fetch queued behind
    // it. mx_rust_media_cancel aborts the task at its next await point;
    // tasks remove their own entry when they resolve normally. Cleared on
    // shutdown with the parked results.
    pub(crate) media_fetch_aborts: Arc<Mutex<HashMap<u64, tokio::task::AbortHandle>>>,
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
            search_index: Arc::new(Mutex::new(None)),
            index_shutdown: Arc::new(AtomicBool::new(false)),
            media_history: Arc::new(Mutex::new(HashMap::new())),
            session_file: Arc::new(Mutex::new(None)),
            client: Arc::new(Mutex::new(None)),
            events: Arc::clone(&events),
            sync_task: Mutex::new(None),
            sync_mode: Arc::new(Mutex::new(SyncMode::Stopped)),
            runtime: Arc::new(runtime),
            timelines: Arc::new(timeline::TimelineRegistry::new(events)),
            call_media_capable: Arc::new(
                std::sync::atomic::AtomicBool::new(false)),
            room_list_service: Arc::new(Mutex::new(None)),
            active_room_subscription: Arc::new(Mutex::new(None)),
            uia_pending: Arc::new(Mutex::new(None)),
            import_task: Mutex::new(None),
            room_action_tasks: Mutex::new(Vec::new()),
            active_typing_room: Arc::new(Mutex::new(None)),
            receipt_targets: Arc::new(Mutex::new(HashMap::new())),
            receipt_privacy: Arc::new(AtomicI32::new(0)),
            qr_login: Arc::new(qrlogin::QrLoginState::new()),
            receipt_serial: Arc::new(tokio::sync::Mutex::new(())),
            invite_actions: Arc::new(Mutex::new(BTreeSet::new())),
            notification_mode_targets: Arc::new(Mutex::new(HashMap::new())),
            notification_mode_serial: Arc::new(tokio::sync::Mutex::new(())),
            notification_settings: Arc::new(Mutex::new(None)),
            active_request: Arc::new(Mutex::new(None)),
            active_sas: Arc::new(Mutex::new(None)),
            oauth_state: Arc::new(Mutex::new(None)),
            token_task: Arc::new(Mutex::new(None)),
            active_qr: Arc::new(Mutex::new(None)),
            verification_tasks: Mutex::new(Vec::new()),
            verification_shutdown: Arc::new(AtomicBool::new(false)),
            import_active: Arc::new(AtomicBool::new(false)),
            media_results: Arc::new(Mutex::new(HashMap::new())),
            media_fetch_aborts: Arc::new(Mutex::new(HashMap::new())),
            command_events: Arc::new(Mutex::new(VecDeque::new())),
            bootstrap_task: Mutex::new(None),
            recovery_nudges: Arc::new(Mutex::new(None)),
            sfu: sfu::SfuState::default(),
            sfu_generation: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    fn stop_sync_and_wait(&self) -> bool {
        // The active-room subscription is user intent scoped to THIS
        // session; forget it before the sync goes down so a later account's
        // sync can never inherit — and subscribe — another account's room.
        if let Ok(mut guard) = self.active_room_subscription.lock() {
            guard.take();
        }
        // A UIA challenge belongs to the session that raised it; a later
        // account must never be able to answer it.
        if let Ok(mut guard) = self.uia_pending.lock() {
            guard.take();
        }
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
            join_task_within_budget(&mut task, "crypto-bootstrap");
        }
        let task = self.sync_task.lock().ok().and_then(|mut guard| guard.take());
        if let Some(mut task) = task {
            join_task_within_budget(&mut task, "sync");
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
    /// Join every handle under ONE budget, then abort and briefly drain
    /// whatever missed it. Returns how many missed.
    ///
    /// WHY THIS IS A FUNCTION AND NOT TWO COPIES OF FOUR LINES. The two
    /// copies it replaces each built a SECOND `join_all` over the SAME
    /// handles after the budget elapsed. `JoinHandle::poll` CONSUMES the
    /// task's output (tokio's `Core::take_output` swaps `Stage::Finished`
    /// for `Stage::Consumed`), and the first `JoinAll` — the only record of
    /// which handles had already completed — was dropped by the `timeout`.
    /// So every task that finished inside the budget was polled a second
    /// time and hit `panic!("JoinHandle polled after completion")`. With
    /// `panic = "abort"` in both profiles that is an immediate SIGABRT of
    /// the whole process: no unwind, nothing for `ffi_string`'s
    /// `catch_unwind` to catch, and no line in any log. A coredump of
    /// exactly this (2026-08-25) has the account switch in its stack.
    ///
    /// It needed TWO conditions in one batch, which is why it fired once
    /// and not reliably: something had to MISS the budget, or round two
    /// never ran at all; and something else had to have FINISHED inside it,
    /// or round two polled only pending handles and was harmless.
    ///
    /// The fix is to keep ONE `JoinAll` that OWNS the handles and poll that
    /// SAME future in both rounds — its `MaybeDone` entries remember what is
    /// done, so round two re-polls only what is still pending. A `JoinSet`
    /// would make the bug unwritable (`join_next` REMOVES each task as it is
    /// joined); that is the better long-term shape and a larger change,
    /// because `spawn_media_fetch` must keep its own `AbortHandle` registry
    /// for `mx_rust_media_cancel`.
    async fn join_or_abort(
        handles: Vec<tokio::task::JoinHandle<()>>,
        budget_secs: u64,
    ) -> usize {
        if handles.is_empty() {
            return 0;
        }
        // Collected BEFORE the vec moves into join_all: the old
        // `for handle in &handles { handle.abort(); }` has nothing left to
        // borrow once the handles are owned by the future.
        let aborts: Vec<tokio::task::AbortHandle> = handles
            .iter()
            .map(tokio::task::JoinHandle::abort_handle)
            .collect();
        let mut all = std::pin::pin!(futures_util::future::join_all(handles));
        if tokio::time::timeout(
            std::time::Duration::from_secs(budget_secs),
            all.as_mut(),
        )
        .await
        .is_ok()
        {
            return 0;
        }
        let missed = aborts.iter().filter(|a| !a.is_finished()).count();
        for a in &aborts {
            a.abort();
        }
        // The SAME future, so a handle joined in round one is never polled
        // again. This is the entire fix.
        let _ = tokio::time::timeout(
            std::time::Duration::from_secs(2),
            all.as_mut(),
        )
        .await;
        missed
    }

    // Returns (import_joined, sync_stopped, actions_missed,
    // verifications_missed, actions_ms, verifications_ms, total_ms).
    //
    // THE DURATIONS ARE THE POINT. Every wait below runs on the CALLING
    // thread — `Runtime::block_on` drives its future there, the workers only
    // serve spawned tasks — and the caller is the GUI thread, through
    // `AppController::switchToAccount` -> `detachSession` ->
    // `releaseRustHandle` -> `mx_rust_shutdown_tasks`. So the whole of this
    // function is a UI freeze, and it was the largest uninstrumented
    // GUI-thread section in the application: a reported 3-5 s freeze on an
    // account switch could not be attributed to any one of its six waits.
    // Milliseconds and counts only — no identifiers, nothing content-derived.
    fn shutdown_managed_tasks(&self)
        -> (bool, bool, usize, usize, u64, u64, u64) {
        let total = std::time::Instant::now();
        // The indexer FIRST, and before any wait: it is cooperative, checked
        // between rooms, so setting the flag here means a sweep in flight
        // stops at the next room boundary rather than after the account it was
        // walking. Closing the connection matters as much as stopping the
        // task — an open SQLite handle inside the store directory is a file
        // sign-out is about to delete, and on Windows a deletion with a handle
        // open FAILS rather than being tidied up later.
        self.index_shutdown.store(true, Ordering::Relaxed);
        if let Ok(mut guard) = self.search_index.lock() {
            *guard = None;
        }
        // The token-persistence watcher holds a strong Client purely to read
        // rotated tokens, and its broadcast sender lives INSIDE that same
        // Client — so recv() can never return Closed on its own and the task
        // would keep this account's crypto store open across mx_rust_destroy,
        // exactly the hazard described for the SAS drivers below.
        // Aborted rather than joined: it is parked in recv() and has no
        // cooperative exit.
        if let Ok(mut guard) = self.token_task.lock() {
            if let Some(handle) = guard.take() {
                handle.abort();
            }
        }
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
        let t_verifications = std::time::Instant::now();
        let verifications_missed = self.runtime.block_on(Self::join_or_abort(
            verifications,
            timeline::SHUTDOWN_JOIN_TIMEOUT_SECS,
        ));
        let verifications_ms = t_verifications.elapsed().as_millis() as u64;
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
        let (pending_sas, pending_qr, pending_request) = take_pending_flows(
            &self.active_request, &self.active_sas, &self.active_qr,
        );
        if pending_sas.is_some() || pending_qr.is_some() || pending_request.is_some() {
            self.runtime.block_on(cancel_flow_best_effort(
                pending_sas.as_ref(),
                pending_qr.as_ref(),
                pending_request.as_ref(),
            ));
        }

        self.timelines.shutdown(&self.runtime);

        // Abort still-running media downloads BEFORE joining the room-action
        // pool: nobody can consume their bytes past this point, and a live
        // multi-hundred-MB transfer would otherwise burn the entire join
        // budget below before being force-aborted.
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }

        let actions = self.room_action_tasks.lock().ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
        // v0.7 defense-in-depth: ONE overall join budget for every pending
        // room-action task (previously 15s EACH, sequentially — a handful
        // of hung media fetches could block an account switch for minutes).
        // Whatever misses the budget is aborted and briefly drained.
        let t_actions = std::time::Instant::now();
        let actions_missed = self.runtime.block_on(Self::join_or_abort(
            actions,
            timeline::SHUTDOWN_JOIN_TIMEOUT_SECS,
        ));
        let actions_ms = t_actions.elapsed().as_millis() as u64;

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
        // Late-registered abort handles (a fetch dispatched between the
        // drain above and sync stop) are cleared with the parked bytes.
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }
        // Notification-settings state is session-scoped: the cached
        // NotificationSettings holds a Client clone (and an event-handler
        // guard) that must not outlive the session, and leftover pending
        // write markers must never leak into the next account.
        if let Ok(mut guard) = self.notification_settings.lock() {
            *guard = None;
        }
        if let Ok(mut guard) = self.notification_mode_targets.lock() {
            guard.clear();
        }
        (import_joined, sync_stopped, actions_missed, verifications_missed,
         actions_ms, verifications_ms, total.elapsed().as_millis() as u64)
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

    /// Media fetches ride the same tracked room-action pool (so shutdown
    /// joins them) but additionally register an abort handle under their op
    /// id, so mx_rust_media_cancel can stop an abandoned download at its
    /// next await point. The future is responsible for removing its own
    /// entry once its network wait resolves (see rooms::media_fetch).
    fn spawn_media_fetch<F>(&self, op_id: u64, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.room_action_tasks.lock() {
            tasks.retain(|task| !task.is_finished());
            let handle = self.runtime.spawn(future);
            if let Ok(mut aborts) = self.media_fetch_aborts.lock() {
                // Bounded: drop entries whose tasks already resolved (the
                // self-removal races task completion only in theory, but a
                // stale inert handle must not accumulate either way).
                aborts.retain(|_, h| !h.is_finished());
                aborts.insert(op_id, handle.abort_handle());
            }
            tasks.push(handle);
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

/// Wait for a cancelled task's thread, but never longer than the budget.
///
/// Returns true when the thread finished and was joined, false when the
/// budget ran out and the thread was DETACHED. See
/// SYNC_TASK_JOIN_BUDGET_MS for why detaching is the right answer: the task
/// is cancelled, its session is going away, and the bridge's generation
/// guards already reject anything a straggler could emit — so the only thing
/// a longer wait buys is a longer freeze.
fn join_task_within_budget(task: &mut SyncTask, label: &str) -> bool {
    if let Some(cancel) = task.cancel.take() {
        let _ = cancel.send(());
    }
    let Some(thread) = task.thread.take() else {
        return true;
    };
    let finished = match task.done.take() {
        // Disconnected = the thread dropped its sender = it has exited.
        // A timeout is the only outcome that means "still running".
        Some(done) => !matches!(
            done.recv_timeout(std::time::Duration::from_millis(
                SYNC_TASK_JOIN_BUDGET_MS
            )),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ),
        // No signal to wait on (a task built before this existed): fall back
        // to the old behaviour rather than detaching something that may be
        // about to finish.
        None => true,
    };
    if finished {
        let _ = thread.join();
        return true;
    }
    // Deliberately NOT joined: dropping the handle detaches it.
    eprintln!(
        "lightning: {label} thread did not stop within {SYNC_TASK_JOIN_BUDGET_MS}ms; \
detaching it rather than blocking the UI"
    );
    false
}

struct SyncTask {
    cancel: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
    /// Signals that `thread` has ACTUALLY finished, so the shutdown can wait
    /// with a budget instead of blocking forever on `join()`.
    ///
    /// Nothing is ever sent on it. The thread owns the Sender, so when the
    /// thread ends — for any reason — the Sender drops and `recv_timeout`
    /// returns `Disconnected`. That is the completion signal, and it costs
    /// the running thread nothing.
    done: Option<std::sync::mpsc::Receiver<()>>,
}

/// How long a session teardown will wait for a sync or bootstrap thread to
/// notice its cancellation before giving up on it.
///
/// THIS EXISTS BECAUSE AN UNBOUNDED JOIN FROZE THE UI FOR 45 SECONDS.
/// Captured on a real desktop with LIGHTNING_GUI_STALL_TRACE: one account
/// switch reported `GUI stall 45618 ms`, and two more at startup (4943 ms and
/// 15492 ms). Each of these threads drives a `current_thread` runtime, and
/// dropping that runtime waits for any `spawn_blocking` already started —
/// which for matrix-sdk includes DNS resolution. A slow or black-holed
/// `getaddrinfo` therefore parked the GUI thread for as long as the resolver
/// took, and the same session log shows the network misbehaving
/// (`own-presence publish failed: "network"`, then `"rate_limited"`).
///
/// On timeout the thread is DETACHED rather than joined. It is cancelled, it
/// belongs to a session that is going away, and every generation guard in the
/// bridge already rejects anything it might still emit — so letting it finish
/// on its own is strictly better than making the user wait for it. This is
/// the same budgeted shape `shutdown_managed_tasks` uses for its tokio tasks;
/// these two were the only waits in the teardown with no budget at all.
const SYNC_TASK_JOIN_BUDGET_MS: u64 = 1500;

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

/// Tighten a store directory and everything in it: 0700 on the directory,
/// 0600 on every regular file. Best effort and silent — a store on a
/// filesystem with no Unix permissions must still open, and a failure here is
/// not a reason to refuse a sign-in. Called before AND after the client
/// opens, because matrix-sdk creates the sqlite files itself and gives no
/// mode hook.
#[cfg(unix)]
fn restrict_store_permissions(path: &std::path::Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700));
    if let Ok(entries) = std::fs::read_dir(path) {
        for entry in entries.flatten() {
            if entry.file_type().map(|t| t.is_file()).unwrap_or(false) {
                let _ = std::fs::set_permissions(
                    entry.path(),
                    std::fs::Permissions::from_mode(0o600),
                );
            }
        }
    }
}

#[cfg(not(unix))]
fn restrict_store_permissions(_path: &std::path::Path) {}

#[no_mangle]
pub extern "C" fn mx_rust_create(store_path: *const c_char) -> *mut c_void {
    match catch_unwind(AssertUnwindSafe(|| {
        let store_path = unsafe { cstr_arg(store_path) }?;
        let path = PathBuf::from(store_path);
        std::fs::create_dir_all(&path)
            .map_err(|err| format!("failed to create Rust SDK store directory: {err}"))?;
        // 0700 ON THE DIRECTORY, 0600 ON THE DATABASES. This directory holds
        // the Megolm and device keys; measured on a live install it was 0755
        // with 0644 sqlite files, and the only thing keeping another local
        // account out was $HOME happening to be 0700 — a distro default, not
        // a guarantee, and not one on a shared or NFS home. The far less
        // sensitive smoke-test session file has been 0600 all along.
        //
        // matrix-sdk offers no mode hook, so the databases are corrected
        // after the client has opened them. Best effort: a filesystem with no
        // Unix modes must not stop a sign-in.
        restrict_store_permissions(&path);
        let client = RustClient::new(path.clone())?;
        restrict_store_permissions(&path);
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
        let token_task = Arc::clone(&bridge.token_task);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
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
                            Arc::clone(&active_qr),
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
                                // Servers that issue refreshable password
                                // sessions rotate them exactly like OAuth, so
                                // the rotated pair must be persisted here too.
                                if let Ok(mut guard) = token_task.lock() {
                                    if let Some(previous) = guard.replace(
                                        oauth::spawn_token_persistence(
                                            &client, Arc::clone(&events)))
                                    {
                                        previous.abort();
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
                                        // Servers that issue refreshable
                                        // password sessions return one; it is
                                        // stored beside the access token so a
                                        // restart can renew instead of dying
                                        // with M_UNKNOWN_TOKEN. Absent on the
                                        // servers that do not.
                                        "refresh_token": response.refresh_token,
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
        let active_qr = Arc::clone(&bridge.active_qr);
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
                            Arc::clone(&active_qr),
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
    refresh_token: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;
        let access_token = unsafe { cstr_arg(access_token) }?;
        // Empty means "the account has no refresh token", which is the normal
        // case for a password session on a server that does not issue them.
        let refresh_token = unsafe { cstr_arg(refresh_token) }?;
        let refresh_token =
            if refresh_token.is_empty() { None } else { Some(refresh_token) };

        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
        // Shared runtime: the SDK's post-restore E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        let token_task = Arc::clone(&bridge.token_task);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "restore", async move {
                match restore_client(
                    &homeserver,
                    &store_path,
                    &user_id,
                    &device_id,
                    access_token,
                    refresh_token,
                )
                .await
                {
                    Ok(client) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                        );
                        if let Ok(mut guard) = token_task.lock() {
                            if let Some(previous) = guard.replace(
                                oauth::spawn_token_persistence(&client, Arc::clone(&events)))
                            {
                                previous.abort();
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
        let room_list_slot = Arc::clone(&bridge.room_list_service);
        let active_subscription = Arc::clone(&bridge.active_room_subscription);
        let sync_timelines = Arc::clone(&bridge.timelines);
        let sync_media_capable = Arc::clone(&bridge.call_media_capable);
        bridge.enqueue(json!({ "type": "status", "state": "syncing" }));

        let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
        // Held by the thread and never sent on: its DROP is what tells the
        // teardown this thread is really gone. See SYNC_TASK_JOIN_BUDGET_MS.
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let sync_client = client.clone();
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                run_authoritative_sync(
                    sync_client, events, sync_mode, room_list_slot,
                    active_subscription, sync_timelines, sync_media_capable,
                    cancel_rx,
                ).await;
            });
        });
        *task_slot = Some(SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
            done: Some(done_rx),
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
                let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
                let thread = std::thread::spawn(move || {
                    let _done = done_tx;
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
                    done: Some(done_rx),
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

/// Build the receipts for one read position under a privacy mode.
///
/// 0 public, 1 private (`m.read.private`), 2 none. Extracted because there
/// are two call sites — reading a room and marking one read from the room
/// list — and a privacy rule applied by one of them is not a privacy rule.
/// It is also the only part of this that can be unit tested: the rest lives
/// inside an async task holding a live `Room`.
///
/// The fully-read marker is present in EVERY mode. It is `m.fully_read`
/// account data, readable only by this user, and it is what carries their
/// own place in the conversation between their own devices and across a
/// reinstall. Dropping it would make a privacy setting cost the user their
/// unread state, which is not what any of the three modes says.
fn receipts_for_mode(event_id: OwnedEventId, mode: i32) -> Receipts {
    match mode {
        1 => Receipts::new()
            .fully_read_marker(event_id.clone())
            .private_read_receipt(event_id),
        2 => Receipts::new().fully_read_marker(event_id),
        _ => Receipts::new()
            .fully_read_marker(event_id.clone())
            .public_read_receipt(event_id),
    }
}

/// Privacy comes from the bridge's stored `receipt_privacy`
/// (`mx_rust_set_receipt_privacy`): 0 public, 1 private (MSC2285
/// `m.read.private`), 2 none.
///
/// The FULLY-READ MARKER is sent in every mode. It is account data — only
/// this user can read it — and it is what makes "where I had got to" survive
/// a reinstall. Withholding a receipt from other people is a different thing
/// from forgetting your own place, and conflating them would make the
/// privacy setting also lose the user's unread state.
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
        let mode = bridge.receipt_privacy.load(Ordering::SeqCst);
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
            let receipts = receipts_for_mode(event_id, mode);
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

/// Add or remove this room's `m.favourite` tag.
///
/// `Room::set_is_favourite` is the whole implementation: it writes the tag
/// AND drops a conflicting `m.lowpriority` tag, so the two mutually
/// exclusive states cannot both be set. Nothing here hand-builds tag
/// account data.
///
/// Deliberately NOT optimistic. The success path re-emits the room list, so
/// the Favourites section comes from `Room::is_favourite()` after the server
/// accepted the write — a rejection leaves the row exactly as it was rather
/// than showing a favourite the account does not have.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_favourite(
    ptr: *mut c_void,
    room_id: *const c_char,
    favourite: c_int,
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
            // `tag_order` stays None: Matrix orders tagged rooms by an
            // optional 0..1 float, and Lightning sorts the Favourites
            // section by activity like every other section. Inventing an
            // order here would write a preference into the ACCOUNT that no
            // Lightning surface can see or edit, and that other clients
            // would then honour.
            match room.set_is_favourite(favourite != 0, None).await {
                Ok(()) => enqueue_rooms(&events, &client).await,
                Err(_) => enqueue(&events, json!({
                    "type": "room_action_error", "action": "favourite"
                })),
            }
        });
        Ok(String::new())
    })
}

/// Send attachment bytes to a room whose live timeline is NOT open.
///
/// The timeline-scoped `mx_rust_timeline_send_attachment_bytes` refuses any
/// room but the open one, which is correct for the composer and fatal for
/// forwarding — a forward's target is by definition a room the user is not
/// looking at. Routes through `Room::send_attachment`; the SDK still
/// encrypts for the target room when it is encrypted.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_send_attachment_bytes(
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
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        rooms::send_attachment_bytes_to_room(
            bridge, room_id, bytes, filename, mime, width, height, op_id,
        )
        .map(|_| String::new())
    })
}

/// Mark a room read WITHOUT opening it.
///
/// The existing read path only advances while the room is open, focused and
/// scrolled near the bottom, and RoomListModel::markRoomRead resolved its
/// target from the LOADED timeline — which is empty for a room that is not
/// open, so marking a closed room read was a silent no-op.
///
/// The target comes from the SDK's own `Room::latest_event()`, so no
/// timeline needs to be open and nothing is guessed. Both markers are sent
/// together, exactly as the in-room path does: the public receipt is what
/// other people see, and `m.fully_read` is the user's OWN read position,
/// which is the part that syncs their place across their own devices.
///
/// When a room has no resolvable latest event there is nothing to point a
/// receipt at, so only the manual unread flag is cleared — the same
/// fallback matrix-sdk-ui's own `Timeline::mark_as_read` takes. Clearing
/// that flag is unconditional either way: a room the user explicitly marked
/// unread must not stay unread after they ask for it to be read.
/// Receipt privacy: 0 public, 1 private (`m.read.private`), 2 none.
///
/// Applies to every subsequent receipt this bridge sends, from any of the
/// three paths. It does NOT retract receipts already sent — a receipt is a
/// published fact and the protocol has no un-send for it, which is worth
/// saying plainly rather than implying the switch is retroactive.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_receipt_privacy(
    ptr: *mut c_void,
    mode: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let mode = if (0..=2).contains(&mode) { mode } else { 0 };
        bridge.receipt_privacy.store(mode, Ordering::SeqCst);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_mark_room_read(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let mode = bridge.receipt_privacy.load(Ordering::SeqCst);
        bridge.spawn_room_action(async move {
            let latest = room.latest_event().event_id();
            let mut ok = true;
            if let Some(event_id) = latest {
                // Same helper as the in-room path: marking a room read
                // from the room list must not disclose more than opening it.
                let receipts = receipts_for_mode(event_id, mode);
                ok = room.send_multiple_receipts(receipts).await.is_ok();
            }
            // Unconditional: an explicitly marked-unread room must not stay
            // unread just because it had no event to receipt.
            if room.set_unread_flag(false).await.is_err() {
                ok = false;
            }
            if ok {
                enqueue(&events, json!({
                    "type": "read_marker_advanced", "room_id": room_id
                }));
                enqueue_rooms(&events, &client).await;
            } else {
                enqueue(&events, json!({
                    "type": "room_action_error", "action": "mark_read",
                    "room_id": room_id
                }));
            }
        });
        Ok(String::new())
    })
}

/// Lightning's per-room notification-mode integers, shared with the C++
/// side (SettingsManager / NotificationManager::RoomMode): 0 = all
/// messages, 1 = mentions & keywords only, 2 = mute. The mapping is
/// label-faithful — mode 0 sets an explicit AllMessages rule server-side.
/// A separate "follow account default" choice (user-rule removal via
/// delete_user_defined_room_rules) is an accepted follow-up; the UI does
/// not offer it yet.
pub(crate) fn notification_mode_to_int(mode: RoomNotificationMode) -> u8 {
    match mode {
        RoomNotificationMode::AllMessages => 0,
        RoomNotificationMode::MentionsAndKeywordsOnly => 1,
        RoomNotificationMode::Mute => 2,
    }
}

pub(crate) fn notification_mode_from_int(mode: c_int) -> Option<RoomNotificationMode> {
    match mode {
        0 => Some(RoomNotificationMode::AllMessages),
        1 => Some(RoomNotificationMode::MentionsAndKeywordsOnly),
        2 => Some(RoomNotificationMode::Mute),
        _ => None,
    }
}

/// True when `mode` is still the newest requested mode for `room_id`
/// (checked before the server write; a superseded task must not write).
fn is_latest_notification_target(
    targets: &Arc<Mutex<HashMap<String, u8>>>,
    room_id: &str,
    mode: u8,
) -> bool {
    targets.lock().ok().and_then(|guard| guard.get(room_id).copied()) == Some(mode)
}

/// Consume the room's pending-write marker iff this task's mode is still
/// the newest AFTER its server round-trip. Returns true when consumed —
/// this task owns the room's authoritative report (success or failure).
/// Returns false when a newer set superseded this one mid-flight: the
/// marker is left in place and the newer task (queued behind the write
/// serial) produces the room's report instead.
fn take_notification_target_if_latest(
    targets: &Arc<Mutex<HashMap<String, u8>>>,
    room_id: &str,
    mode: u8,
) -> bool {
    if let Ok(mut guard) = targets.lock() {
        if guard.get(room_id).copied() == Some(mode) {
            guard.remove(room_id);
            return true;
        }
    }
    false
}

/// Clears this task's pending-write marker on ANY exit path — normal
/// completion (where the authoritative-report consume usually got there
/// first and this is a no-op), supersession, a panic inside the SDK
/// write, an abort during the shutdown join window, or the spawn path
/// declining so the future is dropped unpolled (the guard is created at
/// the FFI entry and MOVED into the future precisely so that last case
/// still drops it). Without this, an orphaned marker would keep
/// `notification_write_pending()` true forever and silently disable
/// read reports for the room for the rest of the session. Only the
/// exact (room, mode) pair this task inserted is removed — a newer
/// task's marker is never touched.
struct NotificationTargetGuard {
    targets: Arc<Mutex<HashMap<String, u8>>>,
    room_id: String,
    mode: u8,
}

impl Drop for NotificationTargetGuard {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.targets.lock() {
            if guard.get(&self.room_id).copied() == Some(self.mode) {
                guard.remove(&self.room_id);
            }
        }
    }
}

/// True while a set for this room is queued or in flight (its marker is
/// consumed only when the owning task reports).
fn notification_write_pending(
    targets: &Arc<Mutex<HashMap<String, u8>>>,
    room_id: &str,
) -> bool {
    targets
        .lock()
        .ok()
        .map(|guard| guard.contains_key(room_id))
        .unwrap_or(false)
}

/// The session's single NotificationSettings, created lazily on first use.
/// All clones share one inner rule set (Arc), so a read issued after a
/// completed write observes the locally-applied post-write rules instead
/// of a stale fresh snapshot. Racing creators are resolved by
/// double-checking under the lock; the loser's instance simply drops.
/// Known limitation (accepted follow-up): if the FIRST call lands before
/// the initial sync delivered m.push_rules, the SDK builds the instance
/// from a fallback rule set (server_default, or empty on a store read
/// error) and that fallback stays cached until a PushRulesEvent arrives.
/// The per-call construction this replaced self-healed but discarded
/// post-write state (a worse trade). Invalidate-after-first-sync would
/// close the window.
async fn notification_settings_handle(
    slot: &Arc<Mutex<Option<NotificationSettings>>>,
    client: &Client,
) -> NotificationSettings {
    if let Some(existing) = slot.lock().ok().and_then(|guard| guard.clone()) {
        return existing;
    }
    let created = client.notification_settings().await;
    if let Ok(mut guard) = slot.lock() {
        if let Some(existing) = guard.clone() {
            return existing;
        }
        *guard = Some(created.clone());
    }
    created
}

/// Set the account's per-room notification mode through the SDK's push-rule
/// manager (`NotificationSettings::set_room_notification_mode`). All rule
/// construction, keyword handling, and conflicting-rule cleanup stay inside
/// matrix-sdk — this bridge never builds or inspects rule JSON. Success
/// enqueues a `room_notification_mode` report; failure enqueues a dedicated
/// sanitized `notification_mode_error` (room id only, never the SDK error
/// text, which can embed rule bodies) so the UI can show an honest
/// "kept on this device" state. Automatic retry on reconnect is an
/// accepted follow-up; this round reports the failure and stops.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_notification_mode(
    ptr: *mut c_void,
    room_id: *const c_char,
    mode: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let mode = notification_mode_from_int(mode)
            .ok_or_else(|| "invalid notification mode".to_owned())?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let targets = Arc::clone(&bridge.notification_mode_targets);
        let serial = Arc::clone(&bridge.notification_mode_serial);
        let settings_slot = Arc::clone(&bridge.notification_settings);
        let my_mode = notification_mode_to_int(mode);
        if let Ok(mut guard) = targets.lock() {
            guard.insert(room_id.clone(), my_mode);
        }
        // Created OUTSIDE the future and moved into it: if the spawn path
        // declines and the future is dropped unpolled, the guard still
        // drops and the marker cannot orphan (see NotificationTargetGuard).
        let target_guard = NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room_id.clone(),
            mode: my_mode,
        };
        bridge.spawn_room_action(async move {
            let _target_guard = target_guard;
            let _serial = serial.lock().await;
            // Already superseded before this task even ran — skip the
            // write; the task holding the newest target performs it.
            if !is_latest_notification_target(&targets, &room_id, my_mode) {
                return;
            }
            let settings = notification_settings_handle(&settings_slot, &client).await;
            let result = settings.set_room_notification_mode(room.room_id(), mode).await;
            // Re-check AFTER the round-trip: a newer choice may have been
            // queued while this write was in flight (its FFI entry replaced
            // the target before its task blocked on the serial). This task
            // must then report NOTHING — neither its now-stale mode as
            // authoritative nor a failure for a choice the user already
            // replaced; the newer task, next on the serial, produces the
            // room's authoritative report. Consuming the marker on report
            // is also what lets the read path treat "marker present" as
            // "write still in flight".
            if !take_notification_target_if_latest(&targets, &room_id, my_mode) {
                return;
            }
            match result {
                Ok(()) => enqueue(&events, json!({
                    "type": "room_notification_mode",
                    "room_id": room_id,
                    "mode": my_mode,
                    "user_defined": true,
                })),
                Err(_) => enqueue(&events, json!({
                    "type": "notification_mode_error",
                    "room_id": room_id,
                })),
            }
        });
        Ok(String::new())
    })
}

/// v0.7: real participants of a thread, for the summary-card facepile.
/// Answers asynchronously with a `thread_participants` poll event carrying
/// presentation-safe rows only (user id, display name, avatar mxc) — never
/// event content. See rooms::thread_participants for why this cannot come
/// from the SDK's thread summary.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_thread_participants(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root_event_id = unsafe { cstr_arg(root_event_id) }?;
        rooms::thread_participants(bridge, room_id, root_event_id)
            .map(|_| String::new())
    })
}

/// 2026-08-18: redact this message's OWN `m.replace` edits, returning it to
/// its original text. Answers asynchronously with a `message_edits_removed`
/// poll event carrying counts only. See rooms::remove_message_edits.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_remove_message_edits(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        rooms::remove_message_edits(bridge, room_id, event_id).map(|_| String::new())
    })
}

/// v0.7: "follow account default" — REMOVE the room's user-defined push
/// rules so the account's own rules decide again.
///
/// This is the honest server-side representation of the choice: Matrix has
/// no "follow default" rule, it has the ABSENCE of a room override. The SDK
/// owns the rule deletion (`delete_user_defined_room_rules`); nothing here
/// writes push-rule JSON.
///
/// Shares the set path's serialization and target marker (using mode 3 as
/// this room's target) so a clear and a set issued back to back cannot land
/// out of order or report each other's outcome. Success reports
/// `user_defined: false` — which is precisely what the room's state now is.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_clear_room_notification_mode(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let targets = Arc::clone(&bridge.notification_mode_targets);
        let serial = Arc::clone(&bridge.notification_mode_serial);
        let settings_slot = Arc::clone(&bridge.notification_settings);
        // 3 = follow-account-default, the C++ side's RoomMode::FollowDefault.
        const FOLLOW_DEFAULT: u8 = 3;
        if let Ok(mut guard) = targets.lock() {
            guard.insert(room_id.clone(), FOLLOW_DEFAULT);
        }
        let target_guard = NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room_id.clone(),
            mode: FOLLOW_DEFAULT,
        };
        bridge.spawn_room_action(async move {
            let _target_guard = target_guard;
            let _serial = serial.lock().await;
            if !is_latest_notification_target(&targets, &room_id, FOLLOW_DEFAULT) {
                return;
            }
            let settings = notification_settings_handle(&settings_slot, &client).await;
            let result = settings
                .delete_user_defined_room_rules(room.room_id())
                .await;
            if !take_notification_target_if_latest(&targets, &room_id, FOLLOW_DEFAULT) {
                return;
            }
            match result {
                Ok(()) => enqueue(&events, json!({
                    "type": "room_notification_mode",
                    "room_id": room_id,
                    "mode": FOLLOW_DEFAULT,
                    // No user-defined rule exists for this room any more.
                    // That is the whole point of the operation, so it is
                    // reported truthfully rather than as a user rule of 3.
                    "user_defined": false,
                    "followed_default": true,
                })),
                Err(_) => enqueue(&events, json!({
                    "type": "notification_mode_error",
                    "room_id": room_id,
                })),
            }
        });
        Ok(String::new())
    })
}

/// Report a room's current notification mode: the account's user-defined
/// room rule when one exists, otherwise the account DEFAULT resolved for
/// this room's shape (encrypted? one-to-one?), flagged `user_defined:false`.
/// Reads are local rule-set lookups on the shared session
/// NotificationSettings (no server round-trip), so there is no
/// asynchronous failure path and no need for the write serial. Refresh is
/// poll-on-open: the C++ side calls this when a notification picker opens;
/// a live `subscribe_to_changes` push-rule watcher is an accepted
/// follow-up, not implemented here.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_get_room_notification_mode(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let room = RoomId::parse(&room_id).ok().and_then(|id| client.get_room(&id))
            .ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let targets = Arc::clone(&bridge.notification_mode_targets);
        let settings_slot = Arc::clone(&bridge.notification_settings);
        bridge.spawn_room_action(async move {
            // A write for this room is queued or in flight: its own report
            // (or failure event) is authoritative and imminent, and a read
            // taken now could observe the pre-write rules and — running on
            // another worker thread — enqueue AFTER the write's report,
            // wedging the C++ cache on the stale mode. Bail instead; the
            // pending write reports for the room.
            if notification_write_pending(&targets, &room_id) {
                return;
            }
            let settings =
                notification_settings_handle(&settings_slot, &client).await;
            if let Some(mode) = settings
                .get_user_defined_room_notification_mode(room.room_id())
                .await
            {
                enqueue(&events, json!({
                    "type": "room_notification_mode",
                    "room_id": room_id,
                    "mode": notification_mode_to_int(mode),
                    "user_defined": true,
                }));
                return;
            }
            // No per-room rule: resolve the account default with the same
            // inputs the SDK's own helpers use. `encryption_state()` is the
            // store's current knowledge; a still-Unknown state deliberately
            // maps to NotEncrypted (it only varies which default push rule
            // answers — never crypto behavior) and this read path fires no
            // state request. One-to-one uses JOINED members: the server
            // evaluates `.m.rule.room_one_to_one`'s member_count against
            // joined members, so counting invitees (active_members_count)
            // would resolve a different default than push evaluation uses.
            let is_encrypted = room.encryption_state().is_encrypted();
            let is_one_to_one = room.joined_members_count() == 2;
            let mode = settings
                .get_default_room_notification_mode(
                    IsEncrypted::from(is_encrypted),
                    IsOneToOne::from(is_one_to_one),
                )
                .await;
            enqueue(&events, json!({
                "type": "room_notification_mode",
                "room_id": room_id,
                "mode": notification_mode_to_int(mode),
                "user_defined": false,
            }));
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
/// How long a displayed QR code waits to be scanned before the flow falls
/// back to SAS (120 s). Bounded on purpose: see `drive_qr_flow`.
const QR_DISPLAY_TICKS: u32 = 240;
/// How long the flow may take to complete AFTER the peer scanned (240 s).
///
/// Deliberately twice `SAS_COMPLETION_TICKS`. Past the scan this step is
/// gated on a HUMAN reading a result off a second device and coming back to
/// confirm — often physically walking to it — whereas SAS asks both users to
/// compare emoji already on screen. Timing the two the same would abort a
/// perfectly good verification while the user was still walking back.
const QR_COMPLETION_TICKS: u32 = 480;
/// Sanity bound on the module count of a QR code we will render. QR version
/// 40 — far above anything a verification payload needs — is 177 modules
/// per side, so anything beyond this is a malformed grid, not a big code.
const QR_MAX_MODULES: usize = 200;

/// The verification methods Lightning advertises, in BOTH directions.
///
/// * `m.sas.v1` — emoji verification. The universal fallback, and the only
///   method that works with a peer that can neither show nor scan.
/// * `m.qr_code.show.v1` — Lightning DISPLAYS a QR code for the other
///   device to scan.
/// * `m.reciprocate.v1` — the method name of the
///   `m.key.verification.start` a SCANNING peer sends back after reading
///   our code. Advertising `show` without it would leave the peer no legal
///   way to answer the code we displayed.
///
/// `m.qr_code.scan.v1` is deliberately ABSENT. Lightning has no camera and
/// no scanner, so claiming it would invite a peer to display a code we can
/// never read; the peer would then sit waiting on a reciprocate that never
/// comes, all the way to matrix-sdk-crypto's 10-minute VERIFICATION_TIMEOUT.
///
/// This replaces the SAS-only vectors introduced by c259b60. That commit's
/// rationale was correct *for a build without the `qrcode` feature*: with
/// the feature off, matrix-sdk-crypto compiles no `ReciprocateV1` arm into
/// `receive_start` at all, so a reciprocate start was answered with a
/// warning and NO cancel, stalling the peer. The feature is now enabled
/// (see rust/Cargo.toml), that arm exists, and advertising reciprocate is
/// precisely what makes showing a QR code possible.
fn advertised_verification_methods() -> Vec<VerificationMethod> {
    vec![
        VerificationMethod::SasV1,
        VerificationMethod::QrCodeShowV1,
        VerificationMethod::ReciprocateV1,
    ]
}

/// Pack a QR module grid into row-major bits and base64 it for the FFI.
///
/// `modules[y * size + x] == true` means a DARK module. Bits are packed
/// most-significant-bit-first within each byte, and every ROW starts on a
/// fresh byte, so the C++ renderer can address a row at `y * stride`
/// (`stride = (size + 7) / 8`) without carrying a bit offset across rows.
///
/// ONLY this geometry crosses the FFI. A verification QR payload encodes
/// cross-signing key material and the flow's shared secret, so the decoded
/// bytes are never logged, persisted, or placed in any error text — the
/// module grid is rendered and forwarded, and nothing else.
fn pack_qr_modules(modules: &[bool], size: usize) -> Option<String> {
    if size == 0 || size > QR_MAX_MODULES || modules.len() != size * size {
        return None;
    }
    let stride = size.div_ceil(8);
    let mut packed = vec![0u8; stride * size];
    for y in 0..size {
        for x in 0..size {
            if modules[y * size + x] {
                packed[y * stride + x / 8] |= 0x80u8 >> (x % 8);
            }
        }
    }
    use base64::Engine;
    Some(base64::engine::general_purpose::STANDARD.encode(&packed))
}

/// Encode arbitrary bytes as a QR code and return the (size, packed-bits)
/// pair the UI draws.
///
/// Split out of `render_qr_payload` when MSC4108 login arrived: it needs the
/// same encoder over `QrCodeData::to_bytes()` rather than over a
/// `QrVerification`, and the C++ side has no QR encoder at all.
pub(crate) fn render_qr_bytes(bytes: &[u8]) -> Option<(usize, String)> {
    use matrix_sdk_base::crypto::matrix_sdk_qrcode::qrcode::{EcLevel, QrCode};
    // The lowest error correction the format allows. A sign-in payload is
    // large and this is read from a screen a few centimetres away, not off a
    // printed label — spending capacity on redundancy here only makes the
    // code denser and harder to scan.
    let code = QrCode::with_error_correction_level(bytes, EcLevel::L).ok()?;
    let size = code.width();
    let modules: Vec<bool> =
        code.to_colors().into_iter().map(|c| c.select(true, false)).collect();
    pack_qr_modules(&modules, size).map(|bits| (size, bits))
}

/// Render an SDK `QrVerification` to the (size, packed-bits) pair the UI
/// needs. Returns `None` if the SDK could not encode the code at all.
fn render_qr_payload(qr: &QrVerification) -> Option<(usize, String)> {
    // `EncodingError` is discarded because it is not user-actionable and the
    // flow is not broken by it — SAS still runs. It carries no secret: the
    // type is `Qr(qrcode::types::QrError) | FlowId(TryFromIntError)`
    // (matrix-sdk-qrcode error.rs), neither of which quotes payload bytes.
    let code = qr.to_qr_code().ok()?;
    let size = code.width();
    // `Color::select(dark, light)` avoids naming `qrcode::Color`, which
    // matrix-sdk does not re-export.
    let modules: Vec<bool> =
        code.to_colors().into_iter().map(|c| c.select(true, false)).collect();
    pack_qr_modules(&modules, size).map(|bits| (size, bits))
}

/// How the show-QR leg of a flow ended.
enum QrOutcome {
    /// Terminal for the whole flow: `verification_done`,
    /// `verification_cancelled` or `verification_failed` has been emitted.
    Finished,
    /// The QR leg is over but the REQUEST is still alive and must continue
    /// on SAS.
    FallBackToSas,
    /// The session is going away; the flow was cancelled best-effort.
    ShuttingDown,
}

/// Ask the SDK for a QR code to display, and publish its module grid.
///
/// Returns `None` whenever showing a code is not possible — which is a
/// normal outcome, never an error, because SAS remains available in every
/// one of those cases.
async fn maybe_generate_qr(
    request: &VerificationRequest,
    flow_id: &str,
    events: &Arc<Mutex<VecDeque<String>>>,
) -> Option<QrVerification> {
    // NEVER generate a code once the request has left Ready.
    //
    // `generate_qr_code()` is PERMITTED from `Transitioned` (matrix-sdk-crypto
    // verification/requests.rs: `InnerRequest::Transitioned(s) =>
    // s.generate_qr_code(..)`), and it ends in `VerificationCache::insert`,
    // which is destructive:
    //
    //     // Cancel all the old verifications as well as the new one we have
    //     // for this user if someone tries to have two verifications going
    //     // on at once.
    //
    // (verification/cache.rs — `insert_qr` calls `insert`, unlike
    // `replace_sas` which calls the non-cancelling `replace`.) So if the peer
    // sent `.ready` and `.start` inside one poll window — the exact race the
    // outbound peer-wait loop already accepts `Transitioned` for — generating
    // a code here would cancel the live SAS the peer just started AND the new
    // QR, and the user would watch a working verification die with a
    // `verification_cancelled` nobody asked for.
    //
    // Staying in Ready means the request has no verification installed yet,
    // so `insert` has nothing to cancel. A `Transitioned` request falls
    // straight through to `drive_sas_flow`, which adopts the peer's Sas.
    //
    // HONEST LIMITATION: this narrows the window but cannot close it — the
    // state may still change between this check and the call. It is also NOT
    // unit-testable: `VerificationRequest` has crate-private constructors, so
    // no test in this repository can build one in either state.
    if !matches!(request.state(), VerificationRequestState::Ready { .. }) {
        return None;
    }
    // Only attempt when the peer advertised that it can SCAN.
    // matrix-sdk-crypto enforces the same rule itself and returns `Ok(None)`
    // ("if the other side doesn't support scanning QR codes bail early",
    // verification/requests.rs `generate_qr_code`), so this is an
    // optimisation and a documentation point rather than the safety net —
    // the `Ok(None)` arm below is the real one.
    let peer_can_scan = request
        .their_supported_methods()
        .is_some_and(|methods| methods.contains(&VerificationMethod::QrCodeScanV1));
    if !peer_can_scan {
        return None;
    }
    let qr = match request.generate_qr_code().await {
        Ok(Some(qr)) => qr,
        // Expected, not exceptional. `Ok(None)` here means the account has no
        // cross-signing identity at all, or its identity carries no master
        // key — there is nothing to bind into a code. Note this is NOT the
        // ordinary new-session case: a fresh session on a cross-signed
        // account still gets a code through the SDK's `new_self_no_master`
        // branch, which is precisely the sign-in-a-new-device flow.
        Ok(None) => return None,
        // Not user-actionable, and the flow is not broken — SAS still runs —
        // so this is dropped rather than surfaced.
        Err(_) => return None,
    };
    let (size, bits_b64) = render_qr_payload(&qr)?;
    enqueue(events, json!({
        "type": "verification_qr_ready",
        "flow_id": flow_id,
        "size": size,
        "bits_b64": bits_b64,
    }));
    Some(qr)
}

/// Drive a displayed QR code to a terminal state, or hand the request back
/// for SAS.
///
/// Polls rather than consuming `qr.changes()` for the same reason
/// `drive_sas_flow` polls: the decisive transition here is a REQUEST-level
/// one. When the peer answers our code with `m.key.verification.start`
/// carrying `m.sas.v1`, matrix-sdk-crypto replaces the request's
/// `Verification` with a `Sas` and the `QrVerification` we hold simply
/// stops changing — a `changes()` stream on it would report nothing at all
/// and the flow would hang until the timeout. Polling both the QR state and
/// the request state is what makes the "peer cannot scan" fallback visible.
///
/// Never reports success unless the SDK reached `QrVerificationState::Done`,
/// and never confirms on the user's behalf: `Scanned` is surfaced to the UI
/// and the flow then waits for an explicit
/// `mx_rust_confirm_qr_verification` call.
async fn drive_qr_flow(
    request: &VerificationRequest,
    qr: &QrVerification,
    flow_id: &str,
    events: &Arc<Mutex<VecDeque<String>>>,
    nudges: &RecoveryNudgeSlot,
    shutdown: &Arc<AtomicBool>,
) -> QrOutcome {
    let poll = std::time::Duration::from_millis(VERIFICATION_POLL_MS);
    let mut emitted_scanned = false;
    let mut emitted_confirmed = false;
    let mut display_ticks: u32 = 0;
    let mut progress_ticks: u32 = 0;

    loop {
        if shutdown.load(Ordering::SeqCst) {
            cancel_flow_best_effort(None, Some(qr), Some(request)).await;
            return QrOutcome::ShuttingDown;
        }
        tokio::time::sleep(poll).await;

        // ONE snapshot per tick, classified with no await inside, so nothing
        // SDK-owned is held across a suspend point.
        let state = qr.state();

        // A REQUEST-level termination is invisible to `qr.state()`: the QR
        // object simply stops changing. That happens when another of our own
        // sessions answers the request (`Passive` — a distinct InnerRequest
        // state, NOT covered by `is_cancelled()`), and when a cancel lands in
        // the window between emitting `verification_ready` and this loop
        // taking over. Without this check the code would stay on screen,
        // scannable and dead, for the whole display window. `drive_sas_flow`
        // guards its pre-SAS wait the same way.
        //
        // Only NON-SUCCESS request terminals may trigger this exit.
        // `request.is_done()` is deliberately ABSENT: the SDK writes the
        // request's Done synchronously but the QR's Done only after an
        // awaited signing/store round (machine.rs receive_done ordering),
        // so a tick sampling qr.state() before that second write while the
        // request already reads Done would report a SUCCESSFUL verification
        // as cancelled — and request-Done is a success terminal, so the
        // "cancelled" label would be wrong even outside the race. A
        // request-Done-but-QR-pending situation resolves on a later tick
        // when the QR's own Done lands, or via the bounded
        // QR_COMPLETION_TICKS expiry (which cancels on the wire first).
        // Both cancel paths ARE written synchronously into the QR by the
        // sync task (machine.rs), so for is_cancelled/is_passive the
        // QR-verdict-first ordering below is genuinely race-free.
        let qr_reached_verdict = matches!(
            state,
            QrVerificationState::Done { .. } | QrVerificationState::Cancelled(_)
        );
        if !qr_reached_verdict
            && (request.is_cancelled() || request.is_passive())
        {
            enqueue(events, json!({
                "type": "verification_cancelled",
                "flow_id": flow_id,
                "message": "cancelled",
            }));
            return QrOutcome::Finished;
        }

        match state {
            QrVerificationState::Done { .. } => {
                enqueue(events, json!({
                    "type": "verification_done",
                    "flow_id": flow_id,
                }));
                notify_recovery_nudge(nudges, RecoveryNudge::VerificationDone);
                return QrOutcome::Finished;
            }
            QrVerificationState::Cancelled(info) => {
                enqueue(events, json!({
                    "type": "verification_cancelled",
                    "flow_id": flow_id,
                    "message": format!("{:?}", info.reason()),
                }));
                return QrOutcome::Finished;
            }
            // The peer read our code and sent `m.reciprocate.v1`. The SDK
            // will NOT complete the flow until we confirm, and confirming
            // is the user's decision — the whole security value of showing
            // a code is that a human checks the other device really did
            // report success.
            QrVerificationState::Scanned => {
                if !emitted_scanned {
                    emitted_scanned = true;
                    enqueue(events, json!({
                        "type": "verification_qr_scanned",
                        "flow_id": flow_id,
                    }));
                }
            }
            // Our confirmation is registered; the SDK is finishing the
            // signature exchange.
            QrVerificationState::Confirmed => {
                if !emitted_confirmed {
                    emitted_confirmed = true;
                    enqueue(events, json!({
                        "type": "verification_qr_confirmed",
                        "flow_id": flow_id,
                    }));
                }
            }
            // Only reachable for the side that SCANNED a code. Lightning
            // never scans, so this is not expected here; treat it as
            // progress rather than asserting on SDK internals.
            QrVerificationState::Reciprocated => {}
            QrVerificationState::Started => {
                // The peer chose emoji instead of scanning. matrix-sdk-crypto
                // allows exactly this while the QR is still in `Started`
                // ("it is legit to transition from QR display to SAS",
                // verification/requests.rs `receive_start`), and the SAS
                // driver adopts the Sas the SDK just installed.
                if request_moved_to_sas(request) {
                    enqueue(events, json!({
                        "type": "verification_qr_dismissed",
                        "flow_id": flow_id,
                        "reason": "peer_started_sas",
                    }));
                    return QrOutcome::FallBackToSas;
                }
                display_ticks += 1;
                // Bounded display. Without this the flow has no exit but the
                // SDK's 10-minute timeout whenever the peer neither scans
                // nor offers an emoji button — and Lightning would have
                // REMOVED the working SAS path it has today for every peer
                // that advertises `m.qr_code.scan.v1`. Falling through to
                // SAS keeps emoji verification the guaranteed outcome.
                //
                // FOLLOW-UP (accepted, not this round): an in-app "Use emoji
                // instead" button would let the user make this switch
                // immediately rather than waiting out the window. It needs a
                // new FFI, and it IS safe to build: the SDK's
                // `start_sas_helper` path stores through
                // `VerificationCache::replace`, which overwrites without
                // cancelling — unlike the `insert` that `insert_qr` uses.
                if display_ticks >= QR_DISPLAY_TICKS {
                    enqueue(events, json!({
                        "type": "verification_qr_dismissed",
                        "flow_id": flow_id,
                        "reason": "not_scanned",
                    }));
                    return QrOutcome::FallBackToSas;
                }
                continue;
            }
        }

        // Past `Started` the reciprocate start has already been exchanged
        // and the spec no longer permits switching to SAS, so this leg owns
        // the flow to the end. Bound it so a peer that stops answering
        // fails visibly instead of hanging.
        progress_ticks += 1;
        if progress_ticks >= QR_COMPLETION_TICKS {
            // Tell the peer before giving up. It has already SCANNED our
            // code, so abandoning the flow silently would leave it waiting
            // out matrix-sdk-crypto's full 10-minute VERIFICATION_TIMEOUT
            // on a confirmation that is never coming.
            //
            // FOLLOW-UP (pre-existing, deliberately not changed here):
            // `drive_sas_flow`'s own completion timeout still returns
            // without a cancel and has the same effect on its peer.
            cancel_flow_best_effort(None, Some(qr), Some(request)).await;
            enqueue(events, json!({
                "type": "verification_failed",
                "flow_id": flow_id,
                "message": "Timed out waiting for QR verification to complete.",
            }));
            return QrOutcome::Finished;
        }
    }
}

/// Drive a request the peer has answered (`m.key.verification.ready`
/// exchanged) to a terminal state, preferring the show-QR path and using
/// SAS as the fallback. Shared by BOTH directions.
#[allow(clippy::too_many_arguments)]
async fn drive_ready_request(
    client: &Client,
    request: &VerificationRequest,
    flow_id: &str,
    events: &Arc<Mutex<VecDeque<String>>>,
    sas_slot: &KeyedFlowSlot<SasVerification>,
    qr_slot: &KeyedFlowSlot<QrVerification>,
    nudges: &RecoveryNudgeSlot,
    shutdown: &Arc<AtomicBool>,
) {
    if let Some(qr) = maybe_generate_qr(request, flow_id, events).await {
        if let Ok(mut guard) = qr_slot.lock() {
            *guard = Some((flow_id.to_owned(), qr.clone()));
        }
        match drive_qr_flow(request, &qr, flow_id, events, nudges, shutdown).await {
            QrOutcome::Finished | QrOutcome::ShuttingDown => return,
            // Release the QR slot before SAS takes over so a cancel issued
            // during the SAS leg cannot try to cancel a retired QR.
            QrOutcome::FallBackToSas => release_keyed_slot(qr_slot, flow_id),
        }
    }
    drive_sas_flow(client, request, flow_id, events, sas_slot, nudges, shutdown).await;
}

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
    qr: Option<&QrVerification>,
    request: Option<&VerificationRequest>,
) {
    let budget = std::time::Duration::from_secs(VERIFICATION_CANCEL_TIMEOUT_SECS);
    if let Some(sas) = sas {
        if !sas.is_cancelled() && !sas.is_done() {
            let _ = tokio::time::timeout(budget, sas.cancel()).await;
        }
    }
    // A displayed QR is just as much a live flow as a SAS: the peer may be
    // holding a camera up to it. Abandoning it silently leaves that peer
    // waiting out matrix-sdk-crypto's 10-minute VERIFICATION_TIMEOUT.
    if let Some(qr) = qr {
        if !qr.is_cancelled() && !qr.is_done() {
            let _ = tokio::time::timeout(budget, qr.cancel()).await;
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

/// True once the SDK has moved this request onto a SAS verification.
///
/// This is the QR driver's hand-off signal. `generate_qr_code()` puts the
/// request into `Transitioned { verification: QrV1(..) }`; if the peer then
/// sends `m.key.verification.start` with `m.sas.v1`, matrix-sdk-crypto's
/// `receive_start` REPLACES that verification with a `Sas`
/// (verification/requests.rs, the `Some(Verification::QrV1(old))` arm:
/// "it is legit to transition from QR display to SAS" while the QR is still
/// in `Started`). That is the peer choosing emoji because it cannot scan,
/// and it is the only thing that may retire a displayed QR in favour of the
/// existing SAS driver.
fn request_moved_to_sas(request: &VerificationRequest) -> bool {
    sas_from_request(request).is_some()
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

impl FlowLiveness for QrVerification {
    fn is_finished(&self) -> bool {
        self.is_cancelled() || self.is_done()
    }
}

/// A flow slot keyed by flow id, for the method-level handles (`SasVerification`,
/// `QrVerification`) that carry no `flow_id()` accessor of their own.
type KeyedFlowSlot<T> = Arc<Mutex<Option<(String, T)>>>;

/// Clear a keyed slot if its occupant can no longer progress, and report
/// whether anything live is left. Dead occupants are dropped in passing so
/// a slot is self-healing rather than sticky.
fn keyed_slot_is_live<T: FlowLiveness>(slot: &KeyedFlowSlot<T>) -> bool {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(_, flow)| flow.is_finished()) {
            *guard = None;
        }
        return guard.is_some();
    }
    false
}

/// Clear a keyed slot, but ONLY where it still holds this flow.
fn release_keyed_slot<T>(slot: &KeyedFlowSlot<T>, flow_id: &str) {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(stored, _)| stored == flow_id) {
            *guard = None;
        }
    }
}

/// True when the single-flow slots still hold a LIVE flow.
///
/// Dead occupants (cancelled, done, or answered by another of our
/// sessions) are cleared in passing, so the slots are self-healing instead
/// of sticky: an abandoned request can never refuse every later attempt
/// for the rest of the process lifetime.
fn flow_slots_are_live<R, S, Q>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &KeyedFlowSlot<S>,
    qr_slot: &KeyedFlowSlot<Q>,
) -> bool
where
    R: FlowLiveness,
    S: FlowLiveness,
    Q: FlowLiveness,
{
    if let Ok(mut guard) = request_slot.lock() {
        if guard.as_ref().is_some_and(|request| request.is_finished()) {
            *guard = None;
        }
    }
    // Evaluate BOTH method slots before short-circuiting: each call is also
    // the sweep that clears a dead occupant, and `||` would skip the QR
    // sweep whenever a SAS flow happened to still be live.
    let sas_live = keyed_slot_is_live(sas_slot);
    let qr_live = keyed_slot_is_live(qr_slot);
    let request_live = request_slot.lock().map(|g| g.is_some()).unwrap_or(false);
    request_live || sas_live || qr_live
}

/// Release the single-flow slots, but ONLY where they still hold this flow.
///
/// The unconditional `*g = None` this replaces meant a terminating flow
/// wiped whatever occupied the slot — including a NEWER request that had
/// arrived in the meantime, whose Accept then failed with "no active
/// verification request".
fn release_flow_slots<R, S, Q>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &KeyedFlowSlot<S>,
    qr_slot: &KeyedFlowSlot<Q>,
    flow_id: &str,
) where
    R: FlowIdentity,
{
    release_keyed_slot(sas_slot, flow_id);
    release_keyed_slot(qr_slot, flow_id);
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
fn take_pending_flows<R, S, Q>(
    request_slot: &Arc<Mutex<Option<R>>>,
    sas_slot: &KeyedFlowSlot<S>,
    qr_slot: &KeyedFlowSlot<Q>,
) -> (Option<S>, Option<Q>, Option<R>) {
    let sas = sas_slot
        .lock()
        .ok()
        .and_then(|mut guard| guard.take())
        .map(|(_, sas)| sas);
    let qr = qr_slot
        .lock()
        .ok()
        .and_then(|mut guard| guard.take())
        .map(|(_, qr)| qr);
    let request = request_slot.lock().ok().and_then(|mut guard| guard.take());
    (sas, qr, request)
}

/// Releases this flow's slots when its driver ends for ANY reason —
/// normal exit, early return, cooperative shutdown, or a panic.
///
/// Releasing by hand at every exit is what previously leaked: several
/// early returns kept `active_request` occupied and
/// `mx_rust_start_own_verification` then refused outright, so one failed
/// attempt bricked verification until the app restarted. Cleanup on drop
/// makes future exits safe by construction rather than by remembering.
struct FlowSlotGuard<R: FlowIdentity, S, Q> {
    request_slot: Arc<Mutex<Option<R>>>,
    sas_slot: KeyedFlowSlot<S>,
    qr_slot: KeyedFlowSlot<Q>,
    flow_id: String,
}

impl<R: FlowIdentity, S, Q> FlowSlotGuard<R, S, Q> {
    fn new(
        request_slot: Arc<Mutex<Option<R>>>,
        sas_slot: KeyedFlowSlot<S>,
        qr_slot: KeyedFlowSlot<Q>,
        flow_id: String,
    ) -> Self {
        Self { request_slot, sas_slot, qr_slot, flow_id }
    }
}

impl<R: FlowIdentity, S, Q> Drop for FlowSlotGuard<R, S, Q> {
    fn drop(&mut self) {
        release_flow_slots(
            &self.request_slot, &self.sas_slot, &self.qr_slot, &self.flow_id,
        );
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
                        cancel_flow_best_effort(None, None, Some(request)).await;
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
            cancel_flow_best_effort(Some(&sas), None, Some(request)).await;
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
            release_flow_slots(
                &bridge.active_request, &bridge.active_sas, &bridge.active_qr,
                &flow_id,
            );
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let qr_slot = Arc::clone(&bridge.active_qr);
        let request_slot = Arc::clone(&bridge.active_request);
        let nudges = Arc::clone(&bridge.recovery_nudges);
        let shutdown = Arc::clone(&bridge.verification_shutdown);
        bridge.spawn_verification_task(async move {
            let _slots = FlowSlotGuard::new(
                Arc::clone(&request_slot),
                Arc::clone(&sas_slot),
                Arc::clone(&qr_slot),
                flow_id.clone(),
            );

            // Advertise exactly what this client can actually perform — see
            // `advertised_verification_methods`. Not `accept()`, which would
            // use the SDK's own SUPPORTED_METHODS: that set is currently
            // identical, but it is the SDK's choice rather than ours, and a
            // future release adding `m.qr_code.scan.v1` to it would silently
            // make Lightning claim a scanner it does not have.
            if let Err(err) = request
                .accept_with_methods(advertised_verification_methods())
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

            drive_ready_request(
                &client, &request, &flow_id, &events, &sas_slot, &qr_slot,
                &nudges, &shutdown,
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

/// The user confirmed that the OTHER device reported a successful scan.
///
/// This is the human check that gives showing a QR code its security value,
/// so it is never issued automatically: `drive_qr_flow` surfaces
/// `verification_qr_scanned` and then waits for exactly this call. The SDK
/// performs the trust change (`QrVerification::confirm` ->
/// `confirm_scanning`); nothing here promotes trust locally.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_confirm_qr_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let entry = match bridge.active_qr.lock() {
            Ok(g) => g.clone(),
            Err(_) => None,
        };
        let Some((stored_flow, qr)) = entry else {
            return Ok("error: no active QR verification.".to_owned());
        };
        if stored_flow != flow_id {
            return Ok("error: QR verification flow id mismatch.".to_owned());
        }
        // `confirm_scanning()` returns None outside `Scanned`
        // (verification/qrcode.rs), so an early confirm would be swallowed
        // and the flow would then sit until its timeout with the UI
        // believing it had acted. Refuse it visibly instead. The wording
        // states the flow's state, not a user mistake — this is reachable
        // from an ordinary race, not only from a misplaced click.
        if !matches!(qr.state(), QrVerificationState::Scanned) {
            return Ok("error: the code has not been scanned yet.".to_owned());
        }
        let events = Arc::clone(&bridge.events);
        // Joinable like the SAS actions: `confirm()` uploads a signature and
        // writes to the crypto store, so it must not still be running when
        // sign-out deletes that store.
        bridge.spawn_verification_task(async move {
            if let Err(err) = qr.confirm().await {
                enqueue(&events, json!({
                    "type": "verification_failed",
                    "flow_id": stored_flow,
                    "message": format_matrix_error("verification_qr_confirm", err),
                }));
            }
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_cancel_verification(
    ptr: *mut c_void,
    flow_id: *const c_char,
) -> *mut c_char {
    // Cancel at EVERY level. Each SDK cancel is idempotent (it returns no
    // outgoing request once the flow is already cancelled or done), so
    // running them all is safe — and necessary. These used to be `if` /
    // `else if`, which meant an `active_sas` belonging to some OTHER flow
    // suppressed the request-level cancel entirely: nothing reached the
    // wire, the peer waited out matrix-sdk-crypto's 10-minute
    // VERIFICATION_TIMEOUT, and both slots were cleared regardless. The QR
    // level joins on the same terms: closing the dialog while a code is on
    // screen must tell the peer, not just hide the picture.
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let flow_id = unsafe { cstr_arg(flow_id) }?;
        let sas_entry = bridge.active_sas.lock().ok().and_then(|g| g.clone());
        let qr_entry = bridge.active_qr.lock().ok().and_then(|g| g.clone());
        let request = bridge.active_request.lock().ok().and_then(|g| g.clone());
        let events = Arc::clone(&bridge.events);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let qr_slot = Arc::clone(&bridge.active_qr);
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
            if let Some((stored_flow, qr)) = qr_entry {
                if stored_flow == flow_id {
                    if !qr.is_cancelled() && !qr.is_done() {
                        let _ = qr.cancel().await;
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
            release_flow_slots(&request_slot, &sas_slot, &qr_slot, &flow_id);
        });
        Ok(String::new())
    })
}

/// Lightning-initiated (outbound) SAS verification of the current
/// session against another session belonging to the same Matrix
/// account. Advertises the methods Lightning can genuinely perform
/// (see `advertised_verification_methods`) — never `m.qr_code.scan.v1`,
/// which it has no scanner for.
///
/// Emits `verification_request_started` as soon as the SDK has sent the
/// request, then `verification_ready` once the peer answers, and hands
/// off to the shared `drive_ready_request` driver — the same one the
/// receive-first path uses, so both directions perform the identical
/// show-QR-then-SAS sequence.
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
        if flow_slots_are_live(
            &bridge.active_request, &bridge.active_sas, &bridge.active_qr,
        ) {
            return Ok("error: A verification is already in progress.".to_owned());
        }

        let events = Arc::clone(&bridge.events);
        let request_slot = Arc::clone(&bridge.active_request);
        let sas_slot = Arc::clone(&bridge.active_sas);
        let qr_slot = Arc::clone(&bridge.active_qr);
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
                .request_verification_with_methods(advertised_verification_methods())
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
                Arc::clone(&qr_slot),
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
                    cancel_flow_best_effort(None, None, Some(&request)).await;
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

            drive_ready_request(
                &client, &request, &flow_id, &events, &sas_slot, &qr_slot,
                &nudges, &shutdown,
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
            // TRI-STATE, NOT unwrap_or(false). `exists_on_server()` performs a
            // real GET /room_keys/version, so a network blip, a 5xx or an
            // unauthenticated moment all return Err — and publishing that as
            // `false` told the user, flatly, that their account has no key
            // backup. That is the most dangerous wrong answer this surface
            // can give: it invites them to treat an existing recovery key as
            // nonexistent, and it arms a button whose action can mint a NEW
            // 4S key over the one they already have.
            //
            // `probe_backup_exists` in this same file already gets this right
            // and says why ("transient network failure — stay unknown"); the
            // health snapshot simply never did. Null means unknown.
            let backup_exists: Option<bool> =
                backups.exists_on_server().await.ok();

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
        bridge.timelines.open_room(&bridge.runtime, client, room_id.clone());
        // The open room is the ONE sliding-sync room subscription, which is
        // the only way subscription-only required state (m.room.pinned_events)
        // reaches the store. An unparseable id cannot be subscribed and the
        // timeline open above stands on its own.
        if let Ok(parsed) = OwnedRoomId::try_from(room_id.as_str()) {
            if let Ok(mut guard) = bridge.active_room_subscription.lock() {
                *guard = Some(parsed);
            }
            apply_room_subscription(bridge);
        }
        Ok(String::new())
    })
}

/// 2026-08-19: re-open the ACTIVE room's live timeline after letting the SDK's
/// event cache release everything the reader paginated in — Lightning's
/// equivalent of Element's `jumpToLiveTimeline()` rebuilding at the live edge
/// rather than scrolling a huge backlog. One caller only: an explicit
/// user-initiated jump to the newest message from far back. Emits the ordinary
/// `timeline_reset` (a new room generation, so stale diffs are rejected) with
/// a `trimmed_from` count so the trim can be verified rather than assumed.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_reload_at_live(
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
        bridge
            .timelines
            .reload_room_at_live(&bridge.runtime, client, room_id.clone());
        // The room stays THE sliding-sync subscription across a reload — the
        // subscription is per-room, not per-timeline-generation, so it is
        // deliberately left exactly as the open established it.
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close();
        // No open room, no room subscription.
        if let Ok(mut guard) = bridge.active_room_subscription.lock() {
            guard.take();
        }
        apply_room_subscription(bridge);
        Ok(String::new())
    })
}

/// (Re)apply the single active-room subscription to the running sliding
/// sync, when there is one. Fire-and-forget by design: the subscription is
/// an optimisation of WHAT sync delivers, never a gate on opening the room,
/// and `subscribe_to_rooms` replaces the previous subscription set wholesale
/// so repeated calls converge on exactly the desired state.
fn apply_room_subscription(bridge: &RustClient) {
    let Some(service) = bridge
        .room_list_service
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
    else {
        return; // No modern sync running; the sync loop applies it on start.
    };
    let desired = bridge
        .active_room_subscription
        .lock()
        .ok()
        .and_then(|guard| guard.clone());
    // Managed (review L3): this future holds an Arc<RoomListService> and
    // through it a strong Client; an untracked task could keep the crypto
    // store open across sign-out teardown.
    bridge.spawn_room_action(async move {
        match desired {
            Some(room_id) => service.subscribe_to_rooms(&[&room_id]).await,
            None => service.subscribe_to_rooms(&[]).await,
        }
    });
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
    body_spec: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        bridge
            .timelines
            .send_text(&bridge.runtime, room_id, body, mentions, spec)
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
    body_spec: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let reply_to = unsafe { cstr_arg(in_reply_to_event_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        bridge
            .timelines
            .send_reply(&bridge.runtime, room_id, reply_to, body, mentions, spec)
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
    body_spec: *const c_char,
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
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge
            .timelines
            .send_thread_text(
                &bridge.runtime, client, room_id, root, body, reply_to, mentions,
                spec,
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
/// v0.9 device management (phase 9): rename ONE of the account's devices
/// through the standard PUT /devices/{id}. No UIA is involved (the endpoint
/// needs none). Answers on `device_renamed {op_id, ok, category}`; the C++
/// side refetches the list on success.
pub unsafe extern "C" fn mx_rust_rename_device(
    ptr: *mut c_void,
    device_id: *const c_char,
    display_name: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let device_id = unsafe { cstr_arg(device_id) }?;
        let display_name = unsafe { cstr_arg(display_name) }?;
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        let timelines = Arc::clone(&bridge.timelines);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "rename_device", async move {
                use matrix_sdk::ruma::api::client::device::update_device;
                use matrix_sdk::ruma::OwnedDeviceId;
                let id: OwnedDeviceId = device_id.clone().into();
                let mut request = update_device::v3::Request::new(id);
                request.display_name = Some(display_name);
                let result = client.send(request).await;
                if !timelines.lifecycle_current(lifecycle) {
                    return;
                }
                enqueue(
                    &events,
                    match result {
                        Ok(_) => json!({
                            "type": "device_renamed", "op_id": op_id,
                            "lifecycle": lifecycle, "ok": true,
                        }),
                        Err(err) => json!({
                            "type": "device_renamed", "op_id": op_id,
                            "lifecycle": lifecycle, "ok": false,
                            "category": rooms::classify_room_error(&err.to_string()),
                        }),
                    },
                );
            });
        });
        Ok(String::new())
    })
}

/// v0.9 key-backup management (phase 9). Every action is the SDK's own
/// recovery/backup flow — nothing here touches the crypto store directly:
///   * "enable": Recovery::enable() — creates secret storage AND the key
///     backup, uploads existing room keys, and returns the NEW RECOVERY KEY;
///   * "create_backup": Recovery::enable_backup() — creates/enables the
///     backup on an account that already has secret storage;
///   * "reset_key": Recovery::reset_key() — a NEW recovery key replaces the
///     old one (the old key stops working); returns the new key;
///   * "disable_and_delete": Backups::disable_and_delete() — deletes the
///     server-side backup version (room keys in it are gone; the local
///     store is untouched);
///   * "disable_recovery": Recovery::disable() — removes secret storage AND
///     the backup.
/// The recovery key crosses the FFI ONCE, in `backup_action_result`, for
/// display; it is never logged and never persisted by this side. Progress
/// of the room-key upload after enable/create rides `backup_progress`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_backup_action(
    ptr: *mut c_void,
    action: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let action = unsafe { cstr_arg(action) }?;
        match action.as_str() {
            "enable" | "create_backup" | "reset_key" | "disable_and_delete"
            | "disable_recovery" => {}
            _ => return Err("unknown backup action".to_owned()),
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        let timelines = Arc::clone(&bridge.timelines);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "backup_action", async move {
                let encryption = client.encryption();
                let recovery = encryption.recovery();
                let backups = encryption.backups();
                // Ok(Some(key)) when a fresh recovery key was minted.
                let result: Result<Option<String>, String> = match action.as_str() {
                    "enable" => recovery
                        .enable()
                        .wait_for_backups_to_upload()
                        .await
                        .map(Some)
                        .map_err(|e| e.to_string()),
                    "create_backup" => recovery
                        .enable_backup()
                        .await
                        .map(|_| None)
                        .map_err(|e| e.to_string()),
                    "reset_key" => recovery
                        .reset_key()
                        .await
                        .map(Some)
                        .map_err(|e| e.to_string()),
                    "disable_and_delete" => backups
                        .disable_and_delete()
                        .await
                        .map(|_| None)
                        .map_err(|e| e.to_string()),
                    "disable_recovery" => recovery
                        .disable()
                        .await
                        .map(|_| None)
                        .map_err(|e| e.to_string()),
                    _ => Err("unknown backup action".to_owned()),
                };
                if !timelines.lifecycle_current(lifecycle) {
                    return;
                }
                match result {
                    Ok(key) => enqueue(
                        &events,
                        json!({
                            "type": "backup_action_result", "op_id": op_id,
                            "lifecycle": lifecycle, "action": action, "ok": true,
                            "recovery_key": key.unwrap_or_default(),
                        }),
                    ),
                    Err(message) => enqueue(
                        &events,
                        json!({
                            "type": "backup_action_result", "op_id": op_id,
                            "lifecycle": lifecycle, "action": action, "ok": false,
                            // Category only — an SDK/server message could carry
                            // account detail.
                            "category": rooms::classify_room_error(&message),
                        }),
                    ),
                }
            });
        });
        Ok(String::new())
    })
}

/// v0.9: one sanitized snapshot of the room-key upload state, for the
/// backup card's progress line. Counts only.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_backup_progress(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        let timelines = Arc::clone(&bridge.timelines);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "backup_progress", async move {
                use matrix_sdk::encryption::backups::UploadState;
                let backups = client.encryption().backups();
                let state = format!("{:?}", backups.state()).to_lowercase();
                let steady = backups.wait_for_steady_state();
                let mut progress = steady.subscribe_to_progress();
                // One bounded observation: the current upload state, if any
                // is reported promptly; otherwise the backup state alone.
                let (backed_up, total, upload) = match tokio::time::timeout(
                    std::time::Duration::from_millis(500),
                    futures_util::StreamExt::next(&mut progress),
                )
                .await
                {
                    Ok(Some(Ok(UploadState::Uploading(counts)))) => {
                        (counts.backed_up as u64, counts.total as u64, "uploading")
                    }
                    Ok(Some(Ok(UploadState::Done))) => (0, 0, "done"),
                    Ok(Some(Ok(UploadState::Error))) => (0, 0, "error"),
                    Ok(Some(Ok(UploadState::Idle))) => (0, 0, "idle"),
                    _ => (0, 0, "unknown"),
                };
                if !timelines.lifecycle_current(lifecycle) {
                    return;
                }
                enqueue(
                    &events,
                    json!({
                        "type": "backup_progress", "lifecycle": lifecycle,
                        "backup_state": state, "upload_state": upload,
                        "backed_up": backed_up, "total": total,
                    }),
                );
            });
        });
        Ok(String::new())
    })
}

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
            .mark_thread_read(&bridge.runtime, room_id, root,
                              bridge.receipt_privacy.load(Ordering::SeqCst))
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
    body_spec: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let new_body = unsafe { cstr_arg(new_body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        bridge
            .timelines
            .edit(&bridge.runtime, room_id, target, new_body, mentions, spec)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_toggle_reaction(
    ptr: *mut c_void,
    room_id: *const c_char,
    thread_root_id: *const c_char,
    target_event_id: *const c_char,
    key: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root_id = unsafe { cstr_arg(thread_root_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let key = unsafe { cstr_arg(key) }?;
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        bridge
            .timelines
            .toggle_reaction(&bridge.runtime, client, room_id, thread_root_id,
                             target, key)
            .map(|_| String::new())
    })
}

/// Parse an optional newline-separated FFI list argument. NULL or an empty
/// string yields an empty list; entries are trimmed and blanks dropped.
/// Optional string FFI argument: NULL is the empty string, not an error.
/// For arguments whose absence is a meaningful default (the v0.9 body spec).
unsafe fn cstr_opt_arg(value: *const c_char) -> Result<String, String> {
    if value.is_null() {
        return Ok(String::new());
    }
    unsafe { cstr_arg(value) }
}

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
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        bridge
            .timelines
            .redact(&bridge.runtime, client, room_id, target, reason)
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

/// Cancel a local echo that has not reached the server yet.
///
/// Routes to `SendHandle::abort`, which also aborts an in-flight media
/// upload. A successful abort produces no event of its own — the SDK's
/// CancelledLocalEvent removes the row through the normal diff path.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_cancel_send(
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
            .cancel_send(&bridge.runtime, room_id, txn)
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

/// v0.7.4: set — or CLEAR — the signed-in account's own display name.
///
/// An EMPTY `name` means clear, which reaches the SDK as `None`; the SDK
/// then picks the MSC4133 delete-profile-field endpoint or the deprecated
/// v3 request by itself. `Some("")` is a different request (store an empty
/// name) and is deliberately unreachable from here.
///
/// Result event: own_display_name_result { op_id, lifecycle, ok, error }.
/// The name never comes back — C++ already holds what it submitted.
/// Rooms this account and `user_id` are BOTH joined to.
///
/// Reads only the store's cached membership — it issues no request, so a
/// room whose members were never synced is not listed. That under-reporting
/// is deliberate; see profile::mutual_rooms.
///
/// Result event: mutual_rooms_result { op_id, lifecycle, user_id, rooms[] }.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_mutual_rooms(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user = unsafe { cstr_arg(user_id) }?;
        profile::mutual_rooms(bridge, user, op_id).map(|_| String::new())
    })
}

/// Upload and set the account's OWN avatar from a local file path.
///
/// Result event: own_avatar_result { op_id, lifecycle, ok, error }.
/// The path never comes back — C++ already holds it, and a home directory
/// carries the user's name.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_own_avatar(
    ptr: *mut c_void,
    local_path: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let path = unsafe { cstr_arg(local_path) }?;
        profile::set_own_avatar(bridge, path, op_id).map(|_| String::new())
    })
}

/// Clear the account's own avatar. Same result event as the setter.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_clear_own_avatar(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        profile::clear_own_avatar(bridge, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_display_name(
    ptr: *mut c_void,
    name: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let name = unsafe { cstr_arg(name) }?;
        profile::set_own_display_name(bridge, name, op_id).map(|_| String::new())
    })
}

/// This account's display name IN ONE ROOM. Empty clears the override, so the
/// global profile shows again. Answers on `room_profile_result`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_member_display_name(
    ptr: *mut c_void,
    room_id: *const c_char,
    name: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let name = unsafe { cstr_arg(name) }?;
        profile::set_room_display_name(bridge, room_id, name, op_id)
            .map(|_| String::new())
    })
}

/// This account's avatar IN ONE ROOM. Empty clears the override.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_member_avatar(
    ptr: *mut c_void,
    room_id: *const c_char,
    mxc: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let mxc = unsafe { cstr_arg(mxc) }?;
        profile::set_room_avatar(bridge, room_id, mxc, op_id).map(|_| String::new())
    })
}

/// Profile banners (MSC4427 over MSC4133 extended profile fields).
///
/// Reads `m.banner_url`, falling back to `chat.commet.profile_banner` — the
/// key Commet already ships and Sable and Haven read — so a banner set in any
/// of them shows up here. Result event: `profile_banner { op_id, lifecycle,
/// user_id, mxc, supported }`, where `supported: false` means the homeserver
/// does not implement extended profile fields at all. That renders as
/// NOTHING, never as "this user has no banner".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_profile_banner(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        banner::fetch_profile_banner(bridge, op_id, user_id).map(|_| String::new())
    })
}

/// Upload a local image and set it as this account's banner, under BOTH the
/// stable and the Commet field names. An EMPTY path clears both. Result
/// event: `profile_banner_set { op_id, lifecycle, ok, mxc, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_profile_banner(
    ptr: *mut c_void,
    local_path: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let path = unsafe { cstr_arg(local_path) }?;
        banner::set_own_profile_banner(bridge, op_id, path).map(|_| String::new())
    })
}

/// A display-name colour the user chose, carried in their Matrix profile
/// (`org.lightning.name_color`, MSC4133) so other Lightning clients see it.
///
/// Result event: `name_color { op_id, lifecycle, user_id, color, supported }`.
/// `color` is `#rrggbb` or empty; `supported: false` means the homeserver has
/// no extended profile fields, which renders as nothing rather than as "this
/// user chose no colour". The value is validated as a colour before it leaves
/// Rust — it is written by somebody else's client and ends up on a QML colour
/// property.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_name_color(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        namecolor::fetch_name_color(bridge, op_id, user_id).map(|_| String::new())
    })
}

/// Set this account's display-name colour, or clear it with an EMPTY value.
/// Result event: `name_color_set { op_id, lifecycle, ok, color, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_name_color(
    ptr: *mut c_void,
    value: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let value = unsafe { cstr_arg(value) }?;
        namecolor::set_name_color(bridge, op_id, value).map(|_| String::new())
    })
}

/// Profile biographies (MSC4440 over MSC4133 extended profile fields).
///
/// Reads `m.biography`, falling back to `gay.fomx.biography` — MSC4440's own
/// unstable prefix, and the key Sable writes today. Result event:
/// `profile_bio { op_id, lifecycle, user_id, bio, supported }`, where
/// `supported: false` means the homeserver does not implement extended profile
/// fields at all. That renders as NOTHING, never as "this user has no bio".
///
/// Only PLAIN TEXT crosses: a bio is remote free text, and rendering the
/// MSC's optional HTML representation would fetch remote media of the profile
/// owner's choosing for every viewer. See the module header in `bio.rs`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_profile_bio(
    ptr: *mut c_void,
    user_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        bio::fetch_profile_bio(bridge, op_id, user_id).map(|_| String::new())
    })
}

/// Set this account's bio, under BOTH the stable and the MSC4440 unstable
/// field names. EMPTY (or whitespace-only) text clears both. Result event:
/// `profile_bio_set { op_id, lifecycle, ok, bio, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_profile_bio(
    ptr: *mut c_void,
    text: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let text = unsafe { cstr_arg(text) }?;
        bio::set_own_profile_bio(bridge, op_id, text).map(|_| String::new())
    })
}

/// Read a room's (or Space's) banner and whether this account may change it.
/// Result event: `room_banner { op_id, lifecycle, room_id, mxc, can_set }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_room_banner(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        banner::fetch_room_banner(bridge, op_id, room_id).map(|_| String::new())
    })
}

/// Upload a local image and set it as the room's banner. An EMPTY path clears
/// it. Result event:
/// `room_banner_set { op_id, lifecycle, room_id, ok, mxc, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_banner(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        banner::set_room_banner(bridge, op_id, room_id, path).map(|_| String::new())
    })
}

// ---------------------------------------------------------------------------
// Stickers and image packs (MSC2545)
// ---------------------------------------------------------------------------

/// Read every image pack available to this account and answer with one
/// `sticker_packs { op_id, lifecycle, room_id, packs[] }` snapshot.
///
/// `room_id` may be empty. When it is set, that room's OWN
/// `im.ponies.room_emotes` packs are included — MSC2545 makes a room's packs
/// available inside that room without any opt-in, and `im.ponies.emote_rooms`
/// is what makes them available elsewhere.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_fetch_packs(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        stickers::fetch_packs(bridge, op_id, room_id).map(|_| String::new())
    })
}

/// Send one `m.sticker`. An empty `thread_root_id` targets the room timeline;
/// otherwise the SDK attaches the `m.thread` relation itself.
///
/// `url` must be a plain `mxc://` — the media a pack holds. Refused otherwise,
/// so a caller can never put an http URL on the wire.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_stickers_send(
    ptr: *mut c_void,
    room_id: *const c_char,
    thread_root_id: *const c_char,
    url: *const c_char,
    body: *const c_char,
    mimetype: *const c_char,
    width: u64,
    height: u64,
    size: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root_id = unsafe { cstr_arg(thread_root_id) }?;
        let url = unsafe { cstr_arg(url) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mimetype = unsafe { cstr_arg(mimetype) }?;
        stickers::send_sticker(
            bridge, room_id, thread_root_id, url, body, mimetype, width, height,
            size,
        )
        .map(|_| String::new())
    })
}

/// Add one image to a ROOM's `im.ponies.room_emotes` pack. Answers on the
/// SAME `sticker_pack_add_result` event as the user-pack path, so a caller
/// has one place to report from.
///
/// ROOM STATE, so it is POWER-LEVEL GATED: the room's own required level for
/// `im.ponies.room_emotes`, asked of the SDK. `category` is "forbidden" when
/// this account may not write it, "duplicate" when that exact mxc is already
/// in the pack, "pack_full" at the size cap, or a coarse room-error class.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_stickers_add_to_room_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    shortcode: *const c_char,
    url: *const c_char,
    body: *const c_char,
    mimetype: *const c_char,
    width: u64,
    height: u64,
    size: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let url = unsafe { cstr_arg(url) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mimetype = unsafe { cstr_arg(mimetype) }?;
        stickers::add_to_room_pack(
            bridge, op_id, room_id, state_key, shortcode, url, body, mimetype,
            width, height, size,
        )
        .map(|_| String::new())
    })
}

/// Turn one ROOM pack on or off in `im.ponies.emote_rooms` — "use this room's
/// stickers everywhere". Answers with
/// `sticker_pack_rooms_set { op_id, lifecycle, ok, category, room_id,
/// state_key, enabled }`.
///
/// ACCOUNT DATA, so no power level is involved: it records the reader's own
/// choice. A room's packs are always usable INSIDE that room whatever this
/// says.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_set_room_pack_enabled(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    enabled: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        stickers::set_room_pack_enabled(bridge, op_id, room_id, state_key, enabled)
            .map(|_| String::new())
    })
}

/// Upload a LOCAL image and add it to this account's own sticker pack.
///
/// The only way to create a pack from nothing: every other route needs an
/// mxc that already exists. Same result event as the save path below,
/// because it is the same pack write underneath.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_upload_to_user_pack(
    ptr: *mut c_void,
    shortcode: *const c_char,
    body: *const c_char,
    local_path: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let body = unsafe { cstr_arg(body) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        stickers::upload_to_user_pack(bridge, op_id, shortcode, body, path)
            .map(|_| String::new())
    })
}

/// "Steal" a sticker into `im.ponies.user_emotes`. Answers with
/// `sticker_pack_add_result { op_id, lifecycle, ok, category, shortcode }`.
///
/// `category` is `duplicate` when that exact mxc is already in the pack,
/// `pack_full` at the size cap, or a coarse room-error class.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_stickers_add_to_user_pack(
    ptr: *mut c_void,
    shortcode: *const c_char,
    url: *const c_char,
    body: *const c_char,
    mimetype: *const c_char,
    width: u64,
    height: u64,
    size: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let url = unsafe { cstr_arg(url) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mimetype = unsafe { cstr_arg(mimetype) }?;
        stickers::add_to_user_pack(
            bridge, op_id, shortcode, url, body, mimetype, width, height, size,
        )
        .map(|_| String::new())
    })
}

// ── Policy lists (Mjolnir-style moderation) ────────────────────────────
//
// Read a policy room's rules, publish or remove one, subscribe to a list,
// and ask whether the subscribed lists cover an entity. Nothing here ACTS on
// a match — see rust/src/policy.rs for why that is the user's call.

#[no_mangle]
pub unsafe extern "C" fn mx_rust_policy_fetch_rules(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        policy::fetch_rules(bridge, op_id, room_id).map(|_| String::new())
    })
}

/// `recommendation` empty REMOVES the rule (an empty state event, the
/// Mjolnir convention and the only removal Matrix state has).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_policy_write_rule(
    ptr: *mut c_void,
    room_id: *const c_char,
    kind: *const c_char,
    entity: *const c_char,
    state_key: *const c_char,
    recommendation: *const c_char,
    reason: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let kind = unsafe { cstr_arg(kind) }?;
        let entity = unsafe { cstr_arg(entity) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let recommendation = unsafe { cstr_arg(recommendation) }?;
        let reason = unsafe { cstr_arg(reason) }?;
        policy::write_rule(bridge, op_id, room_id, kind, entity, state_key,
                           recommendation, reason)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_policy_subscribe(
    ptr: *mut c_void,
    room_id: *const c_char,
    subscribed: c_int,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        policy::subscribe(bridge, op_id, room_id, subscribed != 0).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_policy_subscriptions(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        policy::fetch_subscriptions(bridge, op_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_policy_check(
    ptr: *mut c_void,
    kind: *const c_char,
    entity: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let kind = unsafe { cstr_arg(kind) }?;
        let entity = unsafe { cstr_arg(entity) }?;
        policy::check_entity(bridge, op_id, kind, entity).map(|_| String::new())
    })
}

// ── MSC4108: signing another device in from this one ───────────────────
//
// All four answer immediately with the flow's GENERATION (or an error) and
// then report through `qr_login_progress` poll events. See rust/src/qrlogin.rs
// for why the progress stream is not optional and why only this direction is
// implemented.

/// Show a QR code here for a new device to scan. Returns the generation.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_generate(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        qrlogin::grant_generate(bridge).map(|gen| gen.to_string())
    })
}

/// Consume the QR a new device is showing, as its base64 text. Lightning
/// bundles no camera decoder; the UI says so rather than implying one.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_scan(
    ptr: *mut c_void,
    payload: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let payload = unsafe { cstr_arg(payload) }?;
        qrlogin::grant_scan(bridge, payload).map(|gen| gen.to_string())
    })
}

/// Answer the generate-side flow with the two digits the new device showed.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_check_code(
    ptr: *mut c_void,
    generation: u64,
    code: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if !(0..=99).contains(&code) {
            return Err("a check code is two digits".to_owned());
        }
        qrlogin::submit_check_code(bridge, generation, code as u8)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_cancel(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        qrlogin::cancel(bridge);
        Ok(String::new())
    })
}

/// MSC2545 pack MANAGEMENT: remove an image, rename its shortcode, rename
/// the pack, or empty it.
///
/// `room_id` empty selects this account's OWN pack (`im.ponies.user_emotes`,
/// account data); otherwise the room pack under `state_key`, which is
/// power-level gated exactly as adding to one is.
///
/// `action` is one of "remove_image", "rename_image", "set_name",
/// "delete_pack". `arg_a` / `arg_b` carry that action's operands — the
/// shortcode; the old and new shortcodes; the new name; nothing. One entry
/// point rather than four, because the four differ only in their operands
/// and every one of them is the same read-modify-write against the same two
/// stores.
///
/// Answers asynchronously with `sticker_pack_edit_result`. Nothing is applied
/// optimistically: the caller re-reads the authoritative pack, so a refusal
/// cannot leave a picker showing something the server does not have.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_edit_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    action: *const c_char,
    arg_a: *const c_char,
    arg_b: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let action = unsafe { cstr_arg(action) }?;
        let arg_a = unsafe { cstr_arg(arg_a) }?;
        let arg_b = unsafe { cstr_arg(arg_b) }?;
        // Validated HERE and refused with a message, rather than defaulted to
        // one of the four: a typo that silently deleted a pack because the
        // fallback happened to be DeletePack is exactly the accident this
        // shape makes possible if it is careless.
        let edit = match action.as_str() {
            "remove_image" => {
                if arg_a.is_empty() {
                    return Err("no shortcode given".to_owned());
                }
                stickers::PackEdit::RemoveImage { shortcode: arg_a }
            }
            "rename_image" => {
                if arg_a.is_empty() || arg_b.is_empty() {
                    return Err("rename needs both shortcodes".to_owned());
                }
                stickers::PackEdit::RenameImage { from: arg_a, to: arg_b }
            }
            "set_name" => stickers::PackEdit::SetName { name: arg_a },
            "delete_pack" => stickers::PackEdit::DeletePack,
            _ => return Err("unknown pack edit".to_owned()),
        };
        stickers::edit_pack(bridge, op_id, room_id, state_key, edit)
            .map(|_| String::new())
    })
}

/// v0.7.x Matrix presence: one bounded polling round over a JSON array of
/// user ids (capped in presence.rs). Answers as a single `presence_batch`
/// poll event carrying per-user ok/state/currently_active/last_active_ago.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_get_presence(
    ptr: *mut c_void,
    user_ids_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let payload = unsafe { cstr_arg(user_ids_json) }?;
        presence::fetch_presence(bridge, payload, op_id).map(|_| String::new())
    })
}

/// v0.7.x Matrix presence: publish the local user's own state
/// (0 online, 1 unavailable, 2 offline). Fire-and-forget; a failure
/// surfaces only as a `presence_publish_failed` event.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_presence(
    ptr: *mut c_void,
    state: c_uint,
    status_msg: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let status = unsafe { cstr_opt_arg(status_msg) }?;
        let status = if status.trim().is_empty() { None } else { Some(status) };
        presence::publish_presence(bridge, state, status).map(|_| String::new())
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

/// Moderation: kick (`op` = 0), ban (`op` = 1) or unban (`op` = 2) one
/// user via the SDK's Room::kick_user / ban_user / unban_user. `reason`
/// may be empty. Result event: room_moderation_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_moderate_user(
    ptr: *mut c_void,
    room_id: *const c_char,
    user_id: *const c_char,
    reason: *const c_char,
    op: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        let reason = unsafe { cstr_arg(reason) }?;
        rooms::moderate_member(bridge, room_id, user_id, reason, op, op_id)
            .map(|_| String::new())
    })
}

/// v0.7.x room administration: set ONE member's power level through the
/// SDK's `Room::update_power_levels`, which preserves every other user's
/// level (including arbitrary custom numbers). Result event:
/// room_power_level_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_member_power_level(
    ptr: *mut c_void,
    room_id: *const c_char,
    user_id: *const c_char,
    level: i64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        rooms::set_member_power_level(bridge, room_id, user_id, level, op_id)
            .map(|_| String::new())
    })
}

/// 2026-08-26 Space settings: set ONE threshold in the room's
/// `m.room.power_levels`. `key` must be one of the fixed allowlist in
/// `rooms::set_room_power_level_key` — anything else is refused here, so this
/// never becomes a generic arbitrary-event-type power writer. Result event:
/// room_power_matrix_result { op_id, room_id, key, level, ok, category }.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_power_level_key(
    ptr: *mut c_void,
    room_id: *const c_char,
    key: *const c_char,
    level: i64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let key = unsafe { cstr_arg(key) }?;
        rooms::set_room_power_level_key(bridge, room_id, key, level, op_id)
            .map(|_| String::new())
    })
}

/// v0.7.x room administration: set the room's join rule ("invite",
/// "public" or "knock"). Result event: room_edit_result, field "join_rule".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_join_rule(
    ptr: *mut c_void,
    room_id: *const c_char,
    rule: *const c_char,
    allowed_room_ids: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let rule = unsafe { cstr_arg(rule) }?;
        let allowed = unsafe { cstr_list_arg(allowed_room_ids) }?;
        rooms::set_room_join_rule(bridge, room_id, rule, allowed, op_id)
            .map(|_| String::new())
    })
}

/// v0.9 room access (phase 4). Each answers on room_edit_result with the
/// named field; the directory visibility READ answers on
/// room_directory_visibility {room_id, visibility, ok}.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_history_visibility(
    ptr: *mut c_void,
    room_id: *const c_char,
    visibility: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let visibility = unsafe { cstr_arg(visibility) }?;
        rooms::set_room_history_visibility(bridge, room_id, visibility, op_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_guest_access(
    ptr: *mut c_void,
    room_id: *const c_char,
    access: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let access = unsafe { cstr_arg(access) }?;
        rooms::set_room_guest_access(bridge, room_id, access, op_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_room_directory_visibility(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::request_room_directory_visibility(bridge, room_id).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_directory_visibility(
    ptr: *mut c_void,
    room_id: *const c_char,
    published: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::set_room_directory_visibility(bridge, room_id, published, op_id)
            .map(|_| String::new())
    })
}

/// v0.9 room upgrade (phase 8). Version list answers on `room_versions`;
/// the upgrade answers on `room_upgrade_result` with the replacement id.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_room_versions(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        rooms::request_room_versions(bridge).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_upgrade_room(
    ptr: *mut c_void,
    room_id: *const c_char,
    new_version: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let new_version = unsafe { cstr_arg(new_version) }?;
        rooms::upgrade_room(bridge, room_id, new_version, op_id).map(|_| String::new())
    })
}

/// v0.9 scheduled send (phase 11). See rooms.rs for the protocol limits.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_probe_delayed_events(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        rooms::probe_delayed_events(bridge).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_schedule_message(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    body_spec: *const c_char,
    mention_user_ids: *const c_char,
    delay_ms: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let spec = unsafe { cstr_opt_arg(body_spec) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        rooms::schedule_message(bridge, room_id, body, spec, mentions, delay_ms, op_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_update_scheduled_message(
    ptr: *mut c_void,
    delay_id: *const c_char,
    action: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let delay_id = unsafe { cstr_arg(delay_id) }?;
        let action = unsafe { cstr_arg(action) }?;
        rooms::update_scheduled(bridge, delay_id, action, op_id).map(|_| String::new())
    })
}

/// v0.9 scheduled send: a ROOM-level send (`Room::send`) for any joined room,
/// carrying the same body spec / mentions as the timeline sends plus an
/// optional reply or thread relation. The timeline sends refuse a room whose
/// live timeline is not open, which a scheduled message for another room
/// always is. Answers on `room_send_result {op_id, room_id, ok, category}`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_send_room_message(
    ptr: *mut c_void,
    room_id: *const c_char,
    body: *const c_char,
    mention_user_ids: *const c_char,
    body_spec: *const c_char,
    reply_to_event_id: *const c_char,
    thread_root_event_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        let reply_to = unsafe { cstr_opt_arg(reply_to_event_id) }?;
        let thread_root = unsafe { cstr_opt_arg(thread_root_event_id) }?;
        rooms::send_room_message(
            bridge,
            room_id,
            body,
            spec,
            mentions,
            (!reply_to.is_empty()).then_some(reply_to),
            (!thread_root.is_empty()).then_some(thread_root),
            op_id,
        )
        .map(|_| String::new())
    })
}

/// v0.9 (phase 2): the Activity Center's seed for a fresh session. See
/// rooms::request_activity_seed.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_activity_seed(
    ptr: *mut c_void,
    limit: u32,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        rooms::request_activity_seed(bridge, limit).map(|_| String::new())
    })
}

/// v0.9 message edit history + event source (phase 7). Answers on
/// `message_edit_history` / `event_source` poll events; see rooms.rs.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_edit_history(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        rooms::request_edit_history(bridge, room_id, event_id).map(|_| String::new())
    })
}

/// MSC3030 "jump to date": the event closest to `timestamp_ms`, searching
/// FORWARD, so a chosen day lands on its first message. rooms::
/// event_at_timestamp.
/// A room's widgets, resolved and validated. See rust/src/widgets.rs for why
/// Lightning lists and opens rather than embeds. Answers on
/// `room_widgets {op_id, room_id, ok, widgets:[...]}`.
/// One page of a room's media history, walked backwards INDEPENDENTLY of the
/// live timeline.
///
/// `restart` non-zero begins again at the live edge; otherwise the walk
/// continues from where this room left off, so reopening the panel does not
/// re-fetch what it already has.
///
/// The page reports what it SCANNED as well as what it matched, and whether
/// it reached the start of accessible history — see mediahistory.rs for why
/// the walk is unfiltered and why honest completeness matters more here than
/// a smaller number of requests.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_history_page(
    ptr: *mut c_void,
    room_id: *const c_char,
    limit: c_uint,
    restart: c_int,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = require_client_for_search(bridge)?;
        let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
        let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let cursors = Arc::clone(&bridge.media_history);
        let encrypted_room = room.encryption_state().is_encrypted();
        // Clamped: the panel asks for a screenful, and an unbounded limit is
        // one request that can stall the walk for a very long time.
        let want: u64 = match limit {
            0 => 50,
            n => std::cmp::min(u64::from(n), 200),
        };
        if restart != 0 {
            if let Ok(mut map) = cursors.lock() {
                map.remove(&room_id);
            }
        }
        bridge.spawn_room_action(async move {
            let cursor = cursors
                .lock()
                .ok()
                .and_then(|m| m.get(&room_id).cloned())
                .unwrap_or_default();
            if cursor.exhausted {
                // Nothing older exists. Answer rather than re-asking the
                // server for a page it already said was the end.
                enqueue(&events, json!({
                    "type": "media_history_page",
                    "op_id": op_id,
                    "room_id": room_id,
                    "entries": [],
                    "scanned": 0,
                    "scanned_total": cursor.scanned_total,
                    "undecryptable_total": cursor.undecryptable_total,
                    "complete": true,
                    "encrypted_room": encrypted_room,
                }));
                return;
            }
            let mut opts = MessagesOptions::backward();
            opts.from = cursor.token.clone();
            opts.limit = UInt::new(want).unwrap_or(uint!(50));
            match room.messages(opts).await {
                Ok(messages) => {
                    let mut entries: Vec<serde_json::Value> = Vec::new();
                    let mut scanned = 0u64;
                    let mut undecryptable = 0u64;
                    // backward() gives newest-first, which is the order the
                    // browser shows, so the chunk is NOT reversed here.
                    for event in messages.chunk.iter() {
                        scanned += 1;
                        let raw = event.raw();
                        let Ok(value) = serde_json::from_str::<serde_json::Value>(
                            raw.json().get())
                        else {
                            continue;
                        };
                        let found = mediahistory::classify(&value);
                        if found.undecryptable {
                            undecryptable += 1;
                        }
                        for entry in found.entries {
                            entries.push(entry.to_json());
                        }
                    }
                    // An absent `end` is the server saying there is nothing
                    // older; so is an empty chunk, which some servers answer
                    // with instead.
                    let exhausted =
                        messages.end.is_none() || messages.chunk.is_empty();
                    let next = mediahistory::Cursor {
                        token: messages.end.clone(),
                        exhausted,
                        scanned_total: cursor.scanned_total + scanned,
                        undecryptable_total: cursor.undecryptable_total
                            + undecryptable,
                    };
                    if let Ok(mut map) = cursors.lock() {
                        map.insert(room_id.clone(), next.clone());
                    }
                    enqueue(&events, json!({
                        "type": "media_history_page",
                        "op_id": op_id,
                        "room_id": room_id,
                        "entries": entries,
                        "scanned": scanned,
                        "scanned_total": next.scanned_total,
                        "undecryptable_total": next.undecryptable_total,
                        "complete": exhausted,
                        "encrypted_room": encrypted_room,
                    }));
                }
                Err(err) => {
                    // The category matters: "the server refused" and "there is
                    // no more history" are different answers and the panel
                    // says different things about them.
                    enqueue(&events, json!({
                        "type": "media_history_failed",
                        "op_id": op_id,
                        "room_id": room_id,
                        "message": format_matrix_error(
                            "could not read room history", err),
                    }));
                }
            }
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_widgets(
    ptr: *mut c_void,
    room_id: *const c_char,
    theme: *const c_char,
    language: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let theme = unsafe { cstr_arg(theme) }?;
        let language = unsafe { cstr_arg(language) }?;
        let client = require_client_for_search(bridge)?;
        let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
        let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let homeserver = client.homeserver().to_string();
        let user_id = client.user_id().map(|u| u.to_string()).unwrap_or_default();
        let device_id = client.device_id().map(|d| d.to_string()).unwrap_or_default();
        bridge.spawn_room_action(async move {
            let found = widgets::read_room_widgets(&client, &room).await;
            // The user's own profile, read from the store — these are values a
            // widget URL may template, and resolving them per widget would be
            // one request per row.
            let display_name = match client.user_id() {
                Some(uid) => room
                    .get_member_no_sync(uid)
                    .await
                    .ok()
                    .flatten()
                    .and_then(|m| m.display_name().map(|d| d.to_owned()))
                    .unwrap_or_default(),
                None => String::new(),
            };
            let avatar = match client.user_id() {
                Some(uid) => room
                    .get_member_no_sync(uid)
                    .await
                    .ok()
                    .flatten()
                    .and_then(|m| m.avatar_url().map(|u| u.to_string()))
                    .unwrap_or_default(),
                None => String::new(),
            };
            let payloads: Vec<serde_json::Value> = found
                .iter()
                .map(|w| {
                    let values = widgets::template_values(
                        &user_id, room.room_id().as_str(), &w.id, &display_name,
                        &avatar, &device_id, &homeserver, &theme, &language);
                    widgets::widget_payload(w, &values)
                })
                .collect();
            enqueue(&events, json!({
                "type": "room_widgets", "op_id": op_id, "room_id": room_id,
                "ok": true, "widgets": payloads,
            }));
        });
        Ok(String::new())
    })
}

// ---------------------------------------------------------------------------
// Local message search (SQLite FTS5). See rust/src/localsearch.rs.
// ---------------------------------------------------------------------------

/// Open the account's index if it is not open yet.
///
/// LAZY, from ONE place, rather than at each of the three sites that install a
/// client: the index belongs to the store directory, not to a login flow, and
/// a helper every entry point already calls cannot be the one somebody forgets
/// to add to a fourth flow later.
fn ensure_search_index(bridge: &RustClient) -> Result<(), String> {
    if let Ok(guard) = bridge.search_index.lock() {
        if guard.is_some() {
            return Ok(());
        }
    }
    if bridge.store_path.as_os_str().is_empty() {
        return Err("no store path for this session".to_owned());
    }
    std::fs::create_dir_all(&bridge.store_path)
        .map_err(|e| format!("cannot create the store directory: {e}"))?;
    let index = localsearch::SearchIndex::open_in(&bridge.store_path)?;
    if let Ok(mut guard) = bridge.search_index.lock() {
        *guard = Some(index);
    }
    Ok(())
}

/// Search the local index. Answers on `local_search_result`.
///
/// SYNCHRONOUS on purpose. The whole point of a local index is that the answer
/// is a SQLite query on this machine, not a round trip — putting it on the
/// task pool would add latency to the one thing that has none, and would let
/// a stale answer arrive after the user typed the next character. The result
/// is enqueued rather than returned so the C++ side reads it through the same
/// poll loop as everything else.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_local_search(
    ptr: *mut c_void,
    query: *const c_char,
    room_id: *const c_char,
    limit: i32,
    offset: i32,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let query = unsafe { cstr_arg(query) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        ensure_search_index(bridge)?;

        // A query too short for the trigram tokenizer is REPORTED as such, not
        // answered with an empty list — "no results" and "type one more
        // character" are different sentences and the user can act on only one.
        if !localsearch::query_is_long_enough(&query) {
            enqueue(&bridge.events, json!({
                "type": "local_search_result", "op_id": op_id, "ok": false,
                "category": "too_short",
                "min_chars": localsearch::MIN_QUERY_CHARS,
                "results": [],
            }));
            return Ok(String::new());
        }

        let hits = {
            let guard = bridge.search_index.lock()
                .map_err(|_| "search index unavailable".to_owned())?;
            let index = guard.as_ref()
                .ok_or_else(|| "search index unavailable".to_owned())?;
            index.search(&query, &room_id, limit.max(1) as i64, offset.max(0) as i64)?
        };

        let results: Vec<serde_json::Value> = hits
            .into_iter()
            .map(|hit| json!({
                "event_id": hit.event_id,
                "room_id": hit.room_id,
                "sender": hit.sender,
                "sender_name": hit.sender_name,
                "body": hit.body,
                "msgtype": hit.msgtype,
                "timestamp_ms": hit.ts,
            }))
            .collect();
        enqueue(&bridge.events, json!({
            "type": "local_search_result", "op_id": op_id, "ok": true,
            "category": "", "results": results,
        }));
        Ok(String::new())
    })
}

/// How much the index holds, so the UI can say what search covers instead of
/// implying it covers everything. Answers on `search_index_stats`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_stats(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        ensure_search_index(bridge)?;
        let stats = {
            let guard = bridge.search_index.lock()
                .map_err(|_| "search index unavailable".to_owned())?;
            guard.as_ref()
                .ok_or_else(|| "search index unavailable".to_owned())?
                .stats()?
        };
        enqueue(&bridge.events, json!({
            "type": "search_index_stats", "op_id": op_id,
            "messages": stats.messages, "rooms": stats.rooms,
        }));
        Ok(String::new())
    })
}

/// Sweep every joined room's cached events into the index.
///
/// Runs on the tracked task pool, so sign-out joins it, and checks the
/// cooperative stop between rooms — a sweep must never be the reason an
/// account store cannot be deleted.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_sweep(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let client = require_client_for_search(bridge)?;
        ensure_search_index(bridge)?;
        let index = Arc::clone(&bridge.search_index);
        let stop = Arc::clone(&bridge.index_shutdown);
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            let (rooms, written) = localsearch::sweep(&client, &index, &stop).await;
            let stats = index.lock().ok()
                .and_then(|g| g.as_ref().and_then(|ix| ix.stats().ok()))
                .unwrap_or_default();
            enqueue(&events, json!({
                "type": "search_index_swept", "op_id": op_id,
                "rooms": rooms, "written": written,
                "messages": stats.messages, "indexed_rooms": stats.rooms,
            }));
        });
        Ok(String::new())
    })
}

/// Page ONE room backwards and index what arrives — "index this room's
/// history", the operation that turns "search what you have read" into
/// "search this room". Bounded; answers on `search_index_deepened`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_deep(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = require_client_for_search(bridge)?;
        ensure_search_index(bridge)?;
        let parsed = RoomId::parse(&room_id)
            .map_err(|_| "invalid room id".to_owned())?;
        let room = client.get_room(&parsed)
            .ok_or_else(|| "unknown room".to_owned())?;
        let index = Arc::clone(&bridge.search_index);
        let stop = Arc::clone(&bridge.index_shutdown);
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            match localsearch::deep_index_room(
                &room, &index, &stop, localsearch::DEEP_MAX_PAGES).await
            {
                Ok((pages, reached_start, written)) => {
                    let stats = index.lock().ok()
                        .and_then(|g| g.as_ref().and_then(|ix| ix.stats().ok()))
                        .unwrap_or_default();
                    enqueue(&events, json!({
                        "type": "search_index_deepened", "op_id": op_id,
                        "ok": true, "room_id": room_id, "pages": pages,
                        "reached_start": reached_start, "written": written,
                        "messages": stats.messages,
                        "indexed_rooms": stats.rooms, "category": "",
                    }));
                }
                Err(error) => enqueue(&events, json!({
                    "type": "search_index_deepened", "op_id": op_id,
                    "ok": false, "room_id": room_id, "pages": 0,
                    "reached_start": false, "written": 0,
                    "messages": 0, "indexed_rooms": 0,
                    "category": rooms::classify_room_error(&error),
                })),
            }
        });
        Ok(String::new())
    })
}

/// Forget one event — the redaction path. Synchronous and cheap.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_forget_event(
    ptr: *mut c_void,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event_id = unsafe { cstr_arg(event_id) }?;
        // NOT ensure_search_index: a redaction arriving before anything has
        // searched must not be the thing that CREATES an index file.
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.remove_event(&event_id)?;
            }
        }
        Ok(String::new())
    })
}

/// Forget one room, and the whole index. Both are user-visible actions
/// ("stop indexing this room", "clear the search index").
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_forget_room(
    ptr: *mut c_void,
    room_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.remove_room(&room_id)?;
            }
        }
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_clear(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        ensure_search_index(bridge)?;
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.clear()?;
            }
        }
        Ok(String::new())
    })
}

fn require_client_for_search(bridge: &RustClient) -> Result<Client, String> {
    bridge
        .client
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
        .ok_or_else(|| "no active Matrix session".to_owned())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_event_at_timestamp(
    ptr: *mut c_void,
    room_id: *const c_char,
    timestamp_ms: i64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::event_at_timestamp(bridge, room_id, timestamp_ms, op_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_event_source(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        rooms::request_event_source(bridge, room_id, event_id).map(|_| String::new())
    })
}

/// Replace the room's alternative-alias list (newline-separated). Aliases
/// not yet resolving to this room are published first, like the canonical
/// alias path; the canonical alias itself is preserved.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_alt_aliases(
    ptr: *mut c_void,
    room_id: *const c_char,
    aliases: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let aliases = unsafe { cstr_list_arg(aliases) }?;
        rooms::set_room_alt_aliases(bridge, room_id, aliases, op_id)
            .map(|_| String::new())
    })
}

/// v0.7.x room administration: set (or clear, with an empty string) the
/// room's canonical alias. Result event: room_edit_result, field
/// "canonical_alias".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_canonical_alias(
    ptr: *mut c_void,
    room_id: *const c_char,
    alias: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let alias = unsafe { cstr_arg(alias) }?;
        rooms::set_room_canonical_alias(bridge, room_id, alias, op_id)
            .map(|_| String::new())
    })
}

/// v0.7.x pinned messages: read `m.room.pinned_events` and resolve each id
/// into a displayable row. `allow_remote` (0/1) permits the `/state`
/// fallback taken only when the room carries no pinned-events state at all.
/// Result event: room_pinned.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_pinned(
    ptr: *mut c_void,
    room_id: *const c_char,
    allow_remote: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        pinned::fetch_pinned(bridge, room_id, allow_remote != 0, op_id)
            .map(|_| String::new())
    })
}

/// v0.7.x pinned messages: pin (`pin` = 1) or unpin (`pin` = 0) one event.
/// The SDK performs the read-modify-send of the state event. Result event:
/// room_pin_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_pinned(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
    pin: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        pinned::set_pinned(bridge, room_id, event_id, pin != 0, op_id)
            .map(|_| String::new())
    })
}

// ---------------------------------------------------------------------------
// v0.7.x room discovery / join / knock (discover.rs). One op-id per call;
// results arrive as room_target_resolved / public_rooms_result /
// room_join_result / room_knock_result / knock_cancel_result /
// space_children_result events.
// ---------------------------------------------------------------------------

/// Resolve user input (#alias, !roomid, matrix: URI, matrix.to permalink)
/// into a normalized join target and preview it where the server allows.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_resolve_room_target(
    ptr: *mut c_void,
    input: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let input = unsafe { cstr_arg(input) }?;
        discover::resolve_room_target(bridge, input, op_id).map(|_| String::new())
    })
}

/// One page of the public room directory. `server` optionally targets
/// another homeserver's directory; `since` is the pagination token from the
/// previous page's result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_public_rooms(
    ptr: *mut c_void,
    query: *const c_char,
    server: *const c_char,
    since: *const c_char,
    limit: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let query = unsafe { cstr_arg(query) }?;
        let server = unsafe { thread_root_arg(server) }?;
        let since = unsafe { thread_root_arg(since) }?;
        discover::search_public_rooms(bridge, query, server, since, limit, op_id)
            .map(|_| String::new())
    })
}

/// Join a room by id or alias. `via` is a newline-separated server list
/// (may be NULL/empty).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_join_room(
    ptr: *mut c_void,
    target: *const c_char,
    via: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let target = unsafe { cstr_arg(target) }?;
        let via = unsafe { cstr_list_arg(via) }?;
        discover::join_room(bridge, target, via, op_id).map(|_| String::new())
    })
}

/// Knock on a room by id or alias with an optional reason.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_knock_room(
    ptr: *mut c_void,
    target: *const c_char,
    via: *const c_char,
    reason: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let target = unsafe { cstr_arg(target) }?;
        let via = unsafe { cstr_list_arg(via) }?;
        let reason = unsafe { thread_root_arg(reason) }?;
        discover::knock_room(bridge, target, via, reason, op_id).map(|_| String::new())
    })
}

/// Withdraw a pending knock (leave a Knocked room).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_cancel_knock(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        discover::cancel_knock(bridge, room_id, op_id).map(|_| String::new())
    })
}

// ---------------------------------------------------------------------------
// v0.7.x ignored users + reporting (ignore.rs). SDK account-data and
// reporting APIs only. Events: ignore_user_result / ignored_users_list /
// ignored_users_changed (sync push) / report_message_result.
// ---------------------------------------------------------------------------

/// Ignore (`ignored` = 1) or unignore (0) one user via the SDK's
/// m.ignored_user_list read-modify-write.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_user_ignored(
    ptr: *mut c_void,
    user_id: *const c_char,
    ignored: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let user_id = unsafe { cstr_arg(user_id) }?;
        ignore::set_user_ignored(bridge, user_id, ignored != 0, op_id)
            .map(|_| String::new())
    })
}

/// Read the authoritative ignored-user list from account data.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_list_ignored_users(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        ignore::list_ignored_users(bridge, op_id).map(|_| String::new())
    })
}

/// Send `m.call.invite`. The offer SDP is required and opaque; it is never
/// logged or echoed back. Result: call_send_result via mx_rust_poll_event.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_invite(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    offer_type: *const c_char,
    offer_sdp: *const c_char,
    lifetime_ms: u64,
    invitee: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let offer_type = unsafe { cstr_arg(offer_type) }?;
        let offer_sdp = unsafe { cstr_arg(offer_sdp) }?;
        let invitee = unsafe { cstr_arg(invitee) }?;
        calls::send_invite(
            bridge, room_id, call_id, party_id, offer_type, offer_sdp,
            lifetime_ms, invitee, op_id,
        )
        .map(|_| String::new())
    })
}

/// Send `m.call.answer`. Plumbed for signaling completeness; no production
/// caller exists until a media backend can produce an answer SDP.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_answer(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    answer_type: *const c_char,
    answer_sdp: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let answer_type = unsafe { cstr_arg(answer_type) }?;
        let answer_sdp = unsafe { cstr_arg(answer_sdp) }?;
        calls::send_answer(
            bridge, room_id, call_id, party_id, answer_type, answer_sdp, op_id,
        )
        .map(|_| String::new())
    })
}

/// Send `m.call.reject` for an inbound VoIP-v1 invite.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_reject(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        calls::send_reject(bridge, room_id, call_id, party_id, op_id)
            .map(|_| String::new())
    })
}

/// Send `m.call.hangup` with a reason from the closed outbound set
/// (user_hangup, invite_timeout, user_busy, user_media_failed,
/// unknown_error, replaced).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_hangup(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    reason: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let reason = unsafe { cstr_arg(reason) }?;
        calls::send_hangup(bridge, room_id, call_id, party_id, reason, op_id)
            .map(|_| String::new())
    })
}

/// Send `m.call.select_answer` naming the locked answering party.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_select_answer(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    selected_party_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let selected = unsafe { cstr_arg(selected_party_id) }?;
        calls::send_select_answer(
            bridge, room_id, call_id, party_id, selected, op_id,
        )
        .map(|_| String::new())
    })
}

/// Toggle media-capable mode: whether inbound call handlers include the
/// remote SDP in poll payloads (C++ memory only — the C++ side stores it
/// bounded and single-shot, never logs it, never exposes it to QML).
/// Production leaves this OFF until a media backend is registered.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_set_media_capable(
    ptr: *mut c_void,
    capable: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge
            .call_media_capable
            .store(capable != 0, std::sync::atomic::Ordering::Relaxed);
        Ok(String::new())
    })
}

/// Send `m.call.candidates` (locally gathered ICE; re-validated/bounded).
/// `candidates_json` is a JSON array of {candidate, sdp_mid,
/// sdp_m_line_index} objects.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_candidates(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    candidates_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let candidates = unsafe { cstr_arg(candidates_json) }?;
        calls::send_candidates(bridge, room_id, call_id, party_id,
                               candidates, op_id)
            .map(|_| String::new())
    })
}

/// Fetch the homeserver's TURN servers (short-lived credentials; result
/// event call_turn_servers, consumed only by the media engine).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_turn_servers(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        calls::fetch_turn_servers(bridge, op_id).map(|_| String::new())
    })
}

/// Decline an `m.rtc.notification` ring (SDK-built decline content).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_rtc_decline(
    ptr: *mut c_void,
    room_id: *const c_char,
    notification_event_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(notification_event_id) }?;
        calls::rtc_decline(bridge, room_id, event_id, op_id)
            .map(|_| String::new())
    })
}

/// Connect to the SFU named by `service_url` for `room_id`.
///
/// Obtains authorization with a Matrix OpenID token (the access token never
/// reaches the SFU) and runs LiveKit signalling. Progress arrives as
/// `sfu_state` / `sfu_joined` / `sfu_participants` / `sfu_track_published` /
/// `sfu_speakers` / `sfu_quality` poll events; session descriptions and ICE
/// arrive as `sfu_remote_description` / `sfu_remote_candidate` and ONLY in
/// media-capable mode.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_connect(
    ptr: *mut c_void,
    service_url: *const c_char,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let service_url = unsafe { cstr_arg(service_url) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        sfu::connect(bridge, service_url, room_id, op_id).map(|_| String::new())
    })
}

/// Hand the SFU a local session description for one peer connection.
/// `target` is "publisher" (our tracks) or "subscriber" (everyone else's).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_local_description(
    ptr: *mut c_void,
    kind: *const c_char,
    target: *const c_char,
    sdp: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let kind = unsafe { cstr_arg(kind) }?;
        let target = unsafe { cstr_arg(target) }?;
        let sdp = unsafe { cstr_arg(sdp) }?;
        if sdp.trim().is_empty() {
            return Err("empty sdp".to_owned());
        }
        let target = sfu::target_from_str(&target);
        sfu::send_command(
            bridge,
            match kind.as_str() {
                "offer" => sfu::SfuCommand::Offer { sdp, target },
                "answer" => sfu::SfuCommand::Answer { sdp, target },
                _ => return Err("description kind must be offer or answer".to_owned()),
            },
        );
        Ok(String::new())
    })
}

/// Trickle one local ICE candidate. `candidate_init` is the JSON form
/// LiveKit expects ({candidate, sdpMid, sdpMLineIndex}).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_local_candidate(
    ptr: *mut c_void,
    target: *const c_char,
    candidate_init: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let target = unsafe { cstr_arg(target) }?;
        let candidate_init = unsafe { cstr_arg(candidate_init) }?;
        sfu::send_command(bridge, sfu::SfuCommand::Candidate {
            candidate_init,
            target: sfu::target_from_str(&target),
        });
        Ok(String::new())
    })
}

/// Declare a track before publishing it. `kind` is 0 audio / 1 video.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_add_track(
    ptr: *mut c_void,
    cid: *const c_char,
    name: *const c_char,
    kind: i32,
    width: u32,
    height: u32,
    screen_share: u8,
    encrypted: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let cid = unsafe { cstr_arg(cid) }?;
        let name = unsafe { cstr_arg(name) }?;
        sfu::send_command(bridge, sfu::SfuCommand::AddTrack {
            cid,
            name,
            width,
            height,
            kind: if kind == 1 { 1 } else { 0 },
            screen_share: screen_share != 0,
            encrypted: encrypted != 0,
        });
        Ok(String::new())
    })
}

/// Tell the SFU a published track is muted, so other participants see it.
/// The bytes are already stopped locally by the engine's valve; this is the
/// SIGNAL, not the mute itself.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_mute_track(
    ptr: *mut c_void,
    sid: *const c_char,
    muted: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let sid = unsafe { cstr_arg(sid) }?;
        sfu::send_command(bridge, sfu::SfuCommand::MuteTrack {
            sid,
            muted: muted != 0,
        });
        Ok(String::new())
    })
}

/// Leave the SFU session and tear the signalling down.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_disconnect(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        sfu::disconnect(bridge);
        Ok(String::new())
    })
}

/// Report the current MatrixRTC session in one room.
///
/// Result arrives as an `rtc_session` poll event carrying the participant
/// list, the selected focus and the slot status. Read-only: this publishes
/// nothing and joins nothing.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_session(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rtc::request_session(bridge, room_id, op_id).map(|_| String::new())
    })
}

/// Discover the MatrixRTC transports available to this account.
///
/// `room_id` may be empty; when given it adds the focus the room's existing
/// participants advertise, which is the only discovery route on a homeserver
/// with no MSC4143 endpoint.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_transports(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rtc::request_transports(bridge, room_id, op_id).map(|_| String::new())
    })
}

/// Publish (or refresh) our own MatrixRTC membership in a room's call.
///
/// Answers `rtc_membership_published {ok, category, event_id, delay_id,
/// delayed_category}`. An empty `delay_id` means the server has no MSC4140
/// delayed events, so cleanup falls back to the membership's own `expires`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_publish_membership(
    ptr: *mut c_void,
    room_id: *const c_char,
    focus_url: *const c_char,
    intent: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let focus_url = unsafe { cstr_arg(focus_url) }?;
        let intent = unsafe { cstr_arg(intent) }?;
        rtc::publish_membership(bridge, room_id, focus_url, intent, op_id)
            .map(|_| String::new())
    })
}

/// Restart the server-side delayed retraction, so it keeps not firing.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_restart_delayed_leave(
    ptr: *mut c_void,
    delay_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let delay_id = unsafe { cstr_arg(delay_id) }?;
        rtc::restart_delayed_leave(bridge, delay_id, op_id)
            .map(|_| String::new())
    })
}

/// Retract our membership and cancel any pending delayed retraction.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_retract_membership(
    ptr: *mut c_void,
    room_id: *const c_char,
    delay_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let delay_id = unsafe { cstr_arg(delay_id) }?;
        rtc::retract_membership(bridge, room_id, delay_id, op_id)
            .map(|_| String::new())
    })
}

/// Distribute our media key to the call's participant devices.
///
/// Olm-encrypted per device, so the homeserver never sees the key.
/// `targets_json` is `[{user_id, device_id}]` taken from the observed
/// membership — a device that has not declared itself present is not sent
/// the key. Answers `rtc_key_sent {ok, category, delivered, key_index}`;
/// the key itself is never echoed back.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_send_media_key(
    ptr: *mut c_void,
    room_id: *const c_char,
    key_base64: *const c_char,
    key_index: u8,
    targets_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let key_base64 = unsafe { cstr_arg(key_base64) }?;
        let targets_json = unsafe { cstr_arg(targets_json) }?;
        rtc::send_media_key(bridge, room_id, key_base64, key_index,
                            targets_json, op_id)
            .map(|_| String::new())
    })
}

/// Send an `org.matrix.msc4075.rtc.notification` (the ring).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_notify(
    ptr: *mut c_void,
    room_id: *const c_char,
    notification_type: *const c_char,
    intent: *const c_char,
    lifetime_ms: u64,
    membership_event_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let notification_type = unsafe { cstr_arg(notification_type) }?;
        let intent = unsafe { cstr_arg(intent) }?;
        let membership_event_id = unsafe { cstr_arg(membership_event_id) }?;
        rtc::send_notification(
            bridge,
            room_id,
            notification_type,
            intent,
            lifetime_ms,
            membership_event_id,
            op_id,
        )
        .map(|_| String::new())
    })
}

/// Raise or lower this device's hand in a room's call.
///
/// element-call's own wire format: raising sends an `m.reaction` annotating
/// the sender's OWN `m.call.member` state event with the raised-hand emoji,
/// and lowering REDACTS that reaction. `membership_event_id` is required to
/// raise, `reaction_event_id` to lower.
///
/// Answers `rtc_hand_result {ok, raised, category, event_id}`; on a
/// successful raise `event_id` is the reaction the eventual lower must
/// redact, and without keeping it a raised hand can never be lowered.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_set_hand(
    ptr: *mut c_void,
    room_id: *const c_char,
    membership_event_id: *const c_char,
    reaction_event_id: *const c_char,
    raised: c_uchar,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let membership_event_id = unsafe { cstr_arg(membership_event_id) }?;
        let reaction_event_id = unsafe { cstr_arg(reaction_event_id) }?;
        // `unsigned char` on both sides, matching
        // mx_rust_calls_set_media_capable: a C `bool` and a Rust `bool` agree
        // on size today and nothing in this header depends on that.
        rtc::set_hand_raised(
            bridge,
            room_id,
            membership_event_id,
            reaction_event_id,
            raised != 0,
            op_id,
        )
        .map(|_| String::new())
    })
}

/// Read the hands already raised in a room's call.
///
/// Spent ONCE per join: a hand raised before this client arrived produces no
/// sync event for us, so without this pass an early raiser is invisible for
/// the rest of the call. Bounded and cache-first; answers `rtc_hands`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_read_hands(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rtc::read_raised_hands(bridge, room_id, op_id).map(|_| String::new())
    })
}

/// Report one event to the homeserver administrator (stable /v3 endpoint).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_report_message(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
    reason: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        let reason = unsafe { thread_root_arg(reason) }?;
        ignore::report_message(bridge, room_id, event_id, reason, op_id)
            .map(|_| String::new())
    })
}

// ---------------------------------------------------------------------------
// v0.7.x UIA + device sign-out (uia.rs). The SDK surfaces the server's UIA
// challenge; Lightning parks the operation and answers with the password
// stage. Events: uia_required / device_delete_result.
// ---------------------------------------------------------------------------

/// Delete own devices (newline-separated ids). May raise `uia_required`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_delete_devices(
    ptr: *mut c_void,
    device_ids: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let ids = unsafe { cstr_list_arg(device_ids) }?;
        uia::delete_devices(bridge, ids, op_id).map(|_| String::new())
    })
}

/// Answer the pending UIA challenge with the account password and retry
/// the parked operation. The transit buffer is scrubbed inside.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_uia_submit_password(
    ptr: *mut c_void,
    uia_id: u64,
    password: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let password = unsafe { cstr_arg(password) }?;
        uia::uia_submit_password(bridge, uia_id, password).map(|_| String::new())
    })
}

/// Abandon the pending UIA challenge (dialog cancelled).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_uia_cancel(
    ptr: *mut c_void,
    uia_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        uia::uia_cancel(bridge, uia_id);
        Ok(String::new())
    })
}

/// v0.7.x server-side message search (POST /_matrix/client/v3/search via
/// raw ruma — matrix-sdk has no wrapper). Covers unencrypted rooms ONLY;
/// the server cannot search ciphertext. `room_id` empty = all rooms;
/// `next_batch` pages. Result event: message_search_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_messages(
    ptr: *mut c_void,
    term: *const c_char,
    room_id: *const c_char,
    next_batch: *const c_char,
    filters_json: *const c_char,
    limit: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let term = unsafe { cstr_arg(term) }?;
        let room_id = unsafe { thread_root_arg(room_id) }?;
        let next_batch = unsafe { thread_root_arg(next_batch) }?;
        let filters_json = unsafe { cstr_arg(filters_json) }?;
        search::search_messages(
            bridge, term, room_id, next_batch, filters_json, limit, op_id,
        )
            .map(|_| String::new())
    })
}

/// List a Space's children (joined and unjoined) via the SDK's
/// /hierarchy-backed SpaceRoomList. Bounded; reports `truncated`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_space_children(
    ptr: *mut c_void,
    space_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let space_id = unsafe { cstr_arg(space_id) }?;
        discover::space_children(bridge, space_id, op_id).map(|_| String::new())
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

/// 2026-08-19 Space management: toggles the MSC1772 `suggested` flag on an
/// EXISTING m.space.child (the event's via list and order key are
/// preserved; a non-child is refused, never promoted). Result event:
/// space_child_suggested_result { op_id, space_id, room_id, suggested, ok }.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_space_child_suggested(
    ptr: *mut c_void,
    space_id: *const c_char,
    room_id: *const c_char,
    suggested: c_int,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let space_id = unsafe { cstr_arg(space_id) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::set_space_child_suggested(bridge, space_id, room_id,
                                         suggested != 0, op_id)
            .map(|_| String::new())
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

/// v0.7 video round: send a video with a locally extracted poster frame.
///
/// `thumb_*` is optional (null/zero-length data sends the video with no
/// poster, exactly as the plain attachment path always did). The bytes are
/// bounded here so a hostile length can never allocate unbounded memory,
/// and re-validated by magic sniffing in `rooms::PosterBytes` — a poster
/// that does not validate is dropped and the video still sends. The SDK
/// uploads and (in encrypted rooms) encrypts the poster itself.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_timeline_send_video(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    duration_ms: u64,
    thumb_data: *const u8,
    thumb_len: usize,
    thumb_width: u64,
    thumb_height: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        let poster = unsafe { poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        rooms::send_video_path(
            bridge, room_id, local_path, mime, caption, width, height,
            duration_ms, poster, op_id,
        )
        .map(|_| String::new())
    })
}

/// v0.7 video round: the thread twin of `mx_rust_timeline_send_video`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_thread_send_video(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    duration_ms: u64,
    thumb_data: *const u8,
    thumb_len: usize,
    thumb_width: u64,
    thumb_height: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        let poster = unsafe { poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        rooms::send_thread_video_path(
            bridge, room_id, root, local_path, mime, caption, width, height,
            duration_ms, poster, op_id,
        )
        .map(|_| String::new())
    })
}

/// v0.7 voice round: MSC3245 voice message. `waveform` is 0..=100
/// amplitudes (may be null/empty); bounded here so a hostile length can
/// never allocate unbounded memory. Result echoes on
/// attachment_send_result by op_id exactly like every attachment send.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_timeline_send_voice(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    duration_ms: u64,
    waveform: *const u8,
    waveform_len: usize,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if waveform_len > 1024 {
            return Err("voice waveform is too long".to_owned());
        }
        let waveform = if waveform.is_null() || waveform_len == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(waveform, waveform_len) }.to_vec()
        };
        rooms::send_voice_path(
            bridge, room_id, local_path, mime, duration_ms, waveform, op_id,
        )
        .map(|_| String::new())
    })
}

/// v0.7 thread parity: the thread twin of `mx_rust_timeline_send_voice`.
/// Same MSC3245 metadata and the same waveform bound; routed through the
/// thread-focused SDK timeline so the event carries a real `m.thread`
/// relation to `root_event_id`. Result echoes on attachment_send_result by
/// op_id exactly like every attachment send.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_thread_send_voice(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    local_path: *const c_char,
    mime: *const c_char,
    duration_ms: u64,
    waveform: *const u8,
    waveform_len: usize,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root_event_id = unsafe { cstr_arg(root_event_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if waveform_len > 1024 {
            return Err("voice waveform is too long".to_owned());
        }
        let waveform = if waveform.is_null() || waveform_len == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(waveform, waveform_len) }.to_vec()
        };
        rooms::send_thread_voice_path(
            bridge, room_id, root_event_id, local_path, mime, duration_ms,
            waveform, op_id,
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

/// Cancel an in-flight media fetch by op id. Aborts the download task at
/// its next await point (freeing its bandwidth and its store access) and
/// drops any bytes it already parked. Idempotent: unknown, finished, or
/// already-cancelled op ids are a no-op. The caller (C++ MediaBridge) has
/// already released its own slot for the op, so no terminal event is
/// emitted for a cancelled fetch — a late one would be dropped as stale
/// anyway.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_cancel(ptr: *mut c_void, op_id: u64) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let Ok(bridge) = (unsafe { bridge(ptr) }) else {
            return;
        };
        if let Some(handle) = bridge
            .media_fetch_aborts
            .lock()
            .ok()
            .and_then(|mut guard| guard.remove(&op_id))
        {
            handle.abort();
        }
        if let Ok(mut guard) = bridge.media_results.lock() {
            guard.remove(&op_id);
        }
    }));
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
        // The MISS COUNTS are the necessary condition for the double-poll
        // SIGABRT this function used to be able to take (see join_or_abort),
        // and nothing anywhere recorded that the budget had been exceeded.
        // Counts only — no task identity, no room id, nothing content-derived.
        let (import_joined, sync_stopped, actions_missed, verifications_missed,
             actions_ms, verifications_ms, total_ms) =
            bridge.shutdown_managed_tasks();
        Ok(format!(
            "import_joined={import_joined} sync_stopped={sync_stopped} \
             actions_missed={actions_missed} verifications_missed={verifications_missed} \
             actions_ms={actions_ms} verifications_ms={verifications_ms} \
             total_ms={total_ms}"
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

/// MSC4153 "invisible crypto": whether this process builds its clients to
/// exclude devices that are not cross-signed.
///
/// A PROCESS GLOBAL rather than a parameter, because `build_client` is the
/// one build path for password login, OAuth sign-in and the auth-method
/// probe, and threading a settings argument through all of them (and through
/// the callers that have no settings to give) would put the same value in
/// four places for one switch.
///
/// It is read at BUILD time and never afterwards. matrix-sdk 0.18 exposes no
/// runtime setter for either half of this — `decryption_settings()` is
/// read-only and there is no `set_room_key_recipient_strategy` at all — so a
/// change genuinely does require a new client, and the UI says so rather
/// than pretending otherwise.
static STRICT_DEVICE_TRUST: AtomicBool = AtomicBool::new(false);

/// Set by the app layer BEFORE a login. Takes effect on the next client that
/// is built, which is the honest contract the SDK allows.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_strict_device_trust(enabled: c_int) -> *mut c_char {
    ffi_string(|| {
        STRICT_DEVICE_TRUST.store(enabled != 0, Ordering::SeqCst);
        Ok(String::new())
    })
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
    // An EMPTY store path means "no persistent store": the builder's default
    // is an in-memory store, and skipping .sqlite_store() leaves it there.
    // This is what the OAuth bootstrap phase uses (see oauth.rs) — it must
    // authenticate, learn the canonical user/device, and be dropped WITHOUT
    // ever creating an account store on disk, because until `whoami` answers
    // there is no way to know which account's store it would be. Every
    // ordinary login/restore passes a real path and is unaffected.
    // handle_refresh_tokens defaults to FALSE in matrix-sdk 0.18. Without it
    // the SDK forwards a 401 straight through instead of renewing, so a saved
    // refresh token is inert and an expired access token still surfaces as
    // M_UNKNOWN_TOKEN — carrying the token is necessary but not sufficient.
    // Rotated tokens must then be persisted on every change (see
    // oauth::spawn_token_persistence), or the store keeps a CONSUMED refresh
    // token and an OAuth 2.1 server treats its reuse as compromise.
    // MATRIX DELEGATION. The user types a SERVER NAME ("example.com"), and the
    // client API may live somewhere else entirely — that is what
    // /.well-known/matrix/client is for, and it is how most self-hosted
    // deployments are set up. `homeserver_url()` performs no discovery at all,
    // so a delegated server answered the login request with the web server
    // sitting on the apex domain: "[404] <non-json bytes>", reported as
    // Lightning simply being unable to sign in (issue #5).
    //
    // `server_name_or_homeserver_url()` is the one that handles a field a
    // HUMAN typed. It strips any scheme, tries well-known discovery, and only
    // if that fails falls back to treating the input as a homeserver URL —
    // and unlike `homeserver_url()` it then VERIFIES that URL really is a
    // homeserver before handing back a client. So "example.com",
    // "https://example.com" and "https://matrix.example.com" all work, and a
    // typo fails at build time with a real error instead of a 404 later.
    //
    // This is the single build path behind password login, OAuth sign-in and
    // the login screen's auth-method probe, so all three follow delegation.
    let mut builder = Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .user_agent(USER_AGENT)
        .handle_refresh_tokens()
        .with_encryption_settings(encryption_settings)
        .with_threading_support(ThreadingSupport::Enabled { with_subscriptions: false });

    // MSC4153, and it is deliberately ONE switch driving BOTH halves.
    //
    // The two knobs are independent in the SDK and setting only one gives an
    // ASYMMETRIC client: we would refuse to share room keys with devices that
    // are not cross-signed while still decrypting what those devices send us,
    // or the exact reverse. Neither half is the feature; the pair is.
    //
    //   send    — IdentityBasedStrategy is what the SDK's own documentation
    //             identifies as Element's "exclude insecure devices" mode.
    //   receive — CrossSignedOrLegacy, NEVER CrossSigned. The strict variant
    //             refuses legacy Megolm sessions — the ones created before
    //             clients collected trust information — which would turn a
    //             user's existing history into undecryptable events the
    //             moment they enabled a privacy setting.
    if STRICT_DEVICE_TRUST.load(Ordering::SeqCst) {
        builder = builder
            .with_room_key_recipient_strategy(
                matrix_sdk_base::crypto::CollectStrategy::IdentityBasedStrategy,
            )
            .with_decryption_settings(matrix_sdk_base::crypto::DecryptionSettings {
                sender_device_trust_requirement:
                    matrix_sdk_base::crypto::TrustRequirement::CrossSignedOrLegacy,
            });
    }
    if !store_path.as_os_str().is_empty() {
        builder = builder.sqlite_store(store_path, None);
    }
    let client = builder
        .build()
        .await
        .map_err(|err| format_matrix_error("failed to build Matrix Rust SDK client", err))?;
    // Media-store retention policy. Without one the SDK runs
    // MediaRetentionPolicy::empty(): every fetched payload — including a
    // 500 MiB video — is INSERTed whole into matrix-sdk-media.sqlite3, the
    // store grows without bound, and cleanup never runs. Worse, the media
    // store serializes ALL cache reads and writes on its single write
    // connection (reads bump last_access first), so one giant blob INSERT
    // stalls every avatar/thumbnail/audio fetch behind it and can lapse the
    // cross-process lease into TimedOut errors — the observed "after a
    // video plays, other media loads slowly or not at all". The policy
    // makes the store skip oversized payloads BEFORE the write; the paired
    // guard in rooms::media_fetch skips the doomed cache round-trip
    // entirely for declared-oversize fetches. new() carries the SDK
    // defaults (400 MiB cache budget, 60-day expiry, daily cleanup); only
    // max_file_size is tuned up to keep the 20 MiB animated-GIF class
    // cacheable across sessions.
    let policy = matrix_sdk::media::MediaRetentionPolicy::new()
        .with_max_file_size(Some(rooms::MEDIA_STORE_MAX_FILE_BYTES));
    // Best-effort: a policy write failure must never block login, and the
    // error string may embed the store path — it is deliberately not
    // logged anywhere.
    if client.media().set_media_retention_policy(policy).await.is_ok() {
        // Sweep blobs cached before the policy existed (or by older
        // builds). Runs once per client BUILD (login/restore/switch), not
        // once ever — the SDK's own cleanup_frequency debounces the real
        // work to daily. Fire-and-forget but BOUNDED: the task holds a
        // Client clone, and dropping the shared runtime on session release
        // cancels it before any store deletion; the timeout keeps it from
        // holding the media store's write connection indefinitely either
        // way.
        let media_client = client.clone();
        tokio::spawn(async move {
            let _ = tokio::time::timeout(
                std::time::Duration::from_secs(120),
                media_client.media().clean(),
            )
            .await;
        });
    }
    Ok(client)
}

async fn restore_client(
    homeserver: &str,
    store_path: &Path,
    user_id: &str,
    device_id: &str,
    access_token: String,
    refresh_token: Option<String>,
) -> Result<Client, String> {
    let user_id: OwnedUserId = UserId::parse(user_id)
        .map_err(|err| format!("invalid stored Matrix user id: {err}"))?
        .to_owned();
    let device_id: OwnedDeviceId = device_id.to_owned().into();
    // refresh_token was hardcoded to None until 0.6.7. That was survivable
    // only while every session was a password session against a server that
    // issued non-expiring access tokens: a saved refresh token was dropped on
    // every restore, so a session whose access token expired could not be
    // renewed and surfaced as M_UNKNOWN_TOKEN instead. It must be carried for
    // password sessions too — refreshable password sessions exist — and it is
    // mandatory for OAuth, where tokens are short-lived by design.
    let session = MatrixSession {
        meta: SessionMeta { user_id, device_id },
        tokens: SessionTokens { access_token, refresh_token },
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
    active_sas: KeyedFlowSlot<SasVerification>,
    active_qr: KeyedFlowSlot<QrVerification>,
) {
    // v0.5.0: interactive verification, receive-first. matrix-sdk 0.18 does
    // NOT expose a public `recv_verification_requests` stream, so we
    // observe incoming requests via a to-device event handler and then
    // hydrate the `VerificationRequest` via
    // `client.encryption().get_verification_request(user, flow_id)`.
    //
    // The active flow (single-flow policy) is stored in
    // `active_request`; the FFI accept path drives `accept_with_methods()`
    // and then the show-QR / SAS driver from that stored handle. No secret
    // material is ever forwarded through the FFI — only flow id, mxid,
    // device id, is_self_verification, SAS emojis (safe to display by SAS
    // design), and a QR MODULE GRID (never the payload it encodes).
    let verif_events = Arc::clone(&events);
    let verif_slot = Arc::clone(&active_request);
    let verif_sas_slot = Arc::clone(&active_sas);
    let verif_qr_slot = Arc::clone(&active_qr);
    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: ToDeviceKeyVerificationRequestEvent| {
            let events = Arc::clone(&verif_events);
            let slot = Arc::clone(&verif_slot);
            let sas_slot = Arc::clone(&verif_sas_slot);
            let qr_slot = Arc::clone(&verif_qr_slot);
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
                if flow_slots_are_live(&slot, &sas_slot, &qr_slot) {
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
    // Membership changes from sync: a lightweight per-room poke so an open
    // People panel (and the mention roster) refetches without reopening —
    // sync otherwise never produces a members snapshot (live report
    // 2026-08-14: a user joined, sent a message, and the panel still
    // showed one person). Only the room id crosses; the C++ side routes
    // it to the refetch consumers alone (roomMemberEventSeen, review H1).
    //
    // Rate-limited per room (review M2): m.room.member also covers every
    // display-name/avatar change, and a bridged room can sync several per
    // second — an unthrottled poke would flood the bounded event queue
    // and turn each event into a roster refetch. At most one leading poke
    // per second per room; a suppressed burst schedules ONE trailing poke
    // so the last change of a burst is never silently missed.
    let member_events = Arc::clone(&events);
    let member_poke_state: Arc<Mutex<HashMap<OwnedRoomId, (std::time::Instant, bool)>>> =
        Arc::new(Mutex::new(HashMap::new()));
    client.add_event_handler(move |_ev: SyncRoomMemberEvent, room: Room| {
        let events = Arc::clone(&member_events);
        let poke_state = Arc::clone(&member_poke_state);
        async move {
            const WINDOW: std::time::Duration = std::time::Duration::from_secs(1);
            let room_id = room.room_id().to_owned();
            let now = std::time::Instant::now();
            {
                let mut state = poke_state.lock().unwrap();
                match state.get_mut(&room_id) {
                    Some((last, trailing)) if now.duration_since(*last) < WINDOW => {
                        if *trailing {
                            return; // a trailing poke is already scheduled
                        }
                        *trailing = true;
                        let events = Arc::clone(&events);
                        let poke_state = Arc::clone(&poke_state);
                        let deadline = *last + WINDOW;
                        let room_id = room_id.clone();
                        tokio::spawn(async move {
                            tokio::time::sleep_until(deadline.into()).await;
                            if let Some((last, trailing)) =
                                poke_state.lock().unwrap().get_mut(&room_id)
                            {
                                *last = std::time::Instant::now();
                                *trailing = false;
                            }
                            enqueue(&events, json!({
                                "type": "room_members_changed",
                                "room_id": room_id.to_string(),
                            }));
                        });
                        return;
                    }
                    Some(entry) => *entry = (now, false),
                    None => {
                        state.insert(room_id.clone(), (now, false));
                    }
                }
            }
            enqueue(&events, json!({
                "type": "room_members_changed",
                "room_id": room_id.to_string(),
            }));
        }
    });

    // v0.7.x pinned messages: another client (or another of this user's
    // devices) changed `m.room.pinned_events`. Only the room id crosses —
    // the C++ side re-reads the authoritative list through the normal fetch
    // rather than trusting a payload assembled here, so a remote pin and a
    // local one converge on exactly the same code path.
    //
    // Deliberately NOT rate-limited the way the member poke is: pinning is a
    // human action at human frequency, and coalescing it would delay the
    // very update the user is watching for.
    let pinned_events = Arc::clone(&events);
    client.add_event_handler(move |_ev: SyncRoomPinnedEventsEvent, room: Room| {
        let events = Arc::clone(&pinned_events);
        async move {
            enqueue(&events, json!({
                "type": "room_pinned_changed",
                "room_id": room.room_id().to_string(),
            }));
        }
    });

    // v0.7.x room upgrades: an m.room.tombstone means this room has been
    // replaced, and the banner offering to continue in the successor must
    // appear without waiting for a restart.
    //
    // Not rate-limited, for the same reason the pinned poke above is not,
    // only more so: a room is tombstoned once in its entire lifetime, so
    // the coalescing that protects against member churn has nothing here to
    // protect against and would only delay the update.
    //
    // Unlike the pinned poke this one CARRIES its payload, because the
    // successor id is the whole fact — a payload-free poke would force a
    // re-read of a value already in hand. It is taken from the SDK's
    // `successor_room()` rather than the handler's own event content, so
    // there stays exactly one parse path and one type for a room id.
    let tombstone_events = Arc::clone(&events);
    client.add_event_handler(move |_ev: SyncRoomTombstoneEvent, room: Room| {
        let events = Arc::clone(&tombstone_events);
        async move {
            enqueue(&events, json!({
                "type": "room_tombstone_changed",
                "room_id": room.room_id().to_string(),
                "successor_room_id": room
                    .successor_room()
                    .map(|successor| successor.room_id.to_string())
                    .unwrap_or_default(),
            }));
        }
    });

    // v0.7.x room administration: a power-level change alters who may do
    // what, so it must invalidate the cached permission flags immediately —
    // not when the panel is next reopened. It routes through the EXISTING
    // members poke because the member snapshot is what carries both the
    // per-member levels and the viewer's own permissions; a second, parallel
    // refresh path would be able to disagree with it.
    let power_level_events = Arc::clone(&events);
    client.add_event_handler(move |_ev: SyncRoomPowerLevelsEvent, room: Room| {
        let events = Arc::clone(&power_level_events);
        async move {
            enqueue(&events, json!({
                "type": "room_members_changed",
                "room_id": room.room_id().to_string(),
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

    // v0.9 (phase 2): reactions from sync, ANY room, for the Activity Center
    // — a reaction to the user's own message is activity, and the timeline
    // diff stream only covers the open room. Ids, the sender and a bounded
    // key cross; the C++ side decides whether the target is its own.
    let reaction_events = Arc::clone(&events);
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::reaction::OriginalSyncReactionEvent, room: Room| {
            let events = Arc::clone(&reaction_events);
            async move {
                let key: String = ev.content.relates_to.key.chars().take(32).collect();
                enqueue(
                    &events,
                    json!({
                        "type": "reaction_event",
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "target_event_id": ev.content.relates_to.event_id.to_string(),
                        "sender": ev.sender.to_string(),
                        "key": key,
                        "timestamp_ms": u64::from(ev.origin_server_ts.get()),
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
    room_list_slot: Arc<Mutex<Option<Arc<RoomListService>>>>,
    active_subscription: Arc<Mutex<Option<OwnedRoomId>>>,
    timelines: Arc<timeline::TimelineRegistry>,
    call_media_capable: Arc<std::sync::atomic::AtomicBool>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) {
    // Inbound call-signaling observers live exactly as long as this sync
    // loop: the drop guards unregister every handler on ANY exit path, so
    // an orphaned handler can never fire into a later account's queue.
    let _call_guards = calls::register_handlers(
        &client, &events, &timelines, &call_media_capable);
    // MatrixRTC observation shares that lifetime for the same reason. Kept
    // separate from the legacy lane because it answers a different question:
    // who is in a room's call right now, versus who is inviting whom.
    let _rtc_guards = rtc::register_rtc_handlers(&client, &events, &timelines);

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
            client.clone(), Arc::clone(&events), Arc::clone(&sync_mode),
            room_list_slot, active_subscription, cancel
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
    room_list_slot: Arc<Mutex<Option<Arc<RoomListService>>>>,
    active_subscription: Arc<Mutex<Option<OwnedRoomId>>>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) -> Option<tokio::sync::oneshot::Receiver<()>> {
    // Withdraws the published RoomListService on EVERY exit path of this
    // function — a handle outliving its sync loop would accept subscription
    // calls that can never reach a server again.
    struct RoomListPublication(Arc<Mutex<Option<Arc<RoomListService>>>>);
    impl RoomListPublication {
        fn set(&self, service: Option<Arc<RoomListService>>) {
            if let Ok(mut guard) = self.0.lock() {
                *guard = service;
            }
        }
    }
    impl Drop for RoomListPublication {
        fn drop(&mut self) {
            self.set(None);
        }
    }
    let publication = RoomListPublication(room_list_slot);

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
        // v0.7.x ignored users: the SDK diffs m.ignored_user_list on every
        // sync and publishes only real changes, so this stream is safe to
        // forward directly. Local ignores and remote ones (another client)
        // both arrive here — C++ re-reads through one path.
        let mut ignore_list_sub = client.subscribe_to_ignore_user_list_changes();
        // Bounded latest-event registration state for this sync session.
        let latest_events = client.latest_events().await;
        let mut watched_latest: BTreeSet<OwnedRoomId> = BTreeSet::new();

        // Publish the service so room opens can (re)target the single
        // active-room subscription, then apply the room that is ALREADY
        // open: on a restored session the user's room opens before this
        // loop reaches here, and without this catch-up its
        // subscription-only required state (m.room.pinned_events) would
        // wait for the next room switch.
        publication.set(Some(Arc::clone(&room_list_service)));
        if let Some(room_id) = active_subscription
            .lock()
            .ok()
            .and_then(|guard| guard.clone())
        {
            room_list_service.subscribe_to_rooms(&[&room_id]).await;
        }

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
                changed = ignore_list_sub.next() => {
                    if let Some(users) = changed {
                        enqueue(&events, json!({
                            "type": "ignored_users_changed",
                            "users": users,
                        }));
                    }
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

/// When the classic sync should say it has had no first response, escalating
/// rather than firing once.
///
/// Generous on purpose. The first request is full-state, and on a large
/// account that is genuinely heavy — these are the points at which silence
/// stops being explainable by size, not a deadline anything is held to.
const FIRST_SYNC_STALL_STEPS: &[std::time::Duration] = &[
    std::time::Duration::from_secs(60),
    std::time::Duration::from_secs(180),
    std::time::Duration::from_secs(600),
];

/// Report, at each step, that no first sync response has arrived yet.
///
/// Returns as soon as a response lands, so the caller can park afterwards.
/// Split out from run_classic_sync purely so it can be tested: driving it
/// with millisecond steps is the whole difference between a covered
/// escalation and a 10-minute sleep nobody ever runs.
async fn watch_first_sync_response(
    events: &Arc<Mutex<VecDeque<String>>>,
    first_response: &Arc<AtomicBool>,
    steps: &[std::time::Duration],
) {
    let mut waited = std::time::Duration::ZERO;
    for step in steps {
        tokio::time::sleep(step.saturating_sub(waited)).await;
        waited = *step;
        if !first_response.load(Ordering::SeqCst) {
            return; // the response landed; nothing to report
        }
        enqueue(events, json!({
            "type": "sync_stalled",
            "phase": "first_response",
            "waited_secs": waited.as_secs(),
        }));
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
    // Classic sync v2 populates NO recency information by itself: the SDK's
    // recency_stamp is written only by the simplified-sliding-sync response
    // processor, and LatestEventValue only for rooms registered with the
    // lazy Latest Events API — which the sliding path registers from its
    // room-list diffs and this path therefore cannot rely on. Without help,
    // every ordering stamp is 0 and the room list sorts arbitrarily (first
    // observed live against Beeper, whose server takes the fallback).
    //
    // Two mechanisms, with distinct jobs:
    //
    //  * ORDERING comes from the sync responses themselves. Every response
    //    carries each updated room's new timeline events, and the initial
    //    full-state response carries a recent window for every room — so
    //    harvesting the newest origin_server_ts per room gives every room
    //    a truthful recency stamp, unbounded by any cap, and a room that
    //    wakes up after months re-stamps itself on the response that wakes
    //    it. The stamp map overrides the payload's last_activity_ms
    //    whenever it knows better.
    //
    //  * PREVIEWS come from the Latest Events API, which stays bounded by
    //    the shared cap exactly as on the sliding path. Classic iteration
    //    order is arbitrary where sliding sync's was recency-ordered, so
    //    rooms with unread activity claim the cap first, and when the cap
    //    is full a waking room evicts the stalest watched room rather than
    //    being refused — the cap bounds SDK computation, it must not
    //    freeze the set chosen in the first minute forever.
    let callback_watched: Arc<tokio::sync::Mutex<BTreeSet<OwnedRoomId>>> =
        Arc::new(tokio::sync::Mutex::new(BTreeSet::new()));
    let callback_stamps: Arc<tokio::sync::Mutex<HashMap<OwnedRoomId, u64>>> =
        Arc::new(tokio::sync::Mutex::new(HashMap::new()));
    let sync = client.sync_with_callback(settings, move |response| {
        let client = callback_client.clone();
        let events = Arc::clone(&callback_events);
        let first_response = Arc::clone(&callback_first);
        let watched = Arc::clone(&callback_watched);
        let stamps = Arc::clone(&callback_stamps);
        async move {
            {
                let mut stamps = stamps.lock().await;
                for (room_id, update) in &response.rooms.joined {
                    let mut newest = 0u64;
                    for event in &update.timeline.events {
                        if let Ok(Some(ts)) = event
                            .raw()
                            .get_field::<UInt>("origin_server_ts")
                        {
                            newest = newest.max(u64::from(ts));
                        }
                    }
                    if newest > 0 {
                        let entry = stamps.entry(room_id.clone()).or_default();
                        *entry = (*entry).max(newest);
                    }
                }
            }
            let latest_events = client.latest_events().await;
            {
                let started = std::time::Instant::now();
                let mut watched = watched.lock().await;
                let stamps = stamps.lock().await;
                let joined: Vec<Room> = client
                    .rooms()
                    .into_iter()
                    .filter(|room| matches!(room.state(), matrix_sdk::RoomState::Joined))
                    .collect();

                // The cap is one pool across the whole account, and a
                // unified inbox spans many bridged networks of very unequal
                // volume — allocated by raw recency alone, one firehose
                // network starves the quiet ones of preview slots entirely.
                // So the DESIRED watch set is a round-robin across coarse
                // per-network buckets, most recent first within each: every
                // network keeps previews for its own most active rooms, and
                // spare capacity flows to the busy ones.
                let mut buckets: HashMap<String, Vec<(&Room, u64)>> =
                    HashMap::new();
                for room in &joined {
                    let stamp =
                        stamps.get(room.room_id()).copied().unwrap_or(0);
                    buckets
                        .entry(preview_bucket(room))
                        .or_default()
                        .push((room, stamp));
                }
                for rooms in buckets.values_mut() {
                    rooms.sort_by(|a, b| b.1.cmp(&a.1));
                }
                let mut bucket_keys: Vec<&String> = buckets.keys().collect();
                bucket_keys.sort();

                let mut desired: BTreeSet<OwnedRoomId> = BTreeSet::new();
                let mut depth = 0usize;
                'fill: loop {
                    let mut any = false;
                    for key in &bucket_keys {
                        if desired.len() >= LATEST_EVENT_WATCH_CAP {
                            break 'fill;
                        }
                        if let Some((room, _)) =
                            buckets.get(*key).and_then(|rooms| rooms.get(depth))
                        {
                            desired.insert(room.room_id().to_owned());
                            any = true;
                        }
                    }
                    if !any {
                        break;
                    }
                    depth += 1;
                }

                // Reconcile, don't accumulate: the watched set follows the
                // desired set as stamps move, so the cap can never freeze
                // the first minute's choice, and a bucket's slots return to
                // the pool when its rooms go quiet.
                let stale: Vec<OwnedRoomId> =
                    watched.difference(&desired).cloned().collect();
                let forgot = stale.len();
                for room_id in stale {
                    latest_events.forget_room(&room_id).await;
                    watched.remove(&room_id);
                }
                let mut added = 0usize;
                for room_id in &desired {
                    if !watched.contains(room_id) {
                        watch_latest_event(&latest_events, &mut watched, room_id)
                            .await;
                        added += 1;
                    }
                }

                // Counts and timing only — instrumentation for tuning the
                // cap against a real account, never identifiers.
                enqueue(&events, json!({
                    "type": "latest_event_watch_report",
                    "elapsed_ms": started.elapsed().as_millis() as u64,
                    "watched": watched.len(),
                    "buckets": bucket_keys.len(),
                    "rooms": joined.len(),
                    "forgot": forgot,
                    "added": added,
                }));
            }
            {
                let stamps = stamps.lock().await;
                enqueue_rooms_stamped(&events, &client, Some(&stamps)).await;
            }
            let spaces = SpaceService::new(client.clone()).await;
            enqueue_spaces(&events, &spaces, &client).await;
            enqueue(&events, json!({ "type": "room_list_sync_state", "state": "running" }));
            if first_response.swap(false, Ordering::SeqCst) {
                enqueue(&events, json!({ "type": "initial_sync_done" }));
            }
            LoopCtrl::Continue
        }
    });
    // A WATCHDOG ON THE FIRST RESPONSE, because a sync that dies without
    // returning looks exactly like one that is merely slow.
    //
    // Reported as issue #2: against a server that takes this fallback, the
    // classic sync logged "starting" and then did nothing for 13+ minutes —
    // zero established TCP connections for the process, zero I/O progress,
    // ~0.4% CPU, and no sync_error, although the select! arm below promises
    // one on failure. The future was simply parked. Restarting the client
    // with the same store synced normally, so it is a wedge on a first
    // request rather than an incompatibility, and it has not reproduced on
    // demand since.
    //
    // This deliberately does NOT cancel or restart the sync. A first
    // full-state request on a large account (the report was 1028 joined
    // rooms) can legitimately take a long time, and killing a working sync
    // would be a worse defect than the one being chased. What it does is
    // make the silence VISIBLE — the next occurrence produces a line saying
    // how long it has been waiting, instead of a spinner that means nothing.
    //
    // The future never resolves: after the last escalation it parks forever,
    // so it can never be the arm that ends the select! and stops the sync.
    let watchdog_events = Arc::clone(&events);
    let watchdog_first = Arc::clone(&first_response);
    let watchdog = async move {
        watch_first_sync_response(&watchdog_events, &watchdog_first,
                                  FIRST_SYNC_STALL_STEPS).await;
        std::future::pending::<()>().await
    };
    tokio::pin!(sync);
    tokio::pin!(watchdog);
    tokio::select! {
        result = &mut sync => if let Err(err) = result {
            enqueue(&events, json!({
                "type": "sync_error",
                "message": format_matrix_error("Matrix Rust SDK sync failed", err),
            }));
        },
        _ = &mut watchdog => {},
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

/// One Space's DIRECT children, in the order its own `m.space.child` state
/// declares.
///
/// This exists because the payload's `descendants` list is TRANSITIVE — a
/// subspace's rooms are flattened into every ancestor — which is right for
/// "everything in this Space" and wrong for anything that has to show the
/// structure the Space's admin built. Lightning read `descendants` as though
/// it were the direct children, so a channel list on this backend listed
/// every room in the tree under the top-level Space and then again under its
/// own subspace.
///
/// Order is the spec's: `order` keys compared lexicographically first,
/// children without one last, the room id as the tiebreak in both cases —
/// the same comparator matrix-sdk-ui uses for its own space room list. An
/// empty `via` list is MSC1772 REMOVAL, not a child, and is skipped.
///
/// Reads the local state store (no network); a Space whose state has not
/// synced yet returns nothing and the caller falls back to the SDK's own
/// parent graph.
async fn direct_children_of(room: &Room) -> Vec<String> {
    let Ok(events) = room
        .get_state_events_static::<SpaceChildEventContent>()
        .await
    else {
        return Vec::new();
    };
    let mut entries: Vec<(Option<String>, String)> = Vec::new();
    for raw in events {
        let Ok(event) = raw.deserialize() else { continue };
        let (state_key, content) = match event {
            SyncOrStrippedState::Sync(SyncStateEvent::Original(original)) => {
                (original.state_key, original.content)
            }
            _ => continue,
        };
        if content.via.is_empty() {
            continue;
        }
        entries.push((
            content.order.map(|order| order.as_str().to_owned()),
            state_key.to_string(),
        ));
    }
    entries.sort_by(|a, b| match (&a.0, &b.0) {
        (Some(left), Some(right)) => left.cmp(right).then(a.1.cmp(&b.1)),
        (Some(_), None) => std::cmp::Ordering::Less,
        (None, Some(_)) => std::cmp::Ordering::Greater,
        (None, None) => a.1.cmp(&b.1),
    });
    entries.into_iter().map(|(_, id)| id).collect()
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
    // The strictly-DIRECT half, kept separate from the map below. That one is
    // deliberately widened with `filter.descendants`, which for a level-1
    // filter is every descendant recursively — right for computing the
    // transitive closure, wrong as a fallback for "this Space's own children".
    let mut direct_by_parent = HashMap::<String, BTreeSet<String>>::new();

    // Ask SpaceService's cycle-pruned graph for every known joined room's
    // parents. This extends its two presentation-level filters into a full
    // selectable hierarchy without reparsing raw state in Lightning.
    for room in client.joined_rooms() {
        let child_id = room.room_id().to_string();
        let parents: Vec<String> = service.joined_parents_of_child(room.room_id()).await
            .into_iter().map(|parent| parent.room_id.to_string()).collect();
        for parent in &parents {
            children_by_parent.entry(parent.clone()).or_default().insert(child_id.clone());
            direct_by_parent.entry(parent.clone()).or_default().insert(child_id.clone());
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
        // DIRECT children, admin order first (see direct_children_of), then
        // any link the SDK's own parent graph knows about that the state read
        // did not produce — a child whose m.space.child event has not synced
        // yet still belongs in the list. `direct_by_parent`, NOT
        // `children_by_parent`: the latter is widened with the SpaceFilter's
        // descendants, which for a level-1 filter is the whole subtree.
        let mut children = direct_children_of(room).await;
        if let Some(known) = direct_by_parent.get(&id) {
            for child in known {
                if child != &id && !children.contains(child) {
                    children.push(child.clone());
                }
            }
        }
        let child_spaces: Vec<String> = children.iter()
            .filter(|child| space_ids.contains(*child)).cloned().collect();
        let level = filters.iter().find(|filter| filter.space_room.room_id.as_str() == id)
            .map(|filter| filter.level).unwrap_or(2);
        spaces.push(json!({
            "id": id,
            "name": room_name(room).await,
            "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
            "parents": parents,
            "children": children,
            "child_spaces": child_spaces,
            "descendants": descendants,
            "level": level,
        }));
    }
    enqueue(events, json!({ "type": "space_list_reset", "spaces": spaces }));
}

/// The room-list ordering stamp, in milliseconds.
///
/// Deliberately the SDK's LatestEvent — the message-like event the room-list
/// preview is built from — and only then `Room::latest_event_timestamp()` as a
/// fallback.
///
/// `latest_event_timestamp()` is the newest event of ANY kind. Opening a room
/// loads its timeline, which brings state events (a member joining or leaving)
/// into that answer, so the room jumps up the list; unloading the timeline
/// takes them back out and it drops again. That is the reported "clicking an
/// older room moves it upwards, then it drops back down to where it was", and
/// the tester's own guess — "hidden room updates, like users leaving or
/// joining" — was right. A room's position must follow what was SAID in it.
/// The room list's sort key: when a real CONVERSATION last happened here.
///
/// `LatestEventValue` is the SDK's room-list preview value, so every variant
/// of it is already message-like — that is the whole reason it exists, and it
/// is what makes this a conversation timestamp rather than an "anything
/// happened" one.
///
/// Two things this deliberately does NOT do. It does not match only `Remote`:
/// the three `Local*` variants are the user's OWN message on its way out, and
/// dropping through on those meant a room you had just spoken in did not rise
/// until the echo came back from the server. And it does not reach for
/// `latest_event_timestamp()` except as a last resort, because that one is the
/// latest event of ANY KIND — a membership change, a topic edit, a room whose
/// timeline was loaded and brought state events into view. Ordering a
/// conversation list by that makes rooms jump for things nobody said.
fn room_ordering_timestamp_ms(
    latest: &matrix_sdk_base::latest_event::LatestEventValue,
    room: &Room,
) -> u64 {
    if let Some(ts) = latest.timestamp() {
        return u64::from(ts.get());
    }
    room.latest_event_timestamp().map(|ts| u64::from(ts.get())).unwrap_or(0)
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

    // Read once: the preview text and the ordering stamp must describe the
    // SAME event, and `latest_event()` is not free.
    let latest_event = room.latest_event();

    json!({
        "id": room.room_id().to_string(),
        "membership": membership,
        "name": room_name(room).await,
        "canonical_alias": room.canonical_alias().map(|alias| alias.to_string()).unwrap_or_default(),
        "topic": room.topic().unwrap_or_default(),
        "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
        "last_message_preview": latest_event_preview_text(&latest_event),
        "last_activity_ms": room_ordering_timestamp_ms(&latest_event, room),
        "unread_count": room.num_unread_notifications().max(notifications.notification_count),
        "highlight_count": room.num_unread_mentions().max(notifications.highlight_count),
        "marked_unread": room.is_marked_unread(),
        // Element-parity favourites. The `m.favourite` room tag IS the
        // storage — Lightning invents no list of its own, so a favourite set
        // from Element or Element X is already true here. Read from the
        // SDK's own notable-tag bit on RoomInfo (kept current by the sync
        // loop's room-account-data handling), never by parsing account data.
        "is_favourite": room.is_favourite(),
        "has_unread_messages": room.num_unread_messages() > 0,
        "encrypted": room.encryption_state().is_encrypted(),
        // v0.7.x (review H1): the SDK's EncryptionState is a TRI-state and
        // Unknown must not flatten into "not encrypted" — draft persistence
        // and server-search offers fail closed on it. False until the
        // m.room.encryption state has actually synced for this room.
        "encryption_known": !room.encryption_state().is_unknown(),
        "is_space": room.is_space(),
        "is_direct": !direct_targets.is_empty(),
        "direct_user_id": direct_targets.first().cloned().unwrap_or_default(),
        "direct_user_ids": direct_targets,
        "member_count": room.joined_members_count(),
        "room_type": room.room_type().map(|kind| kind.to_string()).unwrap_or_default(),
        "prev_batch": room.last_prev_batch().unwrap_or_default(),
        "inviter_user_id": inviter_user_id,
        "inviter_display_name": inviter_display_name,
        // v0.7.x room upgrades. Both come from the SDK's own typed
        // accessors, which parse through ruma into an OwnedRoomId — that
        // IS the "treat replacement_room as a real room id" validation, and
        // it is why nothing here hand-parses m.room.tombstone or
        // m.room.create. Empty means "not upgraded" / "no predecessor".
        //
        // There is deliberately no separate is_tombstoned flag: a non-empty
        // successor is the same fact with the successor attached, and one
        // source of truth cannot disagree with itself.
        //
        // The tombstone's `body`/`reason` deliberately does NOT cross this
        // boundary. It is free text chosen by whoever sent the state event,
        // destined for a banner the user is invited to CLICK; Lightning
        // shows its own fixed wording instead. Do not add it "for
        // information".
        "successor_room_id": room
            .successor_room()
            .map(|successor| successor.room_id.to_string())
            .unwrap_or_default(),
        "predecessor_room_id": room
            .predecessor_room()
            .map(|predecessor| predecessor.room_id.to_string())
            .unwrap_or_default(),
    })
}

async fn enqueue_rooms(events: &Arc<Mutex<VecDeque<String>>>, client: &Client) {
    enqueue_rooms_stamped(events, client, None).await;
}

/// `stamps`: response-harvested recency (classic sync only — see
/// run_classic_sync). It overrides a payload's last_activity_ms when it
/// knows a NEWER time; a payload whose own stamp is fresher (a live latest
/// event) always wins, so the two sources can only improve on each other.
async fn enqueue_rooms_stamped(
    events: &Arc<Mutex<VecDeque<String>>>,
    client: &Client,
    stamps: Option<&HashMap<OwnedRoomId, u64>>,
) {
    let mut out = Vec::new();
    for room in client.rooms() {
        if matches!(room.state(), matrix_sdk::RoomState::Joined
            | matrix_sdk::RoomState::Invited | matrix_sdk::RoomState::Knocked) {
            let mut payload = room_payload(&room).await;
            if let Some(stamps) = stamps {
                if let Some(&stamp) = stamps.get(room.room_id()) {
                    let existing = payload["last_activity_ms"]
                        .as_u64()
                        .unwrap_or(0);
                    if stamp > existing {
                        payload["last_activity_ms"] = json!(stamp);
                    }
                }
            }
            out.push(payload);
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

/// Coarse per-network grouping for preview-slot FAIRNESS only. A DM whose
/// partner localpart reads "<prefix>_<rest>" buckets by that prefix;
/// everything else — native rooms, groups, portal rooms — shares one
/// bucket. Deliberately NOT the C++ BridgeNetwork table: a wrong bucket
/// here (a human called `@alice_b:…`) costs a slightly different fairness
/// split and nothing else, so duplicating the known-network list across
/// the FFI for it would buy divergence risk and no correctness.
fn preview_bucket(room: &Room) -> String {
    let targets = room.direct_targets();
    if targets.len() == 1 {
        if let Some(target) = targets.iter().next() {
            let s = target.as_str();
            let local = s.strip_prefix('@').unwrap_or(s);
            let local = local.split(':').next().unwrap_or(local);
            let local = local.strip_prefix('_').unwrap_or(local);
            if let Some(i) = local.find('_') {
                if i > 0 {
                    return local[..i].to_ascii_lowercase();
                }
            }
        }
    }
    "-".to_owned()
}

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

    // A 1:1 DM is named after the person, not the membership arithmetic.
    // The SDK's hero algorithm counts every joined member, and a bridged DM
    // (Beeper et al.) always carries at least the ghost AND the bridge bot —
    // so an unnamed Signal chat with one human rendered as "Sim, and 2
    // others". When m.direct names exactly one partner and their member
    // profile is in the store, that profile name IS the room name. A missing
    // profile falls through to the SDK algorithm unchanged.
    let direct_targets = room.direct_targets();
    if direct_targets.len() == 1 {
        if let Some(target) = direct_targets.iter().next() {
            if let Ok(user_id) = <&UserId>::try_from(target.as_str()) {
                if let Ok(Some(member)) = room.get_member_no_sync(user_id).await {
                    if let Some(name) = member.display_name() {
                        if !name.is_empty() {
                            return name.to_owned();
                        }
                    }
                }
            }
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

/// Wall-clock millis for sync-latency tracing, or None when it is off.
///
/// Opt-in through LIGHTNING_SYNC_TRACE, the same switch the C++ tracer reads
/// (src/app/SyncLatencyTracer.*), so one variable turns on the whole
/// sdk -> bridge -> model -> ui picture rather than two halves that can
/// disagree about whether they are recording.
///
/// Wall clock rather than Instant on purpose: this value is compared against
/// QDateTime::currentMSecsSinceEpoch() on the other side of the FFI, and a
/// monotonic clock has no shared origin there. Read once; disabled it is a
/// relaxed atomic load and nothing else.
pub(crate) fn sync_trace_stamp_ms() -> Option<u64> {
    use std::sync::OnceLock;
    static ENABLED: OnceLock<bool> = OnceLock::new();
    let on = *ENABLED.get_or_init(|| {
        std::env::var("LIGHTNING_SYNC_TRACE")
            .map(|v| {
                let v = v.trim().to_ascii_lowercase();
                !(v.is_empty() || v == "0" || v == "false" || v == "off" || v == "no")
            })
            .unwrap_or(false)
    });
    if !on {
        return None;
    }
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()
        .map(|d| d.as_millis() as u64)
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

/// v0.7 video round: copy an optional send-side poster out of C++ memory.
///
/// Absent (null pointer / zero length) and oversized posters both yield
/// `None` — the video then sends without one rather than failing. The upper
/// bound is deliberately generous relative to the ~640px JPEG the extractor
/// produces and exists only so a bad length can never allocate without
/// limit; `PosterBytes` applies the real (tighter) policy plus magic
/// validation. C++ frees its buffer as soon as this call returns, so the
/// copy is mandatory.
///
/// # Safety
/// `data` must either be null or point to at least `len` readable bytes.
unsafe fn poster_arg(
    data: *const u8,
    len: usize,
    width: u64,
    height: u64,
) -> Option<rooms::PosterBytes> {
    const MAX_FFI_POSTER_BYTES: usize = 8 * 1024 * 1024;
    if data.is_null() || len == 0 || len > MAX_FFI_POSTER_BYTES {
        return None;
    }
    Some(rooms::PosterBytes {
        data: unsafe { std::slice::from_raw_parts(data, len) }.to_vec(),
        width,
        height,
    })
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

    // ── Read-receipt privacy ────────────────────────────────────────────
    //
    // Three modes, one helper, two call sites. What is asserted here is
    // exactly what a member of the room can and cannot observe, plus the
    // invariant that ties the three together: the FULLY-READ MARKER is sent
    // in every mode, because it is the user's own place in the conversation
    // and no privacy setting should cost them that.
    #[test]
    fn receipt_privacy_decides_what_other_members_can_see() {
        use super::receipts_for_mode;
        use matrix_sdk::ruma::EventId;

        let id = EventId::parse("$abc:example.org").unwrap();

        // 0 — public, exactly what every release before 0.9.0 sent.
        let public = receipts_for_mode(id.clone(), 0);
        assert_eq!(public.public_read_receipt.as_deref(), Some(&*id));
        assert!(public.private_read_receipt.is_none());
        assert_eq!(public.fully_read.as_deref(), Some(&*id));

        // 1 — private. The server records it, so THIS account's other
        // devices still clear their badge; no other member ever sees it.
        // The distinction from mode 2 is the entire reason it exists.
        let private = receipts_for_mode(id.clone(), 1);
        assert!(
            private.public_read_receipt.is_none(),
            "a private receipt must never also be published"
        );
        assert_eq!(private.private_read_receipt.as_deref(), Some(&*id));
        assert_eq!(private.fully_read.as_deref(), Some(&*id));

        // 2 — off. No RECEIPT at all: no other member is told, and this
        // account's other devices get none either, so their unread badges
        // stop clearing. The fully-read marker still goes — it is account
        // data and it is what keeps the user's own place — so "your devices
        // learn nothing" would be too strong. What they lose is the receipt,
        // not the read position, and the assertion below says exactly that.
        let off = receipts_for_mode(id.clone(), 2);
        assert!(off.public_read_receipt.is_none());
        assert!(
            off.private_read_receipt.is_none(),
            "\"do not send\" must not quietly send a private one instead"
        );
        assert_eq!(
            off.fully_read.as_deref(),
            Some(&*id),
            "the user's own read position is not a disclosure and must \
             survive every mode"
        );

        // An out-of-range value must not silently mean "off": a corrupt
        // stored setting stopping someone's receipts is a behaviour change
        // they never asked for. It falls back to the previous behaviour.
        let bogus = receipts_for_mode(id.clone(), 77);
        assert_eq!(bogus.public_read_receipt.as_deref(), Some(&*id));
    }

    // A CANCELLED THREAD THAT WILL NOT STOP MUST NOT HOLD THE UI.
    //
    // This is the account-switch freeze, captured on a real desktop with
    // LIGHTNING_GUI_STALL_TRACE=1: `GUI stall 45618 ms` on one switch, plus
    // 4943 ms and 15492 ms at startup. stop_sync_and_wait joined both of
    // these threads with no budget, and each drives a current_thread runtime
    // whose drop waits for any spawn_blocking already started — DNS
    // resolution included. A slow resolver parked the GUI thread for as long
    // as it took.
    //
    // On the unfixed code this test hangs forever, which is the point.
    #[test]
    fn a_task_that_will_not_stop_is_detached_rather_than_waited_for() {
        use super::{join_task_within_budget, SyncTask, SYNC_TASK_JOIN_BUDGET_MS};
        let (park_tx, park_rx) = std::sync::mpsc::channel::<()>();
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
            // Never returns until the test lets it, standing in for a runtime
            // drop blocked on getaddrinfo.
            let _ = park_rx.recv();
        });
        let (cancel, _cancel_rx) = tokio::sync::oneshot::channel::<()>();
        let mut task = SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
            done: Some(done_rx),
        };

        let started = std::time::Instant::now();
        let joined = join_task_within_budget(&mut task, "test");
        let waited = started.elapsed();

        assert!(!joined, "a parked thread must report as detached, not joined");
        // Bounded by the budget, with generous slack for a loaded CI box.
        assert!(
            waited < std::time::Duration::from_millis(SYNC_TASK_JOIN_BUDGET_MS + 2000),
            "waited {waited:?}, which is past the budget"
        );
        // Let the parked thread go so the test process can exit cleanly.
        let _ = park_tx.send(());
    }

    // The ordinary case still joins, and does so promptly: a task that has
    // already finished must not spend any of the budget.
    #[test]
    fn a_task_that_has_already_stopped_is_joined_immediately() {
        use super::{join_task_within_budget, SyncTask};
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
        });
        let (cancel, _cancel_rx) = tokio::sync::oneshot::channel::<()>();
        let mut task = SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
            done: Some(done_rx),
        };
        let started = std::time::Instant::now();
        assert!(join_task_within_budget(&mut task, "test"));
        assert!(started.elapsed() < std::time::Duration::from_millis(1000));
    }
    use super::classify_import_error;

    // ── The account-switch SIGABRT (2026-08-25) ──────────────────────────
    //
    // `shutdown_managed_tasks` used to join its task handles under a budget
    // and, if the budget elapsed, build a SECOND `join_all` over the SAME
    // handles. `JoinHandle::poll` CONSUMES the task's output — tokio's
    // `Core::take_output` swaps `Stage::Finished` for `Stage::Consumed` — and
    // the first `JoinAll`, the only record of which handles had already
    // completed, was dropped by the `timeout`. So every task that finished
    // inside the budget was polled again and hit
    // `panic!("JoinHandle polled after completion")`. `rust/Cargo.toml` sets
    // `panic = "abort"` in BOTH profiles, so that is an immediate SIGABRT of
    // the whole process: no unwind, nothing for `ffi_string`'s `catch_unwind`
    // to catch, and no line in any log.
    //
    // Cargo deliberately IGNORES the `panic` setting for the test profile, so
    // `#[should_panic]` still works here. `panicIsCatchableInTheTestProfile`
    // asserts exactly that, because every case below rests on it.
    //
    // Reported as "lighting crashed once when switching accounts", and ONCE is
    // the whole shape of it: it needs something to MISS the budget (or round
    // two never runs) AND something else to have FINISHED inside it (or round
    // two polls only pending handles and is harmless).

    #[test]
    #[should_panic(expected = "deliberate")]
    fn panic_is_catchable_in_the_test_profile() {
        // If this ever fails, every should_panic case below is silently
        // vacuous and the crash coverage is decoration.
        panic!("deliberate");
    }

    /// The retired two-round shape, reproduced exactly so the test can fail
    /// on it. Nothing in production calls this.
    async fn join_or_abort_the_old_broken_way(
        mut handles: Vec<tokio::task::JoinHandle<()>>,
        budget_ms: u64,
    ) {
        let all = futures_util::future::join_all(handles.iter_mut());
        if tokio::time::timeout(std::time::Duration::from_millis(budget_ms), all)
            .await
            .is_err()
        {
            for handle in &handles {
                handle.abort();
            }
            // The second join_all over the SAME handles. This is the defect.
            let _ = tokio::time::timeout(
                std::time::Duration::from_millis(200),
                futures_util::future::join_all(handles.iter_mut()),
            )
            .await;
        }
    }

    #[test]
    #[should_panic(expected = "polled after completion")]
    fn the_old_two_round_join_panics_when_one_task_beat_the_budget() {
        // ONE task finishes immediately, ONE outlives the budget. That is the
        // exact mix an account switch produces: `room_action_tasks` is a
        // single pool fed by dozens of spawn sites, so what is in flight
        // differs every time.
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        rt.block_on(async {
            let quick = tokio::spawn(async {});
            let slow = tokio::spawn(async {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
            });
            join_or_abort_the_old_broken_way(vec![quick, slow], 200).await;
        });
    }

    #[test]
    fn join_or_abort_survives_the_same_mix_and_reports_the_miss() {
        // ON THE UNFIXED TREE this scenario aborts the process rather than
        // failing, which is why the case above exists to name the panic.
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        let missed = rt.block_on(async {
            let quick = tokio::spawn(async {});
            let slow = tokio::spawn(async {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
            });
            // 0 seconds so the budget is already spent: the slow task cannot
            // possibly make it, and the quick one has.
            super::RustClient::join_or_abort(vec![quick, slow], 0).await
        });
        assert_eq!(missed, 1, "the task that outlived the budget was not counted");
    }

    #[test]
    fn join_or_abort_reports_no_miss_when_everything_finishes() {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        let missed = rt.block_on(async {
            let a = tokio::spawn(async {});
            let b = tokio::spawn(async {});
            super::RustClient::join_or_abort(vec![a, b], 30).await
        });
        assert_eq!(missed, 0);
    }

    #[test]
    fn join_or_abort_is_a_no_op_with_no_handles() {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        assert_eq!(rt.block_on(super::RustClient::join_or_abort(vec![], 0)), 0);
    }

    // THE TWO ORDERINGS THAT NEVER PANICKED, and why the crash was rare.
    // BOTH OF THESE PASS ON THE BROKEN CODE — they are documentation of the
    // window, not coverage of the defect. Do not read a green run of these as
    // evidence that the two-round shape is safe.
    #[test]
    fn the_old_shape_is_harmless_when_nothing_misses_the_budget() {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        rt.block_on(async {
            let a = tokio::spawn(async {});
            let b = tokio::spawn(async {});
            // Round two never runs, so nothing is re-polled.
            join_or_abort_the_old_broken_way(vec![a, b], 5_000).await;
        });
    }

    #[test]
    fn the_old_shape_is_harmless_when_nothing_finishes_inside_the_budget() {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        rt.block_on(async {
            let a = tokio::spawn(async {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
            });
            let b = tokio::spawn(async {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
            });
            // Round two polls only handles that were still pending, which is
            // legal.
            join_or_abort_the_old_broken_way(vec![a, b], 50).await;
        });
    }

    // The HTTP user agent is the one string third parties see. It must be a
    // well-formed `Lightning/X.Y.Z` derived from the crate version, never a
    // hand-maintained literal that can drift from the released version.
    #[test]
    fn user_agent_is_derived_from_the_crate_version() {
        let ua = super::USER_AGENT;
        let version = ua
            .strip_prefix("Lightning/")
            .expect("user agent must be Lightning/<version>");
        assert_eq!(version, env!("CARGO_PKG_VERSION"));
        let fields: Vec<&str> = version.split('.').collect();
        assert_eq!(fields.len(), 3, "version must be X.Y.Z: {version}");
        for field in fields {
            assert!(
                !field.is_empty() && field.chars().all(|c| c.is_ascii_digit()),
                "version field is not numeric: {version}"
            );
        }
    }

    // Server-synchronized per-room notification modes: the FFI integers are
    // a stable contract with C++ (SettingsManager cache values and
    // NotificationManager::RoomMode). The mapping must stay label-faithful
    // in both directions and reject anything outside 0..=2.
    #[test]
    fn notification_mode_ints_round_trip_label_faithfully() {
        use matrix_sdk::notification_settings::RoomNotificationMode;
        for (int, mode) in [
            (0, RoomNotificationMode::AllMessages),
            (1, RoomNotificationMode::MentionsAndKeywordsOnly),
            (2, RoomNotificationMode::Mute),
        ] {
            assert_eq!(super::notification_mode_from_int(int), Some(mode));
            assert_eq!(super::notification_mode_to_int(mode), int as u8);
        }
    }

    #[test]
    fn out_of_range_notification_modes_are_rejected() {
        assert_eq!(super::notification_mode_from_int(-1), None);
        assert_eq!(super::notification_mode_from_int(3), None);
        assert_eq!(super::notification_mode_from_int(i32::MAX), None);
    }

    // The coalescing marker discipline for notification-mode writes: a
    // superseded task must neither write nor report (before OR after its
    // round-trip), the winning task consumes the room's marker exactly
    // once when it reports, and the read path sees "pending" only while a
    // write is genuinely unreported.
    #[test]
    fn notification_targets_supersede_consume_and_pend_correctly() {
        use std::collections::HashMap;
        use std::sync::{Arc, Mutex};
        let targets: Arc<Mutex<HashMap<String, u8>>> =
            Arc::new(Mutex::new(HashMap::new()));
        let room = "!room:example.org";

        // set(2) queued: marker present, latest, read path pends.
        targets.lock().unwrap().insert(room.to_owned(), 2);
        assert!(super::is_latest_notification_target(&targets, room, 2));
        assert!(super::notification_write_pending(&targets, room));

        // set(1) arrives while set(2)'s write is in flight: set(2) is no
        // longer latest — it must skip its report and leave the marker.
        targets.lock().unwrap().insert(room.to_owned(), 1);
        assert!(!super::is_latest_notification_target(&targets, room, 2));
        assert!(!super::take_notification_target_if_latest(&targets, room, 2));
        assert!(super::notification_write_pending(&targets, room));

        // set(1)'s task reports: it is latest, consumes the marker once.
        assert!(super::take_notification_target_if_latest(&targets, room, 1));
        assert!(!super::notification_write_pending(&targets, room));
        // A second consume attempt (double report) finds nothing.
        assert!(!super::take_notification_target_if_latest(&targets, room, 1));

        // Duplicate queued sets of the SAME mode: the first reporter
        // consumes the marker; the second skips silently.
        targets.lock().unwrap().insert(room.to_owned(), 0);
        assert!(super::take_notification_target_if_latest(&targets, room, 0));
        assert!(!super::is_latest_notification_target(&targets, room, 0));

        // Other rooms are independent.
        targets.lock().unwrap().insert("!other:example.org".to_owned(), 2);
        assert!(!super::notification_write_pending(&targets, room));
        assert!(super::notification_write_pending(&targets, "!other:example.org"));
    }

    #[test]
    fn notification_target_guard_clears_only_its_own_marker() {
        use std::collections::HashMap;
        use std::sync::{Arc, Mutex};
        let targets: Arc<Mutex<HashMap<String, u8>>> =
            Arc::new(Mutex::new(HashMap::new()));
        let room = "!room:example.org";

        // Orphan path: the marker survives to guard drop (task panicked,
        // was aborted, or its future was never polled) — the guard clears
        // it so reads for the room are not disabled for the session.
        targets.lock().unwrap().insert(room.to_owned(), 2);
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 2,
        });
        assert!(!super::notification_write_pending(&targets, room));

        // Superseded path: a newer task's marker is NEVER touched.
        targets.lock().unwrap().insert(room.to_owned(), 1);
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 2,
        });
        assert!(super::is_latest_notification_target(&targets, room, 1));

        // Normal path: the report consume got there first; guard no-ops.
        assert!(super::take_notification_target_if_latest(&targets, room, 1));
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 1,
        });
        assert!(!super::notification_write_pending(&targets, room));
    }

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
        // The SAS and show-QR slots are structurally identical (both are
        // keyed by flow id because neither SDK handle exposes one), so one
        // fake covers both halves of the single-flow policy.
        type MethodSlot = Arc<Mutex<Option<(String, FakeFlow)>>>;

        fn slots() -> (RequestSlot, MethodSlot, MethodSlot) {
            (
                Arc::new(Mutex::new(None)),
                Arc::new(Mutex::new(None)),
                Arc::new(Mutex::new(None)),
            )
        }

        fn request_flow(slot: &RequestSlot) -> Option<String> {
            slot.lock().unwrap().as_ref().map(|f| f.flow_id.clone())
        }

        fn sas_flow(slot: &MethodSlot) -> Option<String> {
            slot.lock().unwrap().as_ref().map(|(id, _)| id.clone())
        }

        #[test]
        fn empty_slots_are_not_live() {
            let (request, sas, qr) = slots();
            assert!(!crate::flow_slots_are_live(&request, &sas, &qr));
        }

        #[test]
        fn a_live_request_blocks_a_second_start() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-a"));
            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            // Still there: a live flow must not be silently evicted.
            assert_eq!(request_flow(&request).as_deref(), Some("flow-a"));
        }

        // The brick: an incoming request occupies the slot with no user
        // action at all, and nothing released it when the flow died. A
        // presence-only gate then refused every later attempt for the rest
        // of the process lifetime.
        #[test]
        fn a_finished_occupant_is_cleared_and_does_not_block() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::finished("flow-dead"));
            *sas.lock().unwrap() =
                Some(("flow-dead".to_owned(), FakeFlow::finished("flow-dead")));
            *qr.lock().unwrap() =
                Some(("flow-dead".to_owned(), FakeFlow::finished("flow-dead")));

            assert!(!crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
            assert_eq!(sas_flow(&qr), None);
        }

        #[test]
        fn a_live_sas_alone_still_counts_as_live() {
            let (request, sas, qr) = slots();
            *sas.lock().unwrap() =
                Some(("flow-b".to_owned(), FakeFlow::live("flow-b")));
            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-b"));
        }

        // A QR code on screen is a live flow: the peer may be pointing a
        // camera at it. A second start must be refused exactly as it is for
        // a live SAS, or showing a code would silently orphan itself.
        #[test]
        fn a_live_qr_alone_still_counts_as_live() {
            let (request, sas, qr) = slots();
            *qr.lock().unwrap() =
                Some(("flow-qr".to_owned(), FakeFlow::live("flow-qr")));
            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(sas_flow(&qr).as_deref(), Some("flow-qr"));
        }

        // The sweep that clears dead occupants must run for BOTH method
        // slots. Short-circuiting on a live SAS would leave a dead QR parked
        // forever — the same sticky-slot brick, one slot over.
        #[test]
        fn a_dead_qr_is_swept_even_while_a_sas_is_live() {
            let (request, sas, qr) = slots();
            *sas.lock().unwrap() =
                Some(("flow-live".to_owned(), FakeFlow::live("flow-live")));
            *qr.lock().unwrap() =
                Some(("flow-dead".to_owned(), FakeFlow::finished("flow-dead")));

            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(sas_flow(&qr), None);
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-live"));
        }

        #[test]
        fn releasing_clears_only_the_owning_flow() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-mine"));
            *sas.lock().unwrap() =
                Some(("flow-mine".to_owned(), FakeFlow::live("flow-mine")));
            *qr.lock().unwrap() =
                Some(("flow-mine".to_owned(), FakeFlow::live("flow-mine")));

            crate::release_flow_slots(&request, &sas, &qr, "flow-mine");
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
            assert_eq!(sas_flow(&qr), None);
        }

        // The clobber: a terminating driver used to run `*g = None`
        // unconditionally, wiping whichever flow happened to occupy the
        // slot — including a request that arrived after it. Accept then
        // failed with "no active verification request".
        #[test]
        fn releasing_never_evicts_a_newer_flow() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-new"));
            *sas.lock().unwrap() =
                Some(("flow-new".to_owned(), FakeFlow::live("flow-new")));
            *qr.lock().unwrap() =
                Some(("flow-new".to_owned(), FakeFlow::live("flow-new")));

            // The OLD flow terminates and releases.
            crate::release_flow_slots(&request, &sas, &qr, "flow-old");

            assert_eq!(request_flow(&request).as_deref(), Some("flow-new"));
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-new"));
            assert_eq!(sas_flow(&qr).as_deref(), Some("flow-new"));
        }

        // The QR-to-SAS hand-off: when the peer answers a displayed code
        // with an SAS start, `drive_ready_request` retires ONLY the QR slot
        // and lets the SAS driver take the same request. Releasing more than
        // that would pull the request out from under the driver about to
        // use it.
        #[test]
        fn retiring_a_qr_leaves_the_request_for_the_sas_driver() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-fallback"));
            *qr.lock().unwrap() =
                Some(("flow-fallback".to_owned(), FakeFlow::live("flow-fallback")));

            crate::release_keyed_slot(&qr, "flow-fallback");

            assert_eq!(sas_flow(&qr), None);
            assert_eq!(request_flow(&request).as_deref(), Some("flow-fallback"));
            assert_eq!(sas_flow(&sas), None);
        }

        #[test]
        fn the_guard_releases_on_a_normal_exit() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-guarded"));
            *qr.lock().unwrap() =
                Some(("flow-guarded".to_owned(), FakeFlow::live("flow-guarded")));
            {
                let _guard = FlowSlotGuard::new(
                    Arc::clone(&request),
                    Arc::clone(&sas),
                    Arc::clone(&qr),
                    "flow-guarded".to_owned(),
                );
            }
            assert_eq!(request_flow(&request), None);
            // A QR displayed when the driver exits must not stay parked
            // either, or it would refuse every later attempt.
            assert_eq!(sas_flow(&qr), None);
        }

        // A driver that panicked used to leak both slots: `run_async`
        // reported the panic and nothing cleaned up, so a request parked in
        // Ready blocked verification until the app restarted.
        #[test]
        fn the_guard_releases_on_a_panic() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-panic"));
            *qr.lock().unwrap() =
                Some(("flow-panic".to_owned(), FakeFlow::live("flow-panic")));
            let req = Arc::clone(&request);
            let sasc = Arc::clone(&sas);
            let qrc = Arc::clone(&qr);

            let result = std::panic::catch_unwind(move || {
                let _guard =
                    FlowSlotGuard::new(req, sasc, qrc, "flow-panic".to_owned());
                panic!("driver blew up");
            });

            assert!(result.is_err());
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&qr), None);
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
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-teardown"));
            *sas.lock().unwrap() =
                Some(("flow-teardown".to_owned(), FakeFlow::live("flow-teardown")));

            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);

            // Handed to the caller so it still has something to cancel...
            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            assert_eq!(
                taken_sas.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            assert!(taken_qr.is_none());
            // ...and the slots are empty, so nothing can act on it again.
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
        }

        // Sign-out while a code is on screen. The QR handle is the only
        // thing that can send that peer a cancel, so teardown has to take it
        // rather than clear it — otherwise the peer waits out the SDK's
        // 10-minute timeout staring at a scanner.
        #[test]
        fn teardown_takes_a_displayed_qr_so_the_peer_can_be_told() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-qr-teardown"));
            *qr.lock().unwrap() = Some((
                "flow-qr-teardown".to_owned(),
                FakeFlow::live("flow-qr-teardown"),
            ));

            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);

            assert!(taken_sas.is_none());
            assert_eq!(
                taken_qr.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-qr-teardown")
            );
            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-qr-teardown")
            );
            assert_eq!(sas_flow(&qr), None);
            assert_eq!(request_flow(&request), None);
        }

        #[test]
        fn teardown_has_nothing_to_cancel_when_no_flow_is_parked() {
            let (request, sas, qr) = slots();
            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);
            assert!(taken_sas.is_none());
            assert!(taken_qr.is_none());
            assert!(taken_request.is_none());
        }

        // An incoming request the user never answered has no driver at all,
        // so the slot sweep is the only thing that can tell that peer we are
        // gone.
        #[test]
        fn teardown_takes_a_request_that_never_had_a_driver() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-unanswered"));

            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);

            assert!(taken_sas.is_none());
            assert!(taken_qr.is_none());
            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-unanswered")
            );
            assert_eq!(request_flow(&request), None);
        }

        #[test]
        fn the_guard_leaves_a_newer_flow_alone_on_a_panic() {
            let (request, sas, qr) = slots();
            let req = Arc::clone(&request);
            let sasc = Arc::clone(&sas);
            let qrc = Arc::clone(&qr);

            let result = std::panic::catch_unwind(move || {
                let _guard = FlowSlotGuard::new(
                    Arc::clone(&req),
                    Arc::clone(&sasc),
                    Arc::clone(&qrc),
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

    // ── QR verification: advertisement + module packing ────────────────
    //
    // HONEST LIMITATION, identical to the SAS coverage above: matrix-sdk's
    // `VerificationRequest` and `QrVerification` have crate-private
    // constructors, so no test here can build a real one. The handshake
    // itself — generate_qr_code, the reciprocate exchange, confirm(), and
    // the QR-to-SAS transition against a real peer — is therefore NOT
    // covered by any automated test in this repository and must be
    // validated live against Element / Element X. What IS covered is
    // everything Lightning owns outright: which methods we advertise, and
    // the pure grid-to-bits transform the UI renders.
    mod qr_verification {
        use matrix_sdk::ruma::events::key::verification::VerificationMethod;

        // The security-critical half of the advertisement. Claiming
        // `m.qr_code.scan.v1` would tell a peer to display a code at a
        // client that has no camera, and the peer would then wait out
        // matrix-sdk-crypto's 10-minute VERIFICATION_TIMEOUT for a
        // reciprocate that can never come.
        #[test]
        fn we_never_advertise_a_scanner_we_do_not_have() {
            let methods = crate::advertised_verification_methods();
            assert!(!methods.contains(&VerificationMethod::QrCodeScanV1));
        }

        // Showing a code is useless without `m.reciprocate.v1`: that is the
        // method name of the `m.key.verification.start` the scanning peer
        // sends back, so advertising show without it leaves the peer no
        // legal way to answer.
        #[test]
        fn showing_a_qr_is_advertised_together_with_reciprocate() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::QrCodeShowV1));
            assert!(methods.contains(&VerificationMethod::ReciprocateV1));
        }

        // SAS must survive as the fallback for every peer that can neither
        // show nor scan. Dropping it would strand exactly the peers QR
        // cannot serve.
        #[test]
        fn sas_remains_advertised_as_the_fallback() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::SasV1));
            assert_eq!(methods.len(), 3);
        }

        // Both advertisement sites (inbound `accept_with_methods`, outbound
        // `request_verification_with_methods`) must offer the SAME set. A
        // peer that answered a request advertising one set and then saw
        // another would have no consistent view of what we can do.
        #[test]
        fn the_advertised_set_is_stable_across_calls() {
            assert_eq!(
                crate::advertised_verification_methods(),
                crate::advertised_verification_methods()
            );
        }

        fn decode(bits: &str) -> Vec<u8> {
            use base64::Engine;
            base64::engine::general_purpose::STANDARD.decode(bits).expect("base64")
        }

        // Row-major, MSB-first, one fresh byte per row. The C++ renderer
        // addresses rows at `y * stride` with `stride = (size + 7) / 8`, so
        // a packing that let rows share a byte would shear the image
        // diagonally for every size that is not a multiple of 8.
        #[test]
        fn modules_pack_row_major_msb_first_with_a_fresh_byte_per_row() {
            // 3x3: only the top-left and bottom-right modules are dark.
            let modules = vec![
                true, false, false,
                false, false, false,
                false, false, true,
            ];
            let bits = crate::pack_qr_modules(&modules, 3).expect("packed");
            let bytes = decode(&bits);
            // stride = 1 byte per row, 3 rows.
            assert_eq!(bytes.len(), 3);
            assert_eq!(bytes[0], 0b1000_0000); // x=0 dark
            assert_eq!(bytes[1], 0b0000_0000);
            assert_eq!(bytes[2], 0b0010_0000); // x=2 dark
        }

        // A row wider than one byte must start the next row on a new byte,
        // not continue mid-byte.
        #[test]
        fn a_row_wider_than_one_byte_still_starts_the_next_row_fresh() {
            let size = 9;
            let mut modules = vec![false; size * size];
            modules[0] = true;                 // row 0, x = 0
            modules[size + 8] = true;          // row 1, x = 8
            let bytes = decode(&crate::pack_qr_modules(&modules, size).expect("packed"));
            // stride = 2 bytes per row, 9 rows.
            assert_eq!(bytes.len(), 2 * 9);
            assert_eq!(bytes[0], 0b1000_0000);
            assert_eq!(bytes[1], 0);
            assert_eq!(bytes[2], 0);
            assert_eq!(bytes[3], 0b1000_0000); // row 1 byte 1, bit for x=8
        }

        #[test]
        fn an_all_dark_grid_sets_every_module_bit_and_pads_with_zeros() {
            let size = 3;
            let bytes =
                decode(&crate::pack_qr_modules(&vec![true; size * size], size).expect("packed"));
            assert_eq!(bytes.len(), 3);
            // Three dark modules, five padding bits that must stay clear so
            // the renderer does not draw a black bar past the code edge.
            for byte in bytes {
                assert_eq!(byte, 0b1110_0000);
            }
        }

        // Malformed geometry must be refused, not rendered. A short slice
        // would otherwise index out of bounds, and an absurd size would let
        // a bad grid drive an unbounded allocation.
        #[test]
        fn malformed_grids_are_refused_rather_than_rendered() {
            assert!(crate::pack_qr_modules(&[], 0).is_none());
            assert!(crate::pack_qr_modules(&[true, false], 3).is_none());
            assert!(crate::pack_qr_modules(&[true; 9], 2).is_none());
            let oversized = crate::QR_MAX_MODULES + 1;
            assert!(crate::pack_qr_modules(&vec![false; 4], oversized).is_none());
        }

        // The packed payload is pure geometry. It must round-trip back to
        // the same grid — which is also the proof that nothing else (the
        // encoded secret, the flow id, a device key) rides along in it.
        #[test]
        fn packing_round_trips_to_the_same_grid() {
            let size = 21; // the smallest real QR version.
            let modules: Vec<bool> = (0..size * size).map(|i| i % 7 == 0).collect();
            let bytes = decode(&crate::pack_qr_modules(&modules, size).expect("packed"));
            let stride = size.div_ceil(8);
            for y in 0..size {
                for x in 0..size {
                    let bit = bytes[y * stride + x / 8] & (0x80u8 >> (x % 8)) != 0;
                    assert_eq!(bit, modules[y * size + x], "module ({x},{y})");
                }
            }
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

    // ── LOCAL SEARCH, against a real homeserver ─────────────────────────
    //
    // The claim this feature exists to make is "search works in an ENCRYPTED
    // room, where server search returns nothing". Unit tests prove the index;
    // only a live run proves the path into it — login, sync, the SDK's event
    // cache holding DECRYPTED events, the sweep, and an FTS5 query coming back
    // with the right rows.
    //
    // Gated like every other live test here, and it creates nothing: it reads
    // rooms the fixture account is already in.
    #[test]
    #[ignore = "live homeserver local search; set LIGHTNING_LIVE_SEARCH=1 and credentials env"]
    fn live_local_search_finds_messages_including_in_encrypted_rooms() {
        if env_nonempty("LIGHTNING_LIVE_SEARCH").as_deref() != Some("1") {
            eprintln!("[live] gate off; skipping");
            return;
        }
        let homeserver = env_nonempty("LIGHTNING_TEST_HOMESERVER")
            .expect("LIGHTNING_TEST_HOMESERVER");
        let user = env_nonempty("LIGHTNING_TEST_USER").expect("LIGHTNING_TEST_USER");
        let password =
            env_nonempty("LIGHTNING_TEST_PASSWORD").expect("LIGHTNING_TEST_PASSWORD");
        let needle = env_nonempty("LIGHTNING_TEST_NEEDLE")
            .unwrap_or_else(|| "quarterly deployment".to_owned());

        let dir = tempfile_dir("lightning-live-search");
        let store = CString::new(dir.to_string_lossy().as_ref()).unwrap();
        let handle = unsafe { super::mx_rust_create(store.as_ptr()) };
        assert!(!handle.is_null(), "bridge");

        unsafe {
            let hs = CString::new(homeserver.clone()).unwrap();
            let u = CString::new(user.clone()).unwrap();
            let p = CString::new(password).unwrap();
            let err = take(super::mx_rust_login(
                handle, hs.as_ptr(), u.as_ptr(), p.as_ptr()));
            assert!(err.is_empty(), "login dispatch: {err}");
            wait_for(handle, "login", Duration::from_secs(60), |ev| {
                ev["type"] == "login_ok"
            });
            super::mx_rust_start_sync(handle);
            // Room ids are collected from the sync's own room events; there is
            // no FFI that enumerates them, and inventing one for a test would
            // be adding production surface to make a test convenient.
            let mut room_ids: Vec<String> = Vec::new();
            let mut seen: std::collections::BTreeSet<String> =
                std::collections::BTreeSet::new();
            let deadline = Instant::now() + Duration::from_secs(120);
            let mut running = false;
            loop {
                for ev in poll_all(handle) {
                    if let Some(t) = ev["type"].as_str() {
                        seen.insert(t.to_owned());
                    }
                    if ev["type"] == "room_list_sync_state" && ev["state"] == "running" {
                        running = true;
                    }
                    // Sliding sync delivers the room list as DIFFS, so the
                    // ids arrive across several event kinds and not only in
                    // the reset. Collecting from one of them found nothing and
                    // looked like "the account has no rooms".
                    let mut note = |id: Option<&str>| {
                        if let Some(id) = id {
                            if !id.is_empty() && !room_ids.iter().any(|k| k == id) {
                                room_ids.push(id.to_owned());
                            }
                        }
                    };
                    if let Some(list) = ev["rooms"].as_array() {
                        for room in list {
                            note(room["id"].as_str());
                        }
                    }
                    note(ev["room"]["id"].as_str());
                }
                if running && !room_ids.is_empty() {
                    break;
                }
                if Instant::now() >= deadline {
                    eprintln!("[live] saw types: {:?} running={} rooms={}",
                              seen, running, room_ids.len());
                    panic!("sync did not produce rooms");
                }
                std::thread::sleep(Duration::from_millis(400));
            }
            eprintln!("[live] sync running, {} room(s) known", room_ids.len());

            // Let the event cache actually receive some timeline before
            // sweeping: a sweep of an empty cache proves nothing and would
            // pass on a broken indexer.
            std::thread::sleep(Duration::from_secs(12));

            // Sweep, then deepen every joined room so history the sync did not
            // carry is paged in and indexed.
            let err = take(super::mx_rust_search_index_sweep(handle, 9001));
            assert!(err.is_empty(), "sweep dispatch: {err}");
            let swept = wait_for(handle, "sweep", Duration::from_secs(120), |ev| {
                ev["type"] == "search_index_swept"
            });
            eprintln!(
                "[live] swept rooms={} written={} total={}",
                swept["rooms"], swept["written"], swept["messages"]
            );

            let mut deepened = 0u64;
            for id in room_ids.iter().take(8) {
                let rid = CString::new(id.as_str()).unwrap();
                let op = 9100 + deepened;
                let err = take(super::mx_rust_search_index_deep(
                    handle, rid.as_ptr(), op));
                if !err.is_empty() {
                    continue;
                }
                let done = wait_for(
                    handle, "deep index", Duration::from_secs(180), |ev| {
                        ev["type"] == "search_index_deepened" && ev["op_id"] == op
                    });
                eprintln!(
                    "[live] deepened pages={} start={} written={}",
                    done["pages"], done["reached_start"], done["written"]
                );
                deepened += 1;
            }
            assert!(deepened > 0, "no rooms were deep-indexed");

            let stats_err = take(super::mx_rust_search_index_stats(handle, 9200));
            assert!(stats_err.is_empty(), "stats: {stats_err}");
            let stats = wait_for(handle, "stats", Duration::from_secs(30), |ev| {
                ev["type"] == "search_index_stats"
            });
            let indexed = stats["messages"].as_i64().unwrap_or(0);
            eprintln!(
                "[live] index holds {indexed} messages across {} rooms",
                stats["rooms"]
            );
            assert!(indexed > 0, "the index is empty after a sweep and a deep index");

            // THE ACTUAL CLAIM.
            let q = CString::new(needle.clone()).unwrap();
            let empty = CString::new("").unwrap();
            let err = take(super::mx_rust_local_search(
                handle, q.as_ptr(), empty.as_ptr(), 50, 0, 9300));
            assert!(err.is_empty(), "search dispatch: {err}");
            let result = wait_for(handle, "search", Duration::from_secs(30), |ev| {
                ev["type"] == "local_search_result" && ev["op_id"] == 9300
            });
            assert_eq!(result["ok"], true, "search reported failure");
            let hits = result["results"].as_array().cloned().unwrap_or_default();
            eprintln!("[live] '{needle}' -> {} hit(s)", hits.len());
            assert!(!hits.is_empty(), "the needle was not found");

            // ── THE HEADLINE CLAIM ──────────────────────────────────
            //
            // "Search works in an ENCRYPTED room, where server search returns
            // nothing." Asserted SEPARATELY rather than inferred from the
            // total above: a needle found only in a public room would pass
            // that check while the feature's whole reason for existing was
            // broken.
            //
            // The message is SENT here rather than seeded by the fixture
            // script, because an encrypted message can only be produced by a
            // client that holds the keys — which is the property under test.
            if let Some(encrypted_room) = env_nonempty("LIGHTNING_TEST_ENCRYPTED_ROOM") {
                let secret = format!(
                    "zephyrine-{}", std::process::id());
                let rid = CString::new(encrypted_room.clone()).unwrap();
                let body = CString::new(secret.clone()).unwrap();
                let txn = CString::new(format!("live-{}", std::process::id())).unwrap();
                let err = take(super::mx_rust_send_text(
                    handle, rid.as_ptr(), body.as_ptr(), txn.as_ptr()));
                assert!(err.is_empty(), "encrypted send dispatch: {err}");
                // Give the send and the sync round trip time to land the event
                // in the cache, DECRYPTED.
                std::thread::sleep(Duration::from_secs(15));

                let op = 9310u64;
                let err = take(super::mx_rust_search_index_deep(
                    handle, rid.as_ptr(), op));
                assert!(err.is_empty(), "encrypted deep dispatch: {err}");
                let done = wait_for(handle, "encrypted deep",
                                    Duration::from_secs(180), |ev| {
                    ev["type"] == "search_index_deepened" && ev["op_id"] == op
                });
                eprintln!("[live] encrypted room indexed written={}",
                          done["written"]);

                let q3 = CString::new(secret.clone()).unwrap();
                let err = take(super::mx_rust_local_search(
                    handle, q3.as_ptr(), empty.as_ptr(), 20, 0, 9311));
                assert!(err.is_empty(), "encrypted search dispatch: {err}");
                let r3 = wait_for(handle, "encrypted search",
                                  Duration::from_secs(30), |ev| {
                    ev["type"] == "local_search_result" && ev["op_id"] == 9311
                });
                let hits3 = r3["results"].as_array().cloned().unwrap_or_default();
                eprintln!("[live] ENCRYPTED needle -> {} hit(s)", hits3.len());
                assert!(
                    !hits3.is_empty(),
                    "a message this client sent into an ENCRYPTED room was not \
                     searchable — the one thing this feature exists to do"
                );
                assert_eq!(hits3[0]["room_id"].as_str(), Some(encrypted_room.as_str()));
            }

            let encrypted_needle = env_nonempty("LIGHTNING_TEST_ENCRYPTED_NEEDLE");
            if let Some(secret) = encrypted_needle {
                let q2 = CString::new(secret.clone()).unwrap();
                let err = take(super::mx_rust_local_search(
                    handle, q2.as_ptr(), empty.as_ptr(), 50, 0, 9301));
                assert!(err.is_empty(), "encrypted search dispatch: {err}");
                let r2 = wait_for(handle, "encrypted search",
                                  Duration::from_secs(30), |ev| {
                    ev["type"] == "local_search_result" && ev["op_id"] == 9301
                });
                let hits2 = r2["results"].as_array().cloned().unwrap_or_default();
                eprintln!("[live] encrypted '{secret}' -> {} hit(s)", hits2.len());
                assert!(
                    !hits2.is_empty(),
                    "a message in an ENCRYPTED room was not searchable — the \
                     one thing this feature exists to do"
                );
            }

            // A REDACTED message must not stay findable by its own text.
            // The server empties a redacted event's content, so it never
            // becomes indexable in the first place — this asserts the outcome
            // rather than the mechanism, because the mechanism could change
            // (a client-seen redaction goes through forget_event instead) and
            // the outcome must not.
            if let Some(gone) = env_nonempty("LIGHTNING_TEST_REDACTED_NEEDLE") {
                let q = CString::new(gone.clone()).unwrap();
                let _ = take(super::mx_rust_local_search(
                    handle, q.as_ptr(), empty.as_ptr(), 20, 0, 9303));
                let r = wait_for(handle, "redacted search",
                                 Duration::from_secs(30), |ev| {
                    ev["type"] == "local_search_result" && ev["op_id"] == 9303
                });
                let hits = r["results"].as_array().cloned().unwrap_or_default();
                eprintln!("[live] redacted needle -> {} hit(s)", hits.len());
                assert!(
                    hits.is_empty(),
                    "a REDACTED message is still findable by its own text — \
                     the worst thing a local index can do"
                );
            }

            // ── Jump to date (MSC3030) against the REAL server ───────
            //
            // `timestamp_to_event` is only stable since Matrix 1.6, so
            // whether it answers at all is a property of the homeserver, not
            // of this client — which is exactly why it needs a live check
            // rather than a unit test. Uses a hit the search just returned,
            // so the room and the instant are both real and the answer is
            // checkable: asking for the event AT a known event's timestamp
            // and searching FORWARD must return that same event.
            //
            // A server WITHOUT the endpoint answers 404 M_UNRECOGNIZED and
            // classifies as "unrecognized"; that is reported rather than
            // failed, because an old homeserver is a fact about the server
            // and this test is not the place to fail it.
            {
                let first = &hits[0];
                let room = first["room_id"].as_str().unwrap_or_default().to_owned();
                let want = first["event_id"].as_str().unwrap_or_default().to_owned();
                let at = first["timestamp_ms"].as_i64().unwrap_or(0);
                assert!(!room.is_empty() && at > 0,
                        "a search hit carried no room or timestamp");
                let rid = CString::new(room.clone()).unwrap();
                let err = take(super::mx_rust_event_at_timestamp(
                    handle, rid.as_ptr(), at, 9320));
                assert!(err.is_empty(), "timestamp dispatch: {err}");
                let r = wait_for(handle, "timestamp_to_event",
                                 Duration::from_secs(30), |ev| {
                    ev["type"] == "timestamp_event" && ev["op_id"] == 9320
                });
                if r["ok"] == true {
                    let got = r["event_id"].as_str().unwrap_or_default();
                    eprintln!("[live] MSC3030 at {at} -> {got}");
                    assert_eq!(
                        got, want,
                        "the server returned a different event than the one \
                         AT that exact timestamp, searching forward"
                    );
                } else {
                    let why = r["category"].as_str().unwrap_or_default();
                    eprintln!("[live] MSC3030 unavailable: category={why}");
                    assert_eq!(
                        why, "unrecognized",
                        "jump to date failed for a reason other than the \
                         server not implementing it"
                    );
                }
            }

            // A query below the tokenizer's minimum is REPORTED, not silently
            // empty: the user can act on "type one more character".
            let short = CString::new("ab").unwrap();
            let _ = take(super::mx_rust_local_search(
                handle, short.as_ptr(), empty.as_ptr(), 10, 0, 9302));
            let tooshort = wait_for(handle, "too short",
                                    Duration::from_secs(30), |ev| {
                ev["type"] == "local_search_result" && ev["op_id"] == 9302
            });
            assert_eq!(tooshort["ok"], false);
            assert_eq!(tooshort["category"], "too_short");

            // ── WIDGETS, against the same live account ──────────────
            //
            // Parsing and every refusal rule have unit tests. What only a real
            // room can show is that the state read WORKS: that
            // get_state_events finds widgets a homeserver actually holds, that
            // a tombstone does not come back as a widget, and that the hostile
            // shapes are refused with the right reason rather than opened.
            if let Some(widget_room) = env_nonempty("LIGHTNING_TEST_WIDGET_ROOM") {
                let rid = CString::new(widget_room.clone()).unwrap();
                let theme = CString::new("storm").unwrap();
                let lang = CString::new("en").unwrap();
                let err = take(super::mx_rust_room_widgets(
                    handle, rid.as_ptr(), theme.as_ptr(), lang.as_ptr(), 9400));
                assert!(err.is_empty(), "widgets dispatch: {err}");
                let answer = wait_for(handle, "widgets",
                                      Duration::from_secs(60), |ev| {
                    ev["type"] == "room_widgets" && ev["op_id"] == 9400
                });
                let list = answer["widgets"].as_array().cloned().unwrap_or_default();
                eprintln!("[live] widgets found: {}", list.len());
                for w in &list {
                    eprintln!("[live]   {} kind={} openable={} refusal={}",
                              w["id"], w["kind"],
                              !w["url"].as_str().unwrap_or("").is_empty(),
                              w["refusal"]);
                }
                let by_id = |id: &str| -> Option<&serde_json::Value> {
                    list.iter().find(|w| w["id"] == id)
                };
                // The tombstone is NOT a widget. Reading `{}` as one would
                // resurrect every widget anybody ever deleted.
                assert!(by_id("dead").is_none(), "a tombstone came back as a widget");

                let jitsi = by_id("jitsi").expect("the real widget was not found");
                let url = jitsi["url"].as_str().unwrap_or("");
                assert!(url.starts_with("https://meet.example.org/"), "{url}");
                assert!(!url.contains('$'), "a variable was left in: {url}");
                // The room id and the user id are percent-encoded into the
                // URL, so nothing in them can restructure it. `!` stays
                // literal on purpose: encode() matches encodeURIComponent,
                // which is what matrix-widget-api and matrix-sdk both do, and
                // `!` is a sub-delim that is valid in a path segment and
                // cannot change the URL's shape. What MUST be encoded is
                // anything structural.
                assert!(url.contains("%3A"), "the colon was not encoded: {url}");
                assert!(url.contains("%40"), "the @ was not encoded: {url}");
                let after_authority = url.split_once("meet.example.org").unwrap().1;
                assert!(!after_authority.contains("://"), "{url}");
                assert!(!after_authority.contains('#'), "{url}");
                assert!(url.contains("storm"), "the theme was not substituted: {url}");
                let told = jitsi["discloses"].as_array().cloned().unwrap_or_default();
                assert!(told.iter().any(|d| d == "user_id"));
                assert!(told.iter().any(|d| d == "room_id"));
                assert!(told.iter().any(|d| d == "connection"));

                for (id, reason) in [
                    ("evil-scheme", "not_https"),
                    ("evil-authority", "templated_authority"),
                    ("evil-userinfo", "has_userinfo"),
                ] {
                    let w = by_id(id).unwrap_or_else(|| panic!("{id} missing"));
                    assert_eq!(w["url"].as_str(), Some(""),
                               "{id} was resolved to an openable URL");
                    assert_eq!(w["refusal"].as_str(), Some(reason), "{id}");
                }
            }

            let _ = take(super::mx_rust_shutdown_tasks(handle));
            super::mx_rust_destroy(handle);
        }
        let _ = std::fs::remove_dir_all(&dir);
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

// ── Matrix delegation via /.well-known/matrix/client (issue #5) ──────────
//
// Reported by trakais 2026-08-31: with the Matrix server name `example.com`
// delegating to `matrix.example.com`, entering `https://example.com` failed
// with `[404] <non-json bytes>` — the apex domain's ordinary web server
// answering a Matrix API request. Entering the delegated host directly
// worked, which is exactly the signature of a client that never asks for the
// well-known.
//
// It was `Client::builder().homeserver_url()`, which performs no discovery.
// `server_name_or_homeserver_url()` is the builder method meant for a field a
// human typed: strip the scheme, try discovery, fall back to a verified
// homeserver URL.
//
// These cases run against a real SDK client build over loopback HTTP, with no
// network: the builder picks the HTTP scheme when the input carries one, so a
// plain TcpListener can play both the delegating server name and the real
// homeserver it points at.
#[cfg(test)]
mod delegation_tests {
    use super::build_client;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::path::PathBuf;
    use std::sync::mpsc;
    use std::thread;

    fn respond(mut stream: TcpStream, body: &str) {
        let mut buf = [0u8; 2048];
        let read = stream.read(&mut buf).unwrap_or(0);
        let request = String::from_utf8_lossy(&buf[..read]).to_string();
        // A 404 for anything unrecognised, which is precisely what the apex
        // web server did in the report.
        let (status, payload) = if request.starts_with("GET /.well-known/matrix/client")
            || request.starts_with("GET /_matrix/client/versions")
        {
            ("200 OK", body)
        } else {
            ("404 Not Found", "not found")
        };
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Access-Control-Allow-Origin: *\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    /// A server that answers every request with `body`, until the test drops.
    /// Returns its `host:port`.
    fn serve(body: String) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                match stream {
                    Ok(s) => {
                        let body = body.clone();
                        thread::spawn(move || respond(s, &body));
                    }
                    Err(_) => break,
                }
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    /// The subset of `/_matrix/client/versions` the SDK needs to accept a host
    /// as a homeserver.
    fn versions_body() -> String {
        r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#.to_owned()
    }

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime")
    }

    #[test]
    fn login_follows_well_known_delegation_to_another_host() {
        // The REAL homeserver, on its own host and port.
        let homeserver = serve(versions_body());
        // The server NAME the user types. It serves a well-known pointing at
        // the homeserver above and 404s every Matrix API path, exactly like
        // the apex web server in the report.
        let delegating = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{homeserver}"}}}}"#
        ));

        let client = runtime()
            .block_on(build_client(&format!("http://{delegating}"), &PathBuf::new()))
            .expect("delegated login must build a client");

        // The client must be pointed at the DELEGATED host, not at what was
        // typed. Before the fix it kept the typed host and every request 404ed.
        assert_eq!(
            client.homeserver().host_str(),
            Some("127.0.0.1"),
            "expected the discovered homeserver"
        );
        assert_eq!(
            client.homeserver().port(),
            homeserver.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()),
            "client did not follow the well-known to the delegated port"
        );
    }

    #[test]
    fn a_direct_homeserver_url_still_works_without_any_well_known() {
        // The maintainer's own workflow, and the reporter's workaround: type
        // the homeserver directly. It serves NO well-known, so discovery fails
        // and the builder must fall back to the URL rather than refusing.
        let homeserver = serve(versions_body());

        let client = runtime()
            .block_on(build_client(&format!("http://{homeserver}"), &PathBuf::new()))
            .expect("a direct homeserver URL must still work");

        assert_eq!(
            client.homeserver().port(),
            homeserver.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()),
        );
    }

    #[test]
    fn a_host_that_is_no_homeserver_fails_to_build_instead_of_404ing_later() {
        // Nothing Matrix here: no well-known and no versions endpoint. The old
        // homeserver_url() accepted this happily and the user met a 404 at
        // login; server_name_or_homeserver_url verifies, so it fails here with
        // a real error.
        let bogus = serve("irrelevant".to_owned());
        let result = runtime()
            .block_on(build_client(&format!("http://{bogus}/nope"), &PathBuf::new()));
        assert!(result.is_err(), "a non-homeserver must not build a client");
    }
}

// ── The silent classic-sync wedge (issue #2) ─────────────────────────────
//
// Reported by ThomasRedstone against a server that takes the classic
// fallback: "starting" was logged and then nothing for 13+ minutes — zero
// established TCP connections for the process, zero I/O progress, ~0.4% CPU,
// and no sync_error even though run_classic_sync's select! arm promises one
// on failure. Restarting the client with the same store synced normally.
//
// It has not reproduced on demand, so this is INSTRUMENTATION rather than a
// fix: the escalation makes the silence visible instead of leaving a spinner
// that means nothing. Driving it with millisecond steps is the difference
// between covering the escalation and shipping three sleeps totalling ten
// minutes that no test will ever run.
#[cfg(test)]
mod first_sync_watchdog_tests {
    use super::{watch_first_sync_response, Ordering};
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicBool;
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    fn steps() -> Vec<Duration> {
        vec![Duration::from_millis(10), Duration::from_millis(20),
             Duration::from_millis(30)]
    }

    fn drain(events: &VecDeque<String>) -> Vec<serde_json::Value> {
        events.iter().map(|e| serde_json::from_str(e).unwrap()).collect()
    }

    #[tokio::test]
    async fn silence_is_reported_at_every_step() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        // Never flipped: the wedge, where no response ever arrives.
        let first = Arc::new(AtomicBool::new(true));
        watch_first_sync_response(&events, &first, &steps()).await;

        let seen = drain(&events.lock().unwrap());
        assert_eq!(seen.len(), 3, "expected one report per step");
        for value in &seen {
            assert_eq!(value["type"], "sync_stalled");
            assert_eq!(value["phase"], "first_response");
        }
        // Each report says how long it has been waiting, cumulatively — a
        // report that cannot say "how long" is no better than the spinner.
        assert!(seen[0]["waited_secs"].is_number());
    }

    #[tokio::test]
    async fn a_response_ends_the_watch_and_reports_nothing() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        // Already answered: an ordinary sync, which must stay silent.
        let first = Arc::new(AtomicBool::new(false));
        watch_first_sync_response(&events, &first, &steps()).await;
        assert!(events.lock().unwrap().is_empty(),
                "a healthy sync produced a stall report");
    }

    #[tokio::test]
    async fn a_late_response_stops_further_reports() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        let first = Arc::new(AtomicBool::new(true));
        let flip = Arc::clone(&first);
        // The response lands between the first and second step, which is the
        // case that separates "report once and stop" from "report forever".
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(15)).await;
            flip.store(false, Ordering::SeqCst);
        });
        watch_first_sync_response(&events, &first, &steps()).await;

        let seen = drain(&events.lock().unwrap());
        assert_eq!(seen.len(), 1,
                   "the watch kept reporting after the sync answered");
    }
}
