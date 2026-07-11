#pragma once

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

// Pure C++ backend interface for Matrix operations. UI/models depend only on
// this — never on a concrete backend. Concrete implementations live in
// MockMatrixClient (v0.1 shell / --mock), CppHttpMatrixClient (v0.2+ default),
// and a future RustSdkMatrixClient (v0.4) that will wrap the Matrix Rust SDK
// via FFI for E2EE and sliding sync.
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

    // Session lifecycle
    virtual void login(const QString &homeserver,
                       const QString &user,
                       const QString &password) = 0;
    virtual void logout() = 0;
    virtual bool restoreSession() = 0;
    virtual bool isLoggedIn() const = 0;
    virtual QString currentUserId() const = 0;
    virtual QString homeserverUrl() const = 0;

    // Sync
    virtual void startSync() = 0;
    virtual void stopSync() = 0;
    virtual ConnectionState connectionState() const = 0;

    // v0.4.6: true once at least one /sync response has been processed for
    // the current session. Backends that synthesise state immediately (Mock)
    // return true by default; only backends that talk to a real homeserver
    // need to override and toggle this. QML consumes it to distinguish
    // "still loading rooms" from "sync loop is live but there are no rooms".
    virtual bool initialSyncDone() const { return true; }
    virtual QString syncMode() const { return QStringLiteral("classic_fallback"); }

    // Room + timeline queries
    virtual QList<RoomInfo> rooms() const = 0;
    virtual QList<TimelineEvent> timeline(const QString &roomId) const = 0;

    // Member lookup: display name and avatar mxc. Fallback = MXID / empty.
    virtual QString displayNameFor(const QString &roomId, const QString &userId) const = 0;
    virtual QString avatarMxcFor(const QString &roomId, const QString &userId) const = 0;
    virtual QStringList typingUsersFor(const QString &roomId) const = 0;

    // Media URL helpers. These return authenticated HTTP URLs suitable for
    // <img src="..."> or QDesktopServices::openUrl.
    virtual QUrl mediaDownloadUrl(const QString &mxcUrl) const = 0;
    virtual QUrl mediaThumbnailUrl(const QString &mxcUrl,
                                   int width, int height,
                                   bool crop = false) const = 0;

    // Sending
    virtual void sendTextMessage(const QString &roomId, const QString &body) = 0;
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body) = 0;

    // v0.4.1: reply into a thread rooted at `threadRootEventId`. Default
    // falls back to sendReply — the HTTP backend still delivers the message
    // and it's marked as an in-reply-to on the server. Concrete backends
    // (Mock; later CppHttp v0.5) may override to attach an `m.thread`
    // relation so ThreadManager sees a proper thread grouping.
    virtual void sendThreadReply(const QString &roomId,
                                 const QString &threadRootEventId,
                                 const QString &body)
    {
        sendReply(roomId, threadRootEventId, body);
    }
    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody) = 0;
    virtual void redactEvent(const QString &roomId,
                             const QString &eventId,
                             const QString &reason = QString()) = 0;
    virtual void toggleReaction(const QString &roomId,
                                const QString &targetEventId,
                                const QString &key) = 0;
    virtual void sendTyping(const QString &roomId,
                            bool isTyping,
                            int timeoutMs = 20000) = 0;
    virtual void sendReadReceipt(const QString &roomId,
                                 const QString &eventId) = 0;
    virtual void setRoomMarkedUnread(const QString &roomId, bool unread)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(unread);
    }
    virtual void acceptInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void rejectInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void sendImage(const QString &roomId, const QString &localPath) = 0;
    virtual void sendFile(const QString &roomId, const QString &localPath) = 0;

    // Pagination
    virtual void loadOlderMessages(const QString &roomId) = 0;
    virtual bool canPaginate(const QString &roomId) const = 0;
    virtual bool paginating(const QString &roomId) const = 0;

    // v0.5.7: true when the last backward pagination for this room failed
    // and can be retried. Backends without failure tracking return false.
    virtual bool paginationFailed(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }

    // v0.5.7: retry a failed outgoing message identified by its send-queue
    // transaction id. Only the Rust backend (SDK local echoes) implements
    // this; the default is a no-op so HTTP/Mock behavior is unchanged.
    virtual void retryFailedSend(const QString &roomId,
                                 const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    // ---- v0.5.9: conversation creation, membership, room editing, media.
    //
    // Command methods return an operation id (> 0) echoed on the matching
    // *Finished signal, or 0 when the backend does not support the
    // operation. Defaults are inert so Mock/HTTP builds keep working; the
    // UI hides or disables unsupported actions via supportsRoomManagement /
    // supportsAttachmentSend.
    virtual bool supportsRoomManagement() const { return false; }
    virtual bool supportsAttachmentSend() const { return false; }
    virtual bool supportsMediaBridge() const { return false; }

    virtual quint64 searchUsers(const QString &query, int limit)
    { Q_UNUSED(query); Q_UNUSED(limit); return 0; }
    // v0.5.11: exact profile lookup for one full Matrix user id. Confirms
    // (or refutes) a bare-localpart candidate the directory may not list.
    virtual quint64 fetchUserProfile(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // v0.5.11: homeserver URL preview (the homeserver fetches the target;
    // the client never does). Backends without support return 0.
    virtual bool supportsUrlPreview() const { return false; }
    virtual quint64 fetchUrlPreview(const QString &url)
    { Q_UNUSED(url); return 0; }
    // Existing joined DM rooms for a user, from authoritative m.direct.
    // Each entry: {roomId, name}. Synchronous store lookup.
    virtual QVariantList existingDirectRooms(const QString &userId) const
    { Q_UNUSED(userId); return {}; }
    virtual quint64 createDirectChat(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // options: name, topic, public(bool), encrypted(bool), alias,
    // invites(QStringList), spaceId.
    virtual quint64 createRoom(const QVariantMap &options)
    { Q_UNUSED(options); return 0; }
    virtual quint64 inviteUsers(const QString &roomId, const QStringList &userIds)
    { Q_UNUSED(roomId); Q_UNUSED(userIds); return 0; }
    virtual quint64 requestRoomMembers(const QString &roomId)
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
    virtual quint64 addRoomToSpace(const QString &spaceId, const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }

    // Attachment sending (Rust: SDK send queue with local echo). `mime` is
    // detected by the caller from file content, not just the extension.
    virtual quint64 sendAttachment(const QString &roomId,
                                   const QString &localPath,
                                   const QString &mime,
                                   const QString &caption,
                                   int width, int height, bool animated)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(caption); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(animated);
        return 0;
    }
    // Clipboard images: bytes transfer directly, no temporary file.
    virtual quint64 sendAttachmentBytes(const QString &roomId,
                                        const QByteArray &bytes,
                                        const QString &filename,
                                        const QString &mime,
                                        int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(bytes); Q_UNUSED(filename);
        Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }

    // Media bridge: fetch (and decrypt, inside the SDK) media bytes for a
    // timeline item media key. kind: 0 = full, 1 = thumbnail.
    virtual quint64 fetchMedia(const QString &mediaKey, int kind)
    { Q_UNUSED(mediaKey); Q_UNUSED(kind); return 0; }
    // Server-side thumbnail of a plain mxc URI (avatars).
    virtual quint64 fetchMxcThumbnail(const QString &mxc, int width, int height)
    { Q_UNUSED(mxc); Q_UNUSED(width); Q_UNUSED(height); return 0; }

    // Server upload limit in bytes; 0 while unknown.
    virtual qint64 maxUploadSize() const { return 0; }

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();

    void connectionStateChanged(ConnectionState state);
    void initialSyncDoneChanged();
    void syncModeChanged();
    void roomsChanged();
    void roomUpdated(const QString &roomId);
    void timelineReset(const QString &roomId);
    void eventAppended(const QString &roomId, const TimelineEvent &event);
    void eventStatusChanged(const QString &roomId,
                            const QString &eventId,
                            TimelineEvent::Status status);

    // v0.3.
    void eventReplaced(const QString &roomId,
                       const QString &oldEventId,
                       const TimelineEvent &newEvent);

    // v0.5.7: index-based diff signals for backends whose timeline is a
    // mirrored SDK vector (RustSdkMatrixClient). Indices refer to the
    // backend's timeline(roomId) list AFTER the operation was applied to
    // it; TimelineModel validates them again defensively before mutating
    // its copy.
    void eventInsertedAt(const QString &roomId, int index,
                         const TimelineEvent &event);
    void eventChangedAt(const QString &roomId, int index,
                        const TimelineEvent &event);
    void eventRemovedAt(const QString &roomId, int index);
    void eventsTruncatedTo(const QString &roomId, int length);
    void eventEdited(const QString &roomId, const QString &eventId);
    void eventRedacted(const QString &roomId, const QString &eventId);
    void reactionsChanged(const QString &roomId, const QString &eventId);
    void eventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void paginationStateChanged(const QString &roomId);
    void typingChanged(const QString &roomId);
    void membersChanged(const QString &roomId);

    void errorOccurred(const QString &message);

    // ---- v0.5.9 async command results. Every payload is non-secret:
    // categories are coarse ("network", "forbidden", "rate_limited", ...)
    // and no message body, token, key material, or local path is carried.
    void userSearchFinished(quint64 opId, bool ok,
                            const QVariantList &results, bool limited,
                            const QString &category);
    // v0.5.11: exact profile lookup result. ok=false with category
    // "not_found" means the homeserver does not know the user; other
    // categories are transient ("network", "rate_limited", ...).
    void userProfileFinished(quint64 opId, bool ok, const QString &userId,
                             const QString &displayName,
                             const QString &avatarUrl,
                             const QString &category);
    // v0.5.11: URL-preview result. `fields` carries only whitelisted
    // OpenGraph values (title, description, siteName, imageMxc, imageMime,
    // imageWidth, imageHeight, imageSize) — never the requested URL.
    void urlPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                            const QString &category);
    void dmCreateFinished(quint64 opId, bool ok, const QString &roomId,
                          const QString &category);
    void roomCreateFinished(quint64 opId, bool ok, const QString &roomId,
                            const QString &category, const QString &warning);
    void inviteUserFinished(quint64 opId, const QString &roomId,
                            const QString &userId, bool ok,
                            const QString &category);
    void inviteBatchFinished(quint64 opId, const QString &roomId,
                             int okCount, int failCount);
    // snapshot: ok, truncated, joinedCount, invitedCount, canInvite,
    // canEditName, canEditTopic, canEditAvatar, members(QVariantList of
    // maps: userId, displayName, avatarUrl, membership, role, ambiguous,
    // isOwn).
    void roomMembersReceived(quint64 opId, const QString &roomId,
                             const QVariantMap &snapshot);
    void roomEditFinished(quint64 opId, const QString &roomId,
                          const QString &field, bool ok,
                          const QString &category);
    void roomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    void spaceChildFinished(quint64 opId, const QString &spaceId,
                            const QString &roomId, bool ok);
    // Queue acceptance only — delivery state flows through the timeline
    // item's send state like any other local echo.
    void attachmentQueueFinished(quint64 opId, const QString &roomId,
                                 bool ok, const QString &category);
    void mediaReady(quint64 opId, const QString &mediaKey, int kind,
                    const QByteArray &bytes, const QString &mimetype,
                    const QString &filename);
    void mediaFailed(quint64 opId, const QString &mediaKey, int kind,
                     const QString &category);
    void maxUploadSizeChanged();
};
