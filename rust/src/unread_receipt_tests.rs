use super::*;
use matrix_sdk::ruma::room_id;
use std::io::{Read as _, Write as _};
use std::net::{TcpListener, TcpStream};
use std::time::{Duration, Instant};

const ME: &str = "@example:localhost";
const ALICE: &str = "@alice:localhost";

fn msg(id: &str, ts: u64) -> serde_json::Value {
    json!({"type":"m.room.message","event_id":id,"sender":ALICE,"origin_server_ts":ts,
        "content":{"msgtype":"m.text","body":id}})
}

fn receipt(event_id: &str, thread: Option<&str>) -> serde_json::Value {
    let mut entry = json!({"ts": 1});
    if let Some(t) = thread {
        entry["thread_id"] = json!(t);
    }
    json!({"type":"m.receipt","content":{event_id:{"m.read":{ME: entry}}}})
}

struct Srv {
    bodies: Arc<Mutex<VecDeque<String>>>,
    addr: String,
}

fn read_request(stream: &mut TcpStream) -> String {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    let head_end = loop {
        let n = stream.read(&mut chunk).unwrap_or(0);
        if n == 0 {
            return String::new();
        }
        buf.extend_from_slice(&chunk[..n]);
        if let Some(pos) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break pos + 4;
        }
    };
    let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
    let length = head
        .lines()
        .find_map(|line| {
            line.to_ascii_lowercase()
                .strip_prefix("content-length:")
                .map(|v| v.trim().parse::<usize>().unwrap_or(0))
        })
        .unwrap_or(0);
    while buf.len() < head_end + length {
        let n = stream.read(&mut chunk).unwrap_or(0);
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
    }
    head.lines().next().unwrap_or("").to_owned()
}

fn respond(stream: &mut TcpStream, status: &str, payload: &str) {
    let response = format!(
        "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{}",
        payload.len(),
        payload
    );
    let _ = stream.write_all(response.as_bytes());
    let _ = stream.flush();
}

fn serve() -> Srv {
    let bodies: Arc<Mutex<VecDeque<String>>> = Arc::new(Mutex::new(VecDeque::new()));
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap().to_string();
    let queue = Arc::clone(&bodies);
    std::thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(mut stream) = stream else { break };
            let queue = Arc::clone(&queue);
            std::thread::spawn(move || {
                let line = read_request(&mut stream);
                let target = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                let path = target.split('?').next().unwrap_or("").to_owned();
                if path == "/_matrix/client/versions" {
                    respond(&mut stream, "200 OK",
                            r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                } else if path.ends_with("/sync") {
                    let body = queue.lock().unwrap().pop_front()
                        .unwrap_or_else(|| r#"{"next_batch":"idle"}"#.to_owned());
                    respond(&mut stream, "200 OK", &body);
                } else {
                    respond(&mut stream, "404 Not Found",
                            r#"{"errcode":"M_UNRECOGNIZED","error":"not served"}"#);
                }
            });
        }
    });
    Srv { bodies, addr }
}

async fn sync(client: &Client, srv: &Srv, events: Vec<serde_json::Value>,
              rcpt: Option<serde_json::Value>, pos: &str) {
    let mut room = json!({"timeline":{"events":events,"limited":false,"prev_batch":format!("p{pos}")},
        "state":{"events":[]},"ephemeral":{"events":[]},"account_data":{"events":[]}});
    if let Some(r) = rcpt {
        room["ephemeral"]["events"] = json!([r]);
    }
    let body = json!({"next_batch": format!("s{pos}"), "rooms":{"join":{ "!unread:localhost": room }}});
    srv.bodies.lock().unwrap().push_back(body.to_string());
    if let Err(e) = client.sync_once(matrix_sdk::config::SyncSettings::default()).await {
        eprintln!("sync {pos}: {e}");
    }
}

async fn settled_unread(client: &Client) -> u64 {
    let Some(room) = client.get_room(room_id!("!unread:localhost")) else { return 999 };
    let deadline = Instant::now() + Duration::from_millis(1500);
    let mut last = room.num_unread_messages();
    let mut stable_since = Instant::now();
    while Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(25)).await;
        let now = room.num_unread_messages();
        if now != last {
            last = now;
            stable_since = Instant::now();
        } else if stable_since.elapsed() > Duration::from_millis(300) {
            break;
        }
    }
    last
}

