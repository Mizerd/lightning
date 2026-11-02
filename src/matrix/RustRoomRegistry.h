#pragma once

#include "matrix/RoomInfo.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace matrix::rust_rooms {

struct Registry {
    QHash<QString, RoomInfo> &rooms;
    QStringList &order;
};

RoomInfo roomInfoFromJson(const QJsonObject &obj, const RoomInfo &previous);

void applyIndexReset(Registry registry, const QJsonArray &rooms);

// Apply a `room_snapshot`: updates and adds rooms, drops rooms no producer
// still names, never touches `order`. An absent id is dropped unless it is a
// Space or the index space still names it (only a diff may remove those).
// With an empty `order` (classic sync) the snapshot is simply the room set.
void applySnapshot(Registry registry, const QJsonArray &rooms);

bool applyRoomListDiff(Registry registry, const QJsonObject &event);

QStringList applyRoomActivity(Registry registry, const QJsonArray &rooms);

int retireAbsentSpaces(Registry registry, const QSet<QString> &present);

}
