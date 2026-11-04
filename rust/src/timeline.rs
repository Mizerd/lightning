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
use matrix_sdk::send_queue::RoomSendQueueUpdate;
use matrix_sdk_ui::{
    eyeball_im::{Vector, VectorDiff},
    timeline::{
        default_event_filter,
        thread_list_service::{ThreadListItem, ThreadListService},
        AttachmentConfig, AttachmentSource, EncryptedMessage, EventSendState,
        EventTimelineItem, MsgLikeKind, PollResult, Timeline, TimelineBuilder,
        TimelineDetails, TimelineEventFocusThreadMode, TimelineEventItemId, TimelineFocus, TimelineItem,
        TimelineItemContent, TimelineItemKind, TimelineReadReceiptTracking,
        VirtualTimelineItem,
    },
};
use serde_json::json;

use crate::enqueue;

/// Build an m.mentions payload from MXID strings. Invalid ids are dropped.
///
/// ALWAYS returns a value, and an empty one is not the same as none: an
/// outgoing event that carries `"m.mentions": {}` tells every receiving server
/// it uses intentional mentions (MSC3952, spec v1.7), while one WITHOUT the key
/// is still matched by the legacy `.m.rule.contains_display_name`,
/// `.m.rule.contains_user_name` and `.m.rule.roomnotif` body rules. So "ask Max
/// about it" would highlight anyone named Max, a typed "@room" would ping the
/// room, and because `contains_display_name` precedes `.m.rule.suppress_edits`
/// every `* ...` edit would ping them again. Element and Element X always send
/// the key. `add_mentions` must be called before any relation-adding method
/// (reply / replacement), so ruma's `make_replacement` can carry the new
/// content's mentions into `m.new_content` and only NEW ones to the top level.
pub(crate) fn mentions_from_ids(ids: Vec<String>) -> Mentions {
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
    let mut mentions = Mentions::with_user_ids(users);
    mentions.room = room;
    mentions
}

pub(crate) fn outgoing_content(
    body: &str,
    spec: &SendBodySpec,
    mention_user_ids: Vec<String>,
) -> RoomMessageEventContent {
    composed_content(body, spec).add_mentions(mentions_from_ids(mention_user_ids))
}

pub(crate) fn outgoing_content_without_relation(
    body: &str,
    spec: &SendBodySpec,
    mention_user_ids: Vec<String>,
) -> RoomMessageEventContentWithoutRelation {
    composed_content_without_relation(body, spec)
        .add_mentions(mentions_from_ids(mention_user_ids))
}

pub(crate) const ROOM_MENTION_SENTINEL: &str = "@room";

pub const PAGINATION_BATCH: u16 = 20;

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
    if !default_event_filter(event, rules) && !is_visible_room_message(event) {
        FILTER_DROP_SDK.fetch_add(1, Ordering::Relaxed);
        return false;
    }
    if is_rtc_membership_event(event) {
        FILTER_DROP_RTC.fetch_add(1, Ordering::Relaxed);
        return false;
    }
    true
}

pub(crate) fn shown_by_lightning_filter(
    event: &AnySyncTimelineEvent,
    rules: &RoomVersionRules,
) -> bool {
    (default_event_filter(event, rules) || is_visible_room_message(event))
        && !is_rtc_membership_event(event)
}

/// Every `m.room.message` that is not an edit, whatever its msgtype.
///
/// matrix-sdk-ui's default filter keeps a fixed list of msgtypes and drops
/// the rest without a trace (GitHub #30: a message Element showed was simply
/// absent here). That covered MSC4274 galleries (including Sable's unstable
/// `dm.filament.gallery`), which it keeps only with its own `unstable-msc4274`
/// feature, and every custom msgtype a bot, bridge or other client sends.
/// The spec says a client that does not know a msgtype must show its `body`,
/// which is what Element does and what `fill_message_content`'s fallback arm
/// renders. Admitted under the same rule the default filter applies to known
/// msgtypes: an `m.replace` is folded into its target, never a row.
fn is_visible_room_message(event: &AnySyncTimelineEvent) -> bool {
    use matrix_sdk::ruma::events::{
        room::message::Relation, AnySyncMessageLikeEvent, SyncMessageLikeEvent,
    };
    let AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
        SyncMessageLikeEvent::Original(message),
    )) = event
    else {
        return false;
    };
    !matches!(message.content.relates_to, Some(Relation::Replacement(_)))
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

#[deprecated(note = "size against SHUTDOWN_WORST_CASE_MS in lib.rs instead")]
#[allow(dead_code)]
pub const SHUTDOWN_JOIN_TIMEOUT_SECS: u64 = 15;

pub(crate) fn parse_reply_target(id: &str) -> Result<Option<OwnedEventId>, String> {
    let id = id.trim();
    if id.is_empty() {
        return Ok(None);
    }
    EventId::parse(id)
        .map(Some)
        .map_err(|_| "Invalid reply target event id.".to_owned())
}

pub(crate) fn live_location_body(description: Option<&str>) -> &str {
    description.unwrap_or_default()
}

pub(crate) fn thread_receipt_type(privacy: i32) -> ReceiptType {
    match privacy {
        1 | 2 => ReceiptType::ReadPrivate,
        _ => ReceiptType::Read,
    }
}

pub(crate) fn attachment_config(
    info: Option<AttachmentInfo>,
    thumbnail: Option<Thumbnail>,
    caption: Option<String>,
    in_reply_to: Option<OwnedEventId>,
) -> AttachmentConfig {
    AttachmentConfig {
        txn_id: None,
        info,
        thumbnail,
        caption: caption
            .filter(|c| !c.is_empty())
            .map(TextMessageEventContent::plain),
        mentions: Some(Mentions::new()),
        in_reply_to,
    }
}

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

pub const SHUTDOWN_ABORTED_JOIN_MS: u64 = 250;

type EventQueue = Arc<Mutex<VecDeque<String>>>;

const SEND_STEP_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(30);

async fn bounded<T>(step: impl std::future::Future<Output = T>) -> Option<T> {
    tokio::time::timeout(SEND_STEP_TIMEOUT, step).await.ok()
}

type SendJob = std::pin::Pin<Box<dyn std::future::Future<Output = ()> + Send>>;

struct SendLane {
    id: u64,
    jobs: VecDeque<SendJob>,
}

fn send_lane_key(room_id: &str, thread_root_id: &str) -> String {
    format!("{room_id}\u{1f}{thread_root_id}")
}

/// Media sources captured while serializing timeline items, for the media
/// bridge. `MediaSource::Encrypted` embeds content keys, so these stay in
/// this Rust-side map and never cross the FFI. Keyed by event id (or SDK
/// unique id for local echoes); lives as long as the room's timeline.
pub struct StoredMedia {
    pub source: MediaSource,
    pub thumbnail: Option<MediaSource>,
    pub filename: String,
    pub mimetype: Option<String>,
    pub declared_size: Option<u64>,
}

const REACTION_GUARD_TIMEOUT_SECS: u64 = 30;

const MEDIA_SOURCE_CAP: usize = 4096;

const EXPORT_MEDIA_CAP: usize = 50_000;

#[derive(Default)]
struct EncryptedRoomMedia {
    keys: std::collections::HashSet<String>,
    overflowed: bool,
}

struct MediaRegistry {
    entries: HashMap<String, (StoredMedia, u64)>,
    order: VecDeque<(u64, String)>,
    clock: u64,
    cap: usize,
}

impl MediaRegistry {
    fn with_cap(cap: usize) -> Self {
        Self { entries: HashMap::new(), order: VecDeque::new(), clock: 0, cap: cap.max(1) }
    }

