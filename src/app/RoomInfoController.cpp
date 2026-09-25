#include "app/RoomInfoController.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcRoomInfo, "lightning.roominfo")

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

// The level rooms::CREATOR_POWER_LEVEL reports for a room creator (MSC4289,
// room version 12): 2^53, above every finite level (at most 2^53 - 1). A
// creator's power is not in m.room.power_levels, so no power-level event can
// lower it.
constexpr qlonglong kCreatorPowerLevel = qlonglong(1) << 53;

} // namespace

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
        connect(m_client, &MatrixClient::roomProfileResult, this,
                [this](quint64, const QString &roomId, const QString &,
                       bool ok, const QString &error) {
                    // Another room's answer must not clear this panel's
                    // pending state; the user may have moved on.
                    if (roomId != m_roomId)
                        return;
                    if (m_profileOps > 0)
                        --m_profileOps;
                    if (!ok) {
                        m_profileError = error.isEmpty()
                            ? tr("The server did not accept the change.")
                            : error;
                    }
                    Q_EMIT roomProfileChanged();
                });
        connect(m_client, &MatrixClient::roomMembersReceived,
                this, &RoomInfoController::onRoomMembersReceived);
        connect(m_client, &MatrixClient::roomEditFinished,
                this, &RoomInfoController::onRoomEditFinished);
        // Directory visibility answer. A failed read leaves the tri-state
        // unknown, rendered disabled rather than guessed.
        connect(m_client, &MatrixClient::roomDirectoryVisibilityReceived, this,
                [this](const QString &roomId, bool ok, bool published) {
                    if (roomId != m_roomId)
                        return;
                    const int next = ok ? (published ? 1 : 0) : -1;
                    if (next == m_directoryPublished)
                        return;
                    m_directoryPublished = next;
                    Q_EMIT directoryVisibilityChanged();
                });
        connect(m_client, &MatrixClient::roomLeaveFinished,
                this, &RoomInfoController::onRoomLeaveFinished);
        connect(m_client, &MatrixClient::moderationFinished,
                this, &RoomInfoController::onModerationFinished);
        connect(m_client, &MatrixClient::powerLevelChangeFinished,
                this, &RoomInfoController::onPowerLevelChangeFinished);
        connect(m_client, &MatrixClient::roomPowerMatrixFinished,
                this, &RoomInfoController::onPowerMatrixFinished);
        connect(m_client, &MatrixClient::inviteUserFinished,
                this, &RoomInfoController::onInviteUserFinished);
        connect(m_client, &MatrixClient::mutualRoomsReceived, this,
                [this](quint64 opId, const QString &userId,
                       const QVariantList &rooms) {
            // Match op and user, so a late answer for a closed card never
            // populates the next one.
            if (opId == 0 || opId != m_mutualRoomsOp
                || userId != m_mutualRoomsUser)
                return;
            m_mutualRoomsOp = 0;
            m_mutualRooms = rooms;
            Q_EMIT mutualRoomsChanged();
        });
        // The sync poke, not membersChanged: this controller refetches on the
        // signal, and membersChanged also fires for its own fetches.
        connect(m_client, &MatrixClient::roomMemberEventSeen,
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
    if (m_roomId == roomId) {
        // Refetch on reopen so the panel never shows a stale snapshot with
        // loading == false. After the first sync this is a store read.
        if (!m_roomId.isEmpty() && m_membersOp == 0)
            refreshMembers();
        return;
    }
    m_roomId = roomId;
    // Directory visibility belongs to the room it was read for.
    if (m_directoryPublished != -1) {
        m_directoryPublished = -1;
        Q_EMIT directoryVisibilityChanged();
    }
    // A room switch invalidates every in-flight operation for the old room.
    m_membersOp = 0;
    m_editOp = 0;
    m_leaveOp = 0;
    m_moderationOp = 0;
    m_powerLevelOp = 0;
    m_powerLevelUserId.clear();
    m_powerMatrixOp = 0;
    m_powerMatrixError.clear();
    m_inviteBackUserId.clear();
    m_inviteBackOp = 0;
    m_editError.clear();
    m_leaveError.clear();
    clearSnapshot();
    Q_EMIT roomIdChanged();
    Q_EMIT editStateChanged();
    Q_EMIT leaveStateChanged();
    Q_EMIT moderationStateChanged();
    Q_EMIT powerLevelStateChanged();
    Q_EMIT powerMatrixStateChanged();
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
    m_canNotifyRoom = false;
    m_canUnban = false;
    m_ownPowerLevel = 0;
    m_canChangePowerLevels = false;
    m_canPinMessages = false;
    m_canChangeJoinRule = false;
    m_canChangeAlias = false;
    m_canManageSpaceChildren = false;
    m_usersDefaultPowerLevel = 0;
    m_joinRule.clear();
    m_canonicalAlias.clear();
    // Empty is the unknown state; zeros would claim a real, very permissive
    // configuration.
    m_powerLevels.clear();
    m_roomVersion.clear();
    m_canUpgradeRoom = false;
    m_canPublishCallMembership = true;
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
    // A partial (cache-only) snapshot renders immediately but keeps the op
    // pending until the synced roster lands under the same op.
    const bool partial = snapshot.value(QStringLiteral("partial")).toBool();
    if (!partial)
        m_membersOp = 0;
    if (!snapshot.value(QStringLiteral("ok")).toBool()) {
        Q_EMIT membersChanged();
        return;
    }
    m_members = snapshot.value(QStringLiteral("members")).toList();
    // Counts and the truncation flag are whole-roster facts a cache-only
    // snapshot cannot know, so keep the previous values while loading. Rows
    // and permissions still render immediately.
    if (!partial) {
        m_joinedCount = snapshot.value(QStringLiteral("joinedCount")).toInt();
        m_invitedCount =
            snapshot.value(QStringLiteral("invitedCount")).toInt();
        m_truncated = snapshot.value(QStringLiteral("truncated")).toBool();
    }
    m_canInvite = snapshot.value(QStringLiteral("canInvite")).toBool();
    m_canEditName = snapshot.value(QStringLiteral("canEditName")).toBool();
    m_canEditTopic = snapshot.value(QStringLiteral("canEditTopic")).toBool();
    m_canEditAvatar = snapshot.value(QStringLiteral("canEditAvatar")).toBool();
    m_canKick = snapshot.value(QStringLiteral("canKick")).toBool();
    m_canBan = snapshot.value(QStringLiteral("canBan")).toBool();
    m_canNotifyRoom =
        snapshot.value(QStringLiteral("canNotifyRoom")).toBool();
    m_canUnban = snapshot.value(QStringLiteral("canUnban")).toBool();
    m_ownPowerLevel =
        snapshot.value(QStringLiteral("ownPowerLevel")).toLongLong();
    m_canChangePowerLevels =
        snapshot.value(QStringLiteral("canChangePowerLevels")).toBool();
    m_canPinMessages =
        snapshot.value(QStringLiteral("canPinMessages")).toBool();
    m_canChangeJoinRule =
        snapshot.value(QStringLiteral("canChangeJoinRule")).toBool();
    m_canChangeAlias =
        snapshot.value(QStringLiteral("canChangeAlias")).toBool();
    m_canManageSpaceChildren =
        snapshot.value(QStringLiteral("canManageSpaceChildren")).toBool();
    m_usersDefaultPowerLevel =
        snapshot.value(QStringLiteral("usersDefaultPowerLevel")).toLongLong();
    // Logged because a false gate hides Space Home's Remove / Suggest /
    // Invite controls with no visible trace. No room id here; the shared
    // logging helper covers it.
    qCDebug(lcRoomInfo)
        << "member snapshot power gates ownLevel=" << m_ownPowerLevel
        << "canInvite=" << m_canInvite
        << "canManageSpaceChildren=" << m_canManageSpaceChildren
        << "canChangePowerLevels=" << m_canChangePowerLevels
        << "joined=" << m_joinedCount;
    m_joinRule = snapshot.value(QStringLiteral("joinRule")).toString();
    m_canonicalAlias =
        snapshot.value(QStringLiteral("canonicalAlias")).toString();
    // Absent keys read as "" / empty / false, so a backend that omits them
    // offers nothing rather than claiming a configuration.
    m_historyVisibility =
        snapshot.value(QStringLiteral("historyVisibility")).toString();
    m_guestAccess = snapshot.value(QStringLiteral("guestAccess")).toString();
    m_altAliases = snapshot.value(QStringLiteral("altAliases")).toStringList();
    m_restrictedAllowedRooms =
        snapshot.value(QStringLiteral("restrictedAllowedRooms")).toStringList();
    m_restrictedHasUnknownRules =
        snapshot.value(QStringLiteral("restrictedHasUnknownRules")).toBool();
    m_canChangeHistoryVisibility =
        snapshot.value(QStringLiteral("canChangeHistoryVisibility")).toBool();
    m_canChangeGuestAccess =
        snapshot.value(QStringLiteral("canChangeGuestAccess")).toBool();
    // A backend that omits the thresholds leaves the map empty, so the
    // Permissions matrix renders nothing rather than inventing a model.
    m_powerLevels = snapshot.value(QStringLiteral("powerLevels")).toMap();
    m_roomVersion = snapshot.value(QStringLiteral("roomVersion")).toString();
    m_canUpgradeRoom =
        snapshot.value(QStringLiteral("canUpgradeRoom")).toBool();
    // Defaults to true when unreported: the Join button is not disabled on a
    // guess; the server decides.
    m_canPublishCallMembership =
        !snapshot.contains(QStringLiteral("canPublishCallMembership"))
        || snapshot.value(QStringLiteral("canPublishCallMembership")).toBool();
    Q_EMIT membersChanged();
}

