#pragma once

#include "models/UserSearchModel.h"

#include <QDateTime>
#include <QObject>

#include "matrix/RoomInfo.h"
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
    Q_PROPERTY(QVariantList existingDms READ existingDms NOTIFY existingDmsChanged)
    Q_PROPERTY(UserSearchModel* userSearch READ userSearch CONSTANT)
    Q_PROPERTY(QVariantList inviteResults READ inviteResults NOTIFY inviteResultsChanged)
    Q_PROPERTY(QString unknownTarget READ unknownTarget NOTIFY unknownTargetChanged)
    Q_PROPERTY(bool unknownTargetUnreachable READ unknownTargetUnreachable
                   NOTIFY unknownTargetChanged)

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
    bool unknownTargetUnreachable() const { return m_unknownTargetUnreachable; }

    Q_INVOKABLE void checkExistingDm(const QString &userId);

    Q_INVOKABLE void startDirectMessage(const QString &userId);
    Q_INVOKABLE void startDirectMessageAnyway(const QString &userId);
    Q_INVOKABLE void dismissUnknownTarget();

    Q_INVOKABLE void createRoom(const QVariantMap &options);
    Q_INVOKABLE QString aliasLocalpartProblem(const QString &typed) const;

    Q_INVOKABLE void inviteUsers(const QString &roomId, const QStringList &userIds);

    Q_INVOKABLE QVariantList recentPeople(int limit) const;

    struct RecentPerson {
        QString userId;
        QString roomId;
        QString roomName;
        QString roomAvatar;
        QDateTime lastActivity;
    };
    static QList<RecentPerson> recentDirectPeople(const QList<RoomInfo> &rooms,
                                                  const QString &ownUserId,
                                                  int limit);

    Q_INVOKABLE void clearError();
    Q_INVOKABLE void reset();

    void setOpTimeoutMsForTest(int ms);
    void setTargetCheckTimeoutMsForTest(int ms);

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void existingDmsChanged();
    void inviteResultsChanged();
    void unknownTargetChanged();
    void conversationReady(const QString &roomId);
    void spaceReady(const QString &spaceId);
    void spacePlacementFailed(const QString &roomId);
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
    void setUnknownTarget(const QString &userId, bool unreachable = false);
    void cancelTargetCheck();
    bool refuseDmTarget(const QString &userId);
    void createDirectChat(const QString &userId);
    void clearAbandonedDm();
    void beginWaitForRoom(const QString &roomId);
    void finishWaitForRoom();
    static QString describeCategory(const QString &category);

    MatrixClient *m_client = nullptr;
    UserSearchModel *m_userSearch = nullptr;
    quint64 m_pendingOp = 0;
    quint64 m_inviteOp = 0;
    quint64 m_lateInviteOp = 0;
    QString m_errorMessage;
    QVariantList m_existingDms;
    QVariantList m_inviteResults;

    quint64 m_targetCheckOp = 0;
    QString m_targetCheckUser;
    QTimer m_targetCheckTimeout;
    QString m_unknownTarget;
    bool m_unknownTargetUnreachable = false;

    bool m_waitingForRoom = false;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;
    // Bounds the create call itself: the server federates the invite before
    // answering /createRoom, so an unreachable peer server can hang it.
    QTimer m_opTimeout;
    QString m_pendingDmUser;
    QHash<quint64, QString> m_abandonedDms;
    QTimer m_abandonedGuard;
    bool m_pendingIsSpace = false;

    QString m_pendingAvatarPath;
    quint64 m_avatarOp = 0;
};
