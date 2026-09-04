//! Room media history, walked INDEPENDENTLY of the live timeline.
//!
//! The Room Information panel's Media tab used to read
//! `app.timeline.mediaEntries()` — whatever the open timeline happened to
//! have paginated in — so browsing a room's attachments meant scrolling the
//! conversation backwards until they appeared. This walks `/messages`
//! backwards on its own cursor instead, so the panel can reach the start of
//! accessible history without moving the reader anywhere.
//!
//! # Why the server's own URL filter is not used
//!
//! `RoomEventFilter::url_filter = EventsWithUrl` is the obvious optimisation
//! and it is wrong here twice over:
//!
//!  * an ENCRYPTED room's events are `m.room.encrypted` on the wire and carry
//!    no `url` at all, so the filter returns nothing — the same reason
//!    server-side search cannot see encrypted rooms;
//!  * LINKS live in the `body` of an ordinary `m.text`, which has no `url`
//!    either, so the filter would hide the entire Links category.
//!
//! So the walk is unfiltered and classification happens HERE, after the SDK
//! has decrypted what it can. That costs more events per useful result on a
//! chatty room; it is the only shape that gives the same answer in an
//! encrypted room as in a public one, which is the property that matters.
//!
//! # Honesty about completeness
//!
//! A page reports what it SCANNED, not only what it matched, and whether the
//! walk reached the start of accessible history. The panel needs both to
//! avoid claiming "all media" when it has seen a hundred events of a
//! forty-thousand-event room, and to say "some of this history cannot be
//! read" rather than showing a shorter list as if it were complete.

use serde_json::{json, Value};

/// One row in the browser. Deliberately presentation-ready: the C++ side
/// stores these verbatim, so nothing has to re-parse event JSON later.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct MediaEntry {
    pub event_id: String,
    pub sender: String,
    pub ts_ms: u64,
    /// image | video | audio | voice | file | link
    pub kind: String,
    /// The caption or filename for media; the message text for a link.
    pub body: String,
    pub filename: String,
    pub mimetype: String,
    pub size: u64,
    pub width: u64,
    pub height: u64,
    pub duration_ms: u64,
    /// `mxc://` for media. Never an http URL, and never the decrypted bytes:
    /// the existing MediaBridge fetches and decrypts through the authenticated
    /// path exactly as the timeline does.
    pub mxc: String,
    pub thumbnail_mxc: String,
    /// True when the attachment is encrypted, so the caller knows the fetch
    /// has to go through the decrypting path rather than a plain download.
    pub encrypted: bool,
    /// For `kind == "link"`: the URL, and its host for grouping/filtering.
    pub url: String,
    pub host: String,
}

impl MediaEntry {
    pub(crate) fn to_json(&self) -> Value {
        json!({
            "event_id": self.event_id,
            "sender": self.sender,
            "ts_ms": self.ts_ms,
            "kind": self.kind,
            "body": self.body,
            "filename": self.filename,
            "mimetype": self.mimetype,
            "size": self.size,
            "width": self.width,
            "height": self.height,
            "duration_ms": self.duration_ms,
            "mxc": self.mxc,
            "thumbnail_mxc": self.thumbnail_mxc,
            "encrypted": self.encrypted,
            "url": self.url,
            "host": self.host,
        })
    }
}

/// What one scanned event contributed.
#[derive(Debug, Default, PartialEq)]
pub(crate) struct Scanned {
    pub entries: Vec<MediaEntry>,
    /// The event was `m.room.encrypted` — present in history and unreadable,
    /// which is a different fact from "no media here" and is reported.
    pub undecryptable: bool,
}

/// An `mxc://` URI and nothing else.
///
/// A `url` field is remote, attacker-chosen text that becomes a fetch. An
/// `http(s)` URL there would be an unauthenticated download of somebody
/// else's choosing, and anything else is not addressable at all — so a
/// non-mxc value is dropped rather than carried to the media layer, which
/// would have to refuse it anyway.
fn mxc_only(value: Option<&Value>) -> String {
    value
        .and_then(|v| v.as_str())
        .filter(|s| s.starts_with("mxc://") && s.len() > "mxc://".len())
        .unwrap_or_default()
        .to_owned()
}

fn number(value: Option<&Value>, key: &str) -> u64 {
    value
        .and_then(|v| v.get(key))
        .and_then(|v| v.as_u64())
        .unwrap_or(0)
}

