//! MSC2346 — which network a room is bridged to, as the bridge itself says.
//!
//! # Why this exists
//!
//! Lightning already labels bridged conversations, and until now it derived
//! that label from two identifiers the room list happened to hold: the DM
//! partner's ghost mxid (`@whatsapp_…:server`) and the room's canonical
//! alias. `src/matrix/BridgeNetwork.h` carries the full reasoning. The
//! consequence, reported by a tester on 2026-09-06 as "bridge tags appear
//! only on direct messages", is structural: `direct_targets()` is filled from
//! `m.direct` account data, so the ghost mxid reaches that code for DMs and
//! for nothing else, and mautrix-family bridges publish no canonical alias
//! for a portal room. A bridged GROUP therefore has nothing to match on.
//!
//! It is not that the bridge fails to advertise itself. It advertises through
//! MSC2346 room state, which names the protocol explicitly and is what
//! Element reads for its Bridge Info panel. This module reads that.
//!
//! # The event
//!
//! Stable type `m.bridge`, unstable prefix `uk.half-shot.bridge` — BOTH are
//! read, because the unstable one is what is deployed. The state key is an
//! arbitrary bridge-chosen string (`org.matrix.appservice-irc://irc/freenode/
//! #friends`) and MAY be empty, so there is no key to fetch by: the room's
//! state has to be swept. The content carries `protocol` (required),
//! `network` and `channel` (optional), each `{id, displayname?, avatar_url?,
//! external_url?}`, plus `bridgebot` and `creator` mxids.
//!
//! **A bridge is REMOVED by setting the same state key to `{}`.** An empty
//! content object is a tombstone and must read as "no bridge", exactly as it
//! does for widgets — reading it as a bridge resurrects every bridge anybody
//! ever unplugged.
//!
//! # Everything here is attacker-influenced
//!
//! This is room state: anyone with the power level to send state can write
//! it, and the MSC's own security section says so ("a malicious room admin
//! can specify any user ID in those fields"). So:
//!
//! * `bridgebot` and `creator` are deliberately NOT surfaced. They are mxids
//!   a room admin chose, and a user id shown beside a network name reads as
//!   provenance this client cannot vouch for. Nothing needs them.
//! * every string that does cross the FFI is sanitised HERE, once, so every
//!   consumer inherits it: control characters removed (they forge layout in
//!   a list), Unicode bidi controls removed (a right-to-left override inside
//!   a room-list chip is a spoofing surface), whitespace runs collapsed, and
//!   a hard character bound — measured in CHARACTERS, and truncated on a
//!   char boundary by construction.
//! * the protocol id is what the C++ side maps through its own curated
//!   network table, so a KNOWN id gets OUR label and the bridge's own text is
//!   only ever a fallback for an id we do not recognise.
//!
//! The list is bounded too: a room advertising dozens of bridges is broken or
//! hostile, and either way a chip row nobody can read is not worth building.

use serde_json::{json, Value};

/// The stable type, and the unstable prefix that is what bridges actually
/// send. Read both; write neither — Lightning never publishes bridge state.
pub(crate) const BRIDGE_TYPE: &str = "m.bridge";
pub(crate) const BRIDGE_TYPE_ALT: &str = "uk.half-shot.bridge";

/// Bridges returned per room. A room genuinely bridged to several networks
/// exists (a portal that fans out), but not to eight.
pub(crate) const MAX_BRIDGES: usize = 8;

/// Character bounds. A protocol id is a machine token ("whatsapp",
/// "discord"); a display name is a chip caption; a state key is an id.
const MAX_PROTOCOL_ID: usize = 64;
const MAX_DISPLAY_NAME: usize = 64;
const MAX_ID: usize = 128;

/// Characters removed outright: every non-whitespace control, and the Unicode
/// bidi formatting characters.
///
/// Whitespace controls (tab, newline, NEL) are deliberately KEPT here so the
/// collapse below turns them into a single space — stripping them would glue
/// two words together ("a\nb" -> "ab"), which is a different kind of wrong
/// name. Bidi controls have no legitimate use in a network name and a
/// right-to-left override is how a chip is made to read as something else.
fn is_stripped(c: char) -> bool {
    if c.is_control() && !c.is_whitespace() {
        return true;
    }
    matches!(c,
        '\u{200E}' | '\u{200F}'      // LRM, RLM
        | '\u{202A}'..='\u{202E}'    // LRE, RLE, PDF, LRO, RLO
        | '\u{2066}'..='\u{2069}')   // LRI, RLI, FSI, PDI
}

/// Strip, collapse, trim, bound — in that order.
///
/// The order matters: stripping first means a run of controls between two
/// words does not survive as two spaces, and bounding LAST means the bound
/// counts the characters a reader will actually see. `chars().take()` cannot
/// split a character, so there is no byte-boundary hazard.
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

/// MSC2346: "displayname … if not present, the id should be used". `fallback`
/// is used when the object carries neither.
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

/// One advertised bridge, sanitised.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct BridgeInfo {
    /// The event's state key, bounded. Not shown to anyone: it is the dedup
    /// key, because the stable and unstable types can carry the same bridge.
    pub state_key: String,
    /// The network kind, lowercased ("whatsapp", "discord", "irc"). MSC2346
    /// says the id is case-insensitive and should be lowercase; this makes it
    /// so, because the C++ table lookup is exact.
    pub protocol_id: String,
    /// The bridge's OWN name for the protocol. Attacker-chosen; used only
    /// when the protocol id is unknown to the curated table.
    pub protocol_name: String,
    /// The bridge's own name for the network instance ("Freenode"). Same
    /// trust level, same use.
    pub network_name: String,
}

