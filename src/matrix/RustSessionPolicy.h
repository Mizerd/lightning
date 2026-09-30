#pragma once

#include "storage/AppDataPaths.h"

#include <QString>

namespace matrix::rust_session {

enum class StoreBlockReason {
    None,
    MissingSessionMetadata,
    MissingDeviceId,
    DifferentAccount,
    ExistingStoreNeedsRestore,
    MissingStoreForSavedSession,
    // The homeserver rejected the saved access token (M_UNKNOWN_TOKEN). The
    // local store is fine; only the credential died.
    AccessTokenRevoked,
    // The homeserver rejected the access token with soft_logout true: the
    // device still exists on the server, so a password sign-in resumes it with
    // this store and its keys (Synapse says this when a token has expired).
    // Never reported for an OAuth account: its browser sign-in cannot ask for
    // the device again, so there it counts as AccessTokenRevoked.
    AccessTokenExpired,
    // More than one on-disk store could belong to this account, so adoption
    // would have to guess. Never guess: report and let the user decide.
    AmbiguousStoreCandidates,
    // A record and a store exist but the secret backend cannot be read, so
    // whether the sign-in survives is unknown. Nothing may be destroyed on the
    // strength of a question we could not ask.
    SecretBackendUnavailable,
    // The saved account details cannot be parsed into a safe identity: a broken
    // record, not a foreign one.
    InvalidSavedIdentity,
    // The secret store answers, but no longer returns a sign-in that this
    // install saved for the recorded device and never removed (removing it
    // removes the record too): the store lost it (a keyring reset, a Flatpak
    // or snap keyring that changed), the account did not sign out. The local
    // store is intact and is never moved aside on this evidence.
    KeyringLostSession,
};

// The saved-session inputs describe the target account's own record (several
// accounts may be signed in). Password login is blocked when the target's SDK
// store already exists: an existing store may only be opened by restoring the
// exact saved device, never by attaching a new password-login device.
StoreBlockReason passwordLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool targetHasSavedSession,
    const QString &targetSavedDeviceId);

// The one case where a sign-in may start a new device beside an existing
// store: the server has provably ended the device that store belongs to. That
// means the SDK reported a hard logout (soft_logout false), in this process,
// for exactly the account and device the record names. The store is then
// moved aside, never deleted, and the sign-in goes on into a fresh store.
// Anything less keeps ExistingStoreNeedsRestore. Device ids are opaque and
// case-sensitive, so both comparisons are exact.
bool revokedDeviceMayBeReplaced(const QString &recordUserId,
                                const QString &recordDeviceId,
                                const QString &revokedUserId,
                                const QString &revokedDeviceId);

// The one case where a password sign-in may open an existing store: the
// SDK reported a soft logout (soft_logout true: the server keeps the device)
// for exactly the account, homeserver and device the record names, and the
// sign-in is for that same account on that same homeserver. It then asks for
// that device id again, so the store and its keys carry on. Anything less
// keeps ExistingStoreNeedsRestore: in particular a full user id typed with
// another server must never carry the password and this device id there.
// Homeservers are compared normalized (resolveAccountIdentity); device ids
// are opaque and localparts case-sensitive, so every comparison is exact.
bool softLoggedOutDeviceMayResume(const QString &typedUserId,
                                  const QString &typedHomeserver,
                                  const QString &recordUserId,
                                  const QString &recordHomeserver,
                                  const QString &recordDeviceId,
                                  const QString &softUserId,
                                  const QString &softHomeserver,
                                  const QString &softDeviceId);

// Must an unreadable secret block the login rather than let a repair
// proceed? Pure so it can be tested; its call site needs a live Rust client.
//
// The guarded branch arms a repair that deletes a real crypto store. A record
// plus a store plus an unreadable token is not evidence of an orphan, and
// destructive cleanup keys on the record being absent, never on a secret
// being unreadable.
//
// Both predicates are needed:
//   * `secretBackendUnavailable` catches a keyring that locked after startup.
//   * `secretMissesAreInconclusive` catches a fallback store standing in for
//     a native backend it could not open, whose reads are now conclusive (so
//     the switcher can tell an expired sign-in from an unreadable one). That
//     must not make a key-deleting repair reachable.
bool unreadableSecretBlocksLogin(bool storeExists,
                                 bool targetHasRecord,
                                 bool targetTokenReadable,
                                 bool secretBackendUnavailable,
                                 bool secretMissesAreInconclusive);

// For KeyringLostSession: may a password sign-in continue as the recorded
// device, into its store, as after a soft logout? Only when typed as exactly
// the recorded account on the recorded homeserver: the password and the
// device id go nowhere else. Never for a session that signed in through a
// browser (OAuth, SSO), whose sign-in here cannot ask for a device.
// Homeservers are compared normalized (resolveAccountIdentity); user ids
// exactly. False sends the user to the KeyringLostSession card, never to a
// store reset.
bool lostSignInDeviceMayResume(const QString &typedUserId,
                               const QString &typedHomeserver,
                               const QString &recordUserId,
                               const QString &recordHomeserver,
                               const QString &recordDeviceId,
                               bool recordSignedInThroughBrowser);

// OAuth counterpart of passwordLoginBlockReason, applied in Phase B once the
// server has named the canonical user id and the new device id. It cannot be
// asked earlier, but must be asked before the store is opened: a new device
// attaching to another device's store would inherit its Megolm and identity
// state and diverge from what the server believes.
//
// Same `newDeviceId` and `targetSavedDeviceId`: a re-authorization of the
// session owning the store, so restoring is correct. Different: refuse.
StoreBlockReason oauthLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool targetHasSavedSession,
    const QString &targetSavedDeviceId,
    const QString &newDeviceId);

StoreBlockReason restoreBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    const QString &savedDeviceId);

QString diagnosticName(StoreBlockReason reason);

// Keyed by the diagnostic token so "no destructive action for a reason a
// reset cannot repair" is enforced in C++, not by a QML binding. Unknown
// codes are not repairable by deletion.
bool suggestsLocalResetForCode(const QString &reasonCode);

// The message the user sees, specific to each reason; only a genuine
// ownership mismatch may say the store belongs to another session.
QString userMessage(StoreBlockReason reason);

// Whether the destructive "Reset local Lightning session" action is a
// legitimate remedy for this reason. A missing store has nothing to reset;
// a revoked token needs a new sign-in, not local deletion.
bool suggestsLocalReset(StoreBlockReason reason);

// Narrow classifiers: unrelated SDK/network/auth failures must not be turned
// into a destructive local-reset prompt.
bool isStoreOwnershipMismatch(const QString &message);
bool isUnknownToken(const QString &message);

} // namespace matrix::rust_session
