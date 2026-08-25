//! Live SDK timeline registry (v0.5.7).
//!
//! Owns the persistent `matrix_sdk_ui::Timeline` for the currently open
//! room. One active subscription at a time (the simplest safe design):
//! opening a room advances the room generation, deterministically cancels
//! the previous subscription task, drops the previous `Timeline`, and
//! builds a fresh one whose initial snapshot + incremental `VectorDiff`
//! stream are forwarded to C++ over the existing poll queue as JSON.
//!
//! Nothing in this module ever serializes key material, ciphertext, or raw
//! decrypted event JSON — item payloads carry only UI-safe metadata.

use std::{
    collections::{HashMap, VecDeque},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    },
};

use futures_util::StreamExt;
use matrix_sdk::{
    attachment::{AttachmentInfo, Thumbnail},
    room::edit::EditedContent,
    ruma::{
        api::client::receipt::create_receipt::v3::ReceiptType,
        events::{
            poll::{
                start::PollKind,
                unstable_end::UnstablePollEndEventContent,
                unstable_response::UnstablePollResponseEventContent,
                unstable_start::{
                    NewUnstablePollStartEventContent, UnstablePollAnswer,
                    UnstablePollAnswers, UnstablePollStartContentBlock,
                    UnstablePollStartEventContent,
                },
            },
            receipt::Receipt,
            rtc::notification::CallIntent,
            room::{
                message::{
                    FormattedBody, MessageFormat, MessageType,
                    RoomMessageEventContent,
                    RoomMessageEventContentWithoutRelation, TextMessageEventContent,
                },
                MediaSource,
            },
            AnyMessageLikeEventContent, Mentions,
        },
        EventId, OwnedUserId, RoomId, UserId,
    },
    Client,
};
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    timeline::{
        thread_list_service::{ThreadListItem, ThreadListService},
        AttachmentConfig, AttachmentSource, EncryptedMessage, EventSendState,
        EventTimelineItem, MsgLikeKind, PollResult, Timeline, TimelineBuilder,
        TimelineDetails, TimelineEventItemId, TimelineFocus, TimelineItem,
        TimelineItemContent, TimelineItemKind, TimelineReadReceiptTracking,
        VirtualTimelineItem,
    },
};
use serde_json::json;

use crate::enqueue;

/// v0.7 outgoing @-mentions: build an m.mentions payload from a caller-supplied
/// list of MXID strings. Invalid ids are dropped silently (the mention simply
/// isn't recorded); an all-empty/all-invalid list yields `None` so no
/// `m.mentions` is attached. `add_mentions` MUST be called on message content
/// BEFORE any relation-adding method (reply / replacement) for the mentions to
/// be set correctly.
fn mentions_from_ids(ids: Vec<String>) -> Option<Mentions> {
    // "@room" is the whole-room mention, and it travels in the SAME list as
    // the user ids on purpose. It is not a user id and never can be — a
    // Matrix id needs a domain, so UserId::parse rejects it and no real user
    // can ever collide with the sentinel. That keeps one FFI signature for
    // "who does this message mention", instead of a parallel boolean through
    // every send, thread-send and edit entry point.
    let mut room = false;
    let mut users: Vec<OwnedUserId> = Vec::new();
    for id in ids {
        if id == ROOM_MENTION_SENTINEL {
            room = true;
            continue;
        }
        if let Ok(user) = UserId::parse(&id) {
            users.push(user);
        }
    }
    if users.is_empty() && !room {
        return None;
    }
    let mut mentions = Mentions::with_user_ids(users);
    // Whether the server ACTS on it is the room's own power levels
    // (notifications.room, default 50); the client offers it only where the
    // account may trigger one, and the event is honest either way.
    mentions.room = room;
    Some(mentions)
}

/// The whole-room mention, as it crosses the FFI. Matches the text the
/// composer inserts and the id the suggestion model reports.
pub(crate) const ROOM_MENTION_SENTINEL: &str = "@room";

/// Default number of events requested per backward-pagination batch.
/// Matches the size Element X uses for scroll-triggered backfill: large
/// enough to fill a screen, small enough to stay responsive.
pub const PAGINATION_BATCH: u16 = 20;

/// Bound for joining timeline/import tasks during shutdown. Only a
/// last-resort error boundary — tasks are cancelled/joined deterministically
/// first and normally finish in milliseconds.
pub const SHUTDOWN_JOIN_TIMEOUT_SECS: u64 = 15;

type EventQueue = Arc<Mutex<VecDeque<String>>>;

/// v0.5.9: media sources captured while serializing timeline items so the
/// media bridge can later retrieve (and, via the SDK, decrypt) attachment
/// bytes. `MediaSource::Encrypted` values embed the content keys — they are
/// stored ONLY inside this Rust-side map and never serialized across the
/// FFI. The map is keyed by the item's event id (or SDK unique id for local
/// echoes) and lives only as long as the room's timeline.
pub struct StoredMedia {
    pub source: MediaSource,
    pub thumbnail: Option<MediaSource>,
    pub filename: String,
    pub mimetype: Option<String>,
    /// Declared byte size from the Matrix `info` metadata, when present.
    /// v0.7 defense-in-depth: full-payload fetches are refused pre-flight
    /// when this exceeds the class size cap.
    pub declared_size: Option<u64>,
}

/// Upper bound on how long one reaction toggle may hold its in-flight guard
/// slot. Long enough that a normal round trip never trips it, short enough
/// that a send stuck behind a dead connection cannot leave one reaction
/// permanently unclickable for the rest of the session.
const REACTION_GUARD_TIMEOUT_SECS: u64 = 30;

/// Bound for the per-room media source map. A timeline view never holds
/// anywhere near this many media items; the cap only guards runaway growth.
const MEDIA_SOURCE_CAP: usize = 4096;

struct ActiveTimeline {
    room_id: String,
    room_gen: u64,
    /// Set by the open task once the SDK `Timeline` is built. `None` while
    /// the build is still in flight (or when the build failed).
    timeline: Option<Arc<Timeline>>,
    /// The open/subscription forwarder task. Aborting it drops the diff
    /// stream, which releases the SDK timeline drop-handle and stops the
    /// timeline's internal tasks.
    task: Option<tokio::task::JoinHandle<()>>,
    /// Single-flight guard: only one backward pagination at a time.
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

/// v0.6.0: one SDK-backed thread timeline for the currently open room. The
/// SDK `Timeline` uses `TimelineFocus::Thread`, so the root and its replies
/// (and thread sends, edits, reactions, redactions through it) all use the
/// official thread machinery — no relation JSON is built by hand and no
/// second sync engine exists: this is a filtered view over the same SDK
/// room data the live timeline uses.
struct ActiveThread {
    room_id: String,
    root_event_id: String,
    thread_gen: u64,
    timeline: Option<Arc<Timeline>>,
    task: Option<tokio::task::JoinHandle<()>>,
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

/// v0.6.0 checkpoint 5: the room's paginated SDK thread list
/// (`ThreadListService`) while the Threads view is open. Live updates come
/// from the service's own event-cache listener; full (bounded, page-sized)
/// snapshots are forwarded to C++ on each update batch.
struct ActiveThreadList {
    room_id: String,
    list_gen: u64,
    service: Option<Arc<ThreadListService>>,
    task: Option<tokio::task::JoinHandle<()>>,
    pagination_busy: Arc<AtomicBool>,
}

pub struct TimelineRegistry {
    events: EventQueue,
    active: Mutex<Option<ActiveTimeline>>,
    /// v0.6.0: the open thread panel's SDK timeline, if any. A thread always
    /// belongs to the open room: opening/closing a room closes it.
    active_thread: Mutex<Option<ActiveThread>>,
    /// v0.6.0: the open room's thread list view, if any.
    active_thread_list: Mutex<Option<ActiveThreadList>>,
    thread_list_gen: AtomicU64,
    /// Bumped on every open-thread/close-thread call; stale thread events
    /// are rejected on both sides by this stamp.
    thread_gen: AtomicU64,
    /// Bumped on every open-room call. Diffs/pagination results stamped with
    /// an older generation are stale and must be ignored on both sides.
    room_gen: AtomicU64,
    /// Bumped on shutdown (sign-out / handle release). Everything stamped
    /// with an older lifecycle is stale.
    lifecycle_gen: AtomicU64,
    /// v0.5.9: media sources for the currently open room's items. Cleared
    /// on every room open and on shutdown. Never crosses the FFI.
    media_sources: Mutex<HashMap<String, StoredMedia>>,
    /// 2026-08-18 tester report ("jeigu labai greitai paremovini reactionus
    /// nuo message pradeda tweakinti ir spaminti juos auto grazinti ir
    /// naikinti at the same time"): every click used to spawn its own
    /// independent `Timeline::toggle_reaction` task, and concurrent toggles
    /// for the SAME (room, event, key) each read the state the others were
    /// still changing — so a fast series of clicks raced itself into a storm
    /// of add/remove echoes. One toggle per target at a time; a click that
    /// arrives while its own target is still resolving is DROPPED, because a
    /// toggle is a request for a state flip, and queueing flips would just
    /// replay the same race a moment later. Cleared on shutdown.
    reaction_inflight: Mutex<std::collections::HashSet<String>>,
    /// v0.7 recovery supervisor: backup key-download attempts made this
    /// lifecycle ("<room>" for whole-room passes, "<room>\x1f<session>" for
    /// per-session downloads), so verified-session recovery never polls the
    /// backup endpoints. Cleared on shutdown; manual recovery clears the
    /// open room's entry to force one fresh pass. Session IDENTIFIERS only —
    /// never key material.
    backup_download_attempts: Mutex<std::collections::HashSet<String>>,
}

impl TimelineRegistry {
    pub fn new(events: EventQueue) -> Self {
        Self {
            events,
            active: Mutex::new(None),
            active_thread: Mutex::new(None),
            active_thread_list: Mutex::new(None),
            thread_list_gen: AtomicU64::new(0),
            thread_gen: AtomicU64::new(0),
            room_gen: AtomicU64::new(0),
            lifecycle_gen: AtomicU64::new(1),
            media_sources: Mutex::new(HashMap::new()),
            reaction_inflight: Mutex::new(std::collections::HashSet::new()),
            backup_download_attempts: Mutex::new(std::collections::HashSet::new()),
        }
    }

    /// Record a backup-download attempt. Returns false when the same
    /// attempt already ran this lifecycle (the caller must then skip it).
    fn mark_backup_attempt(&self, key: &str) -> bool {
        match self.backup_download_attempts.lock() {
            Ok(mut guard) => guard.insert(key.to_owned()),
            Err(_) => false,
        }
    }

    /// Forget a whole-room backup pass so an explicit user action (manual
    /// recovery-key/passphrase entry) can force one fresh download pass.
    pub fn clear_backup_attempt(&self, room_id: &str) {
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            guard.remove(room_id);
        }
    }

    /// Room id of the currently open live timeline, if any.
    pub fn active_room_id(&self) -> Option<String> {
        let guard = self.active.lock().ok()?;
        guard.as_ref().map(|active| active.room_id.clone())
    }

    fn is_current(&self, room_gen: u64, lifecycle: u64) -> bool {
        self.room_gen.load(Ordering::SeqCst) == room_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Current lifecycle generation, stamped on v0.5.9 command results so
    /// C++ can reject completions from a signed-out session.
    pub fn lifecycle(&self) -> u64 {
        self.lifecycle_gen.load(Ordering::SeqCst)
    }

    /// True while `lifecycle` is still the active session generation.
    pub fn lifecycle_current(&self, lifecycle: u64) -> bool {
        self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Record a media source captured during item serialization.
    fn remember_media(&self, key: String, media: StoredMedia) {
        if key.is_empty() {
            return;
        }
        if let Ok(mut guard) = self.media_sources.lock() {
            if guard.len() >= MEDIA_SOURCE_CAP && !guard.contains_key(&key) {
                return; // defensive cap; never realistically reached
            }
            guard.insert(key, media);
        }
    }

    /// Look up a media source for the media bridge. Clones the source so the
    /// lock is never held across an await point.
    pub fn media_source(
        &self,
        key: &str,
        thumbnail: bool,
    ) -> Option<(MediaSource, String, Option<String>, Option<u64>, bool)> {
        let guard = self.media_sources.lock().ok()?;
        let media = guard.get(key)?;
        let has_embedded_thumbnail = thumbnail && media.thumbnail.is_some();
        let source = if thumbnail {
            media.thumbnail.clone().unwrap_or_else(|| media.source.clone())
        } else {
            media.source.clone()
        };
        Some((
            source,
            media.filename.clone(),
            media.mimetype.clone(),
            media.declared_size,
            has_embedded_thumbnail,
        ))
    }

    fn clear_media(&self) {
        if let Ok(mut guard) = self.media_sources.lock() {
            guard.clear();
        }
    }

    /// Abort and forget the active timeline, if any. Returns the aborted
    /// task handle so shutdown can await its completion.
    fn take_active(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String)> {
        let mut guard = self.active.lock().ok()?;
        let active = guard.take()?;
        if let Some(task) = &active.task {
            task.abort();
        }
        // Dropping `active.timeline` here releases our Arc; the stream held
        // by the aborted task is dropped when the abort lands, which
        // releases the SDK's internal drop handle.
        Some((active.task, active.room_id))
    }

    /// Open (or re-open) the live timeline for `room_id`. Any previous
    /// timeline is cancelled first. Emits `timeline_reset` with the initial
    /// snapshot, then `timeline_diff` events for every SDK update.
    pub fn open_room(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) {
        self.open_room_inner(runtime, client, room_id, /*shrink_first=*/ false)
    }

    /// Re-open the live timeline for `room_id`, first letting the SDK's event
    /// cache SHRINK back to its last chunk — i.e. releasing everything the
    /// reader paginated in. This is Lightning's equivalent of Element's
    /// `TimelinePanel.jumpToLiveTimeline()` rebuilding its `TimelineWindow` at
    /// the live edge instead of scrolling through the backlog, and it exists
    /// for exactly one caller: an explicit user-initiated jump to the newest
    /// message from far back. It must never be wired to ordinary scrolling or
    /// pagination.
    ///
    /// The shrink itself is the SDK's, not ours: dropping the last
    /// `RoomEventCacheSubscriber` pings matrix-sdk's auto-shrink task, which
    /// calls `shrink_to_last_chunk()` when the subscriber count reaches zero.
    /// The only thing this adds is ORDERING — waiting for our own timeline to
    /// be gone, and for the shrink to actually land, before building the
    /// replacement. Without that wait the new subscription wins the race and
    /// the reopened timeline inherits the whole backlog again.
    pub fn reload_room_at_live(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) {
        self.open_room_inner(runtime, client, room_id, /*shrink_first=*/ true)
    }

    fn open_room_inner(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        shrink_first: bool,
    ) {
        // A thread panel / Threads view never survives into another room.
        self.close_thread();
        self.close_thread_list();
        let room_gen = self.room_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        self.clear_media();

        // Keep the previous task's handle when we intend to shrink: the
        // event-cache subscriber lives inside it, so the shrink cannot happen
        // until it is genuinely gone (abort() only *requests* cancellation).
        let mut previous_task: Option<tokio::task::JoinHandle<()>> = None;
        if let Some((task, old_room)) = self.take_active() {
            if shrink_first {
                previous_task = task;
            }
            enqueue(
                &self.events,
                json!({
                    "type": "timeline_closed",
                    "room_id": old_room,
                }),
            );
        }

        let pagination_busy = Arc::new(AtomicBool::new(false));
        let reached_start = Arc::new(AtomicBool::new(false));
        if let Ok(mut guard) = self.active.lock() {
            *guard = Some(ActiveTimeline {
                room_id: room_id.clone(),
                room_gen,
                timeline: None,
                task: None,
                pagination_busy: Arc::clone(&pagination_busy),
                reached_start: Arc::clone(&reached_start),
            });
        }

        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let handle = runtime.spawn(open_room_task(
            registry,
            client,
            room_id,
            room_gen,
            lifecycle,
            events,
            shrink_first,
            previous_task,
        ));

        if let Ok(mut guard) = self.active.lock() {
            match guard.as_mut() {
                Some(active) if active.room_gen == room_gen => active.task = Some(handle),
                _ => handle.abort(), // superseded before we stored the handle
            }
        } else {
            handle.abort();
        }
    }

    /// Close the active timeline (room deselected). Safe when none is open.
    pub fn close(&self) {
        // A thread (and the Threads view) belongs to the open room; neither
        // outlives it.
        self.close_thread();
        self.close_thread_list();
        // Invalidate stale pagination/send completions for the closed room.
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        if let Some((_task, old_room)) = self.take_active() {
            enqueue(
                &self.events,
                json!({
                    "type": "timeline_closed",
                    "room_id": old_room,
                }),
            );
        }
    }

    // ── v0.6.0: SDK-backed thread timelines ─────────────────────────────

    fn take_active_thread(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String, String)> {
        let mut guard = self.active_thread.lock().ok()?;
        let thread = guard.take()?;
        if let Some(task) = &thread.task {
            task.abort();
        }
        Some((thread.task, thread.room_id, thread.root_event_id))
    }

    fn thread_current(&self, thread_gen: u64, lifecycle: u64) -> bool {
        self.thread_gen.load(Ordering::SeqCst) == thread_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Open (or replace) the thread timeline for `root_event_id` in the
    /// currently relevant room. Uses `TimelineFocus::Thread`, so the SDK owns
    /// thread relations, pagination, and encryption exactly as for the live
    /// room timeline.
    pub fn open_thread(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        root_event_id: String,
    ) {
        let thread_gen = self.thread_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);

        if let Some((_task, old_room, old_root)) = self.take_active_thread() {
            enqueue(
                &self.events,
                json!({
                    "type": "thread_closed",
                    "room_id": old_room,
                    "thread_root_id": old_root,
                }),
            );
        }

        let pagination_busy = Arc::new(AtomicBool::new(false));
        let reached_start = Arc::new(AtomicBool::new(false));
        if let Ok(mut guard) = self.active_thread.lock() {
            *guard = Some(ActiveThread {
                room_id: room_id.clone(),
                root_event_id: root_event_id.clone(),
                thread_gen,
                timeline: None,
                task: None,
                pagination_busy: Arc::clone(&pagination_busy),
                reached_start: Arc::clone(&reached_start),
            });
        }

        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let handle = runtime.spawn(open_thread_task(
            registry,
            client,
            room_id,
            root_event_id,
            thread_gen,
            lifecycle,
            events,
        ));

        if let Ok(mut guard) = self.active_thread.lock() {
            match guard.as_mut() {
                Some(thread) if thread.thread_gen == thread_gen => thread.task = Some(handle),
                _ => handle.abort(),
            }
        } else {
            handle.abort();
        }
    }

    // ── v0.6.0 checkpoint 5: room thread list + threaded read state ─────

    fn take_active_thread_list(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String)> {
        let mut guard = self.active_thread_list.lock().ok()?;
        let list = guard.take()?;
        if let Some(task) = &list.task {
            task.abort();
        }
        Some((list.task, list.room_id))
    }

    fn thread_list_current(&self, list_gen: u64, lifecycle: u64) -> bool {
        self.thread_list_gen.load(Ordering::SeqCst) == list_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Open (or replace) the Threads view for `room_id`: builds the SDK
    /// ThreadListService, fetches the first page, and forwards a full
    /// snapshot on every live update batch.
    pub fn open_thread_list(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) {
        let list_gen = self.thread_list_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let _ = self.take_active_thread_list();

        let pagination_busy = Arc::new(AtomicBool::new(false));
        if let Ok(mut guard) = self.active_thread_list.lock() {
            *guard = Some(ActiveThreadList {
                room_id: room_id.clone(),
                list_gen,
                service: None,
                task: None,
                pagination_busy: Arc::clone(&pagination_busy),
            });
        }

        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let handle = runtime.spawn(open_thread_list_task(
            registry, client, room_id, list_gen, lifecycle, events,
        ));
        if let Ok(mut guard) = self.active_thread_list.lock() {
            match guard.as_mut() {
                Some(list) if list.list_gen == list_gen => list.task = Some(handle),
                _ => handle.abort(),
            }
        } else {
            handle.abort();
        }
    }

    /// Close the Threads view. Safe when none is open.
    pub fn close_thread_list(&self) {
        self.thread_list_gen.fetch_add(1, Ordering::SeqCst);
        let _ = self.take_active_thread_list();
    }

    /// Fetch the next thread-list page. Single-flight; end-of-list requests
    /// are dropped silently (the UI already knows via end_reached).
    pub fn paginate_thread_list(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
    ) -> Result<(), String> {
        let (service, list_gen, lifecycle, busy) = {
            let guard = self
                .active_thread_list
                .lock()
                .map_err(|_| "thread list lock poisoned".to_owned())?;
            let list = guard.as_ref().ok_or("no thread list open")?;
            if list.room_id != room_id {
                return Err("thread list is open for another room".to_owned());
            }
            let service = list.service.clone().ok_or("thread list not ready")?;
            (
                service,
                list.list_gen,
                self.lifecycle_gen.load(Ordering::SeqCst),
                Arc::clone(&list.pagination_busy),
            )
        };
        if busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Ok(());
        }
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let result = service.paginate().await;
            busy.store(false, Ordering::SeqCst);
            if !registry.thread_list_current(list_gen, lifecycle) {
                return;
            }
            emit_thread_list_snapshot(
                &events, &room_id, list_gen, lifecycle, &service,
                result.is_err(),
            );
        });
        Ok(())
    }

