//! What the sync event handlers share with the sync loop and the crypto
//! observer so a notification decision can be made about an event that did
//! not reach C++ through an open timeline.
//!
//! Two halves:
//!
//!  * [`CatchupClock`] marks `backlog` on sync-handler events that are history
//!    rather than arrivals. The open-timeline `BacklogMarker`
//!    (push_verdict.rs) only covers the open room; every other room's events
//!    reach C++ through the sync handlers, and before this nothing there said
//!    "old". Two ways that went wrong: on a first load past one growing batch
//!    (100 rooms) every further room's latest event arrived AFTER
//!    `initial_sync_done`, months old and notifying; and after a suspend or a
//!    network loss the catch-up response notified everything that had piled
//!    up while the client was away.
//!  * [`PendingUtds`] remembers the encrypted events the sync handler forwarded
//!    as undecryptable, so the event-cache redecryptor's report can re-emit
//!    each one ONCE, decrypted, for a verdict computed on the decrypted
//!    content.
//!
//! Neither half ever sees a body.

use std::{
    collections::{HashMap, VecDeque},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Mutex,
    },
};

use matrix_sdk::ruma::{EventId, OwnedEventId, OwnedRoomId, RoomId};

/// How far an event may predate the floor and still count as live when its
/// arrival is measured by the server (`unsigned.age`): the time between the
/// server answering and this handler running.
pub(crate) const AGE_ALLOWANCE_MS: u64 = 10_000;

/// The same allowance when only `origin_server_ts` is known. Two clocks are
/// compared then, so it is wider: a server clock up to this far behind ours
/// cannot make a live event look old.
pub(crate) const TIMESTAMP_ALLOWANCE_MS: u64 = 120_000;

/// Whether a sync-handler event is history. One per bridge, driven by the sync
/// loop:
///
///  * `begin` when a sync session starts: everything from before it is old.
///  * `connection_lost` on any edge out of Running (offline, error, a
///    recovering room list): the next response is a catch-up, and an event in
///    it is new only if it reached the server within the allowance.
///  * `running_edge` on the edge back into Running: from now on an event is
///    new if it reached the server after this moment. Room-list growth after
///    it (rooms 100+, a newly subscribed warm room's 20-event history) only
///    ever re-delivers events from before it.
///
/// Monotonic floor, so nothing an earlier edge called history becomes news.
pub(crate) struct CatchupClock {
    floor_ms: AtomicU64,
    catching_up: AtomicBool,
}

impl Default for CatchupClock {
    fn default() -> Self {
        // Catching up until a sync loop says otherwise: no handler fires before
        // a sync starts, and the safe answer for one that did is "old".
        Self { floor_ms: AtomicU64::new(0), catching_up: AtomicBool::new(true) }
    }
}

impl CatchupClock {
    pub(crate) fn begin(&self, now_ms: u64) {
        self.floor_ms.store(now_ms, Ordering::SeqCst);
        self.catching_up.store(true, Ordering::SeqCst);
    }

    pub(crate) fn connection_lost(&self) {
        self.catching_up.store(true, Ordering::SeqCst);
    }

    pub(crate) fn running_edge(&self, now_ms: u64) {
        self.floor_ms.fetch_max(now_ms, Ordering::SeqCst);
        self.catching_up.store(false, Ordering::SeqCst);
    }

    /// `timestamp_ms`: `origin_server_ts`. `age_ms`: `unsigned.age` as the
    /// server sent it, which needs no clock agreement and so decides when
    /// present.
    pub(crate) fn is_backlog(&self, timestamp_ms: u64, age_ms: Option<u64>, now_ms: u64) -> bool {
        let floor = self.floor_ms.load(Ordering::SeqCst);
        // While catching up, "now" is the reconnect edge: the edge itself is
        // only reported once the catch-up response has been handled.
        let limit = if self.catching_up.load(Ordering::SeqCst) {
            floor.max(now_ms)
        } else {
            floor
        };
        match age_ms {
            Some(age) => now_ms.saturating_sub(age).saturating_add(AGE_ALLOWANCE_MS) < limit,
            None => timestamp_ms.saturating_add(TIMESTAMP_ALLOWANCE_MS) < limit,
        }
    }
}

