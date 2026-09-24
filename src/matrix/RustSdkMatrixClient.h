#pragma once

#include "matrix/MatrixClient.h"
#include "calls/SdpStore.h"
#include "matrix/RustSessionPolicy.h"
#include "matrix/RustTimelineIngest.h"
#include "matrix/SessionLifecycleGuard.h"
#include "storage/AppDataPaths.h"

#include <QHash>
#include <QPair>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <functional>

class SettingsManager;

// QObject/C++ wrapper for the Matrix Rust SDK bridge. QML and models still
// talk only to MatrixClient; Rust owns the SDK client, async work, and SDK
// SQLite store behind the C ABI.
class RustSdkMatrixClient final : public MatrixClient
{
    Q_OBJECT
public:
    /// How long a caller about to touch the store on disk waits for the
    /// retiring client to close it. Only ever reached on deletion,
    /// quarantine and process exit — never on an account switch.
    static constexpr int kStoreCloseBudgetMs = 15000;

    /// Block until every handle retired by releaseRustHandle() has closed, or
    /// the budget expires. Returns true if the pool drained.
    ///
    /// Only for callers about to delete, rename or quarantine the store on
    /// disk, where an open SQLite connection would race, and for process exit.
    /// Account switching must never wait.
    static bool waitForRustRetirement(int budgetMs);

    /// Runs the blocking half of releaseRustHandle() (joining managed tasks,
    /// dropping the tokio runtime) on a worker thread that takes ownership of
    /// `handle`. Public so a test can verify the caller returns immediately.
    static void retireRustHandleAsync(void *handle, const QString &typingRoom);

    explicit RustSdkMatrixClient(SettingsManager *settings, QObject *parent = nullptr);
    ~RustSdkMatrixClient() override;

    // Introspection surfaced for the Settings screen / logs.
    QString rustBackendName() const;
    QString rustBackendStatus() const;
    QString rustBackendVersion() const;
    bool    rustSupportsE2ee() const;

    // Testing hook: when non-empty, the SDK store is created at exactly this
    // absolute path instead of the per-account layout, so the headless smoke
    // harness starts each login from a clean crypto store. Call before login()
    // / restoreSession().
    void setStorePathOverride(const QString &absolutePath);

    // Smoke-only session sidecar used with LIGHTNING_TEST_PERSISTENT_STORE=1.
    // The file contains a Matrix access token. This wrapper never logs it and
    // never exposes it to QML; Rust reads/writes it directly.
    void setPersistentSessionFile(const QString &absolutePath);

    // Introspection for the smoke harness. Populated after ensure has
    // resolved a store path; empty otherwise.
    QString rustStorePath() const;
    bool    rustStorePathIsOverride() const;
    QString currentDeviceId() const;

    // Testing hook for persistent smoke mismatch recovery. Destroys the Rust
    // handle and deletes only the currently selected account's SDK store path.
    // It never touches cache.sqlite, QSettings, or SecretStore entries.
    bool resetRustStore();

    // Signed-out, account-scoped reset used by AppController. Any lingering
    // handle for this account is stopped and released before deletion. The
    // result includes SecretStore/QSettings cleanup as well as Rust files.
    bool resetLocalSession(const matrix::app_data::AccountIdentity &identity,
                           QString *message = nullptr);

    // Smoke-only restore path. Uses the configured persistent session sidecar,
    // not QSettings/SecretStore.
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

    // Smoke-test helper: reload a room's recent timeline via Room::messages
    // (events dedupe by event_id). Emits roomTimelineReloaded(...) or
    // errorOccurred. Not for UI room opens; use openRoomTimeline(), which
    // updates visible rows in place.
    void reloadRoomTimeline(const QString &roomId, int limit = 30);

    // Open (or re-open) the live matrix-sdk-ui timeline for a room. Rust
    // cancels the previous subscription, sends one timeline_reset snapshot,
    // then streams VectorDiff updates applied in place (late decryptions, local
    // echoes with send states, pagination prepends). Each call advances the
    // Rust room generation, so stale callbacks are rejected.
    void openRoomTimeline(const QString &roomId);
    // Re-open the live timeline after the SDK event cache has released the
    // paginated backlog; explicit jump-to-latest from far back only (see
    // mx_rust_timeline_reload_at_live). Returns false when dispatch failed, so
    // the caller never waits for a reset that will not arrive.
    bool reloadRoomTimelineAtLive(const QString &roomId);
    void closeRoomTimeline();