void RoomInfoController::onMembersChanged(const QString &roomId)
{
    // Sync updated membership for the open room (e.g. an invite landed);
    // refresh unless a fetch is already pending.
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

bool RoomInfoController::roomProfilesSupported() const
{
    return m_client && m_client->supportsRoomProfiles();
}

void RoomInfoController::setMyRoomDisplayName(const QString &name)
{
    if (!roomProfilesSupported() || m_roomId.isEmpty())
        return;
    m_profileError.clear();
    if (m_client->setRoomDisplayName(m_roomId, name) != 0)
        ++m_profileOps;
    Q_EMIT roomProfileChanged();
}

void RoomInfoController::setMyRoomAvatar(const QUrl &fileUrl)
{
    if (!roomProfilesSupported() || m_roomId.isEmpty())
        return;
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                               : fileUrl.toString();
    if (path.isEmpty())
        return;
    m_profileError.clear();
    // Rust uploads the file and writes only this room's membership, not the
    // global avatar.
    if (m_client->setRoomMemberAvatar(m_roomId, path) != 0)
        ++m_profileOps;
    Q_EMIT roomProfileChanged();
}

void RoomInfoController::clearMyRoomAvatar()
{
    if (!roomProfilesSupported() || m_roomId.isEmpty())
        return;
    m_profileError.clear();
    if (m_client->setRoomMemberAvatar(m_roomId, QString()) != 0)
        ++m_profileOps;
    Q_EMIT roomProfileChanged();
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
    if (opId != m_editOp || roomId != m_roomId)
        return;
    m_editOp = 0;
    if (!ok) {
        m_editError = category == QLatin1String("forbidden")
            ? tr("You do not have permission to change that.")
            : tr("The change could not be saved.");
    }
    Q_EMIT editStateChanged();
    // The value arrives via sync; no optimistic local write. Name, topic and
    // avatar reach the UI through the room-list model, but join rule and
    // canonical alias ride the member snapshot, which nothing refetches for
    // non-membership state. So those refetch the roster on success.
    if (ok
        && (field == QLatin1String("join_rule")
            || field == QLatin1String("canonical_alias")
            || field == QLatin1String("history_visibility")
            || field == QLatin1String("guest_access")
            || field == QLatin1String("alt_aliases"))) {
        refreshMembers();
    }
    // Directory visibility is not room state and is not re-synced, so a
    // successful write is re-read from the server.
    if (ok && field == QLatin1String("directory_visibility"))
        requestDirectoryVisibility();
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
        // Room-list adapter path, independent of the panel's
        // leavePending/leaveError, using the same sanitized messages.
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
    // SDK-derived flag, never a role label. Unban has its own flag because
    // its required level is max(ban, kick) (ruma PowerLevelAction::Unban).
    const bool allowed = isKick ? m_canKick : isBan ? m_canBan : m_canUnban;
    if (!allowed)
        return false;
    // The target must be a loaded snapshot row; absence is "unknown" and
    // fails closed. Levels may be negative (Element's "Restricted" is -1), so
    // no sentinel can mean unknown.
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId)
            continue;
        if (row.value(QStringLiteral("isOwn")).toBool())
            return false;
        if (!row.contains(QStringLiteral("powerLevel")))
            return false;
        // Unban applies only to banned members, kick/ban never to them (the
        // server would reject both).
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
                                     const QString &reason, bool inviteBack)
{
    // Refuse while busy before arming, or this would overwrite a live
    // unban's pending invite. moderate() re-refuses regardless.
    if (moderationPending())
        return;
    m_inviteBackUserId = inviteBack ? userId : QString();
    moderate(userId, reason, QStringLiteral("unban"));
    // moderate() can refuse synchronously; never leave the invite armed for
    // an unban that was not dispatched.
    if (m_moderationOp == 0)
        m_inviteBackUserId.clear();
}

