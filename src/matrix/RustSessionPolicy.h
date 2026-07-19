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

// Narrow classifiers: unrelated SDK/network/auth failures must not be turned
// into a destructive local-reset prompt.
bool isStoreOwnershipMismatch(const QString &message);
bool isUnknownToken(const QString &message);

} // namespace matrix::rust_session
