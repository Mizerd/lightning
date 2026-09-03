//! The LOCAL message index: SQLite FTS5 over what this client has seen.
//!
//! # Why this exists
//!
//! `search.rs` is server search (`POST /_matrix/client/v3/search`), and the
//! homeserver can only search what it can READ. In an encrypted room it holds
//! ciphertext, so server search returns nothing there — which for most people
//! is most of their conversations. This module is the other half: an index
//! built from the plaintext this client already has, so search works the same
//! in an encrypted room as in a public one.
//!
//! # Where the plaintext already is
//!
//! Building this does NOT introduce decrypted message text to disk. The SDK's
//! own event cache already persists it: `SqliteEventCacheStore::encode_event`
//! serializes the whole `TimelineEvent` including its `Decrypted` variant, its
//! `encode_value` is a no-op when no cypher is configured, and Lightning opens
//! `sqlite_store(path, None)`. The redecryptor's own documentation says it
//! "replace[s] the events in the cache" once a late room key arrives. So the
//! bodies are in `matrix-rust-sdk-store` beside this file already, protected
//! by the same 0700 directory and nothing else.
//!
//! That is a real and separate finding, recorded in `docs/local-search.md`.
//! What it means HERE is narrow and worth stating precisely: this index is a
//! second copy of something already present, in a form that is easier to
//! query. It does not change what an attacker with read access to the store
//! directory can learn; it changes how quickly. Encrypting the whole store at
//! rest is the fix for both, and it is a decision of its own — `sqlite_store`
//! builds ONE config for the state, event-cache, media and crypto stores, and
//! matrix-sdk-sqlite mints a new cipher when it finds no cipher row, so
//! handing it a passphrase would leave every existing install unable to decode
//! its own account pickle.
//!
//! # Tokenizer: trigram, and why
//!
//! Measured in this module's own tests rather than assumed:
//!
//! * `unicode61` splits on whitespace and punctuation. Chinese has neither
//!   between words, so a whole sentence becomes ONE token and no word inside
//!   it can ever be found. That is a silent, total failure for a language
//!   Lightning ships.
//! * `trigram` indexes 3-character sequences, so it matches substrings in
//!   every script — including CJK — and gives the "find the word I half
//!   remember" behaviour people expect from a chat search box. Its cost is a
//!   larger index and a hard 3-character minimum.
//!
//! A visible, explainable limit ("type at least three characters") beats an
//! invisible one ("Chinese never matches"), so: trigram.
//!
//! trigram matches raw code points and folds nothing, so the FOLDING is ours:
//! [`fold`] runs over both the indexed text and the query, which is what makes
//! `koln` find `Köln` and `ß` behave. The folded copies are what FTS5 sees;
//! the original text is kept alongside and is what the UI displays.

use std::collections::HashSet;
use std::path::Path;

use rusqlite::{params, Connection, OptionalExtension};
use unicode_normalization::UnicodeNormalization;

/// File name inside the account's store directory.
pub(crate) const INDEX_FILE: &str = "lightning-search.sqlite3";

/// Rows kept before the oldest are evicted. An index is a convenience, not a
/// second copy of the account: unbounded growth on a machine the user did not
/// choose to spend is its own defect. At roughly 100 bytes of text per message
/// this is tens of megabytes of index — large enough that nobody reaches it by
/// ordinary use, small enough that a runaway cannot fill a disk.
pub(crate) const MAX_ROWS: i64 = 250_000;

/// Shortest query the trigram tokenizer can match. Stated to the user rather
/// than silently returning nothing.
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

/// What the index currently holds, for an honest "search covers N messages".
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct IndexStats {
    pub messages: i64,
    pub rooms: i64,
}

/// Case- and diacritic-folded text, NFC-normalised.
///
/// Applied to BOTH the indexed copy and the query, so the two always agree.
/// Doing it here rather than leaning on `remove_diacritics` keeps one rule for
/// every tokenizer: `trigram` has no folding of its own at all.
///
/// Combining marks are dropped by category (Mn), not by a table of characters,
/// so this covers every script rather than the Latin ones somebody remembered.
pub(crate) fn fold(text: &str) -> String {
    let lowered = text.to_lowercase();
    lowered
        .nfd()
        .filter(|c| !unicode_normalization::char::is_combining_mark(*c))
        .collect::<String>()
        .nfc()
        .collect()
}