    // SAS emoji verification (receive-first) via mx_rust_accept_verification /
    // confirm / mismatch / cancel. Results flow through the verification*
    // signals. One active flow at a time; a stale flow id yields a non-secret
    // error via errorOccurred.
    void acceptVerification(const QString &flowId);
    void confirmVerification(const QString &flowId);
    void mismatchVerification(const QString &flowId);
    void cancelVerification(const QString &flowId);
    // The user confirmed that the OTHER device reported a successful scan
    // of the QR code Lightning displayed. Only the SDK performs the trust
    // change; this never promotes trust locally.
    void confirmQrVerification(const QString &flowId);

    // Lightning-initiated SAS verification of this session against another
    // session of the same account. Emits verificationStartFailed(reason)
    // synchronously when it cannot be dispatched; success flows through
    // verificationRequestStarted / verificationSasReady / verificationDone.
    // Advertises SAS only.
    void startOwnVerification();

    // "Request keys again": ask the Rust recovery coordinator for a fresh
    // encryption-secret request round. Progress arrives as
    // cryptoBootstrapEvent; dispatch failures via errorOccurred.
    void requestMissingSecrets();

    // One sanitized E2EE health snapshot (device trust, cross-signing keys,
    // backup/recovery/secret-storage state) via cryptoHealthUpdated(map).
    // Skipped when not logged in.
    void queryCryptoHealth();

    // The account's device list (server metadata plus SDK crypto trust) via
    // deviceListUpdated(ok, devices).
    void requestDeviceList();

    // Snapshot this session's cross-signing trust state via
    // ownDeviceStatusUpdated(...), aggregate fields only. Skipped when not
    // logged in.
    void refreshOwnDeviceStatus();

    // Check whether the curve25519 identity key this device publishes matches
    // its local Olm account. If not, peers encrypt to a key we cannot read:
    // nothing decrypts while sending still works. Answers through
    // ownDeviceIdentityKeyChecked(...). Asynchronous (needs /keys/query); one
    // check per call, rate-limited by matrix::crypto::OwnDeviceKeyWatch.
    void checkOwnIdentityKey();

    // Encrypted Megolm room-key import. The path goes to Rust unchanged; the
    // passphrase is forwarded once and never logged. Results flow through the
    // roomKeyImport* signals. One import at a time per client.
    void importRoomKeys(const QString &filePath, const QString &passphrase);
    bool roomKeyImportActive() const;

    // MatrixClient interface -------------------------------------------------
    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;

    // OAuth 2.0 / OIDC. Phase A authenticates on a store-less bootstrap handle;
    // phase B opens the account store once the homeserver has named the account
    // and device (see rust/src/oauth.rs).
    bool supportsOAuthLogin() const override { return true; }
    void discoverAuthMethods(const QString &homeserver) override;
    void beginOAuthLogin(const QString &homeserver) override;
    void cancelOAuthLogin() override;
    // Legacy Matrix SSO. A separate flow from OAuth (see rust/src/sso.rs) that
    // shares the loopback listener and the two-phase store lifecycle.
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
    // Outgoing @-mentions: attach m.mentions through the SDK send path.
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

    // Redact this message's own m.replace events (rooms::remove_message_edits);
    // needs event relations, which only the SDK exposes.
    bool supportsRemovingEdits() const override { return true; }
    void removeMessageEdits(const QString &roomId,
                            const QString &eventId) override;

