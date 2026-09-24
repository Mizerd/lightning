//! Profile biographies (MSC4440) over extended profile fields (MSC4133).
//!
//! Uses `banner.rs`'s `profile_field` transport, since ruma's typed requests
//! cannot reach Synapse's MSC4133 implementation (see `banner.rs`).
//!
//!   * Read prefers `m.biography` and falls back to `gay.fomx.biography`
//!     (MSC4440's unstable prefix, which Sable writes).
//!   * Write sets both, so bios are shared with Sable.
//!
//! Plain text only: MSC4440 allows an HTML representation (its example even
//! embeds `<img src="mxc://...">`), but a bio is remote free text and
//! rendering it would fetch content of the owner's choosing for every
//! viewer. An HTML-only bio is stripped to plain text rather than hidden.
//! Text is bounded (`MAX_BIO_CHARS`, per MSC4440's security section),
//! control characters are removed, and bios are never logged.
//!
//! A server without MSC4133 is reported as `supported: false` and renders
//! nothing, distinct from "no bio" (`M_NOT_FOUND`); `banner::is_unsupported`
//! is shared so the two cannot drift.

use std::sync::Arc;

use matrix_sdk::ruma::{OwnedUserId, UserId};
use serde_json::json;

use crate::banner::{is_unsupported, profile_field};
use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// The stable field from MSC4440.
const BIO_FIELD: &str = "m.biography";
/// MSC4440's unstable prefix, which Sable writes. Read and written, as with
/// `banner.rs`'s Commet key.
const BIO_FIELD_UNSTABLE: &str = "gay.fomx.biography";

/// One profile-field round trip on the room-action pool (joined at
/// sign-out): no retry, hard bound.
const BIO_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// Client-side ceiling on a bio, in Unicode scalar values. MSC4440 sets no
/// limit and names long bios as an attack; applied on read and on write.
pub(crate) const MAX_BIO_CHARS: usize = 2048;

/// Client-side ceiling on lines, so 2048 newlines cannot make a card several
/// screens tall.
pub(crate) const MAX_BIO_LINES: usize = 40;

/// Normalise and bound a bio for display. Uses `chars()`, never byte slices
/// (which panic mid code point). Newlines are kept; other control
/// characters become spaces, so no escapes, control-form bidi overrides or
/// NULs reach a label.
pub(crate) fn sanitize_bio(text: &str) -> String {
    // Normalise CRLF and bare CR to LF.
    let normalized = text.replace("\r\n", "\n").replace('\r', "\n");
    let cleaned: String = normalized
        .chars()
        .map(|c| if c == '\n' || !c.is_control() { c } else { ' ' })
        .collect();

    // Collapse runs of blank lines to one and bound the line count.
    let mut lines: Vec<&str> = Vec::new();
    let mut consecutive_blank = 0usize;
    for line in cleaned.lines() {
        // Trim the end only: leading whitespace is the author's (indented lists).
        // A whitespace-only line still reads as blank for the collapse below.
        let trimmed = line.trim_end();
        if trimmed.is_empty() {
            consecutive_blank += 1;
            if consecutive_blank > 1 {
                continue;
            }
        } else {
            consecutive_blank = 0;
        }
        lines.push(trimmed);
        if lines.len() >= MAX_BIO_LINES {
            break;
        }
    }
    // Drop leading/trailing blank lines.
    while lines.first().is_some_and(|l| l.is_empty()) {
        lines.remove(0);
    }
    while lines.last().is_some_and(|l| l.is_empty()) {
        lines.pop();
    }
    lines.join("\n").chars().take(MAX_BIO_CHARS).collect()
}

/// Strip HTML from a formatted bio, conservatively; not a parser. Only for
/// peers that wrote nothing but HTML. The result is shown as plain text, so
/// a missed tag is cosmetic, never an injection. `<br>` and `</p>` become
/// line breaks.
fn strip_html(html: &str) -> String {
    let mut out = String::with_capacity(html.len());
    let mut in_tag = false;
    let mut tag = String::new();
    for c in html.chars() {
        match c {
            '<' => {
                in_tag = true;
                tag.clear();
            }
            '>' if in_tag => {
                in_tag = false;
                let name = tag.trim().to_ascii_lowercase();
                if name.starts_with("br")
                    || name.starts_with("/p")
                    || name.starts_with("/div")
                    || name.starts_with("/li")
                {
                    out.push('\n');
                }
            }
            _ if in_tag => tag.push(c),
            _ => out.push(c),
        }
    }
    // Only the five predefined XML entities; an undecoded entity is legible,
    // and a numeric decoder would be another parser to get wrong.
    out.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        // Ampersand last, so "&amp;lt;" does not become "<".
        .replace("&amp;", "&")
}

