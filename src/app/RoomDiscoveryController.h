#pragma once

#include "models/RoomDirectorySearchModel.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class RoomDiscoveryController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(RoomDirectorySearchModel* directory READ directory CONSTANT)
    Q_PROPERTY(QString resolveState READ resolveState NOTIFY resolveChanged)
    Q_PROPERTY(QVariantMap resolved READ resolved NOTIFY resolveChanged)

public:
    explicit RoomDiscoveryController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool supported() const;
    bool busy() const { return m_joinOp != 0 || m_waitingForRoom; }
    QString errorMessage() const { return m_errorMessage; }
    RoomDirectorySearchModel *directory() const { return m_directory; }
    QString resolveState() const { return m_resolveState; }
    QVariantMap resolved() const { return m_resolved; }

    Q_INVOKABLE void resolve(const QString &input);
    Q_INVOKABLE void clearResolved();

    Q_INVOKABLE void join(const QString &target, const QStringList &via,
                          bool isSpace);
    Q_INVOKABLE void joinSpaceKeepingRoom(const QString &target,
                                          const QStringList &via);
    Q_INVOKABLE void knock(const QString &target, const QStringList &via,
                           const QString &reason);
    Q_INVOKABLE void cancelKnock(const QString &roomId);

    Q_INVOKABLE void refreshSpaceChildren(const QString &spaceId);
    Q_INVOKABLE QVariantList spaceChildren(const QString &spaceId) const;
    Q_INVOKABLE QString spaceChildrenState(const QString &spaceId) const;

    Q_INVOKABLE void clearError();

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void resolveChanged();
    void roomJoined(const QString &roomId);
    // A joined m.space room must be selected in the rail, never given a
    // message timeline.
    void spaceJoined(const QString &spaceId);
    void spaceJoinedInPlace(const QString &spaceId);
    void joinFailed(const QString &target, const QString &message);
    void knockSent(const QString &roomId);
    void knockCancelled(const QString &roomId);
    void spaceChildrenChanged(const QString &spaceId);

private Q_SLOTS:
    void onTargetResolved(quint64 opId, const QVariantMap &result);
    void onJoinFinished(quint64 opId, bool ok, const QString &roomId,
                        const QString &category);
    void onKnockFinished(quint64 opId, bool ok, const QString &roomId,
                         const QString &category);
    void onKnockCancelFinished(quint64 opId, bool ok, const QString &roomId,
                               const QString &category);
    void onSpaceChildren(quint64 opId, const QString &spaceId, bool ok,
                         const QVariantList &rooms, bool truncated,
                         const QString &category);
    void onRoomsChanged();
    void onLoggedOut();

private:
    void setError(const QString &message);
    void joinInternal(const QString &target, const QStringList &via,
                      bool isSpace);
    void beginWaitForRoom(const QString &roomId, bool isSpace);
    void finishWaitForRoom();
public:
    static QString describeJoinCategory(const QString &category);

private:
    MatrixClient *m_client = nullptr;
    RoomDirectorySearchModel *m_directory = nullptr;

    quint64 m_resolveOp = 0;
    QString m_resolveState = QStringLiteral("idle");
    QVariantMap m_resolved;

    quint64 m_joinOp = 0;
    quint64 m_knockOp = 0;
    quint64 m_cancelKnockOp = 0;
    QString m_errorMessage;

    bool m_waitingForRoom = false;
    bool m_awaitedIsSpace = false;
    bool m_keepRoomOnJoin = false;
    QString m_joinTarget;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;

    struct SpaceChildrenEntry {
        QString state;
        QVariantList rows;
        quint64 pendingOp = 0;
    };
    QHash<QString, SpaceChildrenEntry> m_spaceChildren;
};
