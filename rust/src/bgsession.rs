//! Background handles (Labs, Phase 1): the allow-list gate on the event
//! queue, the notification pre-filter, the Session sticky slot and the
//! room-summary aggregator (design 2026-10-15 §3.2.3-§3.2.5 and §3.2.9,
//! contract la-p1 §3.2-§3.5).
//!
//! A Background handle is an ordinary `RustClient` whose queues are
//! registered here. `crate::enqueue` asks [`admit`] about every event bound
//! for a registered queue, but only once `labs::active()` reads true, so with
//! the flag off nothing in this file runs. In Background:
//!
//! * `Session` events flow and the sticky ones are recorded in the slot
//!   (`mx_rust_bg_take_session_state`), so C++ never depends on the queue for
//!   them;
//! * `Notify` events flow only when [`should_forward`] says so, at or above
//!   the floor, with a small `bg` object C++ builds the notification from;
//! * everything else is dropped and counted. The work behind the costly
//!   UiOnly producers is skipped at its source (lib.rs), not just here.
//!
//! The aggregator turns `room_info_notable_update_receiver()` into a dirty
//! set and a flag; `mx_rust_bg_take` serializes the WHOLE counts table at take
//! time, on the caller's thread. No queue entry, so an overflow cannot freeze
//! a badge. The unread RULE is never ported here: C++ applies
//! `roomunread::countsAsUnread` to the counts.
//!
//! Never logged or kept: tokens (scrubbed from every sticky payload), message
//! bodies, keys. Room names and display names cross only inside a forwarded
//! notification's `bg` object, in memory.

use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering};
use std::sync::{Arc, Mutex, OnceLock, RwLock, Weak};
use std::time::{Duration, Instant};

use matrix_sdk::ruma::OwnedRoomId;
use matrix_sdk::{Client, RoomState};
use serde_json::{json, Value};

use crate::labs::{self, EventClass, Mode, Profile};
use crate::EventQueueRef;

/// Bound on the forwarded-event-id set (`event_decrypted`/`event_redacted`
/// flow only for an id this handle forwarded).
pub(crate) const FORWARDED_IDS_CAP: usize = 512;
/// A `Lagged` rescan runs at most this often.
pub(crate) const RESCAN_INTERVAL: Duration = Duration::from_secs(30);
/// A Background verification request leaves the sticky slot after this long
/// even if no cancellation names it: matrix-sdk-crypto times a request out
/// after ten minutes (`VERIFICATION_TIMEOUT`) and says so to nobody.
pub(crate) const PENDING_VERIFICATION_TTL: Duration = Duration::from_secs(10 * 60);

/// Which sticky types the slot keeps (contract §3.3). `ignored_users_changed`
/// is folded into `ignored_users_list`; `crypto_bootstrap` is kept per kind.
const STICKY_TYPES: &[&str] = &[
    "status",
    "login_ok",
    "login_failed",
    "session_restored_offline",
    "initial_sync_done",
    "room_list_sync_state",
    "sync_error",
    "sync_stalled",
    "crypto_bootstrap",
    "crypto_health",
    "key_backup_status",
    "own_identity_key",
    "ignored_users_list",
    "logged_out",
    "verification_request_received",
];

/// Field names that never enter the sticky slot, at any depth (contract
/// §3.3): tokens, private keys, recovery material, secret storage, bodies.
const FORBIDDEN_FIELDS: &[&str] = &[
    "access_token",
    "refresh_token",
    "password",
    "passphrase",
    "recovery_key",
    "private_key",
    "secret",
    "secrets",
    "secret_storage",
    "ssss",
    "body",
    "formatted_body",
    "new_body",
    "preview",
];

/// Remove every forbidden field, at any depth.
pub(crate) fn scrub(value: &mut Value) {
    match value {
        Value::Object(map) => {
            map.retain(|key, _| !FORBIDDEN_FIELDS.contains(&key.as_str()));
            for child in map.values_mut() {
                scrub(child);
            }
        }
        Value::Array(items) => items.iter_mut().for_each(scrub),
        _ => {}
    }
}

/// The versioned Session sticky slot.
///
/// Every entry carries `"_v"`: the slot version at which it was written, so a
/// reader can order entries of DIFFERENT types (review 2026-10-16, finding
/// 2): a `sync_error` is current only if no `room_list_sync_state` was written
/// after it. Two pairs are also settled here, so the common questions need no
/// comparison at all: `login_ok` and `login_failed` replace each other, and a
/// `room_list_sync_state` of `running` (a sync response arrived) removes
/// `sync_error` and `sync_stalled`.
#[derive(Default, Debug)]
pub(crate) struct Sticky {
    version: u64,
    state: BTreeMap<String, Value>,
    /// When the pending `verification_request_received` was recorded.
    pending_since: Option<Instant>,
}

/// `value` with `"_v": version` added (objects only).
fn stamped(value: &Value, version: u64) -> Value {
    let mut value = value.clone();
    if let Some(map) = value.as_object_mut() {
        map.insert("_v".to_owned(), version.into());
    }
    value
}

impl Sticky {
    /// Record one Session event, if its type is sticky. Returns whether the
    /// slot changed. `value` is already scrubbed.
    pub(crate) fn record(&mut self, event_type: &str, value: &Value) -> bool {
        self.record_at(event_type, value, Instant::now())
    }

    pub(crate) fn record_at(&mut self, event_type: &str, value: &Value, now: Instant) -> bool {
        let version = self.version + 1;
        match event_type {
            // Folded: the list as it stands after the change.
            "ignored_users_changed" => {
                let users = value.get("users").cloned().unwrap_or(Value::Array(Vec::new()));
                self.state.insert(
                    "ignored_users_list".to_owned(),
                    json!({ "type": "ignored_users_list", "ok": true, "users": users,
                            "_v": version }),
                );
            }
            // One per kind: verification, recovery and backup states arrive as
            // separate kinds, and the last one alone would lose the others.
            "crypto_bootstrap" => {
                let kind = value.get("kind").and_then(Value::as_str).unwrap_or("").to_owned();
                let slot = self
                    .state
                    .entry("crypto_bootstrap".to_owned())
                    .or_insert_with(|| Value::Object(Default::default()));
                if let Some(map) = slot.as_object_mut() {
                    map.insert(kind, stamped(value, version));
                }
            }
            // A cancelled flow is no longer pending: drop the request it named.
            // When both name the other party, they must agree: a cancellation
            // is only the requester's (or our own devices') to send.
            "verification_cancelled" => {
                let pending = self.state.get("verification_request_received");
                let flow = value.get("flow_id");
                if flow.is_none() || flow != pending.and_then(|v| v.get("flow_id")) {
                    return false;
                }
                let sender = value.get("other_user_id").and_then(Value::as_str);
                let requester = pending
                    .and_then(|v| v.get("other_user_id"))
                    .and_then(Value::as_str);
                if let (Some(sender), Some(requester)) = (sender, requester) {
                    if sender != requester {
                        return false;
                    }
                }
                self.state.remove("verification_request_received");
                self.pending_since = None;
            }
            other if STICKY_TYPES.contains(&other) => {
                match other {
                    "login_ok" => {
                        self.state.remove("login_failed");
                    }
                    "login_failed" => {
                        self.state.remove("login_ok");
                    }
                    "room_list_sync_state"
                        if value.get("state").and_then(Value::as_str) == Some("running") =>
                    {
                        self.state.remove("sync_error");
                        self.state.remove("sync_stalled");
                    }
                    "verification_request_received" => self.pending_since = Some(now),
                    _ => {}
                }
                self.state.insert(other.to_owned(), stamped(value, version));
            }
            _ => return false,
        }
        self.version = version;
        true
    }

    /// Drop a pending verification request older than
    /// [`PENDING_VERIFICATION_TTL`]. Returns whether the slot changed.
    pub(crate) fn expire(&mut self, now: Instant) -> bool {
        match self.pending_since {
            Some(since) if now.saturating_duration_since(since) >= PENDING_VERIFICATION_TTL => {
                self.pending_since = None;
                self.state.remove("verification_request_received");
                self.version += 1;
                true
            }
            _ => false,
        }
    }

    pub(crate) fn version(&self) -> u64 {
        self.version
    }

    /// `{"v":1,"version":N,"state":{...}}`.
    pub(crate) fn to_json(&self) -> Value {
        json!({ "v": 1, "version": self.version, "state": self.state })
    }
}

/// The facts the pre-filter reads from a notify event.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct NotifyFacts {
    pub timestamp_ms: u64,
    /// `None`: no verdict was computed.
    pub push_notify: Option<bool>,
    pub push_highlight: bool,
    pub mentions_me: bool,
    pub in_local_rules: bool,
}

/// Why a notify event was forwarded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ForwardReason {
    Push,
    Mention,
    LocalRule,
    NoVerdict,
}

impl ForwardReason {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Push => "push",
            Self::Mention => "mention",
            Self::LocalRule => "local_rule",
            Self::NoVerdict => "no_verdict",
        }
    }
}

