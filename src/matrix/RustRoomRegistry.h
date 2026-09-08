#pragma once

#include "matrix/RoomInfo.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Pure translation layer between the Rust bridge's room-list JSON protocol
// and the C++ room registry. No Qt models, no FFI, no I/O — unit-testable
// without cargo or a homeserver, exactly like matrix::rust_timeline next to
// it.
//
// THE ONE INVARIANT THIS FILE EXISTS TO HOLD: `order` is the SDK's own room
// list, ONE FOR ONE, because every Set / Insert / Remove / Truncate diff
// addresses it BY INDEX. Only the producer that owns that index space — the
// sliding-sync dynamic adapter, through `room_list_*` diffs and its own
// `room_list_reset` — may define, grow, shrink or renumber it.
//
// `rooms` is the id-keyed map everything else reads, and it holds MORE than
// the index space does: Spaces (which are not in the SDK's room list at all)
// and, on the classic-sync fallback, every room there is.
//
// A `room_snapshot` is a walk of the SDK's whole state store — a different
// vector, differently filtered, differently ordered, and a different length
// (the sliding list starts at 20 rooms and grows in pages of 100). It
// therefore updates `rooms` and NEVER touches `order`. Emitting it as a
// `room_list_reset` is what made the "room_list malformed diff rejected"
// storm self-sustaining: any of a dozen ordinary user actions rebuilt the
// index base from the wrong vector, the adapter's next Set{index} addressed a
// different room, was rejected, and the rejection asked for another snapshot.
namespace matrix::rust_rooms {

// A view over the client's two members, so the pure functions can be driven
// from a test with two local variables and from production with no copying.
struct Registry {
    QHash<QString, RoomInfo> &rooms;
    QStringList &order;
};

// Merge one room payload over what is already known about that room. Absent
// fields keep the previous value where that is right and clear it where a
// present-but-empty value is meaningful; see the field comments.
RoomInfo roomInfoFromJson(const QJsonObject &obj, const RoomInfo &previous);

// Apply a `room_list_reset` (or the legacy `rooms` envelope): the index base.
// Replaces both `rooms` and `order` wholesale, carrying Spaces over because
// the SDK's room list does not mention them.
void applyIndexReset(Registry registry, const QJsonArray &rooms);

// Apply a `room_snapshot`: the SDK state-store walk. Updates and adds rooms,
// drops rooms no producer still names, and never touches `order`.
//
// The removal rule is one rule in both lanes: an id absent from the snapshot
// is dropped UNLESS it is a Space (not in the SDK's list) or the index space
// still names it (only a diff may remove one of those). With an empty `order`
// — the classic lane, which has no diffs — that reduces exactly to "the
// snapshot is the room set", which is what that lane needs.
void applySnapshot(Registry registry, const QJsonArray &rooms);

// One room-list diff, validated against the registry before anything is
// mutated. Returns false — leaving the registry untouched — when the diff
// does not match, so the caller can ask the index space's owner to re-emit
// rather than corrupting the registry.
bool applyRoomListDiff(Registry registry, const QJsonObject &event);

} // namespace matrix::rust_rooms
