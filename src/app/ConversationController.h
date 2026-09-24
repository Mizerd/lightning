#pragma once

#include "models/UserSearchModel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// Starts DMs, creates rooms and invites users. Operations are single-flight
// (`busy`) and keyed by op id; sign-out clears every pending id so a stale
// completion cannot touch the next session. m.direct stays inside the SDK.
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

    // Looks up existing joined DMs with `userId`; updates `existingDms`.
    Q_INVOKABLE void checkExistingDm(const QString &userId);

    // Create a new encrypted DM, after the existing-DM check.
    Q_INVOKABLE void startDirectMessage(const QString &userId);

    // options: name, topic, public, encrypted, alias, invites, spaceId and an
    // optional avatarPath (path or file:// URL). The avatar is applied after
    // creation; a failed upload emits avatarUploadFailed and never fails it.
    Q_INVOKABLE void createRoom(const QVariantMap &options);

    Q_INVOKABLE void inviteUsers(const QString &roomId, const QStringList &userIds);

    Q_INVOKABLE void clearError();
    Q_INVOKABLE void reset();

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void existingDmsChanged();
    void inviteResultsChanged();
    // A created or reused conversation should be opened. Never for Spaces,
    // which use spaceReady.
    void conversationReady(const QString &roomId);
    // A created Space is in the room list (or the wait elapsed): select it in
    // the rail, never open a timeline on it.
    void spaceReady(const QString &spaceId);
    // Room creation succeeded but the optional Space placement failed.
    void spacePlacementFailed(const QString &roomId);
    // Room creation succeeded but the optional avatar upload failed.
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

    // A created room is opened once it appears in the room list, bounded by
    // a timer.
    bool m_waitingForRoom = false;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;
    // Bounds the create call itself: the server federates the invite before
    // answering /createRoom, so an unreachable peer server can hang it.
    QTimer m_opTimeout;
    // Owned here, not by the dialog, so closing it mid-create cannot route a
    // Space into a room timeline.
    bool m_pendingIsSpace = false;

    // Outside busy(): a slow avatar upload must not delay opening the room.
    QString m_pendingAvatarPath;
    quint64 m_avatarOp = 0;
};