/// The user's typed text as an FTS5 MATCH expression.
///
/// ONE QUOTED PHRASE, always. FTS5's query language has operators (`AND`,
/// `OR`, `NOT`, `*`, `^`, `:`, parentheses) and a user typing any of them into
/// a chat search box means the characters, not the operator — and an unbalanced
/// quote is a syntax error that would surface as "search failed" for what is
/// really "you typed a quotation mark". Quoting the whole thing makes the query
/// a literal substring search, which with trigram is exactly the intent.
///
/// A double quote inside a phrase is escaped by doubling it, per SQLite.
pub(crate) fn match_expression(query: &str) -> String {
    let folded = fold(query.trim());
    format!("\"{}\"", folded.replace('"', "\"\""))
}

/// True when a query is long enough for the trigram tokenizer to match at all.
/// Counted in CHARACTERS, not bytes: three Chinese characters are nine bytes
/// and are a perfectly good query.
pub(crate) fn query_is_long_enough(query: &str) -> bool {
    fold(query.trim()).chars().count() >= MIN_QUERY_CHARS
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
        // WAL so an indexing write cannot block a search read, and NORMAL
        // synchronous because losing the tail of an INDEX on a power cut costs
        // a re-index of a few messages, not data.
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
                     ts            INTEGER NOT NULL,
                     fold_body     TEXT NOT NULL DEFAULT '',
                     fold_sender   TEXT NOT NULL DEFAULT ''
                 );
                 CREATE INDEX IF NOT EXISTS messages_room_ts
                     ON messages(room_id, ts DESC);
                 CREATE INDEX IF NOT EXISTS messages_ts ON messages(ts);

                 -- EXTERNAL CONTENT: the FTS table stores only the index and
                 -- reads the text from `messages`, so the folded copies are
                 -- not held twice. content_rowid ties them together.
                 CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(
                     fold_body,
                     fold_sender,
                     content='messages',
                     content_rowid='id',
                     tokenize='trigram'
                 );

                 -- External content means FTS5 does NOT see writes to the
                 -- content table on its own; these triggers are the contract.
                 CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN
                     INSERT INTO messages_fts(rowid, fold_body, fold_sender)
                     VALUES (new.id, new.fold_body, new.fold_sender);
                 END;
                 CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN
                     INSERT INTO messages_fts(messages_fts, rowid, fold_body, fold_sender)
                     VALUES ('delete', old.id, old.fold_body, old.fold_sender);
                 END;
                 CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN
                     INSERT INTO messages_fts(messages_fts, rowid, fold_body, fold_sender)
                     VALUES ('delete', old.id, old.fold_body, old.fold_sender);
                     INSERT INTO messages_fts(rowid, fold_body, fold_sender)
                     VALUES (new.id, new.fold_body, new.fold_sender);
                 END;",
            )
            .map_err(|e| format!("cannot prepare the search index: {e}"))?;
        Ok(())
    }

    /// Add or update one message.
    ///
    /// UPSERT on the event id, which is what makes an EDIT correct: an
    /// `m.replace` carries the new text for an event already indexed, and
    /// inserting it again would leave the old wording findable forever. The
    /// caller resolves the edit; this stores whatever the current text is.
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
        // Nothing to find in an empty body. Storing it would spend a row and a
        // trigram entry on a message no query can ever return.
        if body.trim().is_empty() {
            return Ok(());
        }
        self.conn
            .execute(
                "INSERT INTO messages
                    (event_id, room_id, sender, sender_name, body, msgtype, ts,
                     fold_body, fold_sender)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
                 ON CONFLICT(event_id) DO UPDATE SET
                     body        = excluded.body,
                     sender_name = excluded.sender_name,
                     msgtype     = excluded.msgtype,
                     fold_body   = excluded.fold_body,
                     fold_sender = excluded.fold_sender",
                params![
                    event_id,
                    room_id,
                    sender,
                    sender_name,
                    body,
                    msgtype,
                    ts,
                    fold(body),
                    fold(sender_name),
                ],
            )
            .map_err(|e| format!("cannot index a message: {e}"))?;
        Ok(())
    }

    /// A redaction removes the row outright.
    ///
    /// Not "blank the body": a redacted message is one somebody asked to be
    /// unsayable, and leaving it findable by its own text would be the single
    /// worst thing this index could do.
    pub(crate) fn remove_event(&self, event_id: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM messages WHERE event_id = ?1", params![event_id])
            .map_err(|e| format!("cannot remove a message from the index: {e}"))?;
        Ok(())
    }

    /// Everything for one room — used when the user forgets a room.
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

    /// Evict the oldest rows past [`MAX_ROWS`].
    ///
    /// Oldest FIRST, by timestamp: the newest messages are the ones a search
    /// is most often looking for, and an index that forgot this week to keep
    /// 2019 would be worse than useless.
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

    /// True when this event is already indexed. Lets a backfill skip work
    /// without paying for a write.
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
        // Chunked: SQLite's default parameter limit is 999 and a backfill can
        // hand over a whole room's chunk.
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

    /// Search. `room_id` empty searches every indexed room.
    ///
    /// NEWEST FIRST, not by bm25 relevance. A chat search answers "when did we
    /// talk about this", and the server-side search beside it is ordered by
    /// recency too — two search boxes in one client that disagree about what
    /// "first result" means is worse than either ordering.
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
// ONE source: the SDK's own event cache. Not the live timeline, which exists
// only for the room the user has open — an index built from it would answer
// "search the room you are looking at", which is not search.
//
// `RoomEventCache::events()` returns the room's cached events, already
// DECRYPTED (the store persists the `Decrypted` variant and the redecryptor
// replaces UTDs in place), so an encrypted room indexes exactly like a public
// one. That is the whole reason this approach works at all.
//
// Coverage is therefore "everything Lightning has cached", which grows as the
// user reads and as [`deep_index_room`] pages backwards. It is not "everything
// that ever happened in the room", and the UI says so with a count rather than
// implying completeness.

