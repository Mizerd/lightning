//! Pinned messages: read, resolve and write `m.room.pinned_events`.
//!
//! The list is the state event itself, read via `Room::pinned_event_ids()`
//! (with `Room::load_pinned_events()` as the `/state` fallback) and written
//! via `Room::pin_event()` / `unpin_event()`, which do the read-modify-send.
//!
//! Pinned events are usually not in the loaded timeline, so each id is
//! resolved with `Room::load_or_fetch_event()` (cache-first, one bounded
//! request on a miss; the SDK decrypts as usual). At most
//! `PINNED_RESOLVE_CAP` ids are resolved, sequentially, with a short
//! no-retry timeout; longer lists report `truncated`.
//!
//! Bodies cross for the preview but stay in memory on the C++ side and are
//! never written to `CacheStore`. Nothing here logs ids, senders or bodies.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        events::{
            AnySyncMessageLikeEvent, AnySyncTimelineEvent, StateEventType,
            SyncMessageLikeEvent,
        },
        EventId, OwnedEventId, OwnedRoomId, RoomId,
    },
};
use serde_json::json;

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

/// Max pinned events resolved per snapshot. The most recent are kept (pins
/// are appended).
const PINNED_RESOLVE_CAP: usize = 32;

/// Per-event bound, no retries: refreshes are disposable and the room-action
/// pool is joined at sign-out.
const PINNED_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// A preview is a label, not a message view.
const PREVIEW_MAX_CHARS: usize = 140;

/// One-line preview: no control characters, bounded; newlines collapse here
/// so a single-line label never has to clip them silently.
fn one_line(body: &str) -> String {
    let collapsed: String = body
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let trimmed = collapsed.split_whitespace().collect::<Vec<_>>().join(" ");
    trimmed.chars().take(PREVIEW_MAX_CHARS).collect()
}

/// Classify a resolved pinned event into (kind, preview). `preview` is empty
/// for kinds where text is not useful, never a placeholder.
fn describe(parsed: &AnySyncTimelineEvent) -> (&'static str, String) {
    use matrix_sdk::ruma::events::room::message::MessageType;

    match parsed {
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
            SyncMessageLikeEvent::Original(message),
        )) => match &message.content.msgtype {
            MessageType::Text(c) => ("text", one_line(&c.body)),
            MessageType::Notice(c) => ("notice", one_line(&c.body)),
            MessageType::Emote(c) => ("emote", one_line(&c.body)),
            // For media the body is the filename or caption, the most useful label.
            MessageType::Image(c) => ("image", one_line(&c.body)),
            MessageType::Video(c) => ("video", one_line(&c.body)),
            MessageType::Audio(c) => ("audio", one_line(&c.body)),
            MessageType::File(c) => ("file", one_line(&c.body)),
            MessageType::Location(c) => ("location", one_line(&c.body)),
            // An MSC4274 gallery pins as what it holds, with its caption, never Sable's
            // generated `[name: mxc://…]` body.
            other => match crate::timeline::parse_gallery(other)
                .filter(|g| !g.items.is_empty())
            {
                Some(g) => (
                    if g.all_images() { "image" } else { "file" },
                    one_line(&g.caption),
                ),
                None => ("other", String::new()),
            },
        },
        // Redacted is an answer, not a failure: the pin still exists.
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
            SyncMessageLikeEvent::Redacted(_),
        )) => ("redacted", String::new()),
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(
            SyncMessageLikeEvent::Original(sticker),
        )) => ("sticker", one_line(&sticker.content.body)),
        // Still encrypted after the SDK's attempt: say so, never an empty bubble.
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomEncrypted(_)) => {
            ("encrypted", String::new())
        }
        // A poll pins as its question, never as the generic "Message".
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::UnstablePollStart(
            SyncMessageLikeEvent::Original(poll),
        )) => (
            "poll",
            crate::unstable_poll_start(&poll.content)
                .map(|p| one_line(&p.question))
                .unwrap_or_default(),
        ),
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::PollStart(
            SyncMessageLikeEvent::Original(poll),
        )) => (
            "poll",
            crate::stable_poll_start(&poll.content)
                .map(|p| one_line(&p.question))
                .unwrap_or_default(),
        ),
        _ => ("other", String::new()),
    }
}

