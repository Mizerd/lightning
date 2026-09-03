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
    explicit RustSdkMatrixClient(SettingsManager *settings, QObject *parent = nullptr);
    ~RustSdkMatrixClient() override;

    // Introspection surfaced for the Settings screen / logs.
    QString rustBackendName() const;
    QString rustBackendStatus() const;
    QString rustBackendVersion() const;
    bool    rustSupportsE2ee() const;

    // Testing hook. When non-empty, the SDK store is created at exactly
    // this absolute path, bypassing the per-account subdirectory layout
    // under matrix::app_data::primaryRoot(). Reserved for the headless
    // smoke harness so back-to-back password logins start from a clean
    // temporary crypto store and cannot hit the SDK's
    // "account in the store doesn't match the account in the
    // constructor" error. Must be called BEFORE login() / restoreSession().
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

    // Encrypted-room test probe (v0.5.0-prep+6). Bypasses the C++
    // CryptoManager::supportsE2ee gate and calls
    // mx_rust_probe_encrypted_send. The Rust side still performs the
    // encryption via matrix-sdk (e2e-encryption + sqlite features);
    // C++ never handles ciphertext or keys. Reserved for the headless
    // smoke harness so E2EE can be verified before the UI gate is
    // flipped. Emits encryptedSendProbeResult(...) with a safe marker
    // and either an SDK event id or a non-secret failure message. Do
    // NOT wire this into QML — the interactive send path stays gated
    // on supportsE2ee.
    void probeEncryptedSend(const QString &roomId,
                            const QString &body,
                            const QString &marker);

    // Key-backup recovery (v0.5.0-prep+7). Wraps
    // mx_rust_recover_from_backup so the smoke harness can attempt to
    // restore room keys via matrix-sdk's server-side secret storage
    // WITHOUT the recovery key ever transiting a log or QML property.
    // Result flows through keyBackupResult(...). Do NOT expose to QML.
    void recoverFromBackup(const QString &recoveryKey);

    // v0.5.0-prep+11. Reload a room's recent timeline from matrix-sdk
    // (calls Room::messages). Timeline events dedupe by event_id in
    // handleTimelineEvent so this is safe to call at any time. Emits
    // roomTimelineReloaded(roomId, total, decrypted, undecryptable)
    // when the SDK finishes, or errorOccurred with a non-secret
    // message on failure.
    //
    // v0.5.7: kept only as a smoke-test helper. The interactive room
    // history path is openRoomTimeline() below — do not use this for UI
    // room opens; it cannot update already-visible rows in place.
    void reloadRoomTimeline(const QString &roomId, int limit = 30);

    // v0.5.7. Open (or re-open) the live matrix-sdk-ui timeline for a
    // room. Rust cancels any previous room subscription, builds a
    // persistent SDK Timeline, sends one timeline_reset snapshot, and
    // streams incremental VectorDiff updates that are applied in place —
    // including undecryptable → decrypted replacements after room-key
    // import, SDK local echoes with send-state transitions, and backward
    // pagination prepends. Safe to call repeatedly; each call advances
    // the Rust room generation and stale callbacks are rejected.
    void openRoomTimeline(const QString &roomId);
    // 2026-08-19: re-open the live timeline after the SDK event cache has
    // released the paginated backlog. Explicit jump-to-latest from far back
    // ONLY — see mx_rust_timeline_reload_at_live.
    // Returns false when the dispatch itself failed, so the caller never
    // commits to a reset that will not arrive.
    bool reloadRoomTimelineAtLive(const QString &roomId);
    void closeRoomTimeline();

    // v0.5.0. SAS emoji verification (receive-first). Drives
    // mx_rust_accept_verification / confirm / mismatch / cancel.
    // Results flow through the verification* signals below. Only one
    // active flow at a time in the Rust bridge; calling accept on a
    // stale flow id returns a non-secret error via errorOccurred.
    void acceptVerification(const QString &flowId);
    void confirmVerification(const QString &flowId);
    void mismatchVerification(const QString &flowId);
    void cancelVerification(const QString &flowId);
    // The user confirmed that the OTHER device reported a successful scan
    // of the QR code Lightning displayed. Only the SDK performs the trust
    // change; this never promotes trust locally.
    void confirmQrVerification(const QString &flowId);

    // v0.5.6. Lightning-initiated SAS verification of the current
    // session against another session belonging to the same Matrix
    // account. Emits verificationStartFailed(reason) synchronously
    // when the request cannot be dispatched (already active, no own
    // identity, not logged in); success flows through the normal
    // verificationRequestStarted / verificationSasReady / verificationDone
    // events. Only advertises SAS as a method.
    void startOwnVerification();

    // v0.7.2. Ask the Rust recovery coordinator for a fresh
    // standards-based encryption-secret request round ("Request keys
    // again"). Progress arrives as cryptoBootstrapEvent state updates;
    // failures to dispatch surface through errorOccurred.
    void requestMissingSecrets();

    // v0.6.0 checkpoint 7. Ask Rust for one sanitized E2EE health snapshot
    // (device trust, cross-signing keys, backup/recovery/secret-storage
    // state). Result arrives as cryptoHealthUpdated(map). Safe to call at
    // any time; skipped when not logged in.
    void queryCryptoHealth();

    // v0.6.0 checkpoint 9. Ask Rust for the account's device/session list
    // (server metadata + SDK crypto trust). Result arrives as
    // deviceListUpdated(ok, devices).
    void requestDeviceList();

    // v0.5.6. Snapshot the current session's cross-signing trust state
    // from the SDK. Emits ownDeviceStatusUpdated(...) with only the
    // aggregate fields the UI displays. Safe to call at any time; the
    // wrapper skips the call when not logged in.
    void refreshOwnDeviceStatus();

    // v0.5.6. Encrypted Megolm room-key import. The path is passed to
    // Rust unchanged; the passphrase is forwarded once and never
    // logged. Results flow through roomKeyImportStarted /
    // roomKeyImportProgress / roomKeyImportDone / roomKeyImportFailed.
    // Only one import may be active per Rust client.
    void importRoomKeys(const QString &filePath, const QString &passphrase);
    bool roomKeyImportActive() const;

    // MatrixClient interface -------------------------------------------------
    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;

    // OAuth 2.0 / OIDC. See the two-phase store lifecycle note in
    // rust/src/oauth.rs: phase A authenticates on a store-less bootstrap
    // handle, phase B opens the real account store only once the homeserver
    // has named the account and device.
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
    // v0.7 outgoing @-mentions: attach m.mentions through the SDK send path.
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
    // v0.9 formatted sends: the bodySpec crosses the FFI as one JSON
    // argument; Rust re-validates it and strict-sanitizes any HTML.
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

    // 2026-08-18: redact this message's own m.replace events (see
    // rooms::remove_message_edits). Rust backend only: it needs the event
    // relations, which only the SDK exposes.
    bool supportsRemovingEdits() const override { return true; }
    void removeMessageEdits(const QString &roomId,
                            const QString &eventId) override;

    // v0.7: MSC3381 polls through the SDK timeline (room or thread target).
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
    // v0.7.x Matrix presence: bounded polling (Sliding Sync carries no
    // presence events) + own-state publication. Policy lives in
    // PresenceManager; these are thin command wrappers.
    bool supportsPresence() const override { return true; }
    void requestPresence(const QStringList &userIds, quint64 opId) override;
    bool supportsProfileBanners() const override { return true; }
    void fetchProfileBanner(const QString &userId, quint64 opId) override;
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

    // v0.6.0: SDK-backed thread timelines (TimelineFocus::Thread in Rust).
    // The thread flows through the same diff signals under its composite
    // timeline id; the pagination methods below answer for both id kinds.
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
    bool supportsCancelSend() const override { return true; }
    void cancelSend(const QString &roomId,
                    const QString &transactionId) override;
    void retryFailedSend(const QString &roomId,
                         const QString &transactionId) override;

    // v0.5.9 — conversation creation, membership, room editing, media.
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
    // v0.7.x room administration + pinned messages.
    quint64 setMemberPowerLevel(const QString &roomId, const QString &userId,
                                qlonglong level) override;
    quint64 setRoomPowerLevelKey(const QString &roomId, const QString &key,
                                 qlonglong level) override;
    // v0.9 scheduled send (phase 11).
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
    // v0.9 message edit history + event source (phase 7).
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
    // v0.9 device + backup management (phase 9).
    quint64 renameDevice(const QString &deviceId, const QString &name) override;
    quint64 backupAction(const QString &action) override;
    void requestBackupProgress() override;
    // v0.9 room upgrade (phase 8).
    void requestRoomVersions() override;
    quint64 upgradeRoom(const QString &roomId, const QString &newVersion) override;
    // v0.9 room access (phase 4).
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
    // MatrixRTC (MSC4143): observation and discovery. There is deliberately
    // no join/publish override — see MatrixClient::supportsMatrixRtc.
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcSession(const QString &roomId) override;
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
                           int width, int height, bool animated) override;
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
                                 bool animated) override;
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
    // v0.5.0-prep+6. Fires exactly once per probeEncryptedSend call.
    // `ok`: true when matrix-sdk returned a real server event id.
    // `marker`: caller-supplied opaque identifier (never contains
    //           the probe body). Safe to log.
    // `serverEventId`: only meaningful when ok == true.
    // `message`: non-secret failure detail when ok == false; empty on
    //            success. May be truncated / one-lined by the caller.
    void encryptedSendProbeResult(const QString &roomId,
                                  const QString &marker,
                                  bool ok,
                                  const QString &serverEventId,
                                  const QString &message);

    // v0.5.0-prep+7. Fires once per recoverFromBackup call.
    // `state`: "attempted" (dispatch received) or "ok" / "failed" once
    //          matrix-sdk finishes. `message`: non-secret failure detail
    //          or empty. Never contains the recovery key or imported
    //          key material.
    void keyBackupResult(const QString &state, const QString &message);

    // v0.5.0-prep+11. Fires once per reloadRoomTimeline() call.
    void roomTimelineReloaded(const QString &roomId,
                              int totalEvents,
                              int decryptedEvents,
                              int undecryptableEvents);

    // v0.5.0. SAS emoji verification lifecycle.
    void verificationRequestReceived(const QString &flowId,
                                     const QString &otherUserId,
                                     const QString &otherDeviceId,
                                     bool isSelfVerification);
    // Both sides have exchanged m.key.verification.ready and the SDK is
    // driving the m.key.verification.start/accept/key exchange. This used
    // to be dropped on the floor as "informational", which left the UI
    // parked on "Incoming verification request" — Accept still offered —
    // for the whole handshake, with no way to tell progress from a stall.
    // Flow id only; no key, MAC or SAS material crosses this boundary.
    void verificationReady(const QString &flowId);
    void verificationSasReady(const QString &flowId,
                              const QVariantList &emojis,
                              const QVariantList &decimals);
    // v0.7.1. OUR side's "They match" was registered by the SDK
    // (SasState::Confirmed) — the flow now waits for the OTHER session's
    // confirmation before verificationDone can fire. Emitted at most once
    // per flow; carries the flow id only, never emoji values.
    void verificationSasConfirmed(const QString &flowId);
    // Show-QR verification. Lightning DISPLAYS a code for the other device
    // to scan; it never scans (no camera), so m.qr_code.scan.v1 is never
    // advertised. `modules` is the code's width in modules and `bits` its
    // row-major bitmap (MSB first, each row starting on a fresh byte,
    // stride = (modules + 7) / 8, set bit = dark module). ONLY that
    // geometry crosses this boundary — the payload the code encodes is
    // cross-signing key material and the flow's shared secret, and it never
    // leaves the Rust bridge.
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

    // v0.5.6. Emitted when Lightning successfully dispatches a SAS
    // verification request through mx_rust_start_own_verification. The
    // flow id and self-verification flag are safe metadata; the SDK
    // has already signed and sent the outbound m.key.verification.request.
    void verificationRequestStarted(const QString &flowId,
                                    const QString &otherUserId,
                                    bool isSelfVerification);

    // v0.5.6. Aggregate cross-signing trust snapshot from the SDK. All
    // fields are non-secret metadata; the UI must derive its "Verified"
    // label from deviceCrossSigned, not from generic own-device trust.
    // v0.6.0 checkpoint 7: sanitized E2EE health snapshot (booleans, enum
    // names, public device id only).
    void cryptoHealthUpdated(const QVariantMap &snapshot);
    // v0.7: sanitized verified-session bootstrap observer events (state
    // names + key counts only; never key material or session ids).
    void cryptoBootstrapEvent(const QString &kind, const QString &state,
                              quint64 count);
    // v0.6.0 checkpoint 9: entries carry deviceId, displayName, lastSeenTs,
    // lastSeenIp, isCurrent, hasCryptoIdentity, verified, crossSigned.
    void deviceListUpdated(bool ok, const QVariantList &devices);
    void ownDeviceStatusUpdated(const QString &deviceId,
                                bool ownIdentityAvailable,
                                bool ownIdentityVerified,
                                bool deviceCrossSigned,
                                bool hasMasterKey,
                                bool hasSelfSigningKey,
                                bool hasUserSigningKey);

    // v0.5.6. Encrypted room-key import lifecycle. Aggregate counts and
    // affected room IDs only; the decrypted export never leaves Rust.
    void roomKeyImportStarted();
    void roomKeyImportProgress(int imported, int total);
    void roomKeyImportDone(int imported,
                           int total,
                           int affectedRooms,
                           const QStringList &roomIds);
    void roomKeyImportFailed(const QString &category, const QString &message);

    // v0.5.7. Emitted after a room-key import triggered an immediate
    // decryption retry on the open SDK timeline. Counts only — imported
    // session identifiers stay inside Rust.
    void roomKeysApplied(const QString &roomId, int sessionCount);

    // Structured lifecycle state consumed by AppController/QML.
    //
    // `userId`/`homeserver` identify the account whose local session actually
    // failed to open, which is NOT always the one the settings currently
    // point at (an add-account attempt fails while settings still describe
    // the previously active account). Carrying the identity is what lets the
    // UI repair the right account without asking the user to retype their
    // Matrix ID. Either may be empty when the failure is precisely that no
    // usable identity could be resolved.
    void localSessionResetRequired(const QString &reasonCode,
                                   const QString &userId,
                                   const QString &homeserver);
    // A local session could not be opened, but deleting local data is NOT the
    // remedy — a missing store, a revoked access token, contestable store
    // ownership. Same payload as above; kept separate so the destructive
    // recovery UI is never armed for a condition it cannot fix. The accurate
    // explanation reaches the user through loginFailed().
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

    void refuseSend(const char *op);
    void setState(ConnectionState state);
    void setInitialSyncDone(bool done);
    void clearLocalState();
    void ensurePollTimer();
    bool ensureRustHandleForUser(const QString &userIdForStore);
    // Preferred entry point: opens the account's RECORDED store directory.
    bool ensureRustHandleForIdentity(
        const matrix::app_data::AccountIdentity &identity);
    bool ensureRustHandleForStorePath(const QString &storePath,
                                      const QString &slug);
    void releaseRustHandle();

    // Phase A: a handle with NO persistent store, used only to discover a
    // server's auth methods and to run the browser sign-in.
    //
    // It is deliberately SEPARATE from m_rustHandle rather than replacing it.
    // Discovery runs as soon as a homeserver is entered, and adding a second
    // account must not tear down the signed-in account's live session just
    // because the user typed a URL. It also has no store, so it has no
    // generation to protect — the guard that matters is m_oauthInFlight.
    bool ensureOAuthBootstrapHandle();
    void releaseAuthHandle();
    // Drains the bootstrap handle's event queue. Runs on the same poll timer
    // as the session queue but independently of it, because during a sign-in
    // there may be no session handle at all.
    void drainAuthEvents();
    // Phase B: the homeserver has answered with the canonical identity, so the
    // real account store can finally be chosen, gated and opened.
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

    // PHASE B, shared by both browser flows.
    //
    // This is where the store-ownership gate lives — "a device the server just
    // issued must never adopt a store belonging to a different device" — and
    // it is shared precisely BECAUSE it is the security-critical step. Two
    // copies would let a future fix land on one flow and not the other.
    //
    // `authType` is the persisted routing discriminator ("oauth" or "sso"),
    // and `clientId` is the dynamic-registration id for OAuth and empty for
    // SSO. `restore` performs the SDK-specific session restore once the store
    // is open: oauth().restore_session() for OAuth, the ordinary matrix_auth()
    // path for SSO, which is what an SSO session actually is.
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
    // The homeserver this OAuth attempt targets. Held for phase B, which must
    // derive the account identity from the SERVER's answer plus this URL.
    QString m_oauthHomeserver;
    // True between beginOAuthLogin() and a terminal outcome. Guards against a
    // second attempt racing the first and against a late callback completing
    // a cancelled sign-in.
    bool m_oauthInFlight = false;
    class OAuthCallbackServer *m_ssoCallback = nullptr;
    QString m_ssoHomeserver;
    // Same guard as m_oauthInFlight, for the SSO flow. Separate rather than
    // shared so a stale callback from one flow can never complete the other.
    bool m_ssoInFlight = false;
    QString rustStorePathForUser(const QString &userIdForStore) const;
    void pollRustEvents();
    void handleRustEvent(const QJsonObject &event, quint64 eventGeneration);
    void finishSignOut(const QString &serverResult, const QString &serverMessage);
    bool clearPersistedAccount(const matrix::app_data::AccountIdentity &identity,
                               bool *matchedRecord = nullptr);
    void requireLocalReset(const QString &reasonCode,
                           const matrix::app_data::AccountIdentity &identity);
    // Report a blocked open with its own accurate message instead of the one
    // "belongs to a different session or device" string every condition used
    // to share.
    void failWithBlockReason(matrix::rust_session::StoreBlockReason reason,
                             const matrix::app_data::AccountIdentity &identity);
    // Adopt a store an older build wrote under the TYPED localpart casing so
    // a saved account can be restored instead of dead-ending on a store that
    // was never at the canonical path. Adoption RECORDS the directory and
    // rebinds `identity` to it — nothing on disk is moved or deleted, and the
    // SDK's own account-ownership check remains the authority on whether the
    // adoption was correct. Refuses whenever ownership is contestable.
    bool adoptDivergentStoreIfUnambiguous(
        matrix::app_data::AccountIdentity *identity,
        matrix::rust_session::StoreBlockReason *refusal);
    // Persist where this session's store really is, taken from the directory
    // that was actually opened rather than re-derived from the user id.
    void recordStoreLocation(const matrix::app_data::AccountIdentity &identity);
    void handleRoomsEvent(const QJsonArray &rooms);
    void handleRoomListDiff(const QJsonObject &event);
    void handleSpacesEvent(const QJsonArray &spaces);
    RoomInfo roomInfoFromJson(const QJsonObject &obj) const;
    void handleTimelineEvent(const QJsonObject &event);
    // v0.5.7 live-timeline event handlers.
    void handleTimelineReset(const QJsonObject &event);
    void handleTimelineDiff(const QJsonObject &event);
    /// Report and reset the stale-diff run, if there is one.
    ///
    /// A superseded generation keeps delivering until its subscription stops,
    /// so the run is a whole timeline's worth of diffs. One line per diff is
    /// O(timeline) for a guard that is working correctly.
    void reportStaleTimelineDiffs();
    void handleTimelinePagination(const QJsonObject &event);
    void flushTimelineInsertBatch();
    void clearTimelineInsertBatch();
    // v0.6.0: thread-timeline event handlers (same envelopes as the room
    // handlers, addressed by the composite thread timeline id).
    void handleThreadReset(const QJsonObject &event);
    void handleThreadDiff(const QJsonObject &event);
    void handleThreadPagination(const QJsonObject &event);
    void handleThreadError(const QJsonObject &event);
    void handleThreadClosed(const QJsonObject &event);
    bool threadTimelineActiveFor(const QString &timelineId) const;
    void clearThreadTimelineState();
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

    // v0.5.9: new-command plumbing. nextOpId() never returns 0 (0 means
    // "unsupported" at the interface level).
    // Media-capable SDP store (see MatrixClient::setCallMediaCapable and
    // calls::SdpStore): populated ONLY while media-capable mode is on,
    // wiped with the session.
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
    ConnectionState m_state = Disconnected;
    bool m_initialSyncDone = false;
    SessionLifecycleGuard m_lifecycle;
    quint64 m_handleGeneration = 0;
    matrix::app_data::AccountIdentity m_signOutIdentity;
    QString m_signOutDeviceId;
    QTimer m_pollTimer;
    QHash<QString, RoomInfo> m_rooms;
    QStringList m_roomOrder;
    QString m_syncMode = QStringLiteral("stopped");
    QString m_lastSyncState;
    QHash<QString, QString> m_lastReceiptSent;
    QString m_typingRoom;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    // One Rust poll normally contains a whole backward-pagination page, but
    // matrix-sdk-ui describes it as many one-item inserts after the leading
    // timeline-start sentinel. Keep the mirror exact for every diff while
    // publishing one contiguous Qt model transaction at the drain boundary.
    bool m_coalesceTimelineInserts = false;
    QString m_timelineInsertBatchRoom;
    quint64 m_timelineInsertBatchGeneration = 0;
    int m_timelineInsertBatchFirst = -1;
    int m_timelineInsertBatchCount = 0;
    // `set` diffs are commonly interleaved with the insert diffs that build a
    // pagination page. Updates to rows inside the new range are already folded
    // into the final range payload; updates to existing rows are published
    // after the atomic insertion, resolved by stable identity because their
    // numeric indices move while the page is assembled.
    QStringList m_timelineInsertBatchChangedIds;
    QHash<QString, PendingSend> m_pendingSends;
    QHash<QString, PendingProbe> m_pendingProbes;
    quint64 m_txnCounter = 0;

    // v0.5.7 live SDK timeline state. The tracker adopts the Rust room
    // generation from timeline_reset and rejects stale diffs; pagination
    // state is per room and reset on every (re)open.
    struct PaginationState {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
        bool failureTransient = false;
    };
    matrix::rust_timeline::TimelineGenerationTracker m_timelineTracker;
    /// The superseded generation currently being counted, and how many of its
    /// diffs have been dropped. Reported once, by the first diff the new
    /// generation accepts.
    quint64 m_staleDiffGeneration = 0;
    int m_staleDiffCount = 0;
    // v0.6.0: same tracker type for the single open thread timeline, keyed
    // by the composite thread timeline id and stamped with Rust's
    // thread_generation.
    matrix::rust_timeline::TimelineGenerationTracker m_threadTracker;
    QHash<QString, PaginationState> m_pagination;
    // v0.6.0 checkpoint 5: the room whose Threads view is open, plus the
    // adopted Rust thread-list generation (stale snapshots are rejected).
    QString m_threadListRoom;
    quint64 m_threadListGeneration = 0;
    // v0.6.0 checkpoint 8: bound manual decryption retries (one dispatch per
    // room per short window — a double-click never doubles the SDK work).
    QHash<QString, qint64> m_lastDecryptionRetryMs;

    // v0.5.9: operation-id counter for room-management/media commands and
    // the cached server upload limit (0 until the first upload_limit event).
    quint64 m_opCounter = 0;
    qint64 m_maxUploadSize = 0;
    bool m_uploadLimitRequested = false;
};
