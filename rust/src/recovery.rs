//! Pure decisions behind recovery, key backup and the Sessions page's trust
//! labels. Each one is a function of matrix-sdk state or of a matrix-sdk error
//! value: nothing here decides trust, and nothing here sees key material
//! (CLAUDE.md §6, §9).

use std::path::{Path, PathBuf};

use matrix_sdk::encryption::recovery::RecoveryError;
use matrix_sdk::encryption::secret_storage::{ImportError, SecretStorageError};
use matrix_sdk_base::crypto::secret_storage::DecodeError;

/// Whether a Sessions row is trusted, from matrix-sdk's own predicates.
///
/// The two flags are wrong in opposite rows, so neither serves alone:
/// * This session: `is_verified()` is a constant true. matrix-sdk-crypto marks
///   our own device `LocalTrust::Verified` when it creates it
///   (`machine/mod.rs`), so it would badge an unverified session green.
///   matrix-sdk's own `Encryption::verification_state()` asks
///   `is_cross_signed_by_owner()` for this device, and so does this.
/// * Any other session: `is_cross_signed_by_owner()` only says our
///   self-signing key signed it. When this session has not verified our own
///   identity the SDK does not trust that key, so it would badge a device the
///   SDK does not trust. `is_verified()` is the SDK's answer: locally trusted,
///   or cross-signed under an identity we verified. It also covers a device
///   verified by SAS without cross-signing, which the other flag misses.
pub(crate) fn session_row_trusted(
    is_current: bool,
    verified: bool,
    cross_signed: bool,
) -> bool {
    if is_current {
        cross_signed
    } else {
        verified
    }
}

/// Why `Recovery::recover()` failed, as a fixed category that C++ turns into
/// translated text. The SDK's own message never reaches the user: it is
/// English, and it names internals ("The MAC check for the secret storage key
/// failed") at the one moment the message has to be plain.
pub(crate) fn classify_recover_error(err: &RecoveryError) -> &'static str {
    match err {
        RecoveryError::SecretStorage(err) => classify_secret_storage_error(err),
        RecoveryError::Sdk(err) => classify_sdk_error(err),
        // `recover()` never creates a backup; only `enable()` returns this.
        RecoveryError::BackupExistsOnServer => "failed",
    }
}

fn classify_secret_storage_error(err: &SecretStorageError) -> &'static str {
    match err {
        SecretStorageError::SecretStorageKey(decode) => match decode {
            // The input decoded (or was taken as a passphrase) and does not
            // unlock this account's secret storage.
            DecodeError::Mac(_) => "recovery_key_wrong",
            // Not a recovery key at all: a mistyped character, a truncated
            // paste, or a passphrase on an account that never set one.
            DecodeError::Prefix(..)
            | DecodeError::Parity(..)
            | DecodeError::Base58(_)
            | DecodeError::Base64(_)
            | DecodeError::KeyLength(..) => "recovery_key_malformed",
            // The account's own key description is one this SDK cannot use.
            DecodeError::MacLength(..)
            | DecodeError::IvLength(..)
            | DecodeError::UnsupportedAlgorithm(_)
            | DecodeError::KdfIterationCount(_) => "secret_storage_unsupported",
        },
        SecretStorageError::MissingKeyInfo { .. } => "no_secret_storage",
        // Importing one secret failed. `maybe_enable_backups` wraps a failed
        // request as `ImportError::Sdk` (secret_store.rs:317-330,
        // backups/mod.rs:827), so only the key-shaped variants are a mismatch.
        SecretStorageError::ImportError { error, .. } => match error {
            ImportError::Sdk(err) => classify_sdk_error(err),
            ImportError::Json(_) => "failed",
            // The key opened the store, and what it holds does not match the
            // account as it is now (replaced since the key was saved).
            ImportError::Key(_)
            | ImportError::MismatchedPublicKeys
            | ImportError::Decryption(_) => "secret_storage_mismatch",
        },
        SecretStorageError::Decryption(_)
        | SecretStorageError::InconsistentBackupDecryptionKey => "secret_storage_mismatch",
        // `maybe_enable_backups` returns this for ANY failure to read the
        // backup key from secret storage, a network failure included
        // (secret_store.rs:340-342), so it says nothing about a mismatch.
        SecretStorageError::MissingOrInvalidBackupDecryptionKey => "backup_key_unreadable",
        SecretStorageError::Sdk(err) => classify_sdk_error(err),
        SecretStorageError::Json(_)
        | SecretStorageError::Storage(_)
        | SecretStorageError::Verification(_) => "failed",
    }
}

