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
    // Canonical identity slug: always safeUserSlug(userId).
    QString slug;
    // Where this account's SDK store ACTUALLY lives. Normally identical to
    // `slug`, but it is recorded per account at login rather than re-derived,
    // because the two rules that used to derive it disagree:
    // resolveAccountIdentity() keeps the localpart the user typed, while the
    // homeserver answers a login with its own canonical user id (and, under
    // .well-known delegation, its own server name). Recording the real
    // location is what keeps restore, logout, reset and removal pointed at
    // one directory instead of two. Empty means "same as slug".
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
    // "We actually removed something." ok() alone is NOT evidence of work:
    // a target that never existed counts as `missing`, which keeps cleanup
    // idempotent but must never be reported to the user as a completed
    // reset (see resetLocalSession in RustSdkMatrixClient).
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

// Account-local cleanup for a REPAIR: the SDK store is moved aside by
// quarantineRustStore() rather than deleted, while the smoke-session sidecars
// (which carry an access token, not key material) are removed outright.
//
// This is the counterpart to removeAccountRustState(), and the difference is
// intent. A repair acts on the app's BELIEF that a store is unusable or
// foreign — a belief that has been wrong — so it must stay reversible. An
// explicit sign-out or account removal acts on the user's stated intent that
// the account be gone, where leaving Megolm and device keys on disk would be
// a data-at-rest defect; that path keeps deleting.
//
// A successfully quarantined store counts as `deleted` (removed from
// service), so removedAnything() still distinguishes real work from a no-op.
RemovalSummary quarantineAccountRustState(const AccountIdentity &identity);

// Move an account's SDK store aside instead of deleting it, as
// `<rustStorePath>.orphaned-<UTC timestamp>` in the same account directory.
// Returns the new path, or empty when there was nothing to move or the
// rename failed.
//
// Used where the app believes a store is unclaimed. That belief has been
// wrong before — a store whose account record was keyed under a different
// slug looked orphaned and was recursively deleted, taking Megolm keys that
// existed nowhere else. A verdict that can be wrong must not be irreversible,
// so the store is renamed (one atomic same-directory rename) and left for the
// user to recover or remove.
QString quarantineRustStore(const AccountIdentity &identity);

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

// Point an identity's on-disk paths at an explicitly recorded store slug
// instead of the one derived from its user id. Returns false (leaving the
// identity untouched) when the slug is not a safe, correctly scoped direct
// child of primaryRoot(). Passing an empty slug rebinds to the canonical
// location, which is how a recording is dropped.
bool bindStoreSlug(AccountIdentity *identity, const QString &storeSlug);

// Account slugs under primaryRoot() that hold a real Rust SDK store and whose
// name differs from `identity.slug` ONLY by ASCII case. This is the recovery
// set for stores written by builds that derived the store path from the TYPED
// login name while persisting the account record under the server-canonical
// user id.
//
// The exact-case slug is never returned — this lists only the divergent
// siblings. More than one entry means ownership is contestable and the caller
// MUST refuse to adopt rather than guess which store belongs to the account.
//
// Adoption itself is performed by RECORDING the chosen slug (see
// bindStoreSlug and SettingsManager::setStoreSlugFor), never by relocating a
// directory: the store holds the user's only copy of their Megolm keys, and
// the SDK's own account-ownership check is the authority on whether the
// adoption was right.
QStringList findCaseVariantStoreSlugs(const AccountIdentity &identity);

// The slug an older build would have produced for this account when the user
// typed a BARE LOCALPART against a delegated homeserver.
//
// `resolveAccountIdentity` pairs a bare localpart with the homeserver URL's
// host, but under .well-known delegation the server answers with its real
// server name: typing "alice" at https://matrix.example.com yields the store
// slug `alice_matrix.example.com` while the record is saved as
// `@alice:example.com`. That divergence involves no casing at all, so
// findCaseVariantStoreSlugs() cannot see it.
//
// This is an exact reconstruction of what the old code computed from
// `identity.homeserver` — not a similarity heuristic. Returns empty when it
// would equal the canonical slug (no delegation in play). The caller still
// has to establish that the directory exists, that no other account claims
// it, and that the SDK accepts it.
QString delegatedHomeserverStoreSlug(const AccountIdentity &identity);

} // namespace matrix::app_data