/// Whether an event's own `room_id` (present on a fetched event, absent from
/// the typed sync form) names a different room than the one being resolved.
/// A spec server answers 404 for that; a buggy one must not make this room's
/// Pinned panel show another room's text. A missing `room_id` is not a
/// mismatch: the room-scoped endpoint already vouched for it.
fn from_another_room(event_room: Option<&RoomId>, room: &RoomId) -> bool {
    event_room.is_some_and(|r| r != room)
}

/// Read the room's pinned list and resolve it into displayable rows.
///
/// Result event: `room_pinned { op_id, room_id, ok, can_pin, total,
/// truncated, entries[] }`. Each entry has `event_id` and `available`, and
/// when available `sender`, `sender_display_name`, `sender_avatar_url`,
/// `timestamp_ms`, `kind`, `preview`. Unresolvable ids are reported as
/// unavailable, not dropped.
///
/// `allow_remote` governs only the `/state` fallback when the room has no
/// pinned-events state yet. Subscribed rooms get it from sync, so C++ passes
/// false for sync-driven refreshes; otherwise every pinless room would cost
/// a GET per refresh.
pub(crate) fn fetch_pinned(
    bridge: &RustClient,
    room_id: String,
    allow_remote: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let own_id = client.user_id().map(ToOwned::to_owned);
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        // Room state first; `/state` only when the event has not synced yet.
        // `load_pinned_events()` takes no RequestConfig, so it is wrapped in a
        // timeout (the room-action pool is joined at sign-out). A timeout is a
        // failure, never "pins nothing": C++ keeps its last list on failure.
        let ids: Vec<OwnedEventId> = match room.pinned_event_ids() {
            Some(ids) => ids,
            None if !allow_remote => Vec::new(),
            None => {
                let loaded = tokio::time::timeout(
                    PINNED_REQUEST_TIMEOUT,
                    room.load_pinned_events(),
                )
                .await;
                match loaded {
                    Ok(Ok(Some(ids))) => ids,
                    // No pinned-events state at all: an empty list is the answer.
                    Ok(Ok(None)) => Vec::new(),
                    other => {
                        // Timeout or server error: both failures.
                        let category = match other {
                            Ok(Err(err)) => {
                                classify_room_error(&err.to_string())
                            }
                            _ => "network",
                        };
                        if timelines.lifecycle_current(lifecycle) {
                            enqueue(&events, json!({
                                "type": "room_pinned",
                                "op_id": op_id,
                                "lifecycle": lifecycle,
                                "room_id": room_id,
                                "ok": false,
                                "category": category,
                            }));
                        }
                        return;
                    }
                }
            }
        };

        // Permission from the SDK's check against the real required level, never
        // a role label.
        // The own member row may not be stored yet on a first open; the room's
        // power-levels state answers the same question without it.
        let can_pin = match own_id.as_deref() {
            Some(own) => match room.get_member_no_sync(own).await.ok().flatten() {
                Some(m) => m.can_send_state(StateEventType::RoomPinnedEvents),
                None => room.power_levels().await.ok().is_some_and(|levels| {
                    levels.user_can_send_state(own, StateEventType::RoomPinnedEvents)
                }),
            },
            None => false,
        };

        let total = ids.len();
        let truncated = total > PINNED_RESOLVE_CAP;
        // The complete id list crosses uncapped: it answers "is this pinned?" for
        // the message menu, where a capped answer would be wrong.
        let all_ids: Vec<String> = ids.iter().map(|id| id.to_string()).collect();
        // Newest pins are appended, so the tail is kept.
        let resolve: Vec<OwnedEventId> = ids
            .into_iter()
            .skip(total.saturating_sub(PINNED_RESOLVE_CAP))
            .collect();

        let config = RequestConfig::new()
            .disable_retry()
            .timeout(PINNED_REQUEST_TIMEOUT);
        let mut entries = Vec::with_capacity(resolve.len());
        for event_id in resolve {
            // Re-check between resolutions so a sign-out abandons the round.
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let loaded = room.load_or_fetch_event(&event_id, Some(config)).await;
            let foreign = loaded.as_ref().ok().is_some_and(|ev| {
                let event_room = ev
                    .raw()
                    .get_field::<OwnedRoomId>("room_id")
                    .ok()
                    .flatten();
                from_another_room(event_room.as_deref(), room.room_id())
            });
            let parsed = loaded
                .ok()
                .filter(|_| !foreign)
                .and_then(|ev| ev.raw().deserialize().ok());
            let Some(parsed) = parsed else {
                // Missing, deleted or unreachable: an unavailable row that navigates
                // nowhere.
                entries.push(json!({
                    "event_id": event_id.to_string(),
                    "available": false,
                }));
                continue;
            };
            let (kind, preview) = describe(&parsed);
            let sender = parsed.sender().to_owned();
            let member = room.get_member_no_sync(&sender).await.ok().flatten();
            entries.push(json!({
                "event_id": event_id.to_string(),
                "available": true,
                "sender": sender.to_string(),
                "sender_display_name": member
                    .as_ref()
                    .and_then(|m| m.display_name().map(|s| s.to_owned()))
                    .unwrap_or_default(),
                "sender_avatar_url": member
                    .as_ref()
                    .and_then(|m| m.avatar_url().map(|a| a.to_string()))
                    .unwrap_or_default(),
                "timestamp_ms": u64::from(parsed.origin_server_ts().0),
                "kind": kind,
                "preview": preview,
            }));
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(&events, json!({
            "type": "room_pinned",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "ok": true,
            "can_pin": can_pin,
            "total": total,
            "truncated": truncated,
            "ids": all_ids,
            "entries": entries,
        }));
    });
    Ok(())
}

