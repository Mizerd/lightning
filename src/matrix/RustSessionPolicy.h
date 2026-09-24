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
