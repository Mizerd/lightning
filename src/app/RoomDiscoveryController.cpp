#include "app/RoomDiscoveryController.h"

#include "app/JoinErrorText.h"
#include "matrix/MatrixClient.h"

#include <QCoreApplication>

namespace {
constexpr int kRoomWaitTimeoutMs = 10000;
}

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
    onLoggedOut();
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
    m_keepRoomOnJoin = false;
    joinInternal(target, via, isSpace);
}

void RoomDiscoveryController::joinSpaceKeepingRoom(const QString &target,
                                                   const QStringList &via)
{
    if (!supported() || busy())
        return;
    joinInternal(target, via, true);
    m_keepRoomOnJoin = m_joinOp != 0;
}

void RoomDiscoveryController::joinInternal(const QString &target,
                                           const QStringList &via,
                                           bool isSpace)
{
    const quint64 opId = m_client->joinRoomByIdOrAlias(target, via);
    if (opId == 0) {
        const QString message = describeJoinCategory(QStringLiteral("network"));
        setError(message);
        Q_EMIT joinFailed(target, message);
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
        return;
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
        return;
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
        m_keepRoomOnJoin = false;
        const QString target = m_joinTarget;
        m_joinTarget.clear();
        const QString message = describeJoinCategory(category);
        if (category == QLatin1String("not_found")
            && m_resolveState == QLatin1String("resolved")) {
            m_resolveOp = 0;
            m_resolveState = QStringLiteral("idle");
            m_resolved.clear();
            Q_EMIT resolveChanged();
        }
        setError(message);
        // Emitted before busyChanged, so a consumer that releases ownership on
        // the busy edge still knows which join failed.
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
    m_resolveOp = 0;
    m_resolveState = QStringLiteral("idle");
    m_resolved.clear();
    m_joinOp = 0;
    m_knockOp = 0;
    m_cancelKnockOp = 0;
    m_waitingForRoom = false;
    m_awaitedIsSpace = false;
    m_keepRoomOnJoin = false;
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
    const bool keepRoom = m_keepRoomOnJoin;
    m_keepRoomOnJoin = false;
    m_roomWaitTimeout.stop();
    Q_EMIT busyChanged();
    if (isSpace && keepRoom)
        Q_EMIT spaceJoinedInPlace(roomId);
    else if (isSpace)
        Q_EMIT spaceJoined(roomId);
    else
        Q_EMIT roomJoined(roomId);
}

QString RoomDiscoveryController::describeJoinCategory(const QString &category)
{
    return joinerror::describe(category);
}