void RoomInfoController::moderate(const QString &userId,
                                  const QString &reason, const QString &op)
{
    if (m_moderationOp != 0)
        return;
    // Re-check the full policy at dispatch; QML binds canModerate() but is
    // not the enforcement point.
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
        return; // canModerate() already refused unknown ops; never
                // default a destructive dispatcher to kick.
    if (opId == 0) {
        // Synchronous rejection (backend unsupported, room not joined,
        // invalid user id): report it so the confirm surface is not left
        // armed.
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
    // Invite back only after a successful unban, via the normal invite path;
    // its result arrives via onInviteUserFinished.
    if (ok && op == QLatin1String("unban") && roomId == m_roomId
        && !m_inviteBackUserId.isEmpty() && userId == m_inviteBackUserId
        && m_client) {
        m_inviteBackOp = m_client->inviteUsers(m_roomId, { userId });
        if (m_inviteBackOp == 0) {
            Q_EMIT moderationActionFinished(
                roomId, userId, QStringLiteral("invite_back"), false,
                tr("The invite could not be sent."));
        }
    }
    m_inviteBackUserId.clear();
    // Refresh the roster explicitly: the Rust backend emits membersChanged
    // only for explicit member fetches, never from sync.
    if (ok && roomId == m_roomId)
        refreshMembers();
}

// ---------------------------------------------------------------------------
// Room administration
// ---------------------------------------------------------------------------

qlonglong RoomInfoController::powerLevelFor(const QString &userId) const
{
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId)
            continue;
        if (!row.contains(QStringLiteral("powerLevel")))
            break;
        return row.value(QStringLiteral("powerLevel")).toLongLong();
    }
    return m_usersDefaultPowerLevel;
}

