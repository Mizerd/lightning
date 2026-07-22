#pragma once

#include <QString>
#include <QStringList>

// Central resolver for the on-disk roots Lightning keeps per-account data
// under. Two callers must agree on this layout or things break subtly:
//
//   1. RustSdkMatrixClient — creates
//      <primaryRoot>/<safeUserId>/matrix-rust-sdk-store/ for real logins.
//   2. --reset-crypto-store (preflight in src/main.cpp) — deletes those
//      store directories WITHOUT constructing QGuiApplication first.
//
// Because reset runs before Qt is up, we cannot call
// `QStandardPaths::writableLocation(AppLocalDataLocation)` there. This
// helper computes the same path Qt would return from the environment and
// the compile-time OrganizationName / ApplicationName pair that `main.cpp`
// sets on `QCoreApplication`. The base directory is resolved per platform:
//   - $XDG_DATA_HOME if set (any platform);
//   - on Windows, %LOCALAPPDATA% (or %USERPROFILE%\AppData\Local);
//   - otherwise $HOME/.local/share.
// Reading only POSIX $HOME/$XDG_DATA_HOME (the pre-fix behaviour) returned
// an empty base on native Windows, which refused every account cache path.
//
// The helper also lists LEGACY roots that earlier v0.5.0-prep+3 / +4
// builds wrote to (before this fix). Reset scans those as well so a
// migrated user gets everything cleaned up.
namespace matrix::app_data {

struct AccountIdentity {
    QString homeserver;
    QString userId;
    QString slug;
    QString accountRoot;
    QString rustStorePath;
    QString rustSmokeSessionPath;

    bool isValid() const;
};

struct RemovalSummary {
    int deleted = 0;
    int missing = 0;
    int failed = 0;

    bool ok() const { return failed == 0; }
};

// Same path QStandardPaths::AppLocalDataLocation resolves to at runtime
// (with OrganizationName="MatrixClient" and ApplicationName="matrix-client").
// On Windows this is %LOCALAPPDATA%\MatrixClient\matrix-client. Returns empty
// only when no usable base directory can be resolved from the environment.
QString primaryRoot();

// Pure resolver for the app-data base directory, factored out so the
// platform-specific branch (Windows %LOCALAPPDATA% / %USERPROFILE%) is unit
// testable on any host. `windows` selects the Windows lookup order; the four
// strings are the raw values of $XDG_DATA_HOME, %LOCALAPPDATA%, %USERPROFILE%
// and $HOME (empty when unset). Precedence: XDG_DATA_HOME, then (Windows only)
// LOCALAPPDATA / USERPROFILE\AppData\Local, then HOME/.local/share, then empty.
QString resolveAppDataBase(bool windows,
                           const QString &xdgDataHome,
                           const QString &localAppData,
                           const QString &userProfile,
                           const QString &home);

// Safe per-account directory slug used under the app data roots. Returns an
// empty string for malformed or unsafe Matrix user ids.
QString safeUserSlug(const QString &userId);

// Canonical account identity used by login, reset, the Rust store, and the
// C++ cache. `user` may be a full MXID or a localpart. Localparts are paired
// with the normalized homeserver host. Returns false rather than guessing if
// either input cannot produce a safe account-specific path.
bool resolveAccountIdentity(const QString &homeserver,
                            const QString &user,
                            AccountIdentity *identity,
                            QString *error = nullptr);

// <primaryRoot>/<safeUserId>. Returns empty if primaryRoot() is empty.
QString accountRoot(const QString &userId);

// <accountRoot>/<matrix-rust-sdk-store>. Matches RustSdkMatrixClient.
QString rustSdkStorePath(const QString &userId);

// Smoke-only MatrixSession sidecar used by LIGHTNING_TEST_PERSISTENT_STORE=1.
// It is account-specific session state, not an interactive QSettings or
// SecretStore entry. Never print its token contents.
QString rustSdkSmokeSessionPath(const QString &userId);

// Temporary name used while atomically replacing the smoke session sidecar.
QString rustSdkSmokeSessionTempPath(const QString &userId);

// Validate that the identity and every deletion target are scoped to exactly
// one direct child of primaryRoot(). Existing symlinked account roots are
// rejected so recursive deletion cannot escape through a link.
bool isSafeAccountIdentity(const AccountIdentity &identity);

// Remove only account-local Rust SDK state. cache.sqlite and every other file
// under the account directory are deliberately preserved. Missing targets
// count as successful/idempotent cleanup.
RemovalSummary removeAccountRustState(const AccountIdentity &identity);

// Legacy roots that pre-fix builds may have created directories under.
// Currently just the "no org prefix" variant that the old reset code
// scanned. Never overlaps with `primaryRoot()`.
QStringList legacyRoots();

// primaryRoot() followed by legacyRoots(), deduplicated. Empty strings
// filtered out. Safe to call before QCoreApplication exists.
QStringList allRoots();

// For a given root, return absolute paths of every
// `<root>/<accountSlug>/matrix-rust-sdk-store` directory that currently
// exists. Never touches the filesystem beyond `stat` and one directory
// listing.
QStringList findRustStoresIn(const QString &root);

} // namespace matrix::app_data
