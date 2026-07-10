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

StoreBlockReason passwordLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool savedSessionExists,
    const QString &savedHomeserver,
    const QString &savedUserId,
    const QString &savedDeviceId);

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
