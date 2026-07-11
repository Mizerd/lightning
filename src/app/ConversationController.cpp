#include "app/ConversationController.h"

#include "matrix/MatrixClient.h"
#include "models/UserSearchModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcConv, "matrix.conversations")

namespace {
// Bound for waiting on the authoritative room-list appearance of a freshly
// created room. If sync has not delivered it by then, open by id anyway —
// the room exists server-side; the list entry follows.
constexpr int kRoomWaitTimeoutMs = 10000;
} // namespace

ConversationController::ConversationController(QObject *parent)
    : QObject(parent)
    , m_userSearch(new UserSearchModel(this))
{
    m_roomWaitTimeout.setSingleShot(true);
    m_roomWaitTimeout.setInterval(kRoomWaitTimeoutMs);
    connect(&m_roomWaitTimeout, &QTimer::timeout, this, [this]() {
        if (m_waitingForRoom)
            finishWaitForRoom();
    });
}

void ConversationController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_userSearch->setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::dmCreateFinished,
                this, &ConversationController::onDmCreateFinished);
        connect(m_client, &MatrixClient::roomCreateFinished,
                this, &ConversationController::onRoomCreateFinished);
        connect(m_client, &MatrixClient::inviteUserFinished,
                this, &ConversationController::onInviteUserFinished);
        connect(m_client, &MatrixClient::inviteBatchFinished,
                this, &ConversationController::onInviteBatchFinished);
        connect(m_client, &MatrixClient::roomsChanged,
                this, &ConversationController::onRoomsChanged);
        connect(m_client, &MatrixClient::loggedOut,
                this, &ConversationController::onLoggedOut);
    }
    Q_EMIT supportedChanged();
}

bool ConversationController::supported() const
{
    return m_client && m_client->supportsRoomManagement();
}

QString ConversationController::describeCategory(const QString &category)
{
    if (category == QLatin1String("forbidden"))
        return tr("You do not have permission to do that.");
    if (category == QLatin1String("rate_limited"))
        return tr("The server is rate-limiting requests. Try again shortly.");
    if (category == QLatin1String("alias_taken"))
        return tr("That room address is already in use.");
    if (category == QLatin1String("invalid"))
        return tr("The server rejected the request as invalid.");
    if (category == QLatin1String("not_found"))
        return tr("Not found on this server.");
    return tr("A network or server error occurred.");
}

void ConversationController::checkExistingDm(const QString &userId)
{
    m_existingDms.clear();
    if (m_client && !userId.isEmpty())
        m_existingDms = m_client->existingDirectRooms(userId);
    Q_EMIT existingDmsChanged();
}

void ConversationController::startDirectMessage(const QString &userId)
{
    if (!m_client || busy() || userId.isEmpty())
        return;
    if (userId == m_client->currentUserId()) {
        setError(tr("You cannot start a direct message with yourself."));
        return;
    }
    clearError();
    const quint64 opId = m_client->createDirectChat(userId);
    if (opId == 0) {
        setError(tr("Starting direct messages is not supported on this backend."));
        return;
    }
    m_pendingOp = opId;
    Q_EMIT busyChanged();
}

void ConversationController::createRoom(const QVariantMap &options)
{
    if (!m_client || busy())
        return;
    if (options.value(QStringLiteral("name")).toString().trimmed().isEmpty()) {
        setError(tr("The room needs a name."));
        return;
    }
    clearError();
    const quint64 opId = m_client->createRoom(options);
    if (opId == 0) {
        setError(tr("Creating rooms is not supported on this backend."));
        return;
    }
    m_pendingOp = opId;
    Q_EMIT busyChanged();
}

