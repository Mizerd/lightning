
use std::collections::HashSet;
use std::path::Path;

use rusqlite::{params, Connection, OptionalExtension};

pub(crate) const INDEX_FILE: &str = "lightning-search.sqlite3";

pub(crate) const MAX_ROWS: i64 = 250_000;

pub(crate) const MIN_QUERY_CHARS: usize = 3;

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Hit {
    pub event_id: String,
    pub room_id: String,
    pub sender: String,
    pub sender_name: String,
    pub body: String,
    pub msgtype: String,
    pub ts: i64,
    pub filename: String,
}

const MAX_FILENAME_BYTES: usize = 255;

fn is_attachment_msgtype(msgtype: &str) -> bool {
    matches!(msgtype, "m.image" | "m.video" | "m.audio" | "m.file")
}

/// Bidirectional controls (embeddings/overrides U+202A-202E, isolates
/// U+2066-2069): not `Cc`, but they let `evil\u{202E}gpj.exe` display as
/// `evilexe.jpg`, in a line shown next to a result.
fn is_bidi_control(c: char) -> bool {
    matches!(c, '\u{202A}'..='\u{202E}' | '\u{2066}'..='\u{2069}')
}

/// A file name as stored: control and bidi-control characters removed, trimmed,
/// bounded to [`MAX_FILENAME_BYTES`] bytes. Only the NAME is kept; never a URL,
/// a key or any other media metadata.
fn clean_filename(raw: &str) -> String {
    let mut out = String::new();
    for c in raw.chars().filter(|c| !c.is_control() && !is_bidi_control(*c)) {
        if out.len() + c.len_utf8() > MAX_FILENAME_BYTES {
            break;
        }
        out.push(c);
    }
    out.trim().to_owned()
}

const SCHEMA_VERSION: i64 = 1;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct IndexStats {
    pub messages: i64,
    pub rooms: i64,
}

pub(crate) fn match_expression(query: &str) -> String {
    // Not folded here: `remove_diacritics 2` on the trigram table folds the
    // query too, and a second implementation could drift.
    format!("\"{}\"", query.trim().replace('"', "\"\""))
}

/// True when a query is long enough for trigram to match. Counted in
/// characters: three Chinese characters are nine bytes and a valid query.
pub(crate) fn query_is_long_enough(query: &str) -> bool {
    query.trim().chars().count() >= MIN_QUERY_CHARS
}

pub(crate) struct SearchIndex {
    conn: Connection,
    generation: std::sync::atomic::AtomicU64,
}

static GENERATIONS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

