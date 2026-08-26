#include "app/RoomInfoController.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcRoomInfo, "lightning.roominfo")

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <utility>

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
        connect(m_client, &MatrixClient::powerLevelChangeFinished,
                this, &RoomInfoController::onPowerLevelChangeFinished);
        connect(m_client, &MatrixClient::roomPowerMatrixFinished,
                this, &RoomInfoController::onPowerMatrixFinished);
        connect(m_client, &MatrixClient::inviteUserFinished,
                this, &RoomInfoController::onInviteUserFinished);
        // The sync poke, NOT membersChanged (review H1): this controller
        // REFETCHES on the signal, and membersChanged also fires for the
        // snapshots its own fetches deliver — the pending-op guard is
        // what kept that from looping. roomMemberEventSeen carries only
        // the "membership changed in sync" meaning.
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
        // Reopening the panel for the room it last showed used to render
        // an arbitrarily old snapshot with loading == false and no way to
        // refetch. A refetch here is a store read after the first sync.
        if (!m_roomId.isEmpty() && m_membersOp == 0)
            refreshMembers();
        return;
    }
    m_roomId = roomId;
    // Room switch invalidates every in-flight operation for the old room.
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
    // An EMPTY matrix is the unknown state, and every reader must treat it
    // that way: clearing to zeros would claim the room requires 0 for
    // everything, which is a real (and very permissive) configuration.
    m_powerLevels.clear();
    m_roomVersion.clear();
    m_canUpgradeRoom = false;
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
    // A partial (cache-only) snapshot renders immediately but keeps the
    // op pending — `loading` stays true until the synced roster lands
    // under the same op.
    const bool partial = snapshot.value(QStringLiteral("partial")).toBool();
    if (!partial)
        m_membersOp = 0;
    if (!snapshot.value(QStringLiteral("ok")).toBool()) {
        Q_EMIT membersChanged();
        return;
    }
    m_members = snapshot.value(QStringLiteral("members")).toList();
    // The counts and the truncation flag are WHOLE-roster facts a
    // cache-only snapshot cannot know — a confidently wrong "2 members"
    // is worse than the previous value while loading (review M3). Rows
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
    // Space Home's Remove / Mark-as-suggested / Invite controls are gated on
    // canManageSpaceChildren and canInvite, and when a gate reads false they
    // simply are not rendered — which is indistinguishable, from the outside,
    // from the surface having lost them. Booleans and a power level only; the
    // room id is already truncated by the shared logging helper elsewhere and
    // is deliberately NOT repeated here.
    qCDebug(lcRoomInfo)
        << "member snapshot power gates ownLevel=" << m_ownPowerLevel
        << "canInvite=" << m_canInvite
        << "canManageSpaceChildren=" << m_canManageSpaceChildren
        << "canChangePowerLevels=" << m_canChangePowerLevels
        << "joined=" << m_joinedCount;
    m_joinRule = snapshot.value(QStringLiteral("joinRule")).toString();
    m_canonicalAlias =
        snapshot.value(QStringLiteral("canonicalAlias")).toString();
    // 2026-08-26: the room's real thresholds. A backend that does not send
    // them leaves the map EMPTY rather than defaulted — the Permissions
    // matrix then renders nothing instead of inventing a permission model.
    m_powerLevels = snapshot.value(QStringLiteral("powerLevels")).toMap();
    m_roomVersion = snapshot.value(QStringLiteral("roomVersion")).toString();
    m_canUpgradeRoom =
        snapshot.value(QStringLiteral("canUpgradeRoom")).toBool();
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
    //
    // Name/topic/avatar reach the UI through the room-list model, which sync
    // updates on its own. The v0.7.x room-state fields do NOT: join rule and
    // canonical alias ride the MEMBER SNAPSHOT, and nothing refetches that
    // for a state change that is not a membership change. So those two ask
    // for the roster explicitly, on success only — a failed write changed
    // nothing to re-read.
    if (ok
        && (field == QLatin1String("join_rule")
            || field == QLatin1String("canonical_alias"))) {
        refreshMembers();
    }
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
                                     const QString &reason, bool inviteBack)
{
    // With an action already in flight, arming would OVERWRITE the live
    // unban's pending invite (review L3) — the busy refusal must come
    // before the arm, and moderate() below re-refuses regardless.
    if (moderationPending())
        return;
    m_inviteBackUserId = inviteBack ? userId : QString();
    moderate(userId, reason, QStringLiteral("unban"));
    // moderate() can refuse synchronously; never leave the invite armed
    // for an unban that was not dispatched.
    if (m_moderationOp == 0)
        m_inviteBackUserId.clear();
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
    // Invite-back requested from the unban confirm: only after a
    // SUCCESSFUL unban, through the normal invite path; its own result
    // arrives via onInviteUserFinished.
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
    // The roster refresh is CLIENT-initiated: the Rust backend only emits
    // membersChanged in response to an explicit member fetch, never from
    // sync, so without this the kicked user would stay in the People list
    // until the next room switch (review M2).
    if (ok && roomId == m_roomId)
        refreshMembers();
}

// ---------------------------------------------------------------------------
// v0.7.x room administration
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
    // The conventional Matrix presets, and NOTHING ELSE flattened into
    // them. A room may use any integer; showing "Moderator" for 42 would be
    // a lie, and rounding 42 to 50 on save would destroy the room's own
    // configuration. Anything unconventional renders as its number.
    //
    // The room's OWN default is checked first: in a room that sets
    // users_default to 50, a member sitting at 50 is an ordinary member
    // there, and calling them "Moderator" would misdescribe the room's
    // configuration in the one place the user consults to understand it.
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
            // Self: demotion only. Matrix lets a user lower their own
            // level, and nothing lets them raise it.
            return level < current;
        }
        // A peer at or above your own level is not yours to change.
        return current < m_ownPowerLevel;
    }
    return false;
}

