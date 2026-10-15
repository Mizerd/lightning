// The one rule for "this room is a 1:1 DM with that person", shared by the
// side that announces a MatrixRTC call (SfuCallController: `ring` or
// `notification`) and the side that decides whether an announcement rings
// (CallController). Header-only so both link without each other.
#pragma once

#include <QString>
#include <QStringList>

#include "matrix/RoomInfo.h"

namespace calls {

/// The peer of an unambiguous 1:1 DM: the room's ONE m.direct target, in a
/// room with at most two active (joined or invited) members, or empty. This
/// is matrix-sdk's DmRoomDefinition::TwoMembers, which Element X uses.
/// `isDirect` alone is not 1:1 (a DM a third person was invited into is still
/// direct; docs/security-audit-2026-09-02.md), so two targets, none, or a
/// third member is not a DM here. Backends that report only `directUserId`
/// count as one target; a backend that reports no member count
/// (activeMemberCount 0) is judged by the target alone.
inline QString oneToOneDmPeer(const RoomInfo &room)
{
    if (!room.isDirect)
        return {};
    if (room.activeMemberCount > 2)
        return {};
    QStringList targets = room.directUserIds;
    if (targets.isEmpty() && !room.directUserId.isEmpty())
        targets.append(room.directUserId);
    if (targets.size() != 1)
        return {};
    return targets.first();
}

} // namespace calls
