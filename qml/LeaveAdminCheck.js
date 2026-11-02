.pragma library

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
