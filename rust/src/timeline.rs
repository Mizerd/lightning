//! Live SDK timeline registry.
//!
//! Owns the `matrix_sdk_ui::Timeline` for the open room. One subscription
//! at a time: opening a room advances the room generation, cancels the
//! previous subscription task, drops the previous `Timeline`, and forwards
//! the new one's snapshot and `VectorDiff` stream to C++ as JSON.
//!
//! Never serializes key material, ciphertext or raw decrypted event JSON.

use std::{
    collections::{HashMap, HashSet, VecDeque},
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
        EventId, OwnedEventId, OwnedUserId, RoomId, UserId,
    },
    Client,
};
use matrix_sdk::ruma::{events::AnySyncTimelineEvent, room_version_rules::RoomVersionRules};
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    timeline::{
        default_event_filter,
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

/// Build an m.mentions payload from MXID strings. Invalid ids are dropped;
/// an empty result yields `None`. `add_mentions` must be called before any
/// relation-adding method (reply / replacement).
pub(crate) fn mentions_from_ids(ids: Vec<String>) -> Option<Mentions> {
    // "@room" travels in the same list as user ids. It can never collide with
    // a user (a Matrix id needs a domain), and it keeps one FFI signature for
    // "who does this message mention".
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
    // Whether the server acts on it is up to the room's power levels
    // (notifications.room); the client offers it only where allowed.
    mentions.room = room;
    Some(mentions)
}

/// The whole-room mention as it crosses the FFI; matches the composer text.
pub(crate) const ROOM_MENTION_SENTINEL: &str = "@room";

/// Events per backward-pagination batch (Element X uses the same).
pub const PAGINATION_BATCH: u16 = 20;

/// Pagination filter diagnostics: events offered to the filter, and those
/// dropped by the SDK defaults or as MatrixRTC membership. These separate
/// "the server returned nothing" from "we filtered everything", since
/// `paginate_backwards` only returns a bool.
///
/// Cumulative and process-global: the timeline ingests after pagination
/// returns, so a per-batch delta would race; read the climb across pages.
/// An open thread panel shares them. Events that fail to deserialize never
/// reach the filter, and `hide_threaded_events` is applied after it.
pub(crate) static FILTER_OFFERED: AtomicU64 = AtomicU64::new(0);
pub(crate) static FILTER_DROP_SDK: AtomicU64 = AtomicU64::new(0);
pub(crate) static FILTER_DROP_RTC: AtomicU64 = AtomicU64::new(0);

/// The event filter every timeline uses: the SDK defaults, minus MatrixRTC
/// membership state. A call re-publishes one membership event per
/// participant per minute, so call rooms carry thousands; as rows they made
/// such rooms slow to open and scroll, and nothing on screen needs them (the
/// call UI reads membership from room state).
pub(crate) fn lightning_event_filter(
    event: &AnySyncTimelineEvent,
    rules: &RoomVersionRules,
) -> bool {
    FILTER_OFFERED.fetch_add(1, Ordering::Relaxed);
    if !default_event_filter(event, rules) && !is_visible_gallery_message(event) {
        FILTER_DROP_SDK.fetch_add(1, Ordering::Relaxed);
        return false;
    }
    if is_rtc_membership_event(event) {
        FILTER_DROP_RTC.fetch_add(1, Ordering::Relaxed);
        return false;
    }
    true
}

/// MSC4274 galleries (including Sable's unstable `dm.filament.gallery`).
/// matrix-sdk-ui's default filter keeps a gallery only with its own
/// `unstable-msc4274` feature, which this build does not enable, so
/// galleries would never become rows. Accepted under the same rule the
/// default filter applies to other msgtypes: an `m.replace` is folded into
/// its target, never a row.
fn is_visible_gallery_message(event: &AnySyncTimelineEvent) -> bool {
    use matrix_sdk::ruma::events::{
        room::message::Relation, AnySyncMessageLikeEvent, SyncMessageLikeEvent,
    };
    let AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
        SyncMessageLikeEvent::Original(message),
    )) = event
    else {
        return false;
    };
    is_gallery_msgtype(message.content.msgtype.msgtype())
        && !matches!(message.content.relates_to, Some(Relation::Replacement(_)))
}

pub(crate) fn is_rtc_membership_event(event: &AnySyncTimelineEvent) -> bool {
    let AnySyncTimelineEvent::State(state) = event else {
        return false;
    };
    matches!(
        state.event_type().to_string().as_str(),
        "org.matrix.msc3401.call.member"
            | "org.matrix.msc4143.rtc.member"
            | "m.call.member"
            | "m.rtc.member"
    )
}

/// Retired; do not size anything against this. Use
/// `SHUTDOWN_WORST_CASE_MS` in lib.rs and its compile-time assert.
#[deprecated(note = "size against SHUTDOWN_WORST_CASE_MS in lib.rs instead")]
#[allow(dead_code)]
pub const SHUTDOWN_JOIN_TIMEOUT_SECS: u64 = 15;

/// Re-enable a room's send queue before handing it anything new.
///
/// matrix-sdk disables a room's queue after any send error, and the sync
/// recovery edges only fire when sync recovers, so a send-only failure
/// (429 or 5xx on /send, upload timeout) would leave the room wedged.
/// Composing a new message unwedges it.
///
/// Cheap: `RoomSendQueue::set_enabled` is an atomic store plus a notify
/// (unlike the client-wide `SendQueue::set_enabled`, which queries SQLite).
/// Safe: unrecoverable failures are persisted as wedged and skipped, so
/// nothing already rejected is resent. Not needed for redaction, since
/// Lightning uses `Room::redact`, which bypasses the queue.
fn unwedge_send_queue(timeline: &Timeline) {
    timeline.room().send_queue().set_enabled(true);
}

/// How long `shutdown` waits on a timeline task it has already aborted
/// (`take_active*` call `abort()` first). Only guards against a task stuck
/// in a synchronous stretch. Counted in `SHUTDOWN_WORST_CASE_MS` in lib.rs.
pub const SHUTDOWN_ABORTED_JOIN_MS: u64 = 250;

type EventQueue = Arc<Mutex<VecDeque<String>>>;

/// Media sources captured while serializing timeline items, for the media
/// bridge. `MediaSource::Encrypted` embeds content keys, so these stay in
/// this Rust-side map and never cross the FFI. Keyed by event id (or SDK
/// unique id for local echoes); lives as long as the room's timeline.
pub struct StoredMedia {
    pub source: MediaSource,
    pub thumbnail: Option<MediaSource>,
    pub filename: String,
    pub mimetype: Option<String>,
    /// Declared byte size from `info`, when present. Full-payload fetches over
    /// the class cap are refused pre-flight.
    pub declared_size: Option<u64>,
}

/// Upper bound on how long one reaction toggle may hold its in-flight slot,
/// so a stuck send cannot leave a reaction unclickable.
const REACTION_GUARD_TIMEOUT_SECS: u64 = 30;

/// Bound for the per-room media source map; guards runaway growth only.
const MEDIA_SOURCE_CAP: usize = 4096;

/// The media source map, bounded by evicting the least recently used key.
///
/// Refusing new keys when full would break exactly the rows the reader is
/// looking at (galleries register many keys each). "Used" means registered
/// (every serialization) or looked up by a fetch. A row served from the C++
/// cache touches nothing here, so its key can be evicted; a later fetch then
/// fails as "unknown media item" until the row is serialized again.
struct MediaRegistry {
    entries: HashMap<String, (StoredMedia, u64)>,
    /// (stamp, key) in stamp order. Lazily pruned: an entry whose stamp no
    /// longer matches `entries` is a superseded touch and is skipped.
    order: VecDeque<(u64, String)>,
    clock: u64,
    cap: usize,
}

impl MediaRegistry {
    fn with_cap(cap: usize) -> Self {
        Self { entries: HashMap::new(), order: VecDeque::new(), clock: 0, cap: cap.max(1) }
    }

    /// A fresh stamp for `key`, queued. The caller stores it on the entry.
    fn stamp(&mut self, key: &str) -> u64 {
        // Rebuild before superseded touches outgrow the map by a constant factor.
        if self.order.len() >= self.cap.saturating_mul(4) {
            let mut live: Vec<(u64, String)> =
                self.entries.iter().map(|(k, (_, t))| (*t, k.clone())).collect();
            live.sort_unstable();
            self.order = live.into();
        }
        self.clock += 1;
        self.order.push_back((self.clock, key.to_owned()));
        self.clock
    }

    fn insert(&mut self, key: String, media: StoredMedia) {
        let stamp = self.stamp(&key);
        self.entries.insert(key, (media, stamp));
        while self.entries.len() > self.cap {
            let Some((stamp, key)) = self.order.pop_front() else { break };
            if self.entries.get(&key).map(|(_, t)| *t) == Some(stamp) {
                self.entries.remove(&key);
            }
        }
    }

    fn get(&mut self, key: &str) -> Option<&StoredMedia> {
        if !self.entries.contains_key(key) {
            return None;
        }
        let stamp = self.stamp(key);
        let entry = self.entries.get_mut(key)?;
        entry.1 = stamp;
        Some(&entry.0)
    }

    #[cfg(test)]
    fn contains(&self, key: &str) -> bool {
        self.entries.contains_key(key)
    }

    #[cfg(test)]
    fn len(&self) -> usize {
        self.entries.len()
    }

    fn clear(&mut self) {
        self.entries.clear();
        self.order.clear();
    }
}