    /// Send a threaded read receipt for the open thread panel (the SDK's
    /// focus-aware mark_as_read — never a room-wide receipt).
    pub fn mark_thread_read(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        root_event_id: String,
    ) -> Result<(), String> {
        let Some((timeline, _gen, _lifecycle)) =
            self.thread_timeline_for(&room_id, &root_event_id)
        else {
            return Err("No live thread timeline is open for that root.".to_owned());
        };
        runtime.spawn(async move {
            let _ = timeline.mark_as_read(ReceiptType::Read).await;
        });
        Ok(())
    }

    /// Close the open thread timeline (panel closed / room switch). Safe
    /// when none is open.
    pub fn close_thread(&self) {
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
        if let Some((_task, old_room, old_root)) = self.take_active_thread() {
            enqueue(
                &self.events,
                json!({
                    "type": "thread_closed",
                    "room_id": old_room,
                    "thread_root_id": old_root,
                }),
            );
        }
    }

    /// Snapshot of the open thread timeline when it matches, and its stamps.
    fn thread_timeline_for(
        &self,
        room_id: &str,
        root_event_id: &str,
    ) -> Option<(Arc<Timeline>, u64, u64)> {
        let guard = self.active_thread.lock().ok()?;
        let thread = guard.as_ref()?;
        if thread.room_id != room_id || thread.root_event_id != root_event_id {
            return None;
        }
        let timeline = thread.timeline.clone()?;
        Some((timeline, thread.thread_gen, self.lifecycle_gen.load(Ordering::SeqCst)))
    }

    /// One backward pagination batch for the open thread. Single-flight and
    /// reached-start suppressed, mirroring the room path.
    pub fn paginate_thread_back(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        root_event_id: String,
        count: u16,
    ) -> Result<(), String> {
        let Some((timeline, thread_gen, lifecycle)) =
            self.thread_timeline_for(&room_id, &root_event_id)
        else {
            return Err("No live thread timeline is open for that root.".to_owned());
        };
        let (busy, reached) = {
            let guard = self
                .active_thread
                .lock()
                .map_err(|_| "thread registry lock poisoned".to_owned())?;
            let thread = guard.as_ref().ok_or("thread timeline gone")?;
            (Arc::clone(&thread.pagination_busy), Arc::clone(&thread.reached_start))
        };
        if reached.load(Ordering::SeqCst) {
            return Ok(());
        }
        if busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Ok(()); // single-flight
        }

        let count = if count == 0 { PAGINATION_BATCH } else { count };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        enqueue(
            &events,
            thread_pagination_json(&room_id, &root_event_id, thread_gen, lifecycle,
                                   "loading", json!({})),
        );
        runtime.spawn(async move {
            let result = timeline.paginate_backwards(count).await;
            busy.store(false, Ordering::SeqCst);
            if !registry.thread_current(thread_gen, lifecycle) {
                return; // stale completion after thread/room switch
            }
            let payload = match result {
                Ok(hit_start) => {
                    reached.store(hit_start, Ordering::SeqCst);
                    thread_pagination_json(&room_id, &root_event_id, thread_gen,
                                           lifecycle, "idle",
                                           json!({ "reached_start": hit_start }))
                }
                Err(_err) => thread_pagination_json(&room_id, &root_event_id,
                                                    thread_gen, lifecycle, "failed",
                                                    json!({ "category": "network" })),
            };
            enqueue(&events, payload);
        });
        Ok(())
    }

