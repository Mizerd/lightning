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
    collections::VecDeque,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    },
};

use futures_util::StreamExt;
use matrix_sdk::{
    room::edit::EditedContent,
    ruma::{
        events::{
            room::{
                message::{
                    MessageType, RoomMessageEventContent,
                    RoomMessageEventContentWithoutRelation,
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
        EncryptedMessage, EventSendState, EventTimelineItem, MsgLikeKind, Timeline,
        TimelineBuilder, TimelineDetails, TimelineEventItemId, TimelineItem,
        TimelineItemContent, TimelineItemKind, VirtualTimelineItem,
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

pub struct TimelineRegistry {
    events: EventQueue,
    active: Mutex<Option<ActiveTimeline>>,
    /// Bumped on every open-room call. Diffs/pagination results stamped with
    /// an older generation are stale and must be ignored on both sides.
    room_gen: AtomicU64,
    /// Bumped on shutdown (sign-out / handle release). Everything stamped
    /// with an older lifecycle is stale.
    lifecycle_gen: AtomicU64,
}

impl TimelineRegistry {
    pub fn new(events: EventQueue) -> Self {
        Self {
            events,
            active: Mutex::new(None),
            room_gen: AtomicU64::new(0),
            lifecycle_gen: AtomicU64::new(1),
        }
    }

    fn is_current(&self, room_gen: u64, lifecycle: u64) -> bool {
        self.room_gen.load(Ordering::SeqCst) == room_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
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
        let room_gen = self.room_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);

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

    /// Deterministic stop of all timeline work: advances the lifecycle
    /// generation, aborts the subscription task and awaits it (bounded).
    /// Called before sign-out store cleanup and before handle destruction.
    pub fn shutdown(&self, runtime: &tokio::runtime::Runtime) {
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
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

    /// Send a plain text message through the SDK timeline (send queue +
    /// SDK-owned local echo). Encryption is transparent for encrypted rooms.
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
                RoomMessageEventContent::text_plain(body),
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
            let content = RoomMessageEventContentWithoutRelation::text_plain(body);
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
                RoomMessageEventContentWithoutRelation::text_plain(new_body),
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
    let timeline = match TimelineBuilder::new(&room).build().await {
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
        items.iter().map(|item| item_to_json(item, &own_user)).collect();
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

    while let Some(diffs) = stream.next().await {
        if !registry.is_current(room_gen, lifecycle) {
            break;
        }
        for diff in diffs {
            let value = diff_to_json(&room_id, room_gen, lifecycle, &diff, &own_user);
            enqueue(&events, value);
        }
    }
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
) -> serde_json::Value {
    let base = |op: &str| {
        json!({
            "type": "timeline_diff",
            "room_id": room_id,
            "room_generation": room_gen,
            "lifecycle": lifecycle,
            "op": op,
        })
    };
    match diff {
        VectorDiff::Append { values } => {
            let mut v = base("append");
            v["items"] = values.iter().map(|i| item_to_json(i, own_user)).collect();
            v
        }
        VectorDiff::PushBack { value } => {
            let mut v = base("push_back");
            v["item"] = item_to_json(value, own_user);
            v
        }
        VectorDiff::PushFront { value } => {
            let mut v = base("push_front");
            v["item"] = item_to_json(value, own_user);
            v
        }
        VectorDiff::Insert { index, value } => {
            let mut v = base("insert");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, own_user);
            v
        }
        VectorDiff::Set { index, value } => {
            let mut v = base("set");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, own_user);
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
            v["items"] = values.iter().map(|i| item_to_json(i, own_user)).collect();
            v
        }
    }
}

/// Convert one SDK timeline item into the UI-safe FFI payload.
fn item_to_json(item: &TimelineItem, own_user: &str) -> serde_json::Value {
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
            event_item_to_json(&item.unique_id().0, event, own_user)
        }
    }
}

fn event_item_to_json(
    unique_id: &str,
    event: &EventTimelineItem,
    own_user: &str,
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
            match &msg_like.kind {
                MsgLikeKind::Message(message) => {
                    out["edited"] = message.is_edited().into();
                    fill_message_content(&mut out, message.msgtype());
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
                MsgLikeKind::Sticker(_) => {
                    out["msgtype"] = "unsupported".into();
                    out["body"] = "[sticker]".into();
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
            out["msgtype"] = "state".into();
            out["body"] = "membership change".into();
        }
        TimelineItemContent::ProfileChange(_) => {
            out["msgtype"] = "state".into();
            out["body"] = "profile change".into();
        }
        TimelineItemContent::OtherState(_) => {
            out["msgtype"] = "state".into();
            out["body"] = "room settings change".into();
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

/// Text / notice / emote / media conversion. Media metadata is forwarded
/// only when Lightning already supports it (plain mxc source); encrypted
/// media sources are not exposed as downloadable URLs.
fn fill_message_content(out: &mut serde_json::Value, msgtype: &MessageType) {
    match msgtype {
        MessageType::Text(content) => {
            out["msgtype"] = "text".into();
            out["body"] = content.body.clone().into();
        }
        MessageType::Notice(content) => {
            out["msgtype"] = "notice".into();
            out["body"] = content.body.clone().into();
        }
        MessageType::Emote(content) => {
            out["msgtype"] = "emote".into();
            out["body"] = content.body.clone().into();
        }
        MessageType::Image(content) => {
            out["msgtype"] = "image".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
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
            }
        }
        MessageType::File(content) => {
            out["msgtype"] = "file".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content
                .filename
                .clone()
                .unwrap_or_else(|| content.body.clone())
                .into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            if let Some(info) = &content.info {
                if let Some(mime) = &info.mimetype {
                    out["media_mimetype"] = mime.clone().into();
                }
                if let Some(size) = info.size {
                    out["media_size"] = u64::from(size).into();
                }
            }
        }
        MessageType::Audio(content) => {
            out["msgtype"] = "file".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
        }
        MessageType::Video(content) => {
            out["msgtype"] = "file".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = content.body.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
        }
        other => {
            out["msgtype"] = "unsupported".into();
            out["body"] = other.body().to_owned().into();
        }
    }
}

/// Best-effort short preview for reply boxes. Never includes ciphertext.
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
}
