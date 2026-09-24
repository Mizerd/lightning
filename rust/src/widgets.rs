//! Matrix widgets (MSC1236 / MSC2764): discovery and URL construction.
//!
//! Lightning lists a room's widgets, resolves their URLs, tells the user what
//! the page will learn, and opens them in the user's own browser rather than
//! embedding them (evidence in `docs/widgets.md`). Embedding needs Qt
//! WebEngine, which cannot be built for the MinGW Windows package, would run
//! unsandboxed under Flatpak's seccomp rules next to Megolm keys, forces the
//! whole scenegraph to OpenGL, and is ~429 MB; matrix-sdk 0.18's widget
//! driver also treats any send grant as a redaction grant. A browser gives a
//! separate process with no access to this one's tokens, keys or memory.
//!
//! Widget URLs are attacker-chosen (any member with the power level can
//! write widget state). Per MSC2764's security considerations, only
//! http/https is allowed (including template variables as schemes), and
//! validation happens after templating and before opening.

use serde_json::{json, Value};

/// `im.vector.modular.widgets` is what is deployed. `m.widget` (proposed
/// since 2020, written by nothing) is read as a courtesy, never written.
pub(crate) const WIDGETS_TYPE: &str = "im.vector.modular.widgets";
pub(crate) const WIDGETS_TYPE_ALT: &str = "m.widget";

/// Widgets returned per room; hundreds means broken or hostile.
pub(crate) const MAX_WIDGETS: usize = 32;

/// Bound on every free-text field (attacker-chosen, shown in QML labels).
const MAX_TEXT: usize = 512;

fn bounded(value: &str) -> String {
    // Control characters could forge layout in a list; stripped here so every
    // consumer inherits it.
    value
        .chars()
        .filter(|c| !c.is_control())
        .take(MAX_TEXT)
        .collect()
}

/// Whether this account may write the room's widget state, per the room's
/// required level for `im.vector.modular.widgets` asked of the SDK; false
/// when membership cannot be read. Same shape as `can_manage_room_packs`.
pub(crate) async fn can_manage_widgets(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::room::Room,
) -> bool {
    use matrix_sdk::ruma::events::StateEventType;
    let Some(own) = client.user_id() else {
        return false;
    };
    room.get_member_no_sync(own)
        .await
        .ok()
        .flatten()
        .is_some_and(|m| m.can_send_state(StateEventType::from(WIDGETS_TYPE)))
}

/// Every synchronous refusal of a widget write, shared with the test so it
/// exercises the writer's own code.
///
/// The id bound matches `bounded()` (512, no control characters), so any key
/// the reader can name, the writer can tombstone. `url` may be absent (an
/// empty object is the tombstone), but if present it must be an https
/// address with a host and no credentials, so nothing this client would
/// refuse to open is published.
pub(crate) fn validate_widget_write(widget_id: &str, content: &Value) -> Result<(), String> {
    if widget_id.trim().is_empty()
        || widget_id.chars().count() > MAX_TEXT
        || widget_id.chars().any(char::is_control)
    {
        return Err("invalid widget id".to_owned());
    }
    if !content.is_object() {
        return Err("widget content must be an object".to_owned());
    }
    match content.get("url") {
        None => Ok(()),
        Some(Value::String(url)) => {
            let ok = url::Url::parse(url)
                .map(|u| u.scheme() == "https" && u.username().is_empty()
                     && u.password().is_none() && u.host_str().is_some())
                .unwrap_or(false);
            if ok { Ok(()) } else { Err("a widget address must be https".to_owned()) }
        }
        Some(_) => Err("a widget address must be a string".to_owned()),
    }
}

