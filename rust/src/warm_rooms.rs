
use matrix_sdk::ruma::OwnedRoomId;

pub const WARM_ROOM_LIMIT: usize = 16;

#[derive(Debug, Default)]
pub struct SubscriptionSet {
    recent: Vec<OwnedRoomId>,
    active: Option<OwnedRoomId>,
    /// The membership last handed to the SDK. `subscribe_to_rooms` replaces
    /// the whole map and flags every room in it for a member refetch, and the
    /// public API has no incremental add, so an unchanged membership (a
    /// reopen of a warm room, a close) is not sent at all.
    sent: std::collections::BTreeSet<OwnedRoomId>,
}

impl SubscriptionSet {
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

    pub fn close_active(&mut self) {
        self.active = None;
    }

    pub fn clear(&mut self) {
        self.recent.clear();
        self.active = None;
        self.sent.clear();
    }

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

    pub fn desired(&self) -> Vec<OwnedRoomId> {
        self.recent.clone()
    }

    pub fn len(&self) -> usize {
        self.recent.len()
    }
}

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
        set.open(room(1));
        assert_eq!(set.pending(false), None);
        set.close_active();
        assert_eq!(set.pending(false), None);
        set.open(room(3));
        assert!(set.pending(false).is_some());
        assert!(set.pending(true).is_some());
    }

    #[test]
    fn only_a_return_to_running_resends_the_set() {
        assert!(must_resubscribe(false, true));
        assert!(!must_resubscribe(true, true));
        assert!(!must_resubscribe(false, false));
        assert!(!must_resubscribe(true, false));
    }
}
