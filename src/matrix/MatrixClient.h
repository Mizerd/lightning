#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

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

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();

    void connectionStateChanged(ConnectionState state);
    void initialSyncDoneChanged();
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
};
