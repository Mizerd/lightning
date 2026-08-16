#include "app/RoomDiscoveryController.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

namespace {
// Bound for waiting on the authoritative room-list appearance of a freshly
// joined room. If sync has not delivered it by then, open by id anyway —
// the membership exists server-side; the list entry follows.
constexpr int kRoomWaitTimeoutMs = 10000;
} // namespace

RoomDiscoveryController::RoomDiscoveryController(QObject *parent)
    : QObject(parent)
    , m_directory(new RoomDirectorySearchModel(this))
{
    m_roomWaitTimeout.setSingleShot(true);
    m_roomWaitTimeout.setInterval(kRoomWaitTimeoutMs);
    connect(&m_roomWaitTimeout, &QTimer::timeout, this, [this]() {
        if (m_waitingForRoom)
            finishWaitForRoom();
    });
}

void RoomDiscoveryController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_directory->setClient(client);
    onLoggedOut(); // reset every pending id and cache for the new client
    if (m_client) {
        connect(m_client, &MatrixClient::roomTargetResolved, this,
                &RoomDiscoveryController::onTargetResolved);
        connect(m_client, &MatrixClient::roomJoinFinished, this,
                &RoomDiscoveryController::onJoinFinished);
        connect(m_client, &MatrixClient::roomKnockFinished, this,
                &RoomDiscoveryController::onKnockFinished);
        connect(m_client, &MatrixClient::knockCancelFinished, this,
                &RoomDiscoveryController::onKnockCancelFinished);
        connect(m_client, &MatrixClient::spaceChildrenReceived, this,
                &RoomDiscoveryController::onSpaceChildren);
        connect(m_client, &MatrixClient::roomsChanged, this,
                &RoomDiscoveryController::onRoomsChanged);
        connect(m_client, &MatrixClient::loggedOut, this,
                &RoomDiscoveryController::onLoggedOut);
    }
    Q_EMIT supportedChanged();
}

bool RoomDiscoveryController::supported() const
{
    return m_client && m_client->supportsRoomDiscovery();
}

void RoomDiscoveryController::resolve(const QString &input)
{
    if (!supported())
        return;
    m_resolved.clear();
    const quint64 opId = m_client->resolveRoomTarget(input);
    if (opId == 0) {
        m_resolveOp = 0;
        m_resolveState = QStringLiteral("failed");
        m_resolved.insert(QStringLiteral("category"),
                          QStringLiteral("invalid"));
        Q_EMIT resolveChanged();
        return;
    }
    m_resolveOp = opId;
    m_resolveState = QStringLiteral("resolving");
    Q_EMIT resolveChanged();
}

void RoomDiscoveryController::clearResolved()
{
    m_resolveOp = 0;
    m_resolveState = QStringLiteral("idle");
    m_resolved.clear();
    Q_EMIT resolveChanged();
}

void RoomDiscoveryController::join(const QString &target,
                                   const QStringList &via, bool isSpace)
{
    if (!supported() || busy())
        return;
    const quint64 opId = m_client->joinRoomByIdOrAlias(target, via);
    if (opId == 0) {
        setError(describeJoinCategory(QStringLiteral("network")));
        return;
    }
    m_joinOp = opId;
    m_joinTarget = target;
    m_awaitedIsSpace = isSpace;
    m_errorMessage.clear();
    Q_EMIT errorMessageChanged();
    Q_EMIT busyChanged();
}

void RoomDiscoveryController::knock(const QString &target,
                                    const QStringList &via,
                                    const QString &reason)
{
    if (!supported() || busy() || m_knockOp != 0)
        return;
    const quint64 opId = m_client->knockRoom(target, via, reason);
    if (opId == 0) {
        setError(describeJoinCategory(QStringLiteral("network")));
        return;
    }
    m_knockOp = opId;
    m_errorMessage.clear();
    Q_EMIT errorMessageChanged();
}

