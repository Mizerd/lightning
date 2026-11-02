#include "storage/AppDataPaths.h"

#include "storage/PortableMode.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <cstdlib>
#include <utility>

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

bool portableActive()
{
    return lightning::portable::isPortable();
}

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

}

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

QString personalBackgroundsDir(const QString &accountRootDir)
{
    if (accountRootDir.trimmed().isEmpty())
        return {};
    return accountRootDir + QLatin1String("/backgrounds");
}

RemovalSummary removePersonalBackgrounds(const AccountIdentity &identity)
{
    RemovalSummary summary;
    QStringList dirs;
    const QString recorded = personalBackgroundsDir(identity.accountRoot);
    if (!recorded.isEmpty())
        dirs.append(recorded);
    const QString canonical = personalBackgroundsDir(accountRoot(identity.userId));
    if (!canonical.isEmpty() && !dirs.contains(canonical))
        dirs.append(canonical);
    for (const QString &dir : std::as_const(dirs)) {
        switch (removeAppDataDir(dir)) {
        case DirRemoval::Deleted: ++summary.deleted; break;
        case DirRemoval::Absent: ++summary.missing; break;
        case DirRemoval::Failed: ++summary.failed; break;
        }
    }
    return summary;
}

EmptyDirRemoval removeAccountRootIfEmpty(const QString &dir)
{
    const QString primary = primaryRoot();
    if (dir.trimmed().isEmpty() || primary.isEmpty())
        return EmptyDirRemoval::Refused;
    const QFileInfo info(dir);
    if (info.isSymLink())
        return EmptyDirRemoval::Refused;
    if (!info.exists())
        return EmptyDirRemoval::Absent;
    if (!info.isDir())
        return EmptyDirRemoval::Refused;
    const QString root = QDir::cleanPath(QFileInfo(primary).absoluteFilePath());
    const QFileInfo target(QDir::cleanPath(info.absoluteFilePath()));
    const QString name = target.fileName();
    if (target.path() != root || !isSafePathComponent(name))
        return EmptyDirRemoval::Refused;
    if (!QDir(target.absoluteFilePath())
             .isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                      | QDir::System))
        return EmptyDirRemoval::NotEmpty;
    return QDir(root).rmdir(name) ? EmptyDirRemoval::Deleted
                                  : EmptyDirRemoval::Failed;
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

namespace {
Q_LOGGING_CATEGORY(lcStoreRemoval, "matrix.appdatapaths.removal")

bool defaultRemoveDir(const QString &path, void *)
{
    return QDir(path).removeRecursively();
}

struct RetryBudget {
    QElapsedTimer clock;
    qint64 totalMs = 0;
    int attempts = 1;
    int retryDelayMs = 0;

    bool allowsAnotherTry() const
    {
        return retryDelayMs == 0 || clock.elapsed() + retryDelayMs <= totalMs;
    }
};

void removeStoreDirWithRetry(const QString &path, RemoveDirFn removeDir,
                             void *ctx, const RetryBudget &budget,
                             RemovalSummary *summary)
{
    const int tries = qMax(1, budget.attempts);
    int attempt = 0;
    for (; attempt < tries; ++attempt) {
        if (attempt > 0) {
            if (!budget.allowsAnotherTry())
                break;
            if (budget.retryDelayMs > 0)
                QThread::msleep(static_cast<unsigned long>(budget.retryDelayMs));
        }
        if (removeDir(path, ctx) && !QFileInfo::exists(path)) {
            ++summary->deleted;
            if (attempt > 0) {
                qCInfo(lcStoreRemoval)
                    << "store removed after retrying attempts=" << attempt + 1;
            }
            return;
        }
    }
    qCWarning(lcStoreRemoval)
        << "store could not be removed (still in use, or not writable)"
        << "attempts=" << attempt;
    ++summary->failed;
}
}

RemovalSummary removeAccountRustState(const AccountIdentity &identity)
{
#ifdef Q_OS_WIN
    return removeAccountRustStateWith(identity, &defaultRemoveDir, nullptr, 6,
                                      300);
#else
    return removeAccountRustStateWith(identity, &defaultRemoveDir, nullptr, 1,
                                      0);
#endif
}

RemovalSummary removeAccountRustStateWith(const AccountIdentity &identity,
                                          RemoveDirFn removeDir, void *ctx,
                                          int attempts, int retryDelayMs)
{
    RemovalSummary summary;
    if (!isSafeAccountIdentity(identity) || !removeDir) {
        summary.failed = 1;
        return summary;
    }
    RetryBudget budget;
    budget.attempts = qMax(1, attempts);
    budget.retryDelayMs = qMax(0, retryDelayMs);
    budget.totalMs = qint64(budget.attempts - 1) * budget.retryDelayMs;
    budget.clock.start();

    const QFileInfo storeInfo(identity.rustStorePath);
    if (!storeInfo.exists() && !storeInfo.isSymLink()) {
        ++summary.missing;
    } else if (storeInfo.isSymLink()) {
        removeFileOrLink(identity.rustStorePath, &summary);
    } else if (storeInfo.isDir()) {
        removeStoreDirWithRetry(identity.rustStorePath, removeDir, ctx, budget,
                                &summary);
    } else {
        ++summary.failed;
    }

    removeFileOrLink(identity.rustSmokeSessionPath, &summary);
    removeFileOrLink(identity.rustSmokeSessionPath + QLatin1String(".tmp"),
                     &summary);

    const QFileInfo storePath(identity.rustStorePath);
    QDir accountDir(storePath.absolutePath());
    const QString quarantinePattern =
        storePath.fileName() + QLatin1String(".orphaned-*");
    const auto quarantined = accountDir.entryInfoList(
        {quarantinePattern}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : quarantined) {
        if (entry.isSymLink())
            removeFileOrLink(entry.absoluteFilePath(), &summary);
        else
            removeStoreDirWithRetry(entry.absoluteFilePath(), removeDir, ctx,
                                    budget, &summary);
    }
    return summary;
}

