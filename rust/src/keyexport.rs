//! Exporting this session's room keys to a passphrase-protected file: the
//! counterpart of `mx_rust_import_room_keys`.
//!
//! Only matrix-sdk's own export is used: `Encryption::export_room_keys`, which
//! reads the inbound group sessions through the SDK's store API and writes the
//! standard encrypted Megolm key-export format (`-----BEGIN MEGOLM SESSION
//! DATA-----`, PBKDF2-HMAC-SHA-512 at 500,000 rounds, AES-256-CTR plus
//! HMAC-SHA-256), the file Element, Sable and every other client import.
//! Nothing here opens the crypto store, formats key material or encrypts
//! anything itself (CLAUDE.md §6).
//!
//! Writing: the SDK writes to a sibling temporary file this module created
//! first (mode 0600 on Unix, `create_new`, so it never follows or reuses an
//! existing path), and only a successful export is renamed over the chosen
//! path. A failed export therefore never leaves a truncated file where the
//! user asked for one, and never destroys a file being replaced. Where a
//! sibling cannot be created (a read-only directory with a writable file,
//! some sandbox portals) the SDK writes the chosen path directly: a path that
//! did not exist is pre-created the same private way (and removed again on
//! failure), an existing one keeps its mode, and a failure there is reported
//! as possibly leaving a partial file.
//!
//! Cancellation (§18 review, 2026-10-13): matrix-sdk does its `File::create`
//! and `write_all` inside `spawn_blocking`, which no abort can stop. So the
//! export runs in its own task that the room action only AWAITS: sign-out
//! aborting the room action detaches that task and never cancels it, and the
//! task always ends after the SDK has returned, in a rename or a removal.
//! Nothing deletes a file the SDK may still be writing (deleting it early let
//! the blocking write re-create it at the umask). If the session ended in the
//! meantime the finished file is removed rather than renamed into place, as
//! nobody is left to be told it exists.
//!
//! Secrets: the passphrase arrives as an owned String held in `Secret`, which
//! scrubs on drop on every path (refusal, error, sign-out, abort); the SDK
//! keeps its own copy in `Zeroizing`. It is never logged, enqueued or
//! returned. Events carry counts and a category only: never the path, the
//! passphrase, a room or session id, or anything read from the file.
//!
//! Events:
//!   room_key_export_progress { op_id, lifecycle, collected }
//!   room_key_export_result   { op_id, lifecycle, ok, exported, category }
//! Categories ("" when ok): empty_passphrase, invalid_path, already_running,
//! permission_denied, disk_full, write_failed, partial_file, no_encryption,
//! failed.

use std::ffi::{c_void, OsString};
use std::io;
use std::os::raw::c_char;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;

use serde_json::json;

use crate::password::Secret;
use crate::rooms::require_client;
use crate::{bridge, cstr_arg, enqueue, ffi_string, RustClient};

/// A progress event per this many sessions gathered. The store hands every
/// session over at once, so this is about keeping the event queue small on
/// an account with hundreds of thousands of keys, not about smoothness.
const PROGRESS_EVERY: usize = 500;

/// One export at a time per process. Exports are rare, user-initiated and
/// hold every session in memory while they run; a second one queued behind a
/// slow disk would only double that.
static EXPORT_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Holds `EXPORT_ACTIVE` for the lifetime of one export, released on every
/// exit path including a task aborted at sign-out.
struct ActiveGuard;

impl ActiveGuard {
    fn acquire() -> Option<Self> {
        EXPORT_ACTIVE
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .ok()
            .map(|_| ActiveGuard)
    }
}

impl Drop for ActiveGuard {
    fn drop(&mut self) {
        EXPORT_ACTIVE.store(false, Ordering::SeqCst);
    }
}

/// Where the SDK writes. Cleaned up explicitly once the SDK has returned,
/// never on drop: a drop can happen while the SDK's blocking write still runs.
enum WriteTarget {
    /// A private sibling, renamed over the target on success.
    Staging(PathBuf),
    /// The target itself. `created` when this export created it (and may
    /// therefore remove it again).
    Direct { created: bool },
}