void RoomDiscoveryController::cancelKnock(const QString &roomId)
{
    if (!supported() || m_cancelKnockOp != 0)
        return;
    const quint64 opId = m_client->cancelKnock(roomId);
    if (opId == 0)
        return;
    m_cancelKnockOp = opId;
}

void RoomDiscoveryController::refreshSpaceChildren(const QString &spaceId)
{
    if (!supported() || spaceId.isEmpty())
        return;
    SpaceChildrenEntry &entry = m_spaceChildren[spaceId];
    if (entry.pendingOp != 0)
        return; // one in-flight listing per space
    const quint64 opId = m_client->requestSpaceChildren(spaceId);
    if (opId == 0) {
        entry.state = QStringLiteral("failed");
        Q_EMIT spaceChildrenChanged(spaceId);
        return;
    }
    entry.pendingOp = opId;
    if (entry.state.isEmpty() || entry.state == QStringLiteral("failed"))
        entry.state = QStringLiteral("loading");
    Q_EMIT spaceChildrenChanged(spaceId);
}

QVariantList RoomDiscoveryController::spaceChildren(const QString &spaceId) const
{
    return m_spaceChildren.value(spaceId).rows;
}

QString RoomDiscoveryController::spaceChildrenState(const QString &spaceId) const
{
    return m_spaceChildren.value(spaceId).state;
}

void RoomDiscoveryController::clearError()
{
    if (m_errorMessage.isEmpty())
        return;
    m_errorMessage.clear();
    Q_EMIT errorMessageChanged();
}

void RoomDiscoveryController::onTargetResolved(quint64 opId,
                                               const QVariantMap &result)
{
    if (opId == 0 || opId != m_resolveOp)
        return; // superseded resolution
    m_resolveOp = 0;
    m_resolved = result;
    m_resolveState = result.value(QStringLiteral("ok")).toBool()
                         ? QStringLiteral("resolved")
                         : QStringLiteral("failed");
    Q_EMIT resolveChanged();
}

void RoomDiscoveryController::onJoinFinished(quint64 opId, bool ok,
                                             const QString &roomId,
                                             const QString &category)
{
    if (opId == 0 || opId != m_joinOp)
        return;
    m_joinOp = 0;
    if (!ok) {
        m_awaitedIsSpace = false;
        const QString target = m_joinTarget;
        m_joinTarget.clear();
        const QString message = describeJoinCategory(category);
        setError(message);
        // Targeted, in addition to the shared errorMessage. A consumer that
        // started one specific join (the room-upgrade banner) cannot tell
        // from errorMessageChanged alone whether the failure was its own —
        // knock and knock-withdrawal failures set the same property, and
        // cancelKnock() has no busy guard at all, so one can land while a
        // join is in flight and be mistaken for its result.
        //
        // BEFORE busyChanged, deliberately. A consumer that releases its
        // ownership on the busy edge would otherwise have already forgotten
        // which join this was by the time the reason arrives, and the
        // failure would go unreported.
        Q_EMIT joinFailed(target, message);
        Q_EMIT busyChanged();
        return;
    }
    m_joinTarget.clear();
    beginWaitForRoom(roomId, m_awaitedIsSpace);
}

void RoomDiscoveryController::onKnockFinished(quint64 opId, bool ok,
                                              const QString &roomId,
                                              const QString &category)
{
    if (opId == 0 || opId != m_knockOp)
        return;
    m_knockOp = 0;
    if (!ok) {
        setError(describeJoinCategory(category));
        return;
    }
    Q_EMIT knockSent(roomId);
}

void RoomDiscoveryController::onKnockCancelFinished(quint64 opId, bool ok,
                                                    const QString &roomId,
                                                    const QString &category)
{
    if (opId == 0 || opId != m_cancelKnockOp)
        return;
    m_cancelKnockOp = 0;
    if (!ok) {
        setError(describeJoinCategory(category));
        return;
    }
    Q_EMIT knockCancelled(roomId);
}