use matrix_sdk::room::Room;

/// Rows pulled per backward page while deep-indexing.
const DEEP_PAGE_SIZE: u16 = 100;

/// One message worth indexing, lifted out of a cached event.
struct Indexable {
    event_id: String,
    sender: String,
    body: String,
    msgtype: String,
    ts: i64,
}

/// Pull the searchable text out of one cached event.
///
/// Deliberately narrow. A state change, a reaction, a receipt and a redacted
/// event have no text a person would search for, and indexing them would spend
/// rows and trigram entries to make "joined the room" the most common hit in
/// the account.
///
/// A REDACTED event is skipped by construction rather than by a check: its
/// content has no `body` left once the server has redacted it.
fn indexable_from(raw: &serde_json::Value) -> Option<Indexable> {
    let event_id = raw.get("event_id")?.as_str()?.to_owned();
    let sender = raw.get("sender")?.as_str()?.to_owned();
    let ts = raw.get("origin_server_ts")?.as_i64()?;
    if raw.get("type")?.as_str()? != "m.room.message" {
        return None;
    }
    let content = raw.get("content")?;

    // An EDIT carries the replacement under `m.new_content` and leaves a
    // "* fallback" in `body` for clients that do not understand edits. Indexing
    // the fallback would make every edited message findable by an asterisk and
    // by its OLD text; the edit is applied to the original event's row instead,
    // keyed by the relation target.
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
    // An attachment's `body` IS its filename in Matrix, which is worth
    // finding — "that pdf someone sent" is a real search.
    let body = body_source.get("body").and_then(|v| v.as_str())?.to_owned();
    if body.trim().is_empty() {
        return None;
    }
    Some(Indexable { event_id: target_id, sender, body, msgtype, ts })
}