/// True when this `m.text` entry is an HTML representation.
fn is_html_entry(entry: &serde_json::Value) -> bool {
    entry
        .get("mimetype")
        .and_then(|m| m.as_str())
        .is_some_and(|m| m.eq_ignore_ascii_case("text/html"))
}

/// Pull plain text out of an MSC4440 biography value. Accepts the MSC object
/// (`{ "m.text": [ { "body", "mimetype" }, … ] }`), a bare string (older
/// clients), and an `m.text` array of strings. A plain entry wins over an
/// HTML one regardless of order (the MSC's example puts HTML first).
pub(crate) fn bio_text_from_value(value: &serde_json::Value) -> Option<String> {
    if let Some(text) = value.as_str() {
        let cleaned = sanitize_bio(text);
        return (!cleaned.is_empty()).then_some(cleaned);
    }
    let entries = value.get("m.text")?.as_array()?;

    let mut html_fallback: Option<String> = None;
    for entry in entries {
        if let Some(text) = entry.as_str() {
            let cleaned = sanitize_bio(text);
            if !cleaned.is_empty() {
                return Some(cleaned);
            }
            continue;
        }
        let Some(body) = entry.get("body").and_then(|b| b.as_str()) else {
            continue;
        };
        if is_html_entry(entry) {
            if html_fallback.is_none() {
                let cleaned = sanitize_bio(&strip_html(body));
                if !cleaned.is_empty() {
                    html_fallback = Some(cleaned);
                }
            }
            continue;
        }
        let cleaned = sanitize_bio(body);
        if !cleaned.is_empty() {
            return Some(cleaned);
        }
    }
    html_fallback
}

/// Parse one profile-field response body into displayable bio text.
fn bio_from_body(field: &str, body: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(body).ok()?;
    bio_text_from_value(value.get(field)?)
}

/// The MSC4440 value Lightning writes: one plain `m.text` entry without
/// `mimetype` (absence means plain text). Lightning never writes HTML, so it
/// never puts remote image references in a profile.
pub(crate) fn bio_value(text: &str) -> serde_json::Value {
    json!({ "m.text": [ { "body": text } ] })
}

/// Read one user's bio. Emits `profile_bio`.
pub(crate) fn fetch_profile_bio(
    bridge: &RustClient,
    op_id: u64,
    user_id: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid: OwnedUserId = UserId::parse(user_id.as_str())
        .map_err(|_| "invalid user id".to_owned())?
        .into();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let mut bio = String::new();
        // `supported` is decided across all attempts, as in `banner.rs`: only
        // unsupported if every one came back unrecognised. M_NOT_FOUND means no bio.
        let mut any_answered = false;
        let mut any_unrecognised = false;
        for field in [BIO_FIELD, BIO_FIELD_UNSTABLE] {
            match profile_field::get(&client, uid.as_str(), field, BIO_REQUEST_TIMEOUT).await
            {
                Ok(answer) if answer.status == 200 => {
                    any_answered = true;
                    if let Some(text) = bio_from_body(field, &answer.body) {
                        bio = text;
                        break;
                    }
                }
                Ok(answer) => {
                    if is_unsupported(&answer.body) {
                        any_unrecognised = true;
                    } else {
                        any_answered = true;
                    }
                }
                // A transport failure says nothing about support; never latch unsupported.
                Err(_) => any_answered = true,
            }
        }
        let supported = any_answered || !any_unrecognised;
        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        enqueue(
            &events,
            json!({
                "type": "profile_bio",
                "op_id": op_id,
                "lifecycle": lifecycle,
                "user_id": uid.to_string(),
                "bio": bio,
                "supported": supported,
            }),
        );
    });
    Ok(())
}