/// Bound on remembered undecryptable events. A key that arrives after this many
/// newer UTDs is too late for a notification to mean anything.
pub(crate) const MAX_PENDING_UTDS: usize = 2_048;

/// Encrypted events the sync handler forwarded as undecryptable, with the
/// `backlog` mark they carried, until the redecryptor resolves them.
#[derive(Default)]
pub(crate) struct PendingUtds {
    entries: HashMap<(OwnedRoomId, OwnedEventId), bool>,
    order: VecDeque<(OwnedRoomId, OwnedEventId)>,
}

impl PendingUtds {
    pub(crate) fn remember(&mut self, room_id: &RoomId, event_id: &EventId, backlog: bool) {
        let key = (room_id.to_owned(), event_id.to_owned());
        if self.entries.insert(key.clone(), backlog).is_some() {
            return; // re-delivered; keep its place
        }
        self.order.push_back(key);
        while self.order.len() > MAX_PENDING_UTDS {
            if let Some(oldest) = self.order.pop_front() {
                self.entries.remove(&oldest);
            }
        }
    }

    /// `Some(backlog)` the first time a remembered event is taken, `None`
    /// for one never forwarded as undecryptable or already taken.
    pub(crate) fn take(&mut self, room_id: &RoomId, event_id: &EventId) -> Option<bool> {
        let key = (room_id.to_owned(), event_id.to_owned());
        let backlog = self.entries.remove(&key)?;
        self.order.retain(|entry| entry != &key);
        Some(backlog)
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }
}

/// The per-bridge state. Reset at every session end so a later account starts
/// with no catch-up floor and nothing pending from the previous one.
#[derive(Default)]
pub(crate) struct SyncNotify {
    pub(crate) catchup: CatchupClock,
    pending_utds: Mutex<PendingUtds>,
}

impl SyncNotify {
    pub(crate) fn remember_utd(&self, room_id: &RoomId, event_id: &EventId, backlog: bool) {
        if let Ok(mut pending) = self.pending_utds.lock() {
            pending.remember(room_id, event_id, backlog);
        }
    }

    pub(crate) fn take_utd(&self, room_id: &RoomId, event_id: &EventId) -> Option<bool> {
        self.pending_utds.lock().ok()?.take(room_id, event_id)
    }

    pub(crate) fn reset(&self) {
        if let Ok(mut pending) = self.pending_utds.lock() {
            *pending = PendingUtds::default();
        }
        self.catchup.begin(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::{event_id, room_id};

    const MIN: u64 = 60_000;
    const HOUR: u64 = 60 * MIN;

    // The finding-44 shape: a fresh login, the first room-list batch (rooms
    // 0..99) answered, Running reported, and THEN the growing list delivers
    // room 150's latest event, a month old. Before the clock, nothing marked it
    // and it notified as new.
    #[test]
    fn an_old_event_after_the_first_running_edge_is_backlog() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        let month = 30 * 24 * HOUR;
        // With unsigned.age (Synapse sends it) and without.
        assert!(clock.is_backlog(start - month, Some(month), start + 20_000));
        assert!(clock.is_backlog(start - month, None, start + 20_000));
    }

    #[test]
    fn a_live_event_after_the_edge_is_not_backlog() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        let now = start + 10 * MIN;
        assert!(!clock.is_backlog(now - 300, Some(300), now));
        assert!(!clock.is_backlog(now - 300, None, now));
    }

