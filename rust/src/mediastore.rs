//! The account's SDK media store, encrypted at rest with a per-account key.
//!
//! matrix-sdk keeps two things in its media store: media it was asked to
//! cache, and, through its send queue, every attachment the user sends. The
//! send queue writes the original file BEFORE the upload and keeps it after
//! (`send_queue/upload.rs`, `cache_media` and
//! `update_media_cache_keys_after_upload`), encrypted rooms included. Opened
//! as `sqlite_store(path, None)` opened it, all of that sat on disk unencrypted.
//!
//! Here the media store is opened on its own, in `lightning-media-store/`
//! inside the account's store directory, with a 32-byte key that C++ keeps in
//! the OS keyring (src/matrix/MediaStoreKey.cpp) and hands over per handle
//! (`mx_rust_set_media_store_key`). Only the media store: the state,
//! event-cache and crypto stores keep the config they always had, because one
//! secret over the crypto store would strand every existing install's account
//! pickle (docs/security-audit-2026-09-02.md).
//!
//! Without a key this session (keyring locked or unreadable, a first sign-in,
//! a key that could not be written) the media store is in memory: nothing is
//! kept and nothing reaches disk. It is never opened as a plaintext file,
//! with one exception: the old plaintext store is used again while it still
//! holds an attachment the send queue has not uploaded, because deleting it
//! would lose that attachment. That is bounded: a pinned attachment nobody
//! has touched for ABANDONED_AFTER counts as abandoned, and a file that is not
//! a readable database is deleted outright, so the exception cannot outlive
//! a week of an upload that never finishes. Every session spent in it writes
//! new sent attachments there in plaintext, which is why it is bounded.
//!
//! A key-mismatch wipe (below) also drops attachments still waiting to upload
//! in the encrypted store: they were unreadable without the old key anyway.
//!
//! A key that no longer matches the store on disk (its record was deleted,
//! so C++ made a new one) cannot open it. The store is only a cache, so it is
//! deleted. Which key a store belongs to is recorded beside it as a one-way
//! identifier (`key_id`), so "this store is not for this key" is a fact, not
//! an inference from a failed open.

use std::collections::HashMap;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, SystemTime};

use matrix_sdk::config::StoreConfig;
use matrix_sdk::cross_process_lock::CrossProcessLockConfig;
use matrix_sdk::{
    SqliteCryptoStore, SqliteEventCacheStore, SqliteMediaStore, SqliteStateStore,
    SqliteStoreConfig,
};
use matrix_sdk_base::media::store::MemoryMediaStore;
use sha2::{Digest, Sha256};

use crate::storeclose::StoreProbe;

/// The encrypted store's directory, inside the account's store directory, so
/// it is deleted with the account.
pub(crate) const DIR_NAME: &str = "lightning-media-store";
/// The one-way identifier of the key the store in DIR_NAME belongs to.
const KEY_ID_FILE: &str = "lightning-key-id";
/// SqliteMediaStore's file name, the same in the old location and the new.
const MEDIA_DB: &str = "matrix-sdk-media.sqlite3";
const LEGACY_SIDECARS: [&str; 2] = ["matrix-sdk-media.sqlite3-wal", "matrix-sdk-media.sqlite3-shm"];
/// A pinned attachment in the old store not read or written for this long is
/// taken as abandoned (an upload that wedged and was never cancelled), so the
/// old plaintext store cannot stay in use for ever.
pub(crate) const ABANDONED_AFTER: Duration = Duration::from_secs(7 * 24 * 60 * 60);
/// The holder `sqlite_store()` gave every store's cross-process lock:
/// `ClientBuilder::DEFAULT_CROSS_PROCESS_STORE_LOCKS_HOLDER_NAME`, private in
/// matrix-sdk 0.18 (client/builder/mod.rs:137). The client's own lock config
/// is the builder default, which is the same name.
const STORE_LOCK_HOLDER: &str = "main";

/// Where this account's media store lives this session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Active {
    /// The old plaintext store, kept for one more session for an attachment
    /// the send queue has not uploaded yet. Admits no encrypted-room media.
    Legacy,
    /// Encrypted with the account's key. `admits_encrypted`: that key is held
    /// by a secure OS keyring, so encrypted-room media may be cached too.
    Encrypted { admits_encrypted: bool },
    /// In memory: no key this session. Nothing is kept.
    Memory,
}

impl Active {
    /// Whether anything fetched may be kept between sessions.
    pub(crate) fn persists(self) -> bool {
        !matches!(self, Active::Memory)
    }

    /// Whether media from an encrypted room may be kept.
    pub(crate) fn admits_encrypted(self) -> bool {
        matches!(self, Active::Encrypted { admits_encrypted: true })
    }

    /// The name `mx_rust_media_store_state` reports. A state, never a path.
    pub(crate) fn state_name(self) -> &'static str {
        match self {
            Active::Encrypted { admits_encrypted: true } => "encrypted_admits",
            Active::Encrypted { admits_encrypted: false } => "encrypted",
            Active::Legacy => "legacy",
            Active::Memory => "memory",
        }
    }
}

/// A key copy that is zeroed when dropped. Best effort, like the password
/// buffer on the C++ side; SqliteStoreConfig keeps its own zeroizing copy.
#[derive(Clone)]
pub(crate) struct KeyBytes(pub(crate) [u8; 32]);

impl Drop for KeyBytes {
    fn drop(&mut self) {
        for byte in self.0.iter_mut() {
            // Volatile, so the zeroing of a dying buffer is not optimised away.
            unsafe { std::ptr::write_volatile(byte, 0) };
        }
    }
}

