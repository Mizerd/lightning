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
                  qint64 backedUp, qint64 total, const QString &recoveryState,
                  bool answered, const QVariantList &devices)
{
    Assessment a;
    if (!answered || backupState.isEmpty()) {
        a = make(Level::Checking, QStringLiteral("checking"));
    } else if (backupState == QLatin1String("unknown")) {
        // "Not known" is not "no backup" (CryptoHealthModel's rule).
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (backupState == QLatin1String("disabled")) {
        a = make(Level::NoBackup, QStringLiteral("no_backup"));
    } else if (backupState != QLatin1String("enabled")) {
        // creating / enabling / resuming / downloading / disabling: the
        // backup is in flux, so nothing can be promised about the keys.
        a = make(Level::Unknown, QStringLiteral("unknown"));
    } else if (uploadState == QLatin1String("uploading") && total > backedUp) {
        a = make(Level::Uploading, QStringLiteral("uploading"));
        a.pendingKeys = total - backedUp;
        a.totalKeys = total;
    } else if (uploadState == QLatin1String("error")) {
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