QString RoomInfoController::roleLabelForLevel(qlonglong level) const
{
    // Only the conventional Matrix presets get names; any other level renders
    // as its number, and nothing is rounded on save. The room's own
    // users_default is checked first, so a room whose default is 50 does not
    // call its ordinary members "Moderator".
    if (level >= kCreatorPowerLevel)
        return tr("Creator");
    if (level == m_usersDefaultPowerLevel)
        return tr("Member");
    if (level == 100)
        return tr("Administrator");
    if (level == 50)
        return tr("Moderator");
    if (level == 0)
        return tr("Member");
    return tr("Custom (%1)").arg(level);
}

bool RoomInfoController::canSetPowerLevel(const QString &userId,
                                          qlonglong level) const
{
    if (!m_client || m_roomId.isEmpty() || userId.isEmpty() || !supported())
        return false;
    if (!m_canChangePowerLevels || m_powerLevelOp != 0)
        return false;
    // You cannot grant authority above your own.
    if (level > m_ownPowerLevel)
        return false;
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("userId")).toString() != userId)
            continue;
        // Unknown level for a known row still fails closed: negative levels
        // are legal, so no sentinel can mean "unknown".
        if (!row.contains(QStringLiteral("powerLevel")))
            return false;
        const qlonglong current =
            row.value(QStringLiteral("powerLevel")).toLongLong();
        if (current == level)
            return false; // a no-op is not an action worth offering
        if (row.value(QStringLiteral("isOwn")).toBool()) {
            // Self: demotion only, and never for a creator, whose level the
            // v12 auth rules do not let any power-level event change.
            return level < current && current < kCreatorPowerLevel;
        }
        // A peer at or above your own level is not yours to change.
        return current < m_ownPowerLevel;
    }
    return false;
}

void RoomInfoController::setMemberPowerLevel(const QString &userId,
                                             qlonglong level)
{
    // Re-check the gate: room state can change between menu and click, and
    // a known-unauthorized state event must not be sent.
    if (!canSetPowerLevel(userId, level))
        return;
    const quint64 opId = m_client->setMemberPowerLevel(m_roomId, userId, level);
    if (opId == 0) {
        Q_EMIT powerLevelActionFinished(
            m_roomId, userId, level, false,
            tr("The change could not be sent. Check your connection and "
               "retry."));
        return;
    }
    m_powerLevelOp = opId;
    m_powerLevelUserId = userId;
    Q_EMIT powerLevelStateChanged();
}

