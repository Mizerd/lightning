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
    // v0.7 account switching: end the LOCAL session only — stop sync, tear
    // down subscriptions, drop in-memory state, and emit loggedOut() so every
    // account-scoped model clears — but do NOT log out on the server and do
    // NOT delete the account's persisted store, tokens, or metadata. After a
    // detach, restoreSession() activates whichever account the settings now
    // select. Returns false when the backend cannot detach.
    virtual bool detachSession() { return false; }
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
    // Targeted single-room lookup. The default derives from rooms(), which
    // deep-copies the whole list — backends with a native index override it
    // (the per-appended-event notification context used to pay that full
    // copy for every event of a sync burst). Returns a default-constructed
    // RoomInfo when the room is unknown.
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

    // v0.7 outgoing @-mentions. `mentionUserIds` are full MXIDs to place in
    // m.mentions (deduped, order-insensitive). The default impls forward to
    // the zero-mention versions so Mock/HTTP backends stay correct; the Rust
    // backend overrides these to attach m.mentions through the SDK. The body
    // already carries the matrix.to markdown links for those users.
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
    // ---- v0.6.0: SDK-backed thread timelines.
    //
    // A thread timeline is addressed by a composite timeline id so it flows
    // through the SAME diff signal pipeline (timelineReset, eventInsertedAt,
    // paginationStateChanged, ...) and model code as a room timeline —
    // TimelineModel simply sets its roomId to the composite id. The id
    // embeds a unit separator, which can never appear in Matrix room/event
    // ids, so it can never collide with a real room id.
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

    // True when the backend can open live thread timelines. Backends
    // without support keep the false default and the thread UI stays
    // hidden.
    virtual bool supportsThreadTimelines() const { return false; }
    // Open (or replace) the single live thread timeline. The backend
    // responds with timelineReset(threadTimelineId(...)) on success or
    // threadTimelineFailed(...) on failure. A room switch closes it.
    virtual void openThread(const QString &roomId, const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void closeThread() {}
    // v0.6.0 checkpoint 8: manual decryption retry for a room's visible
    // unable-to-decrypt events. The Rust backend re-runs SDK decryption
    // against key material that has arrived since; other backends have no
    // crypto machine and keep the no-op default. Never resets any store.
    virtual void retryDecryption(const QString &roomId) { Q_UNUSED(roomId); }

    // ---- v0.6.0 checkpoint 5: thread list, follow state, threaded read.
    //
    // The Threads view lists a room's threads (server /threads pagination,
    // kept live by the backend); follow state is MSC4306 server-side thread
    // subscription where the homeserver supports it; markThreadRead sends a
    // THREADED read receipt for the open thread panel only — never a
    // room-wide receipt.
    virtual bool supportsThreadList() const { return false; }
    virtual void openThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void closeThreadList() {}
    virtual void paginateThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void markThreadRead(const QString &roomId,
                                const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
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

    // Rich reply to a specific event WITHIN the thread. Backends without a
    // dedicated path fall back to a plain thread reply (the message still
    // lands in the correct thread).
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body)
    {
        Q_UNUSED(inReplyToEventId);
        sendThreadReply(roomId, threadRootEventId, body);
    }

    // v0.7 outgoing @-mentions inside a thread. `inReplyToEventId` empty is a
    // plain thread reply; non-empty is a rich reply within the thread. Default
    // forwards to the zero-mention path.
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body);
    }

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody) = 0;

    // v0.7 outgoing @-mentions on an edit. Default forwards to the zero-mention
    // edit path.
    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        editMessage(roomId, targetEventId, newBody);
    }
    virtual void redactEvent(const QString &roomId,
                             const QString &eventId,
                             const QString &reason = QString()) = 0;
    virtual void toggleReaction(const QString &roomId,
                                const QString &targetEventId,
                                const QString &key) = 0;

    // v0.7: MSC3381 polls (Rust backend only; mock/HTTP keep the honest
    // false default and the poll actions stay hidden/disabled in the UI).
    // threadRootId empty targets the room's live timeline, otherwise the
    // thread whose root it names. answerIds empty retracts the vote.
    virtual bool supportsPolls() const { return false; }
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
    virtual void setRoomMarkedUnread(const QString &roomId, bool unread)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(unread);
    }
    // Server-synchronized per-room notification mode (account push rules
    // managed entirely by the Matrix SDK). Modes match SettingsManager /
    // NotificationManager::RoomMode: 0 = all messages, 1 = mentions &
    // keywords, 2 = mute. Backends without push-rule support keep the
    // false default and the per-room mode stays a device-local setting.
    // setRoomNotificationMode is label-faithful: mode 0 sets an explicit
    // AllMessages rule (a "follow account default" choice that removes the
    // user rule instead is an accepted follow-up, not offered yet).
    // requestRoomNotificationMode usually answers asynchronously via
    // roomNotificationModeChanged with the user-defined rule when one
    // exists, else the account default resolved for the room's shape —
    // but it is deliberately SKIPPED while a write for the room is queued
    // or in flight (the write's own report is authoritative and imminent).
    virtual bool supportsServerNotificationModes() const { return false; }
    virtual void setRoomNotificationMode(const QString &roomId, int mode)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(mode);
    }
    virtual void requestRoomNotificationMode(const QString &roomId)
    {
        Q_UNUSED(roomId);
    }
    virtual void acceptInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void rejectInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void sendImage(const QString &roomId, const QString &localPath) = 0;
    virtual void sendFile(const QString &roomId, const QString &localPath) = 0;

    // Pagination
    virtual void loadOlderMessages(const QString &roomId) = 0;
    virtual bool canPaginate(const QString &roomId) const = 0;
    virtual bool paginationReady(const QString &roomId) const
    { return canPaginate(roomId) || paginating(roomId) || paginationFailed(roomId); }
    virtual bool paginating(const QString &roomId) const = 0;

    // v0.5.7: true when the last backward pagination for this room failed
    // and can be retried. Backends without failure tracking return false.
    virtual bool paginationFailed(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }
    // True only when the current pagination failure is safe to retry without
    // user intervention (for example a temporary network failure or a live
    // timeline readiness race). Permission and invalid-room failures must
    // remain false so the controller exposes manual Retry immediately.
    virtual bool paginationFailureTransient(const QString &roomId) const
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
    // v0.5.12: client-side URL preview (Rust validates and fetches the target;
    // the client never does). Backends without support return 0.
    virtual bool supportsUrlPreview() const { return false; }
    virtual quint64 fetchUrlPreview(const QString &url)
    { Q_UNUSED(url); return 0; }
    // v0.6.1: bounded, redirect-validated HTTPS GET for an external GIF
    // provider. `url` is built by the GIF provider layer and carries the
    // provider API key — callers must treat it as secret and never log it.
    // Backends without support return 0. Result arrives via gifResponse().
    virtual bool supportsGifProvider() const { return false; }
    virtual quint64 gifGet(const QString &url)
    { Q_UNUSED(url); return 0; }
    // v0.6.1: download + validate a provider GIF (result via
    // gifDownloadFinished, bytes included on success). The URL must be a
    // provider-CDN https .gif; Rust re-validates the host + GIF magic bytes.
    virtual quint64 gifDownload(const QString &url)
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
    // v0.7: MSC1772 child removal (empty-via m.space.child). Never leaves
    // or deletes the child room itself.
    virtual quint64 removeRoomFromSpace(const QString &spaceId,
                                        const QString &roomId)
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

    // v0.6.1: attachment sending INTO a thread. Routed through the SDK's
    // thread-focused timeline so the m.thread relation and (in encrypted
    // rooms) encryption are handled by the SDK — never an ordinary room
    // send. Result echoes on attachmentQueueFinished by op id, exactly like
    // the room path. Backends without thread attachment support return 0.
    virtual quint64 sendThreadAttachment(const QString &roomId,
                                         const QString &rootEventId,
                                         const QString &localPath,
                                         const QString &mime,
                                         const QString &caption,
                                         int width, int height, bool animated)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(caption); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(animated);
        return 0;
    }
    virtual quint64 sendThreadAttachmentBytes(const QString &roomId,
                                              const QString &rootEventId,
                                              const QByteArray &bytes,
                                              const QString &filename,
                                              const QString &mime,
                                              int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(bytes);
        Q_UNUSED(filename); Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }

    // Media bridge: fetch (and decrypt, inside the SDK) media bytes for a
    // timeline item media key. kind: 0 = full, 1 = thumbnail.
    // timeoutClass (v0.7): 0 = standard, 1 = playable materialization,
    // 2 = explicit Save As — the backend bounds the fetch accordingly.
    virtual quint64 fetchMedia(const QString &mediaKey, int kind,
                               int timeoutClass = 0)
    { Q_UNUSED(mediaKey); Q_UNUSED(kind); Q_UNUSED(timeoutClass); return 0; }
    // Server-side thumbnail of a plain mxc URI (avatars).
    virtual quint64 fetchMxcThumbnail(const QString &mxc, int width, int height)
    { Q_UNUSED(mxc); Q_UNUSED(width); Q_UNUSED(height); return 0; }
    // Cancel an in-flight media fetch. Best-effort and idempotent: backends
    // without cancellation ignore it; no mediaReady/mediaFailed is emitted
    // for a cancelled op (the caller already dropped its bookkeeping).
    virtual void cancelMediaFetch(quint64 opId) { Q_UNUSED(opId); }

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
    // v0.6.0: opening a thread timeline failed (unknown root, build failure,
    // network). Success is signalled by timelineReset(threadTimelineId(...)).
    void threadTimelineFailed(const QString &roomId,
                              const QString &rootEventId,
                              const QString &category);
    // v0.6.0 checkpoint 5. Each thread entry map: rootEventId, rootSender,
    // rootSenderName, rootPreview, rootTimestamp, replyCount, latestSender,
    // latestSenderName, latestPreview, latestTimestamp. Bounded to the pages
    // fetched so far.
    void threadListUpdated(const QString &roomId, const QVariantList &threads,
                           bool endReached, bool failed);
    void threadSubscriptionState(const QString &roomId,
                                 const QString &rootEventId, bool supported,
                                 bool subscribed, bool automatic);
    void threadSubscriptionResult(const QString &roomId,
                                  const QString &rootEventId, bool ok,
                                  bool subscribed);
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
    // A contiguous run of SDK inserts applied as one model transaction. The
    // Rust SDK commonly emits backward pagination as many one-item `Insert`
    // diffs (immediately after its timeline-start sentinel); exposing the
    // final contiguous range prevents the UI from laying out once per item.
    void eventsInsertedAt(const QString &roomId, int index,
                          const QList<TimelineEvent> &events);
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

    // Server-reported per-room notification mode (0/1/2 as above).
    // userDefined is true for an explicit per-room rule, false when the
    // report is the account default resolved for this room. Carries mode
    // integers and the room id only — never push-rule JSON.
    void roomNotificationModeChanged(const QString &roomId, int mode,
                                     bool userDefined);
    // A server push-rule write for this room failed: the device-local mode
    // is kept and the UI must say so instead of claiming the mode was
    // saved to the account. Room id only — no error text, no rule JSON.
    void roomNotificationModeWriteFailed(const QString &roomId);

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
    // v0.5.14: httpStatus/redirectCount are sanitized failure diagnostics
    // (0 when not applicable, e.g. a DNS/timeout failure with no response
    // at all) — enough to tell a code regression from live remote policy
    // without ever logging the URL, query string, or response body.
    void urlPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                            const QString &category, int httpStatus = 0,
                            int redirectCount = 0);
    // v0.6.1: one external GIF-provider response. `body` is the bounded JSON
    // text (parsed by the GIF controller into safe structs — never surfaced to
    // QML); empty on failure. `category` is a coarse safe state
    // (ok/rate_limited/provider_error/timeout/network/too_large/blocked). The
    // request URL (which carries the provider key) is never emitted or logged.
    void gifResponse(quint64 opId, bool ok, int httpStatus,
                     const QByteArray &body, const QString &category);
    // v0.6.1: a validated GIF download. `bytes` is the real GIF on success
    // (empty on failure); `category` is a coarse safe reason on failure
    // (blocked/not_a_gif/too_large/invalid_media/timeout/network/provider_error).
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
    void spaceChildRemoveFinished(quint64 opId, const QString &spaceId,
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
