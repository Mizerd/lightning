//! Warm rooms: the rooms the sliding sync keeps subscribed.
//!
//! Until now the sync carried ONE room subscription, the room that is open.
//! Switching back to the room you just left therefore started from a
//! one-event cache and a fill round trip, and after the sliding-sync session
//! expired (`M_UNKNOWN_POS`) the SDK cleared every subscription
//! (`SlidingSync::expire_session`), so even the open room silently stopped
//! getting its richer timeline until the user switched away and back.
//!
//! This is the policy half, kept pure so it can be tested without a server:
//! a bounded most-recently-used set of room ids, and the rule for when the
//! whole set must be sent again.
//!
//! Bound: [`WARM_ROOM_LIMIT`]. Each subscribed room receives a 20-event
//! timeline and extra required state, and those events are written to the
//! event cache, so the set is deliberately modest (Sable keeps 32).

use matrix_sdk::ruma::OwnedRoomId;

/// How many recently opened rooms stay subscribed. Modest on purpose: every
/// subscribed room's events are persisted by the event cache.
pub const WARM_ROOM_LIMIT: usize = 16;

/// The open room plus the most recently opened others, newest first.
#[derive(Debug, Default)]
pub struct SubscriptionSet {
    /// Newest first. Never longer than `WARM_ROOM_LIMIT`.
    recent: Vec<OwnedRoomId>,
    /// The room that is open right now, if any. Always also in `recent`.
    active: Option<OwnedRoomId>,
    /// The membership last handed to the SDK. `subscribe_to_rooms` replaces
    /// the whole map and flags every room in it for a member refetch, and the
    /// public API has no incremental add, so an unchanged membership (a
    /// reopen of a warm room, a close) is not sent at all.
    sent: std::collections::BTreeSet<OwnedRoomId>,
}

impl SubscriptionSet {
    /// A room was opened: it becomes the active room and the newest warm one.
    /// Returns the room that fell off the end, if the bound was exceeded.
    pub fn open(&mut self, room: OwnedRoomId) -> Option<OwnedRoomId> {
        self.recent.retain(|r| r != &room);
        self.recent.insert(0, room.clone());
        self.active = Some(room);
        if self.recent.len() > WARM_ROOM_LIMIT {
            self.recent.pop()
        } else {
            None
        }
    }

    /// A room with a call: subscribed like a recently opened one, without
    /// opening it. The room list's timeline limit is 1, so several `m.call.*`
    /// events in one response would otherwise reach the client as the last one
    /// alone; a subscription delivers them, and its first response replays the
    /// recent ones. Never evicts the open room.
    pub fn warm(&mut self, room: OwnedRoomId) {
        self.recent.retain(|r| r != &room);
        self.recent.insert(0, room);
        if self.recent.len() > WARM_ROOM_LIMIT {
            let active = self.active.clone();
            if let Some(last) =
                self.recent.iter().rposition(|r| Some(r) != active.as_ref())
            {
                self.recent.remove(last);
            }
        }
    }

    /// The open room was closed. It stays warm: that is the point.
    pub fn close_active(&mut self) {
        self.active = None;
    }

    /// Forget everything (sign-out, account switch, sync teardown), so one
    /// account's rooms can never be subscribed by another account's sync.
    pub fn clear(&mut self) {
        self.recent.clear();
        self.active = None;
        self.sent.clear();
    }

    /// What to hand to `subscribe_to_rooms` now, or `None` when the SDK
    /// already holds exactly this membership. `force` is for a fresh or
    /// expired session, whose subscription map is empty whatever we sent
    /// before.
    pub fn pending(&mut self, force: bool) -> Option<Vec<OwnedRoomId>> {
        let now: std::collections::BTreeSet<OwnedRoomId> =
            self.recent.iter().cloned().collect();
        if !force && now == self.sent {
            return None;
        }
        self.sent = now;
        Some(self.desired())
    }

    pub fn active(&self) -> Option<&OwnedRoomId> {
        self.active.as_ref()
    }

    /// Every room to subscribe, newest (the open room) first.
    pub fn desired(&self) -> Vec<OwnedRoomId> {
        self.recent.clone()
    }

    pub fn len(&self) -> usize {
        self.recent.len()
    }
}