void RoomInfoController::onPowerLevelChangeFinished(quint64 opId,
                                                    const QString &roomId,
                                                    const QString &userId,
                                                    qlonglong level, bool ok,
                                                    const QString &category)
{
    if (opId == 0 || opId != m_powerLevelOp)
        return;
    m_powerLevelOp = 0;
    m_powerLevelUserId.clear();
    Q_EMIT powerLevelStateChanged();
    const QString message = ok
        ? QString()
        : (category == QLatin1String("forbidden")
               ? tr("The server refused that change. Your permissions may "
                    "have changed.")
               : category == QLatin1String("rate_limited")
                     ? tr("The server is rate limiting this action. Try "
                          "again shortly.")
                     : tr("The change failed. Check your connection and "
                          "retry."));
    Q_EMIT powerLevelActionFinished(roomId, userId, level, ok, message);
    // Re-read the roster either way: it carries the level the room now holds,
    // and nothing writes the new level into the snapshot by hand.
    if (roomId == m_roomId)
        refreshMembers();
}

void RoomInfoController::dispatchEdit(quint64 opId)
{
    if (opId == 0) {
        m_editError = tr("The change could not be sent.");
        Q_EMIT editStateChanged();
        return;
    }
    m_editOp = opId;
    m_editError.clear();
    Q_EMIT editStateChanged();
}

void RoomInfoController::setJoinRule(const QString &rule)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeJoinRule || m_editOp != 0)
        return;
    if (rule != QLatin1String("invite") && rule != QLatin1String("public")
        && rule != QLatin1String("knock")) {
        return;
    }
    if (rule == m_joinRule)
        return;
    dispatchEdit(m_client->setRoomJoinRule(m_roomId, rule));
}

void RoomInfoController::setRestrictedJoinRule(const QString &rule,
                                               const QStringList &allowedRoomIds)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeJoinRule || m_editOp != 0)
        return;
    if (rule != QLatin1String("restricted")
        && rule != QLatin1String("knock_restricted"))
        return;
    QStringList ids;
    for (const QString &id : allowedRoomIds) {
        const QString trimmed = id.trimmed();
        if (trimmed.startsWith(QLatin1Char('!')) && !ids.contains(trimmed))
            ids.append(trimmed);
    }
    if (ids.isEmpty() && !m_restrictedHasUnknownRules) {
        // Same refusal as the Rust edge: an empty allow list is invite-only
        // under a restricted label.
        m_editError = tr("Choose at least one space whose members may join.");
        Q_EMIT editStateChanged();
        return;
    }
    if (rule == m_joinRule && ids == m_restrictedAllowedRooms)
        return;
    dispatchEdit(m_client->setRoomJoinRule(m_roomId, rule, ids));
}

void RoomInfoController::setHistoryVisibility(const QString &visibility)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeHistoryVisibility || m_editOp != 0)
        return;
    if (visibility != QLatin1String("invited")
        && visibility != QLatin1String("joined")
        && visibility != QLatin1String("shared")
        && visibility != QLatin1String("world_readable"))
        return;
    if (visibility == m_historyVisibility)
        return;
    dispatchEdit(m_client->setRoomHistoryVisibility(m_roomId, visibility));
}

void RoomInfoController::setGuestAccess(const QString &access)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeGuestAccess || m_editOp != 0)
        return;
    if (access != QLatin1String("can_join") && access != QLatin1String("forbidden"))
        return;
    if (access == m_guestAccess)
        return;
    dispatchEdit(m_client->setRoomGuestAccess(m_roomId, access));
}

void RoomInfoController::setDirectoryPublished(bool published)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    // Gated like the alias controls; the server enforces its own policy and
    // a refusal surfaces through the ordinary edit error.
    if (!m_canChangeAlias || m_editOp != 0)
        return;
    if (m_directoryPublished == (published ? 1 : 0))
        return;
    dispatchEdit(m_client->setRoomDirectoryVisibility(m_roomId, published));
}

void RoomInfoController::requestDirectoryVisibility()
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    m_client->requestRoomDirectoryVisibility(m_roomId);
}

QString RoomInfoController::completeAlias(const QString &alias,
                                          QString *error) const
{
    QString value = alias.trimmed();
    if (value.isEmpty())
        return value;
    // Accept a bare localpart and complete it with the account's own server.
    if (!value.startsWith(QLatin1Char('#')))
        value.prepend(QLatin1Char('#'));
    if (!value.contains(QLatin1Char(':'))) {
        const QString own = m_client ? m_client->currentUserId() : QString();
        const int colon = own.indexOf(QLatin1Char(':'));
        if (colon < 0 || colon + 1 >= own.size()) {
            if (error)
                *error = tr("The alias needs a server name.");
            return QString();
        }
        value += own.mid(colon);
    }
    return value;
}