fn next_generation() -> u64 {
    GENERATIONS.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

impl SearchIndex {
    pub(crate) fn open_in(store_dir: &Path) -> Result<Self, String> {
        let path = store_dir.join(INDEX_FILE);
        let conn = Connection::open(&path)
            .map_err(|e| format!("cannot open the search index: {e}"))?;
        Self::from_connection(conn)
    }

    #[cfg(test)]
    pub(crate) fn open_in_memory() -> Result<Self, String> {
        let conn = Connection::open_in_memory().map_err(|e| e.to_string())?;
        Self::from_connection(conn)
    }

    fn from_connection(conn: Connection) -> Result<Self, String> {
        let index = Self {
            conn,
            generation: std::sync::atomic::AtomicU64::new(next_generation()),
        };
        index.ensure_schema()?;
        Ok(index)
    }

    fn ensure_schema(&self) -> Result<(), String> {
        // WAL so indexing writes do not block search reads; NORMAL synchronous,
        // since losing an index tail on power loss only costs a re-index.
        // journal_size_limit: without one the WAL file keeps its high-water
        // size for good (measured: 4 MiB beside a 143 KiB index), and one big
        // room batch from "index all rooms" grows it past that, since nothing
        // checkpoints inside a transaction. Truncated back to 1 MiB after a
        // checkpoint instead.
        self.conn
            .execute_batch(
                "PRAGMA journal_mode=WAL;
                 PRAGMA synchronous=NORMAL;
                 PRAGMA journal_size_limit=1048576;

                 CREATE TABLE IF NOT EXISTS messages(
                     id            INTEGER PRIMARY KEY,
                     event_id      TEXT NOT NULL UNIQUE,
                     room_id       TEXT NOT NULL,
                     sender        TEXT NOT NULL,
                     sender_name   TEXT NOT NULL DEFAULT '',
                     body          TEXT NOT NULL DEFAULT '',
                     msgtype       TEXT NOT NULL DEFAULT '',
                     ts            INTEGER NOT NULL
                     -- `filename` and `edit_id` are added by migrate(): an index
                     -- made before them has neither, and ALTER TABLE ADD COLUMN
                     -- is the one change that costs it nothing.
                 );
                 CREATE INDEX IF NOT EXISTS messages_room_ts
                     ON messages(room_id, ts DESC);
                 CREATE INDEX IF NOT EXISTS messages_ts ON messages(ts);

                 -- EXTERNAL CONTENT: the FTS table stores only the index and
                 -- reads the text from `messages`, so the folded copies are
                 -- not held twice. content_rowid ties them together.
                 -- THE TOKENIZER FOLDS, so nothing here stores a second
                 -- folded copy of the text. Measured, not assumed:
                 -- trigramCanFoldDiacriticsItself pins that
                 -- `remove_diacritics 2` on a trigram table makes both the
                 -- stored text and the QUERY case- and accent-insensitive, so
                 -- `koln` and `KÖLN` both find `Köln`. Carrying folded columns
                 -- as well would spend a fifth of the database on a duplicate
                 -- and add a way for the two copies to disagree.
                 CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
                     body,
                     sender_name,
                     content='messages',
                     content_rowid='id',
                     tokenize='trigram remove_diacritics 2'
                 );

                 -- External content means FTS5 does NOT see writes to the
                 -- content table on its own; these triggers are the contract.
                 CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
                     INSERT INTO messages_fts(rowid, body, sender_name)
                     VALUES (new.id, new.body, new.sender_name);
                 END;
                 CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
                     INSERT INTO messages_fts(messages_fts, rowid, body, sender_name)
                     VALUES ('delete', old.id, old.body, old.sender_name);
                 END;
                 -- messages_au is (re)created by migrate(), with a WHEN clause.",
            )
            .map_err(|e| format!("cannot prepare the search index: {e}"))?;
        self.migrate()
    }

    fn migrate(&self) -> Result<(), String> {
        self.conn
            .execute_batch("BEGIN IMMEDIATE;")
            .map_err(|e| format!("cannot migrate the search index: {e}"))?;
        match self.migrate_steps() {
            Ok(()) => self
                .conn
                .execute_batch("COMMIT;")
                .map_err(|e| format!("cannot migrate the search index: {e}")),
            Err(e) => {
                let _ = self.conn.execute_batch("ROLLBACK;");
                Err(e)
            }
        }
    }

    fn migrate_steps(&self) -> Result<(), String> {
        let version: i64 = self
            .conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .map_err(|e| format!("cannot read the search index version: {e}"))?;
        let fail = |e: rusqlite::Error| format!("cannot migrate the search index: {e}");
        for column in ["filename", "edit_id"] {
            let present: bool = self
                .conn
                .prepare("SELECT 1 FROM pragma_table_info('messages') WHERE name = ?1")
                .and_then(|mut stmt| stmt.exists([column]))
                .map_err(|e| format!("cannot inspect the search index: {e}"))?;
            if !present {
                self.conn
                    .execute_batch(&format!("ALTER TABLE messages ADD COLUMN {column} TEXT;"))
                    .map_err(fail)?;
            }
        }
        if version < SCHEMA_VERSION {
            self.conn
                .execute_batch("DROP TRIGGER IF EXISTS messages_au;")
                .map_err(fail)?;
        }
        self.conn
            .execute_batch(
                "CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages
                 WHEN old.body IS NOT new.body OR old.sender_name IS NOT new.sender_name
                 BEGIN
                     INSERT INTO messages_fts(messages_fts, rowid, body, sender_name)
                     VALUES ('delete', old.id, old.body, old.sender_name);
                     INSERT INTO messages_fts(rowid, body, sender_name)
                     VALUES (new.id, new.body, new.sender_name);
                 END;

                 -- File names: external content again, rows only for non-empty
                 -- names, so the 'delete' command always names what was stored.
                 -- Redacting an edit looks rows up by `edit_id`. Partial, so it
                 -- holds only rows that were edited, and it is empty when
                 -- created: no write burst.
                 CREATE INDEX IF NOT EXISTS messages_edit_id
                     ON messages(edit_id) WHERE edit_id IS NOT NULL;
                 CREATE VIRTUAL TABLE IF NOT EXISTS messages_fn_fts USING fts5(
                     filename,
                     content='messages',
                     content_rowid='id',
                     tokenize='trigram remove_diacritics 2'
                 );
                 CREATE TRIGGER IF NOT EXISTS messages_fn_ai AFTER INSERT ON messages
                 BEGIN
                     INSERT INTO messages_fn_fts(rowid, filename)
                     SELECT new.id, new.filename WHERE ifnull(new.filename, '') <> '';
                 END;
                 CREATE TRIGGER IF NOT EXISTS messages_fn_ad AFTER DELETE ON messages
                 BEGIN
                     INSERT INTO messages_fn_fts(messages_fn_fts, rowid, filename)
                     SELECT 'delete', old.id, old.filename WHERE ifnull(old.filename, '') <> '';
                 END;
                 CREATE TRIGGER IF NOT EXISTS messages_fn_au AFTER UPDATE ON messages
                 WHEN old.filename IS NOT new.filename
                 BEGIN
                     INSERT INTO messages_fn_fts(messages_fn_fts, rowid, filename)
                     SELECT 'delete', old.id, old.filename WHERE ifnull(old.filename, '') <> '';
                     INSERT INTO messages_fn_fts(rowid, filename)
                     SELECT new.id, new.filename WHERE ifnull(new.filename, '') <> '';
                 END;",
            )
            .map_err(fail)?;
        if version < SCHEMA_VERSION {
            self.conn
                .execute_batch(&format!("PRAGMA user_version = {SCHEMA_VERSION};"))
                .map_err(fail)?;
        }
        Ok(())
    }

    pub(crate) fn upsert(
        &self,
        event_id: &str,
        room_id: &str,
        sender: &str,
        sender_name: &str,
        body: &str,
        msgtype: &str,
        ts: i64,
    ) -> Result<bool, String> {
        self.upsert_named(event_id, room_id, sender, sender_name, body, msgtype, ts, None)
    }

    pub(crate) fn upsert_named(
        &self,
        event_id: &str,
        room_id: &str,
        sender: &str,
        sender_name: &str,
        body: &str,
        msgtype: &str,
        ts: i64,
        filename: Option<&str>,
    ) -> Result<bool, String> {
        let filename = filename.map(clean_filename);
        if event_id.is_empty() || room_id.is_empty() {
            return Ok(false);
        }
        if body.trim().is_empty() {
            return Ok(false);
        }
        self.conn
            .execute(
                "INSERT INTO messages
                    (event_id, room_id, sender, sender_name, body, msgtype, ts, filename)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
                 ON CONFLICT(event_id) DO UPDATE SET
                     body        = excluded.body,
                     sender_name = excluded.sender_name,
                     msgtype     = excluded.msgtype,
                     filename    = COALESCE(excluded.filename, messages.filename)
                 WHERE messages.body IS NOT excluded.body
                    OR messages.sender_name IS NOT excluded.sender_name
                    OR messages.msgtype IS NOT excluded.msgtype
                    OR (excluded.filename IS NOT NULL
                        AND messages.filename IS NOT excluded.filename)",
                params![
                    event_id,
                    room_id,
                    sender,
                    sender_name,
                    body,
                    msgtype,
                    ts,
                    filename,
                ],
            )
            .map(|changed| changed != 0)
            .map_err(|e| format!("cannot index a message: {e}"))
    }

    pub(crate) fn stale_filename_ids(
        &self,
        event_ids: &[String],
    ) -> Result<HashSet<String>, String> {
        let mut out = HashSet::new();
        for slice in event_ids.chunks(500) {
            if slice.is_empty() {
                continue;
            }
            let placeholders = std::iter::repeat("?")
                .take(slice.len())
                .collect::<Vec<_>>()
                .join(",");
            let sql = format!(
                "SELECT event_id FROM messages
                 WHERE filename IS NULL
                   AND msgtype IN ('m.image','m.video','m.audio','m.file')
                   AND event_id IN ({placeholders})"
            );
            let mut stmt = self.conn.prepare(&sql).map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map(rusqlite::params_from_iter(slice.iter()), |r| {
                    r.get::<_, String>(0)
                })
                .map_err(|e| e.to_string())?;
            for row in rows {
                out.insert(row.map_err(|e| e.to_string())?);
            }
        }
        Ok(out)
    }

    pub(crate) fn fill_filename(&self, event_id: &str, filename: &str) -> Result<bool, String> {
        self.conn
            .execute(
                "UPDATE messages SET filename = ?2 WHERE event_id = ?1 AND filename IS NULL",
                params![event_id, clean_filename(filename)],
            )
            .map(|changed| changed != 0)
            .map_err(|e| format!("cannot record a file name: {e}"))
    }

    pub(crate) fn sender_of(&self, event_id: &str) -> Result<Option<String>, String> {
        self.conn
            .query_row(
                "SELECT sender FROM messages WHERE event_id = ?1",
                params![event_id],
                |r| r.get(0),
            )
            .optional()
            .map_err(|e| e.to_string())
    }

    pub(crate) fn set_edit_id(&self, event_id: &str, edit_id: &str) -> Result<bool, String> {
        self.conn
            .execute(
                "UPDATE messages SET edit_id = ?2 WHERE event_id = ?1 AND edit_id IS NOT ?2",
                params![event_id, edit_id],
            )
            .map(|changed| changed != 0)
            .map_err(|e| format!("cannot record an edit: {e}"))
    }

    pub(crate) fn remove_event(&self, event_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE event_id = ?1", params![event_id])
            .map_err(|e| format!("cannot remove a message from the index: {e}"))?;
        self.conn
            .execute(
                "DELETE FROM messages WHERE edit_id = ?1 AND edit_id IS NOT NULL",
                params![event_id],
            )
            .map_err(|e| format!("cannot remove an edited message from the index: {e}"))?;
        Ok(())
    }

    pub(crate) fn remove_room(&self, room_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE room_id = ?1", params![room_id])
            .map_err(|e| format!("cannot clear a room from the index: {e}"))?;
        Ok(())
    }

    pub(crate) fn generation(&self) -> u64 {
        self.generation.load(std::sync::atomic::Ordering::Relaxed)
    }

    pub(crate) fn clear(&self) -> Result<(), String> {
        self.generation
            .store(next_generation(), std::sync::atomic::Ordering::Relaxed);
        self.conn
            .execute_batch("DELETE FROM messages;")
            .map_err(|e| format!("cannot clear the search index: {e}"))?;
        Ok(())
    }

    pub(crate) fn prune(&self) -> Result<i64, String> {
        let total: i64 = self
            .conn
            .query_row("SELECT count(*) FROM messages", [], |r| r.get(0))
            .map_err(|e| e.to_string())?;
        if total <= MAX_ROWS {
            return Ok(0);
        }
        let excess = total - MAX_ROWS;
        let removed = self
            .conn
            .execute(
                "DELETE FROM messages WHERE id IN (
                     SELECT id FROM messages ORDER BY ts ASC LIMIT ?1)",
                params![excess],
            )
            .map_err(|e| format!("cannot prune the search index: {e}"))?;
        Ok(removed as i64)
    }

    pub(crate) fn stats(&self) -> Result<IndexStats, String> {
        let messages: i64 = self
            .conn
            .query_row("SELECT count(*) FROM messages", [], |r| r.get(0))
            .map_err(|e| e.to_string())?;
        let rooms: i64 = self
            .conn
            .query_row("SELECT count(DISTINCT room_id) FROM messages", [], |r| r.get(0))
            .map_err(|e| e.to_string())?;
        Ok(IndexStats { messages, rooms })
    }

    #[cfg(test)]
    pub(crate) fn contains(&self, event_id: &str) -> Result<bool, String> {
        let found: Option<i64> = self
            .conn
            .query_row(
                "SELECT id FROM messages WHERE event_id = ?1",
                params![event_id],
                |r| r.get(0),
            )
            .optional()
            .map_err(|e| e.to_string())?;
        Ok(found.is_some())
    }

    pub(crate) fn known(&self, event_ids: &[String]) -> Result<HashSet<String>, String> {
        let mut out = HashSet::new();
        if event_ids.is_empty() {
            return Ok(out);
        }
        for slice in event_ids.chunks(500) {
            let placeholders = std::iter::repeat("?")
                .take(slice.len())
                .collect::<Vec<_>>()
                .join(",");
            let sql = format!(
                "SELECT event_id FROM messages WHERE event_id IN ({placeholders})"
            );
            let mut stmt = self.conn.prepare(&sql).map_err(|e| e.to_string())?;
            let rows = stmt
                .query_map(rusqlite::params_from_iter(slice.iter()), |r| {
                    r.get::<_, String>(0)
                })
                .map_err(|e| e.to_string())?;
            for row in rows {
                out.insert(row.map_err(|e| e.to_string())?);
            }
        }
        Ok(out)
    }

    pub(crate) fn search(
        &self,
        query: &str,
        room_id: &str,
        limit: i64,
        offset: i64,
    ) -> Result<Vec<Hit>, String> {
        if !query_is_long_enough(query) {
            return Ok(Vec::new());
        }
        let expression = match_expression(query);
        let limit = limit.clamp(1, 500);
        let offset = offset.max(0);

        let sql = if room_id.is_empty() {
            "SELECT m.event_id, m.room_id, m.sender, m.sender_name, m.body,
                    m.msgtype, m.ts, ifnull(m.filename, '')
             FROM messages m
             WHERE (m.id IN (SELECT rowid FROM messages_fts WHERE messages_fts MATCH ?1)
                 OR m.id IN (SELECT rowid FROM messages_fn_fts WHERE messages_fn_fts MATCH ?1))
             ORDER BY m.ts DESC
             LIMIT ?2 OFFSET ?3"
        } else {
            "SELECT m.event_id, m.room_id, m.sender, m.sender_name, m.body,
                    m.msgtype, m.ts, ifnull(m.filename, '')
             FROM messages m
             WHERE (m.id IN (SELECT rowid FROM messages_fts WHERE messages_fts MATCH ?1)
                 OR m.id IN (SELECT rowid FROM messages_fn_fts WHERE messages_fn_fts MATCH ?1))
               AND m.room_id = ?4
             ORDER BY m.ts DESC
             LIMIT ?2 OFFSET ?3"
        };

        let mut stmt = self.conn.prepare(sql).map_err(|e| e.to_string())?;
        let map = |row: &rusqlite::Row<'_>| -> rusqlite::Result<Hit> {
            Ok(Hit {
                event_id: row.get(0)?,
                room_id: row.get(1)?,
                sender: row.get(2)?,
                sender_name: row.get(3)?,
                body: row.get(4)?,
                msgtype: row.get(5)?,
                ts: row.get(6)?,
                filename: row.get(7)?,
            })
        };
        let rows = if room_id.is_empty() {
            stmt.query_map(params![expression, limit, offset], map)
        } else {
            stmt.query_map(params![expression, limit, offset, room_id], map)
        }
        .map_err(|e| format!("search failed: {e}"))?;

        let mut hits = Vec::new();
        for row in rows {
            hits.push(row.map_err(|e| e.to_string())?);
        }
        Ok(hits)
    }
}

