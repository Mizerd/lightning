#pragma once

#include "matrix/MatrixClient.h"
#include "calls/SdpStore.h"
#include "matrix/RecoveryReopenBudget.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix/NotificationProducerState.h"
#include "matrix/RustTimelineIngest.h"
#include "matrix/SessionLifecycleGuard.h"
#include "storage/AppDataPaths.h"

#include <QHash>
#include <QPointer>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>
#include <functional>

class SettingsManager;

namespace lightning::accounts {
class RevocationProofs;
class StoreOwnershipRegistry;
}

class RustSdkMatrixClient final : public MatrixClient
{
    Q_OBJECT
public:
    static constexpr int kStoreCloseBudgetMs = 15000;

    /// Block until every handle retired by releaseRustHandle() has closed, or
    /// the budget expires. Returns true if the pool drained.
    ///
    /// Only for callers about to delete, rename or quarantine the store on
    /// disk, where an open SQLite connection would race, and for process exit.
    /// Account switching must never wait.
    static bool waitForRustRetirement(int budgetMs);

    Q_INVOKABLE void beginQuitRetirement();

    static void retireRustHandleAsync(void *handle, const QString &typingRoom);

    struct RetiringSignIn {
        QString userId;
        QString deviceId;
        QByteArray accessDigest;
        bool persist = false;
    };
    static void retireRustHandleAsync(void *handle, const QString &typingRoom,
                                      const RetiringSignIn &signIn,
                                      QPointer<RustSdkMatrixClient> sink,
                                      quint64 storeOwnershipSerial = 0,
                                      lightning::accounts::StoreOwnershipRegistry *storeRegistry
                                      = nullptr);

    void persistRetiredSessionTokens();

    explicit RustSdkMatrixClient(SettingsManager *settings, QObject *parent = nullptr);
    ~RustSdkMatrixClient() override;

    void setStoreOwnershipRegistry(lightning::accounts::StoreOwnershipRegistry *r);

    void setRevocationProofs(lightning::accounts::RevocationProofs *proofs);

    // Live accounts (Labs) on, called once by AppController. One-way per
    // process: true stays true (false is ignored). It makes the store
    // registry ENFORCE its claims (a store another handle holds is never
    // opened, quarantined, moved aside or deleted from here); makes login()
    // and the browser sign-in consult RevocationProofs for the record's
    // account beside their own single-slot fields; and makes
    // settleRetiringSignIn() notice a background host of that account still
    // handing its sign-in back. Off (the default), every path is Phase 0's.
    void setLiveAccountsLabs(bool on);
    bool liveAccountsLabs() const { return m_liveAccountsLabs; }

    QString rustBackendName() const;
    QString rustBackendStatus() const;
    QString rustBackendVersion() const;
    bool    rustSupportsE2ee() const;

    void setStorePathOverride(const QString &absolutePath);

    // Smoke-only session sidecar used with LIGHTNING_TEST_PERSISTENT_STORE=1.
    // The file contains a Matrix access token. This wrapper never logs it and
    // never exposes it to QML; Rust reads/writes it directly.
    void setPersistentSessionFile(const QString &absolutePath);

    QString rustStorePath() const;
    bool    rustStorePathIsOverride() const;
    QString currentDeviceId() const override;

    // Testing hook for persistent smoke mismatch recovery. Destroys the Rust
    // handle and deletes only the currently selected account's SDK store path.
    // It never touches cache.sqlite, QSettings, or SecretStore entries.
    bool resetRustStore();

    bool resetLocalSession(const matrix::app_data::AccountIdentity &identity,
                           QString *message = nullptr);

    bool restoreSessionFromFile(const QString &homeserver,
                                const QString &userIdForStore);

    // Encrypted-room smoke probe: calls mx_rust_probe_encrypted_send, bypassing
    // the CryptoManager::supportsE2ee gate. Encryption happens in matrix-sdk;
    // C++ never handles ciphertext or keys. Emits
    // encryptedSendProbeResult(...). Do not wire this into QML.
    void probeEncryptedSend(const QString &roomId,
                            const QString &body,
                            const QString &marker);

    // Key-backup recovery for the smoke harness (mx_rust_recover_from_backup).
    // The recovery key never transits a log or QML property. Result flows
    // through keyBackupResult(...). Do not expose to QML.
    void recoverFromBackup(const QString &recoveryKey);

    void reloadRoomTimeline(const QString &roomId, int limit = 30);

    void openRoomTimeline(const QString &roomId);
    bool reloadRoomTimelineAtLive(const QString &roomId);
    void closeRoomTimeline();

    void acceptVerification(const QString &flowId);
    void confirmVerification(const QString &flowId);
    void mismatchVerification(const QString &flowId);
    void cancelVerification(const QString &flowId);
    // The user confirmed that the OTHER device reported a successful scan
    // of the QR code Lightning displayed. Only the SDK performs the trust
    // change; this never promotes trust locally.
    void confirmQrVerification(const QString &flowId);

    void startOwnVerification();

    void requestMissingSecrets();

    void queryCryptoHealth();

    void requestDeviceList();

    void refreshOwnDeviceStatus();

    void checkOwnIdentityKey();

    // Encrypted Megolm room-key import. The path goes to Rust unchanged; the
    // passphrase is forwarded once and never logged. Results flow through the
    // roomKeyImport* signals. One import at a time per client.
    void importRoomKeys(const QString &filePath, const QString &passphrase);
    bool roomKeyImportActive() const;

    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;
    bool adoptLiveSession(const lightning::accounts::LiveSessionTicket &ticket) override;
    bool supportsLiveAdoption() const override { return true; }

    bool supportsOAuthLogin() const override { return true; }
    void discoverAuthMethods(const QString &homeserver) override;
    void beginOAuthLogin(const QString &homeserver) override;
    void beginOAuthSignUp(const QString &homeserver) override;
    void cancelOAuthLogin() override;
    bool supportsSsoLogin() const override { return true; }
    void requestSsoProviders(const QString &homeserver) override;
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override;
    void cancelSsoLogin() override;
    bool isLoggedIn() const override { return m_loggedIn; }
    QString currentUserId() const override { return m_userId; }
    QString homeserverUrl() const override { return m_homeserver; }

    void startSync() override;
    void stopSync() override;
    ConnectionState connectionState() const override { return m_state; }
    bool initialSyncDone() const override { return m_initialSyncDone; }
    QString syncMode() const override { return m_syncMode; }

    QList<RoomInfo> rooms() const override;
    RoomInfo roomInfo(const QString &roomId) const override
    { return m_rooms.value(roomId); }
    QList<TimelineEvent> timeline(const QString &roomId) const override;

