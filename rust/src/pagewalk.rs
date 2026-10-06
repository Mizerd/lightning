//! Walking a run of history that the timeline filter empties, inside ONE
//! backward page.
//!
//! A room that hosts calls carries thousands of MatrixRTC membership events,
//! which `lightning_event_filter` drops before they become timeline items
//! (docs/timeline-scrolling.md). matrix-sdk's `paginate_backwards` serves ONE
//! STORED CHUNK per call (about twenty events) and ignores the batch size for
//! it, so walking such a run took one C++ round trip per chunk: dispatch, the
//! bridge poll, the controller's continuation delay, and one unit of the fill
//! or near-top budget each. Measured 2026-10-04: one room open made ~20 pages
//! with `added= 0` while `droppedRtc` climbed to ~987, and the reader had to
//! keep scrolling to get past it.
//!
//! Here the bridge keeps going while a page brought no event the timeline
//! would show and the start is not reached, under its own small bound, so C++
//! sees one page per walk. This is NOT a bigger page: each call asks for the
//! same count, a walk ends on the first page that brings anything shown (so it
//! inserts at most what one ordinary page inserts and the pane's row bounds are
//! untouched), and the time bound stops it after about one network round
//! trip's worth of slow pages.
//!
//! What a page brought is read from the room's EVENT CACHE update stream, not
//! from the timeline's items: matrix-sdk sends a pagination's update before
//! `paginate_backwards` returns, while the timeline applies it on its own task
//! some time later, so an item count taken right after a page can still be the
//! old one and a page of real messages would look empty.

use std::future::Future;
use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::deserialized_responses::TimelineEvent;
use matrix_sdk::event_cache::{RoomEventCacheSubscriber, RoomEventCacheUpdate};
use matrix_sdk::ruma::room_version_rules::RoomVersionRules;
use matrix_sdk_ui::eyeball_im::VectorDiff;
use matrix_sdk_ui::timeline::Timeline;

/// One backward page as the walk sees it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct PageOutcome {
    /// The SDK reports the start of the room's history.
    pub(crate) hit_start: bool,
    /// The page brought at least one event the timeline will show (hidden
    /// activity rows included), or what it brought is unknown. Either ends
    /// the walk.
    pub(crate) grew: bool,
}

/// The bound on one walk. Both apply; whichever is hit first ends it.
#[derive(Debug, Clone, Copy)]
pub(crate) struct WalkBudget {
    /// Pages per walk, the first included.
    pub(crate) max_pages: u32,
    /// Wall-clock time after which no further page is started.
    pub(crate) max_time: Duration,
}

/// Eight chunks of about twenty events: one near-top gesture
/// (`kMaxNearTopEmptyStrikes` = 12 pages) now covers ~1,900 filtered events
/// instead of ~240, enough for the run measured above. The time bound keeps a
/// walk that falls through to the network to roughly one second.
pub(crate) const FILTERED_RUN_WALK: WalkBudget = WalkBudget {
    max_pages: 8,
    max_time: Duration::from_millis(1000),
};

/// What a walk did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct WalkOutcome {
    pub(crate) hit_start: bool,
    /// Pages taken, the first included.
    pub(crate) pages: u32,
}

/// Run `page` once, then again while every page so far added nothing, did not
/// reach the start, the budget allows and `still_wanted()` (the room is still
/// the open one). An error on the FIRST page is returned as is; an error on a
/// later one ends the walk with what was already walked, since the walk is
/// only an optimisation and the next request meets the same error honestly.
pub(crate) async fn walk_filtered_run<F, Fut, E>(
    budget: WalkBudget,
    still_wanted: impl Fn() -> bool,
    mut page: F,
) -> Result<WalkOutcome, E>
where
    F: FnMut() -> Fut,
    Fut: Future<Output = Result<PageOutcome, E>>,
{
    let started = tokio::time::Instant::now();
    let first = page().await?;
    let mut pages = 1u32;
    let mut last = first;
    while !last.hit_start
        && !last.grew
        && pages < budget.max_pages
        && started.elapsed() < budget.max_time
        && still_wanted()
    {
        match page().await {
            Ok(outcome) => {
                last = outcome;
                pages += 1;
            }
            Err(_) => break,
        }
    }
    Ok(WalkOutcome { hit_start: last.hit_start, pages })
}