void RoomInfoController::setAltAliases(const QStringList &aliases)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeAlias || m_editOp != 0)
        return;
    QStringList completed;
    for (const QString &alias : aliases) {
        QString error;
        const QString value = completeAlias(alias, &error);
        if (!error.isEmpty()) {
            m_editError = error;
            Q_EMIT editStateChanged();
            return;
        }
        if (!value.isEmpty() && !completed.contains(value)
            && value != m_canonicalAlias)
            completed.append(value);
    }
    if (completed == m_altAliases)
        return;
    dispatchEdit(m_client->setRoomAltAliases(m_roomId, completed));
}

QVariantList RoomInfoController::joinedSpaces() const
{
    QVariantList out;
    if (!m_client)
        return out;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &room : rooms) {
        if (!room.isSpace || room.membership != RoomInfo::Joined)
            continue;
        out.append(QVariantMap{
            { QStringLiteral("roomId"), room.id },
            { QStringLiteral("name"),
              room.name.isEmpty() ? room.id : room.name },
        });
    }
    return out;
}

void RoomInfoController::setCanonicalAlias(const QString &alias)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeAlias || m_editOp != 0)
        return;
    QString error;
    const QString value = completeAlias(alias, &error);
    if (!error.isEmpty()) {
        m_editError = error;
        Q_EMIT editStateChanged();
        return;
    }
    if (value == m_canonicalAlias)
        return;
    dispatchEdit(m_client->setRoomCanonicalAlias(m_roomId, value));
}

// ---------------------------------------------------------------------------
// The m.room.power_levels matrix (Space settings)
// ---------------------------------------------------------------------------

QStringList RoomInfoController::powerLevelKeys()
{
    // Must match the allowlist in rooms::set_room_power_level_key and the
    // keys the Rust member snapshot emits; anything else is refused at the
    // Rust edge. Built once, since powerLevelKnown() runs per row.
    static const QStringList keys{
        QStringLiteral("users_default"),
        QStringLiteral("events_default"),
        QStringLiteral("state_default"),
        QStringLiteral("invite"),
        QStringLiteral("kick"),
        QStringLiteral("ban"),
        QStringLiteral("redact"),
        QStringLiteral("m.space.child"),
        QStringLiteral("m.room.name"),
        QStringLiteral("m.room.avatar"),
        QStringLiteral("m.room.topic"),
        QStringLiteral("m.room.join_rules"),
        QStringLiteral("m.room.canonical_alias"),
        QStringLiteral("m.room.power_levels"),
        QStringLiteral("m.room.tombstone"),
    };
    return keys;
}

bool RoomInfoController::powerLevelKnown(const QString &key) const
{
    return powerLevelKeys().contains(key) && m_powerLevels.contains(key);
}

qlonglong RoomInfoController::powerLevelForKey(const QString &key) const
{
    // -1 is a sentinel only because callers ask powerLevelKnown() first; it
    // is also a legal Matrix level.
    if (!powerLevelKnown(key))
        return -1;
    return m_powerLevels.value(key).toLongLong();
}

bool RoomInfoController::canSetPowerLevelKey(const QString &key,
                                             qlonglong level) const
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return false;
    if (!m_canChangePowerLevels || m_powerMatrixOp != 0)
        return false;
    if (!powerLevelKnown(key))
        return false; // an unknown threshold fails closed
    if (level < kMinSettableLevel || level > kMaxSettableLevel)
        return false;
    // Raising m.room.power_levels above your own level cannot be undone.
    // This also stops users_default being raised above the person setting it.
    if (level > m_ownPowerLevel)
        return false;
    return m_powerLevels.value(key).toLongLong() != level;
}

void RoomInfoController::setPowerLevelKey(const QString &key, qlonglong level)
{
    // Re-check the gate: room state can change between render and click, and
    // a known-unauthorized state event must not be sent.
    if (!canSetPowerLevelKey(key, level))
        return;
    const quint64 opId = m_client->setRoomPowerLevelKey(m_roomId, key, level);
    if (opId == 0) {
        m_powerMatrixError =
            tr("The change could not be sent. Check your connection and "
               "retry.");
        Q_EMIT powerMatrixStateChanged();
        Q_EMIT powerMatrixActionFinished(m_roomId, key, level, false,
                                         m_powerMatrixError);
        return;
    }
    m_powerMatrixOp = opId;
    m_powerMatrixError.clear();
    Q_EMIT powerMatrixStateChanged();
}

