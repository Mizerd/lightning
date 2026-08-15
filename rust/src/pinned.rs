//! Pinned messages (v0.7.x): `m.room.pinned_events` read, resolve and write.
//!
//! Everything here is SDK-owned Matrix state. Lightning invents no storage
//! format: the pinned list IS the `m.room.pinned_events` state event, read
//! through `Room::pinned_event_ids()` (current room state) with
//! `Room::load_pinned_events()` as the `/state` fallback, and written through
//! `Room::pin_event()` / `Room::unpin_event()`, which perform the
//! read-modify-send of the state event themselves.
//!
//! A pinned event is very often NOT in the loaded timeline — that is the
//! normal case, not an edge case — so each id is resolved through
//! `Room::load_or_fetch_event()`: cache-first, one bounded `/event` request
//! on a miss, and the result is written back into the event cache. In an
//! encrypted room the SDK decrypts on that path exactly like any other
//! event; nothing here touches ciphertext or crypto state.
//!
//! Fan-out is bounded twice: at most `PINNED_RESOLVE_CAP` ids are resolved,
//! sequentially, each with a short no-retry timeout. A room whose pin list
//! is longer reports `truncated` honestly rather than issuing hundreds of
//! sequential GETs.
//!
//! Only presentation-safe fields cross the FFI. Message bodies do cross (the
//! preview is the point of the surface, exactly like the room-list latest
//! preview) but they are memory-only on the C++ side and are never written
//! into `CacheStore`. Nothing here logs event ids, senders or bodies.

use std::sync::Arc;
use std::time::Duration;

use matrix_sdk::{
    config::RequestConfig,
    ruma::{
        events::{
            AnySyncMessageLikeEvent, AnySyncTimelineEvent, StateEventType,
            SyncMessageLikeEvent,
        },
        EventId, OwnedEventId,
    },
};
use serde_json::json;

use crate::rooms::{classify_room_error, joined_room, require_client};
use crate::{enqueue, RustClient};

/// Hard cap on how many pinned events one snapshot resolves. Rooms pin a
/// handful; a pathological list must not become a hundred sequential GETs.
/// The MOST RECENT entries are kept (Matrix appends new pins to the end).
const PINNED_RESOLVE_CAP: usize = 32;

/// Per-event bound. A pinned-list refresh is disposable — the next one
/// supersedes it — and the room-action pool is JOINED during sign-out, so a
/// default-retry request loop here would stall shutdown.
const PINNED_REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// Preview text is a label, not a message view.
const PREVIEW_MAX_CHARS: usize = 140;

/// One-line preview: no control characters, bounded length. Collapsing the
/// newlines here (rather than in QML) keeps a multi-line pin from ever
/// arriving as something a single-line label must silently clip.
fn one_line(body: &str) -> String {
    let collapsed: String = body
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect();
    let trimmed = collapsed.split_whitespace().collect::<Vec<_>>().join(" ");
    trimmed.chars().take(PREVIEW_MAX_CHARS).collect()
}

/// Classify a resolved pinned event into (kind, preview). `kind` is what the
/// UI renders when a textual preview is not useful; `preview` is empty for
/// those kinds rather than a fabricated placeholder.
fn describe(parsed: &AnySyncTimelineEvent) -> (&'static str, String) {
    use matrix_sdk::ruma::events::room::message::MessageType;

    match parsed {
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
            SyncMessageLikeEvent::Original(message),
        )) => match &message.content.msgtype {
            MessageType::Text(c) => ("text", one_line(&c.body)),
            MessageType::Notice(c) => ("notice", one_line(&c.body)),
            MessageType::Emote(c) => ("emote", one_line(&c.body)),
            // For media the body IS the filename/caption in Matrix; it is
            // the most useful label there is, so it crosses as the preview
            // with the kind alongside it.
            MessageType::Image(c) => ("image", one_line(&c.body)),
            MessageType::Video(c) => ("video", one_line(&c.body)),
            MessageType::Audio(c) => ("audio", one_line(&c.body)),
            MessageType::File(c) => ("file", one_line(&c.body)),
            MessageType::Location(c) => ("location", one_line(&c.body)),
            _ => ("other", String::new()),
        },
        // Redacted is an ANSWER, not a failure: the pin still exists and the
        // UI says so plainly instead of pretending the event is missing.
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(
            SyncMessageLikeEvent::Redacted(_),
        )) => ("redacted", String::new()),
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(
            SyncMessageLikeEvent::Original(sticker),
        )) => ("sticker", one_line(&sticker.content.body)),
        // Still encrypted after the SDK's decryption attempt: an honest
        // "cannot show this yet", never an empty text bubble.
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomEncrypted(_)) => {
            ("encrypted", String::new())
        }
        _ => ("other", String::new()),
    }
}

