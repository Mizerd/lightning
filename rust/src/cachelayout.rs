//! The account's store directory: what must survive, and what is a cache.
//!
//! The store directory is the one the account RECORDED (C++
//! `AccountIdentity::rustStorePath`, CLAUDE.md §6). It holds two kinds of data:
//!
//! - durable: the crypto store (Olm account, Megolm sessions), the state store
//!   (rooms, the send queue, the sync position), the search index, and
//!   Lightning's small records beside them.
//! - throwaway: the SDK event cache, the SDK media store
//!   (`lightning-media-store/`, mediastore.rs) and the kept media files
//!   (`lightning-media-files/`, mediafiles.rs), all fetched again from the
//!   homeserver on demand.
//!
//! The throwaway half lives in `cache/` INSIDE the store directory, as
//! Sable's `cache/` beside its `store/` does. Inside rather than beside, so
//! every path that deletes or moves a store (sign-out, account removal, a
//! failed sign-in's rollback, the repair quarantine, `--reset-crypto-store`)
//! takes the cache with it unchanged, and the cache's place follows from the
//! recorded store path with no second record that could disagree with it.
//! The event cache's write churn sits in its own directory, which a
//! `CACHEDIR.TAG` marks for backup tools. Clearing the caches while the app
//! runs is About's "Reset local cache" (aboutinfo.rs and AboutController,
//! through the SDK), not this module.
//!
//! Older installs keep those caches directly in the store directory. `migrate`
//! moves them, only while this process owns the OS profile lock (no other
//! process can have the files open; a GUI running Unguarded, without the
//! lock, never splits) and before any store of the account is opened. Every
//! move is one rename and safe to repeat, and the crypto and state stores are
//! never opened, moved or written here.
//!
//! WHERE each cache opens is decided per cache, from the directory, at every
//! open (`resolve`): a cache still in the store directory opens THERE, one
//! that is not opens in `cache/` when that is a directory. So a move that
//! failed or was interrupted leaves each cache usable where it is, and no
//! session ever runs on an in-memory event cache while a copy exists on disk.
//! That matters: matrix-sdk 0.18 keeps no gap when a sync is not `limited`
//! and the room's linked chunk already holds events (event_cache/caches/room/
//! state.rs), so an on-disk event cache that missed a session's syncs while
//! the state store's sync position moved on would hold a hole nothing ever
//! fills. Only one copy of the event cache is ever written: when both places
//! hold one, the store directory's is the newer (only an older build writes
//! there once `cache/` exists), and it replaces the other.
//!
//! `lightning-store-layout` records that the split finished, for diagnostics;
//! nothing depends on it. Log lines name no path and no account.

use std::collections::HashMap;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

/// The cache directory's name inside the store directory.
pub(crate) const CACHE_DIR: &str = "cache";
/// SqliteEventCacheStore's file name (matrix-sdk-sqlite 0.18
/// event_cache_store.rs `DATABASE_NAME`), the same in both places.
pub(crate) const EVENT_CACHE_DB: &str = "matrix-sdk-event-cache.sqlite3";
const SQLITE_SIDECARS: [&str; 3] = ["-wal", "-shm", "-journal"];
/// The cache directories that move as a whole.
const MOVED_DIRS: [&str; 2] = [crate::mediastore::DIR_NAME, crate::mediafiles::DIR_NAME];
/// In the store directory: written once every cache is in `cache/`.
const LAYOUT_FILE: &str = "lightning-store-layout";
const LAYOUT_SPLIT: &str = "split-cache-v1";
const CACHEDIR_TAG: &str = "CACHEDIR.TAG";
const CACHEDIR_TAG_BODY: &str = "Signature: 8a477f597d28d172789f06886806bc55\n\
# This file is a cache directory tag created by Lightning.\n\
# Everything here is fetched again from the homeserver when needed.\n\
# For information about cache directory tags see https://bford.info/cachedir/\n";

/// The directory each cache is opened in this session: the store directory
/// itself, or its `cache/`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Placement {
    /// Holds `matrix-sdk-event-cache.sqlite3`.
    pub event_cache: PathBuf,
    /// Holds `lightning-media-store/`.
    pub media_store: PathBuf,
    /// Holds `lightning-media-files/`.
    pub kept_files: PathBuf,
}

impl Placement {
    /// "split" (all in `cache/`), "legacy" (all in the store directory) or
    /// "mixed". A state, never a path.
    pub(crate) fn name(&self, store: &Path) -> &'static str {
        let cache = store.join(CACHE_DIR);
        let all = [&self.event_cache, &self.media_store, &self.kept_files];
        if all.iter().all(|dir| **dir == cache) {
            "split"
        } else if all.iter().all(|dir| *dir == store) {
            "legacy"
        } else {
            "mixed"
        }
    }
}