// ---- The per-handle key, by store path -------------------------------------
//
// build_client() takes only a store path, so the key is looked up by it. Each
// entry belongs to the handle (RustClient) that set it: a handle being retired
// for the same account must not remove the entry of the handle replacing it.

struct Entry {
    owner: usize,
    key: Option<KeyBytes>,
    admits_encrypted: bool,
    active: Active,
}

fn registry() -> &'static Mutex<HashMap<PathBuf, Entry>> {
    static REGISTRY: OnceLock<Mutex<HashMap<PathBuf, Entry>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Set this handle's key. `None`: no key this session, so memory.
/// `admits_encrypted` is honoured only with a key.
pub(crate) fn set_key(
    owner: usize,
    store_path: &Path,
    key: Option<KeyBytes>,
    admits_encrypted: bool,
) {
    let admits_encrypted = admits_encrypted && key.is_some();
    if let Ok(mut map) = registry().lock() {
        map.insert(
            store_path.to_path_buf(),
            Entry { owner, key, admits_encrypted, active: Active::Memory },
        );
    }
}

/// Drop this handle's entry, and only this handle's.
pub(crate) fn forget(owner: usize, store_path: &Path) {
    if let Ok(mut map) = registry().lock() {
        if map.get(store_path).is_some_and(|entry| entry.owner == owner) {
            map.remove(store_path);
        }
    }
}

/// The handle that last set this directory's key (`set_key`'s `owner`), if
/// any. The stores opened now belong to it (storeclose.rs).
pub(crate) fn owner_for(store_path: &Path) -> Option<usize> {
    registry()
        .lock()
        .ok()
        .and_then(|map| map.get(store_path).map(|entry| entry.owner))
}

fn key_for(store_path: &Path) -> (Option<KeyBytes>, bool) {
    registry()
        .lock()
        .ok()
        .and_then(|map| {
            map.get(store_path)
                .map(|entry| (entry.key.clone(), entry.admits_encrypted))
        })
        .unwrap_or((None, false))
}

fn record_active(store_path: &Path, active: Active) {
    if let Ok(mut map) = registry().lock() {
        if let Some(entry) = map.get_mut(store_path) {
            entry.active = active;
        }
    }
}

/// What the account at `store_path` opened. Memory when nothing was opened
/// for it, so a caller never keeps what it cannot account for.
pub(crate) fn active(store_path: &Path) -> Active {
    registry()
        .lock()
        .ok()
        .and_then(|map| map.get(store_path).map(|entry| entry.active))
        .unwrap_or(Active::Memory)
}

// ---- Decisions (pure) -------------------------------------------------------

/// The old plaintext store, `<store>/matrix-sdk-media.sqlite3`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Legacy {
    Absent,
    /// Its -wal/-shm without the database: inert, removed.
    Leftovers,
    /// Present, and nothing in it waits for an upload.
    Idle,
    /// Present, and every pinned row is older than ABANDONED_AFTER.
    Abandoned,
    /// Present, and not a readable SQLite database (not one at all, or
    /// corrupt). matrix-sdk could not open it either.
    Damaged,
    /// Present, with a row the send queue pinned until its upload finishes,
    /// touched within ABANDONED_AFTER.
    Pending,
    /// Present, and it could not be asked (permissions, I/O). Treated as
    /// Pending; matrix-sdk's own open is likely to fail the same way.
    Unknown,
}

/// The encrypted store directory, measured against the key in hand.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum OnDisk {
    Nothing,
    Matches,
    /// Recorded for another key: unreadable with this one.
    Differs,
    /// Files, but no key record: not something this module left.
    Unmarked,
    /// The key record could not be read. Never a reason to delete.
    Unreadable,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Plan {
    KeepLegacy,
    Memory { drop_legacy: bool },
    Encrypted { drop_legacy: bool, wipe: bool },
}

/// The whole decision. Without a key the encrypted store is never touched: a
/// key that exists but cannot be read now may be readable at the next start.
pub(crate) fn plan(legacy: Legacy, have_key: bool, disk: OnDisk) -> Plan {
    let drop_legacy = match legacy {
        Legacy::Pending | Legacy::Unknown => return Plan::KeepLegacy,
        Legacy::Absent => false,
        Legacy::Leftovers | Legacy::Idle | Legacy::Abandoned | Legacy::Damaged => true,
    };
    if !have_key {
        return Plan::Memory { drop_legacy };
    }
    match disk {
        OnDisk::Nothing | OnDisk::Matches => Plan::Encrypted { drop_legacy, wipe: false },
        OnDisk::Differs | OnDisk::Unmarked => Plan::Encrypted { drop_legacy, wipe: true },
        OnDisk::Unreadable => Plan::Memory { drop_legacy },
    }
}

/// A one-way identifier of a key: which key a store belongs to, without
/// saying anything about the key.
pub(crate) fn key_id(key: &[u8; 32]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(b"lightning media store key id v1\0");
    hasher.update(key);
    hasher.finalize()[..16].iter().map(|byte| format!("{byte:02x}")).collect()
}

// ---- Filesystem ------------------------------------------------------------

fn failed(err: &rusqlite::Error) -> Legacy {
    match err.sqlite_error_code() {
        Some(rusqlite::ErrorCode::NotADatabase | rusqlite::ErrorCode::DatabaseCorrupt) => {
            Legacy::Damaged
        }
        _ => Legacy::Unknown,
    }
}

