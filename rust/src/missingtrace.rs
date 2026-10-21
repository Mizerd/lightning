//! Opt-in (`LIGHTNING_TIMELINE_TRACE=1`): which messages the homeserver has
//! that the open room's SDK timeline does not hold, and why.
//!
//! GitHub #30/#48: a plain `m.text` that Element showed was absent here, and
//! nothing in any log could say whether the timeline FILTER dropped it, the
//! SDK hid it as a thread reply, or it never reached the room's event cache
//! at all (a gap the cache does not know about, which nothing ever fills:
//! `subscribe_event_cache_before_classic_sync` in lib.rs closes one way that
//! happens). Asking a reporter for an event's source answers one message; this
//! answers every message in view, from a log they can send.
//!
//! A few seconds after a room opens, the newest history is read straight from
//! the server (`/messages`, backwards, bounded, without retries, NOT stored
//! anywhere) and compared with the event ids of the SDK timeline's items.
//! Only an event BETWEEN two items counts as missing: anything older than the
//! oldest loaded item has simply not been paginated yet, and anything newer
//! than the newest has not synced yet.
//!
//! What it measures is the SDK TIMELINE (`sdk_items=`,
//! `missing_from_sdk_timeline=`), not the rows QML paints: a row C++ or QML
//! later hides is not seen here. Membership in the cache is judged against the
//! room's LINKED CHUNK (what timelines are built from), never the store-wide
//! event table, which also holds events fetched out of band (a reply target, a
//! pin) and would hide a real gap; such an event is flagged `out_of_band=true`.
//! When the chunk holds the event, its DECRYPTED form decides the type and
//! relation, so an encrypted reaction or poll vote is not mistaken for a lost
//! message.
//!
//! What a line carries: the room and event ids as short hashes
//! (`redact_matrix_id`, as the C++ logs print them), the event type, a
//! well-formed msgtype and relation type, the minute it was sent, and the
//! reason. Never a body, a sender or a formatted body.
//!
//! Lifetime: the task holds only a `Weak` timeline while it waits, and every
//! request is bounded and dropped as soon as the room is closed or the account
//! signs out, so it never keeps a client (and its stores) alive past either.

use std::collections::{HashMap, HashSet};
use std::future::{Future, IntoFuture};
use std::sync::Weak;
use std::time::Duration;

use matrix_sdk::config::RequestConfig;
use matrix_sdk::ruma::{
    api::client::message::get_message_events, events::AnySyncTimelineEvent,
    room_version_rules::RoomVersionRules, serde::Raw, EventId,
};
use matrix_sdk_ui::timeline::Timeline;
use serde_json::json;
use serde_json::value::RawValue;

use crate::EventQueueRef;

/// Pages of `/messages` one check may read, and their size: enough to cover a
/// screenful of rows even in a room full of reactions and state, small enough
/// to be one second of a reporter's bandwidth.
const MAX_PAGES: u32 = 6;
const PAGE_SIZE: u32 = 50;
/// Per-event lines per check; the summary counts the rest.
const MAX_LINES: usize = 20;
/// Lets the open's first back-paginations land before the timeline is read.
const SETTLE: Duration = Duration::from_secs(4);
/// One request's ceiling. Retries are off, so this is the whole cost.
const REQUEST_BUDGET: Duration = Duration::from_secs(10);
/// How often a request in flight looks whether the room is still open.
const CURRENT_POLL: Duration = Duration::from_millis(200);

/// LIGHTNING_TIMELINE_TRACE, read once per process, with the same "set and not
/// 0/false/off/no" rule as the other traces.
pub(crate) fn enabled() -> bool {
    use std::sync::OnceLock;
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        crate::switchtrace::switch_trace_enabled_from(
            std::env::var("LIGHTNING_TIMELINE_TRACE").ok().as_deref(),
        )
    })
}

/// One event as the check sees it: from the server, or (decrypted) from the
/// room's linked chunk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ServerEvent {
    pub(crate) event_id: String,
    pub(crate) event_type: String,
    pub(crate) msgtype: Option<String>,
    pub(crate) rel_type: Option<String>,
    pub(crate) ts_ms: u64,
    /// `shown_by_lightning_filter`'s verdict; `None` when the event does not
    /// deserialize (the SDK then makes a "failed to parse" item of it).
    pub(crate) filter_shown: Option<bool>,
}