    /// Send a plain-text thread reply through the SDK. When the thread panel
    /// is open its focused timeline is used directly; otherwise a transient
    /// thread-focused timeline is built for the send, so the m.thread
    /// relation (with correct reply fallback) is ALWAYS produced by the SDK
    /// — never hand-assembled — and encrypted rooms use the normal SDK
    /// encryption path. A non-empty `in_reply_to` produces a rich reply TO
    /// THAT EVENT within the thread (the thread-focused timeline enforces
    /// ReplyWithinThread semantics itself).
    pub fn send_thread_text(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        root_event_id: String,
        body: String,
        in_reply_to: Option<String>,
        mention_user_ids: Vec<String>,
    ) -> Result<(), String> {
        let reply_to = match &in_reply_to {
            Some(id) if !id.trim().is_empty() => Some(
                EventId::parse(id)
                    .map_err(|_| "Invalid reply target event id.".to_owned())?,
            ),
            _ => None,
        };
        let mentions = mentions_from_ids(mention_user_ids);
        let events = Arc::clone(&self.events);
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        // Capture the thread generation at dispatch so a send that resolves
        // after the user has switched or closed the thread does not raise a
        // spurious cross-context error toast (mirrors the room path's
        // is_current gate). A thread switch, room switch, or logout all bump
        // thread_gen.
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &root_event_id);
        runtime.spawn(async move {
            let send_on = |timeline: Arc<Timeline>| {
                let body = body.clone();
                let reply_to = reply_to.clone();
                let mentions = mentions.clone();
                async move {
                    match reply_to {
                        Some(reply_to) => {
                            let mut content =
                                RoomMessageEventContentWithoutRelation::text_markdown(
                                    &body,
                                );
                            set_mention_plain_body(&mut content.msgtype, &body);
                            if let Some(mentions) = mentions {
                                content = content.add_mentions(mentions);
                            }
                            timeline.send_reply(content, reply_to).await.is_ok()
                        }
                        None => {
                            let mut message =
                                RoomMessageEventContent::text_markdown(&body);
                            set_mention_plain_body(&mut message.msgtype, &body);
                            if let Some(mentions) = mentions {
                                message = message.add_mentions(mentions);
                            }
                            timeline
                                .send(AnyMessageLikeEventContent::RoomMessage(
                                    message,
                                ))
                                .await
                                .is_ok()
                        }
                    }
                }
            };
            let sent = if let Some((timeline, _gen, _lc)) = open_thread {
                send_on(timeline).await
            } else {
                match build_transient_thread_timeline(&client, &room_id, &root_event_id)
                    .await
                {
                    Some(timeline) => send_on(timeline).await,
                    None => false,
                }
            };
            if !sent && registry.thread_current(thread_gen, lifecycle) {
                enqueue(
                    &events,
                    json!({
                        "type": "thread_send_failed",
                        "room_id": room_id,
                        "thread_root_id": root_event_id,
                        "lifecycle": lifecycle,
                        "category": "rejected",
                    }),
                );
            }
        });
        Ok(())
    }

    /// Send one already-built message-like content to a room or thread
    /// timeline (v0.7 polls). The room path mirrors `edit`/`redact`
    /// (generation-gated `timeline_send_failed`); the thread path mirrors
    /// `send_thread_text` (open panel timeline, else a transient
    /// thread-focused timeline so the SDK owns any thread relation).
    /// Contents that already carry an `m.reference` relation (poll response
    /// / poll end) are left untouched by the SDK's thread-aware send.
    fn send_content_to_timeline(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        content: AnyMessageLikeEventContent,
        failure_category: &'static str,
    ) -> Result<(), String> {
        if thread_root_id.trim().is_empty() {
            let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id)
            else {
                return Err("No live timeline is open for that room.".to_owned());
            };
            let registry = Arc::clone(self);
            let events = Arc::clone(&self.events);
            runtime.spawn(async move {
                if timeline.send(content).await.is_err()
                    && registry.is_current(room_gen, lifecycle)
                {
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_send_failed",
                            "room_id": room_id,
                            "room_generation": room_gen,
                            "lifecycle": lifecycle,
                            "category": failure_category,
                        }),
                    );
                }
            });
            return Ok(());
        }

        let events = Arc::clone(&self.events);
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &thread_root_id);
        runtime.spawn(async move {
            let sent = if let Some((timeline, _gen, _lc)) = open_thread {
                timeline.send(content).await.is_ok()
            } else {
                match build_transient_thread_timeline(
                    &client, &room_id, &thread_root_id,
                )
                .await
                {
                    Some(timeline) => timeline.send(content).await.is_ok(),
                    None => false,
                }
            };
            if !sent && registry.thread_current(thread_gen, lifecycle) {
                enqueue(
                    &events,
                    json!({
                        "type": "thread_send_failed",
                        "room_id": room_id,
                        "thread_root_id": thread_root_id,
                        "lifecycle": lifecycle,
                        "category": failure_category,
                    }),
                );
            }
        });
        Ok(())
    }

    /// Vote on an MSC3381 poll through the SDK timeline. An empty answer
    /// list is the standard "retract my vote" response. Aggregation (latest
    /// vote per user, spoiled/late votes) stays entirely SDK/ruma-side; the
    /// updated poll item arrives back as an in-place Set diff.
    pub fn send_poll_response(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        poll_start_event_id: String,
        answers: Vec<String>,
    ) -> Result<(), String> {
        let poll_start = EventId::parse(&poll_start_event_id)
            .map_err(|_| "Invalid poll event id.".to_owned())?;
        let content = AnyMessageLikeEventContent::UnstablePollResponse(
            UnstablePollResponseEventContent::new(answers, poll_start),
        );
        self.send_content_to_timeline(
            runtime, client, room_id, thread_root_id, content,
            "poll_response_rejected",
        )
    }

    /// End an MSC3381 poll. The SDK/homeserver enforce sender permission;
    /// the UI additionally offers this only for the user's own polls.
    pub fn end_poll(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        poll_start_event_id: String,
    ) -> Result<(), String> {
        let poll_start = EventId::parse(&poll_start_event_id)
            .map_err(|_| "Invalid poll event id.".to_owned())?;
        let content = AnyMessageLikeEventContent::UnstablePollEnd(
            UnstablePollEndEventContent::new("The poll has ended.", poll_start),
        );
        self.send_content_to_timeline(
            runtime, client, room_id, thread_root_id, content,
            "poll_end_rejected",
        )
    }

    /// Create an MSC3381 poll in a room or thread. Content is built by
    /// `build_poll_start_content` (ruma constructors only); a thread target
    /// gets its m.thread relation from the SDK's thread-focused send.
    pub fn send_poll_start(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        question: String,
        answers: Vec<String>,
        undisclosed: bool,
        max_selections: u64,
    ) -> Result<(), String> {
        let content = build_poll_start_content(
            &question, &answers, undisclosed, max_selections,
        )?;
        self.send_content_to_timeline(
            runtime, client, room_id, thread_root_id,
            AnyMessageLikeEventContent::UnstablePollStart(
                UnstablePollStartEventContent::New(content),
            ),
            "poll_create_rejected",
        )
    }

    /// One deduplicated whole-room key-download pass from the active key
    /// backup (v0.7 recovery supervisor). This is the deterministic
    /// replacement for the SDK's fire-once OneShot download, which never
    /// re-runs once a backup decryption key is stored (and whose bulk
    /// download failure is swallowed): whenever backups are usable, each
    /// room gets exactly one explicit `download_room_keys_for_room` pass
    /// per session lifecycle (plus one bounded retry), and manual recovery
    /// can force a fresh pass. Only public SDK APIs; imported keys reach
    /// open timelines through the SDK's own redecryption propagation.
    pub async fn download_backup_keys_for_room(
        self: &Arc<Self>,
        client: &Client,
        room_id: &str,
    ) {
        let backups = client.encryption().backups();
        if !backups.are_enabled().await {
            return; // no usable backup key — nothing to download
        }
        let Ok(room_ref) = RoomId::parse(room_id) else { return };
        if !self.mark_backup_attempt(room_id) {
            return; // already attempted this lifecycle
        }
        let lifecycle = self.lifecycle();
        let emit = |state: &str| {
            if self.lifecycle_current(lifecycle) {
                enqueue(
                    &self.events,
                    json!({
                        "type": "crypto_bootstrap",
                        "kind": "backup_download",
                        "state": state,
                        "count": 0,
                        "lifecycle": lifecycle,
                    }),
                );
            }
        };
        emit("started");
        let mut result = backups.download_room_keys_for_room(&room_ref).await;
        if result.is_err() && self.lifecycle_current(lifecycle) {
            // One bounded retry — a single transient network failure must
            // not silently strand history, but this never polls.
            tokio::time::sleep(std::time::Duration::from_secs(3)).await;
            result = backups.download_room_keys_for_room(&room_ref).await;
        }
        // Sanitized outcome only — no error strings (they can embed URLs
        // and backup versions), no room ids, no counts of key material.
        emit(if result.is_ok() { "ok" } else { "failed" });
        // A failed pass must stay re-runnable: the attempt mark exists to
        // dedup SUCCESSFUL passes within a lifecycle, not to pin a failure
        // until sign-out. The next Verified/BackupState edge (still
        // edge-driven, never polled) may retry.
        if result.is_err() {
            self.clear_backup_attempt(room_id);
        }
    }

    /// Deterministic stop of all timeline work: advances the lifecycle
    /// generation, aborts the subscription task and awaits it (bounded).
    /// Called before sign-out store cleanup and before handle destruction.
    pub fn shutdown(&self, runtime: &tokio::runtime::Runtime) {
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
        self.close_thread_list();
        self.clear_media();
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            guard.clear();
        }
        // A toggle spawned by the departing session can never complete into
        // the next one; leaving its key behind would block that reaction.
        if let Ok(mut guard) = self.reaction_inflight.lock() {
            guard.clear();
        }
        if let Some((task, _room, _root)) = self.take_active_thread() {
            if let Some(task) = task {
                let _ = runtime.block_on(async {
                    tokio::time::timeout(
                        std::time::Duration::from_secs(SHUTDOWN_JOIN_TIMEOUT_SECS),
                        task,
                    )
                    .await
                });
            }
        }
        if let Some((task, _room)) = self.take_active() {
            if let Some(task) = task {
                let _ = runtime.block_on(async {
                    tokio::time::timeout(
                        std::time::Duration::from_secs(SHUTDOWN_JOIN_TIMEOUT_SECS),
                        task,
                    )
                    .await
                });
            }
        }
        enqueue(&self.events, json!({ "type": "timeline_shutdown" }));
    }

    /// Snapshot of the currently active room's timeline, when it matches
    /// `room_id` and has finished building.
    fn timeline_for(&self, room_id: &str) -> Option<(Arc<Timeline>, u64, u64)> {
        let guard = self.active.lock().ok()?;
        let active = guard.as_ref()?;
        if active.room_id != room_id {
            return None;
        }
        let timeline = active.timeline.clone()?;
        Some((timeline, active.room_gen, self.lifecycle_gen.load(Ordering::SeqCst)))
    }

    /// Start one backward pagination batch. Rejected while another request
    /// is running or once the start of history has been reached.
    pub fn paginate_back(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        count: u16,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let (busy, reached) = {
            let guard = self
                .active
                .lock()
                .map_err(|_| "timeline registry lock poisoned".to_owned())?;
            let active = guard.as_ref().ok_or("timeline gone")?;
            (Arc::clone(&active.pagination_busy), Arc::clone(&active.reached_start))
        };
        if reached.load(Ordering::SeqCst) {
            // Not an error — the UI already knows via reached_start.
            return Ok(());
        }
        if busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Ok(()); // single-flight: one request at a time
        }

        let count = if count == 0 { PAGINATION_BATCH } else { count };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        enqueue(
            &events,
            json!({
                "type": "timeline_pagination",
                "room_id": room_id,
                "room_generation": room_gen,
                "lifecycle": lifecycle,
                "state": "loading",
            }),
        );
        runtime.spawn(async move {
            let result = timeline.paginate_backwards(count).await;
            busy.store(false, Ordering::SeqCst);
            if !registry.is_current(room_gen, lifecycle) {
                // Stale completion after room switch / sign-out: drop it.
                return;
            }
            match result {
                Ok(hit_start) => {
                    reached.store(hit_start, Ordering::SeqCst);
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_pagination",
                            "room_id": room_id,
                            "room_generation": room_gen,
                            "lifecycle": lifecycle,
                            "state": "idle",
                            "reached_start": hit_start,
                        }),
                    );
                }
                Err(_err) => {
                    // No message forwarding: pagination errors may embed
                    // server detail. The category is enough for the UI.
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_pagination",
                            "room_id": room_id,
                            "room_generation": room_gen,
                            "lifecycle": lifecycle,
                            "state": "failed",
                            "category": "network",
                        }),
                    );
                }
            }
        });
        Ok(())
    }

    /// Send a text message through the SDK timeline (send queue + SDK-owned
    /// local echo). The body is parsed as markdown by the SDK (formatting
    /// toolbar); plain text without markdown stays a plain m.text event.
    /// Encryption is transparent for encrypted rooms.
    pub fn send_text(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        body: String,
        mention_user_ids: Vec<String>,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let mentions = mentions_from_ids(mention_user_ids);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let mut message = RoomMessageEventContent::text_markdown(&body);
            set_mention_plain_body(&mut message.msgtype, &body);
            if let Some(mentions) = mentions {
                message = message.add_mentions(mentions);
            }
            let content = AnyMessageLikeEventContent::RoomMessage(message);
            if timeline.send(content).await.is_err()
                && registry.is_current(room_gen, lifecycle)
            {
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_send_failed",
                        "room_id": room_id,
                        "room_generation": room_gen,
                        "lifecycle": lifecycle,
                        "category": "rejected",
                    }),
                );
            }
        });
        Ok(())
    }

    /// v0.5.9: send an attachment through the SDK timeline. The send queue
    /// path (`use_send_queue`) provides an SDK-owned local echo that flows
    /// through the existing diff stream — sending, sent and failed states
    /// arrive exactly like text-message echoes, and failed uploads are
    /// retryable via the same `unwedge` route. Encryption of the attachment
    /// (and thumbnail) is handled entirely by the SDK for encrypted rooms.
    ///
    /// `op_id` identifies the queue attempt for C++; only queueing success/
    /// failure is reported here. Delivery state is item state, not op state.
    ///
    /// v0.7 video round: `thumbnail` carries an optional poster image. The
    /// SDK send queue uploads it as its OWN media request before the event
    /// is finalized, encrypting it exactly like the payload in an encrypted
    /// room, and writes the resulting source into `info.thumbnail_source`.
    /// Nothing about the thumbnail is assembled by hand here.
    #[allow(clippy::too_many_arguments)]
    pub fn send_attachment(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        source: AttachmentSource,
        mime_str: String,
        caption: Option<String>,
        info: Option<AttachmentInfo>,
        thumbnail: Option<Thumbnail>,
        op_id: u64,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let mime: mime::Mime = mime_str
            .parse()
            .map_err(|_| "Unsupported attachment MIME type.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let config = AttachmentConfig {
                txn_id: None,
                info,
                thumbnail,
                caption: caption
                    .filter(|c| !c.is_empty())
                    .map(TextMessageEventContent::plain),
                mentions: None,
                in_reply_to: None,
            };
            let result = timeline
                .send_attachment(source, mime, config)
                .use_send_queue()
                .await;
            if !registry.lifecycle_current(lifecycle) {
                return;
            }
            enqueue(
                &events,
                json!({
                    "type": "attachment_send_result",
                    "op_id": op_id,
                    "room_id": room_id,
                    "room_generation": room_gen,
                    "lifecycle": lifecycle,
                    "ok": result.is_ok(),
                    // Coarse category only; SDK errors may embed local paths
                    // or server detail that must not cross the FFI.
                    "category": if result.is_ok() { "" } else { "rejected" },
                }),
            );
        });
        Ok(())
    }

    /// Send a reply through the SDK timeline.
    pub fn send_reply(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        in_reply_to: String,
        body: String,
        mention_user_ids: Vec<String>,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let reply_to = EventId::parse(&in_reply_to)
            .map_err(|_| "Invalid reply target event id.".to_owned())?;
        let mentions = mentions_from_ids(mention_user_ids);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let mut content =
                RoomMessageEventContentWithoutRelation::text_markdown(&body);
            set_mention_plain_body(&mut content.msgtype, &body);
            if let Some(mentions) = mentions {
                content = content.add_mentions(mentions);
            }
            if timeline.send_reply(content, reply_to).await.is_err()
                && registry.is_current(room_gen, lifecycle)
            {
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_send_failed",
                        "room_id": room_id,
                        "room_generation": room_gen,
                        "lifecycle": lifecycle,
                        "category": "rejected",
                    }),
                );
            }
        });
        Ok(())
    }

    /// v0.6.1: send an attachment INTO a thread through the SDK's
    /// thread-focused timeline. Because the timeline is focused on the thread
    /// root, `send_attachment` runs the SDK's `infer_reply`, which attaches a
    /// correct `m.thread` relation (with the reply-to fallback for
    /// non-thread-aware clients) and, in encrypted rooms, encrypts the
    /// attachment — exactly as the SDK does for a thread text reply. No
    /// relation JSON is built by hand and the send never lands as an ordinary
    /// room message. If the panel is not open (a race), a transient
    /// thread-focused timeline is built for the send. The result is gated on
    /// the thread still being current so a stale echo never crosses the FFI.
    #[allow(clippy::too_many_arguments)]
    pub fn send_thread_attachment(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        root_event_id: String,
        source: AttachmentSource,
        mime_str: String,
        caption: Option<String>,
        info: Option<AttachmentInfo>,
        thumbnail: Option<Thumbnail>,
        op_id: u64,
    ) -> Result<(), String> {
        let mime: mime::Mime = mime_str
            .parse()
            .map_err(|_| "Unsupported attachment MIME type.".to_owned())?;
        let events = Arc::clone(&self.events);
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &root_event_id);
        runtime.spawn(async move {
            let timeline = match open_thread {
                Some((timeline, _gen, _lc)) => Some(timeline),
                None => {
                    build_transient_thread_timeline(&client, &room_id, &root_event_id)
                        .await
                }
            };
            let sent = if let Some(timeline) = timeline {
                let config = AttachmentConfig {
                    txn_id: None,
                    info,
                    thumbnail,
                    caption: caption
                        .filter(|c| !c.is_empty())
                        .map(TextMessageEventContent::plain),
                    mentions: None,
                    // None → infer_reply threads it from the focus; never an
                    // ordinary room message.
                    in_reply_to: None,
                };
                timeline
                    .send_attachment(source, mime, config)
                    .use_send_queue()
                    .await
                    .is_ok()
            } else {
                false
            };
            if registry.thread_current(thread_gen, lifecycle) {
                enqueue(
                    &events,
                    json!({
                        "type": "attachment_send_result",
                        "op_id": op_id,
                        "room_id": room_id,
                        "thread_root_id": root_event_id,
                        "lifecycle": lifecycle,
                        "ok": sent,
                        // Coarse category only; never local paths or detail.
                        "category": if sent { "" } else { "rejected" },
                    }),
                );
            }
        });
        Ok(())
    }

    /// Edit an existing message through the SDK timeline.
    pub fn edit(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        target_event_id: String,
        new_body: String,
        mention_user_ids: Vec<String>,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid edit target event id.".to_owned())?;
        let mentions = mentions_from_ids(mention_user_ids);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // ruma's make_replacement puts the new mentions in m.new_content and
            // the top-level content carries only the newly added mentions, so
            // attach them to the WithoutRelation content before wrapping it.
            let mut message =
                RoomMessageEventContentWithoutRelation::text_markdown(&new_body);
            set_mention_plain_body(&mut message.msgtype, &new_body);
            if let Some(mentions) = mentions {
                message = message.add_mentions(mentions);
            }
            let content = EditedContent::RoomMessage(message);
            if timeline.edit(&item_id, content).await.is_err()
                && registry.is_current(room_gen, lifecycle)
            {
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_send_failed",
                        "room_id": room_id,
                        "room_generation": room_gen,
                        "lifecycle": lifecycle,
                        "category": "edit_rejected",
                    }),
                );
            }
        });
        Ok(())
    }

    /// Toggle a reaction through the SDK timeline.
    pub fn toggle_reaction(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        target_event_id: String,
        key: String,
    ) -> Result<(), String> {
        let Some((timeline, _room_gen, _lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid reaction target event id.".to_owned())?;
        // One in-flight toggle per (room, event, key). A second click while
        // the first is still resolving is dropped rather than raced.
        //
        // The key carries the LIFECYCLE generation: shutdown() clears the
        // set, and a task spawned by the departed session still runs its
        // release afterwards. Without the stamp that release would free a
        // slot the NEXT session had just claimed, re-opening the very race
        // this guard exists to close.
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let guard_key =
            format!("{lifecycle}\u{1f}{room_id}\u{1f}{target_event_id}\u{1f}{key}");
        if !self.begin_reaction(&guard_key) {
            return Ok(());
        }
        let registry = Arc::clone(self);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // Failures surface as the reaction simply not appearing; the SDK
            // logs details. Nothing sensitive to forward.
            //
            // The await is bounded and the send is NOT cancelled by the
            // bound: a queued send that is waiting for the network can take
            // arbitrarily long, and a slot held for that whole time would
            // leave the reaction silently unclickable. The inner task keeps
            // running; only the guard is released.
            let send = tokio::spawn(async move {
                let _ = timeline.toggle_reaction(&item_id, &key).await;
            });
            let _ = tokio::time::timeout(
                std::time::Duration::from_secs(REACTION_GUARD_TIMEOUT_SECS),
                send,
            )
            .await;
            registry.end_reaction(&guard_key);
        });
        Ok(())
    }

    /// Claim the single in-flight slot for one reaction target. Returns
    /// false when a toggle for the same target is already running (the
    /// caller must then drop the request). A poisoned lock fails CLOSED:
    /// refusing a reaction is recoverable, racing one is not.
    fn begin_reaction(&self, key: &str) -> bool {
        match self.reaction_inflight.lock() {
            Ok(mut guard) => guard.insert(key.to_owned()),
            Err(_) => false,
        }
    }

    fn end_reaction(&self, key: &str) {
        if let Ok(mut guard) = self.reaction_inflight.lock() {
            guard.remove(key);
        }
    }

    /// Redact an event through the SDK timeline.
    pub fn redact(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        target_event_id: String,
        reason: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid redaction target event id.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            let reason = if reason.is_empty() { None } else { Some(reason.as_str()) };
            if timeline.redact(&item_id, reason).await.is_err()
                && registry.is_current(room_gen, lifecycle)
            {
                enqueue(
                    &events,
                    json!({
                        "type": "timeline_send_failed",
                        "room_id": room_id,
                        "room_generation": room_gen,
                        "lifecycle": lifecycle,
                        "category": "redact_rejected",
                    }),
                );
            }
        });
        Ok(())
    }

    /// Retry a failed local echo, identified by its transaction id.
    /// Uses the SDK send-queue `unwedge` path, which never duplicates the
    /// echo — the same queued item is re-attempted.
    pub fn retry_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let items = timeline.items().await;
            let handle = items.iter().rev().find_map(|item| {
                let event = item.as_event()?;
                if event.transaction_id().map(|t| t.to_string()) == Some(transaction_id.clone()) {
                    event.local_echo_send_handle()
                } else {
                    None
                }
            });
            let Some(handle) = handle else {
                if registry.is_current(room_gen, lifecycle) {
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_send_failed",
                            "room_id": room_id,
                            "room_generation": room_gen,
                            "lifecycle": lifecycle,
                            "category": "retry_target_missing",
                        }),
                    );
                }
                return;
            };
            let _ = handle.unwedge().await;
        });
        Ok(())
    }

    /// Cancel a local echo that has not reached the server yet, identified
    /// by its transaction id.
    ///
    /// `SendHandle::abort` is the whole implementation, and it is the only
    /// correct one: it aborts an in-flight MEDIA upload as well as a queued
    /// event, and it answers `Ok(false)` when the event was already sent —
    /// a race this cannot avoid, only report. Removing the item ourselves
    /// would leave the queue still holding it.
    ///
    /// A successful abort needs no event of its own: the SDK emits
    /// `CancelledLocalEvent`, the timeline drops the item, and the row
    /// disappears through the ordinary diff path.
    pub fn cancel_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let items = timeline.items().await;
            let handle = items.iter().rev().find_map(|item| {
                let event = item.as_event()?;
                if event.transaction_id().map(|t| t.to_string()) == Some(transaction_id.clone()) {
                    event.local_echo_send_handle()
                } else {
                    None
                }
            });
            let category = match handle {
                None => Some("cancel_target_missing"),
                Some(handle) => match handle.abort().await {
                    Ok(true) => None,
                    // Already on the server. Saying "cancelled" here would
                    // be a lie the room can see.
                    Ok(false) => Some("cancel_too_late"),
                    Err(_) => Some("cancel_failed"),
                },
            };
            if let Some(category) = category {
                if registry.is_current(room_gen, lifecycle) {
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_send_failed",
                            "room_id": room_id,
                            "room_generation": room_gen,
                            "lifecycle": lifecycle,
                            "category": category,
                        }),
                    );
                }
            }
        });
        Ok(())
    }

    /// Immediate decryption retry after a successful room-key import
    /// (v0.5.7, the main 0.5.7 acceptance path). `sessions_by_room` carries
    /// only Megolm *session identifiers* — never key material — and stays
    /// inside Rust. If the active timeline's room is affected, the pinned
    /// SDK `Timeline::retry_decryption` is awaited and the SDK emits
    /// in-place `Set` diffs for every newly decryptable item.
    /// v0.6.0 checkpoint 8: manual "Retry decryption" for the open room.
    /// Collects the session ids of the currently visible unable-to-decrypt
    /// items (room timeline + open thread timeline) and asks the SDK to
    /// retry them — one bounded pass per user action, no polling, no custom
    /// key transfer. v0.7: since the client runs the OneShot strategy (the
    /// SDK's per-UTD AfterDecryptionFailure download task does not exist),
    /// each visible UTD session additionally gets one deduplicated
    /// `download_room_key` attempt from the active backup before the
    /// decryption retry, so Retry can actually fetch a missing key instead
    /// of only re-checking key material that already arrived.
    pub fn retry_visible_decryption(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let thread_timeline = {
            let guard = self
                .active_thread
                .lock()
                .map_err(|_| "thread registry lock poisoned".to_owned())?;
            guard.as_ref().and_then(|thread| {
                (thread.room_id == room_id)
                    .then(|| thread.timeline.clone())
                    .flatten()
            })
        };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let mut session_ids = utd_session_ids(&timeline).await;
            if let Some(thread) = &thread_timeline {
                for id in utd_session_ids(thread).await {
                    if !session_ids.contains(&id) {
                        session_ids.push(id);
                    }
                }
            }
            if session_ids.is_empty() {
                return;
            }
            enqueue(
                &events,
                json!({
                    "type": "timeline_retry_decryption",
                    "room_id": room_id,
                    "room_generation": room_gen,
                    "lifecycle": lifecycle,
                    "state": "started",
                    "sessions": session_ids.len(),
                }),
            );
            // Bounded per-session backup downloads (identifiers only; key
            // material stays inside the SDK). `download_room_key` is a
            // harmless Ok(false) when no usable backup exists.
            let backups = client.encryption().backups();
            if backups.are_enabled().await {
                if let Ok(room_ref) = RoomId::parse(&room_id) {
                    for session_id in &session_ids {
                        let key = format!("{room_id}\u{1f}{session_id}");
                        if !registry.mark_backup_attempt(&key) {
                            continue;
                        }
                        let _ = backups
                            .download_room_key(&room_ref, session_id)
                            .await;
                    }
                }
            }
            timeline.retry_decryption(session_ids.iter().cloned()).await;
            if let Some(thread) = &thread_timeline {
                thread.retry_decryption(session_ids.iter().cloned()).await;
            }
            if !registry.is_current(room_gen, lifecycle) {
                return;
            }
            enqueue(
                &events,
                json!({
                    "type": "timeline_retry_decryption",
                    "room_id": room_id,
                    "room_generation": room_gen,
                    "lifecycle": lifecycle,
                    "state": "done",
                    "sessions": session_ids.len(),
                }),
            );
        });
        Ok(())
    }

    pub async fn retry_decryption_after_import(
        &self,
        sessions_by_room: &[(String, Vec<String>)],
    ) {
        let Some((timeline, room_gen, lifecycle, room_id)) = ({
            let guard = self.active.lock().ok();
            guard.and_then(|g| {
                g.as_ref().and_then(|active| {
                    let timeline = active.timeline.clone()?;
                    Some((
                        timeline,
                        active.room_gen,
                        self.lifecycle_gen.load(Ordering::SeqCst),
                        active.room_id.clone(),
                    ))
                })
            })
        }) else {
            return;
        };

        let Some((_, session_ids)) =
            sessions_by_room.iter().find(|(room, _)| *room == room_id)
        else {
            return;
        };
        if session_ids.is_empty() {
            return;
        }

        enqueue(
            &self.events,
            json!({
                "type": "timeline_retry_decryption",
                "room_id": room_id,
                "room_generation": room_gen,
                "lifecycle": lifecycle,
                "state": "started",
                "sessions": session_ids.len(),
            }),
        );
        timeline.retry_decryption(session_ids.iter().cloned()).await;
        if !self.is_current(room_gen, lifecycle) {
            return;
        }
        enqueue(
            &self.events,
            json!({
                "type": "timeline_retry_decryption",
                "room_id": room_id,
                "room_generation": room_gen,
                "lifecycle": lifecycle,
                "state": "done",
                "sessions": session_ids.len(),
            }),
        );
    }
}

