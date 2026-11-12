
use std::{
    collections::{BTreeSet, HashMap, HashSet, VecDeque},
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
mod denoise;
mod discover;
mod gifs;
mod ignore;
mod iotrace;
mod labs;
mod bgsession;
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
mod stablepoll;
mod stickers;
mod uia;
mod warm_rooms;
mod recovery;
mod password;
mod policy;
mod presence;
mod qrlogin;
mod profile;
mod push_verdict;
mod rooms;
mod route;
mod roomaction;
mod roomexport;
mod rtc;
mod aboutinfo;
mod serveradmin;
mod sfu;
mod spoiler;
mod storeclose;
mod syncnotify;
mod timeline;

pub(crate) const USER_AGENT: &str = concat!("Lightning/", env!("CARGO_PKG_VERSION"));

pub(crate) type EventQueueRef = Arc<Mutex<VecDeque<String>>>;

#[derive(Clone, Copy, Debug)]
enum RecoveryNudge {
    VerificationDone,
    ManualRequest,
    SecretEventSeen,
}

type RecoveryNudgeSlot =
    Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<RecoveryNudge>>>>;

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
    serial: u64,
    custody: Arc<labs::CustodyFacts>,
    bg: Option<Arc<bgsession::BgState>>,
    account_state_forgotten: AtomicBool,
    bridge_id: u64,
    /// The local message index (SQLite FTS5), opened lazily inside the
    /// account's store directory so it is deleted with the account.
    ///
    /// A `std` mutex, not tokio's: every hold is a short synchronous query and
    /// must never span an `.await`.
    search_index: Arc<Mutex<Option<localsearch::SearchIndex>>>,
    index_shutdown: Arc<AtomicBool>,
    index_all: Arc<indexall::Control>,
    media_history: Arc<Mutex<HashMap<String, mediahistory::Cursor>>>,
    session_file: Arc<Mutex<Option<PathBuf>>>,
    client: Arc<Mutex<Option<Client>>>,
    events: Arc<Mutex<VecDeque<String>>>,
    sync_task: Mutex<Option<SyncTask>>,
    sync_mode: Arc<Mutex<SyncMode>>,
    runtime: Arc<tokio::runtime::Runtime>,
    timelines: Arc<timeline::TimelineRegistry>,
    // In-flight / returned DM creates; see rooms::DmCreateRegistry.
    dm_creates: Arc<rooms::DmCreateRegistry>,
    call_media_capable: Arc<std::sync::atomic::AtomicBool>,
    sfu: sfu::SfuState,
    rtc_membership_delays: Arc<tokio::sync::Mutex<rtc::MembershipDelayRegistry>>,
    sfu_generation: Arc<std::sync::atomic::AtomicU64>,
    room_list_service: Arc<Mutex<Option<Arc<RoomListService>>>>,
    // The dynamic adapter that owns the room-list index space; C++ indexes its
    // registry by the positions its diffs carry. `set_filter` makes it re-emit
    // a `VectorDiff::Reset`, which is how a rejected diff is recovered. A
    // `client.rooms()` snapshot has a different order and would make the
    // rejection self-sustaining (see `enqueue_rooms_stamped`).
    room_list_entries: Arc<Mutex<Option<Arc<RoomListDynamicEntriesController>>>>,
    active_room_subscription: Arc<Mutex<warm_rooms::SubscriptionSet>>,
    uia_pending: Arc<Mutex<Option<uia::UiaPending>>>,
    import_task: Mutex<Option<tokio::task::JoinHandle<()>>>,
    room_action_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    active_typing_room: Arc<Mutex<Option<String>>>,
    receipt_targets: Arc<Mutex<HashMap<String, OwnedEventId>>>,
    receipt_privacy: Arc<AtomicI32>,
    qr_login: Arc<qrlogin::QrLoginState>,
    receipt_serial: Arc<tokio::sync::Mutex<()>>,
    invite_actions: Arc<Mutex<BTreeSet<String>>>,
    notification_mode_targets: Arc<Mutex<HashMap<String, u8>>>,
    notification_mode_serial: Arc<tokio::sync::Mutex<()>>,
    notification_settings: Arc<Mutex<Option<NotificationSettings>>>,
    notification_watch_lifecycle: Arc<std::sync::atomic::AtomicU64>,
    // The push-rules watcher's own task. It never finishes by itself, so
    // shutdown aborts it BEFORE joining the room-action pool (like the media
    // fetches), or every sign-out would wait out the join budget.
    notification_watch_abort: Arc<Mutex<Option<tokio::task::AbortHandle>>>,
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    // (flow_id, sas): SasVerification has no flow_id() accessor in matrix-sdk
    // 0.18.
    active_sas: Arc<Mutex<Option<(String, SasVerification)>>>,
    // CSRF `state` of the in-flight OAuth request, kept only so a cancelled
    // sign-in can call `OAuth::abort_login(state)`. Scoped to this handle.
    // Never logged.
    oauth_state: Arc<Mutex<Option<matrix_sdk::authentication::oauth::CsrfToken>>>,
    token_task: Arc<Mutex<Option<tokio::task::JoinHandle<()>>>>,
    token_ledger: Arc<sessiontokens::TokenLedger>,
    active_qr: Arc<Mutex<Option<(String, QrVerification)>>>,
    // SAS driver tasks. They hold this account's Client, so one left running
    // keeps the crypto store open while sign-out deletes it. Joined by
    // shutdown_managed_tasks.
    verification_tasks: Mutex<Vec<tokio::task::JoinHandle<()>>>,
    verification_shutdown: Arc<AtomicBool>,
    import_active: Arc<AtomicBool>,
    // Parked media payloads for the take/free bridge, keyed by op id, between
    // `media_ready` and `mx_rust_media_take`. Cleared on shutdown so decrypted
    // media never outlives the session.
    media_results: Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    personal_background_results: bgsync::ParkedPictures,
    personal_switch_cache: bgsync::SwitchCache,
    pub(crate) media_fetch_aborts: Arc<Mutex<HashMap<u64, tokio::task::AbortHandle>>>,
    command_events: EventQueueRef,
    bootstrap_task: Mutex<Option<SyncTask>>,
    recovery_nudges: RecoveryNudgeSlot,
    sync_notify: Arc<syncnotify::SyncNotify>,
}

impl RustClient {
    fn new(store_path: PathBuf) -> Result<Self, String> {
        Self::new_with(store_path, None)
    }

    fn new_with(store_path: PathBuf, profile: Option<&labs::Profile>) -> Result<Self, String> {
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let mut builder = tokio::runtime::Builder::new_multi_thread();
        builder
            .worker_threads(2)
            .thread_stack_size(SESSION_THREAD_STACK_BYTES);
        match profile {
            Some(profile) => {
                builder
                    .thread_name(labs::thread_name("sdk", &profile.slot))
                    .max_blocking_threads(profile.max_blocking_threads);
            }
            None => {
                builder.thread_name("lightning-sdk");
            }
        }
        let runtime = builder
            .enable_all()
            .build()
            .map_err(|err| format!("failed to create shared Tokio runtime: {err}"))?;
        let index_all = Arc::new(indexall::Control::new(&store_path));
        Ok(Self {
            store_path,
            serial: labs::next_handle_serial(),
            custody: Arc::new(labs::CustodyFacts::default()),
            bg: None,
            account_state_forgotten: AtomicBool::new(false),
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
            rtc_membership_delays: Arc::new(tokio::sync::Mutex::new(rtc::MembershipDelayRegistry::default())),
            sfu_generation: Arc::new(std::sync::atomic::AtomicU64::new(0)),
        })
    }

    fn enqueue(&self, value: serde_json::Value) {
        enqueue(&self.events, value);
    }

    fn token_sink(&self) -> sessiontokens::Sink {
        sessiontokens::Sink {
            ledger: Arc::clone(&self.token_ledger),
            lane: Arc::clone(&self.command_events),
            events: Arc::clone(&self.events),
        }
    }

    fn is_background(&self) -> bool {
        self.bg.as_ref().is_some_and(|bg| bg.is_background())
    }

    fn open_context(&self) -> Option<Arc<labs::OpenContext>> {
        if !labs::store_guards_active() {
            return None;
        }
        let profile = self.bg.as_ref().map(|bg| &bg.profile);
        Some(Arc::new(labs::OpenContext {
            serial: self.serial,
            require_account: profile.map(|p| (p.user_id.clone(), p.device_id.clone())),
            low_memory_sqlite: profile.is_some_and(|p| p.low_memory_sqlite),
            request_limits: profile.map(|p| (p.retry_limit, p.max_retry_time_ms)),
            strict_trust: profile.map(|p| p.strict_trust),
            refusal: Mutex::new(None),
        }))
    }

    fn op_thread_name(&self) -> Option<String> {
        self.bg.as_ref().map(|bg| labs::thread_name("op", &bg.profile.slot))
    }

    fn forget_account_state_once(&self, identity: Option<(String, String)>) -> bool {
        let Some((user_id, device_id)) = identity else { return false };
        if self.account_state_forgotten.swap(true, Ordering::SeqCst) {
            return false;
        }
        crate::rtc::forget_account_memberships_published(&user_id, &device_id);
        if let Ok(user) = UserId::parse(user_id.as_str()) {
            crate::previewbackfill::forget_user(&user);
        }
        true
    }

    fn account_identity(&self) -> Option<(String, String)> {
        self.client.lock().ok().and_then(|guard| {
            guard.as_ref().and_then(|client| {
                Some((client.user_id()?.to_string(), client.device_id()?.to_string()))
            })
        })
    }

    fn abandon_uia(&self) {
        if let Some(handle) = uia::abandon_on_teardown(&self.uia_pending) {
            self.spawn_room_action(async move {
                handle.cancel().await;
            });
        }
    }

    fn stop_sync_and_wait(&self) -> bool {
        if let Ok(mut guard) = self.active_room_subscription.lock() {
            guard.clear();
        }
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
        self.sync_notify.reset();
        stopped
    }

    async fn join_or_abort(
        handles: Vec<tokio::task::JoinHandle<()>>,
        budget_ms: u64,
    ) -> usize {
        if handles.is_empty() {
            return 0;
        }
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
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(SHUTDOWN_ABORT_DRAIN_MS),
            all.as_mut(),
        )
        .await;
        missed
    }

    fn shutdown_managed_tasks(&self)
        -> (bool, bool, usize, usize, u64, u64, u64) {
        let total = std::time::Instant::now();
        self.token_ledger.stop_waiting();
        // Stop the indexer first and close its connection: an open SQLite handle
        // in the store directory makes the deletion fail on Windows.
        //
        // Clear RTC membership bookkeeping unconditionally. A stale
        // `OWN_MEMBERSHIP_PUBLISHED` mark would let the next join inherit a dead
        // session's created_ts, which peers read as "not a new joiner" and answer
        // with no media key. The leave path cannot do this reliably because it
        // needs a live client.
        //
        // For THIS handle's (user, device) only (DECISION 11, design
        // §3.2.10): another account's session may still be running, during a
        // switch's overlap or as a Labs background account, and its marks are
        // not this teardown's to clear. A handle with no client never
        // published anything. The same holds for the blank-preview looks'
        // remembered results, which are decrypted text in memory (R1-M8).
        self.forget_account_state_once(self.account_identity());
        if let Some(bg) = &self.bg {
            bg.set_aggregator(None);
        }
        self.index_shutdown.store(true, Ordering::Relaxed);
        if let Ok(mut guard) = self.search_index.lock() {
            *guard = None;
        }
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
        let (pending_sas, pending_qr, pending_request) = take_pending_flows(
            &self.active_request, &self.active_sas, &self.active_qr,
        );
        if pending_sas.is_some() || pending_qr.is_some() || pending_request.is_some() {
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

        if let Ok(mut guard) = self.notification_watch_abort.lock() {
            if let Some(handle) = guard.take() {
                handle.abort();
            }
        }
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }

        self.abandon_uia();
        let actions = self.room_action_tasks.lock().ok()
            .map(|mut guard| std::mem::take(&mut *guard))
            .unwrap_or_default();
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

        if let Ok(mut guard) = self.token_task.lock() {
            if let Some(handle) = guard.take() {
                handle.abort();
            }
        }

        if let Ok(mut guard) = self.media_results.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.personal_background_results.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.media_fetch_aborts.lock() {
            for (_, handle) in guard.drain() {
                handle.abort();
            }
        }
        if let Ok(mut guard) = self.notification_settings.lock() {
            *guard = None;
        }
        if let Ok(mut guard) = self.notification_mode_targets.lock() {
            guard.clear();
        }
        (import_joined, sync_stopped, actions_missed, verifications_missed,
         actions_ms, verifications_ms, total.elapsed().as_millis() as u64)
    }

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

    fn spawn_media_fetch<F>(&self, op_id: u64, future: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Ok(mut tasks) = self.room_action_tasks.lock() {
            tasks.retain(|task| !task.is_finished());
            let handle = self.runtime.spawn(future);
            if let Ok(mut aborts) = self.media_fetch_aborts.lock() {
                aborts.retain(|_, h| !h.is_finished());
                aborts.insert(op_id, handle.abort_handle());
            }
            tasks.push(handle);
        }
    }

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

struct CatchPanic<F> {
    inner: Option<std::pin::Pin<Box<F>>>,
}

impl<F> CatchPanic<F> {
    fn new(future: F) -> Self {
        Self { inner: Some(Box::pin(future)) }
    }
}

impl<F: std::future::Future<Output = ()>> std::future::Future for CatchPanic<F> {
    type Output = Result<(), ()>;

    fn poll(
        self: std::pin::Pin<&mut Self>,
        cx: &mut std::task::Context<'_>,
    ) -> std::task::Poll<Self::Output> {
        use std::task::Poll;
        let this = self.get_mut();
        let Some(future) = this.inner.as_mut() else {
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

fn join_task_within_budget(task: &mut SyncTask, label: &str) -> bool {
    if let Some(cancel) = task.cancel.take() {
        let _ = cancel.send(());
    }
    let Some(thread) = task.thread.take() else {
        return true;
    };
    let finished = match task.done.take() {
        Some(done) => !matches!(
            done.recv_timeout(std::time::Duration::from_millis(
                SYNC_TASK_JOIN_BUDGET_MS
            )),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout)
        ),
        None => true,
    };
    if finished {
        let _ = thread.join();
        return true;
    }
    eprintln!(
        "lightning: {label} thread did not stop within {SYNC_TASK_JOIN_BUDGET_MS}ms; \
detaching it rather than blocking the UI"
    );
    false
}

struct SyncTask {
    cancel: Option<tokio::sync::oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
    done: Option<std::sync::mpsc::Receiver<()>>,
}

const SYNC_TASK_JOIN_BUDGET_MS: u64 = 1500;

const SHUTDOWN_ABORT_DRAIN_MS: u64 = 250;

const SHUTDOWN_FLOW_CANCEL_MS: u64 = 1000;

const SHUTDOWN_VERIFICATION_JOIN_MS: u64 =
    VERIFICATION_POLL_MS + 2 * SHUTDOWN_FLOW_CANCEL_MS;

const SHUTDOWN_FLOW_SWEEP_MS: u64 = 2 * SHUTDOWN_FLOW_CANCEL_MS;

pub(crate) const SHUTDOWN_ACTION_JOIN_MS: u64 = 1500;

const SHUTDOWN_IMPORT_JOIN_MS: u64 = 500;

const STORE_CLOSE_BUDGET_MS: u64 = 15_000;

const SHUTDOWN_DESTROY_RESERVE_MS: u64 = 3_000;

const RETIRE_RUNTIME_SHUTDOWN_MS: u64 = 1_500;

const RETIRE_STORE_RELEASE_WAIT_MS: u64 = 1_000;

const _: () = assert!(
    RETIRE_RUNTIME_SHUTDOWN_MS + RETIRE_STORE_RELEASE_WAIT_MS < SHUTDOWN_DESTROY_RESERVE_MS,
    "retire_bridge's own waits must fit the reserve kept for mx_rust_destroy"
);

const SHUTDOWN_WORST_CASE_MS: u64 = SHUTDOWN_VERIFICATION_JOIN_MS
    + SHUTDOWN_ABORT_DRAIN_MS
    + SHUTDOWN_FLOW_SWEEP_MS
    + SHUTDOWN_ACTION_JOIN_MS
    + SHUTDOWN_ABORT_DRAIN_MS
    + SHUTDOWN_IMPORT_JOIN_MS
    + 2 * SYNC_TASK_JOIN_BUDGET_MS
    + 2 * timeline::SHUTDOWN_ABORTED_JOIN_MS;

const _: () = assert!(
    SHUTDOWN_WORST_CASE_MS + SHUTDOWN_DESTROY_RESERVE_MS <= STORE_CLOSE_BUDGET_MS,
    "shutdown_managed_tasks can now outlast RustSdkMatrixClient::\
     kStoreCloseBudgetMs, so the C++ side will delete the store while a Rust \
     writer may still hold it open. Lower a budget above, or raise \
     kStoreCloseBudgetMs in src/matrix/RustSdkMatrixClient.h and this file's \
     STORE_CLOSE_BUDGET_MS with it."
);

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
        mediastore::forget(self as *const RustClient as usize, &self.store_path);
        route::forget(self as *const RustClient as usize, &self.store_path);
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

#[no_mangle]
pub extern "C" fn mx_rust_supports_e2ee(_client: *mut c_void) -> c_int {
    1
}

#[no_mangle]
pub extern "C" fn mx_rust_version() -> *mut c_char {
    ffi_string(|| Ok(env!("CARGO_PKG_VERSION").to_owned()))
}

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

fn panic_payload_requested(value: Option<&str>) -> bool {
    matches!(value, Some(v) if !v.trim().is_empty())
}

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
            let current = std::thread::current();
            eprintln!("{}", panic_report_line(location, current.name()));
            let trace = std::backtrace::Backtrace::capture();
            if trace.status() == std::backtrace::BacktraceStatus::Captured {
                eprintln!("{trace}");
            }
        }));
    });
}

fn install_sdk_tracing() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let requested = match std::env::var("LIGHTNING_RUST_LOG") {
            Ok(value) if !value.trim().is_empty() => value,
            _ => return,
        };
        let directives = if requested.trim() == "1" {
            "matrix_sdk_crypto=debug,matrix_sdk_base=info,matrix_sdk=info,\
             matrix_sdk_ui=warn"
                .replace(char::is_whitespace, "")
        } else {
            requested
        };
        let directives = clamp_transport_tracing(&directives);
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

const CLAMPED_TRANSPORT_TARGETS: [&str; 4] = ["reqwest", "hyper_util", "hyper", "h2"];

/// `LIGHTNING_RUST_LOG`'s directives with every directive that names a
/// transport target dropped, and each transport target pinned `off`. A
/// target directive outranks a bare level (`trace`) in an `EnvFilter`, so a
/// global level cannot reach them either.
fn clamp_transport_tracing(directives: &str) -> String {
    let names_transport = |directive: &str| {
        let target = directive.split(['=', '[']).next().unwrap_or("").trim();
        CLAMPED_TRANSPORT_TARGETS
            .iter()
            .any(|t| target == *t || target.starts_with(&format!("{t}::")))
    };
    let mut kept: Vec<String> = directives
        .split(',')
        .map(str::trim)
        .filter(|d| !d.is_empty() && !names_transport(d))
        .map(str::to_owned)
        .collect();
    kept.extend(CLAMPED_TRANSPORT_TARGETS.iter().map(|t| format!("{t}=off")));
    kept.join(",")
}

#[no_mangle]
pub extern "C" fn mx_rust_create(store_path: *const c_char) -> *mut c_void {
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
            eprintln!(
                "lightning: a retired client left its store open: {}",
                released.summary()
            );
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_destroy_and_report(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        if ptr.is_null() {
            return Ok(String::new());
        }
        Ok(retire_bridge(unsafe { Box::from_raw(ptr as *mut RustClient) }).summary())
    })
}

