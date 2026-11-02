
use serde_json::{json, Value};
use std::collections::HashMap;

pub(crate) const MAX_ROWS: u64 = 100_000;

pub(crate) const PAGE_SIZE: u64 = 100;

const MAX_PENDING_EDITS: usize = 8192;
const MAX_EDITORS_PER_EVENT: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Range {
    Latest(u64),
    All,
    Between { from_ms: u64, to_ms: u64 },
}

impl Range {
    pub(crate) fn from_ffi(mode: i32, limit: u64, from_ms: u64, to_ms: u64) -> Result<Self, String> {
        match mode {
            0 => {
                if limit == 0 {
                    return Err("a message count is required".to_owned());
                }
                Ok(Range::Latest(limit.min(MAX_ROWS)))
            }
            1 => Ok(Range::All),
            2 => {
                let to_ms = if to_ms == 0 { u64::MAX } else { to_ms };
                if from_ms > to_ms {
                    return Err("the start date is after the end date".to_owned());
                }
                Ok(Range::Between { from_ms, to_ms })
            }
            _ => Err("unknown export range".to_owned()),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum StopReason {
    Start,
    Limit,
    Date,
    Cap,
}

impl StopReason {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            StopReason::Start => "start",
            StopReason::Limit => "limit",
            StopReason::Date => "date",
            StopReason::Cap => "cap",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct ExportRow {
    pub event_id: String,
    pub sender: String,
    pub ts_ms: u64,
    pub kind: String,
    pub body: String,
    /// `org.matrix.custom.html` only, and untrusted: C++ sanitizes it before
    /// it reaches a file.
    pub formatted_body: String,
    pub thread_root: String,
    pub reply_to: String,
    pub edited: bool,
    pub filename: String,
    pub mimetype: String,
    pub size: u64,
}

impl ExportRow {
    pub(crate) fn is_attachment(&self) -> bool {
        matches!(
            self.kind.as_str(),
            "image" | "video" | "audio" | "voice" | "file" | "sticker"
        )
    }

    /// The row as the C++ side reads it. `media_key` is the opaque handle (or
    /// empty); there is deliberately no `mxc` field.
    pub(crate) fn to_json(&self, sender_name: &str, media_key: &str) -> Value {
        json!({
            "event_id": self.event_id,
            "sender": self.sender,
            "sender_name": sender_name,
            "ts_ms": self.ts_ms,
            "kind": self.kind,
            "body": self.body,
            "formatted_body": self.formatted_body,
            "thread_root": self.thread_root,
            "reply_to": self.reply_to,
            "edited": self.edited,
            "filename": self.filename,
            "mimetype": self.mimetype,
            "size": self.size,
            "media_key": media_key,
        })
    }
}

pub(crate) fn media_handle(op_id: u64, event_id: &str) -> String {
    format!("{}{op_id}:{event_id}", EXPORT_KEY_PREFIX)
}

pub(crate) const EXPORT_KEY_PREFIX: &str = "export:";

pub(crate) fn handle_prefix(op_id: u64) -> String {
    format!("{}{op_id}:", EXPORT_KEY_PREFIX)
}

#[derive(Debug, Clone, PartialEq)]
struct Edit {
    sender: String,
    body: String,
    formatted_body: String,
}

#[derive(Debug)]
pub(crate) struct WalkState {
    range: Range,
    pub rows: u64,
    pub scanned: u64,
    pub undecryptable: u64,
    edits: HashMap<String, Vec<Edit>>,
    pending_edits: usize,
    older_seen: bool,
    pub stop: Option<StopReason>,
}

fn str_of<'a>(value: Option<&'a Value>) -> &'a str {
    value.and_then(|v| v.as_str()).unwrap_or("")
}

fn formatted_of(content: &Value) -> String {
    if str_of(content.get("format")) == "org.matrix.custom.html" {
        str_of(content.get("formatted_body")).to_owned()
    } else {
        String::new()
    }
}

fn poll_text(content: &Value) -> String {
    for start_key in ["org.matrix.msc3381.poll.start", "m.poll.start", "m.poll"] {
        if let Some(question) = content.get(start_key).and_then(|s| s.get("question")) {
            for text_key in ["org.matrix.msc1767.text", "m.text", "body"] {
                let text = str_of(question.get(text_key));
                if !text.is_empty() {
                    return text.to_owned();
                }
            }
            if let Some(first) = question
                .get("m.text")
                .and_then(|t| t.as_array())
                .and_then(|a| a.first())
            {
                let text = str_of(first.get("body"));
                if !text.is_empty() {
                    return text.to_owned();
                }
            }
        }
    }
    for key in ["org.matrix.msc1767.text", "body"] {
        let text = str_of(content.get(key));
        if !text.is_empty() {
            return text.to_owned();
        }
    }
    String::new()
}

pub(crate) fn strip_reply_fallback(body: &str) -> String {
    if !body.starts_with("> ") && !body.starts_with(">\n") {
        return body.to_owned();
    }
    let mut lines = body.split('\n').peekable();
    while let Some(line) = lines.peek() {
        if line.starts_with('>') {
            lines.next();
        } else {
            break;
        }
    }
    if lines.peek().map(|l| l.is_empty()).unwrap_or(false) {
        lines.next();
    }
    lines.collect::<Vec<_>>().join("\n")
}

fn kind_for(type_str: &str, content: &Value) -> &'static str {
    match type_str {
        "m.sticker" => "sticker",
        "m.poll.start" | "org.matrix.msc3381.poll.start" => "poll",
        _ => match str_of(content.get("msgtype")) {
            "m.notice" => "notice",
            "m.emote" => "emote",
            "m.location" => "location",
            "m.image" => "image",
            "m.video" => "video",
            "m.file" => "file",
            "m.audio" => {
                if content.get("org.matrix.msc3245.voice").is_some()
                    || content.get("m.voice").is_some()
                {
                    "voice"
                } else {
                    "audio"
                }
            }
            _ => "text",
        },
    }
}

impl WalkState {
    pub(crate) fn new(range: Range) -> Self {
        Self {
            range,
            rows: 0,
            scanned: 0,
            undecryptable: 0,
            edits: HashMap::new(),
            pending_edits: 0,
            older_seen: false,
            stop: None,
        }
    }

