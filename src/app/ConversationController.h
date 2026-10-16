#pragma once

#include "models/UserSearchModel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
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
    // Existing joined DMs found for the selected user (see checkExistingDm).
    Q_PROPERTY(QVariantList existingDms READ existingDms NOTIFY existingDmsChanged)
    Q_PROPERTY(UserSearchModel* userSearch READ userSearch CONSTANT)
    // Per-user invite progress for the invite dialog:
    // list of {userId, state("pending"|"ok"|"failed"), category}.
    Q_PROPERTY(QVariantList inviteResults READ inviteResults NOTIFY inviteResultsChanged)
    // The user a DM was about to be started with when their homeserver
    // answered the profile lookup with 404 M_NOT_FOUND ("" otherwise). The
    // dialog asks before creating: startDirectMessageAnyway() or
    // dismissUnknownTarget().
    Q_PROPERTY(QString unknownTarget READ unknownTarget NOTIFY unknownTargetChanged)

public:
    explicit ConversationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool busy() const
    {
        return m_pendingOp != 0 || m_targetCheckOp != 0 || m_waitingForRoom;
    }
    QString errorMessage() const { return m_errorMessage; }
    QVariantList existingDms() const { return m_existingDms; }
    UserSearchModel *userSearch() const { return m_userSearch; }
    QVariantList inviteResults() const { return m_inviteResults; }
    QString unknownTarget() const { return m_unknownTarget; }

    // Looks up existing joined DMs with `userId`; updates `existingDms`. Each
    // entry is {roomId, name, avatarUrl[, oneToOne]}: the room list's computed
    // name and the room avatar, since m.direct rooms rarely carry an explicit
    // name. `oneToOne` false marks a group DM, which must not take the
    // selected person's avatar.
    Q_INVOKABLE void checkExistingDm(const QString &userId);

    // Create a new encrypted DM, after the existing-DM check. The target's
    // profile is looked up first (as Element does): a 404 M_NOT_FOUND stops
    // here and sets `unknownTarget`; any other failure (network, timeout, a
    // server that hides profiles) does not block.
    Q_INVOKABLE void startDirectMessage(const QString &userId);
    // The user confirmed a target the server says does not exist.
    Q_INVOKABLE void startDirectMessageAnyway(const QString &userId);
    Q_INVOKABLE void dismissUnknownTarget();

    // options: name, topic, public, encrypted, alias, invites, spaceId and an
    // optional avatarPath (path or file:// URL). The avatar is applied after
    // creation; a failed upload emits avatarUploadFailed and never fails it.
    Q_INVOKABLE void createRoom(const QVariantMap &options);

    Q_INVOKABLE void inviteUsers(const QString &roomId, const QStringList &userIds);

    Q_INVOKABLE void clearError();
    Q_INVOKABLE void reset();

    // Test seams: shorten the create-call bound (60 s) and the profile-check
    // bound (15 s).
    void setOpTimeoutMsForTest(int ms);
    void setTargetCheckTimeoutMsForTest(int ms);

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void existingDmsChanged();
    void inviteResultsChanged();
    void unknownTargetChanged();
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
    void onUserProfileFinished(quint64 opId, bool ok, const QString &userId,
                               const QString &displayName,
                               const QString &avatarUrl,
                               const QString &category);

private:
    void setError(const QString &message);
    void setUnknownTarget(const QString &userId);
    // Abandons an in-flight profile check; it has no server-side effect.
    void cancelTargetCheck();
    // The self-DM and timed-out-create refusals, shared by both start paths.
    bool refuseDmTarget(const QString &userId);
    void createDirectChat(const QString &userId);
    void clearAbandonedDm();
    void beginWaitForRoom(const QString &roomId);
    void finishWaitForRoom();
    static QString describeCategory(const QString &category);

    MatrixClient *m_client = nullptr;
    UserSearchModel *m_userSearch = nullptr;
    quint64 m_pendingOp = 0;
    QString m_errorMessage;
    QVariantList m_existingDms;
    QVariantList m_inviteResults;

    // The pre-create profile lookup: its op id, who it is for, and a bound in
    // case no answer ever comes (rooms.rs already bounds it at 10 s).
    quint64 m_targetCheckOp = 0;
    QString m_targetCheckUser;
    QTimer m_targetCheckTimeout;
    QString m_unknownTarget;

    // A created room is opened once it appears in the room list, bounded by
    // a timer.
    bool m_waitingForRoom = false;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;
    // Bounds the create call itself: the server federates the invite before
    // answering /createRoom, so an unreachable peer server can hang it.
    QTimer m_opTimeout;
    // The user a pending DM create is for, and the create we stopped waiting
    // for (timed out) but whose server-side work may still produce a room.
    QString m_pendingDmUser;
    QHash<quint64, QString> m_abandonedDms;   // op id -> user
    QTimer m_abandonedGuard;
    // Owned here, not by the dialog, so closing it mid-create cannot route a
    // Space into a room timeline.
    bool m_pendingIsSpace = false;

    // Outside busy(): a slow avatar upload must not delay opening the room.
    QString m_pendingAvatarPath;
    quint64 m_avatarOp = 0;
};