/// Pin (`pin = true`) or unpin one event. The SDK does the read-modify-send,
/// so a concurrent change is never clobbered by a stale list.
///
/// Result event: `room_pin_result { op_id, room_id, event_id, pin, ok,
/// changed, category }`. `changed` is false when it was already so.
pub(crate) fn set_pinned(
    bridge: &RustClient,
    room_id: String,
    event_id: String,
    pin: bool,
    op_id: u64,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let room = joined_room(&client, &room_id)?;
    let target = EventId::parse(&event_id).map_err(|_| "invalid event id".to_owned())?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let result = if pin {
            room.pin_event(&target).await
        } else {
            room.unpin_event(&target).await
        };
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, changed, category) = match result {
            Ok(changed) => (true, changed, ""),
            Err(err) => (false, false, classify_room_error(&err.to_string())),
        };
        enqueue(&events, json!({
            "type": "room_pin_result",
            "op_id": op_id,
            "lifecycle": lifecycle,
            "room_id": room_id,
            "event_id": event_id,
            "pin": pin,
            "ok": ok,
            "changed": changed,
            "category": category,
        }));
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_line_collapses_and_bounds() {
        assert_eq!(one_line("hello\nworld"), "hello world");
        assert_eq!(one_line("  spaced   out \t line "), "spaced out line");
        // Control characters never survive into a single-line label.
        assert_eq!(one_line("a\u{0007}b"), "a b");
        let long = "x".repeat(PREVIEW_MAX_CHARS + 50);
        assert_eq!(one_line(&long).chars().count(), PREVIEW_MAX_CHARS);
    }

    #[test]
    fn one_line_is_char_bounded_not_byte_bounded() {
        // Multi-byte input is not cut mid-character.
        let long = "é".repeat(PREVIEW_MAX_CHARS + 10);
        let out = one_line(&long);
        assert_eq!(out.chars().count(), PREVIEW_MAX_CHARS);
    }

    // A pinned MSC4274 gallery is described by its contents and caption, not
    // Sable's generated body.
    #[test]
    fn describe_maps_a_gallery_to_its_kind_and_caption() {
        let event = |body: &str, second: &str| {
            let raw = serde_json::json!({
                "type": "m.room.message",
                "event_id": "$gal:example.org",
                "sender": "@alice:example.org",
                "origin_server_ts": 1000,
                "content": {
                    "msgtype": "dm.filament.gallery",
                    "body": body,
                    "itemtypes": [
                        { "itemtype": "m.image", "body": "a.png", "filename": "a.png",
                          "url": "mxc://example.org/a" },
                        { "itemtype": second, "body": "b", "filename": "b",
                          "url": "mxc://example.org/b" },
                    ],
                },
            });
            let parsed: AnySyncTimelineEvent =
                serde_json::from_value(raw).expect("deserialize fixture");
            describe(&parsed)
        };
        assert_eq!(
            event("[a.png: mxc://example.org/a]\n[b: mxc://example.org/b]", "m.image"),
            ("image", String::new())
        );
        assert_eq!(event("side by side", "m.file"), ("file", "side by side".to_owned()));
    }

    #[test]
    fn describe_maps_polls_to_their_question() {
        let stable = serde_json::json!({
            "type": "m.poll.start",
            "event_id": "$p1:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": {
                "m.text": [{ "body": "Lunch?\n1. Pizza\n2. Soup" }],
                "m.poll": {
                    "kind": "m.disclosed",
                    "max_selections": 1,
                    "question": { "m.text": [{ "body": "Lunch?" }] },
                    "answers": [
                        { "m.id": "a", "m.text": [{ "body": "Pizza" }] },
                        { "m.id": "b", "m.text": [{ "body": "Soup" }] },
                    ],
                },
            },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(stable).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("poll", "Lunch?".to_owned()));

        let unstable = serde_json::json!({
            "type": "org.matrix.msc3381.poll.start",
            "event_id": "$p2:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": {
                "org.matrix.msc1767.text": "Tea or coffee?",
                "org.matrix.msc3381.poll.start": {
                    "kind": "org.matrix.msc3381.poll.disclosed",
                    "max_selections": 1,
                    "question": { "org.matrix.msc1767.text": "Tea or coffee?" },
                    "answers": [
                        { "id": "a", "org.matrix.msc1767.text": "Tea" },
                        { "id": "b", "org.matrix.msc1767.text": "Coffee" },
                    ],
                },
            },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(unstable).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("poll", "Tea or coffee?".to_owned()));
    }

    #[test]
    fn an_event_from_another_room_is_foreign_and_an_absent_room_is_not() {
        let here = RoomId::parse("!here:example.org").unwrap();
        let there = RoomId::parse("!there:example.org").unwrap();
        assert!(from_another_room(Some(&there), &here));
        assert!(!from_another_room(Some(&here), &here));
        assert!(!from_another_room(None, &here));
    }

    #[test]
    fn describe_maps_message_kinds() {
        let raw_text = serde_json::json!({
            "type": "m.room.message",
            "event_id": "$one:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": { "msgtype": "m.text", "body": "pinned\nnote" },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(raw_text).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("text", "pinned note".to_owned()));

        let raw_image = serde_json::json!({
            "type": "m.room.message",
            "event_id": "$two:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": {
                "msgtype": "m.image",
                "body": "cat.png",
                "url": "mxc://example.org/abc",
            },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(raw_image).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("image", "cat.png".to_owned()));

        // Redacted is distinguishable from unavailable.
        let raw_redacted = serde_json::json!({
            "type": "m.room.message",
            "event_id": "$three:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": {},
            "unsigned": {
                "redacted_because": {
                    "type": "m.room.redaction",
                    "event_id": "$r:example.org",
                    "sender": "@alice:example.org",
                    "origin_server_ts": 1001,
                    "content": {},
                },
            },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(raw_redacted).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("redacted", String::new()));

        // Encrypted content never crosses as an empty text bubble.
        let raw_encrypted = serde_json::json!({
            "type": "m.room.encrypted",
            "event_id": "$four:example.org",
            "sender": "@alice:example.org",
            "origin_server_ts": 1000,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "AwgAEnB",
                "sender_key": "k",
                "device_id": "D",
                "session_id": "s",
            },
        });
        let parsed: AnySyncTimelineEvent =
            serde_json::from_value(raw_encrypted).expect("deserialize fixture");
        assert_eq!(describe(&parsed), ("encrypted", String::new()));
    }
}