    // Finding 51/122: a laptop wakes from a night's suspend; the catch-up
    // response carries hours-old messages and must not notify them. One that
    // reached the server seconds ago is news.
    #[test]
    fn a_catch_up_response_after_a_lost_connection_is_backlog_except_the_newest_seconds() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start + 5_000);
        clock.connection_lost();
        let wake = start + 8 * HOUR;
        // Sent at 03:00, while suspended.
        assert!(clock.is_backlog(wake - 5 * HOUR, Some(5 * HOUR), wake));
        // Sent two seconds before the catch-up answered.
        assert!(!clock.is_backlog(wake - 2_000, Some(2_000), wake));
        // Back to Running: only what reaches the server from now on is news,
        // even a room delivered for the first time later.
        clock.running_edge(wake + 1_000);
        assert!(clock.is_backlog(wake - HOUR, Some(HOUR + 30_000), wake + 30_000));
        assert!(!clock.is_backlog(wake + 20_000, Some(1_000), wake + 21_000));
    }

    // The age is measured by the server, so a skewed local clock cannot decide
    // the answer when it is present.
    #[test]
    fn the_age_decides_over_a_skewed_timestamp() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start);
        let now = start + HOUR;
        // A timestamp an hour in the past (server clock behind), but the server
        // says it is half a second old.
        assert!(!clock.is_backlog(now - HOUR, Some(500), now));
    }

    // Without an age, a server clock a minute behind ours must not make every
    // live event history for the first minutes after an edge.
    #[test]
    fn a_timestamp_within_the_skew_allowance_of_the_edge_is_live() {
        let clock = CatchupClock::default();
        let start = 1_000 * HOUR;
        clock.begin(start);
        clock.running_edge(start);
        assert!(!clock.is_backlog(start - MIN, None, start + 1_000));
    }

    #[test]
    fn the_floor_never_moves_back() {
        let clock = CatchupClock::default();
        clock.begin(10 * HOUR);
        clock.running_edge(10 * HOUR);
        clock.running_edge(9 * HOUR);
        assert!(clock.is_backlog(9 * HOUR + MIN, Some(HOUR - MIN), 10 * HOUR + 1));
    }

    #[test]
    fn a_pending_utd_is_taken_once_with_its_backlog_mark() {
        let notify = SyncNotify::default();
        let room = room_id!("!r:example.org");
        notify.remember_utd(room, event_id!("$a:example.org"), false);
        notify.remember_utd(room, event_id!("$b:example.org"), true);
        assert_eq!(notify.take_utd(room, event_id!("$a:example.org")), Some(false));
        assert_eq!(notify.take_utd(room, event_id!("$a:example.org")), None, "once");
        assert_eq!(notify.take_utd(room, event_id!("$b:example.org")), Some(true));
        // Never forwarded as undecryptable: nothing to re-emit.
        assert_eq!(notify.take_utd(room, event_id!("$c:example.org")), None);
        // Same event id in another room is another event.
        notify.remember_utd(room, event_id!("$d:example.org"), false);
        assert_eq!(
            notify.take_utd(room_id!("!other:example.org"), event_id!("$d:example.org")),
            None
        );
    }

    #[test]
    fn pending_utds_are_bounded_oldest_first_and_reset_clears_them() {
        let mut pending = PendingUtds::default();
        let room = room_id!("!r:example.org");
        for i in 0..(MAX_PENDING_UTDS + 10) {
            let id = EventId::parse(format!("$e{i}:example.org")).unwrap();
            pending.remember(room, &id, false);
        }
        assert_eq!(pending.len(), MAX_PENDING_UTDS);
        assert_eq!(pending.take(room, event_id!("$e0:example.org")), None);
        let newest = EventId::parse(format!("$e{}:example.org", MAX_PENDING_UTDS + 9)).unwrap();
        assert_eq!(pending.take(room, &newest), Some(false));

        let notify = SyncNotify::default();
        notify.remember_utd(room, event_id!("$x:example.org"), false);
        notify.reset();
        assert_eq!(notify.take_utd(room, event_id!("$x:example.org")), None);
    }
}