struct ActiveTimeline {
    room_id: String,
    room_gen: u64,
    /// Set once the SDK `Timeline` is built; `None` while building or after a
    /// failed build.
    timeline: Option<Arc<Timeline>>,
    /// The subscription forwarder task. Aborting it drops the diff stream and
    /// stops the SDK timeline's internal tasks.
    task: Option<tokio::task::JoinHandle<()>>,
    /// Single-flight guard: only one backward pagination at a time.
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

/// The open room's thread timeline, using `TimelineFocus::Thread`, so the
/// SDK handles all thread relations, sends, edits, reactions and
/// redactions. A filtered view over the same room data.
struct ActiveThread {
    room_id: String,
    root_event_id: String,
    thread_gen: u64,
    timeline: Option<Arc<Timeline>>,
    task: Option<tokio::task::JoinHandle<()>>,
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

/// The room's paginated `ThreadListService` while the Threads view is open.
/// Page-sized snapshots are forwarded to C++ on each update batch.
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
    /// The open thread panel's timeline. Always belongs to the open room.
    active_thread: Mutex<Option<ActiveThread>>,
    /// The open room's thread list view, if any.
    active_thread_list: Mutex<Option<ActiveThreadList>>,
    thread_list_gen: AtomicU64,
    /// Bumped on every open-thread/close-thread call; stale thread events are
    /// rejected on both sides.
    thread_gen: AtomicU64,
    /// Bumped on every open-room call. Diffs/pagination results stamped with
    /// an older generation are stale and must be ignored on both sides.
    room_gen: AtomicU64,
    /// Bumped on shutdown (sign-out / handle release). Everything stamped
    /// with an older lifecycle is stale.
    lifecycle_gen: AtomicU64,
    /// Media sources for the open room. Cleared on room open and shutdown.
    /// Never crosses the FFI.
    media_sources: Mutex<MediaRegistry>,
    /// One reaction toggle per (room, event, key) at a time. Concurrent toggles
    /// for one target raced into a storm of add/remove echoes, so a click
    /// arriving while its target is still resolving is dropped (a toggle is a
    /// request to flip, and queueing flips replays the race). Cleared on
    /// shutdown.
    reaction_inflight: Mutex<std::collections::HashSet<String>>,
    /// Backup key-download attempts this lifecycle ("<room>" for whole-room
    /// passes, "<room>\x1f<session>" per session), so recovery never polls the
    /// backup. Each key records when and how often it was tried, and
    /// `backup_attempt_backoff` decides when another try is due, so a key that
    /// reaches the backup late is still picked up. Cleared on shutdown; manual
    /// recovery clears the open room. Identifiers only.
    backup_download_attempts: Mutex<std::collections::HashMap<String, BackupAttempt>>,
}

/// One key's attempt history this lifecycle. Identifiers and counters only.
///
/// Two counters: `tries` drives the backoff and is never refunded, so a
/// sustained outage escalates; `attempts` is the budget and counts only
/// definitive answers.
#[derive(Clone, Copy)]
pub(crate) struct BackupAttempt {
    /// Every attempt. Drives the backoff (30 s up to 32 min).
    tries: u32,
    /// Definitive answers only (the budget). A transport failure spends
    /// nothing; a permanent refusal spends it all.
    attempts: u32,
    /// When the last attempt ran, for the backoff.
    last: std::time::Instant,
}

/// What an attempt established. Explicit, because `download_room_key`
/// returns `Err` both for "not in your backup" and "could not reach it".
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum BackupOutcome {
    /// The server answered and the key is not there. Spends one unit of
    /// budget; it may be uploaded later.
    Definitive,
    /// This device has the backup's public upload key but not the decryption
    /// key, so no request was sent. `are_enabled()` does not exclude this (it
    /// checks the public key). Typical for a verified device whose recovery
    /// key was never entered; spends no budget and prompts the user to enter
    /// the recovery key.
    NoDecryptionKey,
    /// No such endpoint, or not allowed. Ends this key for the lifecycle.
    PermanentRefusal,
    /// Nothing learned (unreachable, rate-limited, store error). Costs a
    /// backoff step and no budget.
    Inconclusive,
}

/// The retry policy as a pure function, so it can be tested without a
/// clock.
fn backup_attempt_allowed(tries: u32, attempts: u32, since_last: std::time::Duration) -> bool {
    attempts < MAX_BACKUP_ATTEMPTS
        && since_last >= backup_attempt_backoff(tries.saturating_sub(1))
}

/// Maximum automatic attempts per key per lifecycle. The first attempt is
/// always allowed; the wait then doubles from 30 s to ~32 min. Attempts
/// only happen when an undecryptable event is in front of the user.
const MAX_BACKUP_ATTEMPTS: u32 = 8;

/// Marks a key never to be tried again this lifecycle. Distinct from
/// `MAX_BACKUP_ATTEMPTS` so raising the cap cannot un-park refused keys.
/// `clear_backup_attempt` still frees these.
const BACKUP_ATTEMPTS_STOPPED: u32 = u32::MAX;

/// The wait after `attempts_already_made` tries: 30 s, 1, 2, 4, 8, 16 and
/// 32 minutes, then flat. Takes the count already made, not the next one.
fn backup_attempt_backoff(attempts_already_made: u32) -> std::time::Duration {
    let secs = 30u64.saturating_mul(1u64 << attempts_already_made.min(6));
    std::time::Duration::from_secs(secs.min(32 * 60))
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
            media_sources: Mutex::new(MediaRegistry::with_cap(MEDIA_SOURCE_CAP)),
            reaction_inflight: Mutex::new(std::collections::HashSet::new()),
            backup_download_attempts: Mutex::new(std::collections::HashMap::new()),
        }
    }

    /// Record a backup-download attempt. Returns false when the key is not due
    /// yet (see `backup_attempt_backoff`) or has used up `MAX_BACKUP_ATTEMPTS`;
    /// the caller then skips it.
    fn mark_backup_attempt(&self, key: &str) -> bool {
        self.mark_backup_attempt_at(key, std::time::Instant::now())
    }

    /// `mark_backup_attempt` with the clock passed in, so tests can check the
    /// wiring to the policy (argument order included) without sleeping.
    fn mark_backup_attempt_at(&self, key: &str, now: std::time::Instant) -> bool {
        match self.backup_download_attempts.lock() {
            Ok(mut guard) => match guard.get_mut(key) {
                None => {
                    guard.insert(
                        key.to_owned(),
                        BackupAttempt { tries: 1, attempts: 0, last: now },
                    );
                    true
                }
                Some(record) => {
                    // A clock that appears to go backwards must not read as "never due".
                    if !backup_attempt_allowed(
                        record.tries,
                        record.attempts,
                        now.saturating_duration_since(record.last),
                    ) {
                        return false;
                    }
                    record.tries = record.tries.saturating_add(1);
                    record.last = now;
                    true
                }
            },
            Err(_) => false,
        }
    }

    /// Record what an attempt established. Only this spends the budget.
    fn record_backup_outcome(&self, key: &str, outcome: BackupOutcome) {
        let spend = match outcome {
            BackupOutcome::Definitive => 1,
            // A missing endpoint or a refusal will not change on retry.
            BackupOutcome::PermanentRefusal => BACKUP_ATTEMPTS_STOPPED,
            // These say nothing about the backup's contents, so they spend no budget;
            // the backoff already escalated.
            BackupOutcome::Inconclusive | BackupOutcome::NoDecryptionKey => {
                return
            }
        };
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            if let Some(record) = guard.get_mut(key) {
                record.attempts = if spend == BACKUP_ATTEMPTS_STOPPED {
                    BACKUP_ATTEMPTS_STOPPED
                } else {
                    record.attempts.saturating_add(spend)
                };
            }
        }
    }

    /// Forget a room's backup attempts, including its per-session entries, so
    /// manual recovery can force one fresh pass.
    pub fn clear_backup_attempt(&self, room_id: &str) {
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            let prefix = format!("{room_id}\u{1f}");
            guard.retain(|key, _| key != room_id && !key.starts_with(&prefix));
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

    /// Current lifecycle generation, stamped on command results so C++ can
    /// reject completions from a signed-out session.
    pub fn lifecycle(&self) -> u64 {
        self.lifecycle_gen.load(Ordering::SeqCst)
    }

    /// True while `lifecycle` is still the active session generation.
    pub fn lifecycle_current(&self, lifecycle: u64) -> bool {
        self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Record a media source captured during item serialization.
    pub(crate) fn remember_media(&self, key: String, media: StoredMedia) {
        if key.is_empty() {
            return;
        }
        if let Ok(mut guard) = self.media_sources.lock() {
            guard.insert(key, media);
        }
    }

    /// Look up a media source for the media bridge. Clones it so the lock is
    /// never held across an await.
    pub fn media_source(
        &self,
        key: &str,
        thumbnail: bool,
    ) -> Option<(MediaSource, String, Option<String>, Option<u64>, bool)> {
        let mut guard = self.media_sources.lock().ok()?;
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

    /// Abort and forget the active timeline. Returns the task handle so
    /// shutdown can await it.
    fn take_active(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String)> {
        let mut guard = self.active.lock().ok()?;
        let active = guard.take()?;
        if let Some(task) = &active.task {
            task.abort();
        }
        // Dropping our Arc here, and the stream when the abort lands, releases the
        // SDK's internal drop handle.
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

    /// Re-open the live timeline after letting the SDK's event cache shrink to
    /// its last chunk, releasing the paginated backlog (like Element's
    /// `jumpToLiveTimeline()`). Only for an explicit jump to the newest message;
    /// never for ordinary scrolling.
    ///
    /// The SDK shrinks when the last `RoomEventCacheSubscriber` drops. This adds
    /// ordering: wait for our timeline to be gone and the shrink to land before
    /// building the replacement, or the new subscription inherits the backlog.
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

        // Keep the previous task's handle when shrinking: the event-cache
        // subscriber lives in it, and abort() only requests cancellation.
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
        // A thread and the Threads view never outlive their room.
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

    // ── SDK-backed thread timelines ─────────────────────────────────────

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

    /// Open (or replace) the thread timeline for `root_event_id`, using
    /// `TimelineFocus::Thread` so the SDK owns relations, pagination and
    /// encryption.
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

    // ── Room thread list and threaded read state ─────────────────────────

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

    /// Open (or replace) the Threads view for `room_id`: build the SDK
    /// ThreadListService, fetch the first page, and forward a snapshot on each
    /// update batch.
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

    /// Fetch the next thread-list page. Single-flight; requests past the end
    /// are dropped (the UI knows via end_reached).
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

    /// Send a threaded read receipt for the open thread (the SDK's focus-aware
    /// mark_as_read, never a room-wide receipt).
    pub fn mark_thread_read(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        root_event_id: String,
        privacy: i32,
    ) -> Result<(), String> {
        // 2 = none. A thread receipt has no fully-read marker to fall back to, so
        // nothing is sent.
        if privacy == 2 {
            return Ok(());
        }
        let Some((timeline, _gen, _lifecycle)) =
            self.thread_timeline_for(&room_id, &root_event_id)
        else {
            return Err("No live thread timeline is open for that root.".to_owned());
        };
        let receipt_type = if privacy == 1 {
            ReceiptType::ReadPrivate
        } else {
            ReceiptType::Read
        };
        runtime.spawn(async move {
            let _ = timeline.mark_as_read(receipt_type).await;
        });
        Ok(())
    }

    /// Close the open thread timeline. Safe when none is open.
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

    /// The open thread timeline in `room_id`, without a root id. Retry and
    /// Cancel identify messages by transaction id with no thread notion, and
    /// only one thread is open at a time.
    fn open_thread_timeline_in_room(&self, room_id: &str) -> Option<Arc<Timeline>> {
        let guard = self.active_thread.lock().ok()?;
        let thread = guard.as_ref()?;
        if thread.room_id != room_id {
            return None;
        }
        thread.timeline.clone()
    }

    /// The open thread timeline when it matches `room_id` and `root_event_id`,
    /// with its stamps.
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
    /// suppressed after reaching the start, like the room path.
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

    /// Send a plain-text thread reply. Uses the open thread panel's timeline,
    /// or a transient thread-focused one, so the SDK always builds the m.thread
    /// relation and reply fallback, and encryption follows the normal path. A
    /// non-empty `in_reply_to` makes a rich reply within the thread.
    #[allow(clippy::too_many_arguments)]
    pub fn send_thread_text(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        root_event_id: String,
        body: String,
        in_reply_to: Option<String>,
        mention_user_ids: Vec<String>,
        spec: SendBodySpec,
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
        // Capture the thread generation so a send resolving after a thread switch,
        // room switch or logout does not raise a cross-context error toast.
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &root_event_id);
        runtime.spawn(async move {
            let send_on = |timeline: Arc<Timeline>| {
                let body = body.clone();
                let reply_to = reply_to.clone();
                let mentions = mentions.clone();
                let spec = spec.clone();
                async move {
                    match reply_to {
                        Some(reply_to) => {
                            let mut content =
                                composed_content_without_relation(&body, &spec);
                            if let Some(mentions) = mentions {
                                content = content.add_mentions(mentions);
                            }
                            unwedge_send_queue(&timeline);
                            timeline.send_reply(content, reply_to).await.is_ok()
                        }
                        None => {
                            let mut message = composed_content(&body, &spec);
                            if let Some(mentions) = mentions {
                                message = message.add_mentions(mentions);
                            }
                            unwedge_send_queue(&timeline);
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

    /// Send prebuilt message-like content (polls, stickers) to a room or
    /// thread timeline. The room path mirrors `edit`/`redact`; the thread path
    /// mirrors `send_thread_text`, so the SDK attaches any `m.thread` relation
    /// (never built by hand). Content with an `m.reference`
    /// relation is left untouched.
    pub(crate) fn send_content(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        content: AnyMessageLikeEventContent,
        failure_category: &'static str,
    ) -> Result<(), String> {
        self.send_content_to_timeline(
            runtime, client, room_id, thread_root_id, content, failure_category,
        )
    }

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
                unwedge_send_queue(&timeline);
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
                unwedge_send_queue(&timeline);
                timeline.send(content).await.is_ok()
            } else {
                match build_transient_thread_timeline(
                    &client, &room_id, &thread_root_id,
                )
                .await
                {
                    Some(timeline) => {
                        unwedge_send_queue(&timeline);
                        timeline.send(content).await.is_ok()
                    }
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

    /// Vote on an MSC3381 poll. An empty answer list retracts the vote.
    /// Aggregation stays in the SDK; the poll item updates via a Set diff.
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

    /// End an MSC3381 poll. The server enforces permissions; the UI offers it
    /// only for the user's own polls.
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

    /// Create an MSC3381 poll in a room or thread. Content comes from
    /// `build_poll_start_content`; a thread target gets its relation from the
    /// SDK.
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

    /// One deduplicated whole-room key download from the key backup
    /// (`download_room_keys_for_room`), since the SDK's OneShot download never
    /// re-runs once a key is stored. Once per room per lifecycle plus one
    /// bounded retry; manual recovery can force another. Imported keys reach
    /// open timelines through the SDK's redecryption.
    pub async fn download_backup_keys_for_room(
        self: &Arc<Self>,
        client: &Client,
        room_id: &str,
    ) {
        let lifecycle = self.lifecycle();
        let emit = |state: &str| {
            if self.lifecycle_current(lifecycle) {
                enqueue(
                    &self.events,
                    json!({
                        "type": "crypto_bootstrap",
                        // A skip is not a download outcome.
                        "kind": if state.starts_with("skipped_") {
                            "backup_download_skipped"
                        } else {
                            "backup_download"
                        },
                        "state": state,
                        "count": 0,
                        "lifecycle": lifecycle,
                    }),
                );
            }
        };
        // Report why a pass did not run, so "backups unusable", "already done this
        // lifecycle" and "ran and found nothing" can be told apart; this is the
        // only automatic route from a backed-up key to a decrypted row.
        //
        // Skips use their own `kind`: `backup_download` is assigned
        // unconditionally and anything but "failed" reads as Ready, so reusing it
        // would let a skipped room hide another room's failure. Closed
        // vocabulary, no room ids, no key material.
        let backups = client.encryption().backups();
        if !backups.are_enabled().await {
            emit("skipped_no_backup_key");
            return; // no usable backup key — nothing to download
        }
        let Ok(room_ref) = RoomId::parse(room_id) else {
            emit("skipped_bad_room_id");
            return;
        };
        if !self.mark_backup_attempt(room_id) {
            emit("skipped_already_attempted");
            return; // already attempted this lifecycle
        }
        emit("started");
        let mut result = backups.download_room_keys_for_room(&room_ref).await;
        if result.is_err() && self.lifecycle_current(lifecycle) {
            // One bounded retry, so one transient failure does not strand history.
            tokio::time::sleep(std::time::Duration::from_secs(3)).await;
            result = backups.download_room_keys_for_room(&room_ref).await;
        }
        // Sanitized outcome only; error strings can embed URLs and backup
        // versions.
        emit(if result.is_ok() { "ok" } else { "failed" });
        // A failed pass stays re-runnable on the next Verified/BackupState edge.
        if result.is_err() {
            self.clear_backup_attempt(room_id);
        }
    }

    /// Stop all timeline work: advance the lifecycle generation, abort the
    /// subscription task and await it (bounded). Called before sign-out store
    /// cleanup and handle destruction.
    pub fn shutdown(&self, runtime: &tokio::runtime::Runtime) {
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
        self.close_thread_list();
        self.clear_media();
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            guard.clear();
        }
        // A toggle from the departing session must not block that reaction later.
        if let Ok(mut guard) = self.reaction_inflight.lock() {
            guard.clear();
        }
        if let Some((task, _room, _root)) = self.take_active_thread() {
            if let Some(task) = task {
                let _ = runtime.block_on(async {
                    tokio::time::timeout(
                        std::time::Duration::from_millis(SHUTDOWN_ABORTED_JOIN_MS),
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
                        std::time::Duration::from_millis(SHUTDOWN_ABORTED_JOIN_MS),
                        task,
                    )
                    .await
                });
            }
        }
        enqueue(&self.events, json!({ "type": "timeline_shutdown" }));
    }

    /// The active room's timeline, when it matches `room_id` and is built.
    fn timeline_for(&self, room_id: &str) -> Option<(Arc<Timeline>, u64, u64)> {
        let guard = self.active.lock().ok()?;
        let active = guard.as_ref()?;
        if active.room_id != room_id {
            return None;
        }
        let timeline = active.timeline.clone()?;
        Some((timeline, active.room_gen, self.lifecycle_gen.load(Ordering::SeqCst)))
    }

    /// Start one backward pagination batch. Rejected while another runs or
    /// once the start of history was reached.
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
            // Not an error; the UI knows via reached_start.
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
                            // Cumulative (see the counters). `offered` rising with `drop_rtc` means
                            // MatrixRTC churn; rising with neither drop means thread hiding or
                            // aggregation; flat means the page really was empty.
                            "filter_offered": FILTER_OFFERED
                                .load(Ordering::Relaxed),
                            "filter_dropped_sdk": FILTER_DROP_SDK
                                .load(Ordering::Relaxed),
                            "filter_dropped_rtc": FILTER_DROP_RTC
                                .load(Ordering::Relaxed),
                        }),
                    );
                }
                Err(_err) => {
                    // Category only: pagination errors may embed server detail.
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

    /// Send a text message through the SDK timeline (send queue, SDK local
    /// echo). The body is markdown by default; `spec` selects the plain or html
    /// lane (see parse_body_spec).
    pub fn send_text(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        body: String,
        mention_user_ids: Vec<String>,
        spec: SendBodySpec,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let mentions = mentions_from_ids(mention_user_ids);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let mut message = composed_content(&body, &spec);
            if let Some(mentions) = mentions {
                message = message.add_mentions(mentions);
            }
            let content = AnyMessageLikeEventContent::RoomMessage(message);
            unwedge_send_queue(&timeline);
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

    /// Send an attachment through the SDK send queue. The local echo flows
    /// through the diff stream like a text echo, failed uploads retry via
    /// `unwedge`, and the SDK encrypts payload and thumbnail in encrypted rooms.
    ///
    /// `op_id` identifies the queue attempt; only queueing success/failure is
    /// reported here. `thumbnail` is an optional poster that the SDK uploads
    /// and writes into `info.thumbnail_source`.
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
            unwedge_send_queue(&timeline);
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
                    // Coarse category only; SDK errors may embed local paths or server
                    // detail.
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
        spec: SendBodySpec,
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
            let mut content = composed_content_without_relation(&body, &spec);
            if let Some(mentions) = mentions {
                content = content.add_mentions(mentions);
            }
            unwedge_send_queue(&timeline);
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

    /// Send an attachment into a thread via the thread-focused timeline, so the
    /// SDK's `infer_reply` attaches the `m.thread` relation (with reply
    /// fallback) and encrypts as needed; it never lands as an ordinary room
    /// message. Uses a transient thread timeline if the panel is not open. The
    /// result is gated on the thread still being current.
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
                    // None: infer_reply threads it from the focus.
                    in_reply_to: None,
                };
                unwedge_send_queue(&timeline);
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
                        // Coarse category only.
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
        thread_root_id: String,
        target_event_id: String,
        new_body: String,
        mention_user_ids: Vec<String>,
        spec: SendBodySpec,
    ) -> Result<(), String> {
        // Use the timeline that holds the event. `Timeline::edit` looks the target
        // up in its own items, and the live room timeline hides threaded events,
        // so editing a thread reply there always failed. Same rule as redaction,
        // retry/cancel and reactions.
        let in_thread = !thread_root_id.trim().is_empty();
        let resolved = if in_thread {
            self.thread_timeline_for(&room_id, &thread_root_id)
        } else {
            self.timeline_for(&room_id)
        };
        // `timeline_gen`, not `room_gen`: in the thread branch it is a thread
        // generation, which must be checked against the thread counter. Checking
        // it against `room_gen` made the failure report unreachable.
        let Some((timeline, timeline_gen, lifecycle)) = resolved else {
            return Err(if in_thread {
                "No live timeline is open for that thread.".to_owned()
            } else {
                "No live timeline is open for that room.".to_owned()
            });
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid edit target event id.".to_owned())?;
        let mentions = mentions_from_ids(mention_user_ids);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        // The payload field is `room_generation`, so carry the room counter there.
        let room_gen_for_report = self.room_gen.load(Ordering::SeqCst);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // ruma's make_replacement puts mentions in m.new_content and only newly
            // added ones at top level, so attach them before wrapping.
            let mut message = composed_content_without_relation(&new_body, &spec);
            if let Some(mentions) = mentions {
                message = message.add_mentions(mentions);
            }
            let content = EditedContent::RoomMessage(message);
            unwedge_send_queue(&timeline);
            if timeline.edit(&item_id, content).await.is_err() {
                // Each generation against its own counter, as in `toggle_reaction`.
                let current = if in_thread {
                    registry.thread_current(timeline_gen, lifecycle)
                } else {
                    registry.is_current(timeline_gen, lifecycle)
                };
                if current {
                    // timeline_send_failed for both lanes: it yields "The edit could not be
                    // applied.", whereas thread_send_failed would talk about a thread reply.
                    enqueue(
                        &events,
                        json!({
                            "type": "timeline_send_failed",
                            "room_id": room_id,
                            "room_generation": room_gen_for_report,
                            "lifecycle": lifecycle,
                            "category": "edit_rejected",
                        }),
                    );
                }
            }
        });
        Ok(())
    }

    /// Toggle a reaction through the SDK timeline.
    pub fn toggle_reaction(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        target_event_id: String,
        key: String,
    ) -> Result<(), String> {
        // Use the timeline that holds the event: the live room timeline hides
        // threaded events, so a thread reply is resolved on its thread timeline.
        let open_room_timeline = self.timeline_for(&room_id);
        let in_thread = !thread_root_id.trim().is_empty();
        if !in_thread && open_room_timeline.is_none() {
            return Err("No live timeline is open for that room.".to_owned());
        }
        let open_thread = if in_thread {
            self.thread_timeline_for(&room_id, &thread_root_id)
        } else {
            None
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid reaction target event id.".to_owned())?;
        // One in-flight toggle per (room, event, key); a second click while the
        // first resolves is dropped.
        //
        // The key includes the lifecycle generation: a task from a departed
        // session still releases its key after shutdown() cleared the set, and must
        // not free a slot the next session claimed.
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let guard_key =
            format!("{lifecycle}\u{1f}{room_id}\u{1f}{target_event_id}\u{1f}{key}");
        if !self.begin_reaction(&guard_key) {
            return Ok(());
        }
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let room_gen_for_report =
            open_room_timeline.as_ref().map(|(_, g, _)| *g).unwrap_or_else(
                || self.room_gen.load(Ordering::SeqCst));
        let room_timeline = open_room_timeline.map(|(t, _, _)| t);
        let report_room_id = room_id.clone();
        let report_thread_root = thread_root_id.clone();
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // The await is bounded, but the send is not cancelled: a queued send may
            // wait on the network indefinitely, and only the guard is released.
            let send = tokio::spawn(async move {
                // A thread reply reacts on its thread timeline (open or transient), like a
                // thread send.
                let timeline = if in_thread {
                    match open_thread {
                        Some((timeline, _, _)) => Some(timeline),
                        None => build_transient_thread_timeline(
                            &client, &room_id, &thread_root_id,
                        )
                        .await,
                    }
                } else {
                    room_timeline
                };
                match timeline {
                    // Do not discard the result. But `FailedToToggleReaction` (the item is not
                    // in this timeline's loaded window, e.g. after a jump-to-live trim) is not
                    // reported, since that would be a wrong visible error; anything else is.
                    Some(timeline) => {
                        unwedge_send_queue(&timeline);
                        match timeline.toggle_reaction(&item_id, &key).await {
                            Ok(_) => true,
                            Err(matrix_sdk_ui::timeline::Error::FailedToToggleReaction) => {
                                true
                            }
                            Err(_) => false,
                        }
                    }
                    None => false,
                }
            });
            let outcome = tokio::time::timeout(
                std::time::Duration::from_secs(REACTION_GUARD_TIMEOUT_SECS),
                send,
            )
            .await;
            registry.end_reaction(&guard_key);
            // A failed reaction is reported like a failed send. A timeout is not a
            // failure: the inner task is still running.
            if let Ok(Ok(false)) = outcome {
                let stale = if in_thread {
                    !registry.thread_current(thread_gen, lifecycle)
                } else {
                    !registry.is_current(room_gen_for_report, lifecycle)
                };
                if !stale {
                    enqueue(
                        &events,
                        json!({
                            "type": if in_thread { "thread_send_failed" }
                                    else { "timeline_send_failed" },
                            "room_id": report_room_id,
                            "thread_root_id": report_thread_root,
                            "room_generation": room_gen_for_report,
                            "lifecycle": lifecycle,
                            "category": "reaction_rejected",
                        }),
                    );
                }
            }
        });
        Ok(())
    }

    /// Claim the in-flight slot for one reaction target. Returns false when a
    /// toggle for it is already running. A poisoned lock fails closed.
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
        client: Client,
        room_id: String,
        target_event_id: String,
        reason: String,
    ) -> Result<(), String> {
        // Room-level, not timeline-level: `Timeline::redact` looks the target up
        // in its own items, and the live room timeline hides threaded events. A
        // redaction is addressed by event id, and the `m.room.redaction` returns
        // through sync to whichever timeline holds the event.
        let room = crate::rooms::joined_room(&client, &room_id)?;
        // Generations still come from the room's open timeline, so a failure after
        // a room change or sign-out is dropped as stale. With no timeline open the
        // failure is not reportable.
        let (room_gen, lifecycle) = match self.timeline_for(&room_id) {
            Some((_, gen, lc)) => (gen, lc),
            None => (
                self.room_gen.load(Ordering::SeqCst),
                self.lifecycle_gen.load(Ordering::SeqCst),
            ),
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid redaction target event id.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let reason = if reason.is_empty() { None } else { Some(reason.as_str()) };
            if room.redact(&event_id, reason, None).await.is_err()
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

    /// Retry a failed local echo by transaction id via the send queue's
    /// `unwedge`, which re-attempts the same item without duplicating it.
    pub fn retry_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        // A thread reply's local echo is not in the live room timeline (it hides
        // threaded events), and the FFI carries no thread notion, so fall back to
        // the open thread timeline in this room.
        let thread_timeline = self.open_thread_timeline_in_room(&room_id);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            // Generic over the container so the SDK's imbl Vector type need not be
            // named.
            let find_handle = |items: &_| -> Option<_> {
                fn scan<'a, I: IntoIterator<Item = &'a Arc<TimelineItem>>>(
                    items: I,
                    txn: &str,
                ) -> Option<matrix_sdk::send_queue::SendHandle>
                where
                    I::IntoIter: DoubleEndedIterator,
                {
                    items.into_iter().rev().find_map(|item| {
                        let event = item.as_event()?;
                        if event.transaction_id().map(|t| t.to_string())
                            == Some(txn.to_owned())
                        {
                            event.local_echo_send_handle()
                        } else {
                            None
                        }
                    })
                }
                scan(items, &transaction_id)
            };
            let mut handle = find_handle(&timeline.items().await);
            if handle.is_none() {
                if let Some(thread) = thread_timeline.as_ref() {
                    handle = find_handle(&thread.items().await);
                }
            }
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
            // Re-enable the room's queue first; a failed send is what disabled it.
            unwedge_send_queue(&timeline);
            let _ = handle.unwedge().await;
        });
        Ok(())
    }

    /// Cancel a local echo not yet sent, by transaction id, via
    /// `SendHandle::abort`. It also aborts an in-flight media upload and
    /// returns `Ok(false)` if the event was already sent. Success emits nothing;
    /// the SDK's `CancelledLocalEvent` removes the row through the diff path.
    pub fn cancel_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        // A thread reply's local echo is not in the live room timeline; see
        // `retry_send`.
        let thread_timeline = self.open_thread_timeline_in_room(&room_id);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            // Generic over the container so the SDK's imbl Vector type need not be
            // named.
            let find_handle = |items: &_| -> Option<_> {
                fn scan<'a, I: IntoIterator<Item = &'a Arc<TimelineItem>>>(
                    items: I,
                    txn: &str,
                ) -> Option<matrix_sdk::send_queue::SendHandle>
                where
                    I::IntoIter: DoubleEndedIterator,
                {
                    items.into_iter().rev().find_map(|item| {
                        let event = item.as_event()?;
                        if event.transaction_id().map(|t| t.to_string())
                            == Some(txn.to_owned())
                        {
                            event.local_echo_send_handle()
                        } else {
                            None
                        }
                    })
                }
                scan(items, &transaction_id)
            };
            let mut handle = find_handle(&timeline.items().await);
            if handle.is_none() {
                if let Some(thread) = thread_timeline.as_ref() {
                    handle = find_handle(&thread.items().await);
                }
            }
            let category = match handle {
                None => Some("cancel_target_missing"),
                Some(handle) => match handle.abort().await {
                    Ok(true) => None,
                    // Already on the server; do not report it as cancelled.
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

    /// Manual "Retry decryption" for the open room and thread: collect the
    /// session ids of visible undecryptable items and ask the SDK to retry
    /// them. One bounded pass per action, no polling. Since the client uses
    /// the OneShot strategy (no per-UTD download task), each visible UTD
    /// session first gets one deduplicated `download_room_key` attempt.
    /// Session identifiers only; never key material.
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
            // Bounded per-session downloads; `download_room_key` returns Ok(false)
            // without a usable backup.
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

/// Budget for each half of the shrink wait (join and poll), so the helper
/// gives up after ~1.2 s and reopens without trimming.
const SHRINK_WAIT_BUDGET_MS: u64 = 600;
/// Shrink poll step. Coarse because `RoomEventCache::events()` clones
/// every event.
const SHRINK_POLL_STEP_MS: u64 = 60;

/// Wait for the event cache to release the paginated backlog for
/// `room_ref`. Returns the event count beforehand and whether the release
/// was observed.
///
///   1. Await the previous timeline task: until it stops, its `Timeline`
///      keeps a cache subscriber alive. `Err(JoinError::Cancelled)` is
///      success here.
///   2. Poll the cache's event count; the SDK's auto-shrink runs on its own
///      task, so there is nothing to await. Bounded, so a missing shrink
///      degrades to "reopened without trimming".
///
/// Never calls `RoomEventCache::clear()`, which would also wipe persisted
/// events.
async fn await_event_cache_shrink(
    room: &matrix_sdk::Room,
    previous_task: Option<tokio::task::JoinHandle<()>>,
) -> Option<(usize, bool)> {
    // Take the baseline while the old subscriber still holds the backlog; a
    // fast shrink could otherwise land first and be reported as no shrink.
    // The returned drop handles are dropped; the client owns the listeners.
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let before = cache.events().await.ok()?.len();
    if before == 0 {
        return None;
    }
    if let Some(task) = previous_task {
        // Ok or Err(Cancelled) both mean the Arc is released. Bounded: a task slow
        // to cancel must degrade to reopening without trimming.
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(SHRINK_WAIT_BUDGET_MS),
            task,
        )
        .await;
    }
    // Check once before sleeping; the release may already have landed.
    if let Ok(events) = cache.events().await {
        if events.len() < before {
            return Some((before, true));
        }
    }
    // Normally immediate; bounded for the pathological case. Poll sparingly:
    // `events()` clones every event.
    let steps = SHRINK_WAIT_BUDGET_MS / SHRINK_POLL_STEP_MS;
    for _ in 0..steps {
        tokio::time::sleep(std::time::Duration::from_millis(SHRINK_POLL_STEP_MS))
            .await;
        let Ok(events) = cache.events().await else { return Some((before, false)) };
        if events.len() < before {
            return Some((before, true));
        }
    }
    // Timed out: report not shrunk, distinct from a successful release.
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

    // Release the backlog before rebuilding (jump-to-live only);
    // `trimmed_from` in the reset reports whether it actually landed.
    let trim_report: Option<(usize, bool)> = if shrink_first {
        await_event_cache_shrink(&room, previous_task).await
    } else {
        None
    };

    // Read-receipt tracking for message-like events only, so state rows never
    // carry receipts. This also enables the SDK's ReadMarker row; its
    // presentation policy lives in QML (MessageDelegate).
    //
    // Thread timelines keep tracking disabled: the SDK's receipt handling is
    // not thread-aware and would attach unthreaded receipts to thread rows.
    //
    // hide_threaded_events keeps thread replies out of the live timeline (they
    // belong in the thread panel), classified by the SDK's m.thread relation.
    // Thread roots stay in the main timeline.
    let timeline = match TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
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

    // Atomic snapshot plus subscription: no event falls between `items` and
    // the first diff.
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

    // Opt-in send-state reconciliation (LIGHTNING_SEND_TRACE) for echoes stuck
    // at "sending…". matrix-sdk-ui's `room_send_queue_update_task` ignores a
    // lagged broadcast without resyncing, so a lost terminal update leaves an
    // item NotSentYet until the timeline is rebuilt. `SendQueue::local_echoes()`
    // reads what the queue still owes from the store, independently:
    //     queued non-empty -> the send really is outstanding.
    //     queued empty     -> the terminal update was lost.
    //
    // Counts always; transaction ids only when something looks orphaned; never
    // a body. Stops within one tick of the room generation changing.
    if std::env::var_os("LIGHTNING_SEND_TRACE").is_some() {
        let watch_timeline = Arc::clone(&timeline);
        let watch_client = client.clone();
        let watch_registry = Arc::clone(&registry);
        let watch_room = room_id.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                if !watch_registry.is_current(room_gen, lifecycle) {
                    break;
                }
                let mut in_flight: Vec<String> = Vec::new();
                for item in watch_timeline.items().await.iter() {
                    let Some(event) = item.as_event() else { continue };
                    if matches!(
                        event.send_state(),
                        Some(EventSendState::NotSentYet { .. })
                            | Some(EventSendState::SendingFailed { .. })
                    ) {
                        in_flight.push(match event.identifier() {
                            TimelineEventItemId::TransactionId(id) => id.to_string(),
                            TimelineEventItemId::EventId(id) => id.to_string(),
                        });
                    }
                }
                if in_flight.is_empty() {
                    continue;
                }
                let queued: Vec<String> = match watch_client.send_queue().local_echoes().await {
                    Ok(rooms) => rooms
                        .iter()
                        .filter(|(room, _)| room.as_str() == watch_room)
                        .flat_map(|(_, echoes)| {
                            echoes.iter().map(|echo| echo.transaction_id.to_string())
                        })
                        .collect(),
                    Err(err) => {
                        // Report the failure; silence would read as "nothing in flight".
                        eprintln!(
                            "lightning.send_trace: room={watch_room} \
queue read FAILED ({err}) — this tick says nothing either way"
                        );
                        continue;
                    }
                };
                let orphaned = in_flight.iter().filter(|id| !queued.contains(id)).count();
                // eprintln!, not tracing: `tracing` is not a direct dependency. Gated, and
                // read alongside the SDK's own stderr tracing.
                eprintln!(
                    "lightning.send_trace: room={} in_flight={} queued={} \
orphaned={}{}",
                    watch_room,
                    in_flight.len(),
                    queued.len(),
                    orphaned,
                    if orphaned > 0 {
                        // An observation, not a diagnosis: `local_echoes()` also returns empty on a
                        // store read failure. Ids are printed so the case can be chased.
                        format!(
                            "  <-- the timeline calls these in flight and the \
send queue owes nothing for them: {in_flight:?}"
                        )
                    } else {
                        String::new()
                    }
                );
            }
        });
    }

    // Replies whose target the SDK has not resolved: ask once per timeline
    // (see fetch_missing_reply_details).
    let reply_details_fetched: Arc<Mutex<HashSet<String>>> =
        Arc::new(Mutex::new(HashSet::new()));
    fetch_missing_reply_details(&timeline, items.iter(), &reply_details_fetched);

    // Undecryptable history this room opened with: covers keys that reached
    // the backup after the room's one whole-room pass.
    recover_keys_for_utds(
        &registry, &client, &room_id, &timeline,
        utd_sessions_in(items.iter()), RecoveryScope::Room(room_gen),
        lifecycle,
    );

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
            // Counts only: the pre-trim event count and whether the release was
            // observed (absent for an ordinary open).
            "trimmed_from": trim_report.map(|(before, _)| before),
            "trim_shrunk": trim_report.map(|(_, shrunk)| shrunk),
        }),
    );

    // One deduplicated backup key-download pass for this room, concurrent with
    // diff forwarding; imported keys return as in-place Set diffs.
    {
        let registry = Arc::clone(&registry);
        let client = client.clone();
        let room = room_id.clone();
        tokio::spawn(async move {
            registry.download_backup_keys_for_room(&client, &room).await;
        });
    }

    // Hydrate sender profiles. With lazy-loaded members the SDK only knows
    // senders whose member events synced; `Timeline::fetch_members` fills the
    // rest in place (as Set diffs). Runs concurrently with diff forwarding and
    // dies with this task.
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
                    let changed = diff_items(&diff);
                    // Newly arrived replies with an unresolved target get one fetch; the
                    // result returns as a Set diff.
                    fetch_missing_reply_details(
                        &timeline, changed.iter(), &reply_details_fetched,
                    );
                    // Newly arrived undecryptable events get a bounded key attempt; success
                    // returns as a Set diff and the row decrypts in place.
                    recover_keys_for_utds(
                        &registry, &client, &room_id, &timeline,
                        utd_sessions_in(changed.iter()),
                        RecoveryScope::Room(room_gen), lifecycle,
                    );
                    let value = diff_to_json(
                        &room_id, room_gen, lifecycle, &diff, &own_user, &registry,
                    );
                    enqueue(&events, value);
                }
            }
        }
    }
}

