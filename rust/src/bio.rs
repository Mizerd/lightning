//! Profile biographies (MSC4440) over extended profile fields (MSC4133).
//!
//! A bio is a short free-text self-description shown on a profile card. This
//! module is written for INTEROPERABILITY, exactly like its sibling
//! `banner.rs`, and it reuses that module's `profile_field` transport rather
//! than inventing a second one — ruma's typed profile-field requests cannot
//! reach a Synapse that actually implements MSC4133 (the reasoning is written
//! out in full at the top of `banner.rs`; do not "fix" it back).
//!
//!   * READ prefers the stable field `m.biography` and falls back to
//!     `gay.fomx.biography`, MSC4440's own unstable prefix and the key Sable
//!     writes today.
//!   * WRITE sets BOTH, so a bio written in Lightning is visible in Sable and
//!     the other way round. A bio nobody else can see is not a bio.
//!
//! # What crosses the FFI, and what deliberately does not
//!
//! MSC4440 stores an extensible-events object: an ordered `m.text` array whose
//! entries may carry an HTML representation alongside the plain one, and the
//! MSC invites clients to render the HTML in preference.
//!
//! **Lightning does not, and this is a security decision, not an omission.** A
//! bio is free text chosen by a REMOTE user. §6 of the development guide
//! forbids rendering untrusted remote content as rich text, and the MSC's own
//! example carries an `<img src="mxc://...">` — a profile card that rendered
//! that would fetch an image of the profile owner's choosing for every person
//! who so much as looked at them. So only PLAIN TEXT ever crosses this
//! boundary. When a peer supplied nothing but an HTML representation, its
//! markup is stripped here and the result is still delivered as plain text,
//! because the alternative — showing nothing for a bio that plainly exists —
//! is worse interoperability for no additional safety.
//!
//! The text is bounded, control characters are removed, and the value is never
//! logged. MSC4440's own security section names an unbounded bio as the
//! obvious attack; `MAX_BIO_CHARS` is that bound.
//!
//! A homeserver that does not implement MSC4133 answers with an
//! unrecognised-endpoint error, reported as `supported: false` and rendered as
//! NOTHING. That is a DIFFERENT fact from "this user has no bio", which is an
//! ordinary `M_NOT_FOUND`, and the two must not be conflated — see
//! `banner::is_unsupported`, which is shared with this module precisely so the
//! distinction cannot drift between them.

use std::sync::Arc;

use matrix_sdk::ruma::{OwnedUserId, UserId};
use serde_json::json;

use crate::banner::{is_unsupported, profile_field};
use crate::rooms::{classify_room_error, require_client};
use crate::{enqueue, RustClient};

/// The stable field from MSC4440.
const BIO_FIELD: &str = "m.biography";
/// MSC4440's own unstable prefix, and the key Sable already writes. Read AND
/// written, for the same reason `banner.rs` writes `chat.commet.profile_banner`.
const BIO_FIELD_UNSTABLE: &str = "gay.fomx.biography";

/// One profile-field round trip. Rides the room-action pool, which sign-out
/// joins, so no retry and a hard bound.
const BIO_REQUEST_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(15);

/// Client-side ceiling on a bio, in Unicode scalar values.
///
/// MSC4440 specifies no limit and names that as a security consideration:
/// "malicious actors could set an unreasonably long bio, potentially lagging or
/// even crashing clients". This is a CLIENT bound, not a protocol one, and it
/// is applied on read as well as on write — a peer's server is not obliged to
/// enforce ours.
pub(crate) const MAX_BIO_CHARS: usize = 2048;

/// Client-side ceiling on the number of LINES.
///
/// The character bound alone does not stop 2048 newlines, which would render as
/// a profile card several screens tall containing nothing.
pub(crate) const MAX_BIO_LINES: usize = 40;

