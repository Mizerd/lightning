#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>
#include <QUrl>

#include <cstdlib>

namespace matrix::app_data {

namespace {

// Keep these in sync with QCoreApplication::setOrganizationName /
// setApplicationName in src/main.cpp. Changing either constant WILL
// silently orphan the previous store — bump `legacyRoots()` when that
// happens so users get their old crypto-store cleaned up on reset.
constexpr QLatin1String kOrganizationName{"MatrixClient"};
constexpr QLatin1String kApplicationName{"matrix-client"};
constexpr QLatin1String kRustStoreName{"matrix-rust-sdk-store"};
constexpr QLatin1String kSmokeSessionName{"matrix-rust-sdk-smoke-session.json"};

QString envValue(const char *name)
{
    const char *v = std::getenv(name);
    return (v && *v) ? QString::fromLocal8Bit(v) : QString();
}

// Resolve the app-data base from the current process environment. The actual
// selection logic lives in the pure resolveAppDataBase() so it can be tested
// on any host; this only reads getenv (which is why it stays usable before a
// QCoreApplication exists, as --reset-crypto-store requires).
QString appDataBase()
{
#ifdef Q_OS_WIN
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    return resolveAppDataBase(kWindows,
                              envValue("XDG_DATA_HOME"),
                              envValue("LOCALAPPDATA"),
                              envValue("USERPROFILE"),
                              envValue("HOME"));
}

bool isSafePathComponent(const QString &value)
{
    return !value.isEmpty()
        && value != QLatin1String(".")
        && value != QLatin1String("..")
        && !value.contains(QLatin1Char('/'))
        && !value.contains(QLatin1Char('\\'))
        && !value.contains(QChar::Null);
}

QString normalizedHomeserver(const QString &input)
{
    QUrl url(input.trimmed());
    if (!url.isValid()
        || (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0
            && url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0)
        || url.host().isEmpty()
        || !url.userInfo().isEmpty()
        || !url.query().isEmpty()
        || !url.fragment().isEmpty()) {
        return {};
    }

    url.setScheme(url.scheme().toLower());
    url.setHost(url.host().toLower());
    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path == QLatin1String("/"))
        path.clear();
    url.setPath(path);
    return url.toString(QUrl::FullyEncoded);
}

QString serverNameForUrl(const QString &homeserver)
{
    const QUrl url(homeserver);
    QString server = url.host().toLower();
    if (url.port() >= 0)
        server += QLatin1Char(':') + QString::number(url.port());
    return server;
}

bool validLocalpart(const QString &localpart)
{
    if (localpart.isEmpty() || localpart.trimmed() != localpart)
        return false;
    for (const QChar ch : localpart) {
        if (ch.isSpace() || ch == QLatin1Char('/') || ch == QLatin1Char('\\')
            || ch == QLatin1Char(':') || ch == QChar::Null) {
            return false;
        }
    }
    return localpart != QLatin1String(".") && localpart != QLatin1String("..");
}

bool validServerName(const QString &serverName)
{
    if (serverName.isEmpty() || serverName.trimmed() != serverName)
        return false;
    if (serverName.contains(QLatin1Char('/'))
        || serverName.contains(QLatin1Char('\\'))
        || serverName.contains(QLatin1String(".."))) {
        return false;
    }

    // QUrl provides a strict host parser, including bracketed IPv6 and an
    // optional port, without accepting traversal or path components.
    const QUrl probe(QLatin1String("https://") + serverName);
    return probe.isValid() && !probe.host().isEmpty()
        && (probe.path().isEmpty() || probe.path() == QLatin1String("/"));
}

bool removeFileOrLink(const QString &path, RemovalSummary *summary)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        ++summary->missing;
        return true;
    }
    if (QFile::remove(path)) {
        ++summary->deleted;
        return true;
    }
    ++summary->failed;
    return false;
}

} // namespace

bool AccountIdentity::isValid() const
{
    return isSafeAccountIdentity(*this);
}

QString resolveAppDataBase(bool windows,
                           const QString &xdgDataHome,
                           const QString &localAppData,
                           const QString &userProfile,
                           const QString &home)
{
    if (!xdgDataHome.isEmpty())
        return xdgDataHome;
    if (windows) {
        if (!localAppData.isEmpty())
            return localAppData;
        if (!userProfile.isEmpty())
            return userProfile + QLatin1String("/AppData/Local");
    }
    if (!home.isEmpty())
        return home + QLatin1String("/.local/share");
    return {};
}

QString primaryRoot()
{
    const QString base = appDataBase();
    if (base.isEmpty()) return {};
    return base
        + QLatin1Char('/') + kOrganizationName
        + QLatin1Char('/') + kApplicationName;
}

QString safeUserSlug(const QString &userId)
{
    QString s = userId.trimmed();
    if (!s.startsWith(QLatin1Char('@')))
        return {};
    s.remove(0, 1);
    if (s.count(QLatin1Char(':')) < 1)
        return {};
    s.replace(QLatin1Char(':'), QLatin1Char('_'));
    s.replace(QLatin1Char('/'), QLatin1Char('_'));
    s.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return isSafePathComponent(s) ? s : QString{};
}