/// Whether a batch of event-cache diffs inserts anything `shown` accepts.
/// Only insertions count: a `Set` replaces an event already there (e.g. once
/// it decrypts) and removals bring nothing.
pub(crate) fn inserts_any<T: Clone>(diffs: &[VectorDiff<T>], shown: impl Fn(&T) -> bool) -> bool {
    diffs.iter().any(|diff| match diff {
        VectorDiff::Append { values } | VectorDiff::Reset { values } => {
            values.iter().any(|value| shown(value))
        }
        VectorDiff::PushFront { value }
        | VectorDiff::PushBack { value }
        | VectorDiff::Insert { value, .. } => shown(value),
        _ => false,
    })
}

/// Would the live timeline show this event (as any row, hidden ones
/// included)? An event that cannot be read is assumed shown, which only ends
/// a walk early.
fn event_is_shown(event: &TimelineEvent, rules: &RoomVersionRules) -> bool {
    match event.raw().deserialize() {
        Ok(parsed) => crate::timeline::shown_by_lightning_filter(&parsed, rules),
        Err(_) => true,
    }
}

/// Read every update already queued on `subscriber` and say whether any of
/// them brought an event the timeline will show. Never waits: a pagination's
/// update is sent before `paginate_backwards` returns.
fn drain_shows_something(
    subscriber: &mut RoomEventCacheSubscriber,
    rules: &RoomVersionRules,
) -> bool {
    use tokio::sync::broadcast::error::TryRecvError;
    let mut shown = false;
    loop {
        match subscriber.try_recv() {
            Ok(RoomEventCacheUpdate::UpdateTimelineEvents(update)) => {
                if inserts_any(&update.diffs, |event| event_is_shown(event, rules)) {
                    shown = true;
                }
            }
            Ok(_) => {}
            // Updates were dropped, so what this page brought is unknown.
            Err(TryRecvError::Lagged(_)) => shown = true,
            Err(TryRecvError::Empty) | Err(TryRecvError::Closed) => break,
        }
    }
    shown
}

