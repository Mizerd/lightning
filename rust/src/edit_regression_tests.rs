use super::*;
use matrix_sdk::ruma::{api::client::sync::sync_events::v5, room_id, serde::Raw};
use matrix_sdk::test_utils::mocks::MatrixMockServer;
use matrix_sdk_base::RequestedRequiredStates;
use std::time::{Duration, Instant};

const ROOM: &str = "!edits:localhost";
const SENDER: &str = "@alice:localhost";
fn message(id: &str, body: &str, ts: u64) -> serde_json::Value {
    json!({"type":"m.room.message", "event_id":id, "sender":SENDER,
        "origin_server_ts":ts, "content":{"msgtype":"m.text", "body":body}})
}
fn edit(id: &str, target: &str, body: &str, ts: u64) -> serde_json::Value {
    let mut value = message(id, "* WRONG fallback", ts);
    value["content"]["m.new_content"] = json!({"msgtype":"m.text", "body":body});
    value["content"]["m.relates_to"] = json!({"rel_type":"m.replace", "event_id":target});
    value
}
async fn cached(client: &Client, ids: &[String]) {
    let room = client.get_room(room_id!("!edits:localhost")).unwrap();
    let (cache, _handles) = room.event_cache().await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    for id in ids {
        let id = matrix_sdk::ruma::EventId::parse(id).unwrap();
        while cache.find_event(&id).await.unwrap().is_none() {
            assert!(Instant::now() < deadline, "{id} never reached the event cache");
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    }
}
async fn sync(client: &Client, events: Vec<serde_json::Value>, pos: &str) {
    let ids: Vec<String> = events
        .iter()
        .map(|v| v["event_id"].as_str().unwrap().to_owned())
        .collect();
    let mut response = v5::Response::new(pos.to_owned());
    let mut room = v5::response::Room::default();
    room.timeline = events
        .into_iter()
        .map(|v| Raw::from_json_string(v.to_string()).unwrap())
        .collect();
    response
        .rooms
        .insert(room_id!("!edits:localhost").to_owned(), room);
    client
        .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
        .await
        .unwrap();
    cached(client, &ids).await;
}
async fn build(client: &Client) -> Timeline {
    TimelineBuilder::new(&client.get_room(room_id!("!edits:localhost")).unwrap())
        .event_filter(lightning_event_filter)
        .with_focus(TimelineFocus::Live {
            hide_threaded_events: true,
        })
        .build()
        .await
        .unwrap()
}
async fn row(timeline: &Timeline, id: &str) -> serde_json::Value {
    let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
    timeline
        .items()
        .await
        .iter()
        .find_map(|item| {
            let event = item.as_event()?;
            (event.event_id().map(|e| e.as_str()) == Some(id))
                .then(|| item_to_json(item, ROOM, "@me:localhost", &registry))
        })
        .unwrap_or(serde_json::Value::Null)
}
async fn wait_field(
    timeline: &Timeline,
    id: &str,
    field: &str,
    expected: &str,
) -> serde_json::Value {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let value = row(timeline, id).await;
        if value[field] == expected {
            return value;
        }
        assert!(
            Instant::now() < deadline,
            "{id}.{field}: expected {expected:?}, got {value}"
        );
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}
async fn wait_body(timeline: &Timeline, id: &str, expected: &str) -> serde_json::Value {
    wait_field(timeline, id, "body", expected).await
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn live_edits_keep_identity_and_use_new_content() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let mut original = message("$original", "original", 1000);
    original["content"]["format"] = json!("org.matrix.custom.html");
    original["content"]["formatted_body"] = json!("<b>original</b>");
    sync(&client, vec![original], "1").await;
    let timeline = build(&client).await;
    let before = wait_body(&timeline, "$original", "original").await;
    let mut replacement = edit("$edit1", "$original", "first edit", 2000);
    replacement["content"]["m.new_content"]["format"] = json!("org.matrix.custom.html");
    replacement["content"]["m.new_content"]["formatted_body"] = json!("<b>first edit</b>");
    sync(&client, vec![replacement], "2").await;
    let after = wait_body(&timeline, "$original", "first edit").await;
    assert_eq!(after["item_id"], before["item_id"]);
    assert_eq!(after["edited"], true);
    assert_eq!(after["formatted_body"], "<b>first edit</b>");
    sync(
        &client,
        vec![edit("$edit2", "$original", "second edit", 3000)],
        "3",
    )
    .await;
    let after = wait_body(&timeline, "$original", "second edit").await;
    assert_eq!(after["edited"], true);
    assert!(after["formatted_body"]
        .as_str()
        .unwrap_or_default()
        .is_empty());
    let mut invalid = edit("$invalid", "$original", "forged", 4000);
    invalid["sender"] = json!("@mallory:localhost");
    sync(
        &client,
        vec![invalid, message("$barrier", "barrier", 5000)],
        "4",
    )
    .await;
    wait_body(&timeline, "$barrier", "barrier").await;
    assert_eq!(row(&timeline, "$original").await["body"], "second edit");
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_edit_received_without_a_timeline_survives_reopen() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    sync(&client, vec![message("$original", "original", 1000)], "1").await;
    {
        let timeline = build(&client).await;
        wait_body(&timeline, "$original", "original").await;
    }
    sync(
        &client,
        vec![edit("$edit", "$original", "closed room edit", 2000)],
        "2",
    )
    .await;
    let timeline = build(&client).await;
    let value = wait_body(&timeline, "$original", "closed room edit").await;
    assert_eq!(value["edited"], true);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_edit_refreshes_a_reply_quote() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let mut reply = message("$reply", "a reply", 2000);
    reply["content"]["m.relates_to"] = json!({"m.in_reply_to":{"event_id":"$original"}});
    sync(
        &client,
        vec![message("$original", "original", 1000), reply],
        "1",
    )
    .await;
    let timeline = build(&client).await;
    wait_body(&timeline, "$reply", "a reply").await;
    timeline
        .fetch_details_for_event(&matrix_sdk::ruma::EventId::parse("$reply").unwrap())
        .await
        .unwrap();
    sync(
        &client,
        vec![
            edit("$edit", "$original", "edited quote", 3000),
            message("$barrier", "barrier", 4000),
        ],
        "2",
    )
    .await;
    wait_body(&timeline, "$barrier", "barrier").await;
    wait_field(&timeline, "$reply", "reply_to_preview", "edited quote").await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_edit_refreshes_thread_reply_and_main_summary() {
    let server = MatrixMockServer::new().await;
    let client = server
        .client_builder()
        .on_builder(|b| {
            b.with_threading_support(matrix_sdk::ThreadingSupport::Enabled {
                with_subscriptions: false,
            })
        })
        .build()
        .await;
    client.event_cache().subscribe().unwrap();
    let mut reply = message("$reply", "thread original", 2000);
    reply["content"]["m.relates_to"] = json!({"rel_type":"m.thread", "event_id":"$root",
        "is_falling_back":true, "m.in_reply_to":{"event_id":"$root"}});
    sync(&client, vec![message("$root", "root", 1000), reply], "1").await;
    let main = build(&client).await;
    let room = client.get_room(room_id!("!edits:localhost")).unwrap();
    let thread = TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
        .with_focus(TimelineFocus::Thread {
            root_event_id: matrix_sdk::ruma::EventId::parse("$root").unwrap(),
        })
        .build()
        .await
        .unwrap();
    let before = wait_body(&thread, "$reply", "thread original").await;
    sync(
        &client,
        vec![
            edit("$edit", "$reply", "thread edited", 3000),
            message("$barrier", "barrier", 4000),
        ],
        "2",
    )
    .await;
    wait_body(&main, "$barrier", "barrier").await;
    let after = wait_body(&thread, "$reply", "thread edited").await;
    assert_eq!(after["edited"], true);
    assert_eq!(after["item_id"], before["item_id"]);
    assert!(
        row(&main, "$reply").await.is_null(),
        "thread reply leaked into main"
    );
    wait_field(&main, "$root", "thread_latest_preview", "thread edited").await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "requires localhs and LIGHTNING_EDITS_LOCALHS_ACCOUNTS"]
async fn localhs_remote_edit_over_http() {
    use matrix_sdk::{
        authentication::matrix::MatrixSession, store::RoomLoadSettings, SessionMeta, SessionTokens,
    };
    let accounts: serde_json::Value = serde_json::from_slice(
        &std::fs::read(
            std::env::var("LIGHTNING_EDITS_LOCALHS_ACCOUNTS").expect("opt-in accounts path"),
        )
        .unwrap(),
    )
    .unwrap();
    let account = |name| {
        accounts
            .as_array()
            .unwrap()
            .iter()
            .find(|a| a["username"] == name)
            .unwrap()
            .clone()
    };
    let alice = account("ed001");
    let bob = account("ed002");
    let http = reqwest::Client::new();
    async fn call(
        http: &reqwest::Client,
        user: &serde_json::Value,
        method: reqwest::Method,
        path: &str,
        body: serde_json::Value,
    ) -> serde_json::Value {
        let response = http
            .request(
                method,
                format!("http://127.0.0.1:18008/_matrix/client/v3/{path}"),
            )
            .bearer_auth(user["access_token"].as_str().unwrap())
            .header("Content-Type", "application/json")
            .body(body.to_string())
            .send()
            .await
            .unwrap();
        assert!(
            response.status().is_success(),
            "localhs API {path}: {}",
            response.status()
        );
        serde_json::from_slice(&response.bytes().await.unwrap()).unwrap()
    }
    let room = call(
        &http,
        &alice,
        reqwest::Method::POST,
        "createRoom",
        json!({
            "preset":"private_chat", "invite":[bob["user_id"]], "name":"Lightning edits regression"
        }),
    )
    .await["room_id"]
        .as_str()
        .unwrap()
        .to_owned();
    call(
        &http,
        &bob,
        reqwest::Method::POST,
        &format!("join/{room}"),
        json!({}),
    )
    .await;
    let client = Client::builder()
        .homeserver_url("http://127.0.0.1:18008")
        .with_threading_support(matrix_sdk::ThreadingSupport::Enabled {
            with_subscriptions: false,
        })
        .build()
        .await
        .unwrap();
    client
        .matrix_auth()
        .restore_session(
            MatrixSession {
                meta: SessionMeta {
                    user_id: matrix_sdk::ruma::UserId::parse(bob["user_id"].as_str().unwrap())
                        .unwrap(),
                    device_id: bob["device_id"].as_str().unwrap().into(),
                },
                tokens: SessionTokens {
                    access_token: bob["access_token"].as_str().unwrap().to_owned(),
                    refresh_token: None,
                },
            },
            RoomLoadSettings::default(),
        )
        .await
        .unwrap();
    client.event_cache().subscribe().unwrap();
    client
        .sync_once(matrix_sdk::config::SyncSettings::default())
        .await
        .unwrap();
    let original = call(&http, &alice, reqwest::Method::PUT, &format!("rooms/{room}/send/m.room.message/original"),
        json!({"msgtype":"m.text", "body":"original", "format":"org.matrix.custom.html", "formatted_body":"<b>original</b>"})).await;
    let id = original["event_id"].as_str().unwrap();
    client
        .sync_once(matrix_sdk::config::SyncSettings::default())
        .await
        .unwrap();
    let sdk_room = client
        .get_room(&matrix_sdk::ruma::RoomId::parse(&room).unwrap())
        .unwrap();
    assert!(
        !sdk_room.encryption_state().is_encrypted(),
        "localhs test must be unencrypted"
    );
    let timeline = TimelineBuilder::new(&sdk_room)
        .event_filter(lightning_event_filter)
        .with_focus(TimelineFocus::Live {
            hide_threaded_events: true,
        })
        .build()
        .await
        .unwrap();
    let before = wait_body(&timeline, id, "original").await;
    call(&http, &alice, reqwest::Method::PUT, &format!("rooms/{room}/send/m.room.message/edit"), json!({
        "msgtype":"m.text", "body":"* WRONG fallback", "m.new_content":{"msgtype":"m.text", "body":"latest edit",
            "format":"org.matrix.custom.html", "formatted_body":"<b>latest edit</b>"},
        "m.relates_to":{"rel_type":"m.replace", "event_id":id}
    })).await;
    client
        .sync_once(matrix_sdk::config::SyncSettings::default())
        .await
        .unwrap();
    let after = wait_body(&timeline, id, "latest edit").await;
    assert_eq!(after["item_id"], before["item_id"]);
    assert_eq!(after["formatted_body"], "<b>latest edit</b>");
    assert_eq!(after["edited"], true);
    assert_eq!(
        crate::latest_event_preview_text(&sdk_room.latest_event()),
        "latest edit"
    );
    drop(timeline);
    let reopened = TimelineBuilder::new(&sdk_room)
        .event_filter(lightning_event_filter)
        .build()
        .await
        .unwrap();
    wait_body(&reopened, id, "latest edit").await;
    println!("localhs: original -> remote edit -> stable row, edited HTML/marker, latest preview, reopened cache: PASS");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_late_megolm_key_reapplies_an_edit_in_place() {
    use matrix_sdk::ruma::{device_id, user_id};
    use matrix_sdk_base::crypto::{olm::EncryptionSettings, OlmMachine};
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client
        .encryption()
        .wait_for_e2ee_initialization_tasks()
        .await;
    client.event_cache().subscribe().unwrap();
    let receiver = client.olm_machine_for_testing().await;
    let receiver = receiver.as_ref().unwrap();
    async fn encrypted(machine: &OlmMachine, value: serde_json::Value) -> serde_json::Value {
        let room = room_id!("!edits:localhost");
        machine
            .share_room_key(room, std::iter::empty(), EncryptionSettings::default())
            .await
            .unwrap();
        let content = Raw::from_json_string(value["content"].to_string()).unwrap();
        let encrypted = machine
            .encrypt_room_event_raw(room, "m.room.message", &content)
            .await
            .unwrap();
        let mut event = value;
        event["type"] = json!("m.room.encrypted");
        event["content"] = serde_json::from_str(encrypted.content.json().get()).unwrap();
        event
    }
    let first = OlmMachine::new(user_id!("@alice:localhost"), device_id!("FIRST")).await;
    let original = encrypted(&first, message("$original", "original", 1000)).await;
    let keys = first.store().export_room_keys(|_| true).await.unwrap();
    receiver
        .store()
        .import_exported_room_keys(keys, |_, _| {})
        .await
        .unwrap();
    sync(&client, vec![original], "1").await;
    let timeline = build(&client).await;
    let before = wait_body(&timeline, "$original", "original").await;
    let second = OlmMachine::new(user_id!("@alice:localhost"), device_id!("SECOND")).await;
    let replacement = encrypted(&second, edit("$edit", "$original", "late edit", 2000)).await;
    sync(
        &client,
        vec![replacement, message("$barrier", "barrier", 3000)],
        "2",
    )
    .await;
    wait_body(&timeline, "$barrier", "barrier").await;
    assert_eq!(row(&timeline, "$original").await["body"], "original");
    let keys = second.store().export_room_keys(|_| true).await.unwrap();
    receiver
        .store()
        .import_exported_room_keys(keys, |_, _| {})
        .await
        .unwrap();
    let after = wait_body(&timeline, "$original", "late edit").await;
    assert_eq!(after["edited"], true);
    assert_eq!(after["item_id"], before["item_id"]);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_quote_loaded_after_an_edit_uses_the_current_target() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let mut reply = message("$reply", "a reply", 3000);
    reply["content"]["m.relates_to"] = json!({"m.in_reply_to":{"event_id":"$original"}});
    sync(
        &client,
        vec![
            message("$original", "original", 1000),
            edit("$edit", "$original", "edited quote", 2000),
            reply,
        ],
        "1",
    )
    .await;
    let timeline = build(&client).await;
    wait_body(&timeline, "$original", "edited quote").await;
    timeline
        .fetch_details_for_event(&matrix_sdk::ruma::EventId::parse("$reply").unwrap())
        .await
        .unwrap();
    wait_field(&timeline, "$reply", "reply_to_preview", "edited quote").await;
}

// Protocol expectations retained as opt-in upstream regressions. SDK 0.18's
// Aggregations::try_remove_aggregation leaves the previous display untouched
// when no remaining valid edit can be applied. Do not call these fixed.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "known matrix-sdk-ui 0.18 edit-redaction defect"]
async fn sdk_edit_redaction_restores_the_original_after_the_only_edit() {
    sdk_edit_redaction(false).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "known matrix-sdk-ui 0.18 invalid edit blocks redaction fallback"]
async fn sdk_edit_redaction_skips_a_forged_edit_when_restoring_the_previous_version() {
    sdk_edit_redaction(true).await;
}

async fn sdk_edit_redaction(with_forgery: bool) {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let mut events = vec![message("$original", "original", 1000)];
    if with_forgery {
        events.push(edit("$first", "$original", "first edit", 2000));
    }
    events.push(edit("$latest", "$original", "latest edit", 3000));
    if with_forgery {
        let mut invalid = edit("$forged", "$original", "forged", 4000);
        invalid["sender"] = json!("@mallory:localhost");
        events.push(invalid);
    }
    sync(&client, events, "1").await;
    let timeline = build(&client).await;
    wait_body(&timeline, "$original", "latest edit").await;
    sync(
        &client,
        vec![json!({"type":"m.room.redaction", "event_id":"$redact",
        "sender":SENDER, "origin_server_ts":6000, "redacts":"$latest",
        "content":{"redacts":"$latest"}})],
        "2",
    )
    .await;
    wait_body(
        &timeline,
        "$original",
        if with_forgery {
            "first edit"
        } else {
            "original"
        },
    )
    .await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_open_thread_refills_after_its_room_cache_is_cleared() {
    use matrix_sdk::deserialized_responses::TimelineEvent;
    use matrix_sdk::test_utils::mocks::RoomRelationsResponseTemplate;
    let server = MatrixMockServer::new().await;
    let client = server
        .client_builder()
        .on_builder(|b| {
            b.with_threading_support(matrix_sdk::ThreadingSupport::Enabled {
                with_subscriptions: false,
            })
        })
        .build()
        .await;
    client.event_cache().subscribe().unwrap();
    let root = message("$root", "root", 1000);
    let mut reply = message("$reply", "in the thread", 2000);
    reply["content"]["m.relates_to"] = json!({"rel_type":"m.thread", "event_id":"$root",
        "is_falling_back":true, "m.in_reply_to":{"event_id":"$root"}});
    sync(&client, vec![root.clone(), reply.clone()], "1").await;

    let mut fetched = reply.clone();
    fetched["room_id"] = json!(ROOM);
    server
        .mock_room_relations()
        .match_target_event(matrix_sdk::ruma::EventId::parse("$root").unwrap())
        .ok(RoomRelationsResponseTemplate::default()
            .events(vec![Raw::from_json_string(fetched.to_string()).unwrap()]))
        .mount()
        .await;
    server
        .mock_room_event()
        .match_event_id()
        .ok(TimelineEvent::from_plaintext(Raw::from_json_string(root.to_string()).unwrap()))
        .mount()
        .await;

    let events: EventQueue = Arc::new(Mutex::new(VecDeque::new()));
    let registry = Arc::new(TimelineRegistry::new(Arc::clone(&events)));
    registry.thread_gen.store(1, Ordering::SeqCst);
    let reached = Arc::new(AtomicBool::new(true));
    *registry.active_thread.lock().unwrap() = Some(ActiveThread {
        room_id: ROOM.to_owned(),
        root_event_id: "$root".to_owned(),
        thread_gen: 1,
        timeline: None,
        task: None,
        pagination_busy: Arc::new(AtomicBool::new(false)),
        reached_start: Arc::clone(&reached),
        clear_epoch: Arc::new(AtomicU64::new(0)),
    });
    let lifecycle = registry.lifecycle_gen.load(Ordering::SeqCst);
    let task = tokio::spawn(open_thread_task(
        Arc::clone(&registry), client.clone(), ROOM.to_owned(), "$root".to_owned(),
        1, lifecycle, Arc::clone(&events)));

    let has_reply = |v: &serde_json::Value| v.to_string().contains("\"$reply\"");
    next_event(&events, |v| {
        (v["type"] == "thread_reset" || v["type"] == "thread_diff") && has_reply(v)
    }).await;
    tokio::time::sleep(Duration::from_millis(200)).await;
    reached.store(true, Ordering::SeqCst);

    let room = client.get_room(room_id!("!edits:localhost")).unwrap();
    let (cache, _handles) = room.event_cache().await.unwrap();
    cache.clear().await.unwrap();

    next_event(&events, |v| v["type"] == "thread_diff" && v["op"] == "clear").await;
    next_event(&events, |v| v["type"] == "thread_pagination" && v["state"] == "loading").await;
    next_event(&events, |v| v["type"] == "thread_diff" && has_reply(v)).await;
    next_event(&events, |v| v["type"] == "thread_pagination" && v["state"] == "idle").await;

    // A pagination that began before a clear must not mark the new thread as started.
    let (timeline, epoch) = {
        let guard = registry.active_thread.lock().unwrap();
        let thread = guard.as_ref().unwrap();
        (thread.timeline.clone().unwrap(), Arc::clone(&thread.clear_epoch))
    };
    let started = epoch.load(Ordering::SeqCst);
    epoch.fetch_add(1, Ordering::SeqCst);
    reached.store(false, Ordering::SeqCst);
    finish_thread_pagination(&registry, &timeline, &events, ROOM, "$root", 1, lifecycle,
                             &reached, &epoch, started, Ok(true));
    assert!(!reached.load(Ordering::SeqCst), "a stale answer marked the thread as started");
    next_event(&events, |v| v["type"] == "thread_pagination" && v["state"] == "loading").await;
    task.abort();
}

async fn next_event(
    events: &EventQueue,
    pred: impl Fn(&serde_json::Value) -> bool,
) -> serde_json::Value {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let found = {
            let mut queue = events.lock().unwrap();
            let mut hit = None;
            while let Some(raw) = queue.pop_front() {
                let value: serde_json::Value = serde_json::from_str(&raw).unwrap();
                if pred(&value) {
                    hit = Some(value);
                    break;
                }
            }
            hit
        };
        if let Some(value) = found {
            return value;
        }
        assert!(Instant::now() < deadline, "the expected thread event never arrived");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_stable_poll_is_a_poll_row_with_votes_from_both_namespaces() {
    let server = MatrixMockServer::new().await;
    let client = server.client_builder().build().await;
    client.event_cache().subscribe().unwrap();
    let poll = json!({"type":"m.poll.start", "event_id":"$poll", "sender":SENDER,
        "origin_server_ts":1000, "content":{"m.text":[{"body":"Lunch?"}],
        "m.poll":{"kind":"m.disclosed", "max_selections":1,
            "question":{"m.text":[{"body":"Lunch?"}]},
            "answers":[{"m.id":"a", "m.text":[{"body":"Pizza"}]},
                       {"m.id":"b", "m.text":[{"body":"Soup"}]}]}}});
    let reference = json!({"rel_type":"m.reference", "event_id":"$poll"});
    let stable_vote = json!({"type":"m.poll.response", "event_id":"$v1",
        "sender":"@bob:localhost", "origin_server_ts":2000,
        "content":{"m.selections":["a"], "m.relates_to":reference}});
    let unstable_vote = json!({"type":"org.matrix.msc3381.poll.response", "event_id":"$v2",
        "sender":"@carol:localhost", "origin_server_ts":2100,
        "content":{"org.matrix.msc3381.poll.response":{"answers":["a"]},
            "m.relates_to":reference}});
    sync(&client, vec![poll, stable_vote, unstable_vote, message("$after", "after", 3000)], "1")
        .await;
    let timeline = build(&client).await;
    let poll_row = wait_field(&timeline, "$poll", "msgtype", "poll").await;
    assert_eq!(poll_row["poll_question"], "Lunch?");
    assert_eq!(poll_row["poll_answers"].as_array().map(Vec::len), Some(2));
    wait_body(&timeline, "$after", "after").await;
    assert!(row(&timeline, "$v1").await.is_null(), "a vote is never a row");

    let room = client.get_room(room_id!("!edits:localhost")).unwrap();
    let mut watch = crate::stablepoll::Watch::default();
    let items = timeline.items().await;
    let ids = watch.observe(items.iter());
    assert_eq!(ids, vec!["$poll".to_owned()]);
    let registry = TimelineRegistry::new(Arc::new(Mutex::new(VecDeque::new())));
    let changed = watch.refresh(&room, &registry.stable_poll_tallies, &ids, || true).await;
    assert_eq!(changed, ids);
    let poll_item = items
        .iter()
        .find(|item| item.as_event().and_then(|e| e.event_id()).map(|e| e.as_str()) == Some("$poll"))
        .unwrap();
    let rendered = item_to_json(poll_item, ROOM, "@bob:localhost", &registry);
    let pizza = &rendered["poll_answers"][0];
    assert_eq!(pizza["id"], "a");
    assert_eq!(pizza["count"], 2);
    assert_eq!(pizza["by_me"], true);
    assert_eq!(rendered["poll_total_voters"], 2);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_threaded_stable_poll_stays_out_of_the_main_timeline() {
    let server = MatrixMockServer::new().await;
    let client = server
        .client_builder()
        .on_builder(|b| {
            b.with_threading_support(matrix_sdk::ThreadingSupport::Enabled {
                with_subscriptions: false,
            })
        })
        .build()
        .await;
    client.event_cache().subscribe().unwrap();
    let poll = json!({"type":"m.poll.start", "event_id":"$tpoll", "sender":SENDER,
        "origin_server_ts":2000, "content":{"m.text":[{"body":"In a thread?"}],
        "m.relates_to":{"rel_type":"m.thread", "event_id":"$root"},
        "m.poll":{"kind":"m.disclosed", "max_selections":1,
            "question":{"m.text":[{"body":"In a thread?"}]},
            "answers":[{"m.id":"y", "m.text":[{"body":"Yes"}]},
                       {"m.id":"n", "m.text":[{"body":"No"}]}]}}});
    sync(&client, vec![message("$root", "root", 1000), poll, message("$after", "after", 3000)],
         "1").await;
    let timeline = build(&client).await;
    wait_body(&timeline, "$after", "after").await;
    assert!(row(&timeline, "$tpoll").await.is_null(), "a threaded poll leaked into main");
}
