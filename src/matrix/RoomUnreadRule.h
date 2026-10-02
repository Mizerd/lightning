#pragma once

#include "matrix/RoomInfo.h"

#include <QList>

// The one rule for "does this room count as unread", shared by the Spaces rail
// and the tray badge (GitHub #18: the tray ignored the notification mode, so a
// muted room painted a red dot the rail correctly did not).
namespace roomunread {

// SettingsManager::roomNotificationMode(): 0 is All messages, 2 is mute.
constexpr int kModeAllMessages = 0;
constexpr int kModeMute = 2;

// The room list's unread rule: notification_count is 0 for a room whose push
// rules do not notify, so the read-receipt flag and a manual mark count too.
inline bool rawUnread(const RoomInfo &r)
{
    return r.hasUnreadMessages || r.markedUnread || r.unreadCount > 0
           || r.highlightCount > 0;
}

// A muted room is fully silent, as in Element (determineUnreadState returns
// count 0 and level None for a muted room before it reads highlights): not
// unread for a dot, a count or a mention, whatever its receipt state or
// highlight count says. "Mentions & keywords" is the mode where mentions show.
// `mode` is the room's notification mode.
inline bool countsAsUnread(const RoomInfo &r, int mode)
{
    return rawUnread(r) && mode != kModeMute;
}

/// The mention count a badge may show: zero for a muted room.
inline int mentionCount(const RoomInfo &r, int mode)
{
    return mode == kModeMute ? 0 : r.highlightCount;
}

struct Summary {
    int total = 0;
    bool any = false;
};

// The tray's walk. `modeOf(roomId)` is read only for a room that is raw-unread;
// `onRoom(room, countsAsUnread)` sees every joined room (invites are not
// unread messages and have their own notification).
template<typename ModeFn, typename RoomFn>
Summary summarize(const QList<RoomInfo> &rooms, ModeFn modeOf, RoomFn onRoom)
{
    Summary out;
    for (const RoomInfo &room : rooms) {
        if (room.membership != RoomInfo::Joined)
            continue;
        const bool raw = rawUnread(room);
        const bool unread = raw && countsAsUnread(room, modeOf(room.id));
        if (unread) {
            out.any = true;
            out.total += room.unreadCount > 0 ? room.unreadCount : 0;
        }
        onRoom(room, unread);
    }
    return out;
}

} // namespace roomunread