RemovalSummary removeAttemptRustStore(const AccountIdentity &identity)
{
    RemovalSummary summary;
    if (!isSafeAccountIdentity(identity)) {
        summary.failed = 1;
        return summary;
    }
    const QFileInfo storeInfo(identity.rustStorePath);
    if (!storeInfo.exists() && !storeInfo.isSymLink())
        ++summary.missing;
    else if (storeInfo.isSymLink())
        removeFileOrLink(identity.rustStorePath, &summary);
    else if (storeInfo.isDir() && QDir(identity.rustStorePath).removeRecursively())
        ++summary.deleted;
    else
        ++summary.failed;
    if (summary.failed == 0
        && removeAccountRootIfEmpty(identity.accountRoot) == EmptyDirRemoval::Failed)
        qCWarning(lcStoreRemoval) << "a failed attempt's empty account directory "
                                     "could not be removed";
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
        removeFileOrLink(identity.rustStorePath, &summary);
    } else if (storeInfo.isDir()) {
        if (quarantineRustStore(identity).isEmpty())
            ++summary.failed;
        else
            ++summary.deleted;
    } else {
        ++summary.failed;
    }

    removeFileOrLink(identity.rustSmokeSessionPath, &summary);
    removeFileOrLink(identity.rustSmokeSessionPath + QLatin1String(".tmp"),
                     &summary);
    return summary;
}

namespace {
Q_LOGGING_CATEGORY(lcAppDataPaths, "matrix.appdatapaths")

bool defaultRename(const QString &from, const QString &to, void *)
{
    return QDir().rename(from, to);
}

bool moveStoreOnce(const QString &storePath, const QString &target,
                   RenameFn rename, void *ctx, bool *rollbackFailed)
{
    if (rename(storePath, target, ctx))
        return true;

    QDir parent(QFileInfo(storePath).absolutePath());
    if (!parent.mkpath(QFileInfo(target).fileName()))
        return false;
    const QDir source(storePath);
    const QDir destination(target);
    const QStringList names = source.entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QStringList moved;
    for (const QString &name : names) {
        if (!rename(source.absoluteFilePath(name),
                    destination.absoluteFilePath(name), ctx)) {
            int notRestored = 0;
            for (const QString &done : std::as_const(moved)) {
                if (!rename(destination.absoluteFilePath(done),
                            source.absoluteFilePath(done), ctx))
                    ++notRestored;
            }
            if (notRestored > 0) {
                qCWarning(lcAppDataPaths)
                    << "store rollback incomplete; entries left in the aside "
                       "directory:" << notRestored;
                if (rollbackFailed)
                    *rollbackFailed = true;
            }
            parent.rmdir(QFileInfo(target).fileName());
            return false;
        }
        moved.append(name);
    }
    parent.rmdir(QFileInfo(storePath).fileName());
    return true;
}
}

QString quarantineRustStoreWith(const AccountIdentity &identity, RenameFn rename,
                                void *ctx, int attempts, int retryDelayMs,
                                QuarantineFailure *failure)
{
    if (failure)
        *failure = QuarantineFailure::None;
    if (!isSafeAccountIdentity(identity))
        return {};
    const QFileInfo storeInfo(identity.rustStorePath);
    if (!storeInfo.exists() || !storeInfo.isDir() || storeInfo.isSymLink())
        return {};

    const QString stamp = QDateTime::currentDateTimeUtc()
                              .toString(QStringLiteral("yyyyMMdd-hhmmsszzz"));
    const QString target = identity.rustStorePath
        + QLatin1String(".orphaned-") + stamp;
    if (QFileInfo::exists(target))
        return {};

    bool rollbackFailed = false;
    for (int attempt = 0; attempt < qMax(1, attempts); ++attempt) {
        if (attempt > 0 && retryDelayMs > 0)
            QThread::msleep(static_cast<unsigned long>(retryDelayMs));
        if (moveStoreOnce(identity.rustStorePath, target, rename, ctx,
                          &rollbackFailed))
            return target;
    }
    if (failure) {
        const QFileInfo parentInfo(
            QFileInfo(identity.rustStorePath).absolutePath());
        *failure = (rollbackFailed || parentInfo.isWritable())
            ? QuarantineFailure::InUse
                                           : QuarantineFailure::Permission;
    }
    return {};
}

QString quarantineRustStore(const AccountIdentity &identity,
                            QuarantineFailure *failure)
{
    return quarantineRustStoreWith(identity, &defaultRename, nullptr, 6, 300,
                                   failure);
}

QStringList legacyRoots()
{
    QStringList out;
    if (portableActive())
        return out;
    const QString base = appDataBase();
    if (base.isEmpty()) return out;

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
        if (slug == identity.effectiveStoreSlug())
            continue;
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

    const QString urlServer = serverNameForUrl(identity.homeserver);
    if (urlServer.isEmpty() || !validServerName(urlServer)
        || !validLocalpart(localpart)) {
        return {};
    }
    const QString slug = safeUserSlug(
        QStringLiteral("@%1:%2").arg(localpart, urlServer));
    return slug == identity.slug ? QString{} : slug;
}

bool bindStoreSlug(AccountIdentity *identity, const QString &storeSlug)
{
    if (!identity || identity->slug.isEmpty())
        return false;

    const QString root = primaryRoot();
    if (root.isEmpty())
        return false;

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

}