use matrix_sdk::room::Room;

const DEEP_PAGE_SIZE: u16 = 100;

struct Indexable {
    event_id: String,
    sender: String,
    body: String,
    msgtype: String,
    ts: i64,
    filename: Option<String>,
    edit_id: Option<String>,
}

fn indexable_from(raw: &serde_json::Value) -> Option<Indexable> {
    let event_id = raw.get("event_id")?.as_str()?.to_owned();
    let sender = raw.get("sender")?.as_str()?.to_owned();
    let ts = raw.get("origin_server_ts")?.as_i64()?;
    if raw.get("type")?.as_str()? != "m.room.message" {
        return None;
    }
    // A redacted event is never indexable. The live handler removes the row
    // when the redaction arrives, but a sweep re-reads the cache and would put
    // it back. The cache marks redactions with `unsigned.redacted_because`,
    // which is the reliable marker (an empty content alone is not).
    if raw
        .get("unsigned")
        .and_then(|unsigned| unsigned.get("redacted_because"))
        .is_some()
    {
        return None;
    }
    let content = raw.get("content")?;

    let relates = content.get("m.relates_to");
    let is_replacement = relates
        .and_then(|r| r.get("rel_type"))
        .and_then(|v| v.as_str())
        == Some("m.replace");
    let own_id = event_id.clone();
    let (target_id, body_source) = if is_replacement {
        let target = relates
            .and_then(|r| r.get("event_id"))
            .and_then(|v| v.as_str())?
            .to_owned();
        (target, content.get("m.new_content").unwrap_or(content))
    } else {
        (event_id, content)
    };

    let msgtype = body_source
        .get("msgtype")
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_owned();
    let raw_body = body_source.get("body").and_then(|v| v.as_str())?;
    // The quoted original is not part of this message; indexing it would make
    // a reply findable by words it never said.
    let body = if matches!(msgtype.as_str(), "m.text" | "m.notice" | "m.emote") {
        crate::timeline::strip_reply_fallback(raw_body).to_owned()
    } else {
        raw_body.to_owned()
    };
    if body.trim().is_empty() {
        return None;
    }
    let filename = if is_attachment_msgtype(&msgtype) {
        match body_source.get("filename").and_then(|v| v.as_str()) {
            Some(name) => {
                let cleaned = clean_filename(name);
                Some(if cleaned == clean_filename(&body) { String::new() } else { cleaned })
            }
            None if is_replacement => None,
            None => Some(String::new()),
        }
    } else if is_replacement {
        None
    } else {
        Some(String::new())
    };
    let edit_id = if is_replacement { Some(own_id) } else { None };
    Some(Indexable { event_id: target_id, sender, body, msgtype, ts, filename, edit_id })
}

pub(crate) async fn deep_index_room(
    room: &Room,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    stop: &std::sync::atomic::AtomicBool,
    max_pages: u16,
) -> Result<(u16, bool, usize), String> {
    let outcome = deep_index_room_gated(room, index, &StopOnly(stop), max_pages)
        .await
        .map_err(|e| e.message)?;
    Ok((outcome.pages, outcome.reached_start, outcome.written))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Gate {
    Go,
    Wait,
    Stop,
}

pub(crate) trait PageGate: Sync {
    fn gate(&self) -> Gate;
    fn page_gap(&self) -> std::time::Duration {
        std::time::Duration::ZERO
    }
    fn on_page(&self, _so_far: &DeepOutcome) {}
}

pub(crate) struct StopOnly<'a>(pub &'a std::sync::atomic::AtomicBool);

impl PageGate for StopOnly<'_> {
    fn gate(&self) -> Gate {
        if self.0.load(std::sync::atomic::Ordering::Relaxed) {
            Gate::Stop
        } else {
            Gate::Go
        }
    }
}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub(crate) struct DeepOutcome {
    pub pages: u16,
    pub reached_start: bool,
    pub written: usize,
    pub undecryptable: usize,
    pub interrupted: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct DeepError {
    pub message: String,
    pub rate_limited: bool,
    pub retry_after_ms: Option<u64>,
}

impl DeepError {
    fn plain(message: String) -> Self {
        Self { message, rate_limited: false, retry_after_ms: None }
    }

    fn from_pagination(error: &matrix_sdk::event_cache::EventCacheError) -> Self {
        use matrix_sdk::event_cache::EventCacheError;
        use matrix_sdk::ruma::api::error::{ErrorKind, LimitExceededErrorData};
        let mut out = Self::plain(format!("pagination failed: {error}"));
        if let EventCacheError::PaginationError(inner) = error {
            if let Some(ErrorKind::LimitExceeded(LimitExceededErrorData {
                retry_after, ..
            })) = inner.client_api_error_kind()
            {
                out.rate_limited = true;
                out.retry_after_ms = retry_after
                    .as_ref()
                    .and_then(crate::presence::retry_after_to_ms);
            }
        }
        out
    }
}

const GATE_POLL_MS: u64 = 250;

async fn stopped<G: PageGate>(gate: &G) {
    loop {
        if gate.gate() == Gate::Stop {
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(GATE_POLL_MS)).await;
    }
}

async fn paced<G: PageGate>(gate: &G, gap: std::time::Duration) -> bool {
    let poll = std::time::Duration::from_millis(GATE_POLL_MS);
    let mut left = gap;
    while !left.is_zero() {
        if gate.gate() == Gate::Stop {
            return false;
        }
        let step = left.min(poll);
        tokio::time::sleep(step).await;
        left -= step;
    }
    loop {
        match gate.gate() {
            Gate::Go => return true,
            Gate::Stop => return false,
            Gate::Wait => tokio::time::sleep(poll).await,
        }
    }
}

pub(crate) fn current_generation(
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
) -> u64 {
    index
        .lock()
        .ok()
        .and_then(|guard| guard.as_ref().map(SearchIndex::generation))
        .unwrap_or(0)
}

/// Write one collected batch under the index mutex, never across an await.
/// `None` when the index was cleared (or replaced) since `generation` was
/// captured: the batch was collected for an index that no longer exists, and
/// writing it would refill what the user just cleared. The check and the
/// write share the one lock `clear` takes.
fn write_locked(
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    generation: u64,
    room_id: &str,
    batch: &RoomBatch,
) -> Option<usize> {
    let mut guard = index.lock().ok()?;
    let ix = guard.as_mut()?;
    if ix.generation() != generation {
        return None;
    }
    Some(write_batch(ix, room_id, batch))
}

const MAX_TRACKED_UNDECRYPTABLE: usize = 10_000;

pub(crate) async fn deep_index_room_gated<G: PageGate>(
    room: &Room,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    gate: &G,
    max_pages: u16,
) -> Result<DeepOutcome, DeepError> {
    let generation = current_generation(index);
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| DeepError::plain(format!("no event cache for the room: {e}")))?;
    let room_id = room.room_id().as_str();

    let mut outcome = DeepOutcome::default();
    let mut undecryptable: HashSet<String> = HashSet::new();
    if let Ok(batch) = collect_from(room, &room_cache).await {
        let Some(written) = write_locked(index, generation, room_id, &batch) else {
            outcome.interrupted = true;
            return Ok(outcome);
        };
        outcome.written += written;
        note_undecryptable(&mut undecryptable, &batch);
    }

    let pagination = room_cache.pagination();
    for page in 0..max_pages {
        let gap = if page == 0 { std::time::Duration::ZERO } else { gate.page_gap() };
        if !paced(gate, gap).await {
            outcome.interrupted = true;
            break;
        }
        let fetched = tokio::select! {
            result = pagination.run_backwards_once(DEEP_PAGE_SIZE) => Some(result),
            _ = stopped(gate) => None,
        };
        let Some(result) = fetched else {
            outcome.interrupted = true;
            break;
        };
        let page_outcome = result.map_err(|e| DeepError::from_pagination(&e))?;
        outcome.pages += 1;
        if let Ok(batch) = collect_from(room, &room_cache).await {
            let Some(written) = write_locked(index, generation, room_id, &batch) else {
                outcome.interrupted = true;
                break;
            };
            outcome.written += written;
            note_undecryptable(&mut undecryptable, &batch);
        }
        outcome.undecryptable = undecryptable.len();
        gate.on_page(&outcome);
        if page_outcome.reached_start {
            outcome.reached_start = true;
            break;
        }
    }
    outcome.undecryptable = undecryptable.len();
    Ok(outcome)
}

fn note_undecryptable(seen: &mut HashSet<String>, batch: &RoomBatch) {
    for id in &batch.undecryptable {
        if seen.len() >= MAX_TRACKED_UNDECRYPTABLE {
            return;
        }
        seen.insert(id.clone());
    }
}

fn is_undecryptable(raw: &serde_json::Value) -> bool {
    raw.get("type").and_then(|t| t.as_str()) == Some("m.room.encrypted")
}

pub(crate) const SWEEP_ROOM_BATCH: usize = 40;

pub(crate) const DEEP_MAX_PAGES: u16 = 50;

static SWEEP_CURSORS: std::sync::Mutex<Option<std::collections::HashMap<String, usize>>> =
    std::sync::Mutex::new(None);