/// Index every cached event of one room. Returns how many rows were written.
///
/// Existing rows are skipped in ONE query rather than re-written: a sweep runs
/// over every room on every wake-up, and paying a write per already-indexed
/// message would make the common case the expensive one. Edits are the
/// exception — they must overwrite, so they are never skipped.
pub(crate) async fn index_room_from_cache(
    room: &Room,
    index: &SearchIndex,
) -> Result<usize, String> {
    // Room::event_cache() is the PUBLIC accessor; EventCache::for_room is
    // private in 0.18. The drop handles it returns must be kept alive for the
    // duration of the read, which is what binding them here does — dropping
    // them can shrink the linked chunk out from under the walk.
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| format!("no event cache for the room: {e}"))?;
    let events = room_cache
        .events()
        .await
        .map_err(|e| format!("cannot read the event cache: {e}"))?;

    let mut candidates: Vec<Indexable> = Vec::new();
    let mut replacements: Vec<Indexable> = Vec::new();
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
            if is_replacement {
                replacements.push(item);
            } else {
                candidates.push(item);
            }
        }
    }

    let ids: Vec<String> = candidates.iter().map(|c| c.event_id.clone()).collect();
    let known = index.known(&ids)?;

    let room_id = room.room_id().as_str();
    let mut written = 0usize;
    // Display names resolved ONCE per sender per room, from the store — the
    // no-sync accessor, so a sweep cannot turn into one request per message.
    let mut names: std::collections::HashMap<String, String> =
        std::collections::HashMap::new();
    // Ordinary messages first, then the edits — so an edit always lands ON
    // TOP of the row its target just wrote, whatever order the cache held
    // them in. Only the ordinary pass skips what is already indexed; an edit
    // must overwrite by definition, and skipping it would leave the pre-edit
    // wording findable forever.
    for (item, is_edit) in candidates
        .iter()
        .map(|c| (c, false))
        .chain(replacements.iter().map(|r| (r, true)))
    {
        if !is_edit && known.contains(&item.event_id) {
            continue;
        }
        let display = match names.get(&item.sender) {
            Some(name) => name.clone(),
            None => {
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
                names.insert(item.sender.clone(), resolved.clone());
                resolved
            }
        };
        index.upsert(
            &item.event_id,
            room_id,
            &item.sender,
            &display,
            &item.body,
            &item.msgtype,
            item.ts,
        )?;
        written += 1;
    }
    Ok(written)
}