fn classify_sdk_error(err: &matrix_sdk::Error) -> &'static str {
    match err {
        // The server answered with a Matrix error: it was reached.
        matrix_sdk::Error::Http(http) if http.as_client_api_error().is_some() => {
            "server_refused"
        }
        matrix_sdk::Error::Http(_) => "network",
        _ => "failed",
    }
}

/// What a `recover()` that returned Ok actually restored. `Ok(())` alone
/// also covers a recovery that restored no message keys, which was reported
/// as "Recovery complete" (§6: never report a success that did nothing).
///
/// * `backup_usable`: `Backups::are_enabled()` afterwards, i.e. this session
///   holds the backup decryption key for the server's current backup and can
///   fetch room keys. False when the account has NO backup version on the
///   server as well as when secret storage holds no backup key: both restore
///   no history.
/// * `session_verified`: this session's own device is cross-signed by its
///   owner afterwards (`is_cross_signed_by_owner()`, what matrix-sdk's
///   `verification_state()` asks). `import_secrets()` signs this device when
///   secret storage held the self-signing key.
///
/// "ok_no_backup" is therefore "verified, but no history restored", for
/// either reason.
pub(crate) fn recover_outcome(backup_usable: bool, session_verified: bool) -> &'static str {
    match (backup_usable, session_verified) {
        (true, _) => "ok",
        (false, true) => "ok_no_backup",
        (false, false) => "nothing_restored",
    }
}

/// File in the account's store directory that exists while an action that
/// mints a recovery key is in flight. A teardown abort (account switch, quit)
/// or a crash between "secret storage exists" and "the key reached the user"
/// leaves it behind, and the next session reports it instead of saying
/// nothing. Empty: it carries no key material and no identifier.
const SETUP_MARKER: &str = "recovery-setup-pending";

/// Store directories with a minting action running in this process, so a sync
/// restart mid-action does not report its own action as interrupted. A guard
/// entry, not the file: an aborted task drops its guard, which forgets the
/// entry and leaves the file for the report.
static SETUP_IN_FLIGHT: std::sync::Mutex<std::collections::BTreeSet<PathBuf>> =
    std::sync::Mutex::new(std::collections::BTreeSet::new());

fn marker_path(store: &Path) -> Option<PathBuf> {
    // No store directory, no marker: never a file in the working directory.
    (!store.as_os_str().is_empty()).then(|| store.join(SETUP_MARKER))
}

/// Held by a key-minting action for as long as it runs. `finish()` once the
/// outcome has been reported; dropped unfinished (an abort, or a lifecycle
/// that ended first) it leaves the marker for the next session.
pub(crate) struct SetupMarker {
    store: PathBuf,
    finished: bool,
}

impl SetupMarker {
    /// Best effort: a store directory that cannot be written loses only this
    /// report, never the action.
    pub(crate) fn start(store: &Path) -> Self {
        if let Some(path) = marker_path(store) {
            let _ = std::fs::write(path, b"");
        }
        if let Ok(mut running) = SETUP_IN_FLIGHT.lock() {
            running.insert(store.to_path_buf());
        }
        Self { store: store.to_path_buf(), finished: false }
    }

    /// The outcome (success or failure) reached the user.
    pub(crate) fn finish(mut self) {
        if let Some(path) = marker_path(&self.store) {
            let _ = std::fs::remove_file(path);
        }
        self.finished = true;
    }
}

impl Drop for SetupMarker {
    fn drop(&mut self) {
        if let Ok(mut running) = SETUP_IN_FLIGHT.lock() {
            running.remove(&self.store);
        }
    }
}

/// The (bridge, lifecycle) that already sent the report for a store, so a sync
/// restart in the same session does not repeat it while C++ has not
/// acknowledged yet. Keyed on the bridge as well: lifecycle numbers restart at
/// every bridge, and an A -> B -> A switch must report again.
static SETUP_REPORTED: std::sync::Mutex<std::collections::BTreeMap<PathBuf, (u64, u64)>> =
    std::sync::Mutex::new(std::collections::BTreeMap::new());