    QString displayNameFor(const QString &roomId, const QString &userId) const override;
    QString avatarMxcFor(const QString &roomId, const QString &userId) const override;
    QStringList typingUsersFor(const QString &roomId) const override;

    QUrl mediaDownloadUrl(const QString &mxcUrl) const override;
    QUrl mediaThumbnailUrl(const QString &mxcUrl, int width, int height, bool crop) const override;

    void sendTextMessage(const QString &roomId, const QString &body) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body) override;
    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &mentionUserIds) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body,
                   const QStringList &mentionUserIds) override;
    void editMessage(const QString &roomId,
                     const QString &targetEventId,
                     const QString &newBody) override;
    void editMessage(const QString &roomId,
                     const QString &targetEventId,
                     const QString &newBody,
                     const QStringList &mentionUserIds) override;
    // Formatted sends: the bodySpec crosses the FFI as one JSON argument; Rust
    // re-validates it and strict-sanitizes any HTML.
    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &mentionUserIds,
                         const QVariantMap &bodySpec) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body,
                   const QStringList &mentionUserIds,
                   const QVariantMap &bodySpec) override;
    void editMessage(const QString &roomId,
                     const QString &targetEventId,
                     const QString &newBody,
                     const QStringList &mentionUserIds,
                     const QVariantMap &bodySpec) override;
    void redactEvent(const QString &roomId,
                     const QString &eventId,
                     const QString &reason = QString()) override;
    void toggleReaction(const QString &roomId,
                        const QString &targetEventId,
                        const QString &key) override;

    bool supportsRemovingEdits() const override { return true; }
    void removeMessageEdits(const QString &roomId,
                            const QString &eventId) override;

    bool supportsPolls() const override { return true; }
    bool supportsJoinByAddress() const override { return true; }
    void sendPollResponse(const QString &roomId,
                          const QString &threadRootId,
                          const QString &pollStartEventId,
                          const QStringList &answerIds) override;
    void endPoll(const QString &roomId,
                 const QString &threadRootId,
                 const QString &pollStartEventId) override;
    void createPoll(const QString &roomId,
                    const QString &threadRootId,
                    const QString &question,
                    const QStringList &answers,
                    bool undisclosed,
                    int maxSelections) override;

    void sendTyping(const QString &roomId, bool isTyping, int timeoutMs = 20000) override;
    void sendReadReceipt(const QString &roomId, const QString &eventId) override;
    void setFullyReadMarker(const QString &roomId, const QString &eventId) override;
    void setReadReceiptPrivacy(int mode) override;
    void setThreadRepliesInLiveTimeline(bool inLive) override;
    void setUnthreadedReadReceipts(bool on) override;
    /// MSC4153 "invisible crypto", process-wide. Static because Rust reads it
    /// when a client is built (matrix-sdk has no runtime setter), so it must be
    /// set before sign-in.
    static void setStrictDeviceTrust(bool enabled);
    static void setKeepMediaOnDevice(bool enabled);
    void setRoomMarkedUnread(const QString &roomId, bool unread) override;
    bool supportsRoomFavourites() const override { return true; }
    void setRoomFavourite(const QString &roomId, bool favourite) override;
    void setRoomLowPriority(const QString &roomId, bool lowPriority) override;
    bool supportsMarkRoomRead() const override { return true; }
    void markRoomRead(const QString &roomId) override;
    bool supportsServerNotificationModes() const override { return true; }
    void setRoomNotificationMode(const QString &roomId, int mode) override;
    void clearRoomNotificationMode(const QString &roomId) override;
    void requestThreadParticipants(const QString &roomId,
                                   const QString &rootEventId) override;
    bool supportsPresence() const override { return true; }
    bool tracksRoomReadState() const override { return true; }
    void requestPresence(const QStringList &userIds, quint64 opId) override;
    bool supportsProfileBanners() const override { return true; }
    void fetchProfileBanner(const QString &userId, quint64 opId) override;
    bool supportsNameColors() const override { return true; }
    void fetchNameColor(const QString &userId, quint64 opId) override;
    void setNameColor(const QString &value, quint64 opId) override;
    void setProfileBanner(const QString &localPath, quint64 opId) override;
    bool supportsProfileBios() const override { return true; }
    void fetchProfileBio(const QString &userId, quint64 opId) override;
    void setProfileBio(const QString &text, quint64 opId) override;
    bool supportsStickerPacks() const override { return true; }
    void fetchStickerPacks(const QString &roomId, quint64 opId,
                           bool deep) override;
    void migrateUserStickerPack(quint64 opId, bool startNew) override;
    void uploadStickerToRoomPack(const QString &roomId, const QString &stateKey,
                                 const QString &ns, const QString &shortcode,
                                 const QString &body, const QString &localPath,
                                 const QString &reuseUrl, quint64 opId) override;
    void createStickerRoomPack(const QString &roomId, const QString &stateKey,
                               const QString &name, const QString &usage,
                               quint64 opId) override;
    void sendSticker(const QString &roomId, const QString &rootId,
                     const QString &url, const QString &body,
                     const QString &mimetype, quint64 width, quint64 height,
                     quint64 size) override;
    void addStickerToRoomPack(const QString &roomId, const QString &stateKey,
                              const QString &ns,
                              const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64 width, quint64 height, quint64 size,
                              quint64 opId) override;
    void setStickerRoomPackEnabled(const QString &roomId,
                                   const QString &stateKey, const QString &ns,
                                   bool enabled, quint64 opId) override;
    void uploadStickerToUserPack(const QString &shortcode, const QString &body,
                                 const QString &localPath,
                                 const QString &usage,
                                 quint64 opId) override;
    void addStickerToUserPack(const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64 width, quint64 height, quint64 size,
                              const QString &usage, quint64 opId) override;
    bool supportsPolicyLists() const override { return true; }
    void fetchPolicyRules(const QString &roomId, quint64 opId) override;
    void writePolicyRule(const QString &roomId, const QString &kind,
                         const QString &entity, const QString &stateKey,
                         const QString &recommendation,
                         const QString &reason, quint64 opId) override;
    void setPolicySubscribed(const QString &roomId, bool subscribed,
                             quint64 opId) override;
    void fetchPolicySubscriptions(quint64 opId) override;
    void checkPolicyEntity(const QString &kind, const QString &entity,
                           quint64 opId) override;
    bool supportsQrLogin() const override { return true; }
    quint64 qrLoginGenerate() override;
    quint64 qrLoginScan(const QString &payload) override;
    void qrLoginSubmitCheckCode(quint64 generation, int code) override;
    void qrLoginCancel() override;
    void editStickerPack(const QString &roomId, const QString &stateKey,
                         const QString &ns,
                         const QString &action, const QString &argA,
                         const QString &argB, quint64 opId) override;
    bool supportsRoomBanners() const override { return true; }
    void fetchRoomBanner(const QString &roomId, quint64 opId) override;
    void setRoomBanner(const QString &roomId, const QString &localPath,
                       quint64 opId) override;
    bool supportsRoomBackgrounds() const override { return true; }
    void fetchRoomBackground(const QString &roomId, quint64 opId) override;
    void setRoomBackground(const QString &roomId, const QString &localPath,
                           const QString &contentJson, quint64 opId) override;
    bool supportsSettingsSync() const override { return true; }
    void readSyncedSettings(quint64 opId) override;
    void writeSyncedSettings(int kind, const QString &contentJson,
                             quint64 opId) override;
    bool supportsPersonalBackgroundSync() const override { return true; }
    void readPersonalBackgrounds(const QString &scope, quint64 opId) override;
    void downloadPersonalBackground(const QString &scope, const QString &expectedId,
                                    quint64 opId) override;
    void writePersonalBackground(const QString &scope, int mode,
                                 const QString &localPath,
                                 const QString &requestedJson,
                                 quint64 opId) override;
    void clearAllPersonalBackgrounds(const QStringList &knownRooms,
                                     quint64 opId) override;
    void publishPresence(int state) override;
    void publishPresence(int state, const QString &statusMsg) override;
    void requestRoomNotificationMode(const QString &roomId) override;
    void requestAllRoomNotificationModes() override;
    void acceptInvite(const QString &roomId) override;
    void rejectInvite(const QString &roomId) override;
    void sendImage(const QString &roomId, const QString &localPath) override;
    void sendFile(const QString &roomId, const QString &localPath) override;

    bool supportsEventContext() const override { return true; }
    void openEventContext(const QString &roomId, const QString &eventId) override;
    void closeEventContext() override;
    void paginateEventContext(const QString &roomId, const QString &eventId,
                              bool forward) override;
    bool supportsThreadTimelines() const override { return true; }
    void openThread(const QString &roomId, const QString &rootEventId) override;
    void closeThread() override;
    void sendThreadReply(const QString &roomId,
                         const QString &threadRootEventId,
                         const QString &body) override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body) override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds) override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds,
                           const QVariantMap &bodySpec) override;
    void retryDecryption(const QString &roomId) override;
    bool supportsThreadList() const override { return true; }
    void openThreadList(const QString &roomId) override;
    void closeThreadList() override;
    void paginateThreadList(const QString &roomId) override;
    void markThreadRead(const QString &roomId, const QString &rootEventId,
                        quint64 opId) override;
    void queryThreadSubscription(const QString &roomId,
                                 const QString &rootEventId) override;
    void setThreadSubscribed(const QString &roomId, const QString &rootEventId,
                             bool subscribed) override;

    void loadOlderMessages(const QString &roomId) override;
    bool canPaginate(const QString &roomId) const override;
    bool paginationReady(const QString &roomId) const override
    { return timelineReadyForPagination(roomId); }
    bool paginating(const QString &roomId) const override;
    bool paginationFailed(const QString &roomId) const override;
    bool paginationFailureTransient(const QString &roomId) const override;
    bool lastPaginationFullyFiltered(const QString &roomId) const override;
    bool supportsCancelSend() const override { return true; }
    void cancelSend(const QString &roomId,
                    const QString &transactionId) override;
    void retryFailedSend(const QString &roomId,
                         const QString &transactionId) override;

    bool supportsRoomManagement() const override { return true; }
    bool supportsAttachmentSend() const override { return true; }
    bool supportsMediaBridge() const override { return true; }
    quint64 searchUsers(const QString &query, int limit) override;
    quint64 fetchUserProfile(const QString &userId) override;
    bool supportsOwnProfileEditing() const override { return true; }
    void setOwnDisplayName(const QString &name, quint64 opId) override;
    void setOwnAvatar(const QString &localPath, quint64 opId) override;
    void clearOwnAvatar(quint64 opId) override;
    quint64 fetchMutualRooms(const QString &userId) override;
    bool supportsUrlPreview() const override { return true; }
    quint64 fetchUrlPreview(const QString &url) override;
    quint64 fetchLinkMedia(const QString &url, const QString &linkKey,
                           int expect, int timeoutClass) override;
    bool supportsGifProvider() const override { return true; }
    quint64 gifGet(const QString &url) override;
    quint64 gifDownload(const QString &url) override;
    QVariantList existingDirectRooms(const QString &userId) const override;
    quint64 createDirectChat(const QString &userId) override;
    quint64 createRoom(const QVariantMap &options) override;
    quint64 inviteUsers(const QString &roomId, const QStringList &userIds) override;
    quint64 requestRoomMembers(const QString &roomId) override;
    quint64 requestRoomOwnPermissions(const QString &roomId) override;
    quint64 setRoomName(const QString &roomId, const QString &name) override;
    quint64 setRoomTopic(const QString &roomId, const QString &topic) override;
    quint64 setRoomAvatar(const QString &roomId, const QString &localPath) override;
    quint64 removeRoomAvatar(const QString &roomId) override;
    quint64 leaveRoom(const QString &roomId) override;
    quint64 kickUser(const QString &roomId, const QString &userId,
                     const QString &reason) override;
    quint64 banUser(const QString &roomId, const QString &userId,
                    const QString &reason) override;
    quint64 unbanUser(const QString &roomId, const QString &userId,
                      const QString &reason) override;
    quint64 requestModerationPlan(const QStringList &roomIds,
                                  const QString &userId,
                                  const QString &op) override;
    quint64 requestRoomClosurePlan(const QStringList &roomIds,
                                   const QStringList &parentIds) override;
    quint64 closeRoom(const QString &roomId, const QString &reason, bool leave,
                      const QStringList &unlistFromSpaceIds) override;
    quint64 requestServerAdminStatus() override;
    quint64 adminDeleteRoom(const QString &roomId, bool block) override;
    quint64 setMemberPowerLevel(const QString &roomId, const QString &userId,
                                qlonglong level) override;
    quint64 setRoomPowerLevelKey(const QString &roomId, const QString &key,
                                 qlonglong level) override;
    void probeDelayedEvents() override;
    quint64 scheduleMessage(const QString &roomId, const QString &body,
                            const QVariantMap &bodySpec,
                            const QStringList &mentionUserIds,
                            qint64 delayMs) override;
    quint64 updateScheduledMessage(const QString &delayId,
                                   const QString &action) override;
    quint64 sendRoomMessage(const QString &roomId, const QString &body,
                            const QVariantMap &bodySpec,
                            const QStringList &mentionUserIds,
                            const QString &replyToEventId,
                            const QString &threadRootEventId) override;
    void requestActivitySeed(int limit) override;
    void requestActivitySeedPage(const QString &from, int limit) override;
    void requestEditHistory(const QString &roomId, const QString &eventId) override;
    void requestEventSource(const QString &roomId, const QString &eventId) override;
    quint64 eventAtTimestamp(const QString &roomId, qint64 timestampMs) override;
    quint64 localSearch(const QString &query, const QString &roomId,
                        int limit, int offset) override;
    quint64 clearStoredMedia() override;
    quint64 clearEventCache() override;
    bool reportsCacheCompaction() const override { return true; }
    quint64 requestHomeserverInfo() override;
    quint64 requestAccountDataListing(const QString &roomId) override;
    quint64 requestUserClients(const QString &userId) override;
    QString mediaKeepState() const override { return m_mediaKeepState; }
    quint64 searchIndexStats() override;
    quint64 sweepSearchIndex() override;
    quint64 deepenSearchIndex(const QString &roomId) override;
    void forgetIndexedEvent(const QString &eventId) override;
    void forgetIndexedRoom(const QString &roomId) override;
    bool clearSearchIndex() override;
    quint64 indexAllRooms(bool resumeOnly) override;
    quint64 pauseIndexAll() override;
    quint64 cancelIndexAll() override;
    quint64 requestIndexAllStatus() override;
    void setIndexAllHold(unsigned bits) override;
    bool supportsLocalSearch() const override { return m_rustHandle != nullptr; }
    quint64 writeRoomWidget(const QString &roomId, const QString &widgetId,
                            const QString &contentJson) override;
    quint64 roomWidgets(const QString &roomId, const QString &theme,
                        const QString &language) override;
    bool supportsWidgets() const override { return m_rustHandle != nullptr; }
    quint64 roomBridges(const QString &roomId, bool allowNetwork) override;
    bool supportsRoomBridges() const override { return m_rustHandle != nullptr; }
    quint64 requestMediaHistoryPage(const QString &roomId, int limit,
                                    bool restart) override;
    bool supportsMediaHistory() const override
    { return m_rustHandle != nullptr; }
    quint64 startRoomExport(const QString &roomId, int mode, qint64 limit,
                            qint64 fromMs, qint64 toMs) override;
    void cancelRoomExport(quint64 opId) override;
    bool supportsRoomExportWalk() const override
    { return m_rustHandle != nullptr; }
    quint64 setRoomDisplayName(const QString &roomId,
                               const QString &name) override;
    quint64 setRoomMemberAvatar(const QString &roomId,
                                const QString &mxc) override;
    bool supportsRoomProfiles() const override
    { return m_rustHandle != nullptr; }
    quint64 renameDevice(const QString &deviceId, const QString &name) override;
    quint64 backupAction(const QString &action) override;
    quint64 crossSigningAction(const QString &action, const QString &recoveryKey,
                               bool replaceRecoveryKeyConfirmed) override;
    quint64 requestBackupProgress() override;
    void acknowledgeInterruptedSetup() override;
    void readAccountPref(const QString &prefType) override;
    bool supportsAccountPrefs() const override { return true; }
    void writeAccountPref(const QString &prefType,
                          const QVariantMap &content) override;
    void requestInvitePolicySupport() override;
    void requestRoomVersions() override;
    quint64 upgradeRoom(const QString &roomId, const QString &newVersion) override;
    quint64 setRoomJoinRule(const QString &roomId, const QString &rule,
                            const QStringList &allowedRoomIds) override;
    quint64 setRoomHistoryVisibility(const QString &roomId,
                                     const QString &visibility) override;
    quint64 setRoomGuestAccess(const QString &roomId,
                               const QString &access) override;
    void requestRoomDirectoryVisibility(const QString &roomId) override;
    quint64 setRoomDirectoryVisibility(const QString &roomId,
                                       bool published) override;
    quint64 setRoomAltAliases(const QString &roomId,
                              const QStringList &aliases) override;
    quint64 setRoomJoinRule(const QString &roomId,
                            const QString &rule) override;
    quint64 setRoomCanonicalAlias(const QString &roomId,
                                  const QString &alias) override;
    bool supportsPinnedMessages() const override { return true; }
    quint64 requestPinnedMessages(const QString &roomId,
                                  bool allowRemote) override;
    quint64 setEventPinned(const QString &roomId, const QString &eventId,
                           bool pin) override;
    bool supportsRoomDiscovery() const override { return true; }
    quint64 resolveRoomTarget(const QString &input) override;
    quint64 searchPublicRooms(const QString &query, const QString &server,
                              const QString &since, int limit) override;
    quint64 joinRoomByIdOrAlias(const QString &target,
                                const QStringList &via) override;
    quint64 knockRoom(const QString &target, const QStringList &via,
                      const QString &reason) override;
    quint64 cancelKnock(const QString &roomId) override;
    quint64 requestSpaceChildren(const QString &spaceId) override;
    bool supportsMessageSearch() const override { return true; }
    quint64 searchMessages(const QString &term, const QString &roomId,
                           const QString &nextBatch, int limit,
                           const QVariantMap &filters = {}) override;
    bool supportsIgnoredUsers() const override { return true; }
    bool supportsCallSignaling() const override { return true; }
    bool networkRouteActive() const override { return m_networkRouteActive; }
    quint64 callInvite(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &offerType,
                       const QString &offerSdp, quint64 lifetimeMs,
                       const QString &invitee,
                       const QString &streamMetadataJson) override;
    quint64 callAnswer(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &answerType,
                       const QString &answerSdp,
                       const QString &streamMetadataJson) override;
    quint64 callNegotiate(const QString &roomId, const QString &callId,
                          const QString &partyId, const QString &type,
                          const QString &sdp, quint64 lifetimeMs,
                          const QString &streamMetadataJson) override;
    quint64 callStreamMetadata(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &streamMetadataJson) override;
    quint64 callReject(const QString &roomId, const QString &callId,
                       const QString &partyId) override;
    quint64 callHangup(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &reason) override;
    quint64 callSelectAnswer(const QString &roomId, const QString &callId,
                             const QString &partyId,
                             const QString &selectedPartyId) override;
    quint64 callRtcDecline(const QString &roomId,
                           const QString &notificationEventId) override;
    void setCallMediaCapable(bool capable) override;
    QString takeCallSessionDescription(const QString &eventId) override;
    quint64 callCandidates(const QString &roomId, const QString &callId,
                           const QString &partyId,
                           const QVariantList &candidates) override;
    quint64 requestCallTurnServers() override;
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcSession(const QString &roomId, bool preferServer) override;
    quint64 rtcTransports(const QString &roomId) override;
    quint64 rtcPublishMembership(const QString &roomId,
                                 const QString &focusUrl,
                                 const QString &intent,
                                 bool multiSfu = true) override;
    quint64 rtcRestartDelayedLeave(const QString &delayId) override;
    quint64 rtcRetractMembership(const QString &roomId,
                                 const QString &delayId) override;
    quint64 rtcSendMediaKey(const QString &roomId, const QString &keyBase64,
                            int keyIndex,
                            const QString &targetsJson) override;
    bool supportsSfu() const override { return true; }
    quint64 sfuConnect(const QString &serviceUrl,
                       const QString &roomId) override;
    quint64 sfuConnectMany(const QStringList &focusUrls,
                           const QString &roomId) override;
    void sfuLocalDescriptionFor(quint64 generation, int connectionId,
                                const QString &kind, const QString &target,
                                const QString &sdp) override;
    void sfuLocalCandidateFor(quint64 generation, int connectionId,
                              const QString &target,
                              const QString &candidateInit) override;
    void sfuLocalDescription(const QString &kind, const QString &target,
                             const QString &sdp) override;
    void sfuLocalCandidate(const QString &target,
                           const QString &candidateInit) override;
    void sfuAddTrack(const QString &cid, const QString &name, int kind,
                     int width, int height, bool screenShare,
                     bool encrypted) override;
    void sfuAddTrackFor(quint64 generation, int connectionId,
                        const QString &cid, const QString &name, int kind,
                        int width, int height, bool screenShare,
                        bool encrypted) override;
    void sfuAddShareWithBackupFor(quint64 generation, const QString &cid,
                                  const QString &backupCid,
                                  int width, int height, bool encrypted) override;
    void sfuMuteTrack(const QString &sid, bool muted) override;
    void sfuMuteTrackFor(quint64 generation, int connectionId,
                         const QString &sid, bool muted) override;
    void sfuDisconnect() override;
    quint64 rtcSendCallReaction(const QString &roomId,
                                const QString &membershipEventId,
                                const QString &emoji,
                                const QString &name) override;
    quint64 rtcSetHandRaised(const QString &roomId,
                             const QString &membershipEventId,
                             const QString &reactionEventId,
                             bool raised) override;
    quint64 rtcReadRaisedHands(const QString &roomId) override;
    quint64 rtcNotify(const QString &roomId, const QString &notificationType,
                      const QString &intent, quint64 lifetimeMs,
                      const QString &membershipEventId) override;
    quint64 setUserIgnored(const QString &userId, bool ignored) override;
    quint64 requestIgnoredUsers() override;
    bool supportsPushRules() const override { return true; }
    quint64 requestPushRules() override;
    quint64 writePushRule(const QString &kind, const QString &key,
                          const QString &value) override;
    bool supportsEventReporting() const override { return true; }
    quint64 reportMessage(const QString &roomId, const QString &eventId,
                          const QString &reason) override;
    bool supportsDeviceDeletion() const override { return true; }
    quint64 deleteDevices(const QStringList &deviceIds) override;
    bool uiaSubmitPassword(quint64 uiaId, const QString &password) override;
    void uiaCancel(quint64 uiaId) override;
    quint64 requestOAuthManagementUrl(const QString &deviceId) override;
    bool supportsPasswordChange() const override { return true; }
    quint64 changePassword(const QString &currentPassword,
                           const QString &newPassword,
                           bool logoutDevices) override;
    quint64 probePasswordChange() override;
    bool supportsRoomKeyExport() const override { return true; }
    quint64 exportRoomKeys(const QString &filePath,
                           const QString &passphrase) override;
    quint64 addRoomToSpace(const QString &spaceId, const QString &roomId) override;
    quint64 setSpaceChildOrder(const QString &spaceId, const QStringList &roomIds) override;
    quint64 setSpaceChildSuggested(const QString &spaceId,
                                   const QString &roomId,
                                   bool suggested) override;
    quint64 removeRoomFromSpace(const QString &spaceId,
                                const QString &roomId) override;
    quint64 sendAttachment(const QString &roomId, const QString &localPath,
                           const QString &mime, const QString &caption,
                           int width, int height, bool animated,
                           qint64 durationMs,
                           const QString &replyToEventId) override;
    quint64 sendImageWithThumbnail(const QString &roomId,
                                   const QString &localPath,
                                   const QString &mime,
                                   const QString &caption,
                                   int width, int height,
                                   const QByteArray &thumbnail,
                                   int thumbnailWidth,
                                   int thumbnailHeight,
                                   const QString &replyToEventId) override;
    quint64 sendThreadImageWithThumbnail(const QString &roomId,
                                         const QString &rootEventId,
                                         const QString &localPath,
                                         const QString &mime,
                                         const QString &caption,
                                         int width, int height,
                                         const QByteArray &thumbnail,
                                         int thumbnailWidth,
                                         int thumbnailHeight,
                                         const QString &replyToEventId) override;
    quint64 sendVideo(const QString &roomId, const QString &localPath,
                      const QString &mime, const QString &caption,
                      int width, int height, qint64 durationMs,
                      const QByteArray &thumbnail, int thumbnailWidth,
                      int thumbnailHeight,
                      const QString &replyToEventId) override;
    quint64 sendVoiceMessage(const QString &roomId, const QString &localPath,
                             const QString &mime, qint64 durationMs,
                             const QList<int> &waveform,
                             const QString &replyToEventId) override;
    quint64 sendThreadVoiceMessage(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &localPath,
                                   const QString &mime, qint64 durationMs,
                                   const QList<int> &waveform,
                                   const QString &replyToEventId) override;
    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int width, int height,
                                const QString &replyToEventId) override;
    bool supportsRoomScopedAttachmentSend() const override { return true; }
    quint64 sendAttachmentBytesToRoom(const QString &roomId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime,
                                      int width, int height) override;
    quint64 sendThreadAttachment(const QString &roomId,
                                 const QString &rootEventId,
                                 const QString &localPath, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool animated, qint64 durationMs,
                                 const QString &replyToEventId) override;
    quint64 sendThreadVideo(const QString &roomId, const QString &rootEventId,
                            const QString &localPath, const QString &mime,
                            const QString &caption, int width, int height,
                            qint64 durationMs, const QByteArray &thumbnail,
                            int thumbnailWidth, int thumbnailHeight,
                            const QString &replyToEventId) override;
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootEventId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height,
                                      const QString &replyToEventId) override;
    quint64 fetchMedia(const QString &mediaKey, int kind,
                       int timeoutClass = 0) override;
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override;
    void cancelMediaFetch(quint64 opId) override;
    qint64 maxUploadSize() const override { return m_maxUploadSize; }

