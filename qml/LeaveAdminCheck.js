.pragma library

// Whether leaving would strand a room or Space without an administrator: the
// user holds the only power level of 100 or more among the joined members, and
// others remain. `info` is the RoomInfoController, and counts only when it is
// following `roomId` and holds a complete roster; anything partial, still
// loading or about another room answers false, never a guess.
function soleAdminOf(info, roomId) {
    if (!info || info.truncated || info.loading || info.roomId.length === 0
            || (roomId !== undefined && info.roomId !== roomId)
            || info.ownPowerLevel < 100)
        return false
    var admins = 0
    const members = info.joinedMembers
    for (var i = 0; i < members.length; ++i)
        if (members[i].powerLevel !== undefined && members[i].powerLevel >= 100)
            ++admins
    return admins === 1 && members.length > 1
}