fn retire_bridge(bridge: Box<RustClient>) -> storeclose::Released {
    let owner = &*bridge as *const RustClient as usize;
    let probes = storeclose::snapshot_for(&bridge.store_path, owner, Some(bridge.serial));
    let background_queues = bridge
        .bg
        .is_some()
        .then(|| (Arc::clone(&bridge.events), Arc::clone(&bridge.command_events)));
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
        Err(shared) => {
            drop(shared);
            storeclose::RuntimeShutdown::Shared
        }
    };
    if let Some((events, commands)) = background_queues {
        bgsession::unregister(&events, &commands);
    }
    storeclose::wait_released(
        &probes,
        std::time::Duration::from_millis(RETIRE_STORE_RELEASE_WAIT_MS),
        shutdown,
    )
}

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
        let shared_runtime = Arc::clone(&bridge.runtime);
        let custody = Arc::clone(&bridge.custody);
        let open_context = bridge.open_context();
        spawn_session_thread(bridge.op_thread_name(), move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "login",
                labs::scope_open(open_context, async move {
                match build_client(&homeserver, &store_path).await {
                    Ok(client) => {
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
                                if let Ok(mut guard) = token_task.lock() {
                                    if let Some(previous) = guard.replace(
                                        oauth::spawn_token_persistence_custody(
                                            &client,
                                            client.subscribe_to_session_changes(),
                                            Arc::clone(&events),
                                            Some(Arc::clone(&token_sink.ledger)),
                                            custody,
                                        ))
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
            }));
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
        let shared_runtime = Arc::clone(&bridge.runtime);
        let open_context = bridge.open_context();
        spawn_session_thread(bridge.op_thread_name(), move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(
                shared_runtime, runtime_events, "restore_from_file",
                labs::scope_open(open_context,
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
            })));
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
        let shared_runtime = Arc::clone(&bridge.runtime);
        let token_task = Arc::clone(&bridge.token_task);
        let token_sink = bridge.token_sink();
        let custody = Arc::clone(&bridge.custody);
        let open_context = bridge.open_context();
        let background_restore = open_context
            .as_ref()
            .is_some_and(|context| context.require_account.is_some());
        spawn_session_thread(bridge.op_thread_name(), move || {
            let runtime_events = Arc::clone(&events);
            run_async_on(shared_runtime, runtime_events, "restore",
                labs::scope_open(open_context,
                switchtrace::restore_scope(switchtrace::switch_trace_enabled(), async move {
                match labs::bound_background_restore(
                    background_restore,
                    restore_client(
                        &homeserver,
                        &store_path,
                        &user_id,
                        &device_id,
                        access_token,
                        refresh_token,
                        &events,
                        Some(&token_sink),
                    ),
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
                        if let Ok(mut guard) = token_task.lock() {
                            if let Some(previous) = guard.replace(
                                oauth::spawn_token_persistence_custody(
                                    &client, changes, Arc::clone(&events),
                                    Some(Arc::clone(&token_sink.ledger)), custody))
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
            })));
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
        if let Some(client) = client {
            spawn_session_thread(bridge.op_thread_name(), move || {
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
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let sync_client = client.clone();
        let sync_search_index = Arc::clone(&bridge.search_index);
        let sync_index_all = Arc::clone(&bridge.index_all);
        let sync_index_shutdown = Arc::clone(&bridge.index_shutdown);
        let sync_notify = Arc::clone(&bridge.sync_notify);
        let bg_slot = bridge.bg.as_ref().map(|bg| bg.profile.slot.clone());
        let thread = spawn_session_thread(
            bg_slot.as_deref().map(|slot| labs::thread_name("sync", slot)),
            move || {
            let _done = done_tx;
            let runtime_events = Arc::clone(&events);
            run_async(runtime_events, "sync", async move {
                let stale_invites = invites::run_stale_invite_reconciler(
                    sync_client.clone(), Arc::clone(&events),
                );
                let lost_rooms = membership_lost::run_membership_reconciler(
                    sync_client.clone(), Arc::clone(&events), membership_list_slot,
                );
                tokio::select! {
                    _ = switchtrace::sync_scope(switch_trace_started, run_authoritative_sync(
                        sync_client, events, sync_search_index, sync_index_all,
                        sync_index_shutdown, sync_mode, room_list_slot,
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

        if let Ok(mut slot) = bridge.bootstrap_task.lock() {
            if slot.is_none() {
                let observer_events = Arc::clone(&bridge.events);
                let timelines = Arc::clone(&bridge.timelines);
                let lifecycle = bridge.timelines.lifecycle();
                let nudges = Arc::clone(&bridge.recovery_nudges);
                let observer_notify = Arc::clone(&bridge.sync_notify);
                let (cancel, cancel_rx) = tokio::sync::oneshot::channel::<()>();
                let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
                let thread = spawn_session_thread(
                    bg_slot.as_deref().map(|slot| labs::thread_name("boot", slot)),
                    move || {
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

/// Stack for session threads and the SDK runtime's workers. A session thread
/// `block_on`s a whole restore or login future, and a Background handle's is
/// larger still (`scope_open` wraps it). On std's 2 MiB default, a TLS
/// handshake that took a HelloRetryRequest into rustls' hybrid ML-KEM key
/// share ran off the end of the stack inside aws-lc's keygen and SIGSEGVed
/// the process (2026-10-21, Labs live accounts against a real homeserver; the
/// swarm's plain-HTTP local server never negotiates TLS, so it could not see
/// it). Reserved, not resident: a page costs memory only once touched. The
/// runtime setting covers its blocking pool too.
pub(crate) const SESSION_THREAD_STACK_BYTES: usize = 16 * 1024 * 1024;

pub(crate) fn spawn_session_thread<F>(name: Option<String>, body: F) -> std::thread::JoinHandle<()>
where
    F: FnOnce() + Send + 'static,
{
    let builder = std::thread::Builder::new().stack_size(SESSION_THREAD_STACK_BYTES);
    let builder = match name {
        Some(name) => builder.name(name),
        None => builder,
    };
    builder.spawn(body).expect("failed to spawn a session thread")
}

#[cfg(test)]
mod session_stack_tests {
    use super::*;

    #[inline(never)]
    fn consume_stack(depth: usize) -> u64 {
        let mut frame = [0u8; 64 * 1024];
        frame[depth % frame.len()] = depth as u8;
        std::hint::black_box(&mut frame);
        if depth == 0 {
            return u64::from(frame[0]);
        }
        let below = consume_stack(depth - 1);
        std::hint::black_box(&frame);
        below + u64::from(frame[depth % frame.len()]) + 1
    }

    const DEEP: usize = 96;

    #[test]
    fn a_session_thread_survives_a_deep_stack() {
        for name in [None, Some("lt-op-test".to_owned())] {
            let total = spawn_session_thread(name, || {
                assert!(consume_stack(DEEP) > 0);
            })
            .join();
            assert!(total.is_ok(), "the session thread panicked");
        }
    }

    #[test]
    fn the_shared_runtime_workers_survive_a_deep_stack() {
        let bridge = RustClient::new(PathBuf::new()).expect("bridge");
        let task = bridge.runtime.spawn(async { consume_stack(DEEP) });
        let total = bridge.runtime.block_on(task).expect("the worker task panicked");
        assert!(total > 0);
    }
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
    let decrypted_verdicts = push_verdict::SyncVerdicts::default();
    let mut verification = encryption.verification_state();
    let recovery = encryption.recovery();
    let backups = encryption.backups();
    let mut recovery_states = recovery.state_stream();
    let mut backup_states = backups.state_stream();
    let mut room_keys = encryption.room_keys_received_stream().await;
    let mut redecryption = Some(Box::pin(
        client.event_cache().subscribe_to_decryption_reports(),
    ));

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
    let mut backup_exists: Option<bool> = None;
    let mut retry_attempt: usize = 0;
    let mut retry_deadline: Option<tokio::time::Instant> = None;
    let mut recheck_deadline: Option<tokio::time::Instant> = None;
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
                        retry_attempt = 0;
                        retry_deadline = secret_retry_deadline(retry_attempt);
                        arrival_rearms = 0;
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
                                "exhausted"
                            },
                            0,
                        );
                    }
                }
            }
            _ = watchdog_sleep(recheck_deadline) => {
                recheck_deadline = None;
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
                    arrival_rearms += 1;
                    retry_attempt = 1;
                    retry_deadline = secret_retry_deadline(retry_attempt);
                }
            }
            nudge = nudge_rx.recv() => {
                let Some(nudge) = nudge else { continue };
                match nudge {
                    RecoveryNudge::VerificationDone => {
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
                let Ok(state) = state else { continue };
                emit_crypto_bootstrap(
                    &events, lifecycle, "backup_state",
                    backup_state_name(state), 0,
                );
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
                    Some(Ok(RedecryptorReport::Lagging)) => {
                        emit_crypto_bootstrap(
                            &events, lifecycle, "redecryption_lagging", "", 0,
                        );
                    }
                    Some(Err(_)) => {
                        emit_crypto_bootstrap(
                            &events, lifecycle, "redecryption_reports_missed", "", 0,
                        );
                    }
                    Some(Ok(RedecryptorReport::BackupAvailable)) => {}
                    None => redecryption = None,
                }
            }
            keys = async {
                match room_keys.as_mut() {
                    Some(stream) => stream.next().await,
                    None => std::future::pending().await,
                }
            } => {
                let Some(keys) = keys else { break };
                let Ok(infos) = keys else { continue };
                if !infos.is_empty() {
                    emit_crypto_bootstrap(
                        &events, lifecycle, "room_keys_received", "",
                        infos.len() as u64,
                    );
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
        Err(_) => { }
    }
}

async fn watchdog_sleep(deadline: Option<tokio::time::Instant>) {
    match deadline {
        Some(deadline) => tokio::time::sleep_until(deadline).await,
        None => std::future::pending::<()>().await,
    }
}

const SECRET_RETRY_DELAYS_SECS: [u64; 3] = [20, 90, 240];

fn next_secret_retry_delay(attempt: usize) -> Option<u64> {
    SECRET_RETRY_DELAYS_SECS.get(attempt).copied()
}

fn secret_retry_deadline(attempt: usize) -> Option<tokio::time::Instant> {
    next_secret_retry_delay(attempt).map(|secs| {
        tokio::time::Instant::now() + std::time::Duration::from_secs(secs)
    })
}

fn secret_recovery_missing(
    cross_signing_complete: bool,
    backup_exists: Option<bool>,
    backup_enabled: bool,
) -> bool {
    !cross_signing_complete
        || (backup_exists == Some(true) && !backup_enabled)
}

enum SecretAttemptOutcome {
    Complete,
    IdentityUnverified,
    Requested,
    AlreadyPending,
    NoEligibleDevices,
    Unavailable,
}

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

fn automatic_receipt(
    event_id: OwnedEventId,
    mode: i32,
    unthreaded: bool,
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
    let thread = if unthreaded {
        ReceiptThread::Unthreaded
    } else {
        ReceiptThread::Main
    };
    (receipt_type, thread, event_id)
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
        let mode = bridge.receipt_privacy.load(Ordering::SeqCst);
        let unthreaded = bridge.timelines.unthreaded_receipts();
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
            let answered = event_id.to_string();
            let event_id = in_room_receipt_target(&room, event_id).await;
            let (receipt_type, thread, event_id) = automatic_receipt(event_id, mode, unthreaded);
            let moved = event_id.as_str() != answered;
            let mut sent = room
                .send_single_receipt(receipt_type.clone(), thread.clone(), event_id)
                .await;
            if moved && sent.is_err() {
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_room_low_priority(
    ptr: *mut c_void,
    room_id: *const c_char,
    low_priority: c_int,
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
            let (result, attempts) = roomaction::set_low_priority(
                &room, low_priority != 0, &roomaction::ROOM_ACTION_RETRY_DELAYS).await;
            match result {
                Ok(()) => enqueue(&events, json!({
                    "type": "room_tag_confirmed",
                    "room_id": room_id,
                    "low_priority": low_priority != 0,
                })),
                Err(error) => {
                    let mut event = roomaction::failure_event("low_priority", &error, attempts);
                    event["room_id"] = json!(room_id);
                    enqueue(&events, event);
                }
            }
        });
        Ok(String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_receipt_privacy(
    ptr: *mut c_void,
    mode: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if bridge.is_background() {
            return Ok("error: background handle receipt privacy is fixed at create".to_owned());
        }
        let mode = if (0..=2).contains(&mode) { mode } else { 0 };
        bridge.receipt_privacy.store(mode, Ordering::SeqCst);
        Ok(String::new())
    })
}

pub(crate) fn pick_receipt_target(
    cache_newest: Option<OwnedEventId>,
    latest_event: Option<OwnedEventId>,
    fetched_newest: Option<OwnedEventId>,
) -> Option<OwnedEventId> {
    cache_newest.or(latest_event).or(fetched_newest)
}

pub(crate) struct TailEvent {
    pub id: OwnedEventId,
    pub kind: Option<String>,
    pub target: Option<OwnedEventId>,
    pub threaded: bool,
    /// An `m.poll.start` the timeline filter refused, so no row shows it.
    pub unrendered_start: bool,
}

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
        Some("m.reaction" | "org.matrix.msc3381.poll.end" | "m.poll.end") => {
            e.target.as_ref().is_some_and(known_main)
        }
        // matrix-sdk counts these as unread, and no timeline row ever shows them.
        Some("m.message") => !e.threaded,
        Some("m.poll.start") => e.unrendered_start && !e.threaded,
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
            let unrendered_start = kind.as_deref() == Some(stablepoll::START_TYPE)
                && !raw.deserialize().is_ok_and(|e| stablepoll::is_start(&e));
            Some(TailEvent { id, kind, target, threaded, unrendered_start })
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
            let mut failure: Option<(matrix_sdk::Error, u32)> = None;
            if let Some(event_id) = latest {
                let (result, attempts) = roomaction::with_transport_retry(delays, || {
                    let room = room.clone();
                    let receipts = receipts_for_mode(event_id.clone(), mode);
                    async move { room.send_multiple_receipts(receipts).await }
                }).await;
                if let Err(error) = result {
                    failure = Some((error, attempts));
                }
            }
            let (result, attempts) = roomaction::set_unread_flag(&room, false, delays).await;
            if let Err(error) = result {
                failure.get_or_insert((error, attempts));
            }
            match failure {
                None => {
                    enqueue(&events, json!({
                        "type": "read_marker_advanced", "room_id": room_id
                    }));
                    // One room, not the whole list: a bulk pass marks rooms one by one.
                    enqueue(&events, json!({
                        "type": "room_refreshed", "room": room_payload(&room).await
                    }));
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

fn is_latest_notification_target(
    targets: &Arc<Mutex<HashMap<String, u8>>>,
    room_id: &str,
    mode: u8,
) -> bool {
    targets.lock().ok().and_then(|guard| guard.get(room_id).copied()) == Some(mode)
}

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
        let target_guard = NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room_id.clone(),
            mode: my_mode,
        };
        bridge.spawn_room_action(async move {
            let _target_guard = target_guard;
            let _serial = serial.lock().await;
            if !is_latest_notification_target(&targets, &room_id, my_mode) {
                return;
            }
            let settings = notification_settings_handle(&settings_slot, &client).await;
            let result = settings.set_room_notification_mode(room.room_id(), mode).await;
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_poll_command_event(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_acknowledge_interrupted_setup(ptr: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Ok(bridge) = unsafe { bridge(ptr) } {
            recovery::acknowledge_interrupted_setup(&bridge.store_path);
        }
    }));
}

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
            let clamped: u64 = if limit == 0 { 30 } else {
                std::cmp::min(limit as u64, 200)
            };
            opts.limit = UInt::new(clamped).unwrap_or(uint!(30));

            match room.messages(opts).await {
                Ok(messages) => {
                    let mut total = 0u32;
                    let mut decrypted = 0u32;
                    let mut undecryptable_count = 0u32;
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

fn verification_sas_confirmed_event(flow_id: &str) -> serde_json::Value {
    json!({
        "type": "verification_sas_confirmed",
        "flow_id": flow_id,
    })
}

const VERIFICATION_POLL_MS: u64 = 500;
const SAS_HANDSHAKE_TICKS: u32 = 120;
const SAS_COMPLETION_TICKS: u32 = 240;
const SAS_COMPARE_TICKS: u32 = 1200;

struct SasTickBudget {
    left: u32,
    compare_armed: bool,
}

impl SasTickBudget {
    fn new() -> Self {
        Self { left: SAS_COMPLETION_TICKS, compare_armed: false }
    }

    fn tick(&mut self) -> bool {
        if self.left == 0 {
            return false;
        }
        self.left -= 1;
        true
    }

    fn emojis_shown(&mut self) {
        if !self.compare_armed {
            self.compare_armed = true;
            self.left = SAS_COMPARE_TICKS;
        }
    }
}
const VERIFICATION_PEER_TICKS: u32 = 600;
const VERIFICATION_CANCEL_TIMEOUT_SECS: u64 = 3;
const QR_DISPLAY_TICKS: u32 = 240;
const QR_COMPLETION_TICKS: u32 = 480;
const QR_MAX_MODULES: usize = 200;

fn advertised_verification_methods() -> Vec<VerificationMethod> {
    vec![
        VerificationMethod::SasV1,
        VerificationMethod::QrCodeShowV1,
        VerificationMethod::ReciprocateV1,
    ]
}

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

pub(crate) fn render_qr_bytes(bytes: &[u8]) -> Option<(usize, String)> {
    use matrix_sdk_base::crypto::matrix_sdk_qrcode::qrcode::{EcLevel, QrCode};
    let code = QrCode::with_error_correction_level(bytes, EcLevel::L).ok()?;
    let size = code.width();
    let modules: Vec<bool> =
        code.to_colors().into_iter().map(|c| c.select(true, false)).collect();
    pack_qr_modules(&modules, size).map(|bits| (size, bits))
}

fn render_qr_payload(qr: &QrVerification) -> Option<(usize, String)> {
    let code = qr.to_qr_code().ok()?;
    let size = code.width();
    let modules: Vec<bool> =
        code.to_colors().into_iter().map(|c| c.select(true, false)).collect();
    pack_qr_modules(&modules, size).map(|bits| (size, bits))
}

enum QrOutcome {
    Finished,
    FallBackToSas,
    ShuttingDown,
}

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
    let peer_can_scan = request
        .their_supported_methods()
        .is_some_and(|methods| methods.contains(&VerificationMethod::QrCodeScanV1));
    if !peer_can_scan {
        return None;
    }
    let qr = match request.generate_qr_code().await {
        Ok(Some(qr)) => qr,
        Ok(None) => return None,
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

        let state = qr.state();

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
            QrVerificationState::Confirmed => {
                if !emitted_confirmed {
                    emitted_confirmed = true;
                    enqueue(events, json!({
                        "type": "verification_qr_confirmed",
                        "flow_id": flow_id,
                    }));
                }
            }
            QrVerificationState::Reciprocated => {}
            QrVerificationState::Started => {
                if request_moved_to_sas(request) {
                    enqueue(events, json!({
                        "type": "verification_qr_dismissed",
                        "flow_id": flow_id,
                        "reason": "peer_started_sas",
                    }));
                    return QrOutcome::FallBackToSas;
                }
                display_ticks += 1;
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

        progress_ticks += 1;
        if progress_ticks >= QR_COMPLETION_TICKS {
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
            QrOutcome::FallBackToSas => release_keyed_slot(qr_slot, flow_id),
        }
    }
    drive_sas_flow(client, request, flow_id, events, sas_slot, nudges, shutdown).await;
}

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

fn sas_from_request(request: &VerificationRequest) -> Option<SasVerification> {
    match request.state() {
        VerificationRequestState::Transitioned { verification } => verification.sas(),
        _ => None,
    }
}

fn request_moved_to_sas(request: &VerificationRequest) -> bool {
    sas_from_request(request).is_some()
}

trait FlowLiveness {
    fn is_finished(&self) -> bool;
}

trait FlowIdentity {
    fn flow_key(&self) -> &str;
}

impl FlowLiveness for VerificationRequest {
    fn is_finished(&self) -> bool {
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

#[derive(Debug, Clone, PartialEq, Eq)]
enum PendingRequest {
    Waiting,
    Answered,
    Cancelled(String),
}

trait PendingRequestView: FlowIdentity {
    fn pending(&self) -> PendingRequest;
}

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

fn accept_without_request_reply(flow_id: &str) -> String {
    let flows = pending_cancelled_flows().lock().unwrap_or_else(|e| e.into_inner());
    if flows.iter().any(|flow| flow == flow_id) {
        String::new()
    } else {
        "error: no active verification request.".to_owned()
    }
}

const PENDING_REQUEST_WATCH_SECS: u64 = 11 * 60;

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

fn request_other_device(request: &VerificationRequest) -> (String, String) {
    match request.state() {
        VerificationRequestState::Requested { other_device_data, .. }
        | VerificationRequestState::Ready { other_device_data, .. } => (
            other_device_data.device_id().to_string(),
            other_device_data.display_name().unwrap_or_default().to_owned(),
        ),
        _ => (String::new(), String::new()),
    }
}

type KeyedFlowSlot<T> = Arc<Mutex<Option<(String, T)>>>;

fn keyed_slot_is_live<T: FlowLiveness>(slot: &KeyedFlowSlot<T>) -> bool {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(_, flow)| flow.is_finished()) {
            *guard = None;
        }
        return guard.is_some();
    }
    false
}

fn release_keyed_slot<T>(slot: &KeyedFlowSlot<T>, flow_id: &str) {
    if let Ok(mut guard) = slot.lock() {
        if guard.as_ref().is_some_and(|(stored, _)| stored == flow_id) {
            *guard = None;
        }
    }
}

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
    let sas_live = keyed_slot_is_live(sas_slot);
    let qr_live = keyed_slot_is_live(qr_slot);
    let request_live = request_slot.lock().map(|g| g.is_some()).unwrap_or(false);
    request_live || sas_live || qr_live
}

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
            Ok(None) => {
                let mut found: Option<SasVerification> = None;
                for _ in 0..SAS_HANDSHAKE_TICKS {
                    if shutdown.load(Ordering::SeqCst) {
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
            cancel_flow_best_effort(
                Some(&sas), None, Some(request),
                std::time::Duration::from_millis(SHUTDOWN_FLOW_CANCEL_MS),
            )
            .await;
            return;
        }
        tokio::time::sleep(poll).await;

        if let Some(current) = sas_from_request(request) {
            sas = current;
            if let Ok(mut guard) = sas_slot.lock() {
                *guard = Some((flow_id.to_owned(), sas.clone()));
            }
        }

        let needs_accept = {
            match sas.state() {
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_start_own_verification(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
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

            let mut ready = false;
            for _ in 0..VERIFICATION_PEER_TICKS {
                if shutdown.load(Ordering::SeqCst) {
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
                    VerificationRequestState::Ready { .. }
                    | VerificationRequestState::Transitioned { .. } => {
                        ready = true;
                        break;
                    }
                    VerificationRequestState::Done
                    | VerificationRequestState::Cancelled(_) => break,
                    VerificationRequestState::Created { .. }
                    | VerificationRequestState::Requested { .. } => {}
                }
            }
            if !ready {
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_query_own_device_status(
    ptr: *mut c_void,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Ok("error: Rust SDK session is not logged in.".to_owned());
        };
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

pub(crate) fn identity_key_agreement(
    local_base64: Option<&str>,
    published: Option<&str>,
) -> Option<bool> {
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
    keys.keys
        .iter()
        .find(|(id, _)| id.as_str().starts_with("curve25519:"))
        .map(|(_, value)| Some(value.to_owned()))
}

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

const OWN_IDENTITY_REFETCH_MIN: std::time::Duration = std::time::Duration::from_secs(20);

fn own_identity_refetches(
) -> &'static std::sync::Mutex<HashMap<OwnedUserId, (std::time::Instant, [bool; 6])>> {
    static REFETCHES: std::sync::OnceLock<
        std::sync::Mutex<HashMap<OwnedUserId, (std::time::Instant, [bool; 6])>>,
    > = std::sync::OnceLock::new();
    REFETCHES.get_or_init(Default::default)
}

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

fn own_identity_refetch_claim(user: &UserId, seen: [bool; 6]) -> bool {
    let now = std::time::Instant::now();
    let mut map = own_identity_refetches().lock().unwrap_or_else(|e| e.into_inner());
    if !own_identity_refetch_due(map.get(user).copied(), seen, now) {
        return false;
    }
    map.insert(user.to_owned(), (now, seen));
    true
}

fn own_identity_refetch_record(user: &UserId, left: [bool; 6]) {
    let mut map = own_identity_refetches().lock().unwrap_or_else(|e| e.into_inner());
    map.insert(user.to_owned(), (std::time::Instant::now(), left));
}

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
        let handle = bridge.runtime.spawn(async move {
                enqueue(&events, json!({
                    "type": "room_key_import_started",
                }));
                let path_buf = PathBuf::from(&file_path);
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_set_threads_in_live(
    ptr: *mut c_void,
    on: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.set_threads_in_live(on != 0);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_unthreaded_receipts(
    ptr: *mut c_void,
    on: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.set_unthreaded_receipts(on != 0);
        Ok(String::new())
    })
}

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
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_timeline_close(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        bridge.timelines.close();
        if let Ok(mut guard) = bridge.active_room_subscription.lock() {
            guard.close_active();
        }
        apply_room_subscription(bridge);
        Ok(String::new())
    })
}

fn apply_room_subscription(bridge: &RustClient) {
    let Some(service) = bridge
        .room_list_service
        .lock()
        .ok()
        .and_then(|guard| guard.clone())
    else {
        return;
    };
    let desired = bridge
        .active_room_subscription
        .lock()
        .ok()
        .and_then(|mut guard| guard.pending(false));
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_backup_action(
    ptr: *mut c_void,
    action: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let action = unsafe { cstr_arg(action) }?;
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
        let mints_key = matches!(action.as_str(), "enable" | "reset_key");
        bridge.spawn_reported_action("backup_action", async move {
            let marker = mints_key.then(|| recovery::SetupMarker::start(&store_path));
            let encryption = client.encryption();
            let recovery = encryption.recovery();
            let backups = encryption.backups();
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
                        "category": rooms::classify_room_error(&message),
                    }),
                ),
            }
        });
        Ok(String::new())
    })
}

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
            // The endpoint answers 404 on a server without MSC4306 and the SDK
            // maps that 404 to Ok(None), which is indistinguishable from "not
            // following". Ask the server's own feature list first, so a
            // server that lacks the MSC reports unsupported.
            if !thread_subscriptions_advertised(&room.client()).await {
                enqueue(&events, json!({
                    "type": "thread_subscription_state",
                    "room_id": room_id,
                    "thread_root_id": root,
                    "supported": false,
                    "subscribed": false,
                    "automatic": false,
                }));
                return;
            }
            match room.load_or_fetch_thread_subscription(&root_ref).await {
                Ok(subscription) => enqueue(&events, json!({
                    "type": "thread_subscription_state",
                    "room_id": room_id,
                    "thread_root_id": root,
                    "supported": true,
                    "subscribed": subscription.is_some(),
                    "automatic": subscription.map(|s| s.automatic).unwrap_or(false),
                })),
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

async fn thread_subscriptions_advertised(client: &Client) -> bool {
    match client.unstable_features().await {
        Ok(features) => timeline::server_supports_thread_subscriptions(&features),
        Err(_) => false,
    }
}

fn thread_subscription_failure_reason(err: &matrix_sdk::Error) -> &'static str {
    thread_subscription_reason_for_kind(err.client_api_error_kind())
}

fn thread_subscription_reason_for_kind(
    kind: Option<&matrix_sdk::ruma::api::error::ErrorKind>,
) -> &'static str {
    use matrix_sdk::ruma::api::error::ErrorKind;
    match kind {
        Some(ErrorKind::Unrecognized) => "unsupported_by_server",
        Some(ErrorKind::NotFound) => "not_found",
        Some(ErrorKind::LimitExceeded(_)) => "rate_limited",
        Some(ErrorKind::Forbidden) => "forbidden",
        _ => "failed",
    }
}

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
                room.subscribe_thread(root_ref.to_owned(), None).await
            } else {
                room.unsubscribe_thread(root_ref.to_owned()).await
            };
            enqueue(&events, json!({
                "type": "thread_subscription_result",
                "room_id": room_id,
                "thread_root_id": root,
                "ok": result.is_ok(),
                "reason": match &result {
                    Ok(_) => "",
                    Err(err) => thread_subscription_failure_reason(err),
                },
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

unsafe fn thread_root_arg(value: *const c_char) -> Result<String, String> {
    if value.is_null() {
        return Ok(String::new());
    }
    Ok(unsafe { cstr_arg(value) }?.trim().to_owned())
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_invite_policy_support(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        accountprefs::invite_policy_support(bridge).map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_settings_sync_read(ptr: *mut c_void, op_id: u64) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        settingsync::read(bridge, op_id).map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_fetch_packs(
    ptr: *mut c_void,
    room_id: *const c_char,
    deep: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        stickers::fetch_packs(bridge, op_id, room_id, deep).map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_migrate_user_pack(
    ptr: *mut c_void,
    start_new: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        stickers::migrate_user_pack(bridge, op_id, start_new).map(|_| String::new())
    })
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_stickers_upload_to_room_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    namespace: *const c_char,
    shortcode: *const c_char,
    body: *const c_char,
    local_path: *const c_char,
    reuse_url: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let namespace = pack_namespace(&unsafe { cstr_arg(namespace) }?)?;
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let body = unsafe { cstr_arg(body) }?;
        let path = unsafe { cstr_arg(local_path) }?;
        let reuse = unsafe { cstr_arg(reuse_url) }?;
        stickers::upload_to_room_pack(
            bridge, op_id, room_id, state_key, namespace, shortcode, body, path, reuse,
        )
        .map(|_| String::new())
    })
}

fn pack_namespace(text: &str) -> Result<stickers::Namespace, String> {
    stickers::Namespace::parse(text).ok_or_else(|| "unknown pack namespace".to_owned())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_create_room_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    name: *const c_char,
    usage: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let name = unsafe { cstr_arg(name) }?;
        let usage = unsafe { cstr_arg(usage) }?;
        stickers::create_room_pack(bridge, op_id, room_id, state_key, name, usage)
            .map(|_| String::new())
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

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_stickers_add_to_room_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    namespace: *const c_char,
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
        let namespace = pack_namespace(&unsafe { cstr_arg(namespace) }?)?;
        let shortcode = unsafe { cstr_arg(shortcode) }?;
        let url = unsafe { cstr_arg(url) }?;
        let body = unsafe { cstr_arg(body) }?;
        let mimetype = unsafe { cstr_arg(mimetype) }?;
        stickers::add_to_room_pack(
            bridge, op_id, room_id, state_key, shortcode, url, body, mimetype,
            width, height, size, namespace,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_set_room_pack_enabled(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    namespace: *const c_char,
    enabled: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let namespace = pack_namespace(&unsafe { cstr_arg(namespace) }?)?;
        stickers::set_room_pack_enabled(bridge, op_id, room_id, state_key, enabled, namespace)
            .map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_qr_login_generate(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        qrlogin::grant_generate(bridge).map(|gen| gen.to_string())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_stickers_edit_pack(
    ptr: *mut c_void,
    room_id: *const c_char,
    state_key: *const c_char,
    namespace: *const c_char,
    action: *const c_char,
    arg_a: *const c_char,
    arg_b: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let state_key = unsafe { cstr_arg(state_key) }?;
        let namespace_text = unsafe { cstr_arg(namespace) }?;
        let namespace = if room_id.is_empty() {
            stickers::Namespace::Legacy
        } else {
            pack_namespace(&namespace_text)?
        };
        let action = unsafe { cstr_arg(action) }?;
        let arg_a = unsafe { cstr_arg(arg_a) }?;
        let arg_b = unsafe { cstr_arg(arg_b) }?;
        let edit = match action.as_str() {
            "remove_image" => stickers::PackEdit::RemoveImage { key: arg_a },
            "rename_image" => {
                if arg_b.is_empty() {
                    return Err("rename needs a new shortcode".to_owned());
                }
                stickers::PackEdit::RenameImage { from: arg_a, to: arg_b }
            }
            "set_name" => stickers::PackEdit::SetName { name: arg_a },
            "set_usage" => stickers::PackEdit::SetUsage { usage: arg_a },
            "set_body" => stickers::PackEdit::SetBody { key: arg_a, body: arg_b },
            "delete_pack" => stickers::PackEdit::DeletePack,
            _ => return Err("unknown pack edit".to_owned()),
        };
        stickers::edit_pack(bridge, op_id, room_id, state_key, edit, namespace)
            .map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_presence(
    ptr: *mut c_void,
    state: c_uint,
    status_msg: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if bridge.is_background() {
            return Ok("error: background handle publishes no presence".to_owned());
        }
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_request_activity_seed_page(
    ptr: *mut c_void,
    limit: u32,
    from: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let token = unsafe { cstr_arg(from) }?;
        if token.is_empty() || token.len() > 4096 {
            return Err("invalid_page_token".to_owned());
        }
        rooms::request_activity_seed_page(bridge, limit, Some(token))
            .map(|_| String::new())
    })
}

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
        let private_room = encrypted_room || room.encryption_state().is_unknown();
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
                    let mut pending_edits = cursor.pending_edits.clone();
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
        let private_room = encrypted_room || room.encryption_state().is_unknown();
        bridge.spawn_media_fetch(op_id, async move {
            let mut state = roomexport::WalkState::new(range);
            let mut names: HashMap<String, String> = HashMap::new();
            let mut token: Option<String> = None;
            if encrypted_room {
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

fn ensure_search_index(bridge: &RustClient) -> Result<(), String> {
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
    restrict_store_permissions(&bridge.store_path);
    if let Ok(mut guard) = bridge.search_index.lock() {
        *guard = Some(index);
    }
    Ok(())
}

fn local_search_failure_event(op_id: u64) -> serde_json::Value {
    json!({
        "type": "local_search_result", "op_id": op_id, "ok": false,
        "category": "index_unavailable", "results": [],
    })
}

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
            eprintln!("local search: index unavailable: {err}");
            enqueue(&bridge.events, local_search_failure_event(op_id));
            return Err(err);
        }

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_forget_event(
    ptr: *mut c_void,
    event_id: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let event_id = unsafe { cstr_arg(event_id) }?;
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.remove_event(&event_id)?;
            }
        }
        Ok(String::new())
    })
}

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
        bridge.index_all.reset();
        if let Ok(guard) = bridge.search_index.lock() {
            if let Some(index) = guard.as_ref() {
                index.clear()?;
            }
        }
        Ok(String::new())
    })
}

const INDEX_ALL_STATS_EVERY_MS: u64 = 5_000;
const INDEX_ALL_BACKGROUND: c_uint = 2;

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
        "finished_ms": status.finished_ms,
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
    if !matches!(room.state(), matrix_sdk::RoomState::Joined)
        || !room.encryption_state().is_encrypted()
    {
        return indexall::RoomResult::Gone;
    }
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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_start(
    ptr: *mut c_void,
    mode: c_uint,
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
        let joined = move || {
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
        };
        let begin = match mode {
            INDEX_ALL_BACKGROUND => control.begin_background(joined),
            resume_only => control.begin(resume_only != 0, joined),
        };
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

fn refuse_index_all_after_teardown(bridge: &RustClient) -> Result<(), String> {
    if bridge.index_shutdown.load(Ordering::Relaxed) {
        return Err("the local index is closed for this session".to_owned());
    }
    Ok(())
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_search_index_all_hold(
    ptr: *mut c_void,
    bits: c_uint,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if bridge.is_background() {
            return Ok("error: background handle index hold is fixed at create".to_owned());
        }
        bridge.index_all.set_hold(bits & (indexall::HOLD_CALL | indexall::HOLD_SCROLL));
        Ok(String::new())
    })
}

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
    stream_metadata: *const c_char,
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
        let stream_metadata = unsafe { cstr_arg(stream_metadata) }?;
        calls::send_invite(
            bridge, room_id, call_id, party_id, offer_type, offer_sdp,
            lifetime_ms, invitee, stream_metadata, op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_answer(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    answer_type: *const c_char,
    answer_sdp: *const c_char,
    stream_metadata: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let answer_type = unsafe { cstr_arg(answer_type) }?;
        let answer_sdp = unsafe { cstr_arg(answer_sdp) }?;
        let stream_metadata = unsafe { cstr_arg(stream_metadata) }?;
        calls::send_answer(
            bridge, room_id, call_id, party_id, answer_type, answer_sdp,
            stream_metadata, op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_negotiate(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    description_type: *const c_char,
    sdp: *const c_char,
    lifetime_ms: u64,
    stream_metadata: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let description_type = unsafe { cstr_arg(description_type) }?;
        let sdp = unsafe { cstr_arg(sdp) }?;
        let stream_metadata = unsafe { cstr_arg(stream_metadata) }?;
        calls::send_negotiate(
            bridge, room_id, call_id, party_id, description_type, sdp,
            lifetime_ms, stream_metadata, op_id,
        )
        .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_stream_metadata(
    ptr: *mut c_void,
    room_id: *const c_char,
    call_id: *const c_char,
    party_id: *const c_char,
    stream_metadata: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let call_id = unsafe { cstr_arg(call_id) }?;
        let party_id = unsafe { cstr_arg(party_id) }?;
        let stream_metadata = unsafe { cstr_arg(stream_metadata) }?;
        calls::send_stream_metadata(
            bridge, room_id, call_id, party_id, stream_metadata, op_id,
        )
        .map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_calls_set_media_capable(
    ptr: *mut c_void,
    capable: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        if bridge.is_background() {
            return Ok("error: background handle is not media capable".to_owned());
        }
        bridge
            .call_media_capable
            .store(capable != 0, std::sync::atomic::Ordering::Relaxed);
        Ok(String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_connect_many(
    ptr: *mut c_void, connections_json: *const c_char,
    room_id: *const c_char, op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let raw = unsafe { cstr_arg(connections_json) }?;
        if raw.len() > 8192 { return Err("focus plan too large".to_owned()); }
        let rows: Vec<serde_json::Value> = serde_json::from_str(&raw)
            .map_err(|_| "invalid focus plan".to_owned())?;
        let connections = rows.iter().map(|row| {
            let id = row.get("id").and_then(|v| v.as_u64())
                .and_then(|v| u32::try_from(v).ok())
                .ok_or_else(|| "invalid connection id".to_owned())?;
            let url = row.get("url").and_then(|v| v.as_str())
                .ok_or_else(|| "invalid focus url".to_owned())?;
            Ok((id, url.to_owned()))
        }).collect::<Result<Vec<_>, String>>()?;
        let room_id = unsafe { cstr_arg(room_id) }?;
        sfu::connect_many(bridge, connections, room_id, op_id)
            .map(|_| String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_local_description_for(
    ptr: *mut c_void, generation: u64, connection_id: u32,
    kind: *const c_char, target: *const c_char, sdp: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let kind = unsafe { cstr_arg(kind) }?;
        let target = unsafe { cstr_arg(target) }?;
        let sdp = unsafe { cstr_arg(sdp) }?;
        if sdp.trim().is_empty() { return Err("empty sdp".to_owned()); }
        let target = sfu::target_from_str(&target);
        let command = match kind.as_str() {
            "offer" => sfu::SfuCommand::Offer { sdp, target },
            "answer" => sfu::SfuCommand::Answer { sdp, target },
            _ => return Err("invalid description kind".to_owned()),
        };
        sfu::send_command_for(bridge, generation, connection_id, command);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_local_candidate_for(
    ptr: *mut c_void, generation: u64, connection_id: u32,
    target: *const c_char, candidate_init: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let target = unsafe { cstr_arg(target) }?;
        let candidate_init = unsafe { cstr_arg(candidate_init) }?;
        sfu::send_command_for(bridge, generation, connection_id,
            sfu::SfuCommand::Candidate {
                candidate_init, target: sfu::target_from_str(&target),
            });
        Ok(String::new())
    })
}

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
            backup_cid: None,
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_add_track_for(
    ptr: *mut c_void, generation: u64, connection_id: u32,
    cid: *const c_char, name: *const c_char, kind: i32,
    width: u32, height: u32, screen_share: u8, encrypted: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let cid = unsafe { cstr_arg(cid) }?;
        let name = unsafe { cstr_arg(name) }?;
        sfu::send_command_for(bridge, generation, connection_id,
            sfu::SfuCommand::AddTrack {
                cid, name, width, height,
                kind: if kind == 1 { 1 } else { 0 },
                screen_share: screen_share != 0,
                encrypted: encrypted != 0,
                backup_cid: None,
            });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_add_share_backup_for(
    ptr: *mut c_void, generation: u64,
    cid: *const c_char, backup_cid: *const c_char,
    width: u32, height: u32, encrypted: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let cid = unsafe { cstr_arg(cid) }?;
        let backup = unsafe { cstr_arg(backup_cid) }?;
        if cid.is_empty() || cid.len() > 256 || backup.is_empty()
            || backup.len() > 256 || cid == backup
            || cid.chars().chain(backup.chars()).any(char::is_control)
        {
            return Err("invalid share codec cid".to_owned());
        }
        sfu::send_command_for(bridge, generation, 0, sfu::SfuCommand::AddTrack {
            cid, name: "screen".to_owned(), kind: 1, width, height,
            screen_share: true, encrypted: encrypted != 0,
            backup_cid: Some(backup),
        });
        Ok(String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_mute_track_for(
    ptr: *mut c_void, generation: u64, connection_id: u32,
    sid: *const c_char, muted: u8,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let sid = unsafe { cstr_arg(sid) }?;
        sfu::send_command_for(bridge, generation, connection_id,
            sfu::SfuCommand::MuteTrack { sid, muted: muted != 0 });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_sfu_disconnect(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        sfu::disconnect(bridge);
        Ok(String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_rtc_publish_membership(
    ptr: *mut c_void,
    room_id: *const c_char,
    focus_url: *const c_char,
    intent: *const c_char,
    multi_sfu: bool,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let room_id = unsafe { cstr_arg(room_id) }?;
        let focus_url = unsafe { cstr_arg(focus_url) }?;
        let intent = unsafe { cstr_arg(intent) }?;
        rtc::publish_membership(bridge, room_id, focus_url, intent, multi_sfu, op_id)
            .map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_space_child_order(
    ptr: *mut c_void,
    space_id: *const c_char,
    room_ids_json: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr) }?;
        let space_id = unsafe { cstr_arg(space_id) }?;
        let ids: Vec<String> = serde_json::from_str(&unsafe { cstr_arg(room_ids_json) }?)
            .map_err(|_| "invalid child list".to_owned())?;
        rooms::set_space_child_order(bridge, space_id, ids, op_id).map(|_| String::new())
    })
}

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

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mx_rust_send_spoiler_media(
    ptr: *mut c_void,
    room_id: *const c_char,
    root_event_id: *const c_char,
    local_path: *const c_char,
    data: *const u8,
    len: usize,
    filename: *const c_char,
    mime: *const c_char,
    caption: *const c_char,
    width: u64,
    height: u64,
    duration_ms: u64,
    animated: u8,
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
        let thread_root = if root.is_empty() {
            None
        } else {
            Some(
                matrix_sdk::ruma::OwnedEventId::try_from(root)
                    .map_err(|_| "Invalid thread root event id.".to_owned())?,
            )
        };
        let local_path = unsafe { cstr_arg(local_path) }?;
        let filename = unsafe { cstr_arg(filename) }?;
        let mime = unsafe { cstr_arg(mime) }?;
        let caption = unsafe { cstr_arg(caption) }?;
        spoiler::spoiler_msgtype(&mime)
            .ok_or_else(|| "Only images, videos and audio can be marked as spoilers.".to_owned())?;
        let (path, bytes, upload_bytes) = if local_path.is_empty() {
            if data.is_null() || len == 0 {
                return Err("attachment data is empty".to_owned());
            }
            (None, unsafe { std::slice::from_raw_parts(data, len) }.to_vec(), len as u64)
        } else {
            let size = std::fs::metadata(&local_path)
                .ok()
                .filter(|m| m.is_file() && m.len() > 0)
                .ok_or_else(|| "attachment file is not readable".to_owned())?
                .len();
            (Some(std::path::PathBuf::from(local_path)), Vec::new(), size)
        };
        let poster = unsafe { poster_arg(thumb_data, thumb_len, thumb_width, thumb_height) };
        let Some(client) = bridge.client.lock().ok().and_then(|g| g.clone()) else {
            return Err("Rust SDK session is not logged in.".to_owned());
        };
        let request = spoiler::SpoilerSend {
            path,
            data: bytes,
            spec: spoiler::SpoilerMediaSpec {
                filename,
                caption: Some(caption),
                mimetype: mime,
                size: 0,
                width,
                height,
                duration_ms,
                animated: animated != 0,
            },
            poster,
            thread_root,
            in_reply_to,
        };
        bridge
            .timelines
            .send_spoiler_media(&bridge.runtime, client, room_id, request, upload_bytes, op_id)
            .map(|_| String::new())
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_shutdown_tasks(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
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
pub unsafe extern "C" fn mx_rust_final_session_tokens(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        Ok(final_session_tokens_json(bridge.final_session_tokens()))
    })
}

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

#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_retain(ptr: *mut c_void) -> *mut c_void {
    catch_unwind(AssertUnwindSafe(|| match unsafe { bridge(ptr) } {
        Ok(bridge) => Arc::into_raw(Arc::clone(&bridge.token_ledger)) as *mut c_void,
        Err(_) => std::ptr::null_mut(),
    }))
    .unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_latest(ledger: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        if ledger.is_null() {
            return Ok(String::new());
        }
        let ledger = unsafe { &*(ledger as *const sessiontokens::TokenLedger) };
        Ok(final_session_tokens_json(ledger.latest()))
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_token_ledger_release(ledger: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !ledger.is_null() {
            drop(unsafe { Arc::from_raw(ledger as *const sessiontokens::TokenLedger) });
        }
    }));
}

#[no_mangle]
pub extern "C" fn mx_rust_labs_enable() {
    let _ = catch_unwind(labs::enable);
}

#[no_mangle]
pub extern "C" fn mx_rust_labs_active() -> c_int {
    c_int::from(labs::active())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_handle_serial(ptr: *mut c_void) -> u64 {
    catch_unwind(AssertUnwindSafe(|| unsafe { bridge(ptr) }.map_or(0, |b| b.serial)))
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn mx_rust_create_background(
    store_path: *const c_char,
    profile_json: *const c_char,
) -> *mut c_void {
    install_panic_hook();
    install_sdk_tracing();
    match catch_unwind(AssertUnwindSafe(|| {
        if !labs::active() {
            return Err("labs is not active".to_owned());
        }
        let store_path = unsafe { cstr_arg(store_path) }?;
        if store_path.trim().is_empty() {
            return Err("a background handle needs an account store".to_owned());
        }
        let profile = labs::parse_profile(&unsafe { cstr_arg(profile_json) }?)?;
        let path = PathBuf::from(store_path);
        let mut client = RustClient::new_with(path.clone(), Some(&profile))?;
        client.token_ledger.attach_custody(Arc::clone(&client.custody));
        client.receipt_privacy.store(profile.receipt_privacy, Ordering::SeqCst);
        client.index_all.set_hold(profile.index_all_hold);
        let state = Arc::new(bgsession::BgState::new(
            profile,
            Arc::clone(&client.client),
            &client.events,
            &client.command_events,
        ));
        bgsession::register(&client.events, &client.command_events, &state);
        client.bg = Some(state);
        Ok::<*mut c_void, String>(Box::into_raw(Box::new(client)) as *mut c_void)
    })) {
        Ok(Ok(ptr)) => ptr,
        Ok(Err(_)) | Err(_) => std::ptr::null_mut(),
    }
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct MxRustBgStatus {
    pub session_state_version: u64,
    pub token_generation: u64,
    pub notable_updates_total: u64,
    pub queued_events: c_uint,
    pub queued_commands: c_uint,
    pub dirty: c_int,
    pub revoked: c_int,
}

fn background_of(bridge: &RustClient) -> Result<&Arc<bgsession::BgState>, String> {
    bridge
        .bg
        .as_ref()
        .filter(|bg| bg.is_background())
        .ok_or_else(|| "not a background handle".to_owned())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_poll(ptr: *mut c_void, out: *mut MxRustBgStatus) -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        if out.is_null() {
            return -1;
        }
        let Ok(bridge) = (unsafe { bridge(ptr) }) else { return -1 };
        let Ok(bg) = background_of(bridge) else { return -1 };
        bridge.token_ledger.note_drain();
        bg.expire_session_state(std::time::Instant::now());
        let (queued_events, queued_commands) = bg.queue_lengths();
        let status = MxRustBgStatus {
            session_state_version: bg.session_state_version(),
            token_generation: bridge.custody.generation(),
            notable_updates_total: bg.notable_updates_total(),
            queued_events,
            queued_commands,
            dirty: c_int::from(bg.dirty()),
            revoked: c_int::from(bridge.custody.revoked()),
        };
        unsafe { out.write(status) };
        0
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_take(ptr: *mut c_void, max_rows: c_uint) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        Ok(background_of(bridge)?.take(max_rows))
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_take_session_state(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        Ok(background_of(bridge)?.take_session_state())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_set_local_rules(
    ptr: *mut c_void,
    rooms_json: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let bg = background_of(bridge)?;
        let rooms: Vec<String> = serde_json::from_str(&unsafe { cstr_arg(rooms_json) }?)
            .map_err(|_| "local rules must be a JSON array of room ids".to_owned())?;
        bg.set_local_rules(rooms);
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_set_send_queue_enabled(
    ptr: *mut c_void,
    enabled: c_int,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        background_of(bridge)?;
        let client = require_client_for_search(bridge)?;
        bridge.spawn_room_action(async move {
            client.send_queue().set_enabled(enabled != 0).await;
        });
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_bg_promote(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let bg = Arc::clone(background_of(bridge)?);
        let client = bridge
            .client
            .lock()
            .ok()
            .and_then(|guard| guard.clone())
            .ok_or_else(|| "the background handle has no session to adopt".to_owned())?;
        let user_id = client
            .user_id()
            .map(|user| user.to_string())
            .ok_or_else(|| "the background handle has no signed-in user".to_owned())?;
        let device_id = client.device_id().map(|device| device.to_string()).unwrap_or_default();
        bridge.stop_sync_and_wait();
        bridge.timelines.advance_lifecycle();
        bg.promote_to_foreground();
        bgsession::unregister(&bridge.events, &bridge.command_events);
        bridge.token_ledger.detach_custody();
        install_ui_event_handlers(&client, Arc::clone(&bridge.events));
        if let Ok(mut queue) = bridge.events.lock() {
            queue.clear();
        }
        if let Ok(mut queue) = bridge.command_events.lock() {
            queue.clear();
        }
        bridge.enqueue(json!({
            "type": "login_ok",
            "user_id": user_id,
            "device_id": device_id,
            "adopted": true,
        }));
        Ok(String::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_take_custody(ptr: *mut c_void) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let has_client = bridge.client.lock().map(|guard| guard.is_some()).unwrap_or(false);
        if !has_client {
            return Ok(String::new());
        }
        // The facts are read BEFORE the tokens (`custody_json`), so a rotation
        // landing between the two reads leaves the generation older than the
        // tokens, never newer (review 2026-10-16, finding 1). The ledger's
        // generation is read first of all, by the same rule: the host answers
        // `mx_rust_session_tokens_persisted` for it once the pair is written,
        // and the pair is then at least that new.
        let ledger_generation = bridge.token_ledger.generation();
        let mut custody = bridge.custody.custody_json(|| {
            bridge
                .final_session_tokens()
                .map(|tokens| (tokens.access_token, tokens.refresh_token))
        });
        if let Some(map) = custody.as_object_mut() {
            map.insert("ledger_generation".to_owned(), json!(ledger_generation));
        }
        Ok(custody.to_string())
    })
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_store_any_held(store_path: *const c_char) -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(path) = (unsafe { cstr_arg(store_path) }) else { return -1 };
        if path.trim().is_empty() {
            return -1;
        }
        storeclose::any_held_path(Path::new(&path))
    }))
    .unwrap_or(-1)
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

static STRICT_DEVICE_TRUST: AtomicBool = AtomicBool::new(false);

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

/// This handle's network route (route.rs), set before sign-in or restore
/// like the media-store key. Fail closed: input that does not parse is
/// recorded as `Unavailable`, so the store's client is never built direct.
/// The JSON may carry a proxy password: it is never logged or echoed.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_set_network_route(
    ptr: *mut c_void,
    route_json: *const c_char,
) -> *mut c_char {
    ffi_string(|| {
        let bridge = unsafe { bridge(ptr)? };
        let json = unsafe { cstr_arg(route_json) }.unwrap_or_default();
        match route::parse(&json) {
            Ok(state) => {
                route::set(ptr as usize, &bridge.store_path, state);
                Ok(String::new())
            }
            Err(code) => {
                route::set(ptr as usize, &bridge.store_path, Some(route::RouteState::Unavailable));
                Err(format!("network route not applied: {code}"))
            }
        }
    })
}

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
            enqueue(&events, json!({
                "type": "media_cache_cleared", "op_id": op_id,
                "ok": ok, "files": files, "bytes": bytes,
            }));
            if ok {
                let result =
                    aboutinfo::compact_media_store(&client, &store_path, store_before).await;
                aboutinfo::report_compaction(&events, op_id, "media_store", result);
            }
        });
        Ok(String::new())
    })
}

pub(crate) async fn resume_unsent_requests(client: &Client) {
    client.send_queue().respawn_tasks_for_rooms_with_unsent_requests().await;
}

enum HomeserverInput<'a> {
    Discover(&'a str),
    Url(&'a str),
}

const RESOLVED_HOMESERVER_FILE: &str = "lightning-homeserver-url";

fn resolved_homeserver_path(store_path: &Path) -> PathBuf {
    store_path.join(RESOLVED_HOMESERVER_FILE)
}

fn read_resolved_homeserver(store_path: &Path) -> Option<String> {
    if store_path.as_os_str().is_empty() {
        return None;
    }
    let text = std::fs::read_to_string(resolved_homeserver_path(store_path)).ok()?;
    let text = text.trim();
    let url = url::Url::parse(text).ok()?;
    if !matches!(url.scheme(), "http" | "https") || url.host().is_none() {
        return None;
    }
    Some(url.to_string())
}

fn record_resolved_homeserver(store_path: &Path, url: &str) {
    if store_path.as_os_str().is_empty() {
        return;
    }
    let path = resolved_homeserver_path(store_path);
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

pub(crate) async fn build_memory_client(homeserver: &str, route_of: &Path) -> Result<Client, String> {
    build_client_full(HomeserverInput::Discover(homeserver), Path::new(""), route_of).await
}

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
    build_client_full(homeserver, store_path, store_path).await
}

async fn build_client_full(
    homeserver: HomeserverInput<'_>,
    store_path: &Path,
    route_of: &Path,
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
    let discovering = matches!(homeserver, HomeserverInput::Discover(_));
    let base = match homeserver {
        HomeserverInput::Discover(value) => base.server_name_or_homeserver_url(value),
        HomeserverInput::Url(value) => base.homeserver_url(value),
    };
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
    // Labs: a Background handle builds with ITS strict-trust snapshot and
    // bounded SDK retries, so the SDK's own 15-minute retry cannot hide an
    // outage from the scheduler (design §3.2.11, R2-M1). Every other build
    // reads the global exactly as before.
    let labs_context = labs::open_context();
    if let Some((retry_limit, max_retry_ms)) =
        labs_context.as_ref().and_then(|context| context.request_limits)
    {
        builder = builder.request_config(
            matrix_sdk::config::RequestConfig::new()
                .retry_limit(retry_limit)
                .max_retry_time(std::time::Duration::from_millis(max_retry_ms)),
        );
    }
    let strict_trust = labs_context
        .as_ref()
        .and_then(|context| context.strict_trust)
        .unwrap_or_else(|| STRICT_DEVICE_TRUST.load(Ordering::SeqCst));
    if strict_trust {
        builder = builder
            .with_room_key_recipient_strategy(
                matrix_sdk_base::crypto::CollectStrategy::IdentityBasedStrategy,
            )
            .with_decryption_settings(matrix_sdk_base::crypto::DecryptionSettings {
                sender_device_trust_requirement:
                    matrix_sdk_base::crypto::TrustRequirement::CrossSignedOrLegacy,
            });
    }
    if let Some(http) = route::client_for(route_of).map_err(str::to_owned)? {
        builder = builder.http_client(http);
    }
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
    if let Some(mut probe) = store_probe {
        probe.attach_client(&client);
        storeclose::register(store_path, probe);
    }
    client.send_queue().enable_upload_progress(true);
    sendrecovery::spawn(client.clone(), sendrecovery::RecoveryPolicy::production());
    // 0600 on the databases just created, from the one place all login paths
    // pass through: the SDK sqlite files, their -wal/-shm siblings and the
    // search index are created here at the process umask. Best effort.
    if !store_path.as_os_str().is_empty() {
        restrict_store_permissions(store_path);
        record_resolved_homeserver(store_path, client.homeserver().as_str());
    }
    let policy = rooms::media_retention_policy();
    let preserve_caches = mediastore::preserve_caches_for_audit();
    let policy = if preserve_caches {
        policy.with_cleanup_frequency(None)
    } else {
        policy
    };
    if client.media().set_media_retention_policy(policy).await.is_ok()
        && !preserve_caches
    {
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
    let session = MatrixSession {
        meta: SessionMeta { user_id, device_id },
        tokens: SessionTokens { access_token, refresh_token },
    };
    restore_client_with_session(homeserver, store_path, session, events, token_sink).await
}

const RESTORE_BUILD_BUDGET: std::time::Duration = std::time::Duration::from_secs(10);

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
        return Err(reason);
    };
    let client = switchtrace::timed(
        Some(switchtrace::RestoreStep::FallbackBuild),
        build_client_with(HomeserverInput::Url(&url), store_path),
    )
        .await
        .map_err(|fallback| format!("{reason}; restoring from the recorded homeserver URL failed too: {fallback}"))?;
    enqueue(events, json!({ "type": "session_restored_offline" }));
    switchtrace::note_offline_fallback();
    Ok(client)
}

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

pub(crate) fn message_is_voice(msgtype: &MessageType) -> bool {
    matches!(msgtype, MessageType::Audio(content) if content.voice.is_some())
}

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

fn unsigned_age_ms(age: Option<matrix_sdk::ruma::Int>) -> Option<u64> {
    age.map(|age| i64::from(age).max(0) as u64)
}

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

async fn cached_event_ts(room: &Room, event_id: &EventId) -> Option<u64> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    event.raw().get_field::<u64>("origin_server_ts").ok().flatten()
}

async fn current_fully_read(room: &Room) -> Option<OwnedEventId> {
    use matrix_sdk::ruma::events::fully_read::FullyReadEventContent;
    let raw = room.account_data_static::<FullyReadEventContent>().await.ok()??;
    raw.deserialize().ok().map(|event| event.content.event_id)
}

fn fully_read_would_move_back(current_ts: Option<u64>, new_ts: Option<u64>) -> bool {
    matches!((current_ts, new_ts), (Some(current), Some(new)) if new < current)
}

async fn cached_event_sender(room: &Room, event_id: &EventId) -> Option<OwnedUserId> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    event.raw().get_field::<OwnedUserId>("sender").ok().flatten()
}

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

fn room_message_event_json(
    content: &RoomMessageEventContent,
    event_id: &EventId,
    sender: &UserId,
    timestamp_ms: u64,
    own_user_id: &UserId,
    is_encrypted: bool,
) -> Option<serde_json::Value> {
    use matrix_sdk::ruma::events::room::message::Relation;
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
    let kind = match typed_message_row_kind(row_msgtype.msgtype()) {
        Some(kind) => kind,
        None if matches!(row_msgtype, MessageType::VerificationRequest(_)) => return None,
        None => "text",
    };
    let body = match &gallery {
        Some(g) => g.caption.clone(),
        None if matches!(kind, "text" | "notice" | "emote") => {
            crate::timeline::strip_reply_fallback(content.body()).to_owned()
        }
        None => content.body().to_owned(),
    };
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

    let (mentions_me, mentions_room) = match &content.mentions {
        Some(mentions) => (mentions.user_ids.contains(own_user_id), mentions.room),
        None => (false, false),
    };
    let thread_root_id = match &content.relates_to {
        Some(Relation::Thread(thread)) => thread.event_id.to_string(),
        _ => String::new(),
    };
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
        "decrypted": is_encrypted,
    }))
}

fn trace_remote_edit(stage: &str, event_id: &str, target: &str, encrypted: bool) {
    if !target.is_empty() && std::env::var_os("LIGHTNING_EDIT_TRACE").is_some() {
        eprintln!("edit-trace stage={stage} edit={} target={} encrypted={encrypted}",
            redact_matrix_id(event_id), redact_matrix_id(target));
    }
}

struct PollStart {
    question: String,
    thread_root_id: String,
    in_reply_to: String,
}

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
    let verdict = match cached.push_actions() {
        Some(actions) => verdicts.verdict(room, actions, raw.json()).await,
        None => match room.push_context().await.ok().flatten() {
            Some(context) => Some(push_verdict::PushVerdict::from_actions(
                &context.for_event(&raw).await,
            )),
            None => verdicts.without_sdk_context(room, raw.json()).await,
        },
    };
    push_verdict::PushVerdict::write_room_mention(
        verdicts.room_mention_rule_matched(room, raw.json(), verdict).await,
        &mut event,
    );
    push_verdict::PushVerdict::write(verdict, &mut event);
    Some(event)
}

fn install_ui_event_handlers(client: &Client, events: Arc<Mutex<VecDeque<String>>>) {
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
                            return;
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

    let profile_events = Arc::clone(&events);
    client.add_event_handler(move |ev: SyncRoomMemberEvent, _room: Room| {
        let events = Arc::clone(&profile_events);
        async move {
            let user = match &ev {
                SyncStateEvent::Original(original) => original.state_key.to_string(),
                SyncStateEvent::Redacted(redacted) => redacted.state_key.to_string(),
            };
            enqueue(&events, json!({
                "type": "member_profile_changed",
                "user_id": user,
            }));
        }
    });

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

    backdrop::install_change_handler(client, Arc::clone(&events));

    bgsync::install_change_handlers(client, Arc::clone(&events));
    settingsync::install_change_handlers(client, Arc::clone(&events));

    accountprefs::install_change_handlers(client, Arc::clone(&events));

    stickers::install_change_handlers(client, Arc::clone(&events));

    serveradmin::install_deleted_room_handler(client, Arc::clone(&events));

    iotrace::start_if_enabled(Arc::clone(&events));

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
}

fn install_event_handlers(
    client: &Client,
    events: Arc<Mutex<VecDeque<String>>>,
    active_request: Arc<Mutex<Option<VerificationRequest>>>,
    active_sas: KeyedFlowSlot<SasVerification>,
    active_qr: KeyedFlowSlot<QrVerification>,
    sync_notify: Arc<syncnotify::SyncNotify>,
) {
    let background = bgsession::for_queue(&events).is_some();
    // Warm account adoption (live 2026-10-26): `EventCache::subscribe()`
    // spawns the one task that stores sync responses in the event cache, ONCE
    // per Client, on whatever runtime calls it first. For a Background handle
    // that was the sync thread's own runtime (RoomListService::new), so when a
    // promotion stopped that loop the listener died with it and could never
    // be spawned again: the adopted session's room list moved while an open
    // timeline never saw a new event. Subscribing here, on the handle's
    // shared multi-thread runtime (every caller runs inside run_async_on with
    // it), keeps the listener alive for the Client's life. Foreground handles
    // keep today's order.
    if background {
        if let Err(err) = client.event_cache().subscribe() {
            enqueue(&events, json!({
                "type": "error",
                "message": format!("event cache subscribe failed: {err}"),
            }));
        }
    }
    let utd_events = Arc::clone(&events);
    let utd_counts: Arc<Mutex<HashMap<OwnedUserId, u64>>> =
        Arc::new(Mutex::new(HashMap::new()));
    client.add_event_handler(move |ev: ToDeviceRoomEncryptedEvent| {
        let events = Arc::clone(&utd_events);
        let counts = Arc::clone(&utd_counts);
        async move {
            let count = {
                let Ok(mut guard) = counts.lock() else { return };
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

                let (other_device_id, other_device_name) = request_other_device(&request);

                if bgsession::for_queue(&events).is_some() {
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
                    return;
                }

                if flow_slots_are_live(&slot, &sas_slot, &qr_slot) {
                    let _ = request.cancel().await;
                    return;
                }

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
                tokio::spawn(watch_pending_request(Arc::clone(&slot), Arc::clone(&events), watched_flow));
            }
        },
    );

    if background {
        let cancel_events = Arc::clone(&events);
        client.add_event_handler(
            move |ev: matrix_sdk::ruma::events::key::verification::cancel::ToDeviceKeyVerificationCancelEvent| {
                let events = Arc::clone(&cancel_events);
                async move {
                    if bgsession::for_queue(&events).is_none() {
                        return;
                    }
                    enqueue(
                        &events,
                        json!({
                            "type": "verification_cancelled",
                            "flow_id": ev.content.transaction_id.to_string(),
                            "other_user_id": ev.sender.to_string(),
                            "message": ev.content.code.as_str(),
                        }),
                    );
                }
            },
        );
    }

    if !background {
        install_ui_event_handlers(client, Arc::clone(&events));
    }

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
                let verdict = verdicts.verdict(&room, &push_actions, &raw).await;
                push_verdict::PushVerdict::write_room_mention(
                    verdicts.room_mention_rule_matched(&room, &raw, verdict).await,
                    &mut event,
                );
                push_verdict::PushVerdict::write(verdict, &mut event);
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
                let (replaces_event_id, thread_root_id) = encrypted_relation_ids(
                    ev.content.relates_to.as_ref());
                trace_remote_edit("undecryptable", ev.event_id.as_str(), &replaces_event_id, true);
                let mut event = json!({
                    "event_id": ev.event_id.to_string(),
                    "sender": ev.sender.to_string(),
                    "body": "",
                    "msgtype": "encrypted",
                    "timestamp_ms": u64::from(ev.origin_server_ts.get()),
                    "is_encrypted": true,
                    "is_decrypted": false,
                    "undecryptable": true,
                    "error_kind": "no_key",
                    "replaces_event_id": replaces_event_id,
                    "thread_root_id": thread_root_id,
                    "decrypted": false,
                });
                let backlog = mark_sync_backlog(
                    &notify,
                    &mut event,
                    u64::from(ev.origin_server_ts.get()),
                    unsigned_age_ms(ev.unsigned.age),
                );
                notify.remember_utd(room.room_id(), &ev.event_id, backlog);
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

/// Matches the C++ background index-all delay: start-up work goes first.
const GAP_FILL_START_DELAY: std::time::Duration = std::time::Duration::from_secs(60);

/// New room keys may make a room index-all skipped for undecryptable history
/// readable now; the background start revisits only such rooms.
async fn watch_room_keys_for_index_all(client: Client, index_all: Arc<indexall::Control>) {
    let Some(stream) = client.encryption().room_keys_received_stream().await else {
        return;
    };
    let mut stream = std::pin::pin!(stream);
    while let Some(update) = stream.next().await {
        if let Ok(keys) = update {
            index_all.keys_arrived(keys.iter().map(|k| k.room_id.to_string()));
        }
    }
}

async fn run_authoritative_sync(
    client: Client,
    events: Arc<Mutex<VecDeque<String>>>,
    search_index: Arc<Mutex<Option<localsearch::SearchIndex>>>,
    index_all: Arc<indexall::Control>,
    index_shutdown: Arc<AtomicBool>,
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
    let background = bgsession::for_queue(&events);
    let _call_guards = background.is_none().then(|| calls::register_handlers(
        &client, &events, &timelines, &call_media_capable, &active_subscription,
        &room_list_slot));
    let _rtc_guards = background
        .is_none()
        .then(|| rtc::register_rtc_handlers(&client, &events, &timelines));
    let _aggregator = background.as_ref().map(|bg| {
        let task = tokio::spawn(bgsession::run_aggregator(client.clone(), Arc::clone(bg)));
        bg.set_aggregator(Some(task.abort_handle()));
        AbortOnDrop(task.abort_handle())
    });
    let mut jitter = background.as_ref().map(|_| labs::Jitter::seeded(0x5eed));
    let mut probe_failures = 0u32;
    // The local search index holds decrypted plaintext by exception (§6), on
    // condition that a redaction removes the row. Redactions reach C++ only as
    // a row flag, so this is handled here at the source; the sweep re-reads the
    // event cache, so the removal must hold on the Rust side.
    let _redaction_guard = localsearch::register_redaction_handler(
        &client, &search_index);
    let _live_index_guard = localsearch::register_live_index_handler(
        &client, &search_index);
    // Aborted when this sync ends, which a sign-out or switch does first.
    let _gap_filler = background.is_none().then(|| {
        let hold = Arc::clone(&index_all);
        AbortOnDrop(
            localsearch::spawn_gap_filler(
                &client,
                &search_index,
                localsearch::GapGate {
                    start_delay: GAP_FILL_START_DELAY,
                    held: Arc::new(move || hold.held()),
                    stop: Arc::clone(&index_shutdown),
                },
                &events,
            )
            .abort_handle(),
        )
    });
    let _key_watch = background.is_none().then(|| {
        AbortOnDrop(tokio::spawn(watch_room_keys_for_index_all(
            client.clone(), Arc::clone(&index_all))).abort_handle())
    });

    set_sync_mode(&sync_mode, &events, SyncMode::Probing, None);

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
                let pause = match jitter.as_mut() {
                    Some(jitter) => jitter.backoff(probe_failures),
                    None => std::time::Duration::from_secs(3),
                };
                probe_failures = probe_failures.saturating_add(1);
                tokio::select! {
                    _ = &mut cancel => return,
                    _ = tokio::time::sleep(pause) => {}
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
        let views = Arc::clone(&timelines);
        let generation = Arc::clone(&timelines);
        repair_classic_event_cache_once(
            &client,
            &events,
            move || !views.any_view_open(),
            move || generation.lifecycle(),
            push_verdict::now_ms(),
        )
        .await;
        run_classic_sync(client, events, verification_live, sync_notify, cancel).await;
    }
}

struct AbortOnDrop(tokio::task::AbortHandle);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        self.0.abort();
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

fn authentication_retry_category(error: &UnifiedSyncError) -> &'static str {
    match unified_error_kind(error) {
        Some(ErrorKind::Forbidden) => "authentication_retry_forbidden",
        _ => "authentication_retry_unknown_token",
    }
}

const MODERN_AUTH_RETRY_LIMIT: u32 = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ModernAuthVerdict {
    Retry,
    Fatal,
}

#[derive(Debug, Default)]
pub(crate) struct AuthRetryBudget {
    consecutive: u32,
}

impl AuthRetryBudget {
    pub(crate) fn on_auth_error(&mut self) -> ModernAuthVerdict {
        self.consecutive = self.consecutive.saturating_add(1);
        if self.consecutive > MODERN_AUTH_RETRY_LIMIT {
            ModernAuthVerdict::Fatal
        } else {
            ModernAuthVerdict::Retry
        }
    }

    pub(crate) fn on_running(&mut self) {
        self.consecutive = 0;
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ClassicSyncFault {
    Fatal,
    Transient,
}

pub(crate) fn classify_classic_sync_error(kind: Option<&ErrorKind>) -> ClassicSyncFault {
    match kind {
        Some(ErrorKind::UnknownToken { .. }) | Some(ErrorKind::Forbidden) =>
            ClassicSyncFault::Fatal,
        _ => ClassicSyncFault::Transient,
    }
}

pub(crate) fn classic_sync_backoff(consecutive_failures: u32) -> std::time::Duration {
    const CEILING_SECS: u64 = 30;
    let secs = 1u64 << consecutive_failures.min(5);
    std::time::Duration::from_secs(secs.min(CEILING_SECS))
}

const CLASSIC_SYNC_REPORT_AFTER: u32 = 5;

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
    sync_notify.catchup.begin(push_verdict::now_ms());
    let background = bgsession::for_queue(&events);
    let mut jitter = background.as_ref().map(|_| labs::Jitter::seeded(0x0dd5));
    let mut rebuilds = 0u32;
    let mut watch_stamps: HashMap<OwnedRoomId, u64> = HashMap::new();
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

    let first_response = Arc::new(AtomicBool::new(true));
    let watchdog_events = Arc::clone(&events);
    let watchdog_first = Arc::clone(&first_response);
    let watchdog = async move {
        watch_first_sync_response(&watchdog_events, &watchdog_first,
                                  FIRST_SYNC_STALL_STEPS).await;
        std::future::pending::<()>().await
    };
    tokio::pin!(watchdog);

    let mut auth_budget = AuthRetryBudget::default();
    let mut preview_backfill = previewbackfill::Backfill::default();
    let backfill_results = client
        .user_id()
        .filter(|_| background.is_none())
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
                    _ = tokio::time::sleep(retry_pause(&mut jitter, &mut rebuilds)) => continue,
                }
            }
        };

        let room_list_service = service.room_list_service();
        let room_list = match room_list_service.all_rooms().await {
            Ok(list) => list,
            Err(_) => {
                enqueue(&events, json!({ "type": "room_list_error", "category": "setup" }));
                enqueue(&events, json!({
                    "type": "room_list_sync_state", "state": "offline"
                }));
                tokio::select! {
                    _ = &mut cancel => return None,
                    _ = tokio::time::sleep(retry_pause(&mut jitter, &mut rebuilds)) => {
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
        entries_publication.set(Some(Arc::new(controller)));
        let mut forwarded_order: Vec<OwnedRoomId> = Vec::new();
        let mut watch_order: Vec<OwnedRoomId> = Vec::new();

        let space_service = SpaceService::new(client.clone()).await;
        let mut unified_state = service.state();
        let mut list_state = room_list_service.state();
        let mut list_running = false;
        let mut ignore_list_sub = client.subscribe_to_ignore_user_list_changes();
        let mut ignored_before = ignore_list_sub.get();
        let latest_events = client.latest_events().await;
        let mut watched_latest: BTreeSet<OwnedRoomId> = BTreeSet::new();
        let mut room_updates_sub = client.subscribe_to_all_room_updates();
        let mut room_updates_live = background.is_none();
        let mut activity_stamps: HashMap<OwnedRoomId, u64> = HashMap::new();

        publication.set(Some(Arc::clone(&room_list_service)));
        let resubscribe = |service: Arc<RoomListService>| {
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
        let mut backfill_tasks: tokio::task::JoinSet<(OwnedRoomId, previewbackfill::Outcome)> =
            tokio::task::JoinSet::new();
        let mut last_backfill_scan: Option<std::time::Instant> = None;
        loop {
            tokio::select! {
                _ = &mut cancel => {
                    service.stop().await;
                    return None;
                }
                _ = &mut watchdog => {}
                batch = entries.next() => {
                    let Some(batch) = batch else { break; };
                    if let Some(bg) = &background {
                        watch_room_list_diffs(batch, latest_events, &mut watched_latest,
                                              &mut watch_stamps, &mut watch_order,
                                              bg.watch_cap()).await;
                        bg.note_skipped();
                        continue;
                    }
                    forward_room_list_diffs(&events, batch, latest_events,
                                            &mut watched_latest,
                                            &mut forwarded_order).await;
                    enqueue_spaces(&events, &space_service, &client).await;
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
                                let (preview, ts, utd) = match &what {
                                    previewbackfill::Remembered::Message(p, t) => (p.clone(), *t, false),
                                    previewbackfill::Remembered::Undecryptable(t, _) => (String::new(), *t, true),
                                };
                                results.remember(&room_id, what);
                                let (preview, preview_kind, preview_arg) = preview_wire(&preview);
                                enqueue(&events, json!({
                                    "type": "room_preview_backfill",
                                    "room_id": room_id.to_string(),
                                    "preview": preview,
                                    "preview_kind": preview_kind,
                                    "preview_arg": preview_arg,
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
                    if !first_sync
                        && last_backfill_scan
                            .map_or(true, |at| at.elapsed() >= std::time::Duration::from_secs(3))
                    {
                        last_backfill_scan = Some(std::time::Instant::now());
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
                        Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {}
                        Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                            room_updates_live = false;
                        }
                    }
                }
                changed = ignore_list_sub.next() => {
                    if let Some(users) = changed {
                        let delta = previewbackfill::ignore_list_delta(&ignored_before, &users);
                        ignored_before = users.clone();
                        let carried = match &backfill_results {
                            Some(results) => carry_previews_across_ignore_change(
                                &client, results, &mut preview_backfill, &delta,
                            ).await,
                            None => (0, 0, 0),
                        };
                        enqueue(&events, json!({
                            "type": "ignored_users_changed",
                            "users": users,
                            "previews_kept": carried.0,
                            "previews_refreshed": carried.1,
                            "previews_blank": carried.2,
                        }));
                    }
                }
                state = list_state.next() => {
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
                        if !list_running {
                            list_running = true;
                            rebuilds = 0;
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
                            if unsupported_modern_error(&error) { return Some(cancel); }
                            if authentication_error(&error) {
                                if auth_budget.on_auth_error() == ModernAuthVerdict::Fatal {
                                    set_sync_mode(&sync_mode, &events, SyncMode::Failed, None);
                                    enqueue(&events, json!({
                                        "type": "room_list_error", "category": "authentication"
                                    }));
                                    let _ = (&mut cancel).await;
                                    return None;
                                }
                                enqueue(&events, json!({
                                    "type": "room_list_error",
                                    "category": authentication_retry_category(&error)
                                }));
                            }
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
        entries_publication.set(None);
        tokio::select! {
            _ = &mut cancel => return None,
            _ = tokio::time::sleep(retry_pause(&mut jitter, &mut rebuilds)) => {
                enqueue(&events, json!({
                    "type": "room_list_sync_state", "state": "retrying"
                }));
            }
        }
    }
}

const FIRST_SYNC_STALL_STEPS: &[std::time::Duration] = &[
    std::time::Duration::from_secs(60),
    std::time::Duration::from_secs(180),
    std::time::Duration::from_secs(600),
];

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
            return;
        }
        enqueue(events, json!({
            "type": "sync_stalled",
            "phase": "first_response",
            "waited_secs": waited.as_secs(),
        }));
    }
}

const SYNC_PROBE_BUDGET: std::time::Duration = std::time::Duration::from_secs(45);

#[derive(Debug, Clone, Copy)]
struct ClassicSilenceBudget {
    report_after: std::time::Duration,
    restart_after: std::time::Duration,
    resume_gap: std::time::Duration,
}

const CLASSIC_SYNC_SILENCE: ClassicSilenceBudget = ClassicSilenceBudget {
    report_after: std::time::Duration::from_secs(120),
    restart_after: std::time::Duration::from_secs(300),
    resume_gap: std::time::Duration::from_secs(15),
};

const CLASSIC_SYNC_WEDGED_MESSAGE: &str =
    "Sync stopped hearing from the server and was restarted.";

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
            reported = false;
        }
        let next = if quiet < budget.report_after {
            budget.report_after - quiet
        } else {
            budget.restart_after - quiet
        };
        tokio::time::sleep(next.min(std::time::Duration::from_secs(1))).await;
    }
}

const CLASSIC_SYNC_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);
const CLASSIC_SYNC_TIMEOUT_VERIFYING: std::time::Duration = std::time::Duration::from_secs(3);

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

const CLASSIC_CACHE_REPAIR_KEY: &[u8] = b"lightning.event_cache.classic_gap_repair.v1";
const CLASSIC_CACHE_REPAIR_PROGRESS_KEY: &[u8] =
    b"lightning.event_cache.classic_gap_repair.progress.v1";
const CLASSIC_CACHE_REPAIR_MAX_STARTS: u32 = 5;
const CLASSIC_CACHE_REPAIR_MAX_AGE_MS: u64 = 14 * 24 * 60 * 60 * 1000;

#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct ClassicRepairProgress {
    starts: u32,
    first_ms: u64,
    done: BTreeSet<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ClassicCacheRepair {
    Done { cleared: usize, forced: bool },
    Partial { cleared: usize, waiting: usize },
    AlreadyDone,
    Deferred,
    Failed,
}

fn classic_repair_must_finish(progress: &ClassicRepairProgress, now_ms: u64) -> bool {
    progress.starts >= CLASSIC_CACHE_REPAIR_MAX_STARTS
        || now_ms.saturating_sub(progress.first_ms) >= CLASSIC_CACHE_REPAIR_MAX_AGE_MS
}

fn room_unread_would_be_lost(
    encrypted: bool,
    client_messages: u64,
    client_notifications: u64,
    client_mentions: u64,
    server_highlights: u64,
) -> bool {
    client_messages > 0
        || client_mentions > 0
        || server_highlights > 0
        || (encrypted && client_notifications > 0)
}

async fn repair_classic_event_cache_once(
    client: &Client,
    events: &Arc<Mutex<VecDeque<String>>>,
    views_idle: impl Fn() -> bool,
    lifecycle: impl Fn() -> u64,
    now_ms: u64,
) -> ClassicCacheRepair {
    let started_lifecycle = lifecycle();
    let line = |text: String| {
        enqueue(events, json!({ "type": "rust_trace", "line": text }));
    };
    let store = client.state_store();
    match store.get_custom_value(CLASSIC_CACHE_REPAIR_KEY).await {
        Ok(Some(_)) => return ClassicCacheRepair::AlreadyDone,
        Ok(None) => {}
        Err(_) => {
            line("classic sync: event cache repair skipped (state store unreadable)".into());
            return ClassicCacheRepair::Failed;
        }
    }
    let mut progress: ClassicRepairProgress =
        match store.get_custom_value(CLASSIC_CACHE_REPAIR_PROGRESS_KEY).await {
            Ok(Some(bytes)) => serde_json::from_slice(&bytes).unwrap_or_default(),
            Ok(None) => ClassicRepairProgress::default(),
            Err(_) => {
                line("classic sync: event cache repair skipped (state store unreadable)".into());
                return ClassicCacheRepair::Failed;
            }
        };
    if progress.starts == 0 {
        progress.first_ms = now_ms;
    }
    progress.starts = progress.starts.saturating_add(1);
    let save = |progress: ClassicRepairProgress| async move {
        let bytes = serde_json::to_vec(&progress).unwrap_or_default();
        store.set_custom_value(CLASSIC_CACHE_REPAIR_PROGRESS_KEY, bytes).await.is_ok()
    };
    if !save(progress.clone()).await {
        line("classic sync: event cache repair skipped (progress could not be recorded)".into());
        return ClassicCacheRepair::Failed;
    }
    let force = classic_repair_must_finish(&progress, now_ms);
    if !views_idle() && !force {
        let starts = progress.starts;
        line(format!(
            "classic sync: event cache repair deferred, a timeline is open \
             (start {starts}/{CLASSIC_CACHE_REPAIR_MAX_STARTS})"
        ));
        return ClassicCacheRepair::Deferred;
    }

    let _ = client.event_cache().subscribe();
    let mut cleared = 0usize;
    let mut waiting = 0usize;
    let mut failed = false;
    for room in client.rooms() {
        let id = room.room_id().to_string();
        if progress.done.contains(&id) {
            continue;
        }
        let server = room.unread_notification_counts();
        if !force
            && room_unread_would_be_lost(
                room.encryption_state().is_encrypted(),
                room.num_unread_messages(),
                room.num_unread_notifications(),
                room.num_unread_mentions(),
                server.highlight_count,
            )
        {
            waiting += 1;
            continue;
        }
        let ok = match room.event_cache().await {
            Ok((cache, _handles)) => cache.clear().await.is_ok(),
            Err(_) => false,
        };
        if !ok {
            failed = true;
            break;
        }
        progress.done.insert(id);
        cleared += 1;
    }
    if cleared > 0 && !views_idle() && lifecycle() == started_lifecycle {
        enqueue(events, json!({
            "type": "event_cache_repaired", "lifecycle": started_lifecycle,
        }));
    }
    let starts = progress.starts;
    if failed {
        save(progress).await;
        line(format!("classic sync: event cache repair failed (clear) after {cleared} rooms"));
        return ClassicCacheRepair::Failed;
    }
    if waiting > 0 {
        if !save(progress).await {
            line("classic sync: event cache repair progress could not be recorded".into());
            return ClassicCacheRepair::Failed;
        }
        line(format!(
            "classic sync: event cache repair cleared {cleared} rooms; {waiting} wait until \
             they are read (start {starts}/{CLASSIC_CACHE_REPAIR_MAX_STARTS})"
        ));
        return ClassicCacheRepair::Partial { cleared, waiting };
    }
    if store.set_custom_value(CLASSIC_CACHE_REPAIR_KEY, b"1".to_vec()).await.is_err() {
        save(progress).await;
        line("classic sync: event cache repair ran but could not be recorded".into());
        return ClassicCacheRepair::Failed;
    }
    let _ = store.remove_custom_value(CLASSIC_CACHE_REPAIR_PROGRESS_KEY).await;
    line(format!(
        "classic sync: cleared the event cache to repair gaps from older builds \
         (GitHub #48): {cleared} rooms this start, finished{}; rooms refill from \
         the server as they are opened",
        if force { " unconditionally (start or age cap reached)" } else { "" }
    ));
    ClassicCacheRepair::Done { cleared, forced: force }
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
    sync_notify.catchup.begin(push_verdict::now_ms());
    let background = bgsession::for_queue(&events);
    let watch_cap = background
        .as_ref()
        .map_or(LATEST_EVENT_WATCH_CAP, |bg| bg.watch_cap().min(LATEST_EVENT_WATCH_CAP));
    let mut jitter = background.as_ref().map(|_| labs::Jitter::seeded(0xc1a5));

    let first_response = Arc::new(AtomicBool::new(true));
    let progress = SyncProgress::new();
    let failure_count = Arc::new(std::sync::atomic::AtomicU32::new(0));
    let session_gone = Arc::new(AtomicBool::new(false));
    let shared_watched: Arc<tokio::sync::Mutex<BTreeSet<OwnedRoomId>>> =
        Arc::new(tokio::sync::Mutex::new(BTreeSet::new()));
    let shared_stamps: Arc<tokio::sync::Mutex<HashMap<OwnedRoomId, u64>>> =
        Arc::new(tokio::sync::Mutex::new(HashMap::new()));

    let watchdog_events = Arc::clone(&events);
    let watchdog_first = Arc::clone(&first_response);
    let watchdog = async move {
        watch_first_sync_response(&watchdog_events, &watchdog_first,
                                  FIRST_SYNC_STALL_STEPS).await;
        std::future::pending::<()>().await
    };
    tokio::pin!(watchdog);
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
        let callback_background = background.clone();
        progress.touch();
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
            let background = callback_background.clone();
            async move {
                progress.touch();
                let answered_ms = push_verdict::now_ms();
                let response = match result {
                    Ok(response) => response,
                    Err(error) => {
                        if classify_classic_sync_error(error.client_api_error_kind())
                            == ClassicSyncFault::Fatal
                        {
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
                        if seen == CLASSIC_SYNC_REPORT_AFTER {
                            let rate = background.as_ref().map(|_| sync_rate_facts(&error));
                            enqueue(&events, sync_error_event(
                                format_matrix_error("Matrix Rust SDK sync failed", error),
                                rate,
                            ));
                        }
                        let pause = if background.is_some() {
                            labs::Jitter::seeded(u64::from(seen)).backoff(seen - 1)
                        } else {
                            classic_sync_backoff(seen - 1)
                        };
                        tokio::time::sleep(pause).await;
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
                            if desired.len() >= watch_cap {
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

                    if background.is_none() {
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
                }
                if let Some(bg) = &background {
                    bg.note_skipped();
                } else {
                    {
                        let stamps = stamps.lock().await;
                        enqueue_rooms_stamped(&events, &client, Some(&stamps)).await;
                    }
                    let spaces = SpaceService::new(client.clone()).await;
                    enqueue_spaces(&events, &spaces, &client).await;
                }
                enqueue(&events, json!({ "type": "room_list_sync_state", "state": "running" }));
                if first_response.swap(false, Ordering::SeqCst) {
                    enqueue(&events, json!({ "type": "initial_sync_done" }));
                }
                progress.touch();
                Ok::<LoopCtrl, matrix_sdk::Error>(LoopCtrl::Continue)
            }
        };
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
        tokio::select! {
            biased;
            result = &mut sync => if let Err(err) = result {
                let rate = background.as_ref().map(|_| sync_rate_facts(&err));
                enqueue(&events, sync_error_event(
                    format_matrix_error("Matrix Rust SDK sync failed", err),
                    rate,
                ));
            },
            _ = &mut cancel => return,
            _ = &mut watchdog => {},
            _ = supervise_classic_silence(&events, &progress, silence) => {
                wedged = true;
            },
        }
        wedged
        };

        if session_gone.load(Ordering::SeqCst) {
            let _ = (&mut cancel).await;
            return;
        }
        let seen = failure_count.fetch_add(1, Ordering::SeqCst) + 1;
        enqueue(&events, json!({
            "type": "room_list_sync_state", "state": "offline"
        }));
        if wedged && (!wedge_reported || seen == 1) {
            wedge_reported = true;
            enqueue(&events, sync_error_event(
                CLASSIC_SYNC_WEDGED_MESSAGE.to_owned(),
                background.as_ref().map(|_| (false, None)),
            ));
        }
        let pause = match jitter.as_mut() {
            Some(jitter) => jitter.backoff(seen - 1),
            None => classic_sync_backoff(seen - 1),
        };
        tokio::select! {
            _ = &mut cancel => return,
            _ = tokio::time::sleep(pause) => {
                if !wedged {
                    enqueue(&events, json!({
                        "type": "room_list_sync_state", "state": "retrying"
                    }));
                }
            }
        }
    }
}

async fn forward_room_list_diffs(
    events: &Arc<Mutex<VecDeque<String>>>,
    batches: Vec<VectorDiff<RoomListItem>>,
    latest_events: &matrix_sdk::latest_events::LatestEvents,
    watched: &mut BTreeSet<OwnedRoomId>,
    order: &mut Vec<OwnedRoomId>,
) {
    async fn payload(
        room: Room,
        latest_events: &matrix_sdk::latest_events::LatestEvents,
        watched: &mut BTreeSet<OwnedRoomId>,
    ) -> serde_json::Value {
        watch_latest_event(latest_events, watched, room.room_id()).await;
        room_payload(&room).await
    }

    fn id_at(order: &[OwnedRoomId], index: usize) -> String {
        order.get(index).map(|id| id.to_string()).unwrap_or_default()
    }

    for diff in batches {
        let value = match diff {
            VectorDiff::Reset { values } => {
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

fn retry_pause(jitter: &mut Option<labs::Jitter>, attempt: &mut u32) -> std::time::Duration {
    match jitter {
        Some(jitter) => {
            let pause = jitter.backoff(*attempt);
            *attempt = attempt.saturating_add(1);
            pause
        }
        None => std::time::Duration::from_secs(3),
    }
}

fn sync_error_event(message: String, rate: Option<(bool, Option<u64>)>) -> serde_json::Value {
    let value = json!({ "type": "sync_error", "message": message });
    match rate {
        Some(facts) => bgsession::with_rate_limit(value, facts),
        None => value,
    }
}

fn sync_rate_facts(error: &matrix_sdk::Error) -> (bool, Option<u64>) {
    bgsession::rate_limit_facts(
        error.client_api_error_kind(),
        error.as_client_api_error().map(|api| api.status_code.as_u16()),
    )
}

async fn watch_room_list_diffs(
    batches: Vec<VectorDiff<RoomListItem>>,
    latest_events: &matrix_sdk::latest_events::LatestEvents,
    watched: &mut BTreeSet<OwnedRoomId>,
    stamps: &mut HashMap<OwnedRoomId, u64>,
    order: &mut Vec<OwnedRoomId>,
    cap: usize,
) {
    let mut left: Vec<OwnedRoomId> = Vec::new();
    for diff in batches {
        let diff = diff.map(|item| item.into_inner());
        left.extend(
            bgsession::apply_order_diff(order, diff.clone().map(|room| room.room_id().to_owned()))
                .into_iter()
                .filter(|id| watched.contains(id)),
        );
        let rooms: Vec<Room> = match diff {
            VectorDiff::Reset { values } | VectorDiff::Append { values } => {
                values.into_iter().collect()
            }
            VectorDiff::PushFront { value }
            | VectorDiff::PushBack { value }
            | VectorDiff::Insert { value, .. }
            | VectorDiff::Set { value, .. } => vec![value],
            _ => Vec::new(),
        };
        for room in rooms {
            let room_id = room.room_id().to_owned();
            let stamp = room.recency_stamp().map(u64::from).unwrap_or(0);
            stamps.insert(room_id.clone(), stamp);
            if watched.contains(&room_id) {
                continue;
            }
            let current: Vec<(OwnedRoomId, u64)> = watched
                .iter()
                .map(|id| (id.clone(), stamps.get(id).copied().unwrap_or(0)))
                .collect();
            match bgsession::watch_decision(&current, cap, stamp) {
                bgsession::WatchDecision::Skip => continue,
                bgsession::WatchDecision::Replace(evicted) => {
                    latest_events.forget_room(&evicted).await;
                    watched.remove(&evicted);
                }
                bgsession::WatchDecision::Watch => {}
            }
            if latest_events.listen_to_room(&room_id).await.unwrap_or(false) {
                watched.insert(room_id);
            }
        }
    }
    if !left.is_empty() {
        let present: HashSet<&OwnedRoomId> = order.iter().collect();
        let gone: Vec<OwnedRoomId> =
            left.into_iter().filter(|id| !present.contains(id)).collect();
        for room_id in gone {
            if watched.remove(&room_id) {
                latest_events.forget_room(&room_id).await;
            }
        }
    }
    stamps.retain(|id, _| watched.contains(id));
}

async fn direct_children_of(room: &Room) -> Vec<String> {
    let Ok(events) = room
        .get_state_events_static::<SpaceChildEventContent>()
        .await
    else {
        return Vec::new();
    };
    let mut entries: Vec<(Option<String>, u64, String)> = Vec::new();
    for raw in events {
        use matrix_sdk::deserialized_responses::RawSyncOrStrippedState;
        let RawSyncOrStrippedState::Sync(raw) = raw else { continue };
        let Ok(value) = serde_json::from_str::<serde_json::Value>(raw.json().get()) else { continue };
        if let Some(entry) = space_child_entry(value) {
            entries.push(entry);
        }
    }
    sort_space_children(entries)
}

fn space_child_entry(mut value: serde_json::Value) -> Option<(Option<String>, u64, String)> {
    let order = valid_space_order(value.get("content").and_then(|c| c.get("order")));
    value.get_mut("content")?.as_object_mut()?.remove("order");
    let SyncStateEvent::Original(event) = serde_json::from_value::<SyncStateEvent<SpaceChildEventContent>>(value).ok()? else { return None };
    if event.content.via.is_empty() { return None; }
    Some((order, u64::from(event.origin_server_ts.get()), event.state_key.to_string()))
}

fn valid_space_order(value: Option<&serde_json::Value>) -> Option<String> {
    let key = value?.as_str()?;
    (key.len() <= 50 && key.bytes().all(|b| (0x20..=0x7e).contains(&b)))
        .then(|| key.to_owned())
}

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
    let mut direct_by_parent = HashMap::<String, BTreeSet<String>>::new();
    let mut unlinked = HashMap::<String, BTreeSet<String>>::new();
    for space in &joined_spaces {
        let gone = unlinked_children_of(space).await;
        if !gone.is_empty() {
            unlinked.insert(space.room_id().to_string(), gone);
        }
    }

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
        let mut children = direct_children_of(room).await;
        let ordered_children = children.clone();
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
            "ordered_children": ordered_children,
            "child_spaces": child_spaces,
            "descendants": descendants,
            "level": level,
        }));
    }
    enqueue(events, json!({ "type": "space_list_reset", "spaces": spaces }));
}

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

/// Runs before matrix-sdk's own ignore-list task gets to blank every room's
/// latest event, so only rooms a changed user touches go back to the server.
async fn carry_previews_across_ignore_change(
    client: &Client,
    results: &previewbackfill::RunGuard,
    backfill: &mut previewbackfill::Backfill,
    changed: &[String],
) -> (usize, usize, usize) {
    use matrix_sdk_base::latest_event::LatestEventValue;
    let Some(own) = client.user_id() else { return (0, 0, 0) };
    let changed: Vec<OwnedUserId> =
        changed.iter().filter_map(|user| UserId::parse(user.as_str()).ok()).collect();
    let (mut kept, mut refreshed, mut blank) = (0usize, 0usize, 0usize);
    for room in client.joined_rooms() {
        let latest = room.latest_event();
        let sender = match &latest {
            LatestEventValue::Remote(event) => {
                event.raw().get_field::<OwnedUserId>("sender").ok().flatten()
            }
            _ => None,
        };
        let mut involved = sender.is_some_and(|sender| changed.contains(&sender));
        for user in &changed {
            if involved {
                break;
            }
            involved = matches!(room.get_member_no_sync(user).await, Ok(Some(_)));
        }
        let edited_ts = match &latest {
            LatestEventValue::Remote(event) => match replaced_event_id(event.raw()) {
                Some(original) => edited_event_timestamp_ms(&room, &original).await,
                None => None,
            },
            _ => None,
        };
        let carried = previewbackfill::across_ignore_change(
            &latest_event_preview_line(&latest),
            room_ordering_timestamp_ms(&latest, edited_ts),
            involved,
        );
        match carried {
            Some(what) => {
                kept += 1;
                results.remember(room.room_id(), what);
            }
            None if involved => {
                refreshed += 1;
                if let Some(was @ previewbackfill::Remembered::Message(..)) =
                    previewbackfill::remembered(own, room.room_id())
                {
                    previewbackfill::replace_remembered(own, room.room_id(), &was, None);
                }
                backfill.forget(room.room_id());
            }
            None => blank += 1,
        }
    }
    (kept, refreshed, blank)
}

async fn edited_event_timestamp_ms(room: &Room, event_id: &EventId) -> Option<u64> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let event = cache.find_event(event_id).await.ok()??;
    let ts = u64::from(event.raw().get_field::<UInt>("origin_server_ts").ok()??);
    if ts == 0 { None } else { Some(ts) }
}

pub(crate) const CONVERSATION_EVENT_TYPES: &[&str] = &[
    "m.room.message",
    "m.room.encrypted",
    "m.sticker",
    "m.poll.start",
    "org.matrix.msc3381.poll.start",
];

pub(crate) fn conversation_timestamp_ms(
    raw: &matrix_sdk::ruma::serde::Raw<
        matrix_sdk::ruma::events::AnySyncTimelineEvent,
    >,
) -> Option<u64> {
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
    if replaced_event_id(raw).is_some() {
        return None;
    }
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

pub(crate) fn event_ordering_stamp_ms(
    event: &matrix_sdk::deserialized_responses::TimelineEvent,
) -> Option<u64> {
    let raw_ts = conversation_timestamp_ms(event.raw())?;
    let own = event.timestamp_raw().map(|ts| u64::from(ts.get())).filter(|ts| *ts != 0);
    Some(own.map_or(raw_ts, |ts| ts.min(raw_ts)))
}

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

async fn undecryptable_now(room: &Room, event_id: &EventId) -> previewbackfill::UndecryptableNow {
    use previewbackfill::UndecryptableNow;
    let Ok((cache, _drop_handles)) = room.event_cache().await else {
        return UndecryptableNow::Unknown;
    };
    match cache.find_event(event_id).await {
        Ok(Some(event)) if event.kind.is_utd() => UndecryptableNow::StillUndecryptable,
        Ok(Some(event)) => UndecryptableNow::Decrypted(raw_event_preview_line(event.raw())),
        _ => UndecryptableNow::Unknown,
    }
}

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
    let (unread_count, highlight_count) = unread_and_highlight_counts(
        room.encryption_state().is_encrypted(),
        room.num_unread_notifications(),
        room.num_unread_mentions(),
        notifications.notification_count,
        notifications.highlight_count,
    );
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

    let latest_event = room.latest_event();
    let edited_ts = match &latest_event {
        matrix_sdk_base::latest_event::LatestEventValue::Remote(event) => {
            match replaced_event_id(event.raw()) {
                Some(original) => edited_event_timestamp_ms(room, &original).await,
                None => None,
            }
        }
        _ => None,
    };
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
        latest_event_preview_line(&latest_event),
        room_ordering_timestamp_ms(&latest_event, edited_ts),
        settled,
    );
    let own_join_latest = own_preview.is_empty()
        && matches!(&latest_event,
            matrix_sdk_base::latest_event::LatestEventValue::Remote(event)
                if previewbackfill::is_own_join(event.raw(), room.own_user_id()));
    let is_direct = rooms::payload_is_direct(room, !direct_targets.is_empty()).await;
    let (preview_text, preview_kind, preview_arg) = preview_wire(&own_preview);

    json!({
        "id": room.room_id().to_string(),
        "membership": membership,
        "name": room_name(room).await,
        "canonical_alias": room.canonical_alias().map(|alias| alias.to_string()).unwrap_or_default(),
        "topic": room.topic().unwrap_or_default(),
        "avatar_url": room.avatar_url().map(|url| url.to_string()).unwrap_or_default(),
        "last_message_preview": preview_text,
        "last_message_preview_kind": preview_kind,
        "last_message_preview_arg": preview_arg,
        "last_message_undecryptable": own_undecryptable,
        "last_message_own_join": own_join_latest,
        "last_activity_ms": own_activity,
        "unread_count": unread_count,
        "highlight_count": highlight_count,
        "marked_unread": room.is_marked_unread(),
        "unread_threads_only": unread_threads_only,
        "mention_in_threads": mention_in_threads,
        "is_favourite": room.is_favourite(),
        "is_low_priority": room.is_low_priority(),
        "has_unread_messages": room.num_unread_messages() > 0,
        "encrypted": room.encryption_state().is_encrypted(),
        "encryption_known": !room.encryption_state().is_unknown(),
        "is_space": room.is_space(),
        "is_direct": is_direct,
        "direct_user_id": direct_targets.first().cloned().unwrap_or_default(),
        "direct_user_ids": direct_targets,
        "member_count": room.joined_members_count(),
        "active_member_count": room.active_members_count(),
        "join_rule": rooms::join_rule_str(room.join_rule().as_ref()),
        "room_type": room.room_type().map(|kind| kind.to_string()).unwrap_or_default(),
        "prev_batch": room.last_prev_batch().unwrap_or_default(),
        "inviter_user_id": inviter_user_id,
        "inviter_display_name": inviter_display_name,
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

#[cfg(test)]
pub(crate) fn latest_event_preview_text(
    value: &matrix_sdk_base::latest_event::LatestEventValue,
) -> String {
    preview_english(&latest_event_preview_line(value))
}

pub(crate) fn latest_event_preview_line(
    value: &matrix_sdk_base::latest_event::LatestEventValue,
) -> String {
    use matrix_sdk_base::latest_event::LatestEventValue;

    match value {
        LatestEventValue::Remote(event) => raw_event_preview_line(event.raw()),
        _ => String::new(),
    }
}

pub(crate) const PREVIEW_KIND_MARK: char = '\u{1}';

pub(crate) fn kind_preview(kind: &str, arg: &str) -> String {
    format!("{PREVIEW_KIND_MARK}{kind}{PREVIEW_KIND_MARK}{arg}")
}

pub(crate) fn split_kind_preview(line: &str) -> Option<(&str, &str)> {
    line.strip_prefix(PREVIEW_KIND_MARK)?.split_once(PREVIEW_KIND_MARK)
}

pub(crate) fn preview_english(line: &str) -> String {
    let Some((kind, arg)) = split_kind_preview(line) else {
        return line.to_owned();
    };
    match kind {
        "image" => "Image".to_owned(),
        "file" => "File".to_owned(),
        "video" => "Video".to_owned(),
        "audio" => "Audio".to_owned(),
        "voice" => "Voice message".to_owned(),
        "sticker" => "Sticker".to_owned(),
        "poll" | "poll_question" => format!("Poll: {arg}"),
        "images" => format!("{arg} images"),
        "attachments" => format!("{arg} attachments"),
        _ => arg.to_owned(),
    }
}

pub(crate) fn preview_wire(line: &str) -> (String, String, String) {
    match split_kind_preview(line) {
        Some((kind, arg)) => (preview_english(line), kind.to_owned(), arg.to_owned()),
        None => (line.to_owned(), String::new(), String::new()),
    }
}

pub(crate) fn raw_event_preview_line(
    raw: &matrix_sdk::ruma::serde::Raw<matrix_sdk::ruma::events::AnySyncTimelineEvent>,
) -> String {
    use matrix_sdk::ruma::events::{
        room::message::Relation, AnySyncMessageLikeEvent, AnySyncTimelineEvent,
        SyncMessageLikeEvent,
    };

    fn body_or(body: String, kind: &str) -> String {
        if body.is_empty() { kind_preview(kind, "") } else { body }
    }

    fn one_line(text: &str) -> String {
        text.replace(PREVIEW_KIND_MARK, " ")
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ")
    }

    let Ok(deserialized) = raw.deserialize() else {
        return String::new();
    };
    let AnySyncTimelineEvent::MessageLike(message_like) = deserialized else {
        return String::new();
    };
    match message_like {
        AnySyncMessageLikeEvent::RoomMessage(SyncMessageLikeEvent::Original(message)) => {
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
                MessageType::Image(content) => body_or(one_line(&content.body), "image"),
                MessageType::File(content) => body_or(one_line(&content.body), "file"),
                MessageType::Video(content) => body_or(one_line(&content.body), "video"),
                MessageType::Audio(content) => match crate::timeline::voice_message_label(&content) {
                    Some(_) => kind_preview("voice", ""),
                    None => body_or(one_line(&content.body), "audio"),
                },
                other => match crate::timeline::parse_gallery(&other)
                    .filter(|g| !g.items.is_empty())
                {
                    Some(g) if !g.caption.trim().is_empty() => one_line(&g.caption),
                    Some(g) if g.items.len() == 1 => {
                        kind_preview(if g.all_images() { "image" } else { "file" }, "")
                    }
                    Some(g) => kind_preview(
                        if g.all_images() { "images" } else { "attachments" },
                        &g.items.len().to_string(),
                    ),
                    None => String::new(),
                },
            }
        }
        AnySyncMessageLikeEvent::Sticker(SyncMessageLikeEvent::Original(_)) => {
            kind_preview("sticker", "")
        }
        AnySyncMessageLikeEvent::UnstablePollStart(SyncMessageLikeEvent::Original(poll)) => {
            let question = one_line(&poll.content.poll_start().question.text);
            if question.is_empty() {
                kind_preview("poll", "")
            } else {
                kind_preview("poll_question", &question)
            }
        }
        _ => String::new(),
    }
}

const LATEST_EVENT_WATCH_CAP: usize = 200;

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

const EVENT_QUEUE_CAP: usize = 4096;

pub(crate) const COMMAND_QUEUE_CAP: usize = 512;

pub(crate) fn enqueue_terminal(
    queue: &EventQueueRef,
    parked: &Arc<Mutex<HashMap<u64, Vec<u8>>>>,
    value: serde_json::Value,
) {
    enqueue_terminal_routed(queue, Some(parked), None, value);
}

pub(crate) fn enqueue_terminal_routed(
    queue: &EventQueueRef,
    media: Option<&Arc<Mutex<HashMap<u64, Vec<u8>>>>>,
    personal: Option<&Arc<Mutex<HashMap<u64, Vec<u8>>>>>,
    value: serde_json::Value,
) {
    if labs::active() && bgsession::refuses_command(queue) {
        if let Some(op) = value.get("op_id").and_then(|o| o.as_u64()) {
            for map in [media, personal].into_iter().flatten() {
                if let Ok(mut results) = map.lock() {
                    results.remove(&op);
                }
            }
        }
        return;
    }
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

pub(crate) mod switchtrace {
    use std::cell::RefCell;
    use std::future::{Future, IntoFuture};
    use std::time::Instant;

    use serde_json::{json, Value};

    tokio::task_local! {
        static RESTORE_TRACE: RefCell<RestoreTrace>;
        static SYNC_TRACE: RefCell<SyncTrace>;
    }

    pub(crate) fn switch_trace_enabled_from(value: Option<&str>) -> bool {
        value
            .map(|v| {
                let v = v.trim().to_ascii_lowercase();
                !(v.is_empty() || v == "0" || v == "false" || v == "off" || v == "no")
            })
            .unwrap_or(false)
    }

    pub(crate) fn switch_trace_enabled() -> bool {
        use std::sync::OnceLock;
        static ENABLED: OnceLock<bool> = OnceLock::new();
        *ENABLED.get_or_init(|| {
            switch_trace_enabled_from(std::env::var("LIGHTNING_SWITCH_TRACE").ok().as_deref())
        })
    }

    pub(crate) fn unix_epoch_ms() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0)
    }

    fn ms_between(earlier: Instant, later: Instant) -> u64 {
        later.saturating_duration_since(earlier).as_millis() as u64
    }

    pub(crate) trait TraceObject {
        fn to_json(&self) -> Value;
    }

    pub(crate) fn attach_trace<T: TraceObject>(event: &mut Value, trace: Option<&T>) {
        let (Some(trace), Some(map)) = (trace, event.as_object_mut()) else {
            return;
        };
        map.insert("trace".to_owned(), trace.to_json());
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum RestoreStep {
        Discovery,
        StoreOpen,
        FallbackBuild,
        Restore,
        ResumeUnsent,
    }

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

    pub(crate) fn with_restore_trace(mut event: Value) -> Value {
        let trace = RESTORE_TRACE
            .try_with(|cell| cell.try_borrow().ok().map(|trace| trace.clone()))
            .ok()
            .flatten();
        attach_trace(&mut event, trace.as_ref());
        event
    }

    pub(crate) fn note_offline_fallback() {
        let _ = RESTORE_TRACE.try_with(|cell| {
            if let Ok(mut trace) = cell.try_borrow_mut() {
                trace.note_offline_fallback();
            }
        });
    }

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

    pub(crate) async fn timed<F: IntoFuture>(step: Option<RestoreStep>, future: F) -> F::Output {
        let _timer = step
            .filter(|_| RESTORE_TRACE.try_with(|_| ()).is_ok())
            .map(|step| RestoreStepTimer { step, started: Instant::now() });
        future.await
    }

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

    #[derive(Debug, Clone)]
    pub(crate) struct SyncTrace {
        started: Instant,
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

    pub(crate) fn sync_start_stamp() -> Option<Instant> {
        switch_trace_enabled().then(Instant::now)
    }

    pub(crate) async fn sync_scope<F>(started: Option<Instant>, future: F) -> F::Output
    where
        F: Future,
    {
        match started {
            Some(started) => SYNC_TRACE.scope(RefCell::new(SyncTrace::new(started)), future).await,
            None => future.await,
        }
    }

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

    pub(crate) fn first_reset_serialize_start() -> Option<Instant> {
        SYNC_TRACE
            .try_with(|cell| cell.try_borrow().map(|t| !t.reset_reported).unwrap_or(false))
            .unwrap_or(false)
            .then(Instant::now)
    }

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
            let mut scalar = json!("text");
            attach_trace(&mut scalar, Some(&RestoreTrace::new(Instant::now())));
            assert_eq!(scalar, json!("text"));
        }

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

        #[tokio::test]
        async fn a_restore_with_the_trace_on_attaches_what_its_steps_recorded() {
            let emitted = std::sync::Arc::new(std::sync::Mutex::new(None::<Value>));
            let sink = std::sync::Arc::clone(&emitted);
            restore_scope(true, async move {
                let cut = tokio::time::timeout(
                    Duration::from_millis(30),
                    timed(Some(RestoreStep::Discovery), std::future::pending::<()>()),
                )
                .await;
                assert!(cut.is_err());
                timed(Some(RestoreStep::StoreOpen), async {}).await;
                timed(Some(RestoreStep::StoreOpen), async {}).await;
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
            note_offline_fallback();
            timed(Some(RestoreStep::Restore), async {}).await;
            assert_eq!(with_restore_trace(login_ok()), login_ok());
        }

        #[tokio::test]
        async fn a_sync_session_traces_its_first_reset_only_and_only_when_on() {
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
    let value = if labs::active() {
        match bgsession::admit(events, value) {
            Some(value) => value,
            None => return,
        }
    } else {
        value
    };
    let Ok(serialized) = serde_json::to_string(&value) else {
        return;
    };
    if let Ok(mut guard) = events.lock() {
        if guard.len() >= EVENT_QUEUE_CAP {
            let dropped_marker = matches!(
                guard.front().map(|s| s.contains("\"queue_overflow\"")),
                Some(true),
            );
            guard.pop_front();
            if !dropped_marker {
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
    let profile = bgsession::for_queue(&events).map(|bg| bg.profile.clone());
    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut builder = tokio::runtime::Builder::new_current_thread();
        if let Some(profile) = &profile {
            builder
                .thread_name(labs::thread_name(labs::runtime_kind(label), &profile.slot))
                .max_blocking_threads(profile.max_blocking_threads);
        }
        let runtime = builder
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

unsafe fn reply_target_arg(
    ptr: *const c_char,
) -> Result<Option<matrix_sdk::ruma::OwnedEventId>, String> {
    timeline::parse_reply_target(&unsafe { cstr_arg(ptr) }?)
}

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
mod thread_subscription_reason_tests {
    use super::thread_subscription_reason_for_kind as reason;
    use matrix_sdk::ruma::api::error::ErrorKind;

    #[test]
    fn a_refusal_names_a_constant_category() {
        assert_eq!(reason(Some(&ErrorKind::Unrecognized)), "unsupported_by_server");
        assert_eq!(reason(Some(&ErrorKind::NotFound)), "not_found");
        assert_eq!(reason(Some(&ErrorKind::Forbidden)), "forbidden");
        assert_eq!(reason(None), "failed");
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn mark_read_target_does_not_depend_on_latest_event() {
        let id = |s: &str| super::EventId::parse(s).unwrap();
        assert_eq!(
            super::pick_receipt_target(Some(id("$new:x")), None, None),
            Some(id("$new:x")));
        assert_eq!(
            super::pick_receipt_target(Some(id("$new:x")), Some(id("$old:x")), None),
            Some(id("$new:x")));
        assert_eq!(
            super::pick_receipt_target(None, Some(id("$old:x")), Some(id("$f:x"))),
            Some(id("$old:x")));
        assert_eq!(
            super::pick_receipt_target(None, None, Some(id("$f:x"))),
            Some(id("$f:x")));
        assert_eq!(super::pick_receipt_target(None, None, None), None);
    }

    #[test]
    fn receipt_privacy_decides_what_other_members_can_see() {
        use super::receipts_for_mode;
        use matrix_sdk::ruma::EventId;

        let id = EventId::parse("$abc:example.org").unwrap();

        let public = receipts_for_mode(id.clone(), 0);
        assert_eq!(public.public_read_receipt.as_deref(), Some(&*id));
        assert!(public.private_read_receipt.is_none());
        assert_eq!(public.fully_read.as_deref(), Some(&*id));

        let private = receipts_for_mode(id.clone(), 1);
        assert!(
            private.public_read_receipt.is_none(),
            "a private receipt must never also be published"
        );
        assert_eq!(private.private_read_receipt.as_deref(), Some(&*id));
        assert_eq!(private.fully_read.as_deref(), Some(&*id));

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

        let bogus = receipts_for_mode(id.clone(), 77);
        assert_eq!(bogus.public_read_receipt.as_deref(), Some(&*id));
    }

    #[test]
    fn only_an_authorised_redaction_is_forwarded() {
        use matrix_sdk::ruma::user_id;
        let alice = user_id!("@alice:example.org");
        let mallory = user_id!("@mallory:example.org");
        assert!(super::redaction_applies(Some(alice), alice, Some(false)));
        assert!(!super::redaction_applies(Some(alice), mallory, Some(false)));
        assert!(super::redaction_applies(Some(alice), mallory, Some(true)));
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
            unrendered_start: false,
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
        assert_eq!(pick("$m1:x", &tail), id("$m1:x"));
        assert_eq!(pick("$d1:x", &tail), id("$d1:x"));
        assert_eq!(pick("$zz:x", &tail), id("$zz:x"));
        let utd = vec![
            msg("$m2:x"),
            TailEvent { id: id("$u:x"), kind: None, target: None, threaded: false,
                        unrendered_start: false },
        ];
        assert_eq!(pick("$m2:x", &utd), id("$m2:x"));

        let thread = vec![thread_reply("$t1:x"), msg("$m2:x"), react("$r1:x", "$t1:x")];
        assert_eq!(pick("$m2:x", &thread), id("$m2:x"));
        let unknown = vec![msg("$m2:x"), react("$r1:x", "$gone:x")];
        assert_eq!(pick("$m2:x", &unknown), id("$m2:x"));
        let mixed = vec![
            thread_reply("$t1:x"),
            msg("$m2:x"),
            react("$r1:x", "$m2:x"),
            react("$r2:x", "$t1:x"),
        ];
        assert_eq!(pick("$m2:x", &mixed), id("$m2:x"));

        let edit = vec![msg("$m2:x"), ev("$e1:x", "m.room.message", Some("$m2:x"), false)];
        assert_eq!(pick("$m2:x", &edit), id("$m2:x"));
        let poll = vec![
            msg("$m2:x"),
            ev("$p1:x", "org.matrix.msc3381.poll.response", Some("$m2:x"), false),
            react("$r1:x", "$m2:x"),
        ];
        assert_eq!(pick("$m2:x", &poll), id("$m2:x"));
        let redact_msg = vec![msg("$m1:x"), msg("$m2:x"), ev("$d:x", "m.room.redaction", Some("$m1:x"), false)];
        assert_eq!(pick("$m2:x", &redact_msg), id("$m2:x"));

        let ended = vec![
            ev("$p:x", "org.matrix.msc3381.poll.start", None, false),
            msg("$m2:x"),
            ev("$pe:x", "org.matrix.msc3381.poll.end", Some("$p:x"), false),
        ];
        assert_eq!(pick("$m2:x", &ended), id("$pe:x"));
        let ended_stable = vec![
            ev("$p:x", "org.matrix.msc3381.poll.start", None, false),
            msg("$m2:x"),
            ev("$pe:x", "m.poll.end", Some("$p:x"), false),
        ];
        assert_eq!(pick("$m2:x", &ended_stable), id("$pe:x"));
        let ended_unknown = vec![msg("$m2:x"), ev("$pe:x", "m.poll.end", Some("$gone:x"), false)];
        assert_eq!(pick("$m2:x", &ended_unknown), id("$m2:x"));
        let stable_start = vec![msg("$m2:x"), ev("$ps:x", "m.poll.start", None, false)];
        assert_eq!(pick("$m2:x", &stable_start), id("$m2:x"));
        let stable_ended = vec![
            ev("$ps:x", "m.poll.start", None, false),
            msg("$m2:x"),
            ev("$pe:x", "m.poll.end", Some("$ps:x"), false),
        ];
        assert_eq!(pick("$m2:x", &stable_ended), id("$pe:x"));
        let malformed = TailEvent { unrendered_start: true, ..ev("$ps:x", "m.poll.start", None, false) };
        assert_eq!(pick("$m2:x", &[msg("$m2:x"), malformed]), id("$ps:x"));
        let unrendered = vec![msg("$m2:x"), ev("$em:x", "m.message", None, false)];
        assert_eq!(pick("$m2:x", &unrendered), id("$em:x"));
    }

    #[test]
    fn the_fully_read_marker_never_moves_backwards() {
        assert!(super::fully_read_would_move_back(Some(2_000), Some(1_000)));
        assert!(!super::fully_read_would_move_back(Some(1_000), Some(2_000)));
        assert!(!super::fully_read_would_move_back(Some(1_000), Some(1_000)));
        assert!(!super::fully_read_would_move_back(None, Some(1_000)));
        assert!(!super::fully_read_would_move_back(Some(1_000), None));
        assert!(!super::fully_read_would_move_back(None, None));
    }

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
            let (receipt_type, thread, event) =
                automatic_receipt(id.clone(), mode, false);
            assert_eq!(receipt_type, expected, "mode {mode}");
            assert_ne!(receipt_type, ReceiptType::FullyRead, "mode {mode}");
            assert_eq!(thread, ReceiptThread::Main, "mode {mode}");
            assert_eq!(event, id);
            let (receipt_type, thread, event) =
                automatic_receipt(id.clone(), mode, true);
            assert_eq!(receipt_type, expected, "inline mode {mode}");
            assert_eq!(thread, ReceiptThread::Unthreaded, "inline mode {mode}");
            assert_eq!(event, id);
        }
    }

    #[test]
    fn identity_key_agreement_is_tri_state() {
        assert_eq!(
            super::identity_key_agreement(Some("AAAA"), Some("AAAA")),
            Some(true)
        );
        assert_eq!(
            super::identity_key_agreement(Some("AAAA"), Some("BBBB")),
            Some(false)
        );
        assert_eq!(super::identity_key_agreement(None, Some("AAAA")), None,
                   "no local Olm key is 'unknown', not 'broken'");
        assert_eq!(super::identity_key_agreement(Some("AAAA"), None), None,
                   "the server publishing nothing for us is 'unknown'");
        assert_eq!(super::identity_key_agreement(None, None), None);
        assert_eq!(super::identity_key_agreement(Some(""), Some("AAAA")), None);
        assert_eq!(super::identity_key_agreement(Some("AAAA"), Some("")), None);
    }

    #[test]
    fn a_task_that_will_not_stop_is_detached_rather_than_waited_for() {
        use super::{join_task_within_budget, SyncTask, SYNC_TASK_JOIN_BUDGET_MS};
        let (park_tx, park_rx) = std::sync::mpsc::channel::<()>();
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let thread = std::thread::spawn(move || {
            let _done = done_tx;
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
        assert!(
            waited < std::time::Duration::from_millis(SYNC_TASK_JOIN_BUDGET_MS + 2000),
            "waited {waited:?}, which is past the budget"
        );
        let _ = park_tx.send(());
    }

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

    #[test]
    #[should_panic(expected = "deliberate")]
    fn panic_is_catchable_in_the_test_profile() {
        panic!("deliberate");
    }

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
            join_or_abort_the_old_broken_way(vec![a, b], 50).await;
        });
    }

    fn test_bridge() -> super::RustClient {
        super::RustClient::new(
            std::env::temp_dir().join("lightning-spawn-reported-action-test"),
        )
        .expect("bridge")
    }

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

    #[test]
    fn the_shutdown_budget_leaves_the_cpp_store_close_wait_room_to_finish() {
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
        assert!(
            super::SHUTDOWN_VERIFICATION_JOIN_MS
                >= super::VERIFICATION_POLL_MS + 2 * super::SHUTDOWN_FLOW_CANCEL_MS,
            "a SAS driver cannot notice the shutdown flag and cancel both \
             levels inside its join budget"
        );
    }

    #[test]
    fn the_timeline_shutdown_legs_are_bounded_like_an_abort_drain() {
        assert_eq!(crate::timeline::SHUTDOWN_ABORTED_JOIN_MS, 250);
    }

    #[test]
    fn a_panic_report_names_where_it_happened_and_nothing_else() {
        let line = super::panic_report_line(Some(("rust/src/timeline.rs", 412)), Some("tokio-1"));
        assert!(line.contains("rust/src/timeline.rs:412"), "{line}");
        assert!(line.contains("tokio-1"), "{line}");
        assert!(line.contains("LIGHTNING_PANIC_PAYLOAD"), "{line}");
        let bare = super::panic_report_line(None, None);
        assert!(bare.contains("<unknown location>"), "{bare}");
        assert!(bare.contains("<unnamed>"), "{bare}");
    }

    #[test]
    fn only_a_non_empty_override_asks_for_the_stock_panic_hook() {
        assert!(super::panic_payload_requested(Some("1")));
        assert!(super::panic_payload_requested(Some("yes")));
        assert!(!super::panic_payload_requested(None));
        assert!(!super::panic_payload_requested(Some("")));
        assert!(!super::panic_payload_requested(Some("   ")));
    }

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

    #[test]
    fn an_encrypted_room_trusts_only_the_client_computed_counts() {
        assert_eq!(
            super::unread_and_highlight_counts(true, 0, 1, 12, 3),
            (0, 1)
        );
    }

    #[test]
    fn an_unencrypted_room_still_takes_the_larger_of_the_two_counts() {
        assert_eq!(
            super::unread_and_highlight_counts(false, 0, 0, 5, 1),
            (5, 1)
        );
    }

    #[test]
    fn unread_only_a_thread_can_explain_is_reported_for_a_plaintext_room() {
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 4, 1), (true, true));
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 4, 0), (true, false));
    }

    #[test]
    fn thread_only_unread_is_not_claimed_when_it_cannot_be_known() {
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 0, 0), (false, false));
        assert_eq!(super::unread_in_threads_only(false, true, 2, 0, 9, 0), (false, false));
        assert_eq!(super::unread_in_threads_only(false, true, 0, 1, 9, 1), (false, false));
        assert_eq!(super::unread_in_threads_only(true, true, 0, 0, 9, 3), (false, false));
    }

    #[test]
    fn the_own_identity_refetch_is_throttled_unless_local_state_moved() {
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
        let moved = [true, false, false, true, true, true];
        assert!(own_identity_refetch_due(Some((t0, stuck)), moved, t0));
    }

    #[test]
    fn the_own_identity_is_refetched_when_it_looks_stale_and_never_against_the_uia_guard() {
        use super::own_identity_refetch_wanted as wanted;
        let granted = || true;
        let throttled = || false;
        let stale = [false; 6];
        assert!(wanted(stale, true, granted));
        assert!(!wanted(stale, true, throttled));
        assert!(wanted([true, false, true, true, true, true], true, granted));
        assert!(wanted([true, true, false, false, false, false], true, granted));
        assert!(!wanted(stale, false, granted));
        assert!(!wanted([true, true, false, true, true, true], false, granted));
        let asked = std::cell::Cell::new(false);
        assert!(!wanted([true; 6], true, || {
            asked.set(true);
            true
        }));
        assert!(!asked.get());
        assert!(wanted([true, true, false, true, true, true], true, throttled));
    }

    #[test]
    fn invalid_space_order_is_keyless_not_a_missing_child() {
        use serde_json::json;
        let event = |order| json!({"type": "m.space.child", "state_key": "!child:x",
            "sender": "@admin:x", "event_id": "$child", "origin_server_ts": 10,
            "content": {"via": ["x"], "order": order}});
        for order in [json!(42), json!("é"), json!("\n"), json!("x".repeat(51))] {
            let entry = super::space_child_entry(event(order)).expect("child must remain visible");
            assert_eq!(entry, (None, 10, "!child:x".into()));
        }
        for key in ["", " ", "a~Z"] {
            assert_eq!(super::space_child_entry(event(json!(key))).unwrap().0, Some(key.to_owned()));
        }
        let mut removed = event(json!("a"));
        removed["content"]["via"] = json!([]);
        assert!(super::space_child_entry(removed).is_none());
    }

    #[test]
    fn space_children_without_an_order_key_follow_their_add_time() {
        let keyless = vec![
            (None, 1_000, "!zzalpha:x".to_owned()),
            (None, 2_000, "!aabeta:x".to_owned()),
            (None, 3_000, "!mmdelta:x".to_owned()),
        ];
        assert_eq!(
            super::sort_space_children(keyless),
            vec!["!zzalpha:x", "!aabeta:x", "!mmdelta:x"]
        );
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
        assert_eq!(super::unread_in_threads_only(false, false, 0, 0, 3, 0), (false, false));
        assert_eq!(super::unread_in_threads_only(false, false, 0, 0, 1, 1), (false, false));
        assert_eq!(super::unread_in_threads_only(false, true, 0, 0, 3, 0), (true, false));
    }

    #[test]
    fn an_encrypted_room_with_a_real_mention_still_shows_it() {
        assert_eq!(super::unread_and_highlight_counts(true, 3, 2, 3, 0), (3, 2));
    }

    #[test]
    fn notification_targets_supersede_consume_and_pend_correctly() {
        use std::collections::HashMap;
        use std::sync::{Arc, Mutex};
        let targets: Arc<Mutex<HashMap<String, u8>>> =
            Arc::new(Mutex::new(HashMap::new()));
        let room = "!room:example.org";

        targets.lock().unwrap().insert(room.to_owned(), 2);
        assert!(super::is_latest_notification_target(&targets, room, 2));
        assert!(super::notification_write_pending(&targets, room));

        targets.lock().unwrap().insert(room.to_owned(), 1);
        assert!(!super::is_latest_notification_target(&targets, room, 2));
        assert!(!super::take_notification_target_if_latest(&targets, room, 2));
        assert!(super::notification_write_pending(&targets, room));

        assert!(super::take_notification_target_if_latest(&targets, room, 1));
        assert!(!super::notification_write_pending(&targets, room));
        assert!(!super::take_notification_target_if_latest(&targets, room, 1));

        targets.lock().unwrap().insert(room.to_owned(), 0);
        assert!(super::take_notification_target_if_latest(&targets, room, 0));
        assert!(!super::is_latest_notification_target(&targets, room, 0));

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

        targets.lock().unwrap().insert(room.to_owned(), 2);
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 2,
        });
        assert!(!super::notification_write_pending(&targets, room));

        targets.lock().unwrap().insert(room.to_owned(), 1);
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 2,
        });
        assert!(super::is_latest_notification_target(&targets, room, 1));

        assert!(super::take_notification_target_if_latest(&targets, room, 1));
        drop(super::NotificationTargetGuard {
            targets: Arc::clone(&targets),
            room_id: room.to_owned(),
            mode: 1,
        });
        assert!(!super::notification_write_pending(&targets, room));
    }

    #[test]
    fn terminal_queue_overflow_frees_parked_bytes() {
        use std::collections::{HashMap, VecDeque};
        use std::sync::{Arc, Mutex};
        let queue: super::EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let parked: Arc<Mutex<HashMap<u64, Vec<u8>>>> =
            Arc::new(Mutex::new(HashMap::new()));

        parked.lock().unwrap().insert(1, vec![0u8; 64]);
        parked.lock().unwrap().insert(2, vec![0u8; 64]);

        for op in 1..=(super::COMMAND_QUEUE_CAP as u64) {
            super::enqueue_terminal(
                &queue,
                &parked,
                serde_json::json!({ "type": "media_ready", "op_id": op }),
            );
        }
        assert_eq!(queue.lock().unwrap().len(), super::COMMAND_QUEUE_CAP);
        assert_eq!(parked.lock().unwrap().len(), 2);

        super::enqueue_terminal(
            &queue,
            &parked,
            serde_json::json!({ "type": "media_ready", "op_id": 9999u64 }),
        );
        assert_eq!(queue.lock().unwrap().len(), super::COMMAND_QUEUE_CAP);
        assert!(!parked.lock().unwrap().contains_key(&1));
        assert!(parked.lock().unwrap().contains_key(&2));
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

    #[test]
    fn sas_confirmed_event_shape_is_flow_id_only() {
        let event = super::verification_sas_confirmed_event("flow-abc123");
        let obj = event.as_object().expect("json object");
        assert_eq!(obj.len(), 2);
        assert_eq!(obj["type"], "verification_sas_confirmed");
        assert_eq!(obj["flow_id"], "flow-abc123");
    }

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
            assert_eq!(request_flow(&request).as_deref(), Some("flow-a"));
        }

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
            assert_eq!(crate::accept_without_request_reply("flow-a"), "");
            assert_eq!(
                crate::accept_without_request_reply("flow-never-seen"),
                "error: no active verification request."
            );

            let answered = slot(PendingRequest::Answered);
            assert_eq!(pending_request_step(&answered, "flow-a"), (false, None));
            assert!(answered.lock().unwrap().is_some());

            let newer = slot(PendingRequest::Cancelled("x".to_owned()));
            assert_eq!(pending_request_step(&newer, "flow-old"), (false, None));
            assert!(newer.lock().unwrap().is_some());

            let empty: Mutex<Option<Pending>> = Mutex::new(None);
            assert_eq!(pending_request_step(&empty, "flow-a"), (false, None));
        }

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

        #[test]
        fn a_live_qr_alone_still_counts_as_live() {
            let (request, sas, qr) = slots();
            *qr.lock().unwrap() =
                Some(("flow-qr".to_owned(), FakeFlow::live("flow-qr")));
            assert!(crate::flow_slots_are_live(&request, &sas, &qr));
            assert_eq!(sas_flow(&qr).as_deref(), Some("flow-qr"));
        }

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

        #[test]
        fn releasing_never_evicts_a_newer_flow() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-new"));
            *sas.lock().unwrap() =
                Some(("flow-new".to_owned(), FakeFlow::live("flow-new")));
            *qr.lock().unwrap() =
                Some(("flow-new".to_owned(), FakeFlow::live("flow-new")));

            crate::release_flow_slots(&request, &sas, &qr, "flow-old");

            assert_eq!(request_flow(&request).as_deref(), Some("flow-new"));
            assert_eq!(sas_flow(&sas).as_deref(), Some("flow-new"));
            assert_eq!(sas_flow(&qr).as_deref(), Some("flow-new"));
        }

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
            assert_eq!(sas_flow(&qr), None);
        }

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

        #[test]
        fn teardown_takes_the_parked_flow_instead_of_discarding_it() {
            let (request, sas, qr) = slots();
            *request.lock().unwrap() = Some(FakeFlow::live("flow-teardown"));
            *sas.lock().unwrap() =
                Some(("flow-teardown".to_owned(), FakeFlow::live("flow-teardown")));

            let (taken_sas, taken_qr, taken_request) =
                crate::take_pending_flows(&request, &sas, &qr);

            assert_eq!(
                taken_request.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            assert_eq!(
                taken_sas.as_ref().map(|f| f.flow_id.as_str()),
                Some("flow-teardown")
            );
            assert!(taken_qr.is_none());
            assert_eq!(request_flow(&request), None);
            assert_eq!(sas_flow(&sas), None);
        }

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
                *req.lock().unwrap() = Some(FakeFlow::live("flow-new"));
                panic!("driver blew up");
            });

            assert!(result.is_err());
            assert_eq!(request_flow(&request).as_deref(), Some("flow-new"));
        }
    }

    mod qr_verification {
        use matrix_sdk::ruma::events::key::verification::VerificationMethod;

        #[test]
        fn we_never_advertise_a_scanner_we_do_not_have() {
            let methods = crate::advertised_verification_methods();
            assert!(!methods.contains(&VerificationMethod::QrCodeScanV1));
        }

        #[test]
        fn showing_a_qr_is_advertised_together_with_reciprocate() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::QrCodeShowV1));
            assert!(methods.contains(&VerificationMethod::ReciprocateV1));
        }

        #[test]
        fn sas_remains_advertised_as_the_fallback() {
            let methods = crate::advertised_verification_methods();
            assert!(methods.contains(&VerificationMethod::SasV1));
            assert_eq!(methods.len(), 3);
        }

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

        #[test]
        fn modules_pack_row_major_msb_first_with_a_fresh_byte_per_row() {
            let modules = vec![
                true, false, false,
                false, false, false,
                false, false, true,
            ];
            let bits = crate::pack_qr_modules(&modules, 3).expect("packed");
            let bytes = decode(&bits);
            assert_eq!(bytes.len(), 3);
            assert_eq!(bytes[0], 0b1000_0000);
            assert_eq!(bytes[1], 0b0000_0000);
            assert_eq!(bytes[2], 0b0010_0000);
        }

        #[test]
        fn a_row_wider_than_one_byte_still_starts_the_next_row_fresh() {
            let size = 9;
            let mut modules = vec![false; size * size];
            modules[0] = true;
            modules[size + 8] = true;
            let bytes = decode(&crate::pack_qr_modules(&modules, size).expect("packed"));
            assert_eq!(bytes.len(), 2 * 9);
            assert_eq!(bytes[0], 0b1000_0000);
            assert_eq!(bytes[1], 0);
            assert_eq!(bytes[2], 0);
            assert_eq!(bytes[3], 0b1000_0000);
        }

        #[test]
        fn an_all_dark_grid_sets_every_module_bit_and_pads_with_zeros() {
            let size = 3;
            let bytes =
                decode(&crate::pack_qr_modules(&vec![true; size * size], size).expect("packed"));
            assert_eq!(bytes.len(), 3);
            for byte in bytes {
                assert_eq!(byte, 0b1110_0000);
            }
        }

        #[test]
        fn malformed_grids_are_refused_rather_than_rendered() {
            assert!(crate::pack_qr_modules(&[], 0).is_none());
            assert!(crate::pack_qr_modules(&[true, false], 3).is_none());
            assert!(crate::pack_qr_modules(&[true; 9], 2).is_none());
            let oversized = crate::QR_MAX_MODULES + 1;
            assert!(crate::pack_qr_modules(&vec![false; 4], oversized).is_none());
        }

        #[test]
        fn packing_round_trips_to_the_same_grid() {
            let size = 21;
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

    #[test]
    fn secret_retry_ladder_is_bounded_and_ordered() {
        let d0 = super::next_secret_retry_delay(0).expect("first attempt");
        let d1 = super::next_secret_retry_delay(1).expect("second attempt");
        let d2 = super::next_secret_retry_delay(2).expect("third attempt");
        assert!(d0 < d1 && d1 < d2, "delays must back off");
        assert_eq!(super::next_secret_retry_delay(3), None);
        assert_eq!(super::next_secret_retry_delay(usize::MAX), None);
    }

    #[test]
    fn secret_recovery_missing_decision_table() {
        let missing = super::secret_recovery_missing;
        assert!(missing(false, Some(true), false));
        assert!(missing(false, Some(false), false));
        assert!(missing(false, None, true));
        assert!(missing(true, Some(true), false));
        assert!(!missing(true, Some(true), true));
        assert!(!missing(true, Some(false), false));
        assert!(!missing(true, None, false));
    }

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
        assert_eq!(room_ordering_timestamp_ms(&edit, None), 0);
        let plain = remote(
            json!({ "msgtype": "m.text", "body": "new words" }),
            "m.room.message",
        );
        assert_eq!(room_ordering_timestamp_ms(&plain, Some(7)), 42);
    }

    #[test]
    fn redact_matrix_id_matches_the_cpp_logs() {
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

    #[test]
    fn previews_made_of_words_cross_as_a_kind_and_text_does_not() {
        use super::{latest_event_preview_line, preview_wire};
        let wire = |content: serde_json::Value, event_type: &str| {
            preview_wire(&latest_event_preview_line(&remote(content, event_type)))
        };
        let triple = |text: &str, kind: &str, arg: &str| {
            (text.to_owned(), kind.to_owned(), arg.to_owned())
        };
        assert_eq!(
            wire(json!({ "msgtype": "m.image", "body": "", "url": "mxc://e.org/i" }), "m.room.message"),
            triple("Image", "image", "")
        );
        assert_eq!(
            wire(json!({ "msgtype": "m.image", "body": "cat.png", "url": "mxc://e.org/c" }), "m.room.message"),
            triple("cat.png", "", "")
        );
        assert_eq!(
            wire(
                json!({ "msgtype": "m.audio", "body": "t12.ogg", "url": "mxc://e.org/v",
                        "org.matrix.msc3245.voice": {} }),
                "m.room.message"
            ),
            triple("Voice message", "voice", "")
        );
        assert_eq!(
            wire(json!({ "body": "", "url": "mxc://e.org/s", "info": {} }), "m.sticker"),
            triple("Sticker", "sticker", "")
        );
        assert_eq!(
            wire(
                json!({
                    "org.matrix.msc3381.poll.start": {
                        "question": { "org.matrix.msc1767.text": "Lunch?" },
                        "kind": "org.matrix.msc3381.poll.disclosed",
                        "max_selections": 1,
                        "answers": [{ "id": "a", "org.matrix.msc1767.text": "Yes" }],
                    },
                    "org.matrix.msc1767.text": "Lunch?\n1. Yes",
                }),
                "org.matrix.msc3381.poll.start"
            ),
            triple("Poll: Lunch?", "poll_question", "Lunch?")
        );
        let item = |name: &str| {
            json!({ "itemtype": "m.image", "body": name, "filename": name,
                    "url": format!("mxc://e.org/{name}") })
        };
        assert_eq!(
            wire(
                json!({ "msgtype": "dm.filament.gallery", "body": "",
                        "itemtypes": [item("a.png"), item("b.png"), item("c.png")] }),
                "m.room.message"
            ),
            triple("3 images", "images", "3")
        );
        assert_eq!(
            wire(json!({ "msgtype": "m.text", "body": "hello" }), "m.room.message"),
            triple("hello", "", "")
        );
    }

    #[test]
    fn a_body_cannot_pose_as_a_kind() {
        use super::{latest_event_preview_line, preview_wire, split_kind_preview, PREVIEW_KIND_MARK};
        let forged = format!("{PREVIEW_KIND_MARK}image{PREVIEW_KIND_MARK}x");
        for msgtype in ["m.text", "m.notice", "m.emote", "m.file"] {
            let line = latest_event_preview_line(&remote(
                json!({ "msgtype": msgtype, "body": forged, "url": "mxc://e.org/f" }),
                "m.room.message",
            ));
            assert!(!line.contains(PREVIEW_KIND_MARK), "{msgtype}: {line:?}");
            assert_eq!(split_kind_preview(&line), None);
            assert_eq!(preview_wire(&line).1, "");
        }
        let item = json!({ "itemtype": "m.image", "body": "a.png", "filename": "a.png",
                           "url": "mxc://e.org/a" });
        let others = [
            remote(
                json!({ "msgtype": "dm.filament.gallery", "body": forged,
                        "itemtypes": [item.clone(), item] }),
                "m.room.message",
            ),
            remote(
                json!({
                    "org.matrix.msc3381.poll.start": {
                        "question": { "org.matrix.msc1767.text": forged },
                        "kind": "org.matrix.msc3381.poll.disclosed",
                        "max_selections": 1,
                        "answers": [{ "id": "a", "org.matrix.msc1767.text": "Yes" }],
                    },
                    "org.matrix.msc1767.text": forged,
                }),
                "org.matrix.msc3381.poll.start",
            ),
            remote(
                json!({
                    "msgtype": "m.text",
                    "body": format!("* {forged}"),
                    "m.new_content": { "msgtype": "m.text", "body": forged },
                    "m.relates_to": { "rel_type": "m.replace", "event_id": "$original" },
                }),
                "m.room.message",
            ),
        ];
        for n in [0, 2] {
            let line = latest_event_preview_line(&others[n]);
            assert!(!line.is_empty(), "case {n} produced no preview");
            assert!(!line.contains(PREVIEW_KIND_MARK), "case {n}: {line:?}");
            assert_eq!(split_kind_preview(&line), None, "case {n}");
            assert_eq!(preview_wire(&line).1, "", "case {n}");
        }
        let (_, kind, arg) = preview_wire(&latest_event_preview_line(&others[1]));
        assert_eq!(kind, "poll_question");
        assert_eq!(arg, "image x");
    }
}

#[cfg(test)]
mod live_space_order_tests {
    use super::*;

    #[test]
    #[ignore = "local homeserver; requires LIGHTNING_TEST_* credentials"]
    fn shared_order_round_trips_on_local_homeserver() {
        let homeserver = std::env::var("LIGHTNING_TEST_HOMESERVER").expect("test server");
        assert_eq!(homeserver, "http://127.0.0.1:18008");
        let user = std::env::var("LIGHTNING_TEST_USER").expect("test user");
        let password = std::env::var("LIGHTNING_TEST_PASSWORD").expect("test password");
        let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(2)
            .enable_all().build().unwrap();
        runtime.block_on(async {
            use matrix_sdk::ruma::{api::client::room::create_room::v3::{Request, CreationContent},
                room::RoomType, serde::Raw};
            let client = matrix_sdk::Client::builder().homeserver_url(homeserver)
                .build().await.expect("client");
            client.matrix_auth().login_username(user, &password).send().await.expect("login");
            let mut request = Request::new();
            let mut creation = CreationContent::new();
            creation.room_type = Some(RoomType::Space);
            request.creation_content = Some(Raw::new(&creation).unwrap());
            request.name = Some("Lightning local default-order test".into());
            let space = client.create_room(request).await.expect("create space");
            let first = client.create_room(Request::new()).await.expect("first room");
            let second = client.create_room(Request::new()).await.expect("second room");
            let a = first.room_id().to_string();
            let b = second.room_id().to_string();
            space.send_state_event_raw("m.space.child", &a,
                json!({"via": ["localhs.test"], "order": "a", "suggested": true,
                       "org.example.keep": "preserved"})).await.expect("first child");
            space.send_state_event_raw("m.space.child", &b,
                json!({"via": ["localhs.test"], "order": 7})).await.expect("invalid-order child");
            client.sync_once(matrix_sdk::config::SyncSettings::default()).await.expect("sync children");
            assert_eq!(direct_children_of(&space).await, vec![a.clone(), b.clone()]);
            let third = client.create_room(Request::new()).await.expect("parent-only room");
            let c = third.room_id().to_string();
            third.send_state_event_raw("m.space.parent", space.room_id().as_str(),
                json!({"via": ["localhs.test"], "canonical": true})).await.expect("parent link");
            client.sync_once(matrix_sdk::config::SyncSettings::default()).await.expect("sync parent");
            assert_eq!(direct_children_of(&space).await, vec![a.clone(), b.clone()],
                "a parent-only room has no m.space.child event");
            assert!(rooms::set_space_child_order_inner(&client, space.room_id().as_str(),
                &[b.clone(), a.clone(), c.clone()], || true).await.is_err());
            client.sync_once(matrix_sdk::config::SyncSettings::default()).await.expect("sync refusal");
            assert_eq!(direct_children_of(&space).await, vec![a.clone(), b.clone()],
                "the refused save wrote nothing");
            rooms::set_space_child_order_inner(&client, space.room_id().as_str(),
                &[b.clone(), a.clone()], || true).await.expect("save shared order");
            client.sync_once(matrix_sdk::config::SyncSettings::default()).await.expect("sync order");
            assert_eq!(direct_children_of(&space).await, vec![b, a.clone()]);
            let child = RoomId::parse(a).unwrap();
            let raw = space.get_state_event_static_for_key::<SpaceChildEventContent, _>(&child)
                .await.unwrap().unwrap();
            use matrix_sdk::deserialized_responses::RawSyncOrStrippedState;
            let RawSyncOrStrippedState::Sync(raw) = raw else { panic!("sync state") };
            let value: serde_json::Value = serde_json::from_str(raw.json().get()).unwrap();
            assert_eq!(value["content"]["via"], json!(["localhs.test"]));
            assert_eq!(value["content"]["suggested"], true);
            assert_eq!(value["content"]["org.example.keep"], "preserved");
            assert!(rooms::set_space_child_order_inner(&client, space.room_id().as_str(),
                &["!not-a-child:localhs.test".into()], || true).await.is_err());
            for room in [first, second, third, space] { room.leave().await.expect("leave test room"); }
            client.matrix_auth().logout().await.expect("logout test device");
        });
    }
}

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

            let r = take(super::mx_rust_start_own_verification(handle));
            assert!(r.is_empty(), "start verification dispatch");
            let started = wait_for(
                handle,
                "verification request started",
                Duration::from_secs(30),
                |ev| ev["type"] == "verification_request_started",
            );
            let flow_id = started["flow_id"].as_str().unwrap().to_owned();

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

            std::thread::sleep(Duration::from_secs(30));
            pause_tx.send(false).ok();
            eprintln!("[live] peer: resumed");

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

            let stopped = super::mx_rust_stop_sync(handle);
            assert!(stopped >= 0);
            {
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
            let device = CString::new("").unwrap();
            let err = take(super::mx_rust_login(
                handle, hs.as_ptr(), u.as_ptr(), p.as_ptr(), device.as_ptr()));
            assert!(err.is_empty(), "login dispatch: {err}");
            wait_for(handle, "login", Duration::from_secs(60), |ev| {
                ev["type"] == "login_ok"
            });
            super::mx_rust_start_sync(handle);
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

            std::thread::sleep(Duration::from_secs(12));

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

            if let Some(encrypted_room) = env_nonempty("LIGHTNING_TEST_ENCRYPTED_ROOM") {
                let secret = format!(
                    "zephyrine-{}", std::process::id());
                let rid = CString::new(encrypted_room.clone()).unwrap();
                let body = CString::new(secret.clone()).unwrap();
                let txn = CString::new(format!("live-{}", std::process::id())).unwrap();
                let err = take(super::mx_rust_send_text(
                    handle, rid.as_ptr(), body.as_ptr(), txn.as_ptr()));
                assert!(err.is_empty(), "encrypted send dispatch: {err}");
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

            let short = CString::new("ab").unwrap();
            let _ = take(super::mx_rust_local_search(
                handle, short.as_ptr(), empty.as_ptr(), 10, 0, 9302));
            let tooshort = wait_for(handle, "too short",
                                    Duration::from_secs(30), |ev| {
                ev["type"] == "local_search_result" && ev["op_id"] == 9302
            });
            assert_eq!(tooshort["ok"], false);
            assert_eq!(tooshort["category"], "too_short");

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
                assert!(by_id("dead").is_none(), "a tombstone came back as a widget");

                let jitsi = by_id("jitsi").expect("the real widget was not found");
                let url = jitsi["url"].as_str().unwrap_or("");
                assert!(url.starts_with("https://meet.example.org/"), "{url}");
                assert!(!url.contains('$'), "a variable was left in: {url}");
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

    #[test]
    fn a_search_whose_index_cannot_open_still_answers_its_op_id() {
        let dir = tempfile_dir("lightning-search-unopenable");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("store dir");
        let store = CString::new(dir.to_string_lossy().as_ref()).unwrap();
        let handle = super::mx_rust_create(store.as_ptr());
        assert!(!handle.is_null(), "bridge handle");
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
        let homeserver = serve(versions_body());
        let delegating = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{homeserver}"}}}}"#
        ));

        let client = runtime()
            .block_on(build_client(&format!("http://{delegating}"), &PathBuf::new()))
            .expect("delegated login must build a client");

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
        let homeserver = serve(versions_body());

        let client = runtime()
            .block_on(build_client(&format!("http://{homeserver}"), &PathBuf::new()))
            .expect("a direct homeserver URL must still work");

        assert_eq!(
            client.homeserver().port(),
            homeserver.rsplit(':').next().and_then(|p| p.parse::<u16>().ok()),
        );
    }

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
        let _ = TcpStream::connect(addr);
        for _ in 0..200 {
            if TcpStream::connect(addr).is_err() {
                return;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("the test homeserver would not stop listening");
    }

    #[test]
    fn a_restore_survives_the_homeserver_going_away() {
        let (homeserver, stop) = serve_stoppable(versions_body());
        let store = StoreDir::new("offline-restore");
        let typed = format!("http://{homeserver}");
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));

        runtime().block_on(async {
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

            stop_serving(&homeserver, &stop);

            assert!(
                build_client(&typed, store.path()).await.is_err(),
                "the test server is still answering; the rest proves nothing"
            );

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

        let trace = traced(true);
        assert_eq!(trace["store_opens"], 1, "{trace}");
        assert_eq!(trace["offline_fallback"], false, "{trace}");
        assert_eq!(trace["fallback_build_ms"], 0, "{trace}");

        stop_serving(&homeserver, &stop);
        let trace = traced(false);
        assert_eq!(trace["store_opens"], 2, "{trace}");
        assert_eq!(trace["offline_fallback"], true, "{trace}");
    }

    #[test]
    fn a_reachable_homeserver_is_still_rediscovered_on_every_restore() {
        let store = StoreDir::new("rediscover");
        let events: Arc<Mutex<VecDeque<String>>> =
            Arc::new(Mutex::new(VecDeque::new()));

        let first = serve(versions_body());
        let delegating_first = serve(format!(
            r#"{{"m.homeserver":{{"base_url":"http://{first}"}}}}"#
        ));
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
        let bogus = serve("irrelevant".to_owned());
        let result = runtime()
            .block_on(build_client(&format!("http://{bogus}/nope"), &PathBuf::new()));
        assert!(result.is_err(), "a non-homeserver must not build a client");
    }
}

#[cfg(test)]
mod classic_sync_fault_tests {
    use super::{
        classic_sync_backoff, classify_classic_sync_error, ClassicSyncFault,
        CLASSIC_SYNC_REPORT_AFTER,
    };
    use matrix_sdk::ruma::api::error::{ErrorKind, UnknownTokenErrorData};

    #[test]
    fn a_dropped_connection_carries_no_errcode_and_is_transient() {
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
        assert_eq!(
            classify_classic_sync_error(Some(&ErrorKind::Forbidden)),
            ClassicSyncFault::Fatal
        );
        let mut revoked = UnknownTokenErrorData::new();
        assert_eq!(
            classify_classic_sync_error(Some(&ErrorKind::UnknownToken(revoked.clone()))),
            ClassicSyncFault::Fatal
        );
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
        for failures in [5u32, 6, 50, u32::MAX] {
            assert_eq!(classic_sync_backoff(failures).as_secs(), 30,
                       "backoff at {failures} failures");
        }
    }

    #[test]
    fn the_silence_is_escalated_before_the_backoff_is_at_its_ceiling() {
        assert!(classic_sync_backoff(CLASSIC_SYNC_REPORT_AFTER - 1).as_secs() <= 30);
        assert!(CLASSIC_SYNC_REPORT_AFTER >= 2,
                "one flaky request must not raise an error banner");
    }
}

#[cfg(test)]
mod classic_sync_settings_tests {
    use std::time::Duration;

    use matrix_sdk::config::SyncSettings;

    use super::{
        classic_sync_request_timeout, CLASSIC_SYNC_TIMEOUT, CLASSIC_SYNC_TIMEOUT_VERIFYING,
    };

    fn classic_settings() -> SyncSettings {
        super::classic_sync_settings(CLASSIC_SYNC_TIMEOUT)
    }

    #[test]
    fn the_first_request_says_zero_and_never_omits_the_timeout() {
        assert_eq!(classic_sync_request_timeout(true, false), Duration::ZERO);
        assert_eq!(classic_sync_request_timeout(true, true), Duration::ZERO);
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
        assert!(printed.contains("timeout: 30s"), "{printed}");
    }

    #[test]
    fn full_state_would_be_visible_if_it_were_set() {
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
        let first = Arc::new(AtomicBool::new(true));
        watch_first_sync_response(&events, &first, &steps()).await;

        let seen = drain(&events.lock().unwrap());
        assert_eq!(seen.len(), 3, "expected one report per step");
        for value in &seen {
            assert_eq!(value["type"], "sync_stalled");
            assert_eq!(value["phase"], "first_response");
        }
        assert!(seen[0]["waited_secs"].is_number());
    }

    #[tokio::test]
    async fn a_response_ends_the_watch_and_reports_nothing() {
        let events = Arc::new(Mutex::new(VecDeque::new()));
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
            resume_gap: Duration::from_secs(3600),
        }
    }

    #[tokio::test]
    async fn time_spent_suspended_is_not_counted_as_silence() {
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
        assert!(started.elapsed() >= Duration::from_millis(190),
                "given up on before the restart bound");

        let seen = drain(&events);
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

#[cfg(test)]
mod classic_sync_wedge_tests {
    use super::{
        classic_repair_must_finish, repair_classic_event_cache_once, room_unread_would_be_lost,
        run_classic_sync_with, ClassicCacheRepair, ClassicRepairProgress, ClassicSilenceBudget,
        VerificationLive, CLASSIC_CACHE_REPAIR_KEY, CLASSIC_CACHE_REPAIR_MAX_STARTS,
        CLASSIC_CACHE_REPAIR_PROGRESS_KEY, CLASSIC_SYNC_WEDGED_MESSAGE,
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

            client.event_cache().subscribe().expect("subscribe");
            let room_id = matrix_sdk::ruma::RoomId::parse(ROOM).expect("room id");
            let room = client.get_room(&room_id).expect("the sync created the room");
            let (cache, _handles) = room.event_cache().await.expect("room event cache");
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

    async fn restored_client_at(addr: &str, store: &std::path::Path) -> matrix_sdk::Client {
        let client = matrix_sdk::Client::builder()
            .homeserver_url(format!("http://{addr}"))
            .sqlite_store(store, None)
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

    async fn cached_ids(client: &matrix_sdk::Client, room: &str, wanted: Option<&str>) -> Vec<String> {
        client.event_cache().subscribe().expect("subscribe");
        let room_id = matrix_sdk::ruma::RoomId::parse(room).expect("room id");
        let room = client.get_room(&room_id).expect("room known to the state store");
        let (cache, _handles) = room.event_cache().await.expect("room event cache");
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        loop {
            let ids: Vec<String> = cache
                .events()
                .await
                .unwrap_or_default()
                .iter()
                .filter_map(|e| e.event_id().map(|id| id.to_string()))
                .collect();
            let done = match wanted {
                Some(id) => ids.iter().any(|have| have == id),
                None => true,
            };
            if done || tokio::time::Instant::now() > deadline {
                return ids;
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }

    fn serve_scripted_syncs(bodies: Vec<serde_json::Value>) -> String {
        let bodies = Arc::new(bodies);
        let served = Arc::new(AtomicUsize::new(0));
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let bodies = Arc::clone(&bodies);
                let served = Arc::clone(&served);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let target = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    let path = target.split_once('?').map_or(target.as_str(), |(p, _)| p);
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else if path == "/_matrix/client/v3/sync" {
                        let n = served.fetch_add(1, Ordering::SeqCst);
                        let body = bodies
                            .get(n)
                            .cloned()
                            .unwrap_or_else(|| serde_json::json!({ "next_batch": format!("s{n}") }));
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

    fn message(event_id: &str) -> serde_json::Value {
        serde_json::json!({
            "type": "m.room.message",
            "event_id": event_id,
            "sender": "@flo:example.org",
            "origin_server_ts": 1_760_000_000_000u64,
            "content": { "msgtype": "m.text", "body": "x" },
        })
    }

    fn read_receipt(event_id: &str) -> serde_json::Value {
        serde_json::json!({
            "type": "m.receipt",
            "content": { event_id: { "m.read": { "@me:example.org": { "ts": 1u64 } } } },
        })
    }

    fn temp_store(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "lightning-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos())
                .unwrap_or(0)
        ));
        std::fs::create_dir_all(&dir).expect("store dir");
        dir
    }

    fn trace_lines(events: &Arc<Mutex<VecDeque<String>>>) -> Vec<String> {
        drain(events)
            .iter()
            .filter(|v| v["type"] == "rust_trace")
            .filter_map(|v| v["line"].as_str().map(str::to_owned))
            .collect()
    }

    #[test]
    fn the_classic_cache_repair_spares_unread_mentions_and_finishes_once() {
        const MENTIONED: &str = "!mentioned:example.org";
        const QUIET: &str = "!quiet:example.org";
        const IN_MENTIONED: &str = "$in-mentioned:example.org";
        const IN_QUIET: &str = "$in-quiet:example.org";
        const LATER: &str = "$later:example.org";
        let addr = serve_scripted_syncs(vec![
            serde_json::json!({ "next_batch": "s1", "rooms": { "join": {
                MENTIONED: {
                    "timeline": { "limited": false, "prev_batch": "p0",
                                  "events": [message(IN_MENTIONED)] },
                    "unread_notifications": { "highlight_count": 1, "notification_count": 1 },
                },
                QUIET: {
                    "timeline": { "limited": false, "prev_batch": "q0",
                                  "events": [message(IN_QUIET)] },
                    "ephemeral": { "events": [read_receipt(IN_QUIET)] },
                },
            }}}),
            serde_json::json!({ "next_batch": "s2", "rooms": { "join": {
                MENTIONED: {
                    "unread_notifications": { "highlight_count": 0, "notification_count": 0 },
                    "ephemeral": { "events": [read_receipt(IN_MENTIONED)] },
                },
            }}}),
            serde_json::json!({ "next_batch": "s3", "rooms": { "join": {
                QUIET: {
                    "timeline": { "limited": false, "prev_batch": "q1",
                                  "events": [message(LATER)] },
                },
            }}}),
        ]);
        let store = temp_store("classic-repair");
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let room = |id: &str| matrix_sdk::ruma::RoomId::parse(id).expect("room id");
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            use matrix_sdk::config::SyncSettings;
            let quick = || SyncSettings::default().timeout(Duration::ZERO);
            let t0 = 1_760_000_000_000u64;

            let identity = {
                let client = restored_client_at(&addr, &store).await;
                client.event_cache().subscribe().expect("subscribe");
                let _ = client.sync_once(quick()).await;
                assert_eq!(cached_ids(&client, MENTIONED, Some(IN_MENTIONED)).await,
                           vec![IN_MENTIONED.to_owned()]);
                assert_eq!(cached_ids(&client, QUIET, Some(IN_QUIET)).await,
                           vec![IN_QUIET.to_owned()]);
                client
                    .state_store()
                    .set_custom_value(b"msg48.sentinel", b"keep".to_vec())
                    .await
                    .expect("sentinel");
                client.encryption().ed25519_key().await.expect("device key")
            };

            {
                let client = restored_client_at(&addr, &store).await;
                assert_eq!(repair_classic_event_cache_once(&client, &events, || false, || 1, t0).await,
                           ClassicCacheRepair::Deferred);
                assert_eq!(cached_ids(&client, QUIET, None).await, vec![IN_QUIET.to_owned()]);
                let recorded: ClassicRepairProgress = serde_json::from_slice(
                    &client.state_store()
                        .get_custom_value(CLASSIC_CACHE_REPAIR_PROGRESS_KEY)
                        .await.expect("read").expect("progress recorded"),
                ).expect("progress parses");
                assert_eq!((recorded.starts, recorded.first_ms), (1, t0));
            }

            {
                let client = restored_client_at(&addr, &store).await;
                assert_eq!(repair_classic_event_cache_once(&client, &events, || true, || 1, t0 + 1).await,
                           ClassicCacheRepair::Partial { cleared: 1, waiting: 1 });
                assert!(cached_ids(&client, QUIET, None).await.is_empty());
                assert_eq!(cached_ids(&client, MENTIONED, None).await,
                           vec![IN_MENTIONED.to_owned()]);
                let mentioned = client.get_room(&room(MENTIONED)).expect("room");
                assert_eq!(mentioned.unread_notification_counts().highlight_count, 1,
                           "the unread mention badge was lost");
                let quiet = client.get_room(&room(QUIET)).expect("room");
                assert_eq!(quiet.num_unread_messages(), 0,
                           "the fixture's quiet room must be READ, or it proves nothing");
                assert!(client.state_store().get_custom_value(CLASSIC_CACHE_REPAIR_KEY)
                    .await.expect("read").is_none());
                let _ = client.sync_once(quick()).await;
                assert_eq!(mentioned.unread_notification_counts().highlight_count, 0);
            }

            {
                let client = restored_client_at(&addr, &store).await;
                let asked = Arc::new(AtomicUsize::new(0));
                let probe = Arc::clone(&asked);
                let views_idle = move || probe.fetch_add(1, Ordering::SeqCst) == 0;
                assert_eq!(repair_classic_event_cache_once(&client, &events, views_idle, || 1, t0 + 2).await,
                           ClassicCacheRepair::Done { cleared: 1, forced: false });
                assert!(cached_ids(&client, MENTIONED, None).await.is_empty());
                let state = client.state_store();
                assert!(state.get_custom_value(CLASSIC_CACHE_REPAIR_KEY).await.expect("read").is_some());
                assert!(state.get_custom_value(CLASSIC_CACHE_REPAIR_PROGRESS_KEY)
                    .await.expect("read").is_none());
                assert_eq!(state.get_custom_value(b"msg48.sentinel").await.expect("read"),
                           Some(b"keep".to_vec()), "the state store lost a value it did not own");
                assert!(client.get_room(&room(QUIET)).is_some(), "the state store lost a room");
                assert_eq!(client.encryption().ed25519_key().await.as_deref(),
                           Some(identity.as_str()), "the crypto store changed");
                assert!(drain(&events).iter().any(|v| v["type"] == "event_cache_repaired"
                                                   && v["lifecycle"] == 1),
                        "the view opened during the clear was not asked to reload");
            }

            {
                let client = restored_client_at(&addr, &store).await;
                client.event_cache().subscribe().expect("subscribe");
                let _ = client.sync_once(quick()).await;
                assert!(cached_ids(&client, QUIET, Some(LATER)).await.iter().any(|id| id == LATER));
                assert_eq!(repair_classic_event_cache_once(&client, &events, || true, || 1, t0 + 3).await,
                           ClassicCacheRepair::AlreadyDone);
                assert!(cached_ids(&client, QUIET, None).await.iter().any(|id| id == LATER),
                        "a finished repair cleared the cache again");
            }
        });
        let _ = std::fs::remove_dir_all(&store);
        let lines = trace_lines(&events);
        assert_eq!(lines.iter().filter(|l| l.contains("deferred")).count(), 1, "{lines:?}");
        assert_eq!(lines.iter().filter(|l| l.contains("wait until")).count(), 1, "{lines:?}");
        assert_eq!(lines.iter().filter(|l| l.contains("finished")).count(), 1, "{lines:?}");
    }

    #[test]
    fn a_repair_waiting_on_unread_mentions_finishes_at_the_cap() {
        let at = |starts: u32, first_ms: u64| ClassicRepairProgress {
            starts,
            first_ms,
            done: Default::default(),
        };
        let day = 24 * 60 * 60 * 1000u64;
        assert!(!classic_repair_must_finish(&at(4, 0), day));
        assert!(classic_repair_must_finish(&at(5, 0), day));
        assert!(!classic_repair_must_finish(&at(1, 0), 13 * day));
        assert!(classic_repair_must_finish(&at(1, 0), 14 * day));
        assert!(!classic_repair_must_finish(&at(1, 10 * day), day));

        assert!(room_unread_would_be_lost(false, 0, 0, 1, 0), "client mention");
        assert!(room_unread_would_be_lost(false, 0, 0, 0, 1), "server highlight");
        assert!(room_unread_would_be_lost(true, 0, 3, 0, 0), "notifying unread, encrypted");
        assert!(room_unread_would_be_lost(false, 2, 0, 0, 0), "unread dot, unencrypted");
        assert!(room_unread_would_be_lost(true, 2, 0, 0, 0), "unread dot, encrypted");
        assert!(!room_unread_would_be_lost(false, 0, 0, 0, 0));
        assert!(!room_unread_would_be_lost(true, 0, 0, 0, 0));
    }

    #[test]
    fn at_the_cap_the_repair_clears_a_still_mentioned_room_and_says_so() {
        const MENTIONED: &str = "!mentioned:example.org";
        let addr = serve_scripted_syncs(vec![serde_json::json!({
            "next_batch": "s1", "rooms": { "join": { MENTIONED: {
                "timeline": { "limited": false, "prev_batch": "p0",
                              "events": [message("$m:example.org")] },
                "unread_notifications": { "highlight_count": 1, "notification_count": 1 },
            }}},
        })]);
        let store = temp_store("classic-repair-cap");
        let events: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            use matrix_sdk::config::SyncSettings;
            let client = restored_client_at(&addr, &store).await;
            client.event_cache().subscribe().expect("subscribe");
            let _ = client.sync_once(SyncSettings::default().timeout(Duration::ZERO)).await;
            let t0 = 1_760_000_000_000u64;
            for start in 1..CLASSIC_CACHE_REPAIR_MAX_STARTS {
                assert_eq!(
                    repair_classic_event_cache_once(&client, &events, || true, || 1, t0 + start as u64).await,
                    ClassicCacheRepair::Partial { cleared: 0, waiting: 1 },
                    "start {start}"
                );
            }
            let session = Arc::new(AtomicUsize::new(1));
            let ending = Arc::clone(&session);
            let lifecycle = move || ending.fetch_add(1, Ordering::SeqCst) as u64;
            assert_eq!(
                repair_classic_event_cache_once(&client, &events, || false, lifecycle, t0 + 99).await,
                ClassicCacheRepair::Done { cleared: 1, forced: true }
            );
            assert!(cached_ids(&client, MENTIONED, None).await.is_empty());
        });
        let _ = std::fs::remove_dir_all(&store);
        assert!(!drain(&events).iter().any(|v| v["type"] == "event_cache_repaired"),
                "a reload request outlived the session it was for");
        let lines = trace_lines(&events);
        assert!(lines.iter().any(|l| l.contains("finished unconditionally")), "{lines:?}");
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
        assert!(!seen.iter().any(|v| v["state"] == "running"), "{seen:?}");
        assert!(seen.iter().any(|v| v["type"] == "sync_stalled"
                                    && v["phase"] == "no_progress"), "{seen:?}");
        assert!(seen.iter().any(|v| v["type"] == "room_list_sync_state"
                                    && v["state"] == "offline"), "{seen:?}");
        assert!(reported_wedge(&events), "no sync_error for a wedged loop: {seen:?}");
    }
}

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
        assert_eq!(newest_conversation([&message, &reaction]), Some((100, false, m)));
        assert_eq!(newest_conversation([&reaction]), None);
    }

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
        for membership in ["leave", "ban", "invite"] {
            assert!(direct_peer_avatar_payload(
                "!dm:example.org",
                &member("@sam:example.org", membership, Some("mxc://example.org/x")),
                &targets(),
            )
            .is_none(), "{membership}");
        }
        assert!(direct_peer_avatar_payload(
            "!dm:example.org",
            &member("@me:example.org", "join", Some("mxc://example.org/me")),
            &targets(),
        )
        .is_none());
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

    #[test]
    fn a_resumed_device_is_judged_on_the_servers_answer() {
        assert_eq!(state(Some("AAAA"), Some(Some("AAAA"))), "matches");
        assert_eq!(state(Some("AAAA="), Some(Some("AAAA"))), "matches");
        assert_eq!(state(Some("AAAA"), Some(Some("BBBB"))), "different");
        assert_eq!(state(Some("AAAA"), Some(None)), "missing");
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
        let value: serde_json::Value = serde_json::from_str(&final_session_tokens_json(Some(
            SessionTokens { access_token: "fixture-a".to_owned(), refresh_token: None },
        )))
        .unwrap();
        assert!(value["refresh_token"].is_null());
    }
}

#[cfg(all(test, target_os = "linux"))]
mod store_close_tests;

#[cfg(test)]
mod labs_tests {
    use std::collections::VecDeque;
    use std::ffi::{c_void, CStr, CString};
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use serde_json::{json, Value};

    use super::{bgsession, labs, storeclose, EventQueueRef};

    fn labs_on() -> labs::LabsTestGuard {
        labs::LabsTestGuard::enabled()
    }

    fn labs_off() -> labs::LabsTestGuard {
        labs::LabsTestGuard::off()
    }

    unsafe fn take(ptr: *mut std::ffi::c_char) -> String {
        if ptr.is_null() {
            return String::new();
        }
        let text = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
        unsafe { super::mx_rust_free_cstring(ptr) };
        text
    }

    unsafe fn drain(handle: *mut c_void, into: &mut Vec<Value>) {
        loop {
            let raw = unsafe { take(super::mx_rust_poll_event(handle)) };
            if raw.is_empty() {
                return;
            }
            into.push(serde_json::from_str(&raw).expect("queued JSON"));
        }
    }

    unsafe fn wait_for(
        handle: *mut c_void,
        seen: &mut Vec<Value>,
        what: &str,
        pred: impl Fn(&Value) -> bool,
    ) -> Value {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            let from = seen.len();
            unsafe { drain(handle, seen) };
            if let Some(found) = seen[from..].iter().find(|v| pred(v)) {
                return found.clone();
            }
            assert!(Instant::now() < deadline, "timed out waiting for {what}: {seen:?}");
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn serve() -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        std::thread::spawn(move || {
            static BATCH: AtomicUsize = AtomicUsize::new(0);
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                std::thread::spawn(move || {
                    let mut buf = [0u8; 8192];
                    let read = stream.read(&mut buf).unwrap_or(0);
                    let request = String::from_utf8_lossy(&buf[..read]).into_owned();
                    let line = request.lines().next().unwrap_or("");
                    let target = line.split_whitespace().nth(1).unwrap_or("");
                    let path = target.split('?').next().unwrap_or("");
                    let (status, body) = if path.starts_with("/_matrix/client/versions") {
                        ("200 OK", r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#.to_owned())
                    } else if path == "/_matrix/client/v3/sync" {
                        let n = BATCH.fetch_add(1, Ordering::SeqCst);
                        ("200 OK", format!(r#"{{"next_batch":"b{n}"}}"#))
                    } else {
                        ("404 Not Found",
                         r#"{"errcode":"M_UNRECOGNIZED","error":"test"}"#.to_owned())
                    };
                    let response = format!(
                        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
                         Connection: close\r\nContent-Length: {}\r\n\r\n{body}",
                        body.len()
                    );
                    let _ = stream.write_all(response.as_bytes());
                    let _ = stream.flush();
                });
            }
        });
        format!("http://{}:{}", addr.ip(), addr.port())
    }

    struct StoreDir(PathBuf);

    impl StoreDir {
        fn new(tag: &str) -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let path = std::env::temp_dir().join(format!(
                "lightning-labs-{tag}-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).expect("store dir");
            Self(std::fs::canonicalize(&path).unwrap_or(path))
        }

        fn c(&self) -> CString {
            CString::new(self.0.to_string_lossy().as_bytes()).unwrap()
        }
    }

    impl Drop for StoreDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    const USER: &str = "@labs:localhost";
    const DEVICE: &str = "LABSDEVICE";

    fn profile(user: &str, device: &str) -> CString {
        CString::new(
            json!({
                "v": 1, "slot": "t1",
                "require_account": {"user_id": user, "device_id": device},
                "strict_trust": false, "receipt_privacy": 2, "index_all_hold": 4294967295u64,
                "floor_ms": 0, "local_rule_rooms": [], "max_blocking_threads": 8,
                "sqlite": "low_memory", "retry_limit": 1, "max_retry_time_ms": 5000,
                "watch_cap": 200
            })
            .to_string(),
        )
        .unwrap()
    }

    unsafe fn restore(handle: *mut c_void, homeserver: &str) {
        let hs = CString::new(homeserver).unwrap();
        let user = CString::new(USER).unwrap();
        let device = CString::new(DEVICE).unwrap();
        let token = CString::new("labs-fixture-token").unwrap();
        let refresh = CString::new("").unwrap();
        let started = unsafe {
            take(super::mx_rust_restore(
                handle,
                hs.as_ptr(),
                user.as_ptr(),
                device.as_ptr(),
                token.as_ptr(),
                refresh.as_ptr(),
            ))
        };
        assert!(started.is_empty(), "restore refused to start: {started}");
    }

    unsafe fn retire(handle: *mut c_void) -> String {
        let _ = unsafe { take(super::mx_rust_shutdown_tasks(handle)) };
        unsafe { take(super::mx_rust_destroy_and_report(handle)) }
    }

    #[test]
    fn flag_off_enqueue_is_byte_identical_to_todays_serialization() {
        let _labs = labs_off();
        let golden: Vec<Value> = vec![
            json!({"type": "status", "state": "syncing"}),
            json!({"type": "login_ok", "user_id": "@a:x", "access_token": "tok", "refresh_token": "ref"}),
            json!({"type": "session_tokens_refreshed", "access_token": "tok2", "refresh_token": null}),
            json!({"type": "room_list_reset", "rooms": [{"id": "!r:x", "unread_count": 2}]}),
            json!({"type": "timeline_event", "room_id": "!r:x",
                   "event": {"event_id": "$e", "timestamp_ms": 1, "push_notify": false, "body": "hi"}}),
            json!({"type": "typing_update", "room_id": "!r:x", "users": []}),
            json!({"type": "a_type_from_the_future", "x": [1, 2, {"y": "z"}]}),
        ];
        for active in [false, true] {
            if active {
                labs::enable();
            } else {
                labs::disable_for_test();
            }
            let queue: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
            for value in &golden {
                super::enqueue(&queue, value.clone());
            }
            let written: Vec<String> = queue.lock().unwrap().iter().cloned().collect();
            let expected: Vec<String> =
                golden.iter().map(|v| serde_json::to_string(v).unwrap()).collect();
            assert_eq!(written, expected, "labs active = {active}");
            assert!(written.iter().all(|w| !w.contains("\"_ep\"") && !w.contains("\"bg\"")));
        }
        labs::disable_for_test();
    }

    #[test]
    fn flag_off_restore_reserves_nothing_and_no_background_handle_exists() {
        let _labs = labs_off();
        let homeserver = serve();
        let store = StoreDir::new("off");
        let before = storeclose::reserve_calls();
        assert_eq!(super::mx_rust_labs_active(), 0);
        let background = super::mx_rust_create_background(store.c().as_ptr(), profile(USER, DEVICE).as_ptr());
        assert!(background.is_null(), "a Background handle with Labs off");
        unsafe {
            let handle = super::mx_rust_create(store.c().as_ptr());
            assert!(!handle.is_null());
            restore(handle, &homeserver);
            let mut seen = Vec::new();
            wait_for(handle, &mut seen, "login_ok", |v| {
                assert_ne!(v["type"], "login_failed", "{v}");
                v["type"] == "login_ok"
            });
            assert_eq!(storeclose::reserve_calls(), before, "a flag-off restore reserved its store");
            assert_eq!(storeclose::any_held_path(&store.0), 1, "the restored client holds it");
            assert_eq!(
                take(super::mx_rust_set_receipt_privacy(handle, 1)),
                "",
                "a foreground handle may change its receipt privacy"
            );
            let mut status = super::MxRustBgStatus::default();
            assert_eq!(super::mx_rust_bg_poll(handle, &mut status), -1, "not Background");
            assert!(take(super::mx_rust_bg_take(handle, 0)).starts_with("error: "));
            let report = retire(handle);
            assert!(report.starts_with("store_closed=true "), "{report}");
        }
        assert_eq!(storeclose::reserve_calls(), before);
    }

    #[test]
    fn a_background_restore_without_its_crypto_store_is_refused_and_creates_nothing() {
        let _labs = labs_on();
        let homeserver = serve();
        let store = StoreDir::new("missing");
        let missing = store.0.join("account-store");
        let missing_c = CString::new(missing.to_string_lossy().as_bytes()).unwrap();
        unsafe {
            assert!(
                super::mx_rust_create_background(
                    missing_c.as_ptr(),
                    CString::new("{\"v\":1}").unwrap().as_ptr()
                )
                .is_null(),
                "an invalid profile"
            );
            let handle =
                super::mx_rust_create_background(missing_c.as_ptr(), profile(USER, DEVICE).as_ptr());
            assert!(!handle.is_null());
            assert!(super::mx_rust_handle_serial(handle) > 0);
            assert!(!missing.exists(), "create made the store directory");
            restore(handle, &homeserver);
            let mut seen = Vec::new();
            let failed = wait_for(handle, &mut seen, "login_failed", |v| v["type"] == "login_failed");
            assert_eq!(failed["stage"], "store", "{failed}");
            assert_eq!(failed["reason"], "crypto_account_missing", "{failed}");
            assert!(!missing.exists(), "the refused restore created the store directory");
            for event in &seen {
                let kind = event["type"].as_str().unwrap_or("");
                assert!(labs::flows_in_background(labs::classify(kind)), "{kind} reached the queue");
            }
            let state: Value =
                serde_json::from_str(&take(super::mx_rust_bg_take_session_state(handle))).unwrap();
            assert_eq!(state["state"]["login_failed"]["reason"], "crypto_account_missing");
            assert_eq!(take(super::mx_rust_take_custody(handle)), "", "no client, no custody");
            let _ = retire(handle);
            assert!(!missing.exists(), "retirement created the store directory");

            let handle =
                super::mx_rust_create_background(store.c().as_ptr(), profile(USER, DEVICE).as_ptr());
            restore(handle, &homeserver);
            let mut seen = Vec::new();
            let failed = wait_for(handle, &mut seen, "login_failed", |v| v["type"] == "login_failed");
            assert_eq!(failed["reason"], "crypto_account_missing", "{failed}");
            assert!(
                !store.0.join("matrix-sdk-crypto.sqlite3").exists(),
                "the refused restore created the crypto store it was checking for"
            );
            assert!(!store.0.join("matrix-sdk-state.sqlite3").exists());
            assert_eq!(storeclose::any_held_path(&store.0), 0, "the refusal released its reservation");
            let _ = retire(handle);
        }
    }

    #[test]
    fn a_background_handle_waits_for_the_foreground_store_then_restores_as_background() {
        let _labs = labs_on();
        let homeserver = serve();
        let store = StoreDir::new("handover");
        unsafe {
            let foreground = super::mx_rust_create(store.c().as_ptr());
            restore(foreground, &homeserver);
            let mut fg_seen = Vec::new();
            wait_for(foreground, &mut fg_seen, "foreground login_ok", |v| {
                assert_ne!(v["type"], "login_failed", "{v}");
                v["type"] == "login_ok"
            });

            let background =
                super::mx_rust_create_background(store.c().as_ptr(), profile(USER, DEVICE).as_ptr());
            assert!(!background.is_null());
            assert_ne!(
                super::mx_rust_handle_serial(background),
                super::mx_rust_handle_serial(foreground)
            );
            restore(background, &homeserver);
            let mut seen = Vec::new();
            let refused = wait_for(background, &mut seen, "store_in_use", |v| {
                v["type"] == "login_failed"
            });
            assert_eq!(refused["reason"], "store_in_use", "{refused}");

            let report = retire(foreground);
            assert!(report.starts_with("store_closed=true "), "{report}");
            assert_eq!(storeclose::any_held_path(&store.0), 0);

            restore(background, &homeserver);
            let ok = wait_for(background, &mut seen, "background login_ok", |v| {
                assert!(
                    v["type"] != "login_failed" || v["reason"] == "store_in_use",
                    "{v}"
                );
                v["type"] == "login_ok"
            });
            assert!(ok.get("access_token").is_none() && ok.get("refresh_token").is_none());
            assert_eq!(storeclose::any_held_path(&store.0), 1);
            let state: Value =
                serde_json::from_str(&take(super::mx_rust_bg_take_session_state(background))).unwrap();
            assert!(state["state"].get("login_failed").is_none(), "a stale refusal: {state}");
            assert!(state["state"]["login_ok"]["_v"].as_u64().is_some(), "{state}");

            let custody: Value =
                serde_json::from_str(&take(super::mx_rust_take_custody(background))).unwrap();
            assert_eq!(custody["has_session"], true);
            assert_eq!(custody["access_token"], "labs-fixture-token");
            assert_eq!(custody["revoked"], false);

            let status = CString::new("").unwrap();
            assert_eq!(
                take(super::mx_rust_set_presence(background, 0, status.as_ptr())),
                "error: background handle publishes no presence"
            );
            assert!(take(super::mx_rust_set_receipt_privacy(background, 0)).starts_with("error: "));
            assert!(take(super::mx_rust_search_index_all_hold(background, 0)).starts_with("error: "));
            assert!(take(super::mx_rust_calls_set_media_capable(background, 1)).starts_with("error: "));

            let mut status = super::MxRustBgStatus::default();
            assert_eq!(super::mx_rust_bg_poll(background, &mut status), 0);
            assert!(status.session_state_version > 0, "{status:?}");
            assert_eq!(status.revoked, 0);
            let summary: Value =
                serde_json::from_str(&take(super::mx_rust_bg_take(background, 0))).unwrap();
            assert_eq!(summary["v"], 1);
            assert_eq!(summary["baseline"], true);
            assert_eq!(summary["counts"], json!([]));
            let rules = CString::new("[\"!a:localhost\"]").unwrap();
            assert_eq!(take(super::mx_rust_bg_set_local_rules(background, rules.as_ptr())), "");
            let bad = CString::new("{}").unwrap();
            assert!(take(super::mx_rust_bg_set_local_rules(background, bad.as_ptr())).starts_with("error: "));

            for event in &seen {
                let kind = event["type"].as_str().unwrap_or("");
                assert!(labs::flows_in_background(labs::classify(kind)), "{kind} reached the queue");
            }
            let report = retire(background);
            assert!(report.starts_with("store_closed=true "), "{report}");
            assert_eq!(storeclose::any_held_path(&store.0), 0);
        }
        labs::disable_for_test();
    }

    #[test]
    fn a_running_background_handle_is_promoted_in_place_to_the_foreground() {
        let _labs = labs_on();
        let homeserver = serve();
        let store = StoreDir::new("promote");
        unsafe {
            let foreground = super::mx_rust_create(store.c().as_ptr());
            restore(foreground, &homeserver);
            let mut fg_seen = Vec::new();
            wait_for(foreground, &mut fg_seen, "foreground login_ok", |v| {
                assert_ne!(v["type"], "login_failed", "{v}");
                v["type"] == "login_ok"
            });
            assert!(take(super::mx_rust_bg_promote(foreground)).starts_with("error: "));
            let report = retire(foreground);
            assert!(report.starts_with("store_closed=true "), "{report}");

            let background =
                super::mx_rust_create_background(store.c().as_ptr(), profile(USER, DEVICE).as_ptr());
            assert!(!background.is_null());
            assert!(take(super::mx_rust_bg_promote(background)).starts_with("error: "));
            let mut status = super::MxRustBgStatus::default();
            assert_eq!(super::mx_rust_bg_poll(background, &mut status), 0, "a refusal changed the mode");

            restore(background, &homeserver);
            let mut seen = Vec::new();
            wait_for(background, &mut seen, "background login_ok", |v| {
                assert_ne!(v["type"], "login_failed", "{v}");
                v["type"] == "login_ok"
            });
            assert_eq!(storeclose::any_held_path(&store.0), 1);

            assert_eq!(take(super::mx_rust_bg_promote(background)), "");
            let mut promoted = Vec::new();
            let adopted = wait_for(background, &mut promoted, "adopted login_ok", |v| {
                v["type"] == "login_ok"
            });
            assert_eq!(adopted["adopted"], true, "{adopted}");
            assert_eq!(adopted["user_id"], USER, "{adopted}");
            assert!(adopted.get("access_token").is_none() && adopted.get("refresh_token").is_none());
            assert_eq!(storeclose::any_held_path(&store.0), 1, "a second open of the store");
            assert_eq!(super::mx_rust_bg_poll(background, &mut status), -1, "still Background");
            assert_eq!(take(super::mx_rust_set_receipt_privacy(background, 1)), "");
            assert!(take(super::mx_rust_bg_promote(background)).starts_with("error: "));
            super::mx_rust_start_sync(background);
            wait_for(background, &mut promoted, "foreground syncing", |v| {
                v["type"] == "status" && v["state"] == "syncing"
            });
            let report = retire(background);
            assert!(report.starts_with("store_closed=true "), "{report}");
            assert_eq!(storeclose::any_held_path(&store.0), 0);
        }
        labs::disable_for_test();
    }

    #[test]
    fn a_second_teardown_never_clears_a_newer_handles_account_state() {
        use crate::previewbackfill::{remembered, Remembered, RunGuard};
        let user: matrix_sdk::ruma::OwnedUserId = "@forget-once:localhost".try_into().unwrap();
        let room = matrix_sdk::ruma::room_id!("!forget-once:localhost");
        let identity = Some((user.to_string(), "OLDDEVICE".to_owned()));
        let old = super::RustClient::new(std::env::temp_dir().join("lightning-labs-forget-once"))
            .expect("bridge");
        let old_run = RunGuard::new(user.clone());
        old_run.remember(room, Remembered::Message("old session".into(), 1));
        assert!(old.forget_account_state_once(identity.clone()), "the first teardown clears");
        assert_eq!(remembered(&user, room), None);
        drop(old_run);
        let new_run = RunGuard::new(user.clone());
        new_run.remember(room, Remembered::Message("new session".into(), 2));
        assert!(!old.forget_account_state_once(identity));
        assert_eq!(remembered(&user, room), Some(Remembered::Message("new session".into(), 2)));
        drop(new_run);
        drop(old);
    }

    #[test]
    fn a_background_rotation_moves_custody_before_its_wait_and_is_answered() {
        let _labs = labs_on();
        let store = StoreDir::new("ledger");
        let path = store.c();
        let handle =
            super::mx_rust_create_background(path.as_ptr(), profile(USER, DEVICE).as_ptr());
        assert!(!handle.is_null(), "a background handle");
        let bridge = unsafe { super::bridge(handle) }.expect("bridge");
        let mut status = super::MxRustBgStatus::default();
        assert_eq!(unsafe { super::mx_rust_bg_poll(handle, &mut status) }, 0);
        let before = status.token_generation;
        let generation = bridge.token_ledger.record(matrix_sdk::SessionTokens {
            access_token: "rotated-access".to_owned(),
            refresh_token: Some("rotated-refresh".to_owned()),
        });
        assert_eq!(bridge.token_ledger.generation(), generation);
        assert_eq!(unsafe { super::mx_rust_bg_poll(handle, &mut status) }, 0);
        assert_eq!(status.token_generation, before + 1, "custody did not move with the rotation");
        unsafe { super::mx_rust_session_tokens_persisted(handle, generation, 1) };
        assert_eq!(
            bridge.token_ledger.wait_for_answer(generation),
            crate::sessiontokens::WaitOutcome::Saved
        );
        assert_eq!(unsafe { take(super::mx_rust_take_custody(handle)) }, "");
        let _ = unsafe { retire(handle) };

        let fg_store = StoreDir::new("ledger-fg");
        let fg_path = fg_store.c();
        let fg = super::mx_rust_create(fg_path.as_ptr());
        assert!(!fg.is_null());
        let fg_bridge = unsafe { super::bridge(fg) }.expect("bridge");
        let fg_before = fg_bridge.custody.generation();
        fg_bridge.token_ledger.record(matrix_sdk::SessionTokens {
            access_token: "fg-access".to_owned(),
            refresh_token: None,
        });
        assert_eq!(fg_bridge.custody.generation(), fg_before);
        let _ = unsafe { retire(fg) };
    }

    #[test]
    fn a_background_poll_counts_as_a_drain_for_a_waiting_rotation() {
        let _labs = labs_on();
        let store = StoreDir::new("ledger-drain");
        let path = store.c();
        let handle =
            super::mx_rust_create_background(path.as_ptr(), profile(USER, DEVICE).as_ptr());
        assert!(!handle.is_null());
        let bridge = unsafe { super::bridge(handle) }.expect("bridge");
        let ledger = std::sync::Arc::clone(&bridge.token_ledger);
        let generation = ledger.record(matrix_sdk::SessionTokens {
            access_token: "rotated-access".to_owned(),
            refresh_token: Some("rotated-refresh".to_owned()),
        });
        let waiter = {
            let ledger = std::sync::Arc::clone(&ledger);
            std::thread::spawn(move || ledger.wait_for_answer(generation))
        };
        let started = Instant::now();
        let mut status = super::MxRustBgStatus::default();
        while started.elapsed() < Duration::from_millis(800) {
            assert_eq!(unsafe { super::mx_rust_bg_poll(handle, &mut status) }, 0);
            std::thread::sleep(Duration::from_millis(100));
        }
        unsafe { super::mx_rust_session_tokens_persisted(handle, generation, 1) };
        let outcome = waiter.join().expect("waiter");
        assert_eq!(outcome, crate::sessiontokens::WaitOutcome::Saved,
                   "a polling host was taken for a stalled drain");
        let _ = unsafe { retire(handle) };
    }

    #[test]
    fn a_background_command_lane_refuses_session_tokens_refreshed() {
        let _labs = labs_on();
        let store = StoreDir::new("lane-refuse");
        let path = store.c();
        let handle =
            super::mx_rust_create_background(path.as_ptr(), profile(USER, DEVICE).as_ptr());
        assert!(!handle.is_null());
        let bridge = unsafe { super::bridge(handle) }.expect("bridge");
        super::enqueue_terminal_routed(
            &bridge.command_events,
            None,
            None,
            json!({
                "type": "session_tokens_refreshed",
                "access_token": "secret-access",
                "refresh_token": "secret-refresh",
                "generation": 1,
            }),
        );
        assert!(bridge.command_events.lock().unwrap().is_empty(),
                "a token rode a Background handle's command lane");
        assert_eq!(unsafe { take(super::mx_rust_poll_command_event(handle)) }, "");
        let _ = unsafe { retire(handle) };

        let fg_store = StoreDir::new("lane-fg");
        let fg_path = fg_store.c();
        let fg = super::mx_rust_create(fg_path.as_ptr());
        let fg_bridge = unsafe { super::bridge(fg) }.expect("bridge");
        super::enqueue_terminal_routed(
            &fg_bridge.command_events,
            None,
            None,
            json!({ "type": "session_tokens_refreshed", "access_token": "a", "generation": 1 }),
        );
        assert_eq!(fg_bridge.command_events.lock().unwrap().len(), 1);
        let _ = unsafe { retire(fg) };
    }

    fn run_classic_twice(homeserver: &str, events: &EventQueueRef) {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(homeserver)
                .build()
                .await
                .expect("client");
            let session = matrix_sdk::authentication::matrix::MatrixSession {
                meta: matrix_sdk::SessionMeta {
                    user_id: matrix_sdk::ruma::OwnedUserId::try_from(USER).unwrap(),
                    device_id: matrix_sdk::ruma::OwnedDeviceId::from(DEVICE),
                },
                tokens: matrix_sdk::SessionTokens {
                    access_token: "classic-token".to_owned(),
                    refresh_token: None,
                },
            };
            client
                .matrix_auth()
                .restore_session(session, matrix_sdk::store::RoomLoadSettings::default())
                .await
                .expect("restore");
            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel();
            let watched = Arc::clone(events);
            let watcher = async move {
                let deadline = tokio::time::Instant::now() + Duration::from_secs(30);
                while tokio::time::Instant::now() < deadline {
                    let runs = watched
                        .lock()
                        .unwrap()
                        .iter()
                        .filter(|e| e.contains("\"running\""))
                        .count();
                    if runs >= 2 {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                let _ = cancel_tx.send(());
            };
            let silence = super::ClassicSilenceBudget {
                report_after: Duration::from_secs(60),
                restart_after: Duration::from_secs(120),
                resume_gap: Duration::from_secs(3600),
            };
            tokio::join!(
                super::run_classic_sync_with(
                    client,
                    Arc::clone(events),
                    Arc::new(|| false),
                    Default::default(),
                    cancel_rx,
                    silence,
                ),
                watcher
            );
        });
    }

    #[test]
    fn a_background_classic_loop_skips_its_ui_work_at_the_source() {
        let _labs = labs_on();
        let homeserver = serve();

        let foreground: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        run_classic_twice(&homeserver, &foreground);
        let fg: Vec<String> = foreground.lock().unwrap().iter().cloned().collect();
        assert!(
            fg.iter().any(|e| e.contains("\"latest_event_watch_report\"")),
            "the foreground loop must still report its watch set: {fg:?}"
        );

        let background: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let commands: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let bg_profile = labs::parse_profile(&profile(USER, DEVICE).into_string().unwrap()).unwrap();
        let state = Arc::new(bgsession::BgState::new(
            bg_profile,
            Arc::new(Mutex::new(None)),
            &background,
            &commands,
        ));
        bgsession::register(&background, &commands, &state);
        run_classic_twice(&homeserver, &background);
        let bg: Vec<Value> = background
            .lock()
            .unwrap()
            .iter()
            .map(|e| serde_json::from_str(e).unwrap())
            .collect();
        assert!(state.work_skipped() >= 2, "one skip per response: {}", state.work_skipped());
        assert!(bg.iter().any(|v| v["type"] == "initial_sync_done"), "{bg:?}");
        for event in &bg {
            let kind = event["type"].as_str().unwrap_or("");
            assert!(labs::flows_in_background(labs::classify(kind)), "{kind} reached the queue");
        }
        bgsession::unregister(&background, &commands);
        labs::disable_for_test();
    }
}

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

    #[derive(Clone, Copy)]
    enum Script {
        RefuseAlways,
        Rounds { rounds: usize, refuse: usize, answered: usize },
    }

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
        assert!(events.iter().any(|v| v["type"] == "room_list_error"
                                      && v["category"] == "authentication_retry_unknown_token"),
                "{events:?}");
    }

    #[test]
    fn refusal_spells_separated_by_running_never_add_up_to_failed() {
        let seen = Arc::new(Seen::default());
        let rounds = MODERN_AUTH_RETRY_LIMIT as usize + 2;
        let script = Script::Rounds { rounds, refuse: 4, answered: 3 };
        let (events, mode) = run(script, Arc::clone(&seen), Duration::from_secs(90), |e| {
            let retries = e.iter().filter(|v| is_auth_retry(v)).count();
            let last_retry = e.iter().rposition(is_auth_retry);
            retries > MODERN_AUTH_RETRY_LIMIT as usize
                && last_retry.is_some_and(|i| e[i..].iter().any(is_running))
        });

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
        let retries = events.iter().filter(|v| is_auth_retry(v)).count();
        assert_eq!(retries, MODERN_AUTH_RETRY_LIMIT as usize, "{events:?}");
    }

    #[test]
    fn the_budget_is_consecutive_and_running_resets_it() {
        let mut budget = AuthRetryBudget::default();
        for _ in 0..MODERN_AUTH_RETRY_LIMIT {
            assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Retry);
        }
        assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Fatal);

        let mut budget = AuthRetryBudget::default();
        for _ in 0..=MODERN_AUTH_RETRY_LIMIT {
            assert_eq!(budget.on_auth_error(), ModernAuthVerdict::Retry);
            budget.on_running();
        }

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

    #[test]
    fn only_the_first_reset_of_a_traced_sync_carries_the_switch_trace() {
        use crate::switchtrace;
        fn is_reset(v: &serde_json::Value) -> bool {
            v["type"] == "room_list_reset"
        }
        let run_once = |traced: bool| -> Vec<serde_json::Value> {
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

    #[derive(Default)]
    struct Requests {
        subscribed: Mutex<Vec<Vec<String>>>,
        expired_at: AtomicUsize,
    }

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

    const SESSION_IN_PATH: &str = "64H7XKokIx0ASkYDHZKlT5zd%2FZccz%2FcQspPNdvnNULA";

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

    async fn client_as_built(server: &MatrixMockServer) -> Client {
        let client = server
            .client_builder()
            .on_builder(|builder| builder.with_encryption_settings(encryption_settings()))
            .build()
            .await;
        client.encryption().wait_for_e2ee_initialization_tasks().await;
        client
    }

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

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_sessions_trust_rule_follows_the_sdk_on_real_devices() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        let user = client.user_id().expect("logged in").to_owned();

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
        tokio::time::sleep(Duration::from_millis(500)).await;
        assert_eq!(backup_key_fetches(&server).await, 1);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn without_a_backup_key_a_failure_fetches_nothing() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        assert!(!client.encryption().backups().are_enabled().await);

        sync_undecryptable_event(&client).await;
        tokio::time::sleep(Duration::from_millis(800)).await;
        assert_eq!(backup_key_fetches(&server).await, 0);
    }

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

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn no_request_goes_out_without_a_verified_device_of_ours() {
        let server = MatrixMockServer::new().await;
        let client = client_as_built(&server).await;
        add_own_device(&client, false).await;

        sync_undecryptable_event(&client).await;

        assert_eq!(queued_key_requests(&client).await, 0);
    }
}

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
