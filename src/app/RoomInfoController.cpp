#include "app/RoomInfoController.h"

#include "matrix/MatrixClient.h"

#include <QUrl>

RoomInfoController::RoomInfoController(QObject *parent)
    : QObject(parent)
{
}

void RoomInfoController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomMembersReceived,
                this, &RoomInfoController::onRoomMembersReceived);
        connect(m_client, &MatrixClient::roomEditFinished,
                this, &RoomInfoController::onRoomEditFinished);
        connect(m_client, &MatrixClient::roomLeaveFinished,
                this, &RoomInfoController::onRoomLeaveFinished);
        connect(m_client, &MatrixClient::moderationFinished,
                this, &RoomInfoController::onModerationFinished);
        connect(m_client, &MatrixClient::membersChanged,
                this, &RoomInfoController::onMembersChanged);
        connect(m_client, &MatrixClient::loggedOut,
                this, &RoomInfoController::onLoggedOut);
    }
}

bool RoomInfoController::supported() const
{
    return m_client && m_client->supportsRoomManagement();
}

void RoomInfoController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // Room switch invalidates every in-flight operation for the old room.
    m_membersOp = 0;
    m_editOp = 0;
    m_leaveOp = 0;
    m_moderationOp = 0;
    m_editError.clear();
    m_leaveError.clear();
    clearSnapshot();
    Q_EMIT roomIdChanged();
    Q_EMIT editStateChanged();
    Q_EMIT leaveStateChanged();
    Q_EMIT moderationStateChanged();
    if (!m_roomId.isEmpty())
        refreshMembers();
}

void RoomInfoController::clearSnapshot()
{
    m_members.clear();
    m_joinedCount = 0;
    m_invitedCount = 0;
    m_truncated = false;
    m_canInvite = false;
    m_canEditName = false;
    m_canEditTopic = false;
    m_canEditAvatar = false;
    m_canKick = false;
    m_canBan = false;
    m_canUnban = false;
    m_ownPowerLevel = 0;
    Q_EMIT membersChanged();
}

void RoomInfoController::refreshMembers()
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    const quint64 opId = m_client->requestRoomMembers(m_roomId);
    if (opId != 0) {
        m_membersOp = opId;
        Q_EMIT membersChanged();
    }
}

void RoomInfoController::onRoomMembersReceived(quint64 opId,
                                               const QString &roomId,
                                               const QVariantMap &snapshot)
{
    if (opId != m_membersOp || roomId != m_roomId)
        return; // stale snapshot (old room / old request)
    m_membersOp = 0;
    if (!snapshot.value(QStringLiteral("ok")).toBool()) {
        Q_EMIT membersChanged();
        return;
    }
    m_members = snapshot.value(QStringLiteral("members")).toList();
    m_joinedCount = snapshot.value(QStringLiteral("joinedCount")).toInt();
    m_invitedCount = snapshot.value(QStringLiteral("invitedCount")).toInt();
    m_truncated = snapshot.value(QStringLiteral("truncated")).toBool();
    m_canInvite = snapshot.value(QStringLiteral("canInvite")).toBool();
    m_canEditName = snapshot.value(QStringLiteral("canEditName")).toBool();
    m_canEditTopic = snapshot.value(QStringLiteral("canEditTopic")).toBool();
    m_canEditAvatar = snapshot.value(QStringLiteral("canEditAvatar")).toBool();
    m_canKick = snapshot.value(QStringLiteral("canKick")).toBool();
    m_canBan = snapshot.value(QStringLiteral("canBan")).toBool();
    m_canUnban = snapshot.value(QStringLiteral("canUnban")).toBool();
    m_ownPowerLevel =
        snapshot.value(QStringLiteral("ownPowerLevel")).toLongLong();
    Q_EMIT membersChanged();
}

void RoomInfoController::onMembersChanged(const QString &roomId)
{
    // Authoritative sync updated membership for the open room (e.g. an
    // invite landed) — refresh the snapshot unless one is already pending.
    if (roomId == m_roomId && m_membersOp == 0 && !m_roomId.isEmpty())
        refreshMembers();
}

void RoomInfoController::setRoomName(const QString &name)
{
    if (!m_client || m_roomId.isEmpty() || editPending() || !m_canEditName)
        return;
    m_editError.clear();
    m_editOp = m_client->setRoomName(m_roomId, name);
    Q_EMIT editStateChanged();
}

