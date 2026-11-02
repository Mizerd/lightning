
use std::collections::{BTreeMap, HashSet};
use std::future::Future;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        api::client::{membership::joined_rooms, sync::sync_events::v5},
        OwnedRoomId, RoomId, UInt,
    },
    Client, Room, RoomState,
};
use matrix_sdk_base::RoomInfoNotableUpdateReasons;

use crate::EventQueueRef;

const FIRST_PASS_DELAY: Duration = Duration::from_secs(20);
const PASS_PERIOD: Duration = Duration::from_secs(300);
/// Invitations one answer may list. More than this and the answer is
/// incomplete, so nothing is retired.
const INVITE_PAGE: u32 = 200;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
const CONN_ID: &str = "lightning-invite-check";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ServerMembership {
    Pending,
    Joined,
    Gone,
}

#[derive(Debug, Default, Clone)]
pub(crate) struct ServerInvites {
    pub(crate) invited: HashSet<OwnedRoomId>,
    pub(crate) joined: HashSet<OwnedRoomId>,
}

impl ServerInvites {
    pub(crate) fn verdict(&self, room: &RoomId) -> ServerMembership {
        if self.invited.contains(room) {
            ServerMembership::Pending
        } else if self.joined.contains(room) {
            ServerMembership::Joined
        } else {
            ServerMembership::Gone
        }
    }
}

pub(crate) fn invite_list_complete(count: u64, returned: usize) -> bool {
    count <= returned as u64
}

fn is_pending(state: RoomState) -> bool {
    matches!(state, RoomState::Invited | RoomState::Knocked)
}

pub(crate) async fn fetch_server_invites(client: &Client) -> Option<ServerInvites> {
    let config = RequestConfig::new().disable_retry().timeout(REQUEST_TIMEOUT);

    let mut filters = v5::request::ListFilters::default();
    filters.is_invite = Some(true);
    let mut list = v5::request::List::default();
    list.ranges = vec![(UInt::MIN, UInt::from(INVITE_PAGE - 1))];
    list.room_details.timeline_limit = UInt::MIN;
    list.filters = Some(filters);
    let mut request = v5::Request::new();
    request.conn_id = Some(CONN_ID.to_owned());
    request.timeout = Some(Duration::ZERO);
    request.lists = BTreeMap::from([("invites".to_owned(), list)]);
    let response = client.send(request).with_request_config(config).await.ok()?;
    let count = response.lists.get("invites").map(|l| u64::from(l.count))?;
    let invited: HashSet<OwnedRoomId> = response.rooms.into_keys().collect();
    if !invite_list_complete(count, invited.len()) {
        return None;
    }

    let joined = client
        .send(joined_rooms::v3::Request::new())
        .with_request_config(config)
        .await
        .ok()?
        .joined_rooms
        .into_iter()
        .collect();
    Some(ServerInvites { invited, joined })
}

/// Marks a still-pending room left in the local store. Returns whether it
/// changed anything: a sync that moved the room meanwhile wins.
pub(crate) async fn retire_locally(room: &Room) -> bool {
    if !is_pending(room.state()) {
        return false;
    }
    let mut retired = false;
    let saved = room
        .update_and_save_room_info(|mut info| {
            if is_pending(info.state()) {
                info.mark_as_left();
                retired = true;
                (info, RoomInfoNotableUpdateReasons::MEMBERSHIP)
            } else {
                (info, RoomInfoNotableUpdateReasons::NONE)
            }
        })
        .await;
    saved.is_ok() && retired
}

pub(crate) async fn leave_or_retire<F, Fut>(room: &Room, ask: F) -> Result<(), String>
where
    F: FnOnce() -> Fut,
    Fut: Future<Output = Option<ServerInvites>>,
{
    let was_pending = is_pending(room.state());
    match room.leave().await {
        Ok(()) => Ok(()),
        Err(error) => {
            let message = error.to_string();
            if was_pending {
                if let Some(answer) = ask().await {
                    if answer.verdict(room.room_id()) == ServerMembership::Gone
                        && retire_locally(room).await
                    {
                        return Ok(());
                    }
                }
            }
            Err(message)
        }
    }
}

pub(crate) async fn reconcile_with(client: &Client, answer: &ServerInvites) -> usize {
    let mut retired = 0;
    for room in client.rooms() {
        if is_pending(room.state())
            && answer.verdict(room.room_id()) == ServerMembership::Gone
            && retire_locally(&room).await
        {
            retired += 1;
        }
    }
    retired
}

fn any_pending(client: &Client) -> bool {
    client.rooms().iter().any(|room| is_pending(room.state()))
}

