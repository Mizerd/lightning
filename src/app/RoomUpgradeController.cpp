#include "app/RoomUpgradeController.h"

#include "app/RoomDiscoveryController.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"

#include <QCoreApplication>

RoomUpgradeController::RoomUpgradeController(QObject *parent)
    : QObject(parent)
{
}

void RoomUpgradeController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    const bool notify = !m_error.isEmpty() || !m_pendingJoinRoomId.isEmpty();
    // CLEARED, never carried. Discover's own onLoggedOut zeroes m_joinOp and
    // m_waitingForRoom, so no roomJoined can follow a client change and the
    // token would protect nothing — it would only survive as account-scoped
    // residue able to swallow the NEXT account's navigation.
    m_abandonedJoinRoomId.clear();
    m_pendingJoinRoomId.clear();
    m_error.clear();
    if (notify)
        Q_EMIT changed();
    if (m_client) {
        // Every source of a tombstone converges here: the room-list diffs
        // that carry successor_room_id on the payload, and the
        // room_tombstone_changed poke, which updates the same field and
        // emits the same signal. One derivation path, so a live upgrade and
        // a restored one cannot disagree.
        connect(m_client, &MatrixClient::roomsChanged, this,
                &RoomUpgradeController::refresh);
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            // Not merely a refresh: a join we started must not be able to
            // navigate the next account into the previous account's room.
            const bool notify =
                !m_error.isEmpty() || !m_pendingJoinRoomId.isEmpty();
            // Cleared for the same reason as setClient above: a sign-out
            // makes a later roomJoined impossible, so a surviving token
            // could only ever swallow the next account's navigation.
            m_abandonedJoinRoomId.clear();
            m_pendingJoinRoomId.clear();
            m_roomId.clear();
            m_error.clear();
            if (notify)
                Q_EMIT changed();
            refresh();
        });
    }
    refresh();
}

void RoomUpgradeController::setDiscovery(RoomDiscoveryController *discovery)
{
    if (m_discovery == discovery)
        return;
    if (m_discovery)
        disconnect(m_discovery, nullptr, this, nullptr);
    m_discovery = discovery;
    if (!m_discovery)
        return;

    // Ownership release. Discover emits busyChanged BEFORE roomJoined in
    // finishWaitForRoom, and setError->errorMessageChanged before
    // busyChanged on the failure path, so busyChanged is the authoritative
    // "this join has resolved" edge either way — a roomJoined handler here
    // would be dead code. What roomJoined IS needed for is remembering that
    // the join was ours, which the navigation guard below consumes.
    connect(m_discovery, &RoomDiscoveryController::busyChanged, this, [this] {
        if (m_pendingJoinRoomId.isEmpty() || !m_discovery)
            return;
        if (m_discovery->busy())
            return;
        m_pendingJoinRoomId.clear();
        Q_EMIT changed();
    });

    // The failure is reported into the banner so the user sees the reason
    // WITHOUT leaving the old room — Discover's own error is only visible
    // inside its dialog, which is not open here.
    //
    // Matched by TARGET, not merely by "a join of ours is pending". The
    // shared errorMessage property is also written by knock and
    // knock-withdrawal failures, and cancelKnock() has no busy guard, so
    // clicking "Withdraw" on some other room while this join is in flight
    // would otherwise paint that unrelated refusal into the upgrade banner
    // and swallow the real answer.
    connect(m_discovery, &RoomDiscoveryController::joinFailed, this,
            [this](const QString &target, const QString &message) {
        // Retire the suppression token FIRST, before the ownership check
        // below returns for a join that is no longer ours. A join we
        // abandoned and that then FAILED emits no roomJoined ever, so its
        // token would otherwise outlive it and swallow the navigation of a
        // later, deliberate join of the same room.
        if (m_abandonedJoinRoomId == target)
            m_abandonedJoinRoomId.clear();
        if (m_pendingJoinRoomId.isEmpty() || m_pendingJoinRoomId != target)
            return;
        m_pendingJoinRoomId.clear();
        setError(message);
    });
}

void RoomUpgradeController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // A failure belongs to the room it happened in, and a join started from
    // the previous room must not navigate out of the room the user just
    // opened. Dropping m_pendingJoinRoomId stops OUR handlers, but Discover
    // will still emit roomJoined for it, and AppController's connection to
    // openRoom is unconditional — so the abandoned target is remembered and
    // that navigation is suppressed once.
    const bool notify = !m_error.isEmpty() || !m_pendingJoinRoomId.isEmpty();
    m_error.clear();
    if (!m_pendingJoinRoomId.isEmpty())
        m_abandonedJoinRoomId = m_pendingJoinRoomId;
    m_pendingJoinRoomId.clear();
    // refresh() only notifies when successor/predecessor/access/chain
    // change, and error/busy are not among them — two rooms tombstoned to
    // the same unknown successor would otherwise leave a stale error string
    // on screen in the new room.
    if (notify)
        Q_EMIT changed();
    refresh();
}

