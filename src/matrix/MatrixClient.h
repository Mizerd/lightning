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

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();

    void connectionStateChanged(ConnectionState state);
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
    void eventEdited(const QString &roomId, const QString &eventId);
    void eventRedacted(const QString &roomId, const QString &eventId);
    void reactionsChanged(const QString &roomId, const QString &eventId);
    void eventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void paginationStateChanged(const QString &roomId);
    void typingChanged(const QString &roomId);
    void membersChanged(const QString &roomId);

    void errorOccurred(const QString &message);
};