    // MSC3381 polls through the SDK timeline (room or thread target).
    bool supportsPolls() const override { return true; }
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
    void setReadReceiptPrivacy(int mode) override;
    /// MSC4153 "invisible crypto", process-wide. Static because Rust reads it
    /// when a client is built (matrix-sdk has no runtime setter), so it must be
    /// set before sign-in.
    static void setStrictDeviceTrust(bool enabled);
    void setRoomMarkedUnread(const QString &roomId, bool unread) override;
    bool supportsRoomFavourites() const override { return true; }
    void setRoomFavourite(const QString &roomId, bool favourite) override;
    bool supportsMarkRoomRead() const override { return true; }
    void markRoomRead(const QString &roomId) override;
    // Server-synchronized per-room notification modes through the SDK's
    // NotificationSettings push-rule manager (label-faithful 0/1/2).
    bool supportsServerNotificationModes() const override { return true; }
    void setRoomNotificationMode(const QString &roomId, int mode) override;
    void clearRoomNotificationMode(const QString &roomId) override;
    void requestThreadParticipants(const QString &roomId,
                                   const QString &rootEventId) override;
    // Presence: bounded polling (Sliding Sync carries no presence events) plus
    // own-state publication. Policy lives in PresenceManager.
    bool supportsPresence() const override { return true; }
    // The SDK derives unread counts from the user's own read receipt from any
    // device, and roomInfoFromJson writes all four fields on every payload.
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
    void fetchStickerPacks(const QString &roomId, quint64 opId) override;
    void sendSticker(const QString &roomId, const QString &rootId,
                     const QString &url, const QString &body,
                     const QString &mimetype, quint64 width, quint64 height,
                     quint64 size) override;
    void addStickerToRoomPack(const QString &roomId, const QString &stateKey,
                              const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64 width, quint64 height, quint64 size,
                              quint64 opId) override;
    void setStickerRoomPackEnabled(const QString &roomId,
                                   const QString &stateKey, bool enabled,
                                   quint64 opId) override;
    void uploadStickerToUserPack(const QString &shortcode, const QString &body,
                                 const QString &localPath,
                                 quint64 opId) override;
    void addStickerToUserPack(const QString &shortcode, const QString &url,
                              const QString &body, const QString &mimetype,
                              quint64 width, quint64 height, quint64 size,
                              quint64 opId) override;
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
                         const QString &action, const QString &argA,
                         const QString &argB, quint64 opId) override;
    bool supportsRoomBanners() const override { return true; }
    void fetchRoomBanner(const QString &roomId, quint64 opId) override;
    void setRoomBanner(const QString &roomId, const QString &localPath,
                       quint64 opId) override;
    void publishPresence(int state) override;
    void publishPresence(int state, const QString &statusMsg) override;
    void requestRoomNotificationMode(const QString &roomId) override;
    void acceptInvite(const QString &roomId) override;
    void rejectInvite(const QString &roomId) override;
    void sendImage(const QString &roomId, const QString &localPath) override;
    void sendFile(const QString &roomId, const QString &localPath) override;

    // SDK-backed thread timelines (TimelineFocus::Thread). A thread uses the
    // same diff signals under its composite id; the pagination methods below
    // handle both id kinds.
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
    void markThreadRead(const QString &roomId,
                        const QString &rootEventId) override;
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

    // Conversation creation, membership, room editing, media.
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
    bool supportsGifProvider() const override { return true; }
    quint64 gifGet(const QString &url) override;
    quint64 gifDownload(const QString &url) override;
    QVariantList existingDirectRooms(const QString &userId) const override;
    quint64 createDirectChat(const QString &userId) override;
    quint64 createRoom(const QVariantMap &options) override;
    quint64 inviteUsers(const QString &roomId, const QStringList &userIds) override;
    quint64 requestRoomMembers(const QString &roomId) override;
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
    // Room administration and pinned messages.
    quint64 setMemberPowerLevel(const QString &roomId, const QString &userId,
                                qlonglong level) override;
    quint64 setRoomPowerLevelKey(const QString &roomId, const QString &key,
                                 qlonglong level) override;
    // Scheduled send.
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
    // Message edit history and event source.
    void requestEditHistory(const QString &roomId, const QString &eventId) override;
    void requestEventSource(const QString &roomId, const QString &eventId) override;
    quint64 eventAtTimestamp(const QString &roomId, qint64 timestampMs) override;
    quint64 localSearch(const QString &query, const QString &roomId,
                        int limit, int offset) override;
    quint64 searchIndexStats() override;
    quint64 sweepSearchIndex() override;
    quint64 deepenSearchIndex(const QString &roomId) override;
    void forgetIndexedEvent(const QString &eventId) override;
    void forgetIndexedRoom(const QString &roomId) override;
    void clearSearchIndex() override;
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
    quint64 setRoomDisplayName(const QString &roomId,
                               const QString &name) override;
    quint64 setRoomMemberAvatar(const QString &roomId,
                                const QString &mxc) override;
    bool supportsRoomProfiles() const override
    { return m_rustHandle != nullptr; }
    // Device and backup management.
    quint64 renameDevice(const QString &deviceId, const QString &name) override;
    quint64 backupAction(const QString &action) override;
    void requestBackupProgress() override;
    // Room upgrade.
    void requestRoomVersions() override;
    quint64 upgradeRoom(const QString &roomId, const QString &newVersion) override;
    // Room access.
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
    quint64 callInvite(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &offerType,
                       const QString &offerSdp, quint64 lifetimeMs,
                       const QString &invitee) override;
    quint64 callAnswer(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &answerType,
                       const QString &answerSdp) override;
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
    // MatrixRTC (MSC4143).
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcSession(const QString &roomId, bool preferServer) override;
    quint64 rtcTransports(const QString &roomId) override;
    quint64 rtcPublishMembership(const QString &roomId,
                                 const QString &focusUrl,
                                 const QString &intent) override;
    quint64 rtcRestartDelayedLeave(const QString &delayId) override;
    quint64 rtcRetractMembership(const QString &roomId,
                                 const QString &delayId) override;
    quint64 rtcSendMediaKey(const QString &roomId, const QString &keyBase64,
                            int keyIndex,
                            const QString &targetsJson) override;
    bool supportsSfu() const override { return true; }
    quint64 sfuConnect(const QString &serviceUrl,
                       const QString &roomId) override;
    void sfuLocalDescription(const QString &kind, const QString &target,
                             const QString &sdp) override;
    void sfuLocalCandidate(const QString &target,
                           const QString &candidateInit) override;
    void sfuAddTrack(const QString &cid, const QString &name, int kind,
                     int width, int height, bool screenShare,
                     bool encrypted) override;
    void sfuMuteTrack(const QString &sid, bool muted) override;
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
    bool supportsEventReporting() const override { return true; }
    quint64 reportMessage(const QString &roomId, const QString &eventId,
                          const QString &reason) override;
    bool supportsDeviceDeletion() const override { return true; }
    quint64 deleteDevices(const QStringList &deviceIds) override;
    bool uiaSubmitPassword(quint64 uiaId, const QString &password) override;
    void uiaCancel(quint64 uiaId) override;
    quint64 requestOAuthManagementUrl(const QString &deviceId) override;
    quint64 addRoomToSpace(const QString &spaceId, const QString &roomId) override;
    quint64 setSpaceChildSuggested(const QString &spaceId,
                                   const QString &roomId,
                                   bool suggested) override;
    quint64 removeRoomFromSpace(const QString &spaceId,
                                const QString &roomId) override;
    quint64 sendAttachment(const QString &roomId, const QString &localPath,
                           const QString &mime, const QString &caption,
                           int width, int height, bool animated,
                           qint64 durationMs = 0) override;
    quint64 sendVideo(const QString &roomId, const QString &localPath,
                      const QString &mime, const QString &caption,
                      int width, int height, qint64 durationMs,
                      const QByteArray &thumbnail, int thumbnailWidth,
                      int thumbnailHeight) override;
    quint64 sendVoiceMessage(const QString &roomId, const QString &localPath,
                             const QString &mime, qint64 durationMs,
                             const QList<int> &waveform) override;
    quint64 sendThreadVoiceMessage(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &localPath,
                                   const QString &mime, qint64 durationMs,
                                   const QList<int> &waveform) override;
    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int width, int height) override;
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
                                 bool animated, qint64 durationMs = 0) override;
    quint64 sendThreadVideo(const QString &roomId, const QString &rootEventId,
                            const QString &localPath, const QString &mime,
                            const QString &caption, int width, int height,
                            qint64 durationMs, const QByteArray &thumbnail,
                            int thumbnailWidth, int thumbnailHeight) override;
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootEventId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height) override;
    quint64 fetchMedia(const QString &mediaKey, int kind,
                       int timeoutClass = 0) override;
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override;
    void cancelMediaFetch(quint64 opId) override;
    qint64 maxUploadSize() const override { return m_maxUploadSize; }