    fn stamp(&mut self, key: &str) -> u64 {
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
    timeline: Option<Arc<Timeline>>,
    task: Option<tokio::task::JoinHandle<()>>,
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

struct ActiveThread {
    room_id: String,
    root_event_id: String,
    thread_gen: u64,
    timeline: Option<Arc<Timeline>>,
    task: Option<tokio::task::JoinHandle<()>>,
    pagination_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
}

struct ActiveContext {
    room_id: String,
    event_id: String,
    context_gen: u64,
    timeline: Option<Arc<Timeline>>,
    task: Option<tokio::task::JoinHandle<()>>,
    back_busy: Arc<AtomicBool>,
    fwd_busy: Arc<AtomicBool>,
    reached_start: Arc<AtomicBool>,
    reached_end: Arc<AtomicBool>,
}

const CONTEXT_EVENTS: u16 = 20;
const CONTEXT_PAGE: u16 = 20;

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
    active_thread: Mutex<Option<ActiveThread>>,
    active_thread_list: Mutex<Option<ActiveThreadList>>,
    active_context: Mutex<Option<ActiveContext>>,
    context_gen: AtomicU64,
    thread_list_gen: AtomicU64,
    thread_gen: AtomicU64,
    room_gen: AtomicU64,
    lifecycle_gen: AtomicU64,
    media_sources: Mutex<MediaRegistry>,
    encrypted_room_media: Mutex<EncryptedRoomMedia>,
    export_media: Mutex<HashMap<String, (StoredMedia, bool)>>,
    reaction_inflight: Mutex<std::collections::HashSet<String>>,
    send_lanes: Mutex<HashMap<String, SendLane>>,
    send_lane_seq: AtomicU64,
    backup_download_attempts: Mutex<std::collections::HashMap<String, BackupAttempt>>,
    history_reopened: Mutex<std::collections::HashSet<(u64, String)>>,
    /// The open room, and the `origin_server_ts` of the newest event our own
    /// UNTHREADED read receipt (public or private) points at. By spec such a
    /// receipt covers every thread too, so a thread whose latest reply is no
    /// newer reads as read (finding 134). `None` inside: no such receipt, or its
    /// event is not in the local cache — then only threaded receipts count
    /// (conservative, §8). Never a MAIN-thread receipt: that one covers the main
    /// timeline only. Identifiers and a timestamp only.
    unthreaded_read_cover: Mutex<Option<(String, Option<u64>)>>,
    thread_auto_subscriptions: Mutex<Vec<tokio::task::AbortHandle>>,
    threads_in_live: AtomicBool,
    unthreaded_receipts: AtomicBool,
}

#[derive(Clone, Copy)]
pub(crate) struct BackupAttempt {
    tries: u32,
    attempts: u32,
    last: std::time::Instant,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum BackupOutcome {
    Definitive,
    NoDecryptionKey,
    PermanentRefusal,
    Inconclusive,
}

fn backup_attempt_allowed(tries: u32, attempts: u32, since_last: std::time::Duration) -> bool {
    attempts < MAX_BACKUP_ATTEMPTS
        && since_last >= backup_attempt_backoff(tries.saturating_sub(1))
}

const MAX_BACKUP_ATTEMPTS: u32 = 8;

/// Marks a key never to be tried again this lifecycle. Distinct from
/// `MAX_BACKUP_ATTEMPTS` so raising the cap cannot un-park refused keys.
/// `clear_backup_attempt` still frees these.
const BACKUP_ATTEMPTS_STOPPED: u32 = u32::MAX;

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
            active_context: Mutex::new(None),
            context_gen: AtomicU64::new(0),
            thread_list_gen: AtomicU64::new(0),
            thread_gen: AtomicU64::new(0),
            room_gen: AtomicU64::new(0),
            lifecycle_gen: AtomicU64::new(1),
            media_sources: Mutex::new(MediaRegistry::with_cap(MEDIA_SOURCE_CAP)),
            encrypted_room_media: Mutex::new(EncryptedRoomMedia::default()),
            export_media: Mutex::new(HashMap::new()),
            reaction_inflight: Mutex::new(std::collections::HashSet::new()),
            send_lanes: Mutex::new(HashMap::new()),
            send_lane_seq: AtomicU64::new(0),
            backup_download_attempts: Mutex::new(std::collections::HashMap::new()),
            history_reopened: Mutex::new(std::collections::HashSet::new()),
            unthreaded_read_cover: Mutex::new(None),
            thread_auto_subscriptions: Mutex::new(Vec::new()),
            threads_in_live: AtomicBool::new(false),
            unthreaded_receipts: AtomicBool::new(false),
        }
    }

    pub fn set_threads_in_live(&self, on: bool) {
        self.threads_in_live.store(on, Ordering::SeqCst);
    }

    pub fn set_unthreaded_receipts(&self, on: bool) {
        self.unthreaded_receipts.store(on, Ordering::SeqCst);
    }

    pub fn unthreaded_receipts(&self) -> bool {
        self.unthreaded_receipts.load(Ordering::SeqCst)
    }

    fn live_focus(&self) -> TimelineFocus {
        TimelineFocus::Live {
            hide_threaded_events: !self.threads_in_live.load(Ordering::SeqCst),
        }
    }

    fn unthreaded_read_cover_ms(&self, room_id: &str) -> Option<u64> {
        let guard = self.unthreaded_read_cover.lock().ok()?;
        match guard.as_ref() {
            Some((room, cover)) if room == room_id => *cover,
            _ => None,
        }
    }

    fn set_unthreaded_read_cover(&self, room_id: &str, cover: Option<u64>) -> Option<u64> {
        let Ok(mut guard) = self.unthreaded_read_cover.lock() else { return None };
        let previous = match guard.as_ref() {
            Some((room, previous)) if room == room_id => *previous,
            _ => None,
        };
        *guard = Some((room_id.to_owned(), cover));
        previous
    }

    fn mark_backup_attempt(&self, key: &str) -> bool {
        self.mark_backup_attempt_at(key, std::time::Instant::now())
    }

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

    fn record_backup_outcome(&self, key: &str, outcome: BackupOutcome) {
        let spend = match outcome {
            BackupOutcome::Definitive => 1,
            BackupOutcome::PermanentRefusal => BACKUP_ATTEMPTS_STOPPED,
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

    pub fn clear_backup_attempt(&self, room_id: &str) {
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            let prefix = format!("{room_id}\u{1f}");
            guard.retain(|key, _| key != room_id && !key.starts_with(&prefix));
        }
    }

    pub fn active_room_id(&self) -> Option<String> {
        let guard = self.active.lock().ok()?;
        guard.as_ref().map(|active| active.room_id.clone())
    }

    pub(crate) fn any_view_open(&self) -> bool {
        fn held<T>(slot: &Mutex<Option<T>>) -> bool {
            slot.lock().map_or(true, |guard| guard.is_some())
        }
        held(&self.active)
            || held(&self.active_thread)
            || held(&self.active_thread_list)
            || held(&self.active_context)
    }

    fn is_current(&self, room_gen: u64, lifecycle: u64) -> bool {
        self.room_gen.load(Ordering::SeqCst) == room_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    /// Once per room per lifecycle: a start the server confirms again is believed.
    fn claim_history_reopen(&self, room_id: &str, lifecycle: u64) -> bool {
        self.history_reopened
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .insert((lifecycle, room_id.to_owned()))
    }

    pub fn lifecycle(&self) -> u64 {
        self.lifecycle_gen.load(Ordering::SeqCst)
    }

    pub fn lifecycle_current(&self, lifecycle: u64) -> bool {
        self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    pub(crate) fn remember_media(&self, key: String, media: StoredMedia) {
        if key.is_empty() {
            return;
        }
        if let Ok(mut guard) = self.media_sources.lock() {
            guard.insert(key, media);
        }
    }

    pub fn media_source(
        &self,
        key: &str,
        thumbnail: bool,
    ) -> Option<(MediaSource, String, Option<String>, Option<u64>, bool)> {
        if key.starts_with(crate::roomexport::EXPORT_KEY_PREFIX) {
            let guard = self.export_media.lock().ok()?;
            let (media, _) = guard.get(key)?;
            return Some(Self::source_tuple(media, thumbnail));
        }
        let mut guard = self.media_sources.lock().ok()?;
        let media = guard.get(key)?;
        Some(Self::source_tuple(media, thumbnail))
    }

    fn source_tuple(
        media: &StoredMedia,
        thumbnail: bool,
    ) -> (MediaSource, String, Option<String>, Option<u64>, bool) {
        let has_embedded_thumbnail = thumbnail && media.thumbnail.is_some();
        let source = if thumbnail {
            media.thumbnail.clone().unwrap_or_else(|| media.source.clone())
        } else {
            media.source.clone()
        };
        (
            source,
            media.filename.clone(),
            media.mimetype.clone(),
            media.declared_size,
            has_embedded_thumbnail,
        )
    }

    pub(crate) fn remember_export_media(
        &self,
        key: String,
        media: StoredMedia,
        private_room: bool,
    ) -> bool {
        let Ok(mut guard) = self.export_media.lock() else {
            return false;
        };
        if guard.len() >= EXPORT_MEDIA_CAP && !guard.contains_key(&key) {
            return false;
        }
        guard.insert(key, (media, private_room));
        true
    }

    pub(crate) fn release_export_media(&self, prefix: &str) {
        if prefix.is_empty() {
            return;
        }
        if let Ok(mut guard) = self.export_media.lock() {
            guard.retain(|key, _| !key.starts_with(prefix));
        }
    }

    fn clear_media(&self) {
        if let Ok(mut guard) = self.media_sources.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.encrypted_room_media.lock() {
            *guard = EncryptedRoomMedia::default();
        }
    }

    pub(crate) fn mark_media_from_encrypted_room(&self, key: &str) {
        if let Ok(mut guard) = self.encrypted_room_media.lock() {
            if guard.keys.len() >= MEDIA_SOURCE_CAP {
                guard.overflowed = true;
            } else {
                guard.keys.insert(key.to_owned());
            }
        }
    }

    /// Whether `key` was marked, or might have been. True when unsure.
    pub(crate) fn media_from_encrypted_room(&self, key: &str) -> bool {
        if key.starts_with(crate::roomexport::EXPORT_KEY_PREFIX) {
            return self
                .export_media
                .lock()
                .map_or(true, |guard| guard.get(key).map_or(true, |(_, private)| *private));
        }
        self.encrypted_room_media
            .lock()
            .map_or(true, |guard| guard.overflowed || guard.keys.contains(key))
    }

    fn take_active(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String)> {
        let mut guard = self.active.lock().ok()?;
        let active = guard.take()?;
        if let Some(task) = &active.task {
            task.abort();
        }
        Some((active.task, active.room_id))
    }

    pub fn open_room(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) {
        self.open_room_inner(runtime, client, room_id, false)
    }

    pub fn reload_room_at_live(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
    ) {
        self.open_room_inner(runtime, client, room_id, true)
    }

    fn open_room_inner(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        shrink_first: bool,
    ) {
        self.close_thread();
        self.close_context();
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
                _ => handle.abort(),
            }
        } else {
            handle.abort();
        }
    }

    pub fn close(&self) {
        self.close_thread();
        self.close_context();
        self.close_thread_list();
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

    fn take_active_context(&self) -> Option<(Option<tokio::task::JoinHandle<()>>, String, String)> {
        let mut guard = self.active_context.lock().ok()?;
        let ctx = guard.take()?;
        if let Some(task) = &ctx.task {
            task.abort();
        }
        Some((ctx.task, ctx.room_id, ctx.event_id))
    }

    fn context_current(&self, context_gen: u64, lifecycle: u64) -> bool {
        self.context_gen.load(Ordering::SeqCst) == context_gen
            && self.lifecycle_gen.load(Ordering::SeqCst) == lifecycle
    }

    fn context_timeline_for(&self, room_id: &str) -> Option<Arc<Timeline>> {
        let guard = self.active_context.lock().ok()?;
        guard.as_ref().and_then(|ctx| {
            (ctx.room_id == room_id).then(|| ctx.timeline.clone()).flatten()
        })
    }

    pub fn open_context(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        event_id: String,
    ) {
        let context_gen = self.context_gen.fetch_add(1, Ordering::SeqCst) + 1;
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        if let Some((_task, old_room, old_event)) = self.take_active_context() {
            enqueue(
                &self.events,
                json!({
                    "type": "context_closed",
                    "room_id": old_room,
                    "event_id": old_event,
                }),
            );
        }
        let back_busy = Arc::new(AtomicBool::new(false));
        let fwd_busy = Arc::new(AtomicBool::new(false));
        let reached_start = Arc::new(AtomicBool::new(false));
        let reached_end = Arc::new(AtomicBool::new(false));
        if let Ok(mut guard) = self.active_context.lock() {
            *guard = Some(ActiveContext {
                room_id: room_id.clone(),
                event_id: event_id.clone(),
                context_gen,
                timeline: None,
                task: None,
                back_busy,
                fwd_busy,
                reached_start,
                reached_end,
            });
        }
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let handle = runtime.spawn(open_context_task(
            registry, client, room_id, event_id, context_gen, lifecycle, events,
        ));
        if let Ok(mut guard) = self.active_context.lock() {
            match guard.as_mut() {
                Some(ctx) if ctx.context_gen == context_gen => ctx.task = Some(handle),
                _ => handle.abort(),
            }
        } else {
            handle.abort();
        }
    }

    pub fn close_context(&self) {
        self.context_gen.fetch_add(1, Ordering::SeqCst);
        if let Some((_task, old_room, old_event)) = self.take_active_context() {
            enqueue(
                &self.events,
                json!({
                    "type": "context_closed",
                    "room_id": old_room,
                    "event_id": old_event,
                }),
            );
        }
    }

    pub fn paginate_context(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        event_id: String,
        forward: bool,
    ) -> Result<(), String> {
        let (timeline, context_gen, busy, reached) = {
            let guard = self
                .active_context
                .lock()
                .map_err(|_| "context registry lock poisoned".to_owned())?;
            let ctx = guard.as_ref().ok_or("No context view is open.")?;
            if ctx.room_id != room_id || ctx.event_id != event_id {
                return Err("No context view is open for that event.".to_owned());
            }
            let timeline = ctx.timeline.clone().ok_or("context view not ready")?;
            if forward {
                (timeline, ctx.context_gen, Arc::clone(&ctx.fwd_busy),
                 Arc::clone(&ctx.reached_end))
            } else {
                (timeline, ctx.context_gen, Arc::clone(&ctx.back_busy),
                 Arc::clone(&ctx.reached_start))
            }
        };
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let dir = if forward { "forward" } else { "backward" };
        if reached.load(Ordering::SeqCst) {
            enqueue(
                &self.events,
                context_pagination_json(&room_id, &event_id, context_gen,
                    self.lifecycle_gen.load(Ordering::SeqCst), dir, "idle",
                    json!({ "reached_edge": true })),
            );
            return Ok(());
        }
        if busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Ok(());
        }
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        enqueue(
            &events,
            context_pagination_json(&room_id, &event_id, context_gen, lifecycle,
                                    dir, "loading", json!({})),
        );
        runtime.spawn(async move {
            let result = if forward {
                timeline.paginate_forwards(CONTEXT_PAGE).await
            } else {
                timeline.paginate_backwards(CONTEXT_PAGE).await
            };
            busy.store(false, Ordering::SeqCst);
            if !registry.context_current(context_gen, lifecycle) {
                return;
            }
            let payload = match result {
                Ok(hit_edge) => {
                    reached.store(hit_edge, Ordering::SeqCst);
                    context_pagination_json(&room_id, &event_id, context_gen,
                        lifecycle, dir, "idle", json!({ "reached_edge": hit_edge }))
                }
                Err(_err) => context_pagination_json(&room_id, &event_id,
                    context_gen, lifecycle, dir, "failed",
                    json!({ "category": "network" })),
            };
            enqueue(&events, payload);
        });
        Ok(())
    }

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

    pub fn close_thread_list(&self) {
        self.thread_list_gen.fetch_add(1, Ordering::SeqCst);
        let _ = self.take_active_thread_list();
    }

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

    pub fn mark_thread_read(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        root_event_id: String,
        privacy: i32,
        op_id: u64,
    ) -> Result<(), String> {
        let Some((timeline, _gen, _lifecycle)) =
            self.thread_timeline_for(&room_id, &root_event_id)
        else {
            return Err("No live thread timeline is open for that root.".to_owned());
        };
        let receipt_type = thread_receipt_type(privacy);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
            let event = match timeline.mark_as_read(receipt_type).await {
                Ok(_) => json!({
                    "type": "thread_read_marker_advanced",
                    "room_id": room_id,
                    "thread_root_id": root_event_id,
                    "op_id": op_id,
                }),
                Err(_) => json!({
                    "type": "thread_mark_read_failed",
                    "room_id": room_id,
                    "thread_root_id": root_event_id,
                    "op_id": op_id,
                }),
            };
            enqueue(&events, event);
        });
        Ok(())
    }

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

    fn open_thread_timeline_in_room(&self, room_id: &str) -> Option<Arc<Timeline>> {
        let guard = self.active_thread.lock().ok()?;
        let thread = guard.as_ref()?;
        if thread.room_id != room_id {
            return None;
        }
        thread.timeline.clone()
    }

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

    pub(crate) async fn redecrypted_replies_missing_from_open_thread(
        &self,
        client: &Client,
        room_id: &str,
        resolved: &[String],
    ) -> usize {
        let open = self.active_thread.lock().ok().and_then(|guard| {
            guard.as_ref().and_then(|thread| {
                (thread.room_id == room_id).then(|| {
                    Some((thread.root_event_id.clone(), thread.timeline.clone()?))
                })?
            })
        });
        let Some((root, timeline)) = open else { return 0 };
        let Ok(room_ref) = RoomId::parse(room_id) else { return 0 };
        let Some(room) = client.get_room(&room_ref) else { return 0 };
        let Ok((cache, _drop_handles)) = room.event_cache().await else { return 0 };
        let mut missing = 0;
        for id in resolved {
            let Ok(event_id) = matrix_sdk::ruma::EventId::parse(id) else { continue };
            let Ok(Some(event)) = cache.find_event(&event_id).await else { continue };
            if decrypted_thread_root(event.raw()).as_deref() != Some(root.as_str()) {
                continue;
            }
            if timeline.item_by_event_id(&event_id).await.is_none() {
                missing += 1;
            }
        }
        missing
    }

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
            return Ok(());
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
                return;
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
        let events = Arc::clone(&self.events);
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let thread_gen = self.thread_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &root_event_id);
        let lane = send_lane_key(&room_id, &root_event_id);
        self.spawn_ordered(runtime, lane, async move {
            let watcher = bounded(watch_thread_send(&client, &room_id))
                .await
                .flatten();
            let send_on = |timeline: Arc<Timeline>| {
                let body = body.clone();
                let reply_to = reply_to.clone();
                let mention_user_ids = mention_user_ids.clone();
                let spec = spec.clone();
                async move {
                    match reply_to {
                        Some(reply_to) => {
                            let content = outgoing_content_without_relation(
                                &body,
                                &spec,
                                mention_user_ids,
                            );
                            unwedge_send_queue(&timeline);
                            // Never timed: send_reply ends in the queue push, and a
                            // timeout there reports failure for a message still sent.
                            timeline.send_reply(content, reply_to).await.is_ok()
                        }
                        None => {
                            let message =
                                outgoing_content(&body, &spec, mention_user_ids);
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
                match bounded(build_transient_thread_timeline(
                    &client, &room_id, &root_event_id,
                ))
                .await
                .flatten()
                {
                    Some(timeline) => send_on(timeline).await,
                    None => false,
                }
            };
            if sent {
                registry.subscribe_after_thread_send(
                    client.clone(), &room_id, &root_event_id, watcher, lifecycle,
                );
            }
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
            let lane = send_lane_key(&room_id, "");
            self.spawn_ordered(runtime, lane, async move {
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
        let lane = send_lane_key(&room_id, &thread_root_id);
        self.spawn_ordered(runtime, lane, async move {
            let watcher = bounded(watch_thread_send(&client, &room_id))
                .await
                .flatten();
            let sent = if let Some((timeline, _gen, _lc)) = open_thread {
                unwedge_send_queue(&timeline);
                timeline.send(content).await.is_ok()
            } else {
                match bounded(build_transient_thread_timeline(
                    &client, &room_id, &thread_root_id,
                ))
                .await
                .flatten()
                {
                    Some(timeline) => {
                        unwedge_send_queue(&timeline);
                        timeline.send(content).await.is_ok()
                    }
                    None => false,
                }
            };
            if sent {
                registry.subscribe_after_thread_send(
                    client.clone(), &room_id, &thread_root_id, watcher, lifecycle,
                );
            }
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

    pub async fn download_backup_keys_for_room(
        self: &Arc<Self>,
        client: &Client,
        room_id: &str,
    ) {
        self.backup_keys_pass(client, room_id, true).await;
    }

    pub async fn prefetch_backup_keys_for_room(
        self: &Arc<Self>,
        client: &Client,
        room_id: &str,
    ) {
        self.backup_keys_pass(client, room_id, false).await;
    }

    async fn backup_keys_pass(
        self: &Arc<Self>,
        client: &Client,
        room_id: &str,
        report: bool,
    ) {
        let lifecycle = self.lifecycle();
        let emit = |state: &str| {
            if report && self.lifecycle_current(lifecycle) {
                enqueue(
                    &self.events,
                    json!({
                        "type": "crypto_bootstrap",
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
        let backups = client.encryption().backups();
        if !backups.are_enabled().await {
            emit("skipped_no_backup_key");
            return;
        }
        let Ok(room_ref) = RoomId::parse(room_id) else {
            emit("skipped_bad_room_id");
            return;
        };
        if !self.mark_backup_attempt(room_id) {
            emit("skipped_already_attempted");
            return;
        }
        emit("started");
        let mut result = backups.download_room_keys_for_room(&room_ref).await;
        if result.is_err() && self.lifecycle_current(lifecycle) {
            tokio::time::sleep(std::time::Duration::from_secs(3)).await;
            result = backups.download_room_keys_for_room(&room_ref).await;
        }
        emit(if result.is_ok() { "ok" } else { "failed" });
        if result.is_err() {
            self.clear_backup_attempt(room_id);
        }
    }

    /// Runs `job` after every earlier job for the same lane key. Spawning each
    /// send separately lets tasks reach the SDK send queue in any order.
    fn spawn_ordered(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        key: String,
        job: impl std::future::Future<Output = ()> + Send + 'static,
    ) {
        let job: SendJob = Box::pin(job);
        let lane_id = {
            let mut lanes = self
                .send_lanes
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if let Some(lane) = lanes.get_mut(&key) {
                lane.jobs.push_back(job);
                return;
            }
            let id = self.send_lane_seq.fetch_add(1, Ordering::SeqCst);
            lanes.insert(key.clone(), SendLane { id, jobs: VecDeque::from([job]) });
            id
        };
        let registry = Arc::clone(self);
        runtime.spawn(async move {
            loop {
                let next = {
                    let mut lanes = registry
                        .send_lanes
                        .lock()
                        .unwrap_or_else(|poisoned| poisoned.into_inner());
                    match lanes.get_mut(&key) {
                        Some(lane) if lane.id == lane_id => match lane.jobs.pop_front() {
                            Some(job) => job,
                            None => {
                                lanes.remove(&key);
                                return;
                            }
                        },
                        _ => return,
                    }
                };
                let _ = tokio::spawn(next).await;
            }
        });
    }

    fn clear_send_lanes(&self) {
        self.send_lanes
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clear();
    }

    pub fn advance_lifecycle(&self) {
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
    }

    pub fn shutdown(&self, runtime: &tokio::runtime::Runtime) {
        self.clear_send_lanes();
        self.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        self.room_gen.fetch_add(1, Ordering::SeqCst);
        self.thread_gen.fetch_add(1, Ordering::SeqCst);
        self.close_context();
        self.close_thread_list();
        self.clear_media();
        if let Ok(mut guard) = self.export_media.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.backup_download_attempts.lock() {
            guard.clear();
        }
        if let Ok(mut guard) = self.reaction_inflight.lock() {
            guard.clear();
        }
        // Each holds a Client of the departing session. After the lifecycle
        // bump above, so register_thread_auto_subscription() can tell.
        self.abort_thread_auto_subscriptions();
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

    fn timeline_for(&self, room_id: &str) -> Option<(Arc<Timeline>, u64, u64)> {
        let guard = self.active.lock().ok()?;
        let active = guard.as_ref()?;
        if active.room_id != room_id {
            return None;
        }
        let timeline = active.timeline.clone()?;
        Some((timeline, active.room_gen, self.lifecycle_gen.load(Ordering::SeqCst)))
    }

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
            return Ok(());
        }
        if busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Ok(());
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
            let walk = crate::pagewalk::walk_backwards(
                &timeline,
                count,
                crate::pagewalk::FILTERED_RUN_WALK,
                || registry.is_current(room_gen, lifecycle),
                || registry.claim_history_reopen(&room_id, lifecycle),
            )
            .await;
            let walked_pages = walk.as_ref().map(|walk| walk.outcome.pages).unwrap_or(0);
            let history_reopened = walk.as_ref().is_ok_and(|walk| walk.history_reopened);
            let result = walk.map(|walk| walk.outcome.hit_start);
            busy.store(false, Ordering::SeqCst);
            if !registry.is_current(room_gen, lifecycle) {
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
                            "filter_offered": FILTER_OFFERED
                                .load(Ordering::Relaxed),
                            "filter_dropped_sdk": FILTER_DROP_SDK
                                .load(Ordering::Relaxed),
                            "filter_dropped_rtc": FILTER_DROP_RTC
                                .load(Ordering::Relaxed),
                            "walked_pages": walked_pages,
                            "history_reopened": history_reopened,
                        }),
                    );
                }
                Err(_err) => {
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
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let lane = send_lane_key(&room_id, "");
        self.spawn_ordered(runtime, lane, async move {
            let message = outgoing_content(&body, &spec, mention_user_ids);
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
        in_reply_to: Option<OwnedEventId>,
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
        let lane = send_lane_key(&room_id, "");
        self.spawn_ordered(runtime, lane, async move {
            let config = attachment_config(info, thumbnail, caption, in_reply_to);
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
                    "category": if result.is_ok() { "" } else { "rejected" },
                }),
            );
        });
        Ok(())
    }

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
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let lane = send_lane_key(&room_id, "");
        self.spawn_ordered(runtime, lane, async move {
            let content =
                outgoing_content_without_relation(&body, &spec, mention_user_ids);
            unwedge_send_queue(&timeline);
            // Never timed: see the thread reply path.
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
        in_reply_to: Option<OwnedEventId>,
        op_id: u64,
    ) -> Result<(), String> {
        let mime: mime::Mime = mime_str
            .parse()
            .map_err(|_| "Unsupported attachment MIME type.".to_owned())?;
        let events = Arc::clone(&self.events);
        let lifecycle = self.lifecycle_gen.load(Ordering::SeqCst);
        let registry = Arc::clone(self);
        let open_thread = self.thread_timeline_for(&room_id, &root_event_id);
        let lane = send_lane_key(&room_id, &root_event_id);
        self.spawn_ordered(runtime, lane, async move {
            let watcher = bounded(watch_thread_send(&client, &room_id))
                .await
                .flatten();
            let timeline = match open_thread {
                Some((timeline, _gen, _lc)) => Some(timeline),
                None => {
                    bounded(build_transient_thread_timeline(
                        &client, &room_id, &root_event_id,
                    ))
                    .await
                    .flatten()
                }
            };
            let sent = if let Some(timeline) = timeline {
                let config = attachment_config(info, thumbnail, caption, in_reply_to);
                unwedge_send_queue(&timeline);
                timeline
                    .send_attachment(source, mime, config)
                    .use_send_queue()
                    .await
                    .is_ok()
            } else {
                false
            };
            if sent {
                registry.subscribe_after_thread_send(
                    client.clone(), &room_id, &root_event_id, watcher, lifecycle,
                );
            }
            if registry.lifecycle_current(lifecycle) {
                enqueue(
                    &events,
                    json!({
                        "type": "attachment_send_result",
                        "op_id": op_id,
                        "room_id": room_id,
                        "thread_root_id": root_event_id,
                        "lifecycle": lifecycle,
                        "ok": sent,
                        "category": if sent { "" } else { "rejected" },
                    }),
                );
            }
        });
        Ok(())
    }

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
        let in_thread = !thread_root_id.trim().is_empty();
        let resolved = if in_thread {
            self.thread_timeline_for(&room_id, &thread_root_id)
        } else {
            self.timeline_for(&room_id)
        };
        let Some((timeline, timeline_gen, lifecycle)) = resolved else {
            return Err(if in_thread {
                "No live timeline is open for that thread.".to_owned()
            } else {
                "No live timeline is open for that room.".to_owned()
            });
        };
        let event_id = EventId::parse(&target_event_id)
            .map_err(|_| "Invalid edit target event id.".to_owned())?;
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        let room_gen_for_report = self.room_gen.load(Ordering::SeqCst);
        runtime.spawn(async move {
            let item_id = TimelineEventItemId::EventId(event_id);
            // ruma's make_replacement puts mentions in m.new_content and only newly
            // added ones at top level, so attach them before wrapping. ALWAYS, even
            // when empty: an edit without the key re-runs the legacy body rules
            // and pings everyone the text names a second time.
            let message =
                outgoing_content_without_relation(&new_body, &spec, mention_user_ids);
            let content = EditedContent::RoomMessage(message);
            unwedge_send_queue(&timeline);
            if timeline.edit(&item_id, content).await.is_err() {
                let current = if in_thread {
                    registry.thread_current(timeline_gen, lifecycle)
                } else {
                    registry.is_current(timeline_gen, lifecycle)
                };
                if current {
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

    pub fn toggle_reaction(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        thread_root_id: String,
        target_event_id: String,
        key: String,
    ) -> Result<(), String> {
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
            let send = tokio::spawn(async move {
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

    fn subscribe_after_thread_send(
        self: &Arc<Self>,
        client: Client,
        room_id: &str,
        root_event_id: &str,
        watcher: Option<tokio::sync::broadcast::Receiver<RoomSendQueueUpdate>>,
        lifecycle: u64,
    ) {
        if !self.lifecycle_current(lifecycle) {
            return;
        }
        let Some(updates) = watcher else { return };
        let Ok(root) = EventId::parse(root_event_id) else { return };
        let Some(room) = RoomId::parse(room_id).ok().and_then(|id| client.get_room(&id))
        else {
            return;
        };
        let registry = Arc::clone(self);
        let task = tokio::spawn(async move {
            if !registry.lifecycle_current(lifecycle) {
                return;
            }
            let supported = client
                .unstable_features()
                .await
                .map(|features| server_supports_thread_subscriptions(&features))
                .unwrap_or(false);
            if !supported {
                return;
            }
            let Some(sent) = wait_for_thread_send(updates, &root).await else { return };
            if !registry.lifecycle_current(lifecycle) {
                return;
            }
            let _ = room.subscribe_thread_if_needed(&root, Some(sent)).await;
        });
        self.register_thread_auto_subscription(task.abort_handle(), lifecycle);
    }

    fn register_thread_auto_subscription(
        &self,
        handle: tokio::task::AbortHandle,
        lifecycle: u64,
    ) -> bool {
        let Ok(mut guard) = self.thread_auto_subscriptions.lock() else {
            handle.abort();
            return false;
        };
        // Under the lock shutdown() drains with: either this sees the bumped
        // lifecycle, or the drain sees this handle. Only this handle is
        // aborted; a newer session's watchers in the list are its own.
        if !self.lifecycle_current(lifecycle) {
            handle.abort();
            return false;
        }
        guard.retain(|tracked| !tracked.is_finished());
        guard.push(handle);
        true
    }

    fn abort_thread_auto_subscriptions(&self) {
        if let Ok(mut guard) = self.thread_auto_subscriptions.lock() {
            for task in guard.drain(..) {
                task.abort();
            }
        }
    }

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

    pub fn redact(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        client: Client,
        room_id: String,
        target_event_id: String,
        reason: String,
    ) -> Result<(), String> {
        let room = crate::rooms::joined_room(&client, &room_id)?;
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
            let result = room.redact(&event_id, reason, None).await;
            if registry.lifecycle_current(lifecycle) {
                let category = result.as_ref().err().map(|err| {
                    redaction_error_category(err.client_api_error_kind(), &err.to_string())
                });
                enqueue(
                    &events,
                    redaction_finished_payload(
                        &room_id,
                        event_id.as_str(),
                        room_gen,
                        lifecycle,
                        category,
                    ),
                );
            }
        });
        Ok(())
    }

    pub fn retry_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let thread_timeline = self.open_thread_timeline_in_room(&room_id);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
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
            let mut handle = scan(&timeline.items().await, &transaction_id);
            if handle.is_none() {
                if let Some(thread) = thread_timeline.as_ref() {
                    handle = scan(&thread.items().await, &transaction_id);
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
            unwedge_send_queue(&timeline);
            let _ = handle.unwedge().await;
        });
        Ok(())
    }

    pub fn cancel_send(
        self: &Arc<Self>,
        runtime: &tokio::runtime::Runtime,
        room_id: String,
        transaction_id: String,
    ) -> Result<(), String> {
        let Some((timeline, room_gen, lifecycle)) = self.timeline_for(&room_id) else {
            return Err("No live timeline is open for that room.".to_owned());
        };
        let thread_timeline = self.open_thread_timeline_in_room(&room_id);
        let registry = Arc::clone(self);
        let events = Arc::clone(&self.events);
        runtime.spawn(async move {
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
            let mut handle = scan(&timeline.items().await, &transaction_id);
            if handle.is_none() {
                if let Some(thread) = thread_timeline.as_ref() {
                    handle = scan(&thread.items().await, &transaction_id);
                }
            }
            let category = match handle {
                None => Some("cancel_target_missing"),
                Some(handle) => match handle.abort().await {
                    Ok(true) => None,
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
        let context_timeline = self.context_timeline_for(&room_id);
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
            if let Some(context) = &context_timeline {
                for id in utd_session_ids(context).await {
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
            if let Some(context) = &context_timeline {
                context.retry_decryption(session_ids.iter().cloned()).await;
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
        let context = {
            let guard = self.active_context.lock().ok();
            guard.and_then(|g| {
                g.as_ref().and_then(|ctx| {
                    Some((ctx.timeline.clone()?, ctx.room_id.clone()))
                })
            })
        };
        if let Some((context_timeline, context_room)) = context {
            let wanted = sessions_for_room(sessions_by_room, &context_room);
            if !wanted.is_empty() {
                context_timeline.retry_decryption(wanted.iter().cloned()).await;
            }
        }

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

pub(crate) fn sessions_for_room<'a>(
    sessions_by_room: &'a [(String, Vec<String>)],
    room_id: &str,
) -> &'a [String] {
    sessions_by_room
        .iter()
        .find(|(room, _)| room == room_id)
        .map(|(_, ids)| ids.as_slice())
        .unwrap_or(&[])
}

const SHRINK_WAIT_BUDGET_MS: u64 = 600;
const SHRINK_POLL_STEP_MS: u64 = 60;

async fn await_event_cache_shrink(
    room: &matrix_sdk::Room,
    previous_task: Option<tokio::task::JoinHandle<()>>,
) -> Option<(usize, bool)> {
    let (cache, _drop_handles) = room.event_cache().await.ok()?;
    let before = cache.events().await.ok()?.len();
    if before == 0 {
        return None;
    }
    if let Some(task) = previous_task {
        let _ = tokio::time::timeout(
            std::time::Duration::from_millis(SHRINK_WAIT_BUDGET_MS),
            task,
        )
        .await;
    }
    if let Ok(events) = cache.events().await {
        if events.len() < before {
            return Some((before, true));
        }
    }
    let steps = SHRINK_WAIT_BUDGET_MS / SHRINK_POLL_STEP_MS;
    for _ in 0..steps {
        tokio::time::sleep(std::time::Duration::from_millis(SHRINK_POLL_STEP_MS))
            .await;
        let Ok(events) = cache.events().await else { return Some((before, false)) };
        if events.len() < before {
            return Some((before, true));
        }
    }
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
    let opened_ms = crate::push_verdict::now_ms();
    let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let fallback_verdicts = crate::push_verdict::SyncVerdicts::default();

    let Ok(room_ref) = RoomId::parse(&room_id) else {
        emit_timeline_error(&events, &room_id, room_gen, lifecycle, "invalid_room_id");
        return;
    };

    let Some(room) = client.get_room(&room_ref) else {
        emit_timeline_error(&events, &room_id, room_gen, lifecycle, "unknown_room");
        return;
    };

    let trim_report: Option<(usize, bool)> = if shrink_first {
        await_event_cache_shrink(&room, previous_task).await
    } else {
        None
    };

    let timeline = match TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
        .with_focus(registry.live_focus())
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

    let own_receipts = timeline.subscribe_own_user_read_receipts_changed().await;
    tokio::pin!(own_receipts);
    let mut own_receipts_open = true;
    if let Some(own) = client.user_id() {
        let cover = load_unthreaded_read_cover(&room, own).await;
        registry.set_unthreaded_read_cover(&room_id, cover);
    }

    let (items, mut stream) = timeline.subscribe().await;

    {
        let Ok(mut guard) = registry.active.lock() else { return };
        match guard.as_mut() {
            Some(active) if active.room_gen == room_gen => {
                active.timeline = Some(Arc::clone(&timeline));
            }
            _ => return,
        }
    }

    if crate::missingtrace::enabled() {
        let trace_registry = Arc::clone(&registry);
        tokio::spawn(crate::missingtrace::check_open_room(
            Arc::downgrade(&timeline),
            events.clone(),
            move || trace_registry.is_current(room_gen, lifecycle),
        ));
    }

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
                        eprintln!(
                            "lightning.send_trace: room={watch_room} \
queue read FAILED ({err}) — this tick says nothing either way"
                        );
                        continue;
                    }
                };
                let orphaned = in_flight.iter().filter(|id| !queued.contains(id)).count();
                eprintln!(
                    "lightning.send_trace: room={} in_flight={} queued={} \
orphaned={}{}",
                    watch_room,
                    in_flight.len(),
                    queued.len(),
                    orphaned,
                    if orphaned > 0 {
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

    let reply_details_fetched: Arc<Mutex<HashSet<String>>> =
        Arc::new(Mutex::new(HashSet::new()));
    fetch_missing_reply_details(&timeline, items.iter(), &reply_details_fetched);

    recover_keys_for_utds(
        &registry, &client, &room_id, &timeline,
        utd_sessions_in(items.iter()), RecoveryScope::Room(room_gen),
        lifecycle,
    );

    let mut snapshot: Vec<serde_json::Value> =
        items.iter().map(|item| item_to_json(item, &room_id, &own_user, &registry)).collect();
    annotate_snapshot_verdicts(&room, &fallback_verdicts, &items, &mut snapshot).await;
    let mut backlog = crate::push_verdict::BacklogMarker::new(
        opened_ms,
        items.iter().filter_map(|item| crate::push_verdict::item_facts(item)),
    );
    let mut tail = LiveTail::new(items.clone());
    enqueue(
        &events,
        json!({
            "type": "timeline_reset",
            "room_id": room_id,
            "room_generation": room_gen,
            "lifecycle": lifecycle,
            "items": snapshot,
            "trimmed_from": trim_report.map(|(before, _)| before),
            "trim_shrunk": trim_report.map(|(_, shrunk)| shrunk),
        }),
    );

    {
        let registry = Arc::clone(&registry);
        let client = client.clone();
        let room = room_id.clone();
        tokio::spawn(async move {
            registry.download_backup_keys_for_room(&client, &room).await;
        });
    }

    let fetch_timeline = Arc::clone(&timeline);
    let fetch_members = async move { fetch_timeline.fetch_members().await };
    tokio::pin!(fetch_members);
    let mut members_fetched = false;
    loop {
        tokio::select! {
            _ = &mut fetch_members, if !members_fetched => {
                members_fetched = true;
            }
            changed = own_receipts.next(), if own_receipts_open => {
                if changed.is_none() {
                    own_receipts_open = false;
                    continue;
                }
                if !registry.is_current(room_gen, lifecycle) {
                    break;
                }
                let Some(own) = client.user_id() else { continue };
                let cover = load_unthreaded_read_cover(&room, own).await;
                let previous = registry.set_unthreaded_read_cover(&room_id, cover);
                if previous == cover {
                    continue;
                }
                for (index, item) in tail.thread_roots_flipped_by(previous, cover) {
                    let repaint = VectorDiff::Set { index, value: item };
                    enqueue(
                        &events,
                        diff_to_json(
                            &room_id, room_gen, lifecycle, &repaint, &own_user, &registry,
                        ),
                    );
                }
            }
            maybe_diffs = stream.next() => {
                let Some(diffs) = maybe_diffs else { break };
                if !registry.is_current(room_gen, lifecycle) {
                    break;
                }
                let mut push_context = None;
                backlog.begin_batch();
                for diff in diffs {
                    let changed = diff_items(&diff);
                    fetch_missing_reply_details(
                        &timeline, changed.iter(), &reply_details_fetched,
                    );
                    recover_keys_for_utds(
                        &registry, &client, &room_id, &timeline,
                        utd_sessions_in(changed.iter()),
                        RecoveryScope::Room(room_gen), lifecycle,
                    );
                    let mut value = diff_to_json(
                        &room_id, room_gen, lifecycle, &diff, &own_user, &registry,
                    );
                    let now = crate::push_verdict::now_ms();
                    let decrypted = tail.decrypts_in_place(&diff, item_is_utd, item_event_id);
                    let live_insert = tail.classify_and_apply(&diff, |item| {
                        arrived_since_open(item, opened_ms, now)
                    });
                    let judged = mark_live_insert(&diff, live_insert.as_ref(), &mut value);
                    annotate_push_verdicts(
                        &room, &fallback_verdicts, &mut push_context, judged, &mut value,
                    ).await;
                    if decrypted {
                        annotate_decrypted_set(&diff, &mut value, opened_ms, now);
                    }
                    backlog.mark(
                        judged,
                        &mut value,
                        now,
                        |item| crate::push_verdict::item_facts(item),
                    );
                    enqueue(&events, value);
                }
            }
        }
    }
}

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

async fn open_thread_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    root_event_id: String,
    thread_gen: u64,
    lifecycle: u64,
    events: EventQueue,
) {
    let opened_ms = crate::push_verdict::now_ms();
    let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let fallback_verdicts = crate::push_verdict::SyncVerdicts::default();

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
            _ => return,
        }
    }

    let reply_details_fetched: Arc<Mutex<HashSet<String>>> =
        Arc::new(Mutex::new(HashSet::new()));
    fetch_missing_reply_details(&timeline, items.iter(), &reply_details_fetched);

    recover_keys_for_utds(
        &registry, &client, &room_id, &timeline,
        utd_sessions_in(items.iter()), RecoveryScope::Thread(thread_gen),
        lifecycle,
    );

    let mut snapshot: Vec<serde_json::Value> =
        items.iter().map(|item| item_to_json(item, &room_id, &own_user, &registry)).collect();
    annotate_snapshot_verdicts(&room, &fallback_verdicts, &items, &mut snapshot).await;
    let mut backlog = crate::push_verdict::BacklogMarker::new(
        opened_ms,
        items.iter().filter_map(|item| crate::push_verdict::item_facts(item)),
    );
    let mut tail = LiveTail::new(items.clone());
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

    if !has_event_rows {
        let atomics = match registry.active_thread.lock() {
            Ok(guard) => match guard.as_ref() {
                Some(thread) if thread.thread_gen == thread_gen => Some((
                    Arc::clone(&thread.pagination_busy),
                    Arc::clone(&thread.reached_start),
                )),
                _ => return,
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
                        return;
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
                let mut push_context = None;
                backlog.begin_batch();
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
                    let mut value = fill_diff_json(base, &diff, &own_user, &registry);
                    let now = crate::push_verdict::now_ms();
                    let decrypted = tail.decrypts_in_place(&diff, item_is_utd, item_event_id);
                    let live_insert = tail.classify_and_apply(&diff, |item| {
                        arrived_since_open(item, opened_ms, now)
                    });
                    let judged = mark_live_insert(&diff, live_insert.as_ref(), &mut value);
                    annotate_push_verdicts(
                        &room, &fallback_verdicts, &mut push_context, judged, &mut value,
                    ).await;
                    if decrypted {
                        annotate_decrypted_set(&diff, &mut value, opened_ms, now);
                    }
                    backlog.mark(
                        judged,
                        &mut value,
                        now,
                        |item| crate::push_verdict::item_facts(item),
                    );
                    enqueue(&events, value);
                }
            }
        }
    }
}

fn context_pagination_json(
    room_id: &str,
    event_id: &str,
    context_gen: u64,
    lifecycle: u64,
    direction: &str,
    state: &str,
    extra: serde_json::Value,
) -> serde_json::Value {
    let mut v = json!({
        "type": "context_pagination",
        "room_id": room_id,
        "event_id": event_id,
        "context_generation": context_gen,
        "lifecycle": lifecycle,
        "direction": direction,
        "state": state,
    });
    if let (Some(obj), Some(add)) = (v.as_object_mut(), extra.as_object()) {
        for (k, val) in add {
            obj.insert(k.clone(), val.clone());
        }
    }
    v
}

async fn open_context_task(
    registry: Arc<TimelineRegistry>,
    client: Client,
    room_id: String,
    event_id: String,
    context_gen: u64,
    lifecycle: u64,
    events: EventQueue,
) {
    let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let emit_error = |category: &str| {
        enqueue(
            &events,
            json!({
                "type": "context_error",
                "room_id": room_id,
                "event_id": event_id,
                "context_generation": context_gen,
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
    let Ok(event_ref) = EventId::parse(&event_id) else {
        emit_error("invalid_event_id");
        return;
    };

    let timeline = match TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
        .with_focus(TimelineFocus::Event {
            target: event_ref,
            num_context_events: CONTEXT_EVENTS,
            thread_mode: TimelineEventFocusThreadMode::Automatic {
                hide_threaded_events: true,
            },
        })
        .build()
        .await
    {
        Ok(timeline) => Arc::new(timeline),
        Err(_err) => {
            emit_error("context_failed");
            return;
        }
    };

    let (items, mut stream) = timeline.subscribe().await;
    {
        let Ok(mut guard) = registry.active_context.lock() else { return };
        match guard.as_mut() {
            Some(ctx) if ctx.context_gen == context_gen => {
                ctx.timeline = Some(Arc::clone(&timeline));
            }
            _ => return,
        }
    }
    if !registry.context_current(context_gen, lifecycle) {
        return;
    }

    let reply_details_fetched: Arc<Mutex<HashSet<String>>> =
        Arc::new(Mutex::new(HashSet::new()));
    fetch_missing_reply_details(&timeline, items.iter(), &reply_details_fetched);

    let snapshot: Vec<serde_json::Value> =
        items.iter().map(|item| item_to_json(item, &room_id, &own_user, &registry)).collect();
    enqueue(
        &events,
        json!({
            "type": "context_reset",
            "room_id": room_id,
            "event_id": event_id,
            "context_generation": context_gen,
            "lifecycle": lifecycle,
            "items": snapshot,
        }),
    );

    while let Some(diffs) = stream.next().await {
        if !registry.context_current(context_gen, lifecycle) {
            break;
        }
        for diff in diffs {
            let changed = diff_items(&diff);
            fetch_missing_reply_details(&timeline, changed.iter(), &reply_details_fetched);
            let base = json!({
                "type": "context_diff",
                "room_id": room_id,
                "event_id": event_id,
                "context_generation": context_gen,
                "lifecycle": lifecycle,
            });
            enqueue(&events, fill_diff_json(base, &diff, &own_user, &registry));
        }
    }
}

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
            _ => return,
        }
    }

    let (_initial, mut stream) = service.subscribe_to_items_updates();

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

const THREAD_SUBSCRIPTIONS_FEATURE: &str = "org.matrix.msc4306";

pub(crate) fn server_supports_thread_subscriptions(
    features: &std::collections::BTreeSet<matrix_sdk::ruma::api::FeatureFlag>,
) -> bool {
    features.contains(&matrix_sdk::ruma::api::FeatureFlag::from(THREAD_SUBSCRIPTIONS_FEATURE))
}

pub(crate) fn thread_root_of_content(content: &serde_json::Value) -> Option<&str> {
    let relation = content.get("m.relates_to")?;
    if relation.get("rel_type")?.as_str()? != "m.thread" {
        return None;
    }
    relation.get("event_id")?.as_str()
}

async fn watch_thread_send(
    client: &Client,
    room_id: &str,
) -> Option<tokio::sync::broadcast::Receiver<RoomSendQueueUpdate>> {
    let room = RoomId::parse(room_id).ok().and_then(|id| client.get_room(&id))?;
    room.send_queue().subscribe().await.ok().map(|(_, updates)| updates)
}

const THREAD_SEND_WATCH_SECS: u64 = 600;

async fn wait_for_thread_send(
    mut updates: tokio::sync::broadcast::Receiver<RoomSendQueueUpdate>,
    root: &EventId,
) -> Option<OwnedEventId> {
    use matrix_sdk::send_queue::LocalEchoContent;
    use tokio::sync::broadcast::error::RecvError;
    let deadline = tokio::time::Instant::now()
        + std::time::Duration::from_secs(THREAD_SEND_WATCH_SECS);
    let mut ours: HashSet<matrix_sdk::ruma::OwnedTransactionId> = HashSet::new();
    loop {
        let update = match tokio::time::timeout_at(deadline, updates.recv()).await {
            Ok(Ok(update)) => update,
            Ok(Err(RecvError::Lagged(_))) => continue,
            Ok(Err(RecvError::Closed)) | Err(_) => return None,
        };
        match update {
            RoomSendQueueUpdate::NewLocalEvent(echo) => {
                let LocalEchoContent::Event { serialized_event, .. } = echo.content else {
                    continue;
                };
                let (raw, _event_type) = serialized_event.into_raw();
                let Ok(content) = serde_json::from_str::<serde_json::Value>(raw.json().get())
                else {
                    continue;
                };
                if thread_root_of_content(&content) == Some(root.as_str()) {
                    ours.insert(echo.transaction_id);
                }
            }
            RoomSendQueueUpdate::CancelledLocalEvent { transaction_id } => {
                ours.remove(&transaction_id);
            }
            RoomSendQueueUpdate::SentEvent { transaction_id, event_id }
                if ours.contains(&transaction_id) =>
            {
                return Some(event_id);
            }
            _ => {}
        }
    }
}

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
    if let Some(stamp) = crate::sync_trace_stamp_ms() {
        base["trace_sdk_ms"] = stamp.into();
    }
    fill_diff_json(base, diff, own_user, registry)
}

pub(crate) struct LiveTail<T> {
    items: Vector<T>,
    lost: bool,
}

pub(crate) trait RemoteEventItem {
    fn is_remote_event(&self) -> bool;
}

impl RemoteEventItem for Arc<TimelineItem> {
    fn is_remote_event(&self) -> bool {
        self.as_event().is_some_and(|event| !event.is_local_echo())
    }
}

impl<T: Clone + RemoteEventItem> LiveTail<T> {
    pub(crate) fn new(items: Vector<T>) -> Self {
        Self { items, lost: false }
    }

    pub(crate) fn classify_and_apply(
        &mut self,
        diff: &VectorDiff<T>,
        arrived_live: impl Fn(&T) -> bool,
    ) -> Option<VectorDiff<T>> {
        let live = self.classify(diff, arrived_live);
        self.apply(diff);
        live
    }

    fn classify(
        &self,
        diff: &VectorDiff<T>,
        arrived_live: impl Fn(&T) -> bool,
    ) -> Option<VectorDiff<T>> {
        if self.lost {
            return None;
        }
        let VectorDiff::Insert { index, value } = diff else { return None };
        let len = self.items.len();
        if *index > len || !value.is_remote_event() {
            return None;
        }
        if self.items.iter().rev().take(len - *index).any(|item| item.is_remote_event()) {
            return None;
        }
        let follows_remote =
            self.items.iter().rev().skip(len - *index).any(|item| item.is_remote_event());
        if !follows_remote && !arrived_live(value) {
            return None;
        }
        Some(VectorDiff::PushBack { value: value.clone() })
    }

    pub(crate) fn decrypts_in_place(
        &self,
        diff: &VectorDiff<T>,
        is_utd: impl Fn(&T) -> bool,
        event_id: impl Fn(&T) -> Option<String>,
    ) -> bool {
        if self.lost {
            return false;
        }
        let VectorDiff::Set { index, value } = diff else { return false };
        let Some(previous) = self.items.get(*index) else { return false };
        if !is_utd(previous) || is_utd(value) {
            return false;
        }
        match (event_id(previous), event_id(value)) {
            (Some(before), Some(after)) => before == after,
            _ => false,
        }
    }

    fn items(&self) -> Option<&Vector<T>> {
        (!self.lost).then_some(&self.items)
    }

    fn apply(&mut self, diff: &VectorDiff<T>) {
        if self.lost {
            return;
        }
        let len = self.items.len();
        let in_range = match diff {
            VectorDiff::Insert { index, .. } => *index <= len,
            VectorDiff::Set { index, .. } | VectorDiff::Remove { index } => *index < len,
            VectorDiff::Truncate { length } => *length <= len,
            _ => true,
        };
        if !in_range {
            self.lost = true;
            self.items.clear();
            return;
        }
        diff.clone().apply(&mut self.items);
    }
}

impl LiveTail<Arc<TimelineItem>> {
    fn thread_roots_flipped_by(
        &self,
        before: Option<u64>,
        after: Option<u64>,
    ) -> Vec<(usize, Arc<TimelineItem>)> {
        let Some(items) = self.items() else { return Vec::new() };
        items
            .iter()
            .enumerate()
            .filter(|(_, item)| {
                thread_latest_reply_ts(item)
                    .is_some_and(|ts| cover_flips(before, after, ts))
            })
            .map(|(index, item)| (index, Arc::clone(item)))
            .collect()
    }
}

fn thread_latest_reply_ts(item: &TimelineItem) -> Option<u64> {
    let TimelineItemContent::MsgLike(msg_like) = item.as_event()?.content() else {
        return None;
    };
    let summary = msg_like.thread_summary.as_ref()?;
    match &summary.latest_event {
        TimelineDetails::Ready(latest) => Some(u64::from(latest.timestamp.get())),
        _ => None,
    }
}

fn cover_flips(before: Option<u64>, after: Option<u64>, latest_reply_ts_ms: u64) -> bool {
    thread_latest_reply_read(false, false, latest_reply_ts_ms, before)
        != thread_latest_reply_read(false, false, latest_reply_ts_ms, after)
}

fn mark_live_insert<'a>(
    diff: &'a VectorDiff<Arc<TimelineItem>>,
    live_insert: Option<&'a VectorDiff<Arc<TimelineItem>>>,
    value: &mut serde_json::Value,
) -> &'a VectorDiff<Arc<TimelineItem>> {
    match live_insert {
        Some(as_append) => {
            if let Some(object) = value.as_object_mut() {
                object.insert("live_append".to_owned(), true.into());
            }
            as_append
        }
        None => diff,
    }
}

fn arrived_since_open(item: &Arc<TimelineItem>, opened_ms: u64, now_ms: u64) -> bool {
    crate::push_verdict::item_facts(item).is_some_and(|facts| match facts.age_ms {
        Some(age) => now_ms.saturating_sub(age) >= opened_ms,
        None => facts.timestamp_ms >= opened_ms,
    })
}

fn item_is_utd(item: &Arc<TimelineItem>) -> bool {
    item.as_event().is_some_and(|event| {
        matches!(
            event.content(),
            TimelineItemContent::MsgLike(msg_like)
                if matches!(msg_like.kind, MsgLikeKind::UnableToDecrypt(_))
        )
    })
}

fn item_event_id(item: &Arc<TimelineItem>) -> Option<String> {
    item.as_event().and_then(|event| event.event_id()).map(|id| id.to_string())
}

fn annotate_decrypted_set(
    diff: &VectorDiff<Arc<TimelineItem>>,
    value: &mut serde_json::Value,
    opened_ms: u64,
    now_ms: u64,
) {
    let VectorDiff::Set { value: item, .. } = diff else { return };
    if !arrived_since_open(item, opened_ms, now_ms) {
        if let Some(slot) = value.get_mut("item").and_then(|slot| slot.as_object_mut()) {
            slot.insert("backlog".to_owned(), true.into());
        }
    }
}

async fn annotate_snapshot_verdicts<'a, I>(
    room: &matrix_sdk::Room,
    fallback_verdicts: &crate::push_verdict::SyncVerdicts,
    items: I,
    snapshot: &mut [serde_json::Value],
) where
    I: IntoIterator<Item = &'a Arc<TimelineItem>>,
{
    let context = room.push_context().await.ok().flatten();
    for (item, slot) in items.into_iter().zip(snapshot.iter_mut()) {
        let Some(raw) = item.as_event().and_then(|event| event.original_json()) else {
            continue;
        };
        let verdict = crate::push_verdict::for_timeline_json(
            room, context.as_ref(), fallback_verdicts, raw.json(),
        ).await;
        crate::push_verdict::PushVerdict::write_room_mention(
            fallback_verdicts.room_mention_rule_matched(room, raw.json(), verdict).await,
            slot,
        );
        crate::push_verdict::PushVerdict::write(verdict, slot);
    }
}

async fn annotate_push_verdicts(
    room: &matrix_sdk::Room,
    fallback_verdicts: &crate::push_verdict::SyncVerdicts,
    context: &mut Option<Option<matrix_sdk::room::PushContext>>,
    diff: &VectorDiff<Arc<TimelineItem>>,
    value: &mut serde_json::Value,
) {
    for (item, slot) in crate::push_verdict::event_slots(diff, value) {
        let Some(event) = item.as_event() else { continue };
        if event.original_json().is_none() {
            continue;
        }
        if context.is_none() {
            *context = Some(room.push_context().await.ok().flatten());
        }
        let raw = event.original_json().expect("remote timeline item has original JSON");
        let verdict = crate::push_verdict::for_timeline_json(
            room, context.as_ref().and_then(Option::as_ref), fallback_verdicts, raw.json(),
        ).await;
        crate::push_verdict::PushVerdict::write_room_mention(
            fallback_verdicts.room_mention_rule_matched(room, raw.json(), verdict).await,
            slot,
        );
        crate::push_verdict::PushVerdict::write(verdict, slot);
    }
}

fn fill_diff_json(
    envelope: serde_json::Value,
    diff: &VectorDiff<Arc<TimelineItem>>,
    own_user: &str,
    registry: &TimelineRegistry,
) -> serde_json::Value {
    let room_id = envelope
        .get("room_id")
        .and_then(|value| value.as_str())
        .unwrap_or_default()
        .to_owned();
    let room_id = room_id.as_str();
    let base = |op: &str| {
        let mut v = envelope.clone();
        v["op"] = op.into();
        v
    };
    match diff {
        VectorDiff::Append { values } => {
            let mut v = base("append");
            v["items"] = values.iter().map(|i| item_to_json(i, room_id, own_user, registry)).collect();
            v
        }
        VectorDiff::PushBack { value } => {
            let mut v = base("push_back");
            v["item"] = item_to_json(value, room_id, own_user, registry);
            v
        }
        VectorDiff::PushFront { value } => {
            let mut v = base("push_front");
            v["item"] = item_to_json(value, room_id, own_user, registry);
            v
        }
        VectorDiff::Insert { index, value } => {
            let mut v = base("insert");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, room_id, own_user, registry);
            v
        }
        VectorDiff::Set { index, value } => {
            let mut v = base("set");
            v["index"] = (*index).into();
            v["item"] = item_to_json(value, room_id, own_user, registry);
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
            v["items"] = values.iter().map(|i| item_to_json(i, room_id, own_user, registry)).collect();
            v
        }
    }
}

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
        if event.original_json().is_some_and(is_thread_reply_fallback) {
            continue;
        }
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
            let _ = timeline.fetch_details_for_event(&event_id).await;
        }
    });
}

pub(crate) fn thread_latest_reply_read(
    sent_by_us: bool,
    threaded_receipt_on_latest: bool,
    latest_reply_ts_ms: u64,
    unthreaded_cover_ms: Option<u64>,
) -> bool {
    sent_by_us
        || threaded_receipt_on_latest
        || unthreaded_cover_ms.is_some_and(|cover| cover >= latest_reply_ts_ms)
}

async fn load_unthreaded_read_cover(room: &matrix_sdk::Room, own_user: &UserId) -> Option<u64> {
    use matrix_sdk::ruma::events::receipt::{ReceiptThread, ReceiptType as StoredReceipt};
    let mut cover: Option<u64> = None;
    let mut cache: Option<matrix_sdk::event_cache::RoomEventCache> = None;
    let mut _drop_handles = None;
    for receipt_type in [StoredReceipt::Read, StoredReceipt::ReadPrivate] {
        let Ok(Some((event_id, _receipt))) = room
            .load_user_receipt(receipt_type, ReceiptThread::Unthreaded, own_user)
            .await
        else {
            continue;
        };
        if cache.is_none() {
            let Ok((room_cache, handles)) = room.event_cache().await else { return cover };
            cache = Some(room_cache);
            _drop_handles = Some(handles);
        }
        let Some(room_cache) = cache.as_ref() else { continue };
        let Ok(Some(event)) = room_cache.find_event(&event_id).await else { continue };
        if let Some(ts) = event.timestamp() {
            let ts = u64::from(ts.get());
            cover = Some(cover.map_or(ts, |current| current.max(ts)));
        }
    }
    cover
}

fn item_to_json(
    item: &TimelineItem,
    room_id: &str,
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
            event_item_to_json(&item.unique_id().0, event, room_id, own_user, registry)
        }
    }
}

const READ_BY_CAP: usize = 16;

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
    entries.sort_by(|a, b| b.0.unwrap_or(0).cmp(&a.0.unwrap_or(0)));
    entries.truncate(READ_BY_CAP);
    let serialized = entries
        .into_iter()
        .map(|(ts, user_id)| json!({ "user_id": user_id, "ts": ts }))
        .collect();
    (serialized, total)
}

const REACTION_SENDER_CAP: usize = 16;

const REACTION_SENDER_MAX_BYTES: usize = 255;

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

const PROFILE_NAME_CAP: usize = 255;

fn bound_profile_name(name: &str) -> String {
    name.chars().take(PROFILE_NAME_CAP).collect()
}

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
        _ => None,
    }
}

pub(crate) fn state_sentence(kind: &str) -> &'static str {
    match kind {
        "m.room.create" => "room_created",
        "m.room.name" => "room_name_changed",
        "m.room.topic" => "room_topic_changed",
        "m.room.avatar" => "room_avatar_changed",
        "m.room.encryption" => "encryption_enabled",
        "m.room.tombstone" => "room_upgraded",
        "m.call" => "call_started",
        "m.call.video" => "video_call_started",
        kind if is_policy_rule_type(kind) => "policy_rule_updated",
        _ => "settings_updated",
    }
}

fn membership_sentence(change: Option<matrix_sdk_ui::timeline::MembershipChange>) -> &'static str {
    use matrix_sdk_ui::timeline::MembershipChange as M;
    match change {
        Some(M::Joined) => "member_joined",
        Some(M::Left) => "member_left",
        Some(M::Invited) => "member_invited",
        Some(M::InvitationAccepted) => "invitation_accepted",
        Some(M::Kicked) => "member_kicked",
        Some(M::Banned) | Some(M::KickedAndBanned) => "member_banned",
        Some(M::Unbanned) => "member_unbanned",
        Some(M::InvitationRejected) => "invitation_rejected",
        Some(M::InvitationRevoked) => "invitation_revoked",
        _ => "membership_changed",
    }
}

fn state_sentence_english(sentence: &str, actor: &str, target: &str) -> String {
    match sentence {
        "room_created" => format!("{actor} created the room."),
        "room_name_changed" => format!("{actor} changed the room name."),
        "room_name_removed" => format!("{actor} removed the room name."),
        "room_topic_removed" => format!("{actor} removed the room topic."),
        "room_topic_changed" => format!("{actor} changed the room topic."),
        "room_avatar_changed" => format!("{actor} changed the room avatar."),
        "encryption_enabled" => "Encryption was enabled.".to_owned(),
        "room_upgraded" => format!("{actor} upgraded this room."),
        "call_started" => format!("{actor} started a call."),
        "video_call_started" => format!("{actor} started a video call."),
        "policy_rule_updated" => format!("{actor} updated a moderation rule."),
        "space_parent_removed" => format!("{actor} removed this room from a space."),
        "policy_rule_removed" => format!("{actor} removed a moderation rule."),
        "member_joined" => format!("{target} joined the room."),
        "member_left" => format!("{target} left the room."),
        "member_invited" => format!("{actor} invited {target}."),
        "invitation_accepted" => format!("{target} accepted the invitation."),
        "member_kicked" => format!("{actor} removed {target} from the room."),
        "member_banned" => format!("{actor} banned {target}."),
        "member_unbanned" => format!("{actor} unbanned {target}."),
        "invitation_rejected" => format!("{target} rejected the invitation."),
        "invitation_revoked" => format!("{actor} revoked {target}'s invitation."),
        "membership_changed" => format!("Membership for {target} changed."),
        _ => format!("{actor} updated room settings."),
    }
}

fn state_row_text(kind: &str, actor: &str) -> String {
    state_sentence_english(state_sentence(kind), actor, "")
}

fn is_policy_rule_type(event_type: &str) -> bool {
    ["m.policy.rule.", "m.room.rule.", "org.matrix.mjolnir.rule."]
        .iter()
        .any(|prefix| {
            event_type
                .strip_prefix(prefix)
                .is_some_and(|rest| matches!(rest, "user" | "room" | "server"))
        })
}

fn failed_state_sentence(event_type: &str, error_display: &str) -> Option<&'static str> {
    if event_type == "m.space.parent" && error_display.contains("missing field `via`") {
        Some("space_parent_removed")
    } else if is_policy_rule_type(event_type) && error_display.contains("missing field `entity`") {
        Some("policy_rule_removed")
    } else if event_type == "m.room.name" && error_display.contains("missing field `name`") {
        Some("room_name_removed")
    } else if event_type == "m.room.topic" && error_display.contains("missing field `topic`") {
        Some("room_topic_removed")
    } else {
        None
    }
}

#[cfg(test)]
fn failed_state_parse_body(event_type: &str, error_display: &str, actor: &str) -> Option<String> {
    failed_state_sentence(event_type, error_display)
        .map(|sentence| state_sentence_english(sentence, actor, ""))
}

fn event_item_to_json(
    unique_id: &str,
    event: &EventTimelineItem,
    room_id: &str,
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
        if profile.display_name_ambiguous {
            out["sender_name_ambiguous"] = true.into();
        }
        if let Some(avatar) = &profile.avatar_url {
            out["sender_avatar_url"] = avatar.to_string().into();
        }
    }

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
                            "senders": reaction_senders_json(
                                senders.keys().map(|user| user.as_str()),
                                own_user,
                            ),
                        })
                    })
                    .collect();
                out["reactions"] = reactions.into();
            }
            // A thread reply carries `m.in_reply_to` with `is_falling_back`
            // only for clients without threads; it is not a quote the sender
            // chose. matrix-sdk-ui drops it in a thread-focused timeline but
            // keeps it in a live timeline that shows threaded events (the
            // Inline style), where it rendered as a quote card on every
            // reply. An explicit in-thread reply (is_falling_back false or
            // absent) still quotes. A local echo has no original JSON and keeps
            // the SDK's answer until the remote event replaces it.
            let reply_is_thread_fallback =
                event.original_json().is_some_and(is_thread_reply_fallback);
            if let Some(reply) =
                msg_like.in_reply_to.as_ref().filter(|_| !reply_is_thread_fallback)
            {
                out["reply_to_event_id"] = reply.event_id.to_string().into();
                if let TimelineDetails::Ready(embedded) = &reply.event {
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
                    let (reply_kind, reply_count) = content_kind(&embedded.content);
                    out["reply_to_kind"] = reply_kind.into();
                    if reply_count > 1 {
                        out["reply_to_count"] = reply_count.into();
                    }
                    if let TimelineItemContent::MsgLike(embedded_kind) =
                        &embedded.content
                    {
                        if let MsgLikeKind::Message(message) =
                            &embedded_kind.kind
                        {
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
                        let read = thread_latest_reply_read(
                            latest.sender.as_str() == own_user,
                            summary.public_read_receipt_event_id.as_deref()
                                == Some(&**latest_id)
                                || summary.private_read_receipt_event_id.as_deref()
                                    == Some(&**latest_id),
                            u64::from(latest.timestamp.get()),
                            registry.unthreaded_read_cover_ms(room_id),
                        );
                        out["thread_unread"] =
                            (!read && summary.num_replies > 0).into();
                    }
                }
            }
            match &msg_like.kind {
                MsgLikeKind::Message(message) => {
                    out["edited"] = message.is_edited().into();
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
                    let mut sources = fill_message_media(&mut out, message.msgtype(), &key);
                    if matches!(message.msgtype(), MessageType::Audio(_)) {
                        let wire = event
                            .original_json()
                            .and_then(|raw| raw.deserialize_as::<serde_json::Value>().ok());
                        if let (Some(wire), Some((_, primary))) = (wire, sources.first_mut()) {
                            if primary.thumbnail.is_none() {
                                primary.thumbnail = audio_thumbnail_from_wire(&wire);
                            }
                        }
                    }
                    if let Some((primary_key, primary)) = sources.first() {
                        out["media_key"] = primary_key.clone().into();
                        out["media_source_available"] = true.into();
                        out["media_thumb_available"] =
                            primary.thumbnail.is_some().into();
                    }
                    for (media_key, media) in sources {
                        registry.remember_media(media_key, media);
                    }
                    restore_raw_formatted_body(&mut out, event, message.is_edited());
                    if message.is_edited()
                        && std::env::var_os("LIGHTNING_EDIT_TRACE").is_some()
                    {
                        eprintln!(
                            "edit-trace stage=aggregated room={} event={} body_len={} formatted_len={} \
                             original_json={} latest_edit_json={} local_echo={}",
                            crate::redact_matrix_id(room_id),
                            crate::redact_matrix_id(out["event_id"].as_str().unwrap_or("")),
                            message.body().len(),
                            out["formatted_body"].as_str().map_or(0, str::len),
                            event.original_json().is_some(),
                            event.latest_edit_json().is_some(),
                            event.is_local_echo(),
                        );
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
                    if msg_like.thread_root.is_none() {
                        if let Some(root) =
                            event.original_json().and_then(cleartext_thread_root)
                        {
                            out["thread_root_id"] = root.into();
                        }
                    }
                }
                MsgLikeKind::Sticker(sticker) => {
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
                    let latest = state.latest_location();
                    let geo = latest.map(|b| b.geo_uri().to_owned())
                        .unwrap_or_default();
                    let description = state.description();
                    crate::location::fill_location(
                        &mut out,
                        &geo,
                        live_location_body(description),
                        description,
                        Some("m.self"),
                    );
                    out["locationLive"] = true.into();
                    out["locationLiveActive"] = state.is_live().into();
                }
            }
        }
        TimelineItemContent::MembershipChange(_) => {
            if let TimelineItemContent::MembershipChange(change) = event.content() {
                use matrix_sdk_ui::timeline::MembershipChange as M;
                let target = change.display_name().unwrap_or_else(|| change.user_id().to_string());
                let actor = event.sender().to_string();
                let sentence = membership_sentence(change.change());
                let text = state_sentence_english(sentence, &actor, &target);
                out["state_sentence"] = sentence.into();
                out["membership_change"] = match change.change() {
                    Some(M::Joined) | Some(M::InvitationAccepted) => "joined",
                    Some(M::Left) | Some(M::InvitationRejected) => "left",
                    Some(M::Invited) => "invited",
                    Some(M::Kicked) => "kicked",
                    Some(M::Banned) | Some(M::KickedAndBanned) => "banned",
                    Some(M::Unbanned) => "unbanned",
                    Some(M::InvitationRevoked) => "revoked",
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
            out["profile_avatar_changed"] =
                change.avatar_url_change().is_some().into();

            out["body"] = "".into();
        }
        TimelineItemContent::OtherState(state) => {
            let kind = state.content().event_type().to_string();
            let actor = event.sender().to_string();
            out["msgtype"] = "state".into();
            out["state_kind"] = kind.clone().into();
            out["state_sentence"] = state_sentence(&kind).into();
            out["body"] = state_row_text(&kind, &actor).into();
        }
        TimelineItemContent::FailedToParseMessageLike { .. } => {
            out["msgtype"] = "unsupported".into();
            out["body"] = "[unsupported event]".into();
        }
        TimelineItemContent::FailedToParseState { event_type, error, .. } => {
            let actor = event.sender().to_string();
            let event_type = event_type.to_string();
            match failed_state_sentence(&event_type, &error.to_string()) {
                Some(sentence) => {
                    out["msgtype"] = "state".into();
                    out["state_kind"] = event_type.into();
                    out["state_sentence"] = sentence.into();
                    out["body"] = state_sentence_english(sentence, &actor, "").into();
                }
                None => {
                    out["msgtype"] = "unsupported".into();
                    out["body"] = "[unsupported event]".into();
                }
            }
        }
        TimelineItemContent::CallInvite => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "invite".into();
            out["call_video"] = false.into();
            out["body"] = "".into();
        }
        TimelineItemContent::RtcNotification { call_intent, declined_by } => {
            out["msgtype"] = "call".into();
            out["call_kind"] = "notification".into();
            out["call_video"] = matches!(call_intent, Some(CallIntent::Video)).into();
            out["body"] = "".into();
            if !declined_by.is_empty() {
                out["call_declined_count"] = declined_by.len().into();
            }
        }
    }

    out
}

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