/// The msgtypes that carry an attachment, mapped to the browser's categories.
///
/// A voice message is an `m.audio` carrying MSC3245's marker; it gets its own
/// kind so the Audio tab can tell a recording from a music file, which is the
/// distinction a reader actually wants.
fn media_kind(msgtype: &str, content: &Value) -> Option<&'static str> {
    match msgtype {
        "m.image" => Some("image"),
        "m.video" => Some("video"),
        "m.audio" => {
            let voice = content.get("org.matrix.msc3245.voice").is_some()
                || content.get("m.voice").is_some();
            Some(if voice { "voice" } else { "audio" })
        }
        "m.file" => Some("file"),
        _ => None,
    }
}

/// URLs inside a message body.
///
/// Deliberately conservative and NOT a general URL parser: only `http://` and
/// `https://` starts are recognised, the run ends at whitespace, and common
/// trailing punctuation is trimmed so "see https://example.org." does not
/// produce a host of "example.org." — a link list full of near-duplicates is
/// worse than one that occasionally ends a URL early.
///
/// Bounded on purpose. A body is remote text and a message crafted with ten
/// thousand "http://" runs would otherwise turn one event into ten thousand
/// rows; MAX_LINKS_PER_EVENT is what stops a room being made unusable by one
/// message.
pub(crate) const MAX_LINKS_PER_EVENT: usize = 16;
const MAX_URL_LEN: usize = 2048;

pub(crate) fn extract_urls(body: &str) -> Vec<String> {
    let mut found = Vec::new();
    let bytes = body.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() && found.len() < MAX_LINKS_PER_EVENT {
        let rest = &body[i..];
        let start = match rest.find("http") {
            Some(p) => i + p,
            None => break,
        };
        let tail = &body[start..];
        if !(tail.starts_with("http://") || tail.starts_with("https://")) {
            // Advance past this "http" so a body full of the bare word cannot
            // spin here.
            i = start + 4;
            continue;
        }
        let end = tail
            .find(char::is_whitespace)
            .map(|p| start + p)
            .unwrap_or(body.len());
        let mut url = &body[start..end];
        if url.len() > MAX_URL_LEN {
            i = end;
            continue;
        }
        url = url.trim_end_matches(|c| matches!(c, '.' | ',' | ')' | ']' | '>' | '"' | '\'' | ';' | ':' | '!' | '?'));
        // "https://" alone is not a link.
        if url.len() > "https://".len() {
            let owned = url.to_owned();
            if !found.contains(&owned) {
                found.push(owned);
            }
        }
        i = end.max(start + 1);
    }
    found
}

/// The host of a URL, for grouping and filtering. Empty when it cannot be
/// read — never a guess, because the host is shown to the user as the
/// identity of the destination.
pub(crate) fn host_of(url: &str) -> String {
    let after = match url.find("://") {
        Some(p) => &url[p + 3..],
        None => return String::new(),
    };
    let end = after
        .find(|c| matches!(c, '/' | '?' | '#'))
        .unwrap_or(after.len());
    let authority = &after[..end];
    // Strip userinfo, which is where a lookalike host hides:
    // "https://example.org@evil.test/" is evil.test.
    let hostport = match authority.rfind('@') {
        Some(p) => &authority[p + 1..],
        None => authority,
    };
    // Strip the port, but not an IPv6 literal's colons.
    let host = if hostport.starts_with('[') {
        match hostport.find(']') {
            Some(p) => &hostport[..=p],
            None => hostport,
        }
    } else {
        match hostport.rfind(':') {
            Some(p) => &hostport[..p],
            None => hostport,
        }
    };
    host.to_ascii_lowercase()
}

