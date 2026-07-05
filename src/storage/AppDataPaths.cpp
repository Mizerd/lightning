#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>

#include <cstdlib>

namespace matrix::app_data {

namespace {

// Keep these in sync with QCoreApplication::setOrganizationName /
// setApplicationName in src/main.cpp. Changing either constant WILL
// silently orphan the previous store — bump `legacyRoots()` when that
// happens so users get their old crypto-store cleaned up on reset.
constexpr QLatin1String kOrganizationName{"MatrixClient"};
constexpr QLatin1String kApplicationName{"matrix-client"};

QString xdgDataHome()
{
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        return QString::fromLocal8Bit(xdg);
    const char *home = std::getenv("HOME");
    if (home && *home)
        return QString::fromLocal8Bit(home) + QLatin1String("/.local/share");
    return {};
}

} // namespace

QString primaryRoot()
{
    const QString base = xdgDataHome();
    if (base.isEmpty()) return {};
    return base
        + QLatin1Char('/') + kOrganizationName
        + QLatin1Char('/') + kApplicationName;
}

QString safeUserSlug(const QString &userId)
{
    QString s = userId.trimmed();
    if (s.startsWith(QLatin1Char('@')))
        s.remove(0, 1);
    s.replace(QLatin1Char(':'), QLatin1Char('_'));
    s.replace(QLatin1Char('/'), QLatin1Char('_'));
    s.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (s.isEmpty())
        s = QStringLiteral("_unknown");
    return s;
}

QString accountRoot(const QString &userId)
{
    const QString root = primaryRoot();
    if (root.isEmpty())
        return {};
    return root + QLatin1Char('/') + safeUserSlug(userId);
}

QString rustSdkStorePath(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1String("/matrix-rust-sdk-store");
}

QString rustSdkSmokeSessionPath(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1String("/matrix-rust-sdk-smoke-session.json");
}

QStringList legacyRoots()
{
    QStringList out;
    const QString base = xdgDataHome();
    if (base.isEmpty()) return out;

    // v0.5.0-prep+3 and earlier resolved --reset-crypto-store to
    // <XDG_DATA_HOME>/<applicationName>/, missing the OrganizationName
    // segment. Interactive builds may have written stores under that
    // path via ad-hoc XDG_DATA_HOME overrides or older code paths.
    const QString legacyDirect = base + QLatin1Char('/') + kApplicationName;
    const QString primary = primaryRoot();
    if (!legacyDirect.isEmpty() && legacyDirect != primary)
        out.append(legacyDirect);

    return out;
}

QStringList allRoots()
{
    QStringList out;
    const QString p = primaryRoot();
    if (!p.isEmpty()) out.append(p);
    for (const auto &r : legacyRoots()) {
        if (!r.isEmpty() && !out.contains(r))
            out.append(r);
    }
    return out;
}

QStringList findRustStoresIn(const QString &root)
{
    QStringList out;
    if (root.isEmpty()) return out;
    QDir dir(root);
    if (!dir.exists()) return out;
    const auto accounts = dir.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &acct : accounts) {
        const QString cryptoPath = root
            + QLatin1Char('/') + acct
            + QLatin1String("/matrix-rust-sdk-store");
        if (QFileInfo::exists(cryptoPath))
            out.append(cryptoPath);
    }
    return out;
}

} // namespace matrix::app_data