fn raw_displayed_formatted_body(
    is_edited: bool,
    original: Option<&serde_json::Value>,
    latest_edit: Option<&serde_json::Value>,
) -> Option<String> {
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

fn media_display_name(body: &str, filename: Option<&str>) -> String {
    match filename {
        Some(name) if !name.trim().is_empty() => name.to_owned(),
        _ => body.to_owned(),
    }
}

pub(crate) const GALLERY_MSGTYPES: [&str; 2] = ["dm.filament.gallery", "m.gallery"];

pub(crate) const GALLERY_ITEM_CAP: usize = 32;

pub(crate) fn is_gallery_msgtype(msgtype: &str) -> bool {
    GALLERY_MSGTYPES.contains(&msgtype)
}

pub(crate) struct Gallery {
    pub caption: String,
    pub formatted_caption: Option<String>,
    pub items: Vec<MessageType>,
}

impl Gallery {
    pub fn all_images(&self) -> bool {
        self.items.iter().all(|item| matches!(item, MessageType::Image(_)))
    }
}

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

pub(crate) fn gallery_item_key(row_key: &str, index: usize) -> String {
    format!("{row_key}#item{index}")
}

fn gallery_primary_index(gallery: &Gallery) -> usize {
    gallery
        .items
        .iter()
        .position(|item| matches!(item, MessageType::Image(_)))
        .unwrap_or(0)
}

pub(crate) fn audio_thumbnail_from_wire(wire: &serde_json::Value) -> Option<MediaSource> {
    let info = wire.get("content")?.get("info")?;
    if let Some(file) = info.get("thumbnail_file").filter(|v| v.is_object()) {
        if let Ok(file) = serde_json::from_value::<
            matrix_sdk::ruma::events::room::EncryptedFile,
        >(file.clone())
        {
            return Some(MediaSource::Encrypted(Box::new(file)));
        }
    }
    let url = info.get("thumbnail_url")?.as_str()?;
    if url.len() > 512 {
        return None;
    }
    let mxc = matrix_sdk::ruma::OwnedMxcUri::from(url);
    mxc.is_valid().then(|| MediaSource::Plain(mxc))
}

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

pub(crate) struct MessageSummary {
    pub kind: &'static str,
    pub count: usize,
    pub text: String,
}

pub(crate) fn voice_message_label(
    content: &matrix_sdk::ruma::events::room::message::AudioMessageEventContent,
) -> Option<String> {
    content.voice.as_ref().map(|_| "Voice message".to_owned())
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
        MessageType::Audio(c) => match voice_message_label(c) {
            Some(text) => MessageSummary { kind: "audio", count: 0, text },
            None => media("audio", &c.body, c.filename.as_deref()),
        },
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
            set_formatted_body(out, content.formatted.as_ref());
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
            set_formatted_body(out, content.formatted.as_ref());
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
            let filename = media_display_name(&content.body, content.filename.as_deref());
            out["msgtype"] = "audio".into();
            out["body"] = content.body.clone().into();
            set_formatted_body(out, content.formatted.as_ref());
            out["media_filename"] = filename.clone().into();
            if let MediaSource::Plain(mxc) = &content.source {
                out["media_mxc"] = mxc.to_string().into();
            }
            if content.voice.is_some() {
                out["media_voice"] = true.into();
            }
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
            let filename = media_display_name(&content.body, content.filename.as_deref());
            out["msgtype"] = "video".into();
            out["body"] = content.body.clone().into();
            set_formatted_body(out, content.formatted.as_ref());
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
            let body = other.body();
            out["body"] = if body.trim().is_empty() {
                "[unsupported event]".into()
            } else {
                body.to_owned().into()
            };
            None
        }
    }
}

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

    let peak = vals.iter().copied().max().unwrap_or(0);
    if peak == 0 {
        return vec![0; buckets];
    }
    vals.into_iter().map(|v| (v * 100) / peak).collect()
}