/// Build a `thread_pagination` envelope, shared by `open_thread_task`'s
/// auto-load and `paginate_thread_back` so both stamp the same way.
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

/// Build the thread-focused timeline and forward its snapshot and diffs,
/// like `open_room_task`. Payloads carry the thread generation so stale
/// events cannot reach a newer panel.
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
        .event_filter(lightning_event_filter)
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

    // As for the room timeline: resolve reply targets.
    let reply_details_fetched: Arc<Mutex<HashSet<String>>> =
        Arc::new(Mutex::new(HashSet::new()));
    fetch_missing_reply_details(&timeline, items.iter(), &reply_details_fetched);

    // Undecryptable replies get one bounded key attempt, scoped to the thread
    // generation.
    recover_keys_for_utds(
        &registry, &client, &room_id, &timeline,
        utd_sessions_in(items.iter()), RecoveryScope::Thread(thread_gen),
        lifecycle,
    );

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

    // Auto-load cold-cache threads: subscribe() returns only what the event
    // cache holds, and replies that did not arrive via live sync (history,
    // back-pagination) are not placed in a thread, so the panel would open
    // empty. One backward pagination fetches the thread over /relations.
    // Single-flight and reached-start guarded.
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

    // Same member hydration as the room timeline (the SDK deduplicates the
    // /members request).
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
                    let changed = diff_items(&diff);
                    fetch_missing_reply_details(
                        &timeline, changed.iter(), &reply_details_fetched,
                    );
                    recover_keys_for_utds(
                        &registry, &client, &room_id, &timeline,
                        utd_sessions_in(changed.iter()),
                        RecoveryScope::Thread(thread_gen), lifecycle,
                    );
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

/// Build the room's ThreadListService, fetch the first page, and forward a
/// bounded snapshot on every update batch.
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

    // First page (/threads). A failure still emits a snapshot so the UI leaves
    // its loading state.
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

/// Serialize the service's current page-bounded items. Presentation-safe
/// fields only.
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

/// A thread timeline built just for one send when the panel is not open,
/// so the SDK attaches the m.thread relation and encrypts.
async fn build_transient_thread_timeline(
    client: &Client,
    room_id: &str,
    root_event_id: &str,
) -> Option<Arc<Timeline>> {
    let room_ref = RoomId::parse(room_id).ok()?;
    let room = client.get_room(&room_ref)?;
    let root_ref = EventId::parse(root_event_id).ok()?;
    TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
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

/// Every item a diff carries, so reply-detail fetching looks only at new
/// items.
fn diff_items(diff: &VectorDiff<Arc<TimelineItem>>) -> Vec<Arc<TimelineItem>> {
    match diff {
        VectorDiff::Append { values }
        | VectorDiff::Reset { values } => values.iter().cloned().collect(),
        VectorDiff::PushFront { value }
        | VectorDiff::PushBack { value }
        | VectorDiff::Insert { value, .. }
        | VectorDiff::Set { value, .. } => vec![Arc::clone(value)],
        VectorDiff::Clear
        | VectorDiff::PopFront
        | VectorDiff::PopBack
        | VectorDiff::Remove { .. }
        | VectorDiff::Truncate { .. } => Vec::new(),
    }
}

/// Serialize one `VectorDiff` into the FFI envelope. Every SDK variant is
/// covered; no fallback arm silently drops one.
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
    // LIGHTNING_SYNC_TRACE: stamp when this diff left the SDK side. Wall-clock
    // ms (Instant and Qt timers share no origin). One atomic load when off.
    if let Some(stamp) = crate::sync_trace_stamp_ms() {
        base["trace_sdk_ms"] = stamp.into();
    }
    fill_diff_json(base, diff, own_user, registry)
}

/// Fill a diff envelope's `op`/`items`/`index`. The base carries the stream
/// identity (room vs thread).
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

/// Max reply-detail fetches started per batch; the rest follow in later
/// batches.
///
/// `InReplyToDetails::event` starts `Unavailable` and the SDK fills it only
/// via `Timeline::fetch_details_for_event`, so without asking, a quote reads
/// "(original message not loaded)" unless the server bundled the target.
/// The SDK returns early for `Ready`/`Pending`, checks its cache first, and
/// writes the result back as a Set diff. The `fetched` set only avoids
/// repeated write locks.
const REPLY_DETAIL_FETCH_BURST: usize = 8;

fn fetch_missing_reply_details<'a, I>(
    timeline: &Arc<Timeline>,
    items: I,
    fetched: &Arc<Mutex<HashSet<String>>>,
) where
    I: IntoIterator<Item = &'a Arc<TimelineItem>>,
{
    let mut wanted: Vec<OwnedEventId> = Vec::new();
    for item in items {
        let TimelineItemKind::Event(event) = item.kind() else { continue };
        let TimelineItemContent::MsgLike(msg_like) = event.content() else { continue };
        let Some(reply) = &msg_like.in_reply_to else { continue };
        // Only unresolved ones. `Pending` is in flight, and `Error` must not be
        // retried in a loop (e.g. a redacted or invisible target).
        if !matches!(reply.event, TimelineDetails::Unavailable) {
            continue;
        }
        let Some(event_id) = event.event_id() else { continue };
        let key = event_id.to_string();
        match fetched.lock() {
            Ok(mut guard) => {
                if !guard.insert(key) {
                    continue;
                }
            }
            Err(_) => continue,
        }
        wanted.push(event_id.to_owned());
        if wanted.len() >= REPLY_DETAIL_FETCH_BURST {
            break;
        }
    }
    if wanted.is_empty() {
        return;
    }
    let timeline = Arc::clone(timeline);
    tokio::spawn(async move {
        for event_id in wanted {
            // Errors ignored: `EventNotInTimeline` is normal for an item that scrolled
            // away, and a refused target leaves the quote as it was.
            let _ = timeline.fetch_details_for_event(&event_id).await;
        }
    });
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

/// Max receipt entries per event across the FFI. The UI shows 4 chips and
/// a "+N" total, so a bounded newest-first window plus the count suffices.
const READ_BY_CAP: usize = 16;

/// Serialize an item's receipts as (`[{"user_id", "ts"}]`, total_count):
/// public user ids and timestamps only, newest first, capped at
/// [`READ_BY_CAP`], with the uncapped total for the "+N" chip. The SDK
/// attaches each receipt to the latest item it applies to, so a moving
/// receipt arrives as two Set diffs. Thread timelines disable receipt
/// tracking (see the live builder).
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
    // Newest first; missing timestamps last (stable sort).
    entries.sort_by(|a, b| b.0.unwrap_or(0).cmp(&a.0.unwrap_or(0)));
    entries.truncate(READ_BY_CAP);
    let serialized = entries
        .into_iter()
        .map(|(ts, user_id)| json!({ "user_id": user_id, "ts": ts }))
        .collect();
    (serialized, total)
}

/// Max reactor ids per reaction bucket across the FFI; the tooltip names a
/// few and says "and N more" from the uncapped `count`.
const REACTION_SENDER_CAP: usize = 16;

/// Reactor ids longer than this are dropped rather than forwarded.
const REACTION_SENDER_MAX_BYTES: usize = 255;

/// Serialize one reaction bucket's reactors as a bounded id list. The local
/// user comes first so the tooltip can say "You and …"; the rest keep the
/// SDK's insertion order. Only user ids cross.
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

/// Bound on display names in a profile-change row, in chars (byte slicing
/// could split a code point).
const PROFILE_NAME_CAP: usize = 255;

fn bound_profile_name(name: &str) -> String {
    name.chars().take(PROFILE_NAME_CAP).collect()
}

/// Classify an `m.room.member` display-name change as (`kind`, bounded old,
/// bounded new) for C++ to phrase; no English is built here.
///
/// Empty counts as absent, and old == new is no change (the SDK emits a
/// profile-change item when only the avatar moved).
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