// ---- What was opened, by store path ----------------------------------------

fn registry() -> &'static Mutex<HashMap<PathBuf, Placement>> {
    static REGISTRY: OnceLock<Mutex<HashMap<PathBuf, Placement>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

fn record(store: &Path, placement: &Placement) {
    if let Ok(mut map) = registry().lock() {
        map.insert(store.to_path_buf(), placement.clone());
    }
}

/// Where the stores of `store` were opened, or, before any open, where they
/// would be now.
pub(crate) fn placement_of(store: &Path) -> Placement {
    let opened = registry().lock().ok().and_then(|map| map.get(store).cloned());
    opened.unwrap_or_else(|| resolve(store))
}

/// The kept-files directory (mediafiles.rs) for the account at `store`.
pub(crate) fn kept_files_dir(store: &Path) -> Option<PathBuf> {
    if store.as_os_str().is_empty() {
        return None;
    }
    Some(placement_of(store).kept_files.join(crate::mediafiles::DIR_NAME))
}

// ---- Reading the directory ---------------------------------------------------

fn present(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn sidecar(db: &Path, suffix: &str) -> PathBuf {
    let mut name = db.as_os_str().to_owned();
    name.push(suffix);
    PathBuf::from(name)
}

/// `cache/` exists and is a plain directory. A symlink or a file there is
/// neither used nor touched.
fn cache_dir_usable(store: &Path) -> bool {
    fs::symlink_metadata(store.join(CACHE_DIR)).is_ok_and(|meta| meta.is_dir())
}

/// Where each cache opens, without changing anything: the store directory
/// while the cache is still there (it is the newest copy, and moving it is
/// `migrate`'s business), otherwise `cache/` when that is a usable directory,
/// otherwise the store directory. Sidecars of a database that is no longer in
/// the store directory are inert and do not count.
pub(crate) fn resolve(store: &Path) -> Placement {
    let usable = cache_dir_usable(store);
    let pick = |name: &str| {
        if present(&store.join(name)) || !usable {
            store.to_path_buf()
        } else {
            store.join(CACHE_DIR)
        }
    };
    Placement {
        event_cache: pick(EVENT_CACHE_DB),
        media_store: pick(crate::mediastore::DIR_NAME),
        kept_files: pick(crate::mediafiles::DIR_NAME),
    }
}

// ---- Filesystem primitives ---------------------------------------------------

fn remove_file_if_present(path: &Path) -> bool {
    match fs::remove_file(path) {
        Ok(()) => true,
        Err(err) => err.kind() == io::ErrorKind::NotFound,
    }
}

fn remove_dir_if_present(path: &Path) -> bool {
    match fs::remove_dir_all(path) {
        Ok(()) => true,
        Err(err) => err.kind() == io::ErrorKind::NotFound,
    }
}

/// Delete a SQLite database: the database first, so a sidecar that outlives
/// a crash is inert, then its sidecars. False when anything is left.
fn remove_database(db: &Path) -> bool {
    if !remove_file_if_present(db) {
        return false;
    }
    let mut all = true;
    for suffix in SQLITE_SIDECARS {
        all &= remove_file_if_present(&sidecar(db, suffix));
    }
    all
}

#[cfg(unix)]
fn restrict(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = fs::set_permissions(path, fs::Permissions::from_mode(mode));
}

#[cfg(not(unix))]
fn restrict(_path: &Path, _mode: u32) {}

/// Write `body` to `path` whole or not at all: a temporary file, synced, then
/// renamed over it.
fn write_atomically(path: &Path, body: &str) -> io::Result<()> {
    if fs::read_to_string(path).is_ok_and(|text| text == body) {
        return Ok(());
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    // A crash leftover would keep its old mode: OpenOptions::mode applies only
    // to a file it creates.
    let _ = fs::remove_file(&tmp);
    {
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options.open(&tmp)?;
        file.write_all(body.as_bytes())?;
        file.sync_all()?;
    }
    fs::rename(&tmp, path)
}

fn create_cache_dir(cache: &Path) -> io::Result<()> {
    fs::create_dir_all(cache)?;
    restrict(cache, 0o700);
    let tag = cache.join(CACHEDIR_TAG);
    if !present(&tag) {
        // Best effort: only backup tools read it.
        let _ = write_atomically(&tag, CACHEDIR_TAG_BODY);
    }
    Ok(())
}

/// `cache/` holds nothing but its tag.
fn cache_dir_is_empty(cache: &Path) -> bool {
    fs::read_dir(cache).is_ok_and(|entries| {
        entries
            .flatten()
            .all(|entry| entry.file_name() == CACHEDIR_TAG)
    })
}

// ---- Migration ---------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Settled {
    Absent,
    /// One file, no sidecars: one rename moves all of it.
    Ready,
    /// Not a readable database. The SDK could not open it either.
    Damaged,
}

fn damaged_or(err: rusqlite::Error) -> io::Result<Settled> {
    match err.sqlite_error_code() {
        Some(rusqlite::ErrorCode::NotADatabase | rusqlite::ErrorCode::DatabaseCorrupt) => {
            Ok(Settled::Damaged)
        }
        // The text may name the path; only the kind is kept.
        _ => Err(io::Error::other("the old event cache could not be read")),
    }
}

/// Make the old event cache self-contained before it moves: open it (which
/// replays a WAL a crash left behind), fold the WAL into the database and
/// close it, which deletes the -wal and -shm. Moving the three files one by
/// one instead could separate a database from its WAL and lose committed
/// events, which behind a sync position that has moved on is a hole in a
/// timeline nothing would ever fill.
fn settle_database(db: &Path) -> io::Result<Settled> {
    match fs::symlink_metadata(db) {
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(Settled::Absent),
        Err(err) => return Err(err),
        Ok(meta) if !meta.is_file() => {
            return Err(io::Error::other("the old event cache is not a file"))
        }
        Ok(_) => {}
    }
    let conn = match rusqlite::Connection::open_with_flags(
        db,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
    ) {
        Ok(conn) => conn,
        Err(err) => return damaged_or(err),
    };
    // The first query reads the header: a file that is not a database fails
    // here.
    let checked = conn
        .query_row("SELECT count(*) FROM sqlite_master", [], |row| row.get::<_, i64>(0))
        .and_then(|_| {
            conn.query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |row| row.get::<_, i64>(0))
        });
    if let Err(err) = checked {
        drop(conn);
        return damaged_or(err);
    }
    if let Err((_, err)) = conn.close() {
        return damaged_or(err);
    }
    if SQLITE_SIDECARS.iter().any(|suffix| present(&sidecar(db, suffix))) {
        // Something else still has it open (another handle, an indexer).
        return Err(io::Error::other("the old event cache still has a journal"));
    }
    Ok(Settled::Ready)
}