void ConversationController::inviteUsers(const QString &roomId,
                                         const QStringList &userIds)
{
    if (!m_client || busy() || roomId.isEmpty() || userIds.isEmpty())
        return;
    clearError();
    QStringList unique;
    for (const QString &user : userIds) {
        if (!unique.contains(user))
            unique.append(user);
    }
    const quint64 opId = m_client->inviteUsers(roomId, unique);
    if (opId == 0) {
        setError(tr("Inviting users is not supported on this backend."));
        return;
    }
    m_pendingOp = opId;
    m_inviteResults.clear();
    for (const QString &user : unique) {
        QVariantMap row;
        row.insert(QStringLiteral("userId"), user);
        row.insert(QStringLiteral("state"), QStringLiteral("pending"));
        row.insert(QStringLiteral("category"), QString());
        m_inviteResults.append(row);
    }
    Q_EMIT inviteResultsChanged();
    Q_EMIT busyChanged();
}

void ConversationController::clearError()
{
    if (m_errorMessage.isEmpty())
        return;
    m_errorMessage.clear();
    Q_EMIT errorMessageChanged();
}

void ConversationController::reset()
{
    m_pendingOp = 0;
    m_waitingForRoom = false;
    m_awaitedRoomId.clear();
    m_roomWaitTimeout.stop();
    m_existingDms.clear();
    m_inviteResults.clear();
    clearError();
    Q_EMIT existingDmsChanged();
    Q_EMIT inviteResultsChanged();
    Q_EMIT busyChanged();
}

void ConversationController::onDmCreateFinished(quint64 opId, bool ok,
                                                const QString &roomId,
                                                const QString &category)
{
    if (opId != m_pendingOp || m_pendingOp == 0)
        return; // stale or foreign completion
    m_pendingOp = 0;
    if (!ok || roomId.isEmpty()) {
        setError(describeCategory(category));
        Q_EMIT busyChanged();
        return;
    }
    qCInfo(lcConv) << "dm created";
    beginWaitForRoom(roomId);
}

void ConversationController::onRoomCreateFinished(quint64 opId, bool ok,
                                                  const QString &roomId,
                                                  const QString &category,
                                                  const QString &warning)
{
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    m_pendingOp = 0;
    if (!ok || roomId.isEmpty()) {
        setError(describeCategory(category));
        Q_EMIT busyChanged();
        return;
    }
    qCInfo(lcConv) << "room created";
    if (warning == QLatin1String("space_add_failed"))
        Q_EMIT spacePlacementFailed(roomId);
    beginWaitForRoom(roomId);
}

void ConversationController::onInviteUserFinished(quint64 opId,
                                                  const QString &roomId,
                                                  const QString &userId,
                                                  bool ok,
                                                  const QString &category)
{
    Q_UNUSED(roomId);
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    for (QVariant &value : m_inviteResults) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId)
            continue;
        row.insert(QStringLiteral("state"),
                   ok ? QStringLiteral("ok") : QStringLiteral("failed"));
        row.insert(QStringLiteral("category"), category);
        value = row;
        break;
    }
    Q_EMIT inviteResultsChanged();
}

void ConversationController::onInviteBatchFinished(quint64 opId,
                                                   const QString &roomId,
                                                   int okCount, int failCount)
{
    Q_UNUSED(roomId);
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    m_pendingOp = 0;
    Q_EMIT busyChanged();
    Q_EMIT inviteBatchCompleted(okCount, failCount);
}

void ConversationController::beginWaitForRoom(const QString &roomId)
{
    m_waitingForRoom = true;
    m_awaitedRoomId = roomId;
    m_roomWaitTimeout.start();
    Q_EMIT busyChanged();
    // The room may already be in the local store (create_room inserts it),
    // so check immediately as well as on future updates.
    onRoomsChanged();
}

void ConversationController::onRoomsChanged()
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

void ConversationController::finishWaitForRoom()
{
    if (!m_waitingForRoom)
        return;
    const QString roomId = m_awaitedRoomId;
    m_waitingForRoom = false;
    m_awaitedRoomId.clear();
    m_roomWaitTimeout.stop();
    Q_EMIT busyChanged();
    Q_EMIT conversationReady(roomId);
}

void ConversationController::onLoggedOut()
{
    // A signed-out session must never open rooms or surface late errors.
    reset();
}

void ConversationController::setError(const QString &message)
{
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}
