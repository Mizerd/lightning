#include "matrix/RustRoomRegistry.h"

#include "matrix/EventPreview.h"
#include "matrix/PresentableName.h"

#include <QDateTime>
#include <QSet>
#include <QTimeZone>

namespace matrix::rust_rooms {

namespace {

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

QString roomIdOf(const QJsonObject &obj)
{
    return obj.value(QStringLiteral("id")).toString();
}

}

RoomInfo roomInfoFromJson(const QJsonObject &obj, const RoomInfo &previous)
{
    const QString id = roomIdOf(obj);
    RoomInfo room = previous;
    room.id = id;
    // Any member with the power to rename the room writes this, so its
    // direction overrides go here, once, for every surface that shows it (the
    // list, the header, the composer placeholder, notifications): an RLO in a
    // name reversed its tail on screen (break-it s03).
    room.name = matrix::presentable_name::withoutDirectionalControls(
        obj.value(QStringLiteral("name")).toString(room.name));
    if (room.name.isEmpty()) room.name = room.id;
    room.topic = obj.value(QStringLiteral("topic")).toString(room.topic);
    room.canonicalAlias = obj.value(QStringLiteral("canonical_alias")).toString(room.canonicalAlias);
    room.avatarUrl = obj.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
    {
        const QString incomingPreview = matrix::preview::normalizePreviewText(
            obj.value(QStringLiteral("last_message_preview")).toString());
        const bool undecryptable =
            obj.value(QStringLiteral("last_message_undecryptable")).toBool(false);
        const bool newer = room.raiseActivity(timestampFromMs(static_cast<qint64>(
            obj.value(QStringLiteral("last_activity_ms")).toDouble(0))));
        const matrix::preview::Kind incomingKind = matrix::preview::kindFromWire(
            obj.value(QStringLiteral("last_message_preview_kind")).toString());
        if (!incomingPreview.isEmpty() && incomingKind != matrix::preview::Kind::Text)
            room.setPreview(incomingKind,
                            matrix::preview::normalizePreviewText(
                                obj.value(QStringLiteral("last_message_preview_arg"))
                                    .toString()));
        else if (!incomingPreview.isEmpty())
            room.setPreviewText(incomingPreview);
        else if (undecryptable && (room.lastMessagePreview.isEmpty() || newer))
            room.setPreview(matrix::preview::Kind::Undecryptable);
        else if (obj.value(QStringLiteral("last_message_own_join")).toBool(false)
                 && room.lastMessagePreview.isEmpty())
            room.setPreview(matrix::preview::Kind::OwnJoin);
    }
    room.unreadCount = obj.value(QStringLiteral("unread_count")).toInt(room.unreadCount);
    room.highlightCount = obj.value(QStringLiteral("highlight_count")).toInt(room.highlightCount);
    if (obj.contains(QStringLiteral("unread_count"))
        || obj.contains(QStringLiteral("highlight_count")))
        room.countsWrittenAtMs = QDateTime::currentMSecsSinceEpoch();
    room.markedUnread = obj.value(QStringLiteral("marked_unread")).toBool(room.markedUnread);
    room.hasUnreadMessages = obj.value(QStringLiteral("has_unread_messages"))
                                 .toBool(room.hasUnreadMessages || room.unreadCount > 0);
    room.unreadThreadsOnly =
        obj.value(QStringLiteral("unread_threads_only")).toBool(false);
    room.mentionInThreads =
        obj.value(QStringLiteral("mention_in_threads")).toBool(false);
    room.encrypted = obj.value(QStringLiteral("encrypted")).toBool(room.encrypted);
    room.encryptionKnown =
        obj.value(QStringLiteral("encryption_known")).toBool(false);
    room.isSpace = obj.value(QStringLiteral("is_space")).toBool(room.isSpace);
    room.isFavourite = obj.value(QStringLiteral("is_favourite")).toBool(false);
    room.lowPriority = obj.value(QStringLiteral("is_low_priority")).toBool(false);
    room.isDirect = obj.value(QStringLiteral("is_direct")).toBool(false);
    room.directUserId = obj.value(QStringLiteral("direct_user_id")).toString();
    room.directUserIds.clear();
    for (const auto &value : obj.value(QStringLiteral("direct_user_ids")).toArray())
        room.directUserIds.append(value.toString());
    room.activeMemberCount = obj.value(QStringLiteral("active_member_count"))
                                 .toInt(obj.value(QStringLiteral("member_count"))
                                            .toInt(room.activeMemberCount));
    room.joinedMemberCount = obj.value(QStringLiteral("member_count"))
                                 .toInt(room.joinedMemberCount);
    room.joinRule = obj.value(QStringLiteral("join_rule")).toString(room.joinRule);
    room.roomType = obj.value(QStringLiteral("room_type")).toString();
    room.prevBatchToken = obj.value(QStringLiteral("prev_batch")).toString(room.prevBatchToken);
    room.inviterUserId = obj.value(QStringLiteral("inviter_user_id")).toString();
    room.inviterDisplayName = matrix::presentable_name::withoutDirectionalControls(
        obj.value(QStringLiteral("inviter_display_name")).toString());
    room.successorRoomId =
        obj.value(QStringLiteral("successor_room_id")).toString(room.successorRoomId);
    room.predecessorRoomId =
        obj.value(QStringLiteral("predecessor_room_id")).toString(room.predecessorRoomId);
    const QString membership = obj.value(QStringLiteral("membership")).toString(
        QStringLiteral("joined"));
    room.membership = membership == QLatin1String("invited") ? RoomInfo::Invited
        : membership == QLatin1String("knocked") ? RoomInfo::Knocked
        : membership == QLatin1String("left") ? RoomInfo::Left : RoomInfo::Joined;
    return room;
}

void applyIndexReset(Registry registry, const QJsonArray &rooms)
{
    QHash<QString, RoomInfo> nextRooms;
    nextRooms.reserve(rooms.size());
    QStringList nextOrder;
    QSet<QString> seen;

    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        const QString id = roomIdOf(obj);
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.insert(id);
        nextRooms.insert(id, roomInfoFromJson(obj, registry.rooms.value(id)));
        nextOrder.append(id);
    }
    for (auto it = registry.rooms.cbegin(); it != registry.rooms.cend(); ++it) {
        if (it->isSpace && !seen.contains(it.key()))
            nextRooms.insert(it.key(), *it);
    }
    registry.rooms = nextRooms;
    registry.order = nextOrder;
}