// Activity-row wording for an `OtherState` item, separate so it can be
// tested. Interpolates only the state type and sender, never content: the
// tombstone row carries neither the successor id nor the free-text body.
fn state_row_text(kind: &str, actor: &str) -> String {
    match kind {
        "m.room.create" => format!("{actor} created the room."),
        "m.room.name" => format!("{actor} changed the room name."),
        "m.room.topic" => format!("{actor} changed the room topic."),
        "m.room.avatar" => format!("{actor} changed the room avatar."),
        "m.room.encryption" => "Encryption was enabled.".to_owned(),
        "m.room.tombstone" => format!("{actor} upgraded this room."),
        // Calls must not fall through to "updated room settings". Real call rows
        // arrive as `msgtype: "call"`; these arms cover only state-typed `m.call*`
        // events arriving as OtherState.
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
        // SDK-computed ambiguity: two members share this display name. QML adds an
        // MXID disambiguator; members are never merged by name.
        if profile.display_name_ambiguous {
            out["sender_name_ambiguous"] = true.into();
        }
        if let Some(avatar) = &profile.avatar_url {
            out["sender_avatar_url"] = avatar.to_string().into();
        }
    }

    // Per-message read receipts, including our own (exclusion happens once, in
    // TimelineModel). read_by is a bounded window; read_by_total the full count.
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
                // Media upload progress, parked by matrix-sdk-ui on the local echo's send
                // state, so it arrives as an ordinary diff. Only sent when the total is
                // known; text sends carry none, so the UI shows a spinner rather than a
                // synthesized bar.
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
                // Coarse category only; raw errors may embed server or crypto detail.
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
                            // The uncapped total; `senders` is a bounded window.
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
                    // ReplyToSenderRole is a display name: resolve it like every other sender
                    // here, falling back to the id when the profile is not ready.
                    out["reply_to_sender_id"] = embedded.sender.to_string().into();
                    let mut reply_sender = embedded.sender.to_string();
                    if let TimelineDetails::Ready(profile) = &embedded.sender_profile {
                        if let Some(name) = &profile.display_name {
                            if !name.is_empty() {
                                reply_sender = name.clone();
                            }
                        }
                    }
                    out["reply_to_sender"] = reply_sender.into();
                    out["reply_to_preview"] = reply_preview(&embedded.content).into();
                    // What the target is, so the quote can say "Image" or "2 images" (the
                    // thread_latest_kind vocabulary).
                    let (reply_kind, reply_count) = content_kind(&embedded.content);
                    out["reply_to_kind"] = reply_kind.into();
                    if reply_count > 1 {
                        out["reply_to_count"] = reply_count.into();
                    }
                    // Reply-to-image quotes show a thumbnail: register the embedded media under
                    // the target's event id, like a row, and cross only the key. Images only.
                    if let TimelineItemContent::MsgLike(embedded_kind) =
                        &embedded.content
                    {
                        if let MsgLikeKind::Message(message) =
                            &embedded_kind.kind
                        {
                            // A gallery's thumbnail is its primary picture, under its row's key.
                            if matches!(message.msgtype(),
                                        MessageType::Image(_))
                                || is_gallery_msgtype(
                                    message.msgtype().msgtype())
                            {
                                let reply_key = reply.event_id.to_string();
                                let mut scratch = serde_json::json!({});
                                let primary = fill_message_media(
                                    &mut scratch, message.msgtype(),
                                    &reply_key)
                                    .into_iter()
                                    .next();
                                if let Some((key, media)) = primary {
                                    if scratch["msgtype"] == "image" {
                                        out["reply_to_media_key"] =
                                            key.clone().into();
                                        registry.remember_media(key, media);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if let Some(root) = &msg_like.thread_root {
                out["thread_root_id"] = root.to_string().into();
            }
            // Thread summary on thread roots: reply count and latest reply from the
            // server's bundled aggregation. Safe presentation fields only.
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
                    // Latest-reply sender profile from the bundled aggregation; C++ falls back
                    // to the MXID when absent.
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
                        // Conservative unread hint: read only when our threaded receipt points at
                        // the latest reply, or we sent it.
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
                    // Mentions from the event's m.mentions, never substring matching.
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
                    // matrix-sdk-ui sanitizes HTML with `HtmlSanitizerMode::Compat` (a const),
                    // which strips MSC2545's `data-mx-emoticon`, so the raw formatted body is
                    // restored below. Safe: MessageHtml::sanitize is the boundary for
                    // untrusted HTML, stricter than Compat except for that one attribute, and
                    // it rebuilds `<img>` from validated parts.
                    //
                    // Retrieval key: the event id once remote, the SDK unique id for a local
                    // echo.
                    let key: String = match out["event_id"].as_str() {
                        Some(event_id) if !event_id.is_empty() => event_id.to_owned(),
                        _ => unique_id.to_owned(),
                    };
                    let sources = fill_message_media(&mut out, message.msgtype(), &key);
                    // Primary first: the row's media fields describe it; other gallery items
                    // go through `gallery_items`.
                    if let Some((primary_key, primary)) = sources.first() {
                        out["media_key"] = primary_key.clone().into();
                        out["media_source_available"] = true.into();
                        out["media_thumb_available"] =
                            primary.thumbnail.is_some().into();
                    }
                    for (media_key, media) in sources {
                        registry.remember_media(media_key, media);
                    }
                    // After fill_message_content, which sets `formatted_body`.
                    restore_raw_formatted_body(&mut out, event, message.is_edited());
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
                    // Stickers render through the image path; only safe metadata crosses, and
                    // bytes go through the same media bridge as images.
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
                MsgLikeKind::LiveLocation(state) => {
                    // matrix-sdk-ui aggregates every beacon onto this one item, so the row is
                    // the share with its latest point.
                    let latest = state.latest_location();
                    let geo = latest.map(|b| b.geo_uri().to_owned())
                        .unwrap_or_default();
                    let description = state.description();
                    crate::location::fill_location(
                        &mut out,
                        &geo,
                        description.unwrap_or("Live location"),
                        description,
                        Some("m.self"),
                    );
                    out["locationLive"] = true.into();
                    // `is_live()` checks the flag and `ts + timeout`, distinguishing "sharing
                    // now" from "shared earlier and stopped".
                    out["locationLiveActive"] = state.is_live().into();
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
                // The change as a closed set beside the sentence, so the UI can draw a
                // glyph per action without parsing a translated sentence.
                out["membership_change"] = match change.change() {
                    Some(M::Joined) | Some(M::InvitationAccepted) => "joined",
                    Some(M::Left) | Some(M::InvitationRejected) => "left",
                    Some(M::Invited) => "invited",
                    Some(M::Kicked) => "kicked",
                    Some(M::Banned) | Some(M::KickedAndBanned) => "banned",
                    Some(M::Unbanned) => "unbanned",
                    Some(M::InvitationRevoked) => "revoked",
                    // Unknown stays unknown; a wrong glyph is a wrong claim.
                    _ => "",
                }
                .into();
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

            // Typed fields rather than a sentence, so a simultaneous name and avatar
            // change are both reported and C++ can use the resolved actor name.
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
            // Only the fact, never the mxc URI; the avatar is fetched through the
            // member cache.
            out["profile_avatar_changed"] =
                change.avatar_url_change().is_some().into();

            // Empty by contract: the sentence is built in C++ (resolved names,
            // translations).
            out["body"] = "".into();
        }
        TimelineItemContent::OtherState(state) => {
            let kind = state.content().event_type().to_string();
            let actor = event.sender().to_string();
            // Call membership is bookkeeping, suppressed in
            // TimelineModel::stateGroupEntriesFrom. The msgtype stays "state" so the row
            // keeps its place in the SDK's index space; C++ keys on `state_kind`.
            out["msgtype"] = "state".into();
            out["state_kind"] = kind.clone().into();
            out["body"] = state_row_text(&kind, &actor).into();
        }
        TimelineItemContent::FailedToParseMessageLike { .. }
        | TimelineItemContent::FailedToParseState { .. } => {
            out["msgtype"] = "unsupported".into();
            out["body"] = "[unsupported event]".into();
        }
        // ── Call rows are room history, not a room setting ───────────────
        //
        // They have their own msgtype end to end (`call` ->
        // TimelineEvent::CallEvent -> CallEventDelegate.qml), so they break a
        // collapsed state-group run instead of folding into it. Fields are typed;
        // no sender-chosen free text crosses, and the sentence is built in C++.
        TimelineItemContent::CallInvite => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "invite".into();
            // A legacy `m.call.invite` exposes no video intent here, so do not claim
            // video; "started a call" stays true either way.
            out["call_video"] = false.into();
            // Empty by contract, as for a typed profile change.
            out["body"] = "".into();
        }
        TimelineItemContent::RtcNotification { call_intent, declined_by } => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "notification".into();
            out["call_video"] = matches!(call_intent, Some(CallIntent::Video)).into();
            out["body"] = "".into();
            // A count only; who declined is not needed by the row.
            if !declined_by.is_empty() {
                out["call_declined_count"] = declined_by.len().into();
            }
        }
    }

    out
}

/// Forward the `org.matrix.custom.html` formatted body verbatim. It is
/// untrusted HTML; C++ sanitizes it (MessageHtml::sanitize) before it
/// reaches QML. Other formats are ignored so the plain body is used.
fn set_formatted_body(out: &mut serde_json::Value, formatted: Option<&FormattedBody>) {
    if let Some(fb) = formatted {
        if fb.format == MessageFormat::Html {
            out["formatted_body"] = fb.body.clone().into();
        }
    }
}

/// Put the wire `formatted_body` back, when there is one.
///
/// matrix-sdk-ui's Compat sanitizer strips `data-mx-emoticon`, the only
/// marker of an inline custom emoji. Lightning's own sanitizer is the
/// security boundary and runs after this. Only applies when the raw event
/// has an `org.matrix.custom.html` body and a formatted body was already
/// going to be sent; bounded; anything unexpected keeps the SDK's version.
///
/// The raw body must be the one the SDK displays: `original_json()` never
/// changes on edit, so for edited messages the latest edit's content is
/// used (see `raw_displayed_formatted_body`).
fn restore_raw_formatted_body(
    out: &mut serde_json::Value,
    event: &EventTimelineItem,
    is_edited: bool,
) {
    if !out.get("formatted_body").map(|v| v.is_string()).unwrap_or(false) {
        return;
    }
    let parse = |raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>| {
        raw.deserialize_as::<serde_json::Value>().ok()
    };
    let original = event.original_json().and_then(parse);
    let latest_edit = event.latest_edit_json().and_then(parse);
    if let Some(body) = raw_displayed_formatted_body(
        is_edited,
        original.as_ref(),
        latest_edit.as_ref(),
    ) {
        out["formatted_body"] = body.into();
    }
}

/// The wire `formatted_body` of the content the SDK is displaying, or `None`
/// to keep the SDK's own.
///
/// Unedited: the original event's `content`. Edited: the latest
/// replacement's `content["m.new_content"]` (its top-level content is the
/// `* ` fallback). Edited with no edit JSON is a local echo of our own edit,
/// with no wire copy yet, so the SDK's version is kept. Pure over JSON for
/// testing.
fn raw_displayed_formatted_body(
    is_edited: bool,
    original: Option<&serde_json::Value>,
    latest_edit: Option<&serde_json::Value>,
) -> Option<String> {
    /// Longer than any real message, far short of a memory problem.
    const MAX_FORMATTED_BYTES: usize = 64 * 1024;
    let content = if is_edited {
        latest_edit?.get("content")?.get("m.new_content")?
    } else {
        original?.get("content")?
    };
    if content.get("format").and_then(|v| v.as_str()) != Some("org.matrix.custom.html") {
        return None;
    }
    let body = content.get("formatted_body").and_then(|v| v.as_str())?;
    if body.is_empty() || body.len() > MAX_FORMATTED_BYTES {
        return None;
    }
    Some(body.to_owned())
}

/// The name a media row presents: MSC2530's `filename` when present and
/// non-empty, otherwise the body. With a caption, `body` is the caption and
/// `filename` the name (the SDK's `send_attachment`, Element); Sable sends
/// `body: ""` with the name in `filename`.
fn media_display_name(body: &str, filename: Option<&str>) -> String {
    match filename {
        Some(name) if !name.trim().is_empty() => name.to_owned(),
        _ => body.to_owned(),
    }
}

// ── MSC4274 media galleries ─────────────────────────────────────────────
//
// One event with several attachments: `msgtype` is `dm.filament.gallery`
// (unstable, what Sable and matrix-sdk send) or `m.gallery`, `body` is the
// caption, and `itemtypes` holds ordinary attachment contents with
// `msgtype` renamed `itemtype`. Parsed from `MessageType::data()` so a
// typed ruma Gallery (present only via an unrelated feature) and a custom
// msgtype read the same. Each item goes through ruma's typed deserializer,
// so it is as validated as a standalone attachment.

/// The msgtypes that are a gallery.
pub(crate) const GALLERY_MSGTYPES: [&str; 2] = ["dm.filament.gallery", "m.gallery"];

/// Max items rendered per gallery. The event is attacker-authored and each
/// item becomes a registry entry and a tile.
pub(crate) const GALLERY_ITEM_CAP: usize = 32;

pub(crate) fn is_gallery_msgtype(msgtype: &str) -> bool {
    GALLERY_MSGTYPES.contains(&msgtype)
}

/// A parsed gallery. `items` holds only the attachment kinds a gallery may
/// carry, each a real ruma `MessageType`, in the sender's order.
pub(crate) struct Gallery {
    /// The sender's caption, or empty. Never the per-item fallback list.
    pub caption: String,
    /// The caption's `org.matrix.custom.html`, when there is a caption.
    pub formatted_caption: Option<String>,
    pub items: Vec<MessageType>,
}

impl Gallery {
    /// True when every rendered item is a picture.
    pub fn all_images(&self) -> bool {
        self.items.iter().all(|item| matches!(item, MessageType::Image(_)))
    }
}

/// Parse `msgtype` as an MSC4274 gallery, or `None`. Items that are not
/// attachments or fail their kind's deserializer are skipped, not fatal.
pub(crate) fn parse_gallery(msgtype: &MessageType) -> Option<Gallery> {
    if !is_gallery_msgtype(msgtype.msgtype()) {
        return None;
    }
    let data = msgtype.data();
    let raw_items: &[serde_json::Value] = data
        .get("itemtypes")
        .and_then(|v| v.as_array())
        .map(|v| v.as_slice())
        .unwrap_or(&[]);
    let mut items = Vec::new();
    for raw in raw_items {
        if items.len() >= GALLERY_ITEM_CAP {
            break;
        }
        let Some(obj) = raw.as_object() else { continue };
        let Some(itemtype) = obj.get("itemtype").and_then(|v| v.as_str()) else {
            continue;
        };
        if !matches!(itemtype, "m.image" | "m.video" | "m.audio" | "m.file") {
            continue;
        }
        let mut fields = obj.clone();
        fields.remove("itemtype");
        let body = match fields.remove("body") {
            Some(serde_json::Value::String(body)) => body,
            _ => String::new(),
        };
        if let Ok(item) = MessageType::new(itemtype, body, fields) {
            items.push(item);
        }
    }

    let body = msgtype.body();
    let has_caption =
        !body.trim().is_empty() && !gallery_body_is_item_list(body, raw_items);
    let formatted_caption = if has_caption
        && data.get("format").and_then(|v| v.as_str()) == Some("org.matrix.custom.html")
    {
        data.get("formatted_body")
            .and_then(|v| v.as_str())
            .filter(|html| !html.is_empty())
            .map(str::to_owned)
    } else {
        None
    };
    Some(Gallery {
        caption: if has_caption { body.to_owned() } else { String::new() },
        formatted_caption,
        items,
    })
}

/// True when a gallery's `body` is Sable's generated item list, not a
/// caption. Sable writes one `[<filename, else itemtype>: <url, else
/// "file">]` line per item (`buildGalleryContent`); matched exactly, line by
/// line against the items in order, so a real caption using brackets is
/// never swallowed.
fn gallery_body_is_item_list(body: &str, raw_items: &[serde_json::Value]) -> bool {
    if raw_items.is_empty() {
        return false;
    }
    let mut lines = body.split('\n');
    for raw in raw_items {
        let Some(line) = lines.next() else { return false };
        let name = raw
            .get("filename")
            .and_then(|v| v.as_str())
            .or_else(|| raw.get("itemtype").and_then(|v| v.as_str()))
            .unwrap_or("");
        let url = raw.get("url").and_then(|v| v.as_str()).unwrap_or("file");
        if line != format!("[{name}: {url}]") {
            return false;
        }
    }
    lines.next().is_none()
}

/// Registry key for gallery item `index` other than the primary (which uses
/// the row key, so paths addressing a row by event id reach its first
/// picture). A collision needs an event id ending in `#item<n>`, which
/// v3+ ids cannot contain; at worst one row in this room would show
/// another's attachment.
pub(crate) fn gallery_item_key(row_key: &str, index: usize) -> String {
    format!("{row_key}#item{index}")
}

/// The item that stands for the whole row: the first picture, else the
/// first item, so single-attachment surfaces (room list, notifications,
/// reply thumbnail) say something true.
fn gallery_primary_index(gallery: &Gallery) -> usize {
    gallery
        .items
        .iter()
        .position(|item| matches!(item, MessageType::Image(_)))
        .unwrap_or(0)
}

/// Fill a message row's content and return its media sources for the
/// registry, primary first. `row_key` is the row's media key (event id, or
/// the SDK unique id for a local echo).
///
/// A gallery fills the row from its primary item, uses the caption as the
/// body, and lists every item in `gallery_items` with its key; the sources
/// (with content keys) stay in the registry.
fn fill_message_media(
    out: &mut serde_json::Value,
    msgtype: &MessageType,
    row_key: &str,
) -> Vec<(String, StoredMedia)> {
    let Some(gallery) = parse_gallery(msgtype).filter(|g| !g.items.is_empty()) else {
        return fill_message_content(out, msgtype)
            .map(|media| vec![(row_key.to_owned(), media)])
            .unwrap_or_default();
    };
    let primary = gallery_primary_index(&gallery);
    let mut sources: Vec<(String, StoredMedia)> = Vec::with_capacity(gallery.items.len());
    let mut entries: Vec<serde_json::Value> = Vec::with_capacity(gallery.items.len());
    for (index, item) in gallery.items.iter().enumerate() {
        let mut scratch = json!({});
        let Some(media) = fill_message_content(&mut scratch, item) else { continue };
        let key = if index == primary {
            row_key.to_owned()
        } else {
            gallery_item_key(row_key, index)
        };
        let mut entry = json!({
            "media_key": key.clone(),
            "kind": scratch["msgtype"].clone(),
            "filename": scratch["media_filename"].clone(),
            "thumb_available": media.thumbnail.is_some(),
        });
        for (from, to) in [
            ("media_mimetype", "mimetype"),
            ("media_size", "size"),
            ("media_width", "width"),
            ("media_height", "height"),
            ("media_duration_ms", "duration_ms"),
        ] {
            if let Some(value) = scratch.get(from) {
                entry[to] = value.clone();
            }
        }
        entries.push(entry);
        if index == primary {
            fill_message_content(out, item);
            sources.insert(0, (key, media));
        } else {
            sources.push((key, media));
        }
    }
    out["body"] = gallery.caption.clone().into();
    match &gallery.formatted_caption {
        Some(html) => out["formatted_body"] = html.clone().into(),
        None => {
            if let Some(obj) = out.as_object_mut() {
                obj.remove("formatted_body");
            }
        }
    }
    if entries.len() > 1 {
        out["gallery_items"] = entries.into();
    }
    sources
}

/// What a message is, for one-line surfaces (thread card, reply quote): the
/// row kind, the attachment count (0 unless a gallery of two or more), and
/// the text worth quoting (the sender's words, else the attachment name,
/// else nothing; never a gallery's generated list).
pub(crate) struct MessageSummary {
    pub kind: &'static str,
    pub count: usize,
    pub text: String,
}

pub(crate) fn message_summary(msgtype: &MessageType) -> MessageSummary {
    let media = |kind: &'static str, body: &str, filename: Option<&str>| MessageSummary {
        kind,
        count: 0,
        text: if body.trim().is_empty() {
            filename.unwrap_or("").to_owned()
        } else {
            body.to_owned()
        },
    };
    match msgtype {
        MessageType::Text(c) => MessageSummary { kind: "text", count: 0, text: c.body.clone() },
        MessageType::Notice(c) => MessageSummary { kind: "notice", count: 0, text: c.body.clone() },
        MessageType::Emote(c) => MessageSummary { kind: "emote", count: 0, text: c.body.clone() },
        MessageType::Image(c) => {
            let gif = c.info.as_ref().and_then(|info| info.mimetype.as_deref())
                == Some("image/gif");
            media(if gif { "gif" } else { "image" }, &c.body, c.filename.as_deref())
        }
        MessageType::Video(c) => media("video", &c.body, c.filename.as_deref()),
        MessageType::Audio(c) => media("audio", &c.body, c.filename.as_deref()),
        MessageType::File(c) => media("file", &c.body, c.filename.as_deref()),
        other => match parse_gallery(other).filter(|g| !g.items.is_empty()) {
            Some(gallery) if gallery.items.len() == 1 => {
                let mut single = message_summary(&gallery.items[0]);
                if !gallery.caption.is_empty() {
                    single.text = gallery.caption;
                }
                single
            }
            Some(gallery) => MessageSummary {
                // Pictures read as "image"; mixed galleries as "file".
                kind: if gallery.all_images() { "image" } else { "file" },
                count: gallery.items.len(),
                text: gallery.caption,
            },
            None => MessageSummary {
                kind: "text",
                count: 0,
                text: other.body().to_owned(),
            },
        },
    }
}

/// Text / notice / emote / media conversion. Media returns a `StoredMedia`
/// describing how to fetch the bytes through the SDK; encrypted sources stay
/// in Rust. The payload carries only safe metadata (an mxc string for
/// unencrypted media, sizes, dimensions).
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
            let filename = media_display_name(&content.body, content.filename.as_deref());
            out["msgtype"] = "image".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = filename.clone().into();
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
                filename,
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::File(content) => {
            out["msgtype"] = "file".into();
            out["body"] = content.body.clone().into();
            let filename = media_display_name(&content.body, content.filename.as_deref());
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
            // Audio has its own type so the UI can show a compact audio row (duration,
            // MSC3245 voice marker).
            let filename = media_display_name(&content.body, content.filename.as_deref());
            out["msgtype"] = "audio".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = filename.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            if content.voice.is_some() {
                out["media_voice"] = true.into();
            }
            // The real MSC3245 waveform when present, downsampled to 0..=100. The UI
            // never fabricates one.
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
                filename,
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::Video(content) => {
            // Videos reserve their thumbnail geometry and get a typed placeholder.
            let filename = media_display_name(&content.body, content.filename.as_deref());
            out["msgtype"] = "video".into();
            out["body"] = content.body.clone().into();
            out["media_filename"] = filename.clone().into();
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
                filename,
                mimetype,
                declared_size: content.info.as_ref()
                    .and_then(|info| info.size)
                    .map(u64::from),
            })
        }
        MessageType::Location(content) => {
            // The extensible accessors resolve legacy vs MSC3488: `geo_uri()` prefers
            // `location.uri` and falls back to the top-level field.
            crate::location::fill_location(
                out,
                content.geo_uri(),
                &content.body,
                content
                    .location
                    .as_ref()
                    .and_then(|l| l.description.as_deref()),
                Some(content.asset_type().as_str()),
            );
            None
        }
        other => {
            out["msgtype"] = "unsupported".into();
            out["body"] = other.body().to_owned().into();
            None
        }
    }
}

