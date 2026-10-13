#include "crypto/SignOutKeyRisk.h"

#include <QVariantMap>

namespace SignOutKeyRisk {

bool Assessment::warns() const
{
    return level == Level::Uploading || level == Level::NoRecovery
        || level == Level::NoBackup || level == Level::Unknown;
}

static int otherVerified(const QVariantList &devices)
{
    int n = 0;
    for (const QVariant &v : devices) {
        const QVariantMap d = v.toMap();
        if (!d.value(QStringLiteral("isCurrent")).toBool()
            && d.value(QStringLiteral("verified")).toBool())
            ++n;
    }
    return n;
}

static Assessment make(Level level, const QString &name)
{
    Assessment a;
    a.level = level;
    a.name = name;
    return a;
}

Assessment assess(const QString &backupState, const QString &uploadState,
                  qint64 backedUp, qint64 total, int backupOnServer,
                  const QString &recoveryState, bool answered, const QVariantList &devices)
{
    Assessment a;
    if (!answered || backupState.isEmpty()) {
        a = make(Level::Checking, QStringLiteral("checking"));
    } else if (backupOnServer == 1) {
        // The server was asked and holds no backup version.
        a = make(Level::NoBackup, QStringLiteral("no_backup"));
    } else if (backupState != QLatin1String("enabled")) {
        // unknown / creating / enabling / resuming / downloading / disabling:
        // the backup is not (yet) in use, so nothing can be promised.
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (uploadState == QLatin1String("uploading") && total > backedUp) {
        a = make(Level::Uploading, QStringLiteral("uploading"));
        a.pendingKeys = total - backedUp;
        a.totalKeys = total;
    } else if (uploadState != QLatin1String("done")
               && uploadState != QLatin1String("idle")
               && uploadState != QLatin1String("uploading")) {
        // "unknown" (the SDK did not report within the bound) and "error".
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (recoveryState == QLatin1String("disabled")) {
        a = make(Level::NoRecovery, QStringLiteral("no_recovery"));
    } else if (recoveryState == QLatin1String("unknown") || recoveryState.isEmpty()) {
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else {
        // enabled, or incomplete (secret storage exists; this session simply
        // does not hold its secrets, so signing out loses nothing more).
        a = make(Level::Ok, QStringLiteral("ok"));
    }
    a.otherVerifiedDevices = otherVerified(devices);
    return a;
}

} // namespace SignOutKeyRisk
