#include "app/RoomUpgradeController.h"

#include "app/RoomDiscoveryController.h"
#include "matrix/MatrixClient.h"
#include "matrix/RoomInfo.h"
#include "spaces/SpaceManager.h"

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
    // No roomJoined can follow a client change, so a surviving token could
    // only swallow the next account's navigation.
    m_abandonedJoinRoomId.clear();
    m_pendingJoinRoomId.clear();
    m_error.clear();
    if (notify)
        Q_EMIT changed();
    if (m_client) {
        // Every tombstone source (room-list diffs and the tombstone poke)
        // lands in roomsChanged, so there is one derivation path.
        connect(m_client, &MatrixClient::roomsChanged, this,
                &RoomUpgradeController::refresh);
        connect(m_client, &MatrixClient::roomVersionsReceived, this,
                &RoomUpgradeController::onRoomVersionsReceived);
        connect(m_client, &MatrixClient::roomUpgradeFinished, this,
                &RoomUpgradeController::onRoomUpgradeFinished);
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            // Our join must not navigate the next account into this
            // account's room.
            const bool notify =
                !m_error.isEmpty() || !m_pendingJoinRoomId.isEmpty();
            m_abandonedJoinRoomId.clear();
            m_pendingJoinRoomId.clear();
            m_roomId.clear();
            m_error.clear();
            if (notify)
                Q_EMIT changed();
            // An in-flight upgrade dies with the session, and the version
            // list belongs to the previous homeserver.
            const bool upgradeNotify = m_upgradeOp != 0 || !m_upgradeError.isEmpty();
            m_upgradeOp = 0;
            m_upgradeRoomId.clear();
            m_upgradeAddToSpaces = false;
            m_upgradeError.clear();
            m_versionsKnown = false;
            m_defaultVersion.clear();
            m_availableVersions.clear();
            Q_EMIT versionsChanged();
            if (upgradeNotify)
                Q_EMIT upgradeStateChanged();
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

    // busyChanged is the "join resolved" edge on both success and failure;
    // Discover emits it before roomJoined.
    connect(m_discovery, &RoomDiscoveryController::busyChanged, this, [this] {
        if (m_pendingJoinRoomId.isEmpty() || !m_discovery)
            return;
        if (m_discovery->busy())
            return;
        m_pendingJoinRoomId.clear();
        Q_EMIT changed();
    });

    // Report the failure in the banner. Matched by target: Discover's error
    // is also written by unrelated knock/withdraw failures.
    connect(m_discovery, &RoomDiscoveryController::joinFailed, this,
            [this](const QString &target, const QString &message) {
        // Retire the suppression token first: a failed abandoned join never
        // emits roomJoined, so the token would outlive it.
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
    // A join started from the previous room must not navigate out of this
    // one. Discover will still emit roomJoined, so remember the abandoned
    // target and suppress that navigation once.
    const bool notify = !m_error.isEmpty() || !m_pendingJoinRoomId.isEmpty();
    m_error.clear();
    if (!m_pendingJoinRoomId.isEmpty())
        m_abandonedJoinRoomId = m_pendingJoinRoomId;
    m_pendingJoinRoomId.clear();
    // refresh() does not notify for error/busy changes.
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
            // Only offer a predecessor we hold: the link has no join step.
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
                m_successorAccess = NotAccessible;
                break;
            }
            // A successor naming a different predecessor is not this room's
            // replacement, whatever the tombstone claims.
            m_chainVerified = successor->predecessorRoomId == m_roomId;
        }
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
        // Copy first: openRoom() is a direct connection that re-enters
        // setRoomId(), whose refresh() clears m_successorRoomId while the
        // receiver still holds a reference to it.
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

    // Invited or Unknown: join, and let Discover's roomJoined navigate.
    if (!m_discovery || !m_discovery->supported() || m_discovery->busy()) {
        setError(QCoreApplication::translate(
            "RoomUpgradeController",
            "Could not join the new room right now. Try again."));
        return;
    }
    const QString target = m_successorRoomId;
    // A new press is new consent, so retire an earlier token, but only once
    // the join is actually going ahead.
    m_abandonedJoinRoomId.clear();
    m_pendingJoinRoomId = target;
    Q_EMIT changed();
    // No via-servers: the homeserver sent us the tombstone. A local copy for
    // the same aliasing reason as above.
    m_discovery->join(target, QStringList{}, false);
    if (!m_discovery->busy() && !m_pendingJoinRoomId.isEmpty()) {
        // join() refused before starting; clear pending state so the banner
        // does not stay disabled.
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
    // Copied before emitting; see continueToSuccessor().
    const QString target = m_predecessorRoomId;
    Q_EMIT navigateRequested(target);
}

bool RoomUpgradeController::consumeAbandonedJoin(const QString &roomId)
{
    if (roomId.isEmpty() || m_abandonedJoinRoomId != roomId)
        return false;
    m_abandonedJoinRoomId.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Send side
// ---------------------------------------------------------------------------

void RoomUpgradeController::requestRoomVersions()
{
    if (!m_client)
        return;
    m_client->requestRoomVersions();
}

void RoomUpgradeController::onRoomVersionsReceived(bool ok,
                                                   const QString &defaultVersion,
                                                   const QVariantList &available)
{
    // On failure, no list rather than a guessed one.
    m_versionsKnown = ok;
    m_defaultVersion = ok ? defaultVersion : QString();
    m_availableVersions = ok ? available : QVariantList();
    Q_EMIT versionsChanged();
}

void RoomUpgradeController::upgradeRoom(const QString &roomId,
                                        const QString &newVersion,
                                        bool addToSameSpaces)
{
    if (!m_client || upgradeBusy())
        return;
    // Never fall back to m_roomId; see the header. An absent target is
    // reported rather than ignored.
    const QString target = roomId.trimmed();
    if (target.isEmpty()) {
        m_upgradeError = QCoreApplication::translate(
            "RoomUpgradeController", "The room to upgrade is not known.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    const QString version = newVersion.trimmed();
    // Only a version the server advertised.
    bool advertised = false;
    for (const QVariant &row : std::as_const(m_availableVersions)) {
        if (row.toMap().value(QStringLiteral("version")).toString() == version) {
            advertised = true;
            break;
        }
    }
    if (!m_versionsKnown || !advertised) {
        m_upgradeError = QCoreApplication::translate(
            "RoomUpgradeController",
            "Pick a room version the server supports.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    const quint64 opId = m_client->upgradeRoom(target, version);
    if (opId == 0) {
        m_upgradeError = QCoreApplication::translate(
            "RoomUpgradeController", "The upgrade could not be requested.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    m_upgradeOp = opId;
    m_upgradeRoomId = target;
    m_upgradeAddToSpaces = addToSameSpaces;
    m_upgradeError.clear();
    Q_EMIT upgradeStateChanged();
}

void RoomUpgradeController::onRoomUpgradeFinished(quint64 opId,
                                                  const QString &roomId,
                                                  bool ok,
                                                  const QString &replacementRoomId,
                                                  const QString &category)
{
    if (opId == 0 || opId != m_upgradeOp)
        return;
    m_upgradeOp = 0;
    // Destructive, so check the target even though the op id pins it.
    if (!m_upgradeRoomId.isEmpty() && roomId != m_upgradeRoomId) {
        m_upgradeError = QCoreApplication::translate(
            "RoomUpgradeController",
            "The upgrade did not complete. Nothing was changed.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    if (!ok) {
        // Sanitized category only — never server text.
        m_upgradeError = category == QLatin1String("forbidden")
            ? QCoreApplication::translate(
                  "RoomUpgradeController",
                  "The server refused the upgrade: you are not allowed to "
                  "upgrade this room.")
            : QCoreApplication::translate(
                  "RoomUpgradeController",
                  "The upgrade did not complete. Nothing was changed.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    if (replacementRoomId.isEmpty()) {
        // Not a failure: the old room is already tombstoned, so "nothing was
        // changed" would be false.
        m_upgradeError = QCoreApplication::translate(
            "RoomUpgradeController",
            "The room was upgraded, but the server did not say which room "
            "replaced it. Look for the new room in your room list.");
        Q_EMIT upgradeStateChanged();
        return;
    }
    m_lastReplacementRoomId = replacementRoomId;
    // Add the replacement to every Space listing the old room. The old child
    // is kept so its history stays reachable; a refused write is not
    // reported.
    if (m_upgradeAddToSpaces && m_spaces) {
        const QVariantList spaces = m_spaces->allSpaces();
        for (const QVariant &value : spaces) {
            const QString spaceId =
                value.toMap().value(QStringLiteral("spaceId")).toString();
            if (spaceId.isEmpty() || !m_spaces->includesRoom(spaceId, roomId))
                continue;
            m_spaces->addRoomToSpace(spaceId, replacementRoomId);
        }
    }
    Q_EMIT upgradeStateChanged();
    // The upgrader is already a member of the replacement.
    Q_EMIT navigateRequested(replacementRoomId);
}