void applySnapshot(Registry registry, const QJsonArray &rooms)
{
    QSet<QString> seen;
    seen.reserve(rooms.size());
    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        const QString id = roomIdOf(obj);
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.insert(id);
        registry.rooms.insert(id, roomInfoFromJson(obj, registry.rooms.value(id)));
    }

    QSet<QString> indexed;
    indexed.reserve(registry.order.size());
    for (const auto &id : registry.order)
        indexed.insert(id);

    for (auto it = registry.rooms.begin(); it != registry.rooms.end();) {
        const bool keep = seen.contains(it.key())
            || it->isSpace
            || indexed.contains(it.key());
        if (keep)
            ++it;
        else
            it = registry.rooms.erase(it);
    }
}

bool applyRoomListDiff(Registry registry, const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();

    auto addRoom = [&registry](int index, const QJsonObject &object) {
        const QString id = roomIdOf(object);
        if (id.isEmpty() || index < 0 || index > registry.order.size())
            return false;
        if (registry.order.contains(id))
            return false;
        registry.rooms.insert(id, roomInfoFromJson(object, registry.rooms.value(id)));
        registry.order.insert(index, id);
        return true;
    };

    auto expectedIdMatches = [&registry, &event](int index) {
        const QString expected =
            event.value(QStringLiteral("expected_id")).toString();
        if (expected.isEmpty())
            return false;
        return index >= 0 && index < registry.order.size()
            && registry.order.at(index) == expected;
    };
    auto removeAt = [&registry](int index) {
        registry.rooms.remove(registry.order.takeAt(index));
    };

    if (type == QLatin1String("room_list_append")) {
        bool ok = true;
        for (const auto &value : event.value(QStringLiteral("rooms")).toArray())
            ok = addRoom(registry.order.size(), value.toObject()) && ok;
        return ok;
    }
    if (type == QLatin1String("room_list_push_front"))
        return addRoom(0, event.value(QStringLiteral("room")).toObject());
    if (type == QLatin1String("room_list_push_back")) {
        return addRoom(registry.order.size(),
                       event.value(QStringLiteral("room")).toObject());
    }
    if (type == QLatin1String("room_list_insert")) {
        return addRoom(event.value(QStringLiteral("index")).toInt(-1),
                       event.value(QStringLiteral("room")).toObject());
    }
    if (type == QLatin1String("room_list_set")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        const QJsonObject object = event.value(QStringLiteral("room")).toObject();
        const QString id = roomIdOf(object);
        if (index < 0 || index >= registry.order.size() || id.isEmpty())
            return false;
        const QString oldId = registry.order.at(index);
        if (id != oldId && registry.order.contains(id))
            return false;
        if (id != oldId)
            registry.rooms.remove(oldId);
        registry.rooms.insert(id, roomInfoFromJson(object, registry.rooms.value(id)));
        registry.order[index] = id;
        return true;
    }
    if (type == QLatin1String("room_list_remove")) {
        const int index = event.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= registry.order.size()) return false;
        if (!expectedIdMatches(index)) return false;
        removeAt(index);
        return true;
    }
    if (type == QLatin1String("room_list_pop_front")) {
        if (registry.order.isEmpty()) return false;
        if (!expectedIdMatches(0)) return false;
        removeAt(0);
        return true;
    }
    if (type == QLatin1String("room_list_pop_back")) {
        if (registry.order.isEmpty()) return false;
        if (!expectedIdMatches(registry.order.size() - 1)) return false;
        removeAt(registry.order.size() - 1);
        return true;
    }
    if (type == QLatin1String("room_list_clear")) {
        for (const auto &id : registry.order)
            registry.rooms.remove(id);
        registry.order.clear();
        return true;
    }
    if (type == QLatin1String("room_list_truncate")) {
        const int length = event.value(QStringLiteral("length")).toInt(-1);
        if (length < 0 || length > registry.order.size()) return false;
        while (registry.order.size() > length)
            registry.rooms.remove(registry.order.takeLast());
        return true;
    }
    return false;
}

