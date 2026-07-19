#include "matrix/RustSessionPolicy.h"

namespace matrix::rust_session {

StoreBlockReason passwordLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool targetHasSavedSession,
    const QString &targetSavedDeviceId)
{
    Q_UNUSED(target);
    if (!storeExists)
        return StoreBlockReason::None;
    if (!targetHasSavedSession)
        return StoreBlockReason::MissingSessionMetadata;
    if (targetSavedDeviceId.trimmed().isEmpty())
        return StoreBlockReason::MissingDeviceId;

    // Password login asks the homeserver for a new device. An existing SDK
    // store can only be safely opened by restoring the exact saved device;
    // attaching the new device to it is precisely the ownership bug fixed in
    // v0.5.5. With multi-account this account should be activated from the
    // account switcher instead of logged in again.
    return StoreBlockReason::ExistingStoreNeedsRestore;
}

StoreBlockReason restoreBlockReason(
    const app_data::AccountIdentity &,
    bool storeExists,
    const QString &savedDeviceId)
{
    if (savedDeviceId.trimmed().isEmpty())
        return StoreBlockReason::MissingDeviceId;
    if (!storeExists)
        return StoreBlockReason::MissingStoreForSavedSession;
    return StoreBlockReason::None;
}

QString diagnosticName(StoreBlockReason reason)
{
    switch (reason) {
    case StoreBlockReason::None:
        return QStringLiteral("none");
    case StoreBlockReason::MissingSessionMetadata:
        return QStringLiteral("store_without_session_metadata");
    case StoreBlockReason::MissingDeviceId:
        return QStringLiteral("session_without_device_id");
    case StoreBlockReason::DifferentAccount:
        return QStringLiteral("session_account_mismatch");
    case StoreBlockReason::ExistingStoreNeedsRestore:
        return QStringLiteral("existing_store_requires_restore");
    case StoreBlockReason::MissingStoreForSavedSession:
        return QStringLiteral("saved_session_without_store");
    }
    return QStringLiteral("unknown");
}

bool isStoreOwnershipMismatch(const QString &message)
{
    return message.contains(
               QLatin1String("store doesn't match the account in the constructor"),
               Qt::CaseInsensitive)
        || message.contains(
               QLatin1String("account in the store doesn't match"),
               Qt::CaseInsensitive)
        || message.contains(
               QLatin1String("local Lightning Rust SDK store belongs to a different "
                             "Matrix session or device"),
               Qt::CaseInsensitive);
}

bool isUnknownToken(const QString &message)
{
    return message.contains(QLatin1String("M_UNKNOWN_TOKEN"),
                            Qt::CaseInsensitive)
        || message.contains(QLatin1String("Invalid access token"),
                            Qt::CaseInsensitive);
}

} // namespace matrix::rust_session