    fn classify(&mut self, value: &Value) -> Option<ExportRow> {
        let type_str = str_of(value.get("type"));
        let event_id = str_of(value.get("event_id")).to_owned();
        if event_id.is_empty() {
            return None;
        }
        let sender = str_of(value.get("sender")).to_owned();
        let ts_ms = value
            .get("origin_server_ts")
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        let base = ExportRow { event_id: event_id.clone(), sender: sender.clone(), ts_ms, ..Default::default() };
        if !matches!(
            type_str,
            "m.room.encrypted"
                | "m.room.message"
                | "m.sticker"
                | "m.poll.start"
                | "org.matrix.msc3381.poll.start"
        ) {
            return None;
        }
        let content = value.get("content");
        // Checked BEFORE the encrypted branch: a redacted `m.room.encrypted`
        // keeps its type with empty content, and it is a deleted message, not
        // one this device cannot read.
        let redacted = value
            .get("unsigned")
            .and_then(|u| u.get("redacted_because"))
            .is_some()
            || content
                .and_then(|c| c.as_object())
                .map(|o| o.is_empty())
                .unwrap_or(true);
        if redacted {
            return Some(ExportRow { kind: "redacted".to_owned(), ..base });
        }
        if type_str == "m.room.encrypted" {
            let rel_type = str_of(
                content
                    .and_then(|c| c.get("m.relates_to"))
                    .and_then(|r| r.get("rel_type")),
            );
            if matches!(rel_type, "m.annotation" | "m.replace") {
                return None;
            }
            return Some(ExportRow { kind: "undecryptable".to_owned(), ..base });
        }
        let content = content?;
        let relation = content.get("m.relates_to");
        let rel_type = str_of(relation.and_then(|r| r.get("rel_type")));
        if type_str == "m.room.message" && rel_type == "m.replace" {
            let target = str_of(relation.and_then(|r| r.get("event_id")));
            let new_content = content.get("m.new_content");
            if let (false, Some(new_content)) = (target.is_empty(), new_content) {
                if self.pending_edits < MAX_PENDING_EDITS {
                    let list = self.edits.entry(target.to_owned()).or_default();
                    if list.len() < MAX_EDITORS_PER_EVENT
                        && !list.iter().any(|e| e.sender == sender)
                    {
                        list.push(Edit {
                            sender: sender.clone(),
                            body: str_of(new_content.get("body")).to_owned(),
                            formatted_body: formatted_of(new_content),
                        });
                        self.pending_edits += 1;
                    }
                }
            }
            return None;
        }

        let kind = kind_for(type_str, content);
        let thread_root = if rel_type == "m.thread" {
            str_of(relation.and_then(|r| r.get("event_id"))).to_owned()
        } else {
            String::new()
        };
        // In a thread, `m.in_reply_to` with `is_falling_back` is only there for
        // clients without threads; it is not a reply the sender made.
        let falling_back = relation
            .and_then(|r| r.get("is_falling_back"))
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
        let reply_to = if falling_back {
            String::new()
        } else {
            str_of(relation.and_then(|r| r.get("m.in_reply_to")).and_then(|r| r.get("event_id")))
                .to_owned()
        };

        let mut body = if kind == "poll" {
            poll_text(content)
        } else {
            str_of(content.get("body")).to_owned()
        };
        let mut formatted_body = formatted_of(content);
        let mut edited = false;
        // Only the original sender's edit replaces content (the spec's rule): an
        // edit by anyone else is ignored, never applied.
        if let Some(list) = self.edits.remove(&event_id) {
            self.pending_edits = self.pending_edits.saturating_sub(list.len());
            if let Some(edit) = list.into_iter().find(|e| e.sender == sender) {
                body = edit.body;
                formatted_body = edit.formatted_body;
                edited = true;
            }
        }
        if !reply_to.is_empty() && !edited {
            body = strip_reply_fallback(&body);
        }

        let mut row = ExportRow {
            kind: kind.to_owned(),
            thread_root,
            reply_to,
            edited,
            ..base
        };
        if row.is_attachment() {
            let info = content.get("info");
            row.filename = content
                .get("filename")
                .and_then(|v| v.as_str())
                .map(str::to_owned)
                .unwrap_or_else(|| str_of(content.get("body")).to_owned());
            row.mimetype = str_of(info.and_then(|i| i.get("mimetype"))).to_owned();
            row.size = info
                .and_then(|i| i.get("size"))
                .and_then(|v| v.as_u64())
                .unwrap_or(0);
        }
        row.body = body;
        row.formatted_body = formatted_body;
        Some(row)
    }

