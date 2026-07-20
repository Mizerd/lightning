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
    attachment::AttachmentInfo,
    room::edit::EditedContent,
    ruma::{
        api::client::receipt::create_receipt::v3::ReceiptType,
        events::{
            room::{
                message::{
                    FormattedBody, MessageFormat, MessageType,
                    RoomMessageEventContent,
                    RoomMessageEventContentWithoutRelation, TextMessageEventContent,
                },
                MediaSource,
            },
            AnyMessageLikeEventContent,
        },
        EventId, RoomId, UserId,
    },
    Client,
};
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    timeline::{
        thread_list_service::{ThreadListItem, ThreadListService},
        AttachmentConfig, AttachmentSource, EncryptedMessage, EventSendState,
        EventTimelineItem, MsgLikeKind, Timeline, TimelineBuilder, TimelineDetails,
        TimelineEventItemId, TimelineFocus, TimelineItem, TimelineItemContent,
        TimelineItemKind, VirtualTimelineItem,
    },
};
use serde_json::json;

use crate::enqueue;

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
}

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
        }
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
    ) -> Option<(MediaSource, String, Option<String>)> {
        let guard = self.media_sources.lock().ok()?;
        let media = guard.get(key)?;
        let source = if thumbnail {
            media.thumbnail.clone().unwrap_or_else(|| media.source.clone())
        } else {
            media.source.clone()
        };
        Some((source, media.filename.clone(), media.mimetype.clone()))
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
        // A thread panel / Threads view never survives into another room.
        self.close_thread();
        self.close_thread_list();
        let room_gen = self.room_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        self.clear_media();

        if let Some((_task, old_room)) = self.take_active() {
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
    ) -> Result<(), String> {
        let reply_to = match &in_reply_to {
            Some(id) if !id.trim().is_empty() => Some(
                EventId::parse(id)
                    .map_err(|_| "Invalid reply target event id.".to_owned())?,
            ),
            _ => None,
        };
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
                async move {
                    match reply_to {
                        Some(reply_to) => timeline
                            .send_reply(
                                RoomMessageEventContentWithoutRelation::text_markdown(
                                    body,
                                ),
                                reply_to,
                            )
                            .await
                            .is_ok(),
                        None => timeline
                            .send(AnyMessageLikeEventContent::RoomMessage(
                                RoomMessageEventContent::text_markdown(body),
                            ))
                            .await
                            .is_ok(),
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

    /// Deterministic stop of all timeline work: advances the lifecycle
    /// generation, aborts the subscription task and awaits it (bounded).
    /// Called before sign-out store cleanup and before handle destruction.
    pub fn shutdown(&self, runtime: &tokio::runtime::Runtime) {
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
        self.close_thread_list();
        self.clear_media();
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
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let content = AnyMessageLikeEventContent::RoomMessage(
                RoomMessageEventContent::text_markdown(body),
            );
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
    pub fn send_attachment(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        source: AttachmentSource,
        mime_str: String,
        caption: Option<String>,
        info: Option<AttachmentInfo>,
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
                thumbnail: None,
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
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let reply_to = EventId::parse(&in_reply_to)
            .map_err(|_| "Invalid reply target event id.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let content = RoomMessageEventContentWithoutRelation::text_markdown(body);
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
                    thumbnail: None,
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
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid edit target event id.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            let content = EditedContent::RoomMessage(
                RoomMessageEventContentWithoutRelation::text_markdown(new_body),
            );
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
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // Failures surface as the reaction simply not appearing; the SDK
            // logs details. Nothing sensitive to forward.
            let _ = timeline.toggle_reaction(&item_id, &key).await;
        });
        Ok(())
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
    /// key transfer. Key DOWNLOAD attempts (backup) are the SDK's own
    /// AfterDecryptionFailure behavior; this only re-runs decryption against
    /// whatever key material has arrived since.
    pub fn retry_visible_decryption(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
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
async fn open_room_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    room_gen: u64,
    lifecycle: u64,
    events: EventQueue,
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

    // TimelineBuilder::new (not RoomExt::timeline_builder) — read-receipt
    // tracking stays off; Lightning does not consume receipt metadata yet.
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
        }),
    );

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
    let base = json!({
        "type": "timeline_diff",
        "room_id": room_id,
        "room_generation": room_gen,
        "lifecycle": lifecycle,
    });
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

    if let Some(state) = event.send_state() {
        match state {
            EventSendState::NotSentYet { .. } => {
                out["send_state"] = "sending".into();
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
                            "count": senders.len(),
                            "by_me": by_me,
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
                MsgLikeKind::Poll(_) => {
                    out["msgtype"] = "unsupported".into();
                    out["body"] = "[poll]".into();
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
            let target = change.user_id().to_string();
            out["state_target"] = target.clone().into();
            out["body"] = if change.displayname_change().is_some() {
                format!("{target} changed their display name.")
            } else {
                format!("{target} changed their avatar.")
            }.into();
        }
        TimelineItemContent::OtherState(state) => {
            out["msgtype"] = "state".into();
            let kind = state.content().event_type().to_string();
            let actor = event.sender().to_string();
            let text = match kind.as_str() {
                "m.room.create" => format!("{actor} created the room."),
                "m.room.name" => format!("{actor} changed the room name."),
                "m.room.topic" => format!("{actor} changed the room topic."),
                "m.room.avatar" => format!("{actor} changed the room avatar."),
                "m.room.encryption" => "Encryption was enabled.".to_owned(),
                _ => format!("{actor} updated room settings."),
            };
            out["state_kind"] = kind.into();
            out["body"] = text.into();
        }
        TimelineItemContent::FailedToParseMessageLike { .. }
        | TimelineItemContent::FailedToParseState { .. } => {
            out["msgtype"] = "unsupported".into();
            out["body"] = "[unsupported event]".into();
        }
        TimelineItemContent::CallInvite | TimelineItemContent::RtcNotification { .. } => {
            out["msgtype"] = "state".into();
            out["body"] = "call event".into();
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
            })
        }
        other => {
            out["msgtype"] = "unsupported".into();
            out["body"] = other.body().to_owned().into();
            None
        }
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
            MsgLikeKind::Poll(_) => "[poll]".to_owned(),
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
    use super::sessions_by_room_from_import;
    use std::collections::{BTreeMap, BTreeSet};

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
}
