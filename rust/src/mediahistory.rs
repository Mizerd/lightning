//! Room media history, walked independently of the live timeline, so the
//! Room Information Media tab can reach the start of accessible history
//! without moving the reader.
//!
//! The server's `url_filter = EventsWithUrl` is not used: encrypted events
//! carry no `url` on the wire (so it would find nothing in encrypted rooms),
//! and links live in ordinary `m.text` bodies. The walk is unfiltered and
//! classifies after the SDK decrypts, so encrypted and public rooms give the
//! same answer.
//!
//! Each page reports what it scanned as well as what matched, and whether
//! it reached the start of history, so the panel does not claim "all media"
//! from a partial walk or hide that some history is unreadable.

use serde_json::{json, Value};

/// One browser row, presentation-ready: C++ stores these verbatim.
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
    /// `mxc://` for media, never an http URL or the bytes; MediaBridge fetches
    /// and decrypts through the authenticated path, as for the timeline.
    pub mxc: String,
    pub thumbnail_mxc: String,
    /// Whether the attachment is encrypted, so the fetch uses the decrypting
    /// path.
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
            // The registry key a tile fetches through: the event id, as the timeline
            // keys its rows (see stored_media_from_event).
            "media_key": self.event_id,
            "url": self.url,
            "host": self.host,
        })
    }
}

/// What one scanned event contributed.
#[derive(Debug, Default, PartialEq)]
pub(crate) struct Scanned {
    pub entries: Vec<MediaEntry>,
    /// The event was `m.room.encrypted`: present but unreadable, which is
    /// different from "no media" and is reported.
    pub undecryptable: bool,
}

/// An `mxc://` URI and nothing else. `url` is remote text that becomes a
/// fetch; an http(s) value would be an unauthenticated download of someone
/// else's choosing, so non-mxc values are dropped.
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

/// Attachment msgtypes mapped to browser categories. A voice message (an
/// `m.audio` with MSC3245's marker) gets its own kind, separate from music.
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

/// Max link rows per event, so one crafted message cannot produce thousands.
pub(crate) const MAX_LINKS_PER_EVENT: usize = 16;
const MAX_URL_LEN: usize = 2048;

/// URLs inside a message body. Conservative, not a general parser: only
/// `http://` and `https://` starts, ending at whitespace, with common trailing
/// punctuation trimmed. Bounded by `MAX_LINKS_PER_EVENT`; duplicates collapse.
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
            // Skip past this "http" so a body full of the bare word cannot spin.
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