    pub(crate) fn consume(&mut self, value: &Value) -> Option<ExportRow> {
        if self.stop.is_some() {
            return None;
        }
        self.scanned += 1;
        let row = self.classify(value);
        let ts = value
            .get("origin_server_ts")
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        if let Range::Between { from_ms, to_ms } = self.range {
            if ts != 0 && ts < from_ms {
                self.older_seen = true;
            }
            if ts < from_ms || ts > to_ms {
                return None;
            }
        }
        let row = row?;
        if row.kind == "undecryptable" {
            self.undecryptable += 1;
        }
        self.rows += 1;
        if let Range::Latest(n) = self.range {
            if self.rows >= n {
                self.stop = Some(StopReason::Limit);
            }
        }
        if self.stop.is_none() && self.rows >= MAX_ROWS {
            self.stop = Some(StopReason::Cap);
        }
        Some(row)
    }

    pub(crate) fn end_of_page(&mut self, exhausted: bool) {
        if self.stop.is_some() {
            return;
        }
        if self.older_seen {
            self.stop = Some(StopReason::Date);
        } else if exhausted {
            self.stop = Some(StopReason::Start);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(id: &str, sender: &str, ts: u64, body: &str) -> Value {
        json!({
            "type": "m.room.message",
            "event_id": id,
            "sender": sender,
            "origin_server_ts": ts,
            "content": { "msgtype": "m.text", "body": body },
        })
    }

    fn edit(id: &str, sender: &str, ts: u64, target: &str, body: &str) -> Value {
        json!({
            "type": "m.room.message",
            "event_id": id,
            "sender": sender,
            "origin_server_ts": ts,
            "content": {
                "msgtype": "m.text",
                "body": format!("* {body}"),
                "m.new_content": {
                    "msgtype": "m.text",
                    "body": body,
                    "format": "org.matrix.custom.html",
                    "formatted_body": format!("<b>{body}</b>"),
                },
                "m.relates_to": { "rel_type": "m.replace", "event_id": target },
            },
        })
    }

    fn rows_of(state: &mut WalkState, page: &[Value]) -> Vec<ExportRow> {
        page.iter().filter_map(|v| state.consume(v)).collect()
    }

    #[test]
    fn only_the_original_senders_newest_edit_replaces_the_text() {
        let mut state = WalkState::new(Range::All);
        let page = vec![
            edit("$e3", "@mallory:example.org", 400, "$1", "forged"),
            edit("$e2", "@ann:example.org", 300, "$1", "second"),
            edit("$e1", "@ann:example.org", 200, "$1", "first"),
            text("$1", "@ann:example.org", 100, "original"),
        ];
        let rows = rows_of(&mut state, &page);
        assert_eq!(rows.len(), 1, "an edit must never be a row of its own");
        let row = &rows[0];
        assert_eq!(row.event_id, "$1");
        assert_eq!(row.body, "second");
        assert_eq!(row.formatted_body, "<b>second</b>");
        assert!(row.edited);
    }

    #[test]
    fn an_edit_by_someone_else_alone_changes_nothing() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            edit("$e1", "@mallory:example.org", 200, "$1", "forged"),
            text("$1", "@ann:example.org", 100, "original"),
        ]);
        assert_eq!(rows[0].body, "original");
        assert!(!rows[0].edited);
    }