/// Downsample a voice waveform to at most 96 buckets of 0..=100 by
/// averaging the raw MSC3245 amplitudes (0..=`max`).
fn downsample_waveform(raw: &[u64], max: u64) -> Vec<u64> {
    const BUCKETS: usize = 96;
    if raw.is_empty() || max == 0 {
        return Vec::new();
    }
    let buckets = raw.len().min(BUCKETS);
    let mut vals = Vec::with_capacity(buckets);
    for i in 0..buckets {
        let start = i * raw.len() / buckets;
        let end = (((i + 1) * raw.len()) / buckets).max(start + 1);
        let slice = &raw[start..end.min(raw.len())];
        let avg: u64 = slice.iter().sum::<u64>() / slice.len() as u64;
        vals.push(avg.min(max));
    }

    // Scale to the loudest bucket, not a fixed 1024: MSC3245 amplitudes are
    // absolute, so ordinary speech sits low and would render as a flat line
    // under the UI's bar floor. A waveform shows shape, as in Element; a
    // full-range recording is unchanged. The peak is taken after bucketing so
    // the tallest bar is exactly full, and silence stays all zeroes.
    let peak = vals.iter().copied().max().unwrap_or(0);
    if peak == 0 {
        return vec![0; buckets];
    }
    vals.into_iter().map(|v| (v * 100) / peak).collect()
}

/// Reduce matrix.to user links in outgoing markdown to their label for the
/// plain body, the fallback other clients show in room lists and
/// notifications (Element does the same). `formatted_body` keeps the
/// anchor; room/event permalinks and other markdown pass through. Escaped
/// label characters unescape. Best effort on labels with an unescaped `[`;
/// this is display fallback only.
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

/// Apply the mention plain-body reduction to a just-built markdown message;
/// only the plain fallback is rewritten.
fn set_mention_plain_body(msgtype: &mut MessageType, markdown: &str) {
    let plain = mention_plain_body(markdown);
    if plain == markdown {
        return;
    }
    match msgtype {
        MessageType::Text(text) => text.body = plain,
        // /me emotes use the same markdown path and need the same reduction.
        MessageType::Emote(emote) => emote.body = plain,
        _ => {}
    }
}

// ── Formatted sends ──────────────────────────────────────────────────────
//
// Besides markdown (converted by the SDK), the composer sends:
//
//   * "plain": the body verbatim with no markdown parsing (e.g. /shrug's
//     ¯\_(ツ)_/¯);
//   * "html": C++ supplies both plain and formatted bodies, generated from
//     one QTextDocument (rich composer, /spoiler).
//
// The spec is one optional JSON argument shared by send, reply, thread and
// edit. Empty means markdown.
//
// Security: outgoing HTML is sanitized here with ruma's strict
// Matrix-subset sanitizer, so no caller can put script, event handlers or
// unsafe URLs into a formatted_body even if the C++ serializer regresses.

/// How an outgoing text body is interpreted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BodyFormat {
    Markdown,
    Plain,
    Html,
}

/// Parsed form of the FFI `body_spec` argument.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct SendBodySpec {
    pub(crate) format: BodyFormat,
    /// Matrix-subset HTML; only meaningful (and required) for `Html`.
    pub(crate) html: String,
    /// true = m.emote (the /me lane), false = m.text.
    pub(crate) emote: bool,
    /// MSC2545 inline custom emoji: `:shortcode:` -> `mxc://…` for shortcodes
    /// the composer recognised. Substituted here because the default path is
    /// markdown, and C++ building `<img>` would force `format: html`.
    pub(crate) emoticons: Vec<(String, String)>,
}

impl Default for SendBodySpec {
    fn default() -> Self {
        Self {
            format: BodyFormat::Markdown,
            html: String::new(),
            emote: false,
            emoticons: Vec::new(),
        }
    }
}

/// Parse `{"format":"markdown"|"plain"|"html","html":"…","msgtype":"text"|"emote"}`.
/// Empty input is the default markdown/text spec. Unknown values are
/// refused, not defaulted.
pub(crate) fn parse_body_spec(spec: &str) -> Result<SendBodySpec, String> {
    if spec.trim().is_empty() {
        return Ok(SendBodySpec::default());
    }
    let value: serde_json::Value =
        serde_json::from_str(spec).map_err(|_| "Invalid body spec.".to_owned())?;
    let format = match value["format"].as_str().unwrap_or("markdown") {
        "markdown" => BodyFormat::Markdown,
        "plain" => BodyFormat::Plain,
        "html" => BodyFormat::Html,
        _ => return Err("Unknown body format.".to_owned()),
    };
    let html = value["html"].as_str().unwrap_or_default().to_owned();
    if format == BodyFormat::Html && html.trim().is_empty() {
        return Err("HTML body spec without HTML content.".to_owned());
    }
    if format != BodyFormat::Html && !html.is_empty() {
        return Err("HTML content outside the html format.".to_owned());
    }
    let emote = match value["msgtype"].as_str().unwrap_or("text") {
        "text" => false,
        "emote" => true,
        _ => return Err("Unknown body msgtype.".to_owned()),
    };
    // ":code:" -> "mxc://…". Non-mxc values are dropped, not refused: one bad
    // pack entry must not block the message.
    let mut emoticons: Vec<(String, String)> = Vec::new();
    if let Some(map) = value.get("emoticons").and_then(|v| v.as_object()) {
        for (code, target) in map {
            let Some(mxc) = target.as_str() else { continue };
            if !mxc.starts_with("mxc://") || mxc.len() <= "mxc://".len() {
                continue;
            }
            if code.len() < 3 || !code.starts_with(':') || !code.ends_with(':') {
                continue;
            }
            emoticons.push((code.clone(), mxc.to_owned()));
        }
        // Longest first, so `:blob_wave:` is not eaten by `:blob:`.
        emoticons.sort_by(|a, b| b.0.len().cmp(&a.0.len()));
    }
    Ok(SendBodySpec { format, html, emote, emoticons })
}

/// Replace `:shortcode:` runs with MSC2545 emoticon images in the HTML that
/// markdown produced (substituting in the source would get escaped).
///
/// Not inside a tag (attributes contain colons), not inside `<code>`/`<pre>`,
/// and not inside a URL run (`http://host/:foo:`). The plain body keeps the
/// shortcodes, which is the fallback MSC2545 asks for.
fn substitute_emoticons(html: &str, emoticons: &[(String, String)]) -> String {
    if emoticons.is_empty() {
        return html.to_owned();
    }
    let mut out = String::with_capacity(html.len());
    let bytes = html.as_bytes();
    let mut i = 0usize;
    // Depth of code/pre nesting; shortcodes inside are left alone.
    let mut literal_depth = 0usize;
    while i < html.len() {
        if bytes[i] == b'<' {
            let Some(close) = html[i..].find('>') else {
                out.push_str(&html[i..]);
                break;
            };
            let tag = &html[i..i + close + 1];
            let lower = tag.to_ascii_lowercase();
            if lower.starts_with("<code") || lower.starts_with("<pre") {
                literal_depth += 1;
            } else if lower.starts_with("</code") || lower.starts_with("</pre") {
                literal_depth = literal_depth.saturating_sub(1);
            }
            out.push_str(tag);
            i += close + 1;
            continue;
        }
        if literal_depth == 0 && bytes[i] == b':' {
            let mut matched = false;
            for (code, mxc) in emoticons {
                if html[i..].starts_with(code.as_str()) {
                    // Not inside a URL run: look back for a scheme separator with no
                    // whitespace since. Bytes, not chars: 160 bytes back can land inside a
                    // code point, and slicing there panics (silently losing the send). Using
                    // the whole string instead is equivalent, since only the last run is
                    // inspected.
                    let preceding = out
                        .get(out.len().saturating_sub(160)..)
                        .unwrap_or(out.as_str());
                    let in_url = preceding
                        .rsplit(|c: char| c.is_whitespace() || c == '>')
                        .next()
                        .map(|run| run.contains("://"))
                        .unwrap_or(false);
                    if in_url {
                        break;
                    }
                    let label = code.trim_matches(':');
                    out.push_str("<img data-mx-emoticon src=\"");
                    out.push_str(mxc);
                    out.push_str("\" alt=\"");
                    out.push_str(&html_escape_attr(code));
                    out.push_str("\" title=\"");
                    out.push_str(&html_escape_attr(label));
                    out.push_str("\" height=\"32\">");
                    i += code.len();
                    matched = true;
                    break;
                }
            }
            if matched {
                continue;
            }
        }
        // Advance one character, not one byte.
        let step = html[i..].chars().next().map(|c| c.len_utf8()).unwrap_or(1);
        out.push_str(&html[i..i + step]);
        i += step;
    }
    out
}

/// Minimal attribute escaping for values this module builds itself.
fn html_escape_attr(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// Drop every `<img>` whose `src` is not an `mxc:` URI.
///
/// Needed because allowing `data-mx-emoticon` disables ruma's own scheme
/// check: its cleaner returns early at the first attribute with no scheme
/// rules (ruma-html sanitizer_config/clean.rs), and `data-mx-emoticon` is
/// serialized before `src`. So `<img data-mx-emoticon src="https://…">`
/// passed the strict config (see the test below). A remote image in an
/// outgoing message is a tracking pixel. Fails closed: no `src`, a
/// malformed tag, or a non-mxc scheme drops the whole element.
fn strip_non_mxc_images(html: &str) -> String {
    let mut out = String::with_capacity(html.len());
    let mut rest = html;
    loop {
        // Case-insensitive `<img` at a tag boundary.
        let Some(found) = find_img_tag(rest) else {
            out.push_str(rest);
            return out;
        };
        out.push_str(&rest[..found]);
        let tail = &rest[found..];
        // An unterminated tag is malformed; drop everything from here.
        let Some(close) = tail.find('>') else { return out };
        let tag = &tail[..=close];
        if img_src_is_mxc(tag) {
            out.push_str(tag);
        }
        rest = &tail[close + 1..];
    }
}

/// Byte offset of the next `<img` tag, case-insensitive, only where the name
/// ends (so `<image>` does not match).
fn find_img_tag(html: &str) -> Option<usize> {
    let bytes = html.as_bytes();
    let mut i = 0usize;
    while i + 4 <= bytes.len() {
        // `get`, not a length check and slice: slicing across a code point panics.
        if bytes[i] == b'<'
            && html
                .get(i + 1..i + 4)
                .is_some_and(|name| name.eq_ignore_ascii_case("img"))
        {
            let after = bytes.get(i + 4).copied();
            if matches!(after, None | Some(b' ') | Some(b'\t') | Some(b'\n')
                             | Some(b'\r') | Some(b'/') | Some(b'>')) {
                return Some(i);
            }
        }
        i += 1;
    }
    None
}

/// True when the tag carries a `src` whose value begins `mxc://`.
fn img_src_is_mxc(tag: &str) -> bool {
    let lower = tag.to_ascii_lowercase();
    let mut from = 0usize;
    while let Some(at) = lower[from..].find("src") {
        let start = from + at;
        // `src` must be a whole attribute name, not the tail of another.
        let before_ok = start == 0
            || matches!(lower.as_bytes()[start - 1],
                        b' ' | b'\t' | b'\n' | b'\r' | b'<' | b'"' | b'\'');
        let after = lower[start + 3..].trim_start();
        if before_ok && after.starts_with('=') {
            let value = after[1..].trim_start();
            // Terminate the value, or `src="mxc://">` would pass as an mxc URI with no
            // media id.
            let terminated = if let Some(rest) = value.strip_prefix('"') {
                rest.split('"').next().unwrap_or("")
            } else if let Some(rest) = value.strip_prefix('\'') {
                rest.split('\'').next().unwrap_or("")
            } else {
                value
                    .split(|c: char| c.is_whitespace() || c == '>')
                    .next()
                    .unwrap_or("")
            };
            return terminated.starts_with("mxc://")
                && terminated.len() > "mxc://".len();
        }
        from = start + 3;
    }
    false
}

/// Strict-sanitize the formatted half of an outgoing message in place;
/// no-op without a formatted body.
///
/// ruma's Strict config allows `img` with `src` restricted to `mxc`, but not
/// `data-mx-emoticon` (data-mx-* is allowed on `span` only), which is what
/// makes an MSC2545 inline emoji an emoji. So exactly that one attribute is
/// added; nothing else is widened.
fn sanitize_outgoing_formatted(msgtype: &mut MessageType) {
    use matrix_sdk::ruma::html::{
        ElementAttributesSchemes, Html, ListBehavior, PropertiesNames,
        SanitizerConfig,
    };
    let formatted = match msgtype {
        MessageType::Text(text) => text.formatted.as_mut(),
        MessageType::Emote(emote) => emote.formatted.as_mut(),
        _ => None,
    };
    let Some(formatted) = formatted else { return };
    // Pin the scheme rather than inherit it: with the attribute added and the
    // scheme implicit, `<img src="https://tracker.example/pixel.png">` passed
    // the strict config.
    let config = SanitizerConfig::strict()
        .allow_attributes(
            [PropertiesNames { parent: "img", properties: &["data-mx-emoticon"] }],
            ListBehavior::Add,
        )
        .allow_schemes(
            [
                ElementAttributesSchemes {
                    element: "img",
                    attr_schemes: &[PropertiesNames {
                        parent: "src",
                        properties: &["mxc"],
                    }],
                },
                // `a` must be restated: Override replaces the whole scheme list, and an
                // element missing from it is not scheme-checked at all (`javascript:`
                // hrefs would pass). These are the spec's link schemes.
                ElementAttributesSchemes {
                    element: "a",
                    attr_schemes: &[PropertiesNames {
                        parent: "href",
                        properties: &["http", "https", "ftp", "mailto", "magnet"],
                    }],
                },
            ],
            ListBehavior::Override,
        );
    let html = Html::parse(&strip_non_mxc_images(&formatted.body));
    html.sanitize_with(&config);
    formatted.body = html.to_string();
}

/// Put the message's inline custom emoji in. A markdown message without
/// markdown has no formatted body, so one is created from the escaped
/// plain text. The plain body keeps the shortcodes as MSC2545's fallback.
fn apply_emoticons(msgtype: &mut MessageType, spec: &SendBodySpec) {
    if spec.emoticons.is_empty() {
        return;
    }
    let (plain, formatted) = match msgtype {
        MessageType::Text(text) => (text.body.clone(), &mut text.formatted),
        MessageType::Emote(emote) => (emote.body.clone(), &mut emote.formatted),
        _ => return,
    };
    let source = match formatted.as_ref() {
        Some(existing) => existing.body.clone(),
        None => html_escape_attr(&plain),
    };
    let replaced = substitute_emoticons(&source, &spec.emoticons);
    if replaced == source && formatted.is_none() {
        return;   // nothing matched; do not invent a formatted body
    }
    *formatted = Some(FormattedBody::html(replaced));
}

/// Build outgoing message content for one (body, spec) pair. The markdown
/// lane applies the mention plain-body reduction; the html lane uses the C++
/// plain body verbatim and strict-sanitizes the formatted half.
pub(crate) fn composed_content(body: &str, spec: &SendBodySpec) -> RoomMessageEventContent {
    let mut message = match (spec.format, spec.emote) {
        (BodyFormat::Markdown, false) => RoomMessageEventContent::text_markdown(body),
        (BodyFormat::Markdown, true) => RoomMessageEventContent::emote_markdown(body),
        (BodyFormat::Plain, false) => RoomMessageEventContent::text_plain(body),
        (BodyFormat::Plain, true) => RoomMessageEventContent::emote_plain(body),
        (BodyFormat::Html, false) => {
            RoomMessageEventContent::text_html(body, spec.html.clone())
        }
        (BodyFormat::Html, true) => {
            RoomMessageEventContent::emote_html(body, spec.html.clone())
        }
    };
    match spec.format {
        BodyFormat::Markdown => set_mention_plain_body(&mut message.msgtype, body),
        BodyFormat::Plain => {}
        BodyFormat::Html => sanitize_outgoing_formatted(&mut message.msgtype),
    }
    // Emoji go in after the format's own handling and pass through the same
    // mxc-only filter as a hand-supplied `<img>`.
    apply_emoticons(&mut message.msgtype, spec);
    if !spec.emoticons.is_empty() {
        sanitize_outgoing_formatted(&mut message.msgtype);
    }
    message
}

/// The WithoutRelation twin of [`composed_content`], for replies/edits.
pub(crate) fn composed_content_without_relation(
    body: &str,
    spec: &SendBodySpec,
) -> RoomMessageEventContentWithoutRelation {
    let mut message = match (spec.format, spec.emote) {
        (BodyFormat::Markdown, false) => {
            RoomMessageEventContentWithoutRelation::text_markdown(body)
        }
        (BodyFormat::Markdown, true) => {
            RoomMessageEventContentWithoutRelation::emote_markdown(body)
        }
        (BodyFormat::Plain, false) => {
            RoomMessageEventContentWithoutRelation::text_plain(body)
        }
        (BodyFormat::Plain, true) => {
            RoomMessageEventContentWithoutRelation::emote_plain(body)
        }
        (BodyFormat::Html, false) => {
            RoomMessageEventContentWithoutRelation::text_html(body, spec.html.clone())
        }
        (BodyFormat::Html, true) => {
            RoomMessageEventContentWithoutRelation::emote_html(body, spec.html.clone())
        }
    };
    match spec.format {
        BodyFormat::Markdown => set_mention_plain_body(&mut message.msgtype, body),
        BodyFormat::Plain => {}
        BodyFormat::Html => sanitize_outgoing_formatted(&mut message.msgtype),
    }
    // Emoji go in after the format's own handling and pass through the same
    // mxc-only filter as a hand-supplied `<img>`.
    apply_emoticons(&mut message.msgtype, spec);
    if !spec.emoticons.is_empty() {
        sanitize_outgoing_formatted(&mut message.msgtype);
    }
    message
}

/// Build MSC3381 poll-start content with ruma constructors only; pure for
/// testing. Answer ids are opaque and unique within the poll (timestamp +
/// index); ruma enforces at most 20 answers and Lightning requires two. The
/// MSC1767 fallback body lists the question and numbered answers.
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

/// Crate-owned projection of the SDK's aggregated `PollResult`, since
/// `PollResultAnswer` is not re-exported (the serializer would be
/// untestable). Aggregation stays in ruma/the SDK.
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

/// MSC3381 poll payload. For an undisclosed poll that has not ended,
/// per-answer counts are forced to 0; only the user's own selections and the
/// distinct-voter total are forwarded. Other voters' MXIDs never cross.
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

/// Coarse kind of a thread's latest reply ("Image", "GIF", "Encrypted
/// reply", …), from the SDK content type only, never the body.
fn thread_latest_kind(content: &TimelineItemContent) -> &'static str {
    content_kind(content).0
}

/// The coarse kind of an item and, for a gallery, its attachment count (0
/// otherwise). Shared by the thread card and reply quote.
fn content_kind(content: &TimelineItemContent) -> (&'static str, usize) {
    match content {
        TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
            MsgLikeKind::Message(message) => {
                let summary = message_summary(message.msgtype());
                (summary.kind, summary.count)
            }
            MsgLikeKind::Redacted => ("redacted", 0),
            MsgLikeKind::UnableToDecrypt(_) => ("encrypted", 0),
            MsgLikeKind::Sticker(_) => ("sticker", 0),
            MsgLikeKind::Poll(_) => ("poll", 0),
            _ => ("unsupported", 0),
        },
        _ => ("unsupported", 0),
    }
}

/// A room-list-sized preview. 80 characters is right for a 300 px row.
fn content_preview(content: &TimelineItemContent) -> String {
    content_preview_capped(content, 80)
}

/// Preview budget for a reply quote. The quote spans a wide timeline and
/// QML elides to the available width, so this is generous and only bounds
/// pathological bodies. C++ normalizes it and adds a real ellipsis
/// (EventPreview::normalizePreviewText).
fn reply_preview(content: &TimelineItemContent) -> String {
    content_preview_capped(content, 320)
}

fn content_preview_capped(content: &TimelineItemContent, max: usize) -> String {
    let text = match content {
        TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
            // The sender's words, else the attachment name (never a gallery's
            // generated list). Empty is labelled by kind.
            MsgLikeKind::Message(message) => message_summary(message.msgtype()).text,
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
    // Count characters and mark the cut.
    if text.chars().count() > max {
        let mut out: String = text.chars().take(max).collect();
        out.push('\u{2026}');
        out
    } else {
        text
    }
}

/// Megolm session ids of undecryptable items in one batch (the diff that
/// arrived), rather than the whole timeline like `utd_session_ids`.
fn utd_sessions_in<'a>(
    items: impl Iterator<Item = &'a Arc<TimelineItem>>,
) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for item in items {
        let TimelineItemKind::Event(event) = item.kind() else { continue };
        let TimelineItemContent::MsgLike(msg_like) = event.content() else {
            continue;
        };
        let MsgLikeKind::UnableToDecrypt(encrypted) = &msg_like.kind else {
            continue;
        };
        if let EncryptedMessage::MegolmV1AesSha2 { session_id, .. } = encrypted {
            if !out.contains(session_id) {
                out.push(session_id.clone());
            }
        }
    }
    out
}