/// The Background notification pre-filter (contract §3.5). Pure. Below the
/// floor nothing is forwarded; otherwise the push verdict, a mention or
/// highlight, the local-rule set, or the absence of any verdict forwards.
pub(crate) fn should_forward(facts: &NotifyFacts, floor_ms: u64) -> Option<ForwardReason> {
    if facts.timestamp_ms < floor_ms {
        return None;
    }
    if facts.push_notify == Some(true) {
        return Some(ForwardReason::Push);
    }
    if facts.push_highlight || facts.mentions_me {
        return Some(ForwardReason::Mention);
    }
    if facts.in_local_rules {
        return Some(ForwardReason::LocalRule);
    }
    if facts.push_notify.is_none() {
        return Some(ForwardReason::NoVerdict);
    }
    None
}

/// A bounded FIFO set of event ids.
#[derive(Default, Debug)]
pub(crate) struct BoundedIds {
    order: VecDeque<String>,
    set: HashSet<String>,
}

impl BoundedIds {
    pub(crate) fn insert(&mut self, id: &str) {
        if id.is_empty() || self.set.contains(id) {
            return;
        }
        while self.order.len() >= FORWARDED_IDS_CAP {
            if let Some(old) = self.order.pop_front() {
                self.set.remove(&old);
            }
        }
        self.order.push_back(id.to_owned());
        self.set.insert(id.to_owned());
    }

    pub(crate) fn contains(&self, id: &str) -> bool {
        self.set.contains(id)
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.order.len()
    }
}

/// `Lagged` rescans at most once per [`RESCAN_INTERVAL`].
#[derive(Debug, Default)]
pub(crate) struct RescanLimiter {
    last: Option<Instant>,
    pending: bool,
}

impl RescanLimiter {
    /// A `Lagged` arrived. True: rescan now. False: one is now pending.
    pub(crate) fn on_lagged(&mut self, now: Instant) -> bool {
        match self.last {
            Some(last) if now.saturating_duration_since(last) < RESCAN_INTERVAL => {
                self.pending = true;
                false
            }
            _ => {
                self.last = Some(now);
                self.pending = false;
                true
            }
        }
    }

    /// When a pending rescan becomes due, if one is pending.
    pub(crate) fn due_at(&self) -> Option<Instant> {
        if self.pending {
            self.last.map(|last| last + RESCAN_INTERVAL)
        } else {
            None
        }
    }

    /// A pending rescan whose time came. True: rescan now.
    pub(crate) fn on_due(&mut self, now: Instant) -> bool {
        match self.due_at() {
            Some(due) if now >= due => {
                self.last = Some(now);
                self.pending = false;
                true
            }
            _ => false,
        }
    }
}

/// Per-handle Background state. Shared by the bridge, the gate and the
/// aggregator.
pub(crate) struct BgState {
    pub(crate) profile: Profile,
    mode: AtomicU8,
    /// The bridge's client slot, WEAK: a stray `Arc<BgState>` that outlives
    /// the bridge must not keep its Client (and stores) alive, nor drop the
    /// last one on a thread with no Tokio runtime (review 2026-10-16, finding
    /// 8). Locked only with `try_lock` from the gate, which can run under a
    /// caller's own lock on it.
    client: Weak<Mutex<Option<Client>>>,
    events: Weak<Mutex<VecDeque<String>>>,
    commands: Weak<Mutex<VecDeque<String>>>,
    local_rules: RwLock<HashSet<String>>,
    sticky: Mutex<Sticky>,
    session_state_version: AtomicU64,
    forwarded: Mutex<BoundedIds>,
    dirty: AtomicBool,
    dirty_rooms: Mutex<HashSet<OwnedRoomId>>,
    rescan: AtomicBool,
    baseline_taken: AtomicBool,
    seq: AtomicU64,
    notable_updates_total: AtomicU64,
    /// Events the allow-list refused, for the tests and the S5 counters.
    gate_dropped: AtomicU64,
    /// UiOnly work skipped at its source (lib.rs counts each skip).
    work_skipped: AtomicU64,
    /// Aggregator abort handle, set while a sync session runs it.
    aggregator: Mutex<Option<tokio::task::AbortHandle>>,
}

impl BgState {
    pub(crate) fn new(
        profile: Profile,
        client: Arc<Mutex<Option<Client>>>,
        events: &EventQueueRef,
        commands: &EventQueueRef,
    ) -> Self {
        let local_rules = profile.local_rule_rooms.iter().cloned().collect();
        Self {
            profile,
            mode: AtomicU8::new(Mode::Background as u8),
            client: Arc::downgrade(&client),
            events: Arc::downgrade(events),
            commands: Arc::downgrade(commands),
            local_rules: RwLock::new(local_rules),
            sticky: Mutex::new(Sticky::default()),
            session_state_version: AtomicU64::new(0),
            forwarded: Mutex::new(BoundedIds::default()),
            dirty: AtomicBool::new(false),
            dirty_rooms: Mutex::new(HashSet::new()),
            rescan: AtomicBool::new(false),
            baseline_taken: AtomicBool::new(false),
            seq: AtomicU64::new(0),
            notable_updates_total: AtomicU64::new(0),
            gate_dropped: AtomicU64::new(0),
            work_skipped: AtomicU64::new(0),
            aggregator: Mutex::new(None),
        }
    }

    pub(crate) fn mode(&self) -> Mode {
        Mode::from_u8(self.mode.load(Ordering::SeqCst))
    }

    pub(crate) fn is_background(&self) -> bool {
        self.mode() == Mode::Background
    }

    /// Warm account adoption (`mx_rust_bg_promote`): from now on every
    /// `for_queue()` of this handle answers `None`, so the admission gate, the
    /// command lane and the UiOnly producers all take the foreground path. One
    /// way: a promoted handle is never demoted again (it is retired instead).
    pub(crate) fn promote_to_foreground(&self) {
        self.mode.store(Mode::Foreground as u8, Ordering::SeqCst);
    }

    pub(crate) fn watch_cap(&self) -> usize {
        self.profile.watch_cap
    }

    #[cfg(test)]
    pub(crate) fn gate_dropped(&self) -> u64 {
        self.gate_dropped.load(Ordering::Relaxed)
    }

    /// One unit of UiOnly work was not done.
    pub(crate) fn note_skipped(&self) {
        self.work_skipped.fetch_add(1, Ordering::Relaxed);
    }

    #[cfg(test)]
    pub(crate) fn work_skipped(&self) -> u64 {
        self.work_skipped.load(Ordering::Relaxed)
    }

    pub(crate) fn session_state_version(&self) -> u64 {
        self.session_state_version.load(Ordering::SeqCst)
    }

    pub(crate) fn notable_updates_total(&self) -> u64 {
        self.notable_updates_total.load(Ordering::Relaxed)
    }

    pub(crate) fn dirty(&self) -> bool {
        self.dirty.load(Ordering::SeqCst)
    }

    /// Replace the local-rule set.
    pub(crate) fn set_local_rules(&self, rooms: Vec<String>) {
        if let Ok(mut set) = self.local_rules.write() {
            *set = rooms.into_iter().collect();
        }
    }

    fn in_local_rules(&self, room: &str) -> bool {
        self.local_rules.read().map(|set| set.contains(room)).unwrap_or(false)
    }

    // ---- The gate ----------------------------------------------------------

    /// What reaches a Background queue. `None`: dropped (and counted).
    pub(crate) fn admit(&self, mut value: Value) -> Option<Value> {
        if !self.is_background() {
            return Some(value);
        }
        let event_type = value.get("type").and_then(Value::as_str).unwrap_or("").to_owned();
        let class = labs::classify(&event_type);
        if !labs::flows_in_background(class) {
            self.gate_dropped.fetch_add(1, Ordering::Relaxed);
            return None;
        }
        match class {
            Some(EventClass::Session) => {
                // `login_ok` never carries a token here (custody is the only
                // path), and nothing sticky may carry a secret.
                if let Some(map) = value.as_object_mut() {
                    map.remove("access_token");
                    map.remove("refresh_token");
                }
                // The store stage comes from the open context in scope (the
                // restore that hit the refusal), never from the message text,
                // which also carries the server's words (finding 5).
                if event_type == "login_failed" {
                    if let (Some(reason), Some(map)) =
                        (labs::current_store_refusal(), value.as_object_mut())
                    {
                        map.insert("stage".to_owned(), "store".into());
                        map.insert("reason".to_owned(), reason.into());
                    }
                }
                // `rate_limited`/`retry_after_ms` are set at the source from the
                // error's kind and status (`rate_limit_facts`); a sync_error
                // with no error behind it is not rate limited.
                if event_type == "sync_error" {
                    if let Some(map) = value.as_object_mut() {
                        map.entry("rate_limited").or_insert(false.into());
                    }
                }
                let mut sticky_copy = value.clone();
                scrub(&mut sticky_copy);
                if let Ok(mut sticky) = self.sticky.lock() {
                    if sticky.record(&event_type, &sticky_copy) {
                        self.session_state_version.store(sticky.version(), Ordering::SeqCst);
                    }
                }
                Some(value)
            }
            Some(EventClass::Notify) => self.admit_notify(&event_type, value),
            _ => None,
        }
    }

