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
    // A record and a store exist, but the secret backend cannot be read, so
    // whether the sign-in survives is UNKNOWN. Nothing may be destroyed on
    // the strength of a question we could not ask.
    SecretBackendUnavailable,
    // The saved account details themselves cannot be parsed into a safe
    // identity. Nothing is known about a store because no path can be
    // derived — this is a broken record, not a foreign one.
    InvalidSavedIdentity,
};

// v0.7 multi-account: the saved-session inputs describe the TARGET account's
// own record (several accounts may be signed in at once), not a global
// single session. Password login is blocked when the target's SDK store
// already exists — an existing store may only be opened by restoring the
// exact saved device, never by attaching a fresh password-login device.
StoreBlockReason passwordLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool targetHasSavedSession,
    const QString &targetSavedDeviceId);

StoreBlockReason restoreBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    const QString &savedDeviceId);

QString diagnosticName(StoreBlockReason reason);

// Policy keyed by the diagnostic token rather than the enum, so the
// "no destructive action for a reason a reset cannot repair" invariant can be
// enforced in C++ at the call site instead of by a QML label binding.
// Unknown codes are treated as NOT repairable by deletion — the safe default.
bool suggestsLocalResetForCode(const QString &reasonCode);

// The message the user actually sees. Each reason describes what really
// happened and what to do about it: before this existed, six unrelated
// conditions all claimed the store "belongs to a different Matrix session or
// device", which is true of exactly one of them and prescribes a destructive
// reset for the rest.
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