fn sweep_cursor(account: &str) -> usize {
    SWEEP_CURSORS
        .lock()
        .ok()
        .and_then(|g| g.as_ref().and_then(|m| m.get(account).copied()))
        .unwrap_or(0)
}

fn set_sweep_cursor(account: &str, value: usize) {
    if let Ok(mut g) = SWEEP_CURSORS.lock() {
        g.get_or_insert_with(Default::default).insert(account.to_owned(), value);
    }
}

pub(crate) fn sweep_window<T: Clone>(all: Vec<T>, start: usize, batch: usize) -> Vec<T> {
    if all.len() <= batch {
        return all;
    }
    let from = start % all.len();
    all.iter().cycle().skip(from).take(batch).cloned().collect()
}

pub(crate) async fn sweep(
    client: &matrix_sdk::Client,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    stop: &std::sync::atomic::AtomicBool,
) -> (usize, usize) {
    let mut rooms_done = 0usize;
    let mut written = 0usize;
    let generation = current_generation(index);
    let mut joined = client.joined_rooms();
    joined.sort_by(|a, b| a.room_id().cmp(b.room_id()));
    let account = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let start = sweep_cursor(&account);
    for room in sweep_window(joined, start, SWEEP_ROOM_BATCH) {
        if stop.load(std::sync::atomic::Ordering::Relaxed) {
            break;
        }
        rooms_done += 1;
        let events = match collect_room(&room).await {
            Ok(events) => events,
            Err(_) => continue,
        };
        match write_locked(index, generation, room.room_id().as_str(), &events) {
            Some(n) => written += n,
            None => break,
        }
    }
    set_sweep_cursor(&account, start.wrapping_add(rooms_done));
    (rooms_done, written)
}

pub(crate) struct RoomBatch {
    ordinary: Vec<Indexable>,
    edits: Vec<Indexable>,
    names: std::collections::HashMap<String, String>,
    undecryptable: Vec<String>,
}

pub(crate) async fn collect_room(room: &Room) -> Result<RoomBatch, String> {
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| format!("no event cache for the room: {e}"))?;
    collect_from(room, &room_cache).await
}

pub(crate) async fn collect_from(
    room: &Room,
    room_cache: &matrix_sdk::event_cache::RoomEventCache,
) -> Result<RoomBatch, String> {
    let events = room_cache
        .events()
        .await
        .map_err(|e| format!("cannot read the event cache: {e}"))?;

    let mut ordinary = Vec::new();
    let mut edits = Vec::new();
    let mut names: std::collections::HashMap<String, String> =
        std::collections::HashMap::new();
    let mut undecryptable = Vec::new();
    for event in &events {
        let Ok(raw) = event.raw().deserialize_as::<serde_json::Value>() else {
            continue;
        };
        if is_undecryptable(&raw) {
            if let Some(id) = raw.get("event_id").and_then(|v| v.as_str()) {
                undecryptable.push(id.to_owned());
            }
            continue;
        }
        let is_replacement = raw
            .get("content")
            .and_then(|c| c.get("m.relates_to"))
            .and_then(|r| r.get("rel_type"))
            .and_then(|v| v.as_str())
            == Some("m.replace");
        if let Some(item) = indexable_from(&raw) {
            if !names.contains_key(&item.sender) {
                let resolved = match matrix_sdk::ruma::UserId::parse(&item.sender) {
                    Ok(user) => room
                        .get_member_no_sync(&user)
                        .await
                        .ok()
                        .flatten()
                        .and_then(|m| m.display_name().map(|d| d.to_owned()))
                        .unwrap_or_default(),
                    Err(_) => String::new(),
                };
                names.insert(item.sender.clone(), resolved);
            }
            if is_replacement {
                edits.push(item);
            } else {
                ordinary.push(item);
            }
        }
    }
    Ok(RoomBatch { ordinary, edits, names, undecryptable })
}

pub(crate) fn write_batch(index: &SearchIndex, room_id: &str, batch: &RoomBatch) -> usize {
    let tx = index.conn.unchecked_transaction().ok();
    let written = write_batch_rows(index, room_id, batch);
    match tx {
        Some(tx) => {
            if tx.commit().is_ok() {
                written
            } else {
                0
            }
        }
        None => written,
    }
}

fn write_batch_rows(index: &SearchIndex, room_id: &str, batch: &RoomBatch) -> usize {
    let ids: Vec<String> = batch.ordinary.iter().map(|c| c.event_id.clone()).collect();
    let known = index.known(&ids).unwrap_or_default();
    let stale = index.stale_filename_ids(&ids).unwrap_or_default();
    let mut written = 0usize;
    for item in &batch.ordinary {
        if known.contains(&item.event_id) {
            if stale.contains(&item.event_id) {
                if let Some(name) = &item.filename {
                    if index.fill_filename(&item.event_id, name).unwrap_or(false) {
                        written += 1;
                    }
                }
            }
            continue;
        }
        let display = batch.names.get(&item.sender).cloned().unwrap_or_default();
        if index
            .upsert_named(
                &item.event_id,
                room_id,
                &item.sender,
                &display,
                &item.body,
                &item.msgtype,
                item.ts,
                item.filename.as_deref(),
            )
            .unwrap_or(false)
        {
            written += 1;
        }
    }
    // Only the original's sender may edit it (spec), so edits by anyone else
    // are discarded BEFORE the last one is chosen: a later edit by a stranger
    // must not displace the owner's. The original is read from the index, where
    // the pass above has just put it if it was new; when it is not there, its
    // edits are skipped, never inserted under the editor's name.
    //
    // Then the last cached edit of an event wins; intermediate wording is not
    // replayed into FTS on every sweep.
    let mut order: Vec<&str> = Vec::new();
    let mut final_edits: std::collections::HashMap<&str, &Indexable> =
        std::collections::HashMap::new();
    let mut owners: std::collections::HashMap<&str, Option<String>> =
        std::collections::HashMap::new();
    for edit in &batch.edits {
        let target = edit.event_id.as_str();
        let owner = owners
            .entry(target)
            .or_insert_with(|| index.sender_of(target).ok().flatten());
        if owner.as_deref() != Some(edit.sender.as_str()) {
            continue;
        }
        if final_edits.insert(target, edit).is_none() {
            order.push(target);
        }
    }
    for target in order {
        let edit = final_edits[target];
        let display = batch.names.get(&edit.sender).cloned().unwrap_or_default();
        if index
            .upsert_named(
                target,
                room_id,
                &edit.sender,
                &display,
                &edit.body,
                &edit.msgtype,
                edit.ts,
                edit.filename.as_deref(),
            )
            .unwrap_or(false)
        {
            written += 1;
        }
        if let Some(edit_id) = &edit.edit_id {
            let _ = index.set_edit_id(target, edit_id);
        }
    }
    let _ = index.prune();
    written
}