    #[test]
    fn a_redacted_message_is_a_deleted_row_with_no_content() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.message",
            "event_id": "$r",
            "sender": "@ann:example.org",
            "origin_server_ts": 5u64,
            "content": {},
            "unsigned": { "redacted_because": { "type": "m.room.redaction" } },
        })]);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].kind, "redacted");
        assert!(rows[0].body.is_empty());
        assert!(rows[0].formatted_body.is_empty());
    }

    #[test]
    fn a_thread_reply_is_flagged_and_its_fallback_reply_is_not_a_reply() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.message",
            "event_id": "$t",
            "sender": "@ann:example.org",
            "origin_server_ts": 9u64,
            "content": {
                "msgtype": "m.text",
                "body": "in the thread",
                "m.relates_to": {
                    "rel_type": "m.thread",
                    "event_id": "$root",
                    "is_falling_back": true,
                    "m.in_reply_to": { "event_id": "$latest" },
                },
            },
        }), json!({
            "type": "m.room.message",
            "event_id": "$reply",
            "sender": "@bob:example.org",
            "origin_server_ts": 8u64,
            "content": {
                "msgtype": "m.text",
                "body": "> <@ann:example.org> quoted\n> more\n\nthe answer",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$q" } },
            },
        })]);
        assert_eq!(rows[0].thread_root, "$root");
        assert!(rows[0].reply_to.is_empty());
        assert_eq!(rows[1].thread_root, "");
        assert_eq!(rows[1].reply_to, "$q");
        assert_eq!(rows[1].body, "the answer", "the quoted fallback stays out");
    }

    #[test]
    fn an_attachment_row_carries_a_handle_and_never_a_url() {
        let mut state = WalkState::new(Range::All);
        let event = json!({
            "type": "m.room.message",
            "event_id": "$img",
            "sender": "@ann:example.org",
            "origin_server_ts": 7u64,
            "content": {
                "msgtype": "m.image",
                "body": "holiday.png",
                "url": "mxc://example.org/secretmedia",
                "info": {
                    "mimetype": "image/png",
                    "size": 1234,
                    "thumbnail_url": "mxc://example.org/thumb",
                },
            },
        });
        let rows = rows_of(&mut state, &[event.clone()]);
        let row = &rows[0];
        assert_eq!(row.kind, "image");
        assert!(row.is_attachment());
        assert_eq!(row.filename, "holiday.png");
        assert_eq!(row.mimetype, "image/png");
        assert_eq!(row.size, 1234);
        assert!(crate::mediahistory::stored_media_from_event(&event).is_some());
        let handle = media_handle(42, &row.event_id);
        assert!(handle.starts_with(&handle_prefix(42)));
        let serialized = row.to_json("Ann", &handle).to_string();
        assert!(!serialized.contains("mxc://"), "{serialized}");
        assert!(!serialized.contains("http"), "{serialized}");
        assert!(serialized.contains("export:42:$img"));
    }

    #[test]
    fn an_unreadable_event_is_a_counted_row() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.encrypted",
            "event_id": "$u",
            "sender": "@ann:example.org",
            "origin_server_ts": 3u64,
            "content": { "algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "x" },
        })]);
        assert_eq!(rows[0].kind, "undecryptable");
        assert_eq!(state.undecryptable, 1);
    }

    #[test]
    fn a_redacted_encrypted_event_is_deleted_not_undecryptable() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "m.room.encrypted",
            "event_id": "$gone",
            "sender": "@ann:example.org",
            "origin_server_ts": 6u64,
            "content": {},
            "unsigned": { "redacted_because": { "type": "m.room.redaction" } },
        })]);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].kind, "redacted");
        assert_eq!(state.undecryptable, 0);
    }

    #[test]
    fn an_unreadable_reaction_or_edit_is_not_a_row_and_not_counted() {
        let mut state = WalkState::new(Range::Latest(2));
        let encrypted = |id: &str, rel: &str| json!({
            "type": "m.room.encrypted",
            "event_id": id,
            "sender": "@ann:example.org",
            "origin_server_ts": 10u64,
            "content": {
                "algorithm": "m.megolm.v1.aes-sha2",
                "ciphertext": "x",
                "m.relates_to": { "rel_type": rel, "event_id": "$target" },
            },
        });
        let rows = rows_of(&mut state, &[
            encrypted("$r", "m.annotation"),
            encrypted("$e", "m.replace"),
            text("$1", "@a:b", 9, "one"),
            text("$2", "@a:b", 8, "two"),
        ]);
        let ids: Vec<&str> = rows.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$1", "$2"], "relations used up the latest-N budget");
        assert_eq!(state.undecryptable, 0);
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[encrypted("$t", "m.thread")]);
        assert_eq!(rows[0].kind, "undecryptable");
        assert_eq!(state.undecryptable, 1);
    }

    #[test]
    fn state_events_reactions_and_redaction_events_are_not_rows() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[
            json!({ "type": "m.room.member", "event_id": "$m", "sender": "@a:b",
                    "origin_server_ts": 1u64, "state_key": "@a:b",
                    "content": { "membership": "join" } }),
            json!({ "type": "m.reaction", "event_id": "$x", "sender": "@a:b",
                    "origin_server_ts": 1u64, "content": {} }),
            json!({ "type": "m.room.redaction", "event_id": "$y", "sender": "@a:b",
                    "origin_server_ts": 1u64, "redacts": "$z", "content": {} }),
        ]);
        assert!(rows.is_empty());
        assert_eq!(state.scanned, 3);
    }

    #[test]
    fn latest_n_stops_at_exactly_n_rows() {
        let mut state = WalkState::new(Range::Latest(3));
        let page: Vec<Value> = (0..10)
            .map(|i| text(&format!("${i}"), "@a:b", 1000 - i, "hi"))
            .collect();
        let rows = rows_of(&mut state, &page);
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].event_id, "$0", "the NEWEST rows are kept");
        assert_eq!(state.stop, Some(StopReason::Limit));
        assert!(state.consume(&text("$x", "@a:b", 1, "late")).is_none());
        state.end_of_page(true);
        assert_eq!(state.stop, Some(StopReason::Limit));
    }

    #[test]
    fn a_date_range_keeps_its_days_and_ends_after_the_page_that_passes_it() {
        let mut state = WalkState::new(Range::Between { from_ms: 100, to_ms: 200 });
        let rows = rows_of(&mut state, &[
            text("$new", "@a:b", 250, "too new"),
            text("$in1", "@a:b", 200, "edge"),
            text("$in2", "@a:b", 150, "middle"),
            text("$old", "@a:b", 99, "too old"),
            text("$skew", "@a:b", 120, "skewed clock"),
        ]);
        let ids: Vec<&str> = rows.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, vec!["$in1", "$in2", "$skew"]);
        assert_eq!(state.stop, None, "a page is finished before the walk stops");
        state.end_of_page(false);
        assert_eq!(state.stop, Some(StopReason::Date));
    }

    #[test]
    fn the_start_of_the_room_ends_an_unbounded_walk() {
        let mut state = WalkState::new(Range::All);
        rows_of(&mut state, &[text("$1", "@a:b", 1, "first ever")]);
        state.end_of_page(false);
        assert_eq!(state.stop, None);
        state.end_of_page(true);
        assert_eq!(state.stop, Some(StopReason::Start));
    }

    #[test]
    fn the_row_cap_holds_for_every_range() {
        let mut state = WalkState::new(Range::All);
        state.rows = MAX_ROWS - 1;
        assert!(state.consume(&text("$1", "@a:b", 1, "x")).is_some());
        assert_eq!(state.stop, Some(StopReason::Cap));
        assert_eq!(Range::from_ffi(0, 10 * MAX_ROWS, 0, 0), Ok(Range::Latest(MAX_ROWS)));
    }

    #[test]
    fn ffi_ranges_are_validated() {
        assert!(Range::from_ffi(0, 0, 0, 0).is_err());
        assert!(Range::from_ffi(2, 0, 500, 100).is_err());
        assert_eq!(
            Range::from_ffi(2, 0, 500, 0),
            Ok(Range::Between { from_ms: 500, to_ms: u64::MAX })
        );
        assert!(Range::from_ffi(7, 0, 0, 0).is_err());
    }

    #[test]
    fn a_poll_exports_its_question() {
        let mut state = WalkState::new(Range::All);
        let rows = rows_of(&mut state, &[json!({
            "type": "org.matrix.msc3381.poll.start",
            "event_id": "$p",
            "sender": "@a:b",
            "origin_server_ts": 4u64,
            "content": {
                "org.matrix.msc3381.poll.start": {
                    "question": { "org.matrix.msc1767.text": "Lunch?" },
                    "answers": [],
                },
            },
        })]);
        assert_eq!(rows[0].kind, "poll");
        assert_eq!(rows[0].body, "Lunch?");
    }

    #[test]
    fn a_reply_fallback_is_stripped_only_from_its_quote() {
        assert_eq!(strip_reply_fallback("> <@a:b> hi\n\nanswer"), "answer");
        assert_eq!(strip_reply_fallback("plain"), "plain");
        assert_eq!(strip_reply_fallback("> only quote"), "");
    }
}
