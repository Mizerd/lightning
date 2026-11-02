#pragma once

#include <QString>
#include <QVariantList>

namespace SignOutKeyRisk {

enum class Level {
    Ok,
    Checking,
    Unknown,
    Uploading,
    NoRecovery,
    NoBackup
};

struct Assessment {
    Level level = Level::Checking;
    QString name;
    qint64 pendingKeys = 0;
    qint64 totalKeys = 0;
    int otherVerifiedDevices = 0;
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

}