    fn admit_notify(&self, event_type: &str, mut value: Value) -> Option<Value> {
        let room_id = value.get("room_id").and_then(Value::as_str).unwrap_or("").to_owned();
        match event_type {
            "timeline_event" | "reaction_event" | "event_decrypted" => {
                let source = if event_type == "reaction_event" {
                    value.clone()
                } else {
                    value.get("event").cloned().unwrap_or(Value::Null)
                };
                let event_id = source.get("event_id").and_then(Value::as_str).unwrap_or("");
                // A late decryption of an event forwarded earlier always follows it.
                if event_type == "event_decrypted" && self.was_forwarded(event_id) {
                    return Some(value);
                }
                // A reaction notifies only on a message of ours (`noteReaction`),
                // and a Background host has sent nothing, so only the store's
                // own word for it counts.
                if event_type == "reaction_event"
                    && source.get("target_is_own").and_then(Value::as_bool) != Some(true)
                {
                    self.gate_dropped.fetch_add(1, Ordering::Relaxed);
                    return None;
                }
                // `event_decrypted` of an event NOT forwarded is judged on its own
                // decrypted facts, its verdict recomputed on the decrypted content
                // (`decrypted_event_json`): an undecryptable mention in a
                // mentions-only room was dropped on the encrypted verdict, and its
                // decryption is the only place the mention can be seen (review
                // 2026-10-16, finding 3; the foreground announces every one).
                let facts = NotifyFacts {
                    timestamp_ms: source.get("timestamp_ms").and_then(Value::as_u64).unwrap_or(0),
                    push_notify: source.get("push_notify").and_then(Value::as_bool),
                    push_highlight: source
                        .get("push_highlight")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                    mentions_me: source.get("mentions_me").and_then(Value::as_bool).unwrap_or(false),
                    in_local_rules: self.in_local_rules(&room_id),
                };
                let Some(reason) = should_forward(&facts, self.profile.floor_ms) else {
                    self.gate_dropped.fetch_add(1, Ordering::Relaxed);
                    return None;
                };
                if let Ok(mut ids) = self.forwarded.lock() {
                    ids.insert(event_id);
                }
                let sender = source.get("sender").and_then(Value::as_str).unwrap_or("");
                let (room_name, is_direct, sender_name) = self.room_facts(&room_id, sender);
                if let Some(map) = value.as_object_mut() {
                    map.insert(
                        "bg".to_owned(),
                        json!({
                            "reason": reason.as_str(),
                            "room_name": room_name,
                            "is_direct": is_direct,
                            "sender_display_name": sender_name,
                        }),
                    );
                }
                Some(value)
            }
            "event_redacted" => {
                let id = value.get("redacts").and_then(Value::as_str).unwrap_or("");
                if self.was_forwarded(id) {
                    Some(value)
                } else {
                    self.gate_dropped.fetch_add(1, Ordering::Relaxed);
                    None
                }
            }
            _ => None,
        }
    }

    fn was_forwarded(&self, event_id: &str) -> bool {
        !event_id.is_empty()
            && self.forwarded.lock().map(|ids| ids.contains(event_id)).unwrap_or(false)
    }

    /// The room's cached name, whether it is a DM, and the sender's name from
    /// the room's heroes. Synchronous and store-free: `try_lock` on the
    /// client slot (a caller may hold it), cached values only, `""` when
    /// unresolved (C++ falls back to the localpart).
    fn room_facts(&self, room_id: &str, sender: &str) -> (String, bool, String) {
        let Some(slot) = self.client.upgrade() else {
            return (String::new(), false, String::new());
        };
        let Ok(guard) = slot.try_lock() else {
            return (String::new(), false, String::new());
        };
        let Some(client) = guard.as_ref() else {
            return (String::new(), false, String::new());
        };
        let Ok(room_id) = matrix_sdk::ruma::RoomId::parse(room_id) else {
            return (String::new(), false, String::new());
        };
        let Some(room) = client.get_room(&room_id) else {
            return (String::new(), false, String::new());
        };
        let name = room.cached_display_name().map(|n| n.to_string()).unwrap_or_default();
        let sender_name = room
            .heroes()
            .into_iter()
            .find(|hero| hero.user_id.as_str() == sender)
            .and_then(|hero| hero.display_name)
            .unwrap_or_default();
        (name, room.direct_targets_length() > 0, sender_name)
    }

    // ---- Session slot ------------------------------------------------------

    pub(crate) fn take_session_state(&self) -> String {
        self.expire_session_state(Instant::now());
        self.sticky
            .lock()
            .map(|sticky| sticky.to_json().to_string())
            .unwrap_or_else(|_| json!({ "v": 1, "version": 0, "state": {} }).to_string())
    }

    /// Expire a stale pending verification request (`Sticky::expire`). Run
    /// on every poll, so the version a host compares moves when it happens.
    pub(crate) fn expire_session_state(&self, now: Instant) {
        if let Ok(mut sticky) = self.sticky.lock() {
            if sticky.expire(now) {
                self.session_state_version.store(sticky.version(), Ordering::SeqCst);
            }
        }
    }

    // ---- Aggregator --------------------------------------------------------

    pub(crate) fn note_room(&self, room_id: OwnedRoomId) {
        self.notable_updates_total.fetch_add(1, Ordering::Relaxed);
        if let Ok(mut rooms) = self.dirty_rooms.lock() {
            rooms.insert(room_id);
        }
        self.dirty.store(true, Ordering::SeqCst);
    }

    pub(crate) fn note_rescan(&self) {
        self.rescan.store(true, Ordering::SeqCst);
        self.dirty.store(true, Ordering::SeqCst);
    }

    /// A new sync session: the next take is a baseline.
    pub(crate) fn note_session_start(&self) {
        self.dirty.store(true, Ordering::SeqCst);
    }

    pub(crate) fn set_aggregator(&self, handle: Option<tokio::task::AbortHandle>) {
        if let Ok(mut slot) = self.aggregator.lock() {
            if let Some(previous) = std::mem::replace(&mut *slot, handle) {
                previous.abort();
            }
        }
    }

    /// The summary JSON (contract §3.2). Clears `dirty`. The counts table is
    /// the WHOLE table every time; `rows` is empty in Phase 1 (`max_rows` 0).
    /// Serialized under the client-slot lock so the Client is never cloned
    /// here: a clone that became the last reference would close the stores on
    /// this (runtime-less) thread.
    pub(crate) fn take(&self, _max_rows: u32) -> String {
        self.dirty.store(false, Ordering::SeqCst);
        if let Ok(mut rooms) = self.dirty_rooms.lock() {
            rooms.clear();
        }
        let rescan = self.rescan.swap(false, Ordering::SeqCst);
        let baseline = !self.baseline_taken.swap(true, Ordering::SeqCst);
        let seq = self.seq.fetch_add(1, Ordering::SeqCst) + 1;
        let mut counts = Vec::new();
        let mut invites = 0u64;
        let slot = self.client.upgrade();
        if let Some(Ok(guard)) = slot.as_ref().map(|slot| slot.lock()) {
            if let Some(client) = guard.as_ref() {
                for room in client.rooms() {
                    match room.state() {
                        RoomState::Joined if !room.is_space() => {
                            let tuple = RoomCounts::of(&room).and_then(|c| count_tuple(&c));
                            if let Some(tuple) = tuple {
                                counts.push(tuple);
                            }
                        }
                        RoomState::Invited => invites += 1,
                        _ => {}
                    }
                }
            }
        }
        json!({
            "v": 1,
            "seq": seq,
            "baseline": baseline,
            "rescan": rescan,
            "counts": counts,
            "rows": [],
            "invites": invites,
        })
        .to_string()
    }

    /// `mx_rust_bg_poll`'s queue lengths.
    pub(crate) fn queue_lengths(&self) -> (u32, u32) {
        let len = |queue: &Weak<Mutex<VecDeque<String>>>| {
            queue
                .upgrade()
                .and_then(|q| q.lock().ok().map(|g| g.len() as u32))
                .unwrap_or(0)
        };
        (len(&self.events), len(&self.commands))
    }
}

/// `sync_error`'s rate-limit facts in Background (contract §3.3), from the
/// error ITSELF: `M_LIMIT_EXCEEDED` (with the server's retry hint) or an HTTP
/// 429. Never from text, where a port like `:8429` or an id read as a 429
/// and paused the homeserver (review 2026-10-16, finding 5). Pure.
pub(crate) fn rate_limit_facts(
    kind: Option<&matrix_sdk::ruma::api::error::ErrorKind>,
    status: Option<u16>,
) -> (bool, Option<u64>) {
    use matrix_sdk::ruma::api::error::{ErrorKind, LimitExceededErrorData};
    match kind {
        Some(ErrorKind::LimitExceeded(LimitExceededErrorData { retry_after, .. })) => {
            (true, retry_after.as_ref().and_then(crate::presence::retry_after_to_ms))
        }
        _ => (status == Some(429), None),
    }
}