/// Add or remove one widget: write `content` under `widget_id` as the state
/// key; an empty object removes it (as Element does). Content is built in C++
/// from a validated https URL; this re-checks the joined room, the power
/// level and `validate_widget_write`.
pub(crate) fn write_room_widget(
    bridge: &crate::RustClient,
    op_id: u64,
    room_id: String,
    widget_id: String,
    content: Value,
) -> Result<(), String> {
    use std::sync::Arc;
    validate_widget_write(&widget_id, &content)?;
    let client = crate::rooms::require_client(bridge)?;
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let outcome = async {
            let room = crate::rooms::joined_room(&client, &room_id)
                .map_err(|_| "unknown_room".to_owned())?;
            if !can_manage_widgets(&client, &room).await {
                return Err("forbidden".to_owned());
            }
            room.send_state_event_raw(WIDGETS_TYPE, &widget_id, content)
                .await
                .map_err(|err| {
                    crate::rooms::classify_room_error(&err.to_string()).to_owned()
                })?;
            Ok::<(), String>(())
        }
        .await;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        let (ok, category) = match outcome {
            Ok(()) => (true, String::new()),
            Err(category) => (false, category),
        };
        crate::enqueue(
            &events,
            json!({
                "type": "room_widget_written",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "room_id": room_id,
                "ok": ok,
                "category": category,
            }),
        );
    });
    Ok(())
}

/// One widget, as the UI sees it.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Widget {
    pub id: String,
    /// The state key exactly as the event carries it (`id` is the bounded
    /// display form). A tombstone must name this.
    pub state_key: String,
    /// Whether this client can write its tombstone: the event is the type the
    /// writer publishes and the key survived `bounded` unchanged. The panel hides
    /// Remove otherwise.
    pub removable: bool,
    pub creator: String,
    pub kind: String,
    pub name: String,
    pub raw_url: String,
    pub data: Value,
}

/// Read one widget from a room-state event.
///
/// The state key is the id and the sender is the creator. The content's own
/// `id` and `creatorUserId` are ignored (Element overwrites them from the
/// envelope); trusting them would let a widget claim another's id and
/// inherit its remembered consent.
///
/// A widget is live when `type` and `url` are both non-empty. An empty
/// content object is the tombstone (how Element removes a widget).
pub(crate) fn widget_from_state(value: &Value) -> Option<Widget> {
    let state_key = value.get("state_key")?.as_str()?;
    if state_key.is_empty() {
        return None;
    }
    let sender = value.get("sender").and_then(|v| v.as_str()).unwrap_or("");
    let content = value.get("content")?;
    let kind = content.get("type")?.as_str()?;
    let url = content.get("url")?.as_str()?;
    if kind.trim().is_empty() || url.trim().is_empty() {
        return None;
    }
    // `name` falls back to the type, as in Element.
    let name = content
        .get("name")
        .and_then(|v| v.as_str())
        .filter(|s| !s.trim().is_empty())
        .unwrap_or(kind);
    let event_type = value.get("type").and_then(|v| v.as_str()).unwrap_or(WIDGETS_TYPE);
    let removable = event_type == WIDGETS_TYPE && bounded(state_key) == state_key;
    Some(Widget {
        id: bounded(state_key),
        state_key: state_key.to_owned(),
        removable,
        creator: bounded(sender),
        kind: bounded(kind),
        name: bounded(name),
        raw_url: url.trim().to_owned(),
        data: content.get("data").cloned().unwrap_or_else(|| json!({})),
    })
}

/// Every `$variable` the widget API defines, with its value for this user
/// and room. The device id has two spellings (matrix-widget-api uses
/// `$org.matrix.msc3819.matrix_device_id`, matrix-sdk 0.18
/// `$org.matrix.msc2873.matrix_device_id`), so both are substituted.
pub(crate) fn template_values(
    user_id: &str,
    room_id: &str,
    widget_id: &str,
    display_name: &str,
    avatar_url: &str,
    device_id: &str,
    homeserver: &str,
    theme: &str,
    language: &str,
) -> Vec<(&'static str, String)> {
    vec![
        ("$matrix_user_id", user_id.to_owned()),
        ("$matrix_room_id", room_id.to_owned()),
        ("$matrix_widget_id", widget_id.to_owned()),
        // Falls back to the user id, as Element does, rather than an empty name.
        (
            "$matrix_display_name",
            if display_name.is_empty() { user_id.to_owned() } else { display_name.to_owned() },
        ),
        ("$matrix_avatar_url", avatar_url.to_owned()),
        ("$org.matrix.msc2873.client_id", "org.lightning_matrix.Lightning".to_owned()),
        ("$org.matrix.msc2873.client_theme", theme.to_owned()),
        ("$org.matrix.msc2873.client_language", language.to_owned()),
        ("$org.matrix.msc3819.matrix_device_id", device_id.to_owned()),
        ("$org.matrix.msc2873.matrix_device_id", device_id.to_owned()),
        ("$org.matrix.msc4039.matrix_base_url", homeserver.to_owned()),
    ]
}

