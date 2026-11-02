
use std::future::Future;
use std::time::Duration;

use serde_json::{json, Value};

pub(crate) const ROOM_ACTION_RETRY_DELAYS: [Duration; 2] =
    [Duration::from_millis(1000), Duration::from_millis(3000)];

/// True when the request never got an answer from the server: the connection
/// failed, was reset, or timed out. Nothing the server said is in this class,
/// so retrying cannot repeat a refusal.
pub(crate) fn is_transport_failure(err: &matrix_sdk::Error) -> bool {
    matches!(err, matrix_sdk::Error::Http(http)
        if matches!(http.as_ref(), matrix_sdk::HttpError::Reqwest(_)))
}

pub(crate) fn failure_reason(err: &matrix_sdk::Error) -> String {
    if let matrix_sdk::Error::Http(http) = err {
        if matches!(http.as_ref(), matrix_sdk::HttpError::RefreshToken(_)) {
            return "refresh_failed".to_owned();
        }
    }
    match err {
        matrix_sdk::Error::AuthenticationRequired => "not_logged_in".to_owned(),
        _ => crate::oauth::sdk_failure_reason(err),
    }
}

pub(crate) fn failure_event(action: &str, err: &matrix_sdk::Error, attempts: u32) -> Value {
    json!({
        "type": "room_action_error",
        "action": action,
        "reason": failure_reason(err),
        "attempts": attempts,
    })
}

pub(crate) async fn with_transport_retry<T, F, Fut>(
    delays: &[Duration],
    mut op: F,
) -> (matrix_sdk::Result<T>, u32)
where
    F: FnMut() -> Fut,
    Fut: Future<Output = matrix_sdk::Result<T>>,
{
    let mut attempts: u32 = 0;
    loop {
        attempts += 1;
        match op().await {
            Err(err) if is_transport_failure(&err) => {
                match delays.get(attempts as usize - 1) {
                    Some(delay) => tokio::time::sleep(*delay).await,
                    None => return (Err(err), attempts),
                }
            }
            result => return (result, attempts),
        }
    }
}

pub(crate) async fn set_favourite(
    room: &matrix_sdk::Room,
    favourite: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_is_favourite(favourite, None).await }
    })
    .await
}

pub(crate) async fn set_low_priority(
    room: &matrix_sdk::Room,
    low_priority: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_is_low_priority(low_priority, None).await }
    })
    .await
}