void RoomInfoController::setRoomTopic(const QString &topic)
{
    if (!m_client || m_roomId.isEmpty() || editPending() || !m_canEditTopic)
        return;
    m_editError.clear();
    m_editOp = m_client->setRoomTopic(m_roomId, topic);
    Q_EMIT editStateChanged();
}

void RoomInfoController::setRoomAvatar(const QUrl &fileUrl)
{
    if (!m_client || m_roomId.isEmpty() || editPending() || !m_canEditAvatar)
        return;
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                               : fileUrl.toString();
    if (path.isEmpty())
        return;
    m_editError.clear();
    m_editOp = m_client->setRoomAvatar(m_roomId, path);
    Q_EMIT editStateChanged();
}

void RoomInfoController::removeRoomAvatar()
{
    if (!m_client || m_roomId.isEmpty() || editPending() || !m_canEditAvatar)
        return;
    m_editError.clear();
    m_editOp = m_client->removeRoomAvatar(m_roomId);
    Q_EMIT editStateChanged();
}

void RoomInfoController::onRoomEditFinished(quint64 opId, const QString &roomId,
                                            const QString &field, bool ok,
                                            const QString &category)
{
    Q_UNUSED(field);
    Q_UNUSED(category);
    if (opId != m_editOp || roomId != m_roomId)
        return;
    m_editOp = 0;
    if (!ok) {
        m_editError = category == QLatin1String("forbidden")
            ? tr("You do not have permission to change that.")
            : tr("The change could not be saved.");
    }
    Q_EMIT editStateChanged();
    // The authoritative value arrives via sync; no optimistic local write.
}

void RoomInfoController::leaveRoom()
{
    if (!m_client || m_roomId.isEmpty() || leavePending())
        return;
    m_leaveError.clear();
    m_leaveOp = m_client->leaveRoom(m_roomId);
    Q_EMIT leaveStateChanged();
}

void RoomInfoController::leaveRoom(const QString &roomId)
{
    if (!m_client || roomId.isEmpty())
        return;
    const quint64 opId = m_client->leaveRoom(roomId);
    if (opId != 0)
        m_adhocLeaveOps.insert(opId);
}

void RoomInfoController::onRoomLeaveFinished(quint64 opId, const QString &roomId,
                                             bool ok, const QString &category)
{
    if (m_adhocLeaveOps.remove(opId)) {
        // Room-list adapter path: independent of the panel's own
        // leavePending/leaveError state. Reuses the exact same sanitized
        // category-to-text mapping the panel's own leave path uses, so the
        // two surfaces stay honest and consistent — no new sanitization
        // rules.
        if (ok) {
            Q_EMIT roomLeft(roomId);
        } else {
            const QString message = category == QLatin1String("forbidden")
                ? tr("The server refused to leave this room.")
                : tr("Leaving the room failed. Check your connection and retry.");
            Q_EMIT roomLeaveFailed(roomId, message);
        }
        return;
    }
    if (opId != m_leaveOp)
        return;
    m_leaveOp = 0;
    if (!ok) {
        m_leaveError = category == QLatin1String("forbidden")
            ? tr("The server refused to leave this room.")
            : tr("Leaving the room failed. Check your connection and retry.");
        Q_EMIT leaveStateChanged();
        return;
    }
    Q_EMIT leaveStateChanged();
    Q_EMIT roomLeft(roomId);
}

