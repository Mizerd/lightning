use super::*;
use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
use matrix_sdk::test_utils::mocks::{MatrixMockServer, RoomMessagesResponseTemplate};
use matrix_sdk_base::RequestedRequiredStates;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};

const ROOM: &str = "!dm:localhost";

fn message(id: &str, body: &str, ts: u64) -> serde_json::Value {
    from("@friend:localhost", id, body, ts)
}

fn from(sender: &str, id: &str, body: &str, ts: u64) -> serde_json::Value {
    serde_json::json!({"type": "m.room.message", "event_id": id, "room_id": ROOM,
        "sender": sender, "origin_server_ts": ts,
        "content": {"msgtype": "m.text", "body": body}})
}

async fn sync(client: &matrix_sdk::Client, events: Vec<serde_json::Value>, pos: &str, gap: Option<&str>) {
    let mut response = v5::Response::new(pos.to_owned());
    let mut room = v5::response::Room::default();
    room.timeline = events
        .into_iter()
        .map(|v| Raw::from_json_string(v.to_string()).unwrap())
        .collect();
    room.limited = gap.is_some();
    room.prev_batch = gap.map(str::to_owned);
    response.rooms.insert(room_id!("!dm:localhost").to_owned(), room);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
}

fn indexed(index: &Arc<Mutex<Option<SearchIndex>>>, id: &str) -> bool {
    index.lock().unwrap().as_ref().unwrap().contains(id).unwrap()
}

async fn sweep_until(
    client: &matrix_sdk::Client,
    index: &Arc<Mutex<Option<SearchIndex>>>,
    id: &str,
) {
    let stop = AtomicBool::new(false);
    for _ in 0..100 {
        sweep(client, index, &stop).await;
        if indexed(index, id) {
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    panic!("{id} never reached the index");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn messages_behind_a_limited_sync_are_indexed_by_the_next_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));

    sync(&client, vec![message("$m1", "an older line", 1000)], "1", None).await;
    sweep_until(&client, &index, "$m1").await;

    let gap: Vec<Raw<matrix_sdk::ruma::events::AnyTimelineEvent>> = [
        message("$m3", "the zephyrine plan", 3000),
        message("$m2", "a second gap line", 2000),
        message("$m1", "an older line", 1000),
    ]
    .into_iter()
    .map(|v| serde_json::from_value(v).unwrap())
    .collect();
    server
        .mock_room_messages()
        .ok(RoomMessagesResponseTemplate::default().events(gap))
        .mount()
        .await;
    sync(&client, vec![message("$m4", "the newest line", 4000)], "2", Some("gap-token")).await;
    sweep_until(&client, &index, "$m4").await;

    assert!(indexed(&index, "$m3"), "a message that arrived in the sync gap was never indexed");
    assert!(indexed(&index, "$m2"));
    let hits = index.lock().unwrap().as_ref().unwrap().search("zephyrine", "", 10, 0).unwrap();
    assert_eq!(hits.len(), 1);
}

#[test]
fn rooms_active_since_the_last_sweep_come_first() {
    let rooms: Vec<(usize, u64)> = (0..100).map(|i| (i, if i == 97 { 5_000 } else { 10 })).collect();
    let (order, moved) = sweep_order(rooms.clone(), 1_000, 0, 40);
    assert_eq!(order.len(), 40);
    assert_eq!(order[0], 97, "the room with new messages waited for its turn");
    assert_eq!(moved, 39);
    let (first, moved) = sweep_order(rooms, 0, 0, 40);
    assert_eq!(first[0], 97, "the first sweep after start-up skipped the newest room");
    assert_eq!(moved, 0);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_own_message_is_indexed_as_sync_delivers_it_without_a_sweep() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let index = Arc::new(Mutex::new(Some(SearchIndex::open_in_memory().unwrap())));
    let _guard = register_live_index_handler(&client, &index);
    let me = client.user_id().unwrap().to_string();

    sync(&client, vec![from(&me, "$own", "Issue is still there", 1000)], "1", None).await;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while !indexed(&index, "$own") {
        assert!(std::time::Instant::now() < deadline, "the user's own message was never indexed");
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;
    }
    let hits = index.lock().unwrap().as_ref().unwrap().search("issue is still", "", 10, 0).unwrap();
    assert_eq!(hits.len(), 1);
}