/// One step of the migration, for the crash test.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Step {
    CacheDir,
    EventCache,
    Dir(usize),
    Record,
}

/// Every step, in order.
#[cfg(test)]
pub(crate) fn steps() -> Vec<Step> {
    let mut all = vec![Step::CacheDir, Step::EventCache];
    all.extend((0..MOVED_DIRS.len()).map(Step::Dir));
    all.push(Step::Record);
    all
}

/// Move the event cache into `cache/`. Ok(true) when it moved.
fn migrate_event_cache(store: &Path, cache: &Path) -> io::Result<bool> {
    let db = store.join(EVENT_CACHE_DB);
    let moved = match settle_database(&db)? {
        Settled::Absent => false,
        Settled::Damaged => {
            if !remove_database(&db) {
                return Err(io::Error::other("a damaged event cache could not be removed"));
            }
            // A copy in cache/ is older than the damaged one (an older build
            // wrote the store directory's while the sync position moved on),
            // so opening it would leave a hole the next sync never fills. Go
            // empty instead, which is safe.
            if !remove_database(&cache.join(EVENT_CACHE_DB)) {
                return Err(io::Error::other("a stale event cache could not be removed"));
            }
            false
        }
        Settled::Ready => {
            // The copy in cache/ is the older one (only an older build writes
            // the store directory's once cache/ exists).
            let target = cache.join(EVENT_CACHE_DB);
            if !remove_database(&target) {
                return Err(io::Error::other("a stale event cache could not be removed"));
            }
            fs::rename(&db, &target)?;
            true
        }
    };
    // Sidecars of a database that is no longer here: inert, and nothing opens
    // that name again while it is absent.
    for suffix in SQLITE_SIDECARS {
        let _ = remove_file_if_present(&sidecar(&db, suffix));
    }
    Ok(moved)
}

/// Move one cache directory into `cache/`. Ok(true) when it moved.
fn migrate_dir(store: &Path, cache: &Path, name: &str) -> io::Result<bool> {
    let from = store.join(name);
    match fs::symlink_metadata(&from) {
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(err) => return Err(err),
        Ok(meta) if !meta.is_dir() => {
            return Err(io::Error::other("an old cache directory is not a directory"))
        }
        Ok(_) => {}
    }
    let to = cache.join(name);
    if !remove_dir_if_present(&to) {
        return Err(io::Error::other("a stale cache directory could not be removed"));
    }
    fs::rename(&from, &to)?;
    Ok(true)
}

