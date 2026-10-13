#pragma once

#include <QString>
#include <QVariantList>

// Whether signing out would take room keys with it.
//
// Pure function over facts the SDK already reported (key-backup state, upload
// progress, recovery state and the account's device list). It reads nothing
// itself and decides nothing about trust: an unanswered or "unknown" fact is
// reported as `Unknown`, never as "fine" and never as "lost".
namespace SignOutKeyRisk {

enum class Level {
    Ok,         // backup is on and caught up, and recovery is set up
    Checking,   // the SDK has not answered yet
    Unknown,    // the SDK answered but could not say
    Uploading,  // backup is on but some keys are not on the server yet
    NoRecovery, // backup is on and caught up, but no recovery key exists
    NoBackup    // no key backup: keys held only by this session are lost
};

struct Assessment {
    Level level = Level::Checking;
    // Stable machine name for QML and tests: ok | checking | unknown |
    // uploading | no_recovery | no_backup.
    QString name;
    qint64 pendingKeys = 0;   // Uploading only
    qint64 totalKeys = 0;     // Uploading only
    int otherVerifiedDevices = 0;
    // True for every level that needs a warning.
    bool warns() const;
};

// `backupState`: the SDK BackupState as Rust names it: unknown | creating |
// enabling | resuming | enabled | downloading | disabling. The SDK has NO
// "disabled" state, so "there is no backup" is NOT read from here but from
// `backupOnServer` (the crypto-health snapshot's backup_exists_on_server, as
// CryptoHealthModel::TriState: 0 unknown, 1 no, 2 yes).
// `uploadState`: idle | uploading | done | error | unknown.
// `recoveryState`: enabled | disabled | incomplete | unknown.
// `answered`: false until the first reply for THIS dialog opening.
// `devices`: the app's session list ({isCurrent, verified}).
Assessment assess(const QString &backupState, const QString &uploadState,
                  qint64 backedUp, qint64 total, int backupOnServer,
                  const QString &recoveryState, bool answered, const QVariantList &devices);

} // namespace SignOutKeyRisk