/// Whether the room-list sync state means subscriptions may have been lost
/// and the whole set must be sent again.
///
/// `SlidingSync::expire_session` clears the SDK's subscription map, and a
/// rebuilt `RoomListService` starts with none. Both end with the list coming
/// back to `Running` after a state that was not `Running`; a `Running` that
/// follows `Running` (it repeats on every response) is not a reason.
pub fn must_resubscribe(was_running: bool, now_running: bool) -> bool {
    now_running && !was_running
}

#[cfg(test)]
mod tests {
    use super::*;

    fn room(n: usize) -> OwnedRoomId {
        OwnedRoomId::try_from(format!("!room{n}:example.org")).unwrap()
    }

    #[test]
    fn the_newest_room_is_first_and_a_reopen_moves_it_up_without_duplicating() {
        let mut set = SubscriptionSet::default();
        set.open(room(1));
        set.open(room(2));
        set.open(room(3));
        set.open(room(1));
        assert_eq!(set.desired(), vec![room(1), room(3), room(2)]);
        assert_eq!(set.active(), Some(&room(1)));
    }

    #[test]
    fn a_call_room_is_subscribed_without_being_opened_and_keeps_the_open_room() {
        let mut set = SubscriptionSet::default();
        set.open(room(0));
        for n in 1..=WARM_ROOM_LIMIT {
            set.warm(room(n));
        }
        assert_eq!(set.len(), WARM_ROOM_LIMIT);
        assert_eq!(set.active(), Some(&room(0)));
        assert!(set.desired().contains(&room(0)), "the open room stays");
        assert_eq!(set.desired()[0], room(WARM_ROOM_LIMIT));
        assert!(set.pending(false).is_some());
        assert!(set.pending(false).is_none(), "unchanged is not resent");
    }

    #[test]
    fn closing_the_open_room_keeps_it_warm() {
        let mut set = SubscriptionSet::default();
        set.open(room(1));
        set.close_active();
        assert_eq!(set.active(), None);
        assert_eq!(set.desired(), vec![room(1)]);
    }

    #[test]
    fn the_set_is_bounded_and_evicts_the_least_recently_opened() {
        let mut set = SubscriptionSet::default();
        let mut evicted = Vec::new();
        for n in 0..(WARM_ROOM_LIMIT + 4) {
            if let Some(gone) = set.open(room(n)) {
                evicted.push(gone);
            }
        }
        assert_eq!(set.len(), WARM_ROOM_LIMIT);
        assert_eq!(evicted, vec![room(0), room(1), room(2), room(3)]);
        assert_eq!(set.desired()[0], room(WARM_ROOM_LIMIT + 3));
        assert!(!set.desired().contains(&room(0)));
    }

    #[test]
    fn reopening_a_warm_room_never_evicts_anything() {
        let mut set = SubscriptionSet::default();
        for n in 0..WARM_ROOM_LIMIT {
            set.open(room(n));
        }
        assert_eq!(set.open(room(0)), None);
        assert_eq!(set.len(), WARM_ROOM_LIMIT);
    }

    #[test]
    fn clear_drops_every_room_and_the_active_marker() {
        let mut set = SubscriptionSet::default();
        set.open(room(1));
        set.open(room(2));
        set.clear();
        assert_eq!(set.active(), None);
        assert!(set.desired().is_empty());
    }

    #[test]
    fn an_unchanged_membership_is_not_sent_again_until_forced() {
        let mut set = SubscriptionSet::default();
        set.open(room(1));
        set.open(room(2));
        assert_eq!(set.pending(false), Some(vec![room(2), room(1)]));
        // Switching back to a warm room only reorders the set.
        set.open(room(1));
        assert_eq!(set.pending(false), None);
        set.close_active();
        assert_eq!(set.pending(false), None);
        // A new room changes the membership.
        set.open(room(3));
        assert!(set.pending(false).is_some());
        // An expired or rebuilt session needs everything again.
        assert!(set.pending(true).is_some());
    }

    #[test]
    fn only_a_return_to_running_resends_the_set() {
        // Running after an offline/error/recovering gap (a session expiry or a
        // rebuilt service) resends; a repeated Running does not, because that
        // arm fires on every response.
        assert!(must_resubscribe(false, true));
        assert!(!must_resubscribe(true, true));
        assert!(!must_resubscribe(false, false));
        assert!(!must_resubscribe(true, false));
    }
}