/// Move the caches into `cache/`; returns how many moved. Each cache moves on
/// its own, so one that cannot (a file another program holds) stays usable
/// where it is while the others move. When nothing could move and `cache/`
/// was made for nothing, it is removed again, so the install stays as it was.
pub(crate) fn migrate(store: &Path) -> io::Result<usize> {
    migrate_with(store, &mut |_| true)
}

/// The same; `proceed` is asked before each step and a `false` stops right
/// there, as a crash would, with no clean-up.
pub(crate) fn migrate_with(
    store: &Path,
    proceed: &mut dyn FnMut(Step) -> bool,
) -> io::Result<usize> {
    let stopped = || io::Error::new(io::ErrorKind::Interrupted, "stopped");
    let cache = store.join(CACHE_DIR);
    if present(&cache) && !cache_dir_usable(store) {
        return Err(io::Error::other("the cache path is not a directory"));
    }

    if !proceed(Step::CacheDir) {
        return Err(stopped());
    }
    create_cache_dir(&cache)?;

    let mut moved = 0;
    let mut first_error = None;
    if !proceed(Step::EventCache) {
        return Err(stopped());
    }
    match migrate_event_cache(store, &cache) {
        Ok(true) => moved += 1,
        Ok(false) => {}
        Err(err) => first_error = Some(err),
    }
    for (index, name) in MOVED_DIRS.iter().enumerate() {
        if !proceed(Step::Dir(index)) {
            return Err(stopped());
        }
        match migrate_dir(store, &cache, name) {
            Ok(true) => moved += 1,
            Ok(false) => {}
            Err(err) => {
                first_error.get_or_insert(err);
            }
        }
    }

    if let Some(err) = first_error {
        if cache_dir_is_empty(&cache) {
            let _ = fs::remove_dir_all(&cache);
        }
        return Err(err);
    }
    if !proceed(Step::Record) {
        return Err(stopped());
    }
    write_atomically(&store.join(LAYOUT_FILE), LAYOUT_SPLIT)?;
    Ok(moved)
}

/// Whether the record says the split finished. Read by the tests; the file
/// is for a person looking at the directory.
#[cfg(test)]
pub(crate) fn recorded_split(store: &Path) -> bool {
    fs::read_to_string(store.join(LAYOUT_FILE)).is_ok_and(|text| text == LAYOUT_SPLIT)
}

// ---- The entry point -----------------------------------------------------------