pub(crate) fn legacy_state(store_path: &Path, now: SystemTime) -> Legacy {
    let db = store_path.join(MEDIA_DB);
    match fs::symlink_metadata(&db) {
        Err(err) if err.kind() == io::ErrorKind::NotFound => {
            let leftovers = LEGACY_SIDECARS
                .iter()
                .any(|name| fs::symlink_metadata(store_path.join(name)).is_ok());
            return if leftovers { Legacy::Leftovers } else { Legacy::Absent };
        }
        Err(_) => return Legacy::Unknown,
        Ok(meta) if !meta.is_file() => return Legacy::Unknown,
        Ok(_) => {}
    }
    // Read-write: a read-only open of a WAL database can fail for want of its
    // -shm, and the file is ours. Closing checkpoints the WAL into it.
    let conn = match rusqlite::Connection::open_with_flags(
        &db,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
    ) {
        Ok(conn) => conn,
        Err(err) => return failed(&err),
    };
    // A database without the table has nothing pinned. SQLite reads the
    // header here, so a file that is not a database fails on this query.
    let has_table: rusqlite::Result<bool> = conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'media')",
        [],
        |row| row.get(0),
    );
    match has_table {
        Ok(true) => {}
        Ok(false) => return Legacy::Idle,
        Err(err) => return failed(&err),
    }
    // `ignore_policy` is set only by the send queue, for a file and its
    // thumbnail until their upload finishes (IgnoreMediaRetentionPolicy::Yes
    // in send_queue/upload.rs), and cleared when it does. `last_access` is
    // seconds since the epoch, refreshed on every read and write of the row.
    let cutoff = now
        .checked_sub(ABANDONED_AFTER)
        .and_then(|at| at.duration_since(SystemTime::UNIX_EPOCH).ok())
        .map_or(0, |since| since.as_secs() as i64);
    let fresh: rusqlite::Result<bool> = conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM media WHERE ignore_policy AND last_access >= ?1)",
        [cutoff],
        |row| row.get(0),
    );
    match fresh {
        Ok(true) => return Legacy::Pending,
        Ok(false) => {}
        Err(err) => return failed(&err),
    }
    let stale: rusqlite::Result<bool> = conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM media WHERE ignore_policy)",
        [],
        |row| row.get(0),
    );
    match stale {
        Ok(true) => Legacy::Abandoned,
        Ok(false) => Legacy::Idle,
        Err(err) => failed(&err),
    }
}

fn remove_if_present(path: &Path) -> bool {
    match fs::remove_file(path) {
        Ok(()) => true,
        Err(err) => err.kind() == io::ErrorKind::NotFound,
    }
}

/// Delete the old plaintext store. The database first: once it is gone its
/// -wal/-shm are inert, and nothing here opens that name again, so a sidecar
/// that outlives a crash is removed at the next start and never replayed.
/// False when anything is left.
pub(crate) fn remove_legacy(store_path: &Path) -> bool {
    if !remove_if_present(&store_path.join(MEDIA_DB)) {
        return false;
    }
    let mut all = true;
    for name in LEGACY_SIDECARS {
        if !remove_if_present(&store_path.join(name)) {
            all = false;
        }
    }
    all
}

pub(crate) fn on_disk(dir: &Path, key: &[u8; 32]) -> OnDisk {
    match fs::read_to_string(dir.join(KEY_ID_FILE)) {
        Ok(text) if text.trim() == key_id(key) => OnDisk::Matches,
        Ok(_) => OnDisk::Differs,
        Err(err) if err.kind() == io::ErrorKind::NotFound => match fs::read_dir(dir) {
            Ok(mut entries) => {
                if entries.next().is_some() {
                    OnDisk::Unmarked
                } else {
                    OnDisk::Nothing
                }
            }
            Err(err) if err.kind() == io::ErrorKind::NotFound => OnDisk::Nothing,
            Err(_) => OnDisk::Unreadable,
        },
        Err(_) => OnDisk::Unreadable,
    }
}

/// Record which key the store belongs to, before the store is created, so a
/// store never exists without its record. Written to a temporary file and
/// renamed, so the record is whole or absent.
fn ensure_marked(dir: &Path, key: &[u8; 32]) -> io::Result<()> {
    fs::create_dir_all(dir)?;
    restrict(dir, 0o700);
    let path = dir.join(KEY_ID_FILE);
    let id = key_id(key);
    if fs::read_to_string(&path).is_ok_and(|text| text.trim() == id) {
        return Ok(());
    }
    let tmp = dir.join(format!("{KEY_ID_FILE}.tmp"));
    {
        let mut options = fs::OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options.open(&tmp)?;
        file.write_all(id.as_bytes())?;
        file.sync_all()?;
    }
    fs::rename(&tmp, &path)
}

#[cfg(unix)]
fn restrict(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = fs::set_permissions(path, fs::Permissions::from_mode(mode));
}

#[cfg(not(unix))]
fn restrict(_path: &Path, _mode: u32) {}