static NEXT_BRIDGE_ID: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

/// A process-unique id for a new bridge.
pub(crate) fn next_bridge_id() -> u64 {
    NEXT_BRIDGE_ID.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

/// Whether to report an interrupted key-minting action now: a marker no action
/// in this process is holding, not yet reported in `lifecycle`. The marker is
/// NOT removed here; it stays until the user has been shown the report
/// (`acknowledge_interrupted_setup`), so a report dropped on the way (a stale
/// generation, a closing session) is made again by the next session.
pub(crate) fn interrupted_setup_to_report(store: &Path, bridge: u64, lifecycle: u64) -> bool {
    let Some(path) = marker_path(store) else {
        return false;
    };
    if SETUP_IN_FLIGHT
        .lock()
        .map(|running| running.contains(store))
        .unwrap_or(false)
    {
        return false;
    }
    if !path.exists() {
        return false;
    }
    let Ok(mut reported) = SETUP_REPORTED.lock() else {
        return true;
    };
    if reported.get(store) == Some(&(bridge, lifecycle)) {
        return false;
    }
    reported.insert(store.to_path_buf(), (bridge, lifecycle));
    true
}

/// The user has been shown the report: the marker goes.
pub(crate) fn acknowledge_interrupted_setup(store: &Path) {
    if let Some(path) = marker_path(store) {
        let _ = std::fs::remove_file(path);
    }
    if let Ok(mut reported) = SETUP_REPORTED.lock() {
        reported.remove(store);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk_base::crypto::secret_storage::SecretStorageKey;

    // Every row of the matrix. The two rows marked "old" are where the old
    // label (`cross_signed` everywhere) gave the opposite answer.
    #[test]
    fn a_sessions_row_is_trusted_by_the_sdk_predicate_that_fits_it() {
        // This session: is_verified() is constant true, so it never decides.
        assert!(session_row_trusted(true, true, true));
        assert!(!session_row_trusted(true, true, false));
        assert!(session_row_trusted(true, false, true));
        assert!(!session_row_trusted(true, false, false));
        // Another session: the SDK's trust, not the signature alone.
        assert!(session_row_trusted(false, true, true));
        assert!(
            session_row_trusted(false, true, false),
            "old: a device verified by SAS without cross-signing read Not verified"
        );
        assert!(
            !session_row_trusted(false, false, true),
            "old: a device signed by a key this session does not trust read Verified"
        );
        assert!(!session_row_trusted(false, false, false));
    }

    fn decode(input: &str, key: &SecretStorageKey) -> RecoveryError {
        let err = SecretStorageKey::from_account_data(input, key.event_content().clone())
            .err()
            .expect("the input must not open the store");
        RecoveryError::SecretStorage(SecretStorageError::SecretStorageKey(err))
    }

    // Real errors from the SDK's own key parser, so a variant moving in an SDK
    // update fails here rather than falling through to "failed".
    #[test]
    fn a_wrong_recovery_key_is_named_as_wrong() {
        let account = SecretStorageKey::new();
        let other = SecretStorageKey::new();
        assert_eq!(
            classify_recover_error(&decode(&other.to_base58(), &account)),
            "recovery_key_wrong"
        );
    }

    #[test]
    fn a_mistyped_recovery_key_is_named_as_malformed() {
        let account = SecretStorageKey::new();
        let good = account.to_base58();
        // A character outside Base58 (0 is not in the alphabet).
        let typo = good.replacen(|c: char| c.is_ascii_alphanumeric(), "0", 1);
        assert_eq!(classify_recover_error(&decode(&typo, &account)), "recovery_key_malformed");
        // Truncated paste.
        let short: String = good.chars().take(good.len() / 2).collect();
        assert_eq!(classify_recover_error(&decode(&short, &account)), "recovery_key_malformed");
        // A passphrase on an account that never set one.
        assert_eq!(
            classify_recover_error(&decode("correct horse battery staple", &account)),
            "recovery_key_malformed"
        );
    }

    #[test]
    fn an_account_without_secret_storage_says_so() {
        let err = RecoveryError::SecretStorage(SecretStorageError::MissingKeyInfo {
            key_id: None,
        });
        assert_eq!(classify_recover_error(&err), "no_secret_storage");
        let err = RecoveryError::SecretStorage(
            SecretStorageError::InconsistentBackupDecryptionKey,
        );
        assert_eq!(classify_recover_error(&err), "secret_storage_mismatch");
    }

    // Review M1: a failed request inside the backup step arrives wrapped as an
    // import error, and a failed read of the backup key as
    // MissingOrInvalidBackupDecryptionKey. Neither is "the account's keys were
    // replaced since this recovery key was saved".
    #[test]
    fn a_failure_wrapped_in_an_import_error_is_not_called_a_mismatch() {
        use matrix_sdk::ruma::events::secret::request::SecretName;
        let wrapped = |error: ImportError| {
            RecoveryError::SecretStorage(SecretStorageError::ImportError {
                name: SecretName::RecoveryKey,
                error,
            })
        };
        // A non-HTTP SDK error: neither network nor a server refusal.
        assert_eq!(
            classify_recover_error(&wrapped(ImportError::Sdk(
                matrix_sdk::Error::AuthenticationRequired
            ))),
            "failed"
        );
        let json = serde_json::from_str::<serde_json::Value>("{").unwrap_err();
        assert_eq!(classify_recover_error(&wrapped(ImportError::Json(json))), "failed");
        assert_eq!(
            classify_recover_error(&wrapped(ImportError::MismatchedPublicKeys)),
            "secret_storage_mismatch"
        );
        let unreadable = RecoveryError::SecretStorage(
            SecretStorageError::MissingOrInvalidBackupDecryptionKey,
        );
        assert_eq!(classify_recover_error(&unreadable), "backup_key_unreadable");
    }

    #[test]
    fn a_recovery_that_restored_nothing_does_not_report_ok() {
        assert_eq!(recover_outcome(true, true), "ok");
        assert_eq!(recover_outcome(true, false), "ok");
        assert_eq!(recover_outcome(false, true), "ok_no_backup");
        assert_eq!(recover_outcome(false, false), "nothing_restored");
    }

    #[test]
    fn an_interrupted_setup_is_reported_until_acknowledged_and_a_running_one_never() {
        let dir = std::env::temp_dir()
            .join(format!("lightning-recovery-marker-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let store = dir.as_path();
        // Every bridge starts its lifecycles at 1, as the real ones do.
        let first = next_bridge_id();
        assert!(!interrupted_setup_to_report(store, first, 1), "no marker, nothing to report");

        // Running: a sync restart in the middle must not report it.
        let marker = SetupMarker::start(store);
        assert!(!interrupted_setup_to_report(store, first, 1), "still running");
        // Aborted (the task's future dropped): reported once per session...
        drop(marker);
        assert!(interrupted_setup_to_report(store, first, 1), "an aborted action is reported");
        assert!(!interrupted_setup_to_report(store, first, 1), "a sync restart does not repeat it");
        // ...and again by the next session while C++ never took it (review L3):
        // a NEW bridge for the same account, whose lifecycle is 1 again
        // (A -> B -> A, review N2; keyed on the lifecycle alone this was
        // suppressed)...
        let second = next_bridge_id();
        assert_ne!(first, second);
        assert!(interrupted_setup_to_report(store, second, 1), "a new bridge reports it again");
        // ...or a later lifecycle of the same bridge.
        assert!(interrupted_setup_to_report(store, second, 2), "next lifecycle, same bridge");
        acknowledge_interrupted_setup(store);
        assert!(!interrupted_setup_to_report(store, second, 3), "acknowledged: gone");

        // Reported to the user by the action itself: nothing left over.
        SetupMarker::start(store).finish();
        assert!(!interrupted_setup_to_report(store, second, 4), "a reported action left nothing");

        // Another process's marker (a crash or a quit) is always reported.
        std::fs::write(store.join(SETUP_MARKER), b"").unwrap();
        assert!(interrupted_setup_to_report(store, second, 5));
        acknowledge_interrupted_setup(store);

        // No store directory: no marker anywhere, nothing reported.
        SetupMarker::start(Path::new("")).finish();
        assert!(!interrupted_setup_to_report(Path::new(""), second, 6));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