pub(crate) async fn run_stale_invite_reconciler(client: Client, events: EventQueueRef) {
    tokio::time::sleep(FIRST_PASS_DELAY).await;
    loop {
        if any_pending(&client) {
            if let Some(answer) = fetch_server_invites(&client).await {
                let retired = reconcile_with(&client, &answer).await;
                let background = crate::bgsession::for_queue(&events);
                if retired > 0 && background.is_none() {
                    crate::enqueue(
                        &events,
                        serde_json::json!({ "type": "stale_invites_retired", "count": retired }),
                    );
                    crate::enqueue_rooms(&events, &client).await;
                } else if let Some(bg) = background.filter(|_| retired > 0) {
                    bg.note_skipped();
                }
            }
        }
        tokio::time::sleep(PASS_PERIOD).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
    use matrix_sdk::test_utils::mocks::MatrixMockServer;
    use matrix_sdk_base::RequestedRequiredStates;
    use serde_json::json;

    async fn invited(client: &Client, rooms: &[&RoomId]) {
        let me = client.user_id().expect("logged in").to_string();
        let mut response = v5::Response::new("pos-1".to_owned());
        for room in rooms {
            let mut entry = v5::response::Room::default();
            entry.invite_state = Some(vec![
                Raw::from_json_string(
                    json!({
                        "type": "m.room.member", "state_key": me,
                        "sender": "@inviter:example.org",
                        "content": { "membership": "invite" }
                    })
                    .to_string(),
                )
                .unwrap(),
            ]);
            response.rooms.insert((*room).to_owned(), entry);
        }
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        for room in rooms {
            assert_eq!(client.get_room(room).unwrap().state(), RoomState::Invited);
        }
    }

    fn answer(invited: &[&RoomId], joined: &[&RoomId]) -> ServerInvites {
        ServerInvites {
            invited: invited.iter().map(|r| (*r).to_owned()).collect(),
            joined: joined.iter().map(|r| (*r).to_owned()).collect(),
        }
    }

    #[test]
    fn only_absence_from_both_sets_means_gone() {
        let a = room_id!("!a:example.org");
        let b = room_id!("!b:example.org");
        let c = room_id!("!c:example.org");
        let server = answer(&[a], &[b]);
        assert_eq!(server.verdict(a), ServerMembership::Pending);
        assert_eq!(server.verdict(b), ServerMembership::Joined);
        assert_eq!(server.verdict(c), ServerMembership::Gone);
        assert!(invite_list_complete(3, 3));
        assert!(invite_list_complete(0, 0));
        assert!(!invite_list_complete(201, 200));
    }

    #[tokio::test]
    async fn an_invite_rejected_elsewhere_is_retired_only_on_the_servers_word() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let gone = room_id!("!rejected-elsewhere:example.org");
        let pending = room_id!("!still-invited:example.org");
        let joined_elsewhere = room_id!("!accepted-elsewhere:example.org");
        invited(&client, &[gone, pending, joined_elsewhere]).await;

        let retired = reconcile_with(&client, &answer(&[pending], &[joined_elsewhere])).await;

        assert_eq!(retired, 1);
        assert_eq!(client.get_room(gone).unwrap().state(), RoomState::Left);
        assert_eq!(client.get_room(pending).unwrap().state(), RoomState::Invited);
        assert_eq!(client.get_room(joined_elsewhere).unwrap().state(), RoomState::Invited);

        let again = reconcile_with(&client, &answer(&[], &[])).await;
        assert_eq!(again, 2, "only the two still-pending rooms remain");
    }

    // With no answer (request failed, list incomplete) nothing is touched,
    // and the fetch itself fails closed against a server that has no
    // sliding sync endpoint (the mock answers 404).
    #[tokio::test]
    async fn no_answer_retires_nothing() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let room = room_id!("!invited:example.org");
        invited(&client, &[room]).await;
        assert!(fetch_server_invites(&client).await.is_none());
        let r = client.get_room(room).unwrap();
        let result = leave_or_retire(&r, || async { None }).await;
        assert!(result.is_err(), "an unanswered question is not a leave");
        assert_eq!(r.state(), RoomState::Invited);
    }

    #[tokio::test]
    async fn a_refused_reject_falls_back_only_on_a_complete_answer() {
        let server = MatrixMockServer::new().await;
        let client = server.client_builder().build().await;
        let gone = room_id!("!gone:example.org");
        let pending = room_id!("!pending:example.org");
        invited(&client, &[gone, pending]).await;
        // No /leave mock: the mock server answers 404.

        let r = client.get_room(gone).unwrap();
        let result = leave_or_retire(&r, || async { Some(answer(&[pending], &[])) }).await;
        assert_eq!(result, Ok(()));
        assert_eq!(r.state(), RoomState::Left);

        let r = client.get_room(pending).unwrap();
        let result = leave_or_retire(&r, || async { Some(answer(&[pending], &[])) }).await;
        assert!(result.is_err(), "the server still lists it: the refusal stands");
        assert_eq!(r.state(), RoomState::Invited);
    }
}
