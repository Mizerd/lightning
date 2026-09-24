#include "storage/AppDataPaths.h"

#include "storage/PortableMode.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStandardPaths>
#include <QUrl>

#include <cstdlib>

namespace matrix::app_data {

namespace {

// Must match setOrganizationName/setApplicationName in src/main.cpp. Changing
// either orphans existing stores; add the old root to legacyRoots() if so.
constexpr QLatin1String kOrganizationName{"MatrixClient"};
constexpr QLatin1String kApplicationName{"matrix-client"};
constexpr QLatin1String kRustStoreName{"matrix-rust-sdk-store"};
constexpr QLatin1String kSmokeSessionName{"matrix-rust-sdk-smoke-session.json"};

QString envValue(const char *name)
{
    const char *v = std::getenv(name);
    return (v && *v) ? QString::fromLocal8Bit(v) : QString();
}

// The app-data base from the process environment and the portable decision.
// Usable before QCoreApplication exists (--reset-crypto-store); the selection
// logic is the pure resolveAppDataBase().
QString appDataBase()
{
#ifdef Q_OS_WIN
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    const QString portableRoot = lightning::portable::dataRoot();
    if (lightning::portable::isPortable() && portableRoot.isEmpty()) {
        // Portable, but the executable directory is unknown: no root, never a
        // fallback to the environment, which would put the SDK store outside
        // the portable folder. Callers refuse an empty root, and main.cpp exits
        // first.
        return {};
    }
    return resolveAppDataBase(kWindows,
                              envValue("XDG_DATA_HOME"),
                              envValue("LOCALAPPDATA"),
                              envValue("USERPROFILE"),
                              envValue("HOME"),
                              portableRoot);
}

// Asks lightning::portable, which owns the cached decision.
bool portableActive()
{
    return lightning::portable::isPortable();
}

// ASCII-only case-insensitive equality. Full Unicode folding could equate
// distinct localparts (dotless i, Kelvin sign); store adoption must match only
// the a-z/A-Z divergence older builds produced.
bool equalsIgnoringAsciiCase(const QString &a, const QString &b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        QChar x = a.at(i);
        QChar y = b.at(i);
        if (x.unicode() < 128 && y.unicode() < 128) {
            if (x.toLatin1() >= 'A' && x.toLatin1() <= 'Z')
                x = QChar(x.unicode() + 32);
            if (y.toLatin1() >= 'A' && y.toLatin1() <= 'Z')
                y = QChar(y.unicode() + 32);
        }
        if (x != y)
            return false;
    }
    return true;
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

