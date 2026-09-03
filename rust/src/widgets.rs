//! Matrix widgets (MSC1236 / MSC2764): DISCOVERY and URL construction.
//!
//! # What this is, and the decision behind it
//!
//! A widget is a web page a room advertises — a Jitsi call, an Etherpad, a
//! dashboard. Element renders them in a sandboxed iframe. Lightning does not
//! render them: it lists them, resolves their URL, tells the user what the
//! page will learn about them, and opens it in the user's own browser.
//!
//! That is a deliberate choice, not a shortcut, and `docs/widgets.md` carries
//! the evidence. In short, embedding needs Qt WebEngine, and:
//!
//! * **Windows cannot build it.** The Windows package comes from Fedora's
//!   `mingw64-qt6-*` RPMs and there is no `mingw64-qt6-qtwebengine`; Chromium
//!   needs MSVC.
//! * **Flatpak could only ship it unsandboxed.** Flatpak's seccomp blocklist
//!   EPERMs `unshare`/`CLONE_NEWUSER`, so Chromium's own sandbox cannot start
//!   and the documented workaround is to disable it. Untrusted web content
//!   running unsandboxed beside Megolm keys is not something CLAUDE.md §6
//!   permits.
//! * **`QtWebEngineQuick::initialize()` forces the whole application's Qt
//!   Quick scenegraph to OpenGL**, which is a cross-cutting change to a
//!   client whose timeline is a hand-tuned rotated Flickable with a
//!   documented frame-cost history.
//! * The payload is ~429 MB, and matrix-sdk 0.18's widget driver has a
//!   capability bypass (its `send` short-circuits on a `redacts` field before
//!   any type check, so ANY send grant is a redaction grant).
//!
//! Opening in the browser gives up the widget API and gets, in exchange, a
//! containment boundary the operating system already enforces: a separate
//! process with no access to this one's tokens, keys or memory.
//!
//! # A widget URL is attacker-chosen
//!
//! Widget state is written by any room member with permission — a moderator
//! in most rooms, everybody in some. Every URL here is hostile until checked,
//! and the checks are MSC2764's own Security Considerations, which say
//! clients MUST refuse schemes other than http/https "including template
//! variables as schemes", and MUST validate AFTER templating and BEFORE
//! rendering or asking for permission.

use serde_json::{json, Value};

/// Matrix has never specified widgets. `im.vector.modular.widgets` is what is
/// deployed; `m.widget` has been proposed since 2020 and is written by
/// nothing, so it is read as a courtesy and never written.
pub(crate) const WIDGETS_TYPE: &str = "im.vector.modular.widgets";
pub(crate) const WIDGETS_TYPE_ALT: &str = "m.widget";

/// Widgets returned per room. A room advertising hundreds is either broken or
/// hostile, and either way a list nobody can read is not worth building.
pub(crate) const MAX_WIDGETS: usize = 32;

/// Bound on every free-text field. These are attacker-chosen strings that end
/// up in a QML label.
const MAX_TEXT: usize = 512;

fn bounded(value: &str) -> String {
    // Control characters would let a name forge layout in a list. Stripped
    // here rather than in QML, so every consumer inherits it.
    value
        .chars()
        .filter(|c| !c.is_control())
        .take(MAX_TEXT)
        .collect()
}

/// One widget, as the UI sees it.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Widget {
    pub id: String,
    pub creator: String,
    pub kind: String,
    pub name: String,
    pub raw_url: String,
    pub data: Value,
}