/// What a failed `download_room_key` established, from the structured
/// error rather than its text.
///
/// Not `classify_room_error`: it treated `unrecognized` and `forbidden`
/// (permanent) as retryable, and it matches substrings of the Display text,
/// which for a transport error includes the URL and its base64 session id,
/// so a session id containing "404" could spend budget.
fn classify_backup_error(err: &matrix_sdk::Error) -> BackupOutcome {
    use matrix_sdk::ruma::api::error::ErrorKind;
    match err.client_api_error_kind() {
        Some(ErrorKind::NotFound) => BackupOutcome::Definitive,
        Some(ErrorKind::Unrecognized) | Some(ErrorKind::Forbidden) => {
            BackupOutcome::PermanentRefusal
        }
        // Anything else, including transport errors with no errcode, taught us
        // nothing: costs a backoff step, never the budget.
        _ => BackupOutcome::Inconclusive,
    }
}

/// Which generation guard a recovery pass answers to.
#[derive(Clone, Copy)]
pub(crate) enum RecoveryScope {
    Room(u64),
    Thread(u64),
}

/// Automatic backup key recovery for undecryptable events as they arrive.
///
/// The SDK's automatic routes are off here: `automatic-room-key-forwarding`
/// is not enabled (no `m.room_key_request` is sent), and
/// `BackupDownloadStrategy::OneShot` installs no UTD handler or download task.
/// OneShot stays, since it downloads every key when a session is verified. This
/// re-runs `download_room_key` per session, then `retry_decryption`.
/// Identifiers only.
///
/// Bounded without a timer: it runs only when an undecryptable event arrives,
/// only for that batch's sessions, and only while `mark_backup_attempt` says
/// a key is due.
fn recover_keys_for_utds(
    registry: &Arc<TimelineRegistry>,
    client: &Client,
    room_id: &str,
    timeline: &Arc<Timeline>,
    sessions: Vec<String>,
    scope: RecoveryScope,
    lifecycle: u64,
) {
    if sessions.is_empty() {
        return;
    }
    let registry = Arc::clone(registry);
    let client = client.clone();
    let timeline = Arc::clone(timeline);
    let room_id = room_id.to_owned();
    tokio::spawn(async move {
        // Room and thread timelines have separate generations; answering to the
        // wrong one lets a late callback mutate the next timeline. The caller
        // names which.
        let current = |registry: &Arc<TimelineRegistry>| match scope {
            RecoveryScope::Room(generation) => {
                registry.is_current(generation, lifecycle)
            }
            RecoveryScope::Thread(generation) => {
                registry.thread_current(generation, lifecycle)
            }
        };
        if !current(&registry) {
            return;
        }
        let emit = |state: &str, count: usize, inconclusive: usize| {
            // Guarded: C++ drops the lifecycle field, so this is the only gate against
            // a previous account's pass reporting into the next.
            if current(&registry) {
                enqueue(
                    &registry.events,
                    json!({
                        "type": "crypto_bootstrap",
                        "kind": "auto_key_recovery",
                        "state": state,
                        "count": count,
                        // How many sessions taught us nothing, so rate-limited ones are not
                        // mistaken for "not in the backup".
                        "inconclusive": inconclusive,
                        "lifecycle": lifecycle,
                    }),
                );
            }
        };
        let backups = client.encryption().backups();
        if !backups.are_enabled().await {
            // No usable backup key: report "could not have tried" rather than
            // silence. Throttled by the backoff only (no outcome is recorded, so the
            // budget is never spent): escalating, then every 32 minutes while
            // undecryptable rows keep arriving. Returns before the per-session mark,
            // so nothing else throttles it.
            if registry.mark_backup_attempt("\u{1f}auto-recovery-no-backup") {
                emit("skipped_no_backup_key", 0, 0);
            }
            return;
        }
        let Ok(room_ref) = RoomId::parse(&room_id) else { return };
        // Bound one pass. A `Reset` diff hands over every item, so a long
        // undecryptable history could otherwise issue hundreds of round trips. The
        // loop walks every session and stops after 32 pushed; later diffs (scroll,
        // pagination, new messages) offer the rest, since backoff skips those just
        // tried. A successful retry does not cascade on its own.
        const MAX_SESSIONS_PER_PASS: usize = 32;
        let mut wanted: Vec<String> = Vec::new();
        for session_id in sessions {
            if wanted.len() >= MAX_SESSIONS_PER_PASS {
                break;
            }
            let key = format!("{room_id}\u{1f}{session_id}");
            if registry.mark_backup_attempt(&key) {
                wanted.push(session_id);
            }
        }
        if wanted.is_empty() {
            return;
        }
        emit("started", wanted.len(), 0);
        let mut downloaded = 0usize;
        let mut inconclusive = 0usize;
        let mut no_decryption_key = 0usize;
        for session_id in &wanted {
            if !current(&registry) {
                return;
            }
            let key = format!("{room_id}\u{1f}{session_id}");
            match backups.download_room_key(&room_ref, session_id).await {
                Ok(true) => {
                    downloaded += 1;
                    registry
                        .record_backup_outcome(&key, BackupOutcome::Definitive);
                }
                // Not "nothing to fetch": this device lacks the private decryption key
                // (`are_enabled()` checks the public key), so no request was sent.
                Ok(false) => {
                    no_decryption_key += 1;
                    registry.record_backup_outcome(
                        &key, BackupOutcome::NoDecryptionKey,
                    );
                }
                Err(err) => {
                    let outcome = classify_backup_error(&err);
                    if outcome == BackupOutcome::Inconclusive {
                        inconclusive += 1;
                    }
                    registry.record_backup_outcome(&key, outcome);
                }
            }
        }
        // The pinned SDK ignores the session ids: `retry_decryption` computes
        // candidates from the whole timeline
        // (matrix-sdk-ui timeline/controller/mod.rs), so this is O(timeline).
        // Harmless, and `wanted` is still passed in case a later SDK honours it.
        if !current(&registry) {
            return;
        }
        timeline.retry_decryption(wanted.iter().cloned()).await;
        if !current(&registry) {
            return;
        }
        // `no_keys_found` must mean exactly that, not an unreachable server.
        emit(
            if downloaded > 0 {
                "ok"
            } else if no_decryption_key > 0 {
                // Ranked above `failed`: it has a remedy (enter the recovery key), and it
                // distinguishes "the backup key was never here" from a failed fetch.
                "no_decryption_key"
            } else if inconclusive > 0 {
                "failed"
            } else {
                "no_keys_found"
            },
            downloaded,
            inconclusive,
        );
    });
}