/// `.<name>.partial-<pid>-<nanos>` beside the target. Hidden, and never the
/// target's own name.
fn staging_path(target: &Path) -> Option<PathBuf> {
    let parent = target.parent()?;
    let name = target.file_name()?;
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let mut leaf = OsString::from(".");
    leaf.push(name);
    leaf.push(format!(".partial-{}-{}", std::process::id(), nanos));
    Some(parent.join(leaf))
}

/// Create the staging file empty and private. `create_new` refuses an
/// existing path (including a planted symlink), so the SDK's `File::create`
/// later truncates a file this process created and owns.
fn create_private(path: &Path) -> io::Result<()> {
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    options.open(path).map(drop)
}

/// Flush a file to disk. Opened for writing: Windows' FlushFileBuffers
/// refuses a read-only handle.
fn sync_file(path: &Path) {
    if let Ok(file) = std::fs::OpenOptions::new().write(true).open(path) {
        let _ = file.sync_all();
    }
}

/// Make a rename durable (Unix; a directory cannot be opened this way on
/// Windows, where the rename is journaled by NTFS).
fn sync_parent(path: &Path) {
    #[cfg(unix)]
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        if let Ok(dir) = std::fs::File::open(parent) {
            let _ = dir.sync_all();
        }
    }
    #[cfg(not(unix))]
    let _ = path;
}

fn io_category(err: &io::Error) -> &'static str {
    use io::ErrorKind as K;
    match err.kind() {
        K::PermissionDenied | K::ReadOnlyFilesystem => "permission_denied",
        K::StorageFull | K::QuotaExceeded | K::FileTooLarge => "disk_full",
        K::NotFound | K::IsADirectory | K::NotADirectory | K::InvalidFilename => "invalid_path",
        _ => "write_failed",
    }
}

fn sdk_category(err: &matrix_sdk::Error) -> &'static str {
    match err {
        matrix_sdk::Error::Io(io) => io_category(io),
        matrix_sdk::Error::NoOlmMachine => "no_encryption",
        _ => "failed",
    }
}

/// Refusals that need no I/O beyond a metadata read: an empty passphrase, an
/// empty path, a path naming a directory, or a parent that does not exist.
fn precheck(target: &Path, passphrase: &str) -> Result<(), &'static str> {
    if passphrase.is_empty() {
        return Err("empty_passphrase");
    }
    if target.as_os_str().is_empty() || target.file_name().is_none() {
        return Err("invalid_path");
    }
    if std::fs::metadata(target)
        .map(|m| m.is_dir())
        .unwrap_or(false)
    {
        return Err("invalid_path");
    }
    match target.parent() {
        Some(parent) if parent.as_os_str().is_empty() || parent.is_dir() => Ok(()),
        _ => Err("invalid_path"),
    }
}

/// Export every room key this session holds to `target`, encrypted with
/// `passphrase`, through `Encryption::export_room_keys`. `on_collected` is
/// called with the running count while the SDK gathers sessions; `keep` is
/// asked once the SDK has returned (false: the session ended, so remove the
/// file instead of putting it in place). Returns the number of keys in the
/// file, or a category. Must run to completion (see the module docs), which
/// is why `spawn_export` gives it a task of its own.
pub(crate) async fn export_to(
    client: &matrix_sdk::Client,
    target: &Path,
    passphrase: &str,
    on_collected: impl Fn(usize) + Send + Sync,
    keep: impl Fn() -> bool,
) -> Result<usize, &'static str> {
    export_with(client, target, passphrase, on_collected, keep, true).await
}

