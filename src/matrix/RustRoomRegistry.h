#pragma once

#include "matrix/RoomInfo.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Pure translation between the Rust bridge's room-list JSON and the C++ room
// registry. No Qt models, FFI or I/O, so it is unit-testable like
// matrix::rust_timeline.
//
// Invariant: `order` mirrors the SDK's room list one-for-one, because every
// Set / Insert / Remove / Truncate diff addresses it by index. Only the
// sliding-sync dynamic adapter (its `room_list_*` diffs and `room_list_reset`)
// may define or renumber it.
//
// `rooms` is the id-keyed map everything else reads, and holds more than the
// index space: Spaces (not in the SDK's room list) and, on the classic-sync
// fallback, every room.
//
// A `room_snapshot` walks the SDK's whole state store: a different vector in
// membership, order and length. It updates `rooms` and never touches `order`;
// rebuilding the index base from it misaddresses the next Set{index} and
// feeds a reject/resnapshot loop.
namespace matrix::rust_rooms {

// A view over the client's two members, so the pure functions work on test
// locals and production state without copying.
struct Registry {
    QHash<QString, RoomInfo> &rooms;
    QStringList &order;
};

// Merge one room payload over what is already known about that room. Absent
// fields keep the previous value where that is right and clear it where a
// present-but-empty value is meaningful; see the field comments.
RoomInfo roomInfoFromJson(const QJsonObject &obj, const RoomInfo &previous);

// Apply a `room_list_reset` (or the legacy `rooms` envelope), the index base.
// Replaces `rooms` and `order` wholesale, carrying Spaces over since the SDK
// room list never mentions them.
void applyIndexReset(Registry registry, const QJsonArray &rooms);

// Apply a `room_snapshot`: updates and adds rooms, drops rooms no producer
// still names, never touches `order`. An absent id is dropped unless it is a
// Space or the index space still names it (only a diff may remove those).
// With an empty `order` (classic sync) the snapshot is simply the room set.
void applySnapshot(Registry registry, const QJsonArray &rooms);

// One room-list diff, validated before anything is mutated. Returns false,
// leaving the registry untouched, on a mismatch so the caller can ask the
// index space's owner to re-emit.
bool applyRoomListDiff(Registry registry, const QJsonObject &event);

// Apply a `room_activity` payload: response-harvested recency,
// `{ id, last_activity_ms }` per room. Returns only the ids whose activity
// moved.
//
// Every other payload derives last_activity_ms from matrix-sdk's lazily
// computed Room::latest_event(), which can stop moving and leave a row at a
// stale time and position until the room is opened. This reads the sync
// responses directly. Timestamps only; being monotonic it can only agree with
// or improve on the other producer.
QStringList applyRoomActivity(Registry registry, const QJsonArray &rooms);

// The removal half of a `space_list_reset`: `present` is the complete set of
// joined Space ids, so a Space entry absent from it has been left. Returns how
// many entries were erased. Unlike the room-list producer's payloads (see the
// Space exemptions above), absence here is evidence. A Space the index space
// still names is blanked, never erased.
int retireAbsentSpaces(Registry registry, const QSet<QString> &present);

} // namespace matrix::rust_rooms