bool resolveAccountIdentity(const QString &homeserver,
                            const QString &user,
                            AccountIdentity *identity,
                            QString *error)
{
    if (identity)
        *identity = {};
    if (error)
        error->clear();

    const QString hs = normalizedHomeserver(homeserver);
    QString input = user.trimmed();
    if (hs.isEmpty() || input.isEmpty()) {
        if (error) *error = QStringLiteral("invalid homeserver or empty user");
        return false;
    }

    QString localpart;
    QString serverName;
    if (input.startsWith(QLatin1Char('@'))) {
        const qsizetype colon = input.indexOf(QLatin1Char(':'));
        if (colon <= 1 || colon == input.size() - 1) {
            if (error) *error = QStringLiteral("malformed Matrix user id");
            return false;
        }
        localpart = input.mid(1, colon - 1);
        serverName = input.mid(colon + 1).toLower();
    } else {
        if (input.contains(QLatin1Char(':')) || input.startsWith(QLatin1Char('@'))) {
            if (error) *error = QStringLiteral("malformed Matrix localpart");
            return false;
        }
        localpart = input;
        serverName = serverNameForUrl(hs);
    }

    if (!validLocalpart(localpart) || !validServerName(serverName)) {
        if (error) *error = QStringLiteral("unsafe Matrix account identity");
        return false;
    }

    AccountIdentity out;
    out.homeserver = hs;
    out.userId = QStringLiteral("@%1:%2").arg(localpart, serverName);
    out.slug = safeUserSlug(out.userId);
    out.accountRoot = accountRoot(out.userId);
    out.rustStorePath = rustSdkStorePath(out.userId);
    out.rustSmokeSessionPath = rustSdkSmokeSessionPath(out.userId);
    if (!isSafeAccountIdentity(out)) {
        if (error) *error = QStringLiteral("account path is not safely scoped");
        return false;
    }

    if (identity)
        *identity = out;
    return true;
}

QString accountRoot(const QString &userId)
{
    const QString root = primaryRoot();
    const QString slug = safeUserSlug(userId);
    if (root.isEmpty() || slug.isEmpty())
        return {};
    return root + QLatin1Char('/') + slug;
}

QString rustSdkStorePath(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1Char('/') + kRustStoreName;
}

QString rustSdkSmokeSessionPath(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1Char('/') + kSmokeSessionName;
}

QString rustSdkSmokeSessionTempPath(const QString &userId)
{
    const QString session = rustSdkSmokeSessionPath(userId);
    return session.isEmpty() ? QString{} : session + QLatin1String(".tmp");
}

bool isSafeAccountIdentity(const AccountIdentity &identity)
{
    const QString primary = primaryRoot();
    if (primary.isEmpty())
        return false;
    const QString root = QDir::cleanPath(QFileInfo(primary).absoluteFilePath());
    const QString account = QDir::cleanPath(QFileInfo(identity.accountRoot).absoluteFilePath());
    const QString store = QDir::cleanPath(QFileInfo(identity.rustStorePath).absoluteFilePath());
    const QString session = QDir::cleanPath(
        QFileInfo(identity.rustSmokeSessionPath).absoluteFilePath());
    const QFileInfo rootInfo(root);

    if (root.isEmpty() || !isSafePathComponent(identity.slug)
        || safeUserSlug(identity.userId) != identity.slug
        || account != root + QLatin1Char('/') + identity.slug
        || store != account + QLatin1Char('/') + kRustStoreName
        || session != account + QLatin1Char('/') + kSmokeSessionName
        || account == root || account == QDir::rootPath()
        || rootInfo.isSymLink()) {
        return false;
    }

    const QFileInfo accountInfo(account);
    return !accountInfo.isSymLink();
}

RemovalSummary removeAccountRustState(const AccountIdentity &identity)
{
    RemovalSummary summary;
    if (!isSafeAccountIdentity(identity)) {
        summary.failed = 1;
        return summary;
    }

    const QFileInfo storeInfo(identity.rustStorePath);
    if (!storeInfo.exists() && !storeInfo.isSymLink()) {
        ++summary.missing;
    } else if (storeInfo.isSymLink()) {
        removeFileOrLink(identity.rustStorePath, &summary);
    } else if (storeInfo.isDir()
               && QDir(identity.rustStorePath).removeRecursively()) {
        ++summary.deleted;
    } else {
        ++summary.failed;
    }

    removeFileOrLink(identity.rustSmokeSessionPath, &summary);
    removeFileOrLink(rustSdkSmokeSessionTempPath(identity.userId), &summary);
    return summary;
}

QStringList legacyRoots()
{
    QStringList out;
    const QString base = appDataBase();
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
    if (!dir.exists() || QFileInfo(root).isSymLink()) return out;
    const auto accounts = dir.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &acct : accounts) {
        if (!isSafePathComponent(acct))
            continue;
        const QString accountPath = root + QLatin1Char('/') + acct;
        if (QFileInfo(accountPath).isSymLink())
            continue;
        const QString cryptoPath = root
            + QLatin1Char('/') + acct
            + QLatin1Char('/') + kRustStoreName;
        const QFileInfo storeInfo(cryptoPath);
        if (storeInfo.exists() && storeInfo.isDir() && !storeInfo.isSymLink())
            out.append(cryptoPath);
    }
    return out;
}

} // namespace matrix::app_data