/// Normalise and bound a bio for display.
///
/// Deliberately `chars()`, never a byte slice: `&text[..N]` panics on a
/// multi-byte boundary. Newlines survive — a bio is a multi-line block and
/// collapsing it to one line would destroy the only formatting this client
/// honours — but every other control character becomes a space, so a bio
/// cannot carry terminal escapes, bidirectional overrides expressed as
/// controls, or embedded NULs into a label.
pub(crate) fn sanitize_bio(text: &str) -> String {
    // CRLF and bare CR both normalise to LF first, so a Windows-authored bio
    // does not end up with a control character on every line.
    let normalized = text.replace("\r\n", "\n").replace('\r', "\n");
    let cleaned: String = normalized
        .chars()
        .map(|c| if c == '\n' || !c.is_control() { c } else { ' ' })
        .collect();

    // Collapse runs of three or more blank lines to one blank line, and bound
    // the line count. A paragraph break is meaningful; forty of them are not.
    let mut lines: Vec<&str> = Vec::new();
    let mut consecutive_blank = 0usize;
    for line in cleaned.lines() {
        // trim_END only. Trailing whitespace is invisible and carries
        // nothing; LEADING whitespace is content — a bio may indent a list
        // or a snippet, and silently un-indenting someone's own words is a
        // change to what they wrote rather than a bound on it. A line that
        // is nothing BUT whitespace still reads as blank here, which is what
        // the blank-run collapse below needs.
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
    // Leading/trailing blank lines carry no information and would render as
    // empty space inside the card's border.
    while lines.first().is_some_and(|l| l.is_empty()) {
        lines.remove(0);
    }
    while lines.last().is_some_and(|l| l.is_empty()) {
        lines.pop();
    }
    lines.join("\n").chars().take(MAX_BIO_CHARS).collect()
}

/// Strip HTML markup from a formatted bio, conservatively.
///
/// This is NOT an HTML parser and does not try to be one. It exists for a
/// single case: a peer whose client wrote only an HTML representation. The
/// result is delivered and rendered as PLAIN TEXT, so a tag this misses is a
/// cosmetic blemish and never an injection — the safety comes from the
/// rendering mode, not from this function.
///
/// `<br>` and `</p>` become line breaks because a bio written as HTML
/// paragraphs is otherwise delivered as one unbroken run.
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
    // The five predefined XML entities, and nothing else: a numeric-entity
    // decoder here would be a second parser to get wrong, and an undecoded
    // entity in plain text is legible.
    out.replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        // Ampersand LAST, so "&amp;lt;" does not become "<".
        .replace("&amp;", "&")
}

/// True when this `m.text` entry is an HTML representation.
fn is_html_entry(entry: &serde_json::Value) -> bool {
    entry
        .get("mimetype")
        .and_then(|m| m.as_str())
        .is_some_and(|m| m.eq_ignore_ascii_case("text/html"))
}

/// Pull displayable plain text out of an MSC4440 biography value.
///
/// Accepts the three shapes seen in the wild:
///   * the MSC's object — `{ "m.text": [ { "body": …, "mimetype": … }, … ] }`;
///   * a bare string, which some clients stored before the MSC settled;
///   * an `m.text` array of bare strings.
///
/// A plain entry always wins over an HTML one regardless of order (the MSC's
/// example puts HTML FIRST, so taking `[0]` would pick the markup every time).
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

/// The MSC4440 value for a bio, as Lightning writes it.
///
/// Plain text only, one `m.text` entry, no `mimetype` — the absence of a
/// mimetype IS "this is plain text" in extensible events. Lightning never
/// authors an HTML representation, so it can never be the client that puts a
/// remote image reference into somebody else's profile card.
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
        // `supported` is decided by ACCOUNTING, exactly as in `banner.rs`: the
        // server is unsupported only when EVERY attempt came back
        // unrecognised. An M_NOT_FOUND is a supported server answering "this
        // user has not written one", and reporting that as unsupported would
        // hide the whole feature from everyone whose contacts have no bio.
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
                // A transport failure says nothing about what the server
                // implements, so it must never latch "unsupported".
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