/// Read the room's pinned list and resolve it into displayable rows.
///
/// Result event: `room_pinned { op_id, room_id, ok, can_pin, total,
/// truncated, entries[] }`. Each entry carries `event_id`, `available`, and
/// — when available — `sender`, `sender_display_name`, `sender_avatar_url`,
/// `timestamp_ms`, `kind`, `preview`.
///
/// An id that cannot be resolved is reported with `available: false` rather
/// than dropped: the pin genuinely exists in room state, and silently
/// omitting it would misreport the room's own state.
///
/// `allow_remote` governs ONLY the `/state` fallback taken when the room
/// carries no pinned-events state at all. matrix-sdk-ui asks for
/// `m.room.pinned_events` in every room SUBSCRIPTION's required state, so a
/// room the user has open has it from sync — the fallback exists for the
/// first fetch, which can outrun that subscription's first response. C++
/// passes false for refreshes driven by a sync poke, where the state is
/// known to be current; without that gate every room with no pins would
/// spend one 404-able GET on every refresh.
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
        // Current room state first; `/state` only when the state event has
        // not been synced into the room yet.
        //
        // `Room::load_pinned_events()` takes no RequestConfig (unlike the
        // per-event resolves below, which go through `Room::event`), so it
        // would otherwise run with the client's default retry policy — and
        // the room-action pool is JOINED during sign-out, where an
        // unbounded retry loop stalls shutdown. Wrapped in an explicit
        // timeout of the same budget instead.
        //
        // A timeout is reported as a FAILURE, never collapsed into the
        // Ok(None) "this room pins nothing" answer: those two are different
        // facts, and the C++ side keeps its last known list on a failure
        // precisely so a flaky connection cannot read as "the pins are
        // gone".
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
                    // No pinned-events state event at all: an empty list is
                    // the correct, authoritative answer.
                    Ok(Ok(None)) => Vec::new(),
                    other => {
                        // Elapsed timeout or a server error — both failures.
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

        // Permission comes from the SDK's own power-level helper against the
        // real `m.room.pinned_events` required level — never a hardcoded
        // "admin only", never derived from a role label.
        let can_pin = match own_id.as_deref() {
            Some(own) => room
                .get_member_no_sync(own)
                .await
                .ok()
                .flatten()
                .is_some_and(|m| m.can_send_state(StateEventType::RoomPinnedEvents)),
            None => false,
        };

        let total = ids.len();
        let truncated = total > PINNED_RESOLVE_CAP;
        // The COMPLETE id list crosses, uncapped: it is what answers "is
        // this message pinned?" for the message-action menu, and a capped
        // answer there would be a wrong answer rather than a partial one.
        // Ids are cheap, and the list is bounded in practice by the
        // homeserver's own event-size limit on the state event.
        let all_ids: Vec<String> = ids.iter().map(|id| id.to_string()).collect();
        // Newest pins are appended, so the tail is what a capped list keeps.
        let resolve: Vec<OwnedEventId> = ids
            .into_iter()
            .skip(total.saturating_sub(PINNED_RESOLVE_CAP))
            .collect();

        let config = RequestConfig::new()
            .disable_retry()
            .timeout(PINNED_REQUEST_TIMEOUT);
        let mut entries = Vec::with_capacity(resolve.len());
        for event_id in resolve {
            // Re-check between resolutions: a sign-out mid-list must abandon
            // the round rather than issue the remaining GETs.
            if !timelines.lifecycle_current(lifecycle) {
                return;
            }
            let loaded = room.load_or_fetch_event(&event_id, Some(config)).await;
            let parsed = loaded.ok().and_then(|ev| ev.raw().deserialize().ok());
            let Some(parsed) = parsed else {
                // Missing, deleted, or unreachable: an honest unavailable
                // row. The UI must not navigate anywhere for it.
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

/// Pin (`pin = true`) or unpin one event. The SDK performs the
/// read-modify-send of `m.room.pinned_events` itself, so Lightning never
/// constructs the state event content and can never clobber a concurrent
/// change with a stale list of its own.
///
/// Result event: `room_pin_result { op_id, room_id, event_id, pin, ok,
/// changed, category }`. `changed` is false when the event was already in
/// the requested state — a no-op, reported as one rather than as a success
/// that did something.
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
        // Multi-byte input must not be truncated mid-character.
        let long = "é".repeat(PREVIEW_MAX_CHARS + 10);
        let out = one_line(&long);
        assert_eq!(out.chars().count(), PREVIEW_MAX_CHARS);
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

        // A redacted pin is a real state, distinguishable from unavailable.
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

        // Still-encrypted content never crosses as an empty text bubble.
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