void RoomInfoController::onPowerMatrixFinished(quint64 opId,
                                               const QString &roomId,
                                               const QString &key,
                                               qlonglong level, bool ok,
                                               const QString &category)
{
    if (opId == 0 || opId != m_powerMatrixOp)
        return;
    m_powerMatrixOp = 0;
    const QString message = ok
        ? QString()
        : (category == QLatin1String("forbidden")
               ? tr("The server refused that change. Your permissions may "
                    "have changed.")
               : category == QLatin1String("rate_limited")
                     ? tr("The server is rate limiting this action. Try "
                          "again shortly.")
                     : tr("The change failed. Check your connection and "
                          "retry."));
    m_powerMatrixError = message;
    Q_EMIT powerMatrixStateChanged();
    Q_EMIT powerMatrixActionFinished(roomId, key, level, ok, message);
    // Re-read the roster either way: it carries the threshold the room now
    // holds, and nothing writes the new value into the snapshot by hand.
    if (roomId == m_roomId)
        refreshMembers();
}

void RoomInfoController::onInviteUserFinished(quint64 opId,
                                              const QString &roomId,
                                              const QString &userId, bool ok,
                                              const QString &category)
{
    if (opId == 0 || opId != m_inviteBackOp)
        return;
    m_inviteBackOp = 0;
    const QString message = ok
        ? QString()
        : (category == QLatin1String("forbidden")
               ? tr("You do not have permission to invite them.")
               : tr("The invite could not be sent."));
    Q_EMIT moderationActionFinished(roomId, userId,
                                    QStringLiteral("invite_back"), ok,
                                    message);
    // Show the Invited state immediately; a newer fetch supersedes any
    // pending one.
    if (ok && roomId == m_roomId)
        refreshMembers();
}

QVariantList RoomInfoController::filterMembers(const QString &needle) const
{
    return visibleMembers(needle, QString(), false);
}

void RoomInfoController::requestMutualRooms(const QString &userId)
{
    // Clear and announce first, so a card opened on a new person never shows
    // the previous person's rooms. Empty means "not known yet".
    if (!m_mutualRooms.isEmpty()) {
        m_mutualRooms.clear();
        Q_EMIT mutualRoomsChanged();
    }
    m_mutualRoomsUser = userId;
    m_mutualRoomsOp = 0;
    if (!m_client || userId.isEmpty())
        return;
    m_mutualRoomsOp = m_client->fetchMutualRooms(userId);
}

QVariantMap RoomInfoController::memberFor(const QString &userId) const
{
    return memberFor(userId, m_roomId);
}

QVariantMap RoomInfoController::memberFor(const QString &userId,
                                          const QString &roomId) const
{
    if (userId.isEmpty())
        return {};

    // 1. The loaded roster snapshot, when it is this room's: the richest
    //    answer. Exact match only, since Matrix localparts are
    //    case-sensitive.
    if (!roomId.isEmpty() && roomId == m_roomId) {
        for (const QVariant &value : m_members) {
            const QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("userId")).toString() == userId)
                return row;
        }
    }

    // 2. The client's per-room member cache. The snapshot exists only once
    //    Room Information was opened for this room, which a mention click
    //    usually has not; the cache is what mention chips and reply headers
    //    use, so the card agrees with the pill.
    if (!m_client || roomId.isEmpty())
        return {};
    const QString name = m_client->displayNameFor(roomId, userId);
    const QString avatar = m_client->avatarMxcFor(roomId, userId);
    // Both return the user id itself when they know nothing; that is not a
    // display name.
    const QString resolved = name == userId ? QString() : name;
    if (resolved.isEmpty() && avatar.isEmpty())
        return {};
    // No membership or power level: this source does not know them, and an
    // absent key says so.
    return QVariantMap{
        { QStringLiteral("userId"), userId },
        { QStringLiteral("displayName"), resolved },
        { QStringLiteral("avatarUrl"), avatar },
    };
}

QVariantList RoomInfoController::filterMembers(const QString &needle,
                                               const QString &membership,
                                               bool alphabetical) const
{
    return visibleMembers(needle, membership, alphabetical);
}

