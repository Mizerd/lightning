//! The local message index: SQLite FTS5 over what this client has seen.
//!
//! Server search (`search.rs`) cannot read encrypted rooms, so this index,
//! built from plaintext the client already has, makes search work the same
//! in encrypted and public rooms.
//!
//! It adds no new class of data to disk: the SDK event cache already stores
//! decrypted events unencrypted (`encode_event` serializes the `Decrypted`
//! variant and Lightning opens `sqlite_store(path, None)`), in the same 0700
//! directory. The index is a second, more queryable copy (see
//! `docs/local-search.md`). Encrypting the store at rest would fix both, but
//! `sqlite_store` uses one config for all stores, and adding a passphrase
//! would leave existing installs unable to decode their account pickle.
//!
//! Tokenizer: `trigram`, not `unicode61`. unicode61 splits on spaces and
//! punctuation, so unspaced scripts like Chinese become one token and are
//! unsearchable; trigram matches substrings in every script at the cost of
//! a larger index and a 3-character minimum. `remove_diacritics 2` on the
//! trigram table folds both stored text and query (`koln` finds `Köln`).

use std::collections::HashSet;
use std::path::Path;

use rusqlite::{params, Connection, OptionalExtension};

/// File name inside the account's store directory.
pub(crate) const INDEX_FILE: &str = "lightning-search.sqlite3";

/// Rows kept before the oldest are evicted, so the index cannot grow without
/// bound (roughly tens of MB at ~100 bytes per message).
pub(crate) const MAX_ROWS: i64 = 250_000;

/// Shortest query the trigram tokenizer can match; reported to the user
/// rather than silently returning nothing.
pub(crate) const MIN_QUERY_CHARS: usize = 3;

/// One result.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Hit {
    pub event_id: String,
    pub room_id: String,
    pub sender: String,
    pub sender_name: String,
    pub body: String,
    pub msgtype: String,
    pub ts: i64,
    /// The attachment's file name when it differs from `body` (a captioned
    /// attachment); "" otherwise.
    pub filename: String,
}

/// Longest file name kept, in BYTES (cut on a character boundary). A name is
/// at most 255 bytes on every common file system; the bound also stops a
/// hostile event from filling a row.
const MAX_FILENAME_BYTES: usize = 255;

/// Message types whose `filename` is worth finding. Anything else carrying a
/// `filename` key is not an attachment and is ignored.
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

/// Schema version recorded in `PRAGMA user_version`. 0 is every index created
/// before file names were indexed (and a brand-new file, which the same steps
/// bring to 1). Bump only with a migration in [`SearchIndex::migrate`].
const SCHEMA_VERSION: i64 = 1;

/// What the index holds, for "search covers N messages".
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct IndexStats {
    pub messages: i64,
    pub rooms: i64,
}

/// The typed text as an FTS5 MATCH expression: always one quoted phrase, so
/// FTS5 operators (`AND`, `OR`, `NOT`, `*`, `^`, `:`, parentheses) are taken
/// literally and an unbalanced quote is not a syntax error. With trigram this
/// is a literal substring search. Inner double quotes are doubled, per
/// SQLite.
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
    /// Changes when the index is cleared, and is unique per opened index. A
    /// writer captures it before its first `.await` and writes only while it
    /// still matches, checked under the index mutex that `clear` also holds:
    /// so nothing collected before a clear can land after it.
    generation: std::sync::atomic::AtomicU64,
}

/// Source of index generations, process-wide so a reopened index never
/// repeats a value an older writer captured.
static GENERATIONS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

