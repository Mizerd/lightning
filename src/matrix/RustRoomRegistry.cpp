#include "matrix/RustRoomRegistry.h"

#include "matrix/EventPreview.h"

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

} // namespace

RoomInfo roomInfoFromJson(const QJsonObject &obj, const RoomInfo &previous)
{
    const QString id = roomIdOf(obj);
    RoomInfo room = previous;
    room.id = id;
    room.name = obj.value(QStringLiteral("name")).toString(room.name);
    if (room.name.isEmpty()) room.name = room.id;
    room.topic = obj.value(QStringLiteral("topic")).toString(room.topic);
    room.canonicalAlias = obj.value(QStringLiteral("canonical_alias")).toString(room.canonicalAlias);
    room.avatarUrl = obj.value(QStringLiteral("avatar_url")).toString(room.avatarUrl);
    // A present-but-empty preview must not clobber one we already learned
    // from the open timeline or a live event: Rust legitimately sends ""
    // whenever the SDK has no latest event for the room yet, and room-list
    // set/insert diffs arrive on every unread/order change — pre-0.7 this
    // raced previews back to empty until the room was reopened.
    {
        // The Rust latest-event path sends plain text (typed summaries are
        // built Rust-side); normalization still guards legacy multi-line
        // bodies and mention markdown.
        const QString incomingPreview = matrix::preview::normalizePreviewText(
            obj.value(QStringLiteral("last_message_preview")).toString());
        if (!incomingPreview.isEmpty())
            room.lastMessagePreview = incomingPreview;
    }
    // RoomInfo::raiseActivity is monotonic; see its comment. Every writer of
    // the room list's sort key goes through it.
    room.raiseActivity(timestampFromMs(static_cast<qint64>(
        obj.value(QStringLiteral("last_activity_ms")).toDouble(0))));
    room.unreadCount = obj.value(QStringLiteral("unread_count")).toInt(room.unreadCount);
    room.highlightCount = obj.value(QStringLiteral("highlight_count")).toInt(room.highlightCount);
    room.markedUnread = obj.value(QStringLiteral("marked_unread")).toBool(room.markedUnread);
    room.hasUnreadMessages = obj.value(QStringLiteral("has_unread_messages"))
                                 .toBool(room.hasUnreadMessages || room.unreadCount > 0);
    room.encrypted = obj.value(QStringLiteral("encrypted")).toBool(room.encrypted);
    // Review H1: EncryptionState::Unknown must never read as "not
    // encrypted" — absent field defaults to NOT known (fail closed).
    room.encryptionKnown =
        obj.value(QStringLiteral("encryption_known")).toBool(false);
    room.isSpace = obj.value(QStringLiteral("is_space")).toBool(room.isSpace);
    // Defaults to FALSE, not to the previous value, exactly like is_direct
    // below: un-favouriting a room must actually clear the flag. Defaulting
    // to the old value would latch a favourite on for the rest of the
    // session the moment one payload arrived without the field.
    room.isFavourite = obj.value(QStringLiteral("is_favourite")).toBool(false);
    room.isDirect = obj.value(QStringLiteral("is_direct")).toBool(false);
    room.directUserId = obj.value(QStringLiteral("direct_user_id")).toString();
    room.directUserIds.clear();
    for (const auto &value : obj.value(QStringLiteral("direct_user_ids")).toArray())
        room.directUserIds.append(value.toString());
    room.roomType = obj.value(QStringLiteral("room_type")).toString();
    room.prevBatchToken = obj.value(QStringLiteral("prev_batch")).toString(room.prevBatchToken);
    room.inviterUserId = obj.value(QStringLiteral("inviter_user_id")).toString();
    room.inviterDisplayName = obj.value(QStringLiteral("inviter_display_name")).toString();
    // v0.7.x room upgrades. The defaulting form is deliberate and does the
    // right thing in both directions: an ABSENT field (a payload built by
    // an older path, or a backend with no tombstone support) keeps what we
    // already knew, while a PRESENT-but-empty one clears it, because Rust
    // computes these from SDK state on every emission and empty there means
    // the room genuinely has no successor. Both are room ids parsed by
    // ruma; neither is ever free text.
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
    // Spaces are NOT in the SDK's room list, so a reset of that list does not
    // mention them — and replacing the whole map wholesale therefore dropped
    // the entire Space hierarchy until the next spaces event happened to
    // arrive. Carried over instead: they are keyed separately in `rooms` and
    // deliberately absent from `order` (see the header).
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

    // O(1) membership rather than QStringList::contains() per room: with a
    // thousand rooms the linear form is a million string comparisons on a
    // path a dozen ordinary user actions reach.
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

    // Insert at `index`. The duplicate check is against the INDEX SPACE, not
    // against the room map: the map legitimately holds rooms the index space
    // does not (every Space, and any room a snapshot learned about before its
    // diff arrived), and rejecting on those refused a perfectly good diff.
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

    // The room the producer says occupies `index`, for the ops that carry no
    // room of their own. Empty means the producer could not confirm it, which
    // is itself a mismatch: a positional delete that nobody can name is
    // exactly the shape that deleted the wrong room.
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
        // A Set that renames a slot to an id the index space already holds
        // elsewhere would put the same room in two positions.
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
        // Only the index space is cleared. Spaces live in the map alone and
        // this diff says nothing about them.
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

} // namespace matrix::rust_rooms