async fn export_with(
    client: &matrix_sdk::Client,
    target: &Path,
    passphrase: &str,
    on_collected: impl Fn(usize) + Send + Sync,
    keep: impl Fn() -> bool,
    allow_staging: bool,
) -> Result<usize, &'static str> {
    precheck(target, passphrase)?;

    let staging = staging_path(target)
        .filter(|_| allow_staging)
        .filter(|path| create_private(path).is_ok());
    let (write_path, how) = match staging {
        Some(path) => (path.clone(), WriteTarget::Staging(path)),
        None => {
            // Pre-create a missing target privately: the SDK's File::create
            // then truncates it and keeps the mode. An existing one keeps its
            // own mode.
            let created = create_private(target).is_ok();
            (target.to_path_buf(), WriteTarget::Direct { created })
        }
    };

    let collected = AtomicUsize::new(0);
    let result = client
        .encryption()
        .export_room_keys(write_path, passphrase, |_session| {
            let n = collected.fetch_add(1, Ordering::Relaxed) + 1;
            if n % PROGRESS_EVERY == 0 {
                on_collected(n);
            }
            // Every session, as Element exports them.
            true
        })
        .await;
    let exported = collected.load(Ordering::Relaxed);

    // The SDK has returned, so its blocking write is over: only now is any
    // file removed or moved.
    let wanted = keep();
    match (result, how) {
        (Ok(()), WriteTarget::Staging(path)) => {
            if !wanted {
                let _ = std::fs::remove_file(&path);
                return Err("cancelled");
            }
            // Durable before it replaces anything.
            sync_file(&path);
            if let Err(err) = std::fs::rename(&path, target) {
                let _ = std::fs::remove_file(&path);
                return Err(io_category(&err));
            }
            sync_parent(target);
            Ok(exported)
        }
        (Ok(()), WriteTarget::Direct { created }) => {
            if !wanted {
                // Only a file this export created; an overwritten one cannot
                // be given back.
                if created {
                    let _ = std::fs::remove_file(target);
                }
                return Err("cancelled");
            }
            sync_file(target);
            Ok(exported)
        }
        // The target is untouched.
        (Err(err), WriteTarget::Staging(path)) => {
            let _ = std::fs::remove_file(&path);
            Err(sdk_category(&err))
        }
        // A target this export created holds nothing worth keeping.
        (Err(err), WriteTarget::Direct { created: true }) => {
            let _ = std::fs::remove_file(target);
            Err(sdk_category(&err))
        }
        // Written over an existing file: an error while writing may have
        // left it truncated. A refused open (permission, bad path) wrote
        // nothing.
        (Err(err @ matrix_sdk::Error::Io(_)), WriteTarget::Direct { created: false }) => {
            let category = sdk_category(&err);
            Err(if matches!(category, "write_failed" | "disk_full") {
                "partial_file"
            } else {
                category
            })
        }
        (Err(err), WriteTarget::Direct { created: false }) => Err(sdk_category(&err)),
    }
}