pub(crate) async fn set_unread_flag(
    room: &matrix_sdk::Room,
    unread: bool,
    delays: &[Duration],
) -> (matrix_sdk::Result<()>, u32) {
    with_transport_retry(delays, || {
        let room = room.clone();
        async move { room.set_unread_flag(unread).await }
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{mpsc, Arc};
    use std::thread;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{api::client::sync::sync_events::v5, room_id, OwnedDeviceId, OwnedUserId},
        store::RoomLoadSettings,
        Client, SessionMeta, SessionTokens,
    };
    use matrix_sdk_base::RequestedRequiredStates;

    const FAST: [Duration; 2] = [Duration::from_millis(10), Duration::from_millis(10)];

    #[derive(Clone, Copy)]
    enum TagScript {
        DropFirst,
        DropAlways,
        Forbidden,
    }

    fn read_request_line(stream: &mut TcpStream) -> String {
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
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    fn serve(script: TagScript) -> (String, Arc<AtomicUsize>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let tag_requests = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&tag_requests);
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request_line(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else if path.contains("/tags/") {
                        let n = seen.fetch_add(1, Ordering::SeqCst) + 1;
                        match script {
                            TagScript::DropFirst if n == 1 => drop(stream),
                            TagScript::DropAlways => drop(stream),
                            TagScript::Forbidden => respond(
                                &mut stream,
                                "403 Forbidden",
                                r#"{"errcode":"M_FORBIDDEN","error":"secret detail !room:example.org"}"#,
                            ),
                            _ => respond(&mut stream, "200 OK", "{}"),
                        }
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        (format!("http://{}:{}", addr.ip(), addr.port()), tag_requests)
    }

    async fn client_with_room(homeserver: &str) -> (Client, matrix_sdk::Room) {
        let client = Client::builder()
            .homeserver_url(homeserver)
            .handle_refresh_tokens()
            .build()
            .await
            .expect("client");
        let session = MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                device_id: OwnedDeviceId::from("TESTDEVICE"),
            },
            tokens: SessionTokens { access_token: "access-0".to_owned(), refresh_token: None },
        };
        client
            .matrix_auth()
            .restore_session(session, RoomLoadSettings::default())
            .await
            .expect("restore");
        let id = room_id!("!fav:example.org");
        let mut response = v5::Response::new("pos-1".to_owned());
        response.rooms.insert(id.to_owned(), v5::response::Room::default());
        client
            .process_sliding_sync_test_helper(&response, &RequestedRequiredStates::default())
            .await
            .expect("sync processed");
        let room = client.get_room(id).expect("room known after sync");
        (client, room)
    }

    const BOUND: Duration = Duration::from_secs(20);

    #[tokio::test]
    async fn a_favourite_write_survives_one_dropped_connection() {
        let (homeserver, tag_requests) = serve(TagScript::DropFirst);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        assert!(result.is_ok(), "a single dropped connection failed the write: {result:?}");
        assert_eq!(attempts, 2);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 2);
    }

    #[tokio::test]
    async fn the_sdk_alone_does_not_retry_a_dropped_connection() {
        let (homeserver, tag_requests) = serve(TagScript::DropAlways);
        let (_client, room) = client_with_room(&homeserver).await;

        let err = tokio::time::timeout(BOUND, room.set_is_favourite(true, None))
            .await
            .expect("bounded")
            .expect_err("a dropped connection cannot succeed");

        assert!(is_transport_failure(&err), "{err:?}");
        assert_eq!(tag_requests.load(Ordering::SeqCst), 1);
        assert!(
            matches!(failure_reason(&err).as_str(), "network" | "connect" | "timeout"),
            "{}",
            failure_reason(&err)
        );
    }

    #[tokio::test]
    async fn a_dead_server_is_retried_a_bounded_number_of_times() {
        let (homeserver, tag_requests) = serve(TagScript::DropAlways);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        let err = result.expect_err("every attempt was dropped");
        assert_eq!(attempts, 3);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 3);
        let event = failure_event("favourite", &err, attempts);
        assert_eq!(event["attempts"], 3);
        assert!(matches!(event["reason"].as_str(), Some("network" | "connect" | "timeout")),
                "{event}");
    }

    #[tokio::test]
    async fn a_refusal_is_not_retried_and_names_its_errcode() {
        let (homeserver, tag_requests) = serve(TagScript::Forbidden);
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_favourite(&room, true, &FAST)).await.expect("bounded");

        let err = result.expect_err("the server refused");
        assert_eq!(attempts, 1);
        assert_eq!(tag_requests.load(Ordering::SeqCst), 1);
        let event = failure_event("favourite", &err, attempts);
        assert_eq!(event["type"], "room_action_error");
        assert_eq!(event["action"], "favourite");
        assert_eq!(event["reason"], "http_403_M_FORBIDDEN");
        let text = event.to_string();
        assert!(!text.contains("secret") && !text.contains("!room"), "{text}");
    }

    #[tokio::test]
    async fn the_unread_mark_survives_one_dropped_connection() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let writes = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&writes);
        thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let seen = Arc::clone(&seen);
                thread::spawn(move || {
                    let line = read_request_line(&mut stream);
                    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();
                    if path.contains("/rooms/") && path.contains("/account_data/") {
                        if seen.fetch_add(1, Ordering::SeqCst) == 0 {
                            drop(stream);
                        } else {
                            respond(&mut stream, "200 OK", "{}");
                        }
                    } else if path.starts_with("/_matrix/client/versions") {
                        respond(&mut stream, "200 OK", r#"{"versions":["v1.1","v1.11"]}"#);
                    } else {
                        respond(&mut stream, "200 OK", "{}");
                    }
                });
            }
        });
        let homeserver = format!("http://{}:{}", addr.ip(), addr.port());
        let (_client, room) = client_with_room(&homeserver).await;

        let (result, attempts) =
            tokio::time::timeout(BOUND, set_unread_flag(&room, true, &FAST)).await.expect("bounded");

        let writes = writes.load(Ordering::SeqCst);
        assert!(result.is_ok(), "{result:?} writes={writes}");
        assert_eq!(attempts, 2, "writes={writes}");
        assert_eq!(writes, 2);
    }
}
