#pragma once

#include <QString>
#include <QStringList>

namespace matrix::app_data {

struct AccountIdentity {
    QString homeserver;
    QString userId;
    QString slug;
    // Where this account's SDK store actually lives, recorded at login rather
    // than re-derived: the typed localpart and the server-canonical user id
    // (and, under .well-known delegation, the server name) can yield different
    // slugs. Restore, logout, reset and removal all use this one directory.
    // Empty means "same as slug".
    QString storeSlug;
    QString accountRoot;
    QString rustStorePath;
    QString rustSmokeSessionPath;

    bool isValid() const;
    QString effectiveStoreSlug() const {
        return storeSlug.isEmpty() ? slug : storeSlug;
    }
};

struct RemovalSummary {
    int deleted = 0;
    int missing = 0;
    int failed = 0;

    bool ok() const { return failed == 0; }
    // True only when something was actually removed. A missing target counts as
    // `missing` (idempotent), which must never be reported as a completed
    // reset.
    bool removedAnything() const { return deleted > 0; }
};

QString primaryRoot();

QString resolveAppDataBase(bool windows,
                           const QString &xdgDataHome,
                           const QString &localAppData,
                           const QString &userProfile,
                           const QString &home,
                           const QString &portableRoot = QString());

QString composeAppDataRoot(const QString &base, bool portable);

QString cacheRoot();

QString safeUserSlug(const QString &userId);

bool resolveAccountIdentity(const QString &homeserver,
                            const QString &user,
                            AccountIdentity *identity,
                            QString *error = nullptr);

QString accountRoot(const QString &userId);

QString rustSdkStorePath(const QString &userId);

QString starredGifsDir(const QString &userId);

QString personalBackgroundsDir(const QString &accountRootDir);

RemovalSummary removePersonalBackgrounds(const AccountIdentity &identity);

QString bridgeLabelsFile(const QString &userId);

QString customAppIconFile();

enum class DirRemoval { Deleted, Absent, Failed };

DirRemoval removeAppDataDir(const QString &dir);

enum class EmptyDirRemoval {
    Deleted,
    Absent,
    NotEmpty,
    Refused,
    Failed,
};

// Removes an account root ONLY when it is empty. Sign-out deletes the store
// and its sidecars but used to leave the directory that held them, named
// after the account (VM test, 2026-10-07). Never recursive, so it cannot
// delete data: a directory with anything in it is NotEmpty and untouched.
// Refuses a symlink, a non-directory, and anything that is not a direct,
// safely named child of primaryRoot(). The caller passes a RECORDED root
// (AccountIdentity::accountRoot) or the canonical accountRoot(userId); this
// never derives one.
EmptyDirRemoval removeAccountRootIfEmpty(const QString &dir);

inline const char *emptyDirRemovalName(EmptyDirRemoval outcome)
{
    switch (outcome) {
    case EmptyDirRemoval::Deleted: return "deleted";
    case EmptyDirRemoval::Absent: return "absent";
    case EmptyDirRemoval::NotEmpty: return "kept_not_empty";
    case EmptyDirRemoval::Refused: return "refused";
    case EmptyDirRemoval::Failed: return "failed";
    }
    return "failed";
}

QString rustSdkSmokeSessionPath(const QString &userId);

QString rustSdkSmokeSessionTempPath(const QString &userId);

// Validate that the identity and every deletion target are scoped to exactly
// one direct child of primaryRoot(). Existing symlinked account roots are
// rejected so recursive deletion cannot escape through a link.
bool isSafeAccountIdentity(const AccountIdentity &identity);

RemovalSummary removeAccountRustState(const AccountIdentity &identity);

using RemoveDirFn = bool (*)(const QString &path, void *ctx);
RemovalSummary removeAccountRustStateWith(const AccountIdentity &identity,
                                          RemoveDirFn removeDir, void *ctx,
                                          int attempts, int retryDelayMs);

RemovalSummary removeAttemptRustStore(const AccountIdentity &identity);

RemovalSummary quarantineAccountRustState(const AccountIdentity &identity);

enum class QuarantineFailure { None, InUse, Permission };
QString quarantineRustStore(const AccountIdentity &identity,
                            QuarantineFailure *failure = nullptr);

using RenameFn = bool (*)(const QString &from, const QString &to, void *ctx);
QString quarantineRustStoreWith(const AccountIdentity &identity, RenameFn rename,
                                void *ctx, int attempts, int retryDelayMs,
                                QuarantineFailure *failure = nullptr);

QStringList legacyRoots();

QStringList allRoots();

QStringList findRustStoresIn(const QString &root);

bool bindStoreSlug(AccountIdentity *identity, const QString &storeSlug);

QStringList findCaseVariantStoreSlugs(const AccountIdentity &identity);

QString delegatedHomeserverStoreSlug(const AccountIdentity &identity);

}
