#include "app/ConversationController.h"

#include "matrix/MatrixClient.h"
#include "models/UserSearchModel.h"

#include <QLoggingCategory>
#include <QUrl>

Q_LOGGING_CATEGORY(lcConv, "matrix.conversations")

namespace {
// After this, open a created room by id even if sync has not listed it yet.
constexpr int kRoomWaitTimeoutMs = 10000;

// Generous, because a federated invite is slow; it only stops a permanent hang.
constexpr int kCreateOpTimeoutMs = 60000;

// How long a create that outlived kCreateOpTimeoutMs still blocks a second
// create for the same person. The server keeps working on it (the request has
// no cancel and no timeout of its own), and each retry leaves one more room.
constexpr int kAbandonedOpGuardMs = 10 * 60 * 1000;
} // namespace

ConversationController::ConversationController(QObject *parent)
    : QObject(parent)
    , m_userSearch(new UserSearchModel(this))
{
    m_abandonedGuard.setSingleShot(true);
    m_abandonedGuard.setInterval(kAbandonedOpGuardMs);
    connect(&m_abandonedGuard, &QTimer::timeout, this,
            [this]() { clearAbandonedDm(); });
    m_roomWaitTimeout.setSingleShot(true);
    m_roomWaitTimeout.setInterval(kRoomWaitTimeoutMs);
    connect(&m_roomWaitTimeout, &QTimer::timeout, this, [this]() {
        if (m_waitingForRoom)
            finishWaitForRoom();
    });
    m_opTimeout.setSingleShot(true);
    m_opTimeout.setInterval(kCreateOpTimeoutMs);
    connect(&m_opTimeout, &QTimer::timeout, this, [this]() {
        if (m_pendingOp == 0)
            return;
        // The create is still running server-side: remember who it was for so
        // that a retry for the SAME person is refused until it answers, instead
        // of leaving one more empty room per retry.
        if (!m_pendingDmUser.isEmpty()) {
            m_abandonedDms.insert(m_pendingOp, m_pendingDmUser);
            m_abandonedGuard.start();
        }
        m_pendingDmUser.clear();
        m_pendingOp = 0;
        // The server may still create the room, so do not claim failure.
        setError(tr("This is taking longer than expected. The other person's "
                    "server may be slow to respond. If a room appears in your "
                    "list, use that one rather than starting another."));
        Q_EMIT busyChanged();
    });
}

void ConversationController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    // An account switch: the other account's abandoned creates mean nothing.
    clearAbandonedDm();
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
        connect(m_client, &MatrixClient::roomEditFinished,
                this, &ConversationController::onRoomEditFinished);
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
    // M_UNRECOGNIZED: the endpoint is not implemented; retrying cannot help.
    if (category == QLatin1String("unrecognized"))
        return tr("Your homeserver does not support that.");
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
    if (m_abandonedDms.key(userId, 0) != 0) {
        setError(tr("A conversation with this person is still being created "
                    "and their server has not answered yet. If an empty room "
                    "appears in your list, use that one; starting another "
                    "would only add more empty rooms."));
        return;
    }
    clearError();
    const quint64 opId = m_client->createDirectChat(userId);
    if (opId == 0) {
        setError(tr("Starting direct messages is not supported on this backend."));
        return;
    }
    m_pendingIsSpace = false;
    m_pendingOp = opId;
    m_pendingDmUser = userId;
    m_opTimeout.start();
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
    // The avatar is applied after creation through the room-edit path.
    QVariantMap createOptions = options;
    const QString rawAvatar =
        createOptions.take(QStringLiteral("avatarPath")).toString();
    const quint64 opId = m_client->createRoom(createOptions);
    if (opId == 0) {
        setError(tr("Creating rooms is not supported on this backend."));
        return;
    }
    const QUrl avatarUrl(rawAvatar);
    m_pendingAvatarPath = avatarUrl.isLocalFile() ? avatarUrl.toLocalFile()
                                                  : rawAvatar;
    m_pendingIsSpace = options.value(QStringLiteral("isSpace")).toBool();
    m_pendingOp = opId;
    m_opTimeout.start();
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
    m_opTimeout.start();
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
    // Closing the dialog does not cancel a server-side create, so keep
    // `m_pendingOp`: clearing it let users retry a hung create and leave a
    // stray room each time. `m_opTimeout` bounds the guard instead.
    m_waitingForRoom = false;
    m_awaitedRoomId.clear();
    m_pendingIsSpace = false;
    m_roomWaitTimeout.stop();
    m_pendingAvatarPath.clear();
    m_avatarOp = 0;
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
    // The create we stopped waiting for has answered: lift its guard. The
    // dialog was already told, so the room is not opened out from under
    // whatever the user is doing now.
    if (m_abandonedDms.contains(opId)) {
        m_abandonedDms.remove(opId);
        if (m_abandonedDms.isEmpty())
            m_abandonedGuard.stop();
        return;
    }
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    m_opTimeout.stop();
    m_pendingOp = 0;
    m_pendingDmUser.clear();
    if (!ok || roomId.isEmpty()) {
        QString text = describeCategory(category);
        // /createRoom creates the room BEFORE the invite is federated, so a
        // failure that is not a plain refusal can leave an empty room that the
        // SDK never returns to us and so cannot be reused or cleaned up here.
        const bool refusedBeforeCreation = category == QLatin1String("forbidden")
            || category == QLatin1String("rate_limited")
            || category == QLatin1String("invalid")
            || category == QLatin1String("unrecognized")
            || category == QLatin1String("not_found");
        if (!refusedBeforeCreation)
            text += QLatin1Char(' ')
                + tr("An empty room may have been created on your account; if "
                     "you see one in your room list you can leave it.");
        setError(text);
        Q_EMIT busyChanged();
        return;
    }
    qCInfo(lcConv) << "dm created";
    beginWaitForRoom(roomId);
}

