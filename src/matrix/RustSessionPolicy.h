#pragma once

#include "storage/AppDataPaths.h"

#include <QList>
#include <QString>

namespace matrix::rust_session {

enum class StoreBlockReason {
    None,
    MissingSessionMetadata,
    MissingDeviceId,
    DifferentAccount,
    ExistingStoreNeedsRestore,
    MissingStoreForSavedSession,
    AccessTokenRevoked,
    AccessTokenExpired,
    AmbiguousStoreCandidates,
    SecretBackendUnavailable,
    InvalidSavedIdentity,
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

bool revokedDeviceMayBeReplaced(const QString &recordUserId,
                                const QString &recordDeviceId,
                                const QString &revokedUserId,
                                const QString &revokedDeviceId);

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

bool lostSignInDeviceMayResume(const QString &typedUserId,
                               const QString &typedHomeserver,
                               const QString &recordUserId,
                               const QString &recordHomeserver,
                               const QString &recordDeviceId,
                               bool recordSignedInThroughBrowser);

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

QString passwordLoginIdentifier(const QString &typedUser,
                                const app_data::AccountIdentity &typed);

QString savedAccountForBareUsername(const QString &typedUser,
                                    const QString &homeserver,
                                    const QList<app_data::AccountIdentity> &saved,
                                    bool *ambiguous = nullptr);

QString typedUserIdOnSavedServer(const QString &typedUser, const QString &savedUserId);

bool signInAnswerCollidesWithSavedStore(const QString &answerUserId,
                                        const QString &openedStorePath,
                                        const QString &openedStoreOwner,
                                        bool answerHasRecord,
                                        const QString &recordStorePath);

QString diagnosticName(StoreBlockReason reason);

bool suggestsLocalResetForCode(const QString &reasonCode);

QString userMessage(StoreBlockReason reason);

bool suggestsLocalReset(StoreBlockReason reason);

bool isStoreOwnershipMismatch(const QString &message);
bool isUnknownToken(const QString &message);

}