/// Percent-encode a substituted value, always, as matrix-widget-api and the
/// SDK do. A display name may contain `/`, `?`, `#` or `..`, which would
/// otherwise change the URL's structure.
fn encode(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for byte in value.as_bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'!' | b'~'
            | b'*' | b'\'' | b'(' | b')' => out.push(*byte as char),
            other => out.push_str(&format!("%{other:02X}")),
        }
    }
    out
}

/// Why a widget cannot be opened, or `None`. MSC2764: only http/https,
/// including template variables as schemes, validated after templating.
pub(crate) fn refusal(url: &str) -> Option<&'static str> {
    let parsed = match url::Url::parse(url) {
        Ok(parsed) => parsed,
        Err(_) => return Some("not_a_url"),
    };
    if parsed.scheme() != "https" {
        // http is refused too: the page receives the user's display name and device
        // id.
        return Some("not_https");
    }
    if !parsed.username().is_empty() || parsed.password().is_some() {
        // `https://evil.example@trusted.example/` is built for misreading, so it is
        // refused.
        return Some("has_userinfo");
    }
    match parsed.host_str() {
        None => Some("no_host"),
        Some(host) if host.is_empty() => Some("no_host"),
        Some(_) => None,
    }
}

/// True when the raw URL could make the origin depend on a substituted value
/// (e.g. `https://$matrix_display_name.evil.example/`). The origin must come
/// from room state, not from who is viewing, so any variable in the
/// authority disqualifies the widget before substitution.
pub(crate) fn templates_the_authority(raw_url: &str) -> bool {
    let after_scheme = match raw_url.split_once("://") {
        Some((_, rest)) => rest,
        None => raw_url,
    };
    let authority = after_scheme
        .split(['/', '?', '#'])
        .next()
        .unwrap_or(after_scheme);
    authority.contains('$')
}

