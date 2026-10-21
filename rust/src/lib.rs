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
                encrypted::{
                    OriginalSyncRoomEncryptedEvent, ToDeviceRoomEncryptedEvent,
                },
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
    room_list_service::{
        filters, RoomListDynamicEntriesController, RoomListItem, RoomListService,
    },
    spaces::SpaceService,
    sync_service::{Error as UnifiedSyncError, State as UnifiedSyncState, SyncService},
};
use serde::{Deserialize, Serialize};
use serde_json::json;

mod backdrop;
mod accountprefs;
mod bgsync;
mod settingsync;
mod banner;
mod bio;
mod bridges;
mod previewbackfill;
mod calls;
// DeepFilterNet microphone noise suppression (#20); C ABI in
// rust/include/matrix_denoise.h. Stubs when the `deepfilternet` feature is off.
mod denoise;
mod discover;
mod gifs;
mod ignore;
mod iotrace;
mod pushrules;
mod imagesend;
mod linkmedia;
mod indexall;
mod invites;
mod membership_lost;
mod keyexport;
mod localsearch;
mod location;
mod mediafetch;
mod mediafiles;
mod mediahealth;
mod mediahistory;
mod mediastore;
mod missingtrace;
mod cachelayout;
mod storelocking;
mod sendrecovery;
mod sessiontokens;
mod namecolor;
mod widgets;
mod oauth;
mod pagewalk;
mod pinned;
mod search;
mod sso;
mod stickers;
mod uia;
mod warm_rooms;
mod recovery;
// Account password change and its capability probe (FFI in the module).
mod password;
mod policy;
mod presence;
mod qrlogin;
mod profile;
mod push_verdict;
mod rooms;
mod roomaction;
mod roomexport;
mod rtc;
mod aboutinfo;
mod serveradmin;
mod sfu;
mod storeclose;
mod syncnotify;
mod timeline;

/// The single HTTP user agent, for the homeserver and any user-invoked
/// third-party request. Derived from `rust/Cargo.toml`; the
/// `signpath-compliance` test checks the CMake version against it.
pub(crate) const USER_AGENT: &str = concat!("Lightning/", env!("CARGO_PKG_VERSION"));

/// Shared alias for the FFI event queue reference (used by `rooms.rs`).
pub(crate) type EventQueueRef = Arc<Mutex<VecDeque<String>>>;

/// Wake-up signals for the E2EE recovery coordinator. Carries no key
/// material; each variant only means "re-evaluate".
#[derive(Clone, Copy, Debug)]
enum RecoveryNudge {
    /// A SAS flow reached `SasState::Done` locally. Re-arms the secret-request
    /// retry ladder even when the device was already `Verified`, since a repeat
    /// verification produces no state edge.
    VerificationDone,
    /// The user asked to re-request the encryption secrets. Runs one attempt
    /// immediately and re-arms the follow-up ladder.
    ManualRequest,
    /// An `m.secret.send` to-device event was decrypted. Only the arrival is
    /// signalled, never the name or value.
    SecretEventSeen,
}

/// Slot through which SAS flows and the manual-request FFI reach the live
/// coordinator's nudge channel. `None` whenever no observer is running.
type RecoveryNudgeSlot =
    Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<RecoveryNudge>>>>;

/// Deliver a nudge to the live coordinator, if any. A missing channel is
/// fine: the coordinator re-reads SDK state whenever it starts.
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
    /// Process-unique id of this bridge. Lifecycle numbers restart at every
    /// bridge, so "already reported in this lifecycle" must name the bridge
    /// too (`recovery::interrupted_setup_to_report`).
    bridge_id: u64,
    /// The local message index (SQLite FTS5), opened lazily inside the
    /// account's store directory so it is deleted with the account.
    ///
    /// A `std` mutex, not tokio's: every hold is a short synchronous query and
    /// must never span an `.await`.
    search_index: Arc<Mutex<Option<localsearch::SearchIndex>>>,
    /// Cooperative stop for the background indexer, checked between rooms, so
    /// a sweep does not keep the store open while sign-out deletes it.
    index_shutdown: Arc<AtomicBool>,
    /// "Index all rooms": the queue, its persisted position (a file in this
    /// store directory) and its pause/cancel/hold controls. Per bridge, so a
    /// run can only write this account's index.
    index_all: Arc<indexall::Control>,
    /// Per-room cursor of the independent media-history walk, so reopening
    /// the panel continues. Holds only a `/messages` token and counters.
    media_history: Arc<Mutex<HashMap<String, mediahistory::Cursor>>>,
    session_file: Arc<Mutex<Option<PathBuf>>>,
    client: Arc<Mutex<Option<Client>>>,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_task: Mutex<Option<SyncTask>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    // Shared runtime for everything that outlives one FFI call (timelines,
    // send queue, event cache, key import). Per-call `run_async` runtimes
    // cannot host those: their tasks die with the runtime.
    runtime: Arc<tokio::runtime::Runtime>,
    timelines: Arc<timeline::TimelineRegistry>,
    // In-flight / returned DM creates; see rooms::DmCreateRegistry.
    dm_creates: Arc<rooms::DmCreateRegistry>,
    // Only when C++ has a media backend do inbound call handlers include the
    // remote SDP in their payloads. Off by default.
    call_media_capable: Arc<std::sync::atomic::AtomicBool>,
    // The live SFU signalling session and its generation. The generation is a
    // separate Arc so teardown can invalidate the task without taking the
    // session mutex.
    sfu: sfu::SfuState,
    sfu_generation: Arc<std::sync::atomic::AtomicU64>,
    // The sliding-sync RoomListService, published by the modern sync loop and
    // withdrawn on every exit path. Room opens use it to subscribe the active
    // room, because subscription-only required state (`m.room.pinned_events`)
    // never reaches the store otherwise.
    room_list_service: Arc<Mutex<Option<Arc<RoomListService>>>>,
    // The dynamic adapter that owns the room-list index space; C++ indexes its
    // registry by the positions its diffs carry. `set_filter` makes it re-emit
    // a `VectorDiff::Reset`, which is how a rejected diff is recovered. A
    // `client.rooms()` snapshot has a different order and would make the
    // rejection self-sustaining (see `enqueue_rooms_stamped`).
    room_list_entries: Arc<Mutex<Option<Arc<RoomListDynamicEntriesController>>>>,
    // The room the user has open plus the recently opened "warm" rooms: the
    // subscriptions the sliding sync carries (warm_rooms.rs). Kept apart from
    // the service so a room opened before sync starts is subscribed once it
    // appears; cleared in `stop_sync_and_wait` so it can never leak into a
    // later account's sync.
    active_room_subscription: Arc<Mutex<warm_rooms::SubscriptionSet>>,
    // At most one privileged operation parked between a UIA challenge and the
    // user's answer (uia.rs). Cleared on teardown.
    uia_pending: Arc<Mutex<Option<uia::UiaPending>>>,
    // Managed room-key import task, so sign-out can join it.
    import_task: Mutex<Option<tokio::task::JoinHandle<()>>>,
    // Short room-state commands (typing, receipts, invites, marked-unread),
    // joined during shutdown.
    room_action_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    active_typing_room: Arc<Mutex<Option<String>>>,
    receipt_targets: Arc<Mutex<HashMap<String, OwnedEventId>>>,
    // Receipt privacy: 0 public, 1 private (m.read.private), 2 none. Stored on
    // the bridge so all three receipt paths (room, mark-read, thread) honour it.
    receipt_privacy: Arc<AtomicI32>,
    /// MSC4108 sign-in-another-device state: the generation, the one-shot
    /// check-code sender and the running task. See rust/src/qrlogin.rs.
    qr_login: Arc<qrlogin::QrLoginState>,
    receipt_serial: Arc<tokio::sync::Mutex<()>>,
    invite_actions: Arc<Mutex<BTreeSet<String>>>,
    // Per-room notification mode writes (SDK push rules). Serialized behind one
    // async mutex because `set_room_notification_mode` is a read/modify/write,
    // and coalesced per room to the latest requested mode. An entry lives until
    // its reporting task consumes it, so it also marks "write in flight" for
    // the read path (see mx_rust_get_room_notification_mode).
    notification_mode_targets: Arc<Mutex<HashMap<String, u8>>>,
    notification_mode_serial: Arc<tokio::sync::Mutex<()>>,
    // One session-long NotificationSettings: a fresh instance per call would
    // lose the rules the SDK applies locally after a write. Cleared on
    // sign-out / detach.
    notification_settings: Arc<Mutex<Option<NotificationSettings>>>,
    // The lifecycle generation a push-rules change watcher was started for
    // (0 = none), so one watcher runs per session and a stale one ends.
    notification_watch_lifecycle: Arc<std::sync::atomic::AtomicU64>,
    // The push-rules watcher's own task. It never finishes by itself, so
    // shutdown aborts it BEFORE joining the room-action pool (like the media
    // fetches), or every sign-out would wait out the join budget.
    notification_watch_abort: Arc<Mutex<Option<tokio::task::AbortHandle>>>,
    // SAS verification state: one active flow at a time. A second request while
    // one is live is cancelled on the wire. Both slots are released by
    // `FlowSlotGuard`, only for the flow that owns them.
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas): SasVerification has no flow_id() accessor in matrix-sdk
    // 0.18.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
    // CSRF `state` of the in-flight OAuth request, kept only so a cancelled
    // sign-in can call `OAuth::abort_login(state)`. Scoped to this handle.
    // Never logged.
    oauth_state: Arc<Mutex<Option<matrix_sdk::authentication::oauth::CsrfToken>>>,
    // Session-token persistence watcher (oauth::spawn_token_persistence), held
    // so shutdown can abort it; it holds a strong Client.
    token_task: Arc<Mutex<Option<tokio::task::JoinHandle<()>>>>,
    // Every rotation this handle's clients make, recorded inside matrix-sdk's
    // own refresh (sessiontokens.rs). Outlives the Client: the retiring
    // worker reads it after the runtime is shut down.
    token_ledger: Arc<sessiontokens::TokenLedger>,
    // (flow_id, qr): the show-QR half of the single-flow policy. At most one of
    // active_sas / active_qr is occupied for a request.
    active_qr: Arc<Mutex<Option<(String, QrVerification)>>>,
    // SAS driver tasks. They hold this account's Client, so one left running
    // keeps the crypto store open while sign-out deletes it. Joined by
    // shutdown_managed_tasks.
    verification_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    // Cooperative stop for those drivers, checked every poll tick. Set only by
    // teardown.
    verification_shutdown: Arc<AtomicBool>,
    // Room-key import is serialized per client; a second attempt is rejected.
    // C++ also reads this before sign-out to wait for a live import.
    import_active: Arc<AtomicBool>,
    // Parked media payloads for the take/free bridge, keyed by op id, between
    // `media_ready` and `mx_rust_media_take`. Cleared on shutdown so decrypted
    // media never outlives the session.
    media_results: Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    // Decrypted personal chat backgrounds (bgsync.rs) between
    // `personal_background_bytes` and `mx_rust_personal_background_take`. Its
    // OWN map: those op ids come from another counter, and sharing
    // `media_results` could hand a timeline picture to the background code.
    // Bounded (bgsync::park) and cleared on shutdown.
    personal_background_results: bgsync::ParkedPictures,
    // The last switch GET a personal-background read made, shared by
    // concurrent reads (bgsync::switch_after). Keyed by lifecycle.
    personal_switch_cache: bgsync::SwitchCache,
    // Abort handles for in-flight media fetches, keyed by op id, so
    // mx_rust_media_cancel can stop an abandoned download. Tasks remove their
    // own entry on completion; cleared on shutdown.
    pub(crate) media_fetch_aborts: Arc<Mutex<HashMap<u64, tokio::task::AbortHandle>>>,
    // Terminal event lane for op-id-keyed results (media, GIF), so a
    // timeline-diff flood on the bulk queue cannot starve them. C++ drains it
    // before every bulk batch. Bounded only as a tripwire.
    command_events: EventQueueRef,
    // Crypto-bootstrap observer: forwards sanitized verification/recovery/backup
    // state to C++. Lives as long as the sync session.
    bootstrap_task: Mutex<Option<SyncTask>>,
    // Coordinator nudge channel (SAS, manual request, m.secret.send). Cleared
    // when the observer stops so a stale sender cannot reach a later session.
    recovery_nudges: RecoveryNudgeSlot,
    // The sync handlers' backlog clock and their undecryptable events awaiting
    // redecryption (syncnotify.rs), shared with the sync loop that drives the
    // clock and the crypto observer that re-emits a decrypted event. Reset at
    // every session end.
    sync_notify: Arc<syncnotify::SyncNotify>,
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
        let index_all = Arc::new(indexall::Control::new(&store_path));
        Ok(Self {
            store_path,
            bridge_id: recovery::next_bridge_id(),
            search_index: Arc::new(Mutex::new(None)),
            index_shutdown: Arc::new(AtomicBool::new(false)),
            index_all,
            media_history: Arc::new(Mutex::new(HashMap::new())),
            session_file: Arc::new(Mutex::new(None)),
            client: Arc::new(Mutex::new(None)),
            events: Arc::clone(&events),
            sync_task: Mutex::new(None),
            sync_mode: Arc::new(Mutex::new(SyncMode::Stopped)),
            runtime: Arc::new(runtime),
            timelines: Arc::new(timeline::TimelineRegistry::new(events)),
            dm_creates: Arc::new(rooms::DmCreateRegistry::default()),
            call_media_capable: Arc::new(
                std::sync::atomic::AtomicBool::new(false)),
            room_list_service: Arc::new(Mutex::new(None)),
            room_list_entries: Arc::new(Mutex::new(None)),
            active_room_subscription: Arc::new(Mutex::new(Default::default())),
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
            notification_watch_lifecycle: Arc::new(std::sync::atomic::AtomicU64::new(0)),
            notification_watch_abort: Arc::new(Mutex::new(None)),
            active_request: Arc::new(Mutex::new(None)),
            active_sas: Arc::new(Mutex::new(None)),
            oauth_state: Arc::new(Mutex::new(None)),
            token_task: Arc::new(Mutex::new(None)),
            token_ledger: sessiontokens::TokenLedger::new(),
            active_qr: Arc::new(Mutex::new(None)),
            verification_tasks: Mutex::new(Vec::new()),
            verification_shutdown: Arc::new(AtomicBool::new(false)),
            import_active: Arc::new(AtomicBool::new(false)),
            media_results: Arc::new(Mutex::new(HashMap::new())),
            personal_background_results: Arc::new(Mutex::new(HashMap::new())),
            personal_switch_cache: Arc::new(tokio::sync::Mutex::new(None)),
            media_fetch_aborts: Arc::new(Mutex::new(HashMap::new())),
            command_events: Arc::new(Mutex::new(VecDeque::new())),
            bootstrap_task: Mutex::new(None),
            recovery_nudges: Arc::new(Mutex::new(None)),
            sync_notify: Arc::new(syncnotify::SyncNotify::default()),
            sfu: sfu::SfuState::default(),
            sfu_generation: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    /// Where this handle's next client sends its rotations (sessiontokens.rs).
    fn token_sink(&self) -> sessiontokens::Sink {
        sessiontokens::Sink {
            ledger: Arc::clone(&self.token_ledger),
            lane: Arc::clone(&self.command_events),
            events: Arc::clone(&self.events),
        }
    }

    /// A UIA challenge belongs to the session that raised it: at a session
    /// end, drop it and stop a cross-signing job still waiting for an OAuth
    /// approval. The flag is set at once; the SDK reset handle's own cancel is
    /// queued on the room-action pool, which teardown then joins.
    fn abandon_uia(&self) {
        if let Some(handle) = uia::abandon_on_teardown(&self.uia_pending) {
            self.spawn_room_action(async move {
                handle.cancel().await;
            });
        }
    }

    fn stop_sync_and_wait(&self) -> bool {
        // The active-room subscription is scoped to this session; clear it so a
        // later account's sync cannot subscribe another account's room.
        if let Ok(mut guard) = self.active_room_subscription.lock() {
            guard.clear();
        }
        // The UIA slot is NOT cleared here: this also runs when a finished
        // sync thread is reaped and on a plain sync stop, while the session
        // goes on. Session ends call `abandon_uia()` themselves.
        // Stop the bootstrap observer first so no status event is emitted for a
        // dying session; drop the nudge sender before it.
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
        let stopped = if let Some(mut task) = task {
            join_task_within_budget(&mut task, "sync");
            true
        } else {
            false
        };
        // After both loops are gone: nothing a stopped session left undecryptable
        // may be re-emitted into the next one.
        self.sync_notify.reset();
        stopped
    }

    /// Join every handle under one budget, then abort and briefly drain
    /// whatever missed it. Returns how many missed.
    ///
    /// Uses a single `JoinAll` that owns the handles across both rounds.
    /// `JoinHandle::poll` consumes the task output, so building a second
    /// `join_all` over the same handles re-polls finished tasks and panics
    /// with "JoinHandle polled after completion". A `JoinSet` would avoid this
    /// too, but `spawn_media_fetch` needs its own `AbortHandle` registry.
    ///
    /// Budgets are in milliseconds so `SHUTDOWN_WORST_CASE_MS` can sum them.
    async fn join_or_abort(
        handles: Vec<tokio::task::JoinHandle<()>>,
        budget_ms: u64,
    ) -> usize {
        if handles.is_empty() {
            return 0;
        }
        // Collected before the handles move into join_all.
        let aborts: Vec<tokio::task::AbortHandle> = handles
            .iter()
            .map(tokio::task::JoinHandle::abort_handle)
            .collect();
        let mut all = std::pin::pin!(futures_util::future::join_all(handles));
        if tokio::time::timeout(
            std::time::Duration::from_millis(budget_ms),
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
        // The same future, so a handle joined in round one is never polled again.
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(SHUTDOWN_ABORT_DRAIN_MS),
            all.as_mut(),
        )
        .await;
        missed
    }

    // Returns (import_joined, sync_stopped, actions_missed,
    // verifications_missed, actions_ms, verifications_ms, total_ms).
    //
    // Every wait here runs on the calling thread, which is the GUI thread
    // (switchToAccount -> detachSession -> mx_rust_shutdown_tasks), so the
    // durations are reported to attribute UI freezes. Counts and ms only.
    fn shutdown_managed_tasks(&self)
        -> (bool, bool, usize, usize, u64, u64, u64) {
        let total = std::time::Instant::now();
        // From here nothing drains or answers: a refresh during the joins
        // below must not hold its save callback open for the GUI thread that
        // is blocked in them. Rotations are still recorded in the ledger,
        // which the retiring worker reads once the runtime is gone.
        self.token_ledger.stop_waiting();
        // Stop the indexer first and close its connection: an open SQLite handle
        // in the store directory makes the deletion fail on Windows.
        //
        // Clear RTC membership bookkeeping unconditionally. A stale
        // `OWN_MEMBERSHIP_PUBLISHED` mark would let the next join inherit a dead
        // session's created_ts, which peers read as "not a new joiner" and answer
        // with no media key. The leave path cannot do this reliably because it
        // needs a live client.
        crate::rtc::forget_all_memberships_published();
        self.index_shutdown.store(true, Ordering::Relaxed);
        if let Ok(mut guard) = self.search_index.lock() {
            *guard = None;
        }
        // The token watcher is aborted only after sync has stopped (below), so
        // a rotation caused by a request still in flight here is still
        // announced. The SDK adopts a rotated pair inside `Client` either way;
        // what was lost (2026-10-01) was the announcement: it sat in this
        // handle's event queue, which dies with the handle when C++ releases
        // it before the next drain, so the keyring kept the used refresh
        // token. `final_session_tokens`, read after this shutdown, is the fix
        // for that; the reorder only keeps announcements coming meanwhile.
        // SAS drivers first: they hold a Client purely to poll, and one left
        // running keeps the crypto store open while `finishSignOut` deletes it.
        // Signal, join, then abort.
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
            SHUTDOWN_VERIFICATION_JOIN_MS,
        ));
        let verifications_ms = t_verifications.elapsed().as_millis() as u64;
        // Cancel any flow still parked in the slots (including an unanswered
        // incoming request) so the peer does not wait out the SDK's 10-minute
        // VERIFICATION_TIMEOUT. This must live here: every teardown path runs
        // `mx_rust_shutdown_tasks` before logout, so the slots are empty by then.
        let (pending_sas, pending_qr, pending_request) = take_pending_flows(
            &self.active_request, &self.active_sas, &self.active_qr,
        );
        if pending_sas.is_some() || pending_qr.is_some() || pending_request.is_some() {
            // One outer cap over the whole sweep; this step is on the critical path
            // to the store being closed.
            self.runtime.block_on(async {
                let _ = tokio::time::timeout(
                    std::time::Duration::from_millis(SHUTDOWN_FLOW_SWEEP_MS),
                    cancel_flow_best_effort(
                        pending_sas.as_ref(),
                        pending_qr.as_ref(),
                        pending_request.as_ref(),
                        std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
                    ),
                )
                .await;
            });
        }

        self.timelines.shutdown(&self.runtime);

        // The push-rules watcher waits for changes for ever: abort it first.
        if let Ok(mut guard) = self.notification_watch_abort.lock() {
            if let Some(handle) = guard.take() {
                handle.abort();
            }
        }
        // Abort media downloads before joining the room-action pool; a large
        // transfer would otherwise burn the whole join budget.
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }

        // Before the join: a cross-signing job polling an OAuth approval
        // would otherwise run out the whole budget.
        self.abandon_uia();
        let actions = self.room_action_tasks.lock().ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
        // One overall join budget for all room-action tasks; whatever misses it
        // is aborted and briefly drained.
        let t_actions = std::time::Instant::now();
        let actions_missed = self.runtime.block_on(Self::join_or_abort(
            actions,
            SHUTDOWN_ACTION_JOIN_MS,
        ));
        let actions_ms = t_actions.elapsed().as_millis() as u64;

        let import = self.import_task.lock().ok().and_then(|mut guard| guard.take());
        let mut import_joined = true;
        if let Some(handle) = import {
            if !handle.is_finished() {
                let joined = self.runtime.block_on(async {
                    tokio::time::timeout(
                        std::time::Duration::from_millis(
                            SHUTDOWN_IMPORT_JOIN_MS,
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

        // Now nothing of ours sends a request, so no rotation can follow. The
        // watcher holds a strong Client and its broadcast sender lives inside
        // that Client, so recv() never returns Closed and the task would keep
        // the crypto store open. Aborted: it has no cooperative exit. An
        // announcement it queued that C++ never drained is not needed: the
        // Client already holds that pair, and `final_session_tokens` hands it
        // back.
        if let Ok(mut guard) = self.token_task.lock() {
            if let Some(handle) = guard.take() {
                handle.abort();
            }
        }

        // Drop parked (possibly decrypted) media bytes with the session.
        if let Ok(mut guard) = self.media_results.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.personal_background_results.lock() {
            guard.clear();
        }
        // Clear abort handles registered between the drain above and sync stop.
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }
        // The cached NotificationSettings holds a Client clone and an event-handler
        // guard; pending write markers must not leak into the next account.
        if let Ok(mut guard) = self.notification_settings.lock() {
            *guard = None;
        }
        if let Ok(mut guard) = self.notification_mode_targets.lock() {
            guard.clear();
        }
        (import_joined, sync_stopped, actions_missed, verifications_missed,
         actions_ms, verifications_ms, total.elapsed().as_millis() as u64)
    }

    /// The tokens the session holds NOW, which may be newer than the last
    /// `session_tokens_refreshed` C++ drained: a rotation that lands between
    /// that drain and the handle being retired is otherwise lost. `None`
    /// without a client or tokens. SENSITIVE: never log the result.
    fn final_session_tokens(&self) -> Option<matrix_sdk::SessionTokens> {
        self.client
            .lock()
            .ok()
            .and_then(|guard| guard.as_ref().and_then(Client::session_tokens))
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

    /// Media fetches ride the tracked room-action pool and also register an
    /// abort handle under their op id for mx_rust_media_cancel. The future
    /// removes its own entry once resolved (see rooms::media_fetch).
    fn spawn_media_fetch<F>(&self, op_id: u64, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.room_action_tasks.lock() {
            tasks.retain(|task| !task.is_finished());
            let handle = self.runtime.spawn(future);
            if let Ok(mut aborts) = self.media_fetch_aborts.lock() {
                // Drop handles of tasks that already finished.
                aborts.retain(|_, h| !h.is_finished());
                aborts.insert(op_id, handle.abort_handle());
            }
            tasks.push(handle);
        }
    }

    /// Run a SAS driver on the shared runtime as a joinable task, so it cannot
    /// outlive `mx_rust_destroy` holding this account's Client.
    fn spawn_verification_task<F>(&self, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.verification_tasks.lock() {
            tasks.retain(|task| !task.is_finished());
            tasks.push(self.runtime.spawn(future));
        }
    }

    /// Run a detached FFI action on the tracked room-action pool, reporting a
    /// panic as an `error` event.
    ///
    /// Tracking matters for data at rest: an untracked thread owning a Client
    /// (e.g. `Recovery::recover()` importing Megolm sessions) could still be
    /// writing the SQLite store while account removal deletes it. The panic
    /// wrapper keeps a panicking action visible; a bare spawn would hand the
    /// panic to `join_or_abort`, which discards it.
    fn spawn_reported_action<F>(&self, label: &'static str, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        let events = Arc::clone(&self.events);
        // A dropped action must not be silent either: `spawn_room_action` drops
        // the future on a poisoned mutex, so report it the same way.
        let dropped = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
        let claimed = std::sync::Arc::clone(&dropped);
        let report = Arc::clone(&events);
        self.spawn_room_action(async move {
            claimed.store(false, std::sync::atomic::Ordering::SeqCst);
            if CatchPanic::new(future).await.is_err() {
                enqueue(
                    &report,
                    json!({
                        "type": "error",
                        "message": format!("Rust SDK {label} task panicked."),
                    }),
                );
            }
        });
        if dropped.load(std::sync::atomic::Ordering::SeqCst)
            && self.room_action_tasks.lock().is_err()
        {
            enqueue(
                &events,
                json!({
                    "type": "error",
                    "message": format!("Rust SDK {label} task could not start."),
                }),
            );
        }
    }
}

/// A future that catches a panic raised by the future it wraps.
///
/// Not `futures_util::FutureExt::catch_unwind`: that needs futures-util's
/// `std` feature, which we take with `default-features = false`.
///
/// The inner future is boxed so this type is `Unpin`, and dropped as soon
/// as it panics or completes so it is never polled again.
struct CatchPanic<F> {
    inner: Option<std::pin::Pin<Box<F>>>,
}

impl<F> CatchPanic<F> {
    fn new(future: F) -> Self {
        Self { inner: Some(Box::pin(future)) }
    }
}

impl<F: std::future::Future<Output = ()>> std::future::Future for CatchPanic<F> {
    /// `Err(())` means the wrapped future panicked. The payload is dropped, not
    /// reported: it may contain room ids or message bodies. `install_panic_hook`
    /// likewise keeps the payload out of stderr.
    type Output = Result<(), ()>;

    fn poll(
        self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<Self::Output> {
        use std::task::Poll;
        let this = self.get_mut();
        let Some(future) = this.inner.as_mut() else {
            // Already resolved; never poll a future that has unwound.
            return Poll::Ready(Err(()));
        };
        let polled = catch_unwind(AssertUnwindSafe(|| future.as_mut().poll(cx)));
        match polled {
            Ok(Poll::Pending) => Poll::Pending,
            Ok(Poll::Ready(())) => {
                this.inner = None;
                Poll::Ready(Ok(()))
            }
            Err(_) => {
                this.inner = None;
                Poll::Ready(Err(()))
            }
        }
    }
}

/// Wait for a cancelled task's thread, but no longer than the budget.
/// Returns false when the thread was detached instead (see
/// SYNC_TASK_JOIN_BUDGET_MS).
fn join_task_within_budget(task: &mut SyncTask, label: &str) -> bool {
    if let Some(cancel) = task.cancel.take() {
        let _ = cancel.send(());
    }
    let Some(thread) = task.thread.take() else {
        return true;
    };
    let finished = match task.done.take() {
        // Disconnected means the thread dropped its sender, i.e. it exited.
        Some(done) => !matches!(
            done.recv_timeout(std::time::Duration::from_millis(
                SYNC_TASK_JOIN_BUDGET_MS
            )),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ),
        // No completion signal: fall back to joining.
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
    /// Completion signal for `thread`. Nothing is sent; the thread owns the
    /// Sender, so when it exits `recv_timeout` returns `Disconnected`.
    done: Option<std::sync::mpsc::Receiver<()>>,
}

/// How long teardown waits for a sync or bootstrap thread to notice its
/// cancellation.
///
/// These threads drive `current_thread` runtimes, and dropping one waits for
/// any running `spawn_blocking`, which includes DNS resolution; an unbounded
/// join froze the UI for as long as a stalled resolver took. On timeout the
/// thread is detached: it is cancelled, and the generation guards reject
/// anything it might still emit.
const SYNC_TASK_JOIN_BUDGET_MS: u64 = 1500;

// Shutdown budget.
//
// `shutdown_managed_tasks` is a chain of sequential waits, and C++ waits for
// all of it plus `mx_rust_destroy` with one flat
// `waitForRustRetirement(kStoreCloseBudgetMs)` (15 000 ms,
// src/matrix/RustSdkMatrixClient.h). Past that budget C++ deletes the store
// anyway, possibly under a live SQLite writer. The budgets below therefore
// sum, with a reserve for the runtime drop, to less than that, and
// `SHUTDOWN_WORST_CASE_MS` asserts it at compile time.
//
//     | step                                  |    ms |
//     |---------------------------------------|-------|
//     | verification join                     |  2500 |
//     | verification abort drain              |   250 |
//     | verification slot-sweep cancel (cap)  |  2000 |
//     | room-action join                      |  1500 |
//     | room-action abort drain               |   250 |
//     | import join                           |   500 |
//     | stop_sync_and_wait (2 x 1500)         |  3000 |
//     | TimelineRegistry::shutdown (2 x 250)  |   500 |
//     | SHUTDOWN_WORST_CASE_MS                | 10500 |
//     | reserve for mx_rust_destroy           |  3000 |
//     |   of which retire_bridge's runtime    |       |
//     |   shutdown (1500) + store release     |       |
//     |   wait (1000)                         |       |
//     | total vs kStoreCloseBudgetMs = 15000  | 13500 |

/// After `abort()`, how long a cancelled task may take to reach its next
/// await point. Only guards against a task stuck in a synchronous stretch.
const SHUTDOWN_ABORT_DRAIN_MS: u64 = 250;

/// Wire budget for one courtesy `m.key.verification.cancel` during teardown.
/// Shorter than `VERIFICATION_CANCEL_TIMEOUT_SECS`: if it misses, the peer
/// just waits out the SDK's 10-minute VERIFICATION_TIMEOUT.
const SHUTDOWN_FLOW_CANCEL_MS: u64 = 1000;

/// How long the SAS/QR drivers get to notice `verification_shutdown` and
/// tell their peer. One poll tick plus two cancels (`drive_sas_flow`
/// cancels the SAS and the request).
const SHUTDOWN_VERIFICATION_JOIN_MS: u64 =
    VERIFICATION_POLL_MS + 2 * SHUTDOWN_FLOW_CANCEL_MS;

/// One outer cap over the whole slot sweep (at most three flows), rather
/// than a budget per flow.
const SHUTDOWN_FLOW_SWEEP_MS: u64 = 2 * SHUTDOWN_FLOW_CANCEL_MS;

/// Grace period before the room-action pool is force-aborted. Short on
/// purpose: media downloads are already aborted, and the one long-running
/// member (`mx_rust_backup_action`) must not be waited for here.
pub(crate) const SHUTDOWN_ACTION_JOIN_MS: u64 = 1500;

/// Grace period for the key-import task. Aborting is safe: matrix-sdk
/// commits imported sessions in batches, so a cancelled import is partial
/// and retryable.
const SHUTDOWN_IMPORT_JOIN_MS: u64 = 500;

/// Mirror of `RustSdkMatrixClient::kStoreCloseBudgetMs` (the source of
/// truth), so the budget can be checked at compile time.
/// `the_store_close_budget_mirrors_the_cpp_constant` keeps them in sync.
const STORE_CLOSE_BUDGET_MS: u64 = 15_000;

/// Time `waitForRustRetirement` must still have for `mx_rust_destroy` after
/// `mx_rust_shutdown_tasks` returns (the runtime drop waits for in-flight
/// `spawn_blocking` SQLite work).
const SHUTDOWN_DESTROY_RESERVE_MS: u64 = 3_000;

/// `retire_bridge`: how long the shared runtime's shutdown may wait for its
/// blocking pool (in-flight SQLite closes; a stuck DNS lookup is left behind).
const RETIRE_RUNTIME_SHUTDOWN_MS: u64 = 1_500;

/// `retire_bridge`: how long it then waits for every store to be dropped.
const RETIRE_STORE_RELEASE_WAIT_MS: u64 = 1_000;

const _: () = assert!(
    RETIRE_RUNTIME_SHUTDOWN_MS + RETIRE_STORE_RELEASE_WAIT_MS < SHUTDOWN_DESTROY_RESERVE_MS,
    "retire_bridge's own waits must fit the reserve kept for mx_rust_destroy"
);

/// Declared worst case of every sequential wait in `shutdown_managed_tasks`,
/// including `TimelineRegistry::shutdown`.
const SHUTDOWN_WORST_CASE_MS: u64 = SHUTDOWN_VERIFICATION_JOIN_MS
    + SHUTDOWN_ABORT_DRAIN_MS
    + SHUTDOWN_FLOW_SWEEP_MS
    + SHUTDOWN_ACTION_JOIN_MS
    + SHUTDOWN_ABORT_DRAIN_MS
    + SHUTDOWN_IMPORT_JOIN_MS
    + 2 * SYNC_TASK_JOIN_BUDGET_MS
    // TimelineRegistry::shutdown: live and thread timelines, both already
    // aborted.
    + 2 * timeline::SHUTDOWN_ABORTED_JOIN_MS;

const _: () = assert!(
    SHUTDOWN_WORST_CASE_MS + SHUTDOWN_DESTROY_RESERVE_MS <= STORE_CLOSE_BUDGET_MS,
    "shutdown_managed_tasks can now outlast RustSdkMatrixClient::\
     kStoreCloseBudgetMs, so the C++ side will delete the store while a Rust \
     writer may still hold it open. Lower a budget above, or raise \
     kStoreCloseBudgetMs in src/matrix/RustSdkMatrixClient.h and this file's \
     STORE_CLOSE_BUDGET_MS with it."
);

/// The selected sync path. Distinct from transient connectivity: a network
/// loss keeps the mode and is reported through the connection state. Only a
/// fatal authentication failure moves to `Failed`.
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

/// Set the sync mode and announce it only when it changes, so the label
/// does not flicker when the loop re-affirms the same mode.
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
        // This handle's media-store key, and only this handle's: a handle
        // being retired must not take its successor's.
        mediastore::forget(self as *const RustClient as usize, &self.store_path);
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

// Reports 1: matrix-sdk is built with `e2e-encryption`. The C++
// CryptoManager gate remains the source of truth for the UI; this only
// unblocks sending into encrypted rooms in RustSdkMatrixClient.
#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee(_client: *mut c_void) -> c_int {
    1
}

#[no_mangle]
pub extern "C" fn mx_rust_version() -> *mut c_char {
    ffi_string(|| Ok(env!("CARGO_PKG_VERSION").to_owned()))
}

/// Set 0700 on a store directory and 0600 on its regular files. Best
/// effort: a filesystem without Unix modes must still open. Called before
/// and after the client opens, since matrix-sdk creates the sqlite files
/// itself with no mode hook.
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

/// The one line a panic may print, built from metadata only.
///
/// It takes no payload by design: Rust's default hook prints the payload,
/// and a `str` slice panic includes the whole string being sliced, which
/// can be a message body. The location is kept because it makes a panic
/// actionable and cannot carry content.
fn panic_report_line(location: Option<(&str, u32)>, thread: Option<&str>) -> String {
    let where_ = match location {
        Some((file, line)) => format!("{file}:{line}"),
        None => "<unknown location>".to_owned(),
    };
    let who = thread.unwrap_or("<unnamed>");
    format!(
        "Rust panic at {where_} on thread {who} \
         (message withheld: a panic message can quote message content; \
         set LIGHTNING_PANIC_PAYLOAD=1 to print it while debugging)"
    )
}

/// True when the developer asked for the stock hook back. An empty value is
/// not a request (`FOO=` is how a shell unsets a variable).
fn panic_payload_requested(value: Option<&str>) -> bool {
    matches!(value, Some(v) if !v.trim().is_empty())
}

/// Replace the default panic hook with one that prints metadata only.
///
/// Idempotent, and does not chain to the previous hook (that is the one that
/// prints the payload). `catch_unwind` is unaffected. A backtrace is still
/// printed when `RUST_BACKTRACE` asks; it carries no payload.
fn install_panic_hook() {
    // Not in tests: `assert_eq!` reports through the panic hook, so installing
    // it would hide every assertion message. `cfg!` keeps the body compiled in
    // both profiles.
    if cfg!(test) {
        return;
    }
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let requested = std::env::var("LIGHTNING_PANIC_PAYLOAD").ok();
        if panic_payload_requested(requested.as_deref()) {
            return;
        }
        std::panic::set_hook(Box::new(|info| {
            let location = info.location().map(|l| (l.file(), l.line()));
            // Bound, not inlined: the name borrows from the `current()` guard.
            let current = std::thread::current();
            eprintln!("{}", panic_report_line(location, current.name()));
            let trace = std::backtrace::Backtrace::capture();
            if trace.status() == std::backtrace::BacktraceStatus::Captured {
                eprintln!("{trace}");
            }
        }));
    });
}

/// Forward matrix-sdk's own `tracing` diagnostics to stderr, once, when
/// asked.
///
/// The SDK reports to-device decryption, Olm sessions and key gossip only
/// through `tracing`. Opt-in via `LIGHTNING_RUST_LOG` (not `RUST_LOG`):
/// `1` selects a preset aimed at key delivery; any other value is an
/// `EnvFilter` directive. The preset keeps crypto at `debug`, which is
/// state and failure reporting, not key material.
fn install_sdk_tracing() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let requested = match std::env::var("LIGHTNING_RUST_LOG") {
            Ok(value) if !value.trim().is_empty() => value,
            _ => return,
        };
        let directives = if requested.trim() == "1" {
            // Key-delivery lanes, plus matrix_sdk_ui at WARN: target directives leave
            // unlisted targets off, and `room_send_queue_update_task` logs
            // "missed {n} local echoes" at WARN on a lagged receiver without resyncing,
            // which is the evidence for a stuck local echo. Info is too chatty.
            "matrix_sdk_crypto=debug,matrix_sdk_base=info,matrix_sdk=info,\
             matrix_sdk_ui=warn"
                .replace(char::is_whitespace, "")
        } else {
            requested
        };
        let filter = match tracing_subscriber::EnvFilter::try_new(&directives) {
            Ok(filter) => filter,
            Err(err) => {
                eprintln!(
                    "LIGHTNING_RUST_LOG is not a valid filter ({err}); \
                     SDK tracing stays off"
                );
                return;
            }
        };
        // `try_init`: a test may have installed one already.
        if tracing_subscriber::fmt()
            .with_env_filter(filter)
            .with_writer(std::io::stderr)
            .try_init()
            .is_ok()
        {
            eprintln!("matrix-sdk tracing enabled: {directives}");
        }
    });
}

#[no_mangle]
pub extern "C" fn mx_rust_create(store_path: *const c_char) -> *mut c_void {
    // First, before anything that could panic.
    install_panic_hook();
    install_sdk_tracing();
    match catch_unwind(AssertUnwindSafe(|| {
        let store_path = unsafe { cstr_arg(store_path) }?;
        let path = PathBuf::from(store_path);
        std::fs::create_dir_all(&path)
            .map_err(|err| format!("failed to create Rust SDK store directory: {err}"))?;
        // 0700 on the directory, 0600 on the databases: this holds the Megolm and
        // device keys, and $HOME being 0700 is not a guarantee. On a fresh login the
        // directory is still empty here; `build_client`'s `sqlite_store` does its
        // own chmod after creating the databases. These calls keep the directory
        // private from the start and fix an existing store's files. Best effort.
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
        let released = retire_bridge(unsafe { Box::from_raw(ptr as *mut RustClient) });
        if !released.closed {
            // Names and counts only, never the path.
            eprintln!(
                "lightning: a retired client left its store open: {}",
                released.summary()
            );
        }
    }));
}

/// `mx_rust_destroy`, reporting whether the account store was really
/// released: `store_closed=true|false waited_ms=N builds=N client_alive=…
/// still_open=<store names>|none runtime_shutdown=completed|timed_out|shared`.
/// Names and counts only, never a path.
/// Used by the retirement worker, which logs it: "retired" was taken as
/// "closed" before, and was not (GitHub #2).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_destroy_and_report(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        if ptr.is_null() {
            return Ok(String::new());
        }
        // `ffi_string` catches a panic, as `mx_rust_destroy` does.
        Ok(retire_bridge(unsafe { Box::from_raw(ptr as *mut RustClient) }).summary())
    })
}

/// Drop a bridge so that, when this returns, nothing of its account store is
/// still open in this process (or say honestly that something is).
///
/// Three things made `drop(Box<RustClient>)` alone insufficient (GitHub #2):
///
/// * Dropping the last `Client` closes its SQLite pools through deadpool,
///   which hands each connection to `tokio::task::spawn_blocking` and
///   unwraps: outside a runtime that panics, during cleanup it aborts. The
///   bridge's fields used to be dropped on the caller's thread, with no
///   runtime, and only a reference cycle that leaked the Client hid it. So
///   the bridge is dropped inside its own runtime's context.
/// * The runtime is then shut down with a bound, which cancels every task
///   still holding a Client (the SDK's E2EE tasks, the media sweep) and waits
///   for the blocking closes those drops queued.
/// * Then the real signal: every store this account's builds opened, and the
///   Client itself, must fail to upgrade (storeclose.rs). Anything held
///   elsewhere (a detached sync thread still unwinding) gets a bounded wait.
///
/// Only this handle's own builds are waited for (`snapshot` by owner, the
/// address `mediastore::set_key`/`forget` use), so a handle that never built a
/// client, such as the one the GUI thread destroys when its session file is
/// refused, or the in-memory OAuth bootstrap handle, does not wait at all.
fn retire_bridge(bridge: Box<RustClient>) -> storeclose::Released {
    let owner = &*bridge as *const RustClient as usize;
    let probes = storeclose::snapshot(&bridge.store_path, owner);
    let runtime = Arc::clone(&bridge.runtime);
    {
        let _context = runtime.enter();
        drop(bridge);
    }
    let shutdown = match Arc::try_unwrap(runtime) {
        Ok(runtime) => {
            let budget = std::time::Duration::from_millis(RETIRE_RUNTIME_SHUTDOWN_MS);
            let started = std::time::Instant::now();
            runtime.shutdown_timeout(budget);
            storeclose::RuntimeShutdown::from_elapsed(started.elapsed(), budget)
        }
        // A login or restore thread still inside `run_async_on` holds it; it
        // drops the runtime when it returns. Until then its blocking jobs can
        // hold connections, so this is reported, never counted as closed.
        Err(shared) => {
            drop(shared);
            storeclose::RuntimeShutdown::Shared
        }
    };
    storeclose::wait_released(
        &probes,
        std::time::Duration::from_millis(RETIRE_STORE_RELEASE_WAIT_MS),
        shutdown,
    )
}

/// `device_id` empty: the server creates a new device. Non-empty: sign in
/// again as that device, which the store at this handle's path must already
/// hold (C++ passes it only after the SDK reported a soft logout for it).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_login(
    ptr: *mut c_void,
    homeserver: *const c_char,
    user: *const c_char,
    password: *const c_char,
    device_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let homeserver = unsafe { cstr_arg(homeserver) }?;
        let user = unsafe { cstr_arg(user) }?;
        let password = unsafe { cstr_arg(password) }?;
        let device_id = unsafe { cstr_arg(device_id) }?;

        bridge.abandon_uia();
        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let session_file = Arc::clone(&bridge.session_file);
        let client_slot = Arc::clone(&bridge.client);
        let token_task = Arc::clone(&bridge.token_task);
        let token_sink = bridge.token_sink();
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        // Shared runtime: the SDK's post-login E2EE initialization task must
        // outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "login", async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
                        // Before the login: every rotation goes through it.
                        token_sink.install_on(&client);
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                            Arc::clone(&sync_notify),
                        );
                        let mut builder = client
                            .matrix_auth()
                            .login_username(&user, &password)
                            .initial_device_display_name("Lightning");
                        if !device_id.is_empty() {
                            builder = builder.device_id(&device_id);
                        }
                        let login = builder.send().await;
                        match login {
                            Ok(response) => {
                                let session = MatrixSession::from(&response);
                                // Resume unsent requests on the shared runtime, so the sync lanes'
                                // set_enabled(true) finds the queues built (it respawns tasks for rooms
                                // with unsent requests, and those lanes run on a throwaway runtime).
                                crate::resume_unsent_requests(&client).await;
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
                                // Refreshable password sessions rotate like OAuth; persist the new pair.
                                if let Ok(mut guard) = token_task.lock() {
                                    if let Some(previous) = guard.replace(
                                        oauth::spawn_token_persistence(
                                            &client, Arc::clone(&events),
                                            Some(Arc::clone(&token_sink.ledger))))
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
                                        // Present only on servers that issue refreshable password sessions; stored
                                        // so a restart can refresh instead of failing with M_UNKNOWN_TOKEN.
                                        "refresh_token": response.refresh_token,
                                    }),
                                );
                            }
                            Err(err) => {
                                // Release SDK/store ownership before C++ reacts to login_failed with a
                                // local reset.
                                drop(client);
                                // A fixed token (HTTP status + errcode, or the transport class) so the
                                // login screen can say "wrong password" in words; never the server's text.
                                let reason = oauth::sdk_failure_reason(&err);
                                enqueue(
                                    &events,
                                    json!({
                                        "type": "login_failed",
                                        "stage": "password",
                                        "reason": reason,
                                        "message": format_matrix_error("Matrix Rust SDK login failed", err),
                                    }),
                                );
                            }
                        }
                    }
                    Err(err) => enqueue(
                        &events,
                        json!({
                            "type": "login_failed",
                            "stage": "password",
                            "reason": "client_build_failed",
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

        bridge.abandon_uia();
        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let active_sas = Arc::clone(&bridge.active_sas);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        let active_qr = Arc::clone(&bridge.active_qr);
        // Shared runtime: the SDK's post-restore E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(
                shared_runtime, runtime_events, "restore_from_file",
                switchtrace::restore_scope(switchtrace::switch_trace_enabled(), async move {
                let stored = match read_persistent_session(&session_file) {
                    Ok(stored) => stored,
                    Err(err) => {
                        enqueue(
                            &events,
                            switchtrace::with_restore_trace(json!({
                                "type": "login_failed",
                                "message": format!("Rust SDK smoke session restore unavailable: {err}"),
                            })),
                        );
                        return;
                    }
                };

                if stored.homeserver != homeserver {
                    enqueue(
                        &events,
                        switchtrace::with_restore_trace(json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session homeserver does not match this run.",
                        })),
                    );
                    return;
                }

                let stored_user = stored.session.meta.user_id.to_string();
                if !expected_user_id.is_empty() && stored_user != expected_user_id {
                    enqueue(
                        &events,
                        switchtrace::with_restore_trace(json!({
                            "type": "login_failed",
                            "message": "Rust SDK smoke session account does not match this run.",
                        })),
                    );
                    return;
                }

                match restore_client_with_session(
                    &homeserver, &store_path, stored.session.clone(), &events, None)
                    .await
                {
                    // The smoke-session path persists no rotations.
                    Ok((client, _changes)) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                            Arc::clone(&sync_notify),
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
                            switchtrace::with_restore_trace(json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": stored_user,
                                "device_id": stored.session.meta.device_id.to_string(),
                            })),
                        );
                    }
                    Err(err) => enqueue(
                        &events,
                        switchtrace::with_restore_trace(json!({
                            "type": "login_failed",
                            "message": err,
                        })),
                    ),
                }
            }));
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

        bridge.abandon_uia();
        bridge.stop_sync_and_wait();
        bridge.enqueue(json!({ "type": "status", "state": "connecting" }));

        let store_path = bridge.store_path.clone();
        let client_slot = Arc::clone(&bridge.client);
        let events = Arc::clone(&bridge.events);
        let active_request = Arc::clone(&bridge.active_request);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        let active_sas = Arc::clone(&bridge.active_sas);
        let active_qr = Arc::clone(&bridge.active_qr);
        // Shared runtime: the SDK's post-restore E2EE initialization task
        // must outlive this call (see run_async_on).
        let shared_runtime = Arc::clone(&bridge.runtime);
        let token_task = Arc::clone(&bridge.token_task);
        let token_sink = bridge.token_sink();
        std::thread::spawn(move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "restore",
                switchtrace::restore_scope(switchtrace::switch_trace_enabled(), async move {
                match restore_client(
                    &homeserver,
                    &store_path,
                    &user_id,
                    &device_id,
                    access_token,
                    refresh_token,
                    &events,
                    Some(&token_sink),
                )
                .await
                {
                    Ok((client, changes)) => {
                        install_event_handlers(
                            &client,
                            Arc::clone(&events),
                            Arc::clone(&active_request),
                            Arc::clone(&active_sas),
                            Arc::clone(&active_qr),
                            Arc::clone(&sync_notify),
                        );
                        // From the receiver taken before restore_session(), so a
                        // rotation the restore itself caused is still persisted.
                        if let Ok(mut guard) = token_task.lock() {
                            if let Some(previous) = guard.replace(
                                oauth::spawn_token_persistence_with(
                                    &client, changes, Arc::clone(&events),
                                    Some(Arc::clone(&token_sink.ledger))))
                            {
                                previous.abort();
                            }
                        }
                        if let Ok(mut guard) = client_slot.lock() {
                            *guard = Some(client);
                        }
                        enqueue(
                            &events,
                            switchtrace::with_restore_trace(json!({
                                "type": "login_ok",
                                "homeserver": homeserver,
                                "user_id": user_id,
                                "device_id": device_id,
                            })),
                        );
                    }
                    Err(err) => enqueue(
                        &events,
                        switchtrace::with_restore_trace(json!({
                            "type": "login_failed",
                            "message": err,
                        })),
                    ),
                }
            }));
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
        bridge.abandon_uia();
        bridge.stop_sync_and_wait();
        // Moved out of the slot, not cloned: the slot's reference must not be
        // dropped here, on the caller's (GUI) thread with no Tokio runtime.
        // If it were the last one, matrix-sdk's SQLite pool would try to close
        // its connections through `spawn_blocking`, which panics outside a
        // runtime (and aborts the process when it happens during cleanup).
        // The logout thread drops it inside its runtime instead.
        let client = bridge.client.lock().ok().and_then(|mut guard| guard.take());
        let events = Arc::clone(&bridge.events);
        // Pending verifications are cancelled in `shutdown_managed_tasks`, which
        // every teardown path runs before this FFI; the slots are empty here.
        if let Some(client) = client {
            std::thread::spawn(move || {
                let runtime_events = Arc::clone(&events);
                // The outcome is held here and announced only after `run_async` has
                // returned, i.e. after this thread's Client clone AND its runtime are
                // gone. matrix-sdk's SQLite pools drop each connection through
                // `spawn_blocking` on the runtime that drops the last reference, so
                // announcing from inside the future let C++ start deleting the store
                // while this thread could still be closing it (Windows: the delete
                // fails on the open file and sign-out reports cleanup_incomplete).
                let outcome: Arc<Mutex<Option<serde_json::Value>>> =
                    Arc::new(Mutex::new(None));
                let outcome_slot = Arc::clone(&outcome);
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
                    drop(client);
                    if let Ok(mut slot) = outcome_slot.lock() {
                        *slot = Some(event);
                    }
                });
                // run_async has returned: its runtime has been dropped, which waits
                // for every blocking close it was running.
                let event = outcome
                    .lock()
                    .ok()
                    .and_then(|mut slot| slot.take())
                    .unwrap_or_else(|| json!({
                        "type": "logged_out",
                        "result": "failed",
                        "message": "Matrix Rust SDK logout task could not complete.",
                    }));
                enqueue(&events, event);
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
        // Switch trace: `since_start_sync_ms` counts from here. None when off.
        let switch_trace_started = switchtrace::sync_start_stamp();
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
        let membership_list_slot = Arc::clone(&room_list_slot);
        let entries_slot = Arc::clone(&bridge.room_list_entries);
        let active_subscription = Arc::clone(&bridge.active_room_subscription);
        let sync_timelines = Arc::clone(&bridge.timelines);
        let sync_media_capable = Arc::clone(&bridge.call_media_capable);
        let sync_verification_live: VerificationLive = {
            let request = Arc::clone(&bridge.active_request);
            let sas = Arc::clone(&bridge.active_sas);
            let qr = Arc::clone(&bridge.active_qr);
            Arc::new(move || flow_slots_are_live(&request, &sas, &qr))
        };
        bridge.enqueue(json!({ "type": "status", "state": "syncing" }));

        let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
        // Never sent on: its drop tells teardown the thread is gone. See
        // SYNC_TASK_JOIN_BUDGET_MS.
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let sync_client = client.clone();
        let sync_search_index = Arc::clone(&bridge.search_index);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                // Sliding sync never reports an invitation rejected on another
                // device (GitHub #28); this asks the server about pending ones
                // and lives exactly as long as the sync session.
                let stale_invites = invites::run_stale_invite_reconciler(
                    sync_client.clone(), Arc::clone(&events),
                );
                // Nor, on some servers (Continuwuity), a room left, kicked or
                // banned from elsewhere: the list just stops naming it.
                let lost_rooms = membership_lost::run_membership_reconciler(
                    sync_client.clone(), Arc::clone(&events), membership_list_slot,
                );
                tokio::select! {
                    _ = switchtrace::sync_scope(switch_trace_started, run_authoritative_sync(
                        sync_client, events, sync_search_index, sync_mode, room_list_slot,
                        entries_slot, active_subscription, sync_timelines,
                        sync_media_capable, sync_verification_live, sync_notify, cancel_rx,
                    )) => {}
                    _ = stale_invites => {}
                    _ = lost_rooms => {}
                }
            });
        });
        *task_slot = Some(SyncTask {
            cancel: Some(cancel),
            thread: Some(thread),
            done: Some(done_rx),
        });
        drop(task_slot);

        // A recovery-key-minting action from an earlier session that never
        // reported (aborted at teardown, or the process died) left its marker.
        // Say so once, through the result the action would have sent: the
        // account may now have secret storage whose key nobody was shown.
        // op_id 0 is no operation of this session's; BackupController accepts
        // it for this category only.
        // The marker stays until the user has been shown the report
        // (mx_rust_acknowledge_interrupted_setup).
        if recovery::interrupted_setup_to_report(
            &bridge.store_path, bridge.bridge_id, bridge.timelines.lifecycle(),
        ) {
            enqueue(
                &bridge.events,
                json!({
                    "type": "backup_action_result", "op_id": 0,
                    "lifecycle": bridge.timelines.lifecycle(),
                    "action": "enable", "ok": false,
                    "category": "setup_interrupted",
                }),
            );
        }

        // Crypto-bootstrap observer: forwards sanitized state names and counts
        // (never key material, session ids or secrets), stamped with the lifecycle
        // so a stale observer cannot update a later account. Stopped in
        // stop_sync_and_wait.
        if let Ok(mut slot) = bridge.bootstrap_task.lock() {
            if slot.is_none() {
                let observer_events = Arc::clone(&bridge.events);
                let timelines = Arc::clone(&bridge.timelines);
                let lifecycle = bridge.timelines.lifecycle();
                let nudges = Arc::clone(&bridge.recovery_nudges);
                let observer_notify = Arc::clone(&bridge.sync_notify);
                let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
                let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
                let thread = std::thread::spawn(move || {
                    let _done = done_tx;
                    let runtime_events = Arc::clone(&observer_events);
                    run_async(runtime_events, "crypto_bootstrap", async move {
                        run_crypto_bootstrap_observer(
                            client, observer_events, timelines, lifecycle,
                            nudges, observer_notify, cancel_rx,
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

/// Per-account E2EE recovery coordinator. Forwards the SDK's
/// verification/recovery/backup state and received-key counts, and drives
/// the recovery steps matrix-sdk 0.18 leaves undone:
///
///   * once verified, one `fetch_exists_on_server` probe reports whether a
///     key backup exists (`backup_exists`);
///   * when backups become usable while verified, the open room gets one
///     deduplicated `download_room_keys_for_room` pass (the SDK's
///     `AfterDecryptionFailure` strategy fetches one key per failing event and
///     never a whole room);
///   * a bounded secret-request retry ladder. The SDK sends m.secret.request
///     once at SAS completion, and a request that reaches the peer before it
///     finished its own completion is refused and never replayed. The ladder
///     issues fresh requests via
///     `OlmMachine::query_missing_secrets_from_other_sessions` (through the
///     `testing` feature accessor, see `Cargo.toml`) and stops when nothing
///     is missing, the own identity is unverified, or it is exhausted;
///   * an `m.secret.send` arrival handler and nudge channel re-drive
///     evaluation when `VerificationState` shows no edge;
///   * received room keys retry decryption in the active room.
///
/// Public matrix-sdk APIs only; the SDK's GossipMachine creates and
/// validates requests and applies its trust checks. Emits kinds, fixed
/// state strings and counts only, never key material.
async fn run_crypto_bootstrap_observer(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    timelines: Arc<timeline::TimelineRegistry>,
    lifecycle: u64,
    nudges: RecoveryNudgeSlot,
    sync_notify: Arc<syncnotify::SyncNotify>,
    mut cancel_rx: tokio::sync::oneshot::Receiver<()>,
) {
    let encryption = client.encryption();
    // Verdicts for late-decrypted events (see decrypted_event_json).
    let decrypted_verdicts = push_verdict::SyncVerdicts::default();
    let mut verification = encryption.verification_state();
    let recovery = encryption.recovery();
    let backups = encryption.backups();
    let mut recovery_states = recovery.state_stream();
    let mut backup_states = backups.state_stream();
    let mut room_keys = encryption.room_keys_received_stream().await;
    // What matrix-sdk's event-cache redecryptor did with arriving keys: how
    // many stored undecryptable events it resolved, and whether it lagged and
    // may have missed some. Counts only. Without it a log cannot tell "the key
    // never arrived" from "the key arrived and nothing was redecrypted", which
    // is the open question behind a thread reply that stayed "Waiting for
    // keys" until a manual Retry (open-items, F4).
    let mut redecryption = Some(Box::pin(
        client.event_cache().subscribe_to_decryption_reports(),
    ));

    // Nudge channel and m.secret.send observer. The handler forwards only the
    // fact that a secret event was decrypted, never its name, value or sender.
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
    // Result of the one-shot backup probe; None = not yet known.
    let mut backup_exists: Option<bool> = None;
    // Secret-request retry ladder. `attempt` indexes SECRET_RETRY_DELAYS_SECS;
    // `retry_deadline` is the next evaluation (None = disarmed). A short
    // `recheck_deadline` follows each m.secret.send so the imported state is
    // re-read after the SDK processed it.
    let mut retry_attempt: usize = 0;
    let mut retry_deadline: Option<tokio::time::Instant> = None;
    let mut recheck_deadline: Option<tokio::time::Instant> = None;
    // Cap on arrival-driven re-arms of an exhausted ladder, so a device spamming
    // m.secret.send cannot keep it alive forever. Reset by every real arming
    // edge.
    const MAX_ARRIVAL_REARMS: u32 = 2;
    let mut arrival_rearms: u32 = 0;

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

    // Already verified with a usable backup at startup: the SDK will not
    // download on its own. Arming the ladder here also heals a session left
    // "verified but secretless" by a previous run.
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
                        // Re-arm the ladder on every Verified edge.
                        retry_attempt = 0;
                        retry_deadline = secret_retry_deadline(retry_attempt);
                        arrival_rearms = 0;
                        // Verification may complete after the backup key is already usable
                        // (manual recovery first), in which case no later Enabled edge comes. The
                        // pass is deduplicated per lifecycle, so repeating it is safe.
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
                        // Terminal for this ladder; a later arming edge starts a new one.
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
                                // Exhausted with secrets still missing: the UI escalates to manual
                                // recovery.
                                "exhausted"
                            },
                            0,
                        );
                    }
                }
            }
            _ = watchdog_sleep(recheck_deadline) => {
                recheck_deadline = None;
                // Re-read SDK state after the secret event was processed.
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
                    // A secret arrived but recovery is still incomplete (refused, or a backup
                    // key for another version). Re-arm one bounded round, capped against
                    // unrequested m.secret.send spam.
                    arrival_rearms += 1;
                    retry_attempt = 1;
                    retry_deadline = secret_retry_deadline(retry_attempt);
                }
            }
            nudge = nudge_rx.recv() => {
                let Some(nudge) = nudge else { continue };
                match nudge {
                    RecoveryNudge::VerificationDone => {
                        // The SDK queued its own one-shot request set; give the peer a window
                        // before supervising with fresh requests. Runs even without a
                        // VerificationState edge.
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
                                // The manual attempt used slot 0; keep the follow-ups armed.
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
                        // Arrival only: the SDK validated and imported (or refused) the secret.
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
                // The backup key became usable: restore the open room's history in one
                // pass (the SDK only fetches keys for events that fail from now on).
                if verified
                    && matches!(
                        state,
                        matrix_sdk::encryption::backups::BackupState::Enabled
                    )
                {
                    run_backup_download_pass(&client, &timelines).await;
                }
            }
            report = async {
                match redecryption.as_mut() {
                    Some(stream) => stream.next().await,
                    None => std::future::pending().await,
                }
            } => {
                use matrix_sdk::event_cache::RedecryptorReport;
                match report {
                    Some(Ok(RedecryptorReport::ResolvedUtds { room_id, events: resolved })) => {
                        emit_crypto_bootstrap(
                            &events, lifecycle, "utds_resolved", "",
                            resolved.len() as u64,
                        );
                        // A reply decrypted here that the open thread view will
                        // never show (matrix-sdk keeps the thread's stale copy;
                        // see the registry method). Counted for the log only.
                        let ids: Vec<String> =
                            resolved.iter().map(|id| id.to_string()).collect();
                        let missing = timelines
                            .redecrypted_replies_missing_from_open_thread(
                                &client, room_id.as_str(), &ids,
                            )
                            .await;
                        if missing > 0 {
                            emit_crypto_bootstrap(
                                &events, lifecycle, "thread_reply_out_of_view", "",
                                missing as u64,
                            );
                        }
                        // matrix-sdk calls sync event handlers only from a sync
                        // response, so an event that decrypts later never reaches
                        // one again: a mention that arrived before its key was never
                        // notified, and the bell never saw it. Each event the sync
                        // handler forwarded as undecryptable is re-emitted ONCE
                        // (take_utd), decrypted, in this lifecycle only; C++ decides
                        // what a decrypted copy changes. Not a decryption of ours:
                        // the redecryptor already replaced the event in the cache.
                        if let Some(room) = client.get_room(&room_id) {
                            for event_id in &resolved {
                                let Some(backlog) = sync_notify.take_utd(&room_id, event_id)
                                else {
                                    continue;
                                };
                                let Some(event) = decrypted_event_json(
                                    &room, event_id, backlog, &decrypted_verdicts,
                                )
                                .await
                                else {
                                    continue;
                                };
                                trace_remote_edit(
                                    "late-decrypted", event_id.as_str(),
                                    event["replaces_event_id"].as_str().unwrap_or_default(), true,
                                );
                                enqueue(&events, json!({
                                    "type": "event_decrypted",
                                    "lifecycle": lifecycle,
                                    "room_id": room_id.to_string(),
                                    "event": event,
                                }));
                            }
                        }
                    }
                    // The SDK's redecryptor missed room keys.
                    Some(Ok(RedecryptorReport::Lagging)) => {
                        emit_crypto_bootstrap(
                            &events, lifecycle, "redecryption_lagging", "", 0,
                        );
                    }
                    // Our own subscriber fell behind and missed reports; the
                    // redecryptor itself may be fine.
                    Some(Err(_)) => {
                        emit_crypto_bootstrap(
                            &events, lifecycle, "redecryption_reports_missed", "", 0,
                        );
                    }
                    Some(Ok(RedecryptorReport::BackupAvailable)) => {}
                    // The event cache is gone; park this arm.
                    None => redecryption = None,
                }
            }
            keys = async {
                match room_keys.as_mut() {
                    Some(stream) => stream.next().await,
                    // No crypto machine: park instead of spinning.
                    None => std::future::pending().await,
                }
            } => {
                let Some(keys) = keys else { break };
                let Ok(infos) = keys else { continue }; // lagged stream
                if !infos.is_empty() {
                    // Counts only.
                    emit_crypto_bootstrap(
                        &events, lifecycle, "room_keys_received", "",
                        infos.len() as u64,
                    );
                    // Retry the active room's undecryptable rows in place (the registry
                    // filters to the open room and deduplicates).
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

/// Probe once whether a key backup exists on the homeserver and emit
/// `backup_exists`. A failed probe emits nothing (the UI stays "unknown").
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

/// Sleep until the armed deadline, or forever when disarmed. Takes the
/// deadline by value so the select! arm does not borrow coordinator state.
async fn watchdog_sleep(deadline: Option<tokio::time::Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending::<()>().await,
    }
}

/// Retry-ladder delays in seconds after each arming edge. The first leaves
/// time for the SDK's own request set; later ones cover a peer that refused
/// an early request. Past the end the coordinator emits
/// `secrets_pending exhausted` and waits for a new arming edge.
const SECRET_RETRY_DELAYS_SECS: [u64; 3] = [20, 90, 240];

/// Delay for the given ladder attempt, or None when exhausted.
fn next_secret_retry_delay(attempt: usize) -> Option<u64> {
    SECRET_RETRY_DELAYS_SECS.get(attempt).copied()
}

fn secret_retry_deadline(attempt: usize) -> Option<tokio::time::Instant> {
    next_secret_retry_delay(attempt).map(|secs| {
        tokio::time::Instant::now() + std::time::Duration::from_secs(secs)
    })
}

/// Is post-verification recovery still incomplete? Cross-signing private
/// keys are always required; the backup key only when the probe said a
/// backup exists.
fn secret_recovery_missing(
    cross_signing_complete: bool,
    backup_exists: Option<bool>,
    backup_enabled: bool,
) -> bool {
    !cross_signing_complete
        || (backup_exists == Some(true) && !backup_enabled)
}

/// Outcome of one secret-recovery attempt. Counts are device counts only.
enum SecretAttemptOutcome {
    /// Nothing is missing anymore; the ladder stops.
    Complete,
    /// The own identity is not verified here, so the SDK would reject any
    /// gossiped answer. A fresh verification is the remedy.
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

/// Evaluate trust/secret state and, when appropriate, queue m.secret.request
/// through the SDK. Emits `own_identity`, `cross_signing_secrets` and
/// `secret_request` bootstrap events (fixed strings and counts).
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

/// Re-read after a secret arrived: emit the refreshed identity/secret state
/// and report whether nothing is missing any more.
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

/// Count this account's other verified devices (those that could answer a
/// secret request). Device ids never leave this function.
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

/// Queue m.secret.request for every missing secret via
/// `OlmMachine::query_missing_secrets_from_other_sessions`, which creates
/// fresh request ids and deduplicates against unsent ones. The accessor
/// needs matrix-sdk's `testing` feature (see rust/Cargo.toml). Returns
/// whether a new request was queued.
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

/// Run the deduplicated backup download pass for the open room, if any.
/// Rooms opened later get their own pass from `open_room_task`.
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

/// Build the receipts for an EXPLICIT "mark as read" (the room list's
/// `mx_rust_mark_room_read`) under a privacy mode (0 public, 1 private
/// `m.read.private`, 2 "do not send read receipts"). Unthreaded on purpose:
/// marking a room read covers its threads too.
///
/// Mode 2 sends a PRIVATE receipt, as Element does with receipts off. It used
/// to send none, and matrix-sdk computes this device's unread counts from the
/// user's own `m.read` / `m.read.private` only (`ALL_RECEIPT_TYPES`), so every
/// room stayed unread on this device after being read; only other members
/// must not see the read position, and a private receipt shows them nothing.
///
/// `m.fully_read` is sent in every mode: it is private account data and
/// carries the user's own read position across devices.
fn receipts_for_mode(event_id: OwnedEventId, mode: i32) -> Receipts {
    match mode {
        1 | 2 => Receipts::new()
            .fully_read_marker(event_id.clone())
            .private_read_receipt(event_id),
        _ => Receipts::new()
            .fully_read_marker(event_id.clone())
            .public_read_receipt(event_id),
    }
}

/// The receipt the AUTOMATIC in-room path sends while the user reads the live
/// timeline: one receipt, on the MAIN thread, never `m.fully_read`.
///
///  * Main, not unthreaded: the live timeline hides threaded events
///    (`hide_threaded_events: true`), and matrix-sdk-ui's own choice for that
///    focus is `ReceiptThread::Main`. An unthreaded receipt at a later
///    main-timeline event marked every older thread reply read on the server
///    and on the user's other devices, without anyone opening the thread.
///  * No `m.fully_read`: matrix-sdk-ui follows it live and drops the "New
///    messages" marker once it reaches the newest event, so moving it with
///    every receipt removed the divider and the "First unread" pill about a
///    second after a room opened. It moves on `mx_rust_set_fully_read_marker`,
///    which C++ calls when the user leaves the room.
///  * Modes 1 and 2 send `m.read.private` (see `receipts_for_mode`).
fn automatic_receipt(
    event_id: OwnedEventId,
    mode: i32,
) -> (
    matrix_sdk::ruma::api::client::receipt::create_receipt::v3::ReceiptType,
    matrix_sdk::ruma::events::receipt::ReceiptThread,
    OwnedEventId,
) {
    use matrix_sdk::ruma::{
        api::client::receipt::create_receipt::v3::ReceiptType,
        events::receipt::ReceiptThread,
    };
    let receipt_type = match mode {
        1 | 2 => ReceiptType::ReadPrivate,
        _ => ReceiptType::Read,
    };
    (receipt_type, ReceiptThread::Main, event_id)
}

/// Privacy comes from `receipt_privacy` (see `automatic_receipt`).
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
            // Both answers name the event the READER was shown (see below); the
            // receipt itself may sit on a reaction after it.
            let answered = event_id.to_string();
            let event_id = in_room_receipt_target(&room, event_id).await;
            let (receipt_type, thread, event_id) = automatic_receipt(event_id, mode);
            // Not retried: the serial lock is held across this call, and the next
            // receipt supersedes this one anyway.
            // Both answers name the event, so C++ can tell this receipt's
            // answer from an older one's (and from mark-as-read's).
            let moved = event_id.as_str() != answered;
            let mut sent = room
                .send_single_receipt(receipt_type.clone(), thread.clone(), event_id)
                .await;
            if moved && sent.is_err() {
                // The server refused the receipt on the reaction (one it cannot
                // place): send the row the reader saw, once, as before #46.
                if let Ok(original) = EventId::parse(&answered) {
                    sent = room.send_single_receipt(receipt_type, thread, original).await;
                }
            }
            match sent {
                Ok(()) => enqueue(&events, json!({
                    "type": "read_marker_advanced", "room_id": room_id,
                    "event_id": answered
                })),
                Err(error) => {
                    let mut event = roomaction::failure_event("read_receipt", &error, 1);
                    event["room_id"] = json!(room_id);
                    event["event_id"] = json!(answered);
                    enqueue(&events, event);
                }
            }
        });
        Ok(String::new())
    })
}

/// Move this account's `m.fully_read` marker (its place in the room, private
/// account data) to `event_id`, with no read receipt. The automatic receipt no
/// longer carries it (see `automatic_receipt`); C++ calls this when the user
/// leaves a room they have read, so the "New messages" divider stays put for
/// the whole visit, as in Element.
///
/// Sent as a bare `/read_markers` request rather than through
/// `send_multiple_receipts`, which also clears the manual "marked unread" flag
/// and would undo a user who marks a room unread and then switches away. Not
/// subject to the receipt privacy mode: nothing in it is visible to anyone
/// else. Answers only on failure, as `room_action_error` with
/// action="fully_read".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_fully_read_marker(
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
        bridge.spawn_room_action(async move {
            use matrix_sdk::ruma::api::client::read_marker::set_read_marker;
            // Never backwards: another device may already have moved the
            // marker past what this one showed. Judged by the two events'
            // timestamps from the local cache; when either is not cached the
            // write goes ahead, as it always did.
            if let Some(current) = current_fully_read(&room).await {
                if current != event_id {
                    let current_ts = cached_event_ts(&room, &current).await;
                    let new_ts = cached_event_ts(&room, &event_id).await;
                    if fully_read_would_move_back(current_ts, new_ts) {
                        return;
                    }
                }
            }
            let mut request = set_read_marker::v3::Request::new(room.room_id().to_owned());
            request.fully_read = Some(event_id);
            if let Err(error) = client.send(request).await {
                let error = matrix_sdk::Error::Http(Box::new(error));
                let mut event = roomaction::failure_event("fully_read", &error, 1);
                event["room_id"] = json!(room_id);
                enqueue(&events, event);
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
            let (result, attempts) = roomaction::set_unread_flag(
                &room, unread != 0, &roomaction::ROOM_ACTION_RETRY_DELAYS).await;
            match result {
                Ok(()) => enqueue_rooms(&events, &client).await,
                Err(error) => {
                    let mut event = roomaction::failure_event("marked_unread", &error, attempts);
                    event["room_id"] = json!(room_id);
                    enqueue(&events, event);
                }
            }
        });
        Ok(String::new())
    })
}

/// Add or remove this room's `m.favourite` tag via `Room::set_is_favourite`,
/// which also drops a conflicting `m.lowpriority` tag (`roomaction::set_favourite`,
/// which retries a transport failure the SDK does not).
///
/// Not optimistic: the room list is re-emitted on success, so a rejected
/// write leaves the row unchanged. A failure carries its reason (status and
/// errcode, or the transport class) so the log can say why.
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
            let (result, attempts) = roomaction::set_favourite(
                &room, favourite != 0, &roomaction::ROOM_ACTION_RETRY_DELAYS).await;
            match result {
                Ok(()) => enqueue_rooms(&events, &client).await,
                Err(error) => {
                    let mut event = roomaction::failure_event("favourite", &error, attempts);
                    event["room_id"] = json!(room_id);
                    enqueue(&events, event);
                }
            }
        });
        Ok(String::new())
    })
}

/// Send attachment bytes to a room whose timeline is not open (forwarding).
/// The timeline-scoped send refuses any room but the open one. Uses
/// `Room::send_attachment`, which encrypts for encrypted rooms.
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

/// Receipt privacy: 0 public, 1 private (`m.read.private`), 2 none.
///
/// Applies to every later receipt from all three paths. Receipts already
/// sent cannot be retracted.
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

/// Where "mark room read" puts the receipt: the newest event we can actually
/// name, in order of trust. `Room::latest_event()` alone is NOT enough: it is
/// only computed for rooms in the capped `LatestEvents` watch set (large
/// accounts and big public rooms fall outside it and read `None`), it stops
/// moving after a lagged receive, and it has no id while a local echo is
/// sending. Marking such a room read then cleared only the manual flag, sent
/// no receipt, and reported success. The event cache's newest event is what
/// sync delivered last; `latest_event` is a stale-prone fallback; a one-event
/// `/messages` page is the last resort.
pub(crate) fn pick_receipt_target(
    cache_newest: Option<OwnedEventId>,
    latest_event: Option<OwnedEventId>,
    fetched_newest: Option<OwnedEventId>,
) -> Option<OwnedEventId> {
    cache_newest.or(latest_event).or(fetched_newest)
}

/// One cached event, as far as choosing a receipt target needs it.
pub(crate) struct TailEvent {
    pub id: OwnedEventId,
    /// `None` for an event the SDK could not decrypt.
    pub kind: Option<String>,
    /// What a reaction annotates / a redaction removes.
    pub target: Option<OwnedEventId>,
    /// Carries an `m.thread` relation: not a main-timeline event.
    pub threaded: bool,
}

/// The event an in-room receipt should name. The reader was shown `chosen`
/// (the newest TIMELINE row), but a reaction after it is not a row and still
/// counts for the server: reactions notify once the user turns them on, and a
/// receipt at the older event never covers them, so the count stayed for good
/// (GitHub #46). `tail` is the cache's events, oldest first. The receipt moves
/// to the last event after `chosen` only when EVERY event there is skippable:
/// a reaction whose target is a known main-timeline event (a Main receipt must
/// not cover a thread event, section 8), or a redaction of such a reaction. A
/// real message, an edit, a poll, an undecryptable event, a reaction to a
/// thread event or to an unknown event, or a `chosen` the cache does not hold,
/// leaves it alone, so nothing the reader has not seen is marked read.
pub(crate) fn receipt_target_past_relations(
    chosen: &EventId,
    tail: &[TailEvent],
) -> OwnedEventId {
    let Some(at) = tail.iter().rposition(|e| e.id.as_str() == chosen.as_str()) else {
        return chosen.to_owned();
    };
    let known_main = |id: &OwnedEventId| {
        tail.iter().any(|e| &e.id == id && !e.threaded && e.kind.is_some())
    };
    let skippable = |e: &TailEvent| match e.kind.as_deref() {
        Some("m.reaction") => e.target.as_ref().is_some_and(known_main),
        Some("m.room.redaction") => e.target.as_ref().is_some_and(|t| {
            tail.iter().any(|x| &x.id == t && x.kind.as_deref() == Some("m.reaction"))
        }),
        _ => false,
    };
    let after = &tail[at + 1..];
    if !after.is_empty() && after.iter().all(skippable) {
        after[after.len() - 1].id.clone()
    } else {
        chosen.to_owned()
    }
}

async fn in_room_receipt_target(room: &Room, chosen: OwnedEventId) -> OwnedEventId {
    let Ok((cache, _drop_handles)) = room.event_cache().await else {
        return chosen;
    };
    let Ok(events) = cache.events().await else {
        return chosen;
    };
    let tail: Vec<TailEvent> = events
        .iter()
        .filter_map(|event| {
            let id = event.event_id()?;
            let raw = event.raw();
            let kind = raw.get_field::<String>("type").ok().flatten();
            let content = raw.get_field::<serde_json::Value>("content").ok().flatten();
            let relates = content.as_ref().and_then(|c| c.get("m.relates_to"));
            let target = relates
                .and_then(|r| r.get("event_id"))
                .and_then(|v| v.as_str())
                .or_else(|| {
                    content.as_ref().and_then(|c| c.get("redacts")).and_then(|v| v.as_str())
                })
                .and_then(|t| EventId::parse(t).ok());
            let target = target.or_else(|| {
                raw.get_field::<String>("redacts")
                    .ok()
                    .flatten()
                    .and_then(|t| EventId::parse(t).ok())
            });
            let threaded = relates
                .and_then(|r| r.get("rel_type"))
                .and_then(|v| v.as_str())
                == Some("m.thread");
            Some(TailEvent { id, kind, target, threaded })
        })
        .collect();
    receipt_target_past_relations(&chosen, &tail)
}

async fn resolve_mark_read_target(room: &Room) -> Option<OwnedEventId> {
    let cache_newest = match room.event_cache().await {
        Ok((cache, _drop_handles)) => cache.events().await.ok().and_then(|events| {
            events.iter().rev().find_map(|event| event.event_id())
        }),
        Err(_) => None,
    };
    let latest = room.latest_event().event_id();
    if cache_newest.is_some() || latest.is_some() {
        return pick_receipt_target(cache_newest, latest, None);
    }
    let mut opts = MessagesOptions::backward();
    opts.limit = uint!(1);
    let fetched = room.messages(opts).await.ok()
        .and_then(|page| page.chunk.iter().find_map(|event| event.event_id()));
    pick_receipt_target(None, None, fetched)
}

/// Mark a room read without opening it. The target comes from
/// `resolve_mark_read_target` (event cache, then `latest_event()`, then a
/// one-event `/messages`), so no timeline is needed; the public receipt and
/// `m.fully_read` are sent together as in the room. With no latest event
/// only the manual unread flag is cleared, as `Timeline::mark_as_read` does.
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
            let delays = &roomaction::ROOM_ACTION_RETRY_DELAYS;
            let latest = resolve_mark_read_target(&room).await;
            // The first failure, with its attempt count; it is what gets reported.
            let mut failure: Option<(matrix_sdk::Error, u32)> = None;
            if let Some(event_id) = latest {
                // Same helper as the in-room path, so this discloses no more.
                let (result, attempts) = roomaction::with_transport_retry(delays, || {
                    let room = room.clone();
                    let receipts = receipts_for_mode(event_id.clone(), mode);
                    async move { room.send_multiple_receipts(receipts).await }
                }).await;
                if let Err(error) = result {
                    failure = Some((error, attempts));
                }
            }
            // Unconditional: a room explicitly marked unread must not stay unread just
            // because it had no event to receipt.
            let (result, attempts) = roomaction::set_unread_flag(&room, false, delays).await;
            if let Err(error) = result {
                failure.get_or_insert((error, attempts));
            }
            match failure {
                None => {
                    enqueue(&events, json!({
                        "type": "read_marker_advanced", "room_id": room_id
                    }));
                    enqueue_rooms(&events, &client).await;
                }
                Some((error, attempts)) => {
                    let mut event = roomaction::failure_event("mark_read", &error, attempts);
                    event["room_id"] = json!(room_id);
                    enqueue(&events, event);
                }
            }
        });
        Ok(String::new())
    })
}

/// Per-room notification-mode integers shared with C++ (SettingsManager /
/// NotificationManager::RoomMode): 0 all messages, 1 mentions & keywords,
/// 2 mute. Mode 0 sets an explicit AllMessages rule.
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

/// True when `mode` is still the newest requested mode for `room_id`.
/// Checked before the server write.
fn is_latest_notification_target(
    targets: &Arc<Mutex<HashMap<String, u8>>>,
    room_id: &str,
    mode: u8,
) -> bool {
    targets.lock().ok().and_then(|guard| guard.get(room_id).copied()) == Some(mode)
}

/// Consume the room's pending-write marker if this task's mode is still the
/// newest after the round-trip; the task then owns the room's report. On
/// false, a newer task queued behind the serial reports instead.
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

/// Clears this task's pending-write marker on any exit path, including a
/// panic, an abort, or the future being dropped unpolled (hence it is
/// created at the FFI entry and moved in). An orphaned marker would keep
/// `notification_write_pending()` true for the rest of the session. Only
/// this task's exact (room, mode) pair is removed.
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

/// True while a set for this room is queued or in flight.
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

/// The session's single NotificationSettings, created lazily. Clones share
/// one rule set, so a read after a write sees the locally applied rules.
///
/// Known limitation: if the first call precedes the initial m.push_rules
/// sync, the SDK's fallback rule set stays cached until a PushRulesEvent
/// arrives.
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

/// Set the per-room notification mode via
/// `NotificationSettings::set_room_notification_mode`; rule construction
/// stays inside matrix-sdk. Success enqueues `room_notification_mode`;
/// failure enqueues `notification_mode_error` with the room id only (the
/// SDK error text can embed rule bodies).
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
        // Created outside the future and moved in, so it still drops if the spawn
        // path declines and the future is never polled.
        let target_guard = NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room_id.clone(),
            mode: my_mode,
        };
        bridge.spawn_room_action(async move {
            let _target_guard = target_guard;
            let _serial = serial.lock().await;
            // Superseded before running: the newest task performs the write.
            if !is_latest_notification_target(&targets, &room_id, my_mode) {
                return;
            }
            let settings = notification_settings_handle(&settings_slot, &client).await;
            let result = settings.set_room_notification_mode(room.room_id(), mode).await;
            // Re-check after the round-trip: if a newer choice was queued meanwhile,
            // report nothing; the newer task reports for the room.
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

/// Real participants of a thread, for the summary-card facepile. Answers
/// with a `thread_participants` event (user id, display name, avatar mxc;
/// never content). See rooms::thread_participants.
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

/// Redact this message's own `m.replace` edits, restoring the original
/// text. Answers with `message_edits_removed` (counts only). See
/// rooms::remove_message_edits.
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

/// "Follow account default": remove the room's user-defined push rules
/// (`delete_user_defined_room_rules`). Matrix has no "follow default" rule,
/// only the absence of an override.
///
/// Shares the set path's serial and target marker (mode 3), so a clear and
/// a set cannot land out of order. Success reports `user_defined: false`.
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
        // C++ RoomMode::FollowDefault.
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

/// Report a room's notification mode: the user-defined room rule when one
/// exists, otherwise the account default for this room's shape, flagged
/// `user_defined: false`. A local rule-set lookup; C++ calls it when a
/// picker opens.
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
            // A write is in flight and will report; a read now could see pre-write
            // rules and enqueue after the write's report, leaving C++ on the stale mode.
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
            // No per-room rule: resolve the default like the SDK's helpers. Unknown
            // encryption maps to NotEncrypted (it only picks the default rule). One-to-
            // one counts joined members, as `.m.rule.room_one_to_one` does server-side.
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

/// Report the mode of EVERY joined room that has a user-defined rule, as
/// `room_notification_mode` events (`user_defined: true`), so a room muted from
/// another client (Element writes an override rule keyed by the room id with
/// no actions) is known here without a picker ever being opened for it. The
/// classification is the SDK's own; nothing is re-derived. A room with a
/// write in flight is skipped, like the single-room read.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_refresh_room_notification_modes(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let client = bridge.client.lock().ok().and_then(|guard| guard.clone())
            .ok_or_else(|| "no active Matrix session".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let targets = Arc::clone(&bridge.notification_mode_targets);
        let settings_slot = Arc::clone(&bridge.notification_settings);
        let timelines = Arc::clone(&bridge.timelines);
        let lifecycle = timelines.lifecycle();
        let watch_slot = Arc::clone(&bridge.notification_watch_lifecycle);
        let watch_abort = Arc::clone(&bridge.notification_watch_abort);
        bridge.spawn_room_action(async move {
            let settings =
                notification_settings_handle(&settings_slot, &client).await;
            // One watcher per session: a push-rules change made elsewhere (a
            // room muted in Element mid-session) asks C++ to re-read. Debounced
            // so a burst of rule writes is one refresh, and it ends with its
            // lifecycle, so a later account never hears an earlier one.
            let start_watcher = watch_slot
                .swap(lifecycle, std::sync::atomic::Ordering::SeqCst) != lifecycle;
            let mut changes = start_watcher.then(|| settings.subscribe_to_changes());
            for room_id in settings.get_rooms_with_user_defined_rules(Some(true)).await {
                if notification_write_pending(&targets, &room_id) {
                    continue;
                }
                let Ok(parsed) = RoomId::parse(&room_id) else { continue };
                if client.get_room(&parsed).is_none() {
                    continue;
                }
                if let Some(mode) = settings
                    .get_user_defined_room_notification_mode(&parsed)
                    .await
                {
                    enqueue(&events, json!({
                        "type": "room_notification_mode",
                        "room_id": room_id,
                        "mode": notification_mode_to_int(mode),
                        "user_defined": true,
                    }));
                }
            }
            let Some(mut changes) = changes.take() else { return };
            // The watcher is its own task holding ONLY the receiver: not the
            // settings (the broadcast sender) and not the client, so it keeps
            // neither alive, and shutdown aborts it through `watch_abort`.
            drop(settings);
            drop(client);
            let watcher = tokio::spawn(async move {
                loop {
                    match changes.recv().await {
                        Ok(()) | Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {}
                        Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
                    }
                    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                    while changes.try_recv().is_ok() {}
                    if !timelines.lifecycle_current(lifecycle) {
                        break;
                    }
                    enqueue(&events, json!({ "type": "push_rules_changed" }));
                }
            });
            if let Ok(mut guard) = watch_abort.lock() {
                if let Some(old) = guard.replace(watcher.abort_handle()) {
                    old.abort();
                }
            }
        });
        Ok(String::new())
    })
}

/// RAII cleanup for a pending invite action: removes the room id on every
/// task exit, including abort and panic.
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
            let _guard = InviteActionGuard {
                pending: Arc::clone(&pending),
                room_id: room_id.clone(),
            };
            enqueue(&events, json!({
                "type": "invite_state_update", "room_id": room_id,
                "action": if accept { "accept" } else { "reject" }, "state": "pending"
            }));
            // A reject the server refuses for an invitation it already
            // dropped (rejected on another device) is retired locally once the
            // server confirms we are out; see invites.rs.
            let result = if accept {
                room.join().await.map_err(|err| err.to_string())
            } else {
                invites::leave_or_retire(&room, || invites::fetch_server_invites(&client))
                    .await
            };
            enqueue(&events, json!({
                "type": "invite_state_update", "room_id": room_id,
                "action": if accept { "accept" } else { "reject" },
                "state": if result.is_ok() { "done" } else { "failed" }
            }));
            if result.is_ok() { enqueue_rooms(&events, &client).await; }
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

/// Room-list reset, called by C++ after it rejects a malformed diff.
///
/// Recovers from the dynamic adapter, which owns the index space:
/// re-setting its filter yields a `VectorDiff::Reset`. A `client.rooms()`
/// snapshot is ordered differently and made the rejection loop forever. The
/// snapshot remains the fallback for classic sync, which has no adapter.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_resync_rooms(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|guard| guard.clone()) else {
            return Err("no active Matrix session".to_owned());
        };
        let entries = bridge
            .room_list_entries
            .lock()
            .ok()
            .and_then(|guard| guard.clone());
        // `set_filter` returns false once its stream is dropped (between sync
        // loops); the snapshot is the answer then.
        if let Some(entries) = entries {
            if entries.set_filter(Box::new(filters::new_filter_non_left())) {
                return Ok(String::new());
            }
        }
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

/// Drain one event from the terminal command lane (media, GIF results). C++
/// empties it before each bulk mx_rust_poll_event batch.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_poll_command_event(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        // The GUI thread is draining: a rotation waiting for its keyring
        // write can expect an answer (sessiontokens::DRAIN_STALL).
        bridge.token_ledger.note_drain();
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
        bridge.spawn_reported_action("send_text", async move {
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

            // matrix-sdk encrypts automatically for encrypted rooms; failures (missing
            // keys, untrusted devices) come back through send_failed. C++ still
            // refuses when CryptoManager::supportsE2ee() is false.
            // An empty `m.mentions` says this message pings nobody, so the
            // legacy display-name and @room body rules cannot fire on it
            // (MSC3952; the composer's sends do the same, timeline.rs).
            let mut content = RoomMessageEventContent::text_markdown(body)
                .add_mentions(matrix_sdk::ruma::events::Mentions::new());
            // Same strict outgoing sanitizer as the composer's lanes: raw HTML
            // and a `javascript:` link would otherwise reach other clients.
            timeline::sanitize_outgoing_formatted(&mut content.msgtype);
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

        Ok(String::new())
    })
}

/// Encrypted-room smoke-test probe. Like mx_rust_send_text but refuses
/// unencrypted rooms. Never sees ciphertext, keys or session material; the
/// returned event id is safe to log.
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
        bridge.spawn_reported_action("probe_encrypted_send", async move {
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

            let content = RoomMessageEventContent::text_plain(body)
                .add_mentions(matrix_sdk::ruma::events::Mentions::new());
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

        Ok(String::new())
    })
}

/// Manual key-backup recovery via `recovery().recover(input)`, which
/// accepts a Base58 recovery key or a recovery passphrase. Never logs the
/// input or any imported key material. Result events:
///   { "type": "key_backup_status", "state": "attempted" }
///   { "type": "key_backup_status", "state": "ok" | "ok_no_backup"
///                                         | "nothing_restored" }
///   { "type": "key_backup_status", "state": "failed", "category": "…" }
///
/// `recover()` returning Ok does not mean anything was restored: a secret
/// storage without a backup key restores no message keys, and one without
/// cross-signing keys verifies nothing. The state says which
/// (`recovery::recover_outcome`). On failure `category`
/// (`recovery::classify_recover_error`) is all that crosses: C++ shows its
/// own translated text for it.
///
/// After recovery the SDK does not download keys if the backup key was
/// already stored, so a download pass for the open room runs here, with
/// its dedup mark cleared first.
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
        bridge.spawn_reported_action("recover_backup", async move {
            enqueue(
                &events,
                json!({ "type": "key_backup_status", "state": "attempted" }),
            );
            let encryption = client.encryption();
            // Whether this session's own device is cross-signed by its owner,
            // the question matrix-sdk's `verification_state()` asks. Read after
            // the call on BOTH arms: `import_secrets()` signs this device before
            // it touches the key backup (secret_store.rs:403-461), so a failure
            // in the backup step arrives with the session already verified.
            // Read BEFORE as well, so "now verified" is said only when this
            // call did it, not for a session verified earlier by SAS.
            let verified_before = own_session_cross_signed(&client).await;
            match encryption.recovery().recover(&recovery_key).await {
                Ok(_) => {
                    let backup_usable = encryption.backups().are_enabled().await;
                    let identity_held = own_session_cross_signed(&client).await;
                    // Forced post-recover download for the open room. This is the only place
                    // that can re-run a pass a room already had. Visible rows are re-decrypted
                    // indirectly via `room_keys_received_stream` ->
                    // `retry_decryption_after_import` (active room only).
                    if backup_usable {
                        if let Some(room_id) = timelines.active_room_id() {
                            timelines.clear_backup_attempt(&room_id);
                            timelines
                                .download_backup_keys_for_room(
                                    &client, &room_id,
                                )
                                .await;
                        }
                    }
                    enqueue(
                        &events,
                        json!({
                            "type": "key_backup_status",
                            "state": recovery::recover_outcome(
                                backup_usable, identity_held,
                            ),
                        }),
                    );
                }
                Err(err) => {
                    let category = recovery::classify_recover_error(&err);
                    let session_verified = own_session_cross_signed(&client).await;
                    enqueue(
                        &events,
                        json!({
                            "type": "key_backup_status",
                            "state": "failed",
                            // Category only. The SDK's text is English, and a key
                            // that fails to decode is quoted in it (`DecodeError`
                            // prints the offending character, prefix or parity
                            // byte), so it is neither shown nor logged.
                            "category": category,
                            "session_verified": session_verified,
                            "became_verified": !verified_before && session_verified,
                        }),
                    );
                }
            }
        });
        Ok(String::new())
    })
}

/// Room keys in the store and how many of them the current backup version
/// holds. Counts only. Reached through `olm_machine_for_testing` (matrix-sdk's
/// `testing` feature, see rust/Cargo.toml): a read-only call, like the
/// secret-request site.
async fn backup_room_key_counts(client: &Client) -> Option<(u64, u64)> {
    let machine = client.olm_machine_for_testing().await;
    let counts = machine.as_ref()?.backup_machine().room_key_counts().await.ok()?;
    Some((counts.backed_up as u64, counts.total as u64))
}

/// Whether this session's own device is cross-signed by its owner: the
/// question matrix-sdk's `verification_state()` asks (never
/// `Device::is_verified()`, a constant true for our own device).
async fn own_session_cross_signed(client: &Client) -> bool {
    client
        .encryption()
        .get_own_device()
        .await
        .ok()
        .flatten()
        .is_some_and(|device| device.is_cross_signed_by_owner())
}

/// The user has been shown a `setup_interrupted` report (sent at sync start by
/// `recovery::interrupted_setup_to_report`): remove the marker. Until then it
/// stays, so a report lost on the way is made again by the next session.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_acknowledge_interrupted_setup(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Ok(bridge) = unsafe { bridge(ptr) } {
            recovery::acknowledge_interrupted_setup(&bridge.store_path);
        }
    }));
}

/// Reload a room's timeline via Room::messages, emitting the same
/// `timeline_event` shape as the live handlers (C++ dedupes by event_id).
/// Ciphertext is never forwarded; undecryptable rows emit an empty body and
/// undecryptable=true. Summary events:
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
        bridge.spawn_reported_action("reload_timeline", async move {
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
                    // Backward chunks are newest-first; reverse to match live ordering.
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

                        let (is_encrypted, undecryptable, msgtype, body,
                             media_filename) =
                            if type_str == "m.room.encrypted" {
                                undecryptable_count += 1;
                                (true, true, "encrypted".to_owned(),
                                 String::new(), String::new())
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
                                // A msgtype with no typed row falls back to plain text. Media rows here
                                // are kind-only (no mxc, mimetype or size); this smoke-test path has no
                                // callers, and anything reviving it must fill those fields.
                                let kind = typed_message_row_kind(mt)
                                    .unwrap_or("text");
                                let fname = media_filename_for_kind(
                                    kind,
                                    &bd,
                                    content
                                        .and_then(|c| c.get("filename"))
                                        .and_then(|v| v.as_str()),
                                );
                                let kind = kind.to_owned();
                                if is_decrypted {
                                    decrypted += 1;
                                }
                                (is_decrypted, false, kind, bd, fname)
                            } else {
                                // Live sync handles state and other event types.
                                continue;
                            };

                        enqueue(&events, json!({
                            "type": "timeline_event",
                            "room_id": room_id.clone(),
                            "event": {
                                "event_id": event_id,
                                "sender": sender,
                                "body": body,
                                "media_filename": media_filename,
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
        Ok(String::new())
    })
}

/// Emitted once per flow when the SDK reports `SasState::Confirmed`, so the
/// UI can acknowledge "They match" while the peer has not confirmed yet.
/// Flow id only.
fn verification_sas_confirmed_event(flow_id: &str) -> serde_json::Value {
    json!({
        "type": "verification_sas_confirmed",
        "flow_id": flow_id,
    })
}

/// SAS poll cadence. Polls the request, which owns the flow's current
/// `Sas`, so one code path covers both.
const VERIFICATION_POLL_MS: u64 = 500;
/// Bounded wait for an `m.key.verification.start` to land when neither
/// side has produced a SAS yet (60 s).
const SAS_HANDSHAKE_TICKS: u32 = 120;
/// Bounded wait for a started SAS to reach the emoji screen (120 s): the
/// start/accept/key exchange, which needs no human.
const SAS_COMPLETION_TICKS: u32 = 240;
/// How long the emoji screen may stay up before this driver gives up (10
/// minutes, matrix-sdk-crypto's own VERIFICATION_TIMEOUT, so the SDK cancels
/// first anyway). Separate from SAS_COMPLETION_TICKS because this half waits
/// on two people comparing and confirming on two devices, each confirmation
/// crossing a sync hop that measured 30 s on a classic-sync server: a 120 s
/// budget counted from SAS start left about 40 s to compare, and it
/// cancelled a good verification with `m.timeout` while the user was still
/// reading.
const SAS_COMPARE_TICKS: u32 = 1200;

/// The SAS driver's tick budget: SAS_COMPLETION_TICKS for the machine half,
/// re-armed ONCE to SAS_COMPARE_TICKS when the emojis are shown.
struct SasTickBudget {
    left: u32,
    compare_armed: bool,
}

impl SasTickBudget {
    fn new() -> Self {
        Self { left: SAS_COMPLETION_TICKS, compare_armed: false }
    }

    /// Spend one tick; false once the budget is exhausted.
    fn tick(&mut self) -> bool {
        if self.left == 0 {
            return false;
        }
        self.left -= 1;
        true
    }

    /// The emojis are on screen: the human half gets its own budget, counted
    /// from now. Idempotent, so a re-derived SAS cannot extend it.
    fn emojis_shown(&mut self) {
        if !self.compare_armed {
            self.compare_armed = true;
            self.left = SAS_COMPARE_TICKS;
        }
    }
}
/// Bounded wait for the peer to answer our request (5 minutes), shorter
/// than the SDK's 10-minute VERIFICATION_TIMEOUT.
const VERIFICATION_PEER_TICKS: u32 = 600;
/// Wire budget for a cancel sent outside teardown (`drive_qr_flow`'s
/// completion timeout). Teardown uses `SHUTDOWN_FLOW_CANCEL_MS`.
const VERIFICATION_CANCEL_TIMEOUT_SECS: u64 = 3;
/// How long a displayed QR code waits to be scanned before falling back to
/// SAS (120 s).
const QR_DISPLAY_TICKS: u32 = 240;
/// How long the flow may take after the peer scanned (240 s). Twice the SAS
/// budget: the user has to read a result on another device and come back.
const QR_COMPLETION_TICKS: u32 = 480;
/// Sanity bound on QR module count; version 40 is 177 modules per side.
const QR_MAX_MODULES: usize = 200;

/// The verification methods advertised in both directions:
///
/// * `m.sas.v1`: emoji, the universal fallback.
/// * `m.qr_code.show.v1`: we display a QR code.
/// * `m.reciprocate.v1`: the start a scanning peer sends back; required
///   for `show` to be answerable.
///
/// `m.qr_code.scan.v1` is omitted: Lightning cannot scan, and a peer
/// displaying a code for us would wait out the SDK's 10-minute timeout.
/// Reciprocate works only with the `qrcode` feature enabled (see
/// rust/Cargo.toml); without it the SDK has no `ReciprocateV1` arm.
fn advertised_verification_methods() -> Vec<VerificationMethod> {
    vec![
        VerificationMethod::SasV1,
        VerificationMethod::QrCodeShowV1,
        VerificationMethod::ReciprocateV1,
    ]
}

/// Pack a QR module grid (`true` = dark) into row-major bits, MSB first,
/// each row starting on a fresh byte (`stride = (size + 7) / 8`), base64
/// encoded.
///
/// Only this geometry crosses the FFI. The payload encodes cross-signing
/// key material and the flow secret, so decoded bytes are never logged,
/// persisted or put in error text.
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

/// Encode bytes as a QR code and return the (size, packed-bits) pair the UI
/// draws. Also used for MSC4108 login codes; C++ has no QR encoder.
pub(crate) fn render_qr_bytes(bytes: &[u8]) -> Option<(usize, String)> {
    use matrix_sdk_base::crypto::matrix_sdk_qrcode::qrcode::{EcLevel, QrCode};
    // Lowest error correction: the payload is large and read off a nearby
    // screen, so redundancy only makes the code denser.
    let code = QrCode::with_error_correction_level(bytes, EcLevel::L).ok()?;
    let size = code.width();
    let modules: Vec<bool> =
        code.to_colors().into_iter().map(|c| c.select(true, false)).collect();
    pack_qr_modules(&modules, size).map(|bits| (size, bits))
}

/// Render an SDK `QrVerification` to the (size, packed-bits) pair the UI
/// needs. Returns `None` if the SDK could not encode the code at all.
fn render_qr_payload(qr: &QrVerification) -> Option<(usize, String)> {
    // `EncodingError` is not user-actionable and SAS still runs. It carries no
    // payload bytes.
    let code = qr.to_qr_code().ok()?;
    let size = code.width();
    // `Color::select` avoids naming `qrcode::Color`, which matrix-sdk does not
    // re-export.
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

/// Ask the SDK for a QR code to display and publish its module grid.
/// `None` is a normal outcome: SAS remains available.
async fn maybe_generate_qr(
    request: &VerificationRequest,
    flow_id: &str,
    events: &Arc<Mutex<VecDeque<String>>>,
) -> Option<QrVerification> {
    // Only generate a code while the request is Ready. `generate_qr_code()` is
    // allowed from `Transitioned`, and it ends in `VerificationCache::insert`,
    // which cancels every other verification with this user; if the peer sent
    // `.ready` and `.start` in one poll window, that would kill the SAS it just
    // started. In Ready nothing is installed yet. This narrows the race but
    // cannot close it, and is not unit-testable (`VerificationRequest` has
    // crate-private constructors).
    if !matches!(request.state(), VerificationRequestState::Ready { .. }) {
        return None;
    }
    // Only when the peer can scan. The SDK enforces this too (returns
    // `Ok(None)`); the `Ok(None)` arm below is the real safety net.
    let peer_can_scan = request
        .their_supported_methods()
        .is_some_and(|methods| methods.contains(&VerificationMethod::QrCodeScanV1));
    if !peer_can_scan {
        return None;
    }
    let qr = match request.generate_qr_code().await {
        Ok(Some(qr)) => qr,
        // The account has no cross-signing identity or no master key. A fresh
        // session on a cross-signed account still gets a code
        // (`new_self_no_master`).
        Ok(None) => return None,
        // Not user-actionable; SAS still runs.
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
/// Polls instead of consuming `qr.changes()`: when the peer answers with an
/// SAS start, the SDK replaces the request's `Verification` and the
/// `QrVerification` just stops changing, so a stream would hang.
///
/// Reports success only on `QrVerificationState::Done`, and never confirms
/// on the user's behalf: `Scanned` waits for
/// `mx_rust_confirm_qr_verification`.
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
            cancel_flow_best_effort(
                None, Some(qr), Some(request),
                std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
            )
            .await;
            return QrOutcome::ShuttingDown;
        }
        tokio::time::sleep(poll).await;

        // One snapshot per tick, with no await inside, so nothing SDK-owned is held
        // across a suspend point.
        let state = qr.state();

        // Request-level terminations (`Passive`, when another of our sessions
        // answered, or a late cancel) are invisible to `qr.state()`.
        //
        // `request.is_done()` is deliberately not checked: the SDK writes the
        // request's Done before the QR's Done (after an awaited signing round), so
        // it would report a successful verification as cancelled. Cancel and
        // passive are written into the QR synchronously, so checking the QR
        // verdict first is race-free.
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
            // The peer scanned our code. Confirming is the user's decision; that human
            // check is the security value of showing a code.
            QrVerificationState::Scanned => {
                if !emitted_scanned {
                    emitted_scanned = true;
                    enqueue(events, json!({
                        "type": "verification_qr_scanned",
                        "flow_id": flow_id,
                    }));
                }
            }
            // Confirmation registered; the SDK is finishing the signature exchange.
            QrVerificationState::Confirmed => {
                if !emitted_confirmed {
                    emitted_confirmed = true;
                    enqueue(events, json!({
                        "type": "verification_qr_confirmed",
                        "flow_id": flow_id,
                    }));
                }
            }
            // Only reachable on the scanning side, which we never are.
            QrVerificationState::Reciprocated => {}
            QrVerificationState::Started => {
                // The peer chose emoji instead. The SDK allows switching while the QR is
                // in `Started`, and the SAS driver adopts the installed Sas.
                if request_moved_to_sas(request) {
                    enqueue(events, json!({
                        "type": "verification_qr_dismissed",
                        "flow_id": flow_id,
                        "reason": "peer_started_sas",
                    }));
                    return QrOutcome::FallBackToSas;
                }
                display_ticks += 1;
                // Bounded display: fall through to SAS so emoji verification remains the
                // guaranteed outcome when the peer neither scans nor starts SAS.
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

        // Past `Started` the spec no longer permits switching to SAS; bound the
        // leg so an unresponsive peer fails visibly.
        progress_ticks += 1;
        if progress_ticks >= QR_COMPLETION_TICKS {
            // Tell the peer before giving up: it already scanned our code and would
            // otherwise wait out the SDK's 10-minute VERIFICATION_TIMEOUT.
            // (`drive_sas_flow`'s completion timeout still returns without a cancel.)
            cancel_flow_best_effort(
                None, Some(qr), Some(request),
                std::time::Duration::from_secs(VERIFICATION_CANCEL_TIMEOUT_SECS),
            )
            .await;
            enqueue(events, json!({
                "type": "verification_failed",
                "flow_id": flow_id,
                "message": "Timed out waiting for QR verification to complete.",
            }));
            return QrOutcome::Finished;
        }
    }
}

/// Drive a request the peer has answered (`.ready` exchanged) to a terminal
/// state, preferring show-QR with SAS as fallback. Used by both directions.
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
            // Release the QR slot before SAS takes over, so a cancel during the SAS
            // leg does not target a retired QR.
            QrOutcome::FallBackToSas => release_keyed_slot(qr_slot, flow_id),
        }
    }
    drive_sas_flow(client, request, flow_id, events, sas_slot, nudges, shutdown).await;
}

/// Best-effort, bounded cancellation of a flow, so the peer is not left
/// waiting out the SDK's 10-minute `VERIFICATION_TIMEOUT`. Both levels are
/// attempted; each SDK cancel is idempotent.
///
/// The budget is the caller's: teardown passes `SHUTDOWN_FLOW_CANCEL_MS`,
/// other callers `VERIFICATION_CANCEL_TIMEOUT_SECS`.
async fn cancel_flow_best_effort(
    sas: Option<&SasVerification>,
    qr: Option<&QrVerification>,
    request: Option<&VerificationRequest>,
    budget: std::time::Duration,
) {
    if let Some(sas) = sas {
        if !sas.is_cancelled() && !sas.is_done() {
            let _ = tokio::time::timeout(budget, sas.cancel()).await;
        }
    }
    // A displayed QR is a live flow too; the peer may be scanning it.
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

/// The SAS this request is currently using, from SDK state. Authoritative
/// over any cached handle: on simultaneous starts the SDK's tie-break
/// replaces the losing `Sas` (`receive_start` -> `replace_sas`), and a
/// handle captured earlier never changes state again.
fn sas_from_request(request: &VerificationRequest) -> Option<SasVerification> {
    match request.state() {
        VerificationRequestState::Transitioned { verification } => verification.sas(),
        _ => None,
    }
}

/// True once the SDK has moved this request onto SAS. The QR driver's
/// hand-off signal: a peer SAS start while our QR is in `Started` makes
/// `receive_start` replace the QR verification with a `Sas`.
fn request_moved_to_sas(request: &VerificationRequest) -> bool {
    sas_from_request(request).is_some()
}

/// Can this flow still make progress? A trait only so the single-flow slot
/// rules can be unit tested; the SDK types have crate-private constructors.
trait FlowLiveness {
    fn is_finished(&self) -> bool;
}

/// Which flow a slot occupant belongs to, for compare-and-clear.
trait FlowIdentity {
    fn flow_key(&self) -> &str;
}

impl FlowLiveness for VerificationRequest {
    fn is_finished(&self) -> bool {
        // `is_passive`: another of our sessions answered this request.
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

/// Where an incoming request stands while it waits for the user's Accept
/// (break-it B015).
#[derive(Debug, Clone, PartialEq, Eq)]
enum PendingRequest {
    /// Still `Requested`: the dialog's Accept is live.
    Waiting,
    /// Left `Requested` some other way (accepted here): a driver owns it now.
    Answered,
    /// Cancelled by the requester (or answered by another of our sessions)
    /// before anyone accepted it; the `message` for `verification_cancelled`.
    Cancelled(String),
}

/// A trait only so the watch rules can be unit tested (crate-private SDK
/// constructors, as `FlowLiveness`).
trait PendingRequestView: FlowIdentity {
    fn pending(&self) -> PendingRequest;
}

/// The `message` a cancelled request reports: the SDK's reason in the same
/// `{:?}` form `drive_sas_flow` uses, or "cancelled" when another of our
/// sessions answered it (`is_passive`), as `drive_qr_flow` reports that.
fn request_cancel_message(request: &VerificationRequest) -> String {
    match request.state() {
        VerificationRequestState::Cancelled(info) => format!("{:?}", info.reason()),
        _ => "cancelled".to_owned(),
    }
}

impl PendingRequestView for VerificationRequest {
    fn pending(&self) -> PendingRequest {
        if self.is_cancelled() || self.is_passive() {
            PendingRequest::Cancelled(request_cancel_message(self))
        } else if matches!(self.state(), VerificationRequestState::Requested { .. }) {
            PendingRequest::Waiting
        } else {
            PendingRequest::Answered
        }
    }
}

/// One tick of the pending-request watch. `Some(message)` when the request
/// in `slot` for `flow_id` was cancelled while still waiting: the slot is
/// cleared (compare-and-clear, so a newer request is never evicted) and the
/// caller reports it. `None` with `keep_watching` false when the slot no
/// longer holds this flow or a driver has taken it over.
fn pending_request_step<R: PendingRequestView>(
    slot: &Mutex<Option<R>>,
    flow_id: &str,
) -> (bool, Option<String>) {
    let Ok(mut guard) = slot.lock() else { return (false, None) };
    let state = match guard.as_ref() {
        Some(request) if request.flow_key() == flow_id => request.pending(),
        _ => return (false, None),
    };
    match state {
        PendingRequest::Waiting => (true, None),
        PendingRequest::Answered => (false, None),
        PendingRequest::Cancelled(message) => {
            // Noted BEFORE the slot is cleared, under its lock: an Accept that
            // finds the slot empty afterwards also finds the note.
            note_pending_cancelled(flow_id);
            *guard = None;
            (false, Some(message))
        }
    }
}

/// Flows the pending-request watch cancelled and released, newest last,
/// bounded. Flow ids are random per request, so one list serves every bridge.
fn pending_cancelled_flows() -> &'static Mutex<VecDeque<String>> {
    static FLOWS: std::sync::OnceLock<Mutex<VecDeque<String>>> = std::sync::OnceLock::new();
    FLOWS.get_or_init(Default::default)
}

const PENDING_CANCELLED_REMEMBERED: usize = 16;

fn note_pending_cancelled(flow_id: &str) {
    let mut flows = pending_cancelled_flows().lock().unwrap_or_else(|e| e.into_inner());
    if flows.len() >= PENDING_CANCELLED_REMEMBERED {
        flows.pop_front();
    }
    flows.push_back(flow_id.to_owned());
}

/// What Accept answers when the slot holds no request (break-it B015 review):
/// nothing when the watch cancelled this very flow (its `verification_cancelled`
/// is already queued, and an error now would put a second, misleading failure
/// over the cancel), the old error otherwise.
fn accept_without_request_reply(flow_id: &str) -> String {
    let flows = pending_cancelled_flows().lock().unwrap_or_else(|e| e.into_inner());
    if flows.iter().any(|flow| flow == flow_id) {
        String::new()
    } else {
        "error: no active verification request.".to_owned()
    }
}

/// How long an unanswered incoming request is watched: the SDK's own
/// `VERIFICATION_TIMEOUT` is 10 minutes.
const PENDING_REQUEST_WATCH_SECS: u64 = 11 * 60;

/// Watch an incoming request while it sits in `Requested`, so a cancel from
/// the requester closes the responder's dialog at once instead of leaving
/// Accept live until it fails as a handshake timeout (break-it B015).
/// Holds no Client: it reads the request through the slot each tick, and
/// teardown empties the slot (`take_pending_flows`), which ends it.
async fn watch_pending_request(
    slot: Arc<Mutex<Option<VerificationRequest>>>,
    events: Arc<Mutex<VecDeque<String>>>,
    flow_id: String,
) {
    let poll = std::time::Duration::from_millis(VERIFICATION_POLL_MS);
    let deadline =
        tokio::time::Instant::now() + std::time::Duration::from_secs(PENDING_REQUEST_WATCH_SECS);
    while tokio::time::Instant::now() < deadline {
        tokio::time::sleep(poll).await;
        let (keep_watching, cancelled) = pending_request_step(&slot, &flow_id);
        if let Some(message) = cancelled {
            enqueue(&events, json!({
                "type": "verification_cancelled",
                "flow_id": flow_id,
                "message": message,
            }));
        }
        if !keep_watching {
            return;
        }
    }
}

/// A flow slot keyed by flow id, for handles with no `flow_id()` accessor.
type KeyedFlowSlot<T> = Arc<Mutex<Option<(String, T)>>>;

/// Clear a keyed slot if its occupant can no longer progress, and report
/// whether anything live is left.
fn keyed_slot_is_live<T: FlowLiveness>(slot: &KeyedFlowSlot<T>) -> bool {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(_, flow)| flow.is_finished()) {
            *guard = None;
        }
        return guard.is_some();
    }
    false
}

/// Clear a keyed slot only if it still holds this flow.
fn release_keyed_slot<T>(slot: &KeyedFlowSlot<T>, flow_id: &str) {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(stored, _)| stored == flow_id) {
            *guard = None;
        }
    }
}

/// True when the single-flow slots still hold a live flow. Dead occupants
/// are cleared in passing, so an abandoned request cannot block later
/// attempts.
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
    // Evaluate both slots: each call also sweeps a dead occupant, and `||`
    // would skip the QR sweep.
    let sas_live = keyed_slot_is_live(sas_slot);
    let qr_live = keyed_slot_is_live(qr_slot);
    let request_live = request_slot.lock().map(|g| g.is_some()).unwrap_or(false);
    request_live || sas_live || qr_live
}

/// Release the single-flow slots only where they still hold this flow, so
/// a newer request that arrived meanwhile is not evicted.
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
/// Teardown must take before clearing: the slots are the only handle for
/// cancelling a flow the peer is still waiting on.
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

/// Releases this flow's slots when its driver ends for any reason,
/// including early returns and panics. Otherwise a leaked `active_request`
/// makes `mx_rust_start_own_verification` refuse until restart.
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

/// Drive a ready request through SAS to a terminal state. Used by both
/// directions.
///
/// Emits `verification_sas_started`, `verification_sas_ready` (emoji +
/// decimals), `verification_sas_confirmed`, then `verification_done` /
/// `verification_cancelled` / `verification_failed`. Success only on
/// `SasState::Done`.
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

    // Someone must send `m.key.verification.start`; matrix-sdk does not, and
    // two waiting peers park in Ready. `start_sas()` is not idempotent (in
    // Transitioned it creates a second, competing Sas), so adopt the SDK's
    // existing Sas first.
    let sas: Option<SasVerification> = match sas_from_request(request) {
        Some(sas) => Some(sas),
        None => match request.start_sas().await {
            Ok(Some(sas)) => Some(sas),
            // `Ok(None)`: the peer lacks `m.sas.v1` or the request left Ready. Give an
            // in-flight start a bounded chance to land, then fail visibly.
            Ok(None) => {
                let mut found: Option<SasVerification> = None;
                for _ in 0..SAS_HANDSHAKE_TICKS {
                    if shutdown.load(Ordering::SeqCst) {
                        // No SAS yet, but the peer is waiting on the request.
                        cancel_flow_best_effort(
                            None, None, Some(request),
                            std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
                        )
                        .await;
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
        // A request the peer had already cancelled never reaches SAS; that is a
        // cancel, not a handshake timeout (break-it B015).
        if request.is_cancelled() || request.is_passive() {
            enqueue(events, json!({
                "type": "verification_cancelled",
                "flow_id": flow_id,
                "message": request_cancel_message(request),
            }));
        } else {
            fail("Timed out waiting for SAS handshake.".to_owned());
        }
        return;
    };

    // A peer-started SAS needs our `accept` before keys are exchanged.
    // `Sas::accept()` is a no-op outside Started, i.e. when we started it.
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
    let mut budget = SasTickBudget::new();
    while budget.tick() {
        if shutdown.load(Ordering::SeqCst) {
            // Shutting down mid-flow: cancel both levels. The teardown slot sweep only
            // covers flows no driver owns.
            cancel_flow_best_effort(
                Some(&sas), None, Some(request),
                std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
            )
            .await;
            return;
        }
        tokio::time::sleep(poll).await;

        // Re-derive every tick: after a tie-break the SDK's Sas is the live one.
        // Keeping the slot in step makes confirm/mismatch act on it.
        if let Some(current) = sas_from_request(request) {
            sas = current;
            if let Ok(mut guard) = sas_slot.lock() {
                *guard = Some((flow_id.to_owned(), sas.clone()));
            }
        }

        // Drop the SasState snapshot before any await.
        let needs_accept = {
            match sas.state() {
                // Our start lost to the peer's; accepting leaves Started, so no spin.
                SasState::Started { .. } => true,
                SasState::KeysExchanged { .. } if !emitted_emojis => {
                    emitted_emojis = true;
                    budget.emojis_shown();
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
                // Our confirmation registered; Done still needs the peer's MAC. Surface it
                // once so the UI can leave the emoji screen.
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

/// Accept an incoming SAS verification request and drive it to a terminal
/// state. Emits `verification_ready` once `.ready` is exchanged, then hands
/// off to `drive_sas_flow`. Slots are released by `FlowSlotGuard` on every
/// exit path.
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
            return Ok(accept_without_request_reply(&flow_id));
        };
        if request.flow_id() != flow_id {
            return Ok("error: verification flow id mismatch.".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            // Nothing can drive this flow; release the slot so it cannot refuse later
            // attempts.
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

            // Cancelled before the click landed (the watcher reports it within a
            // tick; this closes the gap between).
            if request.is_cancelled() || request.is_passive() {
                enqueue(&events, json!({
                    "type": "verification_cancelled",
                    "flow_id": flow_id,
                    "message": request_cancel_message(&request),
                }));
                return;
            }
            // Advertise our own method list rather than `accept()`'s SDK default, so a
            // future SDK adding `m.qr_code.scan.v1` cannot make us claim a scanner.
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
        // Joinable: `confirm()` uploads a signature and writes the crypto store,
        // so it must not outlive sign-out.
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

/// The user confirmed that the other device reported a successful scan.
/// Never issued automatically: `drive_qr_flow` emits
/// `verification_qr_scanned` and waits for this call. The SDK performs the
/// trust change (`QrVerification::confirm`).
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
        // `confirm_scanning()` returns None outside `Scanned`, which would swallow
        // the confirm silently. Refuse visibly; this is reachable by an ordinary
        // race.
        if !matches!(qr.state(), QrVerificationState::Scanned) {
            return Ok("error: the code has not been scanned yet.".to_owned());
        }
        let events = Arc::clone(&bridge.events);
        // Joinable, like the SAS actions (see above).
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
    // Cancel at every level. Each SDK cancel is idempotent, and an
    // `active_sas` from another flow must not suppress the request-level
    // cancel. Closing the dialog over a QR code must tell the peer too.
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
            // Report and release only if the cancel applied to this flow, so a newer
            // request's handle is not evicted.
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

/// Start an outbound verification of this session against another session
/// of the same account, advertising `advertised_verification_methods`.
///
/// Emits `verification_request_started` once sent, `verification_ready`
/// when the peer answers, then hands off to `drive_ready_request`, the same
/// driver as the incoming path.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_own_verification(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        // Reject a duplicate only if a flow is actually live; a dead parked
        // request (incoming requests occupy the slot without user action) must not
        // block verification.
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

            // Look up the own identity locally, forcing a /keys/query if missing
            // (as Element does).
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

            // Claim the slot only if nothing live took it while our request was in
            // flight. Losing the race cancels our request, never theirs.
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

            // Wait up to 5 minutes for the peer. Watch the state, not `is_ready()`:
            // that is true only while in Ready, and a `.start` arriving in the same
            // sample window moves the request to Transitioned for good.
            let mut ready = false;
            for _ in 0..VERIFICATION_PEER_TICKS {
                if shutdown.load(Ordering::SeqCst) {
                    // Withdraw our pending request.
                    cancel_flow_best_effort(
                        None, None, Some(&request),
                        std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
                    )
                    .await;
                    return;
                }
                tokio::time::sleep(
                    std::time::Duration::from_millis(VERIFICATION_POLL_MS),
                ).await;
                match request.state() {
                    // Both mean the peer answered; the driver adopts the peer's SAS itself.
                    VerificationRequestState::Ready { .. }
                    | VerificationRequestState::Transitioned { .. } => {
                        ready = true;
                        break;
                    }
                    // Passive (answered by another of our sessions) maps to
                    // Cancelled(Accepted) in the SDK, so it ends here.
                    VerificationRequestState::Done
                    | VerificationRequestState::Cancelled(_) => break,
                    VerificationRequestState::Created { .. }
                    | VerificationRequestState::Requested { .. } => {}
                }
            }
            if !ready {
                // Distinguish a cancellation from a timeout.
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

/// Report the cross-signing state of the current session. Aggregate,
/// non-secret metadata only.
///
/// `device_cross_signed` drives this session's "Verified" label.
/// `Device::is_verified()` is deliberately not reported: for our own device
/// it is always true, because matrix-sdk marks it locally trusted on
/// creation.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_query_own_device_status(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        // A status query: run on a short-lived runtime and return synchronously.
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
            // The identity-key check needs a /keys/query and must not block the GUI
            // thread; it lives in mx_rust_check_own_identity_key.
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

/// Does the curve25519 identity key this device publishes match the local
/// Olm account's key?
///
/// If not, peers encrypt to a key we cannot read: no room keys and no call
/// media keys ever decrypt, while sending still works. A fresh sign-in is
/// the remedy. `curve25519_key()` is the local key, which a `/keys/query`
/// cannot overwrite.
///
/// Tri-state: `None` when the question could not be answered (offline,
/// keys not uploaded, no local account), which is not a fault. Only
/// `Some(false)` is.
pub(crate) fn identity_key_agreement(
    local_base64: Option<&str>,
    published: Option<&str>,
) -> Option<bool> {
    // Compare without base64 padding: a server may re-pad what we uploaded,
    // and a false mismatch would push a healthy user toward a sign-out.
    fn unpadded(value: &str) -> &str {
        value.trim_end_matches('=')
    }
    match (local_base64, published) {
        (Some(local), Some(published))
            if !local.is_empty() && !published.is_empty() =>
        {
            Some(unpadded(local) == unpadded(published))
        }
        _ => None,
    }
}

/// Ask the server what it publishes for this device and compare with the
/// local Olm key. Every failure to get an answer is `None`.
///
/// The server's answer is not signature-checked on purpose: in the real
/// fault the published ed25519 key also belongs to the lost Olm account,
/// so a signature check would reject the true positive. The risk from a
/// hostile server is bounded: the result only raises a card whose offered
/// action is an ordinary, user-confirmed sign-out.
async fn own_identity_key_agreement(client: &Client) -> Option<bool> {
    use matrix_sdk::ruma::api::client::keys::get_keys;

    let user = client.user_id().map(|u| u.to_owned())?;
    let this_device = client.device_id().map(|d| d.to_owned())?;
    let local = client.encryption().curve25519_key().await?.to_base64();

    let mut request = get_keys::v3::Request::new();
    request.device_keys.insert(user.clone(), vec![this_device.clone()]);
    let response = client.send(request).await.ok()?;
    let published = response
        .device_keys
        .get(&user)
        .and_then(|devices| devices.get(&this_device))
        .and_then(|raw| raw.deserialize().ok())
        .and_then(|keys| {
            keys.keys
                .iter()
                .find(|(id, _)| id.as_str().starts_with("curve25519:"))
                .map(|(_, value)| value.to_owned())
        });

    identity_key_agreement(Some(local.as_str()), published.as_deref())
}

/// What the server says about a RESUMED device (a password sign-in that asked
/// for the soft-logged-out device again): "matches" when it publishes this
/// store's key, "missing" when it answered and publishes no keys for the
/// device, "different" when it publishes another key, "unknown" when it did
/// not answer or the answer could not be read. `answer` is None for no
/// answer, Some(None) for "answered, nothing for this device".
///
/// Unlike identity_key_agreement, "answered, nothing published" is a verdict
/// here: a resumed device's keys went up with its earlier session, and a
/// device removed and re-created under the same id has none, which this
/// store (keys already marked uploaded) would never send again. Pure and
/// unit-tested.
pub(crate) fn resumed_device_key_state(
    local_base64: Option<&str>,
    answer: Option<Option<&str>>,
) -> &'static str {
    let Some(local) = local_base64.filter(|local| !local.is_empty()) else {
        return "unknown";
    };
    match answer {
        None => "unknown",
        Some(None) => "missing",
        Some(Some(published)) => match identity_key_agreement(Some(local), Some(published)) {
            Some(true) => "matches",
            Some(false) => "different",
            None => "unknown",
        },
    }
}

/// The server's answer for this device's curve25519 key: None when it did
/// not answer (or reported a failure, or the entry could not be read),
/// Some(None) when it answered with no keys for the device.
async fn published_curve25519_key(client: &Client) -> Option<Option<String>> {
    use matrix_sdk::ruma::api::client::keys::get_keys;

    let user = client.user_id().map(|u| u.to_owned())?;
    let this_device = client.device_id().map(|d| d.to_owned())?;
    let mut request = get_keys::v3::Request::new();
    request.device_keys.insert(user.clone(), vec![this_device.clone()]);
    let response = client.send(request).await.ok()?;
    if !response.failures.is_empty() {
        return None;
    }
    let Some(raw) = response
        .device_keys
        .get(&user)
        .and_then(|devices| devices.get(&this_device))
    else {
        return Some(None);
    };
    let keys = raw.deserialize().ok()?;
    // An entry without a curve25519 key is not an answer that can be read.
    keys.keys
        .iter()
        .find(|(id, _)| id.as_str().starts_with("curve25519:"))
        .map(|(_, value)| Some(value.to_owned()))
}

/// For a resumed device, after its sync has started: whether the server still
/// publishes this store's key for it. On "missing" or "different" the session
/// is also logged out on the server: that device is the one the resume
/// re-created under the old id, without keys, and removing it leaves the
/// server as the other client left it, so every later restore (a restart
/// included) meets a hard M_UNKNOWN_TOKEN rather than a silent keyless
/// session. The store is untouched. Answers on the poll queue as
///   { "type": "resumed_device_key", "user_id", "device_id",
///     "state": "matches"|"missing"|"different"|"unknown",
///     "ended_on_server": bool }
/// Public identifiers and the verdict only; no key material crosses the FFI.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_check_resumed_device_key(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            let user = client.user_id().map(ToString::to_string).unwrap_or_default();
            let device = client.device_id().map(ToString::to_string).unwrap_or_default();
            let local = client
                .encryption()
                .curve25519_key()
                .await
                .map(|key| key.to_base64());
            let answer = published_curve25519_key(&client).await;
            let state = resumed_device_key_state(
                local.as_deref(),
                answer.as_ref().map(|published| published.as_deref()),
            );
            let ended_on_server = if state == "missing" || state == "different" {
                client.matrix_auth().logout().await.is_ok()
            } else {
                false
            };
            enqueue(&events, json!({
                "type": "resumed_device_key",
                "user_id": user,
                "device_id": device,
                "state": state,
                "ended_on_server": ended_on_server,
            }));
        });
        Ok(String::new())
    })
}

/// Async form of the check above, answering on the poll queue as
///   { "type": "own_identity_key", "matches_server": true|false|null }
/// so the GUI thread never blocks on `/keys/query`. One check per call;
/// rate limiting is the caller's (OwnDeviceKeyWatch). No key material
/// crosses the FFI.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_check_own_identity_key(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        bridge.spawn_room_action(async move {
            let matches = own_identity_key_agreement(&client).await;
            if matches == Some(false) {
                // eprintln so it is visible without LIGHTNING_RUST_LOG. No key material.
                eprintln!(
                    "matrix.crypto: this device's published identity key does \
                     not match its local account, so nothing encrypted to it \
                     can be decrypted; signing out and in again is the only \
                     repair"
                );
            }
            enqueue(&events, json!({
                "type": "own_identity_key",
                "matches_server": matches,
            }));
        });
        Ok(String::new())
    })
}

/// One async E2EE health snapshot from SDK state APIs, emitted as
/// `crypto_health`:
///   { device_id, device_cross_signed,
///     own_identity_available, own_identity_verified,
///     has_master, has_self_signing, has_user_signing,
///     backup_exists_on_server, backup_state, recovery_state,
///     secret_storage_enabled, lifecycle }
/// Booleans, enum names and the public device id only.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_query_crypto_health(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        let uia_slot = Arc::clone(&bridge.uia_pending);
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
            let mut device_cross_signed = false;
            if let Ok(Some(device)) = client.encryption().get_own_device().await {
                device_cross_signed = device.is_cross_signed_by_owner();
            }
            let (mut has_master, mut has_self_signing, mut has_user_signing) =
                (false, false, false);
            if let Some(status) = client.encryption().cross_signing_status().await {
                has_master = status.has_master;
                has_self_signing = status.has_self_signing;
                has_user_signing = status.has_user_signing;
            }
            // Every private key here but our device not cross-signed: an
            // unconfirmed setup, or a reset the server never accepted (the SDK
            // saves the new keys before uploading). One fresh own-keys query
            // drops local keys the server does not hold, and the snapshot is
            // read again from what it left.
            // Never while a cross-signing job holds the UIA slot: a reset
            // parked at its password or OAuth approval is in exactly this
            // state, and the query would delete the keys it is about to upload.
            // Break-it B014: see `own_identity_refetch_wanted`.
            let seen = [
                own_identity_available,
                own_identity_verified,
                device_cross_signed,
                has_master,
                has_self_signing,
                has_user_signing,
            ];
            let refetch = match &user_id {
                Some(uid) => own_identity_refetch_wanted(
                    seen,
                    uia::health_may_realign(&uia_slot),
                    || own_identity_refetch_claim(uid, seen),
                ),
                None => false,
            };
            if refetch {
                if let Some(uid) = &user_id {
                    if client.encryption().request_user_identity(uid).await.is_ok() {
                        own_identity_available = false;
                        own_identity_verified = false;
                        if let Ok(Some(identity)) =
                            client.encryption().get_user_identity(uid).await
                        {
                            own_identity_available = true;
                            own_identity_verified = identity.is_verified();
                        }
                        if let Ok(Some(device)) = client.encryption().get_own_device().await {
                            device_cross_signed = device.is_cross_signed_by_owner();
                        }
                        if let Some(status) = client.encryption().cross_signing_status().await {
                            has_master = status.has_master;
                            has_self_signing = status.has_self_signing;
                            has_user_signing = status.has_user_signing;
                        }
                        own_identity_refetch_record(
                            uid,
                            [
                                own_identity_available,
                                own_identity_verified,
                                device_cross_signed,
                                has_master,
                                has_self_signing,
                                has_user_signing,
                            ],
                        );
                    }
                }
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
            // Tri-state, not unwrap_or(false): `exists_on_server()` is a real request,
            // and reporting a failure as "no backup" invites the user to mint a new
            // recovery key over the existing one. Null means unknown.
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

/// Minimum spacing between the health snapshot's own-identity refetches for
/// one account while its local state has not moved (break-it B014).
const OWN_IDENTITY_REFETCH_MIN: std::time::Duration = std::time::Duration::from_secs(20);

/// Per account: when the health snapshot last refetched our own keys, and the
/// local state that fetch left.
fn own_identity_refetches(
) -> &'static std::sync::Mutex<HashMap<OwnedUserId, (std::time::Instant, [bool; 6])>> {
    static REFETCHES: std::sync::OnceLock<
        std::sync::Mutex<HashMap<OwnedUserId, (std::time::Instant, [bool; 6])>>,
    > = std::sync::OnceLock::new();
    REFETCHES.get_or_init(Default::default)
}

/// Whether a refetch is due: never fetched, the local state moved since the
/// last one left it (secrets arrived, a verification finished), or the
/// spacing has passed. Pure for tests.
fn own_identity_refetch_due(
    last: Option<(std::time::Instant, [bool; 6])>,
    seen: [bool; 6],
    now: std::time::Instant,
) -> bool {
    match last {
        None => true,
        Some((at, left)) => left != seen || now.duration_since(at) >= OWN_IDENTITY_REFETCH_MIN,
    }
}

/// Whether the health snapshot refetches our own keys, from what it `seen`
/// (`[own identity available, own identity verified, device cross-signed,
/// has master, has self-signing, has user-signing]`). Never unless
/// `may_realign` (a cross-signing job holding the UIA slot would lose the keys
/// it is about to upload). All three private keys present with this device
/// not cross-signed is an unconfirmed setup and is refetched every time, as
/// before. Break-it B014: our identity missing or unverified, or this device
/// not cross-signed, is also what a STALE store looks like when another
/// session set up cross-signing or verified this one and sync's own-user
/// device-list change never reached us; Refresh and the verification-done
/// refresh then re-read the same store. So those refetch too, but only when
/// `claim` (the per-account throttle) grants it, since a fresh account without
/// cross-signing stays in that state. Only a re-read: nothing is trusted or
/// signed here. Pure for tests.
fn own_identity_refetch_wanted(
    seen: [bool; 6],
    may_realign: bool,
    claim: impl FnOnce() -> bool,
) -> bool {
    let [available, verified, device_cross_signed, master, self_signing, user_signing] = seen;
    if !may_realign {
        return false;
    }
    let unconfirmed_setup = master && self_signing && user_signing && !device_cross_signed;
    let suspect_stale = !available || !verified || !device_cross_signed;
    unconfirmed_setup || (suspect_stale && claim())
}

/// Claims a refetch for `user` when one is due, stamping it now so that
/// concurrent snapshots do not all fetch.
fn own_identity_refetch_claim(user: &UserId, seen: [bool; 6]) -> bool {
    let now = std::time::Instant::now();
    let mut map = own_identity_refetches().lock().unwrap_or_else(|e| e.into_inner());
    if !own_identity_refetch_due(map.get(user).copied(), seen, now) {
        return false;
    }
    map.insert(user.to_owned(), (now, seen));
    true
}

/// Records the local state a refetch left, so the next snapshot is throttled
/// against it rather than against what it saw before.
fn own_identity_refetch_record(user: &UserId, left: [bool; 6]) {
    let mut map = own_identity_refetches().lock().unwrap_or_else(|e| e.into_inner());
    map.insert(user.to_owned(), (std::time::Instant::now(), left));
}

/// "Request keys again": nudge the recovery coordinator to run one
/// secret-request attempt now and re-arm its ladder. Progress arrives as
/// `crypto_bootstrap` events.
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

/// Import an encrypted Megolm key export via `Encryption::import_room_keys`.
/// The SDK holds the passphrase in `Zeroizing`. Only counts and public room
/// ids are returned to C++, never key material.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_import_room_keys(
    ptr: *mut c_void,
    file_path: *const c_char,
    passphrase: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let file_path = unsafe { cstr_arg(file_path) }?;
        // matrix-sdk wraps the passphrase in Zeroizing before decrypting; this is
        // the only other copy we keep.
        let passphrase = unsafe { cstr_arg(passphrase) }?;

        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
        if file_path.trim().is_empty() {
            return Ok("error: The selected file path is empty.".to_owned());
        }

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
        // A managed task on the shared runtime, so sign-out can join it.
        let handle = bridge.runtime.spawn(async move {
                enqueue(&events, json!({
                    "type": "room_key_import_started",
                }));
                let path_buf = PathBuf::from(&file_path);
                // Pre-flight for clearer errors (e.g. a directory was picked).
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
                // The passphrase is never logged; matrix-sdk holds the Zeroizing copy.
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
                        // Retry decryption right away; the timeline emits in-place Set diffs.
                        // Session ids stay inside Rust; only counts and room ids cross the FFI.
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
// Live SDK timeline FFI. Functions return "" when accepted for async
// execution or "error: …" synchronously. Results arrive as timeline_reset /
// timeline_diff / timeline_pagination / timeline_send_failed /
// timeline_retry_decryption / timeline_error events, stamped with
// room_generation + lifecycle for stale-callback rejection.
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
        // Subscribe before building the timeline. The sliding-sync room list uses
        // a timeline limit of 1, so an unopened room's cache holds one or two
        // events; the room subscription raises it to 20. Applying it first gets
        // that in flight while the timeline builds, saving fill round trips. It is
        // not awaited.
        if let Ok(parsed) = OwnedRoomId::try_from(room_id.as_str()) {
            if let Ok(mut guard) = bridge.active_room_subscription.lock() {
                guard.open(parsed);
            }
            apply_room_subscription(bridge);
        }
        bridge.timelines.open_room(&bridge.runtime, client, room_id.clone());
        Ok(String::new())
    })
}

/// Re-open the active room's live timeline after letting the event cache
/// drop the paginated backlog (like Element's `jumpToLiveTimeline()`). Used
/// only for an explicit jump to the newest message. Emits `timeline_reset`
/// with a new room generation and a `trimmed_from` count.
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
        // The room subscription is per room, so a reload leaves it as is.
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close();
        // The room stays warm: only the active marker goes.
        if let Ok(mut guard) = bridge.active_room_subscription.lock() {
            guard.close_active();
        }
        apply_room_subscription(bridge);
        Ok(String::new())
    })
}

/// (Re)apply the single active-room subscription to the running sliding
/// sync, if any. Fire-and-forget: it only affects what sync delivers.
/// `subscribe_to_rooms` replaces the whole set, so repeated calls converge.
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
        .and_then(|mut guard| guard.pending(false));
    // Unchanged membership: the SDK already holds exactly this set.
    let Some(desired) = desired else { return };
    // Managed: the future holds the RoomListService and through it a strong
    // Client, which must not keep the store open past sign-out.
    bridge.spawn_room_action(async move {
        let refs: Vec<&matrix_sdk::ruma::RoomId> =
            desired.iter().map(|r| r.as_ref()).collect();
        service.subscribe_to_rooms(&refs).await;
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

// ── Read-only event context view ────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn mx_rust_context_open(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        if room_id.trim().is_empty() || event_id.trim().is_empty() {
            return Err("empty room or event id".to_owned());
        }
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        bridge.timelines.open_context(&bridge.runtime, client, room_id, event_id);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_context_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close_context();
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_context_paginate(
    ptr: *mut c_void,
    room_id: *const c_char,
    event_id: *const c_char,
    forward: bool,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let event_id = unsafe { cstr_arg(event_id) }?;
        bridge
            .timelines
            .paginate_context(&bridge.runtime, room_id, event_id, forward)
            .map(|_| String::new())
    })
}

// ── SDK-backed thread timelines ─────────────────────────────────────────

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

/// Rename one of the account's devices via PUT /devices/{id} (no UIA).
/// Answers on `device_renamed {op_id, ok, category}`; C++ refetches the
/// list on success.
#[no_mangle]
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
        bridge.spawn_reported_action("rename_device", async move {
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
        Ok(String::new())
    })
}

/// Key-backup management, entirely through the SDK's recovery/backup flows:
///   * "enable": Recovery::enable(): creates secret storage and the backup,
///     uploads room keys, returns the new recovery key;
///   * "create_backup": Recovery::enable_backup(), when secret storage
///     exists;
///   * "reset_key": Recovery::reset_key(): a new recovery key replaces the
///     old one; returns it;
///   * "disable_and_delete": Backups::disable_and_delete(): deletes the
///     server-side backup version (local store untouched);
///   * "disable_recovery": Recovery::disable(): removes secret storage and
///     the backup.
/// The recovery key crosses the FFI once, in `backup_action_result`, for
/// display; it is never logged or persisted here. Upload progress rides
/// `backup_progress`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_backup_action(
    ptr: *mut c_void,
    action: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let action = unsafe { cstr_arg(action) }?;
        // Cross-signing setup/reset take the user's explicit decision about an
        // existing recovery key, so they have their own entry point
        // (mx_rust_cross_signing_action) and are refused here.
        if uia::cross_signing_kind_for_action(&action).is_some() {
            return Err("cross-signing actions go through mx_rust_cross_signing_action"
                .to_owned());
        }
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
        let store_path = bridge.store_path.clone();
        // The key is reported as soon as it exists. `enable()` without
        // `wait_for_backups_to_upload()` returns right after secret storage is
        // created and only triggers the room-key upload, which carries on in the
        // SDK's backup task and is reported through `backup_progress` (the card
        // asks for it after every result). Waiting for the upload put minutes of
        // teardown exposure (`SHUTDOWN_ACTION_JOIN_MS` aborts this task on an
        // account switch or quit) between "secret storage exists" and "the user
        // has the key", with nothing reported.
        //
        // No inner timeout, for the same reason. What remains of that window
        // (the secret-storage requests themselves) is covered by the marker:
        // a minting action that never reports leaves it behind, and the next
        // session reports it (`recovery::interrupted_setup_to_report`). The
        // marker is written when the task actually runs, so an action that
        // never started (a dropped spawn) is not reported as interrupted.
        let mints_key = matches!(action.as_str(), "enable" | "reset_key");
        bridge.spawn_reported_action("backup_action", async move {
            let marker = mints_key.then(|| recovery::SetupMarker::start(&store_path));
            let encryption = client.encryption();
            let recovery = encryption.recovery();
            let backups = encryption.backups();
            // Ok(Some(key)) when a fresh recovery key was minted.
            let result: Result<Option<String>, String> = match action.as_str() {
                "enable" => recovery
                    .enable()
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
                // Nobody is left to show the outcome to: the marker stays, and
                // the next session says the setup was interrupted.
                return;
            }
            if let Some(marker) = marker {
                marker.finish();
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
                        // Category only; the message could carry account details.
                        "category": rooms::classify_room_error(&message),
                    }),
                ),
            }
        });
        Ok(String::new())
    })
}

/// Cross-signing setup ("setup_cross_signing") or last-resort reset
/// ("reset_cross_signing"), through the UIA layer (uia.rs): the server may
/// ask for the account password (`uia_required`) or, on an OAuth account, for
/// a browser approval (`cross_signing_approval`). Ends in exactly one
/// `backup_action_result`.
///
/// `recovery_key` (may be empty) is the user's CURRENT recovery key or
/// passphrase: when secret storage exists the new keys are stored under it and
/// it keeps working. `replace_recovery_key` is 1 only after the user
/// explicitly confirmed that a new recovery key replaces one they do not
/// have. With secret storage present and neither given, the job refuses with
/// `recovery_key_required` before any key is created. The key is scrubbed
/// after use and never logged.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_cross_signing_action(
    ptr: *mut c_void,
    action: *const c_char,
    recovery_key: *const c_char,
    replace_recovery_key: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let action = unsafe { cstr_arg(action) }?;
        let recovery_key = unsafe { cstr_arg(recovery_key) }?;
        uia::start_cross_signing(bridge, &action, recovery_key, replace_recovery_key != 0, op_id)
            .map(|_| String::new())
    })
}

/// Snapshot of the room-key upload state for the backup card. Counts only.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_backup_progress(
    ptr: *mut c_void,
    request_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let events = Arc::clone(&bridge.events);
        let lifecycle = bridge.timelines.lifecycle();
        let timelines = Arc::clone(&bridge.timelines);
        bridge.spawn_reported_action("backup_progress", async move {
            use matrix_sdk::encryption::backups::UploadState;
            let backups = client.encryption().backups();
            let state = format!("{:?}", backups.state()).to_lowercase();
            let steady = backups.wait_for_steady_state();
            let mut progress = steady.subscribe_to_progress();
            // One bounded observation: the current upload state if reported promptly,
            // otherwise the backup state alone.
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
            // Outside an upload the stream carries no counts, and right after
            // `enable()` (which no longer waits for the upload) the upload may
            // not have started: "idle" with every key still local. The store's
            // own counts say what is really backed up, so sign-out still warns.
            let (backed_up, total) = if upload == "uploading" {
                (backed_up, total)
            } else {
                backup_room_key_counts(&client).await.unwrap_or((backed_up, total))
            };
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            enqueue(
                &events,
                json!({
                    "type": "backup_progress", "lifecycle": lifecycle,
                    "request_id": request_id,
                    "backup_state": state, "upload_state": upload,
                    "backed_up": backed_up, "total": total,
                }),
            );
        });
        Ok(String::new())
    })
}

/// List the account's devices: the server device list (name, last seen)
/// merged with the crypto store's per-device trust. Emits `device_list`
/// with presentation-safe fields only, never keys, signatures or tokens.
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
                let is_current = device.device_id == own_device_id;
                devices.push(json!({
                    "device_id": device_id,
                    "display_name": device.display_name.unwrap_or_default(),
                    "last_seen_ts": device
                        .last_seen_ts
                        .map(|ts| u64::from(ts.get()))
                        .unwrap_or(0),
                    "last_seen_ip": device.last_seen_ip.unwrap_or_default(),
                    "is_current": is_current,
                    "has_crypto_identity": has_crypto_identity,
                    "verified": verified,
                    "cross_signed": cross_signed,
                    // The one flag every trust label on the Sessions page reads.
                    "trusted": recovery::session_row_trusted(
                        is_current, verified, cross_signed,
                    ),
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

/// Manual "Retry decryption" for the open room and its thread panel. One
/// bounded pass per call.
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

// ── Thread list, follow state, threaded read ────────────────────────────

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
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        bridge
            .timelines
            .mark_thread_read(&bridge.runtime, room_id, root,
                              bridge.receipt_privacy.load(Ordering::SeqCst), op_id)
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
                // Servers without MSC4306 and network failures both surface as
                // unsupported; the UI hides the control.
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

/// Follow/unfollow a thread (MSC4306). Emits thread_subscription_result,
/// then a fresh thread_subscription_state on success.
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
                // Manual subscription (no `automatic` event id): an explicit Follow.
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
    // Empty for a room message, the root event id for a thread reply: the edit
    // must be issued on the timeline that holds the event.
    thread_root_id: *const c_char,
    target_event_id: *const c_char,
    new_body: *const c_char,
    mention_user_ids: *const c_char,
    body_spec: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let thread_root_id = unsafe { cstr_arg(thread_root_id) }?;
        let target = unsafe { cstr_arg(target_event_id) }?;
        let new_body = unsafe { cstr_arg(new_body) }?;
        let mentions = unsafe { cstr_list_arg(mention_user_ids) }?;
        let spec = timeline::parse_body_spec(&unsafe { cstr_opt_arg(body_spec) }?)?;
        bridge
            .timelines
            .edit(
                &bridge.runtime,
                room_id,
                thread_root_id,
                target,
                new_body,
                mentions,
                spec,
            )
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

/// Optional string FFI argument: NULL is the empty string, not an error.
unsafe fn cstr_opt_arg(value: *const c_char) -> Result<String, String> {
    if value.is_null() {
        return Ok(String::new());
    }
    unsafe { cstr_arg(value) }
}

/// Parse an optional newline-separated list argument. NULL or empty yields
/// an empty list; entries are trimmed and blanks dropped.
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

/// Vote on an MSC3381 poll. `answer_ids` is newline-separated; an empty
/// list retracts the vote. Empty `thread_root_id` targets the room timeline.
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

/// End an MSC3381 poll. The UI offers it for own polls; receivers enforce
/// the permission rules.
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

/// Create an MSC3381 poll. `answers` is newline-separated (2..=20 after
/// trimming); `undisclosed` != 0 hides tallies until the end;
/// `max_selections` is clamped to >= 1.
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

/// Cancel a local echo not yet sent, via `SendHandle::abort` (also aborts a
/// media upload). Success emits nothing; the SDK's CancelledLocalEvent
/// removes the row through the normal diff path.
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
// Room management, user search and the media bridge. Thin wrappers: logic
// lives in `rooms.rs`. `op_id` values come from C++ and are echoed back.
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

/// Rooms this account and `user_id` are both joined to, from cached
/// membership only (no request), so rooms with unsynced members are
/// omitted; see profile::mutual_rooms.
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

/// Upload and set the account's own avatar from a local file.
/// Result event: own_avatar_result { op_id, lifecycle, ok, error }. The path
/// is not echoed back (it contains the user's home directory).
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

/// Set or clear the account's display name. An empty `name` clears it
/// (`None`; the SDK picks the MSC4133 or v3 endpoint). Result event:
/// own_display_name_result { op_id, lifecycle, ok, error }.
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

/// This account's display name in one room. Empty clears the override.
/// Answers on `room_profile_result`.
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

/// This account's avatar in one room. Empty clears the override.
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

/// Read a profile banner (MSC4427 over MSC4133): `m.banner_url`, falling
/// back to Commet's `chat.commet.profile_banner`. Result event:
/// `profile_banner { op_id, lifecycle, user_id, mxc, supported }`;
/// `supported: false` (no extended profile fields) renders as nothing, not
/// as "no banner".
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

/// Upload an image and set it as this account's banner under both the
/// stable and Commet field names. An empty path clears both. Result event:
/// `profile_banner_set { op_id, lifecycle, ok, mxc, category }`.
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

/// Read a user's display-name colour (`org.lightning.name_color`, MSC4133).
/// Result event: `name_color { op_id, lifecycle, user_id, color, supported }`
/// with `color` as `#rrggbb` or empty. Validated here because another
/// client wrote it and it ends up on a QML colour property.
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

/// Set this account's display-name colour; an empty value clears it.
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

/// Read a profile bio (MSC4440 over MSC4133): `m.biography`, falling back
/// to `gay.fomx.biography`. Result event: `profile_bio { op_id, lifecycle,
/// user_id, bio, supported }`; `supported: false` renders as nothing.
///
/// Plain text only: rendering the optional HTML form would fetch remote
/// media chosen by the profile owner. See `bio.rs`.
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

/// Set this account's bio under both the stable and unstable field names.
/// Empty or whitespace-only text clears both. Result event:
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

/// Upload an image and set it as the room's banner; an empty path clears
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

/// Read a room's (or Space's) shared chat background and whether this account
/// may change it. Result event: `room_background { op_id, lifecycle, room_id,
/// content (canonical object or null), can_set, unsupported_version }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_fetch_room_background(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        backdrop::fetch_room_background(bridge, op_id, room_id).map(|_| String::new())
    })
}

/// Set a room's shared chat background. A non-empty `local_path` is uploaded;
/// otherwise a usable `url` inside `content_json` is re-sent (presentation
/// change); with neither the background is cleared. Result event:
/// `room_background_set { op_id, lifecycle, room_id, ok, content, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_background(
    ptr: *mut c_void,
    room_id: *const c_char,
    local_path: *const c_char,
    content_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        let content = unsafe { cstr_arg(content_json) }?;
        backdrop::set_room_background(bridge, op_id, room_id, path, content)
            .map(|_| String::new())
    })
}

/// Read one of the two standard account-data preferences (`accountprefs.rs`:
/// `m.media_preview_config`, `m.recent_emoji`) from the state store. Any other
/// type is refused. Result event: `account_pref { pref_type, present, content,
/// lifecycle }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_account_pref_read(
    ptr: *mut c_void,
    pref_type: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let pref_type = unsafe { cstr_arg(pref_type) }?;
        accountprefs::read(bridge, pref_type).map(|_| String::new())
    })
}

/// Write one of the two standard preferences (a bounded JSON object). Result
/// event: `account_pref_written { pref_type, ok, category, lifecycle }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_account_pref_write(
    ptr: *mut c_void,
    pref_type: *const c_char,
    content_json: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let pref_type = unsafe { cstr_arg(pref_type) }?;
        let content = unsafe { cstr_arg(content_json) }?;
        accountprefs::write(bridge, pref_type, content).map(|_| String::new())
    })
}

/// Read this account's personal backgrounds kept in account data
/// (`bgsync.rs`). `scope` is "" (the every-room default), a room id, or "*"
/// for the default and every joined room. Result event: `personal_backgrounds
/// { op_id, lifecycle, scope, entries: [{scope, state, id, info, color,
/// presentation}], failed: [scope] }`. Never carries the file key or url.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_personal_backgrounds_read(
    ptr: *mut c_void,
    scope: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let scope = unsafe { cstr_arg(scope) }?;
        bgsync::read(bridge, op_id, scope).map(|_| String::new())
    })
}

/// Download and decrypt one scope's personal background. The bytes are
/// parked for `mx_rust_personal_background_take(op_id)` (their own map, never
/// the timeline media map). Result event (command lane):
/// `personal_background_bytes { op_id, lifecycle, scope, ok, entry, size,
/// category }`. `expected_id` names the picture to fetch; another one is
/// refused with category "changed".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_personal_background_download(
    ptr: *mut c_void,
    scope: *const c_char,
    expected_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let scope = unsafe { cstr_arg(scope) }?;
        let expected_id = unsafe { cstr_arg(expected_id) }?;
        bgsync::download(bridge, op_id, scope, expected_id).map(|_| String::new())
    })
}

/// Write one scope's personal background. `mode` 0 encrypts and uploads
/// `local_path`, 1 keeps the current picture with new presentation from
/// `requested_json` (only while the server still holds its `expected_id`),
/// 2 clears it, 3 sets the account-wide switch (`enabled`, scope "" only).
/// Result event (command lane): `personal_background_written { op_id,
/// lifecycle, scope, mode, ok, entry, category, stage }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_personal_background_write(
    ptr: *mut c_void,
    scope: *const c_char,
    mode: c_uint,
    local_path: *const c_char,
    requested_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let scope = unsafe { cstr_arg(scope) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        let requested = unsafe { cstr_arg(requested_json) }?;
        bgsync::write(bridge, op_id, scope, mode, path, requested).map(|_| String::new())
    })
}

/// Remove every server copy of this account's personal backgrounds (the
/// account data; uploaded media cannot be deleted by a user). `known_rooms`
/// is a JSON array of room ids C++ holds synced copies for. Also turns the
/// account-wide switch off. Result event (command lane):
/// `personal_backgrounds_cleared { op_id, lifecycle, ok, cleared, failed,
/// skipped }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_personal_backgrounds_clear_all(
    ptr: *mut c_void,
    known_rooms: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let known = unsafe { cstr_arg(known_rooms) }?;
        bgsync::clear_all(bridge, op_id, known).map(|_| String::new())
    })
}

/// Read the settings blob and the account-wide switch from the SERVER
/// (`settingsync.rs`). Result event (command lane): `settings_sync_read
/// { op_id, lifecycle, ok, category, blob_state, blob, switch }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_settings_sync_read(ptr: *mut c_void, op_id: u64) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        settingsync::read(bridge, op_id).map(|_| String::new())
    })
}

/// Write the settings blob (kind 0), the switch (1) or clear the blob (2).
/// Result event: `settings_sync_written { op_id, lifecycle, kind, ok, category }`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_settings_sync_write(
    ptr: *mut c_void,
    kind: u32,
    content_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let content = unsafe { cstr_arg(content_json) }?;
        settingsync::write(bridge, op_id, kind, content).map(|_| String::new())
    })
}

/// Move a decrypted personal background out of its own map. Returns a buffer
/// the caller releases with `mx_rust_media_free`, or null for an unknown op.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_personal_background_take(
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
            .personal_background_results
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

// ---------------------------------------------------------------------------
// Stickers and image packs (MSC2545)
// ---------------------------------------------------------------------------

/// Read every image pack available to this account; answers with one
/// `sticker_packs { op_id, lifecycle, room_id, packs[] }` snapshot.
///
/// When `room_id` is set, that room's own `im.ponies.room_emotes` packs are
/// included (MSC2545 makes them usable in that room without opt-in).
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

/// Send one `m.sticker`. An empty `thread_root_id` targets the room
/// timeline. `url` must be a plain `mxc://`, so no http URL reaches the
/// wire.
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

/// Add one image to a room's `im.ponies.room_emotes` pack. Answers on
/// `sticker_pack_add_result`, like the user-pack path.
///
/// Room state, so gated by the room's required power level. `category` is
/// "forbidden", "duplicate", "pack_full", or a coarse room-error class.
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

/// Toggle one room pack in `im.ponies.emote_rooms` ("use this room's
/// stickers everywhere"). Answers with `sticker_pack_rooms_set { op_id,
/// lifecycle, ok, category, room_id, state_key, enabled }`. Account data,
/// so no power level applies.
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

/// Upload a local image and add it to this account's own sticker pack (the
/// only way to create a pack from nothing). Same result event as the save
/// path below.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_upload_to_user_pack(
    ptr: *mut c_void,
    shortcode: *const c_char,
    body: *const c_char,
    local_path: *const c_char,
    usage: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let body = unsafe { cstr_arg(body) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        let usage = unsafe { cstr_arg(usage) }?;
        stickers::upload_to_user_pack(bridge, op_id, shortcode, body, path, usage)
            .map(|_| String::new())
    })
}

/// Save a sticker into `im.ponies.user_emotes`. Answers with
/// `sticker_pack_add_result { op_id, lifecycle, ok, category, shortcode }`;
/// `category` is `duplicate`, `pack_full`, or a coarse room-error class.
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
    usage: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let url = unsafe { cstr_arg(url) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mimetype = unsafe { cstr_arg(mimetype) }?;
        let usage = unsafe { cstr_arg(usage) }?;
        stickers::add_to_user_pack(
            bridge, op_id, shortcode, url, body, mimetype, width, height, size,
            usage,
        )
        .map(|_| String::new())
    })
}

/// Add or remove a room widget. `content_json` is the full event content
/// (an empty object removes). Answers with `room_widget_written`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_widget_write(
    ptr: *mut c_void,
    room_id: *const c_char,
    widget_id: *const c_char,
    content_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let widget_id = unsafe { cstr_arg(widget_id) }?;
        let content_json = unsafe { cstr_arg(content_json) }?;
        let content: serde_json::Value = serde_json::from_str(&content_json)
            .map_err(|_| "widget content is not JSON".to_owned())?;
        widgets::write_room_widget(bridge, op_id, room_id, widget_id, content)
            .map(|_| String::new())
    })
}

// ── Policy lists (Mjolnir-style moderation) ────────────────────────────
//
// Nothing here acts on a match; see rust/src/policy.rs.

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

/// An empty `recommendation` removes the rule (an empty state event).
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
// Each returns the flow's generation (or an error), then reports through
// `qr_login_progress` events. See rust/src/qrlogin.rs.

/// Show a QR code here for a new device to scan. Returns the generation.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_generate(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        qrlogin::grant_generate(bridge).map(|gen| gen.to_string())
    })
}

/// Consume the QR a new device shows, as base64 text. Lightning bundles no
/// camera decoder.
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

/// MSC2545 pack management: remove an image, rename its shortcode, rename
/// the pack, or empty it.
///
/// Empty `room_id` selects the account's own pack (`im.ponies.user_emotes`);
/// otherwise the room pack under `state_key`, power-level gated. `action`
/// is "remove_image", "rename_image", "set_name" or "delete_pack", with
/// `arg_a` / `arg_b` as its operands (shortcode; old and new shortcode; new
/// name; nothing).
///
/// Answers with `sticker_pack_edit_result`. Not optimistic: the caller
/// re-reads the pack.
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
        // Unknown actions are refused, never defaulted, so a typo cannot delete a
        // pack.
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

/// One bounded presence polling round over a JSON array of user ids (capped
/// in presence.rs). Answers with one `presence_batch` event.
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

/// Publish the local user's presence (0 online, 1 unavailable, 2 offline).
/// A failure emits `presence_publish_failed` with the coarse category and,
/// for `M_LIMIT_EXCEEDED`, the server's `retry_after_ms`.
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

/// Fetch a direct image or video link for MediaBridge key `key` ("link:" +
/// 40 hex). `expect`: 0 image, 1 video. `timeout_class`: 0 viewer, 1 playable,
/// 2 save. Same safe fetch as a preview, a size cap and magic validation;
/// answers with `media_ready` (bytes via mx_rust_media_take) or
/// `media_failed`. The URL is never logged.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_link_media_fetch(
    ptr: *mut c_void,
    url: *const c_char,
    key: *const c_char,
    expect: u32,
    timeout_class: u32,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let url = unsafe { cstr_arg(url) }?;
        let key = unsafe { cstr_arg(key) }?;
        linkmedia::link_media_fetch(bridge, url, key, expect, timeout_class, op_id)
            .map(|_| String::new())
    })
}

/// Bounded, redirect-validated HTTPS GET for a GIF provider. `url` carries
/// the provider API key and is never logged. Answers with `gif_response`
/// (ok / status / category / bounded body). No Matrix identifiers are sent.
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

/// Download and validate a provider GIF, parking the bytes for
/// mx_rust_media_take. Only https provider-CDN URLs; the bytes must be a
/// real GIF (magic + bounded canvas). Answers with `gif_download_result`.
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

/// The open room's own posting facts (membership, may post, may redact
/// others), from local state only; answered as `room_own_permissions`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_own_permissions(
    ptr: *mut c_void,
    room_id: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rooms::room_own_permissions(bridge, room_id, op_id).map(|_| String::new())
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

/// Moderation plan: for each room in `room_ids_json` (a JSON array), whether
/// kick / ban / unban (`op` as above) of `user_id` would be offered there,
/// and why not otherwise. Sends nothing. Result event: moderation_plan.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_moderation_plan(
    ptr: *mut c_void,
    room_ids_json: *const c_char,
    user_id: *const c_char,
    op: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_ids = unsafe { cstr_arg(room_ids_json) }?;
        let user_id = unsafe { cstr_arg(user_id) }?;
        rooms::moderation_plan(bridge, room_ids, user_id, op, op_id)
            .map(|_| String::new())
    })
}

/// Closure plan: for each room in `room_ids_json` (a JSON array), whether the
/// viewer can close it and what closing would do. Sends nothing. Result
/// event: room_closure_plan.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_closure_plan(
    ptr: *mut c_void,
    room_ids_json: *const c_char,
    parent_ids_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_ids = unsafe { cstr_arg(room_ids_json) }?;
        let parent_ids = unsafe { cstr_arg(parent_ids_json) }?;
        rooms::closure_plan(bridge, room_ids, parent_ids, op_id).map(|_| String::new())
    })
}

/// Close a room: invite-only, out of the directory and of the Spaces in
/// `unlist_from_json` (a JSON array), every member the viewer outranks
/// removed with `reason` (may be empty), then leave when `leave` is non-zero
/// and every step succeeded. Deletes nothing. Result events:
/// room_closure_progress, room_closure_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_close_room(
    ptr: *mut c_void,
    room_id: *const c_char,
    reason: *const c_char,
    leave: c_int,
    unlist_from_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let reason = unsafe { cstr_arg(reason) }?;
        let unlist_from = unsafe { cstr_arg(unlist_from_json) }?;
        rooms::close_room(bridge, room_id, reason, leave != 0, unlist_from, op_id)
            .map(|_| String::new())
    })
}

/// Whether the account is a homeserver administrator (Synapse admin API).
/// Result event: server_admin_status.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_server_admin_status(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        serveradmin::server_admin_status(bridge, op_id).map(|_| String::new())
    })
}

/// Delete a room from this homeserver through Synapse's admin API (server
/// administrators only; the server enforces it). `block` non-zero stops local
/// users joining it again. Result events: admin_room_delete_progress,
/// admin_room_delete_result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_admin_delete_room(
    ptr: *mut c_void,
    room_id: *const c_char,
    block: c_int,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        serveradmin::admin_delete_room(bridge, room_id, block != 0, op_id)
            .map(|_| String::new())
    })
}

/// Set one member's power level via `Room::update_power_levels`, which
/// preserves every other level. Result event: room_power_level_result.
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

/// Set one threshold in `m.room.power_levels`. `key` must be in the
/// allowlist of `rooms::set_room_power_level_key`, so this is never a
/// generic power writer. Result event:
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

/// Set the room's join rule ("invite", "public" or "knock"). Result event:
/// room_edit_result, field "join_rule".
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

/// Room access. Each answers on room_edit_result with the named field; the
/// directory visibility read answers on
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

/// Room upgrade. The version list answers on `room_versions`; the upgrade
/// on `room_upgrade_result` with the replacement id.
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

/// Scheduled send. See rooms.rs for the protocol limits.
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

/// Room-level send (`Room::send`) for any joined room, used by scheduled
/// send since the timeline sends require the room to be open. Same body
/// spec and mentions, optional reply or thread relation. Answers on
/// `room_send_result {op_id, room_id, ok, category}`.
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

/// Activity Center seed for a fresh session. See
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

/// Message edit history and event source. Answers on
/// `message_edit_history` / `event_source`; see rooms.rs.
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

/// One page of a room's media history, walked backwards independently of
/// the live timeline. `restart` non-zero begins at the live edge; otherwise
/// the walk continues where it left off.
///
/// The page reports what it scanned as well as what matched, and whether
/// the start of history was reached; see mediahistory.rs.
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
        let timelines = Arc::clone(&bridge.timelines);
        let encrypted_room = room.encryption_state().is_encrypted();
        // For the kept-file cache: an unknown state counts as encrypted.
        let private_room = encrypted_room || room.encryption_state().is_unknown();
        // Clamped: an unbounded limit can stall the walk.
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
                // Nothing older exists; answer without asking the server again.
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
            if encrypted_room && cursor.token.is_none() {
                // A fresh walk: one whole-room backup fetch before paging (see
                // `prefetch_backup_keys_for_room`). Deduplicated per room per lifecycle.
                timelines.prefetch_backup_keys_for_room(&client, &room_id).await;
            }
            let mut opts = MessagesOptions::backward();
            opts.from = cursor.token.clone();
            opts.limit = UInt::new(want).unwrap_or(uint!(50));
            match room.messages(opts).await {
                Ok(messages) => {
                    let mut entries: Vec<serde_json::Value> = Vec::new();
                    let mut scanned = 0u64;
                    let mut undecryptable = 0u64;
                    // An edit is met before the event it replaces; it waits here.
                    let mut pending_edits = cursor.pending_edits.clone();
                    // Backward pages are newest-first, the order the browser shows.
                    for event in messages.chunk.iter() {
                        scanned += 1;
                        let raw = event.raw();
                        let Ok(value) = serde_json::from_str::<serde_json::Value>(
                            raw.json().get())
                        else {
                            continue;
                        };
                        let found = mediahistory::classify_with_edits(&value, &mut pending_edits);
                        if found.undecryptable {
                            undecryptable += 1;
                        }
                        // Register the attachment's sources under its event id, as the timeline
                        // does, so tiles fetch and decrypt through the registry instead of asking
                        // the server to thumbnail ciphertext.
                        if !found.entries.is_empty() {
                            if let (Some(media), Some(event_id)) = (
                                mediahistory::stored_media_from_event(&value),
                                value.get("event_id").and_then(|v| v.as_str()),
                            ) {
                                timelines.remember_media(event_id.to_owned(), media);
                                if private_room {
                                    timelines.mark_media_from_encrypted_room(event_id);
                                }
                            }
                        }
                        for entry in found.entries {
                            entries.push(entry.to_json());
                        }
                    }
                    // No `end`, or an empty chunk, means nothing older.
                    let exhausted =
                        messages.end.is_none() || messages.chunk.is_empty();
                    let next = mediahistory::Cursor {
                        token: messages.end.clone(),
                        exhausted,
                        scanned_total: cursor.scanned_total + scanned,
                        undecryptable_total: cursor.undecryptable_total
                            + undecryptable,
                        pending_edits,
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
                    // Keep the category: "server refused" and "no more history" read
                    // differently in the panel.
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

/// Start a room export's history walk (roomexport.rs): backwards from the
/// live edge, independent of the live timeline, until the range is covered.
/// `mode`: 0 the newest `limit` rows, 1 the whole room, 2 rows with
/// `from_ms <= ts <= to_ms` (`to_ms` 0 = no upper bound).
///
/// Answers on the command lane with `room_export_page` (rows, counters,
/// `done`, `stop_reason`) until `done`, or `room_export_failed`. Attachments
/// are registered under opaque `export:<op>:<event>` handles for
/// `mx_rust_media_fetch`; a row never carries an `mxc:`. The walk is aborted
/// by `mx_rust_room_export_cancel` and by shutdown.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_export_start(
    ptr: *mut c_void,
    room_id: *const c_char,
    mode: c_int,
    limit: u64,
    from_ms: u64,
    to_ms: u64,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let range = roomexport::Range::from_ffi(mode, limit, from_ms, to_ms)?;
        let client = require_client_for_search(bridge)?;
        let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
        let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
        let terminal = Arc::clone(&bridge.command_events);
        let parked = Arc::clone(&bridge.media_results);
        let timelines = Arc::clone(&bridge.timelines);
        let lifecycle = timelines.lifecycle();
        let encrypted_room = room.encryption_state().is_encrypted();
        // For the kept-file cache: an unknown state counts as encrypted.
        let private_room = encrypted_room || room.encryption_state().is_unknown();
        // Registered under the op id so cancel and shutdown can abort it.
        bridge.spawn_media_fetch(op_id, async move {
            let mut state = roomexport::WalkState::new(range);
            // Display names from the store, once per sender.
            let mut names: HashMap<String, String> = HashMap::new();
            let mut token: Option<String> = None;
            if encrypted_room {
                // One whole-room backup fetch first, as the media walk does, so
                // old history decrypts instead of exporting as holes.
                timelines.prefetch_backup_keys_for_room(&client, &room_id).await;
            }
            loop {
                let mut opts = MessagesOptions::backward();
                opts.from = token.clone();
                opts.limit = UInt::new(roomexport::PAGE_SIZE).unwrap_or(uint!(100));
                let messages = match room.messages(opts).await {
                    Ok(messages) => messages,
                    Err(err) => {
                        if timelines.lifecycle_current(lifecycle) {
                            enqueue_terminal(&terminal, &parked, json!({
                                "type": "room_export_failed",
                                "op_id": op_id,
                                "room_id": room_id,
                                "message": format_matrix_error(
                                    "could not read room history", err),
                            }));
                        }
                        return;
                    }
                };
                let mut rows: Vec<serde_json::Value> = Vec::new();
                // Backward pages are newest-first; C++ reverses the whole export.
                for event in messages.chunk.iter() {
                    let Ok(value) = serde_json::from_str::<serde_json::Value>(
                        event.raw().json().get())
                    else {
                        continue;
                    };
                    let Some(row) = state.consume(&value) else {
                        continue;
                    };
                    if !names.contains_key(&row.sender) {
                        let resolved = match UserId::parse(&row.sender) {
                            Ok(user) => room
                                .get_member_no_sync(&user)
                                .await
                                .ok()
                                .flatten()
                                .and_then(|m| m.display_name().map(|d| d.to_owned()))
                                .unwrap_or_default(),
                            Err(_) => String::new(),
                        };
                        names.insert(row.sender.clone(), resolved);
                    }
                    let mut media_key = String::new();
                    if row.is_attachment() {
                        if let Some(media) = mediahistory::stored_media_from_event(&value) {
                            let handle = roomexport::media_handle(op_id, &row.event_id);
                            if timelines.remember_export_media(handle.clone(), media, private_room) {
                                media_key = handle;
                            }
                        }
                    }
                    let name = names.get(&row.sender).map(String::as_str).unwrap_or("");
                    rows.push(row.to_json(name, &media_key));
                }
                // No `end`, or an empty chunk, means nothing older.
                let exhausted = messages.end.is_none() || messages.chunk.is_empty();
                state.end_of_page(exhausted);
                token = messages.end.clone();
                if !timelines.lifecycle_current(lifecycle) {
                    return;
                }
                let done = state.stop.is_some();
                enqueue_terminal(&terminal, &parked, json!({
                    "type": "room_export_page",
                    "op_id": op_id,
                    "room_id": room_id,
                    "rows": rows,
                    "rows_total": state.rows,
                    "scanned_total": state.scanned,
                    "undecryptable_total": state.undecryptable,
                    "done": done,
                    "stop_reason": state.stop.map(|s| s.as_str()).unwrap_or(""),
                    "encrypted_room": encrypted_room,
                }));
                if done {
                    return;
                }
            }
        });
        Ok(String::new())
    })
}

/// Stop a room export: abort its walk if still running and forget every
/// attachment handle it registered. Idempotent; emits nothing. In-flight
/// attachment downloads are cancelled by their own op ids
/// (`mx_rust_media_cancel`).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_export_cancel(ptr: *mut c_void, op_id: u64) {
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
        bridge
            .timelines
            .release_export_media(&roomexport::handle_prefix(op_id));
    }));
}

/// A room's widgets, resolved and validated. Lightning lists and opens
/// widgets rather than embedding them (see rust/src/widgets.rs). Answers on
/// `room_widgets {op_id, room_id, ok, widgets:[...]}`.
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
            let can_manage = widgets::can_manage_widgets(&client, &room).await;
            // The user's own profile from the store, for widget URL templating,
            // instead of one request per widget.
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
                "ok": true, "can_manage": can_manage, "widgets": payloads,
            }));
        });
        Ok(String::new())
    })
}

/// Which networks a room is bridged to, as the bridge advertises (MSC2346).
/// See rust/src/bridges.rs.
///
/// `allow_network` (0/1) permits the `/state` fallback, which is expensive
/// on large rooms; only surfaces the user explicitly opened should allow it.
///
/// Answers with `room_bridges {op_id, room_id, ok, bridges:[{protocol,
/// protocolName, network}]}`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_room_bridges(
    ptr: *mut c_void,
    room_id: *const c_char,
    allow_network: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let client = require_client_for_search(bridge)?;
        let parsed = RoomId::parse(&room_id).map_err(|_| "invalid room id".to_owned())?;
        let room = client.get_room(&parsed).ok_or_else(|| "unknown room".to_owned())?;
        let events = Arc::clone(&bridge.events);
        let timelines = Arc::clone(&bridge.timelines);
        let lifecycle = timelines.lifecycle();
        bridge.spawn_room_action(async move {
            let found =
                bridges::read_room_bridges(&client, &room, allow_network != 0).await;
            // A /state read can outlive an account switch; a late answer must not
            // label the next account's rooms.
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let payloads: Vec<serde_json::Value> =
                found.iter().map(bridges::bridge_payload).collect();
            enqueue(&events, json!({
                "type": "room_bridges", "op_id": op_id, "room_id": room_id,
                "ok": true, "bridges": payloads,
            }));
        });
        Ok(String::new())
    })
}

// ---------------------------------------------------------------------------
// Local message search (SQLite FTS5). See rust/src/localsearch.rs.
// ---------------------------------------------------------------------------

/// Open the account's index if not open yet. Lazy and from one place, since
/// the index belongs to the store directory, not to a login flow.
fn ensure_search_index(bridge: &RustClient) -> Result<(), String> {
    // Teardown closed the index deliberately (an open handle makes the store
    // deletion fail on Windows). A search keystroke between shutdown and
    // destroy must not reopen it; there is no session left to search.
    if bridge.index_shutdown.load(Ordering::Relaxed) {
        return Err("the local index is closed for this session".to_owned());
    }
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
    // `open_in` creates the file at the process umask in the key directory;
    // correct it as in `build_client`.
    restrict_store_permissions(&bridge.store_path);
    if let Ok(mut guard) = bridge.search_index.lock() {
        *guard = Some(index);
    }
    Ok(())
}

/// The answer for a local search that could not run at all (the index would
/// not open, or the query failed). It is a real `local_search_result`, so the
/// panel leaves "loading" and says so.
fn local_search_failure_event(op_id: u64) -> serde_json::Value {
    json!({
        "type": "local_search_result", "op_id": op_id, "ok": false,
        "category": "index_unavailable", "results": [],
    })
}

/// Search the local index. Answers on `local_search_result`.
///
/// Synchronous: it is a local SQLite query, and going through the task pool
/// would add latency and let a stale answer arrive after the next
/// keystroke. Enqueued rather than returned so C++ reads it via the poll
/// loop.
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
        if let Err(err) = ensure_search_index(bridge) {
            // The caller holds an op id and waits for an answer; a bare
            // "error:" return string is discarded by the C++ side and left the
            // panel spinning for ever.
            eprintln!("local search: index unavailable: {err}");
            enqueue(&bridge.events, local_search_failure_event(op_id));
            return Err(err);
        }

        // Report a query too short for the trigram tokenizer as such, not as
        // "no results".
        if !localsearch::query_is_long_enough(&query) {
            enqueue(&bridge.events, json!({
                "type": "local_search_result", "op_id": op_id, "ok": false,
                "category": "too_short",
                "min_chars": localsearch::MIN_QUERY_CHARS,
                "results": [],
            }));
            return Ok(String::new());
        }

        let searched = (|| -> Result<Vec<localsearch::Hit>, String> {
            let guard = bridge.search_index.lock()
                .map_err(|_| "search index unavailable".to_owned())?;
            let index = guard.as_ref()
                .ok_or_else(|| "search index unavailable".to_owned())?;
            index.search(&query, &room_id, limit.max(1) as i64, offset.max(0) as i64)
        })();
        let hits = match searched {
            Ok(hits) => hits,
            Err(err) => {
                eprintln!("local search: query failed: {err}");
                enqueue(&bridge.events, local_search_failure_event(op_id));
                return Ok(String::new());
            }
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
                "filename": hit.filename,
            }))
            .collect();
        enqueue(&bridge.events, json!({
            "type": "local_search_result", "op_id": op_id, "ok": true,
            "category": "", "results": results,
        }));
        Ok(String::new())
    })
}

/// How much the index holds, so the UI can say what search covers.
/// Answers on `search_index_stats`.
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

/// Sweep every joined room's cached events into the index. Runs on the
/// tracked pool and checks the cooperative stop between rooms, so it never
/// blocks store deletion.
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

/// Page one room backwards and index what arrives ("index this room's
/// history"). Bounded; answers on `search_index_deepened`.
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

/// Forget one event (redaction). Synchronous and cheap.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_forget_event(
    ptr: *mut c_void,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event_id = unsafe { cstr_arg(event_id) }?;
        // Not ensure_search_index: a redaction must not create the index file.
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.remove_event(&event_id)?;
            }
        }
        Ok(String::new())
    })
}

/// Forget one room, or the whole index ("stop indexing this room", "clear
/// the search index").
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
        // Forgotten rows mean the room is no longer indexed to its bound. Not
        // once teardown began: the store directory may be being deleted.
        if !bridge.index_shutdown.load(Ordering::Relaxed) {
            bridge.index_all.forget_room(&room_id);
        }
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_clear(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        ensure_search_index(bridge)?;
        // An empty index has no complete rooms: forget the index-all record and
        // cancel a running pass, whose late results the epoch bump discards.
        bridge.index_all.reset();
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.clear()?;
            }
        }
        Ok(String::new())
    })
}

// ---------------------------------------------------------------------------
// "Index all rooms". See rust/src/indexall.rs.
// ---------------------------------------------------------------------------

/// Minimum spacing of the index totals on progress events: counting the
/// index takes its mutex, and a search keystroke must not queue behind it
/// every page.
const INDEX_ALL_STATS_EVERY_MS: u64 = 5_000;

/// The `search_index_all` event. Room ids and counters only, never text.
/// `index` carries the index totals when given.
fn index_all_event(
    op_id: u64,
    lifecycle: u64,
    status: &indexall::Status,
    index: Option<&Arc<Mutex<Option<localsearch::SearchIndex>>>>,
) -> serde_json::Value {
    let mut event = json!({
        "type": "search_index_all", "op_id": op_id, "lifecycle": lifecycle,
        "state": status.state,
        "total": status.total, "position": status.position,
        "current_room": status.current_room,
        "written": status.written, "skipped": status.skipped,
        "undecryptable": status.undecryptable,
        "undecryptable_rooms": status.undecryptable_rooms,
        "failed_rooms": status.failed_rooms,
        "complete_rooms": status.complete_rooms,
        "retry_in_ms": status.retry_in_ms, "elapsed_ms": status.elapsed_ms,
    });
    let stats = index.and_then(|index| {
        index.lock().ok().and_then(|g| g.as_ref().and_then(|ix| ix.stats().ok()))
    });
    if let Some(stats) = stats {
        event["messages"] = json!(stats.messages);
        event["indexed_rooms"] = json!(stats.rooms);
    }
    event
}

/// Index one room of an index-all run with the per-room walk.
async fn index_all_walk(
    client: &Client,
    timelines: &Arc<timeline::TimelineRegistry>,
    index: &Arc<Mutex<Option<localsearch::SearchIndex>>>,
    room_id: &str,
    gate: &indexall::RunGate,
) -> indexall::RoomResult {
    let Ok(parsed) = RoomId::parse(room_id) else {
        return indexall::RoomResult::Gone;
    };
    let Some(room) = client.get_room(&parsed) else {
        return indexall::RoomResult::Gone;
    };
    // Recheck before fetching: only encrypted joined rooms need this background walk.
    if !matches!(room.state(), matrix_sdk::RoomState::Joined)
        || !room.encryption_state().is_encrypted()
    {
        return indexall::RoomResult::Gone;
    }
    // One whole-room backup fetch before paging, so the walk does not make the
    // SDK fetch (and request from our devices) each unknown key one by one.
    // Deduplicated per room per lifecycle; a no-op without a usable backup.
    if (gate.alive)() {
        timelines.prefetch_backup_keys_for_room(client, room_id).await;
    }
    match localsearch::deep_index_room_gated(&room, index, gate, localsearch::DEEP_MAX_PAGES)
        .await
    {
        Ok(outcome) if outcome.interrupted => indexall::RoomResult::Interrupted {
            written: outcome.written,
        },
        Ok(outcome) => indexall::RoomResult::Done {
            written: outcome.written,
            undecryptable: outcome.undecryptable,
        },
        Err(error) => indexall::RoomResult::Failed {
            rate_limited: error.rate_limited,
            retry_after_ms: error.retry_after_ms,
        },
    }
}

/// Start "index all rooms", or continue the pending run. `resume_only`
/// continues a run the last session left unfinished and unpaused, and
/// otherwise only reports. Answers on `search_index_all`, then keeps reporting
/// progress until the run ends.
///
/// One room at a time on the tracked room-action pool; sign-out, an account
/// switch and teardown stop it through `index_shutdown` and the lifecycle
/// generation, and it never emits into a session that is over.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_start(
    ptr: *mut c_void,
    resume_only: c_uint,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let client = require_client_for_search(bridge)?;
        ensure_search_index(bridge)?;
        let control = Arc::clone(&bridge.index_all);
        let index = Arc::clone(&bridge.search_index);
        let events = Arc::clone(&bridge.events);
        let timelines = Arc::clone(&bridge.timelines);
        let lifecycle = timelines.lifecycle();

        let listing = client.clone();
        let begin = control.begin(resume_only != 0, move || {
            listing
                .joined_rooms()
                .into_iter()
                .map(|room| {
                    let stamp = room
                        .latest_event_timestamp()
                        .map(|ts| u64::from(ts.get()))
                        .unwrap_or(0);
                    (room.room_id().to_string(), stamp, room.encryption_state().is_encrypted())
                })
                .collect()
        });
        if begin != indexall::Begin::Started {
            let state = match begin {
                indexall::Begin::NothingToDo => Some("done"),
                _ => None,
            };
            enqueue(&events, index_all_event(
                op_id, lifecycle, &control.status(state), Some(&index)));
            return Ok(String::new());
        }

        let stop = Arc::clone(&bridge.index_shutdown);
        let alive_timelines = Arc::clone(&timelines);
        let alive: Arc<dyn Fn() -> bool + Send + Sync> = Arc::new(move || {
            !stop.load(Ordering::Relaxed) && alive_timelines.lifecycle_current(lifecycle)
        });
        let emit_alive = Arc::clone(&alive);
        let emit_events = Arc::clone(&events);
        let emit_index = Arc::clone(&index);
        let last_stats: Mutex<Option<std::time::Instant>> = Mutex::new(None);
        let emit: Arc<dyn Fn(indexall::Status) + Send + Sync> =
            Arc::new(move |status: indexall::Status| {
                // Never into a session that is over.
                if !emit_alive() {
                    return;
                }
                let settled = status.state != "running" && status.state != "held"
                    && status.state != "backoff";
                let with_stats = match last_stats.lock() {
                    Ok(mut last) => {
                        let due = settled || last.map_or(true, |at| {
                            at.elapsed()
                                >= std::time::Duration::from_millis(INDEX_ALL_STATS_EVERY_MS)
                        });
                        if due {
                            *last = Some(std::time::Instant::now());
                        }
                        due
                    }
                    Err(_) => settled,
                };
                enqueue(&emit_events, index_all_event(
                    op_id, lifecycle, &status, with_stats.then_some(&emit_index)));
            });
        let gate = indexall::RunGate { control: Arc::clone(&control), alive, emit };

        bridge.spawn_room_action(async move {
            let walk = move |room_id: String, gate: indexall::RunGate| {
                let client = client.clone();
                let timelines = Arc::clone(&timelines);
                let index = Arc::clone(&index);
                async move {
                    index_all_walk(&client, &timelines, &index, &room_id, &gate).await
                }
            };
            let _ = indexall::drive(gate, walk, tokio::time::sleep).await;
        });
        Ok(String::new())
    })
}

/// Pause and cancel write the progress record in the store directory; once
/// teardown began that directory may be being deleted, and a file created
/// inside it would make the deletion fail.
fn refuse_index_all_after_teardown(bridge: &RustClient) -> Result<(), String> {
    if bridge.index_shutdown.load(Ordering::Relaxed) {
        return Err("the local index is closed for this session".to_owned());
    }
    Ok(())
}

/// Pause "index all rooms". A running pass stops at its next check and then
/// reports "paused"; an idle one reports now.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_pause(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        refuse_index_all_after_teardown(bridge)?;
        bridge.index_all.pause();
        if !bridge.index_all.running() {
            enqueue(&bridge.events, index_all_event(
                op_id, bridge.timelines.lifecycle(), &bridge.index_all.status(None), None));
        }
        Ok(String::new())
    })
}

/// Cancel "index all rooms": the queue is dropped, rooms already indexed stay
/// recorded as such. A running pass reports "cancelled" when it stops.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_cancel(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        refuse_index_all_after_teardown(bridge)?;
        bridge.index_all.cancel();
        if !bridge.index_all.running() {
            enqueue(&bridge.events, index_all_event(
                op_id, bridge.timelines.lifecycle(), &bridge.index_all.status(None), None));
        }
        Ok(String::new())
    })
}

/// Automatic holds (bit 1: a call is active, bit 2: a timeline is scrolling).
/// A held pass waits between pages and loses nothing.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_hold(
    ptr: *mut c_void,
    bits: c_uint,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.index_all.set_hold(bits & (indexall::HOLD_CALL | indexall::HOLD_SCROLL));
        Ok(String::new())
    })
}

/// Report where "index all rooms" stands. Answers on `search_index_all`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_status(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let status = bridge.index_all.status(None);
        enqueue(&bridge.events, index_all_event(
            op_id, bridge.timelines.lifecycle(), &status, Some(&bridge.search_index)));
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

/// MSC3030 "jump to date": the event closest to `timestamp_ms`, searching
/// forward so a chosen day lands on its first message. See
/// rooms::event_at_timestamp.
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

/// Set or clear (empty string) the room's canonical alias. Result event:
/// room_edit_result, field "canonical_alias".
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

/// Read `m.room.pinned_events` and resolve each id into a displayable row.
/// `allow_remote` (0/1) permits the `/state` fallback, used only when the
/// room has no pinned-events state at all. Result event: room_pinned.
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

/// Pin (`pin` = 1) or unpin (0) one event; the SDK does the
/// read-modify-send. Result event: room_pin_result.
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
// Room discovery / join / knock (discover.rs). One op-id per call; results
// arrive as room_target_resolved / public_rooms_result / room_join_result /
// room_knock_result / knock_cancel_result / space_children_result events.
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
// Ignored users and reporting (ignore.rs). Events: ignore_user_result /
// ignored_users_list / ignored_users_changed (sync push) /
// report_message_result.
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

/// Fetch the account's push rules as the notification settings page's
/// snapshot (pushrules.rs). Result: `push_rules_snapshot`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_push_rules_snapshot(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        pushrules::request_snapshot(bridge, op_id).map(|_| String::new())
    })
}

/// Apply one notification-settings command (`kind`, `key`, `value`; see
/// pushrules::plan). Results: `push_rules_write`, then `push_rules_snapshot`.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_push_rules_write(
    ptr: *mut c_void,
    kind: *const c_char,
    key: *const c_char,
    value: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let kind = unsafe { cstr_arg(kind) }?;
        let key = unsafe { cstr_arg(key) }?;
        let value = unsafe { cstr_arg(value) }?;
        pushrules::write_rule(bridge, kind, key, value, op_id).map(|_| String::new())
    })
}

/// Read the ignored-user list from account data.
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

/// Send `m.call.answer`. For signalling completeness; no production caller
/// until a media backend can produce an answer SDP.
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
/// remote SDP in poll payloads. C++ keeps it bounded and single-shot, never
/// logs it or exposes it to QML. Off until a media backend is registered.
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

/// Connect to the SFU at `service_url` for `room_id`.
///
/// Authorizes with a Matrix OpenID token (the access token never reaches
/// the SFU) and runs LiveKit signalling. Progress: `sfu_state` /
/// `sfu_joined` / `sfu_participants` / `sfu_track_published` /
/// `sfu_speakers` / `sfu_quality`. SDP and ICE arrive as
/// `sfu_remote_description` / `sfu_remote_candidate`, only in
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

/// Tell the SFU a published track is muted. This is only the signal; the
/// engine's valve already stops the bytes locally.
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

/// Report the MatrixRTC session in one room as an `rtc_session` event
/// (participants, selected focus, slot status). Read-only.
///
/// Non-zero `prefer_server` also fetches `/state` and merges it with the
/// store. It costs a request, so use it only with evidence the store is
/// incomplete (an SFU participant with no membership).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_session(
    ptr: *mut c_void,
    room_id: *const c_char,
    prefer_server: u8,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        rtc::request_session(bridge, room_id, prefer_server != 0, op_id)
            .map(|_| String::new())
    })
}

/// Discover the MatrixRTC transports available to this account. A non-empty
/// `room_id` adds the focus the room's participants advertise, the only
/// route on a homeserver without MSC4143.
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

/// Publish or refresh our MatrixRTC membership. Answers
/// `rtc_membership_published {ok, category, event_id, delay_id,
/// delayed_category}`. An empty `delay_id` means no MSC4140 delayed events;
/// cleanup then relies on the membership's `expires`.
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

/// Restart the server-side delayed retraction so it does not fire.
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

/// Distribute our media key to the call's participant devices,
/// Olm-encrypted per device. `targets_json` is `[{user_id, device_id}]`
/// from the observed membership. Answers `rtc_key_sent {ok, category,
/// delivered, key_index}`; the key is never echoed.
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

/// Send element-call's transient call reaction. The pair is validated
/// against element-call's own table; unknown pairs are refused.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_send_call_reaction(
    ptr: *mut c_void,
    room_id: *const c_char,
    membership_event_id: *const c_char,
    emoji: *const c_char,
    name: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let membership_event_id = unsafe { cstr_arg(membership_event_id) }?;
        let emoji = unsafe { cstr_arg(emoji) }?;
        let name = unsafe { cstr_arg(name) }?;
        rtc::send_call_reaction(
            bridge, room_id, membership_event_id, emoji, name, op_id,
        )
        .map(|_| String::new())
    })
}

/// Raise or lower this device's hand in a room's call, in element-call's
/// format: raising sends an `m.reaction` annotating our own `m.call.member`
/// state event; lowering redacts it. Answers `rtc_hand_result {ok, raised,
/// category, event_id}`; keep the reaction id, it is needed to lower.
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
        // `unsigned char` on both sides, like mx_rust_calls_set_media_capable.
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

/// Read the hands already raised in a room's call, once per join: a hand
/// raised earlier produces no sync event for us. Bounded and cache-first;
/// answers `rtc_hands`.
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
// UIA and device sign-out (uia.rs). Lightning parks the operation behind the
// server's UIA challenge and answers with the password stage. Events:
// uia_required / device_delete_result.
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

/// Server-side message search (POST /v3/search via raw ruma; matrix-sdk has
/// no wrapper). Unencrypted rooms only. Empty `room_id` = all rooms;
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

/// Toggle the MSC1772 `suggested` flag on an existing m.space.child,
/// preserving via and order; a non-child is refused. Result event:
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

/// Remove a Space child (MSC1772 empty-via m.space.child). Never leaves or
/// deletes the room. Result event:
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
    duration_ms: u64,
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
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
            duration_ms,
            in_reply_to, op_id,
        )
        .map(|_| String::new())
    })
}

/// Send a video with a locally extracted poster frame.
///
/// `thumb_*` is optional (empty sends no poster). Lengths are bounded here,
/// and the poster is re-validated by magic sniffing in `rooms::PosterBytes`;
/// an invalid poster is dropped and the video still sends. The SDK uploads
/// and encrypts the poster.
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        let poster = unsafe { poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        rooms::send_video_path(
            bridge, room_id, local_path, mime, caption, width, height,
            duration_ms, poster, in_reply_to, op_id,
        )
        .map(|_| String::new())
    })
}

/// Thread twin of `mx_rust_timeline_send_video`.
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        let poster = unsafe { poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        rooms::send_thread_video_path(
            bridge, room_id, root, local_path, mime, caption, width, height,
            duration_ms, poster, in_reply_to, op_id,
        )
        .map(|_| String::new())
    })
}

/// MSC3245 voice message. `waveform` is 0..=100 amplitudes (may be empty),
/// bounded here. Result on attachment_send_result by op_id.
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
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
            bridge, room_id, local_path, mime, duration_ms, waveform, in_reply_to, op_id,
        )
        .map(|_| String::new())
    })
}

/// Thread twin of `mx_rust_timeline_send_voice`, sent on the thread
/// timeline so the event carries an `m.thread` relation. Result on
/// attachment_send_result by op_id.
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
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
            waveform, in_reply_to, op_id,
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        let filename = unsafe { cstr_arg(filename) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if data.is_null() || len == 0 {
            return Err("attachment data is empty".to_owned());
        }
        // One bounded copy into Rust-owned memory; C++ frees its buffer on return.
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        rooms::send_attachment_bytes(
            bridge, room_id, bytes, filename, mime, width, height, in_reply_to, op_id,
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
    duration_ms: u64,
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let local_path = unsafe { cstr_arg(local_path) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        rooms::send_thread_attachment_path(
            bridge, room_id, root, local_path, mime, caption, width, height,
            animated != 0, duration_ms, in_reply_to, op_id,
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
    in_reply_to: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let in_reply_to = unsafe { reply_target_arg(in_reply_to) }?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        let root = unsafe { cstr_arg(root_event_id) }?;
        let filename = unsafe { cstr_arg(filename) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        if data.is_null() || len == 0 {
            return Err("attachment data is empty".to_owned());
        }
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        rooms::send_thread_attachment_bytes(
            bridge, room_id, root, bytes, filename, mime, width, height, in_reply_to, op_id,
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

/// Cancel an in-flight media fetch by op id: aborts the task at its next
/// await point and drops any parked bytes. Idempotent. Emits no terminal
/// event; C++ has already released the op.
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

/// Move a parked media payload out of the bridge. Returns a buffer the
/// caller must release with `mx_rust_media_free`, or null for an unknown op
/// id. The only path media bytes take across the FFI.
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

/// Deterministic shutdown of all managed async work, called by C++ before
/// logout and store cleanup so nothing still holds the crypto store.
/// Returns a short status string for logging.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_shutdown_tasks(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        // Miss counts show when the budget was exceeded. Counts only.
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

/// The session's current tokens as `{"access_token","refresh_token"}`, or an
/// empty string when there is no session. Called by the retiring worker after
/// `mx_rust_shutdown_tasks`. matrix-sdk's own tasks (the send queue, backup
/// downloads, E2EE initialization) can still refresh until the runtime is
/// shut down, so the worker prefers the ledger's pair read AFTER
/// `mx_rust_destroy_and_report` (`mx_rust_token_ledger_latest`).
/// SENSITIVE: carries credentials; C++ writes them to the SecretStore and
/// nothing logs them.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_final_session_tokens(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        Ok(final_session_tokens_json(bridge.final_session_tokens()))
    })
}

/// C++'s answer for the rotation `generation` (the `session_tokens_refreshed`
/// event's own field): `saved` non-zero when the pair is in the keyring, zero
/// when the keyring refused it or no account owns it. Ends that rotation's
/// wait inside matrix-sdk's refresh (sessiontokens.rs). Holds no secret.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_session_tokens_persisted(
    ptr: *mut c_void,
    generation: u64,
    saved: c_int,
) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Ok(bridge) = unsafe { bridge(ptr) } {
            bridge.token_ledger.answer(generation, saved != 0);
        }
    }));
}

/// A reference to this handle's token ledger that outlives the handle, for
/// the retiring worker: taken before `mx_rust_destroy_and_report`, read after
/// it with `mx_rust_token_ledger_latest`, released with
/// `mx_rust_token_ledger_release`. Null for a null handle.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_retain(ptr: *mut c_void) -> *mut c_void {
    catch_unwind(AssertUnwindSafe(|| match unsafe { bridge(ptr) } {
        Ok(bridge) => Arc::into_raw(Arc::clone(&bridge.token_ledger)) as *mut c_void,
        Err(_) => std::ptr::null_mut(),
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// The pair the handle's last client rotated to, as `final_session_tokens`
/// formats it; "" when that client never rotated. Read after the handle was
/// destroyed, when its runtime is shut down and nothing can rotate again.
/// SENSITIVE: carries credentials; never log the result.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_latest(ledger: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        if ledger.is_null() {
            return Ok(String::new());
        }
        // Borrowed: the reference taken by `mx_rust_token_ledger_retain` stays
        // with the caller until it releases it.
        let ledger = unsafe { &*(ledger as *const sessiontokens::TokenLedger) };
        Ok(final_session_tokens_json(ledger.latest()))
    })
}

/// Release a reference taken by `mx_rust_token_ledger_retain`. Null is a
/// no-op.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_release(ledger: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !ledger.is_null() {
            drop(unsafe { Arc::from_raw(ledger as *const sessiontokens::TokenLedger) });
        }
    }));
}

fn final_session_tokens_json(tokens: Option<matrix_sdk::SessionTokens>) -> String {
    match tokens {
        Some(tokens) if !tokens.access_token.is_empty() => json!({
            "access_token": tokens.access_token,
            "refresh_token": tokens.refresh_token,
        })
        .to_string(),
        _ => String::new(),
    }
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

/// MSC4153 "invisible crypto": whether new clients exclude devices that are
/// not cross-signed.
///
/// A process global because `build_client` serves password login, OAuth
/// and the auth probe. Read at build time only: matrix-sdk 0.18 has no
/// runtime setter for either half, so a change needs a new client.
static STRICT_DEVICE_TRUST: AtomicBool = AtomicBool::new(false);

/// Set before a login; applies to the next client built.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_strict_device_trust(enabled: c_int) -> *mut c_char {
    ffi_string(|| {
        STRICT_DEVICE_TRUST.store(enabled != 0, Ordering::SeqCst);
        Ok(String::new())
    })
}

/// This handle's media-store key, set right after `mx_rust_create` and before
/// any sign-in or restore builds a client. `key_len` 32: the account's key
/// from the OS keyring, and the media store is opened encrypted with it.
/// Anything else (0 when the keyring cannot answer, or for an account that is
/// not saved yet): the media store is in memory for this session.
/// `admit_encrypted`: the keyring holding the key is a secure one, so
/// encrypted-room media may be cached. The key is never logged or echoed.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_media_store_key(
    ptr: *mut c_void,
    key: *const u8,
    key_len: u64,
    admit_encrypted: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let key = if key.is_null() || key_len != 32 {
            None
        } else {
            let mut bytes = mediastore::KeyBytes([0u8; 32]);
            unsafe { std::ptr::copy_nonoverlapping(key, bytes.0.as_mut_ptr(), 32) };
            Some(bytes)
        };
        mediastore::set_key(ptr as usize, &bridge.store_path, key, admit_encrypted != 0);
        Ok(String::new())
    })
}

/// Where this handle's media store was opened (`mediastore::active`), for
/// Settings to say what is really kept this session. See the header for the
/// four answers. Before a client is built nothing is opened: "memory".
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_store_state(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        Ok(mediastore::active(&bridge.store_path).state_name().to_owned())
    })
}

/// Whether media the user opens is kept between sessions: the SDK media store
/// (encrypted, mediastore.rs; encrypted-room media only under a key a secure
/// keyring holds) and plain kept files for what it refuses by size, never from
/// an encrypted room (rooms::media_persistence). Process-wide and read on every
/// fetch, so it applies from the next fetch; what is already kept stays until
/// `mx_rust_media_cache_clear`. Sent attachments are the send queue's, and
/// are kept whatever this says.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_keep_media(enabled: c_int) -> *mut c_char {
    ffi_string(|| {
        rooms::set_keep_media(enabled != 0);
        Ok(String::new())
    })
}

/// Remove the media this account keeps on disk: the kept-file directory and
/// what the SDK media store may drop. Answers on
/// `media_cache_cleared {op_id, ok, files, bytes}`, where files/bytes count
/// the kept files only (the SDK store reports no count).
#[no_mangle]
pub unsafe extern "C" fn mx_rust_media_cache_clear(
    ptr: *mut c_void,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let client = rooms::require_client(bridge)?;
        let kept_dir = if bridge.store_path.as_os_str().is_empty() {
            None
        } else {
            cachelayout::kept_files_dir(&bridge.store_path)
        };
        let events = Arc::clone(&bridge.events);
        let store_path = bridge.store_path.clone();
        bridge.spawn_room_action(async move {
            let store_before = {
                let path = store_path.clone();
                tokio::task::spawn_blocking(move || aboutinfo::media_store_disk_bytes(&path))
                    .await
                    .unwrap_or(0)
            };
            let outcome = rooms::clear_kept_media(&client, kept_dir).await;
            let (ok, files, bytes) = match outcome {
                Some((files, bytes)) => (true, files, bytes),
                None => (false, 0, 0),
            };
            // The clear's result goes out at once; compaction never delays it.
            enqueue(&events, json!({
                "type": "media_cache_cleared", "op_id": op_id,
                "ok": ok, "files": files, "bytes": bytes,
            }));
            // The SDK media store deletes rows and leaves its file as big as
            // it was; compact it so the space is really returned, as its own
            // later report. Only after a clean that succeeded.
            if ok {
                let result =
                    aboutinfo::compact_media_store(&client, &store_path, store_before).await;
                aboutinfo::report_compaction(&events, op_id, "media_store", result);
            }
        });
        Ok(String::new())
    })
}

/// Resume what the last session left queued but unsent
/// (`respawn_tasks_for_rooms_with_unsent_requests`), which the SDK
/// recommends at startup; otherwise a room's queue only resumes when the
/// room is reopened.
///
/// Where it is called matters twice over:
/// - Not in `build_client`: it resolves rooms via `client.get_room()`, and
///   the room map is empty until login or `restore_session` runs
///   `load_rooms()`.
/// - Not on the sync lanes: `for_room` spawns an infinite per-room task on
///   the ambient runtime and memoises the queue, so a throwaway
///   `run_async` runtime would leave a dead cached queue that never sends.
///
/// So it runs right after a session is established, on the shared runtime.
/// Callers: `mx_rust_login`, `restore_client_with_session` (every restore,
/// incl. the SSO handoff) and `mx_rust_oauth_restore`. The OAuth/SSO finish
/// paths use an in-memory bootstrap client, and the QR paths reuse an
/// already-restored client.
pub(crate) async fn resume_unsent_requests(client: &Client) {
    client.send_queue().respawn_tasks_for_rooms_with_unsent_requests().await;
}

/// How a client is pointed at its homeserver.
///
/// `Discover` (`server_name_or_homeserver_url()`) does `/.well-known`
/// discovery and verifies the homeserver over HTTP, so it needs the server
/// up. `Url` (`homeserver_url()`) only parses. A typed server name can
/// never be treated as a `Url` (`https://matrix.org` serves its client API
/// at `matrix-client.matrix.org`); only a URL the SDK resolved is safe,
/// which is what `build_client_with` records for `build_client_for_restore`.
enum HomeserverInput<'a> {
    Discover(&'a str),
    Url(&'a str),
}

/// File in the account's store directory recording the homeserver URL
/// matrix-sdk resolved. Kept in the store so it is deleted with it; written
/// 0600 like everything else there.
const RESOLVED_HOMESERVER_FILE: &str = "lightning-homeserver-url";

fn resolved_homeserver_path(store_path: &Path) -> PathBuf {
    store_path.join(RESOLVED_HOMESERVER_FILE)
}

/// The homeserver URL a previous successful build resolved, if one was ever
/// recorded and still parses.
fn read_resolved_homeserver(store_path: &Path) -> Option<String> {
    if store_path.as_os_str().is_empty() {
        return None;
    }
    let text = std::fs::read_to_string(resolved_homeserver_path(store_path)).ok()?;
    let text = text.trim();
    // Parsed, not trusted: an empty or truncated file falls back to discovery.
    let url = url::Url::parse(text).ok()?;
    if !matches!(url.scheme(), "http" | "https") || url.host().is_none() {
        return None;
    }
    Some(url.to_string())
}

/// Record what the SDK resolved, so the next start does not need the
/// server. Best effort.
fn record_resolved_homeserver(store_path: &Path, url: &str) {
    if store_path.as_os_str().is_empty() {
        return;
    }
    let path = resolved_homeserver_path(store_path);
    // Rewrite only on change; this runs on every login and restore.
    if read_resolved_homeserver(store_path).as_deref() == Some(url) {
        return;
    }
    let tmp = path.with_extension("tmp");
    // Remove any stale temp file first: `OpenOptions::mode` is ignored for an
    // existing file, so a crash leftover would keep its old mode.
    let _ = std::fs::remove_file(&tmp);
    let mut options = OpenOptions::new();
    options.create_new(true).write(true);
    #[cfg(unix)]
    {
        options.mode(0o600);
    }
    let Ok(mut file) = options.open(&tmp) else { return };
    if file.write_all(url.as_bytes()).is_err() || file.write_all(b"\n").is_err() {
        let _ = std::fs::remove_file(&tmp);
        return;
    }
    drop(file);
    let _ = std::fs::rename(&tmp, &path);
}

async fn build_client(homeserver: &str, store_path: &Path) -> Result<Client, String> {
    build_client_with(HomeserverInput::Discover(homeserver), store_path).await
}

/// The E2EE settings every client is built with (decided 2026-10-13, CLAUDE.md
/// §9). `AfterDecryptionFailure` makes matrix-sdk download the one missing key
/// from the trusted backup whenever an event fails to decrypt, in any room and
/// on any path (sync, pagination, retry): it installs the UTD event handler
/// and the `BackupDownloadTask` (deduplicated per session, with its own
/// failure backoff), and does nothing while this device holds no backup
/// decryption key. It replaces `OneShot`, which downloaded the WHOLE backup in
/// one request when the backup key arrived and never again; the whole-room
/// part of that now comes from Lightning's own per-room pass
/// (`download_backup_keys_for_room`), on the backup-enabled edge for the open
/// room and on every room open after.
///
/// Both automatic creations stay off: no cross-signing bootstrap and no backup
/// version is ever made without the user asking. Keys only flow from a backup
/// the SDK enabled after SDK-confirmed verification or an explicit
/// recovery-key entry.
fn encryption_settings() -> matrix_sdk::encryption::EncryptionSettings {
    matrix_sdk::encryption::EncryptionSettings {
        backup_download_strategy:
            matrix_sdk::encryption::BackupDownloadStrategy::AfterDecryptionFailure,
        auto_enable_cross_signing: false,
        auto_enable_backups: false,
    }
}

async fn build_client_with(
    homeserver: HomeserverInput<'_>,
    store_path: &Path,
) -> Result<Client, String> {
    let encryption_settings = encryption_settings();
    // Threading support puts m.thread events into per-thread event-cache
    // chunks, without which thread timelines open empty; it also makes unread
    // and receipt computation thread-aware.
    //
    // `with_subscriptions: false` does MORE than leave out the MSC4308 sliding
    // sync extension. In matrix-sdk 0.18 `Client::enabled_thread_subscriptions()`
    // answers false for it before asking the server, and that one switch also
    // gates (a) the push context's thread-subscription lookup, so the MSC4306
    // `subscribed_thread` / `unsubscribed_thread` rules never match and a
    // thread's Follow state does not change THIS device's verdicts, and (b) the
    // event cache's automatic subscription when we reply in or are mentioned in
    // a thread. Explicit Follow / Unfollow (`mx_rust_thread_set_subscribed`)
    // still reaches the server, which applies it to push. Replying in a thread
    // subscribes automatically anyway: timeline.rs does it after each of our
    // own thread sends, on a server that advertises MSC4306
    // (`subscribe_after_thread_send`). Being MENTIONED in a thread does not.
    // The flag stays false (CLAUDE.md §8 pins it); see the 2026-10-15
    // notification sweep notes.
    //
    // An empty store path means an in-memory store, used by the OAuth bootstrap
    // (see oauth.rs), which must not create a store before `whoami` says which
    // account it belongs to.
    //
    // `handle_refresh_tokens` defaults to false in matrix-sdk 0.18; without it
    // a 401 is not renewed. Rotated tokens must also be persisted
    // (oauth::spawn_token_persistence), or a reused refresh token looks like
    // compromise to an OAuth 2.1 server.
    //
    // Delegation: users type a server name, and the client API may live
    // elsewhere (/.well-known/matrix/client). `server_name_or_homeserver_url()`
    // does discovery and verifies the result; `homeserver_url()` does neither,
    // which broke delegated servers (issue #5).
    let base = Client::builder();
    // Switch trace (S8): only the discovering build's `build()` is discovery.
    let discovering = matches!(homeserver, HomeserverInput::Discover(_));
    // Decides whether this build needs a live server.
    let base = match homeserver {
        HomeserverInput::Discover(value) => base.server_name_or_homeserver_url(value),
        HomeserverInput::Url(value) => base.homeserver_url(value),
    };
    // One snapshot drives the client's crypto sync service AND the custom
    // event-cache/media store locks; changing only StoreConfig misses crypto.
    let store_lock_config = storelocking::config();
    let mut builder = base
        .cross_process_store_config(store_lock_config.clone())
        .user_agent(USER_AGENT)
        .handle_refresh_tokens()
        .with_encryption_settings(encryption_settings)
        .with_threading_support(ThreadingSupport::Enabled { with_subscriptions: false });

    // MSC4153: one switch drives both halves, since setting only one gives an
    // asymmetric client.
    //   send    - IdentityBasedStrategy (Element's "exclude insecure devices").
    //   receive - CrossSignedOrLegacy, never CrossSigned, which would make
    //             existing legacy Megolm history undecryptable.
    //
    // matrix-sdk's exemption list for this mode (`olm/account.rs`,
    // `is_from_verified_device_or_allowed_type`) does not include
    // `io.element.call.encryption_keys`. With this on, call media keys from
    // non-cross-signed devices are refused while their room keys are accepted,
    // making such calls one-way silent. That is correct (we never promote
    // trust), but any user-facing copy for this setting must say so. The
    // `m.room.encrypted` to-device counter in install_event_handlers is what
    // makes it observable.
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
    // The state and crypto stores exactly as `sqlite_store(path, None)`
    // opened them; the event cache and the media store where cachelayout.rs
    // placed them (the account's cache directory once moved), the media store
    // encrypted with the account's key, or in memory without one
    // (mediastore.rs).
    let mut store_probe = None;
    if !store_path.as_os_str().is_empty() {
        let (store_config, probe) = switchtrace::timed(
            Some(switchtrace::RestoreStep::StoreOpen),
            mediastore::open_account_stores(store_path, store_lock_config),
        )
        .await?;
        builder = builder.store_config(store_config);
        store_probe = Some(probe);
    }
    let client = switchtrace::timed(
        discovering.then_some(switchtrace::RestoreStep::Discovery),
        builder.build(),
    )
    .await
    .map_err(|err| format_matrix_error("failed to build Matrix Rust SDK client", err))?;
    // Recorded so retirement can wait for these very stores to be dropped,
    // rather than for its handle to be (storeclose.rs, GitHub #2).
    if let Some(mut probe) = store_probe {
        probe.attach_client(&client);
        storeclose::register(store_path, probe);
    }
    // Enable upload progress (off by default in the SDK), without which
    // `EventSendState::NotSentYet` carries no progress and the UI's upload bar
    // stays indeterminate.
    //
    // `respawn_tasks_for_rooms_with_unsent_requests()` does not belong here;
    // see `resume_unsent_requests`.
    client.send_queue().enable_upload_progress(true);
    // Re-enable a room's queue after a recoverable send error (sendrecovery.rs).
    sendrecovery::spawn(client.clone(), sendrecovery::RecoveryPolicy::production());
    // 0600 on the databases just created, from the one place all login paths
    // pass through: the SDK sqlite files, their -wal/-shm siblings and the
    // search index are created here at the process umask. Best effort.
    if !store_path.as_os_str().is_empty() {
        restrict_store_permissions(store_path);
        // Record the resolved URL on every successful build, so the account can be
        // opened again with its homeserver down. See build_client_for_restore.
        record_resolved_homeserver(store_path, client.homeserver().as_str());
    }
    // Media-store retention policy. Without one the store grows without bound,
    // and because it serializes all access on one write connection, a huge
    // blob INSERT stalls every other media fetch. The per-item cap is 100 MiB
    // of payload (rooms::MEDIA_STORE_MAX_FILE_BYTES, so a phone video from an
    // encrypted room is kept), held by Lightning before the write
    // (mediafetch::cache_put); the policy's own max_file_size is on the
    // ENCODED row and is set to admit exactly that (rooms.rs). Declared-
    // oversize fetches skip the cache, and unencrypted ones are kept as files
    // instead. 1 GiB in total; SDK defaults otherwise.
    let policy = rooms::media_retention_policy();
    let preserve_caches = mediastore::preserve_caches_for_audit();
    let policy = if preserve_caches {
        // Do not let a cache read trigger automatic eviction during the audit.
        // Normal startup installs the regular policy again.
        policy.with_cleanup_frequency(None)
    } else {
        policy
    };
    // Best effort; the error may contain the store path, so it is not logged.
    if client.media().set_media_retention_policy(policy).await.is_ok()
        && !preserve_caches
    {
        // Sweep blobs cached before the policy existed. Runs once per client build;
        // the SDK debounces the real work to daily. Bounded, and cancelled with the
        // shared runtime before any store deletion.
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
    events: &Arc<Mutex<VecDeque<String>>>,
    token_sink: Option<&sessiontokens::Sink>,
) -> Result<(Client, tokio::sync::broadcast::Receiver<matrix_sdk::SessionChange>), String> {
    let user_id: OwnedUserId = UserId::parse(user_id)
        .map_err(|err| format!("invalid stored Matrix user id: {err}"))?
        .to_owned();
    let device_id: OwnedDeviceId = device_id.to_owned().into();
    // Carry the refresh token: needed for refreshable password sessions and
    // mandatory for OAuth, whose access tokens are short-lived.
    let session = MatrixSession {
        meta: SessionMeta { user_id, device_id },
        tokens: SessionTokens { access_token, refresh_token },
    };
    restore_client_with_session(homeserver, store_path, session, events, token_sink).await
}

/// How long a restore's discovering client build may take before falling
/// back to the recorded URL.
///
/// Discovery is tried first on every restore so a changed `/.well-known`
/// delegation is followed. The budget bounds how long a dead server keeps
/// the user at a blank window. It covers the whole build, including opening
/// and migrating the stores, so a slow disk can trip it against a healthy
/// server; that only shows a brief, self-correcting offline label.
const RESTORE_BUILD_BUDGET: std::time::Duration = std::time::Duration::from_secs(10);

/// Build the client a restore needs, with the server down as a supported
/// case.
///
/// `build_client` discovers and verifies the homeserver over HTTP, but
/// `restore_session()` only reads the store. So try discovery, and if the
/// build fails, rebuild against the URL the last successful build recorded
/// and let sync report offline; the room list is served from the state
/// store meanwhile.
///
/// Credential rejection is unaffected: nothing here authenticates, so
/// `M_UNKNOWN_TOKEN` still first appears on sync. Any build failure reaches
/// the fallback, including a broken delegation; a store-level failure fails
/// identically on the retry, and the original error is returned.
async fn build_client_for_restore(
    homeserver: &str,
    store_path: &Path,
    events: &Arc<Mutex<VecDeque<String>>>,
) -> Result<Client, String> {
    let online = tokio::time::timeout(
        RESTORE_BUILD_BUDGET,
        build_client(homeserver, store_path),
    )
    .await;
    let reason = match online {
        Ok(Ok(client)) => return Ok(client),
        Ok(Err(err)) => err,
        Err(_) => format!(
            "the homeserver did not answer within {} seconds",
            RESTORE_BUILD_BUDGET.as_secs()
        ),
    };
    let Some(url) = read_resolved_homeserver(store_path) else {
        // Nothing recorded: return the original failure.
        return Err(reason);
    };
    let client = switchtrace::timed(
        Some(switchtrace::RestoreStep::FallbackBuild),
        build_client_with(HomeserverInput::Url(&url), store_path),
    )
        .await
        // Keep both: the original failure says why the server was not used,
        // the fallback's says why the store could not be opened without it.
        .map_err(|fallback| format!("{reason}; restoring from the recorded homeserver URL failed too: {fallback}"))?;
    // No URL, server name or account id: C++ only needs to know the data came
    // off the disk.
    enqueue(events, json!({ "type": "session_restored_offline" }));
    switchtrace::note_offline_fallback();
    Ok(client)
}

/// Also returns a session-change receiver subscribed BEFORE
/// `restore_session()`, which starts matrix-sdk's E2EE initialization task and
/// can already rotate the tokens; hand it to
/// `oauth::spawn_token_persistence_with` so no rotation goes unannounced.
async fn restore_client_with_session(
    homeserver: &str,
    store_path: &Path,
    session: MatrixSession,
    events: &Arc<Mutex<VecDeque<String>>>,
    token_sink: Option<&sessiontokens::Sink>,
) -> Result<(Client, tokio::sync::broadcast::Receiver<matrix_sdk::SessionChange>), String> {
    restore_client_with_session_then(
        homeserver, store_path, session, events, token_sink, |_| async {},
    )
    .await
}

/// `restore_client_with_session` with `after_restore` run right after
/// `restore_session()`, standing for what that call set running, so a test
/// can rotate the tokens at exactly that point. Production passes nothing.
pub(crate) async fn restore_client_with_session_then<F, Fut>(
    homeserver: &str,
    store_path: &Path,
    session: MatrixSession,
    events: &Arc<Mutex<VecDeque<String>>>,
    token_sink: Option<&sessiontokens::Sink>,
    after_restore: F,
) -> Result<(Client, tokio::sync::broadcast::Receiver<matrix_sdk::SessionChange>), String>
where
    F: FnOnce(Client) -> Fut,
    Fut: std::future::Future<Output = ()>,
{
    let client = build_client_for_restore(homeserver, store_path, events).await?;
    // Before restore_session(), whose E2EE task can already refresh.
    if let Some(sink) = token_sink {
        sink.install_on(&client);
    }
    let changes = client.subscribe_to_session_changes();
    switchtrace::timed(
        Some(switchtrace::RestoreStep::Restore),
        client.matrix_auth().restore_session(session, RoomLoadSettings::default()),
    )
    .await
    .map_err(|err| format_matrix_error("Matrix Rust SDK session restore failed", err))?;
    after_restore(client.clone()).await;
    switchtrace::timed(
        Some(switchtrace::RestoreStep::ResumeUnsent),
        resume_unsent_requests(&client),
    )
    .await;
    Ok((client, changes))
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

/// The C++ row kind for one `m.room.message` msgtype. `None` means no typed
/// row; each caller keeps its own fallback (live sync drops it, the reload
/// path renders the plain-text body).
///
/// The set of kinds matters downstream: `handleTimelineEvent`'s
/// `countsAsActivity` excludes `StateChange` and `CallEvent`, so adding
/// `"state"`, `"call"`, `"sticker"` or `"poll"` here changes which rows
/// raise a room's last activity. Read that predicate before extending this.
pub(crate) fn typed_message_row_kind(msgtype: &str) -> Option<&'static str> {
    Some(match msgtype {
        "m.text" => "text",
        "m.notice" => "notice",
        "m.emote" => "emote",
        "m.image" => "image",
        "m.video" => "video",
        "m.audio" => "audio",
        "m.file" => "file",
        "m.location" => "location",
        _ => return None,
    })
}

/// Whether an audio message is a voice message (MSC3245 `org.matrix.msc3245.voice`),
/// so a notification for a room with no open timeline can say so instead of
/// "an audio file". Pure and unit-tested.
pub(crate) fn message_is_voice(msgtype: &MessageType) -> bool {
    matches!(msgtype, MessageType::Audio(content) if content.voice.is_some())
}

/// The `media_filename` for a row of `kind`, matching the live-timeline
/// producer (`rust/src/timeline.rs`) kind for kind:
///
///  * `file`, `image`, `video`, `audio`: MSC2530's `filename`, falling back
///    to the body (Element puts the caption in the body; Sable sends an empty
///    body).
///  * everything else, including `location`, has no file; a location's body
///    is the sender's own words and stays the body.
///
/// `"sticker"` falls to the empty arm here but not in the live producer; if
/// the sync path ever emits sticker rows, add an arm.
pub(crate) fn media_filename_for_kind(
    kind: &str,
    body: &str,
    explicit_filename: Option<&str>,
) -> String {
    match kind {
        "file" | "image" | "video" | "audio" => match explicit_filename {
            Some(name) if !name.trim().is_empty() => name.to_owned(),
            _ => body.to_owned(),
        },
        _ => String::new(),
    }
}

/// `unsigned.age` in milliseconds, when the server sent one.
fn unsigned_age_ms(age: Option<matrix_sdk::ruma::Int>) -> Option<u64> {
    age.map(|age| i64::from(age).max(0) as u64)
}

/// Write `backlog` on a sync-handler payload (see syncnotify::CatchupClock)
/// and return it.
fn mark_sync_backlog(
    notify: &syncnotify::SyncNotify,
    event: &mut serde_json::Value,
    timestamp_ms: u64,
    age_ms: Option<u64>,
) -> bool {
    let backlog = notify.catchup.is_backlog(timestamp_ms, age_ms, push_verdict::now_ms());
    if let Some(object) = event.as_object_mut() {
        object.insert("backlog".to_owned(), backlog.into());
    }
    backlog
}

/// Whether a redaction by `redactor` removes an event sent by
/// `target_sender` (`None`: not in the local cache). `can_redact_others`:
/// the room's power levels let `redactor` redact other people's events
/// (`None`: unknown). Only a positive refusal refuses.
fn redaction_applies(
    target_sender: Option<&matrix_sdk::ruma::UserId>,
    redactor: &matrix_sdk::ruma::UserId,
    can_redact_others: Option<bool>,
) -> bool {
    match target_sender {
        Some(sender) if sender != redactor => can_redact_others != Some(false),
        _ => true,
    }
}

/// The event's `origin_server_ts` from the local event cache, with no request.
async fn cached_event_ts(room: &Room, event_id: &EventId) -> Option<u64> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    event.raw().get_field::<u64>("origin_server_ts").ok().flatten()
}

/// The room's current `m.fully_read` event, from the local account data.
async fn current_fully_read(room: &Room) -> Option<OwnedEventId> {
    use matrix_sdk::ruma::events::fully_read::FullyReadEventContent;
    let raw = room.account_data_static::<FullyReadEventContent>().await.ok()??;
    raw.deserialize().ok().map(|event| event.content.event_id)
}

/// Whether moving `m.fully_read` from an event at `current_ts` to one at
/// `new_ts` would move it backwards. Unknown on either side is not proof of
/// that, so it never refuses.
fn fully_read_would_move_back(current_ts: Option<u64>, new_ts: Option<u64>) -> bool {
    matches!((current_ts, new_ts), (Some(current), Some(new)) if new < current)
}

/// The sender of an event this device already holds in its event cache. Never
/// a request: `None` when the cache does not have it.
async fn cached_event_sender(room: &Room, event_id: &EventId) -> Option<OwnedUserId> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    event.raw().get_field::<OwnedUserId>("sender").ok().flatten()
}

/// `(replaces_event_id, thread_root_id)` from an encrypted event's cleartext
/// `m.relates_to`, empty when absent.
fn encrypted_relation_ids(
    relation: Option<&matrix_sdk::ruma::events::room::encrypted::Relation>,
) -> (String, String) {
    use matrix_sdk::ruma::events::room::encrypted::Relation;
    match relation {
        Some(Relation::Replacement(replacement)) => {
            (replacement.event_id.to_string(), String::new())
        }
        Some(Relation::Thread(thread)) => (String::new(), thread.event_id.to_string()),
        _ => (String::new(), String::new()),
    }
}

/// The event a message replies to, empty when it is not a reply. A thread
/// message counts only when its `m.in_reply_to` is real rather than the
/// fallback every thread message carries for clients without threads.
fn reply_target_of<C>(
    relation: Option<&matrix_sdk::ruma::events::room::message::Relation<C>>,
) -> String {
    use matrix_sdk::ruma::events::room::message::Relation;
    match relation {
        Some(Relation::Reply(reply)) => reply.in_reply_to.event_id.to_string(),
        Some(Relation::Thread(thread)) if !thread.is_falling_back => thread
            .in_reply_to
            .as_ref()
            .map(|reply| reply.event_id.to_string())
            .unwrap_or_default(),
        _ => String::new(),
    }
}

/// The `timeline_event` payload for one room message, before its push
/// verdict and backlog mark. Shared by the sync handler and the late
/// decryption path, so a message reads the same whichever way it arrived.
/// `None` for an in-room verification request, which is not a message.
fn room_message_event_json(
    content: &RoomMessageEventContent,
    event_id: &EventId,
    sender: &UserId,
    timestamp_ms: u64,
    own_user_id: &UserId,
    is_encrypted: bool,
) -> Option<serde_json::Value> {
    use matrix_sdk::ruma::events::room::message::Relation;
    // An MSC4274 gallery arrives as the row of its primary item, as in the
    // live producer, captioned with the gallery caption, never Sable's
    // generated `[name: mxc://…]` body.
    let gallery = crate::timeline::parse_gallery(&content.msgtype)
        .filter(|g| !g.items.is_empty());
    let row_msgtype = match &gallery {
        Some(g) => g
            .items
            .iter()
            .find(|item| matches!(item, MessageType::Image(_)))
            .unwrap_or(&g.items[0]),
        None => &content.msgtype,
    };
    // See typed_message_row_kind. A msgtype with no typed row (a server
    // notice, a bridge's or bot's custom type) is forwarded as text with its
    // body: the push rules notify for any m.room.message, and the open room
    // and the reload path already show its body.
    let kind = match typed_message_row_kind(row_msgtype.msgtype()) {
        Some(kind) => kind,
        None if matches!(row_msgtype, MessageType::VerificationRequest(_)) => return None,
        None => "text",
    };
    let body = match &gallery {
        Some(g) => g.caption.clone(),
        // Text-like bodies only: a media body is a filename.
        None if matches!(kind, "text" | "notice" | "emote") => {
            crate::timeline::strip_reply_fallback(content.body()).to_owned()
        }
        None => content.body().to_owned(),
    };
    // MSC2530's `filename`, as in the live producer. A multi-item gallery has
    // no single name.
    let explicit_filename = match row_msgtype {
        MessageType::File(content) => content.filename.as_deref(),
        MessageType::Image(content) => content.filename.as_deref(),
        MessageType::Video(content) => content.filename.as_deref(),
        MessageType::Audio(content) => content.filename.as_deref(),
        _ => None,
    };
    let media_filename = match &gallery {
        Some(g) if g.items.len() > 1 => String::new(),
        _ => media_filename_for_kind(kind, row_msgtype.body(), explicit_filename),
    };

    // Notification metadata for rooms without a live timeline: m.mentions and
    // the m.thread root, matching the live-timeline payload.
    let (mentions_me, mentions_room) = match &content.mentions {
        Some(mentions) => (mentions.user_ids.contains(own_user_id), mentions.room),
        None => (false, false),
    };
    let thread_root_id = match &content.relates_to {
        Some(Relation::Thread(thread)) => thread.event_id.to_string(),
        _ => String::new(),
    };
    // An edit is not a new message: its body is the "* " fallback, so the
    // replacement text travels as `new_body`. C++ keeps an edit out of the
    // room's preview, order and mirror, and still notifies (an edit that newly
    // mentions you notifies: .m.rule.is_user_mention precedes
    // .m.rule.suppress_edits).
    let (replaces_event_id, new_body) = match &content.relates_to {
        Some(Relation::Replacement(replacement)) => (
            replacement.event_id.to_string(),
            replacement.new_content.msgtype.body().to_owned(),
        ),
        _ => (String::new(), String::new()),
    };
    trace_remote_edit("readable", event_id.as_str(), &replaces_event_id, is_encrypted);
    Some(json!({
        "event_id": event_id.to_string(),
        "sender": sender.to_string(),
        "body": body,
        "media_filename": media_filename,
        "msgtype": kind,
        "media_voice": message_is_voice(row_msgtype),
        "timestamp_ms": timestamp_ms,
        "is_encrypted": is_encrypted,
        "is_decrypted": is_encrypted,
        "undecryptable": false,
        "mentions_me": mentions_me,
        "mentions_room": mentions_room,
        "thread_root_id": thread_root_id,
        "in_reply_to": reply_target_of(content.relates_to.as_ref()),
        "replaces_event_id": replaces_event_id,
        "new_body": new_body,
        // Legacy field for C++ builds that still read `decrypted`.
        "decrypted": is_encrypted,
    }))
}

/// Opt-in correlation across sync, late decryption and the aggregated Set.
/// Same hashed IDs as C++; never content, ciphertext or key material.
fn trace_remote_edit(stage: &str, event_id: &str, target: &str, encrypted: bool) {
    if !target.is_empty() && std::env::var_os("LIGHTNING_EDIT_TRACE").is_some() {
        eprintln!("edit-trace stage={stage} edit={} target={} encrypted={encrypted}",
            redact_matrix_id(event_id), redact_matrix_id(target));
    }
}

/// What a poll-start row needs, from either the stable or the unstable type.
struct PollStart {
    question: String,
    thread_root_id: String,
    in_reply_to: String,
}

/// `None` for a poll edit, which is not a new poll.
fn unstable_poll_start(
    content: &matrix_sdk::ruma::events::poll::unstable_start::UnstablePollStartEventContent,
) -> Option<PollStart> {
    use matrix_sdk::ruma::events::{
        poll::unstable_start::UnstablePollStartEventContent,
        room::message::RelationWithoutReplacement,
    };
    let UnstablePollStartEventContent::New(content) = content else { return None };
    let (thread_root_id, in_reply_to) = match &content.relates_to {
        Some(RelationWithoutReplacement::Thread(thread)) => (
            thread.event_id.to_string(),
            if thread.is_falling_back {
                String::new()
            } else {
                thread.in_reply_to.as_ref().map(|r| r.event_id.to_string()).unwrap_or_default()
            },
        ),
        Some(RelationWithoutReplacement::Reply(reply)) => {
            (String::new(), reply.in_reply_to.event_id.to_string())
        }
        _ => (String::new(), String::new()),
    };
    Some(PollStart { question: content.poll_start.question.text.clone(), thread_root_id, in_reply_to })
}

/// `None` for a poll edit, which is not a new poll.
fn stable_poll_start(
    content: &matrix_sdk::ruma::events::poll::start::PollStartEventContent,
) -> Option<PollStart> {
    use matrix_sdk::ruma::events::room::message::Relation;
    if matches!(content.relates_to, Some(Relation::Replacement(_))) {
        return None;
    }
    let thread_root_id = match &content.relates_to {
        Some(Relation::Thread(thread)) => thread.event_id.to_string(),
        _ => String::new(),
    };
    let question = content
        .poll
        .question
        .text
        .find_plain()
        .or_else(|| content.text.find_plain())
        .unwrap_or_default()
        .to_owned();
    Some(PollStart {
        question,
        thread_root_id,
        in_reply_to: reply_target_of(content.relates_to.as_ref()),
    })
}

/// A poll start as a `timeline_event` payload (kind `poll`, the question as
/// its body), before its push verdict and backlog mark.
fn poll_event_json(
    poll: &PollStart,
    event_id: &EventId,
    sender: &UserId,
    timestamp_ms: u64,
    is_encrypted: bool,
) -> serde_json::Value {
    json!({
        "event_id": event_id.to_string(),
        "sender": sender.to_string(),
        "body": poll.question,
        "media_filename": "",
        "msgtype": "poll",
        "media_voice": false,
        "timestamp_ms": timestamp_ms,
        "is_encrypted": is_encrypted,
        "is_decrypted": is_encrypted,
        "undecryptable": false,
        "mentions_me": false,
        "mentions_room": false,
        "thread_root_id": poll.thread_root_id,
        "in_reply_to": poll.in_reply_to,
        "replaces_event_id": "",
        "new_body": "",
        "decrypted": is_encrypted,
    })
}

/// The `event_decrypted` payload's `event` for an event the sync handler
/// forwarded as undecryptable and the event-cache redecryptor has since
/// decrypted: the same shape as a `timeline_event`, with the push verdict
/// computed on the DECRYPTED content (so m.mentions and keywords count, as
/// in Element X), and the `backlog` mark the undecryptable copy carried.
///
/// Read from the local event cache only; `None` when the event is gone, still
/// encrypted, or not a message or poll.
async fn decrypted_event_json(
    room: &Room,
    event_id: &EventId,
    backlog: bool,
    verdicts: &push_verdict::SyncVerdicts,
) -> Option<serde_json::Value> {
    use matrix_sdk::ruma::events::{
        AnySyncMessageLikeEvent, AnySyncTimelineEvent, SyncMessageLikeEvent,
    };
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let cached = cache.find_event(event_id).await.ok()??;
    // Only a decryption counts: anything else is still the encrypted envelope.
    cached.encryption_info()?;
    let raw = cached.raw().clone();
    let mut event = match raw.deserialize().ok()? {
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
            SyncMessageLikeEvent::Original(ev),
        )) => room_message_event_json(
            &ev.content,
            &ev.event_id,
            &ev.sender,
            u64::from(ev.origin_server_ts.get()),
            room.own_user_id(),
            true,
        )?,
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::UnstablePollStart(
            SyncMessageLikeEvent::Original(ev),
        )) => poll_event_json(
            &unstable_poll_start(&ev.content)?,
            &ev.event_id,
            &ev.sender,
            u64::from(ev.origin_server_ts.get()),
            true,
        ),
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::PollStart(
            SyncMessageLikeEvent::Original(ev),
        )) => poll_event_json(
            &stable_poll_start(&ev.content)?,
            &ev.event_id,
            &ev.sender,
            u64::from(ev.origin_server_ts.get()),
            true,
        ),
        _ => return None,
    };
    if let Some(object) = event.as_object_mut() {
        object.insert("backlog".to_owned(), backlog.into());
    }
    // The redecryptor stores the actions it computed on the decrypted event;
    // without a push context it had none, and the rules are asked here (with
    // SyncVerdicts' fallback context when the SDK cannot build one).
    let verdict = match cached.push_actions() {
        Some(actions) => verdicts.verdict(room, actions, raw.json()).await,
        None => match room.push_context().await.ok().flatten() {
            Some(context) => Some(push_verdict::PushVerdict::from_actions(
                &context.for_event(&raw).await,
            )),
            // No context now, so no actions to read: the fallback alone, never
            // "quiet" for a room once remembered as having our member.
            None => verdicts.without_sdk_context(room, raw.json()).await,
        },
    };
    push_verdict::PushVerdict::write(verdict, &mut event);
    Some(event)
}

fn install_event_handlers(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    active_sas: KeyedFlowSlot<SasVerification>,
    active_qr: KeyedFlowSlot<QrVerification>,
    sync_notify: Arc<syncnotify::SyncNotify>,
) {
    // A to-device handler on `m.room.encrypted` fires exactly for events the SDK
    // could not decrypt: matrix-sdk hands those to handlers as the original
    // envelope, while decrypted ones dispatch under their inner type. This
    // distinguishes "the peer never sent it" from "it arrived and we cannot
    // read it".
    //
    // It cannot say which cause (missing ciphertext for our key, unknown
    // sender device keys, or STRICT_DEVICE_TRUST refusing it). The reason is on
    // `SyncResponse.to_device`, which matrix-sdk 0.18 exposes only via the
    // classic sync callback, not the sliding-sync path; use the SDK tracing
    // bridge to separate them.
    //
    // Sanitized and rate limited: sender id and a count only (first, tenth,
    // every hundredth). rtc.rs has a second handler on this type for peers we
    // sent a call key to; both are intentional.
    let utd_events = Arc::clone(&events);
    let utd_counts: Arc<Mutex<HashMap<OwnedUserId, u64>>> =
        Arc::new(Mutex::new(HashMap::new()));
    client.add_event_handler(move |ev: ToDeviceRoomEncryptedEvent| {
        let events = Arc::clone(&utd_events);
        let counts = Arc::clone(&utd_counts);
        async move {
            let count = {
                let Ok(mut guard) = counts.lock() else { return };
                // Bounded: a diagnostic, not a ledger.
                if guard.len() > 256 {
                    guard.clear();
                }
                let entry = guard.entry(ev.sender.clone()).or_insert(0);
                *entry += 1;
                *entry
            };
            if count == 1 || count == 10 || count % 100 == 0 {
                enqueue(&events, json!({
                    "type": "to_device_undecryptable",
                    "sender": ev.sender.to_string(),
                    "count": count,
                }));
            }
        }
    });

    // Incoming verification requests. matrix-sdk 0.18 has no public request
    // stream, so a to-device handler observes them and hydrates the request via
    // `get_verification_request(user, flow_id)`. Only flow id, mxid, device id,
    // is_self_verification, SAS emoji and a QR module grid ever cross the FFI.
    let verif_events = Arc::clone(&events);
    let verif_slot = Arc::clone(&active_request);
    let verif_sas_slot = Arc::clone(&active_sas);
    let verif_qr_slot = Arc::clone(&active_qr);
    // The Client is an INJECTED argument, never a captured clone: a handler
    // lives inside the Client it is registered on, so a captured Client is a
    // reference cycle and the Client (with every SQLite store it holds) is
    // never dropped. That kept the whole account store open until the process
    // exited, and on Windows sign-out could not delete it (GitHub #2).
    client.add_event_handler(
        move |ev: ToDeviceKeyVerificationRequestEvent, client: Client| {
            let events = Arc::clone(&verif_events);
            let slot = Arc::clone(&verif_slot);
            let sas_slot = Arc::clone(&verif_sas_slot);
            let qr_slot = Arc::clone(&verif_qr_slot);
            async move {
                let flow_id = ev.content.transaction_id.to_string();
                let Some(request) = client
                    .encryption()
                    .get_verification_request(&ev.sender, &flow_id)
                    .await
                else {
                    return;
                };

                // Never evict a live flow: overwriting it orphaned the running driver,
                // whose cleanup then cleared the newcomer's handle. A dead occupant is
                // cleared in passing. Cancel the newcomer on the wire rather than leave
                // the peer to the SDK's 10-minute timeout; it is not surfaced in the UI.
                if flow_slots_are_live(&slot, &sas_slot, &qr_slot) {
                    let _ = request.cancel().await;
                    return;
                }

                // The peer's device id tells the user which session is asking. It comes
                // from the request state; `their_supported_methods()` only lists methods.
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
                let watched_flow = request.flow_id().to_string();
                if let Ok(mut g) = slot.lock() {
                    *g = Some(request);
                }
                // Not a verification task: it holds no Client between ticks.
                tokio::spawn(watch_pending_request(Arc::clone(&slot), Arc::clone(&events), watched_flow));
            }
        },
    );

    // m.typing replaces the room's whole typing set each time. Bound display
    // metadata resolution while keeping enough ids for one/two/many wording.
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
    // Membership changes from sync: a per-room poke so an open People panel
    // and the mention roster refetch (sync produces no members snapshot).
    // Only the room id crosses.
    //
    // Rate-limited per room: m.room.member also covers profile changes, and a
    // bridged room can sync several per second. At most one leading poke per
    // second, plus one trailing poke per suppressed burst so the last change
    // is not missed.
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

    // Another client changed `m.room.pinned_events`. Only the room id crosses;
    // C++ re-reads through the normal fetch, so remote and local pins take the
    // same path. Not rate-limited: pinning happens at human frequency.
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

    // A DM peer's own member event: the avatar that conversation wears now,
    // so its row follows a changed face (DirectAvatarResolver asks the
    // profile once per session). Only the m.direct target's own join
    // crosses; human-paced, so not throttled like the poke above.
    let peer_avatar_events = Arc::clone(&events);
    client.add_event_handler(move |ev: SyncRoomMemberEvent, room: Room| {
        let events = Arc::clone(&peer_avatar_events);
        async move {
            let targets: Vec<String> =
                room.direct_targets().iter().map(ToString::to_string).collect();
            if let Some(payload) =
                direct_peer_avatar_payload(room.room_id().as_str(), &ev, &targets)
            {
                enqueue(&events, payload);
            }
        }
    });

    // m.room.tombstone: the room was replaced, and the "continue in successor"
    // banner must appear immediately. Not rate-limited (once per room
    // lifetime). Carries the successor id, taken from the SDK's
    // `successor_room()` so there is one parse path.
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

    // A shared chat background changed (Lightning's own state event, which
    // sliding sync never delivers as state). Only the room id crosses; C++
    // re-reads the scopes it shows. See backdrop::install_change_handler.
    backdrop::install_change_handler(client, Arc::clone(&events));

    // This account's personal backgrounds changed in account data (another
    // device set or removed one, or our own write came back). Only the scope
    // crosses; C++ re-reads it. See bgsync::install_change_handlers.
    bgsync::install_change_handlers(client, Arc::clone(&events));
    // Synced preferences (settingsync.rs): same pattern, one payload.
    settingsync::install_change_handlers(client, Arc::clone(&events));

    // MSC4278 media-preview config and Element's recent emoji changed in
    // account data. Only the type crosses; C++ re-reads it.
    accountprefs::install_change_handlers(client, Arc::clone(&events));

    // A room an administrator deleted from another of the user's sessions
    // (serveradmin.rs): the server purged it, so sync never says it is gone.
    serveradmin::install_deleted_room_handler(client, Arc::clone(&events));

    // Opt-in (LIGHTNING_IO_TRACE): per-thread disk-write counters, see iotrace.rs.
    iotrace::start_if_enabled(Arc::clone(&events));

    // A power-level change invalidates cached permission flags now. Routed
    // through the existing members poke, since the member snapshot carries
    // both per-member levels and the viewer's own permissions.
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

    // Room messages, decrypted or plaintext (the SDK dispatches both here).
    // `encryption_info` is Some only when the SDK decrypted the payload, which
    // gives C++ its (is_encrypted, is_decrypted) pair. Ciphertext is never
    // forwarded; the encrypted handler below sends an empty body instead.
    // `push_actions` are the account's push rules for the event as the SDK
    // evaluated them during this sync; see push_verdict::SyncVerdicts.
    let sync_verdicts = push_verdict::SyncVerdicts::default();
    let plaintext_events = Arc::clone(&events);
    let plaintext_verdicts = sync_verdicts.clone();
    let plaintext_notify = Arc::clone(&sync_notify);
    client.add_event_handler(
        move |ev: OriginalSyncRoomMessageEvent,
              room: Room,
              encryption_info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>,
              push_actions: Vec<matrix_sdk::ruma::push::Action>,
              raw: matrix_sdk::event_handler::RawEvent| {
            let events = Arc::clone(&plaintext_events);
            let verdicts = plaintext_verdicts.clone();
            let notify = Arc::clone(&plaintext_notify);
            async move {
                let Some(mut event) = room_message_event_json(
                    &ev.content,
                    &ev.event_id,
                    &ev.sender,
                    u64::from(ev.origin_server_ts.get()),
                    room.own_user_id(),
                    encryption_info.is_some(),
                ) else {
                    return;
                };
                mark_sync_backlog(
                    &notify,
                    &mut event,
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                );
                push_verdict::PushVerdict::write(
                    verdicts.verdict(&room, &push_actions, &raw).await,
                    &mut event,
                );
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_event",
                        "room_id": room.room_id().to_string(),
                        "event": event,
                    }),
                );
            }
        },
    );

    // Poll starts (MSC3381, stable and unstable). `.m.rule.poll_start` and
    // `.m.rule.poll_start_one_to_one` notify under the default rules, and the
    // open-room path already emits a poll row; without these a poll in a room
    // with no open timeline was never seen. An edited poll is not a new one.
    let poll_events = Arc::clone(&events);
    let poll_verdicts = sync_verdicts.clone();
    let poll_notify = Arc::clone(&sync_notify);
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::poll::unstable_start::OriginalSyncUnstablePollStartEvent,
              room: Room,
              encryption_info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>,
              push_actions: Vec<matrix_sdk::ruma::push::Action>,
              raw: matrix_sdk::event_handler::RawEvent| {
            let events = Arc::clone(&poll_events);
            let verdicts = poll_verdicts.clone();
            let notify = Arc::clone(&poll_notify);
            async move {
                let Some(start) = unstable_poll_start(&ev.content) else { return };
                let mut event = poll_event_json(
                    &start,
                    &ev.event_id,
                    &ev.sender,
                    u64::from(ev.origin_server_ts.get()),
                    encryption_info.is_some(),
                );
                mark_sync_backlog(
                    &notify,
                    &mut event,
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                );
                push_verdict::PushVerdict::write(
                    verdicts.verdict(&room, &push_actions, &raw).await,
                    &mut event,
                );
                enqueue(&events, json!({
                    "type": "timeline_event",
                    "room_id": room.room_id().to_string(),
                    "event": event,
                }));
            }
        },
    );
    let stable_poll_events = Arc::clone(&events);
    let stable_poll_verdicts = sync_verdicts.clone();
    let stable_poll_notify = Arc::clone(&sync_notify);
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::poll::start::OriginalSyncPollStartEvent,
              room: Room,
              encryption_info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>,
              push_actions: Vec<matrix_sdk::ruma::push::Action>,
              raw: matrix_sdk::event_handler::RawEvent| {
            let events = Arc::clone(&stable_poll_events);
            let verdicts = stable_poll_verdicts.clone();
            let notify = Arc::clone(&stable_poll_notify);
            async move {
                let Some(start) = stable_poll_start(&ev.content) else { return };
                let mut event = poll_event_json(
                    &start,
                    &ev.event_id,
                    &ev.sender,
                    u64::from(ev.origin_server_ts.get()),
                    encryption_info.is_some(),
                );
                mark_sync_backlog(
                    &notify,
                    &mut event,
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                );
                push_verdict::PushVerdict::write(
                    verdicts.verdict(&room, &push_actions, &raw).await,
                    &mut event,
                );
                enqueue(&events, json!({
                    "type": "timeline_event",
                    "room_id": room.room_id().to_string(),
                    "event": event,
                }));
            }
        },
    );

    // Redactions in every room, so C++ can withdraw a notification or an
    // Activity row whose message (or reaction) was deleted. Ids only; the
    // reason is never forwarded. `redacts` moved from the event into its
    // content between room versions, so both are read (as localsearch does).
    let redaction_events = Arc::clone(&events);
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::room::redaction::SyncRoomRedactionEvent,
              room: Room| {
            let events = Arc::clone(&redaction_events);
            async move {
                let Some(original) = ev.as_original() else { return };
                let Some(redacts) = original
                    .redacts
                    .as_ref()
                    .or(original.content.redacts.as_ref())
                else {
                    return;
                };
                // A redaction anyone may SEND is not one that applies: only
                // the event's own sender, or a member allowed to redact
                // others, removes it. An unauthorised one must not withdraw
                // a card or drop an Activity row. Unknowns forward, as
                // before, so a real deletion is never kept on screen.
                let target_sender = cached_event_sender(&room, redacts).await;
                let can_redact_others = match target_sender.as_deref() {
                    Some(sender) if sender != &*original.sender => room
                        .power_levels()
                        .await
                        .ok()
                        .map(|levels| levels.user_can_redact_event_of_other(&original.sender)),
                    _ => None,
                };
                if !redaction_applies(
                    target_sender.as_deref(),
                    &original.sender,
                    can_redact_others,
                ) {
                    return;
                }
                enqueue(&events, json!({
                    "type": "event_redacted",
                    "room_id": room.room_id().to_string(),
                    "redacts": redacts.to_string(),
                    "redaction_event_id": original.event_id.to_string(),
                }));
            }
        },
    );

    // Reactions from sync in any room, for the Activity Center (the timeline
    // diff stream covers only the open room). Ids, sender and a bounded key
    // cross. `target_is_own` comes from the local event cache only (no
    // request): C++'s own-event set holds only what this session sent, so a
    // reaction to an older message of ours was dropped there.
    let reaction_events = Arc::clone(&events);
    let reaction_notify = Arc::clone(&sync_notify);
    client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::reaction::OriginalSyncReactionEvent, room: Room| {
            let events = Arc::clone(&reaction_events);
            let notify = Arc::clone(&reaction_notify);
            async move {
                let key: String = ev.content.relates_to.key.chars().take(32).collect();
                let target = &ev.content.relates_to.event_id;
                let target_is_own =
                    cached_event_sender(&room, target).await.as_deref() == Some(room.own_user_id());
                let backlog = notify.catchup.is_backlog(
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                    push_verdict::now_ms(),
                );
                enqueue(
                    &events,
                    json!({
                        "type": "reaction_event",
                        "room_id": room.room_id().to_string(),
                        "event_id": ev.event_id.to_string(),
                        "target_event_id": target.to_string(),
                        "target_is_own": target_is_own,
                        "sender": ev.sender.to_string(),
                        "key": key,
                        "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                        "backlog": backlog,
                    }),
                );
            }
        },
    );

    // Encrypted messages the SDK could not decrypt. Without this they vanish
    // and the room looks empty. Emits a placeholder row with
    // `undecryptable = true` and no ciphertext. `error_kind` is always
    // "no_key": this path exposes no finer reason in 0.18.
    //
    // Each one is remembered (bounded, per session) so that when the
    // event-cache redecryptor resolves it, the crypto observer re-emits it
    // ONCE as `event_decrypted` with a verdict computed on the decrypted
    // content (see `decrypted_event_json`).
    let encrypted_events = Arc::clone(&events);
    let encrypted_notify = Arc::clone(&sync_notify);
    client.add_event_handler(
        move |ev: OriginalSyncRoomEncryptedEvent,
              room: Room,
              push_actions: Vec<matrix_sdk::ruma::push::Action>,
              raw: matrix_sdk::event_handler::RawEvent| {
            let events = Arc::clone(&encrypted_events);
            let verdicts = sync_verdicts.clone();
            let notify = Arc::clone(&encrypted_notify);
            async move {
                // m.relates_to is outside the ciphertext: an edit that cannot be
                // decrypted yet is still not a new message, and a thread reply
                // still belongs to its thread (§8; the live producer reads the
                // same field in timeline.rs `cleartext_thread_root`).
                let (replaces_event_id, thread_root_id) = encrypted_relation_ids(
                    ev.content.relates_to.as_ref());
                trace_remote_edit("undecryptable", ev.event_id.as_str(), &replaces_event_id, true);
                let mut event = json!({
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
                    "replaces_event_id": replaces_event_id,
                    "thread_root_id": thread_root_id,
                    // Legacy field for older C++ builds.
                    "decrypted": false,
                });
                let backlog = mark_sync_backlog(
                    &notify,
                    &mut event,
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                );
                notify.remember_utd(room.room_id(), &ev.event_id, backlog);
                // Undecryptable, so the rules saw the encrypted event.
                push_verdict::PushVerdict::write(
                    verdicts.verdict(&room, &push_actions, &raw).await,
                    &mut event,
                );
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_event",
                        "room_id": room.room_id().to_string(),
                        "event": event,
                    }),
                );
            }
        },
    );
}

async fn run_authoritative_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    search_index: Arc<Mutex<Option<localsearch::SearchIndex>>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    room_list_slot: Arc<Mutex<Option<Arc<RoomListService>>>>,
    entries_slot: Arc<Mutex<Option<Arc<RoomListDynamicEntriesController>>>>,
    active_subscription: Arc<Mutex<warm_rooms::SubscriptionSet>>,
    timelines: Arc<timeline::TimelineRegistry>,
    call_media_capable: Arc<std::sync::atomic::AtomicBool>,
    verification_live: VerificationLive,
    sync_notify: Arc<syncnotify::SyncNotify>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) {
    // Call-signalling handlers live as long as this sync loop; their drop
    // guards unregister them on any exit, so none fires into a later account.
    let _call_guards = calls::register_handlers(
        &client, &events, &timelines, &call_media_capable);
    // MatrixRTC observation shares that lifetime; kept separate because it
    // tracks who is in a call, not who is inviting whom.
    let _rtc_guards = rtc::register_rtc_handlers(&client, &events, &timelines);
    // The local search index holds decrypted plaintext by exception (§6), on
    // condition that a redaction removes the row. Redactions reach C++ only as
    // a row flag, so this is handled here at the source; the sweep re-reads the
    // event cache, so the removal must hold on the Rust side.
    let _redaction_guard = localsearch::register_redaction_handler(
        &client, &search_index);

    set_sync_mode(&sync_mode, &events, SyncMode::Probing, None);

    // Probe the capability matrix-sdk's Sliding Sync v5 uses. A failed
    // /versions request is connectivity, not incompatibility: stay `Probing`
    // and report offline while retrying.
    //
    // Bounded (issue #2): matrix-sdk retries a 5xx/429 answer inside the
    // request itself, silently, for up to 15 minutes, so an unbounded probe
    // against a failing gateway kept the UI on "Loading rooms…" with nothing
    // reported. A timeout is treated like any other failed probe.
    let modern_supported = loop {
        let probe = tokio::select! {
            _ = &mut cancel => return,
            result = tokio::time::timeout(
                SYNC_PROBE_BUDGET, client.supported_versions()) => result,
        };
        match probe {
            Ok(Ok(versions)) => break versions.features.contains(&FeatureFlag::Msc4186),
            Ok(Err(_)) | Err(_) => {
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
            room_list_slot, entries_slot, active_subscription,
            Arc::clone(&sync_notify), cancel
        ).await {
            set_sync_mode(
                &sync_mode, &events, SyncMode::ClassicSyncFallback, Some("unsupported")
            );
            run_classic_sync(client, events, verification_live, sync_notify, cancel).await;
        }
    } else {
        set_sync_mode(
            &sync_mode, &events, SyncMode::ClassicSyncFallback, Some("unsupported")
        );
        run_classic_sync(client, events, verification_live, sync_notify, cancel).await;
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

/// The `room_list_error` category for an authentication error the modern lane
/// is retrying. Names the errcode so a log can tell the two apart; a literal,
/// never server text.
fn authentication_retry_category(error: &UnifiedSyncError) -> &'static str {
    match unified_error_kind(error) {
        Some(ErrorKind::Forbidden) => "authentication_retry_forbidden",
        _ => "authentication_retry_unknown_token",
    }
}

/// Consecutive authentication errors the modern lane rebuilds through before it
/// declares the session unauthorized. A Running edge resets the count.
///
/// Why any: a 401 from sliding sync does not prove the session is dead.
/// Verified in matrix-sdk 0.18 (`client/futures.rs`): on a 401 it refreshes and
/// retries ONCE, and a 401 on that retry is returned as-is, without
/// broadcasting `SessionChange::UnknownToken`. The LIKELY cause of such a retry
/// failing, NOT YET CAPTURED: MAS revokes the access token paired with every
/// refresh token it consumes (`refresh_token_grant`), so a second refresh in the
/// same burst of 401s on an expired token would revoke the token the first
/// one's retry carries. Parking on the first such error killed the room list for
/// the session while every other request kept working (matrix.org, 2026-10-07;
/// intermittent, a re-run synced fine). A session that really is dead is
/// reported by the SDK itself (`session_token_revoked`, which detaches it), and
/// a server that keeps refusing still ends in `Failed`, after the bound.
const MODERN_AUTH_RETRY_LIMIT: u32 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ModernAuthVerdict {
    Retry,
    Fatal,
}

/// The modern lane's count of consecutive authentication errors. One instance
/// lives for the whole lane, across supervisor rebuilds; `on_running` is the
/// reset, called on the room list's Running edge, so the bound is per spell of
/// refusals and not per session (an intermittent race recurs over a long
/// session and must not accumulate towards `Fatal`).
#[derive(Debug, Default)]
pub(crate) struct AuthRetryBudget {
    consecutive: u32,
}

impl AuthRetryBudget {
    /// Record one authentication error and say whether to rebuild or give up.
    pub(crate) fn on_auth_error(&mut self) -> ModernAuthVerdict {
        self.consecutive = self.consecutive.saturating_add(1);
        if self.consecutive > MODERN_AUTH_RETRY_LIMIT {
            ModernAuthVerdict::Fatal
        } else {
            ModernAuthVerdict::Retry
        }
    }

    /// The server answered this session: earlier refusals are behind us.
    pub(crate) fn on_running(&mut self) {
        self.consecutive = 0;
    }
}

/// What one failed classic `/sync` means for the loop that issued it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ClassicSyncFault {
    /// The session is gone. Stop and never retry a revoked token.
    Fatal,
    /// Everything else. Keep syncing.
    Transient,
}

/// Classify a classic-sync failure from the server's errcode. Pure over
/// `ErrorKind` so it is testable; the caller extracts it.
///
/// The default is `Transient`: a dropped connection has no errcode (`None`),
/// and matrix-sdk does not retry network failures without a `retry_limit`,
/// so treating it as fatal turned a brief Wi-Fi drop into the end of sync.
/// The fatal set mirrors the modern lane's `authentication_error`.
pub(crate) fn classify_classic_sync_error(kind: Option<&ErrorKind>) -> ClassicSyncFault {
    match kind {
        Some(ErrorKind::UnknownToken { .. }) | Some(ErrorKind::Forbidden) =>
            ClassicSyncFault::Fatal,
        _ => ClassicSyncFault::Transient,
    }
}

/// Backoff between classic-sync attempts by consecutive failures, capped at
/// about half a minute.
pub(crate) fn classic_sync_backoff(consecutive_failures: u32) -> std::time::Duration {
    const CEILING_SECS: u64 = 30;
    let secs = 1u64 << consecutive_failures.min(5);
    std::time::Duration::from_secs(secs.min(CEILING_SECS))
}

/// Consecutive failures before "offline" escalates to a visible error. One
/// report per outage; a success clears it.
const CLASSIC_SYNC_REPORT_AFTER: u32 = 5;

/// Run matrix-sdk-ui's unified sync supervisor (its `EncryptionSyncPermit`
/// keeps one encryption sliding sync). Returning `Some(cancel)` is the only
/// way to start classic sync, and happens only for a verified
/// unsupported-endpoint error.
async fn run_modern_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    room_list_slot: Arc<Mutex<Option<Arc<RoomListService>>>>,
    entries_slot: Arc<Mutex<Option<Arc<RoomListDynamicEntriesController>>>>,
    active_subscription: Arc<Mutex<warm_rooms::SubscriptionSet>>,
    sync_notify: Arc<syncnotify::SyncNotify>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
) -> Option<tokio::sync::oneshot::Receiver<()>> {
    // Everything from before this session is history to the sync handlers;
    // see syncnotify::CatchupClock for the edges below.
    sync_notify.catchup.begin(push_verdict::now_ms());
    // Withdraws the published RoomListService on every exit path; a handle
    // outliving the loop would accept subscriptions that go nowhere.
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

    // Same for the dynamic-entries controller, which `mx_rust_resync_rooms`
    // uses; a dropped stream would accept the call and emit nothing.
    struct EntriesPublication(
        Arc<Mutex<Option<Arc<RoomListDynamicEntriesController>>>>,
    );
    impl EntriesPublication {
        fn set(&self, controller: Option<Arc<RoomListDynamicEntriesController>>) {
            if let Ok(mut guard) = self.0.lock() {
                *guard = controller;
            }
        }
    }
    impl Drop for EntriesPublication {
        fn drop(&mut self) {
            self.set(None);
        }
    }
    let entries_publication = EntriesPublication(entries_slot);

    // One first-response watchdog across every supervisor rebuild, so a silent
    // sliding lane ("Loading rooms…" forever) is reported. It never cancels
    // anything: after the last escalation it parks, so it cannot end a
    // select! and stop a working sync.
    let first_response = Arc::new(AtomicBool::new(true));
    let watchdog_events = Arc::clone(&events);
    let watchdog_first = Arc::clone(&first_response);
    let watchdog = async move {
        watch_first_sync_response(&watchdog_events, &watchdog_first,
                                  FIRST_SYNC_STALL_STEPS).await;
        std::future::pending::<()>().await
    };
    tokio::pin!(watchdog);

    // Consecutive authentication errors; see MODERN_AUTH_RETRY_LIMIT.
    let mut auth_budget = AuthRetryBudget::default();
    // Rooms that have had their one blank-preview look, for the whole run.
    let mut preview_backfill = previewbackfill::Backfill::default();
    // Owned by this run and keyed by its account: a run detached after overrunning
    // its budget cannot clear or write into its successor's results.
    let backfill_results = client
        .user_id()
        .map(|user| previewbackfill::RunGuard::new(user.to_owned()));

    loop {
        let service = match switchtrace::timed_sync_service_build(
            SyncService::builder(client.clone()).build(),
        )
        .await
        {
            Ok(service) => service,
            Err(error) if unsupported_modern_error(&error) => return Some(cancel),
            Err(error) => {
                let mut category = "temporary";
                if authentication_error(&error) {
                    if auth_budget.on_auth_error() == ModernAuthVerdict::Fatal {
                        enqueue(&events, json!({
                            "type": "room_list_error", "category": "authentication"
                        }));
                        set_sync_mode(&sync_mode, &events, SyncMode::Failed, None);
                        let _ = (&mut cancel).await;
                        return None;
                    }
                    category = authentication_retry_category(&error);
                }
                enqueue(&events, json!({ "type": "room_list_error", "category": category }));
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
                // Not fatal and not parked: take the same bounded rebuild backoff as the
                // supervisor's own errors. Parking here left a live sync thread doing
                // nothing and a room list that never arrived.
                enqueue(&events, json!({ "type": "room_list_error", "category": "setup" }));
                enqueue(&events, json!({
                    "type": "room_list_sync_state", "state": "offline"
                }));
                tokio::select! {
                    _ = &mut cancel => return None,
                    _ = tokio::time::sleep(std::time::Duration::from_secs(3)) => {
                        enqueue(&events, json!({
                            "type": "room_list_sync_state", "state": "retrying"
                        }));
                        continue;
                    }
                }
            }
        };
        let (entries, controller) = room_list.entries_with_dynamic_adapters(10_000);
        controller.set_filter(Box::new(filters::new_filter_non_left()));
        tokio::pin!(entries);
        // Published only now: before `set_filter` there is no stream to reset.
        entries_publication.set(Some(Arc::new(controller)));
        // The ordered ids forwarded so far, mirroring the emitted diffs. Removals
        // and pops carry no room in `VectorDiff`, so this lets the producer name
        // the room instead of C++ deleting `order[index]` unchecked.
        let mut forwarded_order: Vec<OwnedRoomId> = Vec::new();

        let space_service = SpaceService::new(client.clone()).await;
        let mut unified_state = service.state();
        let mut list_state = room_list_service.state();
        // Whether the list is currently Running. `list_state` re-emits Running on
        // every response, so this turns it into an edge; offline/error clear it so
        // a reconnect is a new edge.
        let mut list_running = false;
        // The SDK publishes only real m.ignored_user_list changes, local or remote;
        // forward them directly.
        let mut ignore_list_sub = client.subscribe_to_ignore_user_list_changes();
        // Bounded latest-event registration state for this sync session.
        let latest_events = client.latest_events().await;
        let mut watched_latest: BTreeSet<OwnedRoomId> = BTreeSet::new();
        // Response-harvested recency (see harvest_room_activity). Owned by this
        // loop only.
        let mut room_updates_sub = client.subscribe_to_all_room_updates();
        let mut room_updates_live = true;
        let mut activity_stamps: HashMap<OwnedRoomId, u64> = HashMap::new();

        // Publish the service and apply the room that is already open: on a
        // restored session it opens before this point, and its subscription-only
        // state (m.room.pinned_events) would otherwise wait for the next switch.
        publication.set(Some(Arc::clone(&room_list_service)));
        let resubscribe = |service: Arc<RoomListService>| {
            // Forced: a new or expired session holds no subscription at all.
            let desired = active_subscription
                .lock()
                .ok()
                .and_then(|mut guard| guard.pending(true))
                .unwrap_or_default();
            async move {
                if !desired.is_empty() {
                    let refs: Vec<&matrix_sdk::ruma::RoomId> =
                        desired.iter().map(|r| r.as_ref()).collect();
                    service.subscribe_to_rooms(&refs).await;
                }
            }
        };
        resubscribe(Arc::clone(&room_list_service)).await;

        set_sync_mode(&sync_mode, &events, SyncMode::SlidingSync, None);
        enqueue(&events, json!({ "type": "room_list_sync_state", "state": "starting" }));
        service.start().await;
        let mut first_sync = true;
        // Dropped with this iteration, which aborts looks still in flight: a
        // rebuilt sync session never receives a previous one's result.
        let mut backfill_tasks: tokio::task::JoinSet<(OwnedRoomId, previewbackfill::Outcome)> =
            tokio::task::JoinSet::new();
        let mut last_backfill_scan: Option<std::time::Instant> = None;
        loop {
            tokio::select! {
                _ = &mut cancel => {
                    service.stop().await;
                    return None;
                }
                // Never resolves after escalating; polled so the silence is reported.
                _ = &mut watchdog => {}
                batch = entries.next() => {
                    let Some(batch) = batch else { break; };
                    forward_room_list_diffs(&events, batch, latest_events,
                                            &mut watched_latest,
                                            &mut forwarded_order).await;
                    enqueue_spaces(&events, &space_service, &client).await;
                    // A blank row (newest event filtered churn, nothing to
                    // preview) gets one bounded back-page, never before the
                    // first sync has settled and never more than a few rooms
                    // per pass.
                    let now_ms = preview_backfill.now_ms();
                    while let Some(done) = backfill_tasks.try_join_next() {
                        if let Ok((room_id, outcome)) = done {
                            let remembered = match &outcome {
                                previewbackfill::Outcome::Found(preview, ts) => {
                                    Some(previewbackfill::Remembered::Message(preview.clone(), *ts))
                                }
                                previewbackfill::Outcome::Undecryptable(ts, id) => {
                                    Some(previewbackfill::Remembered::Undecryptable(*ts, id.clone()))
                                }
                                _ => None,
                            };
                            if let (Some(what), Some(results)) = (remembered, &backfill_results) {
                                // C++ fills a blank row at once; room_payload serves
                                // the same result on every later payload. An
                                // undecryptable result carries a flag, never text.
                                let (preview, ts, utd) = match &what {
                                    previewbackfill::Remembered::Message(p, t) => (p.clone(), *t, false),
                                    previewbackfill::Remembered::Undecryptable(t, _) => (String::new(), *t, true),
                                };
                                results.remember(&room_id, what);
                                enqueue(&events, json!({
                                    "type": "room_preview_backfill",
                                    "room_id": room_id.to_string(),
                                    "preview": preview,
                                    "undecryptable": utd,
                                    "last_activity_ms": ts,
                                }));
                            }
                            preview_backfill.finish(&room_id, &outcome, now_ms);
                        }
                    }
                    if backfill_tasks.is_empty() {
                        preview_backfill.clear_in_flight();
                    }
                    // Scanned at most every few seconds: nothing here needs to
                    // walk every joined room on every batch.
                    if !first_sync
                        && last_backfill_scan
                            .map_or(true, |at| at.elapsed() >= std::time::Duration::from_secs(3))
                    {
                        last_backfill_scan = Some(std::time::Instant::now());
                        // Only a room with NO latest event qualifies: not one
                        // with a local echo or a remote event.
                        let candidates: Vec<OwnedRoomId> = client
                            .joined_rooms()
                            .into_iter()
                            .filter(|room| {
                                matches!(
                                    room.latest_event(),
                                    matrix_sdk_base::latest_event::LatestEventValue::None
                                ) && !matches!(
                                    previewbackfill::remembered(room.own_user_id(), room.room_id()),
                                    Some(previewbackfill::Remembered::Message(..))
                                )
                            })
                            .map(|room| room.room_id().to_owned())
                            .filter(|id| preview_backfill.is_eligible(id, now_ms))
                            .collect();
                        let picked = preview_backfill.pick(candidates, backfill_tasks.len(), now_ms);
                        for room_id in picked {
                            let Some(room) = client.get_room(&room_id) else {
                                preview_backfill.finish(&room_id, &previewbackfill::Outcome::Failed, now_ms);
                                continue;
                            };
                            backfill_tasks.spawn(async move {
                                let outcome = previewbackfill::look(&room).await;
                                (room_id, outcome)
                            });
                        }
                    }
                }
                updates = room_updates_sub.recv(), if room_updates_live => {
                    match updates {
                        Ok(updates) => {
                            let moved = harvest_room_activity(
                                &updates, &mut activity_stamps);
                            if !moved.is_empty() {
                                let rooms: Vec<serde_json::Value> = moved
                                    .into_iter()
                                    .map(|(room_id, ts, undecryptable, event_id)| {
                                        // Remembered so every later room_payload says
                                        // so too (merge_remembered), rather than the
                                        // older message the latest event still holds,
                                        // until `settle` finds it decrypted by its id.
                                        if let (true, Some(results)) = (undecryptable, &backfill_results) {
                                            results.remember(
                                                &room_id,
                                                previewbackfill::Remembered::Undecryptable(ts, event_id),
                                            );
                                        }
                                        json!({
                                            "id": room_id.to_string(),
                                            "last_activity_ms": ts,
                                            "last_message_undecryptable": undecryptable,
                                        })
                                    })
                                    .collect();
                                enqueue(&events, json!({
                                    "type": "room_activity", "rooms": rooms
                                }));
                            }
                        }
                        // A lagged receiver is a gap, not an end. matrix-sdk's own latest_events
                        // listener treats `Lagged` as closed and stops for good; that is the
                        // defect this backstop survives. Stamps are a high-water mark, so a
                        // dropped batch costs at most one response of recency.
                        Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {}
                        // Cannot happen while we hold the owning Client, but a closed receiver
                        // returns immediately and would spin this select at 100% CPU.
                        Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                            room_updates_live = false;
                        }
                    }
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
                    // Any state but Running (Recovering, Error, Terminated) ends a
                    // running spell: the SDK clears every room subscription when a
                    // session expires (M_UNKNOWN_POS), so the next Running must
                    // send the warm set again. The very first Running does not:
                    // the set went out before the service started.
                    if state.is_some() && !matches!(
                        state, Some(matrix_sdk_ui::room_list_service::State::Running))
                    {
                        list_running = false;
                        sync_notify.catchup.connection_lost();
                    }
                    if let Some(matrix_sdk_ui::room_list_service::State::Running) = state {
                        if !first_sync && warm_rooms::must_resubscribe(list_running, true) {
                            resubscribe(Arc::clone(&room_list_service)).await;
                        }
                        enqueue(&events, json!({
                            "type": "room_list_sync_state", "state": "running"
                        }));
                        // Gated on the edge: the room-list loop sets its state on every response
                        // and eyeball's `set()` always notifies, so this arm fires per response.
                        // `set_enabled(true)` walks every room and runs an SQLite query, too
                        // costly to repeat per response.
                        if !list_running {
                            list_running = true;
                            // From here an event is news only if it reached the server
                            // after this edge. Covers the first load too: Running is set
                            // after rooms 0..99, and the growing list then delivers
                            // every further room's (old) latest event.
                            sync_notify.catchup.running_edge(push_verdict::now_ms());
                            auth_budget.on_running();
                            client.send_queue().set_enabled(true).await;
                        }
                        if first_sync {
                            first_sync = false;
                            first_response.store(false, Ordering::SeqCst);
                            enqueue(&events, json!({ "type": "initial_sync_done" }));
                        }
                    }
                }
                state = unified_state.next() => {
                    match state {
                        // Transient loss keeps the mode; only the connection indicator reports
                        // offline. The supervisor reconnects itself.
                        Some(UnifiedSyncState::Offline) => {
                            list_running = false;
                            sync_notify.catchup.connection_lost();
                            enqueue(&events, json!({
                                "type": "room_list_sync_state", "state": "offline"
                            }));
                        }
                        Some(UnifiedSyncState::Error(error)) => {
                            list_running = false;
                            sync_notify.catchup.connection_lost();
                            service.stop().await;
                            // The only path to classic sync: a positively classified unsupported
                            // endpoint.
                            if unsupported_modern_error(&error) { return Some(cancel); }
                            // Authentication failure: never a downgrade, and fatal only
                            // past a bounded number of rebuilds (MODERN_AUTH_RETRY_LIMIT).
                            if authentication_error(&error) {
                                if auth_budget.on_auth_error() == ModernAuthVerdict::Fatal {
                                    set_sync_mode(&sync_mode, &events, SyncMode::Failed, None);
                                    enqueue(&events, json!({
                                        "type": "room_list_error", "category": "authentication"
                                    }));
                                    let _ = (&mut cancel).await;
                                    return None;
                                }
                                // Logged by C++ as a retrying error; not shown as one.
                                enqueue(&events, json!({
                                    "type": "room_list_error",
                                    "category": authentication_retry_category(&error)
                                }));
                            }
                            // Otherwise (or a retried auth error): keep the mode, report offline, rebuild.
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
        // This iteration's stream is gone, so withdraw its controller.
        entries_publication.set(None);
        // Bounded backoff before rebuilding; cancellation exits immediately.
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

/// Escalation points for a missing first classic-sync response. Generous:
/// the first request is full-state and heavy on large accounts.
const FIRST_SYNC_STALL_STEPS: &[std::time::Duration] = &[
    std::time::Duration::from_secs(60),
    std::time::Duration::from_secs(180),
    std::time::Duration::from_secs(600),
];

/// Report at each step that no first sync response has arrived. Returns as
/// soon as one lands. Separate so it can be tested with short steps.
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

/// How long the sync-lane `/versions` probe may take. One attempt is capped
/// by the client's 30 s request timeout; past that the SDK is retrying a
/// 5xx/429 internally, which it does silently for up to 15 minutes.
const SYNC_PROBE_BUDGET: std::time::Duration = std::time::Duration::from_secs(45);

/// How long a classic sync loop may go without delivering a single callback
/// (success or failure) before it is reported, and then restarted (issue #2).
///
/// One `/sync` attempt cannot legitimately be this quiet: the client's request
/// timeout is 30 s and the long-poll adds 30 s, and a failed attempt reaches
/// the callback. What is this quiet is matrix-sdk retrying a 5xx/429 answer
/// INSIDE the request (backoff 0.5-60 s, up to 15 minutes in total, invisible
/// to the callback, no socket open between tries), the same retry inside the
/// E2EE requests `sync_once` sends first, or a lock wait. Processing a large
/// first response is the one legitimate quiet phase, which is why the restart
/// bound is generous.
#[derive(Debug, Clone, Copy)]
struct ClassicSilenceBudget {
    /// Report the loop as offline and log `sync_stalled`.
    report_after: std::time::Duration,
    /// Drop the SDK loop (cancelling its retry sleep or lock wait), raise
    /// `sync_error`, and start a new one.
    restart_after: std::time::Duration,
    /// A gap between two supervisor ticks longer than this is a suspend and
    /// resume, not silence. The supervisor ticks at most a second apart, but on
    /// Windows `Instant` keeps counting through sleep (QueryPerformanceCounter),
    /// so a laptop that slept for an hour would otherwise wake to a loop that
    /// looks an hour silent and get a false `sync_error`. On a resume the
    /// silence clock restarts.
    resume_gap: std::time::Duration,
}

const CLASSIC_SYNC_SILENCE: ClassicSilenceBudget = ClassicSilenceBudget {
    report_after: std::time::Duration::from_secs(120),
    restart_after: std::time::Duration::from_secs(300),
    resume_gap: std::time::Duration::from_secs(15),
};

/// The `sync_error` text for a loop restarted for silence. Fixed text, never
/// server detail.
const CLASSIC_SYNC_WEDGED_MESSAGE: &str =
    "Sync stopped hearing from the server and was restarted.";

/// When the classic sync loop last showed any sign of life. Shared between
/// the SDK callback (which touches it) and the silence supervisor.
#[derive(Clone)]
struct SyncProgress {
    base: tokio::time::Instant,
    last_ms: Arc<std::sync::atomic::AtomicU64>,
}

impl SyncProgress {
    fn new() -> Self {
        Self {
            base: tokio::time::Instant::now(),
            last_ms: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        }
    }

    fn touch(&self) {
        let now = u64::try_from(self.base.elapsed().as_millis()).unwrap_or(u64::MAX);
        self.last_ms.store(now, Ordering::SeqCst);
    }

    fn quiet_for(&self) -> std::time::Duration {
        let last = std::time::Duration::from_millis(self.last_ms.load(Ordering::SeqCst));
        self.base.elapsed().saturating_sub(last)
    }
}

/// Resolves once the loop has been silent for `budget.restart_after`. On the
/// way it reports the silence once per quiet spell: `offline`, so the UI
/// leaves "Loading rooms…", and `sync_stalled` for the log. Never resolves
/// while callbacks keep arriving.
async fn supervise_classic_silence(
    events: &Arc<Mutex<VecDeque<String>>>,
    progress: &SyncProgress,
    budget: ClassicSilenceBudget,
) {
    let mut reported = false;
    let mut last_tick = tokio::time::Instant::now();
    loop {
        let now = tokio::time::Instant::now();
        if now.saturating_duration_since(last_tick) > budget.resume_gap {
            // Woken from a suspend (see `resume_gap`): the time asleep is not
            // the SDK's silence.
            progress.touch();
            reported = false;
        }
        last_tick = now;
        let quiet = progress.quiet_for();
        if quiet >= budget.restart_after {
            return;
        }
        if quiet >= budget.report_after {
            if !reported {
                reported = true;
                enqueue(events, json!({
                    "type": "room_list_sync_state", "state": "offline"
                }));
                enqueue(events, json!({
                    "type": "sync_stalled",
                    "phase": "no_progress",
                    "waited_secs": quiet.as_secs(),
                }));
            }
        } else {
            // A callback arrived: the next quiet spell is reported again.
            reported = false;
        }
        let next = if quiet < budget.report_after {
            budget.report_after - quiet
        } else {
            budget.restart_after - quiet
        };
        // Re-read at least every second so a callback that lands meanwhile
        // re-arms the report promptly.
        tokio::time::sleep(next.min(std::time::Duration::from_secs(1))).await;
    }
}

/// Classic /sync long-poll timeout: the SDK's own default.
const CLASSIC_SYNC_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
/// Classic /sync long-poll timeout while a verification flow is live.
///
/// Measured on a Beeper account (which takes the classic fallback): every
/// verification hop — request to ready, start to emojis, each confirmation —
/// took exactly the long-poll timeout. That server does not wake a pending
/// long-poll for a to-device event; it delivers it when the poll expires. A
/// short poll only while a flow is live turns 30 s hops into ~3 s ones and
/// costs nothing the rest of the time. Synapse wakes the poll anyway, so
/// there it changes nothing.
const CLASSIC_SYNC_TIMEOUT_VERIFYING: std::time::Duration = std::time::Duration::from_secs(3);

/// "Is a verification flow live right now?" Asked before every classic
/// /sync request. A closure rather than the three slots so the sync loop
/// needs no verification types, and so tests can drive it.
type VerificationLive = Arc<dyn Fn() -> bool + Send + Sync>;

/// The long-poll timeout for one classic /sync request.
///
/// The FIRST request of a loop says `timeout=0` explicitly. The SDK's idiom
/// for "do not long-poll the first sync" (`ignore_timeout_on_first_sync`)
/// OMITS the parameter and relies on the server reading absent as zero.
/// Synapse does; Beeper's server applies its default long-poll instead —
/// measured `request_ms=30095 process_ms=85` on a resumed session with
/// nothing new, against `request_ms=227` with an explicit zero. That one
/// request was the whole "shows nothing for 30 s after launch".
fn classic_sync_request_timeout(first: bool, verifying: bool) -> std::time::Duration {
    if first {
        std::time::Duration::ZERO
    } else if verifying {
        CLASSIC_SYNC_TIMEOUT_VERIFYING
    } else {
        CLASSIC_SYNC_TIMEOUT
    }
}

/// The settings for one classic /sync request. No `full_state`: it would ask
/// for every room's full state on every request and adds nothing to an
/// initial sync. The token is the SDK default, `ReusePrevious`, i.e. the
/// stored `next_batch` — exactly what the SDK's own stream sends, since a
/// processed response updates that token and a failed one leaves it.
fn classic_sync_settings(timeout: std::time::Duration) -> SyncSettings {
    SyncSettings::default().timeout(timeout)
}

/// GitHub #30/#48: the event cache must be listening BEFORE the first classic
/// /sync request, or that response never reaches it.
///
/// `EventCache::subscribe()` spawns the only task that stores sync responses
/// in the event cache (matrix-sdk 0.18 event_cache/mod.rs). Sliding sync gets
/// it from `RoomListService::new`; classic sync has no such service, so the
/// first subscriber used to be the first `TimelineBuilder::build`, i.e. the
/// first room the user OPENED. Every response before that (the catch-up after
/// a restart, above all) was processed, its token stored, and its events never
/// cached. A hole like that is permanent: a later response that is not
/// `limited` lands after the room's cached events with its `prev_batch`
/// dropped (event_cache/caches/room/state.rs `handle_sync`), so nothing ever
/// paginates into the gap. Messages that arrived while the client was closed
/// stayed missing from the room for good, while Element showed them.
///
/// Idempotent: the SDK initialises the listener once per client.
fn subscribe_event_cache_before_classic_sync(
    client: &Client,
    events: &Arc<Mutex<VecDeque<String>>>,
) {
    if let Err(err) = client.event_cache().subscribe() {
        // In practice only "the client is gone"; the loop fails on its own then.
        let kind = match err {
            matrix_sdk::event_cache::EventCacheError::ClientDropped => "client_dropped",
            _ => "other",
        };
        enqueue(events, json!({
            "type": "rust_trace",
            "line": format!("classic sync: event cache subscribe failed ({kind})"),
        }));
    }
}

async fn run_classic_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    verification_live: VerificationLive,
    sync_notify: Arc<syncnotify::SyncNotify>,
    cancel: tokio::sync::oneshot::Receiver<()>,
) {
    run_classic_sync_with(
        client, events, verification_live, sync_notify, cancel, CLASSIC_SYNC_SILENCE,
    )
    .await;
}

async fn run_classic_sync_with(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    verification_live: VerificationLive,
    sync_notify: Arc<syncnotify::SyncNotify>,
    mut cancel: tokio::sync::oneshot::Receiver<()>,
    silence: ClassicSilenceBudget,
) {
    enqueue(&events, json!({ "type": "room_list_sync_state", "state": "starting" }));
    subscribe_event_cache_before_classic_sync(&client, &events);
    // See syncnotify::CatchupClock. The SDK runs the event handlers inside
    // `sync_once`, BEFORE the callback below, so a catch-up response is judged
    // while the clock still says "catching up".
    sync_notify.catchup.begin(push_verdict::now_ms());

    let first_response = Arc::new(AtomicBool::new(true));
    // Any callback, success or failure, is a sign of life; see
    // ClassicSilenceBudget.
    let progress = SyncProgress::new();
    // Consecutive failures, shared with the callback: drives the backoff and
    // the escalation. Reset by any success.
    let failure_count = Arc::new(std::sync::atomic::AtomicU32::new(0));
    // Set by the callback on a dead session, so the loop parks instead of
    // restarting into the same refusal.
    let session_gone = Arc::new(AtomicBool::new(false));
    // Classic sync provides no recency: the SDK's recency_stamp comes only
    // from sliding-sync responses, and LatestEventValue only for rooms
    // registered with the Latest Events API. Two mechanisms fill the gap:
    //
    //  * Ordering is harvested from the responses: each carries updated rooms'
    //    new events, and an initial sync (no `since`) carries a recent window
    //    for every room. The newest origin_server_ts per room overrides the
    //    payload's last_activity_ms when newer.
    //  * Previews come from the Latest Events API under the shared cap. Rooms
    //    with unread activity claim it first, and when full a waking room
    //    evicts the stalest watched room, so the set does not freeze.
    let shared_watched: Arc<tokio::sync::Mutex<BTreeSet<OwnedRoomId>>> =
        Arc::new(tokio::sync::Mutex::new(BTreeSet::new()));
    let shared_stamps: Arc<tokio::sync::Mutex<HashMap<OwnedRoomId, u64>>> =
        Arc::new(tokio::sync::Mutex::new(HashMap::new()));

    // First-response watchdog: logs how long the FIRST response has been
    // outstanding, across every attempt. It never resolves and is pinned
    // outside the restart loop so it spans every attempt. It cancels nothing;
    // the per-attempt `supervise_classic_silence` is what bounds a wedged loop
    // (issue #2: silent for 13+ minutes with no connections).
    let watchdog_events = Arc::clone(&events);
    let watchdog_first = Arc::clone(&first_response);
    let watchdog = async move {
        watch_first_sync_response(&watchdog_events, &watchdog_first,
                                  FIRST_SYNC_STALL_STEPS).await;
        std::future::pending::<()>().await
    };
    tokio::pin!(watchdog);
    // Whether a silence restart has already raised `sync_error`; see below.
    let mut wedge_reported = false;

    loop {
        let callback_client = client.clone();
        let callback_events = Arc::clone(&events);
        let callback_first = Arc::clone(&first_response);
        let callback_watched = Arc::clone(&shared_watched);
        let callback_stamps = Arc::clone(&shared_stamps);
        let callback_failures = Arc::clone(&failure_count);
        let callback_gone = Arc::clone(&session_gone);
        let callback_progress = progress.clone();
        let callback_notify = Arc::clone(&sync_notify);
        // A new attempt starts its silence clock now, not at the last callback
        // of the attempt before it.
        progress.touch();
        // The callback sees failed requests as well as responses and returns
        // `Ok(LoopCtrl::Continue)` for a transient one, keeping the loop and its
        // token alive across an outage.
        //
        // Scoped in this block so the loop (and whatever request, retry sleep or
        // lock it holds) is dropped when the block ends, BEFORE the backoff
        // below, not at the end of the loop iteration.
        let wedged = {
        let callback = move |result: Result<matrix_sdk::sync::SyncResponse, matrix_sdk::Error>| {
            let client = callback_client.clone();
            let events = Arc::clone(&callback_events);
            let first_response = Arc::clone(&callback_first);
            let watched = Arc::clone(&callback_watched);
            let stamps = Arc::clone(&callback_stamps);
            let failures = Arc::clone(&callback_failures);
            let session_gone = Arc::clone(&callback_gone);
            let progress = callback_progress.clone();
            let notify = Arc::clone(&callback_notify);
            async move {
                // Touched on entry and again on the way out, so neither a slow
                // callback nor its own backoff sleep counts as SDK silence.
                progress.touch();
                // The edge time for the catch-up clock: when the response was in
                // hand, not after this callback's own (possibly long) work.
                let answered_ms = push_verdict::now_ms();
                let response = match result {
                    Ok(response) => response,
                    Err(error) => {
                        if classify_classic_sync_error(error.client_api_error_kind())
                            == ClassicSyncFault::Fatal
                        {
                            // Stop and report, never retry. (The modern lane now rebuilds
                            // through a bounded number first, MODERN_AUTH_RETRY_LIMIT;
                            // doing the same here is an open follow-up.)
                            session_gone.store(true, Ordering::SeqCst);
                            enqueue(&events, json!({
                                "type": "room_list_error", "category": "authentication"
                            }));
                            return Ok(LoopCtrl::Break);
                        }
                        let seen = failures.fetch_add(1, Ordering::SeqCst) + 1;
                        notify.catchup.connection_lost();
                        enqueue(&events, json!({
                            "type": "room_list_sync_state", "state": "offline"
                        }));
                        // Report once per outage; a success resets the counter and a later
                        // "running" clears the state in C++.
                        if seen == CLASSIC_SYNC_REPORT_AFTER {
                            enqueue(&events, json!({
                                "type": "sync_error",
                                "message": format_matrix_error(
                                    "Matrix Rust SDK sync failed", error),
                            }));
                        }
                        tokio::time::sleep(classic_sync_backoff(seen - 1)).await;
                        progress.touch();
                        return Ok(LoopCtrl::Continue);
                    }
                };
                // Recovery edge (previous attempt failed, this one succeeded): re-enable
                // the send queue, which matrix-sdk disables after any send error and never
                // re-enables. Gated because set_enabled(true) walks all rooms and queries
                // the store.
                let recovered = failures.swap(0, Ordering::SeqCst) > 0;
                if recovered {
                    client.send_queue().set_enabled(true).await;
                }
                // The first response and the first after an outage are the edges
                // (syncnotify::CatchupClock). Not every response: the floor would
                // then trail each callback's own processing time.
                if recovered || first_response.load(Ordering::SeqCst) {
                    notify.catchup.running_edge(answered_ms);
                }
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

                    // The preview cap is one pool across the account. Allocated by recency
                    // alone, one busy bridged network starves the others, so the watch set is
                    // a round-robin over per-network buckets, most recent first in each.
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

                    // Reconcile rather than accumulate, so the watch set follows the desired
                    // set and quiet buckets release their slots.
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

                    // Counts and timing only, for tuning the cap.
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
                progress.touch();
                Ok::<LoopCtrl, matrix_sdk::Error>(LoopCtrl::Continue)
            }
        };
        // The loop itself, rather than `sync_with_result_callback`, because the
        // long-poll timeout must be able to CHANGE between requests (see
        // CLASSIC_SYNC_TIMEOUT_VERIFYING) and the SDK's stream fixes it once.
        // Everything else is the SDK's `sync_with_result_callback` as 0.18 writes
        // it: `sync_once` per request with the stored token, every result handed
        // to the callback, an `Err` from the callback ends the loop, and a server
        // that ignores the timeout is held to one request per second.
        //
        // The silence budget is unaffected: a shorter poll only makes callbacks
        // more frequent, and the longest request is still the 30 s poll plus the
        // client's request timeout it was sized against.
        let loop_client = client.clone();
        let loop_verification_live = Arc::clone(&verification_live);
        let sync = async move {
            let mut first = true;
            let mut last_sync: Option<tokio::time::Instant> = None;
            loop {
                let timeout = classic_sync_request_timeout(first, loop_verification_live());
                first = false;
                let result = loop_client.sync_once(classic_sync_settings(timeout)).await;
                if callback(result).await? == LoopCtrl::Break {
                    break;
                }
                let now = tokio::time::Instant::now();
                if last_sync.is_some_and(|t| now - t <= std::time::Duration::from_secs(1)) {
                    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
                }
                last_sync = Some(now);
            }
            Ok::<(), matrix_sdk::Error>(())
        };
        tokio::pin!(sync);
        let mut wedged = false;
        // Biased, SDK loop first: a loop that finished in the same poll as the
        // supervisor's deadline (e.g. right after a resume from sleep) is a loop
        // that finished, not a wedge.
        tokio::select! {
            biased;
            result = &mut sync => if let Err(err) = result {
                // The callback absorbs transient failures; reaching here means the SDK
                // loop ended for another reason.
                enqueue(&events, json!({
                    "type": "sync_error",
                    "message": format_matrix_error("Matrix Rust SDK sync failed", err),
                }));
            },
            _ = &mut cancel => return,
            // Never resolves; see its construction above.
            _ = &mut watchdog => {},
            // Resolves only after `restart_after` without a single callback. The
            // SDK loop is dropped when this block ends, which cancels whatever it
            // was waiting on.
            //
            // KNOWN RESIDUAL RISK: the callback runs only after matrix-sdk has
            // PROCESSED a response, and 0.18 exposes no hook between the response
            // arriving and its processing. A first response so large that
            // processing it takes longer than `restart_after` would be cancelled
            // and fetched again, every time. 300 s is far beyond any processing
            // measured here, but it is a bound on processing too, not only on the
            // network.
            _ = supervise_classic_silence(&events, &progress, silence) => {
                wedged = true;
            },
        }
        wedged
        };

        // The SDK loop returned; restart it unless the session is gone (`startSync()`
        // runs only once, at login).
        if session_gone.load(Ordering::SeqCst) {
            // A revoked session: C++ has been told; wait for an explicit stop.
            let _ = (&mut cancel).await;
            return;
        }
        let seen = failure_count.fetch_add(1, Ordering::SeqCst) + 1;
        enqueue(&events, json!({
            "type": "room_list_sync_state", "state": "offline"
        }));
        // Surfaced, not just logged: C++ shows the sync error until a response
        // arrives ("running"). No "retrying" below for the same reason, since it
        // would put the UI back on "Loading rooms…" over a loop that has already
        // failed once. Once per outage: a success resets the failure count, so
        // `seen == 1` is the first failure since one.
        if wedged && (!wedge_reported || seen == 1) {
            wedge_reported = true;
            enqueue(&events, json!({
                "type": "sync_error",
                "message": CLASSIC_SYNC_WEDGED_MESSAGE,
            }));
        }
        tokio::select! {
            _ = &mut cancel => return,
            _ = tokio::time::sleep(classic_sync_backoff(seen - 1)) => {
                if !wedged {
                    enqueue(&events, json!({
                        "type": "room_list_sync_state", "state": "retrying"
                    }));
                }
            }
        }
    }
}

/// Forward one batch of room-list diffs, keeping `order` (this lane's
/// mirror of the index space) in step. `order` lets `Remove`/`PopFront`/
/// `PopBack`, which carry no room in `VectorDiff`, name the room they mean;
/// C++ rejects and requests a Reset if its own copy disagrees.
async fn forward_room_list_diffs(
    events: &Arc<Mutex<VecDeque<String>>>,
    batches: Vec<VectorDiff<RoomListItem>>,
    latest_events: &matrix_sdk::latest_events::LatestEvents,
    watched: &mut BTreeSet<OwnedRoomId>,
    order: &mut Vec<OwnedRoomId>,
) {
    // Serialize the room and register it with the Latest Events API so its
    // preview keeps updating without the room being opened.
    async fn payload(
        room: Room,
        latest_events: &matrix_sdk::latest_events::LatestEvents,
        watched: &mut BTreeSet<OwnedRoomId>,
    ) -> serde_json::Value {
        watch_latest_event(latest_events, watched, room.room_id()).await;
        room_payload(&room).await
    }

    // The id a positional diff refers to, or empty when the mirror lacks that
    // position; C++ then rejects rather than deleting on trust.
    fn id_at(order: &[OwnedRoomId], index: usize) -> String {
        order.get(index).map(|id| id.to_string()).unwrap_or_default()
    }

    for diff in batches {
        let value = match diff {
            VectorDiff::Reset { values } => {
                // Switch trace: Some only for the first Reset of a traced sync.
                let trace_started = switchtrace::first_reset_serialize_start();
                let mut rooms = Vec::with_capacity(values.len());
                order.clear();
                for item in values {
                    let room = item.into_inner();
                    order.push(room.room_id().to_owned());
                    rooms.push(payload(room, latest_events, watched).await);
                }
                let room_count = rooms.len();
                let mut value = json!({ "type": "room_list_reset", "rooms": rooms });
                let trace = switchtrace::take_first_reset_trace(trace_started, room_count);
                switchtrace::attach_trace(&mut value, trace.as_ref());
                value
            }
            VectorDiff::Append { values } => {
                let mut rooms = Vec::with_capacity(values.len());
                for item in values {
                    let room = item.into_inner();
                    order.push(room.room_id().to_owned());
                    rooms.push(payload(room, latest_events, watched).await);
                }
                json!({ "type": "room_list_append", "rooms": rooms })
            }
            VectorDiff::PushFront { value } => {
                let room = value.into_inner();
                order.insert(0, room.room_id().to_owned());
                json!({
                    "type": "room_list_push_front",
                    "room": payload(room, latest_events, watched).await
                })
            }
            VectorDiff::PushBack { value } => {
                let room = value.into_inner();
                order.push(room.room_id().to_owned());
                json!({
                    "type": "room_list_push_back",
                    "room": payload(room, latest_events, watched).await
                })
            }
            VectorDiff::PopFront => {
                let expected = id_at(order, 0);
                if !order.is_empty() {
                    order.remove(0);
                }
                json!({ "type": "room_list_pop_front", "expected_id": expected })
            }
            VectorDiff::PopBack => {
                let expected = order.last().map(|id| id.to_string()).unwrap_or_default();
                order.pop();
                json!({ "type": "room_list_pop_back", "expected_id": expected })
            }
            VectorDiff::Insert { index, value } => {
                let room = value.into_inner();
                if index <= order.len() {
                    order.insert(index, room.room_id().to_owned());
                }
                json!({
                    "type": "room_list_insert", "index": index,
                    "room": payload(room, latest_events, watched).await
                })
            }
            VectorDiff::Set { index, value } => {
                let room = value.into_inner();
                if let Some(slot) = order.get_mut(index) {
                    *slot = room.room_id().to_owned();
                }
                json!({
                    "type": "room_list_set", "index": index,
                    "room": payload(room, latest_events, watched).await
                })
            }
            VectorDiff::Remove { index } => {
                let expected = id_at(order, index);
                if index < order.len() {
                    order.remove(index);
                }
                json!({
                    "type": "room_list_remove", "index": index,
                    "expected_id": expected
                })
            }
            VectorDiff::Truncate { length } => {
                if length <= order.len() {
                    order.truncate(length);
                }
                json!({ "type": "room_list_truncate", "length": length })
            }
            VectorDiff::Clear => {
                order.clear();
                json!({ "type": "room_list_clear" })
            }
        };
        enqueue(events, value);
    }
}

/// One Space's direct children, in the order its `m.space.child` state
/// declares. (The payload's `descendants` is transitive.)
///
/// Spec order: `order` keys lexicographically, keyless children last, then
/// the `m.space.child` event's `origin_server_ts`, then room id (the spec's
/// and Element's order). An empty `via` is MSC1772 removal and is skipped.
/// Reads the local state store only; unsynced Spaces return nothing and the
/// caller falls back to the SDK graph.
async fn direct_children_of(room: &Room) -> Vec<String> {
    let Ok(events) = room
        .get_state_events_static::<SpaceChildEventContent>()
        .await
    else {
        return Vec::new();
    };
    let mut entries: Vec<(Option<String>, u64, String)> = Vec::new();
    for raw in events {
        let Ok(event) = raw.deserialize() else { continue };
        let (state_key, content, ts) = match event {
            SyncOrStrippedState::Sync(SyncStateEvent::Original(original)) => {
                (original.state_key, original.content, u64::from(original.origin_server_ts.get()))
            }
            _ => continue,
        };
        if content.via.is_empty() {
            continue;
        }
        entries.push((
            content.order.map(|order| order.as_str().to_owned()),
            ts,
            state_key.to_string(),
        ));
    }
    sort_space_children(entries)
}

/// The ordering `direct_children_of` applies, apart so it can be tested:
/// (order key, `origin_server_ts`, room id). Without the timestamp, keyless
/// children fell back to ROOM-ID order, so rooms added Alpha, Beta, Delta
/// listed as Beta, Alpha, Delta.
fn sort_space_children(mut entries: Vec<(Option<String>, u64, String)>) -> Vec<String> {
    entries.sort_by(|a, b| {
        let by_order = match (&a.0, &b.0) {
            (Some(left), Some(right)) => left.cmp(right),
            (Some(_), None) => std::cmp::Ordering::Less,
            (None, Some(_)) => std::cmp::Ordering::Greater,
            (None, None) => std::cmp::Ordering::Equal,
        };
        by_order.then(a.1.cmp(&b.1)).then_with(|| a.2.cmp(&b.2))
    });
    entries.into_iter().map(|(_, _, id)| id).collect()
}

/// Children the Space's own state unlinked: an `m.space.child` with an
/// empty or missing `via` (matrix-sdk-ui's remove_child sends `{}`), or a
/// redacted one. matrix-sdk-ui's graph still links such children, via the
/// child's `m.space.parent` or outright for `via: []`.
async fn unlinked_children_of(room: &Room) -> BTreeSet<String> {
    use matrix_sdk::deserialized_responses::RawSyncOrStrippedState;
    let Ok(events) = room
        .get_state_events_static::<SpaceChildEventContent>()
        .await
    else {
        return BTreeSet::new();
    };
    let mut out = BTreeSet::new();
    for raw in events {
        let RawSyncOrStrippedState::Sync(raw) = raw else { continue };
        let unlinked_key = match raw.deserialize() {
            Ok(SyncStateEvent::Original(original)) => {
                original.content.via.is_empty().then(|| original.state_key.to_string())
            }
            Ok(SyncStateEvent::Redacted(redacted)) => Some(redacted.state_key.to_string()),
            // Unparsable for another reason (bad `suggested`, a bad server name) is not
            // a removal; only a missing, non-array or empty `via` is.
            Err(_) => {
                let via = raw
                    .get_field::<serde_json::Value>("content")
                    .ok()
                    .flatten()
                    .and_then(|content| content.get("via").cloned());
                let linked = via
                    .as_ref()
                    .and_then(serde_json::Value::as_array)
                    .is_some_and(|servers| !servers.is_empty());
                if linked {
                    None
                } else {
                    raw.get_field::<String>("state_key").ok().flatten()
                }
            }
        };
        if let Some(key) = unlinked_key {
            out.insert(key);
        }
    }
    out
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
    // Strictly direct children, kept apart from the map below, which is widened
    // with `filter.descendants` (recursive for a level-1 filter).
    let mut direct_by_parent = HashMap::<String, BTreeSet<String>>::new();
    let mut unlinked = HashMap::<String, BTreeSet<String>>::new();
    for space in &joined_spaces {
        let gone = unlinked_children_of(space).await;
        if !gone.is_empty() {
            unlinked.insert(space.room_id().to_string(), gone);
        }
    }

    // Ask SpaceService's cycle-pruned graph for every known joined room's
    // parents. This extends its two presentation-level filters into a full
    // selectable hierarchy without reparsing raw state in Lightning.
    for room in client.joined_rooms() {
        let child_id = room.room_id().to_string();
        let parents: Vec<String> = service.joined_parents_of_child(room.room_id()).await
            .into_iter().map(|parent| parent.room_id.to_string())
            .filter(|parent| {
                !unlinked.get(parent).is_some_and(|gone| gone.contains(&child_id))
            })
            .collect();
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
        let gone = unlinked.get(&parent);
        children_by_parent.entry(parent.clone()).or_default()
            .extend(filter.descendants.iter().map(ToString::to_string)
                .filter(|id| !gone.is_some_and(|gone| gone.contains(id))));
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
        // Direct children in admin order, then any link the SDK parent graph knows
        // that the state read lacks (the m.space.child may not have synced yet).
        // Uses `direct_by_parent`, not the recursively widened map.
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

/// The room-list ordering stamp in ms, from the SDK's LatestEvent (the
/// preview event). Ordering by any event would make rooms jump on member
/// joins or topic edits. `Local*` variants count, so a room rises as soon
/// as the user sends.
///
/// There is no fallback here: `Room::latest_event_timestamp()` reads the
/// same value. 0 means unknown and crosses as an invalid QDateTime that
/// `RoomInfo::raiseActivity` ignores. The real backstop is
/// `harvest_room_activity`.
///
/// An edit never moves a room. When the latest message was edited, the SDK's
/// latest event is the EDIT (latest_events builder), stamped with when it was
/// edited; the stamp is then `edited_ts`, the edited event's own, or 0 (the
/// room stays where it is) when that is not known. Pure for tests.
fn room_ordering_timestamp_ms(
    latest: &matrix_sdk_base::latest_event::LatestEventValue,
    edited_ts: Option<u64>,
) -> u64 {
    use matrix_sdk_base::latest_event::LatestEventValue;
    if let LatestEventValue::Remote(event) = latest {
        if replaced_event_id(event.raw()).is_some() {
            return edited_ts.unwrap_or(0);
        }
    }
    latest
        .timestamp()
        .map(|ts| u64::from(ts.get()))
        .unwrap_or(0)
}

/// The event an `m.replace` edit replaces, or `None` when `raw` is not an
/// edit. `m.relates_to` is outside the ciphertext, so this reads encrypted
/// events too.
fn replaced_event_id(
    raw: &matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnySyncTimelineEvent>,
) -> Option<OwnedEventId> {
    let content = raw
        .get_field::<serde_json::Value>("content")
        .ok()
        .flatten()?;
    let relation = content.get("m.relates_to")?;
    if relation.get("rel_type")?.as_str()? != "m.replace" {
        return None;
    }
    EventId::parse(relation.get("event_id")?.as_str()?).ok()
}

/// When the edited event was sent, from the event cache (memory, then the
/// store; never a request). The SDK found it there to accept the edit.
async fn edited_event_timestamp_ms(room: &Room, event_id: &EventId) -> Option<u64> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    let ts = u64::from(event.raw().get_field::<UInt>("origin_server_ts").ok()??);
    if ts == 0 { None } else { Some(ts) }
}

/// Event types whose arrival means somebody said something. An allow-list,
/// so new event types do not reorder rooms by default.
///
/// Mirrors matrix-sdk's `filter_any_message_like_event_content`, except:
///  * `m.room.encrypted` is included: unsuitable for a preview, but it is
///    still a message for ordering purposes.
///  * `m.call.invite` / `m.rtc.notification` are excluded, because C++
///    already refuses to let call rows raise activity
///    (`TimelineEvent::CallEvent` in `handleTimelineEvent`).
pub(crate) const CONVERSATION_EVENT_TYPES: &[&str] = &[
    "m.room.message",
    "m.room.encrypted",
    "m.sticker",
    "m.poll.start",
    "org.matrix.msc3381.poll.start",
];

/// When a conversation happened, for one raw sync timeline event, or `None`
/// when it is not something somebody said. Pure for testing.
pub(crate) fn conversation_timestamp_ms(
    raw: &matrix_sdk::ruma::serde::Raw<
        matrix_sdk::ruma::events::AnySyncTimelineEvent,
    >,
) -> Option<u64> {
    // A state event is never a conversation, whatever its type; checked first,
    // by the presence of the field.
    if raw
        .get_field::<serde_json::Value>("state_key")
        .ok()
        .flatten()
        .is_some()
    {
        return None;
    }
    let kind = raw.get_field::<String>("type").ok().flatten()?;
    if !CONVERSATION_EVENT_TYPES.contains(&kind.as_str()) {
        return None;
    }
    // An edit is not something new said; it must not move the room.
    if replaced_event_id(raw).is_some() {
        return None;
    }
    // Nor is an undecrypted reaction or reference (a poll vote): decrypted
    // they are `m.reaction` / a response type, which never count, and an
    // `m.room.encrypted` wrapper must not make them a "last message".
    // `m.relates_to` is outside the ciphertext.
    if kind == "m.room.encrypted" {
        let rel_type = raw
            .get_field::<serde_json::Value>("content")
            .ok()
            .flatten()
            .and_then(|content| {
                content.get("m.relates_to")?.get("rel_type")?.as_str().map(str::to_owned)
            });
        if matches!(rel_type.as_deref(), Some("m.annotation" | "m.reference")) {
            return None;
        }
    }
    let ts = raw.get_field::<UInt>("origin_server_ts").ok().flatten()?;
    let ts = u64::from(ts);
    if ts == 0 { None } else { Some(ts) }
}

/// A conversation event's ordering stamp on the clock the room list orders by:
/// `None` when `conversation_timestamp_ms` refuses it, otherwise the event's
/// own `TimelineEvent::timestamp`, which the SDK clamps to the local clock when
/// it builds the event (`extract_timestamp`) and which the latest event, the
/// event cache and redecryption all keep. The raw `origin_server_ts` is used
/// only when the event carries none, and never above it. Reading the raw field
/// instead put a sender whose clock runs ahead on a different clock from the
/// latest event (break-it B084 review).
pub(crate) fn event_ordering_stamp_ms(
    event: &matrix_sdk::deserialized_responses::TimelineEvent,
) -> Option<u64> {
    let raw_ts = conversation_timestamp_ms(event.raw())?;
    let own = event.timestamp_raw().map(|ts| u64::from(ts.get())).filter(|ts| *ts != 0);
    Some(own.map_or(raw_ts, |ts| ts.min(raw_ts)))
}

/// Response-harvested recency for the sliding lane.
///
/// `Room::latest_event()` can stop moving while messages arrive: its
/// listener ends on one lagged receive, it skips recomputation while a
/// local echo is unsent, and it ignores undecrypted events. The payload then
/// resends a stale stamp until the room is opened. This reads the events in
/// the response instead. `stamps` is a per-room high-water mark; only rooms
/// whose mark moved are returned. Timestamps (`event_ordering_stamp_ms`, the
/// latest event's clock), whether that newest event is still undecryptable
/// (`m.room.encrypted` after the SDK's own attempt), and its id: the latest
/// event skips such an event, so the room's preview would otherwise stay on an
/// OLDER message while this stamp moves its time to the new one.
fn harvest_room_activity(
    updates: &matrix_sdk::sync::RoomUpdates,
    stamps: &mut HashMap<OwnedRoomId, u64>,
) -> Vec<(OwnedRoomId, u64, bool, Option<OwnedEventId>)> {
    let mut moved = Vec::new();
    for (room_id, update) in &updates.joined {
        let Some((newest, undecryptable, event_id)) = newest_conversation(&update.timeline.events)
        else {
            continue;
        };
        let entry = stamps.entry(room_id.clone()).or_default();
        if newest > *entry {
            *entry = newest;
            moved.push((room_id.clone(), newest, undecryptable, event_id));
        }
    }
    moved
}

/// The newest conversation stamp among `events` (oldest first, as a sync
/// timeline is), whether that event is undecryptable, and its id. On a tie the
/// later event wins. Pure for tests.
fn newest_conversation<'a>(
    events: impl IntoIterator<Item = &'a matrix_sdk::deserialized_responses::TimelineEvent>,
) -> Option<(u64, bool, Option<OwnedEventId>)> {
    let mut newest: Option<(u64, &matrix_sdk::deserialized_responses::TimelineEvent)> = None;
    for event in events {
        let Some(ts) = event_ordering_stamp_ms(event) else { continue };
        if newest.map_or(true, |(best, _)| ts >= best) {
            newest = Some((ts, event));
        }
    }
    newest.map(|(ts, event)| {
        let encrypted = event.raw().get_field::<String>("type").ok().flatten().as_deref()
            == Some("m.room.encrypted");
        (ts, encrypted, event.event_id())
    })
}

/// What the event cache says now of an event remembered as undecryptable
/// (memory, then the store; never a request), for `previewbackfill::settle`.
async fn undecryptable_now(room: &Room, event_id: &EventId) -> previewbackfill::UndecryptableNow {
    use previewbackfill::UndecryptableNow;
    let Ok((cache, _drop_handles)) = room.event_cache().await else {
        return UndecryptableNow::Unknown;
    };
    match cache.find_event(event_id).await {
        Ok(Some(event)) if event.kind.is_utd() => UndecryptableNow::StillUndecryptable,
        Ok(Some(event)) => UndecryptableNow::Decrypted(raw_event_preview_text(event.raw())),
        _ => UndecryptableNow::Unknown,
    }
}

/// What a DM peer's member event says about the face of that conversation,
/// as the `direct_peer_avatar` event: only a join by one of the room's
/// m.direct targets (a leave or a ban carries no profile), never anyone
/// else's. The avatar is empty when the event names none. Pure and
/// unit-tested.
fn direct_peer_avatar_payload(
    room_id: &str,
    event: &SyncRoomMemberEvent,
    direct_targets: &[String],
) -> Option<serde_json::Value> {
    use matrix_sdk::ruma::events::room::member::MembershipState;
    let original = event.as_original()?;
    if original.content.membership != MembershipState::Join {
        return None;
    }
    let user = original.state_key.as_str();
    if !direct_targets.iter().any(|target| target == user) {
        return None;
    }
    Some(json!({
        "type": "direct_peer_avatar",
        "room_id": room_id,
        "user_id": user,
        "avatar_url": original
            .content
            .avatar_url
            .as_ref()
            .map(ToString::to_string)
            .unwrap_or_default(),
    }))
}

/// Combine the client's own event-cache counts (`num_unread_notifications`,
/// `num_unread_mentions`) with the ones the server sent this sync
/// (`UnreadNotificationsCount`).
///
/// For an UNENCRYPTED room the server evaluates every push rule, including
/// content and `m.mentions` conditions, so its count is at least as
/// trustworthy as our own and the higher of the two only protects against
/// either side under-counting while the other catches up.
///
/// For a definitely ENCRYPTED room that trade is backwards. matrix-sdk-base's
/// own doc comment on `Room::unread_notification_counts()` says outright:
/// "these might be incorrect for encrypted rooms, since the server doesn't
/// know which events are relevant standalone messages or not, nor can it
/// inspect mentions... consider using the client-side computed counts". A
/// room set to "Mentions & keywords only" writes a `room`-kind push rule with
/// no actions; the client's own counters apply that SAME ruleset to the
/// DECRYPTED body, so an ordinary message correctly does not raise them. The
/// server-reported count has no such guarantee for ciphertext it cannot
/// evaluate content conditions against, and `.max()` with it is exactly what
/// let a plain, non-mention message in a Mentions-only encrypted room paint a
/// numbered badge instead of the dot the room list means to show. Trust the
/// client-computed counts alone once encryption is definitely known.
fn unread_and_highlight_counts(
    is_encrypted: bool,
    client_notifications: u64,
    client_mentions: u64,
    server_notification_count: u64,
    server_highlight_count: u64,
) -> (u64, u64) {
    if is_encrypted {
        (client_notifications, client_mentions)
    } else {
        (
            client_notifications.max(server_notification_count),
            client_mentions.max(server_highlight_count),
        )
    }
}

/// Whether the room's remaining unread comes from THREADS only, and whether
/// that includes a mention.
///
/// With `ThreadingSupport::Enabled` the SDK's own event-cache counts
/// (`num_unread_notifications` / `num_unread_mentions`) deliberately SKIP
/// events that belong to a thread (`read_receipts.rs`, `extract_thread_root`),
/// while the counts the server sends in `unread_notifications` are not split by
/// thread unless the client asks for MSC3773 `unread_thread_notifications`,
/// which Lightning does not. So for an UNENCRYPTED room, a server count above a
/// client count that has reached zero is unread the main timeline cannot
/// contain: it cleared the receipt, and a thread still holds the badge.
/// Conservative on purpose: both client counts must be zero (a room with
/// plain main-timeline unread is just unread), and an encrypted room reports
/// nothing, because its server counts are not used at all (see
/// `unread_and_highlight_counts`) and the client counts exclude threads, so
/// there is no honest signal. Never an exact per-thread count.
///
/// `counted_from_receipt`: the client counts are evidence only when they were
/// counted FROM our own read receipt (`RoomReadReceipts::latest_active`). A
/// room joined this session and never read has no receipt, and its counts are
/// zero because nothing was counted, not because the main timeline was read:
/// classifying that as thread-only drew the thread glyph, with no number, over
/// plain main-timeline unread. Without a receipt, report ordinary unread.
fn unread_in_threads_only(
    is_encrypted: bool,
    counted_from_receipt: bool,
    client_notifications: u64,
    client_mentions: u64,
    server_notification_count: u64,
    server_highlight_count: u64,
) -> (bool, bool) {
    if is_encrypted
        || !counted_from_receipt
        || client_notifications != 0
        || client_mentions != 0
        || (server_notification_count == 0 && server_highlight_count == 0)
    {
        return (false, false);
    }
    (true, server_highlight_count > 0)
}

/// A Matrix id as the C++ logs print it (`matrix::e2ee::redactId`): the sigil
/// kept, then the first 8 hex characters of SHA-256 of the rest. Stable for
/// correlation, not reversible.
pub(crate) fn redact_matrix_id(id: &str) -> String {
    use sha2::{Digest, Sha256};
    if id.is_empty() {
        return "<none>".to_owned();
    }
    let (sigil, rest) = match id.chars().next() {
        Some(c @ ('!' | '$' | '@' | '#')) => (c.to_string(), &id[1..]),
        _ => (String::new(), id),
    };
    let digest = Sha256::digest(rest.as_bytes());
    let hex: String = digest.iter().take(4).map(|b| format!("{b:02x}")).collect();
    format!("{sigil}{hex}")
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
    // `encryption_state()` is tri-state; only a definite "encrypted" answer
    // takes the client-only path. Unknown falls back to the unencrypted
    // (max-of-both) behaviour, the existing, safer default for a room whose
    // state has not settled yet.
    let (unread_count, highlight_count) = unread_and_highlight_counts(
        room.encryption_state().is_encrypted(),
        room.num_unread_notifications(),
        room.num_unread_mentions(),
        notifications.notification_count,
        notifications.highlight_count,
    );
    // Opt-in (LIGHTNING_UNREAD_TRACE=1): which counts produced this room's
    // number and by which path. Counts only; the room is a short hash so lines
    // correlate without naming it. See the 2026-10-15 rooms notes for the steps.
    if std::env::var_os("LIGHTNING_UNREAD_TRACE").is_some() {
        let state = room.encryption_state();
        eprintln!(
            "unread-trace room={} encryption={} path={} client_notif={} client_mentions={} server_notif={} server_highlight={} -> unread={} highlight={}",
            redact_matrix_id(room.room_id().as_str()),
            if state.is_encrypted() { "encrypted" } else if state.is_unknown() { "unknown" } else { "plain" },
            if state.is_encrypted() { "client-only" } else { "max-of-both" },
            room.num_unread_notifications(),
            room.num_unread_mentions(),
            notifications.notification_count,
            notifications.highlight_count,
            unread_count,
            highlight_count,
        );
    }
    let (unread_threads_only, mention_in_threads) = unread_in_threads_only(
        room.encryption_state().is_encrypted(),
        room.read_receipts().latest_active.is_some(),
        room.num_unread_notifications(),
        room.num_unread_mentions(),
        notifications.notification_count,
        notifications.highlight_count,
    );
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
    // same event.
    let latest_event = room.latest_event();
    // An edit must not move the room: order by the edited event's own time.
    let edited_ts = match &latest_event {
        matrix_sdk_base::latest_event::LatestEventValue::Remote(event) => {
            match replaced_event_id(event.raw()) {
                Some(original) => edited_event_timestamp_ms(room, &original).await,
                None => None,
            }
        }
        _ => None,
    };
    // A room whose own latest event gives no preview (newest event filtered
    // churn) shows what the blank-preview look found, if it has run. A
    // remembered undecryptable event is first checked by its ID against the
    // latest event and the event cache (`settle`): once it has decrypted, it
    // must not keep the placeholder up, whatever the stamps say.
    let remembered = previewbackfill::remembered(room.own_user_id(), room.room_id());
    let latest_event_id = latest_event.event_id();
    let utd_now = match &remembered {
        Some(previewbackfill::Remembered::Undecryptable(_, Some(id)))
            if latest_event_id.as_deref() != Some(&**id) =>
        {
            undecryptable_now(room, id).await
        }
        _ => previewbackfill::UndecryptableNow::Unknown,
    };
    let settled = previewbackfill::settle(remembered.clone(), latest_event_id.as_deref(), utd_now);
    if let Some(was) = remembered.as_ref().filter(|was| settled.as_ref() != Some(*was)) {
        previewbackfill::replace_remembered(room.own_user_id(), room.room_id(), was, settled.clone());
    }
    let (own_preview, own_activity, own_undecryptable) = previewbackfill::merge_remembered(
        latest_event_preview_text(&latest_event),
        room_ordering_timestamp_ms(&latest_event, edited_ts),
        settled,
    );
    // Our own join as the latest event (a room just joined): a flag, worded by
    // C++ (see `previewbackfill::is_own_join`).
    let own_join_latest = own_preview.is_empty()
        && matches!(&latest_event,
            matrix_sdk_base::latest_event::LatestEventValue::Remote(event)
                if previewbackfill::is_own_join(event.raw(), room.own_user_id()));
    // An invitation has no m.direct yet; see `rooms::payload_is_direct`.
    let is_direct = rooms::payload_is_direct(room, !direct_targets.is_empty()).await;

    json!({
        "id": room.room_id().to_string(),
        "membership": membership,
        "name": room_name(room).await,
        "canonical_alias": room.canonical_alias().map(|alias| alias.to_string()).unwrap_or_default(),
        "topic": room.topic().unwrap_or_default(),
        "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
        "last_message_preview": own_preview,
        // True when the last message could not be decrypted: C++ renders the
        // placeholder (no text crosses, so no real body can imitate it).
        "last_message_undecryptable": own_undecryptable,
        // True when the latest event is our own join and there is no message
        // yet: C++ shows its own words, so a newly accepted DM is not blank.
        "last_message_own_join": own_join_latest,
        "last_activity_ms": own_activity,
        "unread_count": unread_count,
        "highlight_count": highlight_count,
        "marked_unread": room.is_marked_unread(),
        // Unread that only threads can explain; see `unread_in_threads_only`.
        "unread_threads_only": unread_threads_only,
        "mention_in_threads": mention_in_threads,
        // The `m.favourite` tag itself, via the SDK's notable-tag bit, so favourites
        // set in other clients show here.
        "is_favourite": room.is_favourite(),
        // m.lowpriority, set on any client. Forwarded only; ordering is C++'s.
        "is_low_priority": room.is_low_priority(),
        "has_unread_messages": room.num_unread_messages() > 0,
        "encrypted": room.encryption_state().is_encrypted(),
        // EncryptionState is tri-state; Unknown must not read as unencrypted (draft
        // persistence and server search fail closed on it).
        "encryption_known": !room.encryption_state().is_unknown(),
        "is_space": room.is_space(),
        "is_direct": is_direct,
        "direct_user_id": direct_targets.first().cloned().unwrap_or_default(),
        "direct_user_ids": direct_targets,
        "member_count": room.joined_members_count(),
        // Joined plus invited: matrix-sdk's DmRoomDefinition::TwoMembers
        // counts these ("at most two active members"), so a DM someone else
        // was invited into is not 1:1 for ringing (src/calls/OneToOneDm.h).
        "active_member_count": room.active_members_count(),
        // The room's join rule, "" while unknown: with the joined count it lets
        // the room list's Leave say whether the room would be left empty and
        // closed (LeaveRoomConfirmDialog).
        "join_rule": rooms::join_rule_str(room.join_rule().as_ref()),
        "room_type": room.room_type().map(|kind| kind.to_string()).unwrap_or_default(),
        "prev_batch": room.last_prev_batch().unwrap_or_default(),
        "inviter_user_id": inviter_user_id,
        "inviter_display_name": inviter_display_name,
        // Successor and predecessor from the SDK's typed accessors, which parse to
        // an OwnedRoomId. Empty means none. A non-empty successor is the tombstone
        // flag.
        //
        // The tombstone's free-text `body`/`reason` is deliberately not forwarded:
        // it would appear on a banner users are invited to click, so Lightning shows
        // fixed wording instead.
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

/// A snapshot of the state store, not an index base.
///
/// `client.rooms()` differs from the dynamic adapter's paged, filtered and
/// sorted vector in length, membership and order, so it is emitted as
/// `room_snapshot`, which C++ applies to the id-keyed room map only.
/// Emitting it as a reset made C++ rebuild its index from it and reject the
/// adapter's next diffs, in a self-sustaining loop.
///
/// `stamps`: response-harvested recency (classic sync only). It overrides
/// last_activity_ms only when newer.
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
    enqueue(events, json!({ "type": "room_snapshot", "rooms": out }));
}

/// Room-list preview text for a room's cached latest event. Pure for tests.
///
/// Text messages give their body (decrypted bodies stay in memory; C++ never
/// persists encrypted-room previews), media their filename-style body.
/// Anything else (undecrypted, state, invites, unsent echoes) yields "" so
/// C++ keeps its placeholder.
pub(crate) fn latest_event_preview_text(
    value: &matrix_sdk_base::latest_event::LatestEventValue,
) -> String {
    use matrix_sdk_base::latest_event::LatestEventValue;

    match value {
        LatestEventValue::Remote(event) => raw_event_preview_text(event.raw()),
        _ => String::new(),
    }
}

/// The room-list preview line for one raw timeline event, empty when it has
/// none. Shared by the latest-event preview and the blank-preview backfill
/// (`previewbackfill`), so both read an event identically.
pub(crate) fn raw_event_preview_text(
    raw: &matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnySyncTimelineEvent>,
) -> String {
    use matrix_sdk::ruma::events::{
        room::message::Relation, AnySyncMessageLikeEvent, AnySyncTimelineEvent,
        SyncMessageLikeEvent,
    };

    fn body_or(body: String, fallback: &str) -> String {
        if body.is_empty() { fallback.to_owned() } else { body }
    }

    // Previews are one visual line and must not define row geometry.
    fn one_line(text: &str) -> String {
        text.split_whitespace().collect::<Vec<_>>().join(" ")
    }

    let Ok(deserialized) = raw.deserialize() else {
        return String::new();
    };
    let AnySyncTimelineEvent::MessageLike(message_like) = deserialized else {
        return String::new();
    };
    match message_like {
        AnySyncMessageLikeEvent::RoomMessage(SyncMessageLikeEvent::Original(message)) => {
            // When the latest message was edited, the SDK's latest event IS the
            // edit (latest_events builder), whose own body is the "* " fallback.
            // Preview its m.new_content, as a plain message would read.
            let msgtype = match message.content.relates_to {
                Some(Relation::Replacement(replacement)) => replacement.new_content.msgtype,
                _ => message.content.msgtype,
            };
            match msgtype {
                MessageType::Text(content) => {
                    one_line(crate::timeline::strip_reply_fallback(&content.body))
                }
                MessageType::Notice(content) => {
                    one_line(crate::timeline::strip_reply_fallback(&content.body))
                }
                MessageType::Emote(content) => {
                    one_line(crate::timeline::strip_reply_fallback(&content.body))
                }
                MessageType::Image(content) => body_or(one_line(&content.body), "Image"),
                MessageType::File(content) => body_or(one_line(&content.body), "File"),
                MessageType::Video(content) => body_or(one_line(&content.body), "Video"),
                MessageType::Audio(content) => match crate::timeline::voice_message_label(&content) {
                    Some(label) => one_line(&label),
                    None => body_or(one_line(&content.body), "Audio"),
                },
                // MSC4274 gallery: caption, else a summary. Never the body (Sable fills it
                // with `[name: mxc://…]` lines).
                other => match crate::timeline::parse_gallery(&other)
                    .filter(|g| !g.items.is_empty())
                {
                    Some(g) if !g.caption.trim().is_empty() => one_line(&g.caption),
                    Some(g) if g.items.len() == 1 => {
                        if g.all_images() { "Image" } else { "File" }.to_owned()
                    }
                    Some(g) => format!(
                        "{} {}",
                        g.items.len(),
                        if g.all_images() { "images" } else { "attachments" }
                    ),
                    None => String::new(),
                },
            }
        }
        AnySyncMessageLikeEvent::Sticker(SyncMessageLikeEvent::Original(_)) => {
            "Sticker".to_owned()
        }
        // MSC3381 poll start: preview the question rather than the multi-line
        // MSC1767 fallback.
        AnySyncMessageLikeEvent::UnstablePollStart(SyncMessageLikeEvent::Original(poll)) => {
            format!("Poll: {}", one_line(&poll.content.poll_start().question.text))
        }
        _ => String::new(),
    }
}

/// Maximum rooms whose latest event the SDK computes. The Latest Events API
/// is lazy: without `listen_to_room` a room gets no preview or activity
/// stamp until opened. Bounded for accounts with thousands of rooms; the
/// list is recency-sorted, so the first rooms delivered are those on screen.
const LATEST_EVENT_WATCH_CAP: usize = 200;

/// Coarse per-network bucket, used only for preview-slot fairness. A DM
/// whose partner localpart reads "<prefix>_<rest>" buckets by prefix;
/// everything else shares one bucket. Not the C++ BridgeNetwork table: a
/// wrong bucket only shifts the fairness split slightly.
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
    // Non-fatal: the room keeps an empty preview until an event or open
    // provides one.
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

    // Name a 1:1 DM after the person. The SDK's hero algorithm counts every
    // member, so a bridged DM (ghost plus bridge bot) reads "Sim, and 2
    // others". When m.direct names one partner with a stored profile, use it.
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

    // Use the SDK's Display impl (the full Matrix naming algorithm, including
    // "Empty Room"); fall back to the raw id only if it yields nothing.
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

/// Bound on the FFI event queue in case the C++ poll timer stalls. On
/// overflow the oldest events are dropped and one `queue_overflow` notice is
/// injected. Well above an initial sync's burst on a real account.
const EVENT_QUEUE_CAP: usize = 4096;

/// Capacity of the terminal-event lane. C++ limits its in-flight commands
/// (8 media slots and a few others), so this is a tripwire.
pub(crate) const COMMAND_QUEUE_CAP: usize = 512;

/// Enqueue an op-id-terminal result on the command lane. On overflow the
/// oldest event is dropped and its parked media payload freed first, so no
/// decrypted bytes are left in `media_results`.
pub(crate) fn enqueue_terminal(
    queue: &EventQueueRef,
    parked: &Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    value: serde_json::Value,
) {
    enqueue_terminal_routed(queue, Some(parked), None, value);
}

/// `enqueue_terminal`, freeing a dropped event's parked payload in the map
/// that event's TYPE owns: personal chat backgrounds (bgsync.rs) park in
/// their own map under op ids from another counter, so a dropped background
/// event must never free a timeline payload with the same number, nor the
/// reverse. A map that is not given is left alone.
pub(crate) fn enqueue_terminal_routed(
    queue: &EventQueueRef,
    media: Option<&Arc<Mutex<HashMap<u64, Vec<u8>>>>>,
    personal: Option<&Arc<Mutex<HashMap<u64, Vec<u8>>>>>,
    value: serde_json::Value,
) {
    let Ok(mut guard) = queue.lock() else { return };
    while guard.len() >= COMMAND_QUEUE_CAP {
        if let Some(dropped) = guard.pop_front() {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&dropped) {
                if let Some(op) = v.get("op_id").and_then(|o| o.as_u64()) {
                    let owned_by_personal = v
                        .get("type")
                        .and_then(|t| t.as_str())
                        .is_some_and(|t| t.starts_with("personal_background"));
                    let owner = if owned_by_personal { personal } else { media };
                    if let Some(map) = owner {
                        if let Ok(mut results) = map.lock() {
                            results.remove(&op);
                        }
                    }
                }
            }
        }
    }
    guard.push_back(value.to_string());
}

/// Wall-clock ms for sync-latency tracing, or None when off.
///
/// Uses LIGHTNING_SYNC_TRACE, the same switch as the C++ tracer
/// (src/app/SyncLatencyTracer.*). Wall clock because it is compared with
/// QDateTime::currentMSecsSinceEpoch() in C++. A relaxed load when off.
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

/// LIGHTNING_SWITCH_TRACE: the Rust half of the account-switch trace.
///
/// C++ `switchtrace` (src/app/SwitchTrace.*) writes one line per switch; the
/// steps it cannot see from the GUI thread are timed here and handed over as a
/// `"trace"` object on three events: `login_ok` / `login_failed` from
/// `mx_rust_restore` and `mx_rust_restore_from_file` (never from password or
/// OAuth login), and the first `room_list_reset` after each
/// `mx_rust_start_sync` (modern lane only).
///
/// Off, nothing changes: no scope is entered, every recorder below is a no-op
/// outside one, and an event without a trace is byte-identical to today's.
/// Numbers only; no ids, names or paths reach a trace object (CLAUDE.md §6).
///
/// The recorders reach the trace through tokio task-locals scoped around the
/// restore future and the sync future, so the shared build and restore helpers
/// keep their signatures and the login paths that also use them, which run
/// outside any scope, record nothing.
pub(crate) mod switchtrace {
    use std::cell::RefCell;
    use std::future::{Future, IntoFuture};
    use std::time::Instant;

    use serde_json::{json, Value};

    tokio::task_local! {
        static RESTORE_TRACE: RefCell<RestoreTrace>;
        static SYNC_TRACE: RefCell<SyncTrace>;
    }

    /// The Phase 0 contract's switch rule, shared with C++ and with
    /// LIGHTNING_SYNC_TRACE: on when set and, trimmed and lowercased, not one
    /// of "", "0", "false", "off", "no".
    pub(crate) fn switch_trace_enabled_from(value: Option<&str>) -> bool {
        value
            .map(|v| {
                let v = v.trim().to_ascii_lowercase();
                !(v.is_empty() || v == "0" || v == "false" || v == "off" || v == "no")
            })
            .unwrap_or(false)
    }

    /// LIGHTNING_SWITCH_TRACE, read once per process.
    pub(crate) fn switch_trace_enabled() -> bool {
        use std::sync::OnceLock;
        static ENABLED: OnceLock<bool> = OnceLock::new();
        *ENABLED.get_or_init(|| {
            switch_trace_enabled_from(std::env::var("LIGHTNING_SWITCH_TRACE").ok().as_deref())
        })
    }

    /// Wall-clock UNIX-epoch milliseconds, computed as `sync_trace_stamp_ms`
    /// computes them (C++ compares with QDateTime::currentMSecsSinceEpoch()).
    pub(crate) fn unix_epoch_ms() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0)
    }

    fn ms_between(earlier: Instant, later: Instant) -> u64 {
        later.saturating_duration_since(earlier).as_millis() as u64
    }

    /// Something that can be attached as an event's `"trace"` object.
    pub(crate) trait TraceObject {
        fn to_json(&self) -> Value;
    }

    /// Add `"trace"` to an event object. `None` (the trace is off, or this is
    /// not the event being traced) leaves the event exactly as it was.
    pub(crate) fn attach_trace<T: TraceObject>(event: &mut Value, trace: Option<&T>) {
        let (Some(trace), Some(map)) = (trace, event.as_object_mut()) else {
            return;
        };
        map.insert("trace".to_owned(), trace.to_json());
    }

    /// The restore steps timed for S8. Durations ACCUMULATE per step.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum RestoreStep {
        /// `builder.build()` of the discovering attempt: well-known, /versions
        /// and client construction (the stores are already open).
        Discovery,
        /// One `mediastore::open_account_stores`; also counts the opens.
        StoreOpen,
        /// The whole recorded-URL fallback `build_client_with`.
        FallbackBuild,
        /// `matrix_auth().restore_session`.
        Restore,
        /// `resume_unsent_requests`.
        ResumeUnsent,
    }

    /// What one restore cost, from scope entry to the event it is attached to.
    #[derive(Debug, Clone)]
    pub(crate) struct RestoreTrace {
        started: Instant,
        discovery_ms: u64,
        store_open_ms: u64,
        store_opens: u32,
        fallback_build_ms: u64,
        offline_fallback: bool,
        restore_ms: u64,
        resume_unsent_ms: u64,
    }

    impl RestoreTrace {
        pub(crate) fn new(started: Instant) -> Self {
            Self {
                started,
                discovery_ms: 0,
                store_open_ms: 0,
                store_opens: 0,
                fallback_build_ms: 0,
                offline_fallback: false,
                restore_ms: 0,
                resume_unsent_ms: 0,
            }
        }

        pub(crate) fn add(&mut self, step: RestoreStep, ms: u64) {
            match step {
                RestoreStep::Discovery => self.discovery_ms = self.discovery_ms.saturating_add(ms),
                RestoreStep::StoreOpen => {
                    self.store_open_ms = self.store_open_ms.saturating_add(ms);
                    self.store_opens = self.store_opens.saturating_add(1);
                }
                RestoreStep::FallbackBuild => {
                    self.fallback_build_ms = self.fallback_build_ms.saturating_add(ms)
                }
                RestoreStep::Restore => self.restore_ms = self.restore_ms.saturating_add(ms),
                RestoreStep::ResumeUnsent => {
                    self.resume_unsent_ms = self.resume_unsent_ms.saturating_add(ms)
                }
            }
        }

        pub(crate) fn note_offline_fallback(&mut self) {
            self.offline_fallback = true;
        }

        /// The object at a given enqueue instant and wall clock. Pure.
        pub(crate) fn to_json_at(&self, now: Instant, wall_ms: u64) -> Value {
            json!({
                "v": 1,
                "discovery_ms": self.discovery_ms,
                "store_open_ms": self.store_open_ms,
                "store_opens": self.store_opens,
                "fallback_build_ms": self.fallback_build_ms,
                "offline_fallback": self.offline_fallback,
                "restore_ms": self.restore_ms,
                "resume_unsent_ms": self.resume_unsent_ms,
                "restore_total_ms": ms_between(self.started, now),
                "enq_wall_ms": wall_ms,
            })
        }
    }

    impl TraceObject for RestoreTrace {
        fn to_json(&self) -> Value {
            self.to_json_at(Instant::now(), unix_epoch_ms())
        }
    }

    /// Run a restore future inside a trace scope when `enabled`, otherwise
    /// exactly as it is. Callers pass `switch_trace_enabled()`.
    pub(crate) async fn restore_scope<F>(enabled: bool, future: F)
    where
        F: Future<Output = ()>,
    {
        if enabled {
            RESTORE_TRACE.scope(RefCell::new(RestoreTrace::new(Instant::now())), future).await
        } else {
            future.await
        }
    }

    /// `event` with the current restore trace attached, or unchanged outside a
    /// restore scope.
    pub(crate) fn with_restore_trace(mut event: Value) -> Value {
        let trace = RESTORE_TRACE
            .try_with(|cell| cell.try_borrow().ok().map(|trace| trace.clone()))
            .ok()
            .flatten();
        attach_trace(&mut event, trace.as_ref());
        event
    }

    /// Mark the current restore as served from the recorded URL.
    pub(crate) fn note_offline_fallback() {
        let _ = RESTORE_TRACE.try_with(|cell| {
            if let Ok(mut trace) = cell.try_borrow_mut() {
                trace.note_offline_fallback();
            }
        });
    }

    /// Records on DROP, so a step cut off by a timeout (the discovering build
    /// under RESTORE_BUILD_BUDGET) still reports the time it took.
    struct RestoreStepTimer {
        step: RestoreStep,
        started: Instant,
    }

    impl Drop for RestoreStepTimer {
        fn drop(&mut self) {
            let ms = ms_between(self.started, Instant::now());
            let step = self.step;
            let _ = RESTORE_TRACE.try_with(|cell| {
                if let Ok(mut trace) = cell.try_borrow_mut() {
                    trace.add(step, ms);
                }
            });
        }
    }

    /// Await `future`, timing it as `step` when one is given and a restore
    /// scope is open. Otherwise exactly `future.await`.
    pub(crate) async fn timed<F: IntoFuture>(step: Option<RestoreStep>, future: F) -> F::Output {
        let _timer = step
            .filter(|_| RESTORE_TRACE.try_with(|_| ()).is_ok())
            .map(|step| RestoreStepTimer { step, started: Instant::now() });
        future.await
    }

    /// The `"trace"` object of the first `room_list_reset` after a start.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub(crate) struct ResetTrace {
        pub(crate) since_start_sync_ms: u64,
        pub(crate) sync_service_build_ms: u64,
        pub(crate) serialize_ms: u64,
        pub(crate) rooms: u32,
        pub(crate) enq_wall_ms: u64,
    }

    impl TraceObject for ResetTrace {
        fn to_json(&self) -> Value {
            json!({
                "v": 1,
                "since_start_sync_ms": self.since_start_sync_ms,
                "sync_service_build_ms": self.sync_service_build_ms,
                "serialize_ms": self.serialize_ms,
                "rooms": self.rooms,
                "enq_wall_ms": self.enq_wall_ms,
            })
        }
    }

    /// One sync session's trace state, scoped around the sync future.
    #[derive(Debug, Clone)]
    pub(crate) struct SyncTrace {
        started: Instant,
        /// The most recent `SyncService` build: the one whose list produced
        /// the first Reset, across supervisor rebuilds.
        sync_service_build_ms: u64,
        reset_reported: bool,
    }

    impl SyncTrace {
        pub(crate) fn new(started: Instant) -> Self {
            Self { started, sync_service_build_ms: 0, reset_reported: false }
        }

        pub(crate) fn note_sync_service_build(&mut self, ms: u64) {
            self.sync_service_build_ms = ms;
        }

        /// The first Reset's trace, once; `None` for every later Reset. Pure.
        pub(crate) fn take_first_reset(
            &mut self,
            serialize_started: Instant,
            now: Instant,
            rooms: usize,
            wall_ms: u64,
        ) -> Option<ResetTrace> {
            if self.reset_reported {
                return None;
            }
            self.reset_reported = true;
            Some(ResetTrace {
                since_start_sync_ms: ms_between(self.started, now),
                sync_service_build_ms: self.sync_service_build_ms,
                serialize_ms: ms_between(serialize_started, now),
                rooms: u32::try_from(rooms).unwrap_or(u32::MAX),
                enq_wall_ms: wall_ms,
            })
        }
    }

    /// Taken at `mx_rust_start_sync` entry: `Some` only while the trace is on.
    pub(crate) fn sync_start_stamp() -> Option<Instant> {
        switch_trace_enabled().then(Instant::now)
    }

    /// Run the sync future inside a trace scope when a start stamp was taken,
    /// otherwise exactly as it is.
    pub(crate) async fn sync_scope<F>(started: Option<Instant>, future: F) -> F::Output
    where
        F: Future,
    {
        match started {
            Some(started) => SYNC_TRACE.scope(RefCell::new(SyncTrace::new(started)), future).await,
            None => future.await,
        }
    }

    /// Await the `SyncService` build, recording its duration in a sync scope.
    pub(crate) async fn timed_sync_service_build<F: IntoFuture>(future: F) -> F::Output {
        let started = SYNC_TRACE.try_with(|_| ()).is_ok().then(Instant::now);
        let output = future.await;
        if let Some(started) = started {
            let ms = ms_between(started, Instant::now());
            let _ = SYNC_TRACE.try_with(|cell| {
                if let Ok(mut trace) = cell.try_borrow_mut() {
                    trace.note_sync_service_build(ms);
                }
            });
        }
        output
    }

    /// Called at the top of the Reset arm: when the first Reset of this sync
    /// session is still unreported, the instant serializing began.
    pub(crate) fn first_reset_serialize_start() -> Option<Instant> {
        SYNC_TRACE
            .try_with(|cell| cell.try_borrow().map(|t| !t.reset_reported).unwrap_or(false))
            .unwrap_or(false)
            .then(Instant::now)
    }

    /// The first Reset's trace, given the instant from
    /// `first_reset_serialize_start`; `None` outside a scope or after it.
    pub(crate) fn take_first_reset_trace(
        serialize_started: Option<Instant>,
        rooms: usize,
    ) -> Option<ResetTrace> {
        let serialize_started = serialize_started?;
        SYNC_TRACE
            .try_with(|cell| {
                cell.try_borrow_mut().ok().and_then(|mut trace| {
                    trace.take_first_reset(serialize_started, Instant::now(), rooms, unix_epoch_ms())
                })
            })
            .ok()
            .flatten()
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use std::collections::BTreeSet;
        use std::time::Duration;

        fn keys(value: &Value) -> BTreeSet<String> {
            value.as_object().expect("an object").keys().cloned().collect()
        }

        fn set(list: &[&str]) -> BTreeSet<String> {
            list.iter().map(|k| (*k).to_owned()).collect()
        }

        /// Written out from the Phase 0 contract §6.2, not from the code: C++
        /// WP0-C reads exactly these keys.
        const RESTORE_KEYS: [&str; 10] = [
            "v", "discovery_ms", "store_open_ms", "store_opens", "fallback_build_ms",
            "offline_fallback", "restore_ms", "resume_unsent_ms", "restore_total_ms",
            "enq_wall_ms",
        ];
        const RESET_KEYS: [&str; 6] = [
            "v", "since_start_sync_ms", "sync_service_build_ms", "serialize_ms", "rooms",
            "enq_wall_ms",
        ];

        #[test]
        fn switch_trace_enabled_from_follows_the_shared_rule() {
            for off in [None, Some(""), Some("   "), Some("0"), Some("false"), Some("FALSE"),
                        Some(" off "), Some("Off"), Some("no"), Some("NO\n")] {
                assert!(!switch_trace_enabled_from(off), "{off:?} must be off");
            }
            for on in [Some("1"), Some("true"), Some("yes"), Some("on"), Some(" 1 "),
                       Some("2"), Some("trace")] {
                assert!(switch_trace_enabled_from(on), "{on:?} must be on");
            }
        }

        #[test]
        fn restore_trace_json_carries_exactly_the_contract_keys() {
            let start = Instant::now();
            let mut trace = RestoreTrace::new(start);
            trace.add(RestoreStep::Discovery, 7);
            trace.add(RestoreStep::StoreOpen, 3);
            trace.add(RestoreStep::StoreOpen, 4);
            trace.add(RestoreStep::FallbackBuild, 11);
            trace.add(RestoreStep::Restore, 5);
            trace.add(RestoreStep::ResumeUnsent, 2);
            trace.note_offline_fallback();
            let value = trace.to_json_at(start + Duration::from_millis(40), 1_700_000_000_123);
            assert_eq!(keys(&value), set(&RESTORE_KEYS));
            assert_eq!(value["v"], 1);
            assert_eq!(value["discovery_ms"], 7);
            assert_eq!(value["store_open_ms"], 7);
            assert_eq!(value["store_opens"], 2);
            assert_eq!(value["fallback_build_ms"], 11);
            assert_eq!(value["offline_fallback"], true);
            assert_eq!(value["restore_ms"], 5);
            assert_eq!(value["resume_unsent_ms"], 2);
            assert_eq!(value["restore_total_ms"], 40);
            assert_eq!(value["enq_wall_ms"], 1_700_000_000_123u64);
            // The production path stamps the clocks itself; same keys.
            assert_eq!(keys(&TraceObject::to_json(&trace)), set(&RESTORE_KEYS));
        }

        #[test]
        fn reset_trace_json_carries_exactly_the_contract_keys() {
            let start = Instant::now();
            let mut sync = SyncTrace::new(start);
            sync.note_sync_service_build(9);
            let trace = sync
                .take_first_reset(start + Duration::from_millis(30),
                                  start + Duration::from_millis(45), 1234, 42)
                .expect("the first Reset is traced");
            let value = trace.to_json();
            assert_eq!(keys(&value), set(&RESET_KEYS));
            assert_eq!(value["v"], 1);
            assert_eq!(value["since_start_sync_ms"], 45);
            assert_eq!(value["sync_service_build_ms"], 9);
            assert_eq!(value["serialize_ms"], 15);
            assert_eq!(value["rooms"], 1234);
            assert_eq!(value["enq_wall_ms"], 42);
        }

        #[test]
        fn only_the_first_reset_of_a_sync_session_is_traced() {
            let start = Instant::now();
            let mut sync = SyncTrace::new(start);
            assert!(sync.take_first_reset(start, start, 1, 0).is_some());
            assert!(sync.take_first_reset(start, start, 1, 0).is_none());
            assert!(sync.take_first_reset(start, start, 1, 0).is_none());
        }

        fn login_ok() -> Value {
            json!({
                "type": "login_ok", "homeserver": "https://hs.invalid",
                "user_id": "@a:hs.invalid", "device_id": "DEV",
            })
        }

        #[test]
        fn attach_trace_with_none_leaves_the_event_byte_identical() {
            for original in [
                login_ok(),
                json!({ "type": "login_failed", "message": "nope" }),
                json!({ "type": "room_list_reset", "rooms": [{ "id": "!r:hs" }] }),
            ] {
                let before = serde_json::to_string(&original).unwrap();
                let mut restored = original.clone();
                attach_trace::<RestoreTrace>(&mut restored, None);
                assert_eq!(serde_json::to_string(&restored).unwrap(), before);
                let mut reset = original.clone();
                attach_trace::<ResetTrace>(&mut reset, None);
                assert_eq!(serde_json::to_string(&reset).unwrap(), before);
            }
        }

        #[test]
        fn attach_trace_with_some_adds_the_trace_key_and_nothing_else() {
            let original = login_ok();
            let mut event = original.clone();
            attach_trace(&mut event, Some(&RestoreTrace::new(Instant::now())));
            let mut expected = keys(&original);
            expected.insert("trace".to_owned());
            assert_eq!(keys(&event), expected);
            for key in keys(&original) {
                assert_eq!(event[&key], original[&key], "{key} changed");
            }
            assert_eq!(keys(&event["trace"]), set(&RESTORE_KEYS));
            // A non-object event is left alone rather than replaced.
            let mut scalar = json!("text");
            attach_trace(&mut scalar, Some(&RestoreTrace::new(Instant::now())));
            assert_eq!(scalar, json!("text"));
        }

        /// Off: the restore future runs unscoped, every recorder is a no-op,
        /// and the events it emits are byte-identical to today's.
        #[tokio::test]
        async fn a_restore_with_the_trace_off_emits_identical_events() {
            let emitted = std::sync::Arc::new(std::sync::Mutex::new(Vec::<String>::new()));
            let sink = std::sync::Arc::clone(&emitted);
            restore_scope(false, async move {
                timed(Some(RestoreStep::StoreOpen), async {}).await;
                timed(Some(RestoreStep::Discovery), async {}).await;
                note_offline_fallback();
                let event = with_restore_trace(login_ok());
                sink.lock().unwrap().push(serde_json::to_string(&event).unwrap());
            })
            .await;
            let emitted = emitted.lock().unwrap();
            assert_eq!(emitted.as_slice(), [serde_json::to_string(&login_ok()).unwrap()]);
        }

        /// On: the steps recorded inside the scope reach the event, a step cut
        /// off by a timeout still reports its time, and nothing outside the
        /// scope is touched.
        #[tokio::test]
        async fn a_restore_with_the_trace_on_attaches_what_its_steps_recorded() {
            let emitted = std::sync::Arc::new(std::sync::Mutex::new(None::<Value>));
            let sink = std::sync::Arc::clone(&emitted);
            restore_scope(true, async move {
                // A discovering build that never answers, cut off by its budget.
                let cut = tokio::time::timeout(
                    Duration::from_millis(30),
                    timed(Some(RestoreStep::Discovery), std::future::pending::<()>()),
                )
                .await;
                assert!(cut.is_err());
                timed(Some(RestoreStep::StoreOpen), async {}).await;
                timed(Some(RestoreStep::StoreOpen), async {}).await;
                // No step: untimed, as for a non-discovering build.
                timed(None, async {}).await;
                note_offline_fallback();
                *sink.lock().unwrap() = Some(with_restore_trace(login_ok()));
            })
            .await;
            let event = emitted.lock().unwrap().take().expect("an event");
            let trace = &event["trace"];
            assert_eq!(keys(trace), set(&RESTORE_KEYS));
            assert!(trace["discovery_ms"].as_u64().unwrap() >= 25,
                    "a cut-off discovery must still report its time: {trace}");
            assert_eq!(trace["store_opens"], 2);
            assert_eq!(trace["offline_fallback"], true);
            assert_eq!(trace["fallback_build_ms"], 0);
            assert!(trace["restore_total_ms"].as_u64().unwrap() >= 25);
            // Outside the scope: identical event, and recorders do not panic.
            note_offline_fallback();
            timed(Some(RestoreStep::Restore), async {}).await;
            assert_eq!(with_restore_trace(login_ok()), login_ok());
        }

        #[tokio::test]
        async fn a_sync_session_traces_its_first_reset_only_and_only_when_on() {
            // Off: no stamp, no scope, nothing recorded or returned.
            let off = sync_scope(None, async {
                timed_sync_service_build(async {}).await;
                let started = first_reset_serialize_start();
                (started.is_some(), take_first_reset_trace(started, 3))
            })
            .await;
            assert_eq!(off, (false, None));

            let on = sync_scope(Some(Instant::now()), async {
                timed_sync_service_build(tokio::time::sleep(Duration::from_millis(20))).await;
                let first = first_reset_serialize_start();
                let first = take_first_reset_trace(first, 5);
                let second = first_reset_serialize_start();
                let second_trace = take_first_reset_trace(second.or(Some(Instant::now())), 5);
                (first, second.is_some(), second_trace)
            })
            .await;
            let (first, second_started, second) = on;
            let first = first.expect("the first Reset is traced");
            assert_eq!(first.rooms, 5);
            assert!(first.sync_service_build_ms >= 15, "{first:?}");
            assert!(first.since_start_sync_ms >= first.sync_service_build_ms, "{first:?}");
            assert!(!second_started, "a second Reset must not start a trace");
            assert_eq!(second, None);
        }
    }
}

pub(crate) fn enqueue(events: &Arc<Mutex<VecDeque<String>>>, value: serde_json::Value) {
    let Ok(serialized) = serde_json::to_string(&value) else {
        return;
    };
    if let Ok(mut guard) = events.lock() {
        if guard.len() >= EVENT_QUEUE_CAP {
            // Drop the oldest; don't add another marker if we just dropped one.
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

/// Like `run_async`, but on the shared runtime, so tasks the SDK spawns
/// survive after the call returns.
///
/// Required for login/restore: matrix-sdk spawns its E2EE initialization
/// tasks (verification-state updater, `Backups::setup_and_resume`,
/// `Recovery::setup`, backup upload/download) on the ambient runtime. On a
/// per-call runtime they die when the call returns, leaving a received
/// backup key with nothing to consume it.
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

/// The event a send replies to, or `None` for an empty string. A malformed id
/// is an error, never a silently plain send.
///
/// # Safety
/// `ptr` must be a valid NUL-terminated string (null is refused by `cstr_arg`).
unsafe fn reply_target_arg(
    ptr: *const c_char,
) -> Result<Option<matrix_sdk::ruma::OwnedEventId>, String> {
    timeline::parse_reply_target(&unsafe { cstr_arg(ptr) }?)
}

/// Copy an optional send-side poster out of C++ memory.
///
/// Absent and oversized posters yield `None`; the video then sends without
/// one. The bound only prevents unbounded allocation; `PosterBytes` applies
/// the real policy and magic check. C++ frees its buffer on return, so the
/// copy is required.
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

/// Map an `import_room_keys` error message to a UI code using coarse
/// substrings. The raw message is passed through separately.
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
    #[test]
    fn mark_read_target_does_not_depend_on_latest_event() {
        let id = |s: &str| super::EventId::parse(s).unwrap();
        // Unwatched room: latest_event() is None, the event cache has the newest.
        assert_eq!(
            super::pick_receipt_target(Some(id("$new:x")), None, None),
            Some(id("$new:x")));
        // Stale latest_event must lose to what sync delivered last.
        assert_eq!(
            super::pick_receipt_target(Some(id("$new:x")), Some(id("$old:x")), None),
            Some(id("$new:x")));
        assert_eq!(
            super::pick_receipt_target(None, Some(id("$old:x")), Some(id("$f:x"))),
            Some(id("$old:x")));
        // Nothing local: the fetched page is used rather than sending no receipt.
        assert_eq!(
            super::pick_receipt_target(None, None, Some(id("$f:x"))),
            Some(id("$f:x")));
        assert_eq!(super::pick_receipt_target(None, None, None), None);
    }


    // ── Read-receipt privacy ────────────────────────────────────────────
    //
    // What other room members can observe in each mode, and that the
    // fully-read marker is sent in every mode.
    #[test]
    fn receipt_privacy_decides_what_other_members_can_see() {
        use super::receipts_for_mode;
        use matrix_sdk::ruma::EventId;

        let id = EventId::parse("$abc:example.org").unwrap();

        // 0: public.
        let public = receipts_for_mode(id.clone(), 0);
        assert_eq!(public.public_read_receipt.as_deref(), Some(&*id));
        assert!(public.private_read_receipt.is_none());
        assert_eq!(public.fully_read.as_deref(), Some(&*id));

        // 1: private. The server records it, so this account's other devices still
        // clear their badge; no other member sees it.
        let private = receipts_for_mode(id.clone(), 1);
        assert!(
            private.public_read_receipt.is_none(),
            "a private receipt must never also be published"
        );
        assert_eq!(private.private_read_receipt.as_deref(), Some(&*id));
        assert_eq!(private.fully_read.as_deref(), Some(&*id));

        // 2: "do not send read receipts". Nothing other members can see, but a
        // PRIVATE receipt, as Element does: matrix-sdk counts this device's unread
        // from the user's own m.read / m.read.private only, so sending none left
        // every room unread here after it was read (finding 66).
        let off = receipts_for_mode(id.clone(), 2);
        assert!(off.public_read_receipt.is_none(), "never visible to other members");
        assert_eq!(
            off.private_read_receipt.as_deref(),
            Some(&*id),
            "without a receipt of its own type this device can never clear a room"
        );
        assert_eq!(
            off.fully_read.as_deref(),
            Some(&*id),
            "the user's own read position is not a disclosure and must \
             survive every mode"
        );

        // An out-of-range value falls back to public rather than silently
        // stopping receipts.
        let bogus = receipts_for_mode(id.clone(), 77);
        assert_eq!(bogus.public_read_receipt.as_deref(), Some(&*id));
    }

    #[test]
    fn only_an_authorised_redaction_is_forwarded() {
        use matrix_sdk::ruma::user_id;
        let alice = user_id!("@alice:example.org");
        let mallory = user_id!("@mallory:example.org");
        // Your own message: always.
        assert!(super::redaction_applies(Some(alice), alice, Some(false)));
        // Someone else's: only with the power to redact others.
        assert!(!super::redaction_applies(Some(alice), mallory, Some(false)));
        assert!(super::redaction_applies(Some(alice), mallory, Some(true)));
        // Unknown power levels or an uncached target: forwarded, as before.
        assert!(super::redaction_applies(Some(alice), mallory, None));
        assert!(super::redaction_applies(None, mallory, Some(false)));
    }

    #[test]
    fn a_receipt_covers_the_reactions_after_the_newest_row() {
        use super::TailEvent;
        let id = |s: &str| super::EventId::parse(s).unwrap();
        let ev = |s: &str, t: &str, target: Option<&str>, threaded: bool| TailEvent {
            id: id(s),
            kind: Some(t.to_owned()),
            target: target.map(id),
            threaded,
        };
        let msg = |s: &str| ev(s, "m.room.message", None, false);
        let thread_reply = |s: &str| ev(s, "m.room.message", None, true);
        let react = |s: &str, to: &str| ev(s, "m.reaction", Some(to), false);
        let pick = |chosen: &str, tail: &[TailEvent]| {
            super::receipt_target_past_relations(&id(chosen), tail)
        };

        let tail = vec![
            msg("$m1:x"),
            msg("$m2:x"),
            react("$r1:x", "$m2:x"),
            ev("$d1:x", "m.room.redaction", Some("$r1:x"), false),
        ];
        assert_eq!(pick("$m2:x", &tail), id("$d1:x"));
        // A message after the chosen row is unseen: never marked read.
        assert_eq!(pick("$m1:x", &tail), id("$m1:x"));
        // Nothing after it and an unknown id stay put.
        assert_eq!(pick("$d1:x", &tail), id("$d1:x"));
        assert_eq!(pick("$zz:x", &tail), id("$zz:x"));
        // An undecryptable tail stays put.
        let utd = vec![
            msg("$m2:x"),
            TailEvent { id: id("$u:x"), kind: None, target: None, threaded: false },
        ];
        assert_eq!(pick("$m2:x", &utd), id("$m2:x"));

        // A reaction to a THREAD reply, which sits before the row the reader
        // saw, must not carry the Main receipt onto the reaction.
        let thread = vec![thread_reply("$t1:x"), msg("$m2:x"), react("$r1:x", "$t1:x")];
        assert_eq!(pick("$m2:x", &thread), id("$m2:x"));
        // Nor one to an event the cache does not hold.
        let unknown = vec![msg("$m2:x"), react("$r1:x", "$gone:x")];
        assert_eq!(pick("$m2:x", &unknown), id("$m2:x"));
        // One thread-annotating reaction among main ones blocks the move.
        let mixed = vec![
            thread_reply("$t1:x"),
            msg("$m2:x"),
            react("$r1:x", "$m2:x"),
            react("$r2:x", "$t1:x"),
        ];
        assert_eq!(pick("$m2:x", &mixed), id("$m2:x"));

        // An edit and a poll answer in the tail are content, not relations.
        let edit = vec![msg("$m2:x"), ev("$e1:x", "m.room.message", Some("$m2:x"), false)];
        assert_eq!(pick("$m2:x", &edit), id("$m2:x"));
        let poll = vec![
            msg("$m2:x"),
            ev("$p1:x", "org.matrix.msc3381.poll.response", Some("$m2:x"), false),
            react("$r1:x", "$m2:x"),
        ];
        assert_eq!(pick("$m2:x", &poll), id("$m2:x"));
        // A redaction of something that is not a reaction does not skip either.
        let redact_msg = vec![msg("$m1:x"), msg("$m2:x"), ev("$d:x", "m.room.redaction", Some("$m1:x"), false)];
        assert_eq!(pick("$m2:x", &redact_msg), id("$m2:x"));
    }

    #[test]
    fn the_fully_read_marker_never_moves_backwards() {
        // Another device read further: refuse.
        assert!(super::fully_read_would_move_back(Some(2_000), Some(1_000)));
        // Forward, or the same instant: write.
        assert!(!super::fully_read_would_move_back(Some(1_000), Some(2_000)));
        assert!(!super::fully_read_would_move_back(Some(1_000), Some(1_000)));
        // Not cached on either side: no evidence, write as before.
        assert!(!super::fully_read_would_move_back(None, Some(1_000)));
        assert!(!super::fully_read_would_move_back(Some(1_000), None));
        assert!(!super::fully_read_would_move_back(None, None));
    }

    // The automatic receipt while reading the live timeline: one receipt on the
    // MAIN thread (an unthreaded one marked older thread replies read on the
    // server, finding 72), never m.fully_read (moving it with every receipt
    // removed the "New messages" divider a second after a room opened, finding
    // 67), and private in modes 1 and 2 (finding 66).
    #[test]
    fn the_automatic_receipt_is_main_thread_and_never_fully_read() {
        use super::automatic_receipt;
        use matrix_sdk::ruma::{
            api::client::receipt::create_receipt::v3::ReceiptType,
            events::receipt::ReceiptThread, EventId,
        };

        let id = EventId::parse("$abc:example.org").unwrap();
        for (mode, expected) in [
            (0, ReceiptType::Read),
            (1, ReceiptType::ReadPrivate),
            (2, ReceiptType::ReadPrivate),
            (77, ReceiptType::Read),
        ] {
            let (receipt_type, thread, event) = automatic_receipt(id.clone(), mode);
            assert_eq!(receipt_type, expected, "mode {mode}");
            assert_ne!(receipt_type, ReceiptType::FullyRead, "mode {mode}");
            assert_eq!(thread, ReceiptThread::Main, "mode {mode}");
            assert_eq!(event, id);
        }
    }

    // Only two real keys that disagree may answer `Some(false)`; every way of
    // not knowing is None.
    #[test]
    fn identity_key_agreement_is_tri_state() {
        // Agreement: the healthy device.
        assert_eq!(
            super::identity_key_agreement(Some("AAAA"), Some("AAAA")),
            Some(true)
        );
        // Disagreement: peers encrypt to the published key, which we do not hold.
        assert_eq!(
            super::identity_key_agreement(Some("AAAA"), Some("BBBB")),
            Some(false)
        );
        // Every way of not knowing is None, never Some(false).
        assert_eq!(super::identity_key_agreement(None, Some("AAAA")), None,
                   "no local Olm key is 'unknown', not 'broken'");
        assert_eq!(super::identity_key_agreement(Some("AAAA"), None), None,
                   "the server publishing nothing for us is 'unknown'");
        assert_eq!(super::identity_key_agreement(None, None), None);
        // An empty string is not an answer either.
        assert_eq!(super::identity_key_agreement(Some(""), Some("AAAA")), None);
        assert_eq!(super::identity_key_agreement(Some("AAAA"), Some("")), None);
    }

    // A cancelled thread that will not stop must not hold the UI: a runtime
    // drop can block on a slow DNS resolution. On unfixed code this hangs.
    #[test]
    fn a_task_that_will_not_stop_is_detached_rather_than_waited_for() {
        use super::{join_task_within_budget, SyncTask, SYNC_TASK_JOIN_BUDGET_MS};
        let (park_tx, park_rx) = std::sync::mpsc::channel::<()>();
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
            // Stands in for a runtime drop blocked on getaddrinfo.
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

    // A finished task joins promptly without spending the budget.
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

    // ── join_or_abort double poll ────────────────────────────────────────
    //
    // Joining the same handles with a second `join_all` after the budget
    // re-polls tasks that already finished and panics with
    // "JoinHandle polled after completion". It needs one task to miss the
    // budget and another to finish inside it.
    //
    // `panic_is_catchable_in_the_test_profile` asserts panics unwind here,
    // which every should_panic case below relies on.

    #[test]
    #[should_panic(expected = "deliberate")]
    fn panic_is_catchable_in_the_test_profile() {
        // If this fails, every should_panic case below is vacuous.
        panic!("deliberate");
    }

    /// The old two-round shape, kept so the test can fail on it. Not used in
    /// production.
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
            // The second join_all over the same handles: the defect.
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
        // One task finishes immediately, one outlives the budget: the mix an
        // account switch produces.
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
        // On the unfixed code this aborts the process rather than failing, hence
        // the case above.
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
            // A zero budget: the slow task cannot make it, the quick one has.
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
            // Milliseconds; generous so a loaded machine does not flake.
            super::RustClient::join_or_abort(vec![a, b], 2000).await
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

    // The two orderings that never panicked. Both pass on the broken code;
    // they document the window, not the fix.
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
            // Round two polls only still-pending handles, which is legal.
            join_or_abort_the_old_broken_way(vec![a, b], 50).await;
        });
    }

    // ── Tracked FFI actions ──────────────────────────────────────────────
    //
    // "A recover() import cannot outlive mx_rust_destroy" needs a logged-in
    // Client and is not tested here. These assert the mechanism: a reported
    // action is registered in the pool `shutdown_managed_tasks` drains,
    // shutdown waits for it, and the panic report survives.

    fn test_bridge() -> super::RustClient {
        // `RustClient::new` only builds the runtime and empty registries.
        super::RustClient::new(
            std::env::temp_dir().join("lightning-spawn-reported-action-test"),
        )
        .expect("bridge")
    }

    /// Released when the owning future is dropped; stands in for the Client
    /// clone each action holds.
    struct ReleaseFlag(std::sync::Arc<std::sync::atomic::AtomicBool>);

    impl Drop for ReleaseFlag {
        fn drop(&mut self) {
            self.0.store(true, std::sync::atomic::Ordering::SeqCst);
        }
    }

    #[test]
    fn a_reported_action_is_registered_in_the_pool_shutdown_drains() {
        let bridge = test_bridge();
        let (release, released) = tokio::sync::oneshot::channel::<()>();
        bridge.spawn_reported_action("test_action", async move {
            let _ = released.await;
        });
        assert_eq!(
            bridge.room_action_tasks.lock().expect("pool").len(),
            1,
            "the action was not tracked; on the unfixed tree it ran on a \
             detached OS thread that no registry knew about"
        );
        let _ = release.send(());
    }

    #[test]
    fn shutdown_does_not_return_while_a_reported_action_still_holds_its_handles() {
        let bridge = test_bridge();
        let released =
            std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let flag = ReleaseFlag(std::sync::Arc::clone(&released));
        let (release, hold) = tokio::sync::oneshot::channel::<()>();
        bridge.spawn_reported_action("test_action", async move {
            let _flag = flag;
            let _ = hold.await;
        });
        // Start the releaser's wait right before shutdown, so scheduling noise
        // cannot satisfy the elapsed assertion.
        let releaser = std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(300));
            let _ = release.send(());
        });
        let began = std::time::Instant::now();
        bridge.shutdown_managed_tasks();
        assert!(
            released.load(std::sync::atomic::Ordering::SeqCst),
            "shutdown_managed_tasks returned while the action still owned \
             its handles"
        );
        assert!(
            began.elapsed() >= std::time::Duration::from_millis(200),
            "shutdown_managed_tasks returned before the action could have \
             finished, so it did not join it"
        );
        releaser.join().expect("releaser");
    }

    #[test]
    fn a_panicking_action_still_reports_itself_the_way_run_async_did() {
        // A panicking action must still enqueue `{"type":"error"}` (shown as a
        // banner); a bare spawn would lose the panic in `join_or_abort`.
        let bridge = test_bridge();
        bridge.spawn_reported_action("test_action", async {
            panic!("deliberate test panic");
        });
        bridge.shutdown_managed_tasks();
        let reported = bridge
            .events
            .lock()
            .expect("events")
            .iter()
            .filter_map(|raw| serde_json::from_str::<serde_json::Value>(raw).ok())
            .any(|event| {
                event["type"] == "error"
                    && event["message"] == "Rust SDK test_action task panicked."
            });
        assert!(reported, "a panicking action produced no error event");
    }

    #[test]
    fn catch_panic_passes_a_future_that_does_not_panic_straight_through() {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        assert_eq!(rt.block_on(super::CatchPanic::new(async {})), Ok(()));
    }

    #[test]
    fn catch_panic_never_polls_a_future_that_has_already_unwound() {
        // The future is dropped when it panics, so the second await takes the
        // "already resolved" branch.
        let polls = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let counter = std::sync::Arc::clone(&polls);
        let mut caught = super::CatchPanic::new(async move {
            counter.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            panic!("deliberate test panic");
        });
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        assert_eq!(rt.block_on(&mut caught), Err(()));
        assert_eq!(rt.block_on(&mut caught), Err(()));
        assert_eq!(
            polls.load(std::sync::atomic::Ordering::SeqCst),
            1,
            "the future was polled again after it unwound"
        );
    }

    // ── Teardown budget vs the C++ store-close wait ──────────────────────
    //
    // C++ covers the whole shutdown chain plus `mx_rust_destroy` with one
    // `waitForRustRetirement(kStoreCloseBudgetMs)`, then deletes the store
    // regardless.

    #[test]
    fn the_shutdown_budget_leaves_the_cpp_store_close_wait_room_to_finish() {
        // The compile-time assertion is the real gate; this prints the numbers
        // when broken and pins the reserve.
        assert_eq!(
            super::SHUTDOWN_WORST_CASE_MS,
            10_500,
            "the declared worst case moved; re-derive it against \
             kStoreCloseBudgetMs (src/matrix/RustSdkMatrixClient.h) and \
             update the table just below SYNC_TASK_JOIN_BUDGET_MS"
        );
        assert!(
            super::SHUTDOWN_WORST_CASE_MS + super::SHUTDOWN_DESTROY_RESERVE_MS
                <= super::STORE_CLOSE_BUDGET_MS,
            "shutdown {} ms + destroy reserve {} ms exceeds the C++ store-close \
             budget of {} ms",
            super::SHUTDOWN_WORST_CASE_MS,
            super::SHUTDOWN_DESTROY_RESERVE_MS,
            super::STORE_CLOSE_BUDGET_MS
        );
        // The verification join must cover one poll tick plus two flow cancels,
        // or a driver is aborted mid-cancel.
        assert!(
            super::SHUTDOWN_VERIFICATION_JOIN_MS
                >= super::VERIFICATION_POLL_MS + 2 * super::SHUTDOWN_FLOW_CANCEL_MS,
            "a SAS driver cannot notice the shutdown flag and cancel both \
             levels inside its join budget"
        );
    }

    #[test]
    fn the_timeline_shutdown_legs_are_bounded_like_an_abort_drain() {
        // The timeline legs abort before awaiting, so they are bounded like an
        // abort drain. If this fails, re-derive SHUTDOWN_WORST_CASE_MS.
        assert_eq!(crate::timeline::SHUTDOWN_ABORTED_JOIN_MS, 250);
        // Nothing else: SHUTDOWN_WORST_CASE_MS is defined as a sum including these
        // legs, so a comparison here could not fail. The compile-time assert can.
    }

    // A panic line may say where, never what: the payload can contain a message
    // body. `panic_report_line` takes no payload; assert it is still actionable.
    #[test]
    fn a_panic_report_names_where_it_happened_and_nothing_else() {
        let line = super::panic_report_line(Some(("rust/src/timeline.rs", 412)), Some("tokio-1"));
        assert!(line.contains("rust/src/timeline.rs:412"), "{line}");
        assert!(line.contains("tokio-1"), "{line}");
        // The escape hatch must be discoverable from the line itself.
        assert!(line.contains("LIGHTNING_PANIC_PAYLOAD"), "{line}");
        // Metadata this crate cannot supply must degrade, never panic.
        let bare = super::panic_report_line(None, None);
        assert!(bare.contains("<unknown location>"), "{bare}");
        assert!(bare.contains("<unnamed>"), "{bare}");
    }

    // An empty value is not a request (`VAR=` unsets in place).
    #[test]
    fn only_a_non_empty_override_asks_for_the_stock_panic_hook() {
        assert!(super::panic_payload_requested(Some("1")));
        assert!(super::panic_payload_requested(Some("yes")));
        assert!(!super::panic_payload_requested(None));
        assert!(!super::panic_payload_requested(Some("")));
        assert!(!super::panic_payload_requested(Some("   ")));
    }

    /// `STORE_CLOSE_BUDGET_MS` mirrors the C++ `kStoreCloseBudgetMs`. If C++
    /// lowered it alone, the compile-time budget check would silently permit a
    /// chain that outlasts the C++ wait.
    #[test]
    fn the_store_close_budget_mirrors_the_cpp_constant() {
        let header = include_str!("../../src/matrix/RustSdkMatrixClient.h");
        let needle = "static constexpr int kStoreCloseBudgetMs = ";
        let value: u64 = header
            .split(needle)
            .nth(1)
            .expect("kStoreCloseBudgetMs is gone from the header, or was renamed")
            .split(';')
            .next()
            .expect("kStoreCloseBudgetMs has no terminating semicolon")
            .trim()
            .parse()
            .expect("kStoreCloseBudgetMs is not a plain integer any more");
        assert_eq!(
            value, super::STORE_CLOSE_BUDGET_MS,
            "src/matrix/RustSdkMatrixClient.h says kStoreCloseBudgetMs = {value}, \
             but STORE_CLOSE_BUDGET_MS here is {}. The \
             compile-time shutdown-budget assertion is checking the wrong \
             number; move them together.",
            super::STORE_CLOSE_BUDGET_MS
        );
    }

    #[test]
    fn no_ffi_entry_point_falls_back_to_an_untracked_thread_plus_a_throwaway_runtime() {
        // Source scan: each label must be on the tracked pool and absent from the
        // old raw shape, so neither a rename nor a deletion passes vacuously.
        let source = include_str!("lib.rs");
        assert!(
            source.contains("fn spawn_reported_action"),
            "the scan is not reading the file it thinks it is"
        );
        for label in [
            "send_text",
            "probe_encrypted_send",
            "recover_backup",
            "reload_timeline",
            "rename_device",
            "backup_action",
            "backup_progress",
        ] {
            assert!(
                source.contains(&format!("spawn_reported_action(\"{label}\"")),
                "{label} no longer rides the tracked room-action pool"
            );
            assert!(
                !source.contains(&format!("run_async(runtime_events, \"{label}\"")),
                "{label} went back to a detached thread and a throwaway runtime"
            );
        }
    }

    // The user agent must be `Lightning/X.Y.Z` from the crate version.
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

    // The FFI notification-mode integers are a contract with C++
    // (SettingsManager, NotificationManager::RoomMode); round-trip both ways
    // and reject anything outside 0..=2.
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

    // How each way of muting a room that other clients use reads through the
    // SDK's own classification (the one Lightning reports). Element Web writes
    // an enabled OVERRIDE rule keyed by the room id with an `event_match`
    // `room_id` condition and NO actions; Element X goes through this same
    // SDK. A ROOM rule without `notify` is "Mentions & keywords" in Element
    // (not Mute), and a disabled rule counts for nothing.
    #[tokio::test]
    async fn every_standard_muted_room_push_rule_form_reads_as_the_right_mode() {
        use matrix_sdk::notification_settings::RoomNotificationMode as M;
        use matrix_sdk::test_utils::mocks::MatrixMockServer;
        use serde_json::json;
        let room = "!muted:matrix.example";
        let override_rule = |rule_id: &str, enabled: bool, actions: serde_json::Value| json!({
            "rule_id": rule_id, "default": false, "enabled": enabled, "actions": actions,
            "conditions": [{"kind": "event_match", "key": "room_id", "pattern": room}],
        });
        let room_rule = |actions: serde_json::Value| json!({
            "rule_id": room, "default": false, "enabled": true, "actions": actions,
        });
        let cases: Vec<(&str, serde_json::Value, serde_json::Value, Option<M>)> = vec![
            ("override, empty actions (Element Web mute)",
             json!({"override": [override_rule(room, true, json!([]))]}), json!({}), Some(M::Mute)),
            ("override, dont_notify",
             json!({"override": [override_rule(room, true, json!(["dont_notify"]))]}), json!({}), Some(M::Mute)),
            ("override with another rule id, condition names the room",
             json!({"override": [override_rule("custom-id", true, json!([]))]}), json!({}), Some(M::Mute)),
            ("disabled override is not a mute",
             json!({"override": [override_rule(room, false, json!([]))]}), json!({}), None),
            ("room rule, empty actions: mentions and keywords",
             json!({"room": [room_rule(json!([]))]}), json!({}), Some(M::MentionsAndKeywordsOnly)),
            ("room rule, dont_notify: mentions and keywords",
             json!({"room": [room_rule(json!(["dont_notify"]))]}), json!({}), Some(M::MentionsAndKeywordsOnly)),
            ("room rule, notify: all messages",
             json!({"room": [room_rule(json!(["notify"]))]}), json!({}), Some(M::AllMessages)),
            ("no rule", json!({}), json!({}), None),
        ];
        for (label, global, _unused, expected) in cases {
            let server = MatrixMockServer::new().await;
            let client = server.client_builder().build().await;
            server
                .mock_sync()
                .ok_and_run(&client, |builder| {
                    builder.add_custom_global_account_data(json!({
                        "type": "m.push_rules",
                        "content": {"global": global},
                    }));
                })
                .await;
            let settings = client.notification_settings().await;
            let parsed = matrix_sdk::ruma::RoomId::parse(room).unwrap();
            let got = settings.get_user_defined_room_notification_mode(&parsed).await;
            assert_eq!(got, expected, "{label}");
        }
    }

    #[test]
    fn out_of_range_notification_modes_are_rejected() {
        assert_eq!(super::notification_mode_from_int(-1), None);
        assert_eq!(super::notification_mode_from_int(3), None);
        assert_eq!(super::notification_mode_from_int(i32::MAX), None);
    }

    // Regression for a Mentions-only encrypted room showing a numbered badge:
    // the server's own count cannot evaluate content/mention conditions on
    // ciphertext, and `.max()`-ing it with the client's correct, decrypted-
    // content count let its number win.
    #[test]
    fn an_encrypted_room_trusts_only_the_client_computed_counts() {
        // The client's own ruleset evaluation (against the decrypted body)
        // found nothing but the one real mention; the server's raw count
        // disagrees. Only the client's answer may reach the badge.
        assert_eq!(
            super::unread_and_highlight_counts(true, 0, 1, 12, 3),
            (0, 1)
        );
    }

    #[test]
    fn an_unencrypted_room_still_takes_the_larger_of_the_two_counts() {
        // Unchanged behaviour: the server can evaluate every rule for a
        // plaintext room, so a count it has seen that the client's own cache
        // has not caught up to yet must not be thrown away.
        assert_eq!(
            super::unread_and_highlight_counts(false, 0, 0, 5, 1),
            (5, 1)
        );
    }

    #[test]
    fn unread_only_a_thread_can_explain_is_reported_for_a_plaintext_room() {
        // Main timeline read (client counts 0), server still counts 4 and one
        // highlight: the remainder is a thread.
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 4, 1), (true, true));
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 4, 0), (true, false));
    }

    #[test]
    fn thread_only_unread_is_not_claimed_when_it_cannot_be_known() {
        // Nothing unread at all.
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 0, 0), (false, false));
        // Plain main-timeline unread is just unread, even if threads add to it.
        assert_eq!(super::unread_in_threads_only(false, true, 2, 0, 9, 0), (false, false));
        assert_eq!(super::unread_in_threads_only(false, true, 0, 1, 9, 1), (false, false));
        // Encrypted: client counts exclude threads and server counts are unused,
        // so there is no honest signal.
        assert_eq!(super::unread_in_threads_only(true, true, 0, 0, 9, 3), (false, false));
    }

    #[test]
    fn the_own_identity_refetch_is_throttled_unless_local_state_moved() {
        // Break-it B014: a stale own identity is refetched, but a state that
        // stays put (an account with no cross-signing at all) is not fetched on
        // every snapshot.
        use super::{own_identity_refetch_due, OWN_IDENTITY_REFETCH_MIN};
        let t0 = std::time::Instant::now();
        let stuck = [false, false, false, false, false, false];
        assert!(own_identity_refetch_due(None, stuck, t0));
        assert!(!own_identity_refetch_due(Some((t0, stuck)), stuck, t0));
        assert!(!own_identity_refetch_due(
            Some((t0, stuck)),
            stuck,
            t0 + OWN_IDENTITY_REFETCH_MIN / 2
        ));
        assert!(own_identity_refetch_due(Some((t0, stuck)), stuck, t0 + OWN_IDENTITY_REFETCH_MIN));
        // Secrets arrived (a verification finished): due at once.
        let moved = [true, false, false, true, true, true];
        assert!(own_identity_refetch_due(Some((t0, stuck)), moved, t0));
    }

    #[test]
    fn the_own_identity_is_refetched_when_it_looks_stale_and_never_against_the_uia_guard() {
        // Break-it B014 review: the decision itself, not only its throttle.
        use super::own_identity_refetch_wanted as wanted;
        let granted = || true;
        let throttled = || false;
        // Nothing known locally (another session set cross-signing up and the
        // device-list change never reached us): refetched when the throttle allows.
        let stale = [false; 6];
        assert!(wanted(stale, true, granted));
        assert!(!wanted(stale, true, throttled));
        // Identity there but unverified, or this device not cross-signed.
        assert!(wanted([true, false, true, true, true, true], true, granted));
        assert!(wanted([true, true, false, false, false, false], true, granted));
        // A cross-signing job holds the UIA slot: never, whatever the state.
        assert!(!wanted(stale, false, granted));
        assert!(!wanted([true, true, false, true, true, true], false, granted));
        // Fully verified and cross-signed: never, and the throttle is not even
        // consulted (a claim would stamp the account for nothing).
        let asked = std::cell::Cell::new(false);
        assert!(!wanted([true; 6], true, || {
            asked.set(true);
            true
        }));
        assert!(!asked.get());
        // All three private keys present, this device not cross-signed: the
        // unconfirmed-setup case, refetched unthrottled as before.
        assert!(wanted([true, true, false, true, true, true], true, throttled));
    }

    #[test]
    fn space_children_without_an_order_key_follow_their_add_time() {
        // Break-it B051: added Alpha, Beta, Delta in that order; room ids
        // sort Beta < Alpha < Delta. Keyless children follow origin_server_ts.
        let keyless = vec![
            (None, 1_000, "!zzalpha:x".to_owned()),
            (None, 2_000, "!aabeta:x".to_owned()),
            (None, 3_000, "!mmdelta:x".to_owned()),
        ];
        assert_eq!(
            super::sort_space_children(keyless),
            vec!["!zzalpha:x", "!aabeta:x", "!mmdelta:x"]
        );
        // An order key still wins, an equal key falls to the timestamp, and
        // the room id breaks a timestamp tie.
        let mixed = vec![
            (None, 1, "!keyless:x".to_owned()),
            (Some("b".to_owned()), 5, "!b5:x".to_owned()),
            (Some("a".to_owned()), 9, "!a9:x".to_owned()),
            (Some("b".to_owned()), 2, "!b2:x".to_owned()),
            (None, 1, "!another:x".to_owned()),
        ];
        assert_eq!(
            super::sort_space_children(mixed),
            vec!["!a9:x", "!b2:x", "!b5:x", "!another:x", "!keyless:x"]
        );
    }

    #[test]
    fn a_room_never_read_is_not_thread_only_on_zero_client_counts() {
        // Break-it B012: joined live, never opened, no receipt. The client
        // counted nothing, so its zeros say nothing about the main timeline;
        // the server's 3 (and its highlight) are ordinary unread, not a thread.
        assert_eq!(super::unread_in_threads_only(false, false, 0, 0, 3, 0), (false, false));
        assert_eq!(super::unread_in_threads_only(false, false, 0, 0, 1, 1), (false, false));
        // The same counts once a receipt anchors the client's count still are.
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 3, 0), (true, false));
    }

    #[test]
    fn an_encrypted_room_with_a_real_mention_still_shows_it() {
        // The fix must not silence a genuine mention just because it is
        // encrypted; the client-computed mention count is exactly what
        // should reach the badge.
        assert_eq!(super::unread_and_highlight_counts(true, 3, 2, 3, 0), (3, 2));
    }

    // Marker discipline for notification-mode writes: a superseded task
    // neither writes nor reports, the winner consumes the marker once, and the
    // read path pends only while a write is unreported.
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

        // set(1) arrives while set(2) is in flight: set(2) must skip its report and
        // leave the marker.
        targets.lock().unwrap().insert(room.to_owned(), 1);
        assert!(!super::is_latest_notification_target(&targets, room, 2));
        assert!(!super::take_notification_target_if_latest(&targets, room, 2));
        assert!(super::notification_write_pending(&targets, room));

        // set(1)'s task reports: it is latest, consumes the marker once.
        assert!(super::take_notification_target_if_latest(&targets, room, 1));
        assert!(!super::notification_write_pending(&targets, room));
        // A second consume attempt (double report) finds nothing.
        assert!(!super::take_notification_target_if_latest(&targets, room, 1));

        // Duplicate sets of the same mode: the first reporter consumes the marker.
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

        // Orphan path (panic, abort, never polled): the guard clears the marker.
        targets.lock().unwrap().insert(room.to_owned(), 2);
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 2,
        });
        assert!(!super::notification_write_pending(&targets, room));

        // Superseded path: a newer task's marker is never touched.
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

    // Overflow of the terminal lane frees the dropped event's parked payload.
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

        // One more drops the oldest (op 1) and frees its bytes; op 2 survives.
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

    // The local-confirmation event carries the flow id only.
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
    // A slot left occupied refused every later attempt, and an unconditional
    // clear evicted a newer request. The SDK types cannot be constructed
    // here, so the rules run through the FlowLiveness/FlowIdentity traits.
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
        // SAS and show-QR slots are both keyed by flow id, so one fake covers both.
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
            // A live flow must not be evicted.
            assert_eq!(request_flow(&request).as_deref(), Some("flow-a"));
        }

        // Break-it B015: an incoming request cancelled by its requester while it
        // still waits for Accept is reported and released at once; one that was
        // accepted, or replaced by a newer flow, is left to its owner.
        #[test]
        fn a_request_cancelled_while_waiting_is_reported_and_released() {
            use crate::{pending_request_step, PendingRequest, PendingRequestView};

            #[derive(Debug)]
            struct Pending {
                flow_id: String,
                state: PendingRequest,
            }
            impl FlowIdentity for Pending {
                fn flow_key(&self) -> &str {
                    &self.flow_id
                }
            }
            impl PendingRequestView for Pending {
                fn pending(&self) -> PendingRequest {
                    self.state.clone()
                }
            }
            let slot = |state| {
                Mutex::new(Some(Pending { flow_id: "flow-a".to_owned(), state }))
            };

            let waiting = slot(PendingRequest::Waiting);
            assert_eq!(pending_request_step(&waiting, "flow-a"), (true, None));
            assert!(waiting.lock().unwrap().is_some());

            let cancelled = slot(PendingRequest::Cancelled("\"The user cancelled the verification.\"".to_owned()));
            assert_eq!(
                pending_request_step(&cancelled, "flow-a"),
                (false, Some("\"The user cancelled the verification.\"".to_owned()))
            );
            assert!(cancelled.lock().unwrap().is_none(), "a cancelled request keeps the slot");
            // Break-it B015 review: an Accept landing after that release answers
            // nothing (the cancel is already queued), not "no active request".
            assert_eq!(crate::accept_without_request_reply("flow-a"), "");
            assert_eq!(
                crate::accept_without_request_reply("flow-never-seen"),
                "error: no active verification request."
            );

            // Accepted: the driver reports its own outcome; the watch just ends.
            let answered = slot(PendingRequest::Answered);
            assert_eq!(pending_request_step(&answered, "flow-a"), (false, None));
            assert!(answered.lock().unwrap().is_some());

            // A newer flow in the slot is never touched, even if it is cancelled.
            let newer = slot(PendingRequest::Cancelled("x".to_owned()));
            assert_eq!(pending_request_step(&newer, "flow-old"), (false, None));
            assert!(newer.lock().unwrap().is_some());

            let empty: Mutex<Option<Pending>> = Mutex::new(None);
            assert_eq!(pending_request_step(&empty, "flow-a"), (false, None));
        }

        // An incoming request occupies the slot with no user action; once dead it
        // must not refuse later attempts.
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

        // A displayed QR code is a live flow: a second start must be refused.
        #[test]
        fn a_live_qr_alone_still_counts_as_live() {
            let (request, sas, qr) = slots();
            *qr.lock().unwrap() =
                Some(("flow-qr".to_owned(), FakeFlow::live("flow-qr")));
            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(sas_flow(&qr).as_deref(), Some("flow-qr"));
        }

        // The dead-occupant sweep must run for both method slots.
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

        // A terminating driver must not clear a newer request's slot.
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

        // QR-to-SAS hand-off: `drive_ready_request` retires only the QR slot and
        // lets the SAS driver keep the request.
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
            // A QR displayed when the driver exits must not stay parked.
            assert_eq!(sas_flow(&qr), None);
        }

        // A panicking driver must not leak the slots.
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

        // Teardown can only cancel a flow still in the slots, so take and clear
        // are one step. Whether the cancel reaches the peer is not testable here.
        #[test]
        fn teardown_takes_the_parked_flow_instead_of_discarding_it() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-teardown"));
            *sas.lock().unwrap() =
                Some(("flow-teardown".to_owned(), FakeFlow::live("flow-teardown")));

            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);

            // Handed to the caller so it can still cancel it...
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

        // Sign-out with a code on screen: teardown must take the QR handle, the
        // only thing that can cancel that peer's flow.
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

        // An unanswered incoming request has no driver, so only the slot sweep can
        // tell that peer we are gone.
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

    // ── QR verification: advertisement and module packing ──────────────
    //
    // The SDK types have crate-private constructors, so the handshake itself
    // (generate_qr_code, reciprocate, confirm, QR-to-SAS) is validated live
    // only. Covered here: the advertised methods and the grid-to-bits packing.
    mod qr_verification {
        use matrix_sdk::ruma::events::key::verification::VerificationMethod;

        // Claiming `m.qr_code.scan.v1` would make a peer display a code for a
        // client with no camera and wait out the SDK's 10-minute timeout.
        #[test]
        fn we_never_advertise_a_scanner_we_do_not_have() {
            let methods = crate::advertised_verification_methods();
            assert!(!methods.contains(&VerificationMethod::QrCodeScanV1));
        }

        // Showing a code needs `m.reciprocate.v1`, the start method a scanning
        // peer answers with.
        #[test]
        fn showing_a_qr_is_advertised_together_with_reciprocate() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::QrCodeShowV1));
            assert!(methods.contains(&VerificationMethod::ReciprocateV1));
        }

        // SAS remains the fallback for peers that can neither show nor scan.
        #[test]
        fn sas_remains_advertised_as_the_fallback() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::SasV1));
            assert_eq!(methods.len(), 3);
        }

        // The inbound and outbound advertisement sites must offer the same set.
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

        // Row-major, MSB-first, a fresh byte per row: the C++ renderer addresses
        // rows at `y * stride`, so shared bytes would shear the image.
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

        // A row wider than one byte must start the next row on a new byte.
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
            // Padding bits must stay clear, or the renderer draws past the code edge.
            for byte in bytes {
                assert_eq!(byte, 0b1110_0000);
            }
        }

        // Malformed geometry is refused: a short slice would index out of bounds
        // and an absurd size would allocate without bound.
        #[test]
        fn malformed_grids_are_refused_rather_than_rendered() {
            assert!(crate::pack_qr_modules(&[], 0).is_none());
            assert!(crate::pack_qr_modules(&[true, false], 3).is_none());
            assert!(crate::pack_qr_modules(&[true; 9], 2).is_none());
            let oversized = crate::QR_MAX_MODULES + 1;
            assert!(crate::pack_qr_modules(&vec![false; 4], oversized).is_none());
        }

        // The payload is pure geometry: it round-trips to the same grid, so
        // nothing else rides along.
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

    // The sanitized event matches the crypto_bootstrap shape exactly (kind,
    // fixed state string, zero count, lifecycle), with no extra fields.
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

    // Retry ladder: bounded, ordered delays; exhausted past the end.
    #[test]
    fn secret_retry_ladder_is_bounded_and_ordered() {
        let d0 = super::next_secret_retry_delay(0).expect("first attempt");
        let d1 = super::next_secret_retry_delay(1).expect("second attempt");
        let d2 = super::next_secret_retry_delay(2).expect("third attempt");
        assert!(d0 < d1 && d1 < d2, "delays must back off");
        assert_eq!(super::next_secret_retry_delay(3), None);
        assert_eq!(super::next_secret_retry_delay(usize::MAX), None);
    }

    // Cross-signing keys are always required; the backup key only when the
    // probe said a backup exists.
    #[test]
    fn secret_recovery_missing_decision_table() {
        let missing = super::secret_recovery_missing;
        // Nothing arrived yet.
        assert!(missing(false, Some(true), false));
        // Cross-signing incomplete alone is enough, whatever the backup.
        assert!(missing(false, Some(false), false));
        assert!(missing(false, None, true));
        // Cross-signing complete but a server-side backup is not usable.
        assert!(missing(true, Some(true), false));
        // Fully recovered.
        assert!(!missing(true, Some(true), true));
        // No backup on the server (or unknown): cross-signing decides alone.
        assert!(!missing(true, Some(false), false));
        assert!(!missing(true, None, false));
    }

    // Every coordinator emission uses the crypto_bootstrap shape, with no
    // extra fields or identifiers.
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

    use super::{latest_event_preview_text, room_ordering_timestamp_ms};

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

    // The raw body of a reply opens with the quoted original (2026-10-13:
    // "> <@moosix:cutefunny.art> You kno..." in the room list).
    #[test]
    fn reply_fallback_is_not_the_preview() {
        let value = remote(
            json!({
                "msgtype": "m.text",
                "body": "> <@moosix:cutefunny.art> You know what\n> I mean\n\nyes I do",
                "format": "org.matrix.custom.html",
                "formatted_body": "<mx-reply><blockquote>x</blockquote></mx-reply>yes I do",
            }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&value), "yes I do");
        let quote_only = remote(
            json!({ "msgtype": "m.text", "body": "> <@a:b.c> quoted\nno separator" }),
            "m.room.message",
        );
        assert_eq!(
            latest_event_preview_text(&quote_only),
            "> <@a:b.c> quoted no separator"
        );
    }

    // Break-it B108: a voice message's body is a file name; the room list
    // said "t12.ogg" where a loaded row said "Voice message".
    #[test]
    fn a_voice_message_previews_as_a_voice_message() {
        let voice = remote(
            json!({
                "msgtype": "m.audio",
                "body": "t12.ogg",
                "url": "mxc://example.org/v",
                "org.matrix.msc3245.voice": {},
                "org.matrix.msc1767.audio": { "duration": 3000 },
            }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&voice), "Voice message");
        // Ordinary audio keeps its name.
        let audio = remote(
            json!({ "msgtype": "m.audio", "body": "song.ogg", "url": "mxc://example.org/a" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&audio), "song.ogg");
    }

    #[test]
    fn text_message_surfaces_body() {
        let value = remote(
            json!({ "msgtype": "m.text", "body": "hello rooms" }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&value), "hello rooms");
    }

    // When the latest message was edited, the SDK's latest event is the edit,
    // whose own body is the "* " fallback (a Sable edit read
    // "* edited: **bold words** ..." in the room list, 2026-09-30).
    #[test]
    fn an_edit_previews_its_new_content_not_the_fallback() {
        let new_body = "edited: **bold words** and `inline code`";
        let edit = remote(
            json!({
                "msgtype": "m.text",
                "body": format!("* {new_body}"),
                "m.new_content": { "msgtype": "m.text", "body": new_body },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$original" },
            }),
            "m.room.message",
        );
        let plain = remote(
            json!({ "msgtype": "m.text", "body": new_body }),
            "m.room.message",
        );
        assert_eq!(latest_event_preview_text(&edit), new_body);
        assert_eq!(
            latest_event_preview_text(&edit),
            latest_event_preview_text(&plain)
        );
    }

    // An edit never moves a room: an edited latest message is stamped with
    // the edited event's time, never the edit's (`remote` stamps 42). Editing
    // the latest message in a room lower in the list moved it to the top
    // (live, 2026-09-30).
    #[test]
    fn an_edited_latest_message_keeps_the_edited_events_stamp() {
        let edit = remote(
            json!({
                "msgtype": "m.text",
                "body": "* new words",
                "m.new_content": { "msgtype": "m.text", "body": "new words" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$original" },
            }),
            "m.room.message",
        );
        assert_eq!(room_ordering_timestamp_ms(&edit, Some(7)), 7);
        // Not known: 0, which C++ ignores, so the room stays where it is.
        assert_eq!(room_ordering_timestamp_ms(&edit, None), 0);
        // Control: a message that is not an edit keeps its own stamp.
        let plain = remote(
            json!({ "msgtype": "m.text", "body": "new words" }),
            "m.room.message",
        );
        assert_eq!(room_ordering_timestamp_ms(&plain, Some(7)), 42);
    }

    #[test]
    fn redact_matrix_id_matches_the_cpp_logs() {
        // sha256("abc") starts ba7816bf; sigil kept, empty is <none>.
        assert_eq!(super::redact_matrix_id("!abc"), "!ba7816bf");
        assert_eq!(super::redact_matrix_id("abc"), "ba7816bf");
        assert_eq!(super::redact_matrix_id(""), "<none>");
    }

    #[test]
    fn a_voice_message_is_told_from_a_plain_audio_file() {
        use super::message_is_voice;
        use matrix_sdk::ruma::events::room::message::MessageType;
        let parse = |content: serde_json::Value| -> MessageType {
            let c: matrix_sdk::ruma::events::room::message::RoomMessageEventContent =
                serde_json::from_value(content).unwrap();
            c.msgtype
        };
        let voice = parse(json!({
            "msgtype": "m.audio", "body": "Voice message",
            "url": "mxc://example.org/v",
            "org.matrix.msc1767.audio": { "duration": 3000 },
            "org.matrix.msc3245.voice": {}
        }));
        let file = parse(json!({
            "msgtype": "m.audio", "body": "song.mp3", "url": "mxc://example.org/s"
        }));
        let text = parse(json!({ "msgtype": "m.text", "body": "hi" }));
        assert!(message_is_voice(&voice));
        assert!(!message_is_voice(&file));
        assert!(!message_is_voice(&text));
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

    // An MSC4274 gallery as Sable sends it (body is a generated `[name: mxc]`
    // list): preview the caption or the contents, never the list.
    #[test]
    fn a_gallery_previews_as_its_caption_or_its_count() {
        let item = |name: &str, itemtype: &str| {
            json!({ "itemtype": itemtype, "body": name, "filename": name,
                    "url": format!("mxc://example.org/{name}") })
        };
        let gallery = |body: &str, items: Vec<serde_json::Value>| {
            remote(
                json!({ "msgtype": "dm.filament.gallery", "body": body,
                        "itemtypes": items }),
                "m.room.message",
            )
        };
        let list = "[a.png: mxc://example.org/a.png]\n[b.png: mxc://example.org/b.png]";
        assert_eq!(
            latest_event_preview_text(&gallery(
                list,
                vec![item("a.png", "m.image"), item("b.png", "m.image")]
            )),
            "2 images"
        );
        assert_eq!(
            latest_event_preview_text(&gallery(
                "",
                vec![item("a.png", "m.image"), item("n.pdf", "m.file")]
            )),
            "2 attachments"
        );
        assert_eq!(
            latest_event_preview_text(&gallery(
                "before\nand after",
                vec![item("a.png", "m.image"), item("b.png", "m.image")]
            )),
            "before and after"
        );
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
        // An undecryptable latest event yields no preview text; C++ shows its
        // placeholder.
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

/// Live two-device E2EE interoperability harness.
///
/// Runs only with `--ignored` and LIGHTNING_LIVE_E2EE=1, against a real
/// homeserver and dedicated test account (LIGHTNING_TEST_HOMESERVER /
/// LIGHTNING_TEST_USER / LIGHTNING_TEST_PASSWORD).
///
/// Device A is a plain matrix-sdk client that bootstraps cross-signing and
/// backup, seeds encrypted history, and answers SAS and secret requests.
/// Device B is the real Lightning bridge. A's sync is paused after its SAS
/// confirmation, so the recovery coordinator must recover once A resumes
/// (ladder, secret acceptance, backup, per-room restore, restart
/// persistence).
///
/// Never prints payloads, tokens, keys or identifiers.
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
    /// Never logs payloads.
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
        // The SDK's sanitized crypto tracing, off unless RUST_LOG is set.
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

        // Cross-signing bootstrap and recovery/backup for the test account.
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
                    // Creates 4S and key backup. The recovery key is dropped immediately. A
                    // stale backup from an earlier run is deleted and recreated.
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

        // Seed encrypted history and wait for its keys to reach backup. The room
        // has a recognizable name and run marker and is removed at the end.
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
            // A new device.
            let device = CString::new("").unwrap();
            let r = take(super::mx_rust_login(
                handle, hs.as_ptr(), us.as_ptr(), pw.as_ptr(), device.as_ptr(),
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

            // ── SAS verification; the peer pauses after its own confirmation. ──
            let r = take(super::mx_rust_start_own_verification(handle));
            assert!(r.is_empty(), "start verification dispatch");
            let started = wait_for(
                handle,
                "verification request started",
                Duration::from_secs(30),
                |ev| ev["type"] == "verification_request_started",
            );
            let flow_id = started["flow_id"].as_str().unwrap().to_owned();

            // The peer accepts and drives SAS to its own confirmation.
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
            // Pause the peer now: its MAC is out, but it will not see ours, sign the
            // new device or answer secret requests until resumed.
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

            // Let the first ladder window run against the paused peer, then resume.
            std::thread::sleep(Duration::from_secs(30));
            pause_tx.send(false).ok();
            eprintln!("[live] peer: resumed");

            // The coordinator must reach the recovered state: cross-signing secrets
            // present and backup enabled.
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
                        // Print coordinator progress (fixed kind/state tokens only).
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

            // A manual re-request after completion reports nothing missing.
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
                // The sqlite pool releases connections via spawn_blocking, so the drop
                // needs an ambient runtime (harness only).
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

            // Sign the test device out and wait for the round trip.
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

        // Remove the test room this run created (by id, as the peer), so repeated
        // runs do not accumulate rooms. Failure is reported but does not change the
        // result. LIGHTNING_LIVE_E2EE_KEEP_ROOMS=1 keeps it.
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

    // ── Local search against a real homeserver ──────────────────────────
    //
    // Proves the path into the index (login, sync, decrypted events in the
    // event cache, sweep, FTS5 query). Gated like the other live tests; reads
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
            // A new device.
            let device = CString::new("").unwrap();
            let err = take(super::mx_rust_login(
                handle, hs.as_ptr(), u.as_ptr(), p.as_ptr(), device.as_ptr()));
            assert!(err.is_empty(), "login dispatch: {err}");
            wait_for(handle, "login", Duration::from_secs(60), |ev| {
                ev["type"] == "login_ok"
            });
            super::mx_rust_start_sync(handle);
            // Room ids come from the sync's own events; no FFI enumerates them.
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
                    // Sliding sync delivers the list as diffs, so collect ids from every event
                    // kind, not only the reset.
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

            // Let the event cache receive some timeline first; sweeping an empty cache
            // would pass on a broken indexer.
            std::thread::sleep(Duration::from_secs(12));

            // Sweep, then deepen every joined room so older history is indexed.
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

            // The actual claim.
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

            // Search in an encrypted room, asserted separately: a needle found only in
            // a public room would pass the check above. The message is sent here
            // because only a client holding the keys can produce it.
            if let Some(encrypted_room) = env_nonempty("LIGHTNING_TEST_ENCRYPTED_ROOM") {
                let secret = format!(
                    "zephyrine-{}", std::process::id());
                let rid = CString::new(encrypted_room.clone()).unwrap();
                let body = CString::new(secret.clone()).unwrap();
                let txn = CString::new(format!("live-{}", std::process::id())).unwrap();
                let err = take(super::mx_rust_send_text(
                    handle, rid.as_ptr(), body.as_ptr(), txn.as_ptr()));
                assert!(err.is_empty(), "encrypted send dispatch: {err}");
                // Let the send and sync land the event in the cache, decrypted.
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

            // A redacted message must not be findable by its text. Asserts the
            // outcome, not the mechanism.
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

            // ── Jump to date (MSC3030) against the real server ───────
            //
            // Whether `timestamp_to_event` exists depends on the homeserver. Asking
            // for the event at a known event's timestamp, searching forward, must
            // return that event. A 404 M_UNRECOGNIZED is reported, not failed.
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

            // A query below the tokenizer minimum is reported as too short.
            let short = CString::new("ab").unwrap();
            let _ = take(super::mx_rust_local_search(
                handle, short.as_ptr(), empty.as_ptr(), 10, 0, 9302));
            let tooshort = wait_for(handle, "too short",
                                    Duration::from_secs(30), |ev| {
                ev["type"] == "local_search_result" && ev["op_id"] == 9302
            });
            assert_eq!(tooshort["ok"], false);
            assert_eq!(tooshort["category"], "too_short");

            // ── Widgets, against the same live account ──────────────
            //
            // Unit tests cover parsing and refusals; this shows the state read finds
            // real widgets, that a tombstone is not a widget, and that hostile shapes
            // are refused with the right reason.
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
                // A tombstone (`{}`) is not a widget.
                assert!(by_id("dead").is_none(), "a tombstone came back as a widget");

                let jitsi = by_id("jitsi").expect("the real widget was not found");
                let url = jitsi["url"].as_str().unwrap_or("");
                assert!(url.starts_with("https://meet.example.org/"), "{url}");
                assert!(!url.contains('$'), "a variable was left in: {url}");
                // Room and user ids are percent-encoded so they cannot restructure the URL.
                // `!` stays literal, as in encodeURIComponent (matrix-widget-api and
                // matrix-sdk do the same).
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

    /// A search during sign-out must not reopen the index (an open handle makes
    /// the store deletion fail on Windows). Offline: the search path needs no
    /// server. The control search before shutdown proves search worked.
    #[test]
    fn a_search_after_shutdown_refuses_instead_of_reopening_the_index() {
        let dir = tempfile_dir("lightning-search-shutdown");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("store dir");
        let store = CString::new(dir.to_string_lossy().as_ref()).unwrap();
        let handle = super::mx_rust_create(store.as_ptr());
        assert!(!handle.is_null(), "bridge handle");

        let index_path = dir.join(super::localsearch::INDEX_FILE);
        unsafe {
            let query = CString::new("quarterly deployment").unwrap();
            let any_room = CString::new("").unwrap();

            let err = take(super::mx_rust_local_search(
                handle, query.as_ptr(), any_room.as_ptr(), 10, 0, 1));
            assert!(err.is_empty(), "a search BEFORE shutdown was refused: {err}");
            assert!(index_path.exists(),
                    "the control search did not open an index, so the                      assertion below would pass on any code");

            let _ = take(super::mx_rust_shutdown_tasks(handle));
            // Stand in for sign-out deleting the store, so a recreated file proves a
            // reopen.
            let _ = std::fs::remove_file(&index_path);

            let err = take(super::mx_rust_local_search(
                handle, query.as_ptr(), any_room.as_ptr(), 10, 0, 2));
            assert!(!err.is_empty(),
                    "a search after shutdown was accepted; it reopened the                      index sign-out had just closed");
            assert!(!index_path.exists(),
                    "a search after shutdown recreated the index file");

            super::mx_rust_destroy(handle);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// An index that cannot open must still ANSWER the search. The C++ side
    /// discards the returned "error:" string and waits for a
    /// `local_search_result` for its op id, so without one the panel spins for
    /// ever and nothing says why.
    #[test]
    fn a_search_whose_index_cannot_open_still_answers_its_op_id() {
        let dir = tempfile_dir("lightning-search-unopenable");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("store dir");
        let store = CString::new(dir.to_string_lossy().as_ref()).unwrap();
        let handle = super::mx_rust_create(store.as_ptr());
        assert!(!handle.is_null(), "bridge handle");
        // Then the "directory" the index would live in becomes a regular file.
        std::fs::remove_dir_all(&dir).expect("remove store");
        std::fs::write(&dir, b"x").expect("blocker");
        unsafe {
            let query = CString::new("quarterly deployment").unwrap();
            let any_room = CString::new("").unwrap();
            let err = take(super::mx_rust_local_search(
                handle, query.as_ptr(), any_room.as_ptr(), 10, 0, 77));
            assert!(!err.is_empty(), "the index opened, so this proves nothing");
            let mut answered = None;
            for _ in 0..50 {
                let ev = take(super::mx_rust_poll_event(handle));
                if let Ok(v) = serde_json::from_str::<serde_json::Value>(&ev) {
                    if v["type"] == "local_search_result" && v["op_id"] == 77 {
                        answered = Some(v);
                        break;
                    }
                }
            }
            let ev = answered.expect("no local_search_result for a failed search");
            assert_eq!(ev["ok"], false);
            assert_eq!(ev["category"], "index_unavailable");
            super::mx_rust_destroy(handle);
        }
        let _ = std::fs::remove_file(&dir);
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
// A server name delegating elsewhere must be followed:
// `server_name_or_homeserver_url()` does discovery, `homeserver_url()` does
// not. These build a real SDK client over loopback HTTP, where plain
// TcpListeners play the delegating host and the real homeserver.
#[cfg(test)]
mod delegation_tests {
    use super::{build_client, build_client_for_restore, read_resolved_homeserver};
    use std::collections::VecDeque;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;

    fn respond(mut stream: TcpStream, body: &str) {
        let mut buf = [0u8; 2048];
        let read = stream.read(&mut buf).unwrap_or(0);
        let request = String::from_utf8_lossy(&buf[..read]).to_string();
        // 404 for anything else, like an apex web server.
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
        // The server name the user types: serves a well-known pointing at the
        // homeserver above and 404s every Matrix API path.
        let delegating = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{homeserver}"}}}}"#
        ));

        let client = runtime()
            .block_on(build_client(&format!("http://{delegating}"), &PathBuf::new()))
            .expect("delegated login must build a client");

        // The client must point at the delegated host, not the typed one.
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
        // Typing the homeserver directly: no well-known, so the builder must fall
        // back to the URL.
        let homeserver = serve(versions_body());

        let client = runtime()
            .block_on(build_client(&format!("http://{homeserver}"), &PathBuf::new()))
            .expect("a direct homeserver URL must still work");

        assert_eq!(
            client.homeserver().port(),
            homeserver.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()),
        );
    }

    /// A `serve()` that can be shut down, so a test can see a homeserver go
    /// away.
    fn serve_stoppable(body: String) -> (String, Arc<AtomicBool>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = Arc::clone(&stop);
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                if thread_stop.load(Ordering::SeqCst) {
                    // Dropping the listener makes the port refuse rather than hang.
                    break;
                }
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
        (format!("{}:{}", addr.ip(), addr.port()), stop)
    }

    /// A per-test store directory, removed on drop (no `tempfile` dependency).
    struct StoreDir(PathBuf);
    impl StoreDir {
        fn new(name: &str) -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let path = std::env::temp_dir().join(format!(
                "lightning-{name}-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).expect("store dir");
            Self(path)
        }
        fn path(&self) -> &std::path::Path {
            &self.0
        }
    }
    impl Drop for StoreDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn stop_serving(addr: &str, stop: &Arc<AtomicBool>) {
        stop.store(true, Ordering::SeqCst);
        // One connection to break `incoming()` out of its blocking accept.
        let _ = TcpStream::connect(addr);
        // The listener is dropped on the next loop iteration; give it one.
        for _ in 0..200 {
            if TcpStream::connect(addr).is_err() {
                return;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("the test homeserver would not stop listening");
    }

    // A homeserver that is down must not send the user back to the login page:
    // `build_client` cannot build without a server, `build_client_for_restore`
    // must.
    #[test]
    fn a_restore_survives_the_homeserver_going_away() {
        let (homeserver, stop) = serve_stoppable(versions_body());
        let store = StoreDir::new("offline-restore");
        let typed = format!("http://{homeserver}");
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));

        // Everything inside one `block_on`: a Client with a sqlite store closes its
        // pool on drop, and deadpool aborts outside a runtime.
        runtime().block_on(async {
            // One online build, which records what the SDK resolved.
            let resolved = {
                let online = build_client(&typed, store.path())
                    .await
                    .expect("the server is up; this must build");
                online.homeserver().to_string()
            };
            assert_eq!(
                read_resolved_homeserver(store.path()).as_deref(),
                Some(resolved.as_str()),
                "a successful build did not record the homeserver it resolved"
            );

            // The server dies.
            stop_serving(&homeserver, &stop);

            // The plain build fails with the server down.
            assert!(
                build_client(&typed, store.path()).await.is_err(),
                "the test server is still answering; the rest proves nothing"
            );

            // The restore path does, from what it recorded.
            let offline = build_client_for_restore(&typed, store.path(), &events)
                .await
                .expect("a restore must survive an unreachable homeserver");
            assert_eq!(
                offline.homeserver().to_string(),
                resolved,
                "the offline client is pointed somewhere else than the account is"
            );
            let said: Vec<String> =
                events.lock().expect("events").iter().cloned().collect();
            assert!(
                said.iter().any(|e| e.contains("session_restored_offline")),
                "the app was never told this session came off the disk: {said:?}"
            );
        });
    }

    // LIGHTNING_SWITCH_TRACE: the real restore helpers record into a traced
    // restore, so the steps the C++ trace reports are the ones that ran. An
    // online restore opens the stores once; an offline one opens them twice
    // (the discovering attempt, then the recorded-URL fallback) and says so.
    #[test]
    fn a_traced_restore_records_what_the_build_actually_did() {
        use crate::switchtrace;
        use serde_json::{json, Value};
        let (homeserver, stop) = serve_stoppable(versions_body());
        let store = StoreDir::new("switch-trace-restore");
        let typed = format!("http://{homeserver}");
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));

        let traced = |online: bool| {
            let typed = typed.clone();
            let path = store.path().to_path_buf();
            let events = Arc::clone(&events);
            let out: Arc<Mutex<Option<Value>>> = Arc::new(Mutex::new(None));
            let sink = Arc::clone(&out);
            runtime().block_on(switchtrace::restore_scope(true, async move {
                let client = build_client_for_restore(&typed, &path, &events).await;
                assert!(client.is_ok(), "online={online}: the restore must build");
                drop(client);
                *sink.lock().unwrap() =
                    Some(switchtrace::with_restore_trace(json!({ "type": "login_ok" })));
            }));
            let event = out.lock().unwrap().take().expect("an event");
            event["trace"].clone()
        };

        // Online: discovery succeeds, one store open, no fallback.
        let trace = traced(true);
        assert_eq!(trace["store_opens"], 1, "{trace}");
        assert_eq!(trace["offline_fallback"], false, "{trace}");
        assert_eq!(trace["fallback_build_ms"], 0, "{trace}");

        // Offline: the discovering attempt and the recorded-URL fallback.
        stop_serving(&homeserver, &stop);
        let trace = traced(false);
        assert_eq!(trace["store_opens"], 2, "{trace}");
        assert_eq!(trace["offline_fallback"], true, "{trace}");
    }

    // A recorded URL must never skip discovery while the server is up, or a
    // changed `/.well-known` delegation would never be followed.
    #[test]
    fn a_reachable_homeserver_is_still_rediscovered_on_every_restore() {
        let store = StoreDir::new("rediscover");
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));

        // Where the account lives today.
        let first = serve(versions_body());
        let delegating_first = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{first}"}}}}"#
        ));
        // The admin moves the client API somewhere else.
        let second = serve(versions_body());
        let delegating_second = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{second}"}}}}"#
        ));

        runtime().block_on(async {
            let client = build_client_for_restore(
                &format!("http://{delegating_first}"), store.path(), &events)
                .await
                .expect("build against the first homeserver");
            assert_eq!(
                client.homeserver().port(),
                first.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()));
            drop(client);

            // A stale record is now on disk. Discovery must still win.
            let client = build_client_for_restore(
                &format!("http://{delegating_second}"), store.path(), &events)
                .await
                .expect("build against the moved homeserver");
            assert_eq!(
                client.homeserver().port(),
                second.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()),
                "the restore used the recorded URL instead of following discovery"
            );
            let said: Vec<String> =
                events.lock().expect("events").iter().cloned().collect();
            assert!(
                !said.iter().any(|e| e.contains("session_restored_offline")),
                "a reachable homeserver was reported as an offline restore: {said:?}"
            );
        });
    }

    #[test]
    fn an_unusable_homeserver_record_is_ignored() {
        let store = StoreDir::new("bad-record");
        for junk in ["", "   ", "not a url", "ftp://example.org", "https://"] {
            std::fs::write(
                store.path().join(super::RESOLVED_HOMESERVER_FILE), junk)
                .expect("write record");
            assert!(
                read_resolved_homeserver(store.path()).is_none(),
                "an unusable recorded homeserver was accepted: {junk:?}"
            );
        }
    }

    #[test]
    fn a_host_that_is_no_homeserver_fails_to_build_instead_of_404ing_later() {
        // No well-known and no versions endpoint: the build must fail here with a
        // real error, rather than a 404 at login.
        let bogus = serve("irrelevant".to_owned());
        let result = runtime()
            .block_on(build_client(&format!("http://{bogus}/nope"), &PathBuf::new()));
        assert!(result.is_err(), "a non-homeserver must not build a client");
    }
}

// ── Classic-sync fault classification, backoff and watchdog ──────────────
//
// A dropped connection must not stop sync for the session; pure functions
// make that testable without a real outage. The first-response watchdog
// (issue #2: a silent wedge of 13+ minutes) is instrumentation, driven here
// with millisecond steps.
#[cfg(test)]
mod classic_sync_fault_tests {
    use super::{
        classic_sync_backoff, classify_classic_sync_error, ClassicSyncFault,
        CLASSIC_SYNC_REPORT_AFTER,
    };
    use matrix_sdk::ruma::api::error::{ErrorKind, UnknownTokenErrorData};

    #[test]
    fn a_dropped_connection_carries_no_errcode_and_is_transient() {
        // A request with no reply has no errcode, and matrix-sdk does not retry
        // network failures without a `retry_limit`.
        assert_eq!(classify_classic_sync_error(None), ClassicSyncFault::Transient);
    }

    #[test]
    fn a_server_side_failure_is_transient() {
        for kind in [
            ErrorKind::Unknown,
            ErrorKind::NotFound,
            ErrorKind::Unrecognized,
            ErrorKind::MissingParam,
        ] {
            assert_eq!(
                classify_classic_sync_error(Some(&kind)),
                ClassicSyncFault::Transient,
                "{kind:?} ended the sync loop"
            );
        }
    }

    #[test]
    fn only_a_dead_session_is_fatal() {
        // The same set the modern lane treats as an authentication error.
        assert_eq!(
            classify_classic_sync_error(Some(&ErrorKind::Forbidden)),
            ClassicSyncFault::Fatal
        );
        let mut revoked = UnknownTokenErrorData::new();
        assert_eq!(
            classify_classic_sync_error(Some(&ErrorKind::UnknownToken(revoked.clone()))),
            ClassicSyncFault::Fatal
        );
        // A soft logout is still a session this client cannot use.
        revoked.soft_logout = true;
        assert_eq!(
            classify_classic_sync_error(Some(&ErrorKind::UnknownToken(revoked))),
            ClassicSyncFault::Fatal
        );
    }

    #[test]
    fn the_backoff_grows_and_then_stops_growing() {
        assert_eq!(classic_sync_backoff(0).as_secs(), 1);
        assert_eq!(classic_sync_backoff(1).as_secs(), 2);
        assert_eq!(classic_sync_backoff(4).as_secs(), 16);
        // Capped, and never grows without bound.
        for failures in [5u32, 6, 50, u32::MAX] {
            assert_eq!(classic_sync_backoff(failures).as_secs(), 30,
                       "backoff at {failures} failures");
        }
    }

    #[test]
    fn the_silence_is_escalated_before_the_backoff_is_at_its_ceiling() {
        // Report offline before the retries slow to their slowest.
        assert!(classic_sync_backoff(CLASSIC_SYNC_REPORT_AFTER - 1).as_secs() <= 30);
        assert!(CLASSIC_SYNC_REPORT_AFTER >= 2,
                "one flaky request must not raise an error banner");
    }
}

/// Classic sync settings must not carry `full_state`: settings apply to
/// every request, so it must not be set at all. `SyncSettings` has no
/// getters, so this asserts on its `Debug` output.
#[cfg(test)]
mod classic_sync_settings_tests {
    use std::time::Duration;

    use matrix_sdk::config::SyncSettings;

    use super::{
        classic_sync_request_timeout, CLASSIC_SYNC_TIMEOUT, CLASSIC_SYNC_TIMEOUT_VERIFYING,
    };

    /// The exact expression `run_classic_sync` builds for each request.
    fn classic_settings() -> SyncSettings {
        super::classic_sync_settings(CLASSIC_SYNC_TIMEOUT)
    }

    #[test]
    fn the_first_request_says_zero_and_never_omits_the_timeout() {
        // Omitted is NOT zero on every server (Beeper long-polls it), so the
        // first request must carry an explicit zero, verifying or not.
        assert_eq!(classic_sync_request_timeout(true, false), Duration::ZERO);
        assert_eq!(classic_sync_request_timeout(true, true), Duration::ZERO);
        // `SyncSettings`' Debug omits a `None` timeout, so this is the
        // explicit zero being present, not merely a zero-ish default.
        let printed = format!("{:?}", super::classic_sync_settings(Duration::ZERO));
        assert!(printed.contains("timeout: 0ns"), "{printed}");
        assert!(printed.contains("ignore_timeout_on_first_sync: false"), "{printed}");
    }

    #[test]
    fn a_live_verification_shortens_the_poll_and_nothing_else_does() {
        assert_eq!(classic_sync_request_timeout(false, false), CLASSIC_SYNC_TIMEOUT);
        assert_eq!(classic_sync_request_timeout(false, true), CLASSIC_SYNC_TIMEOUT_VERIFYING);
        assert!(CLASSIC_SYNC_TIMEOUT_VERIFYING < CLASSIC_SYNC_TIMEOUT);
    }

    #[test]
    fn the_classic_settings_do_not_ask_for_full_state() {
        let printed = format!("{:?}", classic_settings());
        assert!(printed.contains("full_state: false"),
                "classic sync still asks the server to serialise the complete                  state of every joined room on every incremental sync: {printed}");
        // The one value it sets, so this cannot pass on settings that lost
        // everything.
        assert!(printed.contains("timeout: 30s"), "{printed}");
    }

    #[test]
    fn full_state_would_be_visible_if_it_were_set() {
        // Proves the assertion above can fail.
        let printed = format!("{:?}", classic_settings().full_state(true));
        assert!(printed.contains("full_state: true"), "{printed}");
    }
}

#[cfg(test)]
mod sas_tick_budget_tests {
    use super::{SasTickBudget, SAS_COMPARE_TICKS, SAS_COMPLETION_TICKS};

    fn run(emojis_at: Option<u32>) -> u32 {
        let mut budget = SasTickBudget::new();
        let mut spent = 0;
        while budget.tick() {
            spent += 1;
            if Some(spent) == emojis_at {
                budget.emojis_shown();
            }
            // A re-derived SAS reports KeysExchanged again; it must not
            // extend the compare budget.
            if emojis_at.is_some_and(|at| spent == at + 10) {
                budget.emojis_shown();
            }
        }
        spent
    }

    #[test]
    fn the_machine_half_keeps_its_own_bound() {
        assert_eq!(run(None), SAS_COMPLETION_TICKS);
    }

    #[test]
    fn the_compare_half_is_counted_from_the_emojis_not_from_sas_start() {
        // Emojis late in the machine half (30 s hops): the whole compare
        // budget is still there. Counted from SAS start, this left ~40 s.
        let late = SAS_COMPLETION_TICKS - 1;
        assert_eq!(run(Some(late)), late + SAS_COMPARE_TICKS);
        assert_eq!(run(Some(1)), 1 + SAS_COMPARE_TICKS);
    }
}

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
        // Never flipped: no response ever arrives.
        let first = Arc::new(AtomicBool::new(true));
        watch_first_sync_response(&events, &first, &steps()).await;

        let seen = drain(&events.lock().unwrap());
        assert_eq!(seen.len(), 3, "expected one report per step");
        for value in &seen {
            assert_eq!(value["type"], "sync_stalled");
            assert_eq!(value["phase"], "first_response");
        }
        // Each report states the cumulative wait.
        assert!(seen[0]["waited_secs"].is_number());
    }

    #[tokio::test]
    async fn a_response_ends_the_watch_and_reports_nothing() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        // Already answered: an ordinary sync stays silent.
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
        // The response lands between the first and second step, separating "report
        // once" from "report forever". Keyed on the first report rather than a
        // sleep, which flaked under load.
        let seen_by = Arc::clone(&events);
        tokio::spawn(async move {
            while seen_by.lock().unwrap().is_empty() {
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
            flip.store(false, Ordering::SeqCst);
        });
        let wide = vec![Duration::from_millis(10), Duration::from_millis(100),
                        Duration::from_millis(200)];
        watch_first_sync_response(&events, &first, &wide).await;

        let seen = drain(&events.lock().unwrap());
        assert_eq!(seen.len(), 1,
                   "the watch kept reporting after the sync answered");
    }
}

/// The classic loop's silence supervisor (issue #2). matrix-sdk retries a
/// 5xx/429 answer inside the request for up to 15 minutes without reaching
/// the sync callback, so "no callback at all" is what a wedge looks like.
#[cfg(test)]
mod classic_sync_silence_tests {
    use super::{supervise_classic_silence, ClassicSilenceBudget, SyncProgress};
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    fn budget(report_ms: u64, restart_ms: u64) -> ClassicSilenceBudget {
        ClassicSilenceBudget {
            report_after: Duration::from_millis(report_ms),
            restart_after: Duration::from_millis(restart_ms),
            // Far above any tick gap these tests produce.
            resume_gap: Duration::from_secs(3600),
        }
    }

    #[tokio::test]
    async fn time_spent_suspended_is_not_counted_as_silence() {
        // A resume cannot be staged in a test, so the gap is shrunk instead:
        // every tick (at least 60 ms apart) now looks like a wake from suspend.
        // On Windows `Instant` counts through sleep, and without this every
        // resume after a long sleep raised a false sync error.
        let events = Arc::new(Mutex::new(VecDeque::new()));
        let progress = SyncProgress::new();
        progress.touch();
        let resumed = ClassicSilenceBudget {
            resume_gap: Duration::from_millis(20),
            ..budget(60, 200)
        };
        let outcome = tokio::time::timeout(
            Duration::from_millis(800),
            supervise_classic_silence(&events, &progress, resumed),
        )
        .await;
        assert!(outcome.is_err(), "a resumed loop was restarted as wedged");
        assert!(drain(&events).is_empty(), "a resumed loop was reported as stalled");
    }

    fn drain(events: &Arc<Mutex<VecDeque<String>>>) -> Vec<serde_json::Value> {
        events.lock().unwrap().iter().map(|e| serde_json::from_str(e).unwrap()).collect()
    }

    #[tokio::test]
    async fn a_silent_loop_is_reported_once_and_then_given_up_on() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        let progress = SyncProgress::new();
        progress.touch();
        let started = tokio::time::Instant::now();
        tokio::time::timeout(
            Duration::from_secs(10),
            supervise_classic_silence(&events, &progress, budget(60, 200)),
        )
        .await
        .expect("a loop that never calls back must be given up on");
        // 190, not 200: the clock stores whole milliseconds, so the measured
        // quiet can run up to one ahead of this instant.
        assert!(started.elapsed() >= Duration::from_millis(190),
                "given up on before the restart bound");

        let seen = drain(&events);
        // Exactly one report for one quiet spell: the UI leaves "Loading rooms…"
        // (offline) and the log says why.
        assert_eq!(seen.len(), 2, "{seen:?}");
        assert_eq!(seen[0]["type"], "room_list_sync_state");
        assert_eq!(seen[0]["state"], "offline");
        assert_eq!(seen[1]["type"], "sync_stalled");
        assert_eq!(seen[1]["phase"], "no_progress");
    }

    #[tokio::test]
    async fn a_loop_that_keeps_calling_back_is_never_restarted() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
        let progress = SyncProgress::new();
        progress.touch();
        let toucher = progress.clone();
        let heartbeat = tokio::spawn(async move {
            loop {
                toucher.touch();
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        });
        // Twice the restart bound, with a callback every 10 ms throughout.
        let outcome = tokio::time::timeout(
            Duration::from_millis(1200),
            supervise_classic_silence(&events, &progress, budget(300, 600)),
        )
        .await;
        heartbeat.abort();
        assert!(outcome.is_err(), "a healthy loop was restarted");
        assert!(drain(&events).is_empty(), "a healthy loop was reported as stalled");
    }
}

/// Issue #2 end to end, against a loopback homeserver that answers every
/// request but `/versions` with 503. matrix-sdk treats that as transient and
/// retries it inside the request, so the sync callback never runs: before the
/// supervisor this loop said "starting" and nothing else for 15 minutes.
#[cfg(test)]
mod classic_sync_wedge_tests {
    use super::{
        run_classic_sync_with, ClassicSilenceBudget, VerificationLive,
        CLASSIC_SYNC_WEDGED_MESSAGE,
    };
    use std::sync::atomic::AtomicBool;
    use std::collections::VecDeque;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };

    /// Reads one request (head and body) and returns its request line.
    fn read_request(stream: &mut TcpStream) -> String {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        let head_end = loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                return String::new();
            }
            buf.extend_from_slice(&chunk[..n]);
            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                break pos + 4;
            }
        };
        let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
        let length = head
            .lines()
            .find_map(|line| {
                line.to_ascii_lowercase()
                    .strip_prefix("content-length:")
                    .map(|v| v.trim().parse::<usize>().unwrap_or(0))
            })
            .unwrap_or(0);
        while buf.len() < head_end + length {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        head.lines().next().unwrap_or("").to_owned()
    }

    fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    /// A homeserver behind a gateway that fails every API call with 503.
    /// Counts the failed requests, so the test can prove the SDK really was
    /// sending (and silently retrying) them.
    fn serve_503(failed: Arc<AtomicUsize>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let failed = Arc::clone(&failed);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else {
                        failed.fetch_add(1, Ordering::SeqCst);
                        respond(&mut stream, "503 Service Unavailable",
                                r#"{"errcode":"M_UNKNOWN","error":"gateway"}"#);
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    fn drain(events: &Arc<Mutex<VecDeque<String>>>) -> Vec<serde_json::Value> {
        events.lock().unwrap().iter().map(|e| serde_json::from_str(e).unwrap()).collect()
    }

    fn reported_wedge(events: &Arc<Mutex<VecDeque<String>>>) -> bool {
        drain(events).iter().any(|value| {
            value["type"] == "sync_error" && value["message"] == CLASSIC_SYNC_WEDGED_MESSAGE
        })
    }

    fn never_verifying() -> VerificationLive {
        Arc::new(|| false)
    }

    /// A homeserver that answers `/sync` AT ONCE whatever its `timeout`, and
    /// records the `timeout` each request carried ("absent" when there was
    /// none). Everything else but `/versions` gets a 404, which the SDK does
    /// not retry, so the E2EE requests `sync_once` sends first fail fast.
    fn serve_recording_sync(seen: Arc<Mutex<Vec<String>>>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let target = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    let (path, query) = target.split_once('?').unwrap_or((&target, ""));
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else if path == "/_matrix/client/v3/sync" {
                        let timeout = query
                            .split('&')
                            .find_map(|pair| pair.strip_prefix("timeout="))
                            .unwrap_or("absent")
                            .to_owned();
                        let batch = {
                            let mut seen = seen.lock().unwrap();
                            seen.push(timeout);
                            seen.len()
                        };
                        respond(&mut stream, "200 OK",
                                &format!(r#"{{"next_batch":"s{batch}"}}"#));
                    } else {
                        respond(&mut stream, "404 Not Found",
                                r#"{"errcode":"M_UNRECOGNIZED","error":"test"}"#);
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    /// A homeserver whose first two `/sync` answers each carry one plain
    /// `m.text` in a joined room, neither `limited` and both with a
    /// `prev_batch` (a catch-up after a restart, then the next live message),
    /// and every later answer nothing. Counts the syncs.
    fn serve_message_syncs(syncs: Arc<AtomicUsize>, room_id: &'static str,
                           event_ids: [&'static str; 2]) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let syncs = Arc::clone(&syncs);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let target = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    let path = target.split_once('?').map_or(target.as_str(), |(p, _)| p);
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else if path == "/_matrix/client/v3/sync" {
                        let n = syncs.fetch_add(1, Ordering::SeqCst) + 1;
                        let body = if n <= 2 {
                            serde_json::json!({
                                "next_batch": format!("s{n}"),
                                "rooms": { "join": { room_id: {
                                    "timeline": {
                                        "limited": false,
                                        "prev_batch": format!("p{}", n - 1),
                                        "events": [{
                                            "type": "m.room.message",
                                            "event_id": event_ids[n - 1],
                                            "sender": "@flo:example.org",
                                            "origin_server_ts": 1_760_000_000_000u64 + n as u64,
                                            "content": { "msgtype": "m.text", "body": "x" },
                                        }],
                                    },
                                }}},
                            })
                        } else {
                            serde_json::json!({ "next_batch": format!("s{n}") })
                        };
                        respond(&mut stream, "200 OK", &body.to_string());
                    } else {
                        respond(&mut stream, "404 Not Found",
                                r#"{"errcode":"M_UNRECOGNIZED","error":"test"}"#);
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    /// GitHub #30/#48. A message delivered by the classic loop before any room
    /// was opened must be in the room's event cache: that is where every
    /// timeline is built from, and a response the cache never saw leaves a gap
    /// nothing fills (see `subscribe_event_cache_before_classic_sync`).
    ///
    /// Before the fix nothing subscribed the event cache on this path until the
    /// first timeline build, so the cache below (subscribed only AFTER the loop,
    /// as a room open would) held nothing.
    ///
    /// The second response is the one that made the hole permanent in the
    /// field: not `limited`, so the cache drops its `prev_batch` once the room
    /// holds events, and nothing ever paginates back past it. It must land
    /// directly after the first. (The permanent case also needs history cached
    /// in an EARLIER session, which one in-memory client cannot stage.)
    #[test]
    fn a_classic_response_before_any_room_opens_reaches_the_event_cache() {
        const ROOM: &str = "!room:example.org";
        const EVENT: &str = "$long-message:example.org";
        const NEXT: &str = "$next-message:example.org";
        let syncs = Arc::new(AtomicUsize::new(0));
        let addr = serve_message_syncs(Arc::clone(&syncs), ROOM, [EVENT, NEXT]);
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let found = runtime.block_on(async {
            let client = restored_client(&addr).await;
            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(&syncs);
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
                while tokio::time::Instant::now() < deadline
                    && watched.load(Ordering::SeqCst) < 4
                {
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                let _ = cancel_tx.send(());
            };
            let silence = ClassicSilenceBudget {
                report_after: Duration::from_secs(60),
                restart_after: Duration::from_secs(120),
                resume_gap: Duration::from_secs(3600),
            };
            tokio::join!(
                run_classic_sync_with(
                    client.clone(), Arc::clone(&events), never_verifying(),
                    Default::default(), cancel_rx, silence),
                watcher
            );

            // What a room open does first (TimelineBuilder::build).
            client.event_cache().subscribe().expect("subscribe");
            let room_id = matrix_sdk::ruma::RoomId::parse(ROOM).expect("room id");
            let room = client.get_room(&room_id).expect("the sync created the room");
            let (cache, _handles) = room.event_cache().await.expect("room event cache");
            // The cache stores a response on its own task; give it time.
            let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
            loop {
                let ids: Vec<String> = cache
                    .events()
                    .await
                    .unwrap_or_default()
                    .iter()
                    .filter_map(|e| e.event_id().map(|id| id.to_string()))
                    .collect();
                if ids.iter().any(|id| id == NEXT) || tokio::time::Instant::now() > deadline {
                    break ids;
                }
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
        });

        assert!(syncs.load(Ordering::SeqCst) >= 2,
                "the loop never got past the first sync; this test proved nothing: {:?}",
                drain(&events));
        assert!(found.iter().any(|id| id == EVENT),
                "the message the first classic sync delivered never reached the event \
                 cache, so no timeline can show it: cached={found:?}");
        assert_eq!(found, vec![EVENT.to_owned(), NEXT.to_owned()],
                   "the next non-limited response must continue the history the first \
                    one left, with nothing between them");
    }

    async fn restored_client(addr: &str) -> matrix_sdk::Client {
        let client = matrix_sdk::Client::builder()
            .homeserver_url(format!("http://{addr}"))
            .build()
            .await
            .expect("client");
        let session = MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                device_id: OwnedDeviceId::from("TESTDEVICE"),
            },
            tokens: SessionTokens {
                access_token: "test-token".to_owned(),
                refresh_token: None,
            },
        };
        client
            .matrix_auth()
            .restore_session(session, RoomLoadSettings::default())
            .await
            .expect("restore");
        client
    }

    /// The two classic-sync cadence fixes, end to end through the real loop
    /// and a real HTTP request: the first request carries an explicit
    /// `timeout=0` (an omitted timeout is a 30 s long-poll on Beeper), and a
    /// verification going live shortens the very next poll to 3 s.
    #[test]
    fn the_first_poll_says_zero_and_a_live_verification_shortens_the_next() {
        let seen = Arc::new(Mutex::new(Vec::<String>::new()));
        let addr = serve_recording_sync(Arc::clone(&seen));
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let verifying = Arc::new(AtomicBool::new(false));
        let live: VerificationLive = {
            let verifying = Arc::clone(&verifying);
            Arc::new(move || verifying.load(Ordering::SeqCst))
        };
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            let client = restored_client(&addr).await;
            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(&seen);
            let flag = Arc::clone(&verifying);
            // Flip "verifying" once two polls have gone out (the loop then
            // sleeps ~1 s before the next, so the flip lands before it), and
            // stop after four, or at a bound.
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
                while tokio::time::Instant::now() < deadline {
                    let count = watched.lock().unwrap().len();
                    if count >= 2 {
                        flag.store(true, Ordering::SeqCst);
                    }
                    if count >= 4 {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                let _ = cancel_tx.send(());
            };
            let silence = ClassicSilenceBudget {
                report_after: Duration::from_secs(60),
                restart_after: Duration::from_secs(120),
                resume_gap: Duration::from_secs(3600),
            };
            tokio::join!(
                run_classic_sync_with(
                    client, Arc::clone(&events), live, Default::default(), cancel_rx, silence),
                watcher
            );
        });

        let seen = seen.lock().unwrap().clone();
        assert!(seen.len() >= 4, "the loop did not keep polling: {seen:?} {:?}", drain(&events));
        assert_eq!(seen[0], "0", "the first request must say timeout=0 explicitly: {seen:?}");
        assert_eq!(seen[1], "30000", "an idle poll keeps the SDK's 30 s: {seen:?}");
        assert_eq!(seen[2], "3000", "a live verification did not shorten the poll: {seen:?}");
        assert_eq!(seen[3], "3000", "{seen:?}");
        // The responses really were processed, so this is the loop the UI sees.
        assert!(drain(&events).iter().any(|v| v["state"] == "running"));
    }

    #[test]
    fn a_gateway_failing_every_sync_ends_in_a_reported_restart_not_silence() {
        let failed = Arc::new(AtomicUsize::new(0));
        let addr = serve_503(Arc::clone(&failed));
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            let session = MatrixSession {
                meta: SessionMeta {
                    user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                    device_id: OwnedDeviceId::from("TESTDEVICE"),
                },
                tokens: SessionTokens {
                    access_token: "test-token".to_owned(),
                    refresh_token: None,
                },
            };
            client
                .matrix_auth()
                .restore_session(session, RoomLoadSettings::default())
                .await
                .expect("restore");

            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(&events);
            // Stops the loop once the restart was reported, or after a bound far
            // below the SDK's own 15-minute retry window.
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
                while tokio::time::Instant::now() < deadline && !reported_wedge(&watched) {
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                let _ = cancel_tx.send(());
            };
            let silence = ClassicSilenceBudget {
                report_after: Duration::from_millis(300),
                restart_after: Duration::from_millis(900),
                resume_gap: Duration::from_secs(3600),
            };
            tokio::join!(
                run_classic_sync_with(
                    client, Arc::clone(&events), never_verifying(), Default::default(),
                    cancel_rx, silence),
                watcher
            );
        });

        let seen = drain(&events);
        assert!(failed.load(Ordering::SeqCst) > 0,
                "the server never saw a request; this test proved nothing: {seen:?}");
        // The wedge itself: no callback ever ran, so no response ("running")
        // reached the UI.
        assert!(!seen.iter().any(|v| v["state"] == "running"), "{seen:?}");
        // What issue #2 asked for: the silence is reported, then surfaced as an
        // error the UI shows, instead of "starting" for ever.
        assert!(seen.iter().any(|v| v["type"] == "sync_stalled"
                                    && v["phase"] == "no_progress"), "{seen:?}");
        assert!(seen.iter().any(|v| v["type"] == "room_list_sync_state"
                                    && v["state"] == "offline"), "{seen:?}");
        assert!(reported_wedge(&events), "no sync_error for a wedged loop: {seen:?}");
    }
}

/// Which raw sync events count as somebody saying something for room
/// ordering, and which must never move a room.
#[cfg(test)]
mod conversation_recency_tests {
    use matrix_sdk::ruma::events::AnySyncTimelineEvent;
    use matrix_sdk::ruma::serde::Raw;
    use matrix_sdk::ruma::OwnedEventId;
    use serde_json::json;

    use super::conversation_timestamp_ms;

    fn raw(value: serde_json::Value) -> Raw<AnySyncTimelineEvent> {
        Raw::from_json_string(value.to_string()).expect("valid raw event")
    }

    // An edit, plain or still encrypted (m.relates_to is outside the
    // ciphertext), is not something new said and must not raise a room. A
    // reply still is.
    #[test]
    fn an_edit_is_not_a_conversation() {
        let relates = json!({ "rel_type": "m.replace", "event_id": "$original" });
        let plain_edit = raw(json!({
            "type": "m.room.message",
            "event_id": "$edit",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_000u64,
            "content": {
                "msgtype": "m.text",
                "body": "* new words",
                "m.new_content": { "msgtype": "m.text", "body": "new words" },
                "m.relates_to": relates.clone(),
            },
        }));
        assert_eq!(conversation_timestamp_ms(&plain_edit), None);
        let encrypted_edit = raw(json!({
            "type": "m.room.encrypted",
            "event_id": "$edit2",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_000u64,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "AwgAEnAC",
                "sender_key": "k",
                "device_id": "D",
                "session_id": "s",
                "m.relates_to": relates,
            },
        }));
        assert_eq!(conversation_timestamp_ms(&encrypted_edit), None);
        let reply = raw(json!({
            "type": "m.room.message",
            "event_id": "$reply",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_003u64,
            "content": {
                "msgtype": "m.text",
                "body": "a reply",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$original" } },
            },
        }));
        assert_eq!(conversation_timestamp_ms(&reply), Some(1_700_000_000_003));
    }

    #[test]
    fn a_plain_message_is_a_conversation() {
        let event = raw(json!({
            "type": "m.room.message",
            "event_id": "$one",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_000u64,
            "content": { "msgtype": "m.text", "body": "hello" },
        }));
        assert_eq!(conversation_timestamp_ms(&event), Some(1_700_000_000_000));
    }

    // Msgtype-blind: an image must raise activity like text does.
    #[test]
    fn an_image_counts_exactly_like_text() {
        let event = raw(json!({
            "type": "m.room.message",
            "event_id": "$two",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_001u64,
            "content": { "msgtype": "m.image", "body": "cat.png" },
        }));
        assert_eq!(conversation_timestamp_ms(&event), Some(1_700_000_000_001));
    }

    // An undecrypted event is unsuitable as a preview but still moves the room.
    #[test]
    fn an_encrypted_event_counts_even_though_it_cannot_be_previewed() {
        let event = raw(json!({
            "type": "m.room.encrypted",
            "event_id": "$three",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_002u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2" },
        }));
        assert_eq!(conversation_timestamp_ms(&event), Some(1_700_000_000_002));
    }

    // A member joining must not raise a silent room.
    #[test]
    fn a_membership_change_is_not_a_conversation() {
        let event = raw(json!({
            "type": "m.room.member",
            "event_id": "$four",
            "sender": "@a:example.org",
            "state_key": "@a:example.org",
            "origin_server_ts": 1_700_000_000_003u64,
            "content": { "membership": "join" },
        }));
        assert_eq!(conversation_timestamp_ms(&event), None);
    }

    // MatrixRTC membership churn (one per participant per minute) is filtered
    // from timelines and must not return through the ordering stamp.
    #[test]
    fn matrixrtc_membership_churn_is_not_a_conversation() {
        for kind in ["m.call.member", "org.matrix.msc3401.call.member"] {
            let event = raw(json!({
                "type": kind,
                "event_id": "$five",
                "sender": "@a:example.org",
                "state_key": "@a:example.org_DEVICE",
                "origin_server_ts": 1_700_000_000_004u64,
                "content": { "memberships": [] },
            }));
            assert_eq!(conversation_timestamp_ms(&event), None, "{kind}");
        }
    }

    // Reactions and redactions are not something said.
    #[test]
    fn reactions_and_redactions_are_not_conversations() {
        for kind in ["m.reaction", "m.room.redaction"] {
            let event = raw(json!({
                "type": kind,
                "event_id": "$six",
                "sender": "@a:example.org",
                "origin_server_ts": 1_700_000_000_005u64,
                "content": {},
            }));
            assert_eq!(conversation_timestamp_ms(&event), None, "{kind}");
        }
    }

    // Call rows do not raise activity on the C++ side
    // (TimelineEvent::CallEvent); both producers must agree.
    #[test]
    fn a_call_row_does_not_raise_activity_here_either() {
        for kind in ["m.call.invite", "m.rtc.notification"] {
            let event = raw(json!({
                "type": kind,
                "event_id": "$seven",
                "sender": "@a:example.org",
                "origin_server_ts": 1_700_000_000_006u64,
                "content": {},
            }));
            assert_eq!(conversation_timestamp_ms(&event), None, "{kind}");
        }
    }

    // Break-it B084: an undecrypted reaction or poll vote is not a message
    // (decrypted, neither counts), but an undecrypted message or thread reply is.
    #[test]
    fn an_undecrypted_reaction_or_reference_is_not_a_conversation() {
        for rel_type in ["m.annotation", "m.reference"] {
            let event = raw(json!({
                "type": "m.room.encrypted",
                "event_id": "$enc-rel",
                "sender": "@a:example.org",
                "origin_server_ts": 1_700_000_000_008u64,
                "content": {
                    "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x",
                    "m.relates_to": { "rel_type": rel_type, "event_id": "$target" },
                },
            }));
            assert_eq!(conversation_timestamp_ms(&event), None, "{rel_type}");
        }
        let thread_reply = raw(json!({
            "type": "m.room.encrypted",
            "event_id": "$enc-thread",
            "sender": "@a:example.org",
            "origin_server_ts": 1_700_000_000_009u64,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x",
                "m.relates_to": { "rel_type": "m.thread", "event_id": "$root" },
            },
        }));
        assert_eq!(conversation_timestamp_ms(&thread_reply), Some(1_700_000_000_009));
    }

    // Break-it B084: the harvest says whether the newest conversation event is
    // still undecryptable, and which event it is, so the room's preview can
    // follow its stamp until that event decrypts.
    #[test]
    fn the_harvest_flags_a_newest_event_that_is_undecryptable() {
        use super::newest_conversation;
        use matrix_sdk::deserialized_responses::TimelineEvent;
        let ev = |value| TimelineEvent::from_plaintext(raw(value));
        let message = ev(json!({
            "type": "m.room.message", "event_id": "$m", "sender": "@a:example.org",
            "origin_server_ts": 100u64, "content": { "msgtype": "m.text", "body": "hello" },
        }));
        let encrypted = ev(json!({
            "type": "m.room.encrypted", "event_id": "$e", "sender": "@a:example.org",
            "origin_server_ts": 200u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x" },
        }));
        let reaction = ev(json!({
            "type": "m.room.encrypted", "event_id": "$r", "sender": "@a:example.org",
            "origin_server_ts": 300u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x",
                         "m.relates_to": { "rel_type": "m.annotation", "event_id": "$m" } },
        }));
        let e = Some(OwnedEventId::try_from("$e").unwrap());
        let m = Some(OwnedEventId::try_from("$m").unwrap());
        assert_eq!(newest_conversation([&message, &encrypted]), Some((200, true, e.clone())));
        assert_eq!(newest_conversation([&encrypted, &message]), Some((200, true, e)));
        assert_eq!(newest_conversation([&message]), Some((100, false, m.clone())));
        // A later undecrypted reaction neither moves the stamp nor flags it.
        assert_eq!(newest_conversation([&message, &reaction]), Some((100, false, m)));
        assert_eq!(newest_conversation([&reaction]), None);
    }

    // Break-it B084 review: the harvest stamps an event on the clock the SDK's
    // latest event uses. A sender whose clock runs ahead of ours is clamped to
    // ours when the SDK builds the event; the harvest must read that, not the
    // raw `origin_server_ts`, or the two disagree about the same event.
    #[test]
    fn the_harvest_stamp_is_the_clamped_one_the_latest_event_carries() {
        use super::{event_ordering_stamp_ms, newest_conversation};
        use matrix_sdk::deserialized_responses::TimelineEvent;
        use matrix_sdk::ruma::MilliSecondsSinceUnixEpoch;
        let ahead = raw(json!({
            "type": "m.room.encrypted", "event_id": "$ahead", "sender": "@a:example.org",
            "origin_server_ts": 5_000u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x" },
        }));
        let our_now = MilliSecondsSinceUnixEpoch(4_000u32.into());
        let event = TimelineEvent::from_plaintext_with_max_timestamp(ahead, our_now);
        let sdk_stamp = event.timestamp().map(|ts| u64::from(ts.get()));
        assert_eq!(sdk_stamp, Some(4_000), "fixture: the SDK clamps");
        assert_eq!(event_ordering_stamp_ms(&event), sdk_stamp);
        assert_eq!(
            newest_conversation([&event]),
            Some((4_000, true, Some(OwnedEventId::try_from("$ahead").unwrap())))
        );
    }

    // A state event wearing a message's type name is still a state event.
    #[test]
    fn a_state_key_refuses_the_event_whatever_its_type_says() {
        let event = raw(json!({
            "type": "m.room.message",
            "event_id": "$eight",
            "sender": "@a:example.org",
            "state_key": "",
            "origin_server_ts": 1_700_000_000_007u64,
            "content": { "msgtype": "m.text", "body": "hello" },
        }));
        assert_eq!(conversation_timestamp_ms(&event), None);
    }

    // 0 crosses the FFI as an invalid QDateTime that raiseActivity ignores, so
    // "no timestamp" and "the epoch" must not differ.
    #[test]
    fn a_missing_or_zero_timestamp_is_no_answer_at_all() {
        let missing = raw(json!({
            "type": "m.room.message",
            "event_id": "$nine",
            "sender": "@a:example.org",
            "content": { "msgtype": "m.text", "body": "hello" },
        }));
        assert_eq!(conversation_timestamp_ms(&missing), None);
        let zero = raw(json!({
            "type": "m.room.message",
            "event_id": "$ten",
            "sender": "@a:example.org",
            "origin_server_ts": 0u64,
            "content": { "msgtype": "m.text", "body": "hello" },
        }));
        assert_eq!(conversation_timestamp_ms(&zero), None);
    }
}

/// The msgtype-to-row-kind mapping and its filename rule. Media rows must
/// map, so media sent to a room with no open timeline still notifies.
#[cfg(test)]
mod message_row_kind_tests {
    use super::{media_filename_for_kind, typed_message_row_kind};

    #[test]
    fn text_like_msgtypes_keep_their_kinds() {
        for (wire, kind) in
            [("m.text", "text"), ("m.notice", "notice"), ("m.emote", "emote")]
        {
            assert_eq!(typed_message_row_kind(wire), Some(kind), "{wire}");
        }
    }

    // Each of these used to be dropped.
    #[test]
    fn every_media_msgtype_has_a_row_of_its_own() {
        for (wire, kind) in [
            ("m.image", "image"),
            ("m.video", "video"),
            ("m.audio", "audio"),
            ("m.file", "file"),
            ("m.location", "location"),
        ] {
            assert_eq!(typed_message_row_kind(wire), Some(kind), "{wire}");
        }
    }

    // A verification request is not a message; `None` leaves the fallback to
    // each caller.
    #[test]
    fn a_msgtype_with_no_row_answers_none() {
        for wire in [
            "m.key.verification.request",
            "m.server_notice",
            "com.example.custom",
            "",
        ] {
            assert_eq!(typed_message_row_kind(wire), None, "{wire}");
        }
    }

    // Element puts the caption in the body and the name in `filename`
    // (MSC2530); reading the body alone would rename the attachment.
    #[test]
    fn a_file_prefers_its_explicit_filename_over_the_body() {
        assert_eq!(
            media_filename_for_kind("file", "here you go", Some("report.pdf")),
            "report.pdf"
        );
        assert_eq!(
            media_filename_for_kind("file", "report.pdf", None),
            "report.pdf"
        );
    }

    // Sable sends `body: ""` (or a caption) plus `filename`. An empty
    // `filename` falls back to the body.
    #[test]
    fn every_attachment_kind_prefers_its_explicit_filename() {
        for kind in ["image", "video", "audio", "file"] {
            assert_eq!(
                media_filename_for_kind(kind, "", Some("comparison.png")),
                "comparison.png",
                "{kind}"
            );
            assert_eq!(
                media_filename_for_kind(kind, "look at this", Some("cat.png")),
                "cat.png",
                "{kind}"
            );
            assert_eq!(media_filename_for_kind(kind, "cat.png", Some("")), "cat.png");
        }
    }

    #[test]
    fn image_video_and_audio_take_the_body_like_the_live_producer() {
        for kind in ["image", "video", "audio"] {
            assert_eq!(
                media_filename_for_kind(kind, "cat.png", None),
                "cat.png",
                "{kind}"
            );
        }
    }

    // A location has no file; its body is the sender's words and stays the
    // body, which previews, toasts and Activity rows read.
    #[test]
    fn a_location_and_every_text_row_carry_no_filename() {
        for kind in ["location", "text", "notice", "emote"] {
            assert_eq!(
                media_filename_for_kind(kind, "Big Ben, London", None),
                "",
                "{kind}"
            );
        }
    }
}

#[cfg(test)]
mod direct_peer_avatar_tests {
    use super::direct_peer_avatar_payload;
    use matrix_sdk::ruma::events::room::member::SyncRoomMemberEvent;
    use serde_json::json;

    fn member(user: &str, membership: &str, avatar: Option<&str>) -> SyncRoomMemberEvent {
        let mut content = json!({ "membership": membership, "displayname": "Sam" });
        if let Some(avatar) = avatar {
            content["avatar_url"] = json!(avatar);
        }
        serde_json::from_value(json!({
            "type": "m.room.member",
            "state_key": user,
            "sender": user,
            "event_id": "$member:example.org",
            "origin_server_ts": 1,
            "content": content,
        }))
        .expect("member event")
    }

    fn targets() -> Vec<String> {
        vec!["@sam:example.org".to_owned()]
    }

    #[test]
    fn a_peers_join_carries_the_face_it_names() {
        let payload = direct_peer_avatar_payload(
            "!dm:example.org",
            &member("@sam:example.org", "join", Some("mxc://example.org/new")),
            &targets(),
        )
        .expect("a payload");
        assert_eq!(payload["type"], "direct_peer_avatar");
        assert_eq!(payload["room_id"], "!dm:example.org");
        assert_eq!(payload["user_id"], "@sam:example.org");
        assert_eq!(payload["avatar_url"], "mxc://example.org/new");
    }

    // A removed avatar is news too: the row must drop the old face.
    #[test]
    fn a_join_without_an_avatar_says_none() {
        let payload = direct_peer_avatar_payload(
            "!dm:example.org",
            &member("@sam:example.org", "join", None),
            &targets(),
        )
        .expect("a payload");
        assert_eq!(payload["avatar_url"], "");
    }

    #[test]
    fn only_the_direct_targets_own_join_counts() {
        // A leave or a ban carries no profile.
        for membership in ["leave", "ban", "invite"] {
            assert!(direct_peer_avatar_payload(
                "!dm:example.org",
                &member("@sam:example.org", membership, Some("mxc://example.org/x")),
                &targets(),
            )
            .is_none(), "{membership}");
        }
        // Somebody else in the room (our own join included).
        assert!(direct_peer_avatar_payload(
            "!dm:example.org",
            &member("@me:example.org", "join", Some("mxc://example.org/me")),
            &targets(),
        )
        .is_none());
        // Not a DM at all.
        assert!(direct_peer_avatar_payload(
            "!room:example.org",
            &member("@sam:example.org", "join", Some("mxc://example.org/x")),
            &[],
        )
        .is_none());
    }
}

#[cfg(test)]
mod resumed_device_key_tests {
    use super::resumed_device_key_state as state;

    // A resumed device is judged on the server's answer: its own key, none at
    // all (removed and re-created under the same id), another key, or no
    // answer, which proves nothing.
    #[test]
    fn a_resumed_device_is_judged_on_the_servers_answer() {
        assert_eq!(state(Some("AAAA"), Some(Some("AAAA"))), "matches");
        // A server may re-pad what we uploaded.
        assert_eq!(state(Some("AAAA="), Some(Some("AAAA"))), "matches");
        assert_eq!(state(Some("AAAA"), Some(Some("BBBB"))), "different");
        assert_eq!(state(Some("AAAA"), Some(None)), "missing");
        // Every way of not knowing is "unknown", never a verdict.
        assert_eq!(state(Some("AAAA"), None), "unknown");
        assert_eq!(state(None, Some(None)), "unknown");
        assert_eq!(state(Some(""), Some(None)), "unknown");
        assert_eq!(state(Some("AAAA"), Some(Some(""))), "unknown");
    }
}

#[cfg(test)]
mod final_session_tokens_tests {
    use super::{final_session_tokens_json, SessionTokens};

    #[test]
    fn no_session_hands_back_nothing() {
        assert_eq!(final_session_tokens_json(None), "");
        let empty = SessionTokens { access_token: String::new(), refresh_token: None };
        assert_eq!(final_session_tokens_json(Some(empty)), "");
    }

    #[test]
    fn a_session_hands_back_both_tokens() {
        let tokens = SessionTokens {
            access_token: "fixture-a".to_owned(),
            refresh_token: Some("fixture-r".to_owned()),
        };
        let value: serde_json::Value =
            serde_json::from_str(&final_session_tokens_json(Some(tokens))).unwrap();
        assert_eq!(value["access_token"], "fixture-a");
        assert_eq!(value["refresh_token"], "fixture-r");
        // A session that issues no refresh token says so, rather than "".
        let value: serde_json::Value = serde_json::from_str(&final_session_tokens_json(Some(
            SessionTokens { access_token: "fixture-a".to_owned(), refresh_token: None },
        )))
        .unwrap();
        assert!(value["refresh_token"].is_null());
    }
}

// A retired bridge must leave nothing of its account store open (GitHub #2).
#[cfg(all(test, target_os = "linux"))]
mod store_close_tests;

/// The modern lane against a homeserver whose sliding sync answers
/// `M_UNKNOWN_TOKEN` although the session is alive: the SDK refreshes, and the
/// one retry it allows is refused too. matrix-sdk 0.18 returns that retry's 401
/// without broadcasting `SessionChange::UnknownToken` (verified in its source);
/// what refuses the retry on matrix.org is likely MAS revoking the token a
/// concurrent refresh superseded, not yet captured. Seen once on matrix.org on
/// 2026-10-07: room list "failed", "Matrix session is no longer authorized.",
/// while pagination, media and receipts all still worked.
#[cfg(test)]
mod modern_sync_auth_tests {
    use super::{
        run_modern_sync, AuthRetryBudget, ModernAuthVerdict, SyncMode, MODERN_AUTH_RETRY_LIMIT,
    };
    use std::collections::VecDeque;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };

    /// Reads one request; returns its request line and its body.
    pub(super) fn read_request(stream: &mut TcpStream) -> (String, String) {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        let head_end = loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                return (String::new(), String::new());
            }
            buf.extend_from_slice(&chunk[..n]);
            if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
                break pos + 4;
            }
        };
        let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
        let length = head
            .lines()
            .find_map(|line| {
                line.to_ascii_lowercase()
                    .strip_prefix("content-length:")
                    .map(|v| v.trim().parse::<usize>().unwrap_or(0))
            })
            .unwrap_or(0);
        while buf.len() < head_end + length {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        let body = String::from_utf8_lossy(&buf[head_end..]).to_string();
        (head.lines().next().unwrap_or("").to_owned(), body)
    }

    pub(super) fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    /// When the fake homeserver refuses a sliding-sync request.
    #[derive(Clone, Copy)]
    enum Script {
        /// Refuse every request.
        RefuseAlways,
        /// `rounds` times: refuse `refuse` requests, then answer until the
        /// room-list connection has had `answered` responses (enough to reach
        /// Running). After the last round, answer everything.
        Rounds { rounds: usize, refuse: usize, answered: usize },
    }

    /// The script's position. One lock, so a refusal decision and its count
    /// cannot interleave between two concurrent requests.
    #[derive(Default)]
    struct Progress {
        round: usize,
        refused_this_round: usize,
        room_list_answered_this_round: usize,
    }

    #[derive(Default)]
    struct Seen {
        unknown_token: AtomicUsize,
        refreshes: AtomicUsize,
        syncs_served: AtomicUsize,
    }

    /// A homeserver advertising MSC4186. `/refresh` always succeeds: the
    /// session is never dead, which is the whole point.
    fn serve(script: Script, seen: Arc<Seen>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        let progress = Arc::new(Mutex::new(Progress::default()));
        thread::spawn(move || {
            let _ = ready_tx.send(());
            let pos = Arc::new(AtomicUsize::new(0));
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                let pos = Arc::clone(&pos);
                let progress = Arc::clone(&progress);
                thread::spawn(move || {
                    let (line, body) = read_request(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{"org.matrix.simplified_msc3575":true}}"#);
                    } else if path.contains("simplified_msc3575/sync") {
                        let room_list = body.contains("room-list");
                        let refuse = {
                            let mut p = progress.lock().unwrap();
                            match script {
                                Script::RefuseAlways => true,
                                Script::Rounds { rounds, refuse, answered } => {
                                    if p.round >= rounds {
                                        false
                                    } else if p.refused_this_round < refuse {
                                        p.refused_this_round += 1;
                                        true
                                    } else {
                                        if room_list {
                                            p.room_list_answered_this_round += 1;
                                            if p.room_list_answered_this_round >= answered {
                                                *p = Progress { round: p.round + 1, ..Progress::default() };
                                            }
                                        }
                                        false
                                    }
                                }
                            }
                        };
                        if refuse {
                            seen.unknown_token.fetch_add(1, Ordering::SeqCst);
                            respond(&mut stream, "401 Unauthorized",
                                    r#"{"errcode":"M_UNKNOWN_TOKEN","error":"Token is not active","soft_logout":false}"#);
                        } else {
                            // Not a long-poll: pace the loop instead of spinning.
                            thread::sleep(Duration::from_millis(100));
                            seen.syncs_served.fetch_add(1, Ordering::SeqCst);
                            let n = pos.fetch_add(1, Ordering::SeqCst) + 1;
                            respond(&mut stream, "200 OK", &format!(r#"{{"pos":"{n}"}}"#));
                        }
                    } else if path.starts_with("/_matrix/client/v3/refresh") {
                        let n = seen.refreshes.fetch_add(1, Ordering::SeqCst) + 1;
                        respond(&mut stream, "200 OK", &format!(
                            r#"{{"access_token":"access-{n}","refresh_token":"refresh-{n}","expires_in_ms":300000}}"#));
                    } else if path.contains("/keys/upload") {
                        respond(&mut stream, "200 OK",
                                r#"{"one_time_key_counts":{"signed_curve25519":50}}"#);
                    } else if path.contains("/keys/query") {
                        respond(&mut stream, "200 OK", r#"{"device_keys":{},"failures":{}}"#);
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    type Done = fn(&[serde_json::Value]) -> bool;

    fn drain(events: &Arc<Mutex<VecDeque<String>>>) -> Vec<serde_json::Value> {
        events.lock().unwrap().iter().map(|e| serde_json::from_str(e).unwrap()).collect()
    }

    fn is_running(v: &serde_json::Value) -> bool {
        v["type"] == "room_list_sync_state" && v["state"] == "running"
    }

    fn is_failed(v: &serde_json::Value) -> bool {
        v["type"] == "room_list_mode" && v["mode"] == "failed"
    }

    fn is_auth_retry(v: &serde_json::Value) -> bool {
        v["type"] == "room_list_error"
            && v["category"].as_str().is_some_and(|c| c.starts_with("authentication_retry"))
    }

    fn told_session_gone(v: &serde_json::Value) -> bool {
        v["type"] == "room_list_error" && v["category"] == "authentication"
    }

    /// Runs the modern lane until `done` holds, `failed` is seen, or `bound`
    /// passes, then cancels it. Returns the events and the final mode.
    fn run(script: Script, seen: Arc<Seen>, bound: Duration, done: Done)
        -> (Vec<serde_json::Value>, SyncMode)
    {
        let addr = serve(script, seen);
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let sync_mode = Arc::new(Mutex::new(SyncMode::Probing));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .handle_refresh_tokens()
                .build()
                .await
                .expect("client");
            let session = MatrixSession {
                meta: SessionMeta {
                    user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                    device_id: OwnedDeviceId::from("TESTDEVICE"),
                },
                tokens: SessionTokens {
                    access_token: "access-0".to_owned(),
                    refresh_token: Some("refresh-0".to_owned()),
                },
            };
            client
                .matrix_auth()
                .restore_session(session, RoomLoadSettings::default())
                .await
                .expect("restore");

            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(&events);
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + bound;
                while tokio::time::Instant::now() < deadline {
                    let seen = drain(&watched);
                    if done(&seen) || seen.iter().any(is_failed) {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                let _ = cancel_tx.send(());
            };
            let (fallback, ()) = tokio::join!(
                run_modern_sync(
                    client,
                    Arc::clone(&events),
                    Arc::clone(&sync_mode),
                    Arc::new(Mutex::new(None)),
                    Arc::new(Mutex::new(None)),
                    Arc::new(Mutex::new(Default::default())),
                    Default::default(),
                    cancel_rx,
                ),
                watcher
            );
            assert!(fallback.is_none(), "an auth error must never downgrade to classic sync");
        });
        let mode = *sync_mode.lock().unwrap();
        (drain(&events), mode)
    }

    #[test]
    fn a_401_the_sdk_refreshed_through_does_not_end_the_room_list() {
        let seen = Arc::new(Seen::default());
        // One round: enough refusals that the SDK's refresh-and-retry fails.
        let script = Script::Rounds { rounds: 1, refuse: 4, answered: usize::MAX };
        let (events, mode) = run(script, Arc::clone(&seen), Duration::from_secs(40),
                                 |e| e.iter().any(is_running));

        assert!(seen.unknown_token.load(Ordering::SeqCst) >= 2,
                "the server never refused a sync; this test proved nothing: {events:?}");
        assert!(seen.refreshes.load(Ordering::SeqCst) >= 1,
                "the SDK never refreshed, so this is not the matrix.org shape: {events:?}");
        assert!(!events.iter().any(is_failed),
                "a refused sync on a live session ended the room list: {events:?}");
        assert!(!events.iter().any(told_session_gone),
                "told the user the session is gone while it was not: {events:?}");
        assert!(events.iter().any(is_running), "the room list never recovered: {events:?}");
        assert!(seen.syncs_served.load(Ordering::SeqCst) >= 2, "{events:?}");
        assert_eq!(mode, SyncMode::SlidingSync);
        // The retry is visible in the log, naming the errcode, never server text.
        assert!(events.iter().any(|v| v["type"] == "room_list_error"
                                      && v["category"] == "authentication_retry_unknown_token"),
                "{events:?}");
    }

    /// The reset, through the real loop: more refusal spells than the budget,
    /// each followed by a Running edge, must never add up to `Failed`. An
    /// intermittent race recurs over a long session.
    #[test]
    fn refusal_spells_separated_by_running_never_add_up_to_failed() {
        let seen = Arc::new(Seen::default());
        let rounds = MODERN_AUTH_RETRY_LIMIT as usize + 2;
        let script = Script::Rounds { rounds, refuse: 4, answered: 3 };
        let (events, mode) = run(script, Arc::clone(&seen), Duration::from_secs(90), |e| {
            // Done once the budget would have been exhausted without the reset
            // and the list is running again after the last spell.
            let retries = e.iter().filter(|v| is_auth_retry(v)).count();
            let last_retry = e.iter().rposition(is_auth_retry);
            retries > MODERN_AUTH_RETRY_LIMIT as usize
                && last_retry.is_some_and(|i| e[i..].iter().any(is_running))
        });

        // Checked first: without the reset the run ends in Failed after LIMIT
        // retries, which the count below would misreport as a weak fixture.
        assert!(!events.iter().any(is_failed),
                "separate refusal spells accumulated into Failed: {events:?}");
        let retries = events.iter().filter(|v| is_auth_retry(v)).count();
        assert!(retries > MODERN_AUTH_RETRY_LIMIT as usize,
                "fewer refusal spells than the budget; this test proved nothing: \
                 {retries} retries, {events:?}");
        assert!(!events.iter().any(told_session_gone), "{events:?}");
        assert_eq!(mode, SyncMode::SlidingSync, "{events:?}");
    }

    #[test]
    fn a_session_the_server_keeps_refusing_still_ends_in_failed() {
        let seen = Arc::new(Seen::default());
        let (events, mode) = run(Script::RefuseAlways, Arc::clone(&seen),
                                 Duration::from_secs(60), |_| false);

        assert!(seen.unknown_token.load(Ordering::SeqCst) >= 2, "{events:?}");
        assert!(!events.iter().any(is_running), "{events:?}");
        assert_eq!(mode, SyncMode::Failed, "{events:?}");
        assert!(events.iter().any(told_session_gone), "{events:?}");
        // Bounded: exactly MODERN_AUTH_RETRY_LIMIT retries were announced first.
        let retries = events.iter().filter(|v| is_auth_retry(v)).count();
        assert_eq!(retries, MODERN_AUTH_RETRY_LIMIT as usize, "{events:?}");
    }

    #[test]
    fn the_budget_is_consecutive_and_running_resets_it() {
        // LIMIT + 1 in a row: the last one is fatal, none before it.
        let mut budget = AuthRetryBudget::default();
        for _ in 0..MODERN_AUTH_RETRY_LIMIT {
            assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Retry);
        }
        assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Fatal);

        // LIMIT + 1 with a Running edge between each: never fatal.
        let mut budget = AuthRetryBudget::default();
        for _ in 0..=MODERN_AUTH_RETRY_LIMIT {
            assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Retry);
            budget.on_running();
        }

        // A reset in the middle restarts the count from one.
        let mut budget = AuthRetryBudget::default();
        for _ in 0..MODERN_AUTH_RETRY_LIMIT {
            budget.on_auth_error();
        }
        budget.on_running();
        for _ in 0..MODERN_AUTH_RETRY_LIMIT {
            assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Retry);
        }
        assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Fatal);
    }

    /// LIGHTNING_SWITCH_TRACE through the real modern loop: a traced sync's
    /// first `room_list_reset` carries the trace and no later one does; an
    /// untraced sync's resets carry none, exactly as before the trace existed.
    #[test]
    fn only_the_first_reset_of_a_traced_sync_carries_the_switch_trace() {
        use crate::switchtrace;
        fn is_reset(v: &serde_json::Value) -> bool {
            v["type"] == "room_list_reset"
        }
        let run_once = |traced: bool| -> Vec<serde_json::Value> {
            // Answer everything: no refusals.
            let addr = serve(Script::Rounds { rounds: 0, refuse: 0, answered: 0 },
                             Arc::new(Seen::default()));
            let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
            let runtime = tokio::runtime::Builder::new_multi_thread()
                .enable_all()
                .build()
                .expect("runtime");
            runtime.block_on(async {
                let client = matrix_sdk::Client::builder()
                    .homeserver_url(format!("http://{addr}"))
                    .build()
                    .await
                    .expect("client");
                let session = MatrixSession {
                    meta: SessionMeta {
                        user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                        device_id: OwnedDeviceId::from("TESTDEVICE"),
                    },
                    tokens: SessionTokens { access_token: "access-0".to_owned(), refresh_token: None },
                };
                client
                    .matrix_auth()
                    .restore_session(session, RoomLoadSettings::default())
                    .await
                    .expect("restore");
                let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
                let watched = Arc::clone(&events);
                let watcher = async move {
                    let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
                    while tokio::time::Instant::now() < deadline {
                        let seen = drain(&watched);
                        if seen.iter().any(is_reset) && seen.iter().any(is_running) {
                            break;
                        }
                        tokio::time::sleep(Duration::from_millis(50)).await;
                    }
                    let _ = cancel_tx.send(());
                };
                let started = traced.then(std::time::Instant::now);
                let _ = tokio::join!(
                    switchtrace::sync_scope(started, run_modern_sync(
                        client,
                        Arc::clone(&events),
                        Arc::new(Mutex::new(SyncMode::Probing)),
                        Arc::new(Mutex::new(None)),
                        Arc::new(Mutex::new(None)),
                        Arc::new(Mutex::new(Default::default())),
                        Default::default(),
                        cancel_rx,
                    )),
                    watcher
                );
            });
            drain(&events)
        };

        let events = run_once(true);
        let resets: Vec<&serde_json::Value> = events.iter().filter(|v| is_reset(v)).collect();
        let first = resets.first().copied().unwrap_or_else(|| {
            panic!("the loop never sent a room_list_reset; this proved nothing: {events:?}")
        });
        let trace = &first["trace"];
        assert!(trace.is_object(), "the first reset of a traced sync has no trace: {first}");
        assert_eq!(trace["v"], 1, "{trace}");
        assert_eq!(trace["rooms"].as_u64(), first["rooms"].as_array().map(|r| r.len() as u64));
        assert!(trace["since_start_sync_ms"].as_u64() >= trace["sync_service_build_ms"].as_u64(),
                "{trace}");
        assert!(resets[1..].iter().all(|v| v.get("trace").is_none()),
                "a later reset was traced too: {resets:?}");

        let events = run_once(false);
        assert!(events.iter().any(is_reset), "no reset untraced: {events:?}");
        assert!(events.iter().all(|v| v.get("trace").is_none()),
                "an untraced sync emitted a trace: {events:?}");
    }
}

/// The warm rooms against a homeserver whose sliding-sync session expires.
/// `SlidingSync::expire_session` clears every room subscription, so before the
/// warm set was re-sent the open room silently lost its subscription too.
#[cfg(test)]
mod warm_room_expiry_tests {
    use super::modern_sync_auth_tests::{read_request, respond};
    use super::{run_modern_sync, warm_rooms, SyncMode};
    use std::collections::VecDeque;
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{OwnedDeviceId, OwnedRoomId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };

    /// What the room-list connection asked for, request by request: the
    /// subscribed room ids, and the index at which the server answered
    /// M_UNKNOWN_POS.
    #[derive(Default)]
    struct Requests {
        subscribed: Mutex<Vec<Vec<String>>>,
        expired_at: AtomicUsize,
    }

    /// Answers the 4th room-list request with `M_UNKNOWN_POS` (once) and every
    /// other one with an empty, valid response.
    fn serve(requests: Arc<Requests>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            let pos = Arc::new(AtomicUsize::new(0));
            let room_list_requests = Arc::new(AtomicUsize::new(0));
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let requests = Arc::clone(&requests);
                let pos = Arc::clone(&pos);
                let room_list_requests = Arc::clone(&room_list_requests);
                thread::spawn(move || {
                    let (line, body) = read_request(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{"org.matrix.simplified_msc3575":true}}"#);
                    } else if path.contains("simplified_msc3575/sync") {
                        if body.contains("room-list") {
                            let n = room_list_requests.fetch_add(1, Ordering::SeqCst) + 1;
                            let ids: Vec<String> = serde_json::from_str::<serde_json::Value>(&body)
                                .ok()
                                .and_then(|v| v["room_subscriptions"].as_object().cloned())
                                .map(|m| m.keys().cloned().collect())
                                .unwrap_or_default();
                            requests.subscribed.lock().unwrap().push(ids);
                            if n == 4 {
                                requests.expired_at.store(
                                    requests.subscribed.lock().unwrap().len(), Ordering::SeqCst);
                                respond(&mut stream, "400 Bad Request",
                                        r#"{"errcode":"M_UNKNOWN_POS","error":"Unknown pos"}"#);
                                return;
                            }
                        }
                        thread::sleep(Duration::from_millis(100));
                        let n = pos.fetch_add(1, Ordering::SeqCst) + 1;
                        respond(&mut stream, "200 OK", &format!(r#"{{"pos":"{n}"}}"#));
                    } else if path.contains("/keys/upload") {
                        respond(&mut stream, "200 OK",
                                r#"{"one_time_key_counts":{"signed_curve25519":50}}"#);
                    } else if path.contains("/keys/query") {
                        respond(&mut stream, "200 OK", r#"{"device_keys":{},"failures":{}}"#);
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    #[test]
    fn the_warm_rooms_are_sent_again_after_the_session_expires() {
        let requests = Arc::new(Requests::default());
        let addr = serve(Arc::clone(&requests));
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let sync_mode = Arc::new(Mutex::new(SyncMode::Probing));
        let rooms: Vec<String> = vec!["!one:example.org".into(), "!two:example.org".into()];
        let set = Arc::new(Mutex::new(warm_rooms::SubscriptionSet::default()));
        for r in &rooms {
            set.lock().unwrap().open(OwnedRoomId::try_from(r.as_str()).unwrap());
        }
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: SessionMeta {
                            user_id: OwnedUserId::try_from("@me:example.org").unwrap(),
                            device_id: OwnedDeviceId::from("TESTDEVICE"),
                        },
                        tokens: SessionTokens {
                            access_token: "access-0".to_owned(),
                            refresh_token: None,
                        },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");

            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(&requests);
            let want = rooms.clone();
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + Duration::from_secs(40);
                while tokio::time::Instant::now() < deadline {
                    let expired = watched.expired_at.load(Ordering::SeqCst);
                    let seen = watched.subscribed.lock().unwrap().clone();
                    if expired > 0
                        && seen.iter().skip(expired).any(|ids| want.iter().all(|r| ids.contains(r)))
                    {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                let _ = cancel_tx.send(());
            };
            let (fallback, ()) = tokio::join!(
                run_modern_sync(
                    client,
                    Arc::clone(&events),
                    Arc::clone(&sync_mode),
                    Arc::new(Mutex::new(None)),
                    Arc::new(Mutex::new(None)),
                    Arc::clone(&set),
                    Default::default(),
                    cancel_rx,
                ),
                watcher
            );
            assert!(fallback.is_none());
        });
        let expired = requests.expired_at.load(Ordering::SeqCst);
        let seen = requests.subscribed.lock().unwrap().clone();
        assert!(expired > 0, "the server never expired the session: {seen:?}");
        assert!(
            seen[..expired - 1].iter().any(|ids| rooms.iter().all(|r| ids.contains(r))),
            "both warm rooms must be subscribed before the expiry: {seen:?}"
        );
        assert!(
            seen[expired..].iter().any(|ids| rooms.iter().all(|r| ids.contains(r))),
            "both warm rooms must be subscribed again after the expiry: {seen:?}"
        );
    }
}

/// What a decryption failure sets in motion with the settings every client is
/// built with (decided 2026-10-13, CLAUDE.md §9): matrix-sdk fetches the key
/// from the trusted backup, and asks our own VERIFIED devices for it. Against a
/// loopback homeserver (`MatrixMockServer`); the backup key and the second
/// device are throwaway values made here, and nothing is ever decrypted.
#[cfg(test)]
mod utd_recovery_tests {
    use super::encryption_settings;
    use matrix_sdk::encryption::BackupDownloadStrategy;
    use matrix_sdk::ruma::api::client::keys::get_keys;
    use matrix_sdk::ruma::api::client::sync::sync_events::v5;
    use matrix_sdk::ruma::events::ToDeviceEventType;
    use matrix_sdk::ruma::serde::Raw;
    use matrix_sdk::ruma::{device_id, room_id, TransactionId};
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk::Client;
    use matrix_sdk_base::crypto::store::types::BackupDecryptionKey;
    use matrix_sdk_base::crypto::types::requests::AnyOutgoingRequest;
    use matrix_sdk_base::crypto::{LocalTrust, OlmMachine};
    use matrix_sdk_base::RequestedRequiredStates;
    use serde_json::json;
    use std::collections::BTreeMap;
    use std::time::{Duration, Instant};

    /// The Megolm session the event below was sent with, as it appears in a
    /// request path (`/` percent-encoded).
    const SESSION_IN_PATH: &str = "64H7XKokIx0ASkYDHZKlT5zd%2FZccz%2FcQspPNdvnNULA";

    /// matrix-sdk's own UTD fixture (tests/integration/encryption/backups.rs):
    /// a Megolm event whose session this client has never seen.
    fn undecryptable_event() -> serde_json::Value {
        json!({
            "event_id": "$JbFHtZpEJiH8uaajZjPLz0QUZc1xtBR9rPGBOjF6WFM",
            "origin_server_ts": 1698579035927u64,
            "sender": "@example2:morpheus.localhost",
            "type": "m.room.encrypted",
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "AwgAEpABhetEzzZzyYrxtEVUtlJnZtJcURBlQUQJ9irVeklCTs06LwgTMQj61PMUS4Vy\
                               YOX+PD67+hhU40/8olOww+Ud0m2afjMjC3wFX+4fFfSkoWPVHEmRVucfcdSF1RSB4EmK\
                               PIP4eo1X6x8kCIMewBvxl2sI9j4VNvDvAN7M3zkLJfFLOFHbBviI4FN7hSFHFeM739Zg\
                               iwxEs3hIkUXEiAfrobzaMEM/zY7SDrTdyffZndgJo7CZOVhoV6vuaOhmAy4X2t4UnbuV\
                               JGJjKfV57NAhp8W+9oT7ugwO",
                "device_id": "KIUVQQSDTM",
                "sender_key": "LvryVyoCjdONdBCi2vvoSbI34yTOx7YrCFACUEKoXnc",
                "session_id": "64H7XKokIx0ASkYDHZKlT5zd/Zccz/cQspPNdvnNULA"
            }
        })
    }

    /// A client built with Lightning's `encryption_settings()`, its E2EE
    /// setup finished (that is where the SDK installs its UTD handler).
    async fn client_as_built(server: &MatrixMockServer) -> Client {
        let client = server
            .client_builder()
            .on_builder(|builder| builder.with_encryption_settings(encryption_settings()))
            .build()
            .await;
        client.encryption().wait_for_e2ee_initialization_tasks().await;
        client
    }

    /// Deliver the undecryptable event through the sliding-sync path the app
    /// uses.
    async fn sync_undecryptable_event(client: &Client) {
        let mut response = v5::Response::new("pos-1".to_owned());
        let mut entry = v5::response::Room::default();
        entry.required_state = vec![Raw::from_json_string(
            json!({
                "type": "m.room.encryption", "state_key": "",
                "event_id": "$encryption", "origin_server_ts": 1698579035000u64,
                "sender": "@example2:morpheus.localhost",
                "content": { "algorithm": "m.megolm.v1.aes-sha2" }
            })
            .to_string(),
        )
        .unwrap()];
        entry.timeline =
            vec![Raw::from_json_string(undecryptable_event().to_string()).unwrap()];
        response
            .rooms
            .insert(room_id!("!DovneieKSTkdHKpIXy:morpheus.localhost").to_owned(), entry);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
    }

    /// GETs of this session's key from the backup the server has seen so far.
    async fn backup_key_fetches(server: &MatrixMockServer) -> usize {
        server
            .server()
            .received_requests()
            .await
            .unwrap_or_default()
            .iter()
            .filter(|request| {
                let path = request.url.path();
                request.method.as_str() == "GET"
                    && path.contains("/room_keys/keys/")
                    && path.ends_with(SESSION_IN_PATH)
            })
            .count()
    }

    /// Whole-room backup GETs (`/room_keys/keys/{room}`) for the test room.
    async fn whole_room_fetches(server: &MatrixMockServer) -> usize {
        server
            .server()
            .received_requests()
            .await
            .unwrap_or_default()
            .iter()
            .filter(|request| {
                let path = request.url.path();
                request.method.as_str() == "GET"
                    && path.contains("/room_keys/keys/")
                    && path.ends_with("morpheus.localhost")
            })
            .count()
    }

    /// Pending `m.room_key_request`s the crypto layer wants sent.
    async fn queued_key_requests(client: &Client) -> usize {
        let machine = client.olm_machine_for_testing().await;
        let machine = machine.as_ref().expect("olm machine");
        machine
            .outgoing_requests()
            .await
            .expect("outgoing requests")
            .iter()
            .filter(|request| {
                matches!(
                    request.request(),
                    AnyOutgoingRequest::ToDeviceRequest(r)
                        if r.event_type == ToDeviceEventType::RoomKeyRequest
                )
            })
            .count()
    }

    /// A usable backup: its decryption key and version are on this device, as
    /// after verification or recovery-key entry.
    async fn enable_backup(client: &Client) {
        {
            let machine = client.olm_machine_for_testing().await;
            let backup = machine.as_ref().expect("olm machine").backup_machine();
            let key = BackupDecryptionKey::new();
            let public = key.megolm_v1_public_key();
            public.set_version("1".to_owned());
            backup
                .save_decryption_key(Some(key), Some("1".to_owned()))
                .await
                .expect("backup key stored");
            backup.enable_backup_v1(public).await.expect("backup enabled");
        }
        assert!(client.encryption().backups().are_enabled().await);
    }

    /// Give the client a second device of its own, verified (`trusted`) or
    /// not, the way a `/keys/query` answer would.
    async fn add_own_device(client: &Client, trusted: bool) {
        let user = client.user_id().expect("logged in").to_owned();
        let other_id = device_id!("OTHERDEVICE");
        let other = OlmMachine::new(&user, other_id).await;
        let keys = other
            .get_device(&user, other_id, None)
            .await
            .expect("store")
            .expect("own device")
            .as_device_keys()
            .clone();
        let mut response = get_keys::v3::Response::new();
        response.device_keys.insert(
            user.clone(),
            BTreeMap::from([(
                other_id.to_owned(),
                Raw::from_json(serde_json::value::to_raw_value(&keys).unwrap()),
            )]),
        );
        let machine = client.olm_machine_for_testing().await;
        let machine = machine.as_ref().expect("olm machine");
        machine
            .mark_request_as_sent(&TransactionId::new(), &response)
            .await
            .expect("keys query answer accepted");
        let device = machine
            .get_device(&user, other_id, None)
            .await
            .expect("store")
            .expect("the second device is known");
        if trusted {
            device.set_local_trust(LocalTrust::Verified).await.expect("trust stored");
        }
        let device = machine
            .get_device(&user, other_id, None)
            .await
            .expect("store")
            .expect("the second device is known");
        assert_eq!(device.is_verified(), trusted, "fixture: trust as asked");
    }

    /// Give the client a second device of its own that is SIGNED by our
    /// self-signing key, while this client holds none of the cross-signing
    /// private keys, i.e. our own identity is not verified here: the shape of a
    /// fresh, unverified session looking at an account whose other sessions
    /// were cross-signed elsewhere.
    async fn add_own_device_signed_by_an_unverified_identity(client: &Client) {
        let user = client.user_id().expect("logged in").to_owned();
        let other_id = device_id!("SIGNEDDEVICE");
        let other = OlmMachine::new(&user, other_id).await;
        let bootstrap = other.bootstrap_cross_signing(false).await.expect("bootstrap");
        let keys = bootstrap.upload_signing_keys_req;
        let device = other
            .get_device(&user, other_id, None)
            .await
            .expect("store")
            .expect("own device");
        let signed = device.verify().await.expect("signed by our self-signing key");
        // `verify()` signs a copy with its signatures cleared, so merge the
        // self-signing signature into the device's own self-signed keys
        // (signatures are outside the canonical JSON both sign).
        let signed_keys = serde_json::to_value(&signed.signed_keys).unwrap();
        let ssk_signatures =
            signed_keys[user.as_str()][other_id.as_str()]["signatures"][user.as_str()].clone();
        let mut device_json = serde_json::to_value(device.as_device_keys()).unwrap();
        for (key_id, signature) in ssk_signatures.as_object().expect("an SSK signature") {
            device_json["signatures"][user.as_str()][key_id] = signature.clone();
        }
        let device_json = device_json.to_string();
        let cross = |key: Option<matrix_sdk_base::crypto::types::CrossSigningKey>| {
            Raw::from_json_string(serde_json::to_string(&key.expect("key")).unwrap()).unwrap()
        };
        let mut response = get_keys::v3::Response::new();
        response.device_keys.insert(
            user.clone(),
            BTreeMap::from([(
                other_id.to_owned(),
                Raw::from_json_string(device_json).unwrap(),
            )]),
        );
        response.master_keys.insert(user.clone(), cross(keys.master_key));
        response.self_signing_keys.insert(user.clone(), cross(keys.self_signing_key));
        response.user_signing_keys.insert(user.clone(), cross(keys.user_signing_key));
        let machine = client.olm_machine_for_testing().await;
        machine
            .as_ref()
            .expect("olm machine")
            .mark_request_as_sent(&TransactionId::new(), &response)
            .await
            .expect("keys query answer accepted");
    }

    // Review L1: the Sessions page's rule on real SDK devices, not on booleans.
    // A device signed by an identity this session has not verified is NOT
    // trusted (the old label, `crossSigned`, badged it green), and a device
    // verified by SAS without cross-signing IS (the old label said "Not
    // verified").
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_sessions_trust_rule_follows_the_sdk_on_real_devices() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        let user = client.user_id().expect("logged in").to_owned();

        // One fixture at a time: a /keys/query answer is the user's whole
        // device list, so the second replaces the first.
        add_own_device_signed_by_an_unverified_identity(&client).await;

        let encryption = client.encryption();
        let signed = encryption
            .get_device(&user, device_id!("SIGNEDDEVICE"))
            .await
            .expect("store")
            .expect("the signed device is known");
        assert!(signed.is_cross_signed_by_owner(), "fixture: signed by our key");
        assert!(!signed.is_verified(), "fixture: our identity is not verified here");
        assert!(!super::recovery::session_row_trusted(
            false,
            signed.is_verified(),
            signed.is_cross_signed_by_owner(),
        ));

        add_own_device(&client, true).await;
        let sas_only = encryption
            .get_device(&user, device_id!("OTHERDEVICE"))
            .await
            .expect("store")
            .expect("the SAS-verified device is known");
        assert!(!sas_only.is_cross_signed_by_owner(), "fixture: never cross-signed");
        assert!(super::recovery::session_row_trusted(
            false,
            sas_only.is_verified(),
            sas_only.is_cross_signed_by_owner(),
        ));
    }

    // The policy itself, so a later edit cannot quietly drop back to OneShot
    // or Manual, or turn on an automatic creation.
    #[test]
    fn clients_download_after_a_decryption_failure_and_create_nothing() {
        let settings = encryption_settings();
        assert_eq!(
            settings.backup_download_strategy,
            BackupDownloadStrategy::AfterDecryptionFailure
        );
        assert!(!settings.auto_enable_cross_signing, "never bootstrap cross-signing unasked");
        assert!(!settings.auto_enable_backups, "never create a backup version unasked");
    }

    // (a) An event that fails to decrypt makes the SDK fetch exactly that
    // session's key from the backup. With the previous `OneShot` the SDK
    // installed no UTD handler and no download task, so this request never
    // came: this test fails on that code.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_undecryptable_event_fetches_its_key_from_the_backup() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;

        enable_backup(&client).await;
        assert_eq!(backup_key_fetches(&server).await, 0);

        sync_undecryptable_event(&client).await;

        let started = Instant::now();
        while backup_key_fetches(&server).await == 0 {
            assert!(
                started.elapsed() < Duration::from_secs(10),
                "no backup download for the undecryptable event's session"
            );
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        // Deduplicated per session: one failure, one request.
        tokio::time::sleep(Duration::from_millis(500)).await;
        assert_eq!(backup_key_fetches(&server).await, 1);
    }

    // The same event with NO usable backup on this device asks the server for
    // nothing: the SDK's task checks `are_enabled()` first.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn without_a_backup_key_a_failure_fetches_nothing() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        assert!(!client.encryption().backups().are_enabled().await);

        sync_undecryptable_event(&client).await;
        tokio::time::sleep(Duration::from_millis(800)).await;
        assert_eq!(backup_key_fetches(&server).await, 0);
    }

    // (b) `automatic-room-key-forwarding` is compiled in: the crypto layer
    // starts with both halves on, and a decryption failure queues an
    // `m.room_key_request` once one of our OTHER devices is verified. Without
    // the feature `create_outgoing_key_request` is `#[cfg]`'d out and both
    // flags read false, so this fails on the previous Cargo.toml.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_failure_asks_our_verified_device_for_the_key() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        {
            let machine = client.olm_machine_for_testing().await;
            let machine = machine.as_ref().expect("olm machine");
            assert!(machine.are_room_key_requests_enabled(), "feature not compiled in");
            assert!(machine.is_room_key_forwarding_enabled(), "feature not compiled in");
        }
        add_own_device(&client, true).await;
        assert_eq!(queued_key_requests(&client).await, 0);

        sync_undecryptable_event(&client).await;

        assert_eq!(queued_key_requests(&client).await, 1, "one request per session");
    }

    // A background history walk (index-all, media history) fetches the room's
    // keys in ONE request before paging, and says nothing on the recovery
    // status channel: the `crypto_bootstrap` events describe the user's
    // recovery, and a walk over hundreds of rooms must not move it. The
    // reporting pass, by contrast, does speak.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_history_walk_prefetches_a_rooms_keys_without_reporting() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        let room = "!DovneieKSTkdHKpIXy:morpheus.localhost";
        let events: crate::EventQueueRef =
            std::sync::Arc::new(std::sync::Mutex::new(std::collections::VecDeque::new()));
        let registry =
            std::sync::Arc::new(crate::timeline::TimelineRegistry::new(events.clone()));
        let bootstrap_events = || {
            events
                .lock()
                .unwrap()
                .iter()
                .filter(|raw| raw.contains("\"crypto_bootstrap\""))
                .count()
        };

        // No usable backup: the reporting pass says why it skipped, the
        // prefetch is silent, and neither asks the server.
        registry.download_backup_keys_for_room(&client, room).await;
        assert_eq!(bootstrap_events(), 1, "the reporting pass reports its skip");
        registry.prefetch_backup_keys_for_room(&client, room).await;
        assert_eq!(bootstrap_events(), 1, "the prefetch reports nothing");
        assert_eq!(whole_room_fetches(&server).await, 0);

        // With a backup: one whole-room request (the mock answers 404, so the
        // pass makes its one bounded retry), and still no status event.
        enable_backup(&client).await;
        registry.prefetch_backup_keys_for_room(&client, room).await;
        assert!(
            (1..=2).contains(&whole_room_fetches(&server).await),
            "one whole-room fetch plus at most its one retry"
        );
        assert_eq!(bootstrap_events(), 1, "the prefetch reports nothing");
    }

    // The SDK's own gate, pinned: with no verified device of ours to answer,
    // no request is made (an unverified device would not be trusted to send a
    // key back, and would not be answered either).
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn no_request_goes_out_without_a_verified_device_of_ours() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        add_own_device(&client, false).await;

        sync_undecryptable_event(&client).await;

        assert_eq!(queued_key_requests(&client).await, 0);
    }
}

// The sync-handler `timeline_event` payload for rooms with no open timeline:
// what each new field carries, and what the encrypted handler reads from the
// cleartext relation. Pure builders, so no client is needed.
#[cfg(test)]
mod sync_payload_tests {
    use super::{
        encrypted_relation_ids, poll_event_json, room_message_event_json,
        stable_poll_start, unstable_poll_start,
    };
    use matrix_sdk::ruma::{
        event_id,
        events::{
            poll::{
                start::PollStartEventContent,
                unstable_start::UnstablePollStartEventContent,
            },
            room::{
                encrypted::OriginalSyncRoomEncryptedEvent,
                message::RoomMessageEventContent,
            },
        },
        user_id,
    };
    use serde_json::json;

    fn payload(content: serde_json::Value) -> Option<serde_json::Value> {
        let content: RoomMessageEventContent = serde_json::from_value(content).unwrap();
        room_message_event_json(
            &content,
            event_id!("$e:example.org"),
            user_id!("@them:example.org"),
            1_000,
            user_id!("@me:example.org"),
            false,
        )
    }

    fn encrypted(relates_to: serde_json::Value) -> OriginalSyncRoomEncryptedEvent {
        serde_json::from_value(json!({
            "type": "m.room.encrypted",
            "event_id": "$enc:example.org",
            "sender": "@them:example.org",
            "origin_server_ts": 1,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "AwgAEnAC",
                "sender_key": "SENDERKEY",
                "device_id": "DEVICE",
                "session_id": "SESSION",
                "m.relates_to": relates_to,
            },
        }))
        .unwrap()
    }

    // Finding 130: an undecryptable thread reply in a room with no open
    // timeline lost its thread, so C++ mirrored it as a standalone main
    // timeline row (§8) and its notification opened the main timeline. The
    // relation is outside the ciphertext. Before the fix the handler read only
    // `Replacement` and `thread_root_id` was absent.
    #[test]
    fn an_undecryptable_thread_reply_keeps_its_thread_root() {
        let ev = encrypted(json!({
            "rel_type": "m.thread",
            "event_id": "$root:example.org",
            "is_falling_back": true,
            "m.in_reply_to": { "event_id": "$latest:example.org" },
        }));
        assert_eq!(
            encrypted_relation_ids(ev.content.relates_to.as_ref()),
            (String::new(), "$root:example.org".to_owned())
        );
        let edit = encrypted(json!({ "rel_type": "m.replace", "event_id": "$orig:example.org" }));
        assert_eq!(
            encrypted_relation_ids(edit.content.relates_to.as_ref()),
            ("$orig:example.org".to_owned(), String::new())
        );
    }

    // Finding 8: the sync payload carried no reply metadata, so a reply in a
    // room with no open timeline could never be a reply row.
    #[test]
    fn a_reply_names_the_event_it_answers_and_a_thread_fallback_does_not() {
        let reply = payload(json!({
            "msgtype": "m.text",
            "body": "> <@me:example.org> hi\n\nhello",
            "m.relates_to": { "m.in_reply_to": { "event_id": "$asked:example.org" } },
        }))
        .unwrap();
        assert_eq!(reply["in_reply_to"], "$asked:example.org");
        assert_eq!(reply["body"], "hello", "the reply fallback is stripped");

        // Every thread message carries an m.in_reply_to fallback for clients
        // without threads; it is not a reply.
        let in_thread = payload(json!({
            "msgtype": "m.text",
            "body": "in thread",
            "m.relates_to": {
                "rel_type": "m.thread",
                "event_id": "$root:example.org",
                "is_falling_back": true,
                "m.in_reply_to": { "event_id": "$latest:example.org" },
            },
        }))
        .unwrap();
        assert_eq!(in_thread["in_reply_to"], "");
        assert_eq!(in_thread["thread_root_id"], "$root:example.org");

        // A real reply inside a thread is a reply.
        let threaded_reply = payload(json!({
            "msgtype": "m.text",
            "body": "answer",
            "m.relates_to": {
                "rel_type": "m.thread",
                "event_id": "$root:example.org",
                "is_falling_back": false,
                "m.in_reply_to": { "event_id": "$asked:example.org" },
            },
        }))
        .unwrap();
        assert_eq!(threaded_reply["in_reply_to"], "$asked:example.org");
    }

    #[test]
    fn an_edit_carries_its_replacement_text() {
        let edit = payload(json!({
            "msgtype": "m.text",
            "body": "* fixed",
            "m.new_content": { "msgtype": "m.text", "body": "fixed" },
            "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig:example.org" },
        }))
        .unwrap();
        assert_eq!(edit["replaces_event_id"], "$orig:example.org");
        assert_eq!(edit["new_body"], "fixed");
        let plain = payload(json!({ "msgtype": "m.text", "body": "hi" })).unwrap();
        assert_eq!(plain["new_body"], "");
        assert_eq!(plain["in_reply_to"], "");
    }

    // Finding 53: a msgtype with no typed row was dropped, so a server notice
    // or a bridge's custom type never notified in a room that was not open.
    #[test]
    fn an_unknown_msgtype_is_forwarded_as_text_and_a_verification_request_is_not() {
        let notice = payload(json!({
            "msgtype": "m.server_notice",
            "body": "Your server is over its limit",
            "server_notice_type": "m.server_notice.usage_limit_reached",
        }))
        .unwrap();
        assert_eq!(notice["msgtype"], "text");
        assert_eq!(notice["body"], "Your server is over its limit");
        let custom = payload(json!({ "msgtype": "com.example.bot", "body": "beep" })).unwrap();
        assert_eq!(custom["msgtype"], "text");
        assert_eq!(custom["body"], "beep");
        assert!(payload(json!({
            "msgtype": "m.key.verification.request",
            "body": "verify me",
            "from_device": "DEV",
            "methods": ["m.sas.v1"],
            "to": "@me:example.org",
        }))
        .is_none());
    }

    #[test]
    fn mentions_are_read_from_the_content() {
        let mention = payload(json!({
            "msgtype": "m.text",
            "body": "hey",
            "m.mentions": { "user_ids": ["@me:example.org"] },
        }))
        .unwrap();
        assert_eq!(mention["mentions_me"], true);
        assert_eq!(mention["mentions_room"], false);
    }

    // Finding 53: poll starts had no sync handler at all.
    #[test]
    fn a_poll_start_becomes_a_poll_row_and_a_poll_edit_does_not() {
        let unstable: UnstablePollStartEventContent = serde_json::from_value(json!({
            "org.matrix.msc3381.poll.start": {
                "question": { "org.matrix.msc1767.text": "Lunch?" },
                "kind": "org.matrix.msc3381.poll.disclosed",
                "max_selections": 1,
                "answers": [
                    { "id": "a", "org.matrix.msc1767.text": "Yes" },
                    { "id": "b", "org.matrix.msc1767.text": "No" },
                ],
            },
            "org.matrix.msc1767.text": "Lunch?\n1. Yes\n2. No",
            "m.relates_to": { "m.in_reply_to": { "event_id": "$asked:example.org" } },
        }))
        .unwrap();
        let start = unstable_poll_start(&unstable).expect("a new poll");
        let row = poll_event_json(
            &start,
            event_id!("$p:example.org"),
            user_id!("@them:example.org"),
            5,
            false,
        );
        assert_eq!(row["msgtype"], "poll");
        assert_eq!(row["body"], "Lunch?");
        assert_eq!(row["in_reply_to"], "$asked:example.org");

        let stable: PollStartEventContent = serde_json::from_value(json!({
            "m.text": [{ "body": "Lunch?" }],
            "m.poll": {
                "question": { "m.text": [{ "body": "Lunch?" }] },
                "kind": "m.disclosed",
                "max_selections": 1,
                "answers": [
                    { "m.id": "a", "m.text": [{ "body": "Yes" }] },
                    { "m.id": "b", "m.text": [{ "body": "No" }] },
                ],
            },
        }))
        .unwrap();
        assert_eq!(stable_poll_start(&stable).expect("a new poll").question, "Lunch?");

        let edit: PollStartEventContent = serde_json::from_value(json!({
            "m.text": [{ "body": "Lunch?" }],
            "m.poll": {
                "question": { "m.text": [{ "body": "Lunch?" }] },
                "max_selections": 1,
                "answers": [
                    { "m.id": "a", "m.text": [{ "body": "Yes" }] },
                    { "m.id": "b", "m.text": [{ "body": "No" }] },
                ],
            },
            "m.new_content": {
                "m.text": [{ "body": "Dinner?" }],
                "m.poll": {
                    "question": { "m.text": [{ "body": "Dinner?" }] },
                    "max_selections": 1,
                    "answers": [
                        { "m.id": "a", "m.text": [{ "body": "Yes" }] },
                        { "m.id": "b", "m.text": [{ "body": "No" }] },
                    ],
                },
            },
            "m.relates_to": { "rel_type": "m.replace", "event_id": "$p:example.org" },
        }))
        .unwrap();
        assert!(stable_poll_start(&edit).is_none(), "an edited poll is not a new one");
    }
}