/// Strip the legacy rich-reply fallback (spec "Rich replies", removed from
/// the spec in v1.13 but still sent by many clients) from a plain body, for
/// every surface that shows the RAW body as one line of text: room-list
/// previews, notifications, the local search index.
///
/// Strict on purpose, so a message that merely begins with a quote survives:
/// the first line must open `> <@` (or `> * <@` for an emote), every
/// following `> `-prefixed or bare `>` line is part of the block (nested
/// quotes included), and the block must be closed by an EMPTY line. A body
/// without that separator is returned unchanged. The C++ twin is
/// `matrix::preview::stripReplyFallback`; keep them in step.
pub(crate) fn strip_reply_fallback(body: &str) -> &str {
    if !(body.starts_with("> <@") || body.starts_with("> * <@")) {
        return body;
    }
    let mut rest = body;
    loop {
        let (line, tail) = match rest.split_once('\n') {
            Some(parts) => parts,
            None => return body,
        };
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.is_empty() {
            return tail;
        }
        if line == ">" || line.starts_with("> ") {
            rest = tail;
        } else {
            return body;
        }
    }
}

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

fn set_mention_plain_body(msgtype: &mut MessageType, markdown: &str) {
    let plain = mention_plain_body(markdown);
    if plain == markdown {
        return;
    }
    match msgtype {
        MessageType::Text(text) => text.body = plain,
        MessageType::Emote(emote) => emote.body = plain,
        _ => {}
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BodyFormat {
    Markdown,
    Plain,
    Html,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct SendBodySpec {
    pub(crate) format: BodyFormat,
    pub(crate) html: String,
    pub(crate) emote: bool,
    pub(crate) emoticons: Vec<OutgoingEmoticon>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct OutgoingEmoticon {
    pub code: String,
    pub mxc: String,
    pub alt: String,
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
    let mut emoticons: Vec<OutgoingEmoticon> = Vec::new();
    if let Some(map) = value.get("emoticons").and_then(|v| v.as_object()) {
        for (code, target) in map {
            let (mxc, body) = match target {
                serde_json::Value::String(mxc) => (mxc.as_str(), ""),
                serde_json::Value::Object(o) => (
                    o.get("url").and_then(|v| v.as_str()).unwrap_or(""),
                    o.get("body").and_then(|v| v.as_str()).unwrap_or(""),
                ),
                _ => continue,
            };
            if !crate::stickers::is_valid_mxc(mxc) {
                continue;
            }
            if code.len() < 3 || !code.starts_with(':') || !code.ends_with(':') {
                continue;
            }
            let body: String = body
                .chars()
                .map(|c| if c.is_control() { ' ' } else { c })
                .collect::<String>()
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ")
                .chars()
                .take(160)
                .collect();
            let alt = if body.is_empty() {
                code.trim_matches(':').to_owned()
            } else {
                body
            };
            emoticons.push(OutgoingEmoticon { code: code.clone(), mxc: mxc.to_owned(), alt });
        }
        emoticons.sort_by(|a, b| b.code.len().cmp(&a.code.len()));
    }
    Ok(SendBodySpec { format, html, emote, emoticons })
}

fn substitute_emoticons(html: &str, emoticons: &[OutgoingEmoticon]) -> String {
    if emoticons.is_empty() {
        return html.to_owned();
    }
    let mut out = String::with_capacity(html.len());
    let bytes = html.as_bytes();
    let mut i = 0usize;
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
            for emoticon in emoticons {
                let (code, mxc) = (&emoticon.code, &emoticon.mxc);
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
                    out.push_str(&html_escape_attr(mxc));
                    out.push_str("\" alt=\"");
                    out.push_str(&html_escape_attr(&emoticon.alt));
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
        let step = html[i..].chars().next().map(|c| c.len_utf8()).unwrap_or(1);
        out.push_str(&html[i..i + step]);
        i += step;
    }
    out
}

fn html_escape_attr(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

fn strip_non_mxc_images(html: &str) -> String {
    let mut out = String::with_capacity(html.len());
    let mut rest = html;
    loop {
        let Some(found) = find_img_tag(rest) else {
            out.push_str(rest);
            return out;
        };
        out.push_str(&rest[..found]);
        let tail = &rest[found..];
        let Some(close) = tail.find('>') else { return out };
        let tag = &tail[..=close];
        if img_src_is_mxc(tag) {
            out.push_str(tag);
        }
        rest = &tail[close + 1..];
    }
}

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

fn img_src_is_mxc(tag: &str) -> bool {
    let lower = tag.to_ascii_lowercase();
    let mut from = 0usize;
    while let Some(at) = lower[from..].find("src") {
        let start = from + at;
        let before_ok = start == 0
            || matches!(lower.as_bytes()[start - 1],
                        b' ' | b'\t' | b'\n' | b'\r' | b'<' | b'"' | b'\'');
        let after = lower[start + 3..].trim_start();
        if before_ok && after.starts_with('=') {
            let value = after[1..].trim_start();
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

pub(crate) fn sanitize_outgoing_formatted(msgtype: &mut MessageType) {
    use matrix_sdk::ruma::html::{
        ElementAttributesSchemes, Html, ListBehavior, PropertiesNames,
        SanitizerConfig,
    };
    let slot = match msgtype {
        MessageType::Text(text) => &mut text.formatted,
        MessageType::Emote(emote) => &mut emote.formatted,
        _ => return,
    };
    let Some(formatted) = slot.as_mut() else { return };
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
    if formatted.body.trim().is_empty() {
        *slot = None;
    }
}

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
        return;
    }
    *formatted = Some(FormattedBody::html(replaced));
}

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
        BodyFormat::Markdown => {
            set_mention_plain_body(&mut message.msgtype, body);
            sanitize_outgoing_formatted(&mut message.msgtype);
        }
        BodyFormat::Plain => {}
        BodyFormat::Html => sanitize_outgoing_formatted(&mut message.msgtype),
    }
    apply_emoticons(&mut message.msgtype, spec);
    if !spec.emoticons.is_empty() {
        sanitize_outgoing_formatted(&mut message.msgtype);
    }
    message
}

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
        BodyFormat::Markdown => {
            set_mention_plain_body(&mut message.msgtype, body);
            sanitize_outgoing_formatted(&mut message.msgtype);
        }
        BodyFormat::Plain => {}
        BodyFormat::Html => sanitize_outgoing_formatted(&mut message.msgtype),
    }
    apply_emoticons(&mut message.msgtype, spec);
    if !spec.emoticons.is_empty() {
        sanitize_outgoing_formatted(&mut message.msgtype);
    }
    message
}

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

pub(crate) struct PollView {
    pub question: String,
    pub disclosed: bool,
    pub max_selections: u64,
    pub answers: Vec<(String, String)>,
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

fn thread_latest_kind(content: &TimelineItemContent) -> &'static str {
    content_kind(content).0
}

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

fn content_preview(content: &TimelineItemContent) -> String {
    content_preview_capped(content, 80)
}

fn reply_preview(content: &TimelineItemContent) -> String {
    let preview = content_preview_capped(content, 320);
    if is_emote_content(content) {
        format!("* {preview}")
    } else {
        preview
    }
}

fn is_emote_content(content: &TimelineItemContent) -> bool {
    matches!(
        content,
        TimelineItemContent::MsgLike(msg_like)
            if matches!(
                &msg_like.kind,
                MsgLikeKind::Message(message)
                    if matches!(message.msgtype(), MessageType::Emote(_))
            )
    )
}

fn content_preview_capped(content: &TimelineItemContent, max: usize) -> String {
    let text = match content {
        TimelineItemContent::MsgLike(msg_like) => match &msg_like.kind {
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
    if text.chars().count() > max {
        let mut out: String = text.chars().take(max).collect();
        out.push('\u{2026}');
        out
    } else {
        text
    }
}

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

pub(crate) fn redaction_error_category(
    kind: Option<&matrix_sdk::ruma::api::error::ErrorKind>,
    text: &str,
) -> &'static str {
    use matrix_sdk::ruma::api::error::ErrorKind;
    match kind {
        Some(ErrorKind::LimitExceeded(_)) => "rate_limited",
        Some(ErrorKind::Forbidden) => "forbidden",
        Some(ErrorKind::NotFound) => "not_found",
        _ => crate::rooms::classify_room_error(text),
    }
}

pub(crate) fn redaction_finished_payload(
    room_id: &str,
    event_id: &str,
    room_generation: u64,
    lifecycle: u64,
    category: Option<&'static str>,
) -> serde_json::Value {
    json!({
        "type": "timeline_redaction_finished",
        "room_id": room_id,
        "event_id": event_id,
        "room_generation": room_generation,
        "lifecycle": lifecycle,
        "ok": category.is_none(),
        "category": category.unwrap_or_default(),
    })
}

fn classify_backup_error(err: &matrix_sdk::Error) -> BackupOutcome {
    use matrix_sdk::ruma::api::error::ErrorKind;
    match err.client_api_error_kind() {
        Some(ErrorKind::NotFound) => BackupOutcome::Definitive,
        Some(ErrorKind::Unrecognized) | Some(ErrorKind::Forbidden) => {
            BackupOutcome::PermanentRefusal
        }
        _ => BackupOutcome::Inconclusive,
    }
}

#[derive(Clone, Copy)]
pub(crate) enum RecoveryScope {
    Room(u64),
    Thread(u64),
}

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
                        "inconclusive": inconclusive,
                        "lifecycle": lifecycle,
                    }),
                );
            }
        };
        let backups = client.encryption().backups();
        if !backups.are_enabled().await {
            if registry.mark_backup_attempt("\u{1f}auto-recovery-no-backup") {
                emit("skipped_no_backup_key", 0, 0);
            }
            return;
        }
        let Ok(room_ref) = RoomId::parse(&room_id) else { return };
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
        tokio::time::sleep(SDK_KEY_RECOVERY_GRACE).await;
        if !current(&registry) {
            return;
        }
        let asked = wanted.len();
        let wanted =
            still_undecryptable(wanted, &utd_session_ids(&timeline).await);
        if wanted.is_empty() {
            emit("resolved_before_fetch", asked, 0);
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
        if !current(&registry) {
            return;
        }
        timeline.retry_decryption(wanted.iter().cloned()).await;
        if !current(&registry) {
            return;
        }
        emit(
            if downloaded > 0 {
                "ok"
            } else if no_decryption_key > 0 {
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

const SDK_KEY_RECOVERY_GRACE: std::time::Duration =
    std::time::Duration::from_secs(2);

fn still_undecryptable(wanted: Vec<String>, undecryptable_now: &[String]) -> Vec<String> {
    wanted
        .into_iter()
        .filter(|session| undecryptable_now.contains(session))
        .collect()
}

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

fn cleartext_thread_root(
    raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>,
) -> Option<String> {
    if raw.get_field::<String>("type").ok().flatten().as_deref()
        != Some("m.room.encrypted")
    {
        return None;
    }
    let content = raw.get_field::<serde_json::Value>("content").ok().flatten()?;
    let relation = content.get("m.relates_to")?;
    if relation.get("rel_type")?.as_str()? != "m.thread" {
        return None;
    }
    let root = relation.get("event_id")?.as_str()?;
    matrix_sdk::ruma::EventId::parse(root).ok().map(|id| id.to_string())
}

fn is_thread_reply_fallback(raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>) -> bool {
    let Some(content) = raw.get_field::<serde_json::Value>("content").ok().flatten() else {
        return false;
    };
    let Some(relation) = content.get("m.relates_to") else { return false };
    relation.get("rel_type").and_then(|v| v.as_str()) == Some("m.thread")
        && relation.get("is_falling_back").and_then(|v| v.as_bool()) == Some(true)
}

fn decrypted_thread_root(
    raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>,
) -> Option<String> {
    let content = raw.get_field::<serde_json::Value>("content").ok().flatten()?;
    let relation = content.get("m.relates_to")?;
    if relation.get("rel_type")?.as_str()? != "m.thread" {
        return None;
    }
    relation.get("event_id")?.as_str().map(str::to_owned)
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
        backup_attempt_allowed, backup_attempt_backoff, failed_state_parse_body,
        failed_state_sentence, find_img_tag, membership_sentence, state_sentence,
        state_sentence_english,
        is_rtc_membership_event, raw_displayed_formatted_body,
        sessions_by_room_from_import, state_row_text, substitute_emoticons,
        attachment_config, parse_reply_target, TimelineRegistry, MAX_BACKUP_ATTEMPTS,
        still_undecryptable, SDK_KEY_RECOVERY_GRACE,
        redaction_error_category, redaction_finished_payload,
    };
    use matrix_sdk::ruma::events::AnySyncTimelineEvent;
    use std::collections::{BTreeMap, BTreeSet, VecDeque};

    #[test]
    fn a_failed_redaction_is_reported_as_a_category_never_as_text() {
        use matrix_sdk::ruma::api::error::{ErrorKind, LimitExceededErrorData};
        let limited = ErrorKind::LimitExceeded(LimitExceededErrorData::new());
        assert_eq!(redaction_error_category(Some(&limited), "anything"), "rate_limited");
        assert_eq!(redaction_error_category(Some(&ErrorKind::Forbidden), "x"), "forbidden");
        assert_eq!(redaction_error_category(Some(&ErrorKind::NotFound), "x"), "not_found");
        assert_eq!(
            redaction_error_category(None, "the server returned an error: [429 / ] Too Many Requests"),
            "rate_limited");
        assert_eq!(redaction_error_category(None, "connection reset by peer"), "network");

        let secret = "https://matrix.example.org/_matrix/client/v3/rooms/!r/redact/$e?token=abc";
        let failed = redaction_finished_payload(
            "!r:example.org",
            "$e:example.org",
            7,
            3,
            Some(redaction_error_category(None, secret)),
        );
        assert_eq!(failed["type"], "timeline_redaction_finished");
        assert_eq!(failed["ok"], false);
        assert_eq!(failed["category"], "network");
        assert!(failed.get("error").is_none());
        assert!(!failed.to_string().contains("token=abc"));
        assert!(!failed.to_string().contains("matrix.example.org"));

        let ok = redaction_finished_payload("!r:example.org", "$e:example.org", 7, 3, None);
        assert_eq!(ok["ok"], true);
        assert_eq!(ok["category"], "");
        assert_eq!(ok["lifecycle"], 3);
    }

    #[test]
    fn an_attachment_config_carries_the_reply_target() {
        let target = parse_reply_target("$target:example.org").unwrap();
        assert!(target.is_some());
        let config = attachment_config(None, None, Some("look".to_owned()), target.clone());
        assert_eq!(config.in_reply_to, target);
        assert_eq!(
            config.caption.as_ref().map(|c| c.body.as_str()),
            Some("look"),
            "a caption must survive alongside the reply target"
        );
        let plain = attachment_config(None, None, Some(String::new()), None);
        assert!(plain.in_reply_to.is_none());
        assert!(plain.caption.is_none());
    }

    #[test]
    fn the_reply_target_is_empty_valid_or_an_error_never_silently_dropped() {
        assert_eq!(parse_reply_target("").unwrap(), None);
        assert_eq!(parse_reply_target("   ").unwrap(), None);
        assert_eq!(
            parse_reply_target(" $abc:example.org ").unwrap().unwrap().as_str(),
            "$abc:example.org"
        );
        assert!(parse_reply_target("not an event id").is_err());
        assert!(parse_reply_target("!room:example.org").is_err());
    }

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

        assert_eq!(
            raw_displayed_formatted_body(false, Some(&original), None).as_deref(),
            Some("<strong>old</strong>"),
        );
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), Some(&edit)).as_deref(),
            Some("<strong>new</strong>"),
            "an edited message rendered pre-edit HTML",
        );
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), None),
            None,
            "a local-echo edit fell back to the original's HTML",
        );
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
        let opaque = serde_json::json!({ "content": { "algorithm": "m.megolm.v1.aes-sha2" } });
        assert_eq!(
            raw_displayed_formatted_body(true, Some(&original), Some(&opaque)),
            None
        );
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

    #[test]
    fn key_arrival_routes_its_sessions_to_the_context_views_room() {
        use super::sessions_for_room;
        let by_room = vec![
            ("!a:x".to_owned(), vec!["s1".to_owned(), "s2".to_owned()]),
            ("!b:x".to_owned(), vec!["s3".to_owned()]),
        ];
        assert_eq!(sessions_for_room(&by_room, "!a:x"), ["s1", "s2"]);
        assert_eq!(sessions_for_room(&by_room, "!b:x"), ["s3"]);
        assert!(sessions_for_room(&by_room, "!c:x").is_empty());
        assert!(sessions_for_room(&[], "!a:x").is_empty());
    }

    #[test]
    fn both_decryption_retry_paths_reach_the_context_timeline() {
        let src = include_str!("timeline.rs");
        let body = |def: &str, end: &str| -> String {
            let after = src
                .split(def)
                .nth(1)
                .unwrap_or_else(|| panic!("{def} not found"));
            let cut = after.find(end).unwrap_or(after.len());
            after[..cut].to_owned()
        };
        let import_def = concat!("pub async fn ", "retry_decryption_after_import(");
        let manual_def = concat!("pub fn ", "retry_visible_decryption(");
        let after_import = body(import_def, "\n}\n");
        let manual = body(manual_def, import_def);

        assert!(
            after_import.contains("active_context")
                && after_import.contains("sessions_for_room(")
                && after_import.contains(".retry_decryption("),
            "retry_decryption_after_import no longer retries the context view",
        );
        assert!(
            manual.contains("context_timeline_for(")
                && manual.contains("utd_session_ids(context)")
                && manual.contains("context.retry_decryption("),
            "retry_visible_decryption no longer retries the context view",
        );
    }

    #[test]
    fn the_fallback_pass_skips_sessions_the_sdk_already_recovered() {
        let wanted = vec!["a".to_owned(), "b".to_owned(), "c".to_owned()];
        let now = vec!["c".to_owned(), "x".to_owned(), "a".to_owned()];
        assert_eq!(still_undecryptable(wanted.clone(), &now), vec!["a", "c"]);
        assert!(still_undecryptable(wanted, &[]).is_empty());
        assert!(SDK_KEY_RECOVERY_GRACE > std::time::Duration::from_millis(100));
    }

    #[test]
    fn the_backup_retry_schedule_is_the_one_documented() {
        let secs = |n: u32| backup_attempt_backoff(n).as_secs();
        assert_eq!(secs(0), 30);
        assert_eq!(secs(1), 60);
        assert_eq!(secs(2), 120);
        assert_eq!(secs(3), 240);
        assert_eq!(secs(4), 480);
        assert_eq!(secs(5), 960);
        assert_eq!(secs(6), 1920);
        assert_eq!(secs(7), 1920);
        assert_eq!(secs(50), 1920);
        assert!(MAX_BACKUP_ATTEMPTS > 6,
                "the 32-minute ceiling must be reachable before the budget \
                 runs out, or it is decoration");

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
        assert_eq!(
            sends, 11,
            "expected 11 queue-backed sends in timeline.rs; found {sends}. \
             If a send was added or removed, update this number AND confirm \
             every site is still guarded."
        );
    }

    #[test]
    fn a_thread_edit_checks_the_thread_generation() {
        let source = include_str!("timeline.rs");
        let after = source
            .split("pub fn edit(")
            .nth(1)
            .expect("edit() not found — the scan is reading the wrong file");
        let bound = "\n    pub fn toggle_reaction(";
        assert!(
            after.contains(bound),
            "edit() is no longer followed by toggle_reaction — re-bound this \
             scan before trusting it"
        );
        let body = after.split(bound).next().unwrap();

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
        assert!(
            !body.contains("let Some((timeline, room_gen, lifecycle))"),
            "edit() binds the resolved generation as `room_gen`, which is a \
             thread generation in the thread branch"
        );
    }

    #[test]
    fn a_thread_attachment_result_is_gated_on_the_lifecycle_not_the_thread() {
        let source = include_str!("timeline.rs");
        let start = source
            .find(concat!("pub fn send_thread", "_attachment("))
            .expect("send_thread_attachment is gone");
        let end = start
            + source[start..]
                .find("\"type\": \"attachment_send_result\"")
                .expect("the thread attachment result is gone");
        let body = &source[start..end];
        assert!(
            body.contains("registry.lifecycle_current(lifecycle)"),
            "the thread attachment result is not gated on the lifecycle"
        );
        assert!(
            !body.contains("thread_current("),
            "the thread attachment result is gated on the THREAD generation: \
             a re-subscribed thread orphans its chip, image prep and voice file"
        );
    }

    #[test]
    fn retry_send_re_enables_before_unwedging() {
        let source = include_str!("timeline.rs");
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

    #[test]
    fn an_img_scan_does_not_panic_across_a_code_point() {
        assert_eq!(find_img_tag("<a\u{65e5}b"), None);
        assert_eq!(find_img_tag("x<img src=y>"), Some(1));
        assert_eq!(find_img_tag("<image>"), None);
        assert_eq!(find_img_tag("<IMG/>"), Some(0));
        assert_eq!(find_img_tag("<im"), None);
    }

    #[test]
    fn an_emoticon_scan_does_not_panic_on_a_long_multibyte_body() {
        let body = "\u{65e5}".repeat(54);
        let emoticons = vec![super::OutgoingEmoticon {
            code: ":tada:".to_owned(), mxc: "mxc://x/y".to_owned(), alt: "tada".to_owned() }];
        let out = substitute_emoticons(&format!("{body}:tada:"), &emoticons);
        assert!(out.contains("mxc://x/y"), "the emoticon was not substituted");
    }

    #[test]
    fn what_an_attempt_established_decides_whether_it_cost_anything() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let later = |h: u64| {
            std::time::Instant::now()
                .checked_add(std::time::Duration::from_secs(h * 3600))
                .expect("a few hours must be representable")
        };

        let transient = "!room:example.org\u{1f}TRANSIENT";
        assert!(registry.mark_backup_attempt(transient));
        for hour in 1..=12 {
            registry.record_backup_outcome(transient, super::BackupOutcome::Inconclusive);
            assert!(
                registry.mark_backup_attempt_at(transient, later(hour)),
                "hour {hour}: an inconclusive failure must never exhaust the budget"
            );
        }

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

        let refused = "!room:example.org\u{1f}REFUSED";
        assert!(registry.mark_backup_attempt(refused));
        registry.record_backup_outcome(refused, super::BackupOutcome::PermanentRefusal);
        assert!(
            !registry.mark_backup_attempt_at(refused, later(99)),
            "a permanent refusal must not be retried, however much time passes"
        );
        registry.clear_backup_attempt("!room:example.org");
        assert!(registry.mark_backup_attempt(refused));
    }

    #[test]
    fn a_failing_server_is_asked_less_often_and_never_for_ever() {
        let huge = std::time::Duration::from_secs(u32::MAX as u64);
        let secs = std::time::Duration::from_secs;

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

        assert!(backup_attempt_allowed(8, 0, huge));
        assert!(backup_attempt_allowed(99, 0, huge));

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
        assert!(!backup_attempt_allowed(99, MAX_BACKUP_ATTEMPTS, huge));
    }

    #[test]
    fn clearing_a_room_forgets_its_per_session_attempts_too() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let room = "!room:example.org";
        let session = format!("{room}\u{1f}SESSIONID");
        let other_room_session = "!other:example.org\u{1f}SESSIONID";

        assert!(registry.mark_backup_attempt(room));
        assert!(registry.mark_backup_attempt(&session));
        assert!(registry.mark_backup_attempt(other_room_session));
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
        let other = "!room\u{1f}$event\u{1f}\u{2764}";
        assert!(registry.begin_reaction(other));

        registry.end_reaction(target);
        assert!(
            registry.begin_reaction(target),
            "the slot must be reusable once the toggle has completed"
        );
    }

    #[test]
    fn no_timeout_wraps_a_call_that_queues_a_message() {
        let source = include_str!("timeline.rs");
        let wrapped = ["bounded(timeline.send", "bounded(room.send", "bounded(queue.send"];
        let guard = format!("{}{}", "fn no_timeout_wraps", "_a_call");
        let production = source.split(&guard).next().unwrap();
        for needle in wrapped {
            assert!(!production.contains(needle), "{needle} can fail a sent message");
        }
    }

    #[test]
    fn every_send_path_goes_through_the_ordered_lane() {
        let source = include_str!("timeline.rs");
        for name in [
            "pub fn send_text(",
            "pub fn send_reply(",
            "pub fn send_attachment(",
            "pub fn send_thread_text(",
            "pub fn send_thread_attachment(",
            "fn send_content_to_timeline(",
        ] {
            let body = source
                .split(name)
                .nth(1)
                .unwrap_or_else(|| panic!("{name} is gone"))
                .split("\n    pub fn ")
                .next()
                .unwrap();
            let body = body.split("\n    fn ").next().unwrap();
            assert!(
                !body.contains("runtime.spawn("),
                "{name} spawns its send directly; tasks then race to the send queue"
            );
            assert!(body.contains("self.spawn_ordered("), "{name} bypasses the lane");
        }
    }

    #[test]
    fn ordered_jobs_run_in_call_order_per_key_and_do_not_block_other_keys() {
        let registry = Arc::new(TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new()))));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(4)
            .enable_all()
            .build()
            .expect("runtime");
        let seen = Arc::new(Mutex::new(Vec::new()));
        let (gate_tx, gate_rx) = tokio::sync::oneshot::channel::<()>();
        let (first_tx, first_rx) = tokio::sync::oneshot::channel::<()>();
        {
            let seen = Arc::clone(&seen);
            registry.spawn_ordered(&runtime, "room".to_owned(), async move {
                let _ = first_rx.await;
                seen.lock().unwrap().push(0u64);
            });
        }
        registry.spawn_ordered(&runtime, "stalled".to_owned(), async move {
            let _ = gate_rx.await;
        });
        for n in 1..50u64 {
            let seen = Arc::clone(&seen);
            registry.spawn_ordered(&runtime, "room".to_owned(), async move {
                seen.lock().unwrap().push(n);
            });
        }
        let reached = Arc::new(Mutex::new(false));
        let flag = Arc::clone(&reached);
        registry.spawn_ordered(&runtime, "other".to_owned(), async move {
            *flag.lock().unwrap() = true;
        });
        runtime.block_on(async {
            for _ in 0..200 {
                if *reached.lock().unwrap() {
                    break;
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
            }
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        });
        assert!(seen.lock().unwrap().is_empty(), "a later job overtook the first");
        assert!(*reached.lock().unwrap(), "a stalled lane blocked another key");
        first_tx.send(()).unwrap();
        runtime.block_on(async {
            for _ in 0..200 {
                if seen.lock().unwrap().len() == 50 {
                    break;
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
            }
        });
        assert_eq!(*seen.lock().unwrap(), (0..50).collect::<Vec<_>>());
        registry.shutdown(&runtime);
        assert!(registry.send_lanes.lock().unwrap().is_empty());
        drop(gate_tx);
    }

    #[test]
    fn a_panicking_job_does_not_wedge_its_lane() {
        let registry = Arc::new(TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new()))));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("runtime");
        registry.spawn_ordered(&runtime, "room".to_owned(), async {
            panic!("send job panic");
        });
        let ran = Arc::new(Mutex::new(0u32));
        for _ in 0..2 {
            let ran = Arc::clone(&ran);
            registry.spawn_ordered(&runtime, "room".to_owned(), async move {
                *ran.lock().unwrap() += 1;
            });
        }
        runtime.block_on(async {
            for _ in 0..200 {
                if *ran.lock().unwrap() == 2 {
                    break;
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
            }
        });
        assert_eq!(*ran.lock().unwrap(), 2, "jobs after a panic were dropped");
        let later = Arc::clone(&ran);
        registry.spawn_ordered(&runtime, "room".to_owned(), async move {
            *later.lock().unwrap() += 1;
        });
        runtime.block_on(async {
            for _ in 0..200 {
                if *ran.lock().unwrap() == 3 {
                    break;
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
            }
        });
        assert_eq!(*ran.lock().unwrap(), 3, "the lane stayed wedged after a panic");
    }

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

    #[test]
    fn every_state_kind_crosses_as_a_closed_sentence() {
        for (kind, sentence) in [
            ("m.room.create", "room_created"),
            ("m.room.name", "room_name_changed"),
            ("m.room.topic", "room_topic_changed"),
            ("m.room.avatar", "room_avatar_changed"),
            ("m.room.encryption", "encryption_enabled"),
            ("m.room.tombstone", "room_upgraded"),
            ("m.call", "call_started"),
            ("m.call.video", "video_call_started"),
            ("m.policy.rule.user", "policy_rule_updated"),
            ("org.matrix.mjolnir.rule.server", "policy_rule_updated"),
            ("org.example.custom", "settings_updated"),
            ("m.policy.rule.userx", "settings_updated"),
            ("m.room.name.extra", "settings_updated"),
        ] {
            assert_eq!(state_sentence(kind), sentence, "{kind}");
        }
        assert_eq!(
            failed_state_sentence("m.space.parent", "missing field `via` at line 1 column 2"),
            Some("space_parent_removed")
        );
        assert_eq!(
            failed_state_sentence("m.policy.rule.room", "missing field `entity`"),
            Some("policy_rule_removed")
        );
        assert_eq!(failed_state_sentence("m.space.parent", "invalid type"), None);
    }

    #[test]
    fn removing_a_room_name_or_topic_is_a_state_sentence_not_unsupported() {
        use matrix_sdk::ruma::events::AnySyncStateEvent;
        for (kind, _field, sentence, english) in [
            ("m.room.name", "name", "room_name_removed", "@a:b.c removed the room name."),
            ("m.room.topic", "topic", "room_topic_removed", "@a:b.c removed the room topic."),
        ] {
            let err = serde_json::from_value::<AnySyncStateEvent>(serde_json::json!({
                "type": kind, "state_key": "", "event_id": "$p",
                "sender": "@a:b.c", "origin_server_ts": 1, "content": {},
            }))
            .expect_err("empty content does not parse");
            assert_eq!(failed_state_sentence(kind, &err.to_string()), Some(sentence), "{err}");
            assert_eq!(state_sentence_english(sentence, "@a:b.c", ""), english);
            assert_eq!(failed_state_sentence(kind, "invalid type: integer `5`"), None);
        }
        assert_eq!(failed_state_sentence("m.room.avatar", "missing field `name`"), None);
    }

    #[test]
    fn every_membership_change_crosses_as_a_closed_sentence() {
        use matrix_sdk_ui::timeline::MembershipChange as M;
        for (change, sentence, english) in [
            (Some(M::Joined), "member_joined", "Bob joined the room."),
            (Some(M::Left), "member_left", "Bob left the room."),
            (Some(M::Invited), "member_invited", "@a:b.c invited Bob."),
            (Some(M::InvitationAccepted), "invitation_accepted", "Bob accepted the invitation."),
            (Some(M::Kicked), "member_kicked", "@a:b.c removed Bob from the room."),
            (Some(M::Banned), "member_banned", "@a:b.c banned Bob."),
            (Some(M::KickedAndBanned), "member_banned", "@a:b.c banned Bob."),
            (Some(M::Unbanned), "member_unbanned", "@a:b.c unbanned Bob."),
            (Some(M::InvitationRejected), "invitation_rejected", "Bob rejected the invitation."),
            (Some(M::InvitationRevoked), "invitation_revoked", "@a:b.c revoked Bob's invitation."),
            (None, "membership_changed", "Membership for Bob changed."),
        ] {
            assert_eq!(membership_sentence(change), sentence, "{change:?}");
            assert_eq!(state_sentence_english(sentence, "@a:b.c", "Bob"), english, "{sentence}");
        }
    }

    #[test]
    fn tombstone_state_row_names_the_upgrade() {
        assert_eq!(
            state_row_text("m.room.tombstone", "@alice:example.org"),
            "@alice:example.org upgraded this room."
        );
    }

    #[test]
    fn state_row_text_uses_only_kind_and_actor() {
        let row = state_row_text("m.room.tombstone", "@alice:example.org");
        assert!(!row.contains('!'), "a room id must never appear in the row: {row}");

        let unknown = state_row_text("org.example.custom", "@bob:example.org");
        assert_eq!(unknown, "@bob:example.org updated room settings.");
        assert!(!unknown.contains("org.example.custom"));
    }

    #[test]
    fn existing_state_rows_are_unchanged() {
        assert_eq!(state_row_text("m.room.create", "@a:b.c"), "@a:b.c created the room.");
        assert_eq!(state_row_text("m.room.name", "@a:b.c"), "@a:b.c changed the room name.");
        assert_eq!(state_row_text("m.room.topic", "@a:b.c"), "@a:b.c changed the room topic.");
        assert_eq!(state_row_text("m.room.avatar", "@a:b.c"), "@a:b.c changed the room avatar.");
        assert_eq!(state_row_text("m.room.encryption", "@a:b.c"), "Encryption was enabled.");
    }

    #[test]
    fn empty_space_parent_is_described_as_removed_not_unsupported() {
        let body = failed_state_parse_body(
            "m.space.parent",
            "missing field `via` at line 1 column 2",
            "@alice:example.org",
        );
        assert_eq!(
            body,
            Some("@alice:example.org removed this room from a space.".to_owned())
        );
    }

    #[test]
    fn a_differently_malformed_space_parent_stays_unsupported() {
        assert_eq!(
            failed_state_parse_body(
                "m.space.parent",
                "invalid type: string \"nope\", expected a sequence at line 1 column 20",
                "@alice:example.org",
            ),
            None
        );
    }

    #[test]
    fn the_via_message_alone_is_not_enough_without_the_right_event_type() {
        assert_eq!(
            failed_state_parse_body(
                "org.example.unrelated",
                "missing field `via`",
                "@alice:example.org",
            ),
            None
        );
    }

    #[test]
    fn policy_rule_rows_say_what_happened() {
        use matrix_sdk::ruma::events::AnySyncStateEvent;
        let removal = serde_json::from_value::<AnySyncStateEvent>(serde_json::json!({
            "type": "m.policy.rule.user", "state_key": "rule:*", "event_id": "$p",
            "sender": "@a:b.c", "origin_server_ts": 1, "content": {},
        }))
        .expect_err("an empty policy rule does not parse");
        assert!(removal.to_string().contains("missing field `entity`"), "{removal}");
        for kind in [
            "m.policy.rule.user",
            "m.policy.rule.server",
            "m.policy.rule.room",
            "m.room.rule.user",
            "org.matrix.mjolnir.rule.server",
        ] {
            assert_eq!(
                failed_state_parse_body(kind, "missing field `entity` at line 1 column 2", "@a:b.c"),
                Some("@a:b.c removed a moderation rule.".to_owned()),
                "{kind}"
            );
            assert_eq!(state_row_text(kind, "@a:b.c"), "@a:b.c updated a moderation rule.", "{kind}");
        }
        assert_eq!(
            failed_state_parse_body("m.policy.rule.user", "invalid type: integer `5`, expected a string", "@a:b.c"),
            None
        );
        assert_eq!(
            failed_state_parse_body("m.policy.rule.users", "missing field `entity`", "@a:b.c"),
            None
        );
        assert_eq!(state_row_text("m.policy.rule.userx", "@a:b.c"), "@a:b.c updated room settings.");
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
        assert_eq!(mapped[0].1, vec!["session1", "session2", "session3"]);
    }

    #[test]
    fn empty_import_result_maps_to_empty() {
        let keys = BTreeMap::new();
        assert!(sessions_by_room_from_import(&keys).is_empty());
    }

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
        assert_eq!(entries[0]["user_id"], "@alice:example.org");
        assert_eq!(entries[0]["ts"], 1_700_000_123_000u64);
        assert_eq!(entries[1]["user_id"], "@bob:example.org");
        assert!(entries[1]["ts"].is_null());
        for entry in &entries {
            assert_eq!(entry.as_object().unwrap().len(), 2);
        }

        let (empty, empty_total) = super::read_by_json(
            std::iter::empty::<(&OwnedUserId, &Receipt)>(),
        );
        assert!(empty.is_empty());
        assert_eq!(empty_total, 0);
    }

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
        assert_eq!(entries[0]["user_id"], "@u19:example.org");
        assert_eq!(
            entries[super::READ_BY_CAP - 1]["user_id"],
            "@u4:example.org"
        );
    }

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
        assert_eq!(mine[1], "@u0:example.org");
        assert_eq!(mine[6], "@u6:example.org");
        assert_eq!(
            mine[super::REACTION_SENDER_CAP - 1],
            "@u15:example.org"
        );

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

    #[test]
    fn profile_name_change_classifies_set_changed_and_cleared() {
        assert_eq!(
            super::profile_name_change(None, Some("Alice")),
            Some(("set", None, Some("Alice".to_owned())))
        );
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
        assert_eq!(super::profile_name_change(Some("Alice"), Some("Alice")), None);
        assert_eq!(super::profile_name_change(None, None), None);
        assert_eq!(super::profile_name_change(Some(""), None), None);
        assert_eq!(super::profile_name_change(None, Some("")), None);
    }

    #[test]
    fn profile_names_are_bounded_by_chars_never_bytes() {
        let long: String = "🌩".repeat(300);
        let (kind, _, new) = super::profile_name_change(None, Some(&long))
            .expect("a long name is still a set");
        assert_eq!(kind, "set");
        let new = new.expect("set carries the new name");
        assert_eq!(new.chars().count(), super::PROFILE_NAME_CAP);
        assert_eq!(new.len(), super::PROFILE_NAME_CAP * 4);
        assert!(new.chars().all(|c| c == '🌩'));

        let exact: String = "a".repeat(super::PROFILE_NAME_CAP);
        assert_eq!(
            super::profile_name_change(None, Some(&exact)),
            Some(("set", None, Some(exact)))
        );
    }

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

    #[test]
    fn body_spec_defaults_and_refusals() {
        use super::{parse_body_spec, BodyFormat, SendBodySpec};
        assert_eq!(parse_body_spec("").unwrap(), SendBodySpec::default());
        assert_eq!(parse_body_spec("  ").unwrap(), SendBodySpec::default());
        let emote = parse_body_spec(r#"{"msgtype":"emote"}"#).unwrap();
        assert!(emote.emote);
        assert_eq!(emote.format, BodyFormat::Markdown);
        assert!(parse_body_spec(r#"{"format":"htm"}"#).is_err());
        assert!(parse_body_spec(r#"{"msgtype":"notice"}"#).is_err());
        assert!(parse_body_spec("not json").is_err());
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

    #[test]
    fn outgoing_markdown_is_strict_sanitized_too() {
        use super::{composed_content, composed_content_without_relation, SendBodySpec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = SendBodySpec::default();
        let body = "**bold** <b onclick=\"x()\">raw</b> &amp; <script>alert(1)</script> [link](javascript:alert(1))";
        let check = |msgtype: MessageType| match msgtype {
            MessageType::Text(text) => {
                let formatted = text.formatted.expect("formatted body");
                assert!(!formatted.body.contains("<script"), "script survived: {}", formatted.body);
                assert!(!formatted.body.contains("onclick"), "handler survived: {}", formatted.body);
                assert!(!formatted.body.contains("javascript:"), "scheme survived: {}", formatted.body);
                assert!(formatted.body.contains("<strong>bold</strong>"), "{}", formatted.body);
                assert!(formatted.body.contains("<b>raw</b>"), "{}", formatted.body);
                assert_eq!(text.body, body);
            }
            other => panic!("unexpected msgtype: {other:?}"),
        };
        check(composed_content(body, &spec).msgtype);
        check(composed_content_without_relation(body, &spec).msgtype);
        let ok = "[site](https://example.org) [@a](https://matrix.to/#/%40a%3Ax)";
        match composed_content(ok, &spec).msgtype {
            MessageType::Text(text) => {
                let formatted = text.formatted.expect("formatted body").body;
                assert!(formatted.contains("href=\"https://example.org\""), "{formatted}");
                assert!(formatted.contains("https://matrix.to/#/%40a%3Ax"), "{formatted}");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn a_formatted_body_that_sanitizes_to_nothing_is_dropped() {
        use super::sanitize_outgoing_formatted;
        use matrix_sdk::ruma::events::room::message::{
            EmoteMessageEventContent, MessageType, TextMessageEventContent,
        };
        let empties = "<img src=\"https://tracker.example/pixel.png\"> ";
        let mut text = MessageType::Text(TextMessageEventContent::html("plain words", empties));
        sanitize_outgoing_formatted(&mut text);
        match text {
            MessageType::Text(text) => {
                assert!(text.formatted.is_none(), "kept: {:?}", text.formatted);
                assert_eq!(text.body, "plain words");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
        let mut emote = MessageType::Emote(EmoteMessageEventContent::html("waves", empties));
        sanitize_outgoing_formatted(&mut emote);
        match emote {
            MessageType::Emote(emote) => assert!(emote.formatted.is_none(), "kept: {:?}", emote.formatted),
            other => panic!("unexpected msgtype: {other:?}"),
        }
        let mut kept = MessageType::Text(TextMessageEventContent::html("hi", "<em>hi</em><img src=\"https://x/p.png\">"));
        sanitize_outgoing_formatted(&mut kept);
        match kept {
            MessageType::Text(text) => {
                assert_eq!(text.formatted.expect("formatted body kept").body, "<em>hi</em>");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

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
                assert_eq!(text.body, "hey :blob: there");
                let formatted = text.formatted.expect("a formatted body");
                assert!(formatted.body.contains("data-mx-emoticon"),
                        "{}", formatted.body);
                assert!(formatted.body.contains("mxc://e.org/blob"),
                        "{}", formatted.body);
                assert!(!formatted.body.contains(":blob:"),
                        "the shortcode text was left beside the image: {}",
                        formatted.body);
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn a_selected_image_sends_its_description_as_alt_and_shortcode_as_title() {
        use super::{composed_content, parse_body_spec};
        use matrix_sdk::ruma::events::room::message::MessageType;
        let spec = parse_body_spec(
            r#"{"format":"markdown","emoticons":{
                ":Cat_Wave:":{"url":"mxc://packs.example.org/catWave","body":"A cat \"waving\" <hello> & bye"},
                ":plain:":"mxc://e.org/plain",
                ":evil:":{"url":"https://tracker.example/x.png","body":"x"},
                ":bad:":{"url":"mxc://e.org/a/b?c","body":"x"}}}"#,
        )
        .unwrap();
        assert_eq!(spec.emoticons.len(), 2, "{:?}", spec.emoticons);
        match composed_content("Hello :Cat_Wave: :plain: :evil:", &spec).msgtype {
            MessageType::Text(text) => {
                assert_eq!(text.body, "Hello :Cat_Wave: :plain: :evil:");
                let html = text.formatted.expect("formatted").body;
                assert!(html.contains(r#"src="mxc://packs.example.org/catWave""#), "{html}");
                assert!(html.contains(
                    r#"alt="A cat &quot;waving&quot; &lt;hello&gt; &amp; bye""#), "{html}");
                assert!(html.contains(r#"title="Cat_Wave""#), "{html}");
                assert!(html.contains(r#"height="32""#), "{html}");
                assert!(html.contains(r#"alt="plain""#), "{html}");
                assert!(html.contains(r#"title="plain""#), "{html}");
                assert!(html.contains(r#"src="mxc://e.org/plain""#), "{html}");
                assert!(!html.contains("tracker.example"), "{html}");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

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

    #[test]
    fn shortcodes_are_left_alone_where_they_are_not_emoji() {
        use super::substitute_emoticons;
        let map = vec![super::OutgoingEmoticon {
            code: ":blob:".to_owned(), mxc: "mxc://e.org/blob".to_owned(), alt: "blob".to_owned() }];

        for literal in [
            "<code>:blob:</code>",
            "<pre><code>x :blob: y</code></pre>",
        ] {
            assert_eq!(substitute_emoticons(literal, &map), literal,
                       "rewrote a shortcode inside code");
        }

        let url = r#"<a href="https://h.example/:blob:/x">l</a>"#;
        assert_eq!(substitute_emoticons(url, &map), url,
                   "rewrote inside a tag");
        let bare = "see https://h.example/:blob:/x now";
        assert_eq!(substitute_emoticons(bare, &map), bare,
                   "rewrote inside a bare URL run");

        let prose = "hi :blob: there";
        assert!(substitute_emoticons(prose, &map).contains("data-mx-emoticon"));
    }

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

    #[test]
    fn only_mxc_images_survive_the_outgoing_filter() {
        use super::strip_non_mxc_images;

        let good = r#"a <img data-mx-emoticon src="mxc://e.org/b" alt=":b:" height="32"> z"#;
        assert_eq!(strip_non_mxc_images(good), good);

        for hostile in [
            r#"<img data-mx-emoticon src="https://tracker.example/p.png">"#,
            r#"<img src="http://evil.test/x.png">"#,
            r#"<img src="//evil.test/x.png">"#,
            r#"<img src="javascript:evil()">"#,
            r#"<img src="data:image/png;base64,AAAA">"#,
            r#"<img src="mxc://">"#,
            r#"<img data-mx-emoticon alt=":b:">"#,
            r#"<IMG SRC="https://evil.test/x.png">"#,
            r#"<img src='https://evil.test/x.png'>"#,
            r#"<img src = "https://evil.test/x.png">"#,
            r#"<img data-src="mxc://e.org/b" src="https://evil.test/x.png">"#,
        ] {
            let cleaned = strip_non_mxc_images(hostile);
            assert!(
                !cleaned.contains("<img") && !cleaned.contains("<IMG"),
                "{hostile} survived as {cleaned}"
            );
        }

        assert!(!strip_non_mxc_images(r#"ok <img src="mxc://e.org/b""#)
            .contains("<img"));

        let other = "<image>x</image>";
        assert_eq!(strip_non_mxc_images(other), other);
        let text = "1 < 2 and imgs are fine";
        assert_eq!(strip_non_mxc_images(text), text);
    }

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
                assert_eq!(text.body, "hi :blob:");
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

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
                assert_eq!(emote.body, "waves at Alice");
                let formatted = emote.formatted.expect("formatted body");
                assert!(formatted.body.contains("matrix.to/#/@alice:example.org"));
            }
            other => panic!("unexpected msgtype: {other:?}"),
        }
    }

    #[test]
    fn mentions_serialize_user_ids_and_drop_invalid() {
        use matrix_sdk::ruma::events::room::message::RoomMessageEventContent;

        let mentions = super::mentions_from_ids(vec![
            "@alice:example.org".to_owned(),
            "not a user id".to_owned(),
            "@bob:example.org".to_owned(),
        ]);
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

        let none_valid = super::mentions_from_ids(vec!["nope".to_owned()]);
        assert!(none_valid.user_ids.is_empty() && !none_valid.room);
    }

    #[test]
    fn a_message_with_no_pill_still_declares_that_it_mentions_nobody() {
        use super::{outgoing_content, outgoing_content_without_relation, parse_body_spec};
        use serde_json::json;
        let spec = parse_body_spec("").unwrap();
        for ids in [vec![], vec!["not a user id".to_owned()]] {
            let room = serde_json::to_value(outgoing_content(
                "ask Max about it @room",
                &spec,
                ids.clone(),
            ))
            .unwrap();
            assert_eq!(room["m.mentions"], json!({}), "room/scheduled send: {room}");
            let reply = serde_json::to_value(outgoing_content_without_relation(
                "ask Max about it",
                &spec,
                ids,
            ))
            .unwrap();
            assert_eq!(reply["m.mentions"], json!({}), "reply / edit base: {reply}");
        }
        let pill = serde_json::to_value(outgoing_content(
            "hi",
            &spec,
            vec!["@max:example.org".to_owned(), "@room".to_owned()],
        ))
        .unwrap();
        assert_eq!(pill["m.mentions"]["user_ids"], json!(["@max:example.org"]));
        assert_eq!(pill["m.mentions"]["room"], json!(true));
        let config = super::attachment_config(None, None, Some("Max".to_owned()), None);
        let caption_mentions = config.mentions.expect("an attachment always carries m.mentions");
        assert!(caption_mentions.user_ids.is_empty() && !caption_mentions.room);
    }

    #[test]
    fn an_edit_carries_m_mentions_and_pings_only_newly_added_users() {
        use super::{outgoing_content_without_relation, parse_body_spec};
        use serde_json::json;
        use matrix_sdk::ruma::events::room::message::ReplacementMetadata;
        use matrix_sdk::ruma::events::Mentions;
        use matrix_sdk::ruma::{owned_event_id, owned_user_id};
        let spec = parse_body_spec("").unwrap();
        let target = owned_event_id!("$orig:example.org");

        let edit = outgoing_content_without_relation("ask Max, fixed", &spec, vec![])
            .make_replacement(ReplacementMetadata::new(target.clone(), None));
        let value = serde_json::to_value(&edit).unwrap();
        assert_eq!(value["m.mentions"], json!({}), "top level: {value}");
        assert_eq!(value["m.new_content"]["m.mentions"], json!({}), "{value}");
        assert_eq!(value["m.relates_to"]["rel_type"], json!("m.replace"));

        let edit = outgoing_content_without_relation(
            "alice and bob",
            &spec,
            vec!["@alice:example.org".to_owned(), "@bob:example.org".to_owned()],
        )
        .make_replacement(ReplacementMetadata::new(
            target,
            Some(Mentions::with_user_ids([owned_user_id!("@alice:example.org")])),
        ));
        let value = serde_json::to_value(&edit).unwrap();
        assert_eq!(value["m.mentions"]["user_ids"], json!(["@bob:example.org"]));
        assert_eq!(
            value["m.new_content"]["m.mentions"]["user_ids"],
            json!(["@alice:example.org", "@bob:example.org"])
        );
    }

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
        let room = "[room](https://matrix.to/#/%23room%3Ax) **bold**";
        assert_eq!(mention_plain_body(room), room);
        let site = "[site](https://example.org/page)";
        assert_eq!(mention_plain_body(site), site);
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
        assert_eq!(answers[0]["id"], "a1");
        assert_eq!(answers[0]["count"], 2);
        assert_eq!(answers[0]["by_me"], true);
        assert_eq!(answers[1]["count"], 1);
        assert_eq!(answers[1]["by_me"], false);
        assert_eq!(answers[2]["count"], 0);
        assert!(!out.to_string().contains("@alice:example.org"));
    }

    #[test]
    fn undisclosed_running_poll_hides_counts_but_keeps_own_vote() {
        let out = filled(&poll_view(false, false));
        assert_eq!(out["poll_kind"], "undisclosed");
        let answers = out["poll_answers"].as_array().unwrap();
        assert!(answers.iter().all(|a| a["count"] == 0));
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
        assert_ne!(answers[0].id, answers[1].id);
        assert!(matches!(
            content.poll_start.kind,
            matrix_sdk::ruma::events::poll::start::PollKind::Disclosed
        ));
        assert_eq!(u64::from(content.poll_start.max_selections), 2);
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
        let out = super::downsample_waveform(&[0, 512, 1024], 1024);
        assert_eq!(out, vec![0, 50, 100]);
        let long: Vec<u64> = (0..1000).map(|i| i % 1025).collect();
        let out = super::downsample_waveform(&long, 1024);
        assert_eq!(out.len(), 96);
        assert!(out.iter().all(|v| *v <= 100));
        assert!(super::downsample_waveform(&[], 1024).is_empty());
        assert!(super::downsample_waveform(&[5], 0).is_empty());
        assert_eq!(super::downsample_waveform(&[9999], 1024), vec![100]);
    }

    #[test]
    fn waveform_of_a_quiet_recording_is_not_flattened() {
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

        assert_eq!(out.iter().copied().max().unwrap(), 100);
        let above_floor = out.iter().filter(|v| **v >= 12).count();
        assert!(
            above_floor >= 70,
            "only {above_floor} of 96 buckets clear the 0.12 display floor; \
             a quiet recording is still being flattened"
        );
        let distinct: std::collections::BTreeSet<u64> = out.iter().copied().collect();
        assert!(
            distinct.len() >= 30,
            "only {} distinct bar heights", distinct.len()
        );
    }

    #[test]
    fn waveform_of_a_full_range_recording_is_unchanged() {
        let raw: Vec<u64> = (0..96).map(|i| (i * 1024) / 95).collect();
        let out = super::downsample_waveform(&raw, 1024);
        let expected: Vec<u64> = raw.iter().map(|v| (v.min(&1024) * 100) / 1024).collect();
        assert_eq!(out, expected);
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

#[cfg(test)]
mod gallery_tests {
    use super::{
        fill_message_content, fill_message_media, gallery_item_key, lightning_event_filter, message_summary,
        parse_gallery, MediaRegistry, StoredMedia, TimelineRegistry, GALLERY_ITEM_CAP,
        MEDIA_SOURCE_CAP,
    };
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};
    use matrix_sdk::ruma::events::{
        room::{
            message::{MessageType, RoomMessageEventContent},
            MediaSource,
        },
        AnySyncTimelineEvent,
    };
    use matrix_sdk::ruma::room_version_rules::RoomVersionRules;
    use serde_json::{json, Value};

    const MXC_A: &str = "mxc://sable.example/aaaaaaaaaaaaaaaaaaaa";
    const MXC_B: &str = "mxc://sable.example/bbbbbbbbbbbbbbbbbbbb";

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
        let mut stable = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        stable["msgtype"] = "m.gallery".into();
        assert!(lightning_event_filter(&event(stable), &rules));
    }

    #[test]
    fn a_gallery_edit_stays_out() {
        let rules = RoomVersionRules::V11;
        let mut edit = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        edit["m.relates_to"] = json!({ "rel_type": "m.replace", "event_id": "$orig:sable.example" });
        edit["m.new_content"] = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 10, 10)]);
        assert!(!lightning_event_filter(&event(edit), &rules));
    }

    #[test]
    fn a_custom_msgtype_message_is_admitted_and_its_edit_is_not() {
        let rules = RoomVersionRules::V11;
        let custom = json!({ "msgtype": "com.example.custom", "body": "hi" });
        assert!(lightning_event_filter(&event(custom.clone()), &rules));
        assert!(super::shown_by_lightning_filter(&event(custom), &rules));

        let mut edit = json!({ "msgtype": "com.example.custom", "body": "* hi again" });
        edit["m.new_content"] = json!({ "msgtype": "com.example.custom", "body": "hi again" });
        edit["m.relates_to"] = json!({ "rel_type": "m.replace", "event_id": "$orig:sable.example" });
        assert!(!lightning_event_filter(&event(edit.clone()), &rules));
        assert!(!super::shown_by_lightning_filter(&event(edit), &rules));

        let text = json!({ "msgtype": "m.text", "body": "plain" });
        assert!(lightning_event_filter(&event(text), &rules));
        let mut text_edit = json!({ "msgtype": "m.text", "body": "* plain" });
        text_edit["m.new_content"] = json!({ "msgtype": "m.text", "body": "plain" });
        text_edit["m.relates_to"] = json!({ "rel_type": "m.replace", "event_id": "$orig:sable.example" });
        assert!(!lightning_event_filter(&event(text_edit), &rules));
    }

    #[test]
    fn a_custom_msgtype_row_carries_its_body() {
        let custom =
            content(json!({ "msgtype": "com.example.custom", "body": "line one\nline two" }));
        let mut out = json!({});
        assert!(fill_message_content(&mut out, &custom.msgtype).is_none());
        assert_eq!(out["msgtype"], "unsupported");
        assert_eq!(out["body"], "line one\nline two");

        let empty = content(json!({ "msgtype": "com.example.custom", "body": " " }));
        let mut out = json!({});
        fill_message_content(&mut out, &empty.msgtype);
        assert_eq!(out["body"], "[unsupported event]");
    }

    #[test]
    fn a_media_caption_keeps_its_filename_and_its_formatted_body() {
        for source in [
            json!({ "url": "mxc://x/y" }),
            json!({ "file": {
                "url": "mxc://x/y", "v": "v2", "mimetype": "image/png",
                "key": { "kty": "oct", "key_ops": ["encrypt", "decrypt"], "alg": "A256CTR",
                         "k": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ext": true },
                "iv": "AAAAAAAAAAAAAAAAAAAAAA", "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" }
            } }),
        ] {
            for kind in ["m.image", "m.video", "m.audio", "m.file"] {
                let mut wire = json!({
                    "msgtype": kind,
                    "body": "look **here**",
                    "filename": "photo.png",
                    "format": "org.matrix.custom.html",
                    "formatted_body": "look <strong>here</strong>",
                });
                for (k, v) in source.as_object().unwrap() {
                    wire[k] = v.clone();
                }
                let msgtype: MessageType = serde_json::from_value(wire).unwrap();
                let mut out = json!({});
                let _ = fill_message_content(&mut out, &msgtype);
                assert_eq!(out["media_filename"], "photo.png", "{kind}");
                assert_eq!(out["body"], "look **here**", "{kind}");
                assert_eq!(
                    out["formatted_body"], "look <strong>here</strong>",
                    "{kind}: the formatted caption was dropped"
                );
            }
        }
    }

    #[test]
    fn a_two_image_sable_gallery_becomes_one_image_row_with_two_items() {
        let wire = content(sable_gallery(vec![
            sable_plain_image_item("before.png", MXC_A, 1280, 720),
            sable_plain_image_item("after.png", MXC_B, 640, 480),
        ]));
        let mut out = json!({});
        let sources = fill_message_media(&mut out, &wire.msgtype, "$g");

        assert_eq!(out["msgtype"], "image");
        assert_eq!(out["media_filename"], "before.png");
        assert_eq!(out["media_mxc"], MXC_A);
        assert_eq!(out["media_width"], 1280);
        assert_eq!(out["body"], "");
        assert!(out.get("formatted_body").is_none());
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

        let keys: Vec<&str> = sources.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, vec!["$g", "$g#item1"]);
        assert!(matches!(&sources[1].1.source, MediaSource::Plain(m) if m.as_str() == MXC_B));
        assert_eq!(sources[1].1.filename, "after.png");
    }

    #[test]
    fn audio_cover_is_read_from_the_wire() {
        use super::audio_thumbnail_from_wire as cover;
        let plain = json!({ "content": { "msgtype": "m.audio", "info": {
            "thumbnail_url": "mxc://example.org/cover" } } });
        assert!(matches!(cover(&plain), Some(MediaSource::Plain(m)) if m.as_str() == "mxc://example.org/cover"));
        let enc = json!({ "content": { "msgtype": "m.audio", "info": {
            "thumbnail_file": { "url": "mxc://example.org/encthumb",
              "key": { "kty": "oct", "key_ops": ["encrypt","decrypt"], "alg": "A256CTR",
                       "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "ext": true },
              "iv": "AAAAAAAAAAAAAAAAAAAAAA",
              "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" }, "v": "v2" } } } });
        assert!(matches!(cover(&enc), Some(MediaSource::Encrypted(_))));
        for bad in [
            json!({ "content": { "info": { "thumbnail_url": "https://evil.example/x.png" } } }),
            json!({ "content": { "info": { "thumbnail_url": 7 } } }),
            json!({ "content": { "info": { "thumbnail_file": { "url": "nope" } } } }),
            json!({ "content": { "info": {} } }),
            json!({ "content": {} }),
            json!({}),
        ] {
            assert!(cover(&bad).is_none(), "{bad}");
        }
    }

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
    fn a_voice_message_summary_is_a_voice_message() {
        let voice = content(json!({
            "msgtype": "m.audio", "body": "t12.ogg", "url": MXC_A,
            "org.matrix.msc3245.voice": {},
        }));
        let summary = message_summary(&voice.msgtype);
        assert_eq!(summary.kind, "audio");
        assert_eq!(summary.text, "Voice message");
        let audio = content(json!({ "msgtype": "m.audio", "body": "song.ogg", "url": MXC_A }));
        assert_eq!(message_summary(&audio.msgtype).text, "song.ogg");
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

    #[test]
    fn only_the_exact_generated_list_is_dropped_as_a_caption() {
        let mut wire = sable_gallery(vec![sable_plain_image_item("a.png", MXC_A, 1, 1)]);
        wire["body"] = "[a.png: see the diff]".into();
        let gallery = parse_gallery(&content(wire).msgtype).expect("a gallery");
        assert_eq!(gallery.caption, "[a.png: see the diff]");
    }

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
        assert!(registry.media_source("$gallery0", false).is_none());
    }

    #[test]
    fn the_media_registry_evicts_the_least_recently_used_key() {
        let mut registry = MediaRegistry::with_cap(3);
        for key in ["a", "b", "c"] {
            registry.insert(key.to_owned(), stored(key));
        }
        assert!(registry.get("a").is_some());
        registry.insert("d".to_owned(), stored("d"));
        assert!(registry.contains("a") && registry.contains("d"));
        assert!(!registry.contains("b"), "b was the least recently used");
        registry.insert("c".to_owned(), stored("c"));
        registry.insert("e".to_owned(), stored("e"));
        assert!(!registry.contains("a") && registry.contains("c"));
        assert_eq!(registry.len(), 3);
    }

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

#[cfg(test)]
mod reply_fallback_tests {
    use super::strip_reply_fallback as strip;

    #[test]
    fn single_line_quote() {
        assert_eq!(strip("> <@a:b.c> You know\n\nyes I do"), "yes I do");
    }

    #[test]
    fn multi_line_quote_and_crlf() {
        assert_eq!(strip("> <@a:b.c> one\n> two\n>\n> three\n\nreply"), "reply");
        assert_eq!(strip("> <@a:b.c> one\r\n> two\r\n\r\nreply"), "reply");
    }

    #[test]
    fn nested_fallback() {
        assert_eq!(
            strip("> <@a:b.c> > <@d:e.f> older\n> \n> newer\n\nreply"),
            "reply"
        );
        assert_eq!(strip("> <@a:b.c> > <@d:e.f> older\n> newer\n\nreply"), "reply");
    }

    #[test]
    fn emote_fallback() {
        assert_eq!(strip("> * <@a:b.c> waves\n\nok"), "ok");
    }

    #[test]
    fn a_legit_quote_without_a_blank_line_is_kept() {
        for b in [
            "> <@a:b.c> quoting a person\nand my reply with no separator",
            "> <@a:b.c> only a quote",
            "> just a markdown quote\n\ntext",
            "> not a mention <@a:b.c>\n\ntext",
            "plain\n\n> <@a:b.c> later quote\n\nx",
        ] {
            assert_eq!(strip(b), b);
        }
    }

    #[test]
    fn nothing_after_the_fallback_is_empty() {
        assert_eq!(strip("> <@a:b.c> q\n\n"), "");
    }
}

#[cfg(test)]
mod thread_utd_tests {
    use super::{
        cleartext_thread_root, event_item_to_json, is_thread_reply_fallback, lightning_event_filter,
        TimelineRegistry,
    };
    use matrix_sdk::ruma::api::client::sync::sync_events::v5;
    use matrix_sdk::ruma::events::AnySyncTimelineEvent;
    use matrix_sdk::ruma::room_id;
    use matrix_sdk::ruma::serde::Raw;
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk_base::RequestedRequiredStates;
    use matrix_sdk_ui::timeline::{TimelineBuilder, TimelineFocus};
    use serde_json::{json, Value};
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    const ROOT: &str = "$threadroot:morpheus.localhost";
    const REPLY: &str = "$JbFHtZpEJiH8uaajZjPLz0QUZc1xtBR9rPGBOjF6WFM";

    fn encrypted(relates_to: Option<Value>) -> Value {
        let mut content = json!({
            "algorithm": "m.megolm.v1.aes-sha2",
            "ciphertext": "AwgAEpABhetEzzZzyYrxtEVUtlJnZtJcURBlQUQJ9irVeklCTs06LwgTMQj61PMUS4Vy\
                           YOX+PD67+hhU40/8olOww+Ud0m2afjMjC3wFX+4fFfSkoWPVHEmRVucfcdSF1RSB4EmK\
                           PIP4eo1X6x8kCIMewBvxl2sI9j4VNvDvAN7M3zkLJfFLOFHbBviI4FN7hSFHFeM739Zg\
                           iwxEs3hIkUXEiAfrobzaMEM/zY7SDrTdyffZndgJo7CZOVhoV6vuaOhmAy4X2t4UnbuV\
                           JGJjKfV57NAhp8W+9oT7ugwO",
            "device_id": "KIUVQQSDTM",
            "sender_key": "LvryVyoCjdONdBCi2vvoSbI34yTOx7YrCFACUEKoXnc",
            "session_id": "64H7XKokIx0ASkYDHZKlT5zd/Zccz/cQspPNdvnNULA"
        });
        if let Some(relation) = relates_to {
            content["m.relates_to"] = relation;
        }
        json!({
            "event_id": REPLY,
            "origin_server_ts": 1698579035927u64,
            "sender": "@example2:morpheus.localhost",
            "type": "m.room.encrypted",
            "content": content,
        })
    }

    fn thread_relation(root: &str) -> Value {
        json!({
            "rel_type": "m.thread",
            "event_id": root,
            "is_falling_back": true,
            "m.in_reply_to": { "event_id": root },
        })
    }

    fn raw(value: Value) -> Raw<AnySyncTimelineEvent> {
        Raw::from_json_string(value.to_string()).unwrap()
    }

    #[test]
    fn only_a_falling_back_thread_relation_is_a_fallback_quote() {
        let mut plain = encrypted(Some(thread_relation(ROOT)));
        assert!(is_thread_reply_fallback(&raw(plain.clone())));
        plain["type"] = json!("m.room.message");
        assert!(is_thread_reply_fallback(&raw(plain)));
        let explicit = json!({
            "rel_type": "m.thread",
            "event_id": ROOT,
            "is_falling_back": false,
            "m.in_reply_to": { "event_id": "$chosen:example.org" },
        });
        assert!(!is_thread_reply_fallback(&raw(encrypted(Some(explicit)))));
        let absent = json!({
            "rel_type": "m.thread",
            "event_id": ROOT,
            "m.in_reply_to": { "event_id": "$chosen:example.org" },
        });
        assert!(!is_thread_reply_fallback(&raw(encrypted(Some(absent)))));
        let reply = json!({ "m.in_reply_to": { "event_id": ROOT } });
        assert!(!is_thread_reply_fallback(&raw(encrypted(Some(reply)))));
        assert!(!is_thread_reply_fallback(&raw(encrypted(None))));
    }

    #[test]
    fn only_a_cleartext_thread_relation_names_a_root() {
        assert_eq!(
            cleartext_thread_root(&raw(encrypted(Some(thread_relation(ROOT))))).as_deref(),
            Some(ROOT)
        );
        let reply = json!({ "m.in_reply_to": { "event_id": ROOT } });
        assert_eq!(cleartext_thread_root(&raw(encrypted(Some(reply)))), None);
        assert_eq!(cleartext_thread_root(&raw(encrypted(None))), None);
        assert_eq!(
            cleartext_thread_root(&raw(encrypted(Some(thread_relation("not-an-id"))))),
            None
        );
        let mut plain = encrypted(Some(thread_relation(ROOT)));
        plain["type"] = json!("m.room.message");
        assert_eq!(cleartext_thread_root(&raw(plain)), None);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_undecryptable_thread_reply_in_the_room_timeline_names_its_thread() {
        let server = MatrixMockServer::new().await;
        let client = server
            .client_builder()
            .on_builder(|builder| builder.with_encryption_settings(crate::encryption_settings()))
            .build()
            .await;
        client.encryption().wait_for_e2ee_initialization_tasks().await;
        client.event_cache().subscribe().expect("event cache");

        let room = room_id!("!DovneieKSTkdHKpIXy:morpheus.localhost");
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
        entry.timeline = vec![Raw::from_json_string(
            encrypted(Some(thread_relation(ROOT))).to_string(),
        )
        .unwrap()];
        response.rooms.insert(room.to_owned(), entry);
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");

        let room = client.get_room(room).expect("room known");
        let timeline = TimelineBuilder::new(&room)
            .event_filter(lightning_event_filter)
            .with_focus(TimelineFocus::Live { hide_threaded_events: true })
            .build()
            .await
            .expect("timeline");

        let deadline = Instant::now() + Duration::from_secs(10);
        let utd = loop {
            let found = timeline.items().await.iter().find_map(|item| {
                item.as_event()
                    .filter(|event| event.content().is_unable_to_decrypt())
                    .map(|event| (item.unique_id().0.clone(), event.clone()))
            });
            if let Some(found) = found {
                break found;
            }
            assert!(
                Instant::now() < deadline,
                "SDK premise changed: the undecryptable thread reply is not in \
                 the live timeline; the tag may no longer be needed"
            );
            tokio::time::sleep(Duration::from_millis(50)).await;
        };

        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let row = event_item_to_json(
            &utd.0, &utd.1, "!room:morpheus.localhost", "@me:morpheus.localhost", &registry,
        );
        assert_eq!(row["undecryptable"], true);
        assert_eq!(row["event_id"], REPLY);
        assert_eq!(
            row["thread_root_id"], ROOT,
            "the room timeline row must name the thread it belongs to"
        );
    }
}

#[cfg(test)]
mod live_tail_tests {
    use super::{LiveTail, RemoteEventItem};
    use crate::push_verdict::{live_append_slots, BacklogMarker, ItemFacts};
    use matrix_sdk_ui::eyeball_im::{Vector, VectorDiff};
    use serde_json::json;

    #[derive(Clone, Debug, PartialEq)]
    struct Row {
        id: &'static str,
        remote: bool,
        live: bool,
    }
    impl RemoteEventItem for Row {
        fn is_remote_event(&self) -> bool {
            self.remote
        }
    }
    fn remote(id: &'static str) -> Row {
        Row { id, remote: true, live: true }
    }
    fn local(id: &'static str) -> Row {
        Row { id, remote: false, live: true }
    }
    fn tail(rows: &[Row]) -> LiveTail<Row> {
        LiveTail::new(rows.iter().cloned().collect::<Vector<_>>())
    }
    fn facts(row: &Row) -> Option<ItemFacts> {
        row.remote.then(|| ItemFacts {
            event_id: row.id.to_owned(),
            timestamp_ms: 2_000,
            age_ms: None,
        })
    }

    #[test]
    fn only_a_set_that_decrypts_the_same_event_is_a_late_decryption() {
        let utd = |row: &Row| !row.live;
        let id = |row: &Row| row.remote.then(|| row.id.to_owned());
        let cipher = Row { id: "$e", remote: true, live: false };
        let mirror = tail(&[remote("$a"), cipher.clone()]);
        let decrypt = VectorDiff::Set { index: 1, value: remote("$e") };
        assert!(mirror.decrypts_in_place(&decrypt, utd, id));
        let other = VectorDiff::Set { index: 1, value: remote("$f") };
        assert!(!mirror.decrypts_in_place(&other, utd, id));
        let still = VectorDiff::Set { index: 1, value: cipher.clone() };
        assert!(!mirror.decrypts_in_place(&still, utd, id));
        let readable = VectorDiff::Set { index: 0, value: remote("$a") };
        assert!(!mirror.decrypts_in_place(&readable, utd, id));
        let push = VectorDiff::PushBack { value: remote("$e") };
        assert!(!mirror.decrypts_in_place(&push, utd, id));
        let far = VectorDiff::Set { index: 9, value: remote("$e") };
        assert!(!mirror.decrypts_in_place(&far, utd, id));
    }

    #[test]
    fn an_insert_above_a_failed_echo_is_judged_as_a_live_append() {
        let mut mirror = tail(&[remote("$a"), remote("$b"), local("echo")]);
        let insert = VectorDiff::Insert { index: 2, value: remote("$new") };
        let mut payload = json!({ "op": "insert", "index": 2, "item": { "event_id": "$new" } });

        assert!(live_append_slots(&insert, &mut payload).is_empty());

        let judged = mirror
            .classify_and_apply(&insert, |row| row.live)
            .expect("an arrival above the echo region is a live append");
        assert_eq!(judged, VectorDiff::PushBack { value: remote("$new") });
        let slots = live_append_slots(&judged, &mut payload);
        assert_eq!(slots.len(), 1, "the verdict slot is the insert's own `item`");
        assert_eq!(slots[0].0.id, "$new");

        let mut backlog = BacklogMarker::new(1_000, std::iter::empty());
        backlog.begin_batch();
        backlog.mark(&judged, &mut payload, 3_000, facts);
        assert!(payload["item"].get("backlog").is_none());
        let mut again = json!({ "op": "insert", "index": 3, "item": { "event_id": "$new" } });
        backlog.mark(&judged, &mut again, 3_000, facts);
        assert_eq!(again["item"]["backlog"], json!(true));

        let next = VectorDiff::Insert { index: 3, value: remote("$next") };
        assert!(mirror.classify_and_apply(&next, |row| row.live).is_some());
    }

    #[test]
    fn a_virtual_item_below_the_slot_does_not_hide_an_arrival() {
        let mut mirror =
            tail(&[remote("$a"), Row { id: "read-marker", remote: false, live: false }]);
        let insert = VectorDiff::Insert { index: 1, value: remote("$new") };
        assert!(mirror.classify_and_apply(&insert, |row| row.live).is_some());
    }

    #[test]
    fn history_and_echoes_are_not_live_appends() {
        let mut mirror = tail(&[remote("$a"), remote("$b"), local("echo")]);
        let middle = VectorDiff::Insert { index: 1, value: remote("$old") };
        assert!(mirror.classify_and_apply(&middle, |row| row.live).is_none());
        let echo = VectorDiff::Insert { index: 4, value: local("echo2") };
        assert!(mirror.classify_and_apply(&echo, |row| row.live).is_none());
        let push = VectorDiff::PushBack { value: remote("$c") };
        assert!(mirror.classify_and_apply(&push, |row| row.live).is_none());
    }

    #[test]
    fn with_no_remote_event_above_it_only_an_event_from_after_the_open_counts() {
        let mut mirror = tail(&[local("echo")]);
        let old = Row { id: "$old", remote: true, live: false };
        assert!(mirror
            .classify_and_apply(&VectorDiff::Insert { index: 0, value: old }, |row| row.live)
            .is_none());
        let mut mirror = tail(&[local("echo")]);
        let new = VectorDiff::Insert { index: 0, value: remote("$new") };
        assert!(mirror.classify_and_apply(&new, |row| row.live).is_some());
    }

    #[test]
    fn a_diff_the_mirror_cannot_apply_stops_classification_without_panicking() {
        let mut mirror = tail(&[remote("$a"), local("echo")]);
        assert!(mirror
            .classify_and_apply(&VectorDiff::Remove { index: 9 }, |row| row.live)
            .is_none());
        let insert = VectorDiff::Insert { index: 1, value: remote("$new") };
        assert!(mirror.classify_and_apply(&insert, |row| row.live).is_none());
        let reset = VectorDiff::Reset { values: Vector::new() };
        assert!(mirror.classify_and_apply(&reset, |row| row.live).is_none());
    }
}

#[cfg(test)]
mod thread_read_tests {
    use super::{cover_flips, thread_latest_reply_read, thread_receipt_type, TimelineRegistry};
    use matrix_sdk::ruma::api::client::receipt::create_receipt::v3::ReceiptType;
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};

    #[test]
    fn an_unthreaded_receipt_at_or_after_the_latest_reply_reads_the_thread() {
        let reply_ts = 1_000;
        assert!(!thread_latest_reply_read(false, false, reply_ts, None));
        assert!(thread_latest_reply_read(false, false, reply_ts, Some(1_000)));
        assert!(thread_latest_reply_read(false, false, reply_ts, Some(5_000)));
        assert!(!thread_latest_reply_read(false, false, reply_ts, Some(999)));
        assert!(thread_latest_reply_read(true, false, reply_ts, None));
        assert!(thread_latest_reply_read(false, true, reply_ts, Some(1)));
    }

    #[test]
    fn a_cover_change_repaints_only_the_threads_it_crosses() {
        assert!(cover_flips(None, Some(2_000), 1_500));
        assert!(!cover_flips(None, Some(2_000), 2_500));
        assert!(!cover_flips(Some(2_000), Some(3_000), 1_500));
        assert!(cover_flips(Some(2_000), Some(3_000), 2_500));
    }

    #[test]
    fn the_cover_belongs_to_one_room() {
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        assert_eq!(registry.unthreaded_read_cover_ms("!a:x"), None);
        assert_eq!(registry.set_unthreaded_read_cover("!a:x", Some(7)), None);
        assert_eq!(registry.unthreaded_read_cover_ms("!a:x"), Some(7));
        assert_eq!(registry.unthreaded_read_cover_ms("!b:x"), None);
        assert_eq!(registry.set_unthreaded_read_cover("!a:x", Some(9)), Some(7));
        assert_eq!(registry.set_unthreaded_read_cover("!b:x", None), None);
        assert_eq!(registry.unthreaded_read_cover_ms("!a:x"), None);
    }

    #[test]
    fn do_not_send_read_receipts_sends_a_private_thread_receipt() {
        assert_eq!(thread_receipt_type(0), ReceiptType::Read);
        assert_eq!(thread_receipt_type(1), ReceiptType::ReadPrivate);
        assert_eq!(thread_receipt_type(2), ReceiptType::ReadPrivate);
    }
}

#[cfg(test)]
mod live_location_tests {
    use super::live_location_body;

    #[test]
    fn a_live_location_without_a_description_has_an_empty_body() {
        assert_eq!(live_location_body(None), "");
        assert_eq!(live_location_body(Some("On my way")), "On my way");
    }
}

#[cfg(test)]
mod thread_auto_subscription_tests {
    use super::{server_supports_thread_subscriptions, thread_root_of_content};
    use matrix_sdk::ruma::api::FeatureFlag;
    use serde_json::json;
    use std::collections::BTreeSet;

    #[tokio::test(flavor = "current_thread")]
    async fn a_watcher_from_a_departed_session_is_never_kept() {
        use super::TimelineRegistry;
        use std::collections::VecDeque;
        use std::sync::{atomic::Ordering, Arc, Mutex};
        let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
        let lifecycle = registry.lifecycle();
        let pending = || tokio::spawn(std::future::pending::<()>());

        let kept = pending();
        assert!(registry.register_thread_auto_subscription(kept.abort_handle(), lifecycle));
        registry.lifecycle_gen.fetch_add(1, Ordering::SeqCst);
        registry.abort_thread_auto_subscriptions();
        assert!(kept.await.expect_err("aborted").is_cancelled());

        let late = pending();
        assert!(!registry.register_thread_auto_subscription(late.abort_handle(), lifecycle));
        assert!(late.await.expect_err("aborted").is_cancelled());
        assert!(registry.thread_auto_subscriptions.lock().unwrap().is_empty());

        let current = registry.lifecycle();
        let fresh = pending();
        assert!(registry.register_thread_auto_subscription(fresh.abort_handle(), current));
        let stale = pending();
        assert!(!registry.register_thread_auto_subscription(stale.abort_handle(), lifecycle));
        assert_eq!(registry.thread_auto_subscriptions.lock().unwrap().len(), 1);
        assert!(!fresh.is_finished());
        fresh.abort();
    }

    #[test]
    fn only_a_server_that_advertises_msc4306_is_asked() {
        let mut features = BTreeSet::new();
        assert!(!server_supports_thread_subscriptions(&features));
        features.insert(FeatureFlag::from("org.matrix.msc4308"));
        assert!(!server_supports_thread_subscriptions(&features));
        features.insert(FeatureFlag::from("org.matrix.msc4306"));
        assert!(server_supports_thread_subscriptions(&features));
    }

    #[test]
    fn only_a_thread_relation_names_the_thread_it_was_sent_in() {
        let reply = json!({
            "msgtype": "m.text", "body": "hi",
            "m.relates_to": {
                "rel_type": "m.thread", "event_id": "$root",
                "is_falling_back": true,
                "m.in_reply_to": { "event_id": "$root" }
            }
        });
        assert_eq!(thread_root_of_content(&reply), Some("$root"));
        let rich = json!({ "body": "x", "m.relates_to": { "m.in_reply_to": { "event_id": "$a" } } });
        assert_eq!(thread_root_of_content(&rich), None);
        let edit = json!({ "body": "x", "m.relates_to": { "rel_type": "m.replace", "event_id": "$a" } });
        assert_eq!(thread_root_of_content(&edit), None);
        assert_eq!(thread_root_of_content(&json!({ "body": "x" })), None);
    }
}

#[cfg(test)]
#[path = "edit_regression_tests.rs"]
mod edit_regression_tests;