/// Builds the SDK timeline, installs it in the registry, emits the initial
/// snapshot, then forwards every diff batch until cancelled or stale.
/// Total time EITHER half of the shrink wait may take — the join and the poll
/// loop are bounded separately, so the worst case for the whole helper is
/// twice this (~1.2 s) before it gives up and reopens without trimming.
/// Reached only when a task will not cancel AND the shrink never fires.
const SHRINK_WAIT_BUDGET_MS: u64 = 600;
/// Poll step for observing the shrink. `RoomEventCache::events()` clones every
/// event, so this is deliberately coarse.
const SHRINK_POLL_STEP_MS: u64 = 60;

/// Wait for the SDK's event cache to release the paginated backlog for
/// `room_ref`, and report how many events it held beforehand plus whether the
/// release was actually OBSERVED.
///
/// Sequence, and every step of it matters:
///   1. AWAIT the previous timeline task. `abort()` only requests
///      cancellation; until the task actually stops it still owns the
///      `Arc<Timeline>` whose internal `RoomEventCacheSubscriber` keeps the
///      cache's subscriber count above zero. A cancelled task's join returns
///      `Err(JoinError::Cancelled)` — that error IS the success signal here.
///   2. Poll the cache's event count. matrix-sdk's auto-shrink runs on its own
///      task (a channel ping from the subscriber's Drop), so there is nothing
///      to await directly; polling the PUBLIC `events()` is the only honest
///      way to know it landed. Bounded — a shrink that never happens must
///      degrade to "reopened without trimming", never to a hang.
/// Never calls `RoomEventCache::clear()`: that would also wipe the room's
/// PERSISTED events, forcing the live tail itself to be refetched.
async fn await_event_cache_shrink(
    room: &matrix_sdk::Room,
    previous_task: Option<tokio::task::JoinHandle<()>>,
) -> Option<(usize, bool)> {
    // BASELINE FIRST, while the old subscriber is still alive and the cache
    // therefore still holds the whole backlog. Sampling it after the release
    // would race the SDK's auto-shrink task: a FAST shrink could land before
    // the baseline was taken, and the poll below would then see no further
    // decrease and report `trim_shrunk: false` for a trim that in fact
    // succeeded (recheck observation, 2026-08-19).
    //
    // Room::event_cache() is the public accessor; the drop handles it returns
    // are deliberately dropped — they keep the cache's own listener tasks
    // alive, which the client already owns for the session.
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let before = cache.events().await.ok()?.len();
    if before == 0 {
        return None;
    }
    if let Some(task) = previous_task {
        // Ignore the outcome: Ok means it finished, Err(Cancelled) means the
        // abort landed. Both mean its Arc is released, which is all we need.
        // BOUNDED (review finding): abort() cancels at the next await point,
        // and nothing downstream of this helper has a timeout — the FFI is
        // fire-and-forget and the caller has already committed to the trim.
        // A task slow to reach cancellation must degrade to "reopened
        // without trimming", never stall the room indefinitely.
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(SHRINK_WAIT_BUDGET_MS),
            task,
        )
        .await;
    }
    // The release may already have landed while we awaited the task above, so
    // check ONCE before sleeping — otherwise the fastest, most common case
    // would be reported as a 60 ms-late detection at best.
    if let Ok(events) = cache.events().await {
        if events.len() < before {
            return Some((before, true));
        }
    }
    // The shrink is a channel ping plus a state write-lock acquisition, so it
    // is normally immediate; this bound exists for the pathological case.
    // NOTE `events()` CLONES every event, so poll sparingly rather than
    // tightly — the whole point is a large backlog (review finding).
    let steps = SHRINK_WAIT_BUDGET_MS / SHRINK_POLL_STEP_MS;
    for _ in 0..steps {
        tokio::time::sleep(std::time::Duration::from_millis(SHRINK_POLL_STEP_MS))
            .await;
        let Ok(events) = cache.events().await else { return Some((before, false)) };
        if events.len() < before {
            return Some((before, true));
        }
    }
    // Timed out: the cache still holds everything. Reported as NOT shrunk —
    // "we waited and nothing happened" and "we released the backlog" must
    // never be indistinguishable in the emitted data (review finding).
    Some((before, false))
}

async fn open_room_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    room_gen: u64,
    lifecycle: u64,
    events: EventQueue,
    shrink_first: bool,
    previous_task: Option<tokio::task::JoinHandle<()>>,
) {
    let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();

    let Ok(room_ref) = RoomId::parse(&room_id) else {
        emit_timeline_error(&events, &room_id, room_gen, lifecycle, "invalid_room_id");
        return;
    };


    let Some(room) = client.get_room(&room_ref) else {
        emit_timeline_error(&events, &room_id, room_gen, lifecycle, "unknown_room");
        return;
    };

    // Release the paginated backlog before rebuilding (jump-to-live only).
    // Reported honestly in the reset payload below: `trimmed_from` is the
    // event count the cache held before, so a live capture can show whether
    // the shrink actually landed rather than us assuming it did.
    let trim_report: Option<(usize, bool)> = if shrink_first {
        await_event_cache_shrink(&room, previous_task).await
    } else {
        None
    };

    // TimelineBuilder::new (not RoomExt::timeline_builder) with read-receipt
    // tracking enabled for MESSAGE-LIKE events only: Lightning renders
    // Element-style per-message receipt chips, and MessageLikeEvents keeps
    // state rows (membership, topic, …) from ever carrying receipt decoration
    // (AllEvents would attach them there too). ONE switch enables BOTH the
    // per-item receipts and the SDK's VirtualTimelineItem::ReadMarker row —
    // there is no way to get receipts without also reviving the marker row.
    // The marker's presentation policy (render the "New messages" divider,
    // but collapse it while the reader is pinned to the bottom, so the
    // own-receipt ack cycle cannot bounce a 28px divider in and out under a
    // live conversation) lives entirely in QML (MessageDelegate).
    //
    // The thread-focused timelines below deliberately KEEP tracking
    // Disabled. The SDK's receipt handling is not thread-aware (its docs:
    // read_receipts() "currently ignores threads"), so enabling it on a
    // thread timeline would attach the room's UNTHREADED receipts to thread
    // rows — wrong data, not merely missing data. Thread rows therefore
    // never emit read_by entries.
    //
    // hide_threaded_events: true asks the SDK to keep in-thread replies out of
    // the live room timeline (its `should_add` rule is
    // `thread_root.is_none() || !hide_threaded_events`). Lightning presents each
    // thread through a dedicated TimelineFocus::Thread panel plus a summary card
    // on the root, so replies belong there, not as ordinary main-timeline rows.
    // The classification is the SDK's authoritative m.thread relation — never a
    // body-text heuristic — and the thread ROOT (no thread_root, only a
    // thread_summary) still stays in the main timeline.
    let timeline = match TimelineBuilder::new(&room)
        .with_focus(TimelineFocus::Live { hide_threaded_events: true })
        .track_read_marker_and_receipts(TimelineReadReceiptTracking::MessageLikeEvents)
        .build()
        .await
    {
        Ok(timeline) => Arc::new(timeline),
        Err(_err) => {
            emit_timeline_error(&events, &room_id, room_gen, lifecycle, "build_failed");
            return;
        }
    };

    // Atomic initial-snapshot-plus-subscription: no live event can fall
    // between `items` and the first diff.
    let (items, mut stream) = timeline.subscribe().await;

    // Install the timeline handle only if this open is still current.
    {
        let Ok(mut guard) = registry.active.lock() else { return };
        match guard.as_mut() {
            Some(active) if active.room_gen == room_gen => {
                active.timeline = Some(Arc::clone(&timeline));
            }
            _ => return, // superseded while building; drop everything
        }
    }

    let snapshot: Vec<serde_json::Value> =
        items.iter().map(|item| item_to_json(item, &own_user, &registry)).collect();
    enqueue(
        &events,
        json!({
            "type": "timeline_reset",
            "room_id": room_id,
            "room_generation": room_gen,
            "lifecycle": lifecycle,
            "items": snapshot,
            // Counts only, never content: how many events the cache held
            // before a jump-to-live trim, and whether the release was
            // actually observed (both absent for an ordinary open). The
            // pair matters — a timed-out wait must not look like a
            // successful trim.
            "trimmed_from": trim_report.map(|(before, _)| before),
            "trim_shrunk": trim_report.map(|(_, shrunk)| shrunk),
        }),
    );

    // v0.7 recovery supervisor: one deduplicated key-download pass from the
    // active backup for THIS room (no-op without a usable backup key; at
    // most once per room per lifecycle). Runs concurrently — never ahead
    // of — diff forwarding; imported keys come back as ordinary in-place
    // Set diffs through the SDK's own redecryption propagation.
    {
        let registry = Arc::clone(&registry);
        let client = client.clone();
        let room = room_id.clone();
        tokio::spawn(async move {
            registry.download_backup_keys_for_room(&client, &room).await;
        });
    }

    // v0.7: hydrate room-member sender profiles. With lazy-loaded membership
    // the SDK only knows senders whose member events happened to sync, so
    // historical rows showed raw MXIDs and initials while the room list knew
    // the same person's name. `Timeline::fetch_members` runs the (idempotent,
    // SDK-deduplicated) member sync and then fills every missing sender
    // profile in place, which reaches C++ as ordinary Set diffs. It runs
    // concurrently with — never ahead of — live diff forwarding, and dies
    // with this task when the room/generation is superseded.
    let fetch_timeline = Arc::clone(&timeline);
    let fetch_members = async move { fetch_timeline.fetch_members().await };
    tokio::pin!(fetch_members);
    let mut members_fetched = false;
    loop {
        tokio::select! {
            _ = &mut fetch_members, if !members_fetched => {
                members_fetched = true;
            }
            maybe_diffs = stream.next() => {
                let Some(diffs) = maybe_diffs else { break };
                if !registry.is_current(room_gen, lifecycle) {
                    break;
                }
                for diff in diffs {
                    let value = diff_to_json(
                        &room_id, room_gen, lifecycle, &diff, &own_user, &registry,
                    );
                    enqueue(&events, value);
                }
            }
        }
    }
}

/// Build a `thread_pagination` lifecycle envelope. Shared by the initial
/// auto-load in `open_thread_task` and the scroll-driven
/// `paginate_thread_back`, so both stamp the same generation/lifecycle and
/// carry identical `state` semantics.
fn thread_pagination_json(
    room_id: &str,
    root_event_id: &str,
    thread_gen: u64,
    lifecycle: u64,
    state: &str,
    extra: serde_json::Value,
) -> serde_json::Value {
    let mut v = json!({
        "type": "thread_pagination",
        "room_id": room_id,
        "thread_root_id": root_event_id,
        "thread_generation": thread_gen,
        "lifecycle": lifecycle,
        "state": state,
    });
    if let (Some(obj), Some(add)) = (v.as_object_mut(), extra.as_object()) {
        for (k, val) in add {
            obj.insert(k.clone(), val.clone());
        }
    }
    v
}

/// v0.6.0: build the thread-focused SDK timeline and forward its snapshot +
/// diff stream, mirroring `open_room_task`. All payloads are stamped with
/// the thread generation so stale thread events can never mutate a newer
/// panel (or another room's panel).
async fn open_thread_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    root_event_id: String,
    thread_gen: u64,
    lifecycle: u64,
    events: EventQueue,
) {
    let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();

    let emit_error = |category: &str| {
        enqueue(
            &events,
            json!({
                "type": "thread_error",
                "room_id": room_id,
                "thread_root_id": root_event_id,
                "thread_generation": thread_gen,
                "lifecycle": lifecycle,
                "category": category,
            }),
        );
    };

    let Ok(room_ref) = RoomId::parse(&room_id) else {
        emit_error("invalid_room_id");
        return;
    };
    let Some(room) = client.get_room(&room_ref) else {
        emit_error("unknown_room");
        return;
    };
    let Ok(root_ref) = EventId::parse(&root_event_id) else {
        emit_error("invalid_root_id");
        return;
    };

    let timeline = match TimelineBuilder::new(&room)
        .with_focus(TimelineFocus::Thread { root_event_id: root_ref })
        .build()
        .await
    {
        Ok(timeline) => Arc::new(timeline),
        Err(_err) => {
            emit_error("build_failed");
            return;
        }
    };

    let (items, mut stream) = timeline.subscribe().await;

    {
        let Ok(mut guard) = registry.active_thread.lock() else { return };
        match guard.as_mut() {
            Some(thread) if thread.thread_gen == thread_gen => {
                thread.timeline = Some(Arc::clone(&timeline));
            }
            _ => return, // superseded while building
        }
    }

    let snapshot: Vec<serde_json::Value> =
        items.iter().map(|item| item_to_json(item, &own_user, &registry)).collect();
    let has_event_rows = items
        .iter()
        .any(|item| matches!(item.kind(), TimelineItemKind::Event(_)));
    enqueue(
        &events,
        json!({
            "type": "thread_reset",
            "room_id": room_id,
            "thread_root_id": root_event_id,
            "thread_generation": thread_gen,
            "lifecycle": lifecycle,
            "items": snapshot,
        }),
    );

    // Auto-load cold-cache threads. subscribe() only returns what the event
    // cache already holds for this thread; a thread whose replies never went
    // through live sync thread-routing (room history, or events pulled in by
    // back-pagination, which the SDK cannot place into a thread) opens with no
    // event rows even though the root's bundled thread_summary reports replies
    // — the "root says N replies, panel items=0" symptom. Kick one backward
    // pagination so the SDK fetches the thread over /relations; the fetched
    // replies arrive as diffs on the loop below. Threads that already carry
    // cached replies skip this and paginate lazily on scroll. Single-flight and
    // reached-start guarded, so a concurrent scroll cannot double-fetch.
    if !has_event_rows {
        let atomics = match registry.active_thread.lock() {
            Ok(guard) => match guard.as_ref() {
                Some(thread) if thread.thread_gen == thread_gen => Some((
                    Arc::clone(&thread.pagination_busy),
                    Arc::clone(&thread.reached_start),
                )),
                _ => return, // superseded while building
            },
            Err(_) => return,
        };
        if let Some((busy, reached)) = atomics {
            if !reached.load(Ordering::SeqCst)
                && busy
                    .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
                    .is_ok()
            {
                enqueue(
                    &events,
                    thread_pagination_json(&room_id, &root_event_id, thread_gen,
                                           lifecycle, "loading", json!({})),
                );
                let events = Arc::clone(&events);
                let registry = Arc::clone(&registry);
                let timeline = Arc::clone(&timeline);
                let room_id = room_id.clone();
                let root_event_id = root_event_id.clone();
                tokio::spawn(async move {
                    let result = timeline.paginate_backwards(PAGINATION_BATCH).await;
                    busy.store(false, Ordering::SeqCst);
                    if !registry.thread_current(thread_gen, lifecycle) {
                        return; // stale completion after thread/room switch
                    }
                    let payload = match result {
                        Ok(hit_start) => {
                            reached.store(hit_start, Ordering::SeqCst);
                            thread_pagination_json(&room_id, &root_event_id,
                                                   thread_gen, lifecycle, "idle",
                                                   json!({ "reached_start": hit_start }))
                        }
                        Err(_err) => thread_pagination_json(
                            &room_id, &root_event_id, thread_gen, lifecycle,
                            "failed", json!({ "category": "network" })),
                    };
                    enqueue(&events, payload);
                });
            }
        }
    }

    // v0.7: same member-profile hydration as the room timeline — the thread
    // panel's rows resolve sender names/avatars through the identical SDK
    // path (the member sync itself is deduplicated inside the SDK, so a
    // room + thread pair costs one /members request at most).
    let fetch_timeline = Arc::clone(&timeline);
    let fetch_members = async move { fetch_timeline.fetch_members().await };
    tokio::pin!(fetch_members);
    let mut members_fetched = false;
    loop {
        tokio::select! {
            _ = &mut fetch_members, if !members_fetched => {
                members_fetched = true;
            }
            maybe_diffs = stream.next() => {
                let Some(diffs) = maybe_diffs else { break };
                if !registry.thread_current(thread_gen, lifecycle) {
                    break;
                }
                for diff in diffs {
                    let base = json!({
                        "type": "thread_diff",
                        "room_id": room_id,
                        "thread_root_id": root_event_id,
                        "thread_generation": thread_gen,
                        "lifecycle": lifecycle,
                    });
                    let value = fill_diff_json(base, &diff, &own_user, &registry);
                    enqueue(&events, value);
                }
            }
        }
    }
}

/// v0.6.0 checkpoint 5: build the room's ThreadListService, fetch the first
/// page, and forward a full bounded snapshot on every live update batch.
async fn open_thread_list_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    list_gen: u64,
    lifecycle: u64,
    events: EventQueue,
) {
    let Ok(room_ref) = RoomId::parse(&room_id) else {
        enqueue(
            &events,
            json!({
                "type": "thread_list_error",
                "room_id": room_id,
                "thread_list_generation": list_gen,
                "lifecycle": lifecycle,
                "category": "invalid_room_id",
            }),
        );
        return;
    };
    let Some(room) = client.get_room(&room_ref) else {
        enqueue(
            &events,
            json!({
                "type": "thread_list_error",
                "room_id": room_id,
                "thread_list_generation": list_gen,
                "lifecycle": lifecycle,
                "category": "unknown_room",
            }),
        );
        return;
    };

    let service = Arc::new(ThreadListService::new(room));
    {
        let Ok(mut guard) = registry.active_thread_list.lock() else { return };
        match guard.as_mut() {
            Some(list) if list.list_gen == list_gen => {
                list.service = Some(Arc::clone(&service));
            }
            _ => return, // superseded while building
        }
    }

    let (_initial, mut stream) = service.subscribe_to_items_updates();

    // First page (server /threads). A failure still emits a snapshot so the
    // UI leaves its loading state honestly.
    let first_page_failed = service.paginate().await.is_err();
    if !registry.thread_list_current(list_gen, lifecycle) {
        return;
    }
    emit_thread_list_snapshot(
        &events, &room_id, list_gen, lifecycle, &service, first_page_failed,
    );

    while let Some(_diffs) = stream.next().await {
        if !registry.thread_list_current(list_gen, lifecycle) {
            break;
        }
        emit_thread_list_snapshot(&events, &room_id, list_gen, lifecycle, &service, false);
    }
}