/// Why an event between two timeline items is not an item itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) enum Missing {
    /// The filter would show it and the room's linked chunk does not hold
    /// it: a gap the cache does not know about. The #30/#48 suspect. Listed
    /// first, so the line cap can never hide it behind expected ones.
    NotInRoomChunk,
    /// The chunk holds it and the filter would show it, yet the timeline does
    /// not: an SDK timeline fault.
    InChunkNotShown,
    /// The SDK could not deserialize it; it should have been a
    /// "[unsupported event]" item.
    Unparseable,
    /// `lightning_event_filter` drops it (redactions, call membership, call
    /// candidates, unknown state...). Expected unless it is a message.
    Filtered,
    /// An `m.thread` reply: the main timeline hides it on purpose (§8).
    ThreadReply,
    /// An `m.replace`: folded into its target, never an item. Expected.
    Edit,
    /// An `m.annotation` / `m.reference` (reactions, poll votes, beacons,
    /// call declines): aggregated onto its target, never an item. Expected.
    Aggregated,
}

impl Missing {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Missing::NotInRoomChunk => "not_in_room_chunk",
            Missing::InChunkNotShown => "in_chunk_not_shown",
            Missing::Unparseable => "unparseable",
            Missing::Filtered => "filtered",
            Missing::ThreadReply => "thread_reply",
            Missing::Edit => "edit",
            Missing::Aggregated => "aggregated",
        }
    }

    /// Whether a missing event with this reason earns its own line (subject
    /// to `is_message_like` for the expected kinds).
    fn always_reported(self) -> bool {
        matches!(self, Missing::NotInRoomChunk | Missing::InChunkNotShown | Missing::Unparseable)
    }
}

/// Indices (into `newest_first`) of the events that are not items although an
/// event NEWER and an event OLDER than them both are.
pub(crate) fn sandwiched_missing(newest_first: &[ServerEvent], shown: &HashSet<String>) -> Vec<usize> {
    let is_shown: Vec<bool> = newest_first.iter().map(|e| shown.contains(&e.event_id)).collect();
    let Some(newest_shown) = is_shown.iter().position(|s| *s) else { return Vec::new() };
    let Some(oldest_shown) = is_shown.iter().rposition(|s| *s) else { return Vec::new() };
    (newest_shown + 1..oldest_shown).filter(|&i| !is_shown[i]).collect()
}

/// The reason for one missing event. `in_chunk` is the cached copy from the
/// room's linked chunk when it holds the event: decrypted, so it decides the
/// type and relation an encrypted server event hides.
pub(crate) fn classify(server: &ServerEvent, in_chunk: Option<&ServerEvent>) -> Missing {
    let view = in_chunk.unwrap_or(server);
    // An encrypted event keeps `m.relates_to` in the clear, so the server copy
    // already knows the relation even when the chunk does not hold it.
    let rel = view.rel_type.as_deref().or(server.rel_type.as_deref());
    match rel {
        Some("m.replace") => return Missing::Edit,
        Some("m.annotation" | "m.reference") => return Missing::Aggregated,
        Some("m.thread") => return Missing::ThreadReply,
        _ => {}
    }
    match view.filter_shown {
        None => Missing::Unparseable,
        Some(false) => Missing::Filtered,
        Some(true) if in_chunk.is_none() => Missing::NotInRoomChunk,
        Some(true) => Missing::InChunkNotShown,
    }
}

/// Whether a reader would call this event "a message", so an expected reason
/// still earns its own line rather than only a count.
pub(crate) fn is_message_like(event_type: &str) -> bool {
    matches!(
        event_type,
        "m.room.message"
            | "m.room.encrypted"
            | "m.sticker"
            | "m.poll.start"
            | "org.matrix.msc3381.poll.start"
    )
}