/// The host of a URL, for grouping and filtering. Empty when unreadable,
/// never a guess: it is shown as the destination's identity.
pub(crate) fn host_of(url: &str) -> String {
    let after = match url.find("://") {
        Some(p) => &url[p + 3..],
        None => return String::new(),
    };
    let end = after
        .find(|c| matches!(c, '/' | '?' | '#'))
        .unwrap_or(after.len());
    let authority = &after[..end];
    // Strip userinfo, where lookalike hosts hide: "https://example.org@evil.test/"
    // is evil.test.
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

/// The timeline's `StoredMedia` record, built from a raw event, so a browser
/// tile fetches through the registry like a timeline row (asking the server
/// to thumbnail ciphertext cannot work). Encrypted sources carry the key
/// material; plain ones just the mxc. Thumbnails come from
/// `info.thumbnail_file` / `thumbnail_url`.
pub(crate) fn stored_media_from_event(value: &Value) -> Option<crate::timeline::StoredMedia> {
    use matrix_sdk::ruma::events::room::{EncryptedFile, MediaSource};
    use matrix_sdk::ruma::OwnedMxcUri;
    fn source_of(container: &Value, file_key: &str, url_key: &str) -> Option<MediaSource> {
        if let Some(file) = container.get(file_key).filter(|f| f.is_object()) {
            return serde_json::from_value::<EncryptedFile>(file.clone())
                .ok()
                .map(|f| MediaSource::Encrypted(Box::new(f)));
        }
        container
            .get(url_key)
            .and_then(|u| u.as_str())
            .filter(|u| u.starts_with("mxc://") && u.len() > "mxc://".len())
            .map(|u| MediaSource::Plain(OwnedMxcUri::from(u)))
    }
    let content = value.get("content")?;
    let source = source_of(content, "file", "url")?;
    let info = content.get("info");
    let thumbnail = info.and_then(|i| source_of(i, "thumbnail_file", "thumbnail_url"));
    let filename = content
        .get("filename")
        .or_else(|| content.get("body"))
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_owned();
    let mimetype = info
        .and_then(|i| i.get("mimetype"))
        .and_then(|v| v.as_str())
        .map(str::to_owned);
    let declared_size = info.and_then(|i| i.get("size")).and_then(|v| v.as_u64());
    Some(crate::timeline::StoredMedia { source, thumbnail, filename, mimetype, declared_size })
}

/// Classify one raw timeline event as the SDK hands it over: decrypted when
/// possible, still `m.room.encrypted` otherwise.
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
    // A redacted event keeps its type but has no content; it is not media.
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
        // Encrypted attachments carry keys under `file` and no plain `url`.
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
        // No addressable content, no row: a row that can only fail is worse than
        // none.
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
        // An encrypted room must give the same answer as a public one.
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
        // A redacted attachment must not appear as a broken thumbnail.
        let scanned = classify(&json!({
            "type": "m.room.message",
            "event_id": "$3",
            "sender": "@a:example.org",
            "content": {},
        }));
        assert!(scanned.entries.is_empty());
        assert!(!scanned.undecryptable);
    }

    /// Non-mxc `url` values are dropped before reaching the media layer.
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
        // Trailing punctuation is not part of the URL, and the port is not part of
        // the host.
        assert_eq!(scanned.entries[1].url, "http://b.example:8443/x?q=1");
        assert_eq!(scanned.entries[1].host, "b.example");
    }

    /// Userinfo is stripped from the host: `https://example.org@evil.test/` is
    /// evil.test.
    #[test]
    fn userinfo_cannot_disguise_the_host() {
        assert_eq!(host_of("https://example.org@evil.test/path"), "evil.test");
        assert_eq!(host_of("https://user:pw@evil.test:8443/"), "evil.test");
        assert_eq!(host_of("https://[2001:db8::1]:8443/x"), "[2001:db8::1]");
        assert_eq!(host_of("https://EXAMPLE.org/"), "example.org");
        assert_eq!(host_of("not a url"), "");
    }

    /// One hostile message cannot produce unbounded rows.
    #[test]
    fn link_extraction_is_bounded_per_event() {
        let body = "https://a.example/x ".repeat(1_000);
        let urls = extract_urls(&body);
        assert!(urls.len() <= MAX_LINKS_PER_EVENT);
        // Duplicates collapse.
        assert_eq!(urls.len(), 1);

        let varied: String = (0..1_000)
            .map(|i| format!("https://h{i}.example/ "))
            .collect();
        assert_eq!(extract_urls(&varied).len(), MAX_LINKS_PER_EVENT);

        // A body full of the bare word terminates.
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

    #[test]
    fn an_encrypted_attachment_registers_its_key_material_and_its_thumbnail() {
        use matrix_sdk::ruma::events::room::MediaSource;
        let value = serde_json::json!({
            "type": "m.room.message", "event_id": "$enc", "sender": "@a:x",
            "content": {
                "msgtype": "m.image", "body": "pins.png",
                "file": { "url": "mxc://example.org/enc", "key": { "kty": "oct", "key_ops": ["encrypt","decrypt"], "alg": "A256CTR", "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "ext": true }, "iv": "AAAAAAAAAAAAAAAAAAAAAA", "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" }, "v": "v2" },
                "info": { "mimetype": "image/png", "size": 1234,
                          "thumbnail_file": { "url": "mxc://example.org/encthumb", "key": { "kty": "oct", "key_ops": ["encrypt","decrypt"], "alg": "A256CTR", "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "ext": true }, "iv": "AAAAAAAAAAAAAAAAAAAAAA", "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" }, "v": "v2" } }
            }
        });
        let media = stored_media_from_event(&value).expect("an image registers");
        assert!(matches!(media.source, MediaSource::Encrypted(_)), "the key material must travel");
        assert!(matches!(media.thumbnail, Some(MediaSource::Encrypted(_))));
        assert_eq!(media.filename, "pins.png");
        assert_eq!(media.mimetype.as_deref(), Some("image/png"));
        assert_eq!(media.declared_size, Some(1234));
    }

    #[test]
    fn a_plain_attachment_registers_plain_sources_and_text_registers_nothing() {
        use matrix_sdk::ruma::events::room::MediaSource;
        let plain = serde_json::json!({ "event_id": "$p", "content": {
            "msgtype": "m.image", "body": "a.png", "url": "mxc://example.org/plain",
            "info": { "thumbnail_url": "mxc://example.org/plainthumb" } } });
        let media = stored_media_from_event(&plain).expect("registers");
        assert!(matches!(media.source, MediaSource::Plain(_)));
        assert!(matches!(media.thumbnail, Some(MediaSource::Plain(_))));
        let text = serde_json::json!({ "event_id": "$t", "content": { "msgtype": "m.text", "body": "hi" } });
        assert!(stored_media_from_event(&text).is_none());
        let bogus = serde_json::json!({ "event_id": "$b", "content": { "msgtype": "m.image", "body": "x", "url": "https://not-mxc/" } });
        assert!(stored_media_from_event(&bogus).is_none(), "only mxc sources register");
    }
}

/// Where a room's backwards walk has reached. `token` is the last
/// `/messages` `end` (absent: start at the live edge); `exhausted` means the
/// server said nothing is older. They differ because a walk that never ran
/// also has no token.
#[derive(Debug, Default, Clone)]
pub(crate) struct Cursor {
    pub token: Option<String>,
    pub exhausted: bool,
    pub scanned_total: u64,
    pub undecryptable_total: u64,

}