/// Substitute every template variable and return the URL to open, or
/// `Err(reason)`. MSC2764's order: refuse authority templating, substitute,
/// then validate the result (validating first would pass a URL that
/// templates into `javascript:`).
pub(crate) fn resolve_url(
    raw_url: &str,
    values: &[(&'static str, String)],
) -> Result<String, &'static str> {
    if templates_the_authority(raw_url) {
        return Err("templated_authority");
    }
    let mut out = raw_url.to_owned();
    // Longest name first, so a shorter name cannot eat the prefix of a longer
    // one (Element has this hazard).
    let mut ordered: Vec<&(&'static str, String)> = values.iter().collect();
    ordered.sort_by(|a, b| b.0.len().cmp(&a.0.len()));
    for (name, value) in ordered {
        if out.contains(name) {
            out = out.replace(name, &encode(value));
        }
    }
    if let Some(reason) = refusal(&out) {
        return Err(reason);
    }
    Ok(out)
}

/// What the widget will learn about the user, as stable keys the UI renders
/// as sentences. Derived from the URL actually opened, so it never claims
/// more than is shared.
pub(crate) fn disclosures(resolved: &str, raw_url: &str) -> Vec<&'static str> {
    let mut out = Vec::new();
    let mentions = |name: &str| raw_url.contains(name);
    if mentions("$matrix_user_id") {
        out.push("user_id");
    }
    if mentions("$matrix_display_name") {
        out.push("display_name");
    }
    if mentions("$matrix_avatar_url") {
        out.push("avatar_url");
    }
    if mentions("matrix_device_id") {
        out.push("device_id");
    }
    if mentions("$matrix_room_id") {
        out.push("room_id");
    }
    if mentions("client_theme") {
        out.push("theme");
    }
    if mentions("client_language") {
        out.push("language");
    }
    if mentions("matrix_base_url") {
        out.push("homeserver");
    }
    // The origin always learns the request itself (IP, browser fingerprint, its
    // cookies), so the notice is never empty.
    let _ = resolved;
    out.push("connection");
    out
}

// ---------------------------------------------------------------------------
// Reading a room's widgets
// ---------------------------------------------------------------------------

/// Fetch and parse every widget a room advertises, under both type names.
/// `get_state_events` is store-only, and widget state is not in sliding
/// sync's required state, so the caller falls back to a raw `/state` read
/// (as in banner.rs).
pub(crate) async fn read_room_widgets(
    client: &matrix_sdk::Client,
    room: &matrix_sdk::room::Room,
) -> Vec<Widget> {
    use matrix_sdk::config::RequestConfig;
    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    use matrix_sdk::ruma::api::client::state::get_state_events;
    use matrix_sdk::ruma::events::StateEventType;

    let mut out: Vec<Widget> = Vec::new();
    let mut absorb = |value: &Value, out: &mut Vec<Widget>| {
        if let Some(widget) = widget_from_state(value) {
            // One entry per id: both type names and both sources can carry the same
            // widget.
            if !out.iter().any(|w| w.id == widget.id) {
                out.push(widget);
            }
        }
    };

    // 1. The store, free when the state is already there.
    for type_name in [WIDGETS_TYPE, WIDGETS_TYPE_ALT] {
        let Ok(events) = room
            .get_state_events(StateEventType::from(type_name))
            .await
        else {
            continue;
        };
        for raw in events {
            let json = match &raw {
                RawAnySyncOrStrippedState::Sync(ev) => ev.json().get().to_owned(),
                RawAnySyncOrStrippedState::Stripped(ev) => ev.json().get().to_owned(),
            };
            if let Ok(value) = serde_json::from_str::<Value>(&json) {
                absorb(&value, &mut out);
            }
            if out.len() >= MAX_WIDGETS {
                return out;
            }
        }
    }
    if !out.is_empty() {
        return out;
    }

    // 2. The network, the only path that works today: `get_state_events` never
    //    fetches, and `RoomListService::subscribe_to_rooms` cannot add widget
    //    state to required_state, so the store is empty for every room (the
    //    same as for `m.room.pinned_events`). The whole state is fetched since
    //    there is no "all keys of one type" endpoint; on demand only.
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
        if event_type != WIDGETS_TYPE && event_type != WIDGETS_TYPE_ALT {
            continue;
        }
        absorb(&value, &mut out);
        if out.len() >= MAX_WIDGETS {
            break;
        }
    }
    out
}