/// 0700 on the directory and 0600 on its files: the SDK creates the database
/// files at the process umask.
fn restrict_tree(dir: &Path) {
    restrict(dir, 0o700);
    if let Ok(entries) = fs::read_dir(dir) {
        for entry in entries.flatten() {
            if entry.file_type().is_ok_and(|kind| kind.is_file()) {
                restrict(&entry.path(), 0o600);
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Prepared {
    Legacy,
    Memory,
    Encrypted,
}

/// Decide and carry out the filesystem half. Blocking. Log lines name no
/// path, account or key.
fn prepare(store_path: &Path, key: Option<&[u8; 32]>) -> Prepared {
    // Serialised: a restore whose first build timed out starts a second while
    // the first one's blocking half may still be running.
    static PREPARE: Mutex<()> = Mutex::new(());
    let _serial = PREPARE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());

    let dir = store_path.join(DIR_NAME);
    let legacy = legacy_state(store_path, SystemTime::now());
    let disk = key.map_or(OnDisk::Nothing, |key| on_disk(&dir, key));
    let decided = plan(legacy, key.is_some(), disk);
    let drop_legacy = match decided {
        Plan::KeepLegacy => {
            eprintln!(
                "lightning: media store: the old unencrypted media cache still \
                 holds an attachment that has not been uploaded; it is used for \
                 this session and removed at a later start"
            );
            return Prepared::Legacy;
        }
        Plan::Memory { drop_legacy } | Plan::Encrypted { drop_legacy, .. } => drop_legacy,
    };
    match legacy {
        Legacy::Abandoned => eprintln!(
            "lightning: media store: the old unencrypted media cache holds only \
             attachments untouched for 7 days; they are taken as abandoned"
        ),
        Legacy::Damaged => eprintln!(
            "lightning: media store: the old unencrypted media cache is not a \
             readable database"
        ),
        _ => {}
    }
    if drop_legacy {
        if remove_legacy(store_path) {
            eprintln!("lightning: media store: removed the old unencrypted media cache");
        } else {
            eprintln!(
                "lightning: media store: the old unencrypted media cache could \
                 not be removed; retrying at the next start"
            );
        }
    }
    let (Plan::Encrypted { wipe, .. }, Some(key)) = (decided, key) else {
        eprintln!(
            "lightning: media store: no usable key this session; media is kept \
             in memory only"
        );
        return Prepared::Memory;
    };
    if wipe {
        match fs::remove_dir_all(&dir) {
            Ok(()) => eprintln!(
                "lightning: media store: removed an encrypted media cache whose key is gone"
            ),
            Err(err) if err.kind() == io::ErrorKind::NotFound => {}
            Err(_) => {
                eprintln!(
                    "lightning: media store: an encrypted media cache for another \
                     key could not be removed; media is kept in memory only"
                );
                return Prepared::Memory;
            }
        }
    }
    if ensure_marked(&dir, key).is_err() {
        eprintln!(
            "lightning: media store: its directory could not be prepared; media \
             is kept in memory only"
        );
        return Prepared::Memory;
    }
    Prepared::Encrypted
}

fn build_error(err: impl std::fmt::Display) -> String {
    // The prefix build_client always used for a store that would not open.
    crate::format_matrix_error("failed to build Matrix Rust SDK client", err)
}

pub(crate) enum OpenedMedia {
    Sqlite(SqliteMediaStore),
    Memory,
}

/// Open the media store for the account at `store_path`, with the key its
/// handle set. `plain` is the config the other stores use, for the one case
/// that opens the old plaintext store.
pub(crate) async fn open_media(
    store_path: &Path,
    plain: &SqliteStoreConfig,
) -> Result<(OpenedMedia, Active), String> {
    let (key, admits_encrypted) = key_for(store_path);
    let prepared = {
        let path = store_path.to_path_buf();
        let key = key.clone();
        tokio::task::spawn_blocking(move || prepare(&path, key.as_ref().map(|key| &key.0)))
            .await
            .unwrap_or(Prepared::Memory)
    };
    let opened = match (prepared, key) {
        (Prepared::Legacy, _) => {
            // Exactly what `sqlite_store(path, None)` opened; a failure fails
            // the build as it always did.
            let store = SqliteMediaStore::open_with_config(plain).await.map_err(build_error)?;
            (OpenedMedia::Sqlite(store), Active::Legacy)
        }
        (Prepared::Encrypted, Some(key)) => {
            let dir = store_path.join(DIR_NAME);
            let config = SqliteStoreConfig::new(&dir).key(Some(&key.0));
            match SqliteMediaStore::open_with_config(&config).await {
                Ok(store) => {
                    restrict_tree(&dir);
                    (OpenedMedia::Sqlite(store), Active::Encrypted { admits_encrypted })
                }
                Err(_) => {
                    // Left in place: a key that matches its record and still
                    // does not open may be a passing I/O failure.
                    eprintln!(
                        "lightning: media store: the encrypted media cache could \
                         not be opened; media is kept in memory only this session"
                    );
                    (OpenedMedia::Memory, Active::Memory)
                }
            }
        }
        _ => (OpenedMedia::Memory, Active::Memory),
    };
    Ok(opened)
}

/// The stores for the account at `store_path`: the state, event-cache and
/// crypto stores exactly as `ClientBuilder::sqlite_store(path, None)` opened
/// them (matrix-sdk 0.18 client/builder/mod.rs:693-721), and the media store
/// from open_media. Records what was opened for `active()`.
///
/// Each SQLite store is handed to the SDK as an `Arc` this function made, so
/// the returned probe can tell, by a `Weak` failing to upgrade, when the SDK
/// has really let go of it (storeclose.rs, GitHub #2).
pub(crate) async fn open_account_stores(
    store_path: &Path,
) -> Result<(StoreConfig, StoreProbe), String> {
    let config = SqliteStoreConfig::new(store_path);
    let state = Arc::new(
        SqliteStateStore::open_with_config(&config).await.map_err(build_error)?,
    );
    let event_cache = Arc::new(
        SqliteEventCacheStore::open_with_config(&config)
            .await
            .map_err(build_error)?,
    );
    let crypto = Arc::new(
        SqliteCryptoStore::open_with_config(&config).await.map_err(build_error)?,
    );
    let (media, active) = open_media(store_path, &config).await?;
    record_active(store_path, active);
    let mut probe = StoreProbe::default();
    probe.set_owner(owner_for(store_path));
    probe.store("state", &state);
    probe.store("event_cache", &event_cache);
    probe.store("crypto", &crypto);
    let store_config = StoreConfig::new(CrossProcessLockConfig::multi_process(STORE_LOCK_HOLDER))
        .state_store(state)
        .event_cache_store(event_cache)
        .crypto_store(crypto);
    Ok(match media {
        OpenedMedia::Sqlite(store) => {
            let store = Arc::new(store);
            probe.store("media", &store);
            (store_config.media_store(store), probe)
        }
        OpenedMedia::Memory => (store_config.media_store(MemoryMediaStore::new()), probe),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
    use matrix_sdk::ruma::events::room::MediaSource;
    use matrix_sdk::ruma::OwnedMxcUri;
    use matrix_sdk_base::media::store::{IgnoreMediaRetentionPolicy, MediaStore};

    /// A store directory under the system temp dir, removed on drop.
    struct Store(PathBuf);

    impl Store {
        fn new(tag: &str) -> Self {
            let path = std::env::temp_dir().join(format!(
                "lightning-mediastore-{tag}-{}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).expect("test store dir");
            Store(path)
        }
    }

    impl Drop for Store {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    const KEY_A: [u8; 32] = [0xA5; 32];
    const KEY_B: [u8; 32] = [0x5A; 32];
    const MARKER: &[u8] = b"LIGHTNING-PLAINTEXT-MARKER-4f1c";
    const URI: &str = "mxc://example.org/MarkerMediaId";

    fn request(uri: &str) -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(OwnedMxcUri::from(uri)),
            format: MediaFormat::File,
        }
    }

    fn bytes_of_every_file(dir: &Path) -> Vec<u8> {
        let mut all = Vec::new();
        for entry in fs::read_dir(dir).unwrap().flatten() {
            if entry.file_type().unwrap().is_file() {
                all.extend(fs::read(entry.path()).unwrap());
            }
        }
        all
    }

    fn contains(haystack: &[u8], needle: &[u8]) -> bool {
        haystack.windows(needle.len()).any(|window| window == needle)
    }

    // ---- plan ----

    // The send queue still needs what the old store holds: deleting it would
    // lose an attachment the user has already sent from the UI.
    #[test]
    fn an_unsent_attachment_keeps_the_old_store_one_more_session() {
        for have_key in [false, true] {
            for disk in [OnDisk::Nothing, OnDisk::Matches, OnDisk::Differs] {
                assert_eq!(plan(Legacy::Pending, have_key, disk), Plan::KeepLegacy);
                // A store that cannot be asked is treated the same way.
                assert_eq!(plan(Legacy::Unknown, have_key, disk), Plan::KeepLegacy);
            }
        }
    }

    // The bound on that exception: a week-old pin and a file that is not a
    // database are dropped, so the plaintext store is not used for ever.
    #[test]
    fn an_abandoned_or_damaged_old_store_is_dropped() {
        for legacy in [Legacy::Abandoned, Legacy::Damaged] {
            assert_eq!(plan(legacy, false, OnDisk::Nothing), Plan::Memory { drop_legacy: true });
            assert_eq!(
                plan(legacy, true, OnDisk::Nothing),
                Plan::Encrypted { drop_legacy: true, wipe: false }
            );
        }
    }

    // CLAUDE.md §6: an unreadable key is transient. Without a key the
    // encrypted store is left exactly as it is, whatever is on disk.
    #[test]
    fn without_a_key_the_encrypted_store_is_never_touched() {
        for disk in [
            OnDisk::Nothing,
            OnDisk::Matches,
            OnDisk::Differs,
            OnDisk::Unmarked,
            OnDisk::Unreadable,
        ] {
            assert_eq!(plan(Legacy::Absent, false, disk), Plan::Memory { drop_legacy: false });
        }
    }

    #[test]
    fn a_store_for_this_key_is_opened_and_one_for_another_is_wiped() {
        assert_eq!(
            plan(Legacy::Absent, true, OnDisk::Nothing),
            Plan::Encrypted { drop_legacy: false, wipe: false }
        );
        assert_eq!(
            plan(Legacy::Absent, true, OnDisk::Matches),
            Plan::Encrypted { drop_legacy: false, wipe: false }
        );
        assert_eq!(
            plan(Legacy::Absent, true, OnDisk::Differs),
            Plan::Encrypted { drop_legacy: false, wipe: true }
        );
        assert_eq!(
            plan(Legacy::Absent, true, OnDisk::Unmarked),
            Plan::Encrypted { drop_legacy: false, wipe: true }
        );
        // A record that cannot be read is no reason to delete anything.
        assert_eq!(plan(Legacy::Absent, true, OnDisk::Unreadable), Plan::Memory { drop_legacy: false });
    }

    #[test]
    fn an_idle_old_store_is_dropped_with_or_without_a_key() {
        assert_eq!(
            plan(Legacy::Idle, true, OnDisk::Nothing),
            Plan::Encrypted { drop_legacy: true, wipe: false }
        );
        assert_eq!(plan(Legacy::Idle, false, OnDisk::Nothing), Plan::Memory { drop_legacy: true });
        assert_eq!(plan(Legacy::Leftovers, false, OnDisk::Nothing), Plan::Memory { drop_legacy: true });
    }

    #[test]
    fn the_key_id_identifies_a_key_without_revealing_it() {
        assert_eq!(key_id(&KEY_A), key_id(&KEY_A));
        assert_ne!(key_id(&KEY_A), key_id(&KEY_B));
        assert_eq!(key_id(&KEY_A).len(), 32);
    }

    // ---- registry ----

    #[test]
    fn a_retiring_handle_cannot_drop_its_successors_key() {
        let path = PathBuf::from("/nonexistent/lightning-mediastore-registry");
        set_key(1, &path, Some(KeyBytes(KEY_A)), true);
        set_key(2, &path, Some(KeyBytes(KEY_B)), true);
        forget(1, &path);
        let (key, admits) = key_for(&path);
        assert_eq!(key.map(|key| key.0), Some(KEY_B));
        assert!(admits);
        forget(2, &path);
        assert!(key_for(&path).0.is_none());
        // Nothing opened for a path: nothing may be kept.
        assert_eq!(active(&path), Active::Memory);
        // Admission needs a key.
        set_key(3, &path, None, true);
        assert!(!key_for(&path).1);
        forget(3, &path);
    }

    // ---- the old store ----

    async fn plaintext_store_with(store: &Path, pending: bool) {
        let legacy = SqliteMediaStore::open_with_config(&SqliteStoreConfig::new(store))
            .await
            .unwrap();
        let policy = if pending {
            IgnoreMediaRetentionPolicy::Yes
        } else {
            IgnoreMediaRetentionPolicy::No
        };
        legacy.add_media_content(&request(URI), MARKER.to_vec(), policy).await.unwrap();
    }

    /// Age every pinned row of the old store to `days` ago.
    fn age_pins(store: &Path, days: u64) {
        let at = SystemTime::now() - Duration::from_secs(days * 24 * 60 * 60);
        let secs = at.duration_since(SystemTime::UNIX_EPOCH).unwrap().as_secs() as i64;
        let conn = rusqlite::Connection::open(store.join(MEDIA_DB)).unwrap();
        conn.execute("UPDATE media SET last_access = ?1 WHERE ignore_policy", [secs]).unwrap();
    }

    #[tokio::test]
    async fn the_old_store_is_read_for_unsent_attachments() {
        let store = Store::new("legacy-state");
        let now = SystemTime::now();
        assert_eq!(legacy_state(&store.0, now), Legacy::Absent);
        plaintext_store_with(&store.0, false).await;
        assert_eq!(legacy_state(&store.0, now), Legacy::Idle);
        plaintext_store_with(&store.0, true).await;
        assert_eq!(legacy_state(&store.0, now), Legacy::Pending);
        // A pin nobody has touched for longer than the bound.
        age_pins(&store.0, 6);
        assert_eq!(legacy_state(&store.0, now), Legacy::Pending);
        age_pins(&store.0, 8);
        assert_eq!(legacy_state(&store.0, now), Legacy::Abandoned);
        // Not a database at all.
        fs::write(store.0.join(MEDIA_DB), vec![b'x'; 4096]).unwrap();
        for sidecar in LEGACY_SIDECARS {
            let _ = fs::remove_file(store.0.join(sidecar));
        }
        assert_eq!(legacy_state(&store.0, now), Legacy::Damaged);
        // An empty file is an empty database: nothing pinned.
        fs::write(store.0.join(MEDIA_DB), b"").unwrap();
        assert_eq!(legacy_state(&store.0, now), Legacy::Idle);
        // Sidecars alone.
        fs::remove_file(store.0.join(MEDIA_DB)).unwrap();
        fs::write(store.0.join(LEGACY_SIDECARS[0]), b"x").unwrap();
        assert_eq!(legacy_state(&store.0, now), Legacy::Leftovers);
        assert!(remove_legacy(&store.0));
        assert_eq!(legacy_state(&store.0, now), Legacy::Absent);
        // Idempotent.
        assert!(remove_legacy(&store.0));
    }

    // MAJOR 1 of the review: an upload that never finishes must not keep the
    // plaintext store in use, and write every later attachment into it, for
    // ever.
    #[tokio::test]
    async fn an_abandoned_upload_does_not_keep_the_old_store_alive() {
        let store = Store::new("abandoned");
        plaintext_store_with(&store.0, true).await;
        age_pins(&store.0, 8);
        set_key(15, &store.0, Some(KeyBytes(KEY_A)), true);
        let plain = SqliteStoreConfig::new(&store.0);
        let (_media, active) = open_media(&store.0, &plain).await.unwrap();
        assert_eq!(active, Active::Encrypted { admits_encrypted: true });
        assert!(!store.0.join(MEDIA_DB).exists(), "the plaintext store was kept");
        assert!(!contains(&bytes_of_every_file(&store.0), MARKER));
        forget(15, &store.0);
    }

    #[tokio::test]
    async fn an_old_store_that_is_not_a_database_is_deleted() {
        let store = Store::new("damaged");
        fs::write(store.0.join(MEDIA_DB), vec![b'x'; 4096]).unwrap();
        set_key(16, &store.0, None, false);
        let plain = SqliteStoreConfig::new(&store.0);
        let (media, active) = open_media(&store.0, &plain).await.unwrap();
        // Before the bound this opened the damaged file as the media store,
        // and failed the whole client build at every start.
        assert_eq!(active, Active::Memory);
        assert!(matches!(media, OpenedMedia::Memory));
        assert!(!store.0.join(MEDIA_DB).exists());
        forget(16, &store.0);
    }

    // The migration: an old plaintext store with nothing waiting is deleted,
    // and its bytes are gone from the account's directory.
    #[tokio::test]
    async fn an_idle_old_store_is_deleted_when_the_encrypted_store_opens() {
        let store = Store::new("migrate");
        plaintext_store_with(&store.0, false).await;
        assert!(contains(&bytes_of_every_file(&store.0), MARKER));
        set_key(10, &store.0, Some(KeyBytes(KEY_A)), true);
        let plain = SqliteStoreConfig::new(&store.0);
        let (_media, active) = open_media(&store.0, &plain).await.unwrap();
        assert_eq!(active, Active::Encrypted { admits_encrypted: true });
        assert!(!store.0.join(MEDIA_DB).exists());
        assert!(!contains(&bytes_of_every_file(&store.0), MARKER));
        forget(10, &store.0);
    }

    #[tokio::test]
    async fn an_unsent_attachment_survives_the_upgrade() {
        let store = Store::new("pending");
        plaintext_store_with(&store.0, true).await;
        set_key(11, &store.0, Some(KeyBytes(KEY_A)), true);
        let plain = SqliteStoreConfig::new(&store.0);
        let (media, active) = open_media(&store.0, &plain).await.unwrap();
        assert_eq!(active, Active::Legacy);
        let OpenedMedia::Sqlite(media) = media else { panic!("the old store was not opened") };
        assert_eq!(media.get_media_content(&request(URI)).await.unwrap().as_deref(), Some(MARKER));
        assert!(!store.0.join(DIR_NAME).exists(), "the encrypted store was created early");
        forget(11, &store.0);
    }

    // ---- the encrypted store ----

    // The point of the change: what the send queue and the cache write is not
    // readable on disk, and it is still there at the next start.
    #[tokio::test]
    async fn media_is_encrypted_at_rest_and_survives_a_restart() {
        let store = Store::new("at-rest");
        let plain = SqliteStoreConfig::new(&store.0);
        set_key(12, &store.0, Some(KeyBytes(KEY_A)), true);
        {
            let (media, _) = open_media(&store.0, &plain).await.unwrap();
            let OpenedMedia::Sqlite(media) = media else { panic!("not opened on disk") };
            media
                .add_media_content(&request(URI), MARKER.to_vec(), IgnoreMediaRetentionPolicy::Yes)
                .await
                .unwrap();
        }
        let dir = store.0.join(DIR_NAME);
        let on_disk_bytes = bytes_of_every_file(&dir);
        assert!(!on_disk_bytes.is_empty());
        assert!(!contains(&on_disk_bytes, MARKER), "media reached disk in plaintext");
        assert!(!contains(&on_disk_bytes, URI.as_bytes()), "the media's URI reached disk");
        assert!(!store.0.join(MEDIA_DB).exists(), "a plaintext store was created");

        let (media, _) = open_media(&store.0, &plain).await.unwrap();
        let OpenedMedia::Sqlite(media) = media else { panic!("not reopened") };
        assert_eq!(media.get_media_content(&request(URI)).await.unwrap().as_deref(), Some(MARKER));
        forget(12, &store.0);
    }

    // A new key (the old record was provably gone) cannot read the old store,
    // so it is replaced rather than failing every start.
    #[tokio::test]
    async fn a_store_whose_key_is_gone_is_replaced() {
        let store = Store::new("rekey");
        let plain = SqliteStoreConfig::new(&store.0);
        set_key(13, &store.0, Some(KeyBytes(KEY_A)), true);
        {
            let (media, _) = open_media(&store.0, &plain).await.unwrap();
            let OpenedMedia::Sqlite(media) = media else { panic!("not opened") };
            media
                .add_media_content(&request(URI), MARKER.to_vec(), IgnoreMediaRetentionPolicy::No)
                .await
                .unwrap();
        }
        set_key(13, &store.0, Some(KeyBytes(KEY_B)), true);
        let (media, active) = open_media(&store.0, &plain).await.unwrap();
        assert_eq!(active, Active::Encrypted { admits_encrypted: true });
        let OpenedMedia::Sqlite(media) = media else { panic!("not reopened") };
        assert_eq!(media.get_media_content(&request(URI)).await.unwrap(), None);
        assert_eq!(on_disk(&store.0.join(DIR_NAME), &KEY_B), OnDisk::Matches);
        forget(13, &store.0);
    }

    // 2026-10-01, measured on a Fedora VM: a keyed store refused media from
    // about 16 MiB although the cap was 24 MiB, because the SDK measures the
    // ENCODED row against `max_file_size` and the keyed store encodes
    // ciphertext as a msgpack array of integers. Through the KEYED store, as
    // production opens it and with the production policy: a 40 MiB payload (a
    // phone video from an encrypted room; Rokas raised the cap for exactly
    // that) is kept and reads back whole, and the encoded/plain ratio is
    // measured and must leave the policy room for a payload AT the plaintext
    // cap. The plain-store tests above cannot see this: their encoding is
    // identity.
    #[tokio::test]
    async fn the_keyed_store_keeps_a_large_payload() {
        use crate::rooms::{
            media_retention_policy, MEDIA_STORE_ENCODED_MAX_BYTES, MEDIA_STORE_MAX_FILE_BYTES,
        };
        let store = Store::new("large");
        let plain = SqliteStoreConfig::new(&store.0);
        set_key(17, &store.0, Some(KeyBytes(KEY_A)), true);
        let (media, _) = open_media(&store.0, &plain).await.unwrap();
        let OpenedMedia::Sqlite(media) = media else { panic!("not opened on disk") };
        media.set_media_retention_policy(media_retention_policy()).await.unwrap();
        let size: usize = 40 * 1024 * 1024;
        let payload: Vec<u8> = (0..size).map(|i| (i % 251) as u8).collect();
        let started = std::time::Instant::now();
        media
            .add_media_content(&request(URI), payload.clone(), IgnoreMediaRetentionPolicy::No)
            .await
            .unwrap();
        let wrote = started.elapsed();
        let started = std::time::Instant::now();
        let kept = media.get_media_content(&request(URI)).await.unwrap();
        let read = started.elapsed();
        assert!(kept.is_some(), "the keyed store refused a 40 MiB payload");
        assert!(kept.as_deref() == Some(payload.as_slice()), "the payload did not read back whole");
        drop(media);
        let conn = rusqlite::Connection::open(store.0.join(DIR_NAME).join(MEDIA_DB)).unwrap();
        let encoded: i64 = conn
            .query_row("SELECT length(data) FROM media", [], |row| row.get(0))
            .unwrap();
        let ratio = encoded as f64 / size as f64;
        eprintln!(
            "keyed media store: {size} plaintext bytes encode to {encoded} ({ratio:.4}x); \
             write {wrote:?}, read {read:?}"
        );
        assert!((1.45..1.55).contains(&ratio), "encoded/plain ratio {ratio}");
        // A payload AT the plaintext cap, at the measured ratio, still fits,
        // with most of the slack unused.
        let at_cap = (MEDIA_STORE_MAX_FILE_BYTES as f64 * ratio) as u64;
        assert!(
            at_cap + 512 * 1024 <= MEDIA_STORE_ENCODED_MAX_BYTES,
            "{at_cap} encoded bytes at the cap against a policy of {MEDIA_STORE_ENCODED_MAX_BYTES}",
        );
        forget(17, &store.0);
    }

    // A micro-benchmark, not a gate: how long the keyed store takes to write and
    // read a payload at the sizes media actually has, so a slow debug build is
    // not mistaken for a slow product. Run with
    //   cargo test --release --manifest-path rust/Cargo.toml \
    //     the_keyed_store_speed -- --ignored --nocapture
    // It asserts only that the payload reads back whole.
    #[tokio::test]
    #[ignore = "benchmark: run with --release --ignored --nocapture"]
    async fn the_keyed_store_speed_at_40_and_100_mib() {
        use crate::rooms::media_retention_policy;
        for (n, mib) in [(40usize, 40usize), (100, 100)] {
            let store = Store::new(&format!("speed-{n}"));
            let plain = SqliteStoreConfig::new(&store.0);
            set_key(30 + n, &store.0, Some(KeyBytes(KEY_A)), true);
            let (media, _) = open_media(&store.0, &plain).await.unwrap();
            let OpenedMedia::Sqlite(media) = media else { panic!("not opened on disk") };
            media.set_media_retention_policy(media_retention_policy()).await.unwrap();
            let size = mib * 1024 * 1024;
            let payload: Vec<u8> = (0..size).map(|i| (i % 251) as u8).collect();
            let started = std::time::Instant::now();
            media
                .add_media_content(&request(URI), payload.clone(), IgnoreMediaRetentionPolicy::No)
                .await
                .unwrap();
            let wrote = started.elapsed();
            let started = std::time::Instant::now();
            let kept = media.get_media_content(&request(URI)).await.unwrap();
            let read = started.elapsed();
            assert!(kept.as_deref() == Some(payload.as_slice()), "{mib} MiB did not read back whole");
            eprintln!(
                "keyed media store {mib} MiB: write {wrote:?} ({:.1} MiB/s), read {read:?} ({:.1} MiB/s)",
                mib as f64 / wrote.as_secs_f64(),
                mib as f64 / read.as_secs_f64(),
            );
            drop(media);
            forget(30 + n, &store.0);
        }
    }

    // Keyring locked: memory for the session, and the encrypted store, its
    // record and the old store's decision are all left for the next start.
    #[tokio::test]
    async fn without_a_key_nothing_is_written_and_nothing_is_deleted() {
        let store = Store::new("no-key");
        let plain = SqliteStoreConfig::new(&store.0);
        set_key(14, &store.0, Some(KeyBytes(KEY_A)), false);
        {
            let (media, active) = open_media(&store.0, &plain).await.unwrap();
            // An insecure keyring still encrypts, but admits no encrypted rooms.
            assert_eq!(active, Active::Encrypted { admits_encrypted: false });
            let OpenedMedia::Sqlite(media) = media else { panic!("not opened") };
            media
                .add_media_content(&request(URI), MARKER.to_vec(), IgnoreMediaRetentionPolicy::No)
                .await
                .unwrap();
        }
        let dir = store.0.join(DIR_NAME);
        set_key(14, &store.0, None, false);
        let (media, active) = open_media(&store.0, &plain).await.unwrap();
        assert_eq!(active, Active::Memory);
        assert!(matches!(media, OpenedMedia::Memory));
        assert!(!store.0.join(MEDIA_DB).exists(), "fell back to a plaintext file");
        assert_eq!(on_disk(&dir, &KEY_A), OnDisk::Matches, "the key record was touched");
        assert!(dir.join(MEDIA_DB).exists(), "the encrypted store was deleted");
        forget(14, &store.0);
    }
}