void ConversationController::clearAbandonedDm()
{
    m_abandonedDms.clear();
    m_abandonedGuard.stop();
}

void ConversationController::setOpTimeoutMsForTest(int ms)
{
    m_opTimeout.setInterval(ms);
}

void ConversationController::onRoomCreateFinished(quint64 opId, bool ok,
                                                  const QString &roomId,
                                                  const QString &category,
                                                  const QString &warning)
{
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    m_opTimeout.stop();
    m_pendingOp = 0;
    if (!ok || roomId.isEmpty()) {
        m_pendingAvatarPath.clear();
        setError(describeCategory(category));
        Q_EMIT busyChanged();
        return;
    }
    qCInfo(lcConv) << "room created";
    if (warning == QLatin1String("space_add_failed"))
        Q_EMIT spacePlacementFailed(roomId);
    // The room opens regardless of how this completes.
    if (!m_pendingAvatarPath.isEmpty()) {
        const QString path = m_pendingAvatarPath;
        m_pendingAvatarPath.clear();
        m_avatarOp = m_client->setRoomAvatar(roomId, path);
        if (m_avatarOp == 0)
            Q_EMIT avatarUploadFailed(roomId);
    }
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
    m_opTimeout.stop();
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
    m_opTimeout.stop();
    m_pendingOp = 0;
    Q_EMIT busyChanged();
    Q_EMIT inviteBatchCompleted(okCount, failCount);
}

void ConversationController::onRoomEditFinished(quint64 opId,
                                                const QString &roomId,
                                                const QString &field,
                                                bool ok,
                                                const QString &category)
{
    Q_UNUSED(field);
    Q_UNUSED(category);
    if (m_avatarOp == 0 || opId != m_avatarOp)
        return; // not our avatar upload (e.g. a RoomInfoController edit)
    m_avatarOp = 0;
    if (!ok)
        Q_EMIT avatarUploadFailed(roomId);
}

void ConversationController::beginWaitForRoom(const QString &roomId)
{
    m_waitingForRoom = true;
    m_awaitedRoomId = roomId;
    m_roomWaitTimeout.start();
    Q_EMIT busyChanged();
    // create_room may already have inserted it locally, so check now too.
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
    const bool isSpace = m_pendingIsSpace;
    m_waitingForRoom = false;
    m_awaitedRoomId.clear();
    m_pendingIsSpace = false;
    m_roomWaitTimeout.stop();
    Q_EMIT busyChanged();
    if (isSpace)
        Q_EMIT spaceReady(roomId);
    else
        Q_EMIT conversationReady(roomId);
}

void ConversationController::onLoggedOut()
{
    // Unlike reset(), sign-out abandons the pending op: its session is gone
    // and the next account must start clean.
    m_pendingOp = 0;
    m_pendingDmUser.clear();
    clearAbandonedDm();
    m_opTimeout.stop();
    reset();
}

void ConversationController::setError(const QString &message)
{
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}