/// The lines to print, most important first, at most `cap`: every gap and
/// fault before any expected reason, whatever the walk order.
pub(crate) fn lines_to_report(
    missing: &[(usize, Missing, &ServerEvent)],
    cap: usize,
) -> Vec<usize> {
    let mut chosen: Vec<(Missing, usize)> = missing
        .iter()
        .filter(|(_, reason, view)| reason.always_reported() || is_message_like(&view.event_type))
        .map(|(index, reason, _)| (*reason, *index))
        .collect();
    // Stable: within one reason, newest first as walked.
    chosen.sort_by_key(|(reason, _)| *reason);
    chosen.into_iter().take(cap).map(|(_, index)| index).collect()
}

/// A sender-chosen identifier (msgtype, rel_type, type) is logged only when it
/// looks like one; anything else is "other".
fn bounded_identifier(value: &str) -> &str {
    let ok = !value.is_empty()
        && value.len() <= 64
        && value.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'));
    if ok {
        value
    } else {
        "other"
    }
}

/// Read one event (server or cached, encrypted or not) without its body.
pub(crate) fn event_from_json(json: &RawValue, rules: &RoomVersionRules) -> Option<ServerEvent> {
    let raw = Raw::<AnySyncTimelineEvent>::from_json(json.to_owned());
    let event_id: String = raw.get_field("event_id").ok().flatten()?;
    let event_type: String = raw.get_field("type").ok().flatten().unwrap_or_default();
    let ts_ms: u64 = raw.get_field("origin_server_ts").ok().flatten().unwrap_or(0);
    let content: Option<serde_json::Value> = raw.get_field("content").ok().flatten();
    let pointer = |path: &str| {
        content
            .as_ref()
            .and_then(|c| c.pointer(path))
            .and_then(|v| v.as_str())
            .map(str::to_owned)
    };
    let msgtype = pointer("/msgtype");
    let rel_type = pointer("/m.relates_to/rel_type");
    let filter_shown = raw
        .deserialize()
        .ok()
        .map(|event| crate::timeline::shown_by_lightning_filter(&event, rules));
    Some(ServerEvent { event_id, event_type, msgtype, rel_type, ts_ms, filter_shown })
}

/// `YYYY-MM-DDTHH:MMZ` from milliseconds since the epoch, without a date crate
/// (civil-from-days, Howard Hinnant's algorithm).
fn utc_minute(ts_ms: u64) -> String {
    let secs = (ts_ms / 1000) as i64;
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let (hour, minute) = (rem / 3600, (rem % 3600) / 60);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}Z")
}

fn trace(events: &EventQueueRef, line: String) {
    crate::enqueue(events, json!({ "type": "rust_trace", "line": line }));
}

/// Run `work` under `budget`, but give up as soon as `still_current` says the
/// room was closed (or the account signed out). `None` for either.
async fn bounded<T>(
    still_current: &impl Fn() -> bool,
    budget: Duration,
    work: impl Future<Output = T>,
) -> Option<T> {
    let gone = async {
        loop {
            tokio::time::sleep(CURRENT_POLL).await;
            if !still_current() {
                return;
            }
        }
    };
    tokio::select! {
        result = tokio::time::timeout(budget, work) => result.ok(),
        _ = gone => None,
    }
}

