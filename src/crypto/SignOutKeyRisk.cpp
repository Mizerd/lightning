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
        a = make(Level::NoBackup, QStringLiteral("no_backup"));
    } else if (backupState != QLatin1String("enabled")) {
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (total > backedUp) {
        // Keys this backup version does not hold yet, whatever the upload
        // state says: right after setup the upload may not have started
        // ("idle"), and an "error" leaves them local just the same.
        a = make(Level::Uploading, QStringLiteral("uploading"));
        a.pendingKeys = total - backedUp;
        a.totalKeys = total;
    } else if (uploadState != QLatin1String("done")
               && uploadState != QLatin1String("idle")
               && uploadState != QLatin1String("uploading")) {
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (recoveryState == QLatin1String("disabled")) {
        a = make(Level::NoRecovery, QStringLiteral("no_recovery"));
    } else if (recoveryState == QLatin1String("unknown") || recoveryState.isEmpty()) {
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else {
        a = make(Level::Ok, QStringLiteral("ok"));
    }
    a.otherVerifiedDevices = otherVerified(devices);
    return a;
}

}