void RoomUpgradeController::refresh()
{
    const QString previousSuccessor = m_successorRoomId;
    const QString previousPredecessor = m_predecessorRoomId;
    const int previousAccess = m_successorAccess;
    const bool previousChain = m_chainVerified;

    m_successorRoomId.clear();
    m_predecessorRoomId.clear();
    m_successorAccess = Unknown;
    m_chainVerified = false;

    if (m_client && !m_roomId.isEmpty()) {
        const QList<RoomInfo> rooms = m_client->rooms();
        const RoomInfo *active = nullptr;
        const RoomInfo *successor = nullptr;
        for (const RoomInfo &room : rooms) {
            if (room.id == m_roomId)
                active = &room;
        }
        if (active) {
            m_successorRoomId = active->successorRoomId;
            // The predecessor link is only offered for a room we actually
            // hold. Unlike the successor — where attempting the join is the
            // honest way to discover whether an unknown room is reachable —
            // there is no join step here, so a link to a room we have no
            // record of could only ever open an empty view. A user who
            // joined the successor without ever being in its predecessor is
            // exactly that case.
            const QString predecessor = active->predecessorRoomId;
            if (!predecessor.isEmpty()) {
                for (const RoomInfo &room : rooms) {
                    if (room.id == predecessor) {
                        m_predecessorRoomId = predecessor;
                        break;
                    }
                }
            }
        }
        if (!m_successorRoomId.isEmpty()) {
            for (const RoomInfo &room : rooms) {
                if (room.id == m_successorRoomId)
                    successor = &room;
            }
        }
        if (successor) {
            switch (successor->membership) {
            case RoomInfo::Joined:
                m_successorAccess = Joined;
                break;
            case RoomInfo::Invited:
                m_successorAccess = Invited;
                break;
            case RoomInfo::Knocked:
            case RoomInfo::Left:
                // We hold the room and the user is not in it. This is the
                // one case we can honestly call inaccessible.
                m_successorAccess = NotAccessible;
                break;
            }
            // The defensive check: the successor must point back. A
            // successor that names a different predecessor is not this
            // room's replacement, whatever its tombstone claims.
            m_chainVerified = successor->predecessorRoomId == m_roomId;
        }
        // No successor record at all deliberately leaves Unknown, not
        // NotAccessible — see the header's honesty rules.
    }

    if (m_successorRoomId != previousSuccessor
        || m_predecessorRoomId != previousPredecessor
        || m_successorAccess != previousAccess
        || m_chainVerified != previousChain) {
        Q_EMIT changed();
    }
}

void RoomUpgradeController::setError(const QString &message)
{
    if (m_error == message)
        return;
    m_error = message;
    Q_EMIT changed();
}

void RoomUpgradeController::continueToSuccessor()
{
    if (m_successorRoomId.isEmpty() || busy())
        return;
    setError(QString());

    if (m_successorAccess == Joined) {
        // Already a member: navigate and nothing else. No join, no leave,
        // no marking of the old room.
        //
        // The local COPY is load-bearing. navigateRequested is a direct
        // connection to AppController::openRoom, whose `const QString &`
        // parameter would otherwise alias m_successorRoomId — and openRoom
        // calls setCurrentRoomId, which calls our own setRoomId, whose
        // refresh() clears that very member. The caller's reference would
        // read empty from that point on, and openRoom's remaining work
        // (openRoomTimeline included) would be skipped or, for a chained
        // upgrade, aimed at the grand-successor. Both existing emitters of
        // this shape (RoomDiscoveryController and ConversationController's
        // finishWaitForRoom) copy first for the same reason.
        const QString target = m_successorRoomId;
        Q_EMIT navigateRequested(target);
        return;
    }

    if (m_successorAccess == NotAccessible) {
        setError(QCoreApplication::translate(
            "RoomUpgradeController",
            "You are not a member of the new room, and it cannot be joined "
            "from here."));
        return;
    }

    // Invited, or a successor we have never seen. Join explicitly, then let
    // Discover's settled roomJoined do the navigation. Attempting the join
    // is the only honest way to learn whether an unknown successor is
    // reachable.
    if (!m_discovery || !m_discovery->supported() || m_discovery->busy()) {
        setError(QCoreApplication::translate(
            "RoomUpgradeController",
            "Could not join the new room right now. Try again."));
        return;
    }
    const QString target = m_successorRoomId;
    // A new explicit press is new explicit consent, so any token from an
    // earlier attempt is retired here — but only once the join is actually
    // going ahead. Clearing on ENTRY would also retire it for a press that
    // is then refused (an earlier join still in flight), leaving the
    // original join free to navigate the user out of wherever they end up.
    m_abandonedJoinRoomId.clear();
    m_pendingJoinRoomId = target;
    Q_EMIT changed();
    // No via-servers: the successor is a room id from the room's own
    // tombstone, and the homeserver already knows the room exists because
    // it gave us the state event. Passed as a local for the same aliasing
    // reason as the navigate path above — join() forwards this reference
    // down to the client, and anything that re-entered us would move the
    // member out from under it.
    m_discovery->join(target, QStringList{}, false);
    if (!m_discovery->busy() && !m_pendingJoinRoomId.isEmpty()) {
        // join() refused before starting (unsupported backend, or an
        // immediate failure). errorMessageChanged has already run if there
        // was a message; clear the pending state either way so the banner
        // does not sit disabled forever.
        m_pendingJoinRoomId.clear();
        if (m_error.isEmpty()) {
            setError(QCoreApplication::translate(
                "RoomUpgradeController",
                "Could not join the new room right now. Try again."));
        } else {
            Q_EMIT changed();
        }
    }
}

void RoomUpgradeController::goToPredecessor()
{
    if (m_predecessorRoomId.isEmpty())
        return;
    setError(QString());
    m_abandonedJoinRoomId.clear();
    // The predecessor is a room the user was already in — there is nothing
    // to join, and nothing to verify beyond it being a room we hold.
    // Copied before emitting: see continueToSuccessor for why a live member
    // must never be handed to a direct connection that re-enters us.
    const QString target = m_predecessorRoomId;
    Q_EMIT navigateRequested(target);
}

bool RoomUpgradeController::consumeAbandonedJoin(const QString &roomId)
{
    if (roomId.isEmpty() || m_abandonedJoinRoomId != roomId)
        return false;
    // Consumed once. A later, deliberate join of the same room — from the
    // banner again, or from Discover — must navigate normally.
    m_abandonedJoinRoomId.clear();
    return true;
}