/// Whether a state event type is one of the two spellings.
pub(crate) fn is_bridge_state_type(event_type: &str) -> bool {
    event_type == BRIDGE_TYPE || event_type == BRIDGE_TYPE_ALT
}

/// Read one bridge out of a room-state event, or nothing.
///
/// Nothing means: the content is not an object, the content is `{}` (the
/// MSC's own removal), there is no `protocol` object, or its `id` is empty
/// once sanitised. A bridge with no protocol is not a bridge — it is the one
/// field the MSC makes required, and it is the only field this client acts on.
pub(crate) fn bridge_from_state(value: &Value) -> Option<BridgeInfo> {
    let state_key = sanitize(
        value.get("state_key").and_then(|v| v.as_str()).unwrap_or(""),
        MAX_ID,
    );
    let content = value.get("content")?.as_object()?;
    // The tombstone. See the module header.
    if content.is_empty() {
        return None;
    }
    let protocol = content.get("protocol")?.as_object()?;
    // Lowercased BEFORE the bound, so a case fold that changes the character
    // count cannot push the result over it.
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

/// Fold one state event into the list, deduplicating. Returns true while
/// there is still room for another.
///
/// The dedup key is the state key AND the protocol id: the two type names can
/// carry the same bridge, and the store and the network answer can carry it
/// twice, but an empty state key is legal and shared, so the key alone would
/// collapse two genuinely different bridges into one.
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

/// What crosses the FFI. `bridgebot` and `creator` are absent on purpose —
/// see the module header.
pub(crate) fn bridge_payload(info: &BridgeInfo) -> Value {
    json!({
        "protocol": info.protocol_id,
        "protocolName": info.protocol_name,
        "network": info.network_name,
    })
}

/// Every bridge a room advertises.
///
/// Store first, network second — the same two-step `widgets.rs` and
/// `banner.rs` already needed, and for the same reason: `Room::get_state_
/// events` reads the STATE STORE and never the network, state reaches that
/// store only if sliding sync asked for it in `required_state`, and
/// matrix-sdk-ui 0.18's `RoomListService::subscribe_to_rooms` takes room ids
/// ONLY — there is no API to extend the list. So the store answer is empty
/// for every room today, and the `/state` read is not a nicety.
///
/// `allow_network` is the caller's budget control. A full `/state` on a large
/// room is a large response, and the room-list badge must never pay for one;
/// the caller decides which surface may (see `AppController`).
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

    // 1. THE STORE, which costs nothing when the state is already there.
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

    // 2. THE NETWORK. One request, on demand, only when a surface that is
    //    allowed to pay for it actually asks.
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
        // MSC2346 removes a bridge by setting the same state key to `{}`.
        // Reading that as a bridge resurrects every bridge anybody unplugged.
        assert!(bridge_from_state(&state("irc://freenode", json!({}))).is_none());
        // And a tombstone must not be able to REPLACE a live answer either:
        // it contributes nothing to the list.
        let mut out = Vec::new();
        absorb(&state("k", protocol("whatsapp")), &mut out);
        absorb(&state("k", json!({})), &mut out);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].protocol_id, "whatsapp");
    }

    #[test]
    fn aBridgeWithoutAProtocolIsNotABridge() {
        // `protocol` is the one required field and the only one acted on.
        assert!(bridge_from_state(&state("k", json!({"bridgebot": "@bot:x"}))).is_none());
        // Present but not an object.
        assert!(bridge_from_state(&state("k", json!({"protocol": "whatsapp"}))).is_none());
        // Present but empty, and empty AFTER sanitising — a name made only of
        // bidi controls is an empty name.
        assert!(bridge_from_state(&state("k", protocol(""))).is_none());
        assert!(bridge_from_state(&state("k", protocol("\u{202E}\u{202C}"))).is_none());
        assert!(bridge_from_state(&state("k", protocol("   "))).is_none());
        // Content that is not an object at all.
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
        // A right-to-left override in a room-list chip is a spoofing surface:
        // it reverses everything drawn after it.
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

        // Control characters go the same way, and a whitespace control
        // becomes a single space rather than gluing two words together.
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
        // Multi-byte on purpose: a byte bound would either cut fewer
        // characters than intended or split one and produce invalid text.
        let long = "é".repeat(500);
        let info = bridge_from_state(&state(
            "k",
            json!({"protocol": {"id": "custom", "displayname": long}}),
        ))
        .unwrap();
        assert_eq!(info.protocol_name.chars().count(), MAX_DISPLAY_NAME);
        assert!(info.protocol_name.chars().all(|c| c == 'é'));

        // And the state key, which is an id rather than a caption.
        let info = bridge_from_state(&state(&"k".repeat(400), protocol("irc"))).unwrap();
        assert_eq!(info.state_key.chars().count(), MAX_ID);
    }

    #[test]
    fn theDisplayNameFallsBackToTheIdAndThenToTheProtocol() {
        // MSC2346: "if not present, the id should be used".
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
        // The unstable prefix is what mautrix actually sends; the stable type
        // is what the MSC will land as. A lookalike is not either.
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

        // The same bridge under both type names (same state key, same
        // protocol) is ONE row, not two.
        let mut out = Vec::new();
        absorb(&state("irc://freenode", protocol("irc")), &mut out);
        absorb(&state("irc://freenode", protocol("irc")), &mut out);
        assert_eq!(out.len(), 1);
        // An empty state key is legal and shared, so it cannot be the whole
        // dedup key: two different protocols under it are two bridges.
        let mut out = Vec::new();
        absorb(&state("", protocol("irc")), &mut out);
        absorb(&state("", protocol("discord")), &mut out);
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn thePayloadCarriesNoUserIds() {
        // The MSC's own security section: "a malicious room admin can specify
        // any user ID in those fields". Nothing needs them, so nothing gets
        // them — a mxid beside a network name reads as provenance.
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