bool RoomInfoController::canModerate(const QString &userId,
                                     const QString &op) const
{
    const bool isKick = op == QLatin1String("kick");
    const bool isBan = op == QLatin1String("ban");
    const bool isUnban = op == QLatin1String("unban");
    if (!isKick && !isBan && !isUnban)
        return false;
    if (!m_client || m_roomId.isEmpty() || userId.isEmpty() || !supported())
        return false;
    // SDK-derived permission flag — never a role label, never derived
    // from another flag: unban's required level is max(ban, kick)
    // (ruma PowerLevelAction::Unban), so it has its own snapshot flag
    // (review MU1 — gating unban on the ban power alone over-offered in
    // rooms where kick > ban).
    const bool allowed = isKick ? m_canKick : isBan ? m_canBan : m_canUnban;
    if (!allowed)
        return false;
    // The target must be a loaded snapshot row: absence of the row is the
    // "unknown" state and fails closed. The power level itself may be any
    // integer, including negative (Element's "Restricted" is -1), so no
    // sentinel value can stand in for "unknown".
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId)
            continue;
        if (row.value(QStringLiteral("isOwn")).toBool())
            return false;
        if (!row.contains(QStringLiteral("powerLevel")))
            return false;
        // Membership must match the action: unban applies ONLY to banned
        // members, kick/ban never to them (a banned member is already
        // out; the server would reject a second ban as a no-op state
        // change and a kick outright).
        const bool banned = row.value(QStringLiteral("membership"))
                                .toString() == QLatin1String("banned");
        if (isUnban != banned)
            return false;
        // Strictly below the viewer's own level (Element semantics; the
        // server enforces regardless).
        return row.value(QStringLiteral("powerLevel")).toLongLong()
               < m_ownPowerLevel;
    }
    return false;
}

void RoomInfoController::kickMember(const QString &userId,
                                    const QString &reason)
{
    moderate(userId, reason, QStringLiteral("kick"));
}

void RoomInfoController::banMember(const QString &userId,
                                   const QString &reason)
{
    moderate(userId, reason, QStringLiteral("ban"));
}

void RoomInfoController::unbanMember(const QString &userId,
                                     const QString &reason)
{
    moderate(userId, reason, QStringLiteral("unban"));
}

void RoomInfoController::moderate(const QString &userId,
                                  const QString &reason, const QString &op)
{
    if (m_moderationOp != 0)
        return;
    // Re-check the full offer policy at dispatch time — the QML surface
    // binds to canModerate() but must never be the enforcement point.
    if (!canModerate(userId, op))
        return;
    quint64 opId = 0;
    if (op == QLatin1String("kick"))
        opId = m_client->kickUser(m_roomId, userId, reason);
    else if (op == QLatin1String("ban"))
        opId = m_client->banUser(m_roomId, userId, reason);
    else if (op == QLatin1String("unban"))
        opId = m_client->unbanUser(m_roomId, userId, reason);
    else
        return; // canModerate() already refused unknown ops — never
                // default a destructive dispatcher to kick (review LU4).
    if (opId == 0) {
        // Synchronous rejection (backend unsupported, room not joined,
        // invalid user id): report honestly instead of leaving the
        // confirm surface armed forever (review L2).
        Q_EMIT moderationActionFinished(
            m_roomId, userId, op, false,
            tr("The action could not be sent."));
        return;
    }
    m_moderationOp = opId;
    Q_EMIT moderationStateChanged();
}

void RoomInfoController::onModerationFinished(quint64 opId,
                                              const QString &roomId,
                                              const QString &userId,
                                              const QString &op, bool ok,
                                              const QString &category)
{
    if (opId != m_moderationOp)
        return;
    m_moderationOp = 0;
    Q_EMIT moderationStateChanged();
    const QString message = ok
        ? QString()
        : (category == QLatin1String("forbidden")
               ? tr("You do not have permission to do that.")
               : tr("The action failed. Check your connection and retry."));
    Q_EMIT moderationActionFinished(roomId, userId, op, ok, message);
    // The roster refresh is CLIENT-initiated: the Rust backend only emits
    // membersChanged in response to an explicit member fetch, never from
    // sync, so without this the kicked user would stay in the People list
    // until the next room switch (review M2).
    if (ok && roomId == m_roomId)
        refreshMembers();
}

QVariantList RoomInfoController::filterMembers(const QString &needle) const
{
    if (needle.trimmed().isEmpty())
        return m_members;
    const QString lc = needle.trimmed().toLower();
    QVariantList out;
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("displayName")).toString().toLower().contains(lc)
            || row.value(QStringLiteral("userId")).toString().toLower().contains(lc))
            out.append(row);
    }
    return out;
}

void RoomInfoController::onLoggedOut()
{
    m_membersOp = 0;
    m_editOp = 0;
    m_leaveOp = 0;
    m_moderationOp = 0;
    m_adhocLeaveOps.clear();
    m_roomId.clear();
    m_editError.clear();
    m_leaveError.clear();
    clearSnapshot();
    Q_EMIT roomIdChanged();
    Q_EMIT editStateChanged();
    Q_EMIT leaveStateChanged();
    Q_EMIT moderationStateChanged();
}