/// Serialize the service's current (page-bounded) items. Presentation-safe
/// fields only; previews reuse the sanitized content_preview.
fn emit_thread_list_snapshot(
    events: &EventQueue,
    room_id: &str,
    list_gen: u64,
    lifecycle: u64,
    service: &ThreadListService,
    failed: bool,
) {
    use matrix_sdk_ui::timeline::thread_list_service::ThreadListPaginationState;
    let items: Vec<serde_json::Value> =
        service.items().iter().map(thread_list_item_to_json).collect();
    let end_reached = matches!(
        service.pagination_state(),
        ThreadListPaginationState::Idle { end_reached: true }
    );
    enqueue(
        events,
        json!({
            "type": "thread_list_reset",
            "room_id": room_id,
            "thread_list_generation": list_gen,
            "lifecycle": lifecycle,
            "end_reached": end_reached,
            "failed": failed,
            "items": items,
        }),
    );
}

fn thread_list_item_to_json(item: &ThreadListItem) -> serde_json::Value {
    let mut out = json!({
        "root_event_id": item.root_event.event_id.to_string(),
        "root_sender": item.root_event.sender.to_string(),
        "root_timestamp_ms": u64::from(item.root_event.timestamp.get()),
        "root_preview": item
            .root_event
            .content
            .as_ref()
            .map(content_preview)
            .unwrap_or_else(|| "[unsupported event]".to_owned()),
        "reply_count": item.num_replies,
    });
    if let TimelineDetails::Ready(profile) = &item.root_event.sender_profile {
        if let Some(name) = &profile.display_name {
            out["root_sender_name"] = name.clone().into();
        }
    }
    if let Some(latest) = &item.latest_event {
        out["latest_sender"] = latest.sender.to_string().into();
        out["latest_timestamp_ms"] =
            u64::from(latest.timestamp.get()).into();
        out["latest_preview"] = latest
            .content
            .as_ref()
            .map(content_preview)
            .unwrap_or_else(|| "[unsupported event]".to_owned())
            .into();
        if let TimelineDetails::Ready(profile) = &latest.sender_profile {
            if let Some(name) = &profile.display_name {
                out["latest_sender_name"] = name.clone().into();
            }
        }
    }
    out
}

/// One-shot thread timeline used when the panel is not open: a
/// thread-focused SDK timeline is built just for a send (the SDK attaches
/// the m.thread relation and reply fallback, and encrypts for encrypted
/// rooms) and dropped again.
async fn build_transient_thread_timeline(
    client: &Client,
    room_id: &str,
    root_event_id: &str,
) -> Option<Arc<Timeline>> {
    let room_ref = RoomId::parse(room_id).ok()?;
    let room = client.get_room(&room_ref)?;
    let root_ref = EventId::parse(root_event_id).ok()?;
    TimelineBuilder::new(&room)
        .with_focus(TimelineFocus::Thread { root_event_id: root_ref })
        .build()
        .await
        .ok()
        .map(Arc::new)
}

fn emit_timeline_error(
    events: &EventQueue,
    room_id: &str,
    room_gen: u64,
    lifecycle: u64,
    category: &str,
) {
    enqueue(
        events,
        json!({
            "type": "timeline_error",
            "room_id": room_id,
            "room_generation": room_gen,
            "lifecycle": lifecycle,
            "category": category,
        }),
    );
}

/// Serialize one `VectorDiff` into the FFI envelope. Every pinned-SDK
/// variant is covered; there is no fallback arm that silently drops one.
fn diff_to_json(
    room_id: &str,
    room_gen: u64,
    lifecycle: u64,
    diff: &VectorDiff<Arc<TimelineItem>>,
    own_user: &str,
    registry: &TimelineRegistry,
) -> serde_json::Value {
    let mut base = json!({
        "type": "timeline_diff",
        "room_id": room_id,
        "room_generation": room_gen,
        "lifecycle": lifecycle,
    });
    // Sync-latency tracing (LIGHTNING_SYNC_TRACE): stamp the moment this diff
    // left the SDK side, so the C++ tracer can measure the sdk->bridge leg
    // instead of assuming it. Wall-clock millis, because a Rust Instant and a
    // Qt elapsed timer share no origin across the FFI. Off by default and
    // costs one atomic load per diff then.
    if let Some(stamp) = crate::sync_trace_stamp_ms() {
        base["trace_sdk_ms"] = stamp.into();
    }
    fill_diff_json(base, diff, own_user, registry)
}

/// Fill the shared `op`/`items`/`index` fields of a diff envelope. The base
/// carries the stream identity (room vs thread) so room and thread diffs
/// serialize identically everywhere else.
fn fill_diff_json(
    envelope: serde_json::Value,
    diff: &VectorDiff<Arc<TimelineItem>>,
    own_user: &str,
    registry: &TimelineRegistry,
) -> serde_json::Value {
    let base = |op: &str| {
        let mut v = envelope.clone();
        v["op"] = op.into();
        v
    };
    match diff {
        VectorDiff::Append { values } => {
            let mut v = base("append");
            v["items"] = values.iter().map(|i| item_to_json(i, own_user, registry)).collect();
            v
        }
        VectorDiff::PushBack { value } => {
            let mut v = base("push_back");
            v["item"] = item_to_json(value, own_user, registry);
            v
        }
        VectorDiff::PushFront { value } => {
            let mut v = base("push_front");
            v["item"] = item_to_json(value, own_user, registry);
            v
        }
        VectorDiff::Insert { index, value } => {
            let mut v = base("insert");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, own_user, registry);
            v
        }
        VectorDiff::Set { index, value } => {
            let mut v = base("set");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, own_user, registry);
            v
        }
        VectorDiff::Remove { index } => {
            let mut v = base("remove");
            v["index"] = (*index).into();
            v
        }
        VectorDiff::PopFront => base("pop_front"),
        VectorDiff::PopBack => base("pop_back"),
        VectorDiff::Clear => base("clear"),
        VectorDiff::Truncate { length } => {
            let mut v = base("truncate");
            v["length"] = (*length).into();
            v
        }
        VectorDiff::Reset { values } => {
            let mut v = base("reset");
            v["items"] = values.iter().map(|i| item_to_json(i, own_user, registry)).collect();
            v
        }
    }
}

/// Convert one SDK timeline item into the UI-safe FFI payload.
fn item_to_json(
    item: &TimelineItem,
    own_user: &str,
    registry: &TimelineRegistry,
) -> serde_json::Value {
    match item.kind() {
        TimelineItemKind::Virtual(virt) => {
            let (kind, ts) = match virt {
                VirtualTimelineItem::DateDivider(ts) => ("date_divider", u64::from(ts.get())),
                VirtualTimelineItem::ReadMarker => ("read_marker", 0),
                VirtualTimelineItem::TimelineStart => ("timeline_start", 0),
            };
            json!({
                "item_id": item.unique_id().0,
                "kind": kind,
                "timestamp_ms": ts,
            })
        }
        TimelineItemKind::Event(event) => {
            event_item_to_json(&item.unique_id().0, event, own_user, registry)
        }
    }
}

/// Read-receipt chips: at most this many receipt entries cross the FFI per
/// event. The last message of a busy room can carry hundreds of receipts
/// and is re-serialized on every receipt move; the UI shows 4 chips + a
/// "+N" total, so a bounded newest-first window plus the full count is all
/// the presentation ever needs.
const READ_BY_CAP: usize = 16;

/// Read-receipt chips: serialize the SDK's per-item receipt map as
/// (`[{"user_id": "...", "ts": <ms or null>}]`, total_count). ONLY the
/// public receipt metadata the server already shares crosses the FFI — a
/// stable user id and the receipt timestamp; never event content or crypto
/// state. Entries are sorted newest-first and capped at [`READ_BY_CAP`];
/// the returned total is the UNCAPPED receipt count so the "+N" overflow
/// chip stays truthful past the cap. The SDK attaches each user's receipt
/// to the LATEST timeline item it applies to, so a receipt advancing
/// arrives as two ordinary Set diffs (the old row without the entry, the
/// new row with it). Local echoes always have an empty map. Thread
/// timelines never call this with entries: their builders deliberately
/// leave receipt tracking Disabled (see the live-builder comment — the
/// SDK's receipt handling is not thread-aware, so enabling it there would
/// attach unthreaded receipts to thread rows).
fn read_by_json<'a>(
    receipts: impl IntoIterator<Item = (&'a OwnedUserId, &'a Receipt)>,
) -> (Vec<serde_json::Value>, usize) {
    let mut entries: Vec<(Option<u64>, String)> = receipts
        .into_iter()
        .map(|(user_id, receipt)| {
            (
                receipt.ts.map(|ts| u64::from(ts.get())),
                user_id.to_string(),
            )
        })
        .collect();
    let total = entries.len();
    // Newest first; receipts without a timestamp sort last (stable, so
    // ties keep the SDK's order).
    entries.sort_by(|a, b| b.0.unwrap_or(0).cmp(&a.0.unwrap_or(0)));
    entries.truncate(READ_BY_CAP);
    let serialized = entries
        .into_iter()
        .map(|(ts, user_id)| json!({ "user_id": user_id, "ts": ts }))
        .collect();
    (serialized, total)
}

/// Reaction tooltips: at most this many reactor ids cross the FFI per
/// reaction bucket. Same reasoning (and same number) as [`READ_BY_CAP`] —
/// the tooltip lists a handful of names and then says "and N more", so the
/// uncapped `count` is all the presentation needs past the window. A
/// popular reaction in a large room can carry hundreds of senders and is
/// re-serialized on every toggle.
const REACTION_SENDER_CAP: usize = 16;

/// Reactor ids longer than this are dropped rather than forwarded. Sender
/// ids come from the server and a real MXID cannot approach this, but the
/// same bounding rule the call ids follow applies: never hand the C++ side
/// an unbounded string just because the source is nominally trusted.
const REACTION_SENDER_MAX_BYTES: usize = 255;

/// Serialize the reactors of ONE reaction bucket as a bounded id list.
///
/// The local user comes first when present, so the tooltip can say "You and
/// …" without the presentation layer re-scanning the list; everything else
/// keeps the SDK's own iteration order (`IndexMap`, i.e. insertion order),
/// which makes the window deterministic for a given snapshot. Only the
/// stable user id crosses — never the reaction event id or its timestamp,
/// neither of which the tooltip needs, and the redaction target
/// (`my_event_id`) is already carried separately for the local user alone.
fn reaction_senders_json<'a>(
    senders: impl IntoIterator<Item = &'a str>,
    own_user: &str,
) -> Vec<serde_json::Value> {
    let mut own_reacted = false;
    let mut others: Vec<&str> = Vec::new();
    for id in senders {
        if id.len() > REACTION_SENDER_MAX_BYTES {
            continue;
        }
        if !own_user.is_empty() && id == own_user {
            own_reacted = true;
            continue;
        }
        others.push(id);
    }

    let mut out: Vec<serde_json::Value> = Vec::new();
    if own_reacted {
        out.push(own_user.into());
    }
    for id in others {
        if out.len() >= REACTION_SENDER_CAP {
            break;
        }
        out.push(id.into());
    }
    out
}

/// Display names in a profile-change row are bounded at this many CHARS.
/// Chars, never bytes: slicing a `String` by byte offset panics mid
/// code point, and a name is very often emoji or non-Latin.
const PROFILE_NAME_CAP: usize = 255;

fn bound_profile_name(name: &str) -> String {
    name.chars().take(PROFILE_NAME_CAP).collect()
}

/// Classify an `m.room.member` display-name change into the typed triple
/// (`kind`, bounded old, bounded new) the C++ side turns into a sentence.
///
/// Rust deliberately builds no English here: the row is presentation, and a
/// sentence assembled in the bridge cannot be translated and cannot adapt
/// to the resolved actor name the UI already knows.
///
/// Empty is the same as absent — a server that stores `""` for a cleared
/// name must not be reported as "set their display name to nothing". And an
/// old value equal to the new one is NO name change at all: the SDK hands
/// us a profile-change item when only the avatar moved, and claiming a
/// rename that did not happen is worse than saying nothing.
fn profile_name_change(
    old: Option<&str>,
    new: Option<&str>,
) -> Option<(&'static str, Option<String>, Option<String>)> {
    let old = old.filter(|name| !name.is_empty());
    let new = new.filter(|name| !name.is_empty());
    match (old, new) {
        (None, Some(new)) => Some(("set", None, Some(bound_profile_name(new)))),
        (Some(old), None) => Some(("cleared", Some(bound_profile_name(old)), None)),
        (Some(old), Some(new)) if old != new => Some((
            "changed",
            Some(bound_profile_name(old)),
            Some(bound_profile_name(new)),
        )),
        // (None, None), or old == new: nothing to report.
        _ => None,
    }
}

// The activity-row wording for an `OtherState` timeline item. Extracted from
// the match it used to sit in purely so it can be tested: everything else in
// that arm needs a real SDK timeline item to construct, while this mapping is
// the part that actually carries a decision.
//
// Nothing here interpolates event CONTENT — only the room-state type and the
// sender, both of which are already structural. In particular the tombstone
// row carries neither the successor room id (acting on it is the banner's
// job, and a row cannot be clicked) nor the tombstone's `body`, which is
// free text chosen by whoever sent the state event.
fn state_row_text(kind: &str, actor: &str) -> String {
    match kind {
        "m.room.create" => format!("{actor} created the room."),
        "m.room.name" => format!("{actor} changed the room name."),
        "m.room.topic" => format!("{actor} changed the room topic."),
        "m.room.avatar" => format!("{actor} changed the room avatar."),
        "m.room.encryption" => "Encryption was enabled.".to_owned(),
        "m.room.tombstone" => format!("{actor} upgraded this room."),
        // Calls are not a room SETTING, so they must not fall through to the
        // catch-all: "started a call" is the whole reason the row exists.
        //
        // The real call rows no longer come through here at all — CallInvite
        // and RtcNotification emit `msgtype: "call"` with typed fields and an
        // EMPTY body, and C++ phrases them with the actor's resolved display
        // name. These two arms remain only for a genuinely state-typed
        // `m.call*` event arriving through OtherState, so that one cannot
        // regress to "updated room settings."
        "m.call" => format!("{actor} started a call."),
        "m.call.video" => format!("{actor} started a video call."),
        _ => format!("{actor} updated room settings."),
    }
}