pub(crate) fn register_redaction_handler(
    client: &matrix_sdk::Client,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
) -> matrix_sdk::event_handler::EventHandlerDropGuard {
    let index = std::sync::Arc::clone(index);
    let handle = client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::room::redaction::SyncRoomRedactionEvent| {
            let index = std::sync::Arc::clone(&index);
            async move {
                // `redacts` moved from the event to its content between room versions;
                // both are read.
                let Some(redacts) = ev
                    .as_original()
                    .and_then(|original| original.redacts.as_ref())
                    .or_else(|| {
                        ev.as_original()
                            .and_then(|original| original.content.redacts.as_ref())
                    })
                else {
                    return;
                };
                let target = redacts.to_string();
                if let Ok(guard) = index.lock() {
                    if let Some(index) = guard.as_ref() {
                        let _ = index.remove_event(&target);
                    }
                }
            }
        },
    );
    client.event_handler_drop_guard(handle)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn index() -> SearchIndex {
        SearchIndex::open_in_memory().unwrap()
    }

    fn add(ix: &SearchIndex, id: &str, room: &str, body: &str, ts: i64) {
        ix.upsert(id, room, "@a:x", "Ann", body, "m.text", ts).unwrap();
    }

    #[test]
    fn fts5_is_compiled_in() {
        let ix = index();
        add(&ix, "$1", "!r:x", "the quick brown fox", 1000);
        assert_eq!(ix.search("quick", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn substrings_match_in_every_script() {
        let ix = index();
        add(&ix, "$1", "!r:x", "这是一个测试消息", 1000);
        add(&ix, "$2", "!r:x", "hello world", 2000);
        add(&ix, "$3", "!r:x", "привет мир", 3000);
        add(&ix, "$4", "!r:x", "مرحبا بالعالم", 4000);
        assert_eq!(ix.search("一个测", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("ell", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("ривет", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("العا", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn a_query_shorter_than_three_characters_is_refused_not_silently_empty() {
        let ix = index();
        add(&ix, "$1", "!r:x", "ok then", 1000);
        assert!(!query_is_long_enough("ok"));
        assert!(!query_is_long_enough(" a "));
        assert!(!query_is_long_enough("ok "));
        assert!(query_is_long_enough(" okay "));
        assert!(query_is_long_enough("测试消"));
        assert_eq!("测试消".len(), 9);
        assert_eq!(ix.search("ok", "", 10, 0).unwrap().len(), 0);
    }

    #[test]
    fn folding_makes_accents_and_case_irrelevant() {
        let ix = index();
        add(&ix, "$1", "!r:x", "Köln im Sommer", 1000);
        add(&ix, "$2", "!r:x", "CAFÉ au lait", 2000);
        assert_eq!(ix.search("koln", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("KÖLN", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("cafe", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn ftsOperatorsTypedByAUserAreCharactersNotOperators() {
        let ix = index();
        add(&ix, "$1", "!r:x", "deploy AND release notes", 1000);
        add(&ix, "$2", "!r:x", "deploy only", 2000);
        let hits = ix.search("deploy AND release", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].event_id, "$1");
    }

    #[test]
    fn aQuotationMarkIsASearchTermNotASyntaxError() {
        let ix = index();
        add(&ix, "$1", "!r:x", "he said \"hello\" loudly", 1000);
        let hits = ix.search("said \"hello\"", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert!(ix.search("\"\"\"", "", 10, 0).is_ok());
        assert!(ix.search("a*b(c)", "", 10, 0).is_ok());
    }

    #[test]
    fn anEditReplacesTheOldWordingRatherThanAddingToIt() {
        let ix = index();
        add(&ix, "$1", "!r:x", "the meeting is on tuesday", 1000);
        assert_eq!(ix.search("tuesday", "", 10, 0).unwrap().len(), 1);
        add(&ix, "$1", "!r:x", "the meeting is on wednesday", 1000);
        assert_eq!(ix.stats().unwrap().messages, 1);
        assert_eq!(ix.search("wednesday", "", 10, 0).unwrap().len(), 1);
        assert_eq!(
            ix.search("tuesday", "", 10, 0).unwrap().len(),
            0,
            "the pre-edit wording is still findable"
        );
    }

    #[test]
    fn aRedactionRemovesTheMessageFromTheIndex() {
        let ix = index();
        add(&ix, "$1", "!r:x", "something regrettable", 1000);
        ix.remove_event("$1").unwrap();
        assert_eq!(ix.search("regrettable", "", 10, 0).unwrap().len(), 0);
        assert_eq!(ix.stats().unwrap().messages, 0);
        assert!(ix.remove_event("$nope").is_ok());
    }

    #[test]
    fn aRoomScopeNarrowsAndForgettingARoomClearsIt() {
        let ix = index();
        add(&ix, "$1", "!a:x", "shared word", 1000);
        add(&ix, "$2", "!b:x", "shared word", 2000);
        assert_eq!(ix.search("shared", "", 10, 0).unwrap().len(), 2);
        assert_eq!(ix.search("shared", "!a:x", 10, 0).unwrap().len(), 1);
        ix.remove_room("!a:x").unwrap();
        assert_eq!(ix.search("shared", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.stats().unwrap().rooms, 1);
    }

    #[test]
    fn resultsAreNewestFirstAndPageable() {
        let ix = index();
        for i in 1..=5 {
            add(&ix, &format!("${i}"), "!r:x", "paging sample", i * 1000);
        }
        let page1 = ix.search("paging", "", 2, 0).unwrap();
        assert_eq!(page1.len(), 2);
        assert_eq!(page1[0].event_id, "$5");
        assert_eq!(page1[1].event_id, "$4");
        let page2 = ix.search("paging", "", 2, 2).unwrap();
        assert_eq!(page2[0].event_id, "$3");
        assert!(ix.search("paging", "", 100_000, 0).unwrap().len() <= 5);
        assert!(ix.search("paging", "", 0, -5).is_ok());
    }

    #[test]
    fn emptyBodiesAndBlankIdsAreNotIndexed() {
        let ix = index();
        ix.upsert("$1", "!r:x", "@a:x", "Ann", "   ", "m.text", 1).unwrap();
        ix.upsert("", "!r:x", "@a:x", "Ann", "text", "m.text", 1).unwrap();
        ix.upsert("$2", "", "@a:x", "Ann", "text", "m.text", 1).unwrap();
        assert_eq!(ix.stats().unwrap().messages, 0);
    }

    #[test]
    fn aRoomBatchIsWrittenInOneCommit() {
        use std::os::raw::{c_int, c_void};
        use std::sync::atomic::{AtomicUsize, Ordering};
        unsafe extern "C" fn count_commit(arg: *mut c_void) -> c_int {
            (*(arg as *const AtomicUsize)).fetch_add(1, Ordering::SeqCst);
            0
        }
        let ix = index();
        let commits = Box::new(AtomicUsize::new(0));
        let commits_ptr = &*commits as *const AtomicUsize as *mut c_void;
        unsafe {
            rusqlite::ffi::sqlite3_commit_hook(ix.conn.handle(), Some(count_commit), commits_ptr);
        }
        let ordinary: Vec<Indexable> = (0..40)
            .map(|i| Indexable {
                event_id: format!("$m{i}"),
                sender: "@a:x".into(),
                body: format!("message number {i} about the quick brown fox"),
                msgtype: "m.text".into(),
                ts: 1000 + i,
                filename: None,
                edit_id: None,
            })
            .collect();
        let batch = RoomBatch {
            ordinary,
            edits: vec![],
            names: std::collections::HashMap::from([("@a:x".into(), "Ann".into())]),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &batch), 40);
        assert_eq!(commits.load(Ordering::SeqCst), 1, "one commit per room batch");
        assert!(ix.conn.is_autocommit());
        assert_eq!(ix.stats().unwrap().messages, 40);
        assert_eq!(ix.search("quick", "", 100, 0).unwrap().len(), 40);
        assert_eq!(write_batch(&ix, "!r:x", &batch), 0);
        unsafe {
            rusqlite::ffi::sqlite3_commit_hook(ix.conn.handle(), None, std::ptr::null_mut());
        }
        drop(commits);
    }

    #[test]
    fn theIndexWalHasASizeLimit() {
        let ix = index();
        let limit: i64 = ix.conn.query_row("PRAGMA journal_size_limit", [], |r| r.get(0)).unwrap();
        assert_eq!(limit, 1024 * 1024);
    }

    #[test]
    fn identicalUpsertsDoNotRewriteTheFtsIndex() {
        let ix = index();
        add(&ix, "$same", "!r:x", "unchanged wording", 1000);
        let before = ix.conn.total_changes();
        add(&ix, "$same", "!r:x", "unchanged wording", 1000);
        assert_eq!(ix.conn.total_changes(), before);
        assert_eq!(ix.search("unchanged", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn cachedEditHistoryDoesNotRewriteTheFtsIndex() {
        let ix = index();
        let item = |body: &str, ts| Indexable {
            event_id: "$edited".into(), sender: "@a:x".into(),
            body: body.into(), msgtype: "m.text".into(), ts, filename: None, edit_id: None,
        };
        let batch = RoomBatch {
            ordinary: vec![item("original wording", 1000)],
            edits: vec![item("intermediate wording", 2000), item("final wording", 3000)],
            names: std::collections::HashMap::from([("@a:x".into(), "Ann".into())]),
            undecryptable: vec![],
        };
        write_batch(&ix, "!r:x", &batch);
        let before = ix.conn.total_changes();
        write_batch(&ix, "!r:x", &batch);
        assert_eq!(ix.conn.total_changes(), before);
        assert_eq!(ix.search("final", "", 10, 0).unwrap().len(), 1);
        assert!(ix.search("intermediate", "", 10, 0).unwrap().is_empty());
        let timestamp: i64 = ix.conn.query_row(
            "SELECT ts FROM messages WHERE event_id = '$edited'", [], |r| r.get(0),
        ).unwrap();
        assert_eq!(timestamp, 1000);
    }

    #[test]
    fn coalescingEditsKeepsTheOriginalFallbackMetadata() {
        let ix = index();
        ix.upsert("$orig", "!r:x", "@first:x", "First", "first wording", "m.text", 1000)
            .unwrap();
        let edit = |body: &str, ts, edit_id: &str| Indexable {
            event_id: "$orig".into(), sender: "@first:x".into(), body: body.into(),
            msgtype: "m.notice".into(), ts, filename: None, edit_id: Some(edit_id.into()),
        };
        let batch = RoomBatch {
            ordinary: vec![],
            edits: vec![edit("intermediate wording", 1500, "$e1"), edit("final wording", 2000, "$e2")],
            names: std::collections::HashMap::from([("@first:x".into(), "Latest name".into())]),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &batch), 1);
        let hit = ix.search("final", "", 10, 0).unwrap().remove(0);
        assert_eq!(hit.sender, "@first:x");
        assert_eq!(hit.ts, 1000);
        assert_eq!(hit.sender_name, "Latest name");
        assert_eq!(hit.msgtype, "m.notice");
        assert!(ix.search("intermediate", "", 10, 0).unwrap().is_empty());
        assert_eq!(write_batch(&ix, "!r:x", &batch), 0);
    }

    #[test]
    fn an_edit_by_anyone_but_the_original_sender_is_ignored() {
        let ix = index();
        let original = Indexable {
            event_id: "$img".into(), sender: "@alice:x".into(), body: "alices caption".into(),
            msgtype: "m.image".into(), ts: 1000, filename: Some("alice-photo.png".into()),
            edit_id: None,
        };
        let evil_edit = |target: &str, id: &str| Indexable {
            event_id: target.into(), sender: "@evil:x".into(), body: "pwned wording".into(),
            msgtype: "m.image".into(), ts: 2000, filename: Some("pwned.png".into()),
            edit_id: Some(id.into()),
        };
        let batch = RoomBatch {
            ordinary: vec![original],
            edits: vec![evil_edit("$img", "$ev1")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        write_batch(&ix, "!r:x", &batch);
        assert_eq!(ix.search("alices caption", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("alice-photo", "", 10, 0).unwrap().len(), 1);
        assert!(ix.search("pwned", "", 10, 0).unwrap().is_empty());
        let later = RoomBatch {
            ordinary: vec![],
            edits: vec![evil_edit("$img", "$ev2")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &later), 0);
        assert!(ix.search("pwned", "", 10, 0).unwrap().is_empty());
        let orphan = RoomBatch {
            ordinary: vec![],
            edits: vec![evil_edit("$never-seen", "$ev3")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &orphan), 0);
        assert!(!ix.contains("$never-seen").unwrap());
        assert_eq!(ix.stats().unwrap().messages, 1);
        let mut own = evil_edit("$img", "$own");
        own.sender = "@alice:x".into();
        let batch = RoomBatch {
            ordinary: vec![],
            edits: vec![own],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &batch), 1);
        assert_eq!(ix.search("pwned.png", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn a_strangers_later_edit_does_not_displace_the_owners_edit() {
        let ix = index();
        ix.upsert("$m", "!r:x", "@alice:x", "Alice", "original", "m.text", 1).unwrap();
        let edit = |sender: &str, body: &str, id: &str| Indexable {
            event_id: "$m".into(), sender: sender.into(), body: body.into(),
            msgtype: "m.text".into(), ts: 2, filename: None, edit_id: Some(id.into()),
        };
        let batch = RoomBatch {
            ordinary: vec![],
            edits: vec![edit("@alice:x", "alices real edit", "$e1"), edit("@evil:x", "evil wording", "$e2")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        write_batch(&ix, "!r:x", &batch);
        assert_eq!(ix.search("alices real edit", "", 10, 0).unwrap().len(), 1);
        assert!(ix.search("evil wording", "", 10, 0).unwrap().is_empty());
        assert!(ix.search("original", "", 10, 0).unwrap().is_empty());
    }

    #[test]
    fn redacting_an_edit_is_served_by_the_edit_id_index() {
        let ix = index();
        let plan: Vec<String> = ix
            .conn
            .prepare("EXPLAIN QUERY PLAN DELETE FROM messages WHERE edit_id = ?1 AND edit_id IS NOT NULL")
            .unwrap()
            .query_map(["$e"], |r| r.get::<_, String>(3))
            .unwrap()
            .map(Result::unwrap)
            .collect();
        assert!(plan.iter().any(|d| d.contains("messages_edit_id")), "scan: {plan:?}");
    }

    #[test]
    fn redacting_an_edit_takes_its_text_and_file_name_out_of_the_index() {
        let ix = index();
        ix.upsert_named("$img", "!r:x", "@a:x", "Ann", "original caption", "m.image", 1, Some("orig.png"))
            .unwrap();
        let batch = RoomBatch {
            ordinary: vec![],
            edits: vec![Indexable {
                event_id: "$img".into(), sender: "@a:x".into(), body: "regrettable edit".into(),
                msgtype: "m.image".into(), ts: 2, filename: Some("regrettable.png".into()),
                edit_id: Some("$edit".into()),
            }],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        write_batch(&ix, "!r:x", &batch);
        assert_eq!(ix.search("regrettable", "", 10, 0).unwrap().len(), 1);
        ix.remove_event("$edit").unwrap();
        assert!(ix.search("regrettable", "", 10, 0).unwrap().is_empty());
        assert!(!ix.contains("$img").unwrap());
        ix.upsert_named("$other", "!r:x", "@a:x", "Ann", "keepme text", "m.text", 3, None).unwrap();
        ix.remove_event("$unrelated").unwrap();
        assert!(ix.contains("$other").unwrap());
    }

    #[test]
    fn migration_replaces_the_old_trigger_and_clear_removes_file_names() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let ix = SearchIndex::from_connection(conn).unwrap();
        let sql: String = ix
            .conn
            .query_row("SELECT sql FROM sqlite_master WHERE name = 'messages_au'", [], |r| r.get(0))
            .unwrap();
        assert!(sql.contains("WHEN old.body IS NOT new.body"), "the old unconditional trigger survived: {sql}");
        ix.upsert_named("$img2", "!r:x", "@a:x", "Ann", "some caption", "m.image", 5, Some("keep.png")).unwrap();
        let fingerprint = body_fts_fingerprint(&ix);
        ix.fill_filename("$img", "late.png").unwrap();
        assert_eq!(ix.search("late.png", "", 10, 0).unwrap().len(), 1);
        assert_eq!(body_fts_fingerprint(&ix), fingerprint);
        ix.clear().unwrap();
        assert!(ix.search("keep.png", "", 10, 0).unwrap().is_empty());
        assert!(ix.search("late.png", "", 10, 0).unwrap().is_empty());
        let rows: i64 = ix.conn.query_row("SELECT count(*) FROM messages", [], |r| r.get(0)).unwrap();
        assert_eq!(rows, 0);
        let left: i64 = ix
            .conn
            .query_row(
                "SELECT count(*) FROM messages_fn_fts WHERE messages_fn_fts MATCH '\"keep\"'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(left, 0);
    }

    #[test]
    fn a_migration_that_cannot_complete_leaves_the_old_schema_and_no_open_transaction() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let ix = SearchIndex { conn, generation: std::sync::atomic::AtomicU64::new(1) };
        ix.conn.execute_batch("PRAGMA query_only = ON;").unwrap();
        assert!(ix.migrate().is_err());
        ix.conn.execute_batch("PRAGMA query_only = OFF;").unwrap();
        assert!(ix.conn.is_autocommit(), "a transaction was left open");
        let version: i64 = ix.conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, 0);
        let has: bool = ix
            .conn
            .prepare("SELECT 1 FROM pragma_table_info('messages') WHERE name = 'filename'")
            .unwrap()
            .exists([])
            .unwrap();
        assert!(!has);
        ix.migrate().unwrap();
        assert!(ix.conn.is_autocommit());
    }

    #[test]
    fn theSenderIsSearchableToo() {
        let ix = index();
        ix.upsert("$1", "!r:x", "@bob:x", "Roberta", "lunch", "m.text", 1)
            .unwrap();
        assert_eq!(ix.search("Roberta", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("robert", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn pruningEvictsTheOldestAndKeepsTheNewest() {
        let ix = index();
        for i in 1..=10 {
            add(&ix, &format!("${i}"), "!r:x", "bounded sample", i * 1000);
        }
        let removed = ix
            .conn
            .execute(
                "DELETE FROM messages WHERE id IN (
                     SELECT id FROM messages ORDER BY ts ASC LIMIT 4)",
                [],
            )
            .unwrap();
        assert_eq!(removed, 4);
        let left = ix.search("bounded", "", 10, 0).unwrap();
        assert_eq!(left.len(), 6);
        assert_eq!(left[0].event_id, "$10");
        assert_eq!(
            left.last().unwrap().event_id,
            "$5",
            "eviction took the newest instead of the oldest"
        );
        assert_eq!(ix.prune().unwrap(), 0);
    }

    #[test]
    fn knownAnswersInBulkSoABackfillNeedNotWriteToFindOut() {
        let ix = index();
        add(&ix, "$1", "!r:x", "already there", 1000);
        add(&ix, "$2", "!r:x", "also there", 2000);
        let ids: Vec<String> = (1..=4).map(|i| format!("${i}")).collect();
        let known = ix.known(&ids).unwrap();
        assert_eq!(known.len(), 2);
        assert!(known.contains("$1") && known.contains("$2"));
        assert!(ix.contains("$1").unwrap());
        assert!(!ix.contains("$9").unwrap());
        assert!(ix.known(&[]).unwrap().is_empty());
        let many: Vec<String> = (0..1500).map(|i| format!("$x{i}")).collect();
        assert!(ix.known(&many).unwrap().is_empty());
    }

    #[test]
    fn clearingLeavesAUsableEmptyIndex() {
        let ix = index();
        add(&ix, "$1", "!r:x", "before the clear", 1000);
        ix.clear().unwrap();
        assert_eq!(ix.stats().unwrap(), IndexStats { messages: 0, rooms: 0 });
        add(&ix, "$2", "!r:x", "after the clear", 2000);
        assert_eq!(ix.search("after", "", 10, 0).unwrap().len(), 1);
    }

    fn raw(json: serde_json::Value) -> serde_json::Value {
        json
    }

    #[test]
    fn onlyRoomMessagesWithTextAreIndexed() {
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$1", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.member",
            "content": {"membership": "join", "displayname": "Ann"}
        })))
        .is_none());
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$2", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.reaction",
            "content": {"m.relates_to": {"key": "👍"}}
        })))
        .is_none());
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$3", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.message", "content": {}
        })))
        .is_none());
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$4", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.message",
            "content": {"msgtype": "m.text", "body": "   "}
        })))
        .is_none());

        let ok = indexable_from(&raw(serde_json::json!({
            "event_id": "$5", "sender": "@a:x", "origin_server_ts": 42,
            "type": "m.room.message",
            "content": {"msgtype": "m.text", "body": "a real message"}
        })))
        .expect("an ordinary text message was not indexable");
        assert_eq!(ok.event_id, "$5");
        assert_eq!(ok.body, "a real message");
        assert_eq!(ok.msgtype, "m.text");
        assert_eq!(ok.ts, 42);
    }

    #[test]
    fn anUndecryptableEventIsCountedAndNeverIndexed() {
        let utd = raw(serde_json::json!({
            "event_id": "$u", "sender": "@a:x", "origin_server_ts": 3,
            "type": "m.room.encrypted",
            "content": {"algorithm": "m.megolm.v1.aes-sha2",
                        "ciphertext": "AwgAEpAB", "session_id": "s"}
        }));
        assert!(is_undecryptable(&utd));
        assert!(indexable_from(&utd).is_none(), "ciphertext was indexed");
        let decrypted = raw(serde_json::json!({
            "event_id": "$d", "sender": "@a:x", "origin_server_ts": 4,
            "type": "m.room.message",
            "content": {"msgtype": "m.text", "body": "readable"}
        }));
        assert!(!is_undecryptable(&decrypted));

        let mut seen = HashSet::new();
        let batch = RoomBatch {
            ordinary: Vec::new(),
            edits: Vec::new(),
            names: std::collections::HashMap::new(),
            undecryptable: vec!["$u".to_owned(), "$v".to_owned()],
        };
        note_undecryptable(&mut seen, &batch);
        note_undecryptable(&mut seen, &batch);
        assert_eq!(seen.len(), 2);
    }

    #[test]
    fn aBatchCollectedBeforeAClearIsNeverWrittenAfterIt() {
        let index = std::sync::Arc::new(std::sync::Mutex::new(Some(
            SearchIndex::open_in_memory().unwrap(),
        )));
        let batch = RoomBatch {
            ordinary: vec![Indexable {
                event_id: "$late".to_owned(),
                sender: "@a:x".to_owned(),
                body: "collected before the clear".to_owned(),
                msgtype: "m.text".to_owned(),
                ts: 1,
                filename: None, edit_id: None,
            }],
            edits: Vec::new(),
            names: std::collections::HashMap::new(),
            undecryptable: Vec::new(),
        };
        let before = current_generation(&index);
        assert_ne!(before, 0);
        assert_eq!(write_locked(&index, before, "!r:x", &batch), Some(1));

        let generation = current_generation(&index);
        index.lock().unwrap().as_ref().unwrap().clear().unwrap();
        assert_eq!(
            write_locked(&index, generation, "!r:x", &batch),
            None,
            "a batch collected before the clear was written after it"
        );
        let stats = index.lock().unwrap().as_ref().unwrap().stats().unwrap();
        assert_eq!(stats.messages, 0, "the cleared index was refilled");

        let fresh = current_generation(&index);
        assert_eq!(write_locked(&index, fresh, "!r:x", &batch), Some(1));
        let closed = std::sync::Arc::new(std::sync::Mutex::new(None));
        assert_eq!(current_generation(&closed), 0);
        assert_eq!(write_locked(&closed, 0, "!r:x", &batch), None);
        assert_ne!(SearchIndex::open_in_memory().unwrap().generation(), fresh);
    }

    #[test]
    fn anAttachmentIsIndexedByItsFilename() {
        let item = indexable_from(&raw(serde_json::json!({
            "event_id": "$f", "sender": "@a:x", "origin_server_ts": 7,
            "type": "m.room.message",
            "content": {"msgtype": "m.file", "body": "quarterly-report.pdf",
                        "url": "mxc://x/y"}
        })))
        .expect("an attachment was not indexable");
        assert_eq!(item.body, "quarterly-report.pdf");
        assert_eq!(item.msgtype, "m.file");
    }

    #[test]
    fn anEditIsAttributedToTheEventItReplaces() {
        let item = indexable_from(&raw(serde_json::json!({
            "event_id": "$edit", "sender": "@a:x", "origin_server_ts": 99,
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "* corrected wording",
                "m.new_content": {"msgtype": "m.text", "body": "corrected wording"},
                "m.relates_to": {"rel_type": "m.replace", "event_id": "$original"}
            }
        })))
        .expect("an edit was not indexable");
        assert_eq!(
            item.event_id, "$original",
            "the edit was indexed under its own id, so the original keeps its \
             old text and the edit appears as a second message"
        );
        assert_eq!(item.body, "corrected wording");
        assert!(!item.body.starts_with('*'), "the fallback text was indexed");
    }

    #[test]
    fn aReplyIsAnOrdinaryMessageNotAnEdit() {
        let item = indexable_from(&raw(serde_json::json!({
            "event_id": "$reply", "sender": "@a:x", "origin_server_ts": 5,
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text", "body": "agreed",
                "m.relates_to": {"m.in_reply_to": {"event_id": "$original"}}
            }
        })))
        .expect("a reply was not indexable");
        assert_eq!(item.event_id, "$reply");
        assert_eq!(item.body, "agreed");
    }

    #[test]
    fn aMalformedEventIsSkippedRatherThanPanicking()
    {
        for value in [
            serde_json::json!({}),
            serde_json::json!({"event_id": "$1"}),
            serde_json::json!({"event_id": 5, "sender": "@a:x",
                               "origin_server_ts": 1, "type": "m.room.message"}),
            serde_json::json!({"event_id": "$1", "sender": "@a:x",
                               "origin_server_ts": "not a number",
                               "type": "m.room.message",
                               "content": {"body": "x"}}),
            serde_json::json!({"event_id": "$1", "sender": "@a:x",
                               "origin_server_ts": 1, "type": "m.room.message",
                               "content": {"body": 12345}}),
            serde_json::json!({"event_id": "$1", "sender": "@a:x",
                               "origin_server_ts": 1, "type": "m.room.message",
                               "content": {"body": "* x",
                                           "m.relates_to": {"rel_type": "m.replace"}}}),
        ] {
            assert!(indexable_from(&value).is_none(), "{value}");
        }
    }

    #[test]
    fn trigramCanFoldDiacriticsItself() {
        let db = Connection::open_in_memory().unwrap();
        let created = db.execute_batch(
            "CREATE VIRTUAL TABLE t USING fts5(body, tokenize='trigram remove_diacritics 2');
             INSERT INTO t(body) VALUES ('Köln Café');",
        );
        assert!(created.is_ok(), "rejected: {created:?}");
        let koln: i64 = db
            .query_row("SELECT count(*) FROM t WHERE t MATCH '\"koln\"'", [], |r| r.get(0))
            .unwrap();
        let upper: i64 = db
            .query_row("SELECT count(*) FROM t WHERE t MATCH '\"KÖLN\"'", [], |r| r.get(0))
            .unwrap();
        let version: String = db
            .query_row("SELECT sqlite_version()", [], |r| r.get(0))
            .unwrap();
        println!("MEASURED sqlite={version} koln={koln} KOLN={upper}");
        assert_eq!(koln, 1, "trigram remove_diacritics did not fold");
        assert_eq!(upper, 1, "trigram is not case-insensitive");
    }

    fn captioned_image(event_id: &str, caption: &str, filename: &str) -> serde_json::Value {
        serde_json::json!({
            "type": "m.room.message", "event_id": event_id, "sender": "@a:x",
            "origin_server_ts": 5000i64,
            "content": {
                "msgtype": "m.image", "body": caption, "filename": filename,
                "url": "mxc://example.org/never-stored",
                "info": { "mimetype": "image/png", "size": 7 },
            },
        })
    }

    #[test]
    fn a_captioned_image_is_found_by_its_file_name() {
        let ix = index();
        let item = indexable_from(&captioned_image("$img", "look at the sunset", "IMG_2041_sunset.png"))
            .unwrap();
        assert_eq!(item.filename.as_deref(), Some("IMG_2041_sunset.png"));
        let batch = RoomBatch {
            ordinary: vec![item],
            edits: vec![],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &batch), 1);
        let by_name = ix.search("IMG_2041", "", 10, 0).unwrap();
        assert_eq!(by_name.len(), 1);
        assert_eq!(by_name[0].event_id, "$img");
        assert_eq!(by_name[0].filename, "IMG_2041_sunset.png");
        assert_eq!(ix.search("img_2041_SUNSET", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("sunset", "", 10, 0).unwrap().len(), 1);
        assert!(ix.search("IMG_2041", "!other:x", 10, 0).unwrap().is_empty());
    }

    #[test]
    fn only_the_file_name_is_added_to_what_is_stored() {
        let ix = index();
        let batch = RoomBatch {
            ordinary: vec![indexable_from(&captioned_image("$i", "cap", "report.pdf")).unwrap()],
            edits: vec![],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        write_batch(&ix, "!r:x", &batch);
        assert!(ix.search("never-stored", "", 10, 0).unwrap().is_empty());
        assert!(ix.search("image/png", "", 10, 0).unwrap().is_empty());
        let columns: Vec<String> = ix
            .conn
            .prepare("SELECT name FROM pragma_table_info('messages') ORDER BY cid")
            .unwrap()
            .query_map([], |r| r.get(0))
            .unwrap()
            .map(Result::unwrap)
            .collect();
        assert_eq!(
            columns,
            ["id", "event_id", "room_id", "sender", "sender_name", "body", "msgtype", "ts", "filename", "edit_id"]
        );
    }

    #[test]
    fn a_name_equal_to_the_body_is_not_stored_twice_and_a_text_message_has_none() {
        let same = indexable_from(&captioned_image("$a", "plain.png", "plain.png")).unwrap();
        assert_eq!(same.filename.as_deref(), Some(""));
        let no_name = serde_json::json!({
            "type": "m.room.message", "event_id": "$b", "sender": "@a:x",
            "origin_server_ts": 1i64,
            "content": { "msgtype": "m.file", "body": "old-style.zip", "url": "mxc://x/y" },
        });
        assert_eq!(indexable_from(&no_name).unwrap().filename.as_deref(), Some(""));
        let text = serde_json::json!({
            "type": "m.room.message", "event_id": "$c", "sender": "@a:x",
            "origin_server_ts": 1i64,
            "content": { "msgtype": "m.text", "body": "hello", "filename": "x.png" },
        });
        assert_eq!(indexable_from(&text).unwrap().filename.as_deref(), Some(""));
        let long = "n".repeat(10_000);
        let bounded = indexable_from(&captioned_image("$d", "cap", &format!("{long}\n\u{7}"))).unwrap();
        let bounded = bounded.filename.unwrap();
        assert_eq!(bounded.len(), MAX_FILENAME_BYTES);
        let wide = "é".repeat(200);
        let cut = clean_filename(&wide);
        assert!(cut.len() <= MAX_FILENAME_BYTES);
        assert_eq!(cut.chars().count(), MAX_FILENAME_BYTES / 2);
        assert_eq!(clean_filename("evil\u{202E}gpj.exe\u{2066}"), "evilgpj.exe");
        let disguised = indexable_from(&captioned_image("$e", "plain.png", "plain\u{202E}.png")).unwrap();
        assert_eq!(disguised.filename.as_deref(), Some(""));
    }

    #[test]
    fn a_redaction_removes_the_file_name_too() {
        let ix = index();
        ix.upsert_named("$img", "!r:x", "@a:x", "Ann", "caption", "m.image", 1, Some("secret-plans.pdf"))
            .unwrap();
        assert_eq!(ix.search("secret-plans", "", 10, 0).unwrap().len(), 1);
        ix.remove_event("$img").unwrap();
        assert!(ix.search("secret-plans", "", 10, 0).unwrap().is_empty());
        let leftovers: i64 = ix
            .conn
            .query_row(
                "SELECT count(*) FROM messages_fn_fts WHERE messages_fn_fts MATCH '\"secret-plans\"'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(leftovers, 0);
        ix.upsert_named("$2", "!r:x", "@a:x", "Ann", "c", "m.file", 1, Some("ledger.xlsx")).unwrap();
        ix.remove_room("!r:x").unwrap();
        assert!(ix.search("ledger", "", 10, 0).unwrap().is_empty());
    }

    #[test]
    fn editing_a_caption_keeps_the_name_and_a_new_name_replaces_it() {
        let ix = index();
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "first caption", "m.image", 1, Some("old-name.png"))
            .unwrap();
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "second caption", "m.image", 1, None).unwrap();
        assert_eq!(ix.search("old-name", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("second caption", "", 10, 0).unwrap().len(), 1);
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "second caption", "m.image", 1, Some("new-name.png"))
            .unwrap();
        assert!(ix.search("old-name", "", 10, 0).unwrap().is_empty());
        assert_eq!(ix.search("new-name", "", 10, 0).unwrap().len(), 1);
    }

    fn body_fts_fingerprint(ix: &SearchIndex) -> (i64, i64, i64, i64) {
        let data: (i64, i64) = ix
            .conn
            .query_row("SELECT count(*), total(length(block)) FROM messages_fts_data", [], |r| {
                Ok((r.get(0)?, r.get::<_, f64>(1)? as i64))
            })
            .unwrap();
        let sizes: (i64, i64) = ix
            .conn
            .query_row("SELECT count(*), total(length(sz)) FROM messages_fts_docsize", [], |r| {
                Ok((r.get(0)?, r.get::<_, f64>(1)? as i64))
            })
            .unwrap();
        (data.0, data.1, sizes.0, sizes.1)
    }

    const LEGACY_SCHEMA: &str = "
        CREATE TABLE messages(
            id INTEGER PRIMARY KEY, event_id TEXT NOT NULL UNIQUE, room_id TEXT NOT NULL,
            sender TEXT NOT NULL, sender_name TEXT NOT NULL DEFAULT '',
            body TEXT NOT NULL DEFAULT '', msgtype TEXT NOT NULL DEFAULT '', ts INTEGER NOT NULL);
        CREATE INDEX messages_room_ts ON messages(room_id, ts DESC);
        CREATE INDEX messages_ts ON messages(ts);
        CREATE VIRTUAL TABLE messages_fts USING fts5(body, sender_name,
            content='messages', content_rowid='id', tokenize='trigram remove_diacritics 2');
        CREATE TRIGGER messages_ai AFTER INSERT ON messages BEGIN
            INSERT INTO messages_fts(rowid, body, sender_name) VALUES (new.id, new.body, new.sender_name);
        END;
        CREATE TRIGGER messages_ad AFTER DELETE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, body, sender_name)
            VALUES ('delete', old.id, old.body, old.sender_name);
        END;
        CREATE TRIGGER messages_au AFTER UPDATE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, body, sender_name)
            VALUES ('delete', old.id, old.body, old.sender_name);
            INSERT INTO messages_fts(rowid, body, sender_name) VALUES (new.id, new.body, new.sender_name);
        END;
        INSERT INTO messages(event_id, room_id, sender, sender_name, body, msgtype, ts)
            VALUES ('$text', '!r:x', '@a:x', 'Ann', 'an ordinary sentence', 'm.text', 10),
                   ('$img', '!r:x', '@a:x', 'Ann', 'caption of the picture', 'm.image', 20);
        INSERT INTO messages(event_id, room_id, sender, sender_name, body, msgtype, ts)
            VALUES ('$third', '!r:x', '@a:x', 'Ann', 'a third message', 'm.text', 30);
        INSERT INTO messages(event_id, room_id, sender, sender_name, body, msgtype, ts)
            VALUES ('$fourth', '!r:x', '@a:x', 'Ann', 'a fourth message', 'm.text', 40);";

    #[test]
    fn an_index_from_before_file_names_migrates_without_rewriting_anything() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let version: i64 = conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, 0);
        let ix_before = SearchIndex { conn, generation: std::sync::atomic::AtomicU64::new(1) };
        let fingerprint_before = body_fts_fingerprint(&ix_before);
        let changes_before = ix_before.conn.total_changes();
        let SearchIndex { conn, .. } = ix_before;

        let ix = SearchIndex::from_connection(conn).unwrap();

        assert_eq!(body_fts_fingerprint(&ix), fingerprint_before);
        assert_eq!(ix.conn.total_changes() - changes_before, 3);
        let version: i64 = ix.conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(ix.stats().unwrap().messages, 4);
        assert_eq!(ix.search("ordinary", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("picture", "", 10, 0).unwrap().len(), 1);
        let stale = ix
            .stale_filename_ids(&["$text".to_owned(), "$img".to_owned()])
            .unwrap();
        assert_eq!(stale, HashSet::from(["$img".to_owned()]));
    }

    #[test]
    fn the_migration_is_idempotent_and_survives_an_interrupted_run() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let ix = SearchIndex::from_connection(conn).unwrap();
        let before = ix.conn.total_changes();
        ix.ensure_schema().unwrap();
        ix.ensure_schema().unwrap();
        assert_eq!(ix.conn.total_changes(), before);
        ix.conn.execute_batch("PRAGMA user_version = 0;").unwrap();
        ix.ensure_schema().unwrap();
        let version: i64 = ix.conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(ix.search("ordinary", "", 10, 0).unwrap().len(), 1);
    }

    #[test]
    fn a_migrated_attachment_is_backfilled_once_by_the_ordinary_sweep() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let ix = SearchIndex::from_connection(conn).unwrap();
        assert!(ix.search("holiday-photo", "", 10, 0).unwrap().is_empty());

        let batch = RoomBatch {
            ordinary: vec![
                indexable_from(&captioned_image("$img", "caption of the picture", "holiday-photo.jpg"))
                    .unwrap(),
                indexable_from(&serde_json::json!({
                    "type": "m.room.message", "event_id": "$text", "sender": "@a:x",
                    "origin_server_ts": 10i64,
                    "content": { "msgtype": "m.text", "body": "an ordinary sentence" },
                })).unwrap(),
            ],
            edits: vec![],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        ix.conn
            .execute("UPDATE messages SET body = 'edited caption' WHERE event_id = '$img'", [])
            .unwrap();
        assert_eq!(write_batch(&ix, "!r:x", &batch), 1);
        assert_eq!(ix.search("holiday-photo", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("edited caption", "", 10, 0).unwrap().len(), 1, "the edit was overwritten");
        let before = ix.conn.total_changes();
        assert_eq!(write_batch(&ix, "!r:x", &batch), 0);
        assert_eq!(ix.conn.total_changes(), before);
    }

    #[test]
    fn unicode61WouldHaveMadeChineseUnsearchable() {
        let db = Connection::open_in_memory().unwrap();
        db.execute_batch(
            "CREATE VIRTUAL TABLE t USING fts5(body,
                 tokenize='unicode61 remove_diacritics 2');
             INSERT INTO t(body) VALUES ('这是一个测试消息');",
        )
        .unwrap();
        let hits: i64 = db
            .query_row("SELECT count(*) FROM t WHERE t MATCH '测试'", [], |r| {
                r.get(0)
            })
            .unwrap();
        assert_eq!(hits, 0, "unicode61 segmented CJK after all");
    }

    #[test]
    fn theSweepWindowMovesOnAndCoversEveryRoom() {
        let rooms: Vec<usize> = (0..100).collect();
        assert_eq!(sweep_window(rooms.clone(), 0, 40).len(), 40);
        assert_eq!(sweep_window(rooms.clone(), 0, 40)[0], 0);
        assert_eq!(sweep_window(rooms.clone(), 40, 40)[0], 40);
        let mut seen = std::collections::HashSet::new();
        let mut start = 0;
        for _ in 0..3 {
            let w = sweep_window(rooms.clone(), start, 40);
            start += w.len();
            seen.extend(w);
        }
        assert_eq!(seen.len(), 100);
        assert_eq!(sweep_window(rooms.clone(), 90, 40)[10], 0);
        set_sweep_cursor("@a:x", 40);
        assert_eq!(sweep_cursor("@a:x"), 40);
        assert_eq!(sweep_cursor("@b:x"), 0);
        set_sweep_cursor("@b:x", 7);
        assert_eq!(sweep_cursor("@a:x"), 40);
        assert_eq!(sweep_window(vec![1, 2, 3], 7, 40), vec![1, 2, 3]);
    }
}