/// Add [`rate_limit_facts`] to a `sync_error` payload.
pub(crate) fn with_rate_limit(mut value: Value, facts: (bool, Option<u64>)) -> Value {
    if let Some(map) = value.as_object_mut() {
        map.insert("rate_limited".to_owned(), facts.0.into());
        if let Some(ms) = facts.1 {
            map.insert("retry_after_ms".to_owned(), ms.into());
        }
    }
    value
}

/// The counts a room contributes, read from cached state only.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct RoomCounts {
    pub room_id: String,
    pub encrypted: bool,
    pub encryption_known: bool,
    pub client_notifications: u64,
    pub client_mentions: u64,
    pub server_notifications: u64,
    pub server_highlights: u64,
    pub counted_from_receipt: bool,
    pub unread_messages: u64,
    pub marked_unread: bool,
}

impl RoomCounts {
    /// `None` for a quiet room, decided from the plain counters BEFORE the
    /// room id is allocated or the read receipts cloned: most rooms are quiet
    /// and `take` runs per tick (review 2026-10-16, finding 12).
    fn of(room: &matrix_sdk::Room) -> Option<Self> {
        let server = room.unread_notification_counts();
        let mut counts = Self {
            room_id: String::new(),
            encrypted: false,
            encryption_known: false,
            client_notifications: room.num_unread_notifications(),
            client_mentions: room.num_unread_mentions(),
            server_notifications: server.notification_count,
            server_highlights: server.highlight_count,
            counted_from_receipt: false,
            unread_messages: room.num_unread_messages(),
            marked_unread: room.is_marked_unread(),
        };
        if counts.is_quiet() {
            return None;
        }
        let state = room.encryption_state();
        counts.encrypted = state.is_encrypted();
        counts.encryption_known = !state.is_unknown();
        // Only the unencrypted thread-only rule reads the receipt.
        if !counts.encrypted {
            counts.counted_from_receipt = room.read_receipts().latest_active.is_some();
        }
        counts.room_id = room.room_id().to_string();
        Some(counts)
    }

    /// Every counter zero and not marked: `count_tuple` would return `None`
    /// whatever the encryption state and the receipt (a test walks it).
    pub(crate) fn is_quiet(&self) -> bool {
        self.client_notifications == 0
            && self.client_mentions == 0
            && self.server_notifications == 0
            && self.server_highlights == 0
            && self.unread_messages == 0
            && !self.marked_unread
    }
}

pub(crate) const FLAG_HAS_UNREAD: u64 = 1;
pub(crate) const FLAG_MARKED_UNREAD: u64 = 2;
pub(crate) const FLAG_THREADS_ONLY: u64 = 4;
pub(crate) const FLAG_MENTION_IN_THREADS: u64 = 8;
pub(crate) const FLAG_ENCRYPTED: u64 = 16;
pub(crate) const FLAG_ENCRYPTION_KNOWN: u64 = 32;
pub(crate) const FLAG_LOWER_BOUND: u64 = 64;

/// One counts tuple `[room_id, unread, highlight, flags]`, computed with the
/// SAME helpers `room_payload` uses, or `None` when the room contributes
/// nothing. Pure.
pub(crate) fn count_tuple(counts: &RoomCounts) -> Option<Value> {
    let (unread, highlight) = crate::unread_and_highlight_counts(
        counts.encrypted,
        counts.client_notifications,
        counts.client_mentions,
        counts.server_notifications,
        counts.server_highlights,
    );
    let (threads_only, mention_in_threads) = crate::unread_in_threads_only(
        counts.encrypted,
        counts.counted_from_receipt,
        counts.client_notifications,
        counts.client_mentions,
        counts.server_notifications,
        counts.server_highlights,
    );
    let has_unread = counts.unread_messages > 0;
    let mut flags = 0u64;
    if has_unread {
        flags |= FLAG_HAS_UNREAD;
    }
    if counts.marked_unread {
        flags |= FLAG_MARKED_UNREAD;
    }
    if threads_only {
        flags |= FLAG_THREADS_ONLY;
    }
    if mention_in_threads {
        flags |= FLAG_MENTION_IN_THREADS;
    }
    let anything = unread > 0 || highlight > 0 || flags != 0;
    if !anything {
        return None;
    }
    if counts.encrypted {
        // Client-computed from the event cache since the receipt: a gap there
        // makes these a lower bound (design §3.2.5, R2-L1).
        flags |= FLAG_ENCRYPTED | FLAG_LOWER_BOUND;
    }
    if counts.encryption_known {
        flags |= FLAG_ENCRYPTION_KNOWN;
    }
    Some(json!([counts.room_id, unread, highlight, flags]))
}

/// What to do with a room not yet watched, given the watched set with bump
/// stamps and the cap.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum WatchDecision {
    /// Room under the cap: watch it.
    Watch,
    /// At the cap and more recent than the least recent watched room: unwatch
    /// that one (lowest stamp first, ties by id), then watch this one.
    Replace(OwnedRoomId),
    /// At the cap and not more recent than any watched room.
    Skip,
}

/// Pure (design §3.2.3): the watched set is capped by bump stamp.
pub(crate) fn watch_decision(watched: &[(OwnedRoomId, u64)], cap: usize, stamp: u64) -> WatchDecision {
    if watched.len() < cap {
        return WatchDecision::Watch;
    }
    let lowest = watched
        .iter()
        .min_by(|a, b| a.1.cmp(&b.1).then_with(|| a.0.cmp(&b.0)));
    match lowest {
        Some((id, lowest)) if stamp > *lowest => WatchDecision::Replace(id.clone()),
        _ => WatchDecision::Skip,
    }
}

/// The room list as an id mirror, for Background watch maintenance: apply
/// one diff and return the ids it took out of the list, so a room that left
/// gives its watch slot back (review 2026-10-16, finding 12). A position the
/// mirror lacks is ignored, never a panic (eyeball's own `apply` panics).
/// Pure.
pub(crate) fn apply_order_diff<T: Clone>(
    order: &mut Vec<T>,
    diff: matrix_sdk_ui::eyeball_im::VectorDiff<T>,
) -> Vec<T> {
    use matrix_sdk_ui::eyeball_im::VectorDiff;
    match diff {
        VectorDiff::Append { values } => {
            order.extend(values);
            Vec::new()
        }
        VectorDiff::Clear => std::mem::take(order),
        VectorDiff::PushFront { value } => {
            order.insert(0, value);
            Vec::new()
        }
        VectorDiff::PushBack { value } => {
            order.push(value);
            Vec::new()
        }
        VectorDiff::PopFront => {
            if order.is_empty() { Vec::new() } else { vec![order.remove(0)] }
        }
        VectorDiff::PopBack => order.pop().into_iter().collect(),
        VectorDiff::Insert { index, value } => {
            if index <= order.len() {
                order.insert(index, value);
            }
            Vec::new()
        }
        VectorDiff::Set { index, value } => match order.get_mut(index) {
            Some(slot) => vec![std::mem::replace(slot, value)],
            None => Vec::new(),
        },
        VectorDiff::Remove { index } => {
            if index < order.len() { vec![order.remove(index)] } else { Vec::new() }
        }
        VectorDiff::Truncate { length } => {
            if length < order.len() { order.split_off(length) } else { Vec::new() }
        }
        VectorDiff::Reset { values } => {
            std::mem::replace(order, values.into_iter().collect())
        }
    }
}

/// The aggregator: notable room updates into the dirty set. Runs for the
/// life of a sync session on its runtime; aborted with it.
pub(crate) async fn run_aggregator(client: Client, state: Arc<BgState>) {
    let mut updates = client.room_info_notable_update_receiver();
    drop(client);
    state.note_session_start();
    let mut limiter = RescanLimiter::default();
    loop {
        let due = limiter.due_at();
        tokio::select! {
            update = updates.recv() => match update {
                Ok(update) => state.note_room(update.room_id),
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                    if limiter.on_lagged(Instant::now()) {
                        state.note_rescan();
                    }
                }
                Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
            },
            _ = sleep_until_opt(due) => {
                if limiter.on_due(Instant::now()) {
                    state.note_rescan();
                }
            }
        }
    }
}

async fn sleep_until_opt(at: Option<Instant>) {
    match at {
        Some(at) => tokio::time::sleep_until(at.into()).await,
        None => std::future::pending::<()>().await,
    }
}

// ---- Registry: which queues belong to a Background handle -------------------

type Registry = HashMap<usize, (Weak<Mutex<VecDeque<String>>>, Weak<BgState>)>;

fn registry() -> &'static RwLock<Registry> {
    static REGISTRY: OnceLock<RwLock<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(|| RwLock::new(HashMap::new()))
}

