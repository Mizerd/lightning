//! Kept copies of large media from UNENCRYPTED rooms, as plain files.
//!
//! The SDK media store refuses anything over
//! `rooms::MEDIA_STORE_MAX_FILE_BYTES`, because one big blob INSERT stalls
//! every other fetch on its single write connection. Without this, a video
//! over that size was downloaded again in every session.
//!
//! What may come here is decided by `rooms::media_persistence`: a plain
//! source from a room known not to be encrypted. Nothing from an encrypted
//! room does, not even a file sent there unencrypted: these files are not
//! encrypted at rest, and the bytes of an encrypted source are the decrypted
//! ones (CLAUDE.md §6).
//!
//! The directory lives inside the account's store directory, so it is
//! deleted with the account. Files are named by a hash of the mxc URI, so a
//! listing does not name the media the user opened. Bounded by total size and
//! by age; eviction is oldest first by modification time, which `read`
//! refreshes.

use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, SystemTime};

use sha2::{Digest, Sha256};

/// Directory name inside the account's store directory.
pub(crate) const DIR_NAME: &str = "lightning-media-files";
/// Total size the directory is trimmed to after each write.
pub(crate) const MAX_TOTAL_BYTES: u64 = 1024 * 1024 * 1024;
/// A file not read or written for this long is removed at the next trim.
/// Same as the SDK media store's default expiry.
pub(crate) const MAX_AGE: Duration = Duration::from_secs(60 * 24 * 60 * 60);

const SUFFIX: &str = ".media";
const PARTIAL_SUFFIX: &str = ".partial";
/// A partial file younger than this may belong to a write in progress.
const PARTIAL_GRACE: Duration = Duration::from_secs(10 * 60);

static WRITE_SERIAL: AtomicU64 = AtomicU64::new(0);

pub(crate) fn dir_in(store: &Path) -> PathBuf {
    store.join(DIR_NAME)
}

/// File name for an mxc URI: hex SHA-256 plus a suffix, never the URI.
pub(crate) fn entry_name(uri: &str) -> String {
    let digest = Sha256::digest(uri.as_bytes());
    let mut name = String::with_capacity(digest.len() * 2 + SUFFIX.len());
    for byte in digest.iter() {
        name.push_str(&format!("{byte:02x}"));
    }
    name.push_str(SUFFIX);
    name
}

/// The kept bytes for `uri`, if present and no larger than `max_bytes`.
/// Refreshes the file's age so a video watched again is kept.
pub(crate) fn read(dir: &Path, uri: &str, max_bytes: u64) -> Option<Vec<u8>> {
    let path = dir.join(entry_name(uri));
    let meta = fs::metadata(&path).ok()?;
    if !meta.is_file() || meta.len() == 0 || meta.len() > max_bytes {
        return None;
    }
    let bytes = fs::read(&path).ok()?;
    if let Ok(file) = fs::OpenOptions::new().write(true).open(&path) {
        let _ = file.set_modified(SystemTime::now());
    }
    Some(bytes)
}