/// Set — or CLEAR — the signed-in account's bio, under BOTH field names.
///
/// An EMPTY (or whitespace-only) `text` means CLEAR, and clearing DELETES the
/// fields rather than storing an empty object: a present-but-empty bio would
/// render as an empty card in every client that shows one.
///
/// Emits `profile_bio_set`.
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
    // Bounded HERE as well as on read: this is what leaves the machine.
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
                // Clearing a field that was never set is not a failure: the
                // desired end state — no bio under this key — already holds.
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
        // MSC4440's stable name, and its unstable prefix — which is what Sable
        // writes today. Both are READ and both are WRITTEN; a bio nobody else
        // can see is not a bio.
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
        // The MSC's own formatted example puts HTML at index 0, so anything
        // that took entry [0] would render markup — and would pull the remote
        // image in that example's <img src="mxc://...">.
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
        // Interop, not rendering: the value still crosses as plain text and is
        // shown as plain text. Showing nothing for a bio that exists would be
        // worse interoperability for no extra safety.
        let body = r#"{"m.biography":{"m.text":[
            {"body":"hi <b>there</b><br/>second line","mimetype":"text/html"}]}}"#;
        assert_eq!(
            bio_from_body("m.biography", body).as_deref(),
            Some("hi there\nsecond line")
        );
    }

    #[test]
    fn markup_never_survives_into_the_delivered_text() {
        // The exact hazard §6 forbids: a remote image reference in a profile
        // field would be fetched by every viewer if it were rendered as rich
        // text. Nothing resembling a tag may cross.
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
        // Not the MSC's shape, but cheap to tolerate on READ.
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
        // MSC4440's own security section names the unbounded bio as the
        // attack. Both bounds are ours; the protocol specifies neither.
        let long = "a".repeat(MAX_BIO_CHARS * 3);
        assert_eq!(sanitize_bio(&long).chars().count(), MAX_BIO_CHARS);

        let tall = (0..MAX_BIO_LINES * 4)
            .map(|i| format!("line {i}"))
            .collect::<Vec<_>>()
            .join("\n");
        assert_eq!(sanitize_bio(&tall).lines().count(), MAX_BIO_LINES);

        // ...and a bio that is nothing but blank lines collapses to nothing at
        // all, rather than to a card several screens tall.
        assert!(sanitize_bio(&"\n".repeat(500)).is_empty());
    }

    #[test]
    fn bounding_cuts_at_a_scalar_boundary_never_inside_one() {
        // Same rule as `profile::bound_display_name`: a byte slice would panic
        // on a multi-byte boundary or emit half a UTF-8 sequence.
        let text = format!("{}{}", "\u{1F98A}".repeat(MAX_BIO_CHARS), "tail");
        let bounded = sanitize_bio(&text);
        assert_eq!(bounded.chars().count(), MAX_BIO_CHARS);
        assert!(bounded.chars().all(|c| c == '\u{1F98A}'));
    }

    #[test]
    fn blank_line_runs_collapse_and_the_block_is_trimmed() {
        // Leading and trailing BLANK LINES go, a run of them collapses to
        // one, and trailing spaces go. Leading indentation stays: it is the
        // author's own text, not padding this client gets to remove. A line
        // of nothing but spaces still counts as blank.
        assert_eq!(sanitize_bio("\n\n a \n\n\n\n b \n\n"), " a\n\n b");
        assert_eq!(sanitize_bio("   \n a\n   \n"), " a");
        assert_eq!(sanitize_bio("a  \nb\t"), "a\nb");
    }

    #[test]
    fn what_lightning_writes_is_plain_text_with_no_html_representation() {
        // Lightning must never be the client that puts a remote image
        // reference into somebody else's profile card, so it authors no HTML
        // representation at all. The absence of a mimetype IS "plain text" in
        // extensible events.
        let value = bio_value("hello\nworld");
        let entries = value["m.text"].as_array().expect("m.text array");
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0]["body"], "hello\nworld");
        assert!(entries[0].get("mimetype").is_none());
        assert!(!value.to_string().contains("text/html"));
    }

    #[test]
    fn what_lightning_writes_is_what_lightning_reads_back() {
        // Round trip: the write shape must parse through the read path, or a
        // bio set here would be invisible here.
        let value = bio_value(&sanitize_bio("hello\n\nworld"));
        assert_eq!(
            bio_text_from_value(&value).as_deref(),
            Some("hello\n\nworld")
        );
    }
}
