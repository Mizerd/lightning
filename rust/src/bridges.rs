
use serde_json::{json, Value};

pub(crate) const BRIDGE_TYPE: &str = "m.bridge";
pub(crate) const BRIDGE_TYPE_ALT: &str = "uk.half-shot.bridge";

pub(crate) const MAX_BRIDGES: usize = 8;

const MAX_PROTOCOL_ID: usize = 64;
const MAX_DISPLAY_NAME: usize = 64;
const MAX_ID: usize = 128;

fn is_stripped(c: char) -> bool {
    if c.is_control() && !c.is_whitespace() {
        return true;
    }
    matches!(c,
        '\u{200E}' | '\u{200F}'
        | '\u{202A}'..='\u{202E}'
        | '\u{2066}'..='\u{2069}')
}

fn sanitize(value: &str, max_chars: usize) -> String {
    let cleaned: String = value.chars().filter(|c| !is_stripped(*c)).collect();
    cleaned
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(max_chars)
        .collect()
}

/// MSC2346: "displayname … if not present, the id should be used";
/// `fallback` when neither is present.
fn display_name(object: &serde_json::Map<String, Value>, fallback: &str) -> String {
    let named = sanitize(
        object.get("displayname").and_then(|v| v.as_str()).unwrap_or(""),
        MAX_DISPLAY_NAME,
    );
    if !named.is_empty() {
        return named;
    }
    let from_id = sanitize(
        object.get("id").and_then(|v| v.as_str()).unwrap_or(""),
        MAX_DISPLAY_NAME,
    );
    if !from_id.is_empty() {
        return from_id;
    }
    fallback.to_owned()
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct BridgeInfo {
    pub state_key: String,
    pub protocol_id: String,
    pub protocol_name: String,
    pub network_name: String,
}

pub(crate) fn is_bridge_state_type(event_type: &str) -> bool {
    event_type == BRIDGE_TYPE || event_type == BRIDGE_TYPE_ALT
}

pub(crate) fn bridge_from_state(value: &Value) -> Option<BridgeInfo> {
    let state_key = sanitize(
        value.get("state_key").and_then(|v| v.as_str()).unwrap_or(""),
        MAX_ID,
    );
    let content = value.get("content")?.as_object()?;
    if content.is_empty() {
        return None;
    }
    let protocol = content.get("protocol")?.as_object()?;
    let protocol_id = sanitize(
        &protocol
            .get("id")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_lowercase(),
        MAX_PROTOCOL_ID,
    );
    if protocol_id.is_empty() {
        return None;
    }
    let protocol_name = display_name(protocol, &protocol_id);
    let network_name = content
        .get("network")
        .and_then(|v| v.as_object())
        .map(|network| display_name(network, ""))
        .unwrap_or_default();
    Some(BridgeInfo { state_key, protocol_id, protocol_name, network_name })
}

pub(crate) fn absorb(value: &Value, out: &mut Vec<BridgeInfo>) -> bool {
    if let Some(info) = bridge_from_state(value) {
        let seen = out
            .iter()
            .any(|b| b.state_key == info.state_key && b.protocol_id == info.protocol_id);
        if !seen {
            out.push(info);
        }
    }
    out.len() < MAX_BRIDGES
}

/// What crosses the FFI. `bridgebot` and `creator` are omitted on purpose.
pub(crate) fn bridge_payload(info: &BridgeInfo) -> Value {
    json!({
        "protocol": info.protocol_id,
        "protocolName": info.protocol_name,
        "network": info.network_name,
    })
}

/// Every bridge a room advertises. Store first, then the network, as in
/// `widgets.rs`: `get_state_events` never fetches and sliding sync's
/// required state cannot include these types, so the `/state` read is what
/// answers.
///
/// `allow_network` is the caller's budget control: a full `/state` on a
/// large room is expensive, and the room-list badge must never pay for one
/// (see `AppController`).
pub(crate) async fn read_room_bridges(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::room::Room,
    allow_network: bool,
) -> Vec<BridgeInfo> {
    use matrix_sdk::config::RequestConfig;
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    use matrix_sdk::ruma::api::client::state::get_state_events;
    use matrix_sdk::ruma::events::StateEventType;

    let mut out: Vec<BridgeInfo> = Vec::new();

    for type_name in [BRIDGE_TYPE_ALT, BRIDGE_TYPE] {
        let Ok(events) = room.get_state_events(StateEventType::from(type_name)).await else {
            continue;
        };
        for raw in events {
            let json = match &raw {
                RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
            };
            if let Ok(value) = serde_json::from_str::<Value>(&json) {
                if !absorb(&value, &mut out) {
                    return out;
                }
            }
        }
    }
    if !out.is_empty() || !allow_network {
        return out;
    }

    let config = RequestConfig::new()
        .disable_retry()
        .timeout(std::time::Duration::from_secs(20));
    let request = get_state_events::v3::Request::new(room.room_id().to_owned());
    let Ok(response) = client.send(request).with_request_config(config).await else {
        return out;
    };
    for raw in response.room_state {
        let Ok(value) = serde_json::from_str::<Value>(raw.json().get()) else {
            continue;
        };
        let event_type = value.get("type").and_then(|v| v.as_str()).unwrap_or("");
        if !is_bridge_state_type(event_type) {
            continue;
        }
        if !absorb(&value, &mut out) {
            break;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(state_key: &str, content: Value) -> Value {
        json!({"state_key": state_key, "sender": "@admin:example.org",
               "content": content})
    }

    fn protocol(id: &str) -> Value {
        json!({"protocol": {"id": id}})
    }

    #[test]
    fn anEmptyContentIsATombstoneNotABridge() {
        assert!(bridge_from_state(&state("irc://freenode", json!({}))).is_none());
        let mut out = Vec::new();
        absorb(&state("k", protocol("whatsapp")), &mut out);
        absorb(&state("k", json!({})), &mut out);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].protocol_id, "whatsapp");
    }

    #[test]
    fn aBridgeWithoutAProtocolIsNotABridge() {
        assert!(bridge_from_state(&state("k", json!({"bridgebot": "@bot:x"}))).is_none());
        assert!(bridge_from_state(&state("k", json!({"protocol": "whatsapp"}))).is_none());
        assert!(bridge_from_state(&state("k", protocol(""))).is_none());
        assert!(bridge_from_state(&state("k", protocol("\u{202E}\u{202C}"))).is_none());
        assert!(bridge_from_state(&state("k", protocol("   "))).is_none());
        assert!(bridge_from_state(&json!({"state_key": "k", "content": "x"})).is_none());
    }

    #[test]
    fn theProtocolIdIsLowercasedAndBounded() {
        let info = bridge_from_state(&state("k", protocol("WhatsApp"))).unwrap();
        assert_eq!(info.protocol_id, "whatsapp",
                   "the C++ network-table lookup is exact, so the id must be folded here");

        let long = "d".repeat(400);
        let info = bridge_from_state(&state("k", protocol(&long))).unwrap();
        assert_eq!(info.protocol_id.chars().count(), MAX_PROTOCOL_ID);
    }

    #[test]
    fn aBidiOverrideNeverReachesTheChip() {
        let hostile = "Disc\u{202E}drocsi\u{202C}ord\u{200F}";
        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "custom", "displayname": hostile}}),
        ))
        .unwrap();
        assert!(!info.protocol_name.chars().any(is_stripped),
                "a bidi control survived into the display name: {:?}",
                info.protocol_name);
        assert_eq!(info.protocol_name, "Discdrocsiord");

        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "custom",
                                "displayname": "  Free \u{7}node \n\t IRC  "}}),
        ))
        .unwrap();
        assert_eq!(info.protocol_name, "Free node IRC");
    }

    #[test]
    fn anOverLongDisplayNameIsBoundedInCharactersNotBytes() {
        let long = "é".repeat(500);
        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "custom", "displayname": long}}),
        ))
        .unwrap();
        assert_eq!(info.protocol_name.chars().count(), MAX_DISPLAY_NAME);
        assert!(info.protocol_name.chars().all(|c| c == 'é'));

        let info = bridge_from_state(&state(&"k".repeat(400), protocol("irc"))).unwrap();
        assert_eq!(info.state_key.chars().count(), MAX_ID);
    }

    #[test]
    fn theDisplayNameFallsBackToTheIdAndThenToTheProtocol() {
        let info = bridge_from_state(&state("k", protocol("irc"))).unwrap();
        assert_eq!(info.protocol_name, "irc");
        assert_eq!(info.network_name, "", "an absent network object is not a name");

        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "irc", "displayname": "IRC"},
                   "network": {"id": "freenode"}}),
        ))
        .unwrap();
        assert_eq!(info.protocol_name, "IRC");
        assert_eq!(info.network_name, "freenode");

        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "irc"},
                   "network": {"id": "freenode", "displayname": "Freenode"}}),
        ))
        .unwrap();
        assert_eq!(info.network_name, "Freenode");
    }

    #[test]
    fn bothTypeSpellingsAreRead() {
        assert!(is_bridge_state_type("uk.half-shot.bridge"));
        assert!(is_bridge_state_type("m.bridge"));
        assert!(!is_bridge_state_type("uk.half-shot.bridges"));
        assert!(!is_bridge_state_type("m.bridge.info"));
        assert!(!is_bridge_state_type("im.vector.modular.widgets"));
    }

    #[test]
    fn theListIsBoundedAndDeduplicated() {
        let mut out = Vec::new();
        let mut room_left = true;
        for i in 0..40 {
            room_left = absorb(&state(&format!("k{i}"), protocol("irc")), &mut out);
            if !room_left {
                break;
            }
        }
        assert_eq!(out.len(), MAX_BRIDGES);
        assert!(!room_left, "absorb must report the list full so the sweep stops");

        let mut out = Vec::new();
        absorb(&state("irc://freenode", protocol("irc")), &mut out);
        absorb(&state("irc://freenode", protocol("irc")), &mut out);
        assert_eq!(out.len(), 1);
        let mut out = Vec::new();
        absorb(&state("", protocol("irc")), &mut out);
        absorb(&state("", protocol("discord")), &mut out);
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn thePayloadCarriesNoUserIds() {
        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "whatsapp"},
                   "bridgebot": "@whatsappbot:evil.example",
                   "creator": "@mallory:evil.example"}),
        ))
        .unwrap();
        let payload = bridge_payload(&info);
        let text = payload.to_string();
        assert!(!text.contains("evil.example"), "payload leaked a user id: {text}");
        assert_eq!(payload["protocol"], "whatsapp");
    }
}