Q_SIGNALS:
    // Fires once per probeEncryptedSend call.
    // `ok`: matrix-sdk returned a real server event id.
    // `marker`: caller-supplied opaque id (never the probe body); safe to log.
    // `serverEventId`: meaningful only when ok.
    // `message`: non-secret failure detail when not ok; empty on success.
    void encryptedSendProbeResult(const QString &roomId,
                                  const QString &marker,
                                  bool ok,
                                  const QString &serverEventId,
                                  const QString &message);

    // Fires once per recoverFromBackup call. `state` is "attempted", then "ok"
    // or "failed". `message` is non-secret failure detail or empty; never the
    // recovery key or imported key material.
    void keyBackupResult(const QString &state, const QString &message);

    // Fires once per reloadRoomTimeline() call.
    void roomTimelineReloaded(const QString &roomId,
                              int totalEvents,
                              int decryptedEvents,
                              int undecryptableEvents);

    // SAS emoji verification lifecycle.
    void verificationRequestReceived(const QString &flowId,
                                     const QString &otherUserId,
                                     const QString &otherDeviceId,
                                     bool isSelfVerification);
    // Both sides exchanged m.key.verification.ready and the SDK is running the
    // start/accept/key exchange; lets the UI show progress instead of an
    // unanswered request. Flow id only; no key, MAC or SAS material.
    void verificationReady(const QString &flowId);
    void verificationSasReady(const QString &flowId,
                              const QVariantList &emojis,
                              const QVariantList &decimals);
    // Our "They match" was registered (SasState::Confirmed); waiting for the
    // other session before verificationDone. At most once per flow; flow id
    // only, never emoji values.
    void verificationSasConfirmed(const QString &flowId);
    // Show-QR verification. Lightning displays a code for the other device to
    // scan; it never scans (no camera), so m.qr_code.scan.v1 is not advertised.
    // `modules` is the width in modules and `bits` a row-major bitmap (MSB
    // first, each row byte-aligned, stride = (modules + 7) / 8, set bit =
    // dark). Only geometry crosses: the encoded payload is cross-signing key
    // material and the shared secret, and it never leaves Rust.
    void verificationQrReady(const QString &flowId, int modules,
                             const QByteArray &bits);
    // The peer scanned our code. The user must now confirm that the other
    // device reported success; nothing is auto-confirmed.
    void verificationQrScanned(const QString &flowId);
    // Our confirmation reached the SDK; it is finishing the exchange.
    void verificationQrConfirmed(const QString &flowId);
    // The code is no longer usable and the flow continues on SAS.
    // `reason` is a sanitized category, never free-form SDK text:
    // "peer_started_sas" or "not_scanned".
    void verificationQrDismissed(const QString &flowId, const QString &reason);
    void verificationDone(const QString &flowId);
    void verificationCancelled(const QString &flowId, const QString &message);
    void verificationFailed(const QString &flowId, const QString &message);

    // A SAS request was dispatched through mx_rust_start_own_verification. The
    // flow id and self-verification flag are safe metadata.
    void verificationRequestStarted(const QString &flowId,
                                    const QString &otherUserId,
                                    bool isSelfVerification);

    // Sanitized E2EE health snapshot (booleans, enum names, public device id
    // only). Any "Verified" label derives from deviceCrossSigned, not generic
    // own-device trust.
    void cryptoHealthUpdated(const QVariantMap &snapshot);
    // Sanitized verified-session bootstrap events (state names and key counts
    // only; never key material or session ids). `inconclusive` counts attempts
    // that taught nothing (unreachable, rate-limited); zero for kinds without
    // that notion.
    void cryptoBootstrapEvent(const QString &kind, const QString &state,
                              quint64 count, quint64 inconclusive);
    // Entries carry deviceId, displayName, lastSeenTs, lastSeenIp, isCurrent,
    // hasCryptoIdentity, verified, crossSigned.
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
    // Kept separate from the verification signal above: this reports a device
    // that can never decrypt anything.
    //   established == false          -> could not be answered (offline,
    //                                    keys not uploaded yet). Not a fault.
    //   established && !matchesServer -> the fault.
    // Two booleans so "unknown" cannot be misread as "broken".
    void ownDeviceIdentityKeyChecked(bool established, bool matchesServer);

    // Encrypted room-key import lifecycle. Aggregate counts and affected room
    // ids only; the decrypted export never leaves Rust.
    void roomKeyImportStarted();
    void roomKeyImportProgress(int imported, int total);
    void roomKeyImportDone(int imported,
                           int total,
                           int affectedRooms,
                           const QStringList &roomIds);
    void roomKeyImportFailed(const QString &category, const QString &message);

    // A room-key import triggered an immediate decryption retry on the open
    // timeline. Counts only; session ids stay in Rust.
    void roomKeysApplied(const QString &roomId, int sessionCount);

    // Structured lifecycle state consumed by AppController/QML.
    //
    // `userId`/`homeserver` identify the account whose session failed to open,
    // which may differ from the one settings point at (e.g. a failed
    // add-account attempt), so the UI can repair the right account. Either may
    // be empty when no usable identity could be resolved.
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
    struct PendingSend {
        QString roomId;
        QString localEventId;
    };

    struct PendingProbe {
        QString roomId;
        QString marker;
    };

    /// A feature this backend genuinely does not have (sendImage, sendFile).
    void refuseSend(const char *op);
    /// The room has no live SDK timeline yet, which is transient.
    void refuseUntilTimelineReady(const char *op);
    void setState(ConnectionState state);
    void setInitialSyncDone(bool done);
    void clearLocalState();
    void ensurePollTimer();
    bool ensureRustHandleForUser(const QString &userIdForStore);
    // Preferred entry point: opens the account's recorded store directory.
    bool ensureRustHandleForIdentity(
        const matrix::app_data::AccountIdentity &identity);
    bool ensureRustHandleForStorePath(const QString &storePath,
                                      const QString &slug);
    void releaseRustHandle();


    // Phase A: a handle with no persistent store, used only for auth discovery
    // and the browser sign-in. Separate from m_rustHandle so typing a
    // homeserver while adding an account does not tear down the signed-in
    // session. It has no store and so no generation; m_oauthInFlight is the
    // guard.
    bool ensureOAuthBootstrapHandle();
    void releaseAuthHandle();
    // Drains the bootstrap handle's queue on the same poll timer but
    // independently, since a sign-in may have no session handle at all.
    void drainAuthEvents();
    // Phase B: the homeserver has named the canonical identity, so the account
    // store can be chosen, gated and opened.
    void completeOAuthLogin(const QString &userId,
                            const QString &deviceId,
                            const QString &clientId,
                            const QString &accessToken,
                            const QString &refreshToken);
    // Tear down the callback listener and any in-flight authorization.
    void endOAuthAttempt();

    // Legacy SSO. completeSsoLogin() and completeOAuthLogin() both delegate
    // the account-store decision to adoptBrowserSession() below.
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

    // Phase A handle. Never carries a session and never syncs.
    void *m_authHandle = nullptr;
    class OAuthCallbackServer *m_oauthCallback = nullptr;
    // The homeserver this OAuth attempt targets; phase B derives the identity
    // from the server's answer plus this URL.
    QString m_oauthHomeserver;
    // True between beginOAuthLogin() and a terminal outcome. Guards against a
    // second attempt racing the first and a late callback completing a
    // cancelled sign-in.
    bool m_oauthInFlight = false;
    class OAuthCallbackServer *m_ssoCallback = nullptr;
    QString m_ssoHomeserver;
    // Same guard for SSO, kept separate so a stale callback from one flow can
    // never complete the other.
    bool m_ssoInFlight = false;
    QString rustStorePathForUser(const QString &userIdForStore) const;
    void pollRustEvents();
    void handleRustEvent(const QJsonObject &event, quint64 eventGeneration);

    /// Feed one event through the dispatcher `pollRustEvents` uses, at the live
    /// generation, so tests can drive the real handler with real payloads
    /// without a live FFI handle.
    ///
    /// Private with one friend rather than a plain `…ForTest` setter: it
    /// dispatches arbitrary payloads that can reach `saveSession` and
    /// `updateSessionTokens`, and begins a lifecycle generation even on a
    /// client mid-shutdown. Nothing in src/ calls it and QML cannot.
    void handleRustEventForTest(const QJsonObject &event)
    {
        // Start a session first or the generation gate drops everything
        // (`acceptsActive(0)` is false).
        if (!m_lifecycle.acceptsActive(m_lifecycle.activeGeneration()))
            m_lifecycle.beginSession();
        handleRustEvent(event, m_lifecycle.activeGeneration());
    }
    friend class OfflineRestoreStateTest;
    friend class RtcBridgePayloadTest;
    friend class SyncMessageRowTest;

    void finishSignOut(const QString &serverResult, const QString &serverMessage);
    bool clearPersistedAccount(const matrix::app_data::AccountIdentity &identity,
                               bool *matchedRecord = nullptr);
    void requireLocalReset(const QString &reasonCode,
                           const matrix::app_data::AccountIdentity &identity);
    // Report a blocked open with its own accurate message.
    void failWithBlockReason(matrix::rust_session::StoreBlockReason reason,
                             const matrix::app_data::AccountIdentity &identity);
    // Adopt a store an older build wrote under the typed localpart casing.
    // Adoption records the directory and rebinds `identity`; nothing on disk is
    // moved or deleted, and the SDK's ownership check remains authoritative.
    // Refuses whenever ownership is contested.
    bool adoptDivergentStoreIfUnambiguous(
        matrix::app_data::AccountIdentity *identity,
        matrix::rust_session::StoreBlockReason *refusal);
    // Persist where this session's store really is, from the directory actually
    // opened.
    void recordStoreLocation(const matrix::app_data::AccountIdentity &identity);
    // The index base: `room_list_reset` (and the legacy `rooms` envelope), the
    // only producer allowed to define m_roomOrder's indices. See
    // matrix::rust_rooms.
    void handleRoomsEvent(const QJsonArray &rooms);
    // `room_snapshot`: the SDK state-store walk. Updates m_rooms; never
    // touches m_roomOrder.
    void handleRoomSnapshotEvent(const QJsonArray &rooms);
    void handleRoomListDiff(const QJsonObject &event);
    void handleSpacesEvent(const QJsonArray &spaces);
    RoomInfo roomInfoFromJson(const QJsonObject &obj) const;
    void handleTimelineEvent(const QJsonObject &event);
    // Live-timeline event handlers.
    void handleTimelineReset(const QJsonObject &event);
    void handleTimelineDiff(const QJsonObject &event);
    /// Report and reset the stale-diff run, if any. A superseded generation
    /// keeps delivering until its subscription stops, so this is counted, not
    /// logged per diff.
    void reportStaleTimelineDiffs();
    void handleTimelinePagination(const QJsonObject &event);
    void flushTimelineInsertBatch();
    void clearTimelineInsertBatch();
    // Thread-timeline handlers (same envelopes, addressed by the composite
    // thread timeline id).
    void handleThreadReset(const QJsonObject &event);
    void handleThreadDiff(const QJsonObject &event);
    void handleThreadPagination(const QJsonObject &event);
    void handleThreadError(const QJsonObject &event);
    void handleThreadClosed(const QJsonObject &event);
    bool threadTimelineActiveFor(const QString &timelineId) const;
    void clearThreadTimelineState();
    /// Retire the C++ mirror of a room whose live SDK timeline is gone (the
    /// room analogue of clearThreadTimelineState()). Trims it back to
    /// kBackgroundMirrorCap, the bound unopened rooms live under, and drops the
    /// pagination state. A no-op for the room the tracker currently wants, so a
    /// re-open keeps its rows until its reset replaces them.
    void retireRoomTimelineMirror(const QString &roomId);
    void handleThreadListReset(const QJsonObject &event);
    void handleThreadSubscriptionEvent(const QString &type,
                                       const QJsonObject &event);
    void handleTimelineRetryDecryption(const QJsonObject &event);
    void updateRoomPreviewFrom(const QString &roomId,
                               const QList<TimelineEvent> &newestFirstCandidates);
    bool timelineActiveFor(const QString &roomId) const;
    bool timelineReadyForPagination(const QString &roomId) const;
    // Shared implementation behind kickUser()/banUser()/unbanUser();
    // `op` mirrors the FFI encoding (0 = kick, 1 = ban, 2 = unban).
    quint64 moderateUser(const QString &roomId, const QString &userId,
                         const QString &reason, int op);

    // nextOpId() never returns 0 (0 means "unsupported" at the interface).
    // Media-capable SDP store (see MatrixClient::setCallMediaCapable and
    // calls::SdpStore): populated only in media-capable mode, wiped with the
    // session.
    calls::SdpStore m_callSdpStore;
    bool m_callMediaCapable = false;

    quint64 nextOpId() { return ++m_opCounter; }
    // Dispatch one Rust "command result" event to the matching signal.
    // Returns true when the event type was consumed.
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
    // Identity of an in-flight password login whose store did not exist
    // before the attempt; a failure removes that fresh store so it cannot
    // poison later logins. Cleared on login_ok.
    matrix::app_data::AccountIdentity m_freshLoginIdentity;
    // The account whose store this client is currently opening. Set by
    // login()/restoreSession() before the handle exists, so a failure can name
    // the right account even when m_userId is still empty and the settings
    // point somewhere else.
    matrix::app_data::AccountIdentity m_openingIdentity;
    QString m_sessionFilePath;
    QString m_homeserver;
    QString m_userId;
    QString m_deviceId;
    bool m_loggedIn = false;
    // Opened from the local store because the homeserver was unreachable (see
    // `session_restored_offline` in Rust); the connection state starts at
    // Offline. Cleared wherever a session ends.
    bool m_restoredOffline = false;
    // Latched so the periodic backstop logs the fault once per transition;
    // cleared when a later check agrees.
    bool m_ownIdentityKeyMismatchLogged = false;
    ConnectionState m_state = Disconnected;
    bool m_initialSyncDone = false;
    SessionLifecycleGuard m_lifecycle;
    quint64 m_handleGeneration = 0;
    matrix::app_data::AccountIdentity m_signOutIdentity;
    QString m_signOutDeviceId;
    QTimer m_pollTimer;
    // Every room this session knows, including Spaces and, on classic sync,
    // rooms no index space names.
    QHash<QString, RoomInfo> m_rooms;
    // The SDK room list's index space, one-for-one, because room-list diffs
    // address it by index. Only `room_list_*` diffs and `room_list_reset` may
    // write it; never `room_snapshot`. See matrix::rust_rooms.
    QStringList m_roomOrder;
    QString m_syncMode = QStringLiteral("stopped");
    QString m_lastSyncState;
    QHash<QString, QString> m_lastReceiptSent;
    // Kept here too so it survives a login: the setting is read at startup,
    // before any bridge exists.
    int m_readReceiptPrivacy = 0;
    QString m_typingRoom;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    // A backward-pagination page arrives as many one-item inserts after the
    // timeline-start sentinel. Keep the mirror exact per diff but publish one
    // contiguous model transaction at the drain boundary.
    bool m_coalesceTimelineInserts = false;
    QString m_timelineInsertBatchRoom;
    quint64 m_timelineInsertBatchGeneration = 0;
    int m_timelineInsertBatchFirst = -1;
    int m_timelineInsertBatchCount = 0;
    // `set` diffs interleave with the inserts building a page. Updates inside
    // the new range fold into its payload; updates to existing rows are
    // published after the insertion, resolved by stable id since indices move.
    QStringList m_timelineInsertBatchChangedIds;
    QHash<QString, PendingSend> m_pendingSends;
    QHash<QString, PendingProbe> m_pendingProbes;
    quint64 m_txnCounter = 0;

    // Live SDK timeline state. The tracker adopts the Rust room generation from
    // timeline_reset and rejects stale diffs; pagination state is per room and
    // reset on every (re)open.
    struct PaginationState {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
        bool failureTransient = false;
        // Adaptive page size: after a page whose events were all filtered out,
        // ask for more; back to the default as soon as a page yields a row. See
        // loadOlderMessages.
        unsigned short batchSize = 0; // 0 = the default
        // The last completed page's events were all filtered out. See
        // lastPaginationFullyFiltered.
        bool lastFullyFiltered = false;
        // Cumulative filter totals as of this room's last page, for comparison.
        quint64 lastFilterOffered = 0;
        quint64 lastFilterDropped = 0;
    };
    matrix::rust_timeline::TimelineGenerationTracker m_timelineTracker;
    /// The superseded generation being counted and its dropped diffs; reported
    /// once by the first diff the new generation accepts.
    quint64 m_staleDiffGeneration = 0;
    int m_staleDiffCount = 0;
    // Same tracker for the single open thread timeline, keyed by composite id
    // and stamped with Rust's thread_generation.
    matrix::rust_timeline::TimelineGenerationTracker m_threadTracker;
    QHash<QString, PaginationState> m_pagination;
    // The room whose Threads view is open, plus the adopted thread-list
    // generation (stale snapshots are rejected).
    QString m_threadListRoom;
    quint64 m_threadListGeneration = 0;
    // Bounds manual decryption retries to one dispatch per room per short
    // window.
    QHash<QString, qint64> m_lastDecryptionRetryMs;

    // Op-id counter for room-management/media commands, and the cached upload
    // limit (0 until the first upload_limit event).
    quint64 m_opCounter = 0;
    qint64 m_maxUploadSize = 0;
    bool m_uploadLimitRequested = false;
};
