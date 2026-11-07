#pragma once

#include "accounts/LiveSessionTicket.h"
#include "matrix/CallSignal.h"
#include "matrix/RtcSession.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Backend interface for Matrix operations. UI and models depend only on this,
// never on a concrete backend: MockMatrixClient (--mock), CppHttpMatrixClient
// (experimental HTTP) and RustSdkMatrixClient (Matrix Rust SDK over FFI).
class MatrixClient : public QObject
{
    Q_OBJECT
public:
    enum ConnectionState {
        Disconnected,
        Connecting,
        Syncing,
        Error,
        Offline,
    };
    Q_ENUM(ConnectionState)

    explicit MatrixClient(QObject *parent = nullptr) : QObject(parent) {}
    ~MatrixClient() override = default;

    virtual void login(const QString &homeserver,
                       const QString &user,
                       const QString &password) = 0;
    virtual void logout() = 0;
    virtual bool restoreSession() = 0;
    // Account switching: end the local session only (stop sync, drop in-memory
    // state, emit loggedOut()) without a server logout and without deleting the
    // account's store, tokens or metadata. restoreSession() then activates the
    // account the settings select. Returns false when unsupported.
    virtual bool detachSession() { return false; }
    virtual bool adoptLiveSession(const lightning::accounts::LiveSessionTicket &ticket)
    {
        Q_UNUSED(ticket);
        return false;
    }
    // Whether adoptLiveSession() can take a background handle at all. The
    // switch only chooses the warm path when this is true, so a backend that
    // cannot adopt never has a live handle handed to it.
    virtual bool supportsLiveAdoption() const { return false; }

    virtual bool supportsOAuthLogin() const { return false; }
    virtual void discoverAuthMethods(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start a browser sign-in. Emits oauthBrowserUrlReady(), then
    // loginSucceeded()/loginFailed(). The user id is unknown until this
    // completes, so no account store is opened before then.
    virtual void beginOAuthLogin(const QString &homeserver) { Q_UNUSED(homeserver); }
    virtual void beginOAuthSignUp(const QString &homeserver) { Q_UNUSED(homeserver); }
    virtual void cancelOAuthLogin() {}

    virtual bool supportsSsoLogin() const { return false; }
    virtual void requestSsoProviders(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start an SSO sign-in; an empty `idpId` selects the default flow. Emits
    // ssoBrowserUrlReady(), then the normal session path. No store is opened
    // until the user id is known.
    virtual void beginSsoLogin(const QString &homeserver, const QString &idpId)
    {
        Q_UNUSED(homeserver);
        Q_UNUSED(idpId);
    }
    virtual void cancelSsoLogin() {}

    virtual bool isLoggedIn() const = 0;
    virtual QString currentUserId() const = 0;
    /// This session's device id, or empty where the backend has none (mock,
    /// HTTP). Compared, never displayed.
    virtual QString currentDeviceId() const { return {}; }
    virtual QString homeserverUrl() const = 0;

    virtual void startSync() = 0;
    virtual void stopSync() = 0;
    virtual ConnectionState connectionState() const = 0;

    virtual bool initialSyncDone() const { return true; }
    virtual QString syncMode() const { return QStringLiteral("classic_fallback"); }

    virtual QList<RoomInfo> rooms() const = 0;
    virtual RoomInfo roomInfo(const QString &roomId) const
    {
        const auto all = rooms();
        for (const auto &room : all) {
            if (room.id == roomId)
                return room;
        }
        return {};
    }
    virtual QList<TimelineEvent> timeline(const QString &roomId) const = 0;

    virtual QString displayNameFor(const QString &roomId, const QString &userId) const = 0;
    virtual QString avatarMxcFor(const QString &roomId, const QString &userId) const = 0;
    virtual QStringList typingUsersFor(const QString &roomId) const = 0;

    virtual QUrl mediaDownloadUrl(const QString &mxcUrl) const = 0;
    virtual QUrl mediaThumbnailUrl(const QString &mxcUrl,
                                   int width, int height,
                                   bool crop = false) const = 0;

    virtual void sendTextMessage(const QString &roomId, const QString &body) = 0;
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body) = 0;

    virtual void sendTextMessage(const QString &roomId, const QString &body,
                                 const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendTextMessage(roomId, body);
    }
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendReply(roomId, replyToEventId, body);
    }