/// Page a room backwards so the index covers history the user has not
/// scrolled to.
///
/// This is what turns "search what you have read" into "search this room".
/// BOUNDED by `max_pages`: a room with years of history is a long download and
/// an unbounded one would be a background task nobody asked for that never
/// ends. Stops early when the room start is reached.
///
/// Returns (pages run, reached_start).
pub(crate) async fn deep_page_room(
    room: &Room,
    max_pages: u16,
) -> Result<(u16, bool), String> {
    // Room::event_cache() is the PUBLIC accessor; EventCache::for_room is
    // private in 0.18. The drop handles it returns must be kept alive for the
    // duration of the read, which is what binding them here does — dropping
    // them can shrink the linked chunk out from under the walk.
    let (room_cache, _drop_handles) = room
        .event_cache()
        .await
        .map_err(|e| format!("no event cache for the room: {e}"))?;
    let pagination = room_cache.pagination();
    let mut pages = 0u16;
    for _ in 0..max_pages {
        let outcome = pagination
            .run_backwards_once(DEEP_PAGE_SIZE)
            .await
            .map_err(|e| format!("pagination failed: {e}"))?;
        pages += 1;
        if outcome.reached_start {
            return Ok((pages, true));
        }
    }
    Ok((pages, false))
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

    // ── The load-bearing build fact ──────────────────────────────────────
    //
    // FTS5 is a COMPILE-TIME option of SQLite. Without `bundled-sqlite` the
    // build links whatever libsqlite3 the host happens to have, and whether
    // search works at all would be decided per platform, thirty minutes into
    // a release pipeline. This asserts the schema builds, which it cannot
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
        // Chinese has no spaces between words. unicode61 would make the whole
        // sentence one token and find NOTHING here; trigram finds it.
        assert_eq!(ix.search("一个测", "", 10, 0).unwrap().len(), 1);
        // A substring inside a word, which is what people actually type.
        assert_eq!(ix.search("ell", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("ривет", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("العا", "", 10, 0).unwrap().len(), 1);
    }

    // ── The cost of trigram, stated rather than hidden ───────────────────
    #[test]
    fn a_query_shorter_than_three_characters_is_refused_not_silently_empty() {
        let ix = index();
        add(&ix, "$1", "!r:x", "ok then", 1000);
        assert!(!query_is_long_enough("ok"));
        assert!(!query_is_long_enough(" a "));
        // Trimmed BEFORE counting, so trailing space does not buy a character
        // and a query that looks long enough is not accepted and then matched
        // against nothing.
        assert!(!query_is_long_enough("ok "));
        assert!(query_is_long_enough(" okay "));
        assert!(query_is_long_enough("测试消"));
        // Three CJK characters are nine bytes: the minimum is CHARACTERS, or
        // every non-Latin query would be refused for being "too short".
        assert_eq!("测试消".len(), 9);
        assert_eq!(ix.search("ok", "", 10, 0).unwrap().len(), 0);
    }

    #[test]
    fn folding_makes_accents_and_case_irrelevant() {
        let ix = index();
        add(&ix, "$1", "!r:x", "Köln im Sommer", 1000);
        add(&ix, "$2", "!r:x", "CAFÉ au lait", 2000);
        // trigram folds nothing on its own — this passes only because both
        // the stored copy and the query go through fold().
        assert_eq!(ix.search("koln", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("KÖLN", "", 10, 0).unwrap().len(), 1);
        assert_eq!(ix.search("cafe", "", 10, 0).unwrap().len(), 1);
        assert_eq!(fold("Köln"), "koln");
        assert_eq!(fold("ÉCOLE"), "ecole");
    }

    // ── The query is text, not a language ────────────────────────────────
    #[test]
    fn ftsOperatorsTypedByAUserAreCharactersNotOperators() {
        let ix = index();
        add(&ix, "$1", "!r:x", "deploy AND release notes", 1000);
        add(&ix, "$2", "!r:x", "deploy only", 2000);
        // As an FTS5 operator this would match both rows. As text it matches
        // the one that contains the phrase.
        let hits = ix.search("deploy AND release", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].event_id, "$1");
    }

    #[test]
    fn aQuotationMarkIsASearchTermNotASyntaxError() {
        let ix = index();
        add(&ix, "$1", "!r:x", "he said \"hello\" loudly", 1000);
        // Unescaped, this would be an FTS5 syntax error and the whole search
        // would fail rather than the query simply not matching.
        let hits = ix.search("said \"hello\"", "", 10, 0).unwrap();
        assert_eq!(hits.len(), 1);
        // And a lone quote must not blow up either.
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
        // The limit is clamped rather than trusted.
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
        // Prove the policy without writing a quarter of a million rows: the
        // eviction SQL is the same one MAX_ROWS drives.
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
        // And under the cap, prune() does nothing at all.
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
        // Past SQLite's 999-parameter limit, which is why known() chunks.
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
    //
    // The narrow part of the whole feature. Everything the SDK cached goes
    // through here, and each of these cases is a way to put the wrong thing
    // in the index or to miss the right one.
    fn raw(json: serde_json::Value) -> serde_json::Value {
        json
    }

    #[test]
    fn onlyRoomMessagesWithTextAreIndexed() {
        // A state change has no text a person searches for, and indexing it
        // would make "joined the room" the commonest hit in the account.
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
        // A REDACTED message has had its content emptied by the server, so it
        // falls out here by construction rather than by a check that could be
        // forgotten.
        assert!(indexable_from(&raw(serde_json::json!({
            "event_id": "$3", "sender": "@a:x", "origin_server_ts": 1,
            "type": "m.room.message", "content": {}
        })))
        .is_none());
        // And an empty body is nothing to find.
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
        // In Matrix an attachment's `body` IS its filename, and "that pdf
        // someone sent" is a real search.
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
        // THE case that decides whether editing works. An m.replace carries a
        // "* new text" fallback in `body` for clients that do not understand
        // edits; indexing THAT would make every edited message findable by an
        // asterisk, and would leave the original row carrying the old wording
        // forever because the edit arrived under a different event id.
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
        // m.in_reply_to is also an m.relates_to, and treating it like a
        // replacement would file every reply on top of the message it answers
        // — silently deleting the original from the index.
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
        // Every field here is attacker-influenced: the events come off the
        // wire. None of these may be an unwrap.
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
            // An m.replace with no target: the relation is unusable, so the
            // event must be dropped rather than filed under a guess.
            serde_json::json!({"event_id": "$1", "sender": "@a:x",
                               "origin_server_ts": 1, "type": "m.room.message",
                               "content": {"body": "* x",
                                           "m.relates_to": {"rel_type": "m.replace"}}}),
        ] {
            assert!(indexable_from(&value).is_none(), "{value}");
        }
    }

    // ── The tokenizer decision, evidenced ────────────────────────────────
    #[test]
    fn unicode61WouldHaveMadeChineseUnsearchable() {
        // Not a test of Lightning: a test of the ALTERNATIVE, kept so the
        // choice of trigram is defended by a measurement rather than a
        // sentence in a comment.
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
