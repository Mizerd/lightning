#pragma once

#include "matrix/RoomInfo.h"

#include <QList>

namespace roomunread {

constexpr int kModeAllMessages = 0;
constexpr int kModeMentions = 1;
constexpr int kModeMute = 2;
constexpr int kModeMentionsUnverified = 4;

inline bool rawUnread(const RoomInfo &r)
{
    return r.hasUnreadMessages || r.markedUnread || r.unreadCount > 0
           || r.highlightCount > 0;
}

inline bool countsAsUnread(const RoomInfo &r, int mode)
{
    return rawUnread(r) && mode != kModeMute;
}

inline bool listsAsUnread(const RoomInfo &r, int mode)
{
    return countsAsUnread(r, mode) || (r.markedUnread && mode == kModeMute);
}

inline int mentionCount(const RoomInfo &r, int mode)
{
    return mode == kModeMute ? 0 : r.highlightCount;
}

inline int visibleUnreadCount(const RoomInfo &r, int mode)
{
    if (mode == kModeMute || mode == kModeMentionsUnverified)
        return 0;
    // Unread only in threads: the server number counts thread events the main
    // timeline does not show, so no surface may print it (section 8). The room
    // is still unread (countsAsUnread) and draws the thread glyph or a dot.
    if (r.unreadThreadsOnly)
        return 0;
    if (mode == kModeMentions && !r.encryptionKnown)
        return 0;
    return r.unreadCount > 0 ? r.unreadCount : 0;
}

inline int threadUnreadKind(const RoomInfo &r, int mode)
{
    if (mode == kModeMute || !r.unreadThreadsOnly)
        return 0;
    return r.mentionInThreads ? 2 : 1;
}

struct Summary {
    int total = 0;
    bool any = false;
};

template<typename ModeFn, typename RoomFn>
Summary summarize(const QList<RoomInfo> &rooms, ModeFn modeOf, RoomFn onRoom)
{
    Summary out;
    for (const RoomInfo &room : rooms) {
        if (room.membership == RoomInfo::Invited) {
            out.any = true;
            out.total += 1;
            continue;
        }
        if (room.membership != RoomInfo::Joined)
            continue;
        if (room.isSpace) {
            onRoom(room, false);
            continue;
        }
        const bool raw = rawUnread(room);
        const int mode = raw ? modeOf(room.id) : 0;
        const bool unread = raw && countsAsUnread(room, mode);
        if (unread) {
            out.any = true;
            const int shown = visibleUnreadCount(room, mode);
            out.total += shown > 0 ? shown : mentionCount(room, mode);
        }
        onRoom(room, unread);
    }
    return out;
}

}