/// One backward pagination request for the live timeline, walking any run
/// of history the filter empties (see the module comment). This is what
/// `TimelineRegistry::paginate_back` runs.
pub(crate) async fn walk_backwards(
    timeline: &Arc<Timeline>,
    count: u16,
    budget: WalkBudget,
    still_wanted: impl Fn() -> bool,
) -> Result<WalkOutcome, matrix_sdk_ui::timeline::Error> {
    let room = timeline.room();
    let rules = room.clone_info().room_version_rules_or_default();
    // Subscribed BEFORE the first page, and the drop handles kept for the
    // whole walk. Without a subscription nothing is known about a page, so
    // every page counts as productive and the walk is one page, as before it.
    let cache = room.event_cache().await.ok();
    let subscriber = match cache.as_ref() {
        Some((room_cache, _drop_handles)) => {
            room_cache.subscribe().await.ok().map(|(_events, subscriber)| subscriber)
        }
        None => None,
    };
    let subscriber = Arc::new(tokio::sync::Mutex::new(subscriber));
    walk_filtered_run(budget, still_wanted, || {
        let timeline = Arc::clone(timeline);
        let subscriber = Arc::clone(&subscriber);
        let rules = rules.clone();
        async move {
            let hit_start = timeline.paginate_backwards(count).await?;
            let mut guard = subscriber.lock().await;
            let grew = match guard.as_mut() {
                Some(subscriber) => drain_shows_something(subscriber, &rules),
                None => true,
            };
            Ok::<_, matrix_sdk_ui::timeline::Error>(PageOutcome { hit_start, grew })
        }
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::{walk_filtered_run, PageOutcome, WalkBudget, WalkOutcome};
    use std::cell::Cell;
    use std::time::Duration;

    const FILTERED: PageOutcome = PageOutcome { hit_start: false, grew: false };
    const GREW: PageOutcome = PageOutcome { hit_start: false, grew: true };
    const START: PageOutcome = PageOutcome { hit_start: true, grew: false };

    fn budget(max_pages: u32) -> WalkBudget {
        WalkBudget { max_pages, max_time: Duration::from_secs(60) }
    }

    /// Serves `script` one page per call; past its end every page is filtered.
    async fn walk(
        script: &[Result<PageOutcome, ()>],
        budget: WalkBudget,
        wanted: bool,
    ) -> (Result<WalkOutcome, ()>, usize) {
        let calls = Cell::new(0usize);
        let result = walk_filtered_run(budget, || wanted, || {
            let index = calls.get();
            calls.set(index + 1);
            let answer = script.get(index).copied().unwrap_or(Ok(FILTERED));
            async move { answer }
        })
        .await;
        (result, calls.get())
    }

    #[tokio::test]
    async fn a_filtered_run_is_walked_in_one_request_until_something_appears() {
        // Three chunks of pure churn, then a page with a message: one walk, and
        // the caller sees the page that grew.
        let (result, calls) =
            walk(&[Ok(FILTERED), Ok(FILTERED), Ok(FILTERED), Ok(GREW)], budget(8), true)
                .await;
        assert_eq!(calls, 4);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 4 }));
    }

    #[tokio::test]
    async fn a_page_that_adds_anything_is_never_followed_by_another() {
        // The ordinary case must cost exactly what it always did: one page.
        let (result, calls) = walk(&[Ok(GREW)], budget(8), true).await;
        assert_eq!(calls, 1);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[tokio::test]
    async fn the_start_of_history_ends_the_walk_and_is_reported() {
        let (result, calls) = walk(&[Ok(FILTERED), Ok(START)], budget(8), true).await;
        assert_eq!(calls, 2);
        assert_eq!(result, Ok(WalkOutcome { hit_start: true, pages: 2 }));
    }

    #[tokio::test]
    async fn an_endless_filtered_run_stops_at_the_page_bound() {
        let (result, calls) = walk(&[], budget(5), true).await;
        assert_eq!(calls, 5, "the walk ignored its page bound");
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 5 }));
    }

    #[tokio::test]
    async fn an_endless_filtered_run_stops_at_the_time_bound() {
        // Every page takes 30 ms; the walk may not START a page after 300 ms,
        // so at most eleven are taken (fewer on a loaded machine).
        let calls = Cell::new(0usize);
        let result = walk_filtered_run(
            WalkBudget { max_pages: 1000, max_time: Duration::from_millis(300) },
            || true,
            || {
                calls.set(calls.get() + 1);
                async {
                    tokio::time::sleep(Duration::from_millis(30)).await;
                    Ok::<_, ()>(FILTERED)
                }
            },
        )
        .await;
        assert!(result.is_ok());
        assert!(calls.get() >= 2, "the walk never walked");
        assert!(calls.get() <= 12, "the walk ignored its time bound: {} pages", calls.get());
    }

    #[tokio::test]
    async fn a_closed_room_is_not_walked_any_further() {
        let (result, calls) = walk(&[], budget(8), false).await;
        assert_eq!(calls, 1, "a page was walked for a room nobody has open");
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[tokio::test]
    async fn a_first_page_error_is_reported_and_a_later_one_ends_the_walk_quietly() {
        let (result, calls) = walk(&[Err(())], budget(8), true).await;
        assert_eq!(calls, 1);
        assert_eq!(result, Err(()));

        let (result, calls) = walk(&[Ok(FILTERED), Err(())], budget(8), true).await;
        assert_eq!(calls, 2);
        assert_eq!(result, Ok(WalkOutcome { hit_start: false, pages: 1 }));
    }

    #[test]
    fn only_insertions_of_a_shown_item_count_as_growth() {
        use super::inserts_any;
        use matrix_sdk_ui::eyeball_im::VectorDiff;
        let shown = |value: &u32| *value >= 100; // >= 100 stands for "shown"

        assert!(!inserts_any::<u32>(&[], shown));
        // A page of filtered events only.
        assert!(!inserts_any(&[VectorDiff::Append { values: vec![1, 2, 3].into() }], shown));
        // One shown event anywhere in the batch, in any insertion shape.
        assert!(inserts_any(&[VectorDiff::Append { values: vec![1, 100].into() }], shown));
        assert!(inserts_any(&[VectorDiff::PushFront { value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::PushBack { value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::Insert { index: 0, value: 100 }], shown));
        assert!(inserts_any(&[VectorDiff::Reset { values: vec![100].into() }], shown));
        // Replacing or removing what is already there brings nothing.
        assert!(!inserts_any(&[VectorDiff::Set { index: 0, value: 100 }], shown));
        assert!(!inserts_any(
            &[VectorDiff::Remove { index: 0 }, VectorDiff::Clear, VectorDiff::PopFront],
            shown,
        ));
    }

    /// The walk is wired into the bridge's pagination, not just written. A
    /// revert of the `paginate_back` call site to a single
    /// `paginate_backwards` fails here.
    #[test]
    fn paginate_back_runs_the_walk() {
        let source = include_str!("timeline.rs");
        let start = source.find("pub fn paginate_back(").expect("paginate_back exists");
        let end = source[start..]
            .find("pub fn send_text(")
            .map(|offset| start + offset)
            .expect("paginate_back is followed by send_text");
        let body = &source[start..end];
        assert!(body.contains("crate::pagewalk::walk_backwards("),
                "paginate_back no longer walks filtered history");
        assert!(body.contains("crate::pagewalk::FILTERED_RUN_WALK"),
                "paginate_back walks without the documented bound");
        assert!(!body.contains(".paginate_backwards("),
                "paginate_back pages the timeline directly again");
    }
}

/// The walk against a real matrix-sdk timeline and event cache, over a
/// loopback homeserver: two pages of MatrixRTC membership churn, then a real
/// message, then more history. The walk must cross the churn in ONE request
/// and stop at the message, which is exactly what a check on the timeline's
/// item count got wrong (the timeline applies a page on its own task, after
/// `paginate_backwards` has returned).
#[cfg(test)]
mod sdk_tests {
    use super::{walk_backwards, WalkBudget};
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        config::SyncSettings,
        ruma::{OwnedDeviceId, OwnedRoomId, OwnedUserId},
        store::RoomLoadSettings,
        SessionMeta, SessionTokens,
    };
    use matrix_sdk_ui::timeline::{TimelineBuilder, TimelineFocus};
    use serde_json::{json, Value};

    const ROOM: &str = "!churn:example.org";
    const ME: &str = "@me:example.org";

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
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\n\
             Connection: close\r\nContent-Length: {}\r\n\r\n{}",
            payload.len(),
            payload
        );
        let _ = stream.write_all(response.as_bytes());
        let _ = stream.flush();
    }

    fn rtc(n: u64, ts: u64) -> Value {
        json!({
            "type": "org.matrix.msc3401.call.member",
            "state_key": format!("_@caller:example.org_DEV{n}"),
            "sender": "@caller:example.org",
            "event_id": format!("$rtc{n}"),
            "origin_server_ts": ts,
            "room_id": ROOM,
            "content": {},
        })
    }

    fn message(id: &str, ts: u64) -> Value {
        json!({
            "type": "m.room.message",
            "sender": "@caller:example.org",
            "event_id": id,
            "origin_server_ts": ts,
            "room_id": ROOM,
            "content": { "msgtype": "m.text", "body": "a real message" },
        })
    }

    /// Twenty churn events, newest first, numbered from `first`.
    fn churn_page(first: u64, newest_ts: u64) -> Vec<Value> {
        (0..20).map(|i| rtc(first + i, newest_ts - i)).collect()
    }

    fn sync_body() -> String {
        let room = json!({
                "state": { "events": [
                    {
                        "type": "m.room.create", "state_key": "", "sender": ME,
                        "event_id": "$create", "origin_server_ts": 1_000,
                        "content": { "creator": ME, "room_version": "10" },
                    },
                    {
                        "type": "m.room.member", "state_key": ME, "sender": ME,
                        "event_id": "$me", "origin_server_ts": 1_001,
                        "content": { "membership": "join" },
                    },
                ]},
                "timeline": {
                    "events": [rtc(0, 10_000_000)],
                    "limited": true,
                    "prev_batch": "t1",
                },
        });
        let mut join = serde_json::Map::new();
        join.insert(ROOM.to_owned(), room);
        json!({ "next_batch": "s1", "rooms": { "join": join } }).to_string()
    }

    /// `/messages` by `from` token: t1 and t2 are churn, t3 is the message, t4
    /// is more history the walk must NOT reach, t5 is the start.
    fn messages_body(from: &str) -> String {
        let (chunk, end): (Vec<Value>, Option<&str>) = match from {
            "t1" => (churn_page(1, 9_000_000), Some("t2")),
            "t2" => (churn_page(21, 8_000_000), Some("t3")),
            "t3" => (vec![message("$hello", 7_000_000)], Some("t4")),
            "t4" => (vec![message("$older", 6_000_000)], Some("t5")),
            _ => (Vec::new(), None),
        };
        let mut body = json!({ "start": from, "chunk": chunk, "state": [] });
        if let Some(end) = end {
            body["end"] = json!(end);
        }
        body.to_string()
    }

    fn query_value<'a>(target: &'a str, key: &str) -> Option<&'a str> {
        target.split_once('?')?.1.split('&').find_map(|pair| {
            pair.split_once('=').filter(|(k, _)| *k == key).map(|(_, v)| v)
        })
    }

    /// The homeserver. Records every `/messages` token it was asked for.
    fn serve(tokens: Arc<Mutex<Vec<String>>>) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let (ready_tx, ready_rx) = mpsc::channel();
        thread::spawn(move || {
            let _ = ready_tx.send(());
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { break };
                let tokens = Arc::clone(&tokens);
                thread::spawn(move || {
                    let line = read_request(&mut stream);
                    let mut parts = line.split_whitespace();
                    let method = parts.next().unwrap_or("").to_owned();
                    let target = parts.next().unwrap_or("").to_owned();
                    let path = target.split('?').next().unwrap_or("").to_owned();
                    if path == "/_matrix/client/versions" {
                        respond(&mut stream, "200 OK",
                                r#"{"versions":["v1.1","v1.11"],"unstable_features":{}}"#);
                    } else if path.ends_with("/sync") {
                        respond(&mut stream, "200 OK", &sync_body());
                    } else if path.ends_with("/messages") {
                        let from = query_value(&target, "from").unwrap_or("").to_owned();
                        tokens.lock().unwrap().push(from.clone());
                        respond(&mut stream, "200 OK", &messages_body(&from));
                    } else if method == "POST" && path.ends_with("/keys/upload") {
                        respond(&mut stream, "200 OK",
                                r#"{"one_time_key_counts":{"signed_curve25519":50}}"#);
                    } else if method == "POST" && path.ends_with("/keys/query") {
                        respond(&mut stream, "200 OK", r#"{"device_keys":{},"failures":{}}"#);
                    } else {
                        respond(&mut stream, "404 Not Found",
                                r#"{"errcode":"M_UNRECOGNIZED","error":"Unrecognized request"}"#);
                    }
                });
            }
        });
        ready_rx.recv().expect("listener thread started");
        format!("{}:{}", addr.ip(), addr.port())
    }

    #[test]
    fn a_churn_run_is_crossed_in_one_request_and_the_walk_stops_at_the_message() {
        let tokens: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let addr = serve(Arc::clone(&tokens));
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("runtime");
        let outcome = runtime.block_on(async {
            let client = matrix_sdk::Client::builder()
                .homeserver_url(format!("http://{addr}"))
                .build()
                .await
                .expect("client");
            client
                .matrix_auth()
                .restore_session(
                    MatrixSession {
                        meta: SessionMeta {
                            user_id: OwnedUserId::try_from(ME).expect("user id"),
                            device_id: OwnedDeviceId::from("TESTDEVICE"),
                        },
                        tokens: SessionTokens {
                            access_token: "test-token".to_owned(),
                            refresh_token: None,
                        },
                    },
                    RoomLoadSettings::default(),
                )
                .await
                .expect("restore");
            client.event_cache().subscribe().expect("event cache");
            client.sync_once(SyncSettings::default()).await.expect("sync");
            let room_id = OwnedRoomId::try_from(ROOM).expect("room id");
            let room = client.get_room(&room_id).expect("the synced room");
            let timeline = Arc::new(
                TimelineBuilder::new(&room)
                    .event_filter(crate::timeline::lightning_event_filter)
                    .with_focus(TimelineFocus::Live { hide_threaded_events: true })
                    .build()
                    .await
                    .expect("timeline"),
            );
            walk_backwards(
                &timeline,
                20,
                WalkBudget { max_pages: 8, max_time: Duration::from_secs(20) },
                || true,
            )
            .await
            .expect("walk")
        });

        let asked = tokens.lock().unwrap().clone();
        // Both churn pages and the message page, in this one request.
        assert!(asked.iter().any(|t| t == "t1"), "{asked:?}");
        assert!(asked.iter().any(|t| t == "t2"), "{asked:?}");
        assert!(asked.iter().any(|t| t == "t3"), "the walk stopped inside the churn: {asked:?}");
        // And not one page past the first message: rows per request are what
        // one ordinary page gives.
        assert!(!asked.iter().any(|t| t == "t4"),
                "the walk went past a page that brought a message: {asked:?}");
        assert!(!outcome.hit_start, "{outcome:?}");
        assert!(outcome.pages >= 3, "{outcome:?}");
    }
}