fn event_item_to_json(
    unique_id: &str,
    event: &EventTimelineItem,
    own_user: &str,
    registry: &TimelineRegistry,
) -> serde_json::Value {
    let mut out = json!({
        "item_id": unique_id,
        "kind": "event",
        "event_id": event.event_id().map(|e| e.to_string()).unwrap_or_default(),
        "transaction_id": event
            .transaction_id()
            .map(|t| t.to_string())
            .unwrap_or_default(),
        "sender": event.sender().to_string(),
        "timestamp_ms": u64::from(event.timestamp().get()),
        "is_own": event.is_own(),
        "is_local_echo": event.is_local_echo(),
        "is_encrypted": event.encryption_info().is_some(),
        "is_decrypted": event.encryption_info().is_some(),
        "undecryptable": false,
        "redacted": false,
        "edited": false,
    });

    if let TimelineDetails::Ready(profile) = event.sender_profile() {
        if let Some(name) = &profile.display_name {
            out["sender_display_name"] = name.clone().into();
        }
        // v0.5.9: SDK-computed ambiguity — two active members share this
        // display name. QML appends a compact MXID disambiguator; members
        // are never merged by display name.
        if profile.display_name_ambiguous {
            out["sender_name_ambiguous"] = true.into();
        }
        if let Some(avatar) = &profile.avatar_url {
            out["sender_avatar_url"] = avatar.to_string().into();
        }
    }

    // Per-message read receipts (empty for local echoes; absent fields keep
    // the ingest's empty-list default). The user's own receipt is forwarded
    // too — presentation-side exclusion lives in ONE place (TimelineModel),
    // so mock/HTTP rows follow the identical rule. read_by is a bounded
    // newest-first window; read_by_total carries the uncapped count.
    let receipts = event.read_receipts();
    if !receipts.is_empty() {
        let (entries, total) = read_by_json(receipts);
        out["read_by"] = entries.into();
        out["read_by_total"] = total.into();
    }

    if let Some(state) = event.send_state() {
        match state {
            EventSendState::NotSentYet { progress } => {
                out["send_state"] = "sending".into();
                // Real byte progress for a media send, straight from the
                // SDK: the send queue reports MediaUpload progress and
                // matrix-sdk-ui parks it on the local echo's send state, so
                // it arrives as an ordinary timeline diff. There is nothing
                // to subscribe to and nothing to poll.
                //
                // Only crosses when the total is KNOWN. `current`/`total`
                // are byte counts, never content, and a text send carries
                // no progress at all — which is the difference between an
                // upload bar and a spinner, and the reason the presentation
                // side must not synthesise one.
                if let Some(progress) = progress {
                    if progress.progress.total > 0 {
                        out["send_upload_current"] =
                            (progress.progress.current as u64).into();
                        out["send_upload_total"] =
                            (progress.progress.total as u64).into();
                    }
                }
            }
            EventSendState::Sent { .. } => {
                out["send_state"] = "sent".into();
            }
            EventSendState::SendingFailed { is_recoverable, .. } => {
                out["send_state"] = "failed".into();
                // Coarse, non-secret category only. Raw errors may embed
                // server or crypto detail and never cross the FFI.
                out["send_error"] =
                    if *is_recoverable { "network" } else { "rejected" }.into();
            }
        }
    }

    match event.content() {
        TimelineItemContent::MsgLike(msg_like) => {
            if !msg_like.reactions.is_empty() {
                let reactions: Vec<serde_json::Value> = msg_like
                    .reactions
                    .iter()
                    .map(|(key, senders)| {
                        let by_me = UserId::parse(own_user)
                            .map(|uid| senders.contains_key(&uid))
                            .unwrap_or(false);
                        json!({
                            "key": key,
                            // The UNCAPPED total. `senders` below is a
                            // bounded window, exactly like read receipts.
                            "count": senders.len(),
                            "by_me": by_me,
                            "senders": reaction_senders_json(
                                senders.keys().map(|user| user.as_str()),
                                own_user,
                            ),
                        })
                    })
                    .collect();
                out["reactions"] = reactions.into();
            }
            if let Some(reply) = &msg_like.in_reply_to {
                out["reply_to_event_id"] = reply.event_id.to_string().into();
                if let TimelineDetails::Ready(embedded) = &reply.event {
                    out["reply_to_sender"] = embedded.sender.to_string().into();
                    out["reply_to_preview"] = content_preview(&embedded.content).into();
                    // 2026-08-18 tester report #2: reply-to-IMAGE quotes
                    // show a thumbnail. The embedded event carries the
                    // FULL media content (encrypted sources included), so
                    // register it in the media registry under the reply
                    // target's own event id — exactly the row mechanism —
                    // and cross only the retrieval key. Images only
                    // (stickers are MsgLikeKind::Sticker and fall through
                    // untouched): a filename row already serves files,
                    // and video posters are a separate concern.
                    if let TimelineItemContent::MsgLike(embedded_kind) =
                        &embedded.content
                    {
                        if let MsgLikeKind::Message(message) =
                            &embedded_kind.kind
                        {
                            if matches!(message.msgtype(),
                                        MessageType::Image(_))
                            {
                                let mut scratch = serde_json::json!({});
                                if let Some(media) = fill_message_content(
                                    &mut scratch, message.msgtype())
                                {
                                    let reply_key =
                                        reply.event_id.to_string();
                                    out["reply_to_media_key"] =
                                        reply_key.clone().into();
                                    registry.remember_media(
                                        reply_key, media);
                                }
                            }
                        }
                    }
                }
            }
            if let Some(root) = &msg_like.thread_root {
                out["thread_root_id"] = root.to_string().into();
            }
            // v0.6.0: SDK-provided thread summary on thread ROOT events —
            // authoritative reply count and latest-reply metadata from the
            // server's bundled aggregation, kept live by sync. Only safe
            // presentation fields cross the FFI (previews reuse the same
            // sanitized content_preview as reply previews).
            if let Some(summary) = &msg_like.thread_summary {
                out["is_thread_root"] = true.into();
                out["thread_reply_count"] = summary.num_replies.into();
                if let TimelineDetails::Ready(latest) = &summary.latest_event {
                    out["thread_latest_preview"] =
                        content_preview(&latest.content).into();
                    out["thread_latest_kind"] =
                        thread_latest_kind(&latest.content).into();
                    out["thread_latest_sender"] = latest.sender.to_string().into();
                    out["thread_latest_timestamp_ms"] =
                        u64::from(latest.timestamp.get()).into();
                    // Latest-reply sender profile from the SDK's bundled
                    // aggregation — the card shows a friendly name/avatar
                    // without a separate lookup. Absent fields fall back to
                    // the MXID / existing avatar path on the C++ side.
                    if let TimelineDetails::Ready(profile) = &latest.sender_profile {
                        if let Some(name) = &profile.display_name {
                            out["thread_latest_sender_display_name"] =
                                name.clone().into();
                        }
                        if let Some(avatar) = &profile.avatar_url {
                            out["thread_latest_sender_avatar_url"] =
                                avatar.to_string().into();
                        }
                    }
                    if let TimelineEventItemId::EventId(latest_id) = &latest.identifier {
                        // Conservative unread hint: read only when the user's
                        // own threaded receipt points at the latest reply (or
                        // the latest reply is their own). Absent receipts
                        // with replies present count as unread.
                        let read = latest.sender.as_str() == own_user
                            || summary.public_read_receipt_event_id.as_deref()
                                == Some(&**latest_id)
                            || summary.private_read_receipt_event_id.as_deref()
                                == Some(&**latest_id);
                        out["thread_unread"] =
                            (!read && summary.num_replies > 0).into();
                    }
                }
            }
            match &msg_like.kind {
                MsgLikeKind::Message(message) => {
                    out["edited"] = message.is_edited().into();
                    // v0.6.0 checkpoint 11: authoritative mention metadata
                    // from the event's m.mentions — never substring matching.
                    if let Some(mentions) = message.mentions() {
                        if mentions.room {
                            out["mentions_room"] = true.into();
                        }
                        if let Ok(own) = UserId::parse(own_user) {
                            if mentions.user_ids.contains(&own) {
                                out["mentions_me"] = true.into();
                            }
                        }
                    }
                    if let Some(media) = fill_message_content(&mut out, message.msgtype()) {
                        // Stable retrieval key: the event id once the item is
                        // remote, the SDK unique id while it is a local echo.
                        let key: String = match out["event_id"].as_str() {
                            Some(event_id) if !event_id.is_empty() => event_id.to_owned(),
                            _ => unique_id.to_owned(),
                        };
                        out["media_key"] = key.clone().into();
                        out["media_source_available"] = true.into();
                        out["media_thumb_available"] =
                            media.thumbnail.is_some().into();
                        registry.remember_media(key, media);
                    }
                }
                MsgLikeKind::Redacted => {
                    out["msgtype"] = "redacted".into();
                    out["redacted"] = true.into();
                    out["body"] = "".into();
                }
                MsgLikeKind::UnableToDecrypt(encrypted) => {
                    out["msgtype"] = "encrypted".into();
                    out["is_encrypted"] = true.into();
                    out["is_decrypted"] = false.into();
                    out["undecryptable"] = true.into();
                    out["body"] = "".into();
                    out["error_kind"] = utd_category(encrypted).into();
                }
                MsgLikeKind::Sticker(sticker) => {
                    // v0.7: stickers render through the image path with a
                    // transparency-preserving presentation. Only safe
                    // metadata crosses the FFI; bytes flow through the same
                    // validated media bridge as image attachments.
                    let content = sticker.content();
                    out["msgtype"] = "sticker".into();
                    out["body"] = content.body.clone().into();
                    out["media_filename"] = content.body.clone().into();
                    let source: MediaSource = content.source.clone().into();
                    if let MediaSource::Plain(mxc) = &source {
                        out["media_mxc"] = mxc.to_string().into();
                    }
                    let info = &content.info;
                    let mut mimetype = None;
                    if let Some(mime) = &info.mimetype {
                        out["media_mimetype"] = mime.clone().into();
                        mimetype = Some(mime.clone());
                    }
                    if let Some(size) = info.size {
                        out["media_size"] = u64::from(size).into();
                    }
                    if let Some(width) = info.width {
                        out["media_width"] = u64::from(width).into();
                    }
                    if let Some(height) = info.height {
                        out["media_height"] = u64::from(height).into();
                    }
                    let media = StoredMedia {
                        source,
                        thumbnail: info.thumbnail_source.clone(),
                        filename: content.body.clone(),
                        mimetype,
                        declared_size: info.size.map(u64::from),
                    };
                    let key: String = match out["event_id"].as_str() {
                        Some(event_id) if !event_id.is_empty() => {
                            event_id.to_owned()
                        }
                        _ => unique_id.to_owned(),
                    };
                    out["media_key"] = key.clone().into();
                    out["media_source_available"] = true.into();
                    out["media_thumb_available"] =
                        media.thumbnail.is_some().into();
                    registry.remember_media(key, media);
                }
                MsgLikeKind::Poll(state) => {
                    fill_poll_content(
                        &mut out,
                        &PollView::from_results(state.results()),
                        state.fallback_text(),
                        own_user,
                    );
                }
                MsgLikeKind::Other(_) => {
                    out["msgtype"] = "unsupported".into();
                    out["body"] = "[unsupported event]".into();
                }
                MsgLikeKind::LiveLocation(_) => {
                    out["msgtype"] = "unsupported".into();
                    out["body"] = "[live location]".into();
                }
            }
        }
        TimelineItemContent::MembershipChange(_) => {
            if let TimelineItemContent::MembershipChange(change) = event.content() {
                use matrix_sdk_ui::timeline::MembershipChange as M;
                let target = change.display_name().unwrap_or_else(|| change.user_id().to_string());
                let actor = event.sender().to_string();
                let text = match change.change() {
                    Some(M::Joined) => format!("{target} joined the room."),
                    Some(M::Left) => format!("{target} left the room."),
                    Some(M::Invited) => format!("{actor} invited {target}."),
                    Some(M::InvitationAccepted) => format!("{target} accepted the invitation."),
                    Some(M::Kicked) => format!("{actor} removed {target} from the room."),
                    Some(M::Banned) | Some(M::KickedAndBanned) => format!("{actor} banned {target}."),
                    Some(M::Unbanned) => format!("{actor} unbanned {target}."),
                    Some(M::InvitationRejected) => format!("{target} rejected the invitation."),
                    Some(M::InvitationRevoked) => format!("{actor} revoked {target}'s invitation."),
                    _ => format!("Membership for {target} changed."),
                };
                out["msgtype"] = "state".into();
                out["state_kind"] = "membership".into();
                out["state_target"] = target.into();
                out["body"] = text.into();
            }
        }
        TimelineItemContent::ProfileChange(change) => {
            out["msgtype"] = "state".into();
            out["state_kind"] = "member_profile".into();
            out["state_target"] = change.user_id().to_string().into();

            // Typed fields, not a sentence. The old wording also picked the
            // WRONG one whenever both moved at once ("changed their display
            // name" swallowed the avatar), and it addressed the member by
            // raw MXID because the bridge has no resolved name to use.
            let name_change = change.displayname_change().and_then(|names| {
                profile_name_change(names.old.as_deref(), names.new.as_deref())
            });
            match name_change {
                Some((kind, old, new)) => {
                    out["profile_name_change"] = kind.into();
                    if let Some(old) = old {
                        out["profile_name_old"] = old.into();
                    }
                    if let Some(new) = new {
                        out["profile_name_new"] = new.into();
                    }
                }
                None => out["profile_name_change"] = serde_json::Value::Null,
            }
            // Only the fact, never the mxc URI: the row says an avatar
            // changed, and the picture it would point at is fetched through
            // the member cache like every other avatar.
            out["profile_avatar_changed"] =
                change.avatar_url_change().is_some().into();

            // Present but EMPTY by contract — the sentence is built in C++,
            // where the actor's resolved display name and translations live.
            out["body"] = "".into();
        }
        TimelineItemContent::OtherState(state) => {
            let kind = state.content().event_type().to_string();
            let actor = event.sender().to_string();
            // Call MEMBERSHIP is bookkeeping, not room history — every
            // join, keepalive refresh and leave writes one of these. The
            // suppression lives in TimelineModel::stateGroupEntriesFrom,
            // which is where the "N room updates" group is actually built:
            // the msgtype must stay "state" so the row keeps its place in
            // the SDK's index space, and `state_kind` is what C++ keys on.
            out["msgtype"] = "state".into();
            out["state_kind"] = kind.clone().into();
            out["body"] = state_row_text(&kind, &actor).into();
        }
        TimelineItemContent::FailedToParseMessageLike { .. }
        | TimelineItemContent::FailedToParseState { .. } => {
            out["msgtype"] = "unsupported".into();
            out["body"] = "[unsupported event]".into();
        }
        // ── Call rows are room HISTORY, not a room setting ───────────────
        //
        // Both arms used to emit `msgtype: "state"` with `state_kind
        // "m.call"`, and that is precisely what made the reported row read
        // "1 room update" expanding to the literal words "call event": a
        // non-empty `state_kind` is what TimelineModel calls ROUTINE
        // ACTIVITY, so the row was folded into the collapsed state group and
        // governed by the room-activity preference. A call someone started
        // is not a room setting somebody changed. It now has its own msgtype
        // end to end (`call` -> TimelineEvent::CallEvent ->
        // CallEventDelegate.qml), which also means it BREAKS a state-group
        // run instead of joining one.
        //
        // Everything below is TYPED and presentation-safe. No free text
        // chosen by a sender crosses: the sentence is built in C++, where
        // the actor's resolved display name and the translations live, and
        // where nothing a remote user wrote can reach a control the reader
        // is invited to click (the rule the tombstone banner established).
        TimelineItemContent::CallInvite => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "invite".into();
            // A legacy `m.call.invite` states no intent the SDK surfaces
            // here, so this row must not CLAIM video. False means "not known
            // to be video" and the sentence stays "started a call", which is
            // true of a video call as well.
            out["call_video"] = false.into();
            // EMPTY BY CONTRACT, exactly like a typed profile change: a
            // sentence built here could be neither translated nor written
            // with the actor's resolved display name (the old one addressed
            // the caller by raw MXID).
            out["body"] = "".into();
        }
        TimelineItemContent::RtcNotification { call_intent, declined_by } => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "notification".into();
            out["call_video"] = matches!(call_intent, Some(CallIntent::Video)).into();
            out["body"] = "".into();
            // A COUNT, never the ids: who declined a call is not something
            // the timeline row needs and every id crossing the FFI is one
            // more thing a row can leak.
            if !declined_by.is_empty() {
                out["call_declined_count"] = declined_by.len().into();
            }
        }
    }

    out
}

/// Text / notice / emote / media conversion.
///
/// v0.5.9: media messages return a `StoredMedia` describing how to fetch
/// the bytes through the SDK (including encrypted sources, which stay in
/// Rust). The serialized payload still carries only safe metadata — a plain
/// mxc string for unencrypted media (HTTP-backend parity), sizes, and
/// dimensions. Encrypted source material never crosses the FFI.
/// Forward the sender's `org.matrix.custom.html` formatted body verbatim. It
/// is untrusted HTML; the C++ side sanitizes it to a safe RichText subset
/// (see MessageHtml::sanitize) before it ever reaches QML. Only the HTML
/// format is forwarded — an unknown format is ignored so the plain body is
/// used instead.
fn set_formatted_body(out: &mut serde_json::Value, formatted: Option<&FormattedBody>) {
    if let Some(fb) = formatted {
        if fb.format == MessageFormat::Html {
            out["formatted_body"] = fb.body.clone().into();
        }
    }
}

