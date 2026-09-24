#pragma once

#include <QString>
#include <QStringList>

// Resolver for the on-disk roots Lightning keeps per-account data under. Two
// callers must agree on this layout:
//
//   1. RustSdkMatrixClient creates
//      <primaryRoot>/<safeUserId>/matrix-rust-sdk-store/ for real logins.
//   2. --reset-crypto-store (preflight in src/main.cpp) deletes those store
//      directories before QGuiApplication exists.
//
// Reset runs before Qt is up, so this computes the path QStandardPaths would
// return from the environment and the OrganizationName/ApplicationName pair
// main.cpp sets. Base directory by platform:
//   - $XDG_DATA_HOME if set (any platform);
//   - on Windows, %LOCALAPPDATA% (or %USERPROFILE%\AppData\Local);
//   - otherwise $HOME/.local/share.
//
// Legacy roots written by older builds are also listed so reset cleans them.
namespace matrix::app_data {

struct AccountIdentity {
    QString homeserver;
    QString userId;
    // Canonical identity slug: always safeUserSlug(userId).
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
    // The slug the on-disk paths are built from.
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

// Same path QStandardPaths::AppLocalDataLocation resolves to at runtime
// (with OrganizationName="MatrixClient" and ApplicationName="matrix-client").
// On Windows this is %LOCALAPPDATA%\MatrixClient\matrix-client. Returns empty
// only when no usable base directory can be resolved from the environment.
QString primaryRoot();

// Pure resolver for the app-data base directory, factored out so the
// platform-specific branch (Windows %LOCALAPPDATA% / %USERPROFILE%) is unit
// testable on any host. `windows` selects the Windows lookup order; the four
// strings are the raw values of $XDG_DATA_HOME, %LOCALAPPDATA%, %USERPROFILE%
// and $HOME (empty when unset). Precedence: a non-empty `portableRoot`, then
// XDG_DATA_HOME, then (Windows only) LOCALAPPDATA / USERPROFILE\AppData\Local,
// then HOME/.local/share, then empty.
//
// `portableRoot` (lightning::portable::dataRoot()) is passed in to keep this
// function pure and testable. It wins over every environment variable: a
// portable installation must not be steered back into AppData by an inherited
// XDG_DATA_HOME or LOCALAPPDATA.
QString resolveAppDataBase(bool windows,
                           const QString &xdgDataHome,
                           const QString &localAppData,
                           const QString &userProfile,
                           const QString &home,
                           const QString &portableRoot = QString());

// Pure: the full app-data root for a resolved base.
//
// Installed builds keep Qt's AppLocalDataLocation layout
// (<base>/MatrixClient/matrix-client). A portable tree omits the vendor/app
// segments; <program dir>/data is already Lightning's. Empty for an empty
// base.
QString composeAppDataRoot(const QString &base, bool portable);

// The cache root Lightning owns: <portable dataRoot>/cache when portable,
// otherwise QStandardPaths::writableLocation(CacheLocation). Only for
// Lightning's own caches, never for user-facing locations such as Save As
// defaults.
QString cacheRoot();

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

// <accountRoot(userId)>/starred-gifs, the GifStarredStore directory. The
// opener and the sign-out/removal cleanup must use the exact same path, or
// removal reports success while decrypted bytes survive. Always derived from
// the canonical accountRoot(userId), never a recorded store slug.
QString starredGifsDir(const QString &userId);

// <accountRoot(userId)>/bridge-labels.json (BridgeLabelStore). One path for
// the opener and the cleanup, for the same reason as starredGifsDir.
QString bridgeLabelsFile(const QString &userId);

// <primaryRoot()>/branding/custom-app-icon.png. Device-global, not
// account-scoped: the window icon applies before any account is restored.
// Always the normalized 512x512 PNG from appicon::normalizeIconBytes, never a
// reference to the user's original file.
QString customAppIconFile();

// Outcome of removing one resolved app-data directory. "Deleted" and "absent"
// must be reported as distinct outcomes.
enum class DirRemoval { Deleted, Absent, Failed };

// Removes `dir` if it exists. Never derives an identity; the caller passes a
// path from accountRoot()/starredGifsDir(). A symlink is removed, never
// followed.
DirRemoval removeAppDataDir(const QString &dir);

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

// Account cleanup for a repair: the SDK store is quarantined (moved aside)
// rather than deleted, and the smoke-session sidecars (an access token, no
// key material) are removed. A repair acts on the app's belief that a store is
// unusable, which can be wrong, so it must be reversible; sign-out and account
// removal use removeAccountRustState() and delete.
//
// A quarantined store counts as `deleted`, so removedAnything() still
// distinguishes work from a no-op.
RemovalSummary quarantineAccountRustState(const AccountIdentity &identity);

// Moves an account's SDK store aside as `<rustStorePath>.orphaned-<UTC
// timestamp>` in the same directory (one atomic rename). Returns the new path,
// or empty when there was nothing to move or the rename failed. Used where the
// app believes a store is unclaimed; that verdict can be wrong, and the store
// may hold the only copy of Megolm keys.
QString quarantineRustStore(const AccountIdentity &identity);

// Legacy roots older builds may have used. Never overlaps primaryRoot().
QStringList legacyRoots();

// primaryRoot() followed by legacyRoots(), deduplicated. Empty strings
// filtered out. Safe to call before QCoreApplication exists.
QStringList allRoots();

// For a given root, return absolute paths of every
// `<root>/<accountSlug>/matrix-rust-sdk-store` directory that currently
// exists. Never touches the filesystem beyond `stat` and one directory
// listing.
QStringList findRustStoresIn(const QString &root);

// Point an identity's on-disk paths at an explicitly recorded store slug
// instead of the one derived from its user id. Returns false (leaving the
// identity untouched) when the slug is not a safe, correctly scoped direct
// child of primaryRoot(). Passing an empty slug rebinds to the canonical
// location, which is how a recording is dropped.
bool bindStoreSlug(AccountIdentity *identity, const QString &storeSlug);

// Account slugs under primaryRoot() holding a Rust SDK store whose name
// differs from `identity.slug` only by ASCII case: stores from builds that
// derived the path from the typed login name.
//
// The exact-case slug is never returned. More than one entry means ownership
// is contested and the caller must refuse to adopt. Adoption records the slug
// (bindStoreSlug, SettingsManager::setStoreSlugFor) and never moves the
// directory; the SDK's ownership check decides whether it was right.
QStringList findCaseVariantStoreSlugs(const AccountIdentity &identity);

// The slug older builds produced when the user typed a bare localpart against
// a delegated homeserver: "alice" at https://matrix.example.com gave
// `alice_matrix.example.com` while the record is `@alice:example.com`.
// findCaseVariantStoreSlugs() cannot see this.
//
// An exact reconstruction, not a heuristic. Empty when equal to the canonical
// slug. The caller must still check the directory exists, is unclaimed and is
// accepted by the SDK.
QString delegatedHomeserverStoreSlug(const AccountIdentity &identity);

} // namespace matrix::app_data
