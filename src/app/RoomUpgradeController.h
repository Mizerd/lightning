#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class MatrixClient;
class RoomDiscoveryController;

// Room upgrades (`m.room.tombstone`): banner and link, never auto-follow.
// The tombstone is state anyone with the power level can send, so following
// it silently would let them relocate the user; every transition here is the
// direct result of the user activating the banner.
//
// Tracks the ACTIVE room (the banner sits above the open timeline).
//
// - A successor we hold no record of is Unknown, not NotAccessible; the join
//   attempt decides.
// - chainVerified == false means "not established", not "bad".
// - Joins go through RoomDiscoveryController; a failed join leaves the user
//   in the old room with the reason shown.
// - The tombstone's free-text body never crosses the FFI.
class RoomUpgradeController : public QObject
{
    Q_OBJECT

    // SuccessorAccess; Unknown is the default and distinct from NotAccessible.
    Q_PROPERTY(int successorAccess READ successorAccess NOTIFY changed)
    Q_PROPERTY(bool upgraded READ upgraded NOTIFY changed)
    Q_PROPERTY(QString successorRoomId READ successorRoomId NOTIFY changed)
    // True only when we hold the successor and its predecessor points back
    // at the active room.
    Q_PROPERTY(bool chainVerified READ chainVerified NOTIFY changed)
    // The active room's predecessor, for the "Previous room" link. Empty
    // unless we hold that room, since the link has no join step.
    Q_PROPERTY(QString predecessorRoomId READ predecessorRoomId NOTIFY changed)
    // Sanitized text for the last failed Continue, shown in the banner.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Send side. Versions come from /capabilities: availableVersions is
    // [{version, stable}] in display order, defaultVersion the server's
    // recommendation.
    Q_PROPERTY(bool versionsKnown READ versionsKnown NOTIFY versionsChanged)
    Q_PROPERTY(QString defaultVersion READ defaultVersion NOTIFY versionsChanged)
    Q_PROPERTY(QVariantList availableVersions READ availableVersions
                   NOTIFY versionsChanged)
    Q_PROPERTY(bool upgradeBusy READ upgradeBusy NOTIFY upgradeStateChanged)
    Q_PROPERTY(QString upgradeError READ upgradeError NOTIFY upgradeStateChanged)
    Q_PROPERTY(QString lastReplacementRoomId READ lastReplacementRoomId
                   NOTIFY upgradeStateChanged)

public:
    enum SuccessorAccess {
        // No successor, or no record of it; Continue still attempts a join.
        Unknown = 0,
        // We hold the successor and the user is not in it (left, banned).
        NotAccessible = 1,
        Invited = 2,
        Joined = 3,
    };
    Q_ENUM(SuccessorAccess)

    explicit RoomUpgradeController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Required before continueToSuccessor() can join.
    void setDiscovery(RoomDiscoveryController *discovery);
    void setRoomId(const QString &roomId);

    QString roomId() const { return m_roomId; }
    bool upgraded() const { return !m_successorRoomId.isEmpty(); }
    QString successorRoomId() const { return m_successorRoomId; }
    int successorAccess() const { return m_successorAccess; }
    bool chainVerified() const { return m_chainVerified; }
    QString predecessorRoomId() const { return m_predecessorRoomId; }
    QString error() const { return m_error; }
    bool busy() const { return !m_pendingJoinRoomId.isEmpty(); }

    // Joined: navigate. Invited/Unknown: join, then navigate once settled.
    // NotAccessible: report and stay.
    Q_INVOKABLE void continueToSuccessor();
    Q_INVOKABLE void goToPredecessor();

    // upgradeRoom() runs the standard /upgrade (the server copies state) and,
    // with `addToSameSpaces`, adds the replacement to every Space containing
    // the old room, then navigates to it. A re-parent the server refuses is
    // not reported (known gap).
    //
    // The target is explicit, never m_roomId: the upgrade is offered from Room
    // Information and Space settings, which need not show the active room,
    // and an upgrade cannot be undone.
    void setSpaces(class SpaceManager *spaces) { m_spaces = spaces; }
    Q_INVOKABLE void requestRoomVersions();
    Q_INVOKABLE void upgradeRoom(const QString &roomId, const QString &newVersion,
                                 bool addToSameSpaces);
    bool versionsKnown() const { return m_versionsKnown; }
    QString defaultVersion() const { return m_defaultVersion; }
    QVariantList availableVersions() const { return m_availableVersions; }
    bool upgradeBusy() const { return m_upgradeOp != 0; }
    QString upgradeError() const { return m_upgradeError; }
    QString lastReplacementRoomId() const { return m_lastReplacementRoomId; }

    // True (once) when `roomId` is a banner join the user moved away from
    // before it settled, so its navigation must be suppressed; Discover's
    // roomJoined -> openRoom connection is unconditional.
    bool consumeAbandonedJoin(const QString &roomId);

Q_SIGNALS:
    void changed();
    // Emitted only from a user action.
    void navigateRequested(const QString &roomId);
    void versionsChanged();
    void upgradeStateChanged();

private:
    // The only place successor/predecessor/access/chain are derived.
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
    // Our in-flight join, if any; also filters out joins started elsewhere.
    QString m_pendingJoinRoomId;
    // See consumeAbandonedJoin().
    QString m_abandonedJoinRoomId;

    // Send side.
    class SpaceManager *m_spaces = nullptr;
    bool m_versionsKnown = false;
    QString m_defaultVersion;
    QVariantList m_availableVersions;
    quint64 m_upgradeOp = 0;
    QString m_upgradeRoomId;      // the room the pending op upgrades
    bool m_upgradeAddToSpaces = false;
    QString m_upgradeError;
    QString m_lastReplacementRoomId;
    void onRoomVersionsReceived(bool ok, const QString &defaultVersion,
                                const QVariantList &available);
    void onRoomUpgradeFinished(quint64 opId, const QString &roomId, bool ok,
                               const QString &replacementRoomId,
                               const QString &category);
};