/// Read one widget out of a room-state event.
///
/// THE STATE KEY IS THE ID AND THE SENDER IS THE CREATOR. The content carries
/// `id` and `creatorUserId` fields too, and Element does not even write them —
/// it reconstructs both from the envelope and overwrites whatever the content
/// said. Trusting the content would let a widget claim an id that belongs to
/// another widget, which is how a remembered consent gets applied to the wrong
/// page.
///
/// A widget is LIVE when `type` and `url` are both present and non-empty. An
/// empty content object is the tombstone: it is how Element removes a widget,
/// and reading it as a widget would resurrect deleted ones.
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
    // `name` falls back to the type, as Element's own does — a widget with no
    // name should read as "Jitsi", not as a blank row.
    let name = content
        .get("name")
        .and_then(|v| v.as_str())
        .filter(|s| !s.trim().is_empty())
        .unwrap_or(kind);
    Some(Widget {
        id: bounded(state_key),
        creator: bounded(sender),
        kind: bounded(kind),
        name: bounded(name),
        raw_url: url.trim().to_owned(),
        data: content.get("data").cloned().unwrap_or_else(|| json!({})),
    })
}

/// Every `$variable` the widget API defines, with its value for this user and
/// room.
///
/// Ten of them, and TWO SPELLINGS of the device id: matrix-widget-api says
/// `$org.matrix.msc3819.matrix_device_id` and matrix-sdk 0.18 writes
/// `$org.matrix.msc2873.matrix_device_id`. They disagree, so both are
/// substituted — a widget expecting the other spelling would otherwise be
/// handed a literal `$org.matrix...` in its URL.
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
        // Element falls back to the user id here; a widget that greets you by
        // name should not greet you as an empty string.
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

/// Percent-encode a substituted value.
///
/// Unconditional, for every variable, in path, query and fragment alike —
/// which is what matrix-widget-api and the SDK both do. A display name is
/// user-chosen text and may contain `/`, `?`, `#` or `..`; substituted raw it
/// would change the URL's structure rather than fill a slot in it.
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

/// Why a widget cannot be opened, or `None` when it can.
///
/// MSC2764: clients MUST NOT render a widget whose scheme is anything but
/// http/https, **including template variables as schemes**, and MUST validate
/// AFTER templating.
pub(crate) fn refusal(url: &str) -> Option<&'static str> {
    let parsed = match url::Url::parse(url) {
        Ok(parsed) => parsed,
        Err(_) => return Some("not_a_url"),
    };
    if parsed.scheme() != "https" {
        // http is refused too, not only javascript/data/file. A widget is a
        // page that will be handed the user's display name and device id;
        // sending those over cleartext because the room said so is not a
        // choice worth offering.
        return Some("not_https");
    }
    if !parsed.username().is_empty() || parsed.password().is_some() {
        // `https://evil.example@trusted.example/` reads as trusted.example to
        // a person and resolves to evil.example nowhere — but the shape is
        // built for misreading, so it is refused rather than explained.
        return Some("has_userinfo");
    }
    match parsed.host_str() {
        None => Some("no_host"),
        Some(host) if host.is_empty() => Some("no_host"),
        Some(_) => None,
    }
}

/// True when the RAW url could make the origin depend on a substituted value.
///
/// Templating is textual over the whole URL, so `https://$matrix_display_name
/// .evil.example/` becomes a host derived from the user's own profile. The
/// origin must be a property of the room's state, not of who is looking at it,
/// so a variable anywhere in the authority disqualifies the widget before any
/// substitution happens.
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