/// Set or clear the account's bio under both field names. Empty or
/// whitespace-only `text` clears by deleting the fields (an empty object
/// would render as an empty card elsewhere). Emits `profile_bio_set`.
pub(crate) fn set_own_profile_bio(
    bridge: &RustClient,
    op_id: u64,
    text: String,
) -> Result<(), String> {
    let client = require_client(bridge)?;
    let uid = client
        .user_id()
        .map(ToOwned::to_owned)
        .ok_or_else(|| "no session".to_owned())?;
    // Bounded here too: this is what leaves the machine.
    let bounded = sanitize_bio(&text);
    let clearing = bounded.is_empty();
    let events = Arc::clone(&bridge.events);
    let timelines = Arc::clone(&bridge.timelines);
    let lifecycle = timelines.lifecycle();
    bridge.spawn_room_action(async move {
        let value = bio_value(&bounded);
        let mut last_error: Option<String> = None;
        let mut wrote_any = false;
        for field in [BIO_FIELD, BIO_FIELD_UNSTABLE] {
            let answer = if clearing {
                profile_field::delete(&client, uid.as_str(), field, BIO_REQUEST_TIMEOUT).await
            } else {
                profile_field::set_json(
                    &client,
                    uid.as_str(),
                    field,
                    &value,
                    BIO_REQUEST_TIMEOUT,
                )
                .await
            };
            let outcome = match answer {
                Ok(a) if (200..300).contains(&a.status) => Ok(()),
                // Clearing a never-set field is not a failure.
                Ok(a) if clearing && a.status == 404 && !is_unsupported(&a.body) => Ok(()),
                Ok(a) => Err(a.body),
                Err(text) => Err(text),
            };
            match outcome {
                Ok(()) => wrote_any = true,
                Err(text) => last_error = Some(text),
            }
        }

        if !timelines.lifecycle_current(lifecycle) {
            return;
        }
        if wrote_any {
            enqueue(
                &events,
                json!({
                    "type": "profile_bio_set",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": true,
                    "bio": bounded,
                    "category": "",
                }),
            );
        } else {
            let text = last_error.unwrap_or_else(|| "unknown".to_owned());
            let category = if is_unsupported(&text) {
                "unsupported".to_owned()
            } else {
                classify_room_error(&text).to_owned()
            };
            enqueue(
                &events,
                json!({
                    "type": "profile_bio_set",
                    "op_id": op_id,
                    "lifecycle": lifecycle,
                    "ok": false,
                    "bio": "",
                    "category": category,
                }),
            );
        }
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_field_names_are_the_ones_other_clients_already_use() {
        // MSC4440's stable name and its unstable prefix (Sable's); both are read
        // and written.
        assert_eq!(BIO_FIELD, "m.biography");
        assert_eq!(BIO_FIELD_UNSTABLE, "gay.fomx.biography");
        assert_ne!(BIO_FIELD, BIO_FIELD_UNSTABLE);
    }

    #[test]
    fn the_msc_example_parses_to_its_plain_text() {
        // Verbatim from MSC4440's first example.
        let body = r#"{"m.biography":{"m.text":[
            {"body":"hello world!\n\ninterests:\n-  programming"}]}}"#;
        assert_eq!(
            bio_from_body("m.biography", body).as_deref(),
            Some("hello world!\n\ninterests:\n-  programming")
        );
    }

    #[test]
    fn the_plain_entry_wins_even_though_the_html_one_comes_first() {
        // The MSC's formatted example puts HTML at index 0 (with a remote image);
        // the plain entry must win.
        let body = r#"{"m.biography":{"m.text":[
            {"body":"hello <b>world</b>!<br/>bye","mimetype":"text/html"},
            {"body":"hello world!\nbye"}]}}"#;
        assert_eq!(
            bio_from_body("m.biography", body).as_deref(),
            Some("hello world!\nbye")
        );
    }

    #[test]
    fn an_html_only_bio_is_delivered_as_stripped_plain_text() {
        // An HTML-only bio is still delivered, as stripped plain text.
        let body = r#"{"m.biography":{"m.text":[
            {"body":"hi <b>there</b><br/>second line","mimetype":"text/html"}]}}"#;
        assert_eq!(
            bio_from_body("m.biography", body).as_deref(),
            Some("hi there\nsecond line")
        );
    }

    #[test]
    fn markup_never_survives_into_the_delivered_text() {
        // No tag-like text may survive into what crosses.
        let body = r#"{"m.biography":{"m.text":[
            {"body":"look <img data-mx-emoticon src=\"mxc://evil.example/x\" /> at me",
             "mimetype":"text/html"}]}}"#;
        let text = bio_from_body("m.biography", body).expect("bio");
        assert!(!text.contains('<'), "markup survived: {text:?}");
        assert!(!text.contains("mxc://"), "media reference survived: {text:?}");
        assert!(!text.contains("img"), "tag name survived: {text:?}");
        assert_eq!(text, "look  at me");
    }

    #[test]
    fn entities_decode_without_reintroducing_markup() {
        assert_eq!(strip_html("a &amp;lt; b"), "a &lt; b");
        assert_eq!(strip_html("5 &lt; 6 &amp;&amp; 7 &gt; 6"), "5 < 6 && 7 > 6");
    }

    #[test]
    fn a_bare_string_value_is_accepted() {
        // Not the MSC's shape, but tolerated on read.
        let body = r#"{"m.biography":"just a sentence"}"#;
        assert_eq!(
            bio_from_body("m.biography", body).as_deref(),
            Some("just a sentence")
        );
    }

    #[test]
    fn an_absent_or_empty_field_yields_nothing_rather_than_an_empty_bio() {
        assert_eq!(bio_from_body("m.biography", r#"{}"#), None);
        assert_eq!(bio_from_body("m.biography", r#"{"m.biography":{}}"#), None);
        assert_eq!(
            bio_from_body("m.biography", r#"{"m.biography":{"m.text":[]}}"#),
            None
        );
        assert_eq!(
            bio_from_body("m.biography", r#"{"m.biography":{"m.text":[{"body":"  "}]}}"#),
            None
        );
        assert_eq!(bio_from_body("m.biography", "not json"), None);
    }

    #[test]
    fn control_characters_are_removed_but_newlines_survive() {
        let text = sanitize_bio("line one\nline two\u{7}\u{0}\ttab\r\nwindows");
        assert!(text.contains('\n'), "newlines must survive: {text:?}");
        assert!(
            !text.chars().any(|c| c.is_control() && c != '\n'),
            "control character survived: {text:?}"
        );
        assert_eq!(text, "line one\nline two   tab\nwindows");
    }

    #[test]
    fn a_bio_is_bounded_in_both_length_and_height() {
        // Both bounds are ours; the protocol specifies neither.
        let long = "a".repeat(MAX_BIO_CHARS * 3);
        assert_eq!(sanitize_bio(&long).chars().count(), MAX_BIO_CHARS);

        let tall = (0..MAX_BIO_LINES * 4)
            .map(|i| format!("line {i}"))
            .collect::<Vec<_>>()
            .join("\n");
        assert_eq!(sanitize_bio(&tall).lines().count(), MAX_BIO_LINES);

        // A bio of only blank lines collapses to nothing.
        assert!(sanitize_bio(&"\n".repeat(500)).is_empty());
    }

    #[test]
    fn bounding_cuts_at_a_scalar_boundary_never_inside_one() {
        // Cuts at scalar boundaries, as in `profile::bound_display_name`.
        let text = format!("{}{}", "\u{1F98A}".repeat(MAX_BIO_CHARS), "tail");
        let bounded = sanitize_bio(&text);
        assert_eq!(bounded.chars().count(), MAX_BIO_CHARS);
        assert!(bounded.chars().all(|c| c == '\u{1F98A}'));
    }

    #[test]
    fn blank_line_runs_collapse_and_the_block_is_trimmed() {
        // Blank lines at the ends go, runs collapse, trailing spaces go; leading
        // indentation stays. A whitespace-only line counts as blank.
        assert_eq!(sanitize_bio("\n\n a \n\n\n\n b \n\n"), " a\n\n b");
        assert_eq!(sanitize_bio("   \n a\n   \n"), " a");
        assert_eq!(sanitize_bio("a  \nb\t"), "a\nb");
    }

    #[test]
    fn what_lightning_writes_is_plain_text_with_no_html_representation() {
        // Lightning writes no HTML representation; no mimetype means plain text.
        let value = bio_value("hello\nworld");
        let entries = value["m.text"].as_array().expect("m.text array");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0]["body"], "hello\nworld");
        assert!(entries[0].get("mimetype").is_none());
        assert!(!value.to_string().contains("text/html"));
    }

    #[test]
    fn what_lightning_writes_is_what_lightning_reads_back() {
        // The write shape must parse through the read path.
        let value = bio_value(&sanitize_bio("hello\n\nworld"));
        assert_eq!(
            bio_text_from_value(&value).as_deref(),
            Some("hello\n\nworld")
        );
    }
}
