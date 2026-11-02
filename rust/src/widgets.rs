
use serde_json::{json, Value};

pub(crate) const WIDGETS_TYPE: &str = "im.vector.modular.widgets";
pub(crate) const WIDGETS_TYPE_ALT: &str = "m.widget";

pub(crate) const MAX_WIDGETS: usize = 32;

const MAX_TEXT: usize = 512;

fn bounded(value: &str) -> String {
    value
        .chars()
        .filter(|c| !c.is_control())
        .take(MAX_TEXT)
        .collect()
}

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

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Widget {
    pub id: String,
    pub state_key: String,
    pub removable: bool,
    pub creator: String,
    pub kind: String,
    pub name: String,
    pub raw_url: String,
    pub data: Value,
}

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
    let _ = resolved;
    out.push("connection");
    out
}

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
            if !out.iter().any(|w| w.id == widget.id) {
                out.push(widget);
            }
        }
    };

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
        assert!(widget_from_state(&state("k", "@a:x", json!({}))).is_none());
        assert!(widget_from_state(&state("k", "@a:x", json!({"type": "jitsi"}))).is_none());
        assert!(widget_from_state(&state("k", "@a:x",
            json!({"type": "jitsi", "url": "   "}))).is_none());
        assert!(widget_from_state(&state("k", "@a:x",
            json!({"type": "", "url": "https://x/"}))).is_none());
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
        let values = template_values("@a:x", "!r:x", "w", "Ann", "", "DEV",
                                     "https://hs.example", "dark", "en");
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

    #[test]
    fn disclosuresDescribeThisWidgetAndNotWidgetsInGeneral() {
        let raw = "https://ok.example/?u=$matrix_user_id&d=$org.matrix.msc3819.matrix_device_id";
        let told = disclosures("https://ok.example/", raw);
        assert!(told.contains(&"user_id"));
        assert!(told.contains(&"device_id"));
        assert!(!told.contains(&"avatar_url"),
                "the notice claimed something this widget never receives");
        assert!(!told.contains(&"room_id"));

        let plain = disclosures("https://ok.example/", "https://ok.example/");
        assert_eq!(plain, vec!["connection"]);
    }

    #[test]
    fn a_widget_that_lightning_could_not_open_is_refused_before_any_task() {
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
        assert!(validate_widget_write("w1", &json!({})).is_ok());
        assert!(validate_widget_write("w1", &json!({ "url": 7 })).is_err());
        assert!(validate_widget_write(&"k".repeat(512), &json!({})).is_ok());
        assert!(validate_widget_write(&"k".repeat(513), &json!({})).is_err());
        assert!(validate_widget_write("bad\u{7}key", &json!({})).is_err());
        assert!(validate_widget_write("  ", &json!({})).is_err());
        assert!(validate_widget_write("w1", &json!("not an object")).is_err());
    }
}