/// Run one export in a task of its own on `runtime` and hand its outcome to
/// `on_done`. `hold` (the passphrase, the single-flight guard) lives until the
/// export has finished. The caller may await the returned handle or drop it:
/// dropping or aborting the awaiting task detaches this one, it never cancels
/// it, so the cleanup in `export_with` always runs after the SDK's write.
#[allow(clippy::too_many_arguments)]
fn spawn_export<H, P, K, D>(
    runtime: &tokio::runtime::Handle,
    client: matrix_sdk::Client,
    target: PathBuf,
    passphrase: Secret,
    hold: H,
    on_collected: P,
    keep: K,
    on_done: D,
) -> tokio::task::JoinHandle<()>
where
    H: Send + 'static,
    P: Fn(usize) + Send + Sync + 'static,
    K: Fn() -> bool + Send + Sync + 'static,
    D: FnOnce(Result<usize, &'static str>) + Send + 'static,
{
    runtime.spawn(async move {
        let outcome = export_to(&client, &target, &passphrase.0, on_collected, keep).await;
        drop(passphrase);
        drop(client);
        on_done(outcome);
        drop(hold);
    })
}

fn progress_event(op_id: u64, lifecycle: u64, collected: usize) -> serde_json::Value {
    json!({
        "type": "room_key_export_progress",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "collected": collected,
    })
}

fn result_event(
    op_id: u64,
    lifecycle: u64,
    outcome: Result<usize, &'static str>,
) -> serde_json::Value {
    let (exported, category) = match outcome {
        Ok(n) => (n, ""),
        Err(category) => (0, category),
    };
    json!({
        "type": "room_key_export_result",
        "op_id": op_id,
        "lifecycle": lifecycle,
        "ok": category.is_empty(),
        "exported": exported,
        "category": category,
    })
}

/// Validate synchronously, then run the export on the room-action pool, which
/// sign-out joins (and aborts past its budget). Synchronous refusals come
/// back as the category alone.
fn start_export(
    handle: &RustClient,
    target: PathBuf,
    passphrase: Secret,
    op_id: u64,
) -> Result<(), String> {
    precheck(&target, &passphrase.0).map_err(str::to_owned)?;
    let client = require_client(handle).map_err(|_| "not_signed_in".to_owned())?;
    let guard = ActiveGuard::acquire().ok_or_else(|| "already_running".to_owned())?;
    let events = Arc::clone(&handle.events);
    let progress_events = Arc::clone(&events);
    let timelines = Arc::clone(&handle.timelines);
    let keep_timelines = Arc::clone(&timelines);
    let lifecycle = timelines.lifecycle();
    let export = spawn_export(
        handle.runtime.handle(),
        client,
        target,
        passphrase,
        guard,
        move |n| enqueue(&progress_events, progress_event(op_id, lifecycle, n)),
        move || keep_timelines.lifecycle_current(lifecycle),
        move |outcome| {
            if timelines.lifecycle_current(lifecycle) {
                enqueue(&events, result_event(op_id, lifecycle, outcome));
            }
        },
    );
    // Sign-out joins the room-action pool and aborts past its budget; that
    // abort lands here and detaches the export, which then finishes (and
    // cleans up) on its own.
    handle.spawn_room_action(async move {
        let _ = export.await;
    });
    Ok(())
}

/// Export this session's room keys to `file_path`, encrypted with
/// `passphrase`. Returns "" when started (the answer is a
/// `room_key_export_result` event), or "error: <category>" when refused:
/// empty_passphrase, invalid_path, not_signed_in, already_running.
#[no_mangle]
pub unsafe extern "C" fn mx_rust_export_room_keys(
    ptr: *mut c_void,
    file_path: *const c_char,
    passphrase: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
        // First, so it is scrubbed on every refusal below.
        let passphrase = Secret(unsafe { cstr_arg(passphrase) }?);
        let handle = unsafe { bridge(ptr)? };
        let file_path = unsafe { cstr_arg(file_path) }?;
        start_export(handle, PathBuf::from(file_path), passphrase, op_id).map(|_| String::new())
    })
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use matrix_sdk::{
        authentication::matrix::MatrixSession,
        ruma::{
            events::AnyMessageLikeEventContent, room_id, serde::Raw, OwnedDeviceId, OwnedUserId,
        },
        store::RoomLoadSettings,
        Client, SessionMeta, SessionTokens,
    };
    use matrix_sdk_base::crypto::{
        types::events::room::encrypted::EncryptedEvent, DecryptionSettings, EncryptionSettings,
        TrustRequirement,
    };

    use super::*;

    /// A scratch directory removed on drop.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Self {
            let nanos = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let dir = std::env::temp_dir().join(format!(
                "lightning-keyexport-{tag}-{}-{nanos}",
                std::process::id()
            ));
            std::fs::create_dir_all(&dir).expect("scratch dir");
            Scratch(dir)
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    /// A signed-in client on its own fresh SQLite store, as Lightning builds
    /// one. Nothing here talks to a server.
    async fn client_on_store(store: &Path, device: &str) -> Client {
        let client = Client::builder()
            .homeserver_url("http://127.0.0.1:9")
            .sqlite_store(store, None)
            .build()
            .await
            .expect("client");
        let session = MatrixSession {
            meta: SessionMeta {
                user_id: OwnedUserId::try_from("@me:example.org").expect("user id"),
                device_id: OwnedDeviceId::from(device),
            },
            tokens: SessionTokens {
                access_token: "test-token".to_owned(),
                refresh_token: None,
            },
        };
        client
            .matrix_auth()
            .restore_session(session, RoomLoadSettings::default())
            .await
            .expect("restore");
        client
    }

    /// Create a Megolm session in `room` on `client` (through the SDK's
    /// `share_room_key` with nobody to share to) and encrypt one message
    /// with it. Returns the encrypted event and its session id.
    async fn encrypted_message(
        client: &Client,
        room: &matrix_sdk::ruma::RoomId,
        body: &str,
    ) -> (Raw<EncryptedEvent>, String) {
        let machine = client.olm_machine_for_testing().await;
        let machine = machine.as_ref().expect("olm machine");
        machine
            .share_room_key(room, std::iter::empty(), EncryptionSettings::default())
            .await
            .expect("session created");
        let content: Raw<AnyMessageLikeEventContent> =
            Raw::from_json_string(json!({ "msgtype": "m.text", "body": body }).to_string())
                .expect("content");
        let encrypted = machine
            .encrypt_room_event_raw(room, "m.room.message", &content)
            .await
            .expect("encrypted");
        let session_id = encrypted
            .encryption_info
            .session_id()
            .expect("megolm session id")
            .to_owned();
        let event = json!({
            "type": "m.room.encrypted",
            "event_id": "$exported:example.org",
            "sender": "@me:example.org",
            "origin_server_ts": 1_700_000_000_000u64,
            "room_id": room.as_str(),
            "content": encrypted.content,
        });
        (
            Raw::from_json_string(event.to_string()).expect("event"),
            session_id,
        )
    }

    fn decryption_settings() -> DecryptionSettings {
        DecryptionSettings {
            sender_device_trust_requirement: TrustRequirement::Untrusted,
        }
    }

    // The round trip the feature exists for: what this module writes, the
    // SDK's import (the call mx_rust_import_room_keys makes) reads into a
    // brand-new store, and the message encrypted before the export decrypts
    // there. Fails if the export wrote nothing, wrote another format, or
    // dropped the session.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_export_imports_into_a_fresh_store_and_decrypts_the_same_messages() {
        let scratch = Scratch::new("roundtrip");
        let room = room_id!("!keys:example.org");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        let (event, session_id) = encrypted_message(&source, room, "kept secret").await;

        let file = scratch.0.join("keys.txt");
        let seen = Arc::new(Mutex::new(Vec::new()));
        let seen_in = Arc::clone(&seen);
        let exported = export_to(
            &source,
            &file,
            "correct horse battery staple",
            move |n| seen_in.lock().unwrap().push(n),
            || true,
        )
        .await
        .expect("export");
        assert_eq!(exported, 1, "the one session this store holds");

        let text = std::fs::read_to_string(&file).expect("export written");
        assert!(text.starts_with("-----BEGIN MEGOLM SESSION DATA-----"));
        assert!(text
            .trim_end()
            .ends_with("-----END MEGOLM SESSION DATA-----"));
        // Encrypted, not a JSON dump: neither the session id nor the room
        // appears in the file.
        assert!(!text.contains(&session_id));
        assert!(!text.contains(room.as_str()));
        // Nothing left beside it.
        let leftovers: Vec<_> = std::fs::read_dir(&scratch.0)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|name| name.contains(".partial-"))
            .collect();
        assert!(
            leftovers.is_empty(),
            "staging file left behind: {leftovers:?}"
        );

        let target = client_on_store(&scratch.0.join("target"), "TARGET").await;
        {
            let machine = target.olm_machine_for_testing().await;
            let machine = machine.as_ref().expect("olm machine");
            assert!(
                machine
                    .decrypt_room_event(&event, room, &decryption_settings())
                    .await
                    .is_err(),
                "the fresh store must not be able to decrypt before the import"
            );
        }
        let import = target
            .encryption()
            .import_room_keys(file.clone(), "correct horse battery staple")
            .await
            .expect("import");
        assert_eq!((import.imported_count, import.total_count), (1, 1));
        let imported_sessions: Vec<String> = import
            .keys
            .get(room)
            .map(|by_sender| by_sender.values().flatten().cloned().collect())
            .unwrap_or_default();
        assert_eq!(imported_sessions, vec![session_id]);

        let machine = target.olm_machine_for_testing().await;
        let machine = machine.as_ref().expect("olm machine");
        let decrypted = machine
            .decrypt_room_event(&event, room, &decryption_settings())
            .await
            .expect("decrypts after the import");
        let plain = decrypted.event.json().get().to_owned();
        assert!(plain.contains("kept secret"), "same message, same key");
    }

    #[cfg(unix)]
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_export_file_is_private_and_a_wrong_passphrase_cannot_read_it() {
        use std::os::unix::fs::PermissionsExt;
        let scratch = Scratch::new("private");
        let room = room_id!("!keys:example.org");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        encrypted_message(&source, room, "x").await;
        let file = scratch.0.join("keys.txt");
        export_to(&source, &file, "right", |_| {}, || true)
            .await
            .expect("export");
        let mode = std::fs::metadata(&file).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "owner-only, whatever the umask");

        let target = client_on_store(&scratch.0.join("target"), "TARGET").await;
        assert!(target
            .encryption()
            .import_room_keys(file, "wrong")
            .await
            .is_err());
    }

    // Replacing an existing file: the old one survives a refused export
    // untouched, and is replaced only by a complete one.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_existing_file_is_replaced_whole_or_not_at_all() {
        let scratch = Scratch::new("replace");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        encrypted_message(&source, room_id!("!a:example.org"), "x").await;
        let file = scratch.0.join("keys.txt");
        std::fs::write(&file, "previous export").unwrap();

        assert_eq!(
            export_to(&source, &file, "", |_| {}, || true).await,
            Err("empty_passphrase")
        );
        assert_eq!(std::fs::read_to_string(&file).unwrap(), "previous export");

        export_to(&source, &file, "pass", |_| {}, || true)
            .await
            .expect("export");
        assert!(std::fs::read_to_string(&file)
            .unwrap()
            .starts_with("-----BEGIN MEGOLM SESSION DATA-----"));
    }

    #[test]
    fn refusals_need_no_session_and_name_their_reason() {
        let scratch = Scratch::new("refuse");
        let file = scratch.0.join("keys.txt");
        assert_eq!(precheck(&file, ""), Err("empty_passphrase"));
        assert_eq!(precheck(Path::new(""), "p"), Err("invalid_path"));
        assert_eq!(
            precheck(&scratch.0, "p"),
            Err("invalid_path"),
            "a directory"
        );
        assert_eq!(
            precheck(&scratch.0.join("missing").join("keys.txt"), "p"),
            Err("invalid_path"),
            "a parent that does not exist"
        );
        assert_eq!(precheck(&file, "p"), Ok(()));
    }

    fn partial_files(dir: &Path) -> Vec<String> {
        std::fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|name| name.contains(".partial-"))
            .collect()
    }

    // §18 review: sign-out aborts the room action that awaits the export.
    // That abort must detach the export task, not cancel it, so the SDK's
    // blocking write is followed by the rename (or a removal) and never by
    // a deletion it then re-creates at the umask. Abort the awaiting task at
    // once and the export still lands whole, private, with nothing left
    // beside it. (The old design ran the export inside the aborted task; its
    // failure was timing-dependent, so this pins the structure.)
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn aborting_the_awaiting_task_never_cancels_the_export_or_its_cleanup() {
        let scratch = Scratch::new("abort");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        encrypted_message(&source, room_id!("!a:example.org"), "x").await;
        let file = scratch.0.join("keys.txt");

        let (done_tx, done_rx) = tokio::sync::oneshot::channel();
        let export = spawn_export(
            &tokio::runtime::Handle::current(),
            source.clone(),
            file.clone(),
            Secret("pass".to_owned()),
            (),
            |_| {},
            || true,
            move |outcome| {
                let _ = done_tx.send(outcome);
            },
        );
        let awaiting = tokio::spawn(async move {
            let _ = export.await;
        });
        awaiting.abort();
        let outcome = tokio::time::timeout(std::time::Duration::from_secs(60), done_rx)
            .await
            .expect("the export finishes although its awaiting task was aborted")
            .expect("outcome delivered");
        assert_eq!(outcome, Ok(1));
        assert!(std::fs::read_to_string(&file)
            .unwrap()
            .starts_with("-----BEGIN MEGOLM SESSION DATA-----"));
        assert!(
            partial_files(&scratch.0).is_empty(),
            "{:?}",
            partial_files(&scratch.0)
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&file).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, 0o600);
        }
    }

    // The session ended while the SDK was writing: the finished file is
    // removed, not put in place, and nothing is left beside it.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_export_whose_session_ended_leaves_no_file() {
        let scratch = Scratch::new("ended");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        encrypted_message(&source, room_id!("!a:example.org"), "x").await;
        let file = scratch.0.join("keys.txt");
        assert_eq!(
            export_to(&source, &file, "pass", |_| {}, || false).await,
            Err("cancelled")
        );
        assert!(!file.exists());
        assert!(partial_files(&scratch.0).is_empty());

        // An existing file at the target is left alone.
        std::fs::write(&file, "previous export").unwrap();
        assert_eq!(
            export_to(&source, &file, "pass", |_| {}, || false).await,
            Err("cancelled")
        );
        assert_eq!(std::fs::read_to_string(&file).unwrap(), "previous export");
        assert!(partial_files(&scratch.0).is_empty());
    }

    // §18 review: without a sibling, a target the export creates is created
    // private too (the SDK's File::create keeps the mode of the file it
    // truncates; on its own it would use the umask, 0644 here).
    #[cfg(unix)]
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_direct_write_creates_the_target_private() {
        use std::os::unix::fs::PermissionsExt;
        let scratch = Scratch::new("direct");
        let source = client_on_store(&scratch.0.join("source"), "SOURCE").await;
        encrypted_message(&source, room_id!("!a:example.org"), "x").await;
        let file = scratch.0.join("keys.txt");
        let exported = export_with(&source, &file, "pass", |_| {}, || true, false)
            .await
            .expect("export");
        assert_eq!(exported, 1);
        let mode = std::fs::metadata(&file).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        assert!(std::fs::read_to_string(&file)
            .unwrap()
            .starts_with("-----BEGIN MEGOLM SESSION DATA-----"));
    }

    #[test]
    fn a_staging_file_is_hidden_beside_the_target_and_never_the_target() {
        let target = Path::new("/home/u/keys.txt");
        let staged = staging_path(target).unwrap();
        assert_eq!(staged.parent(), target.parent());
        let leaf = staged.file_name().unwrap().to_string_lossy().into_owned();
        assert!(leaf.starts_with(".keys.txt.partial-"), "{leaf}");
        assert_ne!(staged, target);
    }

    #[test]
    fn io_errors_map_to_categories_the_ui_can_explain() {
        use io::ErrorKind as K;
        assert_eq!(
            io_category(&io::Error::from(K::PermissionDenied)),
            "permission_denied"
        );
        assert_eq!(io_category(&io::Error::from(K::StorageFull)), "disk_full");
        assert_eq!(io_category(&io::Error::from(K::NotFound)), "invalid_path");
        assert_eq!(
            io_category(&io::Error::from(K::Interrupted)),
            "write_failed"
        );
    }

    // Everything an export puts on the event queue, for a passphrase and a
    // path that are easy to find: neither appears, nor any key material.
    #[test]
    fn events_carry_counts_and_a_category_only() {
        let progress = progress_event(7, 3, 1000).to_string();
        let done = result_event(7, 3, Ok(1234)).to_string();
        let failed = result_event(7, 3, Err("disk_full")).to_string();
        for event in [&progress, &done, &failed] {
            let value: serde_json::Value = serde_json::from_str(event).unwrap();
            let keys: Vec<&str> = value
                .as_object()
                .unwrap()
                .keys()
                .map(String::as_str)
                .collect();
            for key in keys {
                assert!(
                    [
                        "type",
                        "op_id",
                        "lifecycle",
                        "collected",
                        "ok",
                        "exported",
                        "category"
                    ]
                    .contains(&key),
                    "unexpected field {key} in {event}"
                );
            }
        }
        assert!(done.contains("\"exported\":1234") && done.contains("\"ok\":true"));
        assert!(failed.contains("\"category\":\"disk_full\"") && failed.contains("\"ok\":false"));
    }
}
