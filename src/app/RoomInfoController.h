#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.5.9: state for the Room Information panel — member snapshot,
// SDK-derived permissions, room profile editing (name/topic/avatar) and
// Leave room.
//
// Permission flags come exclusively from the Rust member snapshot
// (RoomMember::can_invite / can_send_state); nothing here guesses from role
// labels. Member data lives only in memory — it is never written into
// CacheStore. All async completions are matched by operation id and
// invalidated on room switch and sign-out.
class RoomInfoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY roomIdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY membersChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
    Q_PROPERTY(int joinedCount READ joinedCount NOTIFY membersChanged)
    Q_PROPERTY(int invitedCount READ invitedCount NOTIFY membersChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY membersChanged)
    Q_PROPERTY(bool canInvite READ canInvite NOTIFY membersChanged)
    Q_PROPERTY(bool canEditName READ canEditName NOTIFY membersChanged)
    Q_PROPERTY(bool canEditTopic READ canEditTopic NOTIFY membersChanged)
    Q_PROPERTY(bool canEditAvatar READ canEditAvatar NOTIFY membersChanged)
    Q_PROPERTY(bool editPending READ editPending NOTIFY editStateChanged)
    Q_PROPERTY(QString editError READ editError NOTIFY editStateChanged)
    Q_PROPERTY(bool leavePending READ leavePending NOTIFY leaveStateChanged)
    Q_PROPERTY(QString leaveError READ leaveError NOTIFY leaveStateChanged)

public:
    explicit RoomInfoController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool supported() const;
    bool loading() const { return m_membersOp != 0; }
    QVariantList members() const { return m_members; }
    int joinedCount() const { return m_joinedCount; }
    int invitedCount() const { return m_invitedCount; }
    bool truncated() const { return m_truncated; }
    bool canInvite() const { return m_canInvite; }
    bool canEditName() const { return m_canEditName; }
    bool canEditTopic() const { return m_canEditTopic; }
    bool canEditAvatar() const { return m_canEditAvatar; }
    bool editPending() const { return m_editOp != 0; }
    QString editError() const { return m_editError; }
    bool leavePending() const { return m_leaveOp != 0; }
    QString leaveError() const { return m_leaveError; }

    Q_INVOKABLE void refreshMembers();
    Q_INVOKABLE void setRoomName(const QString &name);
    Q_INVOKABLE void setRoomTopic(const QString &topic);
    Q_INVOKABLE void setRoomAvatar(const QUrl &fileUrl);
    Q_INVOKABLE void removeRoomAvatar();
    Q_INVOKABLE void leaveRoom();
    // v0.6.5 (SPEC 1d): room-list context-menu adapter. Acts on an EXPLICIT
    // room id instead of the controller's own roomId property — the Room
    // Information panel may be showing a different room (or none), and this
    // must never mutate that binding. Tracked independently of the panel's
    // own leavePending/leaveError state (see m_adhocLeaveOps) so the two
    // paths can never corrupt each other.
    Q_INVOKABLE void leaveRoom(const QString &roomId);
    // Case-insensitive member filter over the loaded snapshot; returns the
    // same map shape as `members`.
    Q_INVOKABLE QVariantList filterMembers(const QString &needle) const;

Q_SIGNALS:
    void roomIdChanged();
    void membersChanged();
    void editStateChanged();
    void leaveStateChanged();
    // The active room was left; AppController closes the timeline. Fired for
    // both the panel's own Leave button and the room-list adapter above.
    void roomLeft(const QString &roomId);
    // v0.6.5: the room-list adapter's own honest failure surface — the
    // panel's leaveError/leaveStateChanged stay reserved for the panel's own
    // pending room and are not touched by this path.
    void roomLeaveFailed(const QString &roomId, const QString &message);

private Q_SLOTS:
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);
    void onRoomEditFinished(quint64 opId, const QString &roomId,
                            const QString &field, bool ok,
                            const QString &category);
    void onRoomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                             const QString &category);
    void onMembersChanged(const QString &roomId);
    void onLoggedOut();

private:
    void clearSnapshot();

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_membersOp = 0;
    quint64 m_editOp = 0;
    quint64 m_leaveOp = 0;
    // v0.6.5: in-flight room-list adapter leave() calls, keyed by opId —
    // deliberately separate from m_leaveOp (the panel's own pending leave).
    QSet<quint64> m_adhocLeaveOps;
    QVariantList m_members;
    int m_joinedCount = 0;
    int m_invitedCount = 0;
    bool m_truncated = false;
    bool m_canInvite = false;
    bool m_canEditName = false;
    bool m_canEditTopic = false;
    bool m_canEditAvatar = false;
    QString m_editError;
    QString m_leaveError;
};
