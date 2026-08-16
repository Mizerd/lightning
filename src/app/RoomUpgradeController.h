#pragma once

#include <QObject>
#include <QString>

class MatrixClient;
class RoomDiscoveryController;

// v0.7.x room upgrades (`m.room.tombstone`) — banner and link.
//
// When a room is upgraded, Matrix leaves the old room in place and creates a
// replacement. Lightning's policy, chosen deliberately, is banner-and-link:
// the old room stays open and readable, and the user is OFFERED the
// successor. There is no auto-follow anywhere in this class.
//
// That is not merely a preference. A room transition discards navigation
// context and can discard draft state, and the event that announces the
// upgrade is attacker-controllable state — anyone with the power level to
// send `m.room.tombstone` names the room you would be moved into. Following
// it silently would let that person relocate a user's session. So every
// transition here is the direct result of the user activating the banner.
//
// Like PinnedMessagesController this tracks the ACTIVE room, not Room
// Information's room (which can be a Space home), because the banner sits
// above the open timeline.
//
// Honesty rules:
//   - A successor Lightning has never heard of is Unknown, NOT
//     NotAccessible. We do not know the user cannot join it, so the banner
//     still offers Continue and the join decides.
//   - chainVerified being false because the successor is not in our room
//     set means "not yet established", not "bad". The banner is still shown
//     and still actionable. Only a CONTRADICTED chain — we hold the
//     successor and its predecessor names some other room — is evidence of
//     something wrong, and even then the only consequence is withholding
//     the room list's de-emphasis.
//   - The join reuses RoomDiscoveryController rather than reimplementing
//     one, so error categories and the wait-for-room settle behave exactly
//     as they do in Discover. A failed join leaves the user in the old room
//     with the reason visible.
//   - The tombstone's own body never reaches this class. It is free text
//     chosen by whoever sent the state event, and it never crosses the FFI.
class RoomUpgradeController : public QObject
{
    Q_OBJECT

    // What Lightning knows about the successor's reachability. Unknown is
    // the honest default and is distinct from NotAccessible.
    Q_PROPERTY(int successorAccess READ successorAccess NOTIFY changed)
    // True when the ACTIVE room has been replaced.
    Q_PROPERTY(bool upgraded READ upgraded NOTIFY changed)
    Q_PROPERTY(QString successorRoomId READ successorRoomId NOTIFY changed)
    // True only when we actually hold the successor AND its predecessor
    // points back at the active room. See the honesty rules above: false is
    // "not established yet", not an error.
    Q_PROPERTY(bool chainVerified READ chainVerified NOTIFY changed)
    // The active room's own predecessor, for the "Previous room" link. A
    // room can be both an opened successor and someone's predecessor, so
    // this is independent of `upgraded`. Empty unless we actually HOLD the
    // predecessor: there is no join step on this link, so offering one for
    // a room we have no record of could only open an empty view.
    Q_PROPERTY(QString predecessorRoomId READ predecessorRoomId NOTIFY changed)
    // Sanitized, user-facing text for the last failed Continue; empty when
    // none. Shown inline in the banner so the failure is visible WITHOUT
    // leaving the old room.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // A join started from the banner is in flight.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    enum SuccessorAccess {
        // No successor, or we hold no room record for it. The banner still
        // offers Continue — attempting the join is how we find out.
        Unknown = 0,
        // We hold the successor and the user is not in it (left, banned).
        NotAccessible = 1,
        Invited = 2,
        Joined = 3,
    };
    Q_ENUM(SuccessorAccess)

    explicit RoomUpgradeController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // The join and the navigation both go through Discover's existing
    // machinery; this wires that up. Must be called before continueToSuccessor
    // can join.
    void setDiscovery(RoomDiscoveryController *discovery);
    // Called by AppController when the active room changes.
    void setRoomId(const QString &roomId);

    QString roomId() const { return m_roomId; }
    bool upgraded() const { return !m_successorRoomId.isEmpty(); }
    QString successorRoomId() const { return m_successorRoomId; }
    int successorAccess() const { return m_successorAccess; }
    bool chainVerified() const { return m_chainVerified; }
    QString predecessorRoomId() const { return m_predecessorRoomId; }
    QString error() const { return m_error; }
    bool busy() const { return !m_pendingJoinRoomId.isEmpty(); }

    // The banner's action. Joined -> navigate. Invited/Unknown -> join
    // first, navigate only once the join settles. NotAccessible -> report
    // and stay. Never leaves the old room on any failure path.
    Q_INVOKABLE void continueToSuccessor();
    // The "Previous room" link. The predecessor is a room the user was
    // already in, so there is no join step.
    Q_INVOKABLE void goToPredecessor();

    // True when `roomId` is a join THIS controller started from the banner
    // and then abandoned, because the user moved to another room (or signed
    // out) before it settled. Discover's roomJoined -> openRoom connection
    // is unconditional, so without this a join that takes a moment would
    // yank the user out of the room they had since opened and started
    // typing in — the switch would no longer be the direct consequence of
    // the click that started it. Consumed once: a later legitimate join of
    // the same room still navigates.
    bool consumeAbandonedJoin(const QString &roomId);

Q_SIGNALS:
    void changed();
    // Ask AppController to open a room. Emitted only from a user action.
    void navigateRequested(const QString &roomId);

private:
    // Recompute successor/predecessor/access/chain from the client's room
    // set. Cheap, and the only place these are derived.
    void refresh();
    void setError(const QString &message);

    MatrixClient *m_client = nullptr;
    RoomDiscoveryController *m_discovery = nullptr;
    QString m_roomId;
    QString m_successorRoomId;
    QString m_predecessorRoomId;
    int m_successorAccess = Unknown;
    bool m_chainVerified = false;
    QString m_error;
    // The successor we asked Discover to join, empty when no join of ours
    // is in flight. Also the guard that stops us reacting to a join the
    // user started somewhere else (the Discover dialog), and to our own
    // join completing after the user moved to a different room.
    QString m_pendingJoinRoomId;
    // A join we started and then walked away from. See
    // consumeAbandonedJoin.
    QString m_abandonedJoinRoomId;
};