/// The check, once per room open. `still_current` ends it, and drops anything
/// in flight, when the user has moved on (another room, sign-out).
pub(crate) async fn check_open_room(
    timeline: Weak<Timeline>,
    events: EventQueueRef,
    still_current: impl Fn() -> bool,
) {
    tokio::time::sleep(SETTLE).await;
    if !still_current() {
        return;
    }
    // The timeline is borrowed only to read its items and its room; the room
    // (and with it the client) is held only while a bounded request runs.
    let (room, shown_in_order) = {
        let Some(timeline) = timeline.upgrade() else { return };
        // Oldest first, as the timeline holds them.
        let shown: Vec<String> = timeline
            .items()
            .await
            .iter()
            .filter_map(|item| item.as_event()?.event_id().map(|id| id.to_string()))
            .collect();
        (timeline.room().clone(), shown)
    };
    let room_hash = crate::redact_matrix_id(room.room_id().as_str());
    let rules = room.clone_info().room_version_rules_or_default();
    let Some(oldest_shown) = shown_in_order.first().cloned() else {
        trace(&events, format!("timeline-trace room={room_hash} sdk_items=0 (nothing to compare)"));
        return;
    };
    let shown: HashSet<String> = shown_in_order.into_iter().collect();

    let mut newest_first: Vec<ServerEvent> = Vec::new();
    let mut from: Option<String> = None;
    let mut pages = 0u32;
    let mut reached_oldest_shown = false;
    let mut outcome = "ok";
    while pages < MAX_PAGES {
        let mut request = get_message_events::v3::Request::backward(room.room_id().to_owned());
        request.from = from.clone();
        request.limit = PAGE_SIZE.into();
        let send = room
            .client()
            .send(request)
            .with_request_config(RequestConfig::new().disable_retry().timeout(REQUEST_BUDGET))
            .into_future();
        let response = match bounded(&still_current, REQUEST_BUDGET, send).await {
            Some(Ok(response)) => response,
            Some(Err(_)) => {
                outcome = "request_failed";
                break;
            }
            None if still_current() => {
                outcome = "request_timed_out";
                break;
            }
            None => return,
        };
        pages += 1;
        for raw in &response.chunk {
            if let Some(event) = event_from_json(raw.json(), &rules) {
                if event.event_id == oldest_shown {
                    reached_oldest_shown = true;
                }
                newest_first.push(event);
            }
        }
        match response.end {
            Some(end) if !reached_oldest_shown && !response.chunk.is_empty() => from = Some(end),
            _ => break,
        }
    }
    if !still_current() {
        return;
    }

    let missing = sandwiched_missing(&newest_first, &shown);
    // The room's linked chunk (in memory: everything between two timeline
    // items is loaded), decrypted; and, separately, whether the store-wide
    // event table knows an event the chunk lacks (fetched out of band).
    let cached = bounded(&still_current, REQUEST_BUDGET, async {
        let (cache, _handles) = room.event_cache().await.ok()?;
        let chunk: HashMap<String, ServerEvent> = cache
            .events()
            .await
            .ok()?
            .iter()
            .filter_map(|event| event_from_json(event.raw().json(), &rules))
            .map(|event| (event.event_id.clone(), event))
            .collect();
        let mut out_of_band: HashSet<String> = HashSet::new();
        for &index in &missing {
            let id = &newest_first[index].event_id;
            if chunk.contains_key(id) {
                continue;
            }
            if let Ok(parsed) = EventId::parse(id) {
                if cache.find_event(&parsed).await.ok().flatten().is_some() {
                    out_of_band.insert(id.clone());
                }
            }
        }
        Some((chunk, out_of_band))
    })
    .await;
    let Some(Some((chunk, out_of_band))) = cached else {
        if still_current() {
            trace(&events, format!("timeline-trace room={room_hash} outcome=no_event_cache"));
        }
        return;
    };
    drop(room);

    let classified: Vec<(usize, Missing, &ServerEvent)> = missing
        .iter()
        .map(|&index| {
            let server = &newest_first[index];
            let cached = chunk.get(&server.event_id);
            (index, classify(server, cached), cached.unwrap_or(server))
        })
        .collect();
    let mut counts: std::collections::BTreeMap<&'static str, usize> = Default::default();
    for (_, reason, _) in &classified {
        *counts.entry(reason.as_str()).or_default() += 1;
    }
    for index in lines_to_report(&classified, MAX_LINES) {
        let Some((_, reason, view)) = classified.iter().find(|(i, _, _)| *i == index) else {
            continue;
        };
        let server = &newest_first[index];
        trace(
            &events,
            format!(
                "timeline-trace room={room_hash} missing_from_sdk_timeline event={} type={} \
                 msgtype={} rel={} sent={} in_room_chunk={} out_of_band={} filter={} reason={}",
                crate::redact_matrix_id(&server.event_id),
                bounded_identifier(&view.event_type),
                view.msgtype.as_deref().map_or("none", bounded_identifier),
                view.rel_type
                    .as_deref()
                    .or(server.rel_type.as_deref())
                    .map_or("none", bounded_identifier),
                utc_minute(server.ts_ms),
                chunk.contains_key(&server.event_id),
                out_of_band.contains(&server.event_id),
                match view.filter_shown {
                    Some(true) => "shown",
                    Some(false) => "dropped",
                    None => "unparseable",
                },
                reason.as_str(),
            ),
        );
    }
    let counted: Vec<String> = counts.iter().map(|(k, v)| format!("{k}={v}")).collect();
    trace(
        &events,
        format!(
            "timeline-trace room={room_hash} outcome={outcome} pages={pages} server_events={} \
             sdk_items={} reached_oldest_item={reached_oldest_shown} \
             missing_from_sdk_timeline={} {}",
            newest_first.len(),
            shown.len(),
            missing.len(),
            counted.join(" "),
        ),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(id: &str) -> ServerEvent {
        ServerEvent {
            event_id: id.to_owned(),
            event_type: "m.room.message".to_owned(),
            msgtype: Some("m.text".to_owned()),
            rel_type: None,
            ts_ms: 0,
            filter_shown: Some(true),
        }
    }

    fn set(ids: &[&str]) -> HashSet<String> {
        ids.iter().map(|s| (*s).to_owned()).collect()
    }

    fn parse(value: serde_json::Value) -> ServerEvent {
        let json = RawValue::from_string(value.to_string()).unwrap();
        event_from_json(&json, &RoomVersionRules::V11).expect("parsed")
    }

    #[test]
    fn only_an_event_between_two_shown_items_counts_as_missing() {
        // Newest first: e5 not synced yet, e4 shown, e3 missing, e2 shown,
        // e1 older than anything loaded.
        let walk = vec![ev("e5"), ev("e4"), ev("e3"), ev("e2"), ev("e1")];
        assert_eq!(sandwiched_missing(&walk, &set(&["e4", "e2"])), vec![2]);
        // Nothing shown, or one item: nothing can be between.
        assert!(sandwiched_missing(&walk, &set(&[])).is_empty());
        assert!(sandwiched_missing(&walk, &set(&["e3"])).is_empty());
    }

    #[test]
    fn the_reason_separates_the_filter_a_thread_aggregation_and_a_gap() {
        let mut thread = ev("t");
        thread.rel_type = Some("m.thread".to_owned());
        assert_eq!(classify(&thread, Some(&thread)), Missing::ThreadReply);
        let mut edit = ev("e");
        edit.rel_type = Some("m.replace".to_owned());
        assert_eq!(classify(&edit, None), Missing::Edit);
        let mut vote = ev("v");
        vote.rel_type = Some("m.reference".to_owned());
        assert_eq!(classify(&vote, None), Missing::Aggregated);
        let mut redaction = ev("r");
        redaction.filter_shown = Some(false);
        assert_eq!(classify(&redaction, Some(&redaction)), Missing::Filtered);
        let mut broken = ev("b");
        broken.filter_shown = None;
        assert_eq!(classify(&broken, Some(&broken)), Missing::Unparseable);
        assert_eq!(classify(&ev("m"), None), Missing::NotInRoomChunk);
        assert_eq!(classify(&ev("m"), Some(&ev("m"))), Missing::InChunkNotShown);
    }

    /// Review MEDIUM-2: `/messages` returns an encrypted reaction as a plain
    /// `m.room.encrypted`, which the filter keeps. Judged by the server copy
    /// alone it read as a lost message; the chunk's decrypted copy says what
    /// it really is. And a call candidate with no relation is filtered once
    /// decrypted.
    #[test]
    fn an_encrypted_reaction_or_call_event_is_judged_by_its_decrypted_copy() {
        let encrypted = |id: &str| {
            parse(json!({
                "type": "m.room.encrypted",
                "event_id": id,
                "sender": "@flo:example.org",
                "origin_server_ts": 1u64,
                "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "AwgA",
                             "sender_key": "k", "session_id": "s", "device_id": "D" },
            }))
        };
        let server = encrypted("$react:example.org");
        assert_eq!(server.filter_shown, Some(true), "the filter keeps every encrypted event");
        let decrypted = parse(json!({
            "type": "m.reaction",
            "event_id": "$react:example.org",
            "sender": "@flo:example.org",
            "origin_server_ts": 1u64,
            "content": { "m.relates_to": {
                "rel_type": "m.annotation", "event_id": "$a:example.org", "key": "x" } },
        }));
        assert_eq!(classify(&server, Some(&decrypted)), Missing::Aggregated);

        let candidates_server = encrypted("$cand:example.org");
        let candidates = parse(json!({
            "type": "m.call.candidates",
            "event_id": "$cand:example.org",
            "sender": "@flo:example.org",
            "origin_server_ts": 1u64,
            "content": { "call_id": "c", "version": "1", "party_id": "p", "candidates": [] },
        }));
        assert_eq!(candidates.filter_shown, Some(false));
        let reason = classify(&candidates_server, Some(&candidates));
        assert_eq!(reason, Missing::Filtered);
        // Not a message, so no line of its own: only the count.
        let list = vec![(0usize, reason, &candidates)];
        assert!(lines_to_report(&list, 20).is_empty());
    }

    #[test]
    fn a_gap_is_reported_before_any_expected_reason_however_many() {
        let message = ev("m");
        let mut rows: Vec<(usize, Missing, &ServerEvent)> =
            (0..30).map(|i| (i, Missing::ThreadReply, &message)).collect();
        rows.push((30, Missing::NotInRoomChunk, &message));
        let lines = lines_to_report(&rows, 20);
        assert_eq!(lines.len(), 20);
        assert_eq!(lines[0], 30, "the gap must not be crowded out by thread replies");
    }

    #[test]
    fn a_server_event_is_read_without_its_body_and_judged_by_the_real_filter() {
        let event = parse(json!({
            "type": "m.room.message",
            "event_id": "$a:example.org",
            "room_id": "!r:example.org",
            "sender": "@flo:example.org",
            "origin_server_ts": 1_760_000_000_000u64,
            "content": { "msgtype": "m.text", "body": "secret words" },
        }));
        assert_eq!(event.event_id, "$a:example.org");
        assert_eq!(event.msgtype.as_deref(), Some("m.text"));
        assert_eq!(event.filter_shown, Some(true));
        assert!(!format!("{event:?}").contains("secret"));

        let reaction = parse(json!({
            "type": "m.reaction",
            "event_id": "$b:example.org",
            "sender": "@flo:example.org",
            "origin_server_ts": 1u64,
            "content": { "m.relates_to": {
                "rel_type": "m.annotation", "event_id": "$a:example.org", "key": "x" } },
        }));
        assert_eq!(reaction.rel_type.as_deref(), Some("m.annotation"));
        assert_eq!(reaction.filter_shown, Some(false));
    }

    #[test]
    fn identifiers_are_bounded_and_times_are_minutes() {
        assert_eq!(bounded_identifier("m.text"), "m.text");
        assert_eq!(bounded_identifier("com.example.custom_type-2"), "com.example.custom_type-2");
        assert_eq!(bounded_identifier("hello world"), "other");
        assert_eq!(bounded_identifier(&"x".repeat(65)), "other");
        assert_eq!(utc_minute(0), "1970-01-01T00:00Z");
        assert_eq!(utc_minute(1_760_000_000_000), "2025-10-09T08:53Z");
    }

    /// Review MEDIUM-3: a request in flight is abandoned when the room closes,
    /// not run out to its budget.
    #[tokio::test]
    async fn a_request_in_flight_is_dropped_when_the_room_closes() {
        let open = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true));
        let flag = std::sync::Arc::clone(&open);
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(300)).await;
            flag.store(false, std::sync::atomic::Ordering::SeqCst);
        });
        let started = tokio::time::Instant::now();
        let still = || open.load(std::sync::atomic::Ordering::SeqCst);
        let result = bounded(&still, Duration::from_secs(30), std::future::pending::<()>()).await;
        assert!(result.is_none());
        assert!(started.elapsed() < Duration::from_secs(3), "{:?}", started.elapsed());
    }
}