/// Keep `bytes` for `uri`: written to a private partial file, then renamed,
/// so a reader never sees a torn file.
///
/// The directory is created only when its parent exists. `create_dir_all`
/// would recreate an account store directory that sign-out has just deleted.
pub(crate) fn write(dir: &Path, uri: &str, bytes: &[u8]) -> io::Result<()> {
    match fs::create_dir(dir) {
        Ok(()) => {}
        Err(err) if err.kind() == io::ErrorKind::AlreadyExists => {}
        Err(err) => return Err(err),
    }
    restrict(dir, 0o700);
    let name = entry_name(uri);
    let final_path = dir.join(&name);
    // Unique per write: two fetches of one URI must not share a partial file.
    let serial = WRITE_SERIAL.fetch_add(1, Ordering::Relaxed);
    let partial = dir.join(format!(
        "{name}.{}-{serial}{PARTIAL_SUFFIX}",
        std::process::id()
    ));
    let result = (|| {
        let mut options = fs::OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options.open(&partial)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        fs::rename(&partial, &final_path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&partial);
    }
    result
}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TrimReport {
    pub removed: u64,
    pub kept_bytes: u64,
}

/// Remove expired files, then the oldest until the total fits `max_total`.
/// Also removes partial files a crash left behind. Ignores anything this
/// module did not name.
pub(crate) fn trim(
    dir: &Path,
    max_total: u64,
    max_age: Duration,
    now: SystemTime,
) -> io::Result<TrimReport> {
    let mut report = TrimReport::default();
    let listing = match fs::read_dir(dir) {
        Ok(listing) => listing,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(report),
        Err(err) => return Err(err),
    };
    let mut kept: Vec<(SystemTime, u64, PathBuf)> = Vec::new();
    for entry in listing.flatten() {
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() {
            continue;
        }
        let file_name = entry.file_name();
        let name = file_name.to_string_lossy();
        let modified = meta.modified().unwrap_or(now);
        let age = now.duration_since(modified).unwrap_or_default();
        let path = entry.path();
        if name.ends_with(PARTIAL_SUFFIX) {
            if age > PARTIAL_GRACE && fs::remove_file(&path).is_ok() {
                report.removed += 1;
            }
            continue;
        }
        if !name.ends_with(SUFFIX) {
            continue;
        }
        if age >= max_age {
            if fs::remove_file(&path).is_ok() {
                report.removed += 1;
            }
            continue;
        }
        kept.push((modified, meta.len(), path));
    }
    let mut total: u64 = kept.iter().map(|(_, len, _)| *len).sum();
    // Oldest first.
    kept.sort_by(|a, b| a.0.cmp(&b.0));
    for (_, len, path) in kept {
        if total <= max_total {
            break;
        }
        if fs::remove_file(&path).is_ok() {
            total -= len;
            report.removed += 1;
        }
    }
    report.kept_bytes = total;
    Ok(report)
}

/// Remove every file this module wrote. Returns (files, bytes) removed;
/// (0, 0) when nothing was kept, which callers must not report as a removal.
pub(crate) fn clear(dir: &Path) -> io::Result<(u64, u64)> {
    let listing = match fs::read_dir(dir) {
        Ok(listing) => listing,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok((0, 0)),
        Err(err) => return Err(err),
    };
    let mut files = 0u64;
    let mut bytes = 0u64;
    for entry in listing.flatten() {
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() {
            continue;
        }
        let file_name = entry.file_name();
        let name = file_name.to_string_lossy();
        if !(name.ends_with(SUFFIX) || name.ends_with(PARTIAL_SUFFIX)) {
            continue;
        }
        if fs::remove_file(entry.path()).is_ok() {
            files += 1;
            bytes += meta.len();
        }
    }
    Ok((files, bytes))
}

#[cfg(unix)]
fn restrict(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = fs::set_permissions(path, fs::Permissions::from_mode(mode));
}

#[cfg(not(unix))]
fn restrict(_path: &Path, _mode: u32) {}

#[cfg(test)]
mod tests {
    use super::*;

    /// A store directory under the system temp dir, removed on drop.
    struct Store(PathBuf);

    impl Store {
        fn new(tag: &str) -> Self {
            let path = std::env::temp_dir().join(format!(
                "lightning-mediafiles-{tag}-{}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).expect("test store dir");
            Store(path)
        }
        fn dir(&self) -> PathBuf {
            dir_in(&self.0)
        }
    }

    impl Drop for Store {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn set_age(path: &Path, age: Duration) {
        let file = fs::OpenOptions::new().write(true).open(path).unwrap();
        file.set_modified(SystemTime::now() - age).unwrap();
    }

    const URI: &str = "mxc://example.org/AbCdEfGhIjKlMnOp";

    #[test]
    fn a_kept_file_reads_back_the_same_bytes() {
        let store = Store::new("roundtrip");
        let payload = vec![7u8; 4096];
        write(&store.dir(), URI, &payload).unwrap();
        assert_eq!(read(&store.dir(), URI, u64::MAX), Some(payload));
        assert_eq!(read(&store.dir(), "mxc://example.org/other", u64::MAX), None);
    }

    #[test]
    fn a_file_over_the_callers_cap_is_not_served() {
        let store = Store::new("cap");
        write(&store.dir(), URI, &[1u8; 100]).unwrap();
        assert_eq!(read(&store.dir(), URI, 99), None);
        assert!(read(&store.dir(), URI, 100).is_some());
    }

    #[test]
    fn the_listing_does_not_name_the_media() {
        let store = Store::new("names");
        write(&store.dir(), URI, b"x").unwrap();
        let names: Vec<String> = fs::read_dir(store.dir())
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, vec![entry_name(URI)]);
        assert!(!names[0].contains("AbCdEfGhIjKlMnOp"));
        assert!(!names[0].contains("example.org"));
        // Stable, so a later session finds the same file.
        assert_eq!(entry_name(URI), entry_name(URI));
        assert_ne!(entry_name(URI), entry_name("mxc://example.org/other"));
    }

    #[cfg(unix)]
    #[test]
    fn kept_files_are_private() {
        use std::os::unix::fs::PermissionsExt;
        let store = Store::new("perms");
        write(&store.dir(), URI, b"x").unwrap();
        let dir_mode = fs::metadata(store.dir()).unwrap().permissions().mode();
        let file_mode = fs::metadata(store.dir().join(entry_name(URI)))
            .unwrap()
            .permissions()
            .mode();
        assert_eq!(dir_mode & 0o777, 0o700);
        assert_eq!(file_mode & 0o777, 0o600);
    }

    #[test]
    fn a_write_after_sign_out_does_not_recreate_the_store() {
        let store = Store::new("signout");
        let account = store.0.join("account");
        fs::create_dir_all(&account).unwrap();
        let dir = dir_in(&account);
        fs::remove_dir_all(&account).unwrap();
        assert!(write(&dir, URI, b"late").is_err());
        assert!(!account.exists(), "a late write recreated a deleted store");
    }

    #[test]
    fn trim_removes_the_oldest_until_the_total_fits() {
        let store = Store::new("trim-size");
        let dir = store.dir();
        for (i, age_days) in [(0u8, 3u64), (1, 2), (2, 1)] {
            let uri = format!("mxc://example.org/{i}");
            write(&dir, &uri, &[i; 1000]).unwrap();
            set_age(&dir.join(entry_name(&uri)), Duration::from_secs(age_days * 86_400));
        }
        let report = trim(&dir, 2000, MAX_AGE, SystemTime::now()).unwrap();
        assert_eq!(report, TrimReport { removed: 1, kept_bytes: 2000 });
        assert!(read(&dir, "mxc://example.org/0", u64::MAX).is_none(), "oldest survived");
        assert!(read(&dir, "mxc://example.org/1", u64::MAX).is_some());
        assert!(read(&dir, "mxc://example.org/2", u64::MAX).is_some());
    }

    #[test]
    fn reading_a_file_makes_it_the_newest() {
        let store = Store::new("trim-touch");
        let dir = store.dir();
        write(&dir, "mxc://example.org/old", &[0; 1000]).unwrap();
        write(&dir, "mxc://example.org/new", &[1; 1000]).unwrap();
        set_age(&dir.join(entry_name("mxc://example.org/old")), Duration::from_secs(7200));
        set_age(&dir.join(entry_name("mxc://example.org/new")), Duration::from_secs(3600));
        // Watching the old one again must protect it from the next trim.
        assert!(read(&dir, "mxc://example.org/old", u64::MAX).is_some());
        trim(&dir, 1000, MAX_AGE, SystemTime::now()).unwrap();
        assert!(read(&dir, "mxc://example.org/old", u64::MAX).is_some());
        assert!(read(&dir, "mxc://example.org/new", u64::MAX).is_none());
    }

    #[test]
    fn trim_removes_expired_files_and_stale_partials_only() {
        let store = Store::new("trim-age");
        let dir = store.dir();
        write(&dir, "mxc://example.org/expired", b"a").unwrap();
        write(&dir, "mxc://example.org/fresh", b"b").unwrap();
        set_age(&dir.join(entry_name("mxc://example.org/expired")), MAX_AGE + Duration::from_secs(1));
        let stale = dir.join(format!("{}.1-1{PARTIAL_SUFFIX}", entry_name("mxc://x/y")));
        let live = dir.join(format!("{}.1-2{PARTIAL_SUFFIX}", entry_name("mxc://x/z")));
        fs::write(&stale, b"torn").unwrap();
        fs::write(&live, b"writing").unwrap();
        set_age(&stale, PARTIAL_GRACE + Duration::from_secs(1));
        let foreign = dir.join("not-ours.txt");
        fs::write(&foreign, b"keep me").unwrap();
        set_age(&foreign, MAX_AGE * 2);

        let report = trim(&dir, MAX_TOTAL_BYTES, MAX_AGE, SystemTime::now()).unwrap();
        assert_eq!(report.removed, 2);
        assert!(!stale.exists());
        assert!(live.exists(), "a partial file a write may own was removed");
        assert!(foreign.exists(), "trim removed a file it did not write");
        assert!(read(&dir, "mxc://example.org/fresh", u64::MAX).is_some());
        assert!(read(&dir, "mxc://example.org/expired", u64::MAX).is_none());
    }

    #[test]
    fn clear_counts_what_it_removed_and_nothing_else() {
        let store = Store::new("clear");
        let dir = store.dir();
        assert_eq!(clear(&dir).unwrap(), (0, 0), "an absent directory reported a removal");
        write(&dir, "mxc://example.org/a", &[0; 10]).unwrap();
        write(&dir, "mxc://example.org/b", &[0; 20]).unwrap();
        let foreign = dir.join("not-ours.txt");
        fs::write(&foreign, b"keep me").unwrap();
        assert_eq!(clear(&dir).unwrap(), (2, 30));
        assert!(foreign.exists());
        assert_eq!(clear(&dir).unwrap(), (0, 0));
    }
}