fn fill_message_content(
    out: &mut serde_json::Value,
    msgtype: &MessageType,
) -> Option<StoredMedia> {
    match msgtype {
        MessageType::Text(content) => {
            out["msgtype"] = "text".into();
            out["body"] = content.body.clone().into();
            set_formatted_body(out, content.formatted.as_ref());
            None
        }
        MessageType::Notice(content) => {
            out["msgtype"] = "notice".into();
            out["body"] = content.body.clone().into();
            set_formatted_body(out, content.formatted.as_ref());
            None
        }
        MessageType::Emote(content) => {
            out["msgtype"] = "emote".into();
            out["body"] = content.body.clone().into();
            set_formatted_body(out, content.formatted.as_ref());
            None
        }
        MessageType::Image(content) => {
            out["msgtype"] = "image".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            let mut mimetype = None;
            let mut thumbnail = None;
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
                    mimetype = Some(mime.clone());
                }
                if let Some(size) = info.size {
                    out["media_size"] = u64::from(size).into();
                }
                if let Some(width) = info.width {
                    out["media_width"] = u64::from(width).into();
                }
                if let Some(height) = info.height {
                    out["media_height"] = u64::from(height).into();
                }
                thumbnail = info.thumbnail_source.clone();
            }
            Some(StoredMedia {
                source: content.source.clone(),
                thumbnail,
                filename: content.body.clone(),
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::File(content) => {
            out["msgtype"] = "file".into();
            out["body"] = content.body.clone().into();
            let filename = content
                .filename
                .clone()
                .unwrap_or_else(|| content.body.clone());
            out["media_filename"] = filename.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            let mut mimetype = None;
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
                    mimetype = Some(mime.clone());
                }
                if let Some(size) = info.size {
                    out["media_size"] = u64::from(size).into();
                }
            }
            Some(StoredMedia {
                source: content.source.clone(),
                thumbnail: None,
                filename,
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::Audio(content) => {
            // v0.7: audio gets its own semantic type so the UI can reserve
            // a compact audio row (with duration and the MSC3245 voice
            // marker) instead of a generic file card. Bytes still flow
            // through the same safe media path.
            out["msgtype"] = "audio".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            if content.voice.is_some() {
                out["media_voice"] = true.into();
            }
            // v0.7: the REAL MSC3245 waveform, when the event carried one —
            // downsampled and normalized to a bounded 0..=100 array. The UI
            // never fabricates a decorative waveform; absent metadata means
            // a plain progress track.
            if let Some(audio) = &content.audio {
                if !audio.waveform.is_empty() {
                    let raw: Vec<u64> = audio
                        .waveform
                        .iter()
                        .map(|amp| u64::from(amp.get()))
                        .collect();
                    let normalized = downsample_waveform(&raw, 1024);
                    out["media_waveform"] = normalized.into();
                }
            }
            let mut mimetype = None;
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
                    mimetype = Some(mime.clone());
                }
                if let Some(size) = info.size {
                    out["media_size"] = u64::from(size).into();
                }
                if let Some(duration) = info.duration {
                    out["media_duration_ms"] =
                        (duration.as_millis() as u64).into();
                }
            }
            Some(StoredMedia {
                source: content.source.clone(),
                thumbnail: None,
                filename: content.body.clone(),
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::Video(content) => {
            // v0.7: videos reserve their thumbnail geometry (Matrix info
            // width/height/duration) and render a type-specific placeholder
            // instead of a generic file card.
            out["msgtype"] = "video".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            let mut mimetype = None;
            let mut thumbnail = None;
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
                    mimetype = Some(mime.clone());
                }
                if let Some(size) = info.size {
                    out["media_size"] = u64::from(size).into();
                }
                if let Some(width) = info.width {
                    out["media_width"] = u64::from(width).into();
                }
                if let Some(height) = info.height {
                    out["media_height"] = u64::from(height).into();
                }
                if let Some(duration) = info.duration {
                    out["media_duration_ms"] =
                        (duration.as_millis() as u64).into();
                }
                thumbnail = info.thumbnail_source.clone();
            }
            Some(StoredMedia {
                source: content.source.clone(),
                thumbnail,
                filename: content.body.clone(),
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        other => {
            out["msgtype"] = "unsupported".into();
            out["body"] = other.body().to_owned().into();
            None
        }
    }
}

/// Downsample a voice-message waveform to at most 96 buckets of 0..=100
/// (v0.7). Pure: bucket-averages the raw MSC3245 amplitudes (0..=`max`),
/// so the payload crossing the FFI is small, normalized, and carries no
/// information beyond the coarse envelope the sender already published.
fn downsample_waveform(raw: &[u64], max: u64) -> Vec<u64> {
    const BUCKETS: usize = 96;
    if raw.is_empty() || max == 0 {
        return Vec::new();
    }
    let buckets = raw.len().min(BUCKETS);
    let mut out = Vec::with_capacity(buckets);
    for i in 0..buckets {
        let start = i * raw.len() / buckets;
        let end = (((i + 1) * raw.len()) / buckets).max(start + 1);
        let slice = &raw[start..end.min(raw.len())];
        let avg: u64 = slice.iter().sum::<u64>() / slice.len() as u64;
        out.push((avg.min(max) * 100) / max);
    }
    out
}

/// Build MSC3381 poll-start content through ruma constructors only (v0.7).
/// Pure so the shape is unit-testable. Answer ids are opaque, unique within
/// the poll (timestamp + index); ruma enforces the 1..=20 answer bound and
/// Lightning additionally requires two answers, matching the creation UI.
/// Reduce matrix.to USER links in outgoing markdown to their label for the
/// PLAIN body: the plain body is the fallback other clients render in room
/// lists and notifications, and Element's mention pills serialize their
/// display text there too — never the raw `[label](https://matrix.to/…)`
/// source. `formatted_body` keeps the full anchor; user-typed markdown and
/// room/event permalinks pass through untouched. Escaped label characters
/// ("\\]", "\\\\") unescape. Best-effort on pathological labels containing
/// an unescaped '[' (the rightmost bracket wins) — this feeds a display
/// fallback, never protocol state.
pub(crate) fn mention_plain_body(markdown: &str) -> String {
    const TARGET: &str = "](https://matrix.to/#/";
    let mut out = String::with_capacity(markdown.len());
    let mut rest = markdown;
    loop {
        let Some(mid) = rest.find(TARGET) else {
            out.push_str(rest);
            break;
        };
        let head = &rest[..mid];
        let after = &rest[mid + TARGET.len()..];
        let (Some(open), Some(close)) = (head.rfind('['), after.find(')'))
        else {
            out.push_str(&rest[..mid + TARGET.len()]);
            rest = after;
            continue;
        };
        let url_frag = &after[..close];
        let is_user = (url_frag.starts_with("%40") || url_frag.starts_with('@'))
            && !url_frag.contains(char::is_whitespace);
        let consumed = mid + TARGET.len() + close + 1;
        if !is_user {
            out.push_str(&rest[..consumed]);
            rest = &rest[consumed..];
            continue;
        }
        out.push_str(&head[..open]);
        let mut chars = head[open + 1..].chars();
        while let Some(c) = chars.next() {
            if c == '\\' {
                if let Some(n) = chars.next() {
                    out.push(n);
                }
            } else {
                out.push(c);
            }
        }
        rest = &rest[consumed..];
    }
    out
}

/// Apply the mention plain-body reduction to a just-built markdown message:
/// the SDK derived `formatted_body` from the full markdown already; only the
/// plain fallback is rewritten.
fn set_mention_plain_body(msgtype: &mut MessageType, markdown: &str) {
    let plain = mention_plain_body(markdown);
    if plain == markdown {
        return;
    }
    if let MessageType::Text(text) = msgtype {
        text.body = plain;
    }
}

/// The MSC1767 fallback body lists the question and numbered answers so
/// clients without poll support show something honest.
pub(crate) fn build_poll_start_content(
    question: &str,
    answers: &[String],
    undisclosed: bool,
    max_selections: u64,
) -> Result<NewUnstablePollStartEventContent, String> {
    let question = question.trim();
    if question.is_empty() {
        return Err("A poll needs a question.".to_owned());
    }
    let trimmed: Vec<&str> = answers
        .iter()
        .map(|text| text.trim())
        .filter(|text| !text.is_empty())
        .collect();
    if trimmed.len() < 2 {
        return Err("A poll needs at least two answers.".to_owned());
    }
    let unique_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let poll_answers: Vec<UnstablePollAnswer> = trimmed
        .iter()
        .enumerate()
        .map(|(idx, text)| {
            UnstablePollAnswer::new(format!("lp{unique_ms:x}-{idx}"), *text)
        })
        .collect();
    let poll_answers = UnstablePollAnswers::try_from(poll_answers)
        .map_err(|_| "A poll allows at most 20 answers.".to_owned())?;

    let mut block = UnstablePollStartContentBlock::new(question, poll_answers);
    block.kind = if undisclosed {
        PollKind::Undisclosed
    } else {
        PollKind::Disclosed
    };
    let clamped = max_selections.clamp(1, trimmed.len() as u64);
    if let Some(max) = matrix_sdk::ruma::UInt::new(clamped) {
        block.max_selections = max;
    }

    let mut fallback = question.to_owned();
    for (idx, text) in trimmed.iter().enumerate() {
        fallback.push_str(&format!("\n{}. {}", idx + 1, text));
    }
    Ok(NewUnstablePollStartEventContent::plain_text(fallback, block))
}

/// Crate-owned projection of the SDK's aggregated `PollResult` (v0.7).
/// Exists because `PollResultAnswer` is not re-exported by matrix-sdk-ui,
/// which would make the serializer untestable if it consumed `PollResult`
/// directly. Aggregation (latest vote per user, spoiled votes,
/// max-selection truncation, votes-after-end, redacted responses) is
/// entirely ruma/SDK-owned — this projection only carries the outcome.
pub(crate) struct PollView {
    pub question: String,
    pub disclosed: bool,
    pub max_selections: u64,
    /// `(stable answer id, answer text)` in poll-start declaration order.
    pub answers: Vec<(String, String)>,
    /// Answer id -> voter MXIDs, as aggregated by ruma.
    pub votes: HashMap<String, Vec<String>>,
    pub ended: bool,
    pub edited: bool,
}

impl PollView {
    fn from_results(results: PollResult) -> Self {
        PollView {
            question: results.question,
            disclosed: matches!(results.kind, PollKind::Disclosed),
            max_selections: results.max_selections,
            answers: results
                .answers
                .into_iter()
                .map(|answer| (answer.id, answer.text))
                .collect(),
            votes: results.votes,
            ended: results.end_time.is_some(),
            edited: results.has_been_edited,
        }
    }
}

/// MSC3381 poll presentation payload (v0.7). Privacy: for an undisclosed
/// poll that has not ended, per-answer tallies never cross the FFI (counts
/// are forced to 0); only the user's own selections and the distinct-voter
/// total are forwarded. Voter MXIDs other than the requesting user's own
/// membership in an answer never cross the FFI.
fn fill_poll_content(
    out: &mut serde_json::Value,
    view: &PollView,
    fallback_text: Option<String>,
    own_user: &str,
) {
    let show_counts = view.disclosed || view.ended;

    let mut voters: Vec<&str> = view
        .votes
        .values()
        .flatten()
        .map(String::as_str)
        .collect();
    voters.sort_unstable();
    voters.dedup();

    let answers: Vec<serde_json::Value> = view
        .answers
        .iter()
        .map(|(id, text)| {
            let votes = view.votes.get(id);
            let count = votes.map(Vec::len).unwrap_or(0);
            let by_me = votes
                .map(|v| v.iter().any(|voter| voter == own_user))
                .unwrap_or(false);
            json!({
                "id": id,
                "text": text,
                "count": if show_counts { count } else { 0 },
                "by_me": by_me,
            })
        })
        .collect();

    out["msgtype"] = "poll".into();
    out["body"] = fallback_text
        .unwrap_or_else(|| view.question.clone())
        .into();
    out["poll_question"] = view.question.clone().into();
    out["poll_kind"] =
        if view.disclosed { "disclosed" } else { "undisclosed" }.into();
    out["poll_max_selections"] = view.max_selections.into();
    out["poll_answers"] = answers.into();
    out["poll_total_voters"] = (voters.len() as u64).into();
    out["poll_ended"] = view.ended.into();
    if view.edited {
        out["edited"] = true.into();
    }
}

/// Best-effort short preview for reply boxes. Never includes ciphertext.
/// Coarse semantic kind of a thread's latest reply, so the summary card can
/// render a safe label ("Image", "GIF", "Encrypted reply", …) instead of a
/// raw body or a placeholder token. Derived from the SDK content type only —
/// never from body text. Ciphertext and media URLs never leave the SDK.
fn thread_latest_kind(content: &TimelineItemContent) -> &'static str {
    match content {
        TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
            MsgLikeKind::Message(message) => match message.msgtype() {
                MessageType::Text(_) => "text",
                MessageType::Notice(_) => "notice",
                MessageType::Emote(_) => "emote",
                MessageType::Image(content) => {
                    let gif = content
                        .info
                        .as_ref()
                        .and_then(|info| info.mimetype.as_deref())
                        == Some("image/gif");
                    if gif { "gif" } else { "image" }
                }
                MessageType::Video(_) => "video",
                MessageType::Audio(_) => "audio",
                MessageType::File(_) => "file",
                _ => "text",
            },
            MsgLikeKind::Redacted => "redacted",
            MsgLikeKind::UnableToDecrypt(_) => "encrypted",
            MsgLikeKind::Sticker(_) => "sticker",
            MsgLikeKind::Poll(_) => "poll",
            _ => "unsupported",
        },
        _ => "unsupported",
    }
}

fn content_preview(content: &TimelineItemContent) -> String {
    const PREVIEW_MAX: usize = 80;
    let text = match content {
        TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
            MsgLikeKind::Message(message) => message.body().to_owned(),
            MsgLikeKind::Redacted => "[message deleted]".to_owned(),
            MsgLikeKind::UnableToDecrypt(_) => "[unable to decrypt]".to_owned(),
            MsgLikeKind::Sticker(_) => "[sticker]".to_owned(),
            MsgLikeKind::Poll(state) => {
                let question = state.results().question;
                if question.is_empty() {
                    "[poll]".to_owned()
                } else {
                    format!("Poll: {question}")
                }
            }
            MsgLikeKind::Other(_) | MsgLikeKind::LiveLocation(_) => {
                "[unsupported event]".to_owned()
            }
        },
        _ => "[event]".to_owned(),
    };
    text.chars().take(PREVIEW_MAX).collect()
}

/// Safe, coarse UTD reason categories. No crypto internals are exposed.
/// Megolm session IDS (identifiers only — no key material) of the timeline's
/// currently visible unable-to-decrypt items.
async fn utd_session_ids(timeline: &Timeline) -> Vec<String> {
    let mut session_ids = Vec::new();
    for item in timeline.items().await.iter() {
        let TimelineItemKind::Event(event) = item.kind() else { continue };
        let TimelineItemContent::MsgLike(msg_like) = event.content() else {
            continue;
        };
        let MsgLikeKind::UnableToDecrypt(encrypted) = &msg_like.kind else {
            continue;
        };
        if let EncryptedMessage::MegolmV1AesSha2 { session_id, .. } = encrypted {
            if !session_ids.contains(session_id) {
                session_ids.push(session_id.clone());
            }
        }
    }
    session_ids
}

fn utd_category(encrypted: &EncryptedMessage) -> &'static str {
    use matrix_sdk_base::crypto::types::events::UtdCause;
    match encrypted {
        EncryptedMessage::MegolmV1AesSha2 { cause, .. } => match cause {
            UtdCause::SentBeforeWeJoined => "membership",
            UtdCause::VerificationViolation
            | UtdCause::UnsignedDevice
            | UtdCause::UnknownDevice => "device_trust",
            UtdCause::WithheldForUnverifiedOrInsecureDevice | UtdCause::WithheldBySender => {
                "withheld"
            }
            _ => "no_key",
        },
        EncryptedMessage::OlmV1Curve25519AesSha2 { .. } | EncryptedMessage::Unknown => "no_key",
    }
}