void RoomDiscoveryController::onSpaceChildren(quint64 opId,
                                              const QString &spaceId, bool ok,
                                              const QVariantList &rooms,
                                              bool truncated,
                                              const QString &category)
{
    Q_UNUSED(truncated);
    Q_UNUSED(category);
    auto it = m_spaceChildren.find(spaceId);
    if (it == m_spaceChildren.end() || opId == 0 || it->pendingOp != opId)
        return;
    it->pendingOp = 0;
    if (!ok) {
        // A failed refresh keeps any previously shown rows: a flaky
        // connection must not read as "this space has no more rooms".
        it->state = it->rows.isEmpty() ? QStringLiteral("failed")
                                       : QStringLiteral("ready");
        Q_EMIT spaceChildrenChanged(spaceId);
        return;
    }
    it->rows = rooms;
    it->state = QStringLiteral("ready");
    Q_EMIT spaceChildrenChanged(spaceId);
}

void RoomDiscoveryController::onRoomsChanged()
{
    if (!m_waitingForRoom || !m_client)
        return;
    const auto rooms = m_client->rooms();
    for (const auto &room : rooms) {
        if (room.id == m_awaitedRoomId) {
            finishWaitForRoom();
            return;
        }
    }
}

void RoomDiscoveryController::onLoggedOut()
{
    // A signed-out session must never navigate or surface late errors.
    m_resolveOp = 0;
    m_resolveState = QStringLiteral("idle");
    m_resolved.clear();
    m_joinOp = 0;
    m_knockOp = 0;
    m_cancelKnockOp = 0;
    m_waitingForRoom = false;
    m_awaitedIsSpace = false;
    m_awaitedRoomId.clear();
    m_roomWaitTimeout.stop();
    m_errorMessage.clear();
    m_spaceChildren.clear();
    Q_EMIT resolveChanged();
    Q_EMIT busyChanged();
    Q_EMIT errorMessageChanged();
}

void RoomDiscoveryController::setError(const QString &message)
{
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}

void RoomDiscoveryController::beginWaitForRoom(const QString &roomId,
                                               bool isSpace)
{
    m_waitingForRoom = true;
    m_awaitedRoomId = roomId;
    m_awaitedIsSpace = isSpace;
    m_roomWaitTimeout.start();
    Q_EMIT busyChanged();
    // The room may already be in the local store (join inserts it), so
    // check immediately as well as on future updates.
    onRoomsChanged();
}

void RoomDiscoveryController::finishWaitForRoom()
{
    if (!m_waitingForRoom)
        return;
    const QString roomId = m_awaitedRoomId;
    const bool isSpace = m_awaitedIsSpace;
    m_waitingForRoom = false;
    m_awaitedRoomId.clear();
    m_awaitedIsSpace = false;
    m_roomWaitTimeout.stop();
    Q_EMIT busyChanged();
    if (isSpace)
        Q_EMIT spaceJoined(roomId);
    else
        Q_EMIT roomJoined(roomId);
}

QString RoomDiscoveryController::describeJoinCategory(const QString &category)
{
    if (category == QLatin1String("banned")) {
        return QCoreApplication::translate("RoomDiscoveryController",
                                           "You are banned from this room.");
    }
    if (category == QLatin1String("forbidden")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "This room is invite-only, or you do not have permission to "
            "join it.");
    }
    if (category == QLatin1String("restricted_denied")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "This room is restricted: joining requires membership of "
            "another room or space you are not in.");
    }
    if (category == QLatin1String("not_found")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "No room with that address could be found.");
    }
    if (category == QLatin1String("rate_limited")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "The server is rate limiting this action. Try again shortly.");
    }
    if (category == QLatin1String("invalid")) {
        return QCoreApplication::translate(
            "RoomDiscoveryController",
            "That is not a valid room address or link.");
    }
    return QCoreApplication::translate(
        "RoomDiscoveryController",
        "Could not reach the server. Try again.");
}