void RoomInfoController::setMemberPowerLevel(const QString &userId,
                                             qlonglong level)
{
    // Re-check the gate the UI used: room state can change between the menu
    // opening and the click, and a state event that is known to be
    // unauthorized must not be sent.
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
    // Re-read the authoritative roster either way. On success it carries
    // the level the room now holds; on failure it discards anything the UI
    // might have shown optimistically. Nothing here writes the new level
    // into the snapshot by hand.
    if (roomId == m_roomId)
        refreshMembers();
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
    const quint64 opId = m_client->setRoomJoinRule(m_roomId, rule);
    if (opId == 0) {
        m_editError = tr("The change could not be sent.");
        Q_EMIT editStateChanged();
        return;
    }
    m_editOp = opId;
    m_editError.clear();
    Q_EMIT editStateChanged();
}

void RoomInfoController::setCanonicalAlias(const QString &alias)
{
    if (!m_client || m_roomId.isEmpty() || !supported())
        return;
    if (!m_canChangeAlias || m_editOp != 0)
        return;
    QString value = alias.trimmed();
    if (!value.isEmpty()) {
        // Accept a bare localpart and complete it with the account's own
        // server; typing "#name:server.example" by hand is not something to
        // require of the user.
        if (!value.startsWith(QLatin1Char('#')))
            value.prepend(QLatin1Char('#'));
        if (!value.contains(QLatin1Char(':'))) {
            const QString own = m_client->currentUserId();
            const int colon = own.indexOf(QLatin1Char(':'));
            if (colon < 0 || colon + 1 >= own.size()) {
                m_editError = tr("The alias needs a server name.");
                Q_EMIT editStateChanged();
                return;
            }
            value += own.mid(colon);
        }
    }
    if (value == m_canonicalAlias)
        return;
    const quint64 opId = m_client->setRoomCanonicalAlias(m_roomId, value);
    if (opId == 0) {
        m_editError = tr("The change could not be sent.");
        Q_EMIT editStateChanged();
        return;
    }
    m_editOp = opId;
    m_editError.clear();
    Q_EMIT editStateChanged();
}

// ---------------------------------------------------------------------------
// 2026-08-26: the m.room.power_levels matrix (Space settings, Sable parity)
// ---------------------------------------------------------------------------

QStringList RoomInfoController::powerLevelKeys()
{
    // Must stay in step with the allowlist in rooms::set_room_power_level_key
    // and with the keys the Rust member snapshot emits. Anything not here is
    // refused at the Rust edge, so a QML typo produces an inert control
    // rather than an unexpected state event.
    //
    // Built once: powerLevelKnown() is called from a per-row binding, and a
    // fresh 15-element list per row per roster answer is pure waste.
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
    // -1 is a SENTINEL for "not known", not a level. It is safe only because
    // callers are told to ask powerLevelKnown() first: -1 is itself a legal
    // Matrix level (Element renders it as "Restricted"), so no in-band
    // number can carry the unknown meaning on its own.
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
    // THE ONE-WAY DOOR. Requiring MORE than you have for m.room.power_levels
    // locks you out of the key that would undo it, and no server will help.
    // The same clause is what stops users_default being raised above the
    // person raising it — which is how a room accidentally hands everyone
    // moderator rights.
    if (level > m_ownPowerLevel)
        return false;
    return m_powerLevels.value(key).toLongLong() != level;
}

void RoomInfoController::setPowerLevelKey(const QString &key, qlonglong level)
{
    // Re-check the gate the UI used: room state can change between the
    // control rendering and the click, and a state event that is known to be
    // unauthorized must not be sent.
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
    // Re-read the authoritative roster either way. On success it carries the
    // threshold the room now holds; on failure it discards anything the UI
    // might have shown optimistically. Nothing here writes the new value
    // into the snapshot by hand.
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
    // Show the Invited state without waiting for the next panel open; a
    // newer fetch op simply supersedes any pending one.
    if (ok && roomId == m_roomId)
        refreshMembers();
}

QVariantList RoomInfoController::filterMembers(const QString &needle) const
{
    return visibleMembers(needle, QString(), false);
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
        // An unrecognised facet matches NOTHING rather than quietly meaning
        // "all": a filter that silently stops filtering looks identical to
        // a filter that found everything.
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
        // Case-insensitive by the name a person actually reads, falling back
        // to the user id when a member has no display name. The Rust
        // snapshot's own order (joined → invited → banned, then power level
        // descending) is what the cap was applied to — see the header: past
        // MEMBER_SNAPSHOT_CAP this re-orders only the rows that survived it.
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
                      // A total order: two members may share a display name
                      // (that is what `ambiguous` is for), and an unstable
                      // comparator is undefined behaviour in std::sort.
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
    // Bucket by EXACT level, highest first. Not by the role LABEL: a room
    // using 42 and a room using 50 would collide under "Moderator" if two
    // custom numbers ever mapped to the same text, and folding them would
    // misdescribe the room's own configuration.
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
        // A header needs an id a delegate can key on, and it must never be
        // mistaken for a user id: '@' starts every Matrix user id, so a level
        // prefix is used instead.
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