/// Map a `RoomKeyImportResult`-shaped key set into `(room_id, session_ids)`
/// pairs for `Timeline::retry_decryption`. Only session *identifiers* are
/// retained; sender keys are deliberately flattened away and key material
/// never reaches this function.
pub fn sessions_by_room_from_import(
    keys: &std::collections::BTreeMap<
        matrix_sdk::ruma::OwnedRoomId,
        std::collections::BTreeMap<String, std::collections::BTreeSet<String>>,
    >,
) -> Vec<(String, Vec<String>)> {
    keys.iter()
        .map(|(room_id, by_sender)| {
            let mut sessions: Vec<String> =
                by_sender.values().flatten().cloned().collect();
            sessions.sort();
            sessions.dedup();
            (room_id.to_string(), sessions)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{sessions_by_room_from_import, state_row_text, TimelineRegistry};
    use std::collections::{BTreeMap, BTreeSet, VecDeque};
    use std::sync::{Arc, Mutex};

    // 2026-08-18 tester report: rapid reaction clicks used to spawn one
    // independent SDK toggle per click for the SAME target, and those raced
    // each other into an add/remove storm. Exactly one toggle per
    // (room, event, key) may be in flight; the rest are dropped, and the
    // slot is reusable as soon as the first one finishes.
    #[test]
    fn only_one_reaction_toggle_per_target_is_in_flight() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let target = "!room\u{1f}$event\u{1f}\u{1f44d}";
        assert!(registry.begin_reaction(target), "the first click must run");
        assert!(
            !registry.begin_reaction(target),
            "a second click while the first is in flight must be dropped"
        );
        // A different key on the same message is a different target.
        let other = "!room\u{1f}$event\u{1f}\u{2764}";
        assert!(registry.begin_reaction(other));

        registry.end_reaction(target);
        assert!(
            registry.begin_reaction(target),
            "the slot must be reusable once the toggle has completed"
        );
    }

    // A session change must not strand a claimed slot: the toggle it
    // belonged to can never complete into the next session, and leaving the
    // key behind would make that reaction permanently unclickable.
    #[test]
    fn shutdown_releases_in_flight_reaction_slots() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let target = "!room\u{1f}$event\u{1f}\u{1f44d}";
        assert!(registry.begin_reaction(target));
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("runtime");
        registry.shutdown(&runtime);
        assert!(
            registry.begin_reaction(target),
            "shutdown must clear the in-flight set"
        );
    }

    // v0.7.x room upgrades: a tombstone used to fall into the catch-all and
    // render as the generic "updated room settings", which told the reader
    // nothing about the one state change that ends a room.
    #[test]
    fn tombstone_state_row_names_the_upgrade() {
        assert_eq!(
            state_row_text("m.room.tombstone", "@alice:example.org"),
            "@alice:example.org upgraded this room."
        );
    }

    // The row must not leak the tombstone's body or the successor id. It is
    // built from the state TYPE and the sender only, so there is no path by
    // which event content could reach it — this pins that.
    #[test]
    fn state_row_text_uses_only_kind_and_actor() {
        let row = state_row_text("m.room.tombstone", "@alice:example.org");
        assert!(!row.contains('!'), "a room id must never appear in the row: {row}");

        // An unknown state type still falls back rather than echoing the
        // type string at the reader.
        let unknown = state_row_text("org.example.custom", "@bob:example.org");
        assert_eq!(unknown, "@bob:example.org updated room settings.");
        assert!(!unknown.contains("org.example.custom"));
    }

    // The extraction must not have changed any existing wording.
    #[test]
    fn existing_state_rows_are_unchanged() {
        assert_eq!(state_row_text("m.room.create", "@a:b.c"), "@a:b.c created the room.");
        assert_eq!(state_row_text("m.room.name", "@a:b.c"), "@a:b.c changed the room name.");
        assert_eq!(state_row_text("m.room.topic", "@a:b.c"), "@a:b.c changed the room topic.");
        assert_eq!(state_row_text("m.room.avatar", "@a:b.c"), "@a:b.c changed the room avatar.");
        // Deliberately actor-free: the sender of an encryption event is not
        // the interesting fact, the room becoming encrypted is.
        assert_eq!(state_row_text("m.room.encryption", "@a:b.c"), "Encryption was enabled.");
    }

    #[test]
    fn import_result_maps_to_room_session_pairs() {
        let mut keys = BTreeMap::new();
        let mut senders = BTreeMap::new();
        senders.insert(
            "sender_key_a".to_owned(),
            BTreeSet::from(["session1".to_owned(), "session2".to_owned()]),
        );
        senders.insert(
            "sender_key_b".to_owned(),
            BTreeSet::from(["session2".to_owned(), "session3".to_owned()]),
        );
        keys.insert(
            matrix_sdk::ruma::RoomId::parse("!room:example.org").unwrap().to_owned(),
            senders,
        );

        let mapped = sessions_by_room_from_import(&keys);
        assert_eq!(mapped.len(), 1);
        assert_eq!(mapped[0].0, "!room:example.org");
        // Sessions across sender keys are merged and deduplicated; sender
        // keys themselves are dropped.
        assert_eq!(mapped[0].1, vec!["session1", "session2", "session3"]);
    }

    #[test]
    fn empty_import_result_maps_to_empty() {
        let keys = BTreeMap::new();
        assert!(sessions_by_room_from_import(&keys).is_empty());
    }

    // Read-receipt serialization: only user id + timestamp cross the FFI,
    // and a receipt without a timestamp serializes ts as null (the C++
    // ingest maps that to 0 = "no timestamp").
    #[test]
    fn read_by_serializes_user_id_and_timestamp_only() {
        use matrix_sdk::ruma::{
            events::receipt::Receipt, MilliSecondsSinceUnixEpoch, OwnedUserId,
            UInt, UserId,
        };

        let alice: OwnedUserId =
            UserId::parse("@alice:example.org").unwrap().to_owned();
        let bob: OwnedUserId =
            UserId::parse("@bob:example.org").unwrap().to_owned();
        let with_ts = Receipt::new(MilliSecondsSinceUnixEpoch(
            UInt::try_from(1_700_000_123_000u64).unwrap(),
        ));
        let without_ts = Receipt::default();

        let (entries, total) =
            super::read_by_json([(&alice, &with_ts), (&bob, &without_ts)]);
        assert_eq!(entries.len(), 2);
        assert_eq!(total, 2);
        // Newest first; the timestampless receipt sorts last.
        assert_eq!(entries[0]["user_id"], "@alice:example.org");
        assert_eq!(entries[0]["ts"], 1_700_000_123_000u64);
        assert_eq!(entries[1]["user_id"], "@bob:example.org");
        assert!(entries[1]["ts"].is_null());
        // Exactly the two documented fields — nothing else crosses the FFI.
        for entry in &entries {
            assert_eq!(entry.as_object().unwrap().len(), 2);
        }

        let (empty, empty_total) = super::read_by_json(
            std::iter::empty::<(&OwnedUserId, &Receipt)>(),
        );
        assert!(empty.is_empty());
        assert_eq!(empty_total, 0);
    }

    // The FFI window is bounded: a busy room's last message can carry
    // hundreds of receipts, but only the newest READ_BY_CAP entries cross,
    // while the total keeps the uncapped count for a truthful "+N".
    #[test]
    fn read_by_caps_entries_newest_first_and_reports_total() {
        use matrix_sdk::ruma::{
            events::receipt::Receipt, MilliSecondsSinceUnixEpoch, OwnedUserId,
            UInt, UserId,
        };

        let users: Vec<OwnedUserId> = (0..20)
            .map(|i| {
                UserId::parse(format!("@u{i}:example.org"))
                    .unwrap()
                    .to_owned()
            })
            .collect();
        // Ascending timestamps: @u19 is the newest reader.
        let receipts: Vec<Receipt> = (0..20)
            .map(|i| {
                Receipt::new(MilliSecondsSinceUnixEpoch(
                    UInt::try_from(1_700_000_000_000u64 + i * 1000).unwrap(),
                ))
            })
            .collect();

        let (entries, total) =
            super::read_by_json(users.iter().zip(receipts.iter()));
        assert_eq!(total, 20);
        assert_eq!(entries.len(), super::READ_BY_CAP);
        // Newest first: @u19 leads, and the 4 oldest (@u0..@u3) fell
        // outside the window.
        assert_eq!(entries[0]["user_id"], "@u19:example.org");
        assert_eq!(
            entries[super::READ_BY_CAP - 1]["user_id"],
            "@u4:example.org"
        );
    }

    // Reaction tooltips: the local user leads the window so the sentence
    // can start with "You", the rest keeps the SDK's insertion order, and
    // the window is bounded exactly like the receipt window is. `count`
    // stays uncapped at the call site — this helper only builds the list.
    #[test]
    fn reaction_senders_put_the_local_user_first_and_cap_the_rest() {
        let ids: Vec<String> = (0..20).map(|i| format!("@u{i}:example.org")).collect();
        let refs: Vec<&str> = ids.iter().map(String::as_str).collect();

        let mine = super::reaction_senders_json(
            refs.iter().copied(),
            "@u5:example.org",
        );
        assert_eq!(mine.len(), super::REACTION_SENDER_CAP);
        assert_eq!(mine[0], "@u5:example.org");
        // Then the SDK's order with our own id removed, not re-sorted.
        assert_eq!(mine[1], "@u0:example.org");
        assert_eq!(mine[6], "@u6:example.org");
        assert_eq!(
            mine[super::REACTION_SENDER_CAP - 1],
            "@u15:example.org"
        );

        // A reaction we did not send keeps the plain order.
        let theirs = super::reaction_senders_json(
            refs.iter().copied(),
            "@someone-else:example.org",
        );
        assert_eq!(theirs.len(), super::REACTION_SENDER_CAP);
        assert_eq!(theirs[0], "@u0:example.org");
        assert_eq!(
            theirs[super::REACTION_SENDER_CAP - 1],
            "@u15:example.org"
        );

        // Under the cap nothing is dropped, and an empty bucket stays empty.
        let few = super::reaction_senders_json(
            ["@a:example.org", "@b:example.org"],
            "@b:example.org",
        );
        assert_eq!(few.len(), 2);
        assert_eq!(few[0], "@b:example.org");
        assert_eq!(few[1], "@a:example.org");
        assert!(
            super::reaction_senders_json(std::iter::empty::<&str>(), "@a:b.c")
                .is_empty()
        );
    }

    // Ids come from the server, so they are bounded anyway — an absurd one
    // is dropped rather than forwarded into a QStringList.
    #[test]
    fn reaction_senders_drop_an_unbounded_id() {
        let huge = format!("@{}:example.org", "x".repeat(4096));
        let out = super::reaction_senders_json(
            [huge.as_str(), "@ok:example.org"],
            "@me:example.org",
        );
        assert_eq!(out.len(), 1);
        assert_eq!(out[0], "@ok:example.org");
    }

    // Typed profile changes: the classification the C++ sentence is built
    // from. The old==new case is the one that matters most — the SDK emits
    // a profile-change item when only the AVATAR moved, and claiming a
    // rename that did not happen is worse than saying nothing.
    #[test]
    fn profile_name_change_classifies_set_changed_and_cleared() {
        assert_eq!(
            super::profile_name_change(None, Some("Alice")),
            Some(("set", None, Some("Alice".to_owned())))
        );
        // A server that stores the cleared name as "" must not read as a set.
        assert_eq!(
            super::profile_name_change(Some(""), Some("Alice")),
            Some(("set", None, Some("Alice".to_owned())))
        );
        assert_eq!(
            super::profile_name_change(Some("Alice"), Some("Alice A.")),
            Some((
                "changed",
                Some("Alice".to_owned()),
                Some("Alice A.".to_owned())
            ))
        );
        assert_eq!(
            super::profile_name_change(Some("Alice"), None),
            Some(("cleared", Some("Alice".to_owned()), None))
        );
        assert_eq!(
            super::profile_name_change(Some("Alice"), Some("")),
            Some(("cleared", Some("Alice".to_owned()), None))
        );
    }

    #[test]
    fn profile_name_change_reports_nothing_when_the_name_did_not_move() {
        // Avatar-only change: same name on both sides.
        assert_eq!(super::profile_name_change(Some("Alice"), Some("Alice")), None);
        assert_eq!(super::profile_name_change(None, None), None);
        assert_eq!(super::profile_name_change(Some(""), None), None);
        assert_eq!(super::profile_name_change(None, Some("")), None);
    }

    // The bound is CHARS. A byte slice would panic mid code point on a name
    // made of emoji, which is exactly the kind of name people set.
    #[test]
    fn profile_names_are_bounded_by_chars_never_bytes() {
        let long: String = "🌩".repeat(300);
        let (kind, _, new) = super::profile_name_change(None, Some(&long))
            .expect("a long name is still a set");
        assert_eq!(kind, "set");
        let new = new.expect("set carries the new name");
        assert_eq!(new.chars().count(), super::PROFILE_NAME_CAP);
        // Four bytes per storm cloud: the byte length proves nothing was
        // sliced at a code-point boundary that does not exist.
        assert_eq!(new.len(), super::PROFILE_NAME_CAP * 4);
        assert!(new.chars().all(|c| c == '🌩'));

        // A name at exactly the cap is untouched.
        let exact: String = "a".repeat(super::PROFILE_NAME_CAP);
        assert_eq!(
            super::profile_name_change(None, Some(&exact)),
            Some(("set", None, Some(exact)))
        );
    }

    // The interactive send paths construct message content through the
    // SDK's markdown constructors. These prove the pinned SDK converts the
    // toolbar's syntax into a formatted body and leaves ordinary text as a
    // plain m.text event (no formatted_body).
    #[test]
    fn markdown_body_produces_formatted_content() {
        use matrix_sdk::ruma::events::room::message::{
            MessageType, RoomMessageEventContent,
        };
        let content = RoomMessageEventContent::text_markdown("**bold** _it_");
        match content.msgtype {
            MessageType::Text(text) => {
                let formatted = text.formatted.expect("formatted body");
                assert!(formatted.body.contains("<strong>bold</strong>"));
                assert!(formatted.body.contains("<em>it</em>"));
                assert_eq!(text.body, "**bold** _it_");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn plain_body_stays_plain() {
        use matrix_sdk::ruma::events::room::message::{
            MessageType, RoomMessageEventContent,
        };
        let content = RoomMessageEventContent::text_markdown("hello world");
        match content.msgtype {
            MessageType::Text(text) => {
                assert!(text.formatted.is_none());
                assert_eq!(text.body, "hello world");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    // v0.7 outgoing @-mentions: content built with add_mentions serializes an
    // "m.mentions".user_ids array, invalid MXIDs are dropped, and an all-empty
    // list produces no mentions at all.
    #[test]
    fn mentions_serialize_user_ids_and_drop_invalid() {
        use matrix_sdk::ruma::events::room::message::RoomMessageEventContent;

        let mentions = super::mentions_from_ids(vec![
            "@alice:example.org".to_owned(),
            "not a user id".to_owned(),
            "@bob:example.org".to_owned(),
        ])
        .expect("valid ids should produce mentions");
        let content =
            RoomMessageEventContent::text_markdown("hi").add_mentions(mentions);
        let value = serde_json::to_value(&content).expect("serialize");
        let user_ids = value["m.mentions"]["user_ids"]
            .as_array()
            .expect("user_ids array");
        let ids: Vec<&str> =
            user_ids.iter().map(|v| v.as_str().unwrap()).collect();
        assert_eq!(ids.len(), 2, "the invalid MXID must be dropped");
        assert!(ids.contains(&"@alice:example.org"));
        assert!(ids.contains(&"@bob:example.org"));

        // No valid ids → no Mentions → no m.mentions in the payload.
        assert!(super::mentions_from_ids(vec!["nope".to_owned()]).is_none());
        let plain = RoomMessageEventContent::text_markdown("hi");
        let plain_value = serde_json::to_value(&plain).expect("serialize");
        assert!(plain_value.get("m.mentions").is_none());
    }

    // Outgoing mention sends: the PLAIN body carries the display text (the
    // fallback other clients show in room lists/notifications), while
    // formatted_body keeps the full matrix.to anchor.
    #[test]
    fn mention_plain_body_reduces_user_links_only() {
        use super::mention_plain_body;
        assert_eq!(
            mention_plain_body(
                "[@test](https://matrix.to/#/%40test%3Amatrix.example.org) hi"
            ),
            "@test hi"
        );
        assert_eq!(
            mention_plain_body(
                "[@a](https://matrix.to/#/%40a%3Ax) and [@b](https://matrix.to/#/@b:x)"
            ),
            "@a and @b"
        );
        // Room/event permalinks and ordinary markdown are untouched.
        let room = "[room](https://matrix.to/#/%23room%3Ax) **bold**";
        assert_eq!(mention_plain_body(room), room);
        let site = "[site](https://example.org/page)";
        assert_eq!(mention_plain_body(site), site);
        // Escaped label characters unescape.
        assert_eq!(
            mention_plain_body("[@A\\]B](https://matrix.to/#/%40a%3Ax)"),
            "@A]B"
        );
        assert_eq!(mention_plain_body("no links"), "no links");
    }

    #[test]
    fn outgoing_mention_body_is_display_text_with_formatted_anchor() {
        use matrix_sdk::ruma::events::room::message::{
            MessageType, RoomMessageEventContent,
        };
        let markdown =
            "[@test](https://matrix.to/#/%40test%3Amatrix.example.org) hello";
        let mut message = RoomMessageEventContent::text_markdown(markdown);
        super::set_mention_plain_body(&mut message.msgtype, markdown);
        match message.msgtype {
            MessageType::Text(text) => {
                assert_eq!(text.body, "@test hello");
                let formatted = text.formatted.expect("formatted body");
                assert!(formatted.body.contains("https://matrix.to/#/"));
                assert!(formatted.body.contains("@test"));
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    // ---- v0.7 polls -----------------------------------------------------

    fn poll_view(disclosed: bool, ended: bool) -> super::PollView {
        let mut votes = std::collections::HashMap::new();
        votes.insert(
            "a1".to_owned(),
            vec!["@alice:example.org".to_owned(), "@me:example.org".to_owned()],
        );
        votes.insert("a2".to_owned(), vec!["@bob:example.org".to_owned()]);
        super::PollView {
            question: "Favourite colour?".to_owned(),
            disclosed,
            max_selections: 1,
            answers: vec![
                ("a1".to_owned(), "Blue".to_owned()),
                ("a2".to_owned(), "Green".to_owned()),
                ("a3".to_owned(), "Red".to_owned()),
            ],
            votes,
            ended,
            edited: false,
        }
    }

    fn filled(view: &super::PollView) -> serde_json::Value {
        let mut out = serde_json::json!({});
        super::fill_poll_content(&mut out, view, None, "@me:example.org");
        out
    }

    #[test]
    fn disclosed_poll_payload_exposes_counts_and_own_vote() {
        let out = filled(&poll_view(true, false));
        assert_eq!(out["msgtype"], "poll");
        assert_eq!(out["poll_question"], "Favourite colour?");
        assert_eq!(out["poll_kind"], "disclosed");
        assert_eq!(out["poll_ended"], false);
        assert_eq!(out["poll_total_voters"], 3);
        let answers = out["poll_answers"].as_array().unwrap();
        assert_eq!(answers.len(), 3);
        // Declaration order preserved; counts and own-vote flags accurate.
        assert_eq!(answers[0]["id"], "a1");
        assert_eq!(answers[0]["count"], 2);
        assert_eq!(answers[0]["by_me"], true);
        assert_eq!(answers[1]["count"], 1);
        assert_eq!(answers[1]["by_me"], false);
        assert_eq!(answers[2]["count"], 0);
        // Voter MXIDs never cross the FFI.
        assert!(!out.to_string().contains("@alice:example.org"));
    }

    #[test]
    fn undisclosed_running_poll_hides_counts_but_keeps_own_vote() {
        let out = filled(&poll_view(false, false));
        assert_eq!(out["poll_kind"], "undisclosed");
        let answers = out["poll_answers"].as_array().unwrap();
        // Hidden tallies never cross the FFI while the poll is running…
        assert!(answers.iter().all(|a| a["count"] == 0));
        // …but the user's own selection still renders.
        assert_eq!(answers[0]["by_me"], true);
        assert_eq!(out["poll_total_voters"], 3);
    }

    #[test]
    fn undisclosed_ended_poll_discloses_counts() {
        let out = filled(&poll_view(false, true));
        assert_eq!(out["poll_ended"], true);
        let answers = out["poll_answers"].as_array().unwrap();
        assert_eq!(answers[0]["count"], 2);
        assert_eq!(answers[1]["count"], 1);
    }

    #[test]
    fn poll_body_prefers_fallback_text() {
        let view = poll_view(true, false);
        let mut out = serde_json::json!({});
        super::fill_poll_content(
            &mut out, &view, Some("fallback".to_owned()), "@me:example.org",
        );
        assert_eq!(out["body"], "fallback");
        // Without a fallback the question doubles as the body.
        assert_eq!(filled(&view)["body"], "Favourite colour?");
    }

    #[test]
    fn poll_start_content_builds_msc3381_shape() {
        let content = super::build_poll_start_content(
            "  Question?  ",
            &["One".to_owned(), " Two ".to_owned(), "".to_owned()],
            false,
            2,
        )
        .expect("valid poll");
        assert_eq!(content.poll_start.question.text, "Question?");
        let answers: Vec<_> = content.poll_start.answers.iter().collect();
        assert_eq!(answers.len(), 2);
        assert_eq!(answers[0].text, "One");
        assert_eq!(answers[1].text, "Two");
        // Ids are unique within the poll.
        assert_ne!(answers[0].id, answers[1].id);
        assert!(matches!(
            content.poll_start.kind,
            matrix_sdk::ruma::events::poll::start::PollKind::Disclosed
        ));
        assert_eq!(u64::from(content.poll_start.max_selections), 2);
        // MSC1767 fallback lists the question and numbered answers.
        let fallback = content.text.expect("fallback text");
        assert!(fallback.contains("Question?"));
        assert!(fallback.contains("1. One"));
        assert!(fallback.contains("2. Two"));
    }

    #[test]
    fn poll_start_content_rejects_bad_input() {
        assert!(super::build_poll_start_content(
            "", &["a".to_owned(), "b".to_owned()], false, 1
        )
        .is_err());
        assert!(super::build_poll_start_content(
            "Q", &["only one".to_owned()], false, 1
        )
        .is_err());
        let too_many: Vec<String> =
            (0..21).map(|i| format!("answer {i}")).collect();
        assert!(super::build_poll_start_content("Q", &too_many, false, 1).is_err());
        // max_selections clamps into the valid range instead of failing.
        let clamped = super::build_poll_start_content(
            "Q", &["a".to_owned(), "b".to_owned()], true, 99,
        )
        .expect("valid poll");
        assert_eq!(u64::from(clamped.poll_start.max_selections), 2);
        assert!(matches!(
            clamped.poll_start.kind,
            matrix_sdk::ruma::events::poll::start::PollKind::Undisclosed
        ));
    }

    #[test]
    fn waveform_downsamples_and_normalizes() {
        // Fewer samples than buckets: kept 1:1, normalized to 0..=100.
        let out = super::downsample_waveform(&[0, 512, 1024], 1024);
        assert_eq!(out, vec![0, 50, 100]);
        // Long input caps at 96 buckets, all within range.
        let long: Vec<u64> = (0..1000).map(|i| i % 1025).collect();
        let out = super::downsample_waveform(&long, 1024);
        assert_eq!(out.len(), 96);
        assert!(out.iter().all(|v| *v <= 100));
        // Degenerate inputs are safe.
        assert!(super::downsample_waveform(&[], 1024).is_empty());
        assert!(super::downsample_waveform(&[5], 0).is_empty());
        // Out-of-spec amplitudes clamp instead of overflowing the scale.
        assert_eq!(super::downsample_waveform(&[9999], 1024), vec![100]);
    }

    #[test]
    fn poll_response_content_uses_reference_relation() {
        use matrix_sdk::ruma::events::poll::unstable_response::UnstablePollResponseEventContent;
        let event_id =
            matrix_sdk::ruma::EventId::parse("$poll:example.org").unwrap();
        let content = UnstablePollResponseEventContent::new(
            vec!["a1".to_owned()],
            event_id.to_owned(),
        );
        let json = serde_json::to_value(&content).unwrap();
        assert_eq!(json["m.relates_to"]["rel_type"], "m.reference");
        assert_eq!(json["m.relates_to"]["event_id"], "$poll:example.org");
        assert_eq!(
            json["org.matrix.msc3381.poll.response"]["answers"][0], "a1"
        );
        // Retraction: the empty answer list is legal and serializes.
        let retract = UnstablePollResponseEventContent::new(
            Vec::new(), event_id.to_owned(),
        );
        let json = serde_json::to_value(&retract).unwrap();
        assert_eq!(
            json["org.matrix.msc3381.poll.response"]["answers"]
                .as_array()
                .unwrap()
                .len(),
            0
        );
    }
}
