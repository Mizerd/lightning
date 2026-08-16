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

// v0.7.x: orchestrates the Discover / Join Room experience — identifier
// resolution + preview, joining, knocking, knock withdrawal, and the
// Space-children listing (joined AND unjoined rows).
//
// All operations are identified by backend operation ids and safe against
// sign-out: MatrixClient::loggedOut clears every pending id and cache, so a
// stale completion can never navigate or populate another account's state.
// Join/knock protocol behaviour is entirely the SDK's; this class only
// sequences requests and shapes results for QML.
class RoomDiscoveryController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(RoomDirectorySearchModel* directory READ directory CONSTANT)
    // "idle" | "resolving" | "resolved" | "failed"
    Q_PROPERTY(QString resolveState READ resolveState NOTIFY resolveChanged)
    // The last resolution result map (see MatrixClient::roomTargetResolved).
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

    // Resolve #alias / !roomid / matrix: URI / matrix.to permalink into a
    // previewed target. Supersedes any earlier unanswered resolution.
    Q_INVOKABLE void resolve(const QString &input);
    Q_INVOKABLE void clearResolved();

    // Join by id or alias (via = routing servers, may be empty). On success
    // waits (bounded) for the room to appear in the authoritative list,
    // then emits roomJoined / spaceJoined.
    Q_INVOKABLE void join(const QString &target, const QStringList &via,
                          bool isSpace);
    // Knock with an optional reason. Success emits knockSent.
    Q_INVOKABLE void knock(const QString &target, const QStringList &via,
                           const QString &reason);
    // Withdraw a pending knock (room list row action). Emits knockCancelled.
    Q_INVOKABLE void cancelKnock(const QString &roomId);

    // Space children including unjoined rows. Results are cached per space
    // until refreshed; read with spaceChildren()/spaceChildrenState().
    Q_INVOKABLE void refreshSpaceChildren(const QString &spaceId);
    Q_INVOKABLE QVariantList spaceChildren(const QString &spaceId) const;
    // "" (never asked) | "loading" | "ready" | "failed"
    Q_INVOKABLE QString spaceChildrenState(const QString &spaceId) const;

    Q_INVOKABLE void clearError();

Q_SIGNALS:
    void supportedChanged();
    void busyChanged();
    void errorMessageChanged();
    void resolveChanged();
    // The joined room is present in the authoritative list (or the bounded
    // wait elapsed): open it.
    void roomJoined(const QString &roomId);
    // A joined m.space room must be selected in the rail, never given a
    // message timeline.
    void spaceJoined(const QString &spaceId);
    // A join FAILED, naming the target it was for. errorMessage carries the
    // same text, but only this says which request it belonged to — knock
    // and knock-withdrawal failures write that shared property too.
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
    void beginWaitForRoom(const QString &roomId, bool isSpace);
    void finishWaitForRoom();
    // Join/knock failure categories → honest user-facing strings. Public
    // Matrix conditions only; never raw server text. Static for unit tests.
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
    // The target of the in-flight join, so its failure can name itself.
    QString m_joinTarget;
    QString m_awaitedRoomId;
    QTimer m_roomWaitTimeout;

    struct SpaceChildrenEntry {
        QString state; // "loading" | "ready" | "failed"
        QVariantList rows;
        quint64 pendingOp = 0;
    };
    QHash<QString, SpaceChildrenEntry> m_spaceChildren;
};