/// Substitute every template variable and return the URL to open.
///
/// Returns `Err(reason)` when the result must not be opened. The order is
/// load-bearing and is MSC2764's: refuse an authority-templating URL FIRST,
/// substitute SECOND, validate the RESULT THIRD. Validating before
/// substitution would pass a URL whose final scheme is `javascript:`.
pub(crate) fn resolve_url(
    raw_url: &str,
    values: &[(&'static str, String)],
) -> Result<String, &'static str> {
    if templates_the_authority(raw_url) {
        return Err("templated_authority");
    }
    let mut out = raw_url.to_owned();
    // LONGEST NAME FIRST. The names share prefixes
    // (`$org.matrix.msc2873.client_id` and `$org.matrix.msc2873.client_theme`
    // do not, but `$matrix_room_id` and a shorter `$matrix_room` would), and a
    // plain replace in declaration order can eat the prefix of a longer name
    // and leave its tail behind. Element has exactly this hazard.
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

/// What the widget will learn about the user, as stable keys the UI renders as
/// sentences.
///
/// Derived from the URL that will actually be opened, so it never claims more
/// than is shared: a widget whose URL uses no variables is told nothing beyond
/// the request itself, and saying otherwise would train people to ignore the
/// notice.
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
    // The origin ALWAYS learns the request itself: an IP address, a browser
    // fingerprint, and whatever cookies it has already set. Listed
    // unconditionally so the notice is never empty and never implies "this
    // widget learns nothing".
    let _ = resolved;
    out.push("connection");
    out
}

// ---------------------------------------------------------------------------
// Reading a room's widgets
// ---------------------------------------------------------------------------

/// Fetch and parse every widget a room advertises.
///
/// Reads BOTH deployed type names. `get_state_events` is store-only in
/// matrix-sdk 0.18 (no network fallback), and widget state is not in sliding
/// sync's required-state list — which `RoomListService::subscribe_to_rooms`
/// gives no way to extend — so a room whose state has not been fetched would
/// answer "no widgets" rather than "not loaded yet". The caller therefore
/// falls back to a raw `/state` read, the same shape banner.rs already needed.
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
            // One id wins once. The two type names can carry the same widget,
            // and the store and the network answer can carry it twice — a list
            // with duplicates is a list that opens the same page twice.
            if !out.iter().any(|w| w.id == widget.id) {
                out.push(widget);
            }
        }
    };

    // 1. THE STORE, which costs nothing when the state is already there.
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

    // 2. THE NETWORK, and this is not a nicety — it is the only path that
    //    works today.
    //
    //    `Room::get_state_events` reads the STATE STORE and never the network.
    //    Widget state reaches that store only if sliding sync asked for it in
    //    `required_state`, and matrix-sdk-ui 0.18's
    //    `RoomListService::subscribe_to_rooms` takes room ids ONLY — there is
    //    no API to extend the list. So the store answer is empty for every
    //    room, and a live run against a real homeserver found exactly zero
    //    widgets in a room that had four. This is the same shape recorded for
    //    `m.room.pinned_events` and answered the same way banner.rs answers
    //    it: store first, raw ruma second.
    //
    //    The whole state is fetched because widgets are keyed by state key and
    //    there is no "all keys of one type" endpoint. One request, on demand,
    //    only when a surface actually asks.
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

/// One widget as it crosses the FFI. `url` is the RESOLVED, validated URL, or
/// empty with `refusal` naming why it cannot be opened — the UI shows the row
/// either way, because a widget silently missing from the list is
/// indistinguishable from a room having none.
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
        // The content's own `id`/`creatorUserId` are IGNORED. Element does not
        // even write them, and trusting them would let a widget claim an id
        // belonging to another — which is how a remembered consent gets
        // applied to the wrong page.
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
        // `{}` is how Element REMOVES a widget. Reading it as one would
        // resurrect every widget anybody ever deleted.
        assert!(widget_from_state(&state("k", "@a:x", json!({}))).is_none());
        // Either field missing or blank is equally dead.
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
        // Substitution is textual over the whole URL, so a variable in the
        // authority makes the ORIGIN depend on the user's own profile. The
        // origin has to be a property of the room's state, not of who is
        // looking at it.
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
        // A display name is user-chosen text. Substituted raw, `../` or a `#`
        // would change the URL's STRUCTURE rather than fill a slot in it.
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
        // The order is the whole point: validate the RESULT, not the input. A
        // template that assembles a scheme must not slip through.
        let values = template_values("@a:x", "!r:x", "w", "Ann", "", "DEV",
                                     "https://hs.example", "dark", "en");
        // A raw URL that does not parse as https is refused whatever it
        // templates to.
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
        // BOTH device spellings: matrix-widget-api and matrix-sdk disagree,
        // and a widget expecting the other one would receive a literal.
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

        // A widget using no variables still learns the connection itself, so
        // the notice is never empty and never implies "this learns nothing".
        let plain = disclosures("https://ok.example/", "https://ok.example/");
        assert_eq!(plain, vec!["connection"]);
    }
}