Q_SIGNALS:
    void encryptedSendProbeResult(const QString &roomId,
                                  const QString &marker,
                                  bool ok,
                                  const QString &serverEventId,
                                  const QString &message);

    void keyBackupResult(const QString &state, const QString &message);

    void roomTimelineReloaded(const QString &roomId,
                              int totalEvents,
                              int decryptedEvents,
                              int undecryptableEvents);

    void verificationRequestReceived(const QString &flowId,
                                     const QString &otherUserId,
                                     const QString &otherDeviceId,
                                     bool isSelfVerification);
    void verificationReady(const QString &flowId);
    void verificationSasReady(const QString &flowId,
                              const QVariantList &emojis,
                              const QVariantList &decimals);
    void verificationSasConfirmed(const QString &flowId);
    void verificationQrReady(const QString &flowId, int modules,
                             const QByteArray &bits);
    void verificationQrScanned(const QString &flowId);
    void verificationQrConfirmed(const QString &flowId);
    void verificationQrDismissed(const QString &flowId, const QString &reason);
    void verificationDone(const QString &flowId);
    void verificationCancelled(const QString &flowId, const QString &message);
    void verificationFailed(const QString &flowId, const QString &message);

    void verificationRequestStarted(const QString &flowId,
                                    const QString &otherUserId,
                                    bool isSelfVerification);

    // Sanitized E2EE health snapshot (booleans, enum names, public device id
    // only). Any "Verified" label derives from deviceCrossSigned, not generic
    // own-device trust.
    void cryptoHealthUpdated(const QVariantMap &snapshot);
    void cryptoBootstrapEvent(const QString &kind, const QString &state,
                              quint64 count, quint64 inconclusive);
    // Entries carry deviceId, displayName, lastSeenTs, lastSeenIp, isCurrent,
    // hasCryptoIdentity, verified, crossSigned, trusted. Trust labels read
    // `trusted` only: `isCurrent ? crossSigned : verified`, decided in Rust.
    void deviceListUpdated(bool ok, const QVariantList &devices);
    // `deviceCrossSigned` is `is_cross_signed_by_owner()` and is what the
    // current session's trust label uses. Do not use `Device::is_verified()`
    // for that: matrix-sdk marks our own device locally trusted at creation, so
    // it is always true here.
    void ownDeviceStatusUpdated(const QString &deviceId,
                                bool ownIdentityAvailable,
                                bool ownIdentityVerified,
                                bool deviceCrossSigned,
                                bool hasMasterKey,
                                bool hasSelfSigningKey,
                                bool hasUserSigningKey);
    void ownDeviceIdentityKeyChecked(bool established, bool matchesServer);

    void roomKeyImportStarted();
    void roomKeyImportProgress(int imported, int total);
    void roomKeyImportDone(int imported,
                           int total,
                           int affectedRooms,
                           const QStringList &roomIds);
    void roomKeyImportFailed(const QString &category, const QString &message);

    void roomKeysApplied(const QString &roomId, int sessionCount);

    void localSessionResetRequired(const QString &reasonCode,
                                   const QString &userId,
                                   const QString &homeserver);
    // A local session could not be opened, but deleting local data is not the
    // remedy (missing store, revoked token, contested ownership). Separate so
    // the destructive recovery UI is never armed for a condition it cannot fix.
    // The explanation reaches the user through loginFailed().
    void localSessionBlocked(const QString &reasonCode,
                             const QString &userId,
                             const QString &homeserver);
    void localSessionCleanupFinished(bool ok, const QString &message);