fn key(queue: &EventQueueRef) -> usize {
    Arc::as_ptr(queue) as *const () as usize
}

/// Register a Background handle's two queues. The entry holds a `Weak` of the
/// queue, so its allocation (and address) cannot be reused while the entry
/// exists: no ABA.
pub(crate) fn register(events: &EventQueueRef, commands: &EventQueueRef, state: &Arc<BgState>) {
    if let Ok(mut map) = registry().write() {
        map.retain(|_, (queue, state)| queue.strong_count() > 0 && state.strong_count() > 0);
        for queue in [events, commands] {
            map.insert(key(queue), (Arc::downgrade(queue), Arc::downgrade(state)));
        }
    }
}

pub(crate) fn unregister(events: &EventQueueRef, commands: &EventQueueRef) {
    if let Ok(mut map) = registry().write() {
        map.remove(&key(events));
        map.remove(&key(commands));
    }
}

/// The Background state a queue belongs to. `None` at one relaxed load when
/// Labs is off.
pub(crate) fn for_queue(queue: &EventQueueRef) -> Option<Arc<BgState>> {
    if !labs::active() {
        return None;
    }
    let map = registry().read().ok()?;
    let (weak_queue, state) = map.get(&key(queue))?;
    // The same allocation, still alive.
    if !weak_queue.upgrade().is_some_and(|q| Arc::ptr_eq(&q, queue)) {
        return None;
    }
    state.upgrade().filter(|state| state.is_background())
}

/// The gate `crate::enqueue` calls when Labs is on. A queue no Background
/// handle owns passes the value through unchanged.
pub(crate) fn admit(queue: &EventQueueRef, value: Value) -> Option<Value> {
    match for_queue(queue) {
        Some(state) => state.admit(value),
        None => Some(value),
    }
}

