#include "matrix/RustSessionPolicy.h"

#include <QCoreApplication>

namespace matrix::rust_session {

bool unreadableSecretBlocksLogin(bool storeExists,
                                 bool targetHasRecord,
                                 bool targetTokenReadable,
                                 bool secretBackendUnavailable,
                                 bool secretMissesAreInconclusive)
{
    if (!storeExists || !targetHasRecord || targetTokenReadable)
        return false;
    return secretBackendUnavailable || secretMissesAreInconclusive;
}

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

    // Password login creates a new device, and an existing SDK store may only
    // be opened by restoring its exact saved device. The account should be
    // activated from the switcher instead.
    return StoreBlockReason::ExistingStoreNeedsRestore;
}

StoreBlockReason oauthLoginBlockReason(
    const app_data::AccountIdentity &target,
    bool storeExists,
    bool targetHasSavedSession,
    const QString &targetSavedDeviceId,
    const QString &newDeviceId)
{
    Q_UNUSED(target);
    // Nothing on disk for this account: a first OAuth sign-in creates the
    // store fresh, which is always safe.
    if (!storeExists)
        return StoreBlockReason::None;

    // A store with no record beside it cannot be proven to belong to this
    // account. Same verdict as the password path.
    if (!targetHasSavedSession)
        return StoreBlockReason::MissingSessionMetadata;

    const QString saved = targetSavedDeviceId.trimmed();
    if (saved.isEmpty())
        return StoreBlockReason::MissingDeviceId;

    // The authorization server must name the device; without it
    // re-authorization cannot be told from a new device, and guessing is not
    // allowed.
    const QString fresh = newDeviceId.trimmed();
    if (fresh.isEmpty())
        return StoreBlockReason::MissingDeviceId;

    // Re-authorizing the device that owns this store (an expired session signed
    // in again): restoring into it is correct. Device ids are opaque
    // case-sensitive strings, so compare exactly.
    if (saved == fresh)
        return StoreBlockReason::None;

    // A new device: the existing store belongs to the old one and must not be
    // adopted.
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
    case StoreBlockReason::AccessTokenRevoked:
        return QStringLiteral("access_token_revoked");
    case StoreBlockReason::AmbiguousStoreCandidates:
        return QStringLiteral("ambiguous_store_candidates");
    case StoreBlockReason::SecretBackendUnavailable:
        return QStringLiteral("secret_backend_unavailable");
    case StoreBlockReason::InvalidSavedIdentity:
        // Stable token: restoreSession() emits it as a literal and the UI keys
        // off it.
        return QStringLiteral("invalid_saved_account_identity");
    }
    return QStringLiteral("unknown");
}

QString userMessage(StoreBlockReason reason)
{
    // Not named `tr`: lupdate would warn that this namespace "lacks Q_OBJECT
    // macro". The context is explicit in translate(), so extraction works
    // either way.
    const auto msg = [](const char *text) {
        return QCoreApplication::translate("matrix::rust_session", text);
    };
    switch (reason) {
    case StoreBlockReason::None:
        return {};
    case StoreBlockReason::MissingStoreForSavedSession:
        // The store is not there, so there is nothing to reset; signing in
        // again is the remedy.
        return msg("Lightning has a saved sign-in for this account but its "
                  "local encryption store is missing. Sign in again to "
                  "recreate it. Messages already on the server are not "
                  "affected, but encrypted history whose keys were only "
                  "stored locally may need to be recovered from key backup "
                  "or another signed-in session.");
    case StoreBlockReason::AmbiguousStoreCandidates:
        return msg("Lightning found more than one local encryption store that "
                  "could belong to this account and will not guess between "
                  "them. Sign out of the accounts you no longer use, or "
                  "remove the unused one from Settings, then sign in again. "
                  "Nothing has been deleted.");
    case StoreBlockReason::AccessTokenRevoked:
        return msg("This session was signed out on the server. Sign in again "
                  "to continue. Your local data is intact.");
    case StoreBlockReason::MissingSessionMetadata:
        return msg("Lightning found a local encryption store for this account "
                  "with no sign-in saved alongside it. Sign in again to "
                  "continue.");
    case StoreBlockReason::MissingDeviceId:
        return msg("The saved sign-in for this account is incomplete — it has "
                  "no device. Reset the local Lightning session for this "
                  "account, then sign in again. This does not delete server "
                  "messages or Element data.");
    case StoreBlockReason::ExistingStoreNeedsRestore:
        return msg("This account is already signed in on this device. Switch "
                  "to it from the account menu instead of signing in again.");
    case StoreBlockReason::SecretBackendUnavailable:
        return msg("Lightning cannot read your saved sign-in for this account. "
                  "Your system keyring is locked or unavailable — unlock it "
                  "and try again. Nothing has been deleted, and your local "
                  "encryption store is intact.");
    case StoreBlockReason::InvalidSavedIdentity:
        return msg("Lightning could not read the saved account details for "
                  "this session. Reset the local Lightning session for this "
                  "account, then sign in again. This does not delete server "
                  "messages or Element data.");
    case StoreBlockReason::DifferentAccount:
        return msg("This local Lightning Rust SDK store belongs to a different "
                  "Matrix session or device. Reset the local Lightning "
                  "session for this account, then sign in again. This does "
                  "not delete server messages or Element data.");
    }
    return msg("Lightning could not open the local session for this account.");
}

bool suggestsLocalReset(StoreBlockReason reason)
{
    switch (reason) {
    case StoreBlockReason::DifferentAccount:
    case StoreBlockReason::MissingDeviceId:
    case StoreBlockReason::MissingSessionMetadata:
    case StoreBlockReason::InvalidSavedIdentity:
        return true;
    case StoreBlockReason::None:
    case StoreBlockReason::MissingStoreForSavedSession:
    case StoreBlockReason::AmbiguousStoreCandidates:
    case StoreBlockReason::AccessTokenRevoked:
    case StoreBlockReason::ExistingStoreNeedsRestore:
    // The sign-in may be fine and merely unreadable; deleting the store for a
    // locked keyring destroys room keys for nothing.
    case StoreBlockReason::SecretBackendUnavailable:
        return false;
    }
    return false;
}

bool suggestsLocalResetForCode(const QString &reasonCode)
{
    const QString code = reasonCode.trimmed();
    if (code.isEmpty())
        return false;
    for (const StoreBlockReason reason :
         {StoreBlockReason::None, StoreBlockReason::MissingSessionMetadata,
          StoreBlockReason::MissingDeviceId, StoreBlockReason::DifferentAccount,
          StoreBlockReason::ExistingStoreNeedsRestore,
          StoreBlockReason::MissingStoreForSavedSession,
          StoreBlockReason::AccessTokenRevoked,
          StoreBlockReason::AmbiguousStoreCandidates,
          StoreBlockReason::SecretBackendUnavailable,
          StoreBlockReason::InvalidSavedIdentity}) {
        if (diagnosticName(reason) == code)
            return suggestsLocalReset(reason);
    }
    // Codes emitted outside the enum. `cleanup_incomplete` means a previous
    // repair did not finish, so retrying it is the remedy.
    if (code == QLatin1String("cleanup_incomplete"))
        return true;
    if (code == QLatin1String("sdk_store_ownership_mismatch"))
        return suggestsLocalReset(StoreBlockReason::DifferentAccount);
    return false;
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