    virtual void sendTextMessage(const QString &roomId, const QString &body,
                                 const QStringList &mentionUserIds,
                                 const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendTextMessage(roomId, body, mentionUserIds);
    }
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds,
                           const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendReply(roomId, replyToEventId, body, mentionUserIds);
    }

    virtual void sendThreadReply(const QString &roomId,
                                 const QString &threadRootEventId,
                                 const QString &body)
    {
        sendReply(roomId, threadRootEventId, body);
    }
    static QString redactionFailureText(const QString &category);

    static QString threadTimelineId(const QString &roomId,
                                    const QString &rootEventId)
    {
        return roomId + QStringLiteral("\x1f" "thread" "\x1f") + rootEventId;
    }
    static bool isThreadTimelineId(const QString &timelineId)
    {
        return timelineId.contains(QStringLiteral("\x1f" "thread" "\x1f"));
    }
    static QString threadTimelineRoomId(const QString &timelineId)
    {
        const int sep = timelineId.indexOf(QChar(0x1f));
        return sep < 0 ? timelineId : timelineId.left(sep);
    }
    static QString threadTimelineRootId(const QString &timelineId)
    {
        const int sep = timelineId.lastIndexOf(QChar(0x1f));
        return sep < 0 ? QString{} : timelineId.mid(sep + 1);
    }

    static QString contextTimelineId(const QString &roomId,
                                     const QString &eventId)
    {
        return roomId + QStringLiteral("\x1f" "ctx" "\x1f") + eventId;
    }
    static bool isContextTimelineId(const QString &timelineId)
    {
        return timelineId.contains(QStringLiteral("\x1f" "ctx" "\x1f"));
    }
    virtual bool supportsEventContext() const { return false; }
    virtual void openEventContext(const QString &roomId, const QString &eventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
    }
    virtual void closeEventContext() {}
    virtual void paginateEventContext(const QString &roomId,
                                      const QString &eventId, bool forward)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
        Q_UNUSED(forward);
    }

    virtual bool supportsThreadTimelines() const { return false; }
    virtual void openThread(const QString &roomId, const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void closeThread() {}
    virtual void setThreadRepliesInLiveTimeline(bool inLive) { Q_UNUSED(inLive); }
    virtual void setUnthreadedReadReceipts(bool on) { Q_UNUSED(on); }
    // Manual decryption retry for a room's unable-to-decrypt events against key
    // material that has arrived since. No-op without a crypto machine. Never
    // resets any store.
    virtual void retryDecryption(const QString &roomId) { Q_UNUSED(roomId); }

    virtual bool supportsThreadList() const { return false; }
    virtual void openThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void closeThreadList() {}
    virtual void paginateThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void markThreadRead(const QString &roomId,
                                const QString &rootEventId, quint64 opId)
    {
        // A backend with no threaded receipt has nothing to wait for; say it
        // was accepted so the Activity bell is not left holding thread rows
        // (the Rust backend answers asynchronously instead).
        Q_EMIT threadReadMarkerAdvanced(roomId, rootEventId, opId);
    }
    virtual void queryThreadSubscription(const QString &roomId,
                                         const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void setThreadSubscribed(const QString &roomId,
                                     const QString &rootEventId,
                                     bool subscribed)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
        Q_UNUSED(subscribed);
    }

    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body)
    {
        Q_UNUSED(inReplyToEventId);
        sendThreadReply(roomId, threadRootEventId, body);
    }

    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body);
    }

    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds,
                                   const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body,
                          mentionUserIds);
    }

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody) = 0;

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        editMessage(roomId, targetEventId, newBody);
    }

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds,
                             const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        editMessage(roomId, targetEventId, newBody, mentionUserIds);
    }
    virtual void redactEvent(const QString &roomId,
                             const QString &eventId,
                             const QString &reason = QString()) = 0;
    virtual void toggleReaction(const QString &roomId,
                                const QString &targetEventId,
                                const QString &key) = 0;

    virtual bool supportsRemovingEdits() const { return false; }
    virtual void removeMessageEdits(const QString &roomId,
                                    const QString &eventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
    }

    virtual bool supportsPolls() const { return false; }
    virtual bool supportsJoinByAddress() const { return false; }
    virtual void sendPollResponse(const QString &roomId,
                                  const QString &threadRootId,
                                  const QString &pollStartEventId,
                                  const QStringList &answerIds)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(pollStartEventId);
        Q_UNUSED(answerIds);
    }
    virtual void endPoll(const QString &roomId,
                         const QString &threadRootId,
                         const QString &pollStartEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(pollStartEventId);
    }
    virtual void createPoll(const QString &roomId,
                            const QString &threadRootId,
                            const QString &question,
                            const QStringList &answers,
                            bool undisclosed,
                            int maxSelections)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(question);
        Q_UNUSED(answers);
        Q_UNUSED(undisclosed);
        Q_UNUSED(maxSelections);
    }
    virtual void sendTyping(const QString &roomId,
                            bool isTyping,
                            int timeoutMs = 20000) = 0;
    virtual void sendReadReceipt(const QString &roomId,
                                 const QString &eventId) = 0;
    virtual void setFullyReadMarker(const QString &roomId, const QString &eventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
    }
    virtual void setReadReceiptPrivacy(int mode) { Q_UNUSED(mode); }
    virtual void setRoomMarkedUnread(const QString &roomId, bool unread)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(unread);
    }
    virtual bool supportsRoomFavourites() const { return false; }
    virtual void setRoomFavourite(const QString &roomId, bool favourite)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(favourite);
    }
    virtual void setRoomLowPriority(const QString &roomId, bool lowPriority)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(lowPriority);
    }
    virtual bool supportsMarkRoomRead() const { return false; }
    virtual void markRoomRead(const QString &roomId) { Q_UNUSED(roomId); }
    virtual bool supportsServerNotificationModes() const { return false; }
    virtual void setRoomNotificationMode(const QString &roomId, int mode)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(mode);
    }
    virtual void requestThreadParticipants(const QString &roomId,
                                           const QString &rootEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId);
    }
    virtual bool supportsPresence() const { return false; }
    // Whether this backend keeps a room's read state (unreadCount,
    // highlightCount, hasUnreadMessages, markedUnread) current across devices.
    // The mock and HTTP backends never write hasUnreadMessages or markedUnread,
    // so "every unread signal is clear" is vacuously true there, and
    // notification_count alone is not a read signal. Ask this before acting on
    // "this room has been read"; see
    // ActivityModel::reconcileRoomsAgainstTheirReadState.
    virtual bool tracksRoomReadState() const { return false; }
    virtual void requestPresence(const QStringList &userIds, quint64 opId)
    {
        Q_UNUSED(userIds); Q_UNUSED(opId);
    }
    virtual void publishPresence(int state) { Q_UNUSED(state); }
    virtual void publishPresence(int state, const QString &statusMsg)
    { Q_UNUSED(statusMsg); publishPresence(state); }
    virtual bool supportsProfileBanners() const { return false; }
    virtual void fetchProfileBanner(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    virtual void setProfileBanner(const QString &localPath, quint64 opId)
    {
        Q_UNUSED(localPath); Q_UNUSED(opId);
    }
    virtual bool supportsNameColors() const { return false; }
    virtual void fetchNameColor(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    virtual void setNameColor(const QString &value, quint64 opId)
    {
        Q_UNUSED(value); Q_UNUSED(opId);
    }
    virtual bool supportsProfileBios() const { return false; }
    virtual void fetchProfileBio(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    virtual void setProfileBio(const QString &text, quint64 opId)
    {
        Q_UNUSED(text); Q_UNUSED(opId);
    }
    virtual bool supportsRoomBanners() const { return false; }
    virtual void fetchRoomBanner(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    virtual void setRoomBanner(const QString &roomId, const QString &localPath,
                               quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(opId);
    }
    virtual bool supportsRoomBackgrounds() const { return false; }
    virtual void fetchRoomBackground(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    virtual void setRoomBackground(const QString &roomId,
                                   const QString &localPath,
                                   const QString &contentJson, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(contentJson);
        Q_UNUSED(opId);
    }
    virtual bool supportsSettingsSync() const { return false; }
    virtual void readSyncedSettings(quint64 opId) { Q_UNUSED(opId); }
    virtual void writeSyncedSettings(int kind, const QString &contentJson,
                                     quint64 opId)
    {
        Q_UNUSED(kind); Q_UNUSED(contentJson); Q_UNUSED(opId);
    }
    virtual bool supportsPersonalBackgroundSync() const { return false; }
    virtual void readPersonalBackgrounds(const QString &scope, quint64 opId)
    {
        Q_UNUSED(scope); Q_UNUSED(opId);
    }
    virtual void downloadPersonalBackground(const QString &scope,
                                            const QString &expectedId,
                                            quint64 opId)
    {
        Q_UNUSED(scope); Q_UNUSED(expectedId); Q_UNUSED(opId);
    }
    virtual void writePersonalBackground(const QString &scope, int mode,
                                         const QString &localPath,
                                         const QString &requestedJson,
                                         quint64 opId)
    {
        Q_UNUSED(scope); Q_UNUSED(mode); Q_UNUSED(localPath);
        Q_UNUSED(requestedJson); Q_UNUSED(opId);
    }
    virtual void clearAllPersonalBackgrounds(const QStringList &knownRooms,
                                             quint64 opId)
    {
        Q_UNUSED(knownRooms); Q_UNUSED(opId);
    }

    virtual bool supportsStickerPacks() const { return false; }
    virtual void fetchStickerPacks(const QString &roomId, quint64 opId,
                                   bool deep)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId); Q_UNUSED(deep);
    }
    virtual void uploadStickerToRoomPack(const QString &roomId,
                                         const QString &stateKey,
                                         const QString &ns,
                                         const QString &shortcode,
                                         const QString &body,
                                         const QString &localPath,
                                         const QString &reuseUrl, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(ns); Q_UNUSED(shortcode);
        Q_UNUSED(body); Q_UNUSED(localPath); Q_UNUSED(reuseUrl); Q_UNUSED(opId);
    }
    virtual void migrateUserStickerPack(quint64 opId, bool startNew)
    {
        Q_UNUSED(opId); Q_UNUSED(startNew);
    }
    virtual void createStickerRoomPack(const QString &roomId,
                                       const QString &stateKey,
                                       const QString &name,
                                       const QString &usage, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(name); Q_UNUSED(usage);
        Q_UNUSED(opId);
    }
    virtual void sendSticker(const QString &roomId, const QString &rootId,
                             const QString &url, const QString &body,
                             const QString &mimetype, quint64 width,
                             quint64 height, quint64 size)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootId); Q_UNUSED(url); Q_UNUSED(body);
        Q_UNUSED(mimetype); Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(size);
    }
    virtual void addStickerToRoomPack(const QString &roomId,
                                      const QString &stateKey,
                                      const QString &ns,
                                      const QString &shortcode,
                                      const QString &url, const QString &body,
                                      const QString &mimetype, quint64 width,
                                      quint64 height, quint64 size,
                                      quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(ns); Q_UNUSED(shortcode);
        Q_UNUSED(url); Q_UNUSED(body); Q_UNUSED(mimetype); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(size); Q_UNUSED(opId);
    }
    virtual void setStickerRoomPackEnabled(const QString &roomId,
                                           const QString &stateKey,
                                           const QString &ns,
                                           bool enabled, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(ns); Q_UNUSED(enabled);
        Q_UNUSED(opId);
    }
    virtual void uploadStickerToUserPack(const QString &shortcode,
                                         const QString &body,
                                         const QString &localPath,
                                         const QString &usage,
                                         quint64 opId)
    {
        Q_UNUSED(shortcode); Q_UNUSED(body); Q_UNUSED(localPath);
        Q_UNUSED(usage); Q_UNUSED(opId);
    }
    virtual void addStickerToUserPack(const QString &shortcode,
                                      const QString &url, const QString &body,
                                      const QString &mimetype, quint64 width,
                                      quint64 height, quint64 size,
                                      const QString &usage, quint64 opId)
    {
        Q_UNUSED(shortcode); Q_UNUSED(url); Q_UNUSED(body);
        Q_UNUSED(mimetype); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(size); Q_UNUSED(usage); Q_UNUSED(opId);
    }
    virtual bool supportsPolicyLists() const { return false; }
    virtual void fetchPolicyRules(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    virtual void writePolicyRule(const QString &roomId, const QString &kind,
                                 const QString &entity,
                                 const QString &stateKey,
                                 const QString &recommendation,
                                 const QString &reason, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(kind); Q_UNUSED(entity);
        Q_UNUSED(stateKey); Q_UNUSED(recommendation); Q_UNUSED(reason);
        Q_UNUSED(opId);
    }
    virtual void setPolicySubscribed(const QString &roomId, bool subscribed,
                                     quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(subscribed); Q_UNUSED(opId);
    }
    virtual void fetchPolicySubscriptions(quint64 opId) { Q_UNUSED(opId); }
    virtual void checkPolicyEntity(const QString &kind, const QString &entity,
                                   quint64 opId)
    {
        Q_UNUSED(kind); Q_UNUSED(entity); Q_UNUSED(opId);
    }

    virtual bool supportsQrLogin() const { return false; }
    virtual quint64 qrLoginGenerate() { return 0; }
    virtual quint64 qrLoginScan(const QString &payload)
    {
        Q_UNUSED(payload);
        return 0;
    }
    virtual void qrLoginSubmitCheckCode(quint64 generation, int code)
    {
        Q_UNUSED(generation); Q_UNUSED(code);
    }
    virtual void qrLoginCancel() {}

    virtual void editStickerPack(const QString &roomId,
                                 const QString &stateKey, const QString &ns,
                                 const QString &action, const QString &argA,
                                 const QString &argB, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(ns); Q_UNUSED(action);
        Q_UNUSED(argA); Q_UNUSED(argB); Q_UNUSED(opId);
    }
    virtual void clearRoomNotificationMode(const QString &roomId)
    {
        Q_UNUSED(roomId);
    }
    virtual void requestRoomNotificationMode(const QString &roomId)
    {
        Q_UNUSED(roomId);
    }
    virtual void requestAllRoomNotificationModes() {}
    virtual void acceptInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void rejectInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void sendImage(const QString &roomId, const QString &localPath) = 0;
    virtual void sendFile(const QString &roomId, const QString &localPath) = 0;

    virtual void loadOlderMessages(const QString &roomId) = 0;
    virtual bool canPaginate(const QString &roomId) const = 0;
    virtual bool paginationReady(const QString &roomId) const
    { return canPaginate(roomId) || paginating(roomId) || paginationFailed(roomId); }
    virtual bool paginating(const QString &roomId) const = 0;

    virtual bool paginationFailed(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }
    virtual bool paginationFailureTransient(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }

    virtual bool lastPaginationFullyFiltered(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }

    virtual void retryFailedSend(const QString &roomId,
                                 const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    virtual bool supportsCancelSend() const { return false; }
    virtual void cancelSend(const QString &roomId,
                            const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    virtual bool supportsRoomManagement() const { return false; }
    virtual bool supportsAttachmentSend() const { return false; }
    virtual bool supportsMediaBridge() const { return false; }

    virtual quint64 searchUsers(const QString &query, int limit)
    { Q_UNUSED(query); Q_UNUSED(limit); return 0; }
    virtual quint64 fetchUserProfile(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    virtual bool supportsOwnProfileEditing() const { return false; }
    virtual void setOwnDisplayName(const QString &name, quint64 opId)
    { Q_UNUSED(name); Q_UNUSED(opId); }
    virtual void setOwnAvatar(const QString &localPath, quint64 opId)
    { Q_UNUSED(localPath); Q_UNUSED(opId); }
    virtual void clearOwnAvatar(quint64 opId) { Q_UNUSED(opId); }
    virtual quint64 fetchMutualRooms(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    virtual bool supportsUrlPreview() const { return false; }
    virtual quint64 fetchUrlPreview(const QString &url)
    { Q_UNUSED(url); return 0; }
    // Bounded, redirect-validated HTTPS GET for a GIF provider. `url` carries
    // the provider API key: treat it as secret and never log it. Result arrives
    // via gifResponse(); 0 when unsupported.
    virtual bool supportsGifProvider() const { return false; }
    virtual quint64 gifGet(const QString &url)
    { Q_UNUSED(url); return 0; }
    virtual quint64 gifDownload(const QString &url)
    { Q_UNUSED(url); return 0; }
    virtual QVariantList existingDirectRooms(const QString &userId) const
    { Q_UNUSED(userId); return {}; }
    virtual quint64 createDirectChat(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    virtual quint64 createRoom(const QVariantMap &options)
    { Q_UNUSED(options); return 0; }
    virtual quint64 inviteUsers(const QString &roomId, const QStringList &userIds)
    { Q_UNUSED(roomId); Q_UNUSED(userIds); return 0; }
    virtual quint64 requestRoomMembers(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 requestRoomOwnPermissions(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 setRoomName(const QString &roomId, const QString &name)
    { Q_UNUSED(roomId); Q_UNUSED(name); return 0; }
    virtual quint64 setRoomTopic(const QString &roomId, const QString &topic)
    { Q_UNUSED(roomId); Q_UNUSED(topic); return 0; }
    virtual quint64 setRoomAvatar(const QString &roomId, const QString &localPath)
    { Q_UNUSED(roomId); Q_UNUSED(localPath); return 0; }
    virtual quint64 removeRoomAvatar(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 leaveRoom(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 kickUser(const QString &roomId, const QString &userId,
                             const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    virtual quint64 banUser(const QString &roomId, const QString &userId,
                            const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    virtual quint64 unbanUser(const QString &roomId, const QString &userId,
                              const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    virtual quint64 requestModerationPlan(const QStringList &roomIds,
                                          const QString &userId,
                                          const QString &op)
    { Q_UNUSED(roomIds); Q_UNUSED(userId); Q_UNUSED(op); return 0; }
    virtual quint64 requestRoomClosurePlan(const QStringList &roomIds,
                                           const QStringList &parentIds)
    { Q_UNUSED(roomIds); Q_UNUSED(parentIds); return 0; }
    virtual quint64 closeRoom(const QString &roomId, const QString &reason,
                              bool leave, const QStringList &unlistFromSpaceIds)
    {
        Q_UNUSED(roomId); Q_UNUSED(reason); Q_UNUSED(leave);
        Q_UNUSED(unlistFromSpaceIds);
        return 0;
    }
    virtual quint64 requestServerAdminStatus() { return 0; }
    virtual quint64 adminDeleteRoom(const QString &roomId, bool block)
    { Q_UNUSED(roomId); Q_UNUSED(block); return 0; }
    virtual quint64 setMemberPowerLevel(const QString &roomId,
                                        const QString &userId,
                                        qlonglong level)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(level); return 0; }
    virtual quint64 setRoomPowerLevelKey(const QString &roomId,
                                         const QString &key, qlonglong level)
    { Q_UNUSED(roomId); Q_UNUSED(key); Q_UNUSED(level); return 0; }

    virtual quint64 setRoomJoinRule(const QString &roomId, const QString &rule)
    { Q_UNUSED(roomId); Q_UNUSED(rule); return 0; }
    virtual quint64 setRoomJoinRule(const QString &roomId, const QString &rule,
                                    const QStringList &allowedRoomIds)
    { Q_UNUSED(allowedRoomIds); return setRoomJoinRule(roomId, rule); }
    virtual quint64 setRoomHistoryVisibility(const QString &roomId,
                                             const QString &visibility)
    { Q_UNUSED(roomId); Q_UNUSED(visibility); return 0; }
    virtual quint64 setRoomGuestAccess(const QString &roomId,
                                       const QString &access)
    { Q_UNUSED(roomId); Q_UNUSED(access); return 0; }
    virtual void requestRoomDirectoryVisibility(const QString &roomId)
    { Q_UNUSED(roomId); }
    virtual quint64 setRoomDirectoryVisibility(const QString &roomId,
                                               bool published)
    { Q_UNUSED(roomId); Q_UNUSED(published); return 0; }
    virtual quint64 setRoomAltAliases(const QString &roomId,
                                      const QStringList &aliases)
    { Q_UNUSED(roomId); Q_UNUSED(aliases); return 0; }
    virtual void requestRoomVersions() {}
    virtual quint64 upgradeRoom(const QString &roomId, const QString &newVersion)
    { Q_UNUSED(roomId); Q_UNUSED(newVersion); return 0; }
    virtual quint64 renameDevice(const QString &deviceId, const QString &name)
    { Q_UNUSED(deviceId); Q_UNUSED(name); return 0; }
    virtual quint64 backupAction(const QString &action)
    { Q_UNUSED(action); return 0; }
    virtual quint64 crossSigningAction(const QString &action,
                                       const QString &recoveryKey,
                                       bool replaceRecoveryKeyConfirmed)
    {
        Q_UNUSED(action); Q_UNUSED(recoveryKey);
        Q_UNUSED(replaceRecoveryKeyConfirmed);
        return 0;
    }
    virtual quint64 requestBackupProgress() { return 0; }
    virtual void acknowledgeInterruptedSetup() {}

    virtual bool supportsAccountPrefs() const { return false; }
    virtual void readAccountPref(const QString &prefType)
    {
        QMetaObject::invokeMethod(this, [this, prefType] {
            Q_EMIT accountPrefRead(prefType, false, QVariantMap());
        }, Qt::QueuedConnection);
    }
    virtual void writeAccountPref(const QString &prefType,
                                  const QVariantMap &content)
    { Q_UNUSED(prefType); Q_UNUSED(content); }
    virtual void requestInvitePolicySupport() {}
    virtual void requestEditHistory(const QString &roomId, const QString &eventId)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); }
    virtual void requestEventSource(const QString &roomId, const QString &eventId)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); }
    virtual quint64 eventAtTimestamp(const QString &roomId, qint64 timestampMs)
    { Q_UNUSED(roomId); Q_UNUSED(timestampMs); return 0; }

    virtual quint64 localSearch(const QString &query, const QString &roomId,
                                int limit, int offset)
    { Q_UNUSED(query); Q_UNUSED(roomId); Q_UNUSED(limit); Q_UNUSED(offset);
      return 0; }
    virtual quint64 clearStoredMedia() { return 0; }
    virtual quint64 clearEventCache() { return 0; }
    virtual bool reportsCacheCompaction() const { return false; }
    virtual quint64 requestHomeserverInfo() { return 0; }
    virtual quint64 requestAccountDataListing(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 requestUserClients(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    virtual QString mediaKeepState() const { return {}; }
    virtual quint64 searchIndexStats() { return 0; }
    virtual quint64 sweepSearchIndex() { return 0; }
    virtual quint64 deepenSearchIndex(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual void forgetIndexedEvent(const QString &eventId) { Q_UNUSED(eventId); }
    virtual void forgetIndexedRoom(const QString &roomId) { Q_UNUSED(roomId); }
    virtual bool clearSearchIndex() { return false; }
    virtual quint64 indexAllRooms(bool resumeOnly)
    { Q_UNUSED(resumeOnly); return 0; }
    virtual quint64 pauseIndexAll() { return 0; }
    virtual quint64 cancelIndexAll() { return 0; }
    virtual quint64 requestIndexAllStatus() { return 0; }
    enum IndexAllHold : unsigned {
        IndexAllHoldCall = 1,
        IndexAllHoldScroll = 2,
    };
    virtual void setIndexAllHold(unsigned bits) { Q_UNUSED(bits); }
    virtual bool supportsLocalSearch() const { return false; }

    virtual quint64 roomWidgets(const QString &roomId, const QString &theme,
                                const QString &language)
    { Q_UNUSED(roomId); Q_UNUSED(theme); Q_UNUSED(language); return 0; }
    virtual bool supportsWidgets() const { return false; }
    virtual quint64 writeRoomWidget(const QString &roomId,
                                    const QString &widgetId,
                                    const QString &contentJson)
    { Q_UNUSED(roomId); Q_UNUSED(widgetId); Q_UNUSED(contentJson); return 0; }

    virtual quint64 roomBridges(const QString &roomId, bool allowNetwork)
    { Q_UNUSED(roomId); Q_UNUSED(allowNetwork); return 0; }
    virtual bool supportsRoomBridges() const { return false; }

    virtual quint64 requestMediaHistoryPage(const QString &roomId, int limit,
                                            bool restart)
    { Q_UNUSED(roomId); Q_UNUSED(limit); Q_UNUSED(restart); return 0; }
    virtual bool supportsMediaHistory() const { return false; }

    virtual quint64 startRoomExport(const QString &roomId, int mode,
                                    qint64 limit, qint64 fromMs, qint64 toMs)
    {
        Q_UNUSED(roomId); Q_UNUSED(mode); Q_UNUSED(limit); Q_UNUSED(fromMs);
        Q_UNUSED(toMs);
        return 0;
    }
    virtual void cancelRoomExport(quint64 opId) { Q_UNUSED(opId); }
    virtual bool supportsRoomExportWalk() const { return false; }

    virtual quint64 setRoomDisplayName(const QString &roomId,
                                       const QString &name)
    { Q_UNUSED(roomId); Q_UNUSED(name); return 0; }
    virtual quint64 setRoomMemberAvatar(const QString &roomId,
                                        const QString &mxc)
    { Q_UNUSED(roomId); Q_UNUSED(mxc); return 0; }
    virtual bool supportsRoomProfiles() const { return false; }
    virtual void probeDelayedEvents() {}
    virtual quint64 scheduleMessage(const QString &roomId, const QString &body,
                                    const QVariantMap &bodySpec,
                                    const QStringList &mentionUserIds,
                                    qint64 delayMs)
    { Q_UNUSED(roomId); Q_UNUSED(body); Q_UNUSED(bodySpec);
      Q_UNUSED(mentionUserIds); Q_UNUSED(delayMs); return 0; }
    virtual quint64 updateScheduledMessage(const QString &delayId,
                                           const QString &action)
    { Q_UNUSED(delayId); Q_UNUSED(action); return 0; }
    virtual quint64 sendRoomMessage(const QString &roomId, const QString &body,
                                    const QVariantMap &bodySpec,
                                    const QStringList &mentionUserIds,
                                    const QString &replyToEventId,
                                    const QString &threadRootEventId)
    { Q_UNUSED(roomId); Q_UNUSED(body); Q_UNUSED(bodySpec);
      Q_UNUSED(mentionUserIds); Q_UNUSED(replyToEventId);
      Q_UNUSED(threadRootEventId); return 0; }
    virtual void requestActivitySeed(int limit) { Q_UNUSED(limit); }
    virtual void requestActivitySeedPage(const QString &from, int limit)
    { Q_UNUSED(from); Q_UNUSED(limit); }
    virtual quint64 setRoomCanonicalAlias(const QString &roomId,
                                          const QString &alias)
    { Q_UNUSED(roomId); Q_UNUSED(alias); return 0; }
    virtual bool supportsPinnedMessages() const { return false; }
    virtual quint64 requestPinnedMessages(const QString &roomId,
                                          bool allowRemote)
    { Q_UNUSED(roomId); Q_UNUSED(allowRemote); return 0; }
    virtual quint64 setEventPinned(const QString &roomId,
                                   const QString &eventId, bool pin)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(pin); return 0; }
    virtual bool supportsRoomDiscovery() const { return false; }
    virtual quint64 resolveRoomTarget(const QString &input)
    { Q_UNUSED(input); return 0; }
    virtual quint64 searchPublicRooms(const QString &query,
                                      const QString &server,
                                      const QString &since, int limit)
    {
        Q_UNUSED(query); Q_UNUSED(server); Q_UNUSED(since); Q_UNUSED(limit);
        return 0;
    }
    virtual quint64 joinRoomByIdOrAlias(const QString &target,
                                        const QStringList &via)
    { Q_UNUSED(target); Q_UNUSED(via); return 0; }
    virtual quint64 knockRoom(const QString &target, const QStringList &via,
                              const QString &reason)
    { Q_UNUSED(target); Q_UNUSED(via); Q_UNUSED(reason); return 0; }
    virtual quint64 cancelKnock(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 requestSpaceChildren(const QString &spaceId)
    { Q_UNUSED(spaceId); return 0; }
    virtual bool supportsIgnoredUsers() const { return false; }
    virtual quint64 setUserIgnored(const QString &userId, bool ignored)
    { Q_UNUSED(userId); Q_UNUSED(ignored); return 0; }
    virtual quint64 requestIgnoredUsers() { return 0; }
    virtual bool supportsPushRules() const { return false; }
    virtual quint64 requestPushRules() { return 0; }
    virtual quint64 writePushRule(const QString &kind, const QString &key,
                                  const QString &value)
    { Q_UNUSED(kind); Q_UNUSED(key); Q_UNUSED(value); return 0; }
    virtual bool supportsEventReporting() const { return false; }
    virtual quint64 reportMessage(const QString &roomId,
                                  const QString &eventId,
                                  const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(reason); return 0; }
    // Voice-call signaling (MSC2746 m.call.* v1 plus
    // m.rtc.notification/decline). SDP strings are opaque inputs from the media
    // backend, never logged or echoed. Non-capable backends return 0.
    virtual bool supportsCallSignaling() const { return false; }
    virtual bool networkRouteActive() const { return false; }
    virtual quint64 callInvite(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &offerType,
                               const QString &offerSdp, quint64 lifetimeMs,
                               const QString &invitee,
                               const QString &streamMetadataJson = QString())
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(offerType); Q_UNUSED(offerSdp); Q_UNUSED(lifetimeMs);
        Q_UNUSED(invitee); Q_UNUSED(streamMetadataJson); return 0;
    }
    virtual quint64 callAnswer(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &answerType,
                               const QString &answerSdp,
                               const QString &streamMetadataJson = QString())
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(answerType); Q_UNUSED(answerSdp);
        Q_UNUSED(streamMetadataJson); return 0;
    }
    virtual quint64 callNegotiate(const QString &roomId, const QString &callId,
                                  const QString &partyId, const QString &type,
                                  const QString &sdp, quint64 lifetimeMs,
                                  const QString &streamMetadataJson)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId); Q_UNUSED(type);
        Q_UNUSED(sdp); Q_UNUSED(lifetimeMs); Q_UNUSED(streamMetadataJson);
        return 0;
    }
    virtual quint64 callStreamMetadata(const QString &roomId,
                                       const QString &callId,
                                       const QString &partyId,
                                       const QString &streamMetadataJson)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(streamMetadataJson); return 0;
    }
    virtual quint64 callReject(const QString &roomId, const QString &callId,
                               const QString &partyId)
    { Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId); return 0; }
    virtual quint64 callHangup(const QString &roomId, const QString &callId,
                               const QString &partyId, const QString &reason)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(reason); return 0;
    }
    virtual quint64 callSelectAnswer(const QString &roomId,
                                     const QString &callId,
                                     const QString &partyId,
                                     const QString &selectedPartyId)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(selectedPartyId); return 0;
    }
    virtual quint64 callRtcDecline(const QString &roomId,
                                   const QString &notificationEventId)
    { Q_UNUSED(roomId); Q_UNUSED(notificationEventId); return 0; }
    // Only while a media backend is registered does the backend carry remote
    // SDP into a bounded, C++-memory-only, single-shot store (take removes).
    // SDP must never reach QML, logs or persistence.
    virtual void setCallMediaCapable(bool capable) { Q_UNUSED(capable); }
    virtual QString takeCallSessionDescription(const QString &eventId)
    { Q_UNUSED(eventId); return {}; }
    virtual quint64 callCandidates(const QString &roomId,
                                   const QString &callId,
                                   const QString &partyId,
                                   const QVariantList &candidates)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(candidates); return 0;
    }
    virtual quint64 requestCallTurnServers() { return 0; }

    virtual bool supportsMatrixRtc() const { return false; }
    virtual quint64 rtcSession(const QString &roomId, bool preferServer = false)
    { Q_UNUSED(roomId); Q_UNUSED(preferServer); return 0; }
    virtual quint64 rtcTransports(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 rtcPublishMembership(const QString &roomId,
                                         const QString &focusUrl,
                                         const QString &intent,
                                         bool multiSfu = true)
    {
        Q_UNUSED(roomId); Q_UNUSED(focusUrl); Q_UNUSED(intent);
        Q_UNUSED(multiSfu); return 0;
    }
    virtual quint64 rtcRestartDelayedLeave(const QString &delayId)
    { Q_UNUSED(delayId); return 0; }
    virtual quint64 rtcRetractMembership(const QString &roomId,
                                         const QString &delayId)
    { Q_UNUSED(roomId); Q_UNUSED(delayId); return 0; }
    /// Distribute a media key, Olm-encrypted per device. The key is raw
    /// bytes base64'd; it is never logged and never reaches QML.
    virtual quint64 rtcSendMediaKey(const QString &roomId,
                                    const QString &keyBase64, int keyIndex,
                                    const QString &targetsJson)
    {
        Q_UNUSED(roomId); Q_UNUSED(keyBase64); Q_UNUSED(keyIndex);
        Q_UNUSED(targetsJson); return 0;
    }

    virtual bool supportsSfu() const { return false; }
    virtual quint64 sfuConnect(const QString &serviceUrl,
                               const QString &roomId)
    { Q_UNUSED(serviceUrl); Q_UNUSED(roomId); return 0; }
    virtual quint64 sfuConnectMany(const QStringList &focusUrls,
                                   const QString &roomId)
    { Q_UNUSED(focusUrls); Q_UNUSED(roomId); return 0; }
    virtual void sfuLocalDescriptionFor(quint64 generation, int connectionId,
                                        const QString &kind, const QString &target,
                                        const QString &sdp)
    { Q_UNUSED(generation); Q_UNUSED(connectionId); Q_UNUSED(kind);
      Q_UNUSED(target); Q_UNUSED(sdp); }
    virtual void sfuLocalCandidateFor(quint64 generation, int connectionId,
                                      const QString &target,
                                      const QString &candidateInit)
    { Q_UNUSED(generation); Q_UNUSED(connectionId); Q_UNUSED(target);
      Q_UNUSED(candidateInit); }
    virtual void sfuLocalDescription(const QString &kind,
                                     const QString &target,
                                     const QString &sdp)
    { Q_UNUSED(kind); Q_UNUSED(target); Q_UNUSED(sdp); }
    virtual void sfuLocalCandidate(const QString &target,
                                   const QString &candidateInit)
    { Q_UNUSED(target); Q_UNUSED(candidateInit); }
    virtual void sfuAddTrack(const QString &cid, const QString &name,
                             int kind, int width, int height,
                             bool screenShare, bool encrypted)
    {
        Q_UNUSED(cid); Q_UNUSED(name); Q_UNUSED(kind);
        Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(screenShare); Q_UNUSED(encrypted);
    }
    virtual void sfuAddTrackFor(quint64 generation, int connectionId,
                                const QString &cid, const QString &name,
                                int kind, int width, int height,
                                bool screenShare, bool encrypted)
    {
        Q_UNUSED(generation); Q_UNUSED(connectionId);
        sfuAddTrack(cid, name, kind, width, height, screenShare, encrypted);
    }
    virtual void sfuAddShareWithBackupFor(quint64 generation, const QString &cid,
                                          const QString &backupCid,
                                          int width, int height, bool encrypted)
    {
        Q_UNUSED(backupCid);
        sfuAddTrackFor(generation, 0, cid, QStringLiteral("screen"), 1,
                       width, height, true, encrypted);
    }
    virtual void sfuMuteTrack(const QString &sid, bool muted)
    { Q_UNUSED(sid); Q_UNUSED(muted); }
    virtual void sfuMuteTrackFor(quint64 generation, int connectionId,
                                 const QString &sid, bool muted)
    { Q_UNUSED(generation); Q_UNUSED(connectionId); sfuMuteTrack(sid, muted); }
    virtual void sfuDisconnect() {}

    virtual quint64 rtcNotify(const QString &roomId,
                              const QString &notificationType,
                              const QString &intent, quint64 lifetimeMs,
                              const QString &membershipEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(notificationType); Q_UNUSED(intent);
        Q_UNUSED(lifetimeMs); Q_UNUSED(membershipEventId); return 0;
    }
    virtual quint64 rtcSetHandRaised(const QString &roomId,
                                     const QString &membershipEventId,
                                     const QString &reactionEventId,
                                     bool raised)
    {
        Q_UNUSED(roomId); Q_UNUSED(membershipEventId);
        Q_UNUSED(reactionEventId); Q_UNUSED(raised); return 0;
    }
    virtual quint64 rtcSendCallReaction(const QString &roomId,
                                        const QString &membershipEventId,
                                        const QString &emoji,
                                        const QString &name)
    {
        Q_UNUSED(roomId); Q_UNUSED(membershipEventId);
        Q_UNUSED(emoji); Q_UNUSED(name); return 0;
    }
    virtual quint64 rtcReadRaisedHands(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual bool supportsDeviceDeletion() const { return false; }
    virtual quint64 deleteDevices(const QStringList &deviceIds)
    { Q_UNUSED(deviceIds); return 0; }
    virtual bool uiaSubmitPassword(quint64 uiaId, const QString &password)
    { Q_UNUSED(uiaId); Q_UNUSED(password); return false; }
    virtual void uiaCancel(quint64 uiaId) { Q_UNUSED(uiaId); }
    virtual quint64 requestOAuthManagementUrl(const QString &deviceId)
    { Q_UNUSED(deviceId); return 0; }
    virtual bool supportsPasswordChange() const { return false; }
    virtual quint64 changePassword(const QString &currentPassword,
                                   const QString &newPassword,
                                   bool logoutDevices)
    {
        Q_UNUSED(currentPassword); Q_UNUSED(newPassword);
        Q_UNUSED(logoutDevices); return 0;
    }
    virtual quint64 probePasswordChange() { return 0; }
    virtual bool supportsRoomKeyExport() const { return false; }
    virtual quint64 exportRoomKeys(const QString &filePath,
                                   const QString &passphrase)
    {
        Q_UNUSED(filePath); Q_UNUSED(passphrase); return 0;
    }
    virtual bool supportsMessageSearch() const { return false; }
    virtual quint64 searchMessages(const QString &term, const QString &roomId,
                                   const QString &nextBatch, int limit,
                                   const QVariantMap &filters = {})
    {
        Q_UNUSED(term); Q_UNUSED(roomId); Q_UNUSED(nextBatch); Q_UNUSED(limit);
        Q_UNUSED(filters);
        return 0;
    }
    virtual quint64 addRoomToSpace(const QString &spaceId, const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }
    virtual quint64 removeRoomFromSpace(const QString &spaceId,
                                        const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }
    virtual quint64 setSpaceChildOrder(const QString &spaceId, const QStringList &roomIds)
    { Q_UNUSED(spaceId); Q_UNUSED(roomIds); return 0; }
    virtual quint64 setSpaceChildSuggested(const QString &spaceId,
                                           const QString &roomId,
                                           bool suggested)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); Q_UNUSED(suggested); return 0; }

    virtual quint64 sendAttachment(const QString &roomId,
                                   const QString &localPath,
                                   const QString &mime,
                                   const QString &caption,
                                   int width, int height, bool animated,
                                   qint64 durationMs,
                                   const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(caption); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(animated); Q_UNUSED(durationMs); Q_UNUSED(replyToEventId);
        return 0;
    }
    virtual quint64 sendVideo(const QString &roomId,
                              const QString &localPath,
                              const QString &mime,
                              const QString &caption,
                              int width, int height, qint64 durationMs,
                              const QByteArray &thumbnail,
                              int thumbnailWidth, int thumbnailHeight,
                              const QString &replyToEventId)
    {
        Q_UNUSED(durationMs); Q_UNUSED(thumbnail);
        Q_UNUSED(thumbnailWidth); Q_UNUSED(thumbnailHeight);
        return sendAttachment(roomId, localPath, mime, caption, width, height,
                              false, 0, replyToEventId);
    }
    virtual quint64 sendImageWithThumbnail(const QString &roomId,
                                           const QString &localPath,
                                           const QString &mime,
                                           const QString &caption,
                                           int width, int height,
                                           const QByteArray &thumbnail,
                                           int thumbnailWidth,
                                           int thumbnailHeight,
                                           const QString &replyToEventId)
    {
        Q_UNUSED(thumbnail); Q_UNUSED(thumbnailWidth);
        Q_UNUSED(thumbnailHeight);
        return sendAttachment(roomId, localPath, mime, caption, width, height,
                              false, 0, replyToEventId);
    }
    virtual quint64 sendAttachmentBytes(const QString &roomId,
                                        const QByteArray &bytes,
                                        const QString &filename,
                                        const QString &mime,
                                        int width, int height,
                                        const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(bytes); Q_UNUSED(filename);
        Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(replyToEventId);
        return 0;
    }
    virtual bool supportsRoomScopedAttachmentSend() const { return false; }
    // MSC4193 spoiler media. localPath empty sends `bytes`; threadRootId empty
    // sends to the room. 0 is a refusal: never fall back to an unmarked send.
    virtual bool supportsSpoilerMedia() const { return false; }
    virtual quint64 sendSpoilerMedia(const QString &roomId,
                                     const QString &threadRootId,
                                     const QString &localPath,
                                     const QByteArray &bytes,
                                     const QString &filename,
                                     const QString &mime,
                                     const QString &caption,
                                     int width, int height, qint64 durationMs,
                                     const QByteArray &thumbnail,
                                     int thumbnailWidth, int thumbnailHeight,
                                     const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(threadRootId); Q_UNUSED(localPath);
        Q_UNUSED(bytes); Q_UNUSED(filename); Q_UNUSED(mime); Q_UNUSED(caption);
        Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(durationMs);
        Q_UNUSED(thumbnail); Q_UNUSED(thumbnailWidth);
        Q_UNUSED(thumbnailHeight); Q_UNUSED(replyToEventId);
        return 0;
    }
    virtual quint64 sendAttachmentBytesToRoom(const QString &roomId,
                                              const QByteArray &bytes,
                                              const QString &filename,
                                              const QString &mime,
                                              int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(bytes); Q_UNUSED(filename);
        Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }
    virtual quint64 sendVoiceMessage(const QString &roomId,
                                     const QString &localPath,
                                     const QString &mime,
                                     qint64 durationMs,
                                     const QList<int> &waveform,
                                     const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(durationMs); Q_UNUSED(waveform); Q_UNUSED(replyToEventId);
        return 0;
    }
    // Thread twin of sendVoiceMessage, sent through the SDK's thread-focused
    // timeline as a real m.thread reply. 0 is a refusal, never a licence to
    // fall back to a room send.
    virtual quint64 sendThreadVoiceMessage(const QString &roomId,
                                           const QString &rootEventId,
                                           const QString &localPath,
                                           const QString &mime,
                                           qint64 durationMs,
                                           const QList<int> &waveform,
                                           const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(durationMs); Q_UNUSED(waveform);
        Q_UNUSED(replyToEventId);
        return 0;
    }

    // Attachment into a thread, through the SDK's thread-focused timeline so
    // the relation and encryption are the SDK's; never an ordinary room send.
    // Answers on attachmentQueueFinished; 0 when unsupported.
    virtual quint64 sendThreadAttachment(const QString &roomId,
                                         const QString &rootEventId,
                                         const QString &localPath,
                                         const QString &mime,
                                         const QString &caption,
                                         int width, int height, bool animated,
                                         qint64 durationMs,
                                         const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(caption); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(animated); Q_UNUSED(durationMs);
        Q_UNUSED(replyToEventId);
        return 0;
    }
    virtual quint64 sendThreadImageWithThumbnail(const QString &roomId,
                                                 const QString &rootEventId,
                                                 const QString &localPath,
                                                 const QString &mime,
                                                 const QString &caption,
                                                 int width, int height,
                                                 const QByteArray &thumbnail,
                                                 int thumbnailWidth,
                                                 int thumbnailHeight,
                                                 const QString &replyToEventId)
    {
        Q_UNUSED(thumbnail); Q_UNUSED(thumbnailWidth);
        Q_UNUSED(thumbnailHeight);
        return sendThreadAttachment(roomId, rootEventId, localPath, mime,
                                    caption, width, height, false, 0,
                                    replyToEventId);
    }
    virtual quint64 sendThreadVideo(const QString &roomId,
                                    const QString &rootEventId,
                                    const QString &localPath,
                                    const QString &mime,
                                    const QString &caption,
                                    int width, int height, qint64 durationMs,
                                    const QByteArray &thumbnail,
                                    int thumbnailWidth, int thumbnailHeight,
                                    const QString &replyToEventId)
    {
        Q_UNUSED(durationMs); Q_UNUSED(thumbnail);
        Q_UNUSED(thumbnailWidth); Q_UNUSED(thumbnailHeight);
        return sendThreadAttachment(roomId, rootEventId, localPath, mime,
                                    caption, width, height, false, 0,
                                    replyToEventId);
    }
    virtual quint64 sendThreadAttachmentBytes(const QString &roomId,
                                              const QString &rootEventId,
                                              const QByteArray &bytes,
                                              const QString &filename,
                                              const QString &mime,
                                              int width, int height,
                                              const QString &replyToEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(bytes);
        Q_UNUSED(filename); Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(replyToEventId);
        return 0;
    }

    virtual quint64 fetchMedia(const QString &mediaKey, int kind,
                               int timeoutClass = 0)
    { Q_UNUSED(mediaKey); Q_UNUSED(kind); Q_UNUSED(timeoutClass); return 0; }
    virtual quint64 fetchMxcThumbnail(const QString &mxc, int width, int height)
    { Q_UNUSED(mxc); Q_UNUSED(width); Q_UNUSED(height); return 0; }
    virtual quint64 fetchLinkMedia(const QString &url, const QString &linkKey,
                                   int expect, int timeoutClass = 0)
    {
        Q_UNUSED(url); Q_UNUSED(linkKey); Q_UNUSED(expect);
        Q_UNUSED(timeoutClass);
        return 0;
    }
    virtual void cancelMediaFetch(quint64 opId) { Q_UNUSED(opId); }

    virtual qint64 maxUploadSize() const { return 0; }

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();
    void authMethodsDiscovered(const QString &homeserver,
                               bool password,
                               bool oauth,
                               bool sso);
    void authDiscoveryDetails(const QString &homeserver, const QVariantMap &details);
    void oauthBrowserUrlReady(const QString &url);
    void ssoBrowserUrlReady(const QString &url);
    void browserLaunchFailed();
    // m.login.sso providers as {id, name, icon} maps; empty with sso true means
    // one unnamed flow. `icon` is an mxc: URI or empty, never http, so the
    // login screen cannot be made to fetch from a server-chosen host.
    void ssoProvidersReceived(const QString &homeserver, bool sso,
                              const QVariantList &providers);

    void connectionStateChanged(ConnectionState state);
    void homeserverReachable();
    void initialSyncDoneChanged();
    void syncModeChanged();
    void roomsChanged();
    void roomUpdated(const QString &roomId);
    void timelineReset(const QString &roomId);
    void threadTimelineFailed(const QString &roomId,
                              const QString &rootEventId,
                              const QString &category);
    void eventContextFailed(const QString &roomId, const QString &eventId,
                            const QString &category);
    void eventContextPagination(const QString &roomId, const QString &eventId,
                                bool forward, const QString &state,
                                bool reachedEdge);
    void threadListUpdated(const QString &roomId, const QVariantList &threads,
                           bool endReached, bool failed);
    void threadSubscriptionState(const QString &roomId,
                                 const QString &rootEventId, bool supported,
                                 bool subscribed, bool automatic);
    void threadSubscriptionResult(const QString &roomId,
                                  const QString &rootEventId, bool ok,
                                  bool subscribed);
    void eventAppended(const QString &roomId, const TimelineEvent &event);
    void liveEventObserved(const QString &roomId, const TimelineEvent &event);
    void eventDecrypted(const QString &roomId, const TimelineEvent &event);
    void roomHighlightsUnnotified(const QString &roomId, int count);
    void eventStatusChanged(const QString &roomId,
                            const QString &eventId,
                            TimelineEvent::Status status);

    void eventReplaced(const QString &roomId,
                       const QString &oldEventId,
                       const TimelineEvent &newEvent);

    void eventInsertedAt(const QString &roomId, int index,
                         const TimelineEvent &event);
    void eventsInsertedAt(const QString &roomId, int index,
                          const QList<TimelineEvent> &events);
    void eventChangedAt(const QString &roomId, int index,
                        const TimelineEvent &event);
    void eventRemovedAt(const QString &roomId, int index);
    void eventsTruncatedTo(const QString &roomId, int length);
    void eventEdited(const QString &roomId, const QString &eventId);
    void eventRedacted(const QString &roomId, const QString &eventId);
    void redactionFinished(const QString &roomId, const QString &eventId,
                           bool ok, const QString &category);
    void reactionsChanged(const QString &roomId, const QString &eventId);
    void eventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void paginationStateChanged(const QString &roomId);
    void typingChanged(const QString &roomId);
    void membersChanged(const QString &roomId);
    void roomMembersEvicted(const QString &roomId);
    void roomMemberEventSeen(const QString &roomId);
    void roomMembershipLost(const QString &roomId, const QString &loss,
                            const QString &roomName);
    void directPeerAvatarChanged(const QString &roomId, const QString &userId,
                                 const QString &avatarUrl);

    void roomNotificationModeChanged(const QString &roomId, int mode,
                                     bool userDefined);
    void roomNotificationModeWriteFailed(const QString &roomId);
    void readMarkerAdvanced(const QString &roomId);
    void markRoomReadFailed(const QString &roomId);
    void readReceiptAccepted(const QString &roomId, const QString &eventId);
    void readReceiptFailed(const QString &roomId, const QString &eventId);
    void threadReadMarkerAdvanced(const QString &roomId,
                                  const QString &rootEventId, quint64 opId);
    void threadMarkReadFailed(const QString &roomId, const QString &rootEventId,
                              quint64 opId);
    void pushRulesChanged();
    void roomNotificationModeCleared(const QString &roomId);

    void errorOccurred(const QString &message);

    void userSearchFinished(quint64 opId, bool ok,
                            const QVariantList &results, bool limited,
                            const QString &category);
    void userProfileFinished(quint64 opId, bool ok, const QString &userId,
                             const QString &displayName,
                             const QString &avatarUrl,
                             const QString &category);
    void ownDisplayNameChanged(quint64 opId, bool ok, const QString &error);
    void ownAvatarChanged(quint64 opId, bool ok, const QString &error);
    void mutualRoomsReceived(quint64 opId, const QString &userId,
                             const QVariantList &rooms);
    void urlPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                            const QString &category, int httpStatus = 0,
                            int redirectCount = 0);
    void gifResponse(quint64 opId, bool ok, int httpStatus,
                     const QByteArray &body, const QString &category);
    void gifDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                             const QString &mime, int width, int height,
                             qint64 size, const QString &category);
    void dmCreateFinished(quint64 opId, bool ok, const QString &roomId,
                          const QString &category);
    void roomCreateFinished(quint64 opId, bool ok, const QString &roomId,
                            const QString &category, const QString &warning);
    void inviteUserFinished(quint64 opId, const QString &roomId,
                            const QString &userId, bool ok,
                            const QString &category);
    void inviteBatchFinished(quint64 opId, const QString &roomId,
                             int okCount, int failCount);
    void roomMembersReceived(quint64 opId, const QString &roomId,
                             const QVariantMap &snapshot);
    void roomOwnPermissionsReceived(quint64 opId, const QString &roomId,
                                    const QVariantMap &facts);
    void threadParticipantsReceived(const QString &roomId,
                                    const QString &rootEventId,
                                    const QVariantList &participants,
                                    int distinct, bool truncated);
    void messageEditsRemoved(const QString &roomId, const QString &eventId,
                             bool ok, int removed, int failed,
                             bool truncated);
    void presenceReceived(quint64 opId, const QVariantList &entries);
    void profileBannerReceived(quint64 opId, const QString &userId,
                               const QString &mxc, bool supported);
    void profileBannerSet(quint64 opId, bool ok, const QString &mxc,
                          const QString &category);
    void nameColorReceived(quint64 opId, const QString &userId,
                           const QString &color, bool supported);
    void nameColorFailed(quint64 opId, const QString &userId);
    void userProfileChanged(const QString &userId);
    void nameColorSet(quint64 opId, bool ok, const QString &color,
                      const QString &category);
    void profileBioReceived(quint64 opId, const QString &userId,
                            const QString &bio, bool supported);
    void profileBioSet(quint64 opId, bool ok, const QString &bio,
                       const QString &category);
    void roomBannerReceived(quint64 opId, const QString &roomId,
                            const QString &mxc, bool canSet);
    void roomBannerSet(quint64 opId, const QString &roomId, bool ok,
                       const QString &mxc, const QString &category);
    void roomBackgroundReceived(quint64 opId, const QString &roomId,
                                const QVariantMap &content, bool canSet,
                                bool unsupportedVersion);
    void roomBackgroundSet(quint64 opId, const QString &roomId, bool ok,
                           const QVariantMap &content,
                           const QString &category);
    void roomBackgroundChanged(const QString &roomId);
    void personalBackgroundsRead(quint64 opId, const QString &scope,
                                 const QVariantList &entries,
                                 const QStringList &failed, int enabled);
    void personalBackgroundDownloaded(quint64 opId, const QString &scope,
                                      bool ok, const QVariantMap &entry,
                                      const QByteArray &bytes,
                                      const QString &category);
    void personalBackgroundWritten(quint64 opId, const QString &scope,
                                   bool ok, const QVariantMap &entry,
                                   const QString &category);
    void personalBackgroundsCleared(quint64 opId, bool ok, int cleared,
                                    int failed, int skipped, bool switchedOff);
    void syncedSettingsRead(quint64 opId, bool ok, const QString &category,
                            const QString &blobState, const QString &blobJson,
                            int switchState);
    void syncedSettingsWritten(quint64 opId, int kind, bool ok,
                               const QString &category);
    void syncedSettingsChanged();
    void personalBackgroundChanged(const QString &scope);
    void stickerPacksReceived(quint64 opId, const QString &roomId,
                              bool roomCanManage, const QVariantList &packs);
    void stickerPackSnapshotMeta(quint64 opId, const QVariantMap &meta);
    void policyRulesReceived(quint64 opId, bool ok, const QString &roomId,
                             bool canWrite, bool truncated,
                             const QVariantList &rules);
    void policyRuleWritten(quint64 opId, bool ok, const QString &category);
    void policySubscriptionsReceived(quint64 opId, bool ok,
                                     const QString &category,
                                     const QStringList &rooms);
    void policyCheckFinished(quint64 opId, const QString &entity, bool matched,
                             const QVariantMap &detail);
    void qrLoginProgress(quint64 generation, const QString &step,
                         const QVariantMap &detail);
    void stickerPackEditFinished(quint64 opId, bool ok, const QString &category,
                                 const QString &shortcode);
    void stickerPackUploaded(quint64 opId, const QString &mxcUrl);
    void stickerPackAddFinished(quint64 opId, bool ok, const QString &category,
                                const QString &shortcode);
    void stickerPackRoomsSet(quint64 opId, bool ok, const QString &category,
                             const QString &roomId, const QString &stateKey,
                             bool enabled, const QString &mirror);
    void stickerPackMigrated(quint64 opId, bool ok, const QString &category,
                             const QVariantMap &detail);
    void stickerPacksChanged(const QString &roomId);
    void presencePublishFailed(const QString &category, qint64 retryAfterMs);
    void presencePublished();
    void roomEditFinished(quint64 opId, const QString &roomId,
                          const QString &field, bool ok,
                          const QString &category);
    void roomDirectoryVisibilityReceived(const QString &roomId, bool ok,
                                         bool published);
    void roomVersionsReceived(bool ok, const QString &defaultVersion,
                              const QVariantList &available);
    void roomUpgradeFinished(quint64 opId, const QString &roomId, bool ok,
                             const QString &replacementRoomId,
                             const QString &category);
    void deviceRenamed(quint64 opId, bool ok, const QString &category);
    // SENSITIVE: recoveryKey is real secret material when non-empty.
    // Consumers display it once and clear it; nothing may log it.
    void backupActionFinished(quint64 opId, const QString &action, bool ok,
                              const QString &recoveryKey,
                              const QString &category);
    void crossSigningApprovalRequired(quint64 opId, const QString &url);
    void backupProgress(const QString &backupState, const QString &uploadState,
                        qint64 backedUp, qint64 total, quint64 requestId = 0);
    void accountPrefRead(const QString &prefType, bool present,
                         const QVariantMap &content);
    void accountPrefChanged(const QString &prefType);
    void accountPrefReadFailed(const QString &prefType, const QString &category);
    void accountPrefWritten(const QString &prefType, bool ok,
                            const QString &category);
    void roomActionFailed(const QString &action, const QString &roomId);
    void invitePolicySupportReceived(bool ok, bool stable, bool unstable);
    void editHistoryReceived(const QString &roomId, const QString &eventId,
                             bool ok, bool partial,
                             const QVariantList &revisions);
    void eventSourceReceived(const QString &roomId, const QString &eventId,
                             bool ok, const QString &json,
                             const QVariantMap &encryption);
    void eventAtTimestampReceived(quint64 opId, const QString &roomId, bool ok,
                                  const QString &eventId, qint64 timestampMs,
                                  const QString &category);
    void localSearchFinished(quint64 opId, bool ok, const QString &category,
                             int minChars, const QVariantList &results);
    void roomWidgetsReceived(quint64 opId, const QString &roomId, bool ok,
                             bool canManage, const QVariantList &widgets);
    void roomWidgetWritten(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    void roomBridgesReceived(quint64 opId, const QString &roomId, bool ok,
                             const QVariantList &bridges);
    void mediaHistoryPage(quint64 opId, const QString &roomId,
                          const QVariantList &entries, qint64 scanned,
                          qint64 scannedTotal, qint64 undecryptable,
                          bool complete, bool encryptedRoom);
    void mediaHistoryFailed(quint64 opId, const QString &roomId,
                            const QString &message);
    void roomExportPage(quint64 opId, const QString &roomId,
                        const QVariantList &rows, qint64 rowsTotal,
                        qint64 scannedTotal, qint64 undecryptableTotal,
                        bool done, const QString &stopReason,
                        bool encryptedRoom);
    void roomExportFailed(quint64 opId, const QString &roomId,
                          const QString &message);
    void roomProfileResult(quint64 opId, const QString &roomId,
                           const QString &field, bool ok,
                           const QString &error);
    void storedMediaCleared(quint64 opId, bool ok, qint64 files, qint64 bytes);
    void mediaKeepStateChanged();
    void searchIndexStatsReceived(quint64 opId, qint64 messages, qint64 rooms);
    void searchIndexSwept(quint64 opId, int rooms, int written,
                          qint64 messages, qint64 indexedRooms);
    void searchIndexDeepened(quint64 opId, bool ok, const QString &roomId,
                             int pages, bool reachedStart, int written,
                             qint64 messages, const QString &category);
    void searchIndexAllProgress(quint64 opId, const QVariantMap &status);
    void delayedEventsSupportReceived(bool supported, bool advertised);
    void scheduledSendFinished(quint64 opId, const QString &roomId, bool ok,
                               const QString &delayId, const QString &category);
    void scheduledUpdateFinished(quint64 opId, const QString &delayId,
                                 const QString &action, bool ok,
                                 const QString &category);
    void roomSendFinished(quint64 opId, const QString &roomId, bool ok,
                          const QString &category);
    void reactionEventReceived(const QString &roomId, const QString &reactionEventId,
                               const QString &targetEventId, const QString &senderId,
                               const QString &key, qint64 timestampMs,
                               bool targetIsOwn = false, bool backlog = false);
    void activitySeedReceived(const QVariantList &entries);
    void activitySeedCursorReceived(const QString &nextToken);
    void activitySeedPageReceived(const QVariantList &entries,
                                  const QString &nextToken);
    void activitySeedPageFailed(const QString &category);
    void activitySeedFailed(const QString &category = QString());
    void inviteAnswered(const QString &roomId, bool accepted);
    void roomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    void moderationFinished(quint64 opId, const QString &roomId,
                            const QString &userId, const QString &op,
                            bool ok, const QString &category);
    void moderationPlanReceived(quint64 opId, const QString &userId,
                                const QString &op, bool truncated,
                                const QVariantList &rooms);
    void roomClosurePlanReceived(quint64 opId, bool truncated,
                                 const QVariantList &rooms);
    void roomClosureProgress(quint64 opId, const QString &roomId, int done,
                             int total);
    void roomClosureFinished(quint64 opId, const QString &roomId,
                             const QVariantMap &result);
    void homeserverInfoReceived(quint64 opId, const QVariantMap &info);
    void accountDataListed(quint64 opId, const QVariantMap &listing);
    void userClientsListed(quint64 opId, const QVariantMap &result);
    void eventCacheCleared(quint64 opId, bool ok);
    void cacheCompacted(quint64 opId, const QString &store,
                        const QString &outcome, qint64 bytesBefore,
                        qint64 bytesAfter);
    void serverAdminStatusReceived(quint64 opId, bool admin,
                                   const QString &detail);
    void adminRoomDeleteProgress(quint64 opId, const QString &roomId,
                                 const QString &status);
    void adminRoomDeleteFinished(quint64 opId, const QString &roomId,
                                 const QVariantMap &result);
    void powerLevelChangeFinished(quint64 opId, const QString &roomId,
                                  const QString &userId, qlonglong level,
                                  bool ok, const QString &category);
    void roomPowerMatrixFinished(quint64 opId, const QString &roomId,
                                 const QString &key, qlonglong level,
                                 bool ok, const QString &category);
    void pinnedReceived(quint64 opId, const QString &roomId,
                        const QVariantMap &snapshot);
    void pinChangeFinished(quint64 opId, const QString &roomId,
                           const QString &eventId, bool pin, bool ok,
                           bool changed, const QString &category);
    void pinnedEventsChanged(const QString &roomId);
    void roomTargetResolved(quint64 opId, const QVariantMap &result);
    void publicRoomsReceived(quint64 opId, bool ok, const QVariantList &rooms,
                             const QString &nextBatch, quint64 totalEstimate,
                             const QString &category);
    void roomJoinFinished(quint64 opId, bool ok, const QString &roomId,
                          const QString &category);
    void roomKnockFinished(quint64 opId, bool ok, const QString &roomId,
                           const QString &category);
    void knockCancelFinished(quint64 opId, bool ok, const QString &roomId,
                             const QString &category);
    void ignoreUserFinished(quint64 opId, const QString &userId, bool ignored,
                            bool ok, const QString &category);
    void ignoredUsersReceived(quint64 opId, bool ok, const QStringList &users);
    void ignoredUsersChanged(const QStringList &users);
    void eventCacheRepaired();
    void pushRulesReceived(quint64 opId, bool ok, const QVariantMap &snapshot,
                           const QString &category);
    void pushRuleWriteFinished(quint64 opId, const QString &kind, bool ok,
                               const QString &category);
    void reportMessageFinished(quint64 opId, const QString &roomId,
                               const QString &eventId, bool ok,
                               const QString &category);
    void callSignalReceived(const CallSignal &signal);
    void callSendFinished(quint64 opId, bool ok, const QString &category,
                          const QString &callId, const QString &eventId);
    void callCandidatesReceived(const QString &roomId, const QString &callId,
                                const QString &partyId, const QString &sender,
                                bool own,
                                const QVariantList &candidates);
    // Short-lived TURN credentials: engine-only, never logged.
    void callTurnServersReceived(quint64 opId, bool ok,
                                 const QString &username,
                                 const QString &password,
                                 const QStringList &uris, qint64 ttlSeconds,
                                 const QString &category);

    void rtcSessionReceived(quint64 opId, const RtcSessionData &session);
    void rtcSessionChanged(const QString &roomId);
    void rtcTransportsReceived(quint64 opId, bool serverAnswered,
                               const QString &category,
                               const QStringList &serverServiceUrls,
                               const QString &participantFocusUrl);
    void rtcSendFinished(quint64 opId, bool ok, const QString &category,
                         const QString &eventId);
    void rtcHandResult(quint64 opId, bool ok, bool raised,
                       const QString &category, const QString &eventId);
    void rtcHandChanged(const QString &roomId, const QString &sender,
                        const QString &membershipEventId,
                        const QString &reactionEventId, bool raised);
    void rtcCallReactionReceived(const QString &roomId, const QString &sender,
                                 const QString &membershipEventId,
                                 const QString &emoji);
    void rtcHandsReceived(quint64 opId, const QString &roomId,
                          const QVariantList &hands);
    void rtcMembershipPublished(quint64 opId, bool ok, const QString &category,
                                const QString &eventId,
                                const QString &delayId,
                                const QString &delayedCategory);
    void rtcMembershipRetracted(quint64 opId, bool ok,
                                const QString &category);
    void rtcMediaKeySent(quint64 opId, bool ok, const QString &category,
                         int delivered, int keyIndex,
                         const QVariantList &undelivered = {});
    /// A media key from another device, already Olm-decrypted. `sender` is
    /// vouched for by the SDK; `claimedDeviceId` is only a claim. The key is
    /// base64 raw bytes: C++ memory only, never QML or logs.
    void rtcMediaKeyReceived(const QString &roomId, const QString &sender,
                             const QString &claimedDeviceId, int keyIndex,
                             const QString &keyBase64);

    void sfuStateChanged(const QString &state, const QString &category);
    void sfuConnectionEvent(const QVariantMap &event);
    /// LiveKit's per-room server-injected-frame trailer
    /// (`JoinResponse.sif_trailer`): at most 64 raw bytes, empty when absent.
    /// It marks the unencrypted blank frames the SFU writes into encrypted
    /// tracks; see SfuMediaEngine::framesServerInjected().
    void sfuJoined(const QString &identity, const QVariantList &participants,
                   const QVariantList &iceServers,
                   const QByteArray &sifTrailer);
    void sfuParticipantsChanged(const QVariantList &participants);
    void sfuTrackPublished(const QString &cid, const QString &sid);
    void sfuSpeakersChanged(const QVariantList &speakers);
    void sfuConnectionQuality(const QVariantList &updates);
    void sfuRemoteDescription(const QString &kind, const QString &target,
                              const QString &sdp);
    void sfuRemoteCandidate(const QString &target,
                            const QString &candidateInit);
    void uiaRequired(quint64 uiaId, bool hasPasswordStage,
                     bool wrongPassword, const QStringList &stages);
    void deviceDeleteFinished(quint64 opId, bool ok, const QString &category);
    void oauthManagementUrlReceived(quint64 opId, bool ok, const QString &url);
    void passwordChangeProbed(quint64 opId, bool known, bool canChange,
                              const QString &managementUrl);
    void passwordChangeFinished(quint64 opId, bool ok, const QString &category);
    void roomKeyExportProgress(quint64 opId, int collected);
    void roomKeyExportFinished(quint64 opId, bool ok, int exported,
                               const QString &category);
    void messageSearchFinished(quint64 opId, bool ok,
                               const QVariantList &results,
                               const QString &nextBatch, quint64 count,
                               const QString &category);
    void spaceChildrenReceived(quint64 opId, const QString &spaceId, bool ok,
                               const QVariantList &rooms, bool truncated,
                               const QString &category);
    void spaceChildFinished(quint64 opId, const QString &spaceId,
                            const QString &roomId, bool ok);
    void spaceChildRemoveFinished(quint64 opId, const QString &spaceId,
                                  const QString &roomId, bool ok);
    void spaceChildOrderFinished(quint64 opId, const QString &spaceId, bool ok);
    void spaceChildSuggestedFinished(quint64 opId, const QString &spaceId,
                                     const QString &roomId, bool suggested,
                                     bool ok);
    void attachmentQueueFinished(quint64 opId, const QString &roomId,
                                 bool ok, const QString &category);
    void mediaReady(quint64 opId, const QString &mediaKey, int kind,
                    const QByteArray &bytes, const QString &mimetype,
                    const QString &filename);
    void mediaFailed(quint64 opId, const QString &mediaKey, int kind,
                     const QString &category);
    void maxUploadSizeChanged();
};