    // QUrl's strict host parser handles bracketed IPv6 and a port and rejects
    // path components.
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
                           const QString &home,
                           const QString &portableRoot)
{
    // An override, not a fallback; see the header.
    if (!portableRoot.isEmpty())
        return portableRoot;
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

QString composeAppDataRoot(const QString &base, bool portable)
{
    if (base.isEmpty())
        return {};
    if (portable)
        return base + QLatin1String("/matrix");
    return base
        + QLatin1Char('/') + kOrganizationName
        + QLatin1Char('/') + kApplicationName;
}

QString primaryRoot()
{
    return composeAppDataRoot(appDataBase(), portableActive());
}

QString cacheRoot()
{
    if (portableActive())
        return lightning::portable::cacheDir();
    // Unchanged for installed builds.
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
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
    out.storeSlug = out.slug;
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

QString starredGifsDir(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1String("/starred-gifs");
}

QString bridgeLabelsFile(const QString &userId)
{
    const QString account = accountRoot(userId);
    if (account.isEmpty())
        return {};
    return account + QLatin1String("/bridge-labels.json");
}

QString customAppIconFile()
{
    const QString root = primaryRoot();
    if (root.isEmpty())
        return {};
    return root + QLatin1String("/branding/custom-app-icon.png");
}

DirRemoval removeAppDataDir(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return DirRemoval::Absent;
    const QFileInfo info(dir);
    if (!info.exists() && !info.isSymLink())
        return DirRemoval::Absent;
    if (info.isSymLink()) {
        // Never recurse through a symlink; remove only the link.
        return QFile::remove(dir) ? DirRemoval::Deleted : DirRemoval::Failed;
    }
    return QDir(dir).removeRecursively() ? DirRemoval::Deleted
                                         : DirRemoval::Failed;
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
    // Paths use the recorded store slug, which may differ from the identity
    // slug. Both are validated: the identity slug against the user id, the
    // store slug as a safe direct child of primaryRoot().
    const QString storeSlug = identity.effectiveStoreSlug();

    if (root.isEmpty() || !isSafePathComponent(identity.slug)
        || !isSafePathComponent(storeSlug)
        || safeUserSlug(identity.userId) != identity.slug
        || account != root + QLatin1Char('/') + storeSlug
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
    // From the identity's own recorded session path, not re-derived from the
    // user id: with a divergent store slug that would delete another account's
    // file and report the reset as completed.
    removeFileOrLink(identity.rustSmokeSessionPath + QLatin1String(".tmp"),
                     &summary);

    // Quarantined stores are siblings of rustStorePath, each a complete crypto
    // store. Remove them too, or sign-out leaves key material behind (and
    // repairs accumulate them).
    const QFileInfo storePath(identity.rustStorePath);
    QDir accountDir(storePath.absolutePath());
    const QString quarantinePattern =
        storePath.fileName() + QLatin1String(".orphaned-*");
    const auto quarantined = accountDir.entryInfoList(
        {quarantinePattern}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : quarantined) {
        if (entry.isSymLink())
            removeFileOrLink(entry.absoluteFilePath(), &summary);
        else if (QDir(entry.absoluteFilePath()).removeRecursively())
            ++summary.deleted;
        else
            ++summary.failed;
    }
    return summary;
}

RemovalSummary quarantineAccountRustState(const AccountIdentity &identity)
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
        // A symlink only points at a store; removing it destroys nothing.
        removeFileOrLink(identity.rustStorePath, &summary);
    } else if (storeInfo.isDir()) {
        if (quarantineRustStore(identity).isEmpty())
            ++summary.failed;
        else
            ++summary.deleted;
    } else {
        ++summary.failed;
    }

    // Sidecars carry an access token, not keys; do not leave it behind.
    removeFileOrLink(identity.rustSmokeSessionPath, &summary);
    removeFileOrLink(identity.rustSmokeSessionPath + QLatin1String(".tmp"),
                     &summary);
    return summary;
}

QString quarantineRustStore(const AccountIdentity &identity)
{
    if (!isSafeAccountIdentity(identity))
        return {};
    const QFileInfo storeInfo(identity.rustStorePath);
    if (!storeInfo.exists() || !storeInfo.isDir() || storeInfo.isSymLink())
        return {};

    // A timestamped sibling in the same account directory: still scoped, easy
    // to find, and recoverable if the verdict was wrong.
    const QString stamp = QDateTime::currentDateTimeUtc()
                              .toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    const QString target = identity.rustStorePath
        + QLatin1String(".orphaned-") + stamp;
    if (QFileInfo::exists(target))
        return {};
    // Same directory: one atomic rename on POSIX.
    if (QDir().rename(identity.rustStorePath, target))
        return target;

    // Windows refuses to rename a directory while files in it are open, and
    // matrix-sdk holds its sqlite files open. Renaming the files is allowed (as
    // in AtomicReplace::moveDirectoryEntries), so move the entries into a fresh
    // sibling. An empty original directory is no longer a store the SDK adopts,
    // so it counts as moved.
    QDir parent(QFileInfo(identity.rustStorePath).absolutePath());
    if (!parent.mkpath(QFileInfo(target).fileName()))
        return {};
    const QDir source(identity.rustStorePath);
    const QDir destination(target);
    const QStringList names = source.entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (!QDir().rename(source.absoluteFilePath(name),
                           destination.absoluteFilePath(name))) {
            // Partial move: fail rather than leave the store split across two
            // directories. Nothing was deleted.
            return {};
        }
    }
    // Removed only if still empty, so a file that appeared mid-move survives.
    parent.rmdir(QFileInfo(identity.rustStorePath).fileName());
    return target;
}

QStringList legacyRoots()
{
    QStringList out;
    // A portable tree has no legacy layout; do not hand a never-existing
    // directory to --reset-crypto-store's scan.
    if (portableActive())
        return out;
    const QString base = appDataBase();
    if (base.isEmpty()) return out;

    // Very old builds resolved --reset-crypto-store to
    // <XDG_DATA_HOME>/<applicationName>/ without the organization segment.
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

QStringList findCaseVariantStoreSlugs(const AccountIdentity &identity)
{
    QStringList out;
    const QString root = primaryRoot();
    if (root.isEmpty() || !isSafeAccountIdentity(identity))
        return out;

    QDir dir(root);
    if (!dir.exists() || QFileInfo(root).isSymLink())
        return out;

    const auto accounts = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                        QDir::Name);
    for (const QString &slug : accounts) {
        // Skip the directory the caller already uses (its effective store slug,
        // not the canonical one, which may be the divergent directory being
        // sought).
        if (slug == identity.effectiveStoreSlug())
            continue;
        // Case-insensitive on purpose. QSettings keys are case-sensitive on
        // Linux but not on Windows/macOS; the filesystem keeps the directories
        // apart either way, so the recovery set comes from the listing, not
        // from settings.
        if (!equalsIgnoringAsciiCase(slug, identity.slug))
            continue;
        if (!isSafePathComponent(slug))
            continue;
        const QString accountPath = root + QLatin1Char('/') + slug;
        if (QFileInfo(accountPath).isSymLink())
            continue;
        const QFileInfo storeInfo(accountPath + QLatin1Char('/') + kRustStoreName);
        if (storeInfo.exists() && storeInfo.isDir() && !storeInfo.isSymLink())
            out.append(slug);
    }
    return out;
}

QString delegatedHomeserverStoreSlug(const AccountIdentity &identity)
{
    if (identity.slug.isEmpty() || identity.homeserver.isEmpty())
        return {};
    const QString userId = identity.userId.trimmed();
    const qsizetype colon = userId.indexOf(QLatin1Char(':'));
    if (!userId.startsWith(QLatin1Char('@')) || colon <= 1)
        return {};
    const QString localpart = userId.mid(1, colon - 1);

    // The same pairing resolveAccountIdentity() uses for a bare localpart: the
    // URL host (with port), lowercased.
    const QString urlServer = serverNameForUrl(identity.homeserver);
    if (urlServer.isEmpty() || !validServerName(urlServer)
        || !validLocalpart(localpart)) {
        return {};
    }
    const QString slug = safeUserSlug(
        QStringLiteral("@%1:%2").arg(localpart, urlServer));
    // No delegation: the URL host is the server name.
    return slug == identity.slug ? QString{} : slug;
}

bool bindStoreSlug(AccountIdentity *identity, const QString &storeSlug)
{
    if (!identity || identity->slug.isEmpty())
        return false;

    const QString root = primaryRoot();
    if (root.isEmpty())
        return false;

    // Empty drops the recording and returns to the canonical layout.
    const QString slug = storeSlug.trimmed().isEmpty() ? identity->slug
                                                       : storeSlug.trimmed();
    if (!isSafePathComponent(slug))
        return false;

    AccountIdentity probe = *identity;
    probe.storeSlug = slug;
    probe.accountRoot = root + QLatin1Char('/') + slug;
    probe.rustStorePath =
        probe.accountRoot + QLatin1Char('/') + kRustStoreName;
    probe.rustSmokeSessionPath =
        probe.accountRoot + QLatin1Char('/') + kSmokeSessionName;
    // Refuse rather than half-apply; paths must stay inside the account layout.
    if (!isSafeAccountIdentity(probe))
        return false;

    *identity = probe;
    return true;
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
