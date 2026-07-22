#pragma once

#include "models/UserSearchModel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.5.9: orchestrates starting Direct Messages, creating rooms and inviting
// users into existing rooms.
//
// All operations are single-flight (`busy`), identified by backend operation
// ids, and safe against sign-out: MatrixClient::loggedOut clears every
// pending id, so a stale completion can never open a room or mutate state
// for a new session. m.direct handling stays entirely inside the SDK
// (create_dm / mark_as_dm) — this class never composes account data.
class ConversationController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Existing joined DMs found for the selected user (list of {roomId,name}).
    Q_PROPERTY(QVariantList existingDms READ existingDms NOTIFY existingDmsChanged)
    Q_PROPERTY(UserSearchModel* userSearch READ userSearch CONSTANT)
    // Per-user invite progress for the invite dialog:
    // list of {userId, state("pending"|"ok"|"failed"), category}.
    Q_PROPERTY(QVariantList inviteResults READ inviteResults NOTIFY inviteResultsChanged)

public:
    explicit ConversationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool busy() const { return m_pendingOp != 0 || m_waitingForRoom; }
    QString errorMessage() const { return m_errorMessage; }
    QVariantList existingDms() const { return m_existingDms; }
    UserSearchModel *userSearch() const { return m_userSearch; }
    QVariantList inviteResults() const { return m_inviteResults; }

    // Step 1 of the DM flow: look up existing joined DMs with `userId`
    // through the SDK's m.direct projection. Updates `existingDms`.
    Q_INVOKABLE void checkExistingDm(const QString &userId);

    // Create a new encrypted DM (explicitly, after the existing-DM check).
    Q_INVOKABLE void startDirectMessage(const QString &userId);

    // Create a room. options: name, topic, public, encrypted, alias,
    // invites (list of user ids), spaceId, and an optional avatarPath (a
    // local file path or file:// URL). The avatar is applied AFTER the room
    // is created; a failed upload emits avatarUploadFailed and never blocks
    // or fails the creation itself.
    Q_INVOKABLE void createRoom(const QVariantMap &options);

    // Invite one or more users into a joined room.
    Q_INVOKABLE void inviteUsers(const QString &roomId, const QStringList &userIds);

    Q_INVOKABLE void clearError();
    Q_INVOKABLE void reset();

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void existingDmsChanged();
    void inviteResultsChanged();
    // Fired when a created (or reused) conversation should be opened.
    // Never fired for Spaces — an m.space room must not get a message
    // timeline; spaceReady carries those.
    void conversationReady(const QString &roomId);
    // Fired when a created Space is present in the authoritative room list
    // (or the bounded wait elapsed): select it in the rail, never open a
    // timeline on it.
    void spaceReady(const QString &spaceId);
    // Room creation succeeded but the optional Space placement failed.
    void spacePlacementFailed(const QString &roomId);
    // Room creation succeeded but the optional avatar upload failed (or is
    // unsupported by the backend). A warning, never a failed create.
    void avatarUploadFailed(const QString &roomId);
    void inviteBatchCompleted(int okCount, int failCount);

private Q_SLOTS:
    void onDmCreateFinished(quint64 opId, bool ok, const QString &roomId,
                            const QString &category);
    void onRoomCreateFinished(quint64 opId, bool ok, const QString &roomId,
                              const QString &category, const QString &warning);
    void onInviteUserFinished(quint64 opId, const QString &roomId,
                              const QString &userId, bool ok,
                              const QString &category);
    void onInviteBatchFinished(quint64 opId, const QString &roomId,
                               int okCount, int failCount);
    void onRoomEditFinished(quint64 opId, const QString &roomId,
                            const QString &field, bool ok,
                            const QString &category);
    void onRoomsChanged();
    void onLoggedOut();

private:
    void setError(const QString &message);
    void beginWaitForRoom(const QString &roomId);
    void finishWaitForRoom();
    static QString describeCategory(const QString &category);

    MatrixClient *m_client = nullptr;
    UserSearchModel *m_userSearch = nullptr;
    quint64 m_pendingOp = 0;
    QString m_errorMessage;
    QVariantList m_existingDms;
    QVariantList m_inviteResults;

    // After creation the room must appear in the authoritative room list
    // before it is opened; a bounded timer prevents a stuck busy state.
    bool m_waitingForRoom = false;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;
    // Whether the pending create is an m.space room. Owned here — not in
    // the dialog — so closing the dialog mid-create can never reroute a
    // Space into an ordinary room timeline.
    bool m_pendingIsSpace = false;

    // Optional avatar applied after a successful room create. Deliberately
    // OUTSIDE busy(): a slow or failed avatar upload must never block or
    // time out opening the new room.
    QString m_pendingAvatarPath;
    quint64 m_avatarOp = 0;
};