private:
    QString tokenOwnerForRunningSession() const;
    bool settleRetiringSignIn(const QString &userId);
    void persistRetiredSessionTokens(const QString &restoringUserId);

    struct PendingSend {
        QString roomId;
        QString localEventId;
    };

    struct PendingProbe {
        QString roomId;
        QString marker;
    };

    void refuseSend(const char *op);
    void refuseUntilTimelineReady(const char *op);
    void setState(ConnectionState state);
    void setInitialSyncDone(bool done);
    void clearLocalState();
    void ensurePollTimer();
    bool ensureRustHandleForUser(const QString &userIdForStore);
    bool ensureRustHandleForIdentity(
        const matrix::app_data::AccountIdentity &identity);
    void applyPerHandleSettings(bool adopted = false);
    void reconcileAdoptedTokens();
    bool ensureRustHandleForStorePath(const QString &storePath,
                                      const QString &slug);
    void applyMediaStoreKey(const QString &slug);
    bool applyNetworkRoute(const QString &slug);
    void refreshMediaKeepState();
    void setMediaKeepState(const QString &state);
    void releaseRustHandle();

    bool ensureOAuthBootstrapHandle(const QString &homeserver);
    void applySignInRoute(void *handle, const QString &homeserver,
                          const QString &userId = QString());
    void startOAuthAttempt(const QString &homeserver, bool createAccount);
    void releaseAuthHandle();
    void drainAuthEvents();
    void completeOAuthLogin(const QString &userId,
                            const QString &deviceId,
                            const QString &clientId,
                            const QString &accessToken,
                            const QString &refreshToken);
    void endOAuthAttempt();
    bool rollBackFailedAttempt(const matrix::app_data::AccountIdentity &store,
                               const matrix::app_data::AccountIdentity &record);
    // A sign-in for an account whose recorded device the server provably
    // ended (m_revokedUserId/m_revokedDeviceId): moves that device's store
    // aside, never deleting it, so the sign-in can start a new device in a
    // fresh store. False when the store could not be moved.
    bool moveRevokedDeviceStoreAside(const matrix::app_data::AccountIdentity &identity);
    QString storeMoveFailureText() const;
    matrix::app_data::QuarantineFailure m_lastStoreMoveFailure
        = matrix::app_data::QuarantineFailure::None;
    QString m_revokedUserId;
    QString m_revokedDeviceId;
    QString m_softLogoutUserId;
    QString m_softLogoutHomeserver;
    QString m_softLogoutDeviceId;
    lightning::accounts::RevocationProofs *m_revocationProofs = nullptr;
    lightning::accounts::StoreOwnershipRegistry *m_storeOwnershipRegistry = nullptr;
    quint64 m_storeOwnershipSerial = 0;
    lightning::accounts::StoreOwnershipRegistry &storeOwnershipRegistry() const;
    bool m_liveAccountsLabs = false;
    QString m_storeClaimRefusalText;
    bool labsHardProofAllowsReplace(const QString &recordUserId,
                                    const QString &recordDeviceId) const;
    bool labsSoftProofAllowsResume(const QString &typedUserId, const QString &typedHomeserver,
                                   const QString &recordUserId,
                                   const QString &recordHomeserver,
                                   const QString &recordDeviceId) const;
    void reportStoreClaimRefused(int verdict, const QString &slug, const char *operation);
    static QString storeInUseByBackgroundText();
    bool storeHeldByBackgroundHost(const QString &storePath) const;
    QString storeClaimRefusalText(const QString &storePath) const;
    QString m_resumeDeviceId;
    static constexpr int kResumedDeviceCheckDelayMs = 15000;
    static constexpr int kResumedDeviceCheckAttempts = 3;
    QString m_resumedCheckUserId;
    QString m_resumedCheckDeviceId;
    int m_resumedCheckAttempts = 0;
    quint64 m_resumedCheckGeneration = 0;
    void armResumedDeviceCheck();
    void scheduleResumedDeviceCheck();
    void disarmResumedDeviceCheck();
    bool resumedDeviceCheckIsFor(const QString &userId,
                                 const QString &deviceId) const;
    QString browserRestoreFailureText(bool rolledBack, const QString &detail) const;

    void completeSsoLogin(const QString &userId,
                          const QString &deviceId,
                          const QString &accessToken,
                          const QString &refreshToken);
    void endSsoAttempt();

    // Phase B, shared by both browser flows. Holds the store-ownership gate (a
    // device the server just issued must never adopt a store belonging to a
    // different device); shared so a fix cannot land on one flow only.
    //
    // `authType` is the persisted routing discriminator ("oauth" or "sso");
    // `clientId` is the OAuth dynamic-registration id, empty for SSO. `restore`
    // performs the SDK-specific restore once the store is open.
    void adoptBrowserSession(
        const QString &homeserver,
        const QString &userId,
        const QString &deviceId,
        const QString &clientId,
        const QString &accessToken,
        const QString &refreshToken,
        const QString &authType,
        const std::function<QString(const matrix::app_data::AccountIdentity &,
                                    const QString &deviceId)> &restore);

    void *m_authHandle = nullptr;
    quint64 m_authHandleGeneration = 0;
    void *m_providersHandle = nullptr;
    void drainProvidersEvents();
    void *m_revokeHandle = nullptr;
    void *takeAuthHandle();
    void revokeRefusedSignIn(void *signInHandle, const QString &homeserver,
                             const char *flow, const QString &clientId,
                             const QString &userId, const QString &deviceId,
                             const QString &accessToken,
                             const QString &refreshToken);
    void drainRevokeEvents();
    void releaseRevokeHandle();
    struct AdoptedSessionCheck {
        QString userId;
        QString deviceId;
        QString homeserver;
    };
    void *m_whoamiHandle = nullptr;
    AdoptedSessionCheck m_adoptedSessionCheck;
    QSet<QString> m_adoptedSessionsChecked;
    bool startAdoptedSessionCheck(const matrix::app_data::AccountIdentity &identity,
                                  const QString &userId, const QString &deviceId,
                                  const QString &accessToken);
    void drainWhoamiEvents();
    void releaseWhoamiHandle();
    void releaseProvidersHandle();
    class OAuthCallbackServer *m_oauthCallback = nullptr;
    QString m_oauthHomeserver;
    bool m_oauthInFlight = false;
    class OAuthCallbackServer *m_ssoCallback = nullptr;
    QString m_ssoHomeserver;
    bool m_ssoInFlight = false;
    QString rustStorePathForUser(const QString &userIdForStore) const;
    void pollRustEvents();
    void handleRustEvent(const QJsonObject &event, quint64 eventGeneration);

    void handleRustEventForTest(const QJsonObject &event)
    {
        if (!m_lifecycle.acceptsActive(m_lifecycle.activeGeneration()))
            m_lifecycle.beginSession();
        handleRustEvent(event, m_lifecycle.activeGeneration());
    }
    friend class OfflineRestoreStateTest;
    friend class ContextHandlersTest;
    friend class BrowserSignInRollbackTest;
    friend class RoomNotificationModeTest;
    friend class RtcBridgePayloadTest;
    friend class SyncMessageRowTest;
    friend class E2eeBridgeTest;
    friend class NotificationProducerTest;

    void finishSignOut(const QString &serverResult, const QString &serverMessage);
    bool clearPersistedAccount(const matrix::app_data::AccountIdentity &identity,
                               bool *matchedRecord = nullptr);
    void requireLocalReset(const QString &reasonCode,
                           const matrix::app_data::AccountIdentity &identity);
    void failWithBlockReason(matrix::rust_session::StoreBlockReason reason,
                             const matrix::app_data::AccountIdentity &identity);
    bool adoptDivergentStoreIfUnambiguous(
        matrix::app_data::AccountIdentity *identity,
        matrix::rust_session::StoreBlockReason *refusal);
    void recordStoreLocation(const matrix::app_data::AccountIdentity &identity);
    void handleRoomsEvent(const QJsonArray &rooms, qint64 *parseMs = nullptr,
                          qint64 *modelMs = nullptr);
    void handleRoomSnapshotEvent(const QJsonArray &rooms);
    void handleRoomListDiff(const QJsonObject &event);
    void handleSpacesEvent(const QJsonArray &spaces);
    RoomInfo roomInfoFromJson(const QJsonObject &obj) const;
    void handleTimelineEvent(const QJsonObject &event);
    void handleEventRedacted(const QJsonObject &event);
    void handleEventDecrypted(const QJsonObject &event);
    void emitDiffNotificationFacts(const QString &roomId,
                                   const QString &threadRootId,
                                   const matrix::rust_timeline::DiffOutcome &outcome);
    void noteNotifiedEvent(const QString &roomId, const TimelineEvent &event);
    void emitDecryptedOnce(const QString &roomId, TimelineEvent event);
    void observeHighlightCounts(const QJsonArray &rooms);
    void flushHighlightGaps();
    void resetNotificationProducerState();
    void handleTimelineReset(const QJsonObject &event);
    void handleTimelineDiff(const QJsonObject &event);
    void reportStaleTimelineDiffs();
    void handleTimelinePagination(const QJsonObject &event);
    void flushTimelineInsertBatch();
    void clearTimelineInsertBatch();
    void handleThreadReset(const QJsonObject &event);
    void handleThreadDiff(const QJsonObject &event);
    void handleThreadPagination(const QJsonObject &event);
    void handleThreadError(const QJsonObject &event);
    void handleThreadClosed(const QJsonObject &event);
    void handleContextReset(const QJsonObject &event);
    void handleContextDiff(const QJsonObject &event);
    void handleContextPagination(const QJsonObject &event);
    void handleContextError(const QJsonObject &event);
    void handleContextClosed(const QJsonObject &event);
    void clearContextTimelineState();
    bool threadTimelineActiveFor(const QString &timelineId) const;
    void clearThreadTimelineState();
    void retireRoomTimelineMirror(const QString &roomId);
    bool admitRecoveryReopen(const QString &timelineId, const char *reason,
                             std::function<bool()> stillOpen,
                             std::function<void()> reopen);
    bool roomTimelineStillOpen(const QString &roomId) const;
    bool threadTimelineStillOpen(const QString &timelineId) const;
    bool contextTimelineStillOpen(const QString &timelineId) const;
    void handleThreadListReset(const QJsonObject &event);
    void handleThreadSubscriptionEvent(const QString &type,
                                       const QJsonObject &event);
    void handleTimelineRetryDecryption(const QJsonObject &event);
    void updateRoomPreviewFrom(const QString &roomId,
                               const QList<TimelineEvent> &newestFirstCandidates);
    bool timelineActiveFor(const QString &roomId) const;
    bool timelineReadyForPagination(const QString &roomId) const;
    quint64 moderateUser(const QString &roomId, const QString &userId,
                         const QString &reason, int op);

    calls::SdpStore m_callSdpStore;
    bool m_callMediaCapable = false;
    quint64 m_sfuConnectOp = 0;
    quint64 m_sfuSessionGeneration = 0;

    quint64 nextOpId() { return ++m_opCounter; }
    bool handleRoomCommandEvent(const QString &type, const QJsonObject &event);
    void handleMediaReady(const QJsonObject &event);
    void handleSendOk(const QJsonObject &event);
    void handleSendFailed(const QJsonObject &event);
    void handleEncryptedSendOk(const QJsonObject &event);
    void handleEncryptedSendFailed(const QJsonObject &event);
    QString nextTxnId();
    bool isRoomEncrypted(const QString &roomId) const;
    TimelineEvent buildOwnEcho(const QString &roomId,
                               const QString &body,
                               TimelineEvent::Type type) const;
    void failPendingSend(const QString &transactionId, const QString &message);

    SettingsManager *m_settings;
    void *m_rustHandle = nullptr;
    QString m_storePath;
    QString m_storePathOverride;
    matrix::app_data::AccountIdentity m_freshLoginIdentity;
    matrix::app_data::AccountIdentity m_freshBrowserRecord;
    matrix::app_data::AccountIdentity m_openingIdentity;
    bool m_loginSentBareUsername = false;
    QString m_sessionFilePath;
    QString m_homeserver;
    QString m_userId;
    QString m_deviceId;
    QByteArray m_persistedAccessDigest;
    bool m_retireAsSignOut = false;
    bool m_quitRetirementBegun = false;
    QElapsedTimer m_quitRetirementClock;
    bool m_loggedIn = false;
    bool m_restoredOffline = false;
    QString m_mediaKeyOutcome;
    bool m_networkRouteActive = false;
    QString m_mediaKeepState;
    bool m_ownIdentityKeyMismatchLogged = false;
    ConnectionState m_state = Disconnected;
    bool m_initialSyncDone = false;
    SessionLifecycleGuard m_lifecycle;
    quint64 m_handleGeneration = 0;
    matrix::app_data::AccountIdentity m_signOutIdentity;
    QString m_signOutDeviceId;
    QTimer m_pollTimer;
    QTimer m_tokenWriteRetry;
    int m_tokenWriteRetryMs = 30000;
    int m_tokenWriteAttempts = 0;
    int m_tokenWriteGiveUpMs = 3600000;
    QElapsedTimer m_tokenWriteFailingFor;
    void retryRotatedTokenWrite();
    void scheduleRotatedTokenWrite();
    friend class RustRetirementTest;
    friend class BrowserSignInEdgeTest;
    friend class SessionCredentialTest;
    QHash<QString, RoomInfo> m_rooms;
    QStringList m_memberListRecency;
    QHash<QString, quint64> m_typingSerial;
    int m_typingExpiryMs = 120 * 1000;
    // The SDK room list's index space, one-for-one, because room-list diffs
    // address it by index. Only `room_list_*` diffs and `room_list_reset` may
    // write it; never `room_snapshot`. See matrix::rust_rooms.
    QStringList m_roomOrder;
    QString m_syncMode = QStringLiteral("stopped");
    QString m_lastSyncState;
    QHash<QString, QString> m_lastReceiptSent;
    int m_readReceiptPrivacy = 0;
    bool m_threadsInLive = false;
    bool m_unthreadedReceipts = false;
    unsigned m_indexAllHold = 0;
    QString m_typingRoom;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    matrix::notification_producer::BoundedIdMap m_forwardedEditIds{512};
    matrix::notification_producer::BoundedIdMap m_undecryptableSeen{4096};
    matrix::notification_producer::HighlightGapTracker m_highlightGaps;
    QTimer m_highlightGapTimer;
    bool m_coalesceTimelineInserts = false;
    QString m_timelineInsertBatchRoom;
    quint64 m_timelineInsertBatchGeneration = 0;
    int m_timelineInsertBatchFirst = -1;
    int m_timelineInsertBatchCount = 0;
    QStringList m_timelineInsertBatchChangedIds;
    QHash<QString, PendingSend> m_pendingSends;
    QHash<QString, PendingProbe> m_pendingProbes;
    quint64 m_txnCounter = 0;

    struct PaginationState {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
        bool failureTransient = false;
        unsigned short batchSize = 0;
        bool lastFullyFiltered = false;
        quint64 lastFilterOffered = 0;
        quint64 lastFilterDropped = 0;
    };
    matrix::rust_timeline::TimelineGenerationTracker m_timelineTracker;
    quint64 m_staleDiffGeneration = 0;
    int m_staleDiffCount = 0;
    matrix::rust_timeline::TimelineGenerationTracker m_threadTracker;
    matrix::rust_timeline::TimelineGenerationTracker m_contextTracker;
    matrix::RecoveryReopenBudget m_recoveryReopens;
    QHash<QString, PaginationState> m_pagination;
    QString m_threadListRoom;
    quint64 m_threadListGeneration = 0;
    QHash<QString, qint64> m_lastDecryptionRetryMs;

    quint64 m_opCounter = 0;
    qint64 m_maxUploadSize = 0;
    bool m_uploadLimitRequested = false;
};