async fn client() -> (Srv, Client) {
    let srv = serve();
    let client = Client::builder()
        .homeserver_url(format!("http://{}", srv.addr))
        .with_threading_support(matrix_sdk::ThreadingSupport::Enabled { with_subscriptions: false })
        .build()
        .await
        .unwrap();
    client
        .matrix_auth()
        .restore_session(
            matrix_sdk::authentication::matrix::MatrixSession {
                meta: matrix_sdk::SessionMeta {
                    user_id: matrix_sdk::ruma::OwnedUserId::try_from(ME).unwrap(),
                    device_id: matrix_sdk::ruma::OwnedDeviceId::from("PROBE"),
                },
                tokens: matrix_sdk::SessionTokens {
                    access_token: "t".to_owned(),
                    refresh_token: None,
                },
            },
            matrix_sdk::store::RoomLoadSettings::default(),
        )
        .await
        .unwrap();
    client.event_cache().subscribe().unwrap();
    (srv, client)
}

async fn latest_visible(client: &Client) -> Option<String> {
    let room = client.get_room(room_id!("!unread:localhost")).unwrap();
    let timeline = TimelineBuilder::new(&room)
        .event_filter(lightning_event_filter)
        .with_focus(TimelineFocus::Live { hide_threaded_events: true })
        .build()
        .await
        .unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    timeline
        .items()
        .await
        .iter()
        .rev()
        .find_map(|item| item.as_event()?.event_id().map(|e| e.to_string()))
}

async fn read_then_receipt_where_lightning_puts_it(after: Vec<serde_json::Value>) -> u64 {
    let (srv, client) = client().await;
    let start = json!({"type":"org.matrix.msc3381.poll.start","event_id":"$poll","sender":ALICE,
        "origin_server_ts":1,"content":{"org.matrix.msc1767.text":"q",
        "org.matrix.msc3381.poll.start":{"kind":"org.matrix.msc3381.poll.disclosed",
            "max_selections":1,"question":{"org.matrix.msc1767.text":"q"},
            "answers":[{"id":"a","org.matrix.msc1767.text":"a"},
                       {"id":"b","org.matrix.msc1767.text":"b"}]}}});
    sync(&client, &srv, vec![start, msg("$m2", 2)], Some(receipt("$m2", Some("main"))), "1").await;
    assert_eq!(settled_unread(&client).await, 0, "the room starts read");
    sync(&client, &srv, after, None, "2").await;
    let row = latest_visible(&client).await.expect("a visible row");
    assert_eq!(row, "$m2", "the new event drew a row of its own");
    let room = client.get_room(room_id!("!unread:localhost")).unwrap();
    let target = crate::in_room_receipt_target(
        &room, matrix_sdk::ruma::OwnedEventId::try_from(row).unwrap()).await;
    sync(&client, &srv, vec![], Some(receipt(target.as_str(), Some("main"))), "3").await;
    settled_unread(&client).await
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_ended_poll_after_the_newest_row_does_not_leave_the_room_unread() {
    let end = json!({"type":"org.matrix.msc3381.poll.end","event_id":"$end","sender":ALICE,
        "origin_server_ts":3,"content":{"org.matrix.msc1767.text":"ended",
        "m.relates_to":{"rel_type":"m.reference","event_id":"$poll"}}});
    assert_eq!(read_then_receipt_where_lightning_puts_it(vec![end]).await, 0);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_stable_poll_no_row_shows_does_not_leave_the_room_unread() {
    let start = json!({"type":"m.poll.start","event_id":"$ps","sender":ALICE,
        "origin_server_ts":3,"content":{"m.text":[{"body":"q"}],
        "m.poll":{"kind":"m.disclosed","max_selections":1,"question":{"m.text":[{"body":"q"}]},
        "answers":[{"m.id":"a","m.text":[{"body":"a"}]},{"m.id":"b","m.text":[{"body":"b"}]}]}}});
    assert_eq!(read_then_receipt_where_lightning_puts_it(vec![start]).await, 0);
}