QStringList applyRoomActivity(Registry registry, const QJsonArray &rooms)
{
    QStringList moved;
    for (const auto &value : rooms) {
        const QJsonObject obj = value.toObject();
        const QString id = roomIdOf(obj);
        if (id.isEmpty())
            continue;
        auto it = registry.rooms.find(id);
        if (it == registry.rooms.end())
            continue;
        // Two producers write this field with opposite mechanisms: the C++ live
        // path is a deny-list over TimelineEvent types (!isVirtual, !=
        // StateChange, != CallEvent), the Rust harvest an allow-list of wire
        // type names. Both only raise, so the effective policy is their union.
        // Fragile: a new row kind can start raising on one side only, and only
        // the Rust half is tested.
        const auto ms = static_cast<qint64>(
            obj.value(QStringLiteral("last_activity_ms")).toDouble(0));
        if (it->raiseActivity(timestampFromMs(ms))) {
            if (obj.value(QStringLiteral("last_message_undecryptable")).toBool(false))
                it->setPreview(matrix::preview::Kind::Undecryptable);
            moved.append(id);
        }
    }
    return moved;
}

int retireAbsentSpaces(Registry registry, const QSet<QString> &present)
{
    QSet<QString> indexed(registry.order.cbegin(), registry.order.cend());
    int erased = 0;
    for (auto it = registry.rooms.begin(); it != registry.rooms.end();) {
        // Joined only: `present` comes from joined_space_rooms(), so an invited
        // Space is always absent, and erasing it would make invitations
        // impossible to see or accept. Left Spaces are still recorded as
        // Joined.
        if (it->isSpace && it->membership == RoomInfo::Joined
            && !present.contains(it.key())) {
            if (!indexed.contains(it.key())) {
                it = registry.rooms.erase(it);
                ++erased;
                continue;
            }
            it->childRoomIds.clear();
            it->parentSpaceIds.clear();
        }
        ++it;
    }
    return erased;
}

}