/// Decide where the account's caches open this session, first moving them
/// out of the store directory when `may_change`. Blocking; called before any
/// store of the account is opened.
///
/// `may_change`: this process owns the OS profile lock (storelocking.rs; a GUI
/// running Unguarded and the smoke test never set it, so they never split) and
/// is not preserving caches for an I/O audit. Even then nothing moves while a
/// client in this process still holds a store opened from this directory
/// (storeclose.rs).
pub(crate) fn prepare(store: &Path, may_change: bool) -> Placement {
    // Serialised: a restore whose first build timed out starts a second while
    // the first one's blocking half may still be running.
    static SERIAL: Mutex<()> = Mutex::new(());
    let _serial = SERIAL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());

    let may_change = may_change && !crate::storeclose::any_held(store);
    let migrated = if may_change { Some(migrate(store)) } else { None };
    let placement = resolve(store);
    match migrated {
        Some(Ok(0)) | None => {}
        Some(Ok(moved)) => eprintln!(
            "lightning: store layout: moved {moved} cache(s) into the cache directory"
        ),
        Some(Err(err)) => eprintln!(
            "lightning: store layout: a cache could not be moved out of the store \
             ({:?}); each is used where it is (layout {}) and moved at a later start",
            err.kind(),
            placement.name(store)
        ),
    }
    record(store, &placement);
    placement
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::collections::BTreeMap;

    use matrix_sdk::cross_process_lock::CrossProcessLockConfig;
    use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
    use matrix_sdk::ruma::events::room::MediaSource;
    use matrix_sdk::ruma::OwnedMxcUri;
    use matrix_sdk::{SqliteEventCacheStore, SqliteMediaStore, SqliteStoreConfig};
    use matrix_sdk_base::media::store::{IgnoreMediaRetentionPolicy, MediaStore};

    /// A store directory under the system temp dir, removed on drop.
    struct Store(PathBuf);

    impl Store {
        fn new(tag: &str) -> Self {
            let path = std::env::temp_dir().join(format!(
                "lightning-cachelayout-{tag}-{}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).expect("test store dir");
            Store(path)
        }

        fn cache(&self) -> PathBuf {
            self.0.join(CACHE_DIR)
        }

        fn split(&self) -> Placement {
            Placement {
                event_cache: self.cache(),
                media_store: self.cache(),
                kept_files: self.cache(),
            }
        }

        fn legacy(&self) -> Placement {
            Placement {
                event_cache: self.0.clone(),
                media_store: self.0.clone(),
                kept_files: self.0.clone(),
            }
        }
    }

    impl Drop for Store {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    const EVENT_MARKER: &str = "LIGHTNING-EVENT-CACHE-MARKER-7d2e";
    const MEDIA_FILE: &str = "0123abcd.media";
    const MEDIA_FILE_BYTES: &[u8] = b"kept media file bytes";
    const URI: &str = "mxc://example.org/CacheLayoutMedia";

    /// Every byte of the durable stores, by file name: the crypto and state
    /// databases WITH their sidecars, the search index and the other records.
    /// None of it may change, ever.
    fn durable_bytes(store: &Path) -> BTreeMap<String, Vec<u8>> {
        let mut all = BTreeMap::new();
        for entry in fs::read_dir(store).unwrap().flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let durable = name.starts_with("matrix-sdk-crypto")
                || name.starts_with("matrix-sdk-state")
                || name.starts_with("lightning-search")
                || name == "lightning-index-all.json";
            if durable && entry.file_type().unwrap().is_file() {
                all.insert(name, fs::read(entry.path()).unwrap());
            }
        }
        all
    }

    /// A real SQLite database with one marker row, closed cleanly.
    fn sqlite_with_row(path: &Path, table: &str, value: &str) {
        let conn = rusqlite::Connection::open(path).unwrap();
        conn.execute_batch(&format!(
            "PRAGMA journal_mode = wal; CREATE TABLE IF NOT EXISTS {table} (v TEXT);"
        ))
        .unwrap();
        conn.execute(&format!("INSERT INTO {table} (v) VALUES (?1)"), [value]).unwrap();
    }

    /// The durable half of an install: crypto and state databases, each with
    /// a -wal/-shm of made-up bytes (any open or checkpoint of them would
    /// change those), plus the search index and a record.
    fn durable_fixture(store: &Path) {
        sqlite_with_row(&store.join("matrix-sdk-crypto.sqlite3"), "olm", "account pickle");
        sqlite_with_row(&store.join("matrix-sdk-state.sqlite3"), "state", "session");
        fs::write(store.join("matrix-sdk-crypto.sqlite3-wal"), b"crypto wal bytes").unwrap();
        fs::write(store.join("matrix-sdk-crypto.sqlite3-shm"), b"crypto shm bytes").unwrap();
        fs::write(store.join("matrix-sdk-state.sqlite3-wal"), b"state wal bytes").unwrap();
        sqlite_with_row(&store.join("lightning-search.sqlite3"), "idx", "message text");
        fs::write(store.join("lightning-index-all.json"), b"{\"progress\":1}").unwrap();
    }

    /// An event cache as a crashed process leaves it: the SDK's own schema,
    /// and a committed row that is still only in the WAL. Built elsewhere and
    /// copied in while its writer is still open, with no -shm.
    async fn crashed_event_cache(dir: &Path) {
        let scratch = Store::new(&format!(
            "ec-src-{}",
            dir.file_name().unwrap().to_string_lossy()
        ));
        let sdk = SqliteEventCacheStore::open_with_config(&SqliteStoreConfig::new(&scratch.0))
            .await
            .unwrap();
        sdk.close().await.unwrap();
        drop(sdk);
        let db = scratch.0.join(EVENT_CACHE_DB);
        let conn = rusqlite::Connection::open(&db).unwrap();
        conn.execute_batch("PRAGMA wal_autocheckpoint = 0; CREATE TABLE lightning_marker (v TEXT);")
            .unwrap();
        conn.execute("INSERT INTO lightning_marker (v) VALUES (?1)", [EVENT_MARKER]).unwrap();
        fs::create_dir_all(dir).unwrap();
        fs::copy(&db, dir.join(EVENT_CACHE_DB)).unwrap();
        let wal = sidecar(&db, "-wal");
        assert!(fs::metadata(&wal).unwrap().len() > 0, "the fixture has nothing in its WAL");
        fs::copy(&wal, sidecar(&dir.join(EVENT_CACHE_DB), "-wal")).unwrap();
        drop(conn);
    }

    fn event_marker_in(db: &Path) -> Option<String> {
        let conn =
            rusqlite::Connection::open_with_flags(db, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY)
                .ok()?;
        conn.query_row("SELECT v FROM lightning_marker", [], |row| row.get(0)).ok()
    }

    fn request() -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(URI)),
            format: MediaFormat::File,
        }
    }

    /// A media store directory as mediastore.rs leaves it (a plain config:
    /// the key does not matter to the layout), and a kept-files directory.
    async fn media_fixture(parent: &Path) {
        let dir = parent.join(crate::mediastore::DIR_NAME);
        let media =
            SqliteMediaStore::open_with_config(&SqliteStoreConfig::new(&dir)).await.unwrap();
        media
            .add_media_content(&request(), b"media bytes".to_vec(), IgnoreMediaRetentionPolicy::No)
            .await
            .unwrap();
        media.close().await.unwrap();
        fs::write(dir.join("lightning-key-id"), b"0011").unwrap();
        let files = parent.join(crate::mediafiles::DIR_NAME);
        fs::create_dir_all(&files).unwrap();
        fs::write(files.join(MEDIA_FILE), MEDIA_FILE_BYTES).unwrap();
    }

    /// An install from before the split: everything in the store directory.
    async fn legacy_install(store: &Path) {
        durable_fixture(store);
        crashed_event_cache(store).await;
        media_fixture(store).await;
    }

    fn assert_split_with_everything(store: &Store) {
        assert_eq!(resolve(&store.0), store.split());
        assert_eq!(resolve(&store.0).name(&store.0), "split");
        for suffix in SQLITE_SIDECARS {
            assert!(!present(&sidecar(&store.0.join(EVENT_CACHE_DB), suffix)));
        }
        assert_eq!(
            event_marker_in(&store.cache().join(EVENT_CACHE_DB)).as_deref(),
            Some(EVENT_MARKER),
            "an event committed only to the WAL was lost in the move"
        );
        assert_eq!(
            fs::read(store.cache().join(crate::mediafiles::DIR_NAME).join(MEDIA_FILE)).unwrap(),
            MEDIA_FILE_BYTES
        );
        assert_eq!(
            fs::read(store.cache().join(crate::mediastore::DIR_NAME).join("lightning-key-id"))
                .unwrap(),
            b"0011"
        );
        assert!(recorded_split(&store.0));
        assert!(present(&store.cache().join(CACHEDIR_TAG)));
    }

    // ---- fresh install ----

    #[tokio::test]
    async fn a_fresh_install_opens_its_caches_in_the_cache_directory() {
        let store = Store::new("fresh");
        let (config, _probe) = crate::mediastore::open_account_stores_with(
            &store.0,
            CrossProcessLockConfig::SingleProcess,
            true,
        )
        .await
        .unwrap();
        assert_eq!(placement_of(&store.0), store.split());
        assert!(store.cache().join(EVENT_CACHE_DB).is_file(), "the event cache is not in cache/");
        assert!(!store.0.join(EVENT_CACHE_DB).exists(), "an event cache in the store directory");
        assert!(store.0.join("matrix-sdk-crypto.sqlite3").is_file());
        assert!(store.0.join("matrix-sdk-state.sqlite3").is_file());
        assert!(!store.cache().join("matrix-sdk-crypto.sqlite3").exists());
        assert!(!store.cache().join("matrix-sdk-state.sqlite3").exists());
        assert_eq!(fs::read_to_string(store.cache().join(CACHEDIR_TAG)).unwrap(), CACHEDIR_TAG_BODY);
        assert!(recorded_split(&store.0));
        assert_eq!(kept_files_dir(&store.0), Some(store.cache().join(crate::mediafiles::DIR_NAME)));
        drop(config);
    }

    // ---- migration ----

    #[tokio::test]
    async fn an_existing_install_is_moved_and_its_crypto_store_is_untouched() {
        let store = Store::new("migrate");
        legacy_install(&store.0).await;
        let durable = durable_bytes(&store.0);
        assert_eq!(durable.len(), 7, "the durable fixture is incomplete");
        assert_eq!(resolve(&store.0), store.legacy());

        assert_eq!(prepare(&store.0, true), store.split());
        assert_split_with_everything(&store);
        assert_eq!(durable_bytes(&store.0), durable, "a durable store changed");

        // The SDK opens what was moved, schema and all.
        let moved = SqliteEventCacheStore::open_with_config(&SqliteStoreConfig::new(store.cache()))
            .await
            .expect("the moved event cache does not open");
        moved.close().await.unwrap();
        drop(moved);

        // Idempotent: a second start changes nothing.
        assert_eq!(migrate(&store.0).unwrap(), 0);
        assert_split_with_everything(&store);
        assert_eq!(durable_bytes(&store.0), durable);
    }

    // Through the real open path: the stores the SDK is handed are the moved
    // ones, and nothing is opened at the old place.
    #[tokio::test]
    async fn the_client_build_opens_the_moved_event_cache() {
        let store = Store::new("open-moved");
        crashed_event_cache(&store.0).await;
        media_fixture(&store.0).await;
        let (config, _probe) = crate::mediastore::open_account_stores_with(
            &store.0,
            CrossProcessLockConfig::SingleProcess,
            true,
        )
        .await
        .unwrap();
        assert_eq!(placement_of(&store.0), store.split());
        assert!(!store.0.join(EVENT_CACHE_DB).exists());
        assert!(!store.0.join(crate::mediafiles::DIR_NAME).exists());
        drop(config);
        assert_eq!(
            event_marker_in(&store.cache().join(EVENT_CACHE_DB)).as_deref(),
            Some(EVENT_MARKER)
        );
    }

    // Without the profile lock another process may have the files open:
    // nothing moves, and the install opens exactly as before.
    #[tokio::test]
    async fn a_process_without_the_profile_lock_moves_nothing() {
        let store = Store::new("unguarded");
        legacy_install(&store.0).await;
        let durable = durable_bytes(&store.0);
        let before = fs::read(store.0.join(EVENT_CACHE_DB)).unwrap();
        assert_eq!(prepare(&store.0, false), store.legacy());
        assert!(!store.cache().exists());
        assert_eq!(fs::read(store.0.join(EVENT_CACHE_DB)).unwrap(), before);
        assert!(present(&sidecar(&store.0.join(EVENT_CACHE_DB), "-wal")));
        assert_eq!(durable_bytes(&store.0), durable);
        assert_eq!(kept_files_dir(&store.0), Some(store.0.join(crate::mediafiles::DIR_NAME)));
    }

    // A crash after any step: every cache opens where it IS at the next start
    // (never in memory, never a stale copy), and the next start with the lock
    // finishes, losing nothing.
    #[tokio::test]
    async fn a_crash_at_any_step_is_finished_at_the_next_start() {
        let all = steps();
        assert_eq!(all.len(), 5);
        for (index, stop_at) in all.into_iter().enumerate() {
            let store = Store::new(&format!("crash-{index}"));
            legacy_install(&store.0).await;
            let durable = durable_bytes(&store.0);
            let result = migrate_with(&store.0, &mut |step| step != stop_at);
            assert_eq!(
                result.map_err(|err| err.kind()),
                Err(io::ErrorKind::Interrupted),
                "{stop_at:?}"
            );
            // Each cache is opened in the one place it is.
            let half = prepare(&store.0, false);
            for (dir, name) in [
                (&half.event_cache, EVENT_CACHE_DB),
                (&half.media_store, crate::mediastore::DIR_NAME),
                (&half.kept_files, crate::mediafiles::DIR_NAME),
            ] {
                assert!(dir.join(name).exists(), "{stop_at:?}: {name} opened where it is not");
            }
            let expected = match stop_at {
                Step::CacheDir | Step::EventCache => "legacy",
                Step::Dir(_) => "mixed",
                Step::Record => "split",
            };
            assert_eq!(half.name(&store.0), expected, "{stop_at:?}");
            assert_eq!(durable_bytes(&store.0), durable, "{stop_at:?}");

            assert_eq!(prepare(&store.0, true), store.split(), "{stop_at:?}");
            assert_split_with_everything(&store);
            assert_eq!(durable_bytes(&store.0), durable, "{stop_at:?}");
        }
    }

    // Downgraded, then upgraded again: the older build wrote a new event
    // cache and kept files in the store directory. Those are the newer ones,
    // are opened in place until they move, and then replace the old copies.
    #[tokio::test]
    async fn after_a_downgrade_the_copy_in_the_store_directory_wins() {
        let store = Store::new("downgrade");
        legacy_install(&store.0).await;
        assert_eq!(prepare(&store.0, true), store.split());
        sqlite_with_row(&store.0.join(EVENT_CACHE_DB), "lightning_marker", "newer");
        fs::create_dir_all(store.0.join(crate::mediafiles::DIR_NAME)).unwrap();
        fs::write(store.0.join(crate::mediafiles::DIR_NAME).join("newer.media"), b"n").unwrap();
        let unguarded = prepare(&store.0, false);
        assert_eq!(unguarded.event_cache, store.0, "the stale copy in cache/ was preferred");
        assert_eq!(unguarded.kept_files, store.0);
        assert_eq!(unguarded.media_store, store.cache());

        assert_eq!(prepare(&store.0, true), store.split());
        assert_eq!(event_marker_in(&store.cache().join(EVENT_CACHE_DB)).as_deref(), Some("newer"));
        let files = store.cache().join(crate::mediafiles::DIR_NAME);
        assert!(files.join("newer.media").exists());
        assert!(!files.join(MEDIA_FILE).exists(), "the stale copy was merged, not replaced");
    }

    #[test]
    fn an_event_cache_that_is_not_a_database_is_dropped() {
        let store = Store::new("damaged");
        durable_fixture(&store.0);
        let durable = durable_bytes(&store.0);
        // An older copy in cache/ (upgrade, downgrade, then the downgraded
        // build's copy was damaged): it must not be opened in its place.
        fs::create_dir_all(store.cache()).unwrap();
        sqlite_with_row(&store.cache().join(EVENT_CACHE_DB), "lightning_marker", "stale");
        assert_eq!(event_marker_in(&store.cache().join(EVENT_CACHE_DB)).as_deref(), Some("stale"));
        fs::write(store.0.join(EVENT_CACHE_DB), vec![b'x'; 4096]).unwrap();
        assert_eq!(prepare(&store.0, true), store.split());
        assert!(!store.0.join(EVENT_CACHE_DB).exists());
        assert!(!store.cache().join(EVENT_CACHE_DB).exists(), "the stale copy in cache/ survived");
        for suffix in SQLITE_SIDECARS {
            assert!(!present(&sidecar(&store.cache().join(EVENT_CACHE_DB), suffix)));
        }
        assert_eq!(durable_bytes(&store.0), durable);
    }

    // The Windows case: another program holds the old event cache, so its
    // WAL cannot be folded in and it cannot move. Nothing is separated from
    // its WAL, the event cache keeps opening where it is (never in memory,
    // so no session's syncs are lost from it), and cache/ made for nothing is
    // removed again.
    #[test]
    fn an_event_cache_that_cannot_move_stays_in_use_where_it_is() {
        let store = Store::new("held");
        let db = store.0.join(EVENT_CACHE_DB);
        let writer = rusqlite::Connection::open(&db).unwrap();
        writer
            .execute_batch(
                "PRAGMA journal_mode = wal; PRAGMA wal_autocheckpoint = 0; \
                 CREATE TABLE lightning_marker (v TEXT); \
                 INSERT INTO lightning_marker (v) VALUES ('held');",
            )
            .unwrap();
        assert!(migrate(&store.0).is_err());
        assert!(!store.cache().exists(), "an empty cache/ was left behind");
        assert_eq!(prepare(&store.0, true), store.legacy());
        assert!(db.exists());
        drop(writer);
        // Released: the next start moves it.
        assert_eq!(prepare(&store.0, true), store.split());
        assert_eq!(event_marker_in(&store.cache().join(EVENT_CACHE_DB)).as_deref(), Some("held"));
    }

    // The same with media alongside: what can move does, what cannot stays
    // in use where it is.
    #[tokio::test]
    async fn the_caches_move_independently() {
        let store = Store::new("independent");
        media_fixture(&store.0).await;
        let db = store.0.join(EVENT_CACHE_DB);
        let writer = rusqlite::Connection::open(&db).unwrap();
        writer
            .execute_batch(
                "PRAGMA journal_mode = wal; PRAGMA wal_autocheckpoint = 0; \
                 CREATE TABLE lightning_marker (v TEXT); \
                 INSERT INTO lightning_marker (v) VALUES ('held');",
            )
            .unwrap();
        let placement = prepare(&store.0, true);
        assert_eq!(placement.event_cache, store.0);
        assert_eq!(placement.media_store, store.cache());
        assert_eq!(placement.kept_files, store.cache());
        assert!(!recorded_split(&store.0));
        drop(writer);
    }

    #[test]
    fn a_cache_path_that_is_not_a_directory_is_never_used_or_touched() {
        let store = Store::new("not-a-dir");
        fs::write(store.cache(), b"something else").unwrap();
        assert_eq!(prepare(&store.0, true), store.legacy());
        assert_eq!(fs::read(store.cache()).unwrap(), b"something else");
        assert_eq!(kept_files_dir(&store.0), Some(store.0.join(crate::mediafiles::DIR_NAME)));
    }

    // While a client in this process still holds stores from this directory,
    // nothing is moved under it.
    #[tokio::test]
    async fn nothing_moves_under_a_store_this_process_holds() {
        let store = Store::new("held-in-process");
        legacy_install(&store.0).await;
        let held = std::sync::Arc::new(7u32);
        let mut probe = crate::storeclose::StoreProbe::default();
        probe.store("event_cache", &held);
        crate::storeclose::register(&store.0, probe);
        assert_eq!(prepare(&store.0, true), store.legacy());
        assert!(!store.cache().exists(), "moved under an open store");
        drop(held);
        assert_eq!(prepare(&store.0, true), store.split());
    }

    // ---- sign-out ----

    // Sign-out deletes the RECORDED store directory (C++
    // removeAccountRustState); the cache is inside it, so no part of the
    // account's cache can outlive it.
    #[tokio::test]
    async fn the_cache_lives_inside_the_store_directory() {
        let store = Store::new("signout");
        legacy_install(&store.0).await;
        assert_eq!(prepare(&store.0, true), store.split());
        assert_eq!(store.cache().parent(), Some(store.0.as_path()));
        fs::remove_dir_all(&store.0).unwrap();
        assert!(!store.cache().exists());
    }
}