QVariantList RoomInfoController::visibleMembers(const QString &needle,
                                                const QString &membership,
                                                bool alphabetical) const
{
    const QString lc = needle.trimmed().toLower();
    const QString facet = membership.trimmed();
    QVariantList out;
    out.reserve(m_members.size());
    for (const QVariant &value : m_members) {
        const QVariantMap row = value.toMap();
        // An unrecognised facet matches nothing rather than meaning "all".
        if (!facet.isEmpty()
            && row.value(QStringLiteral("membership")).toString() != facet) {
            continue;
        }
        if (!lc.isEmpty()
            && !row.value(QStringLiteral("displayName")).toString().toLower().contains(lc)
            && !row.value(QStringLiteral("userId")).toString().toLower().contains(lc)) {
            continue;
        }
        out.append(row);
    }
    if (alphabetical) {
        // Case-insensitive by display name, falling back to the user id. See
        // the header on how this interacts with MEMBER_SNAPSHOT_CAP.
        std::sort(out.begin(), out.end(),
                  [](const QVariant &lhs, const QVariant &rhs) {
                      const QVariantMap a = lhs.toMap();
                      const QVariantMap b = rhs.toMap();
                      const QString an =
                          a.value(QStringLiteral("displayName")).toString().isEmpty()
                              ? a.value(QStringLiteral("userId")).toString()
                              : a.value(QStringLiteral("displayName")).toString();
                      const QString bn =
                          b.value(QStringLiteral("displayName")).toString().isEmpty()
                              ? b.value(QStringLiteral("userId")).toString()
                              : b.value(QStringLiteral("displayName")).toString();
                      const int cmp = an.compare(bn, Qt::CaseInsensitive);
                      if (cmp != 0)
                          return cmp < 0;
                      // Total order: display names can collide, and an
                      // unstable comparator is UB in std::sort.
                      return a.value(QStringLiteral("userId")).toString()
                             < b.value(QStringLiteral("userId")).toString();
                  });
    }
    return out;
}

QVariantList RoomInfoController::memberRoleGroups(const QString &needle,
                                                  const QString &membership,
                                                  bool alphabetical) const
{
    const QVariantList rows = visibleMembers(needle, membership, alphabetical);
    // Bucket by exact level, highest first, never by label.
    QList<qlonglong> order;
    QHash<qlonglong, QVariantList> buckets;
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QString userId = row.value(QStringLiteral("userId")).toString();
        const qlonglong level =
            row.contains(QStringLiteral("powerLevel"))
                ? row.value(QStringLiteral("powerLevel")).toLongLong()
                : powerLevelFor(userId);
        if (!buckets.contains(level))
            order.append(level);
        buckets[level].append(row);
    }
    std::sort(order.begin(), order.end(), std::greater<qlonglong>());
    QVariantList groups;
    groups.reserve(order.size());
    for (const qlonglong level : std::as_const(order)) {
        QVariantMap group;
        group.insert(QStringLiteral("level"), level);
        group.insert(QStringLiteral("label"), roleLabelForLevel(level));
        group.insert(QStringLiteral("members"), buckets.value(level));
        groups.append(group);
    }
    return groups;
}

QVariantList RoomInfoController::memberRoleRows(const QString &needle,
                                                const QString &membership,
                                                bool alphabetical) const
{
    const QVariantList groups = memberRoleGroups(needle, membership,
                                                 alphabetical);
    QVariantList out;
    for (const QVariant &value : groups) {
        const QVariantMap group = value.toMap();
        const QVariantList members =
            group.value(QStringLiteral("members")).toList();
        const QString label = group.value(QStringLiteral("label")).toString();
        const qlonglong level =
            group.value(QStringLiteral("level")).toLongLong();
        QVariantMap header;
        header.insert(QStringLiteral("kind"), QStringLiteral("header"));
        header.insert(QStringLiteral("label"), label);
        header.insert(QStringLiteral("level"), level);
        header.insert(QStringLiteral("count"), members.size());
        // A header id that can never be mistaken for a user id ('@' starts
        // every Matrix user id).
        header.insert(QStringLiteral("userId"),
                      QStringLiteral("role:%1").arg(level));
        out.append(header);
        for (const QVariant &memberValue : members) {
            QVariantMap row = memberValue.toMap();
            row.insert(QStringLiteral("kind"), QStringLiteral("member"));
            row.insert(QStringLiteral("roleLabel"), label);
            row.insert(QStringLiteral("powerLevel"), level);
            out.append(row);
        }
    }
    return out;
}

void RoomInfoController::onLoggedOut()
{
    m_membersOp = 0;
    m_editOp = 0;
    m_leaveOp = 0;
    m_moderationOp = 0;
    m_powerLevelOp = 0;
    m_powerLevelUserId.clear();
    m_powerMatrixOp = 0;
    m_powerMatrixError.clear();
    m_inviteBackUserId.clear();
    m_inviteBackOp = 0;
    m_adhocLeaveOps.clear();
    m_roomId.clear();
    m_editError.clear();
    m_leaveError.clear();
    clearSnapshot();
    Q_EMIT roomIdChanged();
    Q_EMIT editStateChanged();
    Q_EMIT leaveStateChanged();
    Q_EMIT moderationStateChanged();
    Q_EMIT powerLevelStateChanged();
    Q_EMIT powerMatrixStateChanged();
}
