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
}

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
        let index = Self { conn };
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
                 CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
                     INSERT INTO messages_fts(messages_fts, rowid, body, sender_name)
                     VALUES ('delete', old.id, old.body, old.sender_name);
                     INSERT INTO messages_fts(rowid, body, sender_name)
                     VALUES (new.id, new.body, new.sender_name);
                 END;",
            )
            .map_err(|e| format!("cannot prepare the search index: {e}"))?;
        Ok(())
    }

    /// Add or update one message. An upsert on the event id, so an edit
    /// replaces the old wording instead of leaving it findable. The caller
    /// resolves the edit.
    pub(crate) fn upsert(
        &self,
        event_id: &str,
        room_id: &str,
        sender: &str,
        sender_name: &str,
        body: &str,
        msgtype: &str,
        ts: i64,
    ) -> Result<(), String> {
        if event_id.is_empty() || room_id.is_empty() {
            return Ok(());
        }
        // An empty body can never be found; do not spend a row on it.
        if body.trim().is_empty() {
            return Ok(());
        }
        self.conn
            .execute(
                "INSERT INTO messages
                    (event_id, room_id, sender, sender_name, body, msgtype, ts)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT(event_id) DO UPDATE SET
                     body        = excluded.body,
                     sender_name = excluded.sender_name,
                     msgtype     = excluded.msgtype",
                params![
                    event_id,
                    room_id,
                    sender,
                    sender_name,
                    body,
                    msgtype,
                    ts,
                ],
            )
            .map_err(|e| format!("cannot index a message: {e}"))?;
        Ok(())
    }

    /// A redaction removes the row outright; a redacted message must not stay
    /// findable by its text.
    pub(crate) fn remove_event(&self, event_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE event_id = ?1", params![event_id])
            .map_err(|e| format!("cannot remove a message from the index: {e}"))?;
        Ok(())
    }

    /// Everything for one room (the user forgets a room).
    pub(crate) fn remove_room(&self, room_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE room_id = ?1", params![room_id])
            .map_err(|e| format!("cannot clear a room from the index: {e}"))?;
        Ok(())
    }

    /// Drop the whole index.
    pub(crate) fn clear(&self) -> Result<(), String> {
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

        let sql = if room_id.is_empty() {
            "SELECT m.event_id, m.room_id, m.sender, m.sender_name, m.body,
                    m.msgtype, m.ts
             FROM messages_fts f
             JOIN messages m ON m.id = f.rowid
             WHERE messages_fts MATCH ?1
             ORDER BY m.ts DESC
             LIMIT ?2 OFFSET ?3"
        } else {
            "SELECT m.event_id, m.room_id, m.sender, m.sender_name, m.body,
                    m.msgtype, m.ts
             FROM messages_fts f
             JOIN messages m ON m.id = f.rowid
             WHERE messages_fts MATCH ?1 AND m.room_id = ?4
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
    let body = body_source.get("body").and_then(|v| v.as_str())?.to_owned();
    if body.trim().is_empty() {
        return None;
    }
    Some(Indexable { event_id: target_id, sender, body, msgtype, ts })
}

/// Page a room backwards, indexing what each page brings in.
///
/// Read after every page: `RoomEventCache::events()` reads the in-memory
/// linked chunk, which Lightning's jump-to-live trim shrinks to about one
/// page, so reading only at the end would collect one page.
///
/// Bounded by `max_pages`, stops at the room start, and checks `stop`
/// between pages so sign-out never waits. Returns (pages run,
/// reached_start, rows written).
pub(crate) async fn deep_index_room(
    room: &Room,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    stop: &std::sync::atomic::AtomicBool,
    max_pages: u16,
) -> Result<(u16, bool, usize), String> {
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| format!("no event cache for the room: {e}"))?;
    let room_id = room.room_id().as_str();

    let mut written = 0usize;
    // What is already loaded, before any request.
    if let Ok(batch) = collect_from(room, &room_cache).await {
        if let Ok(mut guard) = index.lock() {
            if let Some(ix) = guard.as_mut() {
                written += write_batch(ix, room_id, &batch);
            }
        }
    }

    let pagination = room_cache.pagination();
    let mut pages = 0u16;
    let mut reached_start = false;
    for _ in 0..max_pages {
        if stop.load(std::sync::atomic::Ordering::Relaxed) {
            break;
        }
        let outcome = pagination
            .run_backwards_once(DEEP_PAGE_SIZE)
            .await
            .map_err(|e| format!("pagination failed: {e}"))?;
        pages += 1;
        if let Ok(batch) = collect_from(room, &room_cache).await {
            if let Ok(mut guard) = index.lock() {
                if let Some(ix) = guard.as_mut() {
                    written += write_batch(ix, room_id, &batch);
                }
            }
        }
        if outcome.reached_start {
            reached_start = true;
            break;
        }
    }
    Ok((pages, reached_start, written))
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

/// Sweep every joined room's cached events into the index. Returns (rooms
/// visited, rows written). `stop` is checked between rooms.
pub(crate) async fn sweep(
    client: &matrix_sdk::Client,
    index: &std::sync::Arc<std::sync::Mutex<Option<SearchIndex>>>,
    stop: &std::sync::atomic::AtomicBool,
) -> (usize, usize) {
    let mut rooms_done = 0usize;
    let mut written = 0usize;
    for room in client.joined_rooms() {
        if stop.load(std::sync::atomic::Ordering::Relaxed) {
            break;
        }
        if rooms_done >= SWEEP_ROOM_BATCH {
            break;
        }
        rooms_done += 1;
        // Lock per room, not for the whole sweep, so a search is not blocked.
        let events = match collect_room(&room).await {
            Ok(events) => events,
            Err(_) => continue,
        };
        if let Ok(mut guard) = index.lock() {
            if let Some(ix) = guard.as_mut() {
                written += write_batch(ix, room.room_id().as_str(), &events);
            }
        }
    }
    (rooms_done, written)
}

/// One room's indexable events, ready to write. Separate from the write so
/// the index mutex is never held across an `.await`.
pub(crate) struct RoomBatch {
    ordinary: Vec<Indexable>,
    edits: Vec<Indexable>,
    names: std::collections::HashMap<String, String>,
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
    for event in &events {
        let Ok(raw) = event.raw().deserialize_as::<serde_json::Value>() else {
            continue;
        };
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
    Ok(RoomBatch { ordinary, edits, names })
}

/// Write one collected room. Synchronous, so the index mutex is held for a
/// bounded burst of inserts, never across a network wait.
pub(crate) fn write_batch(index: &SearchIndex, room_id: &str, batch: &RoomBatch) -> usize {
    let ids: Vec<String> = batch.ordinary.iter().map(|c| c.event_id.clone()).collect();
    let known = index.known(&ids).unwrap_or_default();
    let mut written = 0usize;
    for (item, is_edit) in batch
        .ordinary
        .iter()
        .map(|c| (c, false))
        .chain(batch.edits.iter().map(|r| (r, true)))
    {
        if !is_edit && known.contains(&item.event_id) {
            continue;
        }
        let display = batch.names.get(&item.sender).cloned().unwrap_or_default();
        if index
            .upsert(
                &item.event_id,
                room_id,
                &item.sender,
                &display,
                &item.body,
                &item.msgtype,
                item.ts,
            )
            .is_ok()
        {
            written += 1;
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
}