/// Megolm session ids (identifiers only) of the visible undecryptable items.
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
/// for `Timeline::retry_decryption`. Only session identifiers are kept.
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
    use super::{
        backup_attempt_allowed, backup_attempt_backoff, find_img_tag,
        is_rtc_membership_event, raw_displayed_formatted_body,
        sessions_by_room_from_import, state_row_text, substitute_emoticons,
        TimelineRegistry, MAX_BACKUP_ATTEMPTS,
    };
    use matrix_sdk::ruma::events::AnySyncTimelineEvent;
    use std::collections::{BTreeMap, BTreeSet, VecDeque};

    /// An edited message must render its edited HTML: `original_json()` is
    /// never updated on edit. Real wire shapes: an `m.replace` carries its new
    /// HTML in `m.new_content`, and its top-level content is the `* ` fallback.
    #[test]
    fn an_edited_message_renders_the_edits_html_not_the_originals() {
        let original = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "**old**",
                "format": "org.matrix.custom.html",
                "formatted_body": "<strong>old</strong>",
            },
        });
        let edit = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "* **new**",
                "format": "org.matrix.custom.html",
                "formatted_body": "* <strong>new</strong>",
                "m.new_content": {
                    "msgtype": "m.text",
                    "body": "**new**",
                    "format": "org.matrix.custom.html",
                    "formatted_body": "<strong>new</strong>",
                },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig" },
            },
        });

        // Unedited: the original's wire HTML, as before.
        assert_eq!(
            raw_displayed_formatted_body(false, Some(&original), None).as_deref(),
            Some("<strong>old</strong>"),
        );
        // Edited and echoed: the replacement's m.new_content, never the original
        // or the `* ` fallback.
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), Some(&edit)).as_deref(),
            Some("<strong>new</strong>"),
            "an edited message rendered pre-edit HTML",
        );
        // Our own edit as a local echo: no wire copy yet, keep the SDK's edited
        // HTML.
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), None),
            None,
            "a local-echo edit fell back to the original's HTML",
        );
        // An edit that removed the markdown restores nothing.
        let plain_edit = serde_json::json!({
            "content": {
                "msgtype": "m.text",
                "body": "* plain",
                "m.new_content": { "msgtype": "m.text", "body": "plain" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig" },
            },
        });
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), Some(&plain_edit)),
            None,
        );
        // Defensive: matrix-sdk 0.18 never applies an `m.room.encrypted` edit, but
        // an edit JSON without m.new_content must not be guessed from.
        let opaque = serde_json::json!({ "content": { "algorithm": "m.megolm.v1.aes-sha2" } });
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), Some(&opaque)),
            None
        );
        // The emoticon marker this restore exists for survives an edit too.
        let emoji_edit = serde_json::json!({
            "content": {
                "m.new_content": {
                    "msgtype": "m.text",
                    "body": ":blob:",
                    "format": "org.matrix.custom.html",
                    "formatted_body": "<img data-mx-emoticon src=\"mxc://e.org/blob\" alt=\":blob:\">",
                },
            },
        });
        assert!(
            raw_displayed_formatted_body(true, Some(&original), Some(&emoji_edit))
                .is_some_and(|html| html.contains("data-mx-emoticon"))
        );
    }

    /// The automatic key-recovery backoff schedule, pinned. The backoff takes
    /// the number of attempts already made.
    #[test]
    fn the_backup_retry_schedule_is_the_one_documented() {
        let secs = |n: u32| backup_attempt_backoff(n).as_secs();
        // The wait after the first try is the first entry.
        assert_eq!(secs(0), 30);
        assert_eq!(secs(1), 60);
        assert_eq!(secs(2), 120);
        assert_eq!(secs(3), 240);
        assert_eq!(secs(4), 480);
        assert_eq!(secs(5), 960);
        assert_eq!(secs(6), 1920);
        // Flat at the ceiling, which must be reachable within the budget.
        assert_eq!(secs(7), 1920);
        assert_eq!(secs(50), 1920);
        assert!(MAX_BACKUP_ATTEMPTS > 6,
                "the 32-minute ceiling must be reachable before the budget \
                 runs out, or it is decoration");

        // Monotonic and never zero (zero would be a poll).
        let mut previous = 0;
        for n in 0..10 {
            let current = secs(n);
            assert!(current > 0);
            assert!(current >= previous);
            previous = current;
        }
    }

    /// Every queue-backed send unwedges the room first (source scan).
    ///
    /// Splits on braces as well as semicolons (a block's tail expression has no
    /// `;`) and requires the unwedge in the immediately preceding statement,
    /// so builder chains split by rustfmt still match and a neighbour's unwedge
    /// cannot satisfy another send. Mutation-checked per site: this catches 11
    /// of the 12 `unwedge_send_queue` calls; `retry_send`'s is covered by
    /// `retry_send_re_enables_before_unwedging` (its "send" is
    /// `handle.unwedge()`). `Timeline::redact` is absent because Lightning uses
    /// `Room::redact`, which bypasses the queue.
    #[test]
    fn every_queue_backed_send_unwedges_the_room_first() {
        let source = include_str!("timeline.rs");
        assert!(
            source.contains("fn unwedge_send_queue"),
            "the scan is not reading the file it thinks it is"
        );
        // Collapse statements onto one line so rustfmt-split chains match. Comment
        // lines are dropped first, since this file mentions these names in prose.
        let code: String = source
            .lines()
            .map(|l| {
                let trimmed = l.trim();
                if trimmed.starts_with("//") { "" } else { l }
            })
            .collect::<Vec<_>>()
            .join("\n");
        // Split on braces too: a tail expression has no `;`, and would merge with
        // the next block so a neighbour's unwedge satisfied it.
        let statements: Vec<String> = code
            .split(|c| c == ';' || c == '{' || c == '}')
            .map(|s| s.split_whitespace().collect::<Vec<_>>().join(" "))
            .filter(|s| !s.is_empty())
            .collect();

        const SENDS: [&str; 5] = [
            ".send(",
            ".send_reply(",
            ".send_attachment(",
            ".edit(",
            ".toggle_reaction(",
        ];
        // The unwedge must be its own statement immediately before the send.
        let mut sends = 0usize;
        for (i, stmt) in statements.iter().enumerate() {
            let is_send = SENDS.iter().any(|n| stmt.contains(n))
                && stmt.contains("timeline")
                && !stmt.contains("fn ")
                && !stmt.contains("const SENDS");
            if !is_send {
                continue;
            }
            sends += 1;
            // Only the immediately preceding statement; a wider window lets one send's
            // unwedge satisfy its neighbour's.
            let guarded = statements[i.saturating_sub(1)..=i]
                .iter()
                .any(|s| s.contains("unwedge_send_queue("));
            assert!(
                guarded,
                "queue-backed send #{sends} is not preceded by \
                 unwedge_send_queue — a send-only failure would leave that \
                 room unable to send for the life of the process:\n  {}",
                stmt.chars().take(140).collect::<String>()
            );
        }
        // Exact, not a floor, so a removed site or a blind scan both fail. It
        // cannot catch a send on a receiver not named `timeline`; new sends still
        // need reading.
        assert_eq!(
            sends, 11,
            "expected 11 queue-backed sends in timeline.rs; found {sends}. \
             If a send was added or removed, update this number AND confirm \
             every site is still guarded."
        );
    }

    /// A thread edit must check the thread generation: `thread_timeline_for`
    /// returns `thread_gen`, while `is_current` compares with `room_gen`, so
    /// passing a thread generation there can never match and the failure report
    /// becomes dead code.
    ///
    /// A source scan (no live SDK timeline here): inside `edit`, the staleness
    /// test must branch on `in_thread` and reach `thread_current`.
    #[test]
    fn a_thread_edit_checks_the_thread_generation() {
        let source = include_str!("timeline.rs");
        let after = source
            .split("pub fn edit(")
            .nth(1)
            .expect("edit() not found — the scan is reading the wrong file");
        // Bound the window and prove it: if the marker moved, `split().next()`
        // returns the whole remainder, full of other `thread_current` calls.
        let bound = "\n    /// Toggle a reaction";
        assert!(
            after.contains(bound),
            "edit() is no longer followed by toggle_reaction — re-bound this \
             scan before trusting it"
        );
        let body = after.split(bound).next().unwrap();

        // Isolate each arm: two independent `contains` checks would survive
        // swapping the arms, which restores the defect.
        let (then_arm, else_arm) = {
            let head = "let current = if in_thread {";
            let split = "} else {";
            let rest = body
                .split(head)
                .nth(1)
                .expect("edit() no longer branches its staleness test by lane");
            let mut halves = rest.split(split);
            let then_arm = halves
                .next()
                .expect("the in_thread arm is empty");
            let else_arm = halves
                .next()
                .expect("edit()'s staleness test has no else arm")
                .split("};")
                .next()
                .expect("the else arm is unterminated");
            (then_arm, else_arm)
        };
        assert!(
            then_arm.contains("registry.thread_current(timeline_gen, lifecycle)"),
            "the THREAD arm does not check the thread generation, so a \
             rejected thread edit reports nothing: {then_arm:?}"
        );
        assert!(
            !then_arm.contains("registry.is_current("),
            "the THREAD arm checks the ROOM counter — this IS the original \
             defect: {then_arm:?}"
        );
        assert!(
            else_arm.contains("registry.is_current(timeline_gen, lifecycle)"),
            "the ROOM arm does not check the room generation: {else_arm:?}"
        );
        assert!(
            !else_arm.contains("registry.thread_current("),
            "the ROOM arm checks the thread counter: {else_arm:?}"
        );
        // The binding must not be named room_gen while holding a thread
        // generation.
        assert!(
            !body.contains("let Some((timeline, room_gen, lifecycle))"),
            "edit() binds the resolved generation as `room_gen`, which is a \
             thread generation in the thread branch"
        );
    }

    /// `retry_send`'s re-enable, which the scan above cannot see (its "send" is
    /// `handle.unwedge()`). A failed send is what disabled the queue, so
    /// without the re-enable `unwedge()` wakes a loop that parks again.
    #[test]
    fn retry_send_re_enables_before_unwedging() {
        let source = include_str!("timeline.rs");
        // Bounded at the next item, or the slice would reach this test's own doc
        // comments, which quote `handle.unwedge()`.
        let body = source
            .split("pub fn retry_send(")
            .nth(1)
            .expect("retry_send not found — the scan is reading the wrong file")
            .split("\n    pub fn ")
            .next()
            .expect("retry_send has no following item to bound it");
        let unwedge = body
            .find("unwedge_send_queue(")
            .expect("retry_send no longer re-enables the queue; handle.unwedge() alone parks");
        let call = body
            .find("handle.unwedge()")
            .expect("retry_send no longer calls unwedge()");
        assert!(
            unwedge < call,
            "retry_send re-enables the queue AFTER unwedge(), so the wake is lost"
        );
    }

    use std::sync::{Arc, Mutex};

    // One reaction toggle per (room, event, key) may be in flight; the rest
    // are dropped, and the slot is reusable once the first finishes.

    // Both of these panicked on a multi-byte code point, silently losing the
    // send.
    #[test]
    fn an_img_scan_does_not_panic_across_a_code_point() {
        // The three bytes after `<` span a code-point boundary: `a` plus the first
        // two bytes of a three-byte character.
        assert_eq!(find_img_tag("<a\u{65e5}b"), None);
        assert_eq!(find_img_tag("x<img src=y>"), Some(1));
        assert_eq!(find_img_tag("<image>"), None);
        assert_eq!(find_img_tag("<IMG/>"), Some(0));
        // A truncated tail must not read past the end either.
        assert_eq!(find_img_tag("<im"), None);
    }

    #[test]
    fn an_emoticon_scan_does_not_panic_on_a_long_multibyte_body() {
        // 160 bytes back from the end lands mid code point for 3-byte characters.
        let body = "\u{65e5}".repeat(54);
        let emoticons = vec![(":tada:".to_owned(), "mxc://x/y".to_owned())];
        let out = substitute_emoticons(&format!("{body}:tada:"), &emoticons);
        assert!(out.contains("mxc://x/y"), "the emoticon was not substituted");
    }

    /// Tests the wiring from the registry to the policy (argument order, and
    /// which outcomes spend budget), using a fabricated "much later" instant so
    /// the backoff is never what refuses.
    #[test]
    fn what_an_attempt_established_decides_whether_it_cost_anything() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let later = |h: u64| {
            std::time::Instant::now()
                .checked_add(std::time::Duration::from_secs(h * 3600))
                .expect("a few hours must be representable")
        };

        // A failure that taught nothing costs a backoff step and no budget, so the
        // key is eventually allowed again.
        let transient = "!room:example.org\u{1f}TRANSIENT";
        assert!(registry.mark_backup_attempt(transient));
        for hour in 1..=12 {
            registry.record_backup_outcome(transient, super::BackupOutcome::Inconclusive);
            assert!(
                registry.mark_backup_attempt_at(transient, later(hour)),
                "hour {hour}: an inconclusive failure must never exhaust the budget"
            );
        }

        // Nor does "this device cannot decrypt the backup": no request was sent.
        // The steady state of a device whose recovery key was never entered.
        let undecryptable = "!room:example.org\u{1f}NODECRYPTKEY";
        assert!(registry.mark_backup_attempt(undecryptable));
        for hour in 1..=12 {
            registry
                .record_backup_outcome(undecryptable, super::BackupOutcome::NoDecryptionKey);
            assert!(
                registry.mark_backup_attempt_at(undecryptable, later(hour)),
                "hour {hour}: a missing decryption key must not spend the budget"
            );
        }

        // A definitive answer spends budget, and it runs out.
        let definitive = "!room:example.org\u{1f}DEFINITIVE";
        assert!(registry.mark_backup_attempt(definitive));
        let mut allowed = 1;
        for hour in 1..=20 {
            registry.record_backup_outcome(definitive, super::BackupOutcome::Definitive);
            if registry.mark_backup_attempt_at(definitive, later(hour)) {
                allowed += 1;
            }
        }
        assert_eq!(
            allowed, MAX_BACKUP_ATTEMPTS,
            "a definitive answer must spend exactly one unit of budget"
        );

        // A permanent refusal stops the key at once.
        let refused = "!room:example.org\u{1f}REFUSED";
        assert!(registry.mark_backup_attempt(refused));
        registry.record_backup_outcome(refused, super::BackupOutcome::PermanentRefusal);
        assert!(
            !registry.mark_backup_attempt_at(refused, later(99)),
            "a permanent refusal must not be retried, however much time passes"
        );
        // ...and manual recovery is still the escape.
        registry.clear_backup_attempt("!room:example.org");
        assert!(registry.mark_backup_attempt(refused));
    }

    /// A failing server is asked less often (escalating backoff), and a failure
    /// that taught nothing does not spend the budget; one counter cannot do
    /// both.
    #[test]
    fn a_failing_server_is_asked_less_often_and_never_for_ever() {
        let huge = std::time::Duration::from_secs(u32::MAX as u64);
        let secs = std::time::Duration::from_secs;

        // Probe escalation through the policy itself (comparing against
        // `backup_attempt_backoff` would not show whether the policy uses it):
        // with 45 s elapsed, one try (30 s) allows and two tries (60 s) do not.
        assert!(
            backup_attempt_allowed(1, 0, secs(45)),
            "45s must satisfy the 30s wait that follows the first try"
        );
        assert!(
            !backup_attempt_allowed(2, 0, secs(45)),
            "45s must NOT satisfy the wait after two tries -- if it does, the \
             backoff is pinned at its floor and a failing server is hammered"
        );
        assert!(!backup_attempt_allowed(3, 0, secs(45)));
        assert!(!backup_attempt_allowed(4, 0, secs(200)));
        assert!(backup_attempt_allowed(4, 0, secs(300)));

        // The required wait keeps growing.
        let mut last_needed = 0u64;
        for tries in 1..=7u32 {
            let needed = (0..=4000u64)
                .find(|&t| backup_attempt_allowed(tries, 0, secs(t)))
                .expect("some wait must eventually be enough");
            assert!(
                needed > last_needed || tries > 6,
                "try {tries}: required wait {needed}s did not grow past \
                 {last_needed}s"
            );
            last_needed = needed;
        }
        assert!(
            last_needed >= 1920,
            "the ceiling must actually be reached within the budget"
        );

        // Inconclusive failures never exhaust the budget.
        assert!(backup_attempt_allowed(8, 0, huge));
        assert!(backup_attempt_allowed(99, 0, huge));

        // Definitive answers spend it, and it runs out.
        for attempts in 0..MAX_BACKUP_ATTEMPTS {
            assert!(
                backup_attempt_allowed(1, attempts, huge),
                "budget {attempts} of {MAX_BACKUP_ATTEMPTS} must still allow a try"
            );
        }
        assert!(
            !backup_attempt_allowed(1, MAX_BACKUP_ATTEMPTS, huge),
            "a spent budget must stop the key, however long we wait"
        );
        // A permanent refusal sets the budget to the cap, so it stops at once.
        assert!(!backup_attempt_allowed(99, MAX_BACKUP_ATTEMPTS, huge));
    }

    /// Manual recovery clears the room's per-session entries
    /// (`<room>\x1f<session>`), not just the whole-room one.
    #[test]
    fn clearing_a_room_forgets_its_per_session_attempts_too() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let room = "!room:example.org";
        let session = format!("{room}\u{1f}SESSIONID");
        let other_room_session = "!other:example.org\u{1f}SESSIONID";

        assert!(registry.mark_backup_attempt(room));
        assert!(registry.mark_backup_attempt(&session));
        assert!(registry.mark_backup_attempt(other_room_session));
        // Immediately again: the backoff refuses, so the clear below is
        // meaningful.
        assert!(
            !registry.mark_backup_attempt(&session),
            "a second attempt inside the backoff must be refused"
        );

        registry.clear_backup_attempt(room);

        assert!(registry.mark_backup_attempt(room), "the room pass is freed");
        assert!(
            registry.mark_backup_attempt(&session),
            "and so is every session recorded under that room"
        );
        assert!(
            !registry.mark_backup_attempt(other_room_session),
            "but another room's sessions are left alone"
        );
    }

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

    // A session change must not strand a claimed slot, or that reaction would
    // stay unclickable.
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

    // A tombstone gets its own row text, not "updated room settings".
    #[test]
    fn tombstone_state_row_names_the_upgrade() {
        assert_eq!(
            state_row_text("m.room.tombstone", "@alice:example.org"),
            "@alice:example.org upgraded this room."
        );
    }

    // The row must not leak the tombstone's body or the successor id; it uses
    // only the state type and sender.
    #[test]
    fn state_row_text_uses_only_kind_and_actor() {
        let row = state_row_text("m.room.tombstone", "@alice:example.org");
        assert!(!row.contains('!'), "a room id must never appear in the row: {row}");

        // An unknown state type falls back rather than echoing the type string.
        let unknown = state_row_text("org.example.custom", "@bob:example.org");
        assert_eq!(unknown, "@bob:example.org updated room settings.");
        assert!(!unknown.contains("org.example.custom"));
    }

    // Existing wording is unchanged.
    #[test]
    fn existing_state_rows_are_unchanged() {
        assert_eq!(state_row_text("m.room.create", "@a:b.c"), "@a:b.c created the room.");
        assert_eq!(state_row_text("m.room.name", "@a:b.c"), "@a:b.c changed the room name.");
        assert_eq!(state_row_text("m.room.topic", "@a:b.c"), "@a:b.c changed the room topic.");
        assert_eq!(state_row_text("m.room.avatar", "@a:b.c"), "@a:b.c changed the room avatar.");
        // Actor-free: the room becoming encrypted is the fact.
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
        // Sessions across sender keys are merged and deduplicated; sender keys
        // are dropped.
        assert_eq!(mapped[0].1, vec!["session1", "session2", "session3"]);
    }

    #[test]
    fn empty_import_result_maps_to_empty() {
        let keys = BTreeMap::new();
        assert!(sessions_by_room_from_import(&keys).is_empty());
    }

    // Only user id and timestamp cross; a missing timestamp serializes as
    // null (C++ maps it to 0).
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
        // Exactly the two documented fields.
        for entry in &entries {
            assert_eq!(entry.as_object().unwrap().len(), 2);
        }

        let (empty, empty_total) = super::read_by_json(
            std::iter::empty::<(&OwnedUserId, &Receipt)>(),
        );
        assert!(empty.is_empty());
        assert_eq!(empty_total, 0);
    }

    // Only the newest READ_BY_CAP entries cross; the total stays uncapped.
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
        // Newest first; the 4 oldest fell outside the window.
        assert_eq!(entries[0]["user_id"], "@u19:example.org");
        assert_eq!(
            entries[super::READ_BY_CAP - 1]["user_id"],
            "@u4:example.org"
        );
    }

    // The local user leads the window, the rest keep insertion order, and the
    // window is bounded like receipts.
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
        // Then the SDK's order minus our own id.
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

    // An absurd id is dropped rather than forwarded.
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

    // Typed profile changes. old == new (an avatar-only change) must report no
    // rename.
    #[test]
    fn profile_name_change_classifies_set_changed_and_cleared() {
        assert_eq!(
            super::profile_name_change(None, Some("Alice")),
            Some(("set", None, Some("Alice".to_owned())))
        );
        // A cleared name stored as "" is not a set.
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

    // The bound is in chars; a byte slice would panic inside an emoji.
    #[test]
    fn profile_names_are_bounded_by_chars_never_bytes() {
        let long: String = "🌩".repeat(300);
        let (kind, _, new) = super::profile_name_change(None, Some(&long))
            .expect("a long name is still a set");
        assert_eq!(kind, "set");
        let new = new.expect("set carries the new name");
        assert_eq!(new.chars().count(), super::PROFILE_NAME_CAP);
        // Four bytes each: the byte length shows no code point was split.
        assert_eq!(new.len(), super::PROFILE_NAME_CAP * 4);
        assert!(new.chars().all(|c| c == '🌩'));

        // A name at exactly the cap is untouched.
        let exact: String = "a".repeat(super::PROFILE_NAME_CAP);
        assert_eq!(
            super::profile_name_change(None, Some(&exact)),
            Some(("set", None, Some(exact)))
        );
    }

    // The pinned SDK's markdown constructors produce a formatted body for the
    // toolbar's syntax and a plain m.text for ordinary text.
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

    // ── Formatted sends (parse_body_spec / composed_content) ────────────

    #[test]
    fn body_spec_defaults_and_refusals() {
        use super::{parse_body_spec, BodyFormat, SendBodySpec};
        assert_eq!(parse_body_spec("").unwrap(), SendBodySpec::default());
        assert_eq!(parse_body_spec("  ").unwrap(), SendBodySpec::default());
        let emote = parse_body_spec(r#"{"msgtype":"emote"}"#).unwrap();
        assert!(emote.emote);
        assert_eq!(emote.format, BodyFormat::Markdown);
        // Refused, never defaulted.
        assert!(parse_body_spec(r#"{"format":"htm"}"#).is_err());
        assert!(parse_body_spec(r#"{"msgtype":"notice"}"#).is_err());
        assert!(parse_body_spec("not json").is_err());
        // html format requires html content; html content requires the format.
        assert!(parse_body_spec(r#"{"format":"html"}"#).is_err());
        assert!(parse_body_spec(r#"{"format":"plain","html":"<b>x</b>"}"#).is_err());
    }

    #[test]
    fn html_spec_sends_both_bodies_from_the_caller() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec =
            parse_body_spec(r#"{"format":"html","html":"<strong>hi</strong>"}"#)
                .unwrap();
        let content = composed_content("hi", &spec);
        match content.msgtype {
            MessageType::Text(text) => {
                assert_eq!(text.body, "hi");
                let formatted = text.formatted.expect("formatted body");
                assert_eq!(formatted.body, "<strong>hi</strong>");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn outgoing_html_is_strict_sanitized_at_the_boundary() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        // Script, event handlers and unsafe schemes must not survive even if the
        // C++ serializer regresses.
        let spec = parse_body_spec(
            r#"{"format":"html","html":"<b onclick=\"x()\">b</b><script>evil()</script><a href=\"javascript:evil()\">l</a>"}"#,
        )
        .unwrap();
        let content = composed_content("b l", &spec);
        match content.msgtype {
            MessageType::Text(text) => {
                let formatted = text.formatted.expect("formatted body");
                assert!(!formatted.body.contains("script"),
                        "script survived: {}", formatted.body);
                assert!(!formatted.body.contains("onclick"),
                        "event handler survived: {}", formatted.body);
                assert!(!formatted.body.contains("javascript:"),
                        "unsafe scheme survived: {}", formatted.body);
                assert!(formatted.body.contains("<b>b</b>"));
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    /// An inline custom emoji becomes an MSC2545 image in the formatted body;
    /// the plain body keeps the shortcode as fallback.
    #[test]
    fn a_shortcode_becomes_an_image_and_the_plain_body_keeps_it() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(
            r#"{"format":"markdown","emoticons":{":blob:":"mxc://e.org/blob"}}"#,
        )
        .unwrap();
        match composed_content("hey :blob: there", &spec).msgtype {
            MessageType::Text(text) => {
                // The fallback an unsupporting client shows.
                assert_eq!(text.body, "hey :blob: there");
                let formatted = text.formatted.expect("a formatted body");
                assert!(formatted.body.contains("data-mx-emoticon"),
                        "{}", formatted.body);
                assert!(formatted.body.contains("mxc://e.org/blob"),
                        "{}", formatted.body);
                assert!(!formatted.body.contains(":blob:")
                            || formatted.body.contains("alt=\":blob:\""),
                        "the shortcode text was left beside the image: {}",
                        formatted.body);
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    /// Markdown still works alongside emoji, which is why substitution runs
    /// after markdown.
    #[test]
    fn an_emoji_does_not_cost_the_message_its_markdown() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(
            r#"{"format":"markdown","emoticons":{":blob:":"mxc://e.org/blob"}}"#,
        )
        .unwrap();
        match composed_content("**bold** :blob: `code`", &spec).msgtype {
            MessageType::Text(text) => {
                let body = text.formatted.expect("formatted").body;
                assert!(body.contains("<strong>bold</strong>"), "{body}");
                assert!(body.contains("data-mx-emoticon"), "{body}");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    /// Three places a shortcode is ordinary text and must be left alone.
    #[test]
    fn shortcodes_are_left_alone_where_they_are_not_emoji() {
        use super::substitute_emoticons;
        let map = vec![(":blob:".to_owned(), "mxc://e.org/blob".to_owned())];

        // Inside a code span or block: literal.
        for literal in [
            "<code>:blob:</code>",
            "<pre><code>x :blob: y</code></pre>",
        ] {
            assert_eq!(substitute_emoticons(literal, &map), literal,
                       "rewrote a shortcode inside code");
        }

        // Inside a URL: a path segment.
        let url = r#"<a href="https://h.example/:blob:/x">l</a>"#;
        assert_eq!(substitute_emoticons(url, &map), url,
                   "rewrote inside a tag");
        let bare = "see https://h.example/:blob:/x now";
        assert_eq!(substitute_emoticons(bare, &map), bare,
                   "rewrote inside a bare URL run");

        // It does fire in ordinary prose around them.
        let prose = "hi :blob: there";
        assert!(substitute_emoticons(prose, &map).contains("data-mx-emoticon"));
    }

    /// The longer shortcode wins, or `:blob:` eats the front of `:blob_wave:`.
    #[test]
    fn the_longest_shortcode_wins() {
        use super::{parse_body_spec, substitute_emoticons};
        let spec = parse_body_spec(
            r#"{"format":"markdown","emoticons":{":blob:":"mxc://e.org/a",":blob_wave:":"mxc://e.org/b"}}"#,
        )
        .unwrap();
        let out = substitute_emoticons("x :blob_wave: y", &spec.emoticons);
        assert!(out.contains("mxc://e.org/b"), "{out}");
        assert!(!out.contains("mxc://e.org/a"), "{out}");
    }

    /// The image filter against realistic attacks; it fails closed.
    #[test]
    fn only_mxc_images_survive_the_outgoing_filter() {
        use super::strip_non_mxc_images;

        // Kept: a real emoticon in the MSC2545 shape.
        let good = r#"a <img data-mx-emoticon src="mxc://e.org/b" alt=":b:" height="32"> z"#;
        assert_eq!(strip_non_mxc_images(good), good);

        for hostile in [
            r#"<img data-mx-emoticon src="https://tracker.example/p.png">"#,
            r#"<img src="http://evil.test/x.png">"#,
            r#"<img src="//evil.test/x.png">"#,
            r#"<img src="javascript:evil()">"#,
            r#"<img src="data:image/png;base64,AAAA">"#,
            r#"<img src="mxc://">"#,
            // No src: dropped.
            r#"<img data-mx-emoticon alt=":b:">"#,
            // Case is not an escape.
            r#"<IMG SRC="https://evil.test/x.png">"#,
            // Nor are single quotes.
            r#"<img src='https://evil.test/x.png'>"#,
            // Nor whitespace around the equals sign.
            r#"<img src = "https://evil.test/x.png">"#,
            // Nor hiding the real src behind a lookalike attribute.
            r#"<img data-src="mxc://e.org/b" src="https://evil.test/x.png">"#,
        ] {
            let cleaned = strip_non_mxc_images(hostile);
            assert!(
                !cleaned.contains("<img") && !cleaned.contains("<IMG"),
                "{hostile} survived as {cleaned}"
            );
        }

        // An unterminated tag is malformed; drop the rest.
        assert!(!strip_non_mxc_images(r#"ok <img src="mxc://e.org/b""#)
            .contains("<img"));

        // It must not eat elements that merely start similarly.
        let other = "<image>x</image>";
        assert_eq!(strip_non_mxc_images(other), other);
        let text = "1 < 2 and imgs are fine";
        assert_eq!(strip_non_mxc_images(text), text);
    }

    /// MSC2545 inline emoji: `data-mx-emoticon` must survive, although ruma's
    /// strict allow-list permits `data-mx-*` only on `span`.
    #[test]
    fn an_inline_custom_emoji_keeps_the_attribute_that_makes_it_one() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(
            r#"{"format":"html","html":"hi <img data-mx-emoticon src=\"mxc://example.org/blob\" alt=\":blob:\" title=\":blob:\" height=\"32\" />"}"#,
        )
        .unwrap();
        let content = composed_content("hi :blob:", &spec);
        match content.msgtype {
            MessageType::Text(text) => {
                let formatted = text.formatted.expect("formatted body");
                assert!(
                    formatted.body.contains("data-mx-emoticon"),
                    "the emoticon marker was stripped: {}",
                    formatted.body
                );
                assert!(formatted.body.contains("mxc://example.org/blob"));
                assert!(formatted.body.contains("alt="));
                assert!(formatted.body.contains("height="));
                // The plain body is the fallback an unsupporting client shows.
                assert_eq!(text.body, "hi :blob:");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    /// Nothing else was widened: images may only use `mxc`, event handlers are
    /// still removed, and `data-mx-*` is not allowed elsewhere.
    #[test]
    fn widening_for_emoji_did_not_widen_anything_else() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(
            r#"{"format":"html","html":"<img data-mx-emoticon src=\"https://tracker.example/pixel.png\" onerror=\"evil()\" /><img src=\"http://evil.test/x.png\" /><b data-mx-emoticon>b</b>"}"#,
        )
        .unwrap();
        let content = composed_content("x", &spec);
        match content.msgtype {
            MessageType::Text(text) => {
                let body = text.formatted.expect("formatted body").body;
                assert!(
                    !body.contains("tracker.example")
                        && !body.contains("evil.test"),
                    "an http image survived, so an outgoing message could \
                     carry a tracking pixel: {body}"
                );
                assert!(!body.contains("onerror"), "handler survived: {body}");
                assert!(
                    !body.contains("<b data-mx-emoticon"),
                    "the marker leaked onto a non-image element: {body}"
                );
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }

        // An element missing from the override list is not scheme-checked at all,
        // so the link half is asserted too.
        let links = parse_body_spec(
            r#"{"format":"html","html":"<a href=\"javascript:evil()\">a</a><a href=\"https://ok.example/\">b</a>"}"#,
        )
        .unwrap();
        match composed_content("x", &links).msgtype {
            MessageType::Text(text) => {
                let body = text.formatted.expect("formatted body").body;
                assert!(
                    !body.contains("javascript:"),
                    "pinning img schemes stopped href being checked: {body}"
                );
                assert!(body.contains("https://ok.example/"),
                        "an ordinary link was refused: {body}");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn plain_spec_never_parses_markdown() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        // /shrug: markdown would eat the escaped underscore.
        let spec = parse_body_spec(r#"{"format":"plain"}"#).unwrap();
        let content = composed_content(r"¯\_(ツ)_/¯ **not bold**", &spec);
        match content.msgtype {
            MessageType::Text(text) => {
                assert!(text.formatted.is_none());
                assert_eq!(text.body, r"¯\_(ツ)_/¯ **not bold**");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn emote_spec_builds_an_emote_with_markdown_and_mention_reduction() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(r#"{"msgtype":"emote"}"#).unwrap();
        let markdown =
            "waves at [Alice](https://matrix.to/#/@alice:example.org)";
        let content = composed_content(markdown, &spec);
        match content.msgtype {
            MessageType::Emote(emote) => {
                // The /me lane gets the mention reduction: the anchor stays in
                // formatted_body, the plain body carries the label.
                assert_eq!(emote.body, "waves at Alice");
                let formatted = emote.formatted.expect("formatted body");
                assert!(formatted.body.contains("matrix.to/#/@alice:example.org"));
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    // add_mentions serializes "m.mentions".user_ids, invalid MXIDs are
    // dropped, and an all-invalid list produces no mentions.
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

    // Mention sends: the plain body carries the display text; formatted_body
    // keeps the matrix.to anchor.
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

    // ---- Polls ------------------------------------------------------------

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
    fn waveform_of_a_quiet_recording_is_not_flattened() {
        // A voice message at ordinary indoor level (peaking at 175/1024) must not
        // be crushed under AudioPlayerCard's 0.12 display floor.
        let raw: Vec<u64> = (0..180)
            .map(|i| {
                let f = i as f64;
                let env = 0.4 + 0.6 * (f / 23.0).sin().abs();
                (90.0 + 90.0 * (f / 7.0).sin() * env).max(0.0) as u64
            })
            .collect();
        assert!(raw.iter().copied().max().unwrap() < 200, "fixture is not quiet");

        let out = super::downsample_waveform(&raw, 1024);
        assert_eq!(out.len(), 96);

        // The loudest bucket reaches full height...
        assert_eq!(out.iter().copied().max().unwrap(), 100);
        // Most buckets clear the UI's 0.12 floor (the fixed-1024 scaling left 68%
        // beneath it).
        let above_floor = out.iter().filter(|v| **v >= 12).count();
        assert!(
            above_floor >= 70,
            "only {above_floor} of 96 buckets clear the 0.12 display floor; \
             a quiet recording is still being flattened"
        );
        // A waveform, not a block: many distinct heights.
        let distinct: std::collections::BTreeSet<u64> = out.iter().copied().collect();
        assert!(
            distinct.len() >= 30,
            "only {} distinct bar heights", distinct.len()
        );
    }

    #[test]
    fn waveform_of_a_full_range_recording_is_unchanged() {
        // A recording that already uses the full range is unchanged.
        let raw: Vec<u64> = (0..96).map(|i| (i * 1024) / 95).collect();
        let out = super::downsample_waveform(&raw, 1024);
        let expected: Vec<u64> = raw.iter().map(|v| (v.min(&1024) * 100) / 1024).collect();
        assert_eq!(out, expected);
        // Digital silence stays silent rather than dividing by zero.
        assert_eq!(super::downsample_waveform(&[0, 0, 0], 1024), vec![0, 0, 0]);
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

    // MatrixRTC membership churn must not become timeline items.
    #[test]
    fn rtc_membership_state_is_filtered_and_messages_are_not() {
        let member: AnySyncTimelineEvent = serde_json::from_value(serde_json::json!({
            "type": "org.matrix.msc3401.call.member",
            "state_key": "@a:example.org",
            "sender": "@a:example.org",
            "event_id": "$m",
            "origin_server_ts": 1,
            "content": { "memberships": [] }
        }))
        .expect("a call member state event deserializes");
        assert!(is_rtc_membership_event(&member));

        let custom: AnySyncTimelineEvent = serde_json::from_value(serde_json::json!({
            "type": "org.matrix.msc4143.rtc.member",
            "state_key": "@a:example.org_DEV",
            "sender": "@a:example.org",
            "event_id": "$r",
            "origin_server_ts": 1,
            "content": {}
        }))
        .expect("a custom state event deserializes");
        assert!(is_rtc_membership_event(&custom));

        let message: AnySyncTimelineEvent = serde_json::from_value(serde_json::json!({
            "type": "m.room.message",
            "sender": "@a:example.org",
            "event_id": "$t",
            "origin_server_ts": 1,
            "content": { "msgtype": "m.text", "body": "hi" }
        }))
        .expect("a message deserializes");
        assert!(!is_rtc_membership_event(&message));

        let topic: AnySyncTimelineEvent = serde_json::from_value(serde_json::json!({
            "type": "m.room.topic",
            "state_key": "",
            "sender": "@a:example.org",
            "event_id": "$s",
            "origin_server_ts": 1,
            "content": { "topic": "x" }
        }))
        .expect("a topic state event deserializes");
        assert!(!is_rtc_membership_event(&topic));
    }
}

/// MSC4274 galleries and MSC2530 empty-body attachments in the exact shapes
/// Sable sends (`msgContent.ts` `buildGalleryContent` /
/// `getGalleryItemContent` / `getImageMsgContent`, `RoomInput.tsx`).
#[cfg(test)]
mod gallery_tests {
    use super::{
        fill_message_media, gallery_item_key, lightning_event_filter, message_summary,
        parse_gallery, MediaRegistry, StoredMedia, TimelineRegistry, GALLERY_ITEM_CAP,
        MEDIA_SOURCE_CAP,
    };
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};
    use matrix_sdk::ruma::events::{
        room::{message::RoomMessageEventContent, MediaSource},
        AnySyncTimelineEvent,
    };
    use matrix_sdk::ruma::room_version_rules::RoomVersionRules;
    use serde_json::{json, Value};

    const MXC_A: &str = "mxc://sable.example/aaaaaaaaaaaaaaaaaaaa";
    const MXC_B: &str = "mxc://sable.example/bbbbbbbbbbbbbbbbbbbb";

    /// One `itemtypes` entry as Sable builds it for an image in an unencrypted
    /// room: `getImageMsgContent` with `msgtype` renamed `itemtype`, plus the
    /// MSC4193 spoiler flag and blurhash.
    fn sable_plain_image_item(name: &str, mxc: &str, w: u64, h: u64) -> Value {
        json!({
            "filename": name,
            "body": name,
            "page.codeberg.everypizza.msc4193.spoiler": false,
            "info": {
                "w": w,
                "h": h,
                "mimetype": "image/png",
                "size": 482_113,
                "xyz.amorgan.blurhash": "LEHV6nWB2yk8pyo0adR*.7kCMdnj",
            },
            "url": mxc,
            "itemtype": "m.image",
        })
    }

    /// The same item from an encrypted room: `file` instead of `url`.
    fn sable_encrypted_image_item(name: &str, mxc: &str) -> Value {
        json!({
            "filename": name,
            "body": name,
            "page.codeberg.everypizza.msc4193.spoiler": false,
            "info": { "w": 1920, "h": 1080, "mimetype": "image/png", "size": 1_024_000 },
            "file": {
                "v": "v2",
                "url": mxc,
                "key": {
                    "alg": "A256CTR",
                    "ext": true,
                    "k": "qcHVMSgYg-71CauWBezXI5qkaRb0LuIy-Wx5kIaHMIA",
                    "key_ops": ["encrypt", "decrypt"],
                    "kty": "oct",
                },
                "iv": "X85+XgHN+HEAAAAAAAAAAA",
                "hashes": { "sha256": "7jHGYSGzBf/mE4W5DGf1vU6Iw7NI0eXyV3NtP9yZw0E" },
            },
            "itemtype": "m.image",
        })
    }

    /// `buildGalleryContent(items)` with NO caption: the body is Sable's
    /// generated list, `[<filename>: <url, else "file">]` per item.
    fn sable_gallery(items: Vec<Value>) -> Value {
        let body = items
            .iter()
            .map(|item| {
                format!(
                    "[{}: {}]",
                    item["filename"].as_str().unwrap(),
                    item.get("url").and_then(Value::as_str).unwrap_or("file")
                )
            })
            .collect::<Vec<_>>()
            .join("\n");
        json!({ "msgtype": "dm.filament.gallery", "body": body, "itemtypes": items })
    }

    fn content(value: Value) -> RoomMessageEventContent {
        serde_json::from_value(value).expect("the wire content deserializes")
    }

    fn event(content: Value) -> AnySyncTimelineEvent {
        serde_json::from_value(json!({
            "type": "m.room.message",
            "event_id": "$gallery:sable.example",
            "sender": "@seikm:sable.example",
            "origin_server_ts": 1_790_000_000_000u64,
            "content": content,
        }))
        .expect("the wire event deserializes")
    }

    // matrix-sdk-ui's default filter rejected the custom msgtype, so the
    // gallery never became a timeline item.
    #[test]
    fn a_sable_gallery_is_admitted_to_the_timeline() {
        let rules = RoomVersionRules::V11;
        let plain = sable_gallery(vec![
            sable_plain_image_item("before.png", MXC_A, 1280, 720),
            sable_plain_image_item("after.png", MXC_B, 1280, 720),
        ]);
        assert!(lightning_event_filter(&event(plain), &rules));
        let encrypted = sable_gallery(vec![
            sable_encrypted_image_item("before.png", MXC_A),
            sable_encrypted_image_item("after.png", MXC_B),
        ]);
        assert!(lightning_event_filter(&event(encrypted), &rules));
        // The stable name the MSC will become.
        let mut stable = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        stable["msgtype"] = "m.gallery".into();
        assert!(lightning_event_filter(&event(stable), &rules));
    }

    // The default filter's rule for other msgtypes: an edit folds into its
    // target. An unrelated custom msgtype stays dropped.
    #[test]
    fn a_gallery_edit_and_other_custom_msgtypes_stay_out() {
        let rules = RoomVersionRules::V11;
        let mut edit = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        edit["m.relates_to"] = json!({ "rel_type": "m.replace", "event_id": "$orig:sable.example" });
        edit["m.new_content"] = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        assert!(!lightning_event_filter(&event(edit), &rules));
        let custom = json!({ "msgtype": "com.example.custom", "body": "hi" });
        assert!(!lightning_event_filter(&event(custom), &rules));
    }

    #[test]
    fn a_two_image_sable_gallery_becomes_one_image_row_with_two_items() {
        let wire = content(sable_gallery(vec![
            sable_plain_image_item("before.png", MXC_A, 1280, 720),
            sable_plain_image_item("after.png", MXC_B, 640, 480),
        ]));
        let mut out = json!({});
        let sources = fill_message_media(&mut out, &wire.msgtype, "$g");

        // The row is its first picture...
        assert_eq!(out["msgtype"], "image");
        assert_eq!(out["media_filename"], "before.png");
        assert_eq!(out["media_mxc"], MXC_A);
        assert_eq!(out["media_width"], 1280);
        // ...its caption is empty, not Sable's generated `[name: mxc]` list...
        assert_eq!(out["body"], "");
        assert!(out.get("formatted_body").is_none());
        // ...and every item is listed with its own key, in the sender's order.
        let items = out["gallery_items"].as_array().expect("gallery_items");
        assert_eq!(items.len(), 2);
        assert_eq!(items[0]["media_key"], "$g");
        assert_eq!(items[1]["media_key"], gallery_item_key("$g", 1));
        assert_eq!(items[0]["kind"], "image");
        assert_eq!(items[1]["filename"], "after.png");
        assert_eq!(items[1]["width"], 640);
        assert_eq!(items[1]["height"], 480);
        assert_eq!(items[1]["mimetype"], "image/png");
        assert_eq!(items[1]["thumb_available"], false);

        // Every source is registered, the primary first under the row key.
        let keys: Vec<&str> = sources.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, vec!["$g", "$g#item1"]);
        assert!(matches!(&sources[1].1.source, MediaSource::Plain(m) if m.as_str() == MXC_B));
        assert_eq!(sources[1].1.filename, "after.png");
    }

    // Encrypted-room sources carry content keys and stay in Rust; nothing
    // mxc-shaped crosses for them.
    #[test]
    fn an_encrypted_sable_gallery_keeps_its_sources_in_rust() {
        let wire = content(sable_gallery(vec![
            sable_encrypted_image_item("before.png", MXC_A),
            sable_encrypted_image_item("after.png", MXC_B),
        ]));
        let mut out = json!({});
        let sources = fill_message_media(&mut out, &wire.msgtype, "$enc");
        assert_eq!(out["msgtype"], "image");
        assert_eq!(out["body"], "", "the `[name: file]` list is not a caption");
        assert!(out.get("media_mxc").is_none());
        assert_eq!(sources.len(), 2);
        assert!(sources.iter().all(|(_, m)| matches!(m.source, MediaSource::Encrypted(_))));
        let wire_json = out.to_string();
        assert!(!wire_json.contains("A256CTR") && !wire_json.contains(MXC_B));
        assert_eq!(out["gallery_items"].as_array().map(Vec::len), Some(2));
    }

    #[test]
    fn a_gallery_caption_is_the_body_and_keeps_its_html() {
        let mut wire = sable_gallery(vec![
            sable_plain_image_item("before.png", MXC_A, 10, 10),
            sable_plain_image_item("after.png", MXC_B, 10, 10),
        ]);
        wire["body"] = "left is **0.9.8**".into();
        wire["format"] = "org.matrix.custom.html".into();
        wire["formatted_body"] = "left is <strong>0.9.8</strong>".into();
        let wire = content(wire);
        let mut out = json!({});
        fill_message_media(&mut out, &wire.msgtype, "$c");
        assert_eq!(out["body"], "left is **0.9.8**");
        assert_eq!(out["formatted_body"], "left is <strong>0.9.8</strong>");
        let summary = message_summary(&wire.msgtype);
        assert_eq!((summary.kind, summary.count), ("image", 2));
        assert_eq!(summary.text, "left is **0.9.8**");
    }

    // A caption that merely looks like the generated list is kept: the match
    // is exact, line for line.
    #[test]
    fn only_the_exact_generated_list_is_dropped_as_a_caption() {
        let mut wire = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 1, 1)]);
        wire["body"] = "[a.png: see the diff]".into();
        let gallery = parse_gallery(&content(wire).msgtype).expect("a gallery");
        assert_eq!(gallery.caption, "[a.png: see the diff]");
    }

    // Read by the reply quote and thread card; QML builds "Image" / "2 images"
    // from kind and count, and the text is never the generated list.
    #[test]
    fn a_gallery_summarises_as_its_kind_and_count() {
        let images = content(sable_gallery(vec![
            sable_plain_image_item("a.png", MXC_A, 1, 1),
            sable_plain_image_item("b.png", MXC_B, 1, 1),
        ]));
        let summary = message_summary(&images.msgtype);
        assert_eq!((summary.kind, summary.count, summary.text.as_str()), ("image", 2, ""));

        let mut file_item = sable_plain_image_item("notes.pdf", MXC_B, 1, 1);
        file_item["itemtype"] = "m.file".into();
        file_item["info"] = json!({ "mimetype": "application/pdf", "size": 10 });
        let mixed = content(sable_gallery(vec![
            sable_plain_image_item("a.png", MXC_A, 1, 1),
            file_item,
        ]));
        let summary = message_summary(&mixed.msgtype);
        assert_eq!((summary.kind, summary.count), ("file", 2));
    }

    // Disallowed item types are skipped and the rest render. The array is
    // bounded (attacker-authored). An item with no source fails ruma's typed
    // deserializer, and with it the whole event, before this code runs.
    #[test]
    fn foreign_items_are_skipped_and_the_array_is_bounded() {
        let mut text_item = sable_plain_image_item("x", MXC_A, 1, 1);
        text_item["itemtype"] = "m.text".into();
        let mut odd_item = sable_plain_image_item("y", MXC_A, 1, 1);
        odd_item["itemtype"] = "com.example.hologram".into();
        let wire = content(json!({
            "msgtype": "dm.filament.gallery",
            "body": "",
            "itemtypes": [text_item, odd_item, sable_plain_image_item("ok.png", MXC_B, 1, 1)],
        }));
        let gallery = parse_gallery(&wire.msgtype).expect("a gallery");
        assert_eq!(gallery.items.len(), 1);
        // One surviving item is a single attachment, not a grid.
        let mut out = json!({});
        let sources = fill_message_media(&mut out, &wire.msgtype, "$one");
        assert_eq!(out["media_filename"], "ok.png");
        assert!(out.get("gallery_items").is_none());
        assert_eq!(sources.len(), 1);

        let many: Vec<Value> = (0..GALLERY_ITEM_CAP + 10)
            .map(|i| sable_plain_image_item(&format!("{i}.png"), MXC_A, 1, 1))
            .collect();
        let wire = content(json!({ "msgtype": "dm.filament.gallery", "body": "", "itemtypes": many }));
        assert_eq!(parse_gallery(&wire.msgtype).unwrap().items.len(), GALLERY_ITEM_CAP);
    }

    // Sable's default for one attachment without caption: an m.image with an
    // empty body and the name in MSC2530's `filename`.
    #[test]
    fn an_empty_body_image_takes_its_name_from_filename() {
        let mut wire = sable_plain_image_item("comparison.png", MXC_A, 800, 600);
        wire.as_object_mut().unwrap().remove("itemtype");
        wire["msgtype"] = "m.image".into();
        wire["body"] = "".into();
        let wire = content(wire);
        let mut out = json!({});
        let sources = fill_message_media(&mut out, &wire.msgtype, "$single");
        assert_eq!(out["msgtype"], "image");
        assert_eq!(out["body"], "");
        assert_eq!(out["media_filename"], "comparison.png");
        assert_eq!(sources.len(), 1);
        assert_eq!(sources[0].0, "$single");
        let summary = message_summary(&wire.msgtype);
        assert_eq!((summary.kind, summary.text.as_str()), ("image", "comparison.png"));
    }

    fn stored(name: &str) -> StoredMedia {
        StoredMedia {
            source: MediaSource::Plain(format!("mxc://sable.example/{name}").as_str().into()),
            thumbnail: None,
            filename: name.to_owned(),
            mimetype: None,
            declared_size: None,
        }
    }

    // When full, the map evicts the least recently used key, so a newly
    // registered row always gets its source (refusing let ~128 galleries break
    // every later row).
    #[test]
    fn a_full_media_registry_still_takes_a_new_rows_key() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let galleries = MEDIA_SOURCE_CAP / GALLERY_ITEM_CAP + 8;
        for g in 0..galleries {
            let row = format!("$gallery{g}");
            registry.remember_media(row.clone(), stored(&row));
            for i in 1..GALLERY_ITEM_CAP {
                registry.remember_media(gallery_item_key(&row, i), stored(&row));
            }
        }
        registry.remember_media("$new".to_owned(), stored("new"));
        assert!(registry.media_source("$new", false).is_some());
        let last = format!("$gallery{}", galleries - 1);
        assert!(registry.media_source(&last, false).is_some());
        // The oldest went, not the newest.
        assert!(registry.media_source("$gallery0", false).is_none());
    }

    // A key still in use (re-registered or fetched) outlives untouched keys.
    #[test]
    fn the_media_registry_evicts_the_least_recently_used_key() {
        let mut registry = MediaRegistry::with_cap(3);
        for key in ["a", "b", "c"] {
            registry.insert(key.to_owned(), stored(key));
        }
        assert!(registry.get("a").is_some()); // a fetch touches it
        registry.insert("d".to_owned(), stored("d"));
        assert!(registry.contains("a") && registry.contains("d"));
        assert!(!registry.contains("b"), "b was the least recently used");
        registry.insert("c".to_owned(), stored("c")); // re-registered
        registry.insert("e".to_owned(), stored("e"));
        assert!(!registry.contains("a") && registry.contains("c"));
        assert_eq!(registry.len(), 3);
    }

    // The stamp queue is compacted, so it stays bounded and the map never
    // exceeds its cap.
    #[test]
    fn the_media_registry_stays_bounded_under_repeated_touches() {
        let mut registry = MediaRegistry::with_cap(4);
        for round in 0..1000 {
            registry.insert(format!("k{}", round % 7), stored("x"));
            let _ = registry.get("k0");
            let _ = registry.get("k3");
            assert!(registry.len() <= 4);
            assert!(registry.order.len() <= 4 * 4 + 1, "{}", registry.order.len());
        }
        assert!(registry.contains("k0") && registry.contains("k3"));
    }

    // MSC2530 with a caption (as matrix-sdk and Lightning send it): the name is
    // `filename`, the caption stays the body.
    #[test]
    fn a_captioned_image_keeps_caption_and_name_apart() {
        let wire = content(json!({
            "msgtype": "m.image",
            "body": "look at this",
            "filename": "cat.png",
            "info": { "mimetype": "image/png" },
            "url": MXC_A,
        }));
        let mut out = json!({});
        fill_message_media(&mut out, &wire.msgtype, "$cap");
        assert_eq!(out["body"], "look at this");
        assert_eq!(out["media_filename"], "cat.png");
    }
}