fn next_generation() -> u64 {
    GENERATIONS.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

impl SearchIndex {
    /// Open (creating if needed) the index for an account store directory.
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
        self.conn
            .execute_batch(
                "PRAGMA journal_mode=WAL;
                 PRAGMA synchronous=NORMAL;

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

    /// Bring an index of any earlier shape to [`SCHEMA_VERSION`]. Idempotent:
    /// every statement is guarded, so a run interrupted half way, or repeated
    /// on every open, converges on the same schema.
    ///
    /// Version 1 indexes attachment file names (a captioned attachment's
    /// `body` is the caption; the name lives in `filename`). Deliberately NOT
    /// done by rebuilding `messages_fts`: that rewrites the whole text index in
    /// one burst on an idle machine. Instead:
    ///   * `messages.filename` is added as NULL = "not examined yet";
    ///   * a SECOND, initially empty FTS table holds the names, so the
    ///     existing text index is untouched;
    ///   * old attachment rows are filled in as the paced sweeps meet their
    ///     events again ([`SearchIndex::stale_filename_ids`]), one tiny update
    ///     each, never a table scan.
    fn migrate(&self) -> Result<(), String> {
        // One transaction: a crash or an error part way leaves the previous
        // schema, never a half-migrated one. (SQLite DDL is transactional.)
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
            // The pre-1 trigger fired on ANY update, so a filename-only update
            // would have rewritten the body's FTS entries for nothing.
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

    /// Add or update one message. An upsert on the event id, so an edit
    /// replaces the old wording instead of leaving it findable. The caller
    /// resolves the edit. Returns whether a row actually changed.
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

    /// [`upsert`](Self::upsert) with the attachment's file name. `None` means
    /// "the event does not say" (an edit without `filename`): a stored name is
    /// kept, and a new row is left unexamined. `Some("")` means "examined, no
    /// separate name".
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
        // An empty body can never be found; do not spend a row on it.
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

    /// Attachment rows among `event_ids` indexed before file names were: their
    /// `filename` is still NULL. Text rows are deliberately not listed (they
    /// have no name), which bounds the backfill to attachments.
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

    /// Record the file name of an already indexed row, touching nothing else
    /// (an edited body must survive). Only fills a NULL, so a row is written
    /// at most once by the backfill. Returns whether a row changed.
    pub(crate) fn fill_filename(&self, event_id: &str, filename: &str) -> Result<bool, String> {
        self.conn
            .execute(
                "UPDATE messages SET filename = ?2 WHERE event_id = ?1 AND filename IS NULL",
                params![event_id, clean_filename(filename)],
            )
            .map(|changed| changed != 0)
            .map_err(|e| format!("cannot record a file name: {e}"))
    }

    /// The sender stored for an event: the original's, which is the only
    /// sender allowed to edit it.
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

    /// Remember which edit event produced a row's current text, so redacting
    /// that edit can take its text out (see [`remove_event`](Self::remove_event)).
    /// An event id only. Writes nothing when it is already recorded.
    pub(crate) fn set_edit_id(&self, event_id: &str, edit_id: &str) -> Result<bool, String> {
        self.conn
            .execute(
                "UPDATE messages SET edit_id = ?2 WHERE event_id = ?1 AND edit_id IS NOT ?2",
                params![event_id, edit_id],
            )
            .map(|changed| changed != 0)
            .map_err(|e| format!("cannot record an edit: {e}"))
    }

    /// A redaction removes the row outright; a redacted message must not stay
    /// findable by its text. `event_id` may also be the EDIT that produced the
    /// row's current text (`edit_id`): the edit's wording (and file name) lives
    /// in the original's row, and the index cannot restore the wording before
    /// it, so the row goes too. That is the conservative direction: the
    /// original is re-indexed from the event cache by the next sweep, which
    /// skips the redacted edit ([`indexable_from`]) and applies any earlier one.
    pub(crate) fn remove_event(&self, event_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE event_id = ?1", params![event_id])
            .map_err(|e| format!("cannot remove a message from the index: {e}"))?;
        // Separate statement so the partial index serves it (an OR would scan).
        self.conn
            .execute(
                "DELETE FROM messages WHERE edit_id = ?1 AND edit_id IS NOT NULL",
                params![event_id],
            )
            .map_err(|e| format!("cannot remove an edited message from the index: {e}"))?;
        Ok(())
    }

    /// Everything for one room (the user forgets a room).
    pub(crate) fn remove_room(&self, room_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE room_id = ?1", params![room_id])
            .map_err(|e| format!("cannot clear a room from the index: {e}"))?;
        Ok(())
    }

    /// The current generation; see the field.
    pub(crate) fn generation(&self) -> u64 {
        self.generation.load(std::sync::atomic::Ordering::Relaxed)
    }

    /// Drop the whole index. The generation moves FIRST, so a writer holding
    /// an older one is refused even when the delete itself fails.
    pub(crate) fn clear(&self) -> Result<(), String> {
        self.generation
            .store(next_generation(), std::sync::atomic::Ordering::Relaxed);
        self.conn
            .execute_batch("DELETE FROM messages;")
            .map_err(|e| format!("cannot clear the search index: {e}"))?;
        Ok(())
    }

    /// Evict the oldest rows past [`MAX_ROWS`], by timestamp: recent messages
    /// are the ones most often searched.
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

    /// True when this event is already indexed.
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

    /// Which of `event_ids` are already indexed, in one query.
    pub(crate) fn known(&self, event_ids: &[String]) -> Result<HashSet<String>, String> {
        let mut out = HashSet::new();
        if event_ids.is_empty() {
            return Ok(out);
        }
        // Chunked below SQLite's default 999-parameter limit.
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

    /// Search; empty `room_id` searches every indexed room. Newest first rather
    /// than by bm25, matching the server search beside it.
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

        // A row matches on its text (body, sender name) OR its file name.
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

// ---------------------------------------------------------------------------
// Feeding the index
// ---------------------------------------------------------------------------
//
// One source: the SDK event cache, not the live timeline (which covers only
// the open room). `RoomEventCache::events()` returns events already
// decrypted (the redecryptor replaces UTDs in place), so encrypted rooms
// index like public ones.
//
// Coverage is what Lightning has cached, growing as the user reads and as
// [`deep_index_room`] pages backwards; the UI shows a count rather than
// implying completeness.

use matrix_sdk::room::Room;

/// Events per backward page while deep-indexing.
const DEEP_PAGE_SIZE: u16 = 100;

/// One message worth indexing, taken from a cached event.
struct Indexable {
    event_id: String,
    sender: String,
    body: String,
    msgtype: String,
    ts: i64,
    /// `Some(name)`: the event states a file name distinct from the body
    /// ("" = examined, none). `None`: it does not say (an edit without one).
    filename: Option<String>,
    /// For a replacement: the id of the EDIT event itself (`event_id` is the
    /// original's). `None` for an ordinary event.
    edit_id: Option<String>,
}

/// Pull the searchable text out of one cached event. Only messages with text
/// qualify; state changes, reactions and receipts would make "joined the
/// room" the most common hit. Redacted events are skipped (see below).
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

    // An edit carries its text in `m.new_content` and a "* fallback" in `body`.
    // Indexing the fallback would make edits findable by an asterisk and keep
    // the old text; the edit is applied to the original event's row instead.
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
    // An attachment's `body` is its filename, which is worth finding.
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
    // The name of a captioned attachment (MSC2530): `body` is the caption and
    // `filename` the file. When it is the same string, or absent on an original
    // event, `body` already is the name and nothing extra is stored.
    let filename = if is_attachment_msgtype(&msgtype) {
        match body_source.get("filename").and_then(|v| v.as_str()) {
            // Compared AFTER cleaning, on both sides, so a name that differs
            // from the body only by control or bidi characters is not stored.
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

/// Page a room backwards, indexing what each page brings in ("Index this
/// room"). The same walk as [`deep_index_room_gated`], stopped only by
/// sign-out and unpaced, because the user is waiting on it.
///
/// Returns (pages run, reached_start, rows written).
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

/// What a paged walk is told between pages.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Gate {
    /// Fetch the next page.
    Go,
    /// Hold (a call, a scrolling reader): wait and ask again, losing nothing.
    Wait,
    /// Stop now: sign-out, account switch, cancel or pause.
    Stop,
}

/// How a caller paces and stops [`deep_index_room_gated`]. Sync methods only,
/// so the walk future stays `Send` without an async trait.
pub(crate) trait PageGate: Sync {
    /// Asked before every page, and polled while a page is in flight so a
    /// stop never waits on a slow request (only `Stop` matters then).
    fn gate(&self) -> Gate;
    /// Pause between pages. Zero for the interactive per-room index.
    fn page_gap(&self) -> std::time::Duration {
        std::time::Duration::ZERO
    }
    /// Called after each page is indexed, with the walk so far.
    fn on_page(&self, _so_far: &DeepOutcome) {}
}

/// The per-room gate: sign-out stops it, nothing paces it.
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

/// How one walk ended.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub(crate) struct DeepOutcome {
    pub pages: u16,
    pub reached_start: bool,
    pub written: usize,
    /// Distinct events in the walked history that are still undecryptable.
    /// They are never indexed (their `type` is `m.room.encrypted`); counted
    /// so a room holding them is not recorded as fully indexed.
    pub undecryptable: usize,
    /// The gate stopped the walk before its bound or the room start.
    pub interrupted: bool,
}

/// Why a walk failed. `retry_after_ms` is the server's own hint on an
/// `M_LIMIT_EXCEEDED` that matrix-sdk's retries did not absorb.
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

/// How often a hold or an in-flight page re-reads its gate.
const GATE_POLL_MS: u64 = 250;

/// Resolves once the gate says `Stop`; raced against an in-flight page.
async fn stopped<G: PageGate>(gate: &G) {
    loop {
        if gate.gate() == Gate::Stop {
            return;
        }
        tokio::time::sleep(std::time::Duration::from_millis(GATE_POLL_MS)).await;
    }
}

/// Wait `gap`, then out any hold. False when the gate stopped the walk.
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

/// The generation a writer captures before its first `.await`; 0 when no
/// index is open (every write is then refused, which is right: there is
/// nothing to write into).
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

/// Upper bound on the undecryptable ids one walk remembers: 50 pages of 100
/// events is 5,000, so this never truncates a real walk and still bounds a
/// pathological one.
const MAX_TRACKED_UNDECRYPTABLE: usize = 10_000;

/// Page a room backwards, indexing what each page brings in.
///
/// Read after every page: `RoomEventCache::events()` reads the in-memory
/// linked chunk, which Lightning's jump-to-live trim shrinks to about one
/// page, so reading only at the end would collect one page.
///
/// Bounded by `max_pages` and stops at the room start. The gate is asked
/// before every page and polled while one is in flight, so sign-out never
/// waits on a request. Pages already fetched come from the event-cache store
/// before the network, so re-walking a room is mostly local reads.
pub(crate) async fn deep_index_room_gated<G: PageGate>(
    room: &Room,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    gate: &G,
    max_pages: u16,
) -> Result<DeepOutcome, DeepError> {
    // Before the first await: every write below must go into this index, so a
    // clear during any await, this one included, refuses them.
    let generation = current_generation(index);
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| DeepError::plain(format!("no event cache for the room: {e}")))?;
    let room_id = room.room_id().as_str();

    let mut outcome = DeepOutcome::default();
    let mut undecryptable: HashSet<String> = HashSet::new();
    // What is already loaded, before any request.
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
            // Cleared while this page was in flight: stop, write nothing.
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

/// True for an event the client holds but could not decrypt: the cache keeps
/// a UTD in its encrypted form. Never indexed ([`indexable_from`] takes only
/// `m.room.message`); counted instead.
fn is_undecryptable(raw: &serde_json::Value) -> bool {
    raw.get("type").and_then(|t| t.as_str()) == Some("m.room.encrypted")
}

// ---------------------------------------------------------------------------
// The background indexer
// ---------------------------------------------------------------------------

/// Rooms swept per wake-up, so an account with hundreds of rooms does not
/// hold the runtime for long.
pub(crate) const SWEEP_ROOM_BATCH: usize = 40;

/// Backward pages per room for the deep index: 50 x 100 events, useful yet
/// bounded.
pub(crate) const DEEP_MAX_PAGES: u16 = 50;

/// Where the next sweep's window starts, over the sorted joined rooms, PER
/// ACCOUNT: two signed-in accounts (or a sign-out and a different sign-in in
/// one process) have unrelated room lists and must not advance each other's
/// window.
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

/// The `batch` items starting at `start` (wrapping), or all of them when they
/// fit in one batch.
pub(crate) fn sweep_window<T: Clone>(all: Vec<T>, start: usize, batch: usize) -> Vec<T> {
    if all.len() <= batch {
        return all;
    }
    let from = start % all.len();
    all.iter().cycle().skip(from).take(batch).cloned().collect()
}

/// Sweep every joined room's cached events into the index. Returns (rooms
/// visited, rows written). `stop` is checked between rooms.
pub(crate) async fn sweep(
    client: &matrix_sdk::Client,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    stop: &std::sync::atomic::AtomicBool,
) -> (usize, usize) {
    let mut rooms_done = 0usize;
    let mut written = 0usize;
    // Captured before the first await, like the deep walk: a clear during the
    // sweep ends it rather than being refilled from batches collected before.
    let generation = current_generation(index);
    // A batch of SWEEP_ROOM_BATCH rooms per wake that always started at the
    // same place meant an account with more joined rooms than that never had
    // the rest swept at all. The window now moves on by what it visited, over
    // a stable order, so every room is met within ceil(rooms / batch) wakes.
    let mut joined = client.joined_rooms();
    joined.sort_by(|a, b| a.room_id().cmp(b.room_id()));
    let account = client.user_id().map(|u| u.to_string()).unwrap_or_default();
    let start = sweep_cursor(&account);
    for room in sweep_window(joined, start, SWEEP_ROOM_BATCH) {
        if stop.load(std::sync::atomic::Ordering::Relaxed) {
            break;
        }
        rooms_done += 1;
        // Lock per room, not for the whole sweep, so a search is not blocked.
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

/// One room's indexable events, ready to write. Separate from the write so
/// the index mutex is never held across an `.await`.
pub(crate) struct RoomBatch {
    ordinary: Vec<Indexable>,
    edits: Vec<Indexable>,
    names: std::collections::HashMap<String, String>,
    /// Ids of events still undecryptable; ids only, never content.
    undecryptable: Vec<String>,
}

pub(crate) async fn collect_room(room: &Room) -> Result<RoomBatch, String> {
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| format!("no event cache for the room: {e}"))?;
    collect_from(room, &room_cache).await
}

/// The same walk against a cache handle the caller already holds; re-taking
/// it per page would let the linked chunk shrink under the walk.
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

/// Write one collected room. Synchronous, so the index mutex is held for a
/// bounded burst of inserts, never across a network wait.
pub(crate) fn write_batch(index: &SearchIndex, room_id: &str, batch: &RoomBatch) -> usize {
    let ids: Vec<String> = batch.ordinary.iter().map(|c| c.event_id.clone()).collect();
    let known = index.known(&ids).unwrap_or_default();
    // Attachments indexed before file names were: their cached events are met
    // by the ordinary paced sweeps, and each is filled in once (below).
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

/// Remove a redacted message's row as the redaction arrives. Registered for
/// the sync loop's lifetime, like the RTC and call observers, so it never
/// fires into a later account's index. Best effort: `indexable_from` also
/// refuses to re-add a redacted event on the next sweep.
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

    // FTS5 is a compile-time SQLite option; `bundled-sqlite` makes it a build
    // constant rather than whatever the host has. The schema cannot build
    // without FTS5.
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
        // Chinese has no spaces: unicode61 would find nothing here, trigram does.
        assert_eq!(ix.search("一个测", "", 10, 0).unwrap().len(), 1);
        // A substring inside a word.
        assert_eq!(ix.search("ell", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("ривет", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("العا", "", 10, 0).unwrap().len(), 1);
    }

    // ── The cost of trigram ──────────────────────────────────────────────
    #[test]
    fn a_query_shorter_than_three_characters_is_refused_not_silently_empty() {
        let ix = index();
        add(&ix, "$1", "!r:x", "ok then", 1000);
        assert!(!query_is_long_enough("ok"));
        assert!(!query_is_long_enough(" a "));
        // Trimmed before counting, so trailing space does not count.
        assert!(!query_is_long_enough("ok "));
        assert!(query_is_long_enough(" okay "));
        assert!(query_is_long_enough("测试消"));
        // The minimum is in characters, not bytes, or CJK queries would be refused.
        assert_eq!("测试消".len(), 9);
        assert_eq!(ix.search("ok", "", 10, 0).unwrap().len(), 0);
    }

    #[test]
    fn folding_makes_accents_and_case_irrelevant() {
        let ix = index();
        add(&ix, "$1", "!r:x", "Köln im Sommer", 1000);
        add(&ix, "$2", "!r:x", "CAFÉ au lait", 2000);
        // Passes only because `remove_diacritics 2` folds both sides.
        assert_eq!(ix.search("koln", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("KÖLN", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("cafe", "", 10, 0).unwrap().len(), 1);
    }

    // ── The query is text, not a language ────────────────────────────────
    #[test]
    fn ftsOperatorsTypedByAUserAreCharactersNotOperators() {
        let ix = index();
        add(&ix, "$1", "!r:x", "deploy AND release notes", 1000);
        add(&ix, "$2", "!r:x", "deploy only", 2000);
        // As an FTS5 operator this would match both rows; as text, only one.
        let hits = ix.search("deploy AND release", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].event_id, "$1");
    }

    #[test]
    fn aQuotationMarkIsASearchTermNotASyntaxError() {
        let ix = index();
        add(&ix, "$1", "!r:x", "he said \"hello\" loudly", 1000);
        // Unescaped, this would be an FTS5 syntax error.
        let hits = ix.search("said \"hello\"", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        // A lone quote must not fail either.
        assert!(ix.search("\"\"\"", "", 10, 0).is_ok());
        assert!(ix.search("a*b(c)", "", 10, 0).is_ok());
    }

    // ── Editing, redaction, scope ────────────────────────────────────────
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
        // Removing what is not there is not an error.
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
        // The limit is clamped.
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

    // Several cached edits of one event: the last wins and the row keeps the
    // original's sender, timestamp and message type metadata. Every edit here is
    // by the ORIGINAL's sender (anything else is refused, see the next test).
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

    // An m.replace is honoured only from the original's sender. On the old
    // code @evil could rewrite another user's message and file name, or plant
    // a row under their own name for an event the index had never seen.
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
        // 1. Original in the same batch.
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
        // 2. Original known only from the stored row (a later sweep).
        let later = RoomBatch {
            ordinary: vec![],
            edits: vec![evil_edit("$img", "$ev2")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &later), 0);
        assert!(ix.search("pwned", "", 10, 0).unwrap().is_empty());
        // 3. Original unknown to the index and the batch: no row is invented
        // under the editor's name.
        let orphan = RoomBatch {
            ordinary: vec![],
            edits: vec![evil_edit("$never-seen", "$ev3")],
            names: std::collections::HashMap::new(),
            undecryptable: vec![],
        };
        assert_eq!(write_batch(&ix, "!r:x", &orphan), 0);
        assert!(!ix.contains("$never-seen").unwrap());
        assert_eq!(ix.stats().unwrap().messages, 1);
        // The owner's own edit still lands.
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

    // A stranger's edit that comes AFTER the owner's must not displace it
    // (the last edit used to be chosen before the sender was checked).
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

    // Redacting an edit finds the row through an index, not a table scan.
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

    // Redacting an EDIT must not leave its wording (or file name) behind: the
    // edit lives in the original's row, so the row is dropped and re-indexed
    // from the cache, which skips the redacted edit.
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
        // The redaction names the edit event, not the original.
        ix.remove_event("$edit").unwrap();
        assert!(ix.search("regrettable", "", 10, 0).unwrap().is_empty());
        assert!(!ix.contains("$img").unwrap());
        // Control: redacting an unrelated id removes nothing.
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
        // A filename-only update must not rewrite the body's FTS entries.
        ix.upsert_named("$img2", "!r:x", "@a:x", "Ann", "some caption", "m.image", 5, Some("keep.png")).unwrap();
        let fingerprint = body_fts_fingerprint(&ix);
        ix.fill_filename("$img", "late.png").unwrap();
        // The name was indexed, the body's FTS entries were not rewritten.
        assert_eq!(ix.search("late.png", "", 10, 0).unwrap().len(), 1);
        assert_eq!(body_fts_fingerprint(&ix), fingerprint);
        ix.clear().unwrap();
        assert!(ix.search("keep.png", "", 10, 0).unwrap().is_empty());
        assert!(ix.search("late.png", "", 10, 0).unwrap().is_empty());
        let rows: i64 = ix.conn.query_row("SELECT count(*) FROM messages", [], |r| r.get(0)).unwrap();
        assert_eq!(rows, 0);
        // Not just joined away: nothing is left matchable in the name table.
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

    // The migration runs in one transaction: when it cannot complete, the
    // index is left exactly as it was and no transaction stays open.
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
        // And it still migrates cleanly afterwards.
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

    // ── Bounds ───────────────────────────────────────────────────────────
    #[test]
    fn pruningEvictsTheOldestAndKeepsTheNewest() {
        let ix = index();
        // Exercise the eviction SQL MAX_ROWS drives without writing 250k rows.
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
        // Under the cap, prune() does nothing.
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
        // Past SQLite's 999-parameter limit, hence the chunking.
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


    // ── Lifting text out of a cached event ───────────────────────────────
    fn raw(json: serde_json::Value) -> serde_json::Value {
        json
    }

    #[test]
    fn onlyRoomMessagesWithTextAreIndexed() {
        // State changes are not indexed.
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$1", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.member",
            "content": {"membership": "join", "displayname": "Ann"}
        })))
        .is_none());
        // A reaction is not a message either.
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$2", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.reaction",
            "content": {"m.relates_to": {"key": "👍"}}
        })))
        .is_none());
        // A redacted message's content is emptied by the server.
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$3", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.message", "content": {}
        })))
        .is_none());
        // An empty body is nothing to find.
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

    // "Index all rooms" must not record a room as complete while part of its
    // history is still ciphertext. The cache keeps a UTD in its encrypted
    // form; it is counted, and never indexed. Old code had no such count, so
    // a walk over a room of UTDs reported nothing unusual.
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
        // The same page read twice (the in-memory chunk overlaps) counts once.
        note_undecryptable(&mut seen, &batch);
        assert_eq!(seen.len(), 2);
    }

    // "Clear index" while a page is in flight: the walk (or sweep) captured
    // the generation before its await, the clear moves it under the same
    // mutex, and the batch collected for the old index is refused. Old code:
    // write_locked had no generation, so this batch refilled the cleared
    // index with up to a page of decrypted text.
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
        // Control: the same batch writes when nothing was cleared.
        let before = current_generation(&index);
        assert_ne!(before, 0);
        assert_eq!(write_locked(&index, before, "!r:x", &batch), Some(1));

        let generation = current_generation(&index);
        // The page is "in flight"; the user clears the index.
        index.lock().unwrap().as_ref().unwrap().clear().unwrap();
        assert_eq!(
            write_locked(&index, generation, "!r:x", &batch),
            None,
            "a batch collected before the clear was written after it"
        );
        let stats = index.lock().unwrap().as_ref().unwrap().stats().unwrap();
        assert_eq!(stats.messages, 0, "the cleared index was refilled");

        // A writer that starts after the clear writes normally.
        let fresh = current_generation(&index);
        assert_eq!(write_locked(&index, fresh, "!r:x", &batch), Some(1));
        // No index open: nothing to write into, and nothing is written.
        let closed = std::sync::Arc::new(std::sync::Mutex::new(None));
        assert_eq!(current_generation(&closed), 0);
        assert_eq!(write_locked(&closed, 0, "!r:x", &batch), None);
        // A reopened index never reuses a generation an old writer holds.
        assert_ne!(SearchIndex::open_in_memory().unwrap().generation(), fresh);
    }

    #[test]
    fn anAttachmentIsIndexedByItsFilename() {
        // An attachment's `body` is its filename.
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
        // An m.replace's "* new text" fallback must not be indexed; the edit is
        // attributed to the original event, or the old wording would stay findable.
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
        // m.in_reply_to is also an m.relates_to; treating a reply as a replacement
        // would overwrite the original's row.
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
        // Events come off the wire: none of these may unwrap.
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
            // An m.replace without a target is dropped, not filed under a guess.
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
        // Measures that trigram's remove_diacritics folds diacritics itself.
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

    // ── Attachment file names (schema version 1) ─────────────────────────

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

    /// An image with a caption is found by its file name, which `body` no
    /// longer carries. Fails on the old code: only `body` was indexed.
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
        // Folded like text, and the caption still works.
        assert_eq!(ix.search("img_2041_SUNSET", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("sunset", "", 10, 0).unwrap().len(), 1);
        // Scoped search still scopes.
        assert!(ix.search("IMG_2041", "!other:x", 10, 0).unwrap().is_empty());
    }

    /// Nothing but the name is stored: not the mxc URI, not the mime type.
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
        // A stray `filename` on a text message is not an attachment name.
        let text = serde_json::json!({
            "type": "m.room.message", "event_id": "$c", "sender": "@a:x",
            "origin_server_ts": 1i64,
            "content": { "msgtype": "m.text", "body": "hello", "filename": "x.png" },
        });
        assert_eq!(indexable_from(&text).unwrap().filename.as_deref(), Some(""));
        // Bounded and stripped of control characters.
        let long = "n".repeat(10_000);
        let bounded = indexable_from(&captioned_image("$d", "cap", &format!("{long}\n\u{7}"))).unwrap();
        let bounded = bounded.filename.unwrap();
        assert_eq!(bounded.len(), MAX_FILENAME_BYTES);
        // Cut on a character boundary, never in the middle of one.
        let wide = "é".repeat(200); // 400 bytes
        let cut = clean_filename(&wide);
        assert!(cut.len() <= MAX_FILENAME_BYTES);
        assert_eq!(cut.chars().count(), MAX_FILENAME_BYTES / 2);
        // Bidi controls (not Cc) are removed: they would disguise an extension.
        assert_eq!(clean_filename("evil\u{202E}gpj.exe\u{2066}"), "evilgpj.exe");
        // The name==body comparison happens AFTER cleaning.
        let disguised = indexable_from(&captioned_image("$e", "plain.png", "plain\u{202E}.png")).unwrap();
        assert_eq!(disguised.filename.as_deref(), Some(""));
    }

    /// A redaction removes the row, and with it the file name in the FTS table.
    #[test]
    fn a_redaction_removes_the_file_name_too() {
        let ix = index();
        ix.upsert_named("$img", "!r:x", "@a:x", "Ann", "caption", "m.image", 1, Some("secret-plans.pdf"))
            .unwrap();
        assert_eq!(ix.search("secret-plans", "", 10, 0).unwrap().len(), 1);
        ix.remove_event("$img").unwrap();
        assert!(ix.search("secret-plans", "", 10, 0).unwrap().is_empty());
        // The FTS table itself is empty, not merely joined away.
        let leftovers: i64 = ix
            .conn
            .query_row(
                "SELECT count(*) FROM messages_fn_fts WHERE messages_fn_fts MATCH '\"secret-plans\"'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(leftovers, 0);
        // Same for a whole forgotten room and for clear().
        ix.upsert_named("$2", "!r:x", "@a:x", "Ann", "c", "m.file", 1, Some("ledger.xlsx")).unwrap();
        ix.remove_room("!r:x").unwrap();
        assert!(ix.search("ledger", "", 10, 0).unwrap().is_empty());
    }

    /// An edited name replaces the old one; a body-only edit keeps the name.
    #[test]
    fn editing_a_caption_keeps_the_name_and_a_new_name_replaces_it() {
        let ix = index();
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "first caption", "m.image", 1, Some("old-name.png"))
            .unwrap();
        // An edit that states no filename (None) changes the caption only.
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "second caption", "m.image", 1, None).unwrap();
        assert_eq!(ix.search("old-name", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("second caption", "", 10, 0).unwrap().len(), 1);
        ix.upsert_named("$i", "!r:x", "@a:x", "Ann", "second caption", "m.image", 1, Some("new-name.png"))
            .unwrap();
        assert!(ix.search("old-name", "", 10, 0).unwrap().is_empty());
        assert_eq!(ix.search("new-name", "", 10, 0).unwrap().len(), 1);
    }

    /// A fingerprint of the BODY text index's storage: row counts and byte
    /// totals of its shadow tables. A rebuild or a rewrite of any entry moves it.
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

    /// The schema an index had before file names existed.
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
        // (legacy tables have no `messages_fts_data` fingerprint problem: FTS5 made them)
        let fingerprint_before = body_fts_fingerprint(&ix_before);
        let changes_before = ix_before.conn.total_changes();
        let SearchIndex { conn, .. } = ix_before;

        let ix = SearchIndex::from_connection(conn).unwrap();

        // The existing text index was neither rebuilt nor touched, so there is
        // no write burst on an idle machine. (`total_changes` also counts the
        // handful of bookkeeping rows SQLite writes when FTS5 creates the empty
        // name table, hence a small bound rather than zero.)
        assert_eq!(body_fts_fingerprint(&ix), fingerprint_before);
        // The exact measured value (FTS5's own config row and friends), so a
        // rebuild/optimize or any extra write moves it. The fixture has four
        // separately committed inserts, so the body index holds several segments
        // and an `optimize` would change the fingerprint above.
        assert_eq!(ix.conn.total_changes() - changes_before, 3);
        let version: i64 = ix.conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        // Old rows survive and stay searchable by their text.
        assert_eq!(ix.stats().unwrap().messages, 4);
        assert_eq!(ix.search("ordinary", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("picture", "", 10, 0).unwrap().len(), 1);
        // The old attachment is unexamined (NULL), the text row is not listed.
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
        // Running the steps again on every open changes nothing and fails nothing.
        let before = ix.conn.total_changes();
        ix.ensure_schema().unwrap();
        ix.ensure_schema().unwrap();
        assert_eq!(ix.conn.total_changes(), before);
        // An interrupted migration: column added, version never written.
        ix.conn.execute_batch("PRAGMA user_version = 0;").unwrap();
        ix.ensure_schema().unwrap();
        let version: i64 = ix.conn.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(ix.search("ordinary", "", 10, 0).unwrap().len(), 1);
    }

    /// A migrated attachment gains its name when the paced sweep meets its
    /// event again, once, without disturbing an edited caption.
    #[test]
    fn a_migrated_attachment_is_backfilled_once_by_the_ordinary_sweep() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_SCHEMA).unwrap();
        let ix = SearchIndex::from_connection(conn).unwrap();
        assert!(ix.search("holiday-photo", "", 10, 0).unwrap().is_empty());

        let batch = RoomBatch {
            ordinary: vec![
                // The cached ORIGINAL still has the old caption; the row holds a newer one.
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
        // One row filled in; the text row is not touched.
        assert_eq!(write_batch(&ix, "!r:x", &batch), 1);
        assert_eq!(ix.search("holiday-photo", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("edited caption", "", 10, 0).unwrap().len(), 1, "the edit was overwritten");
        // Nothing left to do: the second pass writes nothing at all.
        let before = ix.conn.total_changes();
        assert_eq!(write_batch(&ix, "!r:x", &batch), 0);
        assert_eq!(ix.conn.total_changes(), before);
    }

    // ── The tokenizer decision, evidenced ────────────────────────────────
    #[test]
    fn unicode61WouldHaveMadeChineseUnsearchable() {
        // Tests the alternative, so choosing trigram rests on a measurement.
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

    // A sweep that always began at room 0 never reached room 41 onward.
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
        // Wraps instead of stopping short.
        assert_eq!(sweep_window(rooms.clone(), 90, 40)[10], 0);
        // Each account has its own cursor.
        set_sweep_cursor("@a:x", 40);
        assert_eq!(sweep_cursor("@a:x"), 40);
        assert_eq!(sweep_cursor("@b:x"), 0);
        set_sweep_cursor("@b:x", 7);
        assert_eq!(sweep_cursor("@a:x"), 40);
        // Few rooms: all of them, whatever the cursor.
        assert_eq!(sweep_window(vec![1, 2, 3], 7, 40), vec![1, 2, 3]);
    }
}