/// The command lane in Background: CommandResult is empty in Phase 1, so
/// nothing is queued. True: drop.
pub(crate) fn refuses_command(queue: &EventQueueRef) -> bool {
    match for_queue(queue) {
        Some(state) => {
            state.gate_dropped.fetch_add(1, Ordering::Relaxed);
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile(floor_ms: u64) -> Profile {
        crate::labs::parse_profile(
            &json!({
                "v": 1, "slot": "t0",
                "require_account": {"user_id": "@u:hs", "device_id": "DEV"},
                "strict_trust": false, "receipt_privacy": 2, "index_all_hold": 0,
                "floor_ms": floor_ms, "local_rule_rooms": ["!local:hs"],
                "max_blocking_threads": 8, "sqlite": "low_memory",
                "retry_limit": 3, "max_retry_time_ms": 30000, "watch_cap": 3
            })
            .to_string(),
        )
        .unwrap()
    }

    fn state(floor_ms: u64) -> (Arc<BgState>, EventQueueRef, EventQueueRef) {
        let events: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let commands: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let st = Arc::new(BgState::new(
            profile(floor_ms),
            Arc::new(Mutex::new(None)),
            &events,
            &commands,
        ));
        (st, events, commands)
    }

    fn timeline(room: &str, id: &str, ts: u64, extra: Value) -> Value {
        let mut event = json!({ "event_id": id, "sender": "@s:hs", "timestamp_ms": ts,
                                "body": "secret body" });
        for (k, v) in extra.as_object().unwrap() {
            event[k] = v.clone();
        }
        json!({ "type": "timeline_event", "room_id": room, "event": event })
    }

    #[test]
    fn the_pre_filter_forwards_only_what_can_notify_and_nothing_below_the_floor() {
        let base = NotifyFacts { timestamp_ms: 1000, push_notify: Some(false), ..Default::default() };
        assert_eq!(should_forward(&base, 0), None, "a verdict of no and nothing else");
        let mut f = base.clone();
        f.push_notify = Some(true);
        assert_eq!(should_forward(&f, 0), Some(ForwardReason::Push));
        assert_eq!(should_forward(&f, 1001), None, "the floor beats every reason");
        assert_eq!(should_forward(&f, 1000), Some(ForwardReason::Push), "the floor is inclusive");
        let mut f = base.clone();
        f.push_highlight = true;
        assert_eq!(should_forward(&f, 0), Some(ForwardReason::Mention));
        let mut f = base.clone();
        f.mentions_me = true;
        assert_eq!(should_forward(&f, 0), Some(ForwardReason::Mention));
        let mut f = base.clone();
        f.in_local_rules = true;
        assert_eq!(should_forward(&f, 0), Some(ForwardReason::LocalRule));
        let mut f = base.clone();
        f.push_notify = None;
        assert_eq!(should_forward(&f, 0), Some(ForwardReason::NoVerdict));
    }

    #[test]
    fn the_gate_is_an_allow_list_and_drops_ui_work() {
        let (st, _, _) = state(0);
        assert!(st.admit(json!({ "type": "room_list_reset", "rooms": [] })).is_none());
        assert!(st.admit(json!({ "type": "typing_update" })).is_none());
        assert!(st.admit(json!({ "type": "brand_new_crypto_event" })).is_none());
        assert!(
            st.admit(json!({ "type": "session_tokens_refreshed", "access_token": "x" })).is_none(),
            "tokens travel by custody only"
        );
        assert_eq!(st.gate_dropped(), 4);
        assert!(st.admit(json!({ "type": "status", "state": "syncing" })).is_some());
        assert!(st.admit(json!({ "type": "queue_overflow" })).is_some());
    }

    #[test]
    fn notify_events_are_filtered_and_labelled_and_their_follow_ups_track_them() {
        let (st, _, _) = state(500);
        // Below the floor, even with a push verdict.
        assert!(st.admit(timeline("!a:hs", "$old", 100, json!({"push_notify": true}))).is_none());
        // A verdict of "no" in a room with no local rule: dropped.
        assert!(st.admit(timeline("!a:hs", "$quiet", 900, json!({"push_notify": false}))).is_none());
        // The same in a local-rule room: forwarded.
        let local = st
            .admit(timeline("!local:hs", "$local", 900, json!({"push_notify": false})))
            .expect("local rule");
        assert_eq!(local["bg"]["reason"], "local_rule");
        let pushed = st
            .admit(timeline("!a:hs", "$push", 900, json!({"push_notify": true})))
            .expect("push");
        assert_eq!(pushed["bg"]["reason"], "push");
        assert_eq!(pushed["bg"]["room_name"], "");
        assert_eq!(pushed["bg"]["sender_display_name"], "");
        assert_eq!(pushed["bg"]["is_direct"], false);
        let mention = st
            .admit(timeline("!a:hs", "$m", 900, json!({"push_notify": false, "mentions_me": true})))
            .expect("mention");
        assert_eq!(mention["bg"]["reason"], "mention");
        // Reactions carry no verdict, and notify only on a message of ours.
        let reaction = st
            .admit(json!({"type": "reaction_event", "room_id": "!a:hs", "event_id": "$r",
                          "target_is_own": true, "timestamp_ms": 900}))
            .expect("no verdict forwards");
        assert_eq!(reaction["bg"]["reason"], "no_verdict");
        for target_is_own in [json!(false), Value::Null] {
            assert!(st
                .admit(json!({"type": "reaction_event", "room_id": "!a:hs", "event_id": "$r2",
                              "target_is_own": target_is_own, "timestamp_ms": 900}))
                .is_none(), "a reaction to someone else's message: {target_is_own}");
        }
        // A forwarded event's late decryption always follows it; another is
        // judged on its OWN decrypted facts.
        assert!(st
            .admit(json!({"type": "event_decrypted", "room_id": "!a:hs",
                          "event": {"event_id": "$push"}}))
            .is_some());
        assert!(st
            .admit(json!({"type": "event_decrypted", "room_id": "!a:hs",
                          "event": {"event_id": "$quiet", "timestamp_ms": 900,
                                    "push_notify": false}}))
            .is_none());
        assert!(st
            .admit(json!({"type": "event_redacted", "room_id": "!a:hs", "redacts": "$m"}))
            .is_some());
        assert!(st
            .admit(json!({"type": "event_redacted", "room_id": "!a:hs", "redacts": "$quiet"}))
            .is_none());
        // Local rules are replaceable.
        st.set_local_rules(vec![]);
        assert!(st
            .admit(timeline("!local:hs", "$l2", 900, json!({"push_notify": false})))
            .is_none());
    }

    // Review 2026-10-16, finding 3: in a mentions-only room an undecryptable
    // event's verdict (computed on the ciphertext) is "no", so it is dropped;
    // when it decrypts into a mention, that decryption must still notify, and
    // its own redaction then follows it. Below the floor it never does.
    #[test]
    fn a_mention_that_decrypts_late_is_forwarded_on_its_decrypted_verdict() {
        let (st, _, _) = state(500);
        let utd = timeline("!a:hs", "$late", 900,
                           json!({"push_notify": false, "undecryptable": true}));
        assert!(st.admit(utd).is_none(), "the encrypted verdict says no");
        let decrypted = st
            .admit(json!({"type": "event_decrypted", "room_id": "!a:hs",
                          "event": {"event_id": "$late", "sender": "@s:hs", "timestamp_ms": 900,
                                    "push_notify": true, "push_highlight": true,
                                    "mentions_me": true}}))
            .expect("a late mention must notify");
        assert_eq!(decrypted["bg"]["reason"], "push");
        assert!(st
            .admit(json!({"type": "event_redacted", "room_id": "!a:hs", "redacts": "$late"}))
            .is_some(), "its redaction follows it now");
        // The floor still applies to a late decryption.
        assert!(st
            .admit(json!({"type": "event_decrypted", "room_id": "!a:hs",
                          "event": {"event_id": "$older", "timestamp_ms": 100,
                                    "mentions_me": true}}))
            .is_none());
    }

    #[test]
    fn the_forwarded_id_set_is_bounded() {
        let mut ids = BoundedIds::default();
        for i in 0..(FORWARDED_IDS_CAP + 10) {
            ids.insert(&format!("$e{i}"));
        }
        assert_eq!(ids.len(), FORWARDED_IDS_CAP);
        assert!(!ids.contains("$e0"), "the oldest goes first");
        assert!(ids.contains(&format!("$e{}", FORWARDED_IDS_CAP + 9)));
    }

    #[test]
    fn a_ten_thousand_event_flood_loses_no_mention() {
        let (st, _, _) = state(0);
        let mut forwarded = 0;
        for i in 0..10_000u64 {
            let extra = if i % 1000 == 7 {
                json!({"push_notify": false, "mentions_me": true})
            } else {
                json!({"push_notify": false})
            };
            if st.admit(timeline("!busy:hs", &format!("$e{i}"), 10 + i, extra)).is_some() {
                forwarded += 1;
            }
            // Non-notify noise in between, as a busy account produces.
            assert!(st.admit(json!({"type": "room_list_set", "index": 1})).is_none());
        }
        assert_eq!(forwarded, 10, "every mention, and nothing else");
    }

    #[test]
    fn the_sticky_slot_keeps_the_last_of_each_type_and_never_a_secret() {
        let (st, _, _) = state(0);
        let planted = "PLANTED-SECRET-7f3a";
        st.admit(json!({"type": "status", "state": "connecting"}));
        st.admit(json!({"type": "status", "state": "syncing"}));
        let ok = st
            .admit(json!({"type": "login_ok", "user_id": "@u:hs", "device_id": "DEV",
                          "access_token": planted, "refresh_token": planted}))
            .unwrap();
        assert!(ok.get("access_token").is_none() && ok.get("refresh_token").is_none(),
                "login_ok must cross with no token");
        st.admit(json!({"type": "crypto_bootstrap", "kind": "verification", "state": "verified",
                        "nested": {"recovery_key": planted, "body": planted}}));
        st.admit(json!({"type": "crypto_bootstrap", "kind": "backup", "state": "enabled"}));
        st.admit(json!({"type": "ignored_users_changed", "users": ["@x:hs"]}));
        st.admit(json!({"type": "verification_request_received", "flow_id": "f1"}));
        st.admit(json!({"type": "queue_overflow"}));
        let taken: Value = serde_json::from_str(&st.take_session_state()).unwrap();
        let text = taken.to_string();
        assert!(!text.contains(planted), "a planted secret reached the slot: {text}");
        let s = &taken["state"];
        assert_eq!(s["status"]["state"], "syncing");
        assert_eq!(s["crypto_bootstrap"]["verification"]["state"], "verified");
        assert_eq!(s["crypto_bootstrap"]["backup"]["state"], "enabled");
        assert_eq!(s["ignored_users_list"]["users"][0], "@x:hs");
        assert_eq!(s["verification_request_received"]["flow_id"], "f1");
        assert!(s.get("queue_overflow").is_none(), "not a sticky type");
        assert_eq!(taken["version"].as_u64(), Some(st.session_state_version()));
        // Every entry says when it was written.
        assert!(s["status"]["_v"].as_u64().unwrap() < s["login_ok"]["_v"].as_u64().unwrap());
        assert_eq!(s["verification_request_received"]["_v"].as_u64(), taken["version"].as_u64());
        assert!(s["crypto_bootstrap"]["backup"]["_v"].as_u64().is_some());
        assert!(s["ignored_users_list"]["_v"].as_u64().is_some());
        // Walk every payload for a forbidden field name.
        fn walk(value: &Value, path: &str) {
            match value {
                Value::Object(map) => {
                    for (k, v) in map {
                        assert!(!FORBIDDEN_FIELDS.contains(&k.as_str()), "{path}.{k}");
                        walk(v, &format!("{path}.{k}"));
                    }
                }
                Value::Array(items) => items.iter().for_each(|v| walk(v, path)),
                _ => {}
            }
        }
        walk(s, "state");
        // A cancelled flow is not pending any more; another flow's cancel is
        // not this one's.
        let before = st.session_state_version();
        st.admit(json!({"type": "verification_cancelled", "flow_id": "other"}));
        assert_eq!(st.session_state_version(), before);
        st.admit(json!({"type": "verification_cancelled", "flow_id": "f1"}));
        let taken: Value = serde_json::from_str(&st.take_session_state()).unwrap();
        assert!(taken["state"].get("verification_request_received").is_none());
        assert!(st.session_state_version() > before);
    }

    // Review 2026-10-16, finding 2: the slot says which state is current.
    #[test]
    fn the_sticky_slot_orders_its_entries_and_settles_the_pairs_that_contradict() {
        let (st, _, _) = state(0);
        let read = |st: &BgState| -> Value {
            serde_json::from_str::<Value>(&st.take_session_state()).unwrap()["state"].clone()
        };
        // A store refusal, then the same handle restores: only login_ok is left.
        st.admit(json!({"type": "login_failed", "message": "x"}));
        st.admit(json!({"type": "login_ok", "user_id": "@u:hs"}));
        let s = read(&st);
        assert!(s.get("login_failed").is_none(), "a stale failure beside login_ok: {s}");
        assert_eq!(s["login_ok"]["user_id"], "@u:hs");
        st.admit(json!({"type": "login_failed", "message": "y"}));
        let s = read(&st);
        assert!(s.get("login_ok").is_none(), "{s}");
        // A sync error, then a response: running clears it and the stall.
        st.admit(json!({"type": "sync_error", "message": "boom"}));
        st.admit(json!({"type": "sync_stalled", "phase": "no_progress"}));
        st.admit(json!({"type": "room_list_sync_state", "state": "offline"}));
        let s = read(&st);
        assert!(s["sync_error"]["_v"].as_u64() < s["room_list_sync_state"]["_v"].as_u64());
        st.admit(json!({"type": "room_list_sync_state", "state": "running"}));
        let s = read(&st);
        assert!(s.get("sync_error").is_none() && s.get("sync_stalled").is_none(), "{s}");
        // An error after the running state is current, and says so by order.
        st.admit(json!({"type": "sync_error", "message": "again"}));
        let s = read(&st);
        assert!(s["sync_error"]["_v"].as_u64() > s["room_list_sync_state"]["_v"].as_u64());
    }

    // Review 2026-10-16, finding 4: a pending request leaves the slot when its
    // requester cancels, never on a stranger's cancel naming its flow, and
    // after the SDK's timeout if nobody says anything.
    #[test]
    fn a_pending_verification_request_is_cancelled_by_its_requester_or_expires() {
        let mut sticky = Sticky::default();
        let t0 = Instant::now();
        let request = json!({"type": "verification_request_received", "flow_id": "f1",
                             "other_user_id": "@peer:hs"});
        assert!(sticky.record_at("verification_request_received", &request, t0));
        assert!(!sticky.record_at("verification_cancelled",
            &json!({"flow_id": "f1", "other_user_id": "@stranger:hs"}), t0));
        assert!(!sticky.expire(t0 + PENDING_VERIFICATION_TTL - Duration::from_secs(1)));
        assert!(sticky.record_at("verification_cancelled",
            &json!({"flow_id": "f1", "other_user_id": "@peer:hs"}), t0));
        assert!(sticky.to_json()["state"].get("verification_request_received").is_none());
        // Nobody cancels: gone after the SDK's timeout, and the version moves.
        sticky.record_at("verification_request_received", &request, t0);
        let before = sticky.version();
        assert!(sticky.expire(t0 + PENDING_VERIFICATION_TTL));
        assert!(sticky.version() > before);
        assert!(sticky.to_json()["state"].get("verification_request_received").is_none());
        assert!(!sticky.expire(t0 + PENDING_VERIFICATION_TTL * 2), "once");
        // Through the state: the poll path moves the version a host compares.
        let (st, _, _) = state(0);
        st.admit(request.clone());
        let before = st.session_state_version();
        st.expire_session_state(Instant::now() + PENDING_VERIFICATION_TTL);
        assert!(st.session_state_version() > before);
    }

    // Review 2026-10-16, finding 5: the store stage comes from the open
    // context of the restore that was refused, never from text the server
    // controls; and the rate-limit facts from the error, never from text.
    #[test]
    fn only_a_recorded_refusal_makes_a_store_stage_and_text_sets_nothing() {
        let (st, _, _) = state(0);
        // Server text that names a marker: no stage, outside or inside a context.
        let spoof = json!({"type": "login_failed",
            "message": "M_FORBIDDEN: [lightning-store:crypto_account_mismatch] 429 :8429"});
        let failed = st.admit(spoof.clone()).unwrap();
        assert!(failed.get("stage").is_none(), "{failed}");
        let runtime = tokio::runtime::Builder::new_current_thread().build().unwrap();
        let clean = Arc::new(crate::labs::OpenContext::default());
        let failed = runtime
            .block_on(crate::labs::scope_open(Some(clean), async { st.admit(spoof.clone()) }))
            .unwrap();
        assert!(failed.get("stage").is_none(), "{failed}");
        // A refusal recorded by the open: the stage and reason.
        let refused = Arc::new(crate::labs::OpenContext::default());
        let failed = runtime
            .block_on(crate::labs::scope_open(Some(refused), async {
                let message = crate::labs::refuse_store(crate::labs::REFUSAL_CRYPTO_MISSING);
                st.admit(json!({"type": "login_failed", "message": message}))
            }))
            .unwrap();
        assert_eq!(failed["stage"], "store");
        assert_eq!(failed["reason"], "crypto_account_missing");
        // sync_error: text never sets rate_limited.
        let plain = st
            .admit(json!({"type": "sync_error",
                          "message": "failed: http://hs:8429 [429 / M_LIMIT_EXCEEDED] retry_after_ms: 2500"}))
            .unwrap();
        assert_eq!(plain["rate_limited"], false);
        assert!(plain.get("retry_after_ms").is_none());
        // The source's facts pass through.
        let limited = st
            .admit(with_rate_limit(json!({"type": "sync_error", "message": "x"}), (true, Some(2500))))
            .unwrap();
        assert_eq!(limited["rate_limited"], true);
        assert_eq!(limited["retry_after_ms"], 2500);
    }

    #[test]
    fn rate_limit_facts_come_from_the_error_kind_and_status() {
        use matrix_sdk::ruma::api::error::{ErrorKind, LimitExceededErrorData, RetryAfter};
        let mut data = LimitExceededErrorData::new();
        data.retry_after = Some(RetryAfter::Delay(Duration::from_millis(2500)));
        let limited = ErrorKind::LimitExceeded(data);
        assert_eq!(rate_limit_facts(Some(&limited), Some(429)), (true, Some(2500)));
        let bare = ErrorKind::LimitExceeded(LimitExceededErrorData::new());
        assert_eq!(rate_limit_facts(Some(&bare), None), (true, None));
        assert_eq!(rate_limit_facts(None, Some(429)), (true, None), "a bare 429");
        assert_eq!(rate_limit_facts(Some(&ErrorKind::Forbidden), Some(403)), (false, None));
        assert_eq!(rate_limit_facts(None, None), (false, None));
    }

    fn counts(id: &str) -> RoomCounts {
        RoomCounts { room_id: id.to_owned(), encryption_known: true, ..Default::default() }
    }

    #[test]
    fn a_counts_tuple_uses_the_room_payload_rule_and_skips_quiet_rooms() {
        assert_eq!(count_tuple(&counts("!q:hs")), None, "a quiet room is absent");
        // Unencrypted: max of client and server, as room_payload.
        let mut plain = counts("!p:hs");
        plain.client_notifications = 1;
        plain.server_notifications = 4;
        plain.server_highlights = 1;
        plain.counted_from_receipt = true;
        plain.unread_messages = 2;
        assert_eq!(
            count_tuple(&plain),
            Some(json!(["!p:hs", 4, 1, FLAG_HAS_UNREAD | FLAG_ENCRYPTION_KNOWN]))
        );
        // Encrypted: client counts only, and a lower bound.
        let mut enc = counts("!e:hs");
        enc.encrypted = true;
        enc.client_notifications = 2;
        enc.server_notifications = 9;
        assert_eq!(
            count_tuple(&enc),
            Some(json!(["!e:hs", 2, 0,
                        FLAG_ENCRYPTED | FLAG_ENCRYPTION_KNOWN | FLAG_LOWER_BOUND]))
        );
        // Thread-only unread.
        let mut threads = counts("!t:hs");
        threads.counted_from_receipt = true;
        threads.server_notifications = 3;
        threads.server_highlights = 1;
        let tuple = count_tuple(&threads).unwrap();
        let flags = tuple[3].as_u64().unwrap();
        assert!(flags & FLAG_THREADS_ONLY != 0 && flags & FLAG_MENTION_IN_THREADS != 0);
        // Marked unread alone counts.
        let mut marked = counts("!m:hs");
        marked.marked_unread = true;
        assert_eq!(count_tuple(&marked).unwrap()[3].as_u64().unwrap() & FLAG_MARKED_UNREAD, 2);
    }

    // Review 2026-10-16, finding 12: `RoomCounts::of` skips a quiet room
    // before reading its id, encryption or receipt. That is safe only if a
    // quiet room yields no tuple whatever those are: walk every combination.
    #[test]
    fn a_quiet_room_has_no_tuple_whatever_its_encryption_and_receipt() {
        for encrypted in [false, true] {
            for encryption_known in [false, true] {
                for counted_from_receipt in [false, true] {
                    let quiet = RoomCounts {
                        room_id: "!q:hs".to_owned(),
                        encrypted,
                        encryption_known,
                        counted_from_receipt,
                        ..Default::default()
                    };
                    assert!(quiet.is_quiet());
                    assert_eq!(count_tuple(&quiet), None);
                }
            }
        }
        // And any one non-zero counter is not quiet.
        for field in 0..6 {
            let mut c = counts("!n:hs");
            match field {
                0 => c.client_notifications = 1,
                1 => c.client_mentions = 1,
                2 => c.server_notifications = 1,
                3 => c.server_highlights = 1,
                4 => c.unread_messages = 1,
                _ => c.marked_unread = true,
            }
            assert!(!c.is_quiet(), "field {field}");
        }
    }

    #[test]
    fn the_room_list_mirror_reports_what_left_and_never_panics() {
        use matrix_sdk_ui::eyeball_im::{Vector, VectorDiff};
        let v = |items: &[&str]| -> Vector<String> {
            items.iter().map(|s| (*s).to_owned()).collect()
        };
        let mut order: Vec<String> = Vec::new();
        assert!(apply_order_diff(&mut order, VectorDiff::Reset { values: v(&["a", "b", "c"]) })
            .is_empty());
        assert_eq!(apply_order_diff(&mut order, VectorDiff::Remove { index: 1 }), ["b"]);
        assert_eq!(apply_order_diff(&mut order, VectorDiff::Set { index: 0, value: "d".into() }),
                   ["a"]);
        apply_order_diff(&mut order, VectorDiff::PushBack { value: "e".into() });
        apply_order_diff(&mut order, VectorDiff::Insert { index: 1, value: "f".into() });
        assert_eq!(order, ["d", "f", "c", "e"]);
        assert_eq!(apply_order_diff(&mut order, VectorDiff::Truncate { length: 2 }), ["c", "e"]);
        assert_eq!(apply_order_diff(&mut order, VectorDiff::PopFront), ["d"]);
        assert_eq!(apply_order_diff(&mut order, VectorDiff::PopBack), ["f"]);
        // Out of range: ignored.
        assert!(apply_order_diff(&mut order, VectorDiff::Remove { index: 9 }).is_empty());
        assert!(apply_order_diff(&mut order, VectorDiff::Set { index: 9, value: "x".into() })
            .is_empty());
        assert!(apply_order_diff(&mut order, VectorDiff::Insert { index: 9, value: "x".into() })
            .is_empty());
        assert!(apply_order_diff(&mut order, VectorDiff::PopFront).is_empty());
        apply_order_diff(&mut order, VectorDiff::Append { values: v(&["g", "h"]) });
        assert_eq!(apply_order_diff(&mut order, VectorDiff::Clear), ["g", "h"]);
        apply_order_diff(&mut order, VectorDiff::Append { values: v(&["i"]) });
        assert_eq!(apply_order_diff(&mut order, VectorDiff::Reset { values: v(&["j"]) }), ["i"]);
        assert_eq!(order, ["j"]);
    }

    #[test]
    fn more_than_two_hundred_unread_rooms_are_all_in_the_table() {
        let tuples: Vec<Value> = (0..450)
            .filter_map(|i| {
                let mut c = counts(&format!("!r{i}:hs"));
                c.client_notifications = 1;
                count_tuple(&c)
            })
            .collect();
        assert_eq!(tuples.len(), 450, "the counts table has no 200 cap");
    }

    #[test]
    fn a_take_clears_dirty_and_the_first_take_is_the_baseline() {
        let (st, _, _) = state(0);
        assert!(!st.dirty());
        st.note_room("!a:hs".try_into().unwrap());
        st.note_room("!a:hs".try_into().unwrap());
        assert!(st.dirty());
        assert_eq!(st.notable_updates_total(), 2);
        let first: Value = serde_json::from_str(&st.take(0)).unwrap();
        assert!(!st.dirty());
        assert_eq!(first["baseline"], true);
        assert_eq!(first["seq"], 1);
        assert_eq!(first["rows"], json!([]));
        assert_eq!(first["counts"], json!([]), "no client: an empty table");
        st.note_rescan();
        let second: Value = serde_json::from_str(&st.take(0)).unwrap();
        assert_eq!(second["baseline"], false);
        assert_eq!(second["rescan"], true);
        let third: Value = serde_json::from_str(&st.take(0)).unwrap();
        assert_eq!(third["rescan"], false);
    }

    #[test]
    fn a_lagged_rescan_runs_at_most_once_per_interval() {
        let mut limiter = RescanLimiter::default();
        let t0 = Instant::now();
        assert!(limiter.on_lagged(t0));
        assert!(!limiter.on_lagged(t0 + Duration::from_secs(1)), "within the interval");
        assert!(!limiter.on_lagged(t0 + Duration::from_secs(2)));
        assert_eq!(limiter.due_at(), Some(t0 + RESCAN_INTERVAL), "one pending, not two");
        assert!(!limiter.on_due(t0 + Duration::from_secs(29)));
        assert!(limiter.on_due(t0 + RESCAN_INTERVAL));
        assert_eq!(limiter.due_at(), None);
        assert!(limiter.on_lagged(t0 + RESCAN_INTERVAL * 2));
    }

    #[test]
    fn the_watch_set_is_capped_and_the_least_recent_room_gives_way() {
        let ids: Vec<(OwnedRoomId, u64)> = [("!a:hs", 5), ("!b:hs", 1), ("!c:hs", 9), ("!d:hs", 1)]
            .into_iter()
            .map(|(id, s)| (id.try_into().unwrap(), s))
            .collect();
        assert_eq!(watch_decision(&ids, 5, 0), WatchDecision::Watch, "under the cap");
        assert_eq!(watch_decision(&ids, 4, 1), WatchDecision::Skip, "not more recent than the lowest");
        assert_eq!(
            watch_decision(&ids, 4, 2),
            WatchDecision::Replace("!b:hs".try_into().unwrap()),
            "the lowest stamp goes first, ties by id"
        );
        assert_eq!(watch_decision(&[], 0, 99), WatchDecision::Skip, "a zero cap watches nothing");
    }

    #[test]
    fn the_registry_finds_only_live_background_queues() {
        let _labs = crate::labs::LabsTestGuard::off();
        let (st, events, commands) = state(0);
        let stranger: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        register(&events, &commands, &st);
        crate::labs::disable_for_test();
        assert!(for_queue(&events).is_none(), "labs off: no lookup at all");
        crate::labs::enable();
        assert!(for_queue(&events).is_some());
        assert!(for_queue(&commands).is_some());
        assert!(for_queue(&stranger).is_none());
        assert!(admit(&stranger, json!({"type": "room_list_reset"})).is_some(),
                "a foreground queue passes everything through");
        assert!(admit(&events, json!({"type": "room_list_reset"})).is_none());
        unregister(&events, &commands);
        assert!(for_queue(&events).is_none());
    }
}

/// §18 round 1, L13: what one `take()` costs on a large account, measured on
/// a real SDK Client synced from a loopback homeserver with `ROOMS` joined
/// rooms (one in ten unread, so the counts table is not empty). Ignored by
/// default: a measurement, not a gate. Run with
/// `cargo test take_cost -- --ignored --nocapture`; it prints the numbers.
#[cfg(test)]
mod take_cost_measure {
    use super::*;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::thread;

    use matrix_sdk::authentication::matrix::MatrixSession;
    use matrix_sdk::config::SyncSettings;
    use matrix_sdk::store::RoomLoadSettings;
    use matrix_sdk::{SessionMeta, SessionTokens};

    const ME: &str = "@measure:example.org";

    fn sync_body(rooms: usize) -> String {
        let mut join = serde_json::Map::new();
        for i in 0..rooms {
            let id = format!("!room{i}:example.org");
            let unread = i % 10 == 0;
            join.insert(
                id,
                json!({
                    "state": { "events": [
                        { "type": "m.room.create", "state_key": "", "sender": ME,
                          "event_id": format!("$c{i}"), "origin_server_ts": 1_000,
                          "content": { "creator": ME, "room_version": "10" } },
                        { "type": "m.room.member", "state_key": ME, "sender": ME,
                          "event_id": format!("$m{i}"), "origin_server_ts": 1_001,
                          "content": { "membership": "join" } },
                        { "type": "m.room.name", "state_key": "", "sender": ME,
                          "event_id": format!("$n{i}"), "origin_server_ts": 1_002,
                          "content": { "name": format!("Room {i}") } },
                    ]},
                    "timeline": { "events": [], "limited": false },
                    "unread_notifications": {
                        "notification_count": if unread { 3 } else { 0 },
                        "highlight_count": if unread { 1 } else { 0 },
                    },
                }),
            );
        }
        json!({ "next_batch": "s1", "rooms": { "join": join } }).to_string()
    }

    fn read_head(stream: &mut TcpStream) -> String {
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            let n = stream.read(&mut chunk).unwrap_or(0);
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
            if buf.windows(4).any(|w| w == b"\r\n\r\n") {
                break;
            }
        }
        String::from_utf8_lossy(&buf).lines().next().unwrap_or("").to_owned()
    }

    fn serve(rooms: usize) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let body = Arc::new(sync_body(rooms));
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let body = Arc::clone(&body);
                thread::spawn(move || {
                    let line = read_head(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    let path = path.split('?').next().unwrap_or("").to_owned();
                    let payload: String = if path == "/_matrix/client/versions" {
                        r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#.to_owned()
                    } else if path.ends_with("/sync") {
                        (*body).clone()
                    } else if path.ends_with("/keys/upload") {
                        r#"{"one_time_key_counts":{"signed_curve25519":50}}"#.to_owned()
                    } else if path.ends_with("/keys/query") {
                        r#"{"device_keys":{},"failures":{}}"#.to_owned()
                    } else {
                        "{}".to_owned()
                    };
                    let response = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\
                         Connection: close\r\nContent-Length: {}\r\n\r\n{}",
                        payload.len(),
                        payload
                    );
                    let _ = stream.write_all(response.as_bytes());
                });
            }
        });
        format!("{}:{}", addr.ip(), addr.port())
    }

    fn profile() -> Profile {
        crate::labs::parse_profile(
            &json!({
                "v": 1, "slot": "m0",
                "require_account": {"user_id": ME, "device_id": "DEV"},
                "strict_trust": false, "receipt_privacy": 2, "index_all_hold": 0,
                "floor_ms": 0, "local_rule_rooms": [],
                "max_blocking_threads": 8, "sqlite": "low_memory",
                "retry_limit": 3, "max_retry_time_ms": 30000, "watch_cap": 3
            })
            .to_string(),
        )
        .unwrap()
    }

    /// One account of `rooms` rooms: (median µs per take, counts rows).
    fn measure(runtime: &tokio::runtime::Runtime, rooms: usize) -> (u128, usize) {
        let addr = serve(rooms);
        let client = runtime.block_on(async {
            let client = Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: SessionMeta {
                            user_id: matrix_sdk::ruma::OwnedUserId::try_from(ME).unwrap(),
                            device_id: "DEV".into(),
                        },
                        tokens: SessionTokens {
                            access_token: "t".to_owned(),
                            refresh_token: None,
                        },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");
            client.sync_once(SyncSettings::default()).await.expect("sync");
            client
        });
        assert_eq!(client.rooms().len(), rooms);
        let slot = Arc::new(Mutex::new(Some(client)));
        let events: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let commands: EventQueueRef = Arc::new(Mutex::new(VecDeque::new()));
        let st = BgState::new(profile(), Arc::clone(&slot), &events, &commands);
        let first: Value = serde_json::from_str(&st.take(0)).unwrap();
        let rows = first["counts"].as_array().map(|a| a.len()).unwrap_or(0);
        let mut samples: Vec<u128> = (0..41)
            .map(|_| {
                let started = Instant::now();
                std::hint::black_box(st.take(0));
                started.elapsed().as_micros()
            })
            .collect();
        samples.sort_unstable();
        let _guard = runtime.enter();
        drop(slot.lock().unwrap().take());
        (samples[samples.len() / 2], rows)
    }

    #[test]
    #[ignore]
    fn take_cost_on_large_accounts() {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let mut total = 0u128;
        for rooms in [500usize, 1000, 2000, 4000] {
            let (median_us, rows) = measure(&runtime, rooms);
            total += median_us;
            println!("take_cost rooms={rooms} counts_rows={rows} median_us={median_us}");
        }
        println!("take_cost four_accounts_sum_us={total}");
    }
}