/// One widget as it crosses the FFI. `url` is the resolved, validated URL,
/// or empty with `refusal` naming why. The row is shown either way, so a
/// refused widget is not mistaken for none.
pub(crate) fn widget_payload(
    widget: &Widget,
    values: &[(&'static str, String)],
) -> Value {
    let (url, refusal) = match resolve_url(&widget.raw_url, values) {
        Ok(url) => (url, String::new()),
        Err(reason) => (String::new(), reason.to_owned()),
    };
    let told = if url.is_empty() {
        Vec::new()
    } else {
        disclosures(&url, &widget.raw_url)
    };
    json!({
        "id": widget.id,
        "stateKey": widget.state_key,
        "removable": widget.removable,
        "creator": widget.creator,
        "kind": widget.kind,
        "name": widget.name,
        "url": url,
        "refusal": refusal,
        "discloses": told,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(state_key: &str, sender: &str, content: Value) -> Value {
        json!({"state_key": state_key, "sender": sender, "content": content})
    }

    #[test]
    fn theStateKeyIsTheIdAndTheSenderIsTheCreator() {
        // The content's `id`/`creatorUserId` are ignored; trusting them would let a
        // widget claim another's id.
        let w = widget_from_state(&state(
            "real-key",
            "@alice:x",
            json!({"type": "jitsi", "url": "https://w.example/j",
                   "id": "forged", "creatorUserId": "@mallory:x"}),
        ))
        .expect("a well-formed widget was refused");
        assert_eq!(w.id, "real-key");
        assert_eq!(w.creator, "@alice:x");
    }

    #[test]
    fn aTombstoneIsNotAWidget() {
        // `{}` is Element's removal; reading it as a widget would resurrect deleted
        // ones.
        assert!(widget_from_state(&state("k", "@a:x", json!({}))).is_none());
        // Either field missing or blank is dead.
        assert!(widget_from_state(&state("k", "@a:x", json!({"type": "jitsi"}))).is_none());
        assert!(widget_from_state(&state("k", "@a:x",
            json!({"type": "jitsi", "url": "   "}))).is_none());
        assert!(widget_from_state(&state("k", "@a:x",
            json!({"type": "", "url": "https://x/"}))).is_none());
        // An empty state key is not an id.
        assert!(widget_from_state(&state("", "@a:x",
            json!({"type": "jitsi", "url": "https://x/"}))).is_none());
    }

    #[test]
    fn theNameFallsBackToTheTypeAndTextIsBounded() {
        let w = widget_from_state(&state("k", "@a:x",
            json!({"type": "jitsi", "url": "https://x/"}))).unwrap();
        assert_eq!(w.name, "jitsi", "a nameless widget renders as a blank row");

        let hostile = format!("a\nb\tc\u{7}d{}", "x".repeat(9000));
        let w = widget_from_state(&state("k", "@a:x",
            json!({"type": "custom", "url": "https://x/", "name": hostile}))).unwrap();
        assert!(!w.name.contains('\n') && !w.name.contains('\t'),
                "a control character survived into a label");
        assert!(w.name.len() <= MAX_TEXT);
    }

    // ── The URL is hostile until checked ─────────────────────────────────
    #[test]
    fn onlyHttpsSurvives() {
        for bad in [
            "javascript:alert(1)",
            "data:text/html,<script>alert(1)</script>",
            "file:///etc/passwd",
            "vector://vector/webapp",
            "http://plain.example/",
            "not a url at all",
        ] {
            assert!(refusal(bad).is_some(), "{bad} was allowed");
        }
        assert_eq!(refusal("https://ok.example/path?q=1"), None);
    }

    #[test]
    fn aUserinfoUrlIsRefusedBecauseItIsBuiltForMisreading() {
        assert_eq!(refusal("https://trusted.example@evil.example/"),
                   Some("has_userinfo"));
        assert_eq!(refusal("https://user:pw@evil.example/"), Some("has_userinfo"));
    }

    #[test]
    fn aTemplatedAuthorityIsRefusedBeforeAnySubstitution() {
        // A variable in the authority would make the origin depend on the viewer.
        assert!(templates_the_authority("https://$matrix_display_name.evil.example/"));
        assert!(templates_the_authority("https://$matrix_user_id@x.example/"));
        assert!(!templates_the_authority("https://ok.example/?u=$matrix_user_id"));
        assert!(!templates_the_authority("https://ok.example/#/$matrix_room_id"));

        let values = template_values("@a:x", "!r:x", "w", "Ann", "", "DEV",
                                     "https://hs.example", "dark", "en");
        assert_eq!(
            resolve_url("https://$matrix_display_name.evil.example/", &values),
            Err("templated_authority")
        );
    }

    #[test]
    fn everySubstitutedValueIsPercentEncoded() {
        // A raw display name could change the URL's structure.
        let values = template_values(
            "@a:x", "!r:x", "w", "../../evil?x=#y", "", "DEV",
            "https://hs.example", "dark", "en");
        let out = resolve_url("https://ok.example/u/$matrix_display_name", &values)
            .expect("refused a legitimate url");
        assert!(out.starts_with("https://ok.example/u/"), "{out}");
        assert!(!out.contains("../"), "{out}");
        assert!(!out.contains('#'), "{out}");
        assert!(out.contains("%2F"), "{out}");
    }

    #[test]
    fn substitutionCannotProduceANonHttpsUrl() {
        // Validate the result, not the input.
        let values = template_values("@a:x", "!r:x", "w", "Ann", "", "DEV",
                                     "https://hs.example", "dark", "en");
        // A raw URL that is not https is refused whatever it templates to.
        assert!(resolve_url("javascript:$matrix_user_id", &values).is_err());
    }

    #[test]
    fn allTenVariablesSubstituteAndBothDeviceSpellingsAreHonoured() {
        let values = template_values(
            "@ann:x", "!room:x", "wid", "Ann", "mxc://x/a", "DEVICE",
            "https://hs.example", "storm", "en-GB");
        let raw = "https://ok.example/?u=$matrix_user_id&r=$matrix_room_id\
                   &w=$matrix_widget_id&n=$matrix_display_name\
                   &a=$matrix_avatar_url&c=$org.matrix.msc2873.client_id\
                   &t=$org.matrix.msc2873.client_theme\
                   &l=$org.matrix.msc2873.client_language\
                   &d1=$org.matrix.msc3819.matrix_device_id\
                   &d2=$org.matrix.msc2873.matrix_device_id\
                   &b=$org.matrix.msc4039.matrix_base_url";
        let out = resolve_url(raw, &values).expect("refused");
        assert!(!out.contains('$'), "a variable was left unsubstituted: {out}");
        // Both device spellings are substituted.
        assert_eq!(out.matches("DEVICE").count(), 2, "{out}");
        assert!(out.contains("storm") && out.contains("en-GB"));
    }

    #[test]
    fn anEmptyDisplayNameFallsBackToTheUserId() {
        let values = template_values("@ann:x", "!r:x", "w", "", "", "D",
                                     "https://hs.example", "d", "en");
        let out = resolve_url("https://ok.example/?n=$matrix_display_name", &values)
            .unwrap();
        assert!(out.contains("%40ann%3Ax"), "{out}");
    }

    // ── The notice must be true ──────────────────────────────────────────
    #[test]
    fn disclosuresDescribeThisWidgetAndNotWidgetsInGeneral() {
        let raw = "https://ok.example/?u=$matrix_user_id&d=$org.matrix.msc3819.matrix_device_id";
        let told = disclosures("https://ok.example/", raw);
        assert!(told.contains(&"user_id"));
        assert!(told.contains(&"device_id"));
        assert!(!told.contains(&"avatar_url"),
                "the notice claimed something this widget never receives");
        assert!(!told.contains(&"room_id"));

        // A widget with no variables still learns the connection itself.
        let plain = disclosures("https://ok.example/", "https://ok.example/");
        assert_eq!(plain, vec!["connection"]);
    }

    // ── The write side ──────────────────────────────────────────────────
    //
    // Only the checks before a task is spawned are testable here (the power
    // gate needs a live room). A widget this client could not open (non-https,
    // credentials) is never published.
    #[test]
    fn a_widget_that_lightning_could_not_open_is_refused_before_any_task() {
        // The writer's own predicate, not a copy.
        let bad_urls = ["http://pad.example/p/x", "https://user:pw@pad.example/",
                        "ftp://pad.example/", "javascript:alert(1)", "",
                        "https://"];
        for url in bad_urls {
            let content = json!({ "type": "m.custom", "url": url, "name": "x" });
            assert!(validate_widget_write("w1", &content).is_err(),
                    "{url} must not pass the https-only check");
        }
        let good = json!({ "type": "m.custom", "url": "https://pad.example/p/notes" });
        assert!(validate_widget_write("w1", &good).is_ok());
        // The tombstone carries no url and must pass.
        assert!(validate_widget_write("w1", &json!({})).is_ok());
        // A non-string url is not an address.
        assert!(validate_widget_write("w1", &json!({ "url": 7 })).is_err());
        // The id bound matches the reader's.
        assert!(validate_widget_write(&"k".repeat(512), &json!({})).is_ok());
        assert!(validate_widget_write(&"k".repeat(513), &json!({})).is_err());
        assert!(validate_widget_write("bad\u{7}key", &json!({})).is_err());
        assert!(validate_widget_write("  ", &json!({})).is_err());
        assert!(validate_widget_write("w1", &json!("not an object")).is_err());
    }
}