/// Classify ONE raw timeline event.
///
/// `value` is the event JSON as the SDK hands it over — already decrypted
/// when decryption succeeded, still `m.room.encrypted` when it did not.
pub(crate) fn classify(value: &Value) -> Scanned {
    let mut out = Scanned::default();
    let type_str = value.get("type").and_then(|v| v.as_str()).unwrap_or("");
    if type_str == "m.room.encrypted" {
        out.undecryptable = true;
        return out;
    }
    if type_str != "m.room.message" {
        return out;
    }
    let event_id = value
        .get("event_id")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_owned();
    if event_id.is_empty() {
        return out;
    }
    let sender = value
        .get("sender")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_owned();
    let ts_ms = value
        .get("origin_server_ts")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    let Some(content) = value.get("content") else {
        return out;
    };
    // A REDACTED event keeps its type and loses its content. It is not media
    // any more and must not be listed as a broken thumbnail.
    if content.as_object().map(|o| o.is_empty()).unwrap_or(true) {
        return out;
    }
    let msgtype = content
        .get("msgtype")
        .and_then(|v| v.as_str())
        .unwrap_or("m.text");
    let body = content
        .get("body")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_owned();

    if let Some(kind) = media_kind(msgtype, content) {
        let info = content.get("info");
        // Encrypted attachments carry the key material under `file` and no
        // plain `url`; unencrypted ones carry `url`.
        let file = content.get("file");
        let encrypted = file.is_some();
        let mxc = if encrypted {
            mxc_only(file.and_then(|f| f.get("url")))
        } else {
            mxc_only(content.get("url"))
        };
        let thumbnail_mxc = {
            let plain = info.and_then(|i| i.get("thumbnail_url"));
            let enc = info
                .and_then(|i| i.get("thumbnail_file"))
                .and_then(|f| f.get("url"));
            let from_enc = mxc_only(enc);
            if from_enc.is_empty() { mxc_only(plain) } else { from_enc }
        };
        // No addressable content is not a media row. A file whose url is
        // missing or is not an mxc cannot be fetched, and a row that can only
        // ever fail is worse than no row.
        if mxc.is_empty() {
            return out;
        }
        let filename = content
            .get("filename")
            .and_then(|v| v.as_str())
            .unwrap_or(&body)
            .to_owned();
        out.entries.push(MediaEntry {
            event_id,
            sender,
            ts_ms,
            kind: kind.to_owned(),
            body,
            filename,
            mimetype: info
                .and_then(|i| i.get("mimetype"))
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_owned(),
            size: number(info, "size"),
            width: number(info, "w"),
            height: number(info, "h"),
            duration_ms: number(info, "duration"),
            mxc,
            thumbnail_mxc,
            encrypted,
            url: String::new(),
            host: String::new(),
        });
        return out;
    }

    // Everything else is a candidate for the Links category.
    for url in extract_urls(&body) {
        let host = host_of(&url);
        out.entries.push(MediaEntry {
            event_id: event_id.clone(),
            sender: sender.clone(),
            ts_ms,
            kind: "link".to_owned(),
            body: body.clone(),
            filename: String::new(),
            mimetype: String::new(),
            size: 0,
            width: 0,
            height: 0,
            duration_ms: 0,
            mxc: String::new(),
            thumbnail_mxc: String::new(),
            encrypted: false,
            host,
            url,
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image_event(encrypted: bool) -> Value {
        let mut content = json!({
            "msgtype": "m.image",
            "body": "holiday.png",
            "info": { "mimetype": "image/png", "size": 1234, "w": 800, "h": 600 },
        });
        if encrypted {
            content["file"] = json!({ "url": "mxc://example.org/enc" });
            content["info"]["thumbnail_file"] =
                json!({ "url": "mxc://example.org/encthumb" });
        } else {
            content["url"] = json!("mxc://example.org/plain");
            content["info"]["thumbnail_url"] = json!("mxc://example.org/thumb");
        }
        json!({
            "type": "m.room.message",
            "event_id": "$1",
            "sender": "@a:example.org",
            "origin_server_ts": 1_000u64,
            "content": content,
        })
    }

    #[test]
    fn an_encrypted_attachment_is_found_exactly_like_a_plain_one() {
        // The whole reason the walk is unfiltered: an encrypted room must
        // give the same answer as a public one.
        for encrypted in [false, true] {
            let scanned = classify(&image_event(encrypted));
            assert_eq!(scanned.entries.len(), 1, "encrypted={encrypted}");
            let entry = &scanned.entries[0];
            assert_eq!(entry.kind, "image");
            assert_eq!(entry.encrypted, encrypted);
            assert!(entry.mxc.starts_with("mxc://"));
            assert!(entry.thumbnail_mxc.starts_with("mxc://"));
            assert_eq!(entry.width, 800);
            assert_eq!(entry.size, 1234);
        }
    }

    #[test]
    fn an_unreadable_event_is_counted_rather_than_skipped_silently() {
        let scanned = classify(&json!({
            "type": "m.room.encrypted",
            "event_id": "$2",
            "sender": "@a:example.org",
            "content": { "algorithm": "m.megolm.v1.aes-sha2" },
        }));
        assert!(scanned.undecryptable);
        assert!(scanned.entries.is_empty());
    }

    #[test]
    fn a_redacted_attachment_is_not_a_broken_thumbnail() {
        // Redaction strips content and leaves the type. Listing it would put
        // a row in the grid that can only ever fail to load.
        let scanned = classify(&json!({
            "type": "m.room.message",
            "event_id": "$3",
            "sender": "@a:example.org",
            "content": {},
        }));
        assert!(scanned.entries.is_empty());
        assert!(!scanned.undecryptable);
    }

    /// A `url` is remote text that becomes a FETCH. Anything but an mxc is
    /// dropped rather than handed to the media layer.
    #[test]
    fn a_non_mxc_url_is_refused_rather_than_fetched() {
        for hostile in [
            json!("https://evil.test/pixel.png"),
            json!("http://127.0.0.1/admin"),
            json!("javascript:alert(1)"),
            json!("mxc://"),
            json!(""),
            json!(42),
        ] {
            let mut event = image_event(false);
            event["content"]["url"] = hostile.clone();
            assert!(
                classify(&event).entries.is_empty(),
                "{hostile:?} must not become a media row"
            );
        }
    }

    #[test]
    fn a_voice_message_is_its_own_kind() {
        let mut event = image_event(false);
        event["content"] = json!({
            "msgtype": "m.audio",
            "body": "voice.ogg",
            "url": "mxc://example.org/voice",
            "org.matrix.msc3245.voice": {},
            "info": { "mimetype": "audio/ogg", "duration": 4200 },
        });
        let scanned = classify(&event);
        assert_eq!(scanned.entries[0].kind, "voice");
        assert_eq!(scanned.entries[0].duration_ms, 4200);

        event["content"].as_object_mut().unwrap().remove("org.matrix.msc3245.voice");
        assert_eq!(classify(&event).entries[0].kind, "audio");
    }

    #[test]
    fn links_come_out_of_ordinary_message_bodies() {
        let event = json!({
            "type": "m.room.message",
            "event_id": "$4",
            "sender": "@a:example.org",
            "origin_server_ts": 5u64,
            "content": {
                "msgtype": "m.text",
                "body": "see https://example.org/a and http://b.example:8443/x?q=1.",
            },
        });
        let scanned = classify(&event);
        assert_eq!(scanned.entries.len(), 2);
        assert!(scanned.entries.iter().all(|e| e.kind == "link"));
        assert_eq!(scanned.entries[0].url, "https://example.org/a");
        assert_eq!(scanned.entries[0].host, "example.org");
        // The trailing full stop is not part of the URL, and the port is not
        // part of the host.
        assert_eq!(scanned.entries[1].url, "http://b.example:8443/x?q=1");
        assert_eq!(scanned.entries[1].host, "b.example");
    }

    /// The host is shown as the IDENTITY of the destination, so userinfo has
    /// to be stripped: `https://example.org@evil.test/` is evil.test, and
    /// showing "example.org" would be actively misleading.
    #[test]
    fn userinfo_cannot_disguise_the_host() {
        assert_eq!(host_of("https://example.org@evil.test/path"), "evil.test");
        assert_eq!(host_of("https://user:pw@evil.test:8443/"), "evil.test");
        assert_eq!(host_of("https://[2001:db8::1]:8443/x"), "[2001:db8::1]");
        assert_eq!(host_of("https://EXAMPLE.org/"), "example.org");
        assert_eq!(host_of("not a url"), "");
    }

    /// One hostile message must not be able to produce unbounded rows.
    #[test]
    fn link_extraction_is_bounded_per_event() {
        let body = "https://a.example/x ".repeat(1_000);
        let urls = extract_urls(&body);
        assert!(urls.len() <= MAX_LINKS_PER_EVENT);
        // Duplicates collapse, so the realistic flood is one row.
        assert_eq!(urls.len(), 1);

        let varied: String = (0..1_000)
            .map(|i| format!("https://h{i}.example/ "))
            .collect();
        assert_eq!(extract_urls(&varied).len(), MAX_LINKS_PER_EVENT);

        // A body full of the bare word must terminate rather than spin.
        assert!(extract_urls(&"http".repeat(10_000)).is_empty());
    }

    #[test]
    fn a_caption_and_a_filename_are_both_kept() {
        let mut event = image_event(false);
        event["content"]["body"] = json!("look at this");
        event["content"]["filename"] = json!("holiday.png");
        let scanned = classify(&event);
        assert_eq!(scanned.entries[0].body, "look at this");
        assert_eq!(scanned.entries[0].filename, "holiday.png");
    }
}

/// Where a room's independent backwards walk has reached.
///
/// `token` is the opaque `end` from the last `/messages` response; absent
/// means "begin at the live edge". `exhausted` is the separate fact that the
/// server has said there is nothing older — the two are not the same, because
/// a walk that has never run also has no token.
#[derive(Debug, Default, Clone)]
pub(crate) struct Cursor {
    pub token: Option<String>,
    pub exhausted: bool,
    pub scanned_total: u64,
    pub undecryptable_total: u64,
}
