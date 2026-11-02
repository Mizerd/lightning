
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

const PROGRESS_EVERY: usize = 500;

static EXPORT_ACTIVE: AtomicBool = AtomicBool::new(false);

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
    Staging(PathBuf),
    Direct { created: bool },
}

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

fn sync_file(path: &Path) {
    if let Ok(file) = std::fs::OpenOptions::new().write(true).open(path) {
        let _ = file.sync_all();
    }
}

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
            true
        })
        .await;
    let exported = collected.load(Ordering::Relaxed);

    let wanted = keep();
    match (result, how) {
        (Ok(()), WriteTarget::Staging(path)) => {
            if !wanted {
                let _ = std::fs::remove_file(&path);
                return Err("cancelled");
            }
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
                if created {
                    let _ = std::fs::remove_file(target);
                }
                return Err("cancelled");
            }
            sync_file(target);
            Ok(exported)
        }
        (Err(err), WriteTarget::Staging(path)) => {
            let _ = std::fs::remove_file(&path);
            Err(sdk_category(&err))
        }
        (Err(err), WriteTarget::Direct { created: true }) => {
            let _ = std::fs::remove_file(target);
            Err(sdk_category(&err))
        }
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
    handle.spawn_room_action(async move {
        let _ = export.await;
    });
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn mx_rust_export_room_keys(
    ptr: *mut c_void,
    file_path: *const c_char,
    passphrase: *const c_char,
    op_id: u64,
) -> *mut c_char {
    ffi_string(|| {
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
        assert!(!text.contains(&session_id));
        assert!(!text.contains(room.as_str()));
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

        std::fs::write(&file, "previous export").unwrap();
        assert_eq!(
            export_to(&source, &file, "pass", |_| {}, || false).await,
            Err("cancelled")
        );
        assert_eq!(std::fs::read_to_string(&file).unwrap(), "previous export");
        assert!(partial_files(&scratch.0).is_empty());
    }

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
