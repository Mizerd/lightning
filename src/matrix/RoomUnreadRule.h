#pragma once

#include "matrix/RoomInfo.h"

#include <QList>

namespace roomunread {

constexpr int kModeAllMessages = 0;
constexpr int kModeMentions = 1;
constexpr int kModeMute = 2;
constexpr int kModeMentionsUnverified = 4;

// Policy bits above the notification mode. A "quiet" room shows what notified
// (counts, mentions) and an explicit marked-unread mark, but no activity dot;
// kDotNever also drops the plain count, keeping only mentions.
constexpr int kModeMask = 0xff;
constexpr int kDotNotifying = 0x100;
constexpr int kDotNever = 0x200;
constexpr int kAccountQuiet = 0x400;

constexpr int kUnreadDotAll = 0;
constexpr int kUnreadDotNotifying = 1;
constexpr int kUnreadDotNever = 2;

inline int baseMode(int mode)
{
    return mode & kModeMask;
}

inline int withDotPolicy(int mode, int dotSetting, bool accountQuiet)
{
    int out = mode & kModeMask;
    if (dotSetting == kUnreadDotNever)
        out |= kDotNever;
    else if (dotSetting == kUnreadDotNotifying)
        out |= kDotNotifying;
    if (accountQuiet)
        out |= kAccountQuiet;
    return out;
}

inline bool rawUnread(const RoomInfo &r)
{
    return r.hasUnreadMessages || r.markedUnread || r.unreadCount > 0
           || r.highlightCount > 0;
}

inline bool quiet(const RoomInfo &r, int mode)
{
    if (mode & kDotNever)
        return true;
    if (!(mode & kDotNotifying))
        return false;
    const int base = baseMode(mode);
    if (base == kModeMentions || base == kModeMentionsUnverified
        || base == kModeMute)
        return true;
    // Following the account's rules: a notifying unread proves they notify here.
    return (mode & kAccountQuiet) && r.unreadCount <= 0;
}

inline int mentionCount(const RoomInfo &r, int mode)
{
    return baseMode(mode) == kModeMute ? 0 : r.highlightCount;
}

inline int visibleUnreadCount(const RoomInfo &r, int mode)
{
    const int base = baseMode(mode);
    if (base == kModeMute || base == kModeMentionsUnverified)
        return 0;
    // Unread only in threads: the server number counts thread events the main
    // timeline does not show, so no surface may print it (section 8). The room
    // is still unread (countsAsUnread) and draws the thread glyph or a dot.
    if (r.unreadThreadsOnly)
        return 0;
    if (base == kModeMentions && !r.encryptionKnown)
        return 0;
    if (mode & kDotNever)
        return 0;
    return r.unreadCount > 0 ? r.unreadCount : 0;
}

inline int threadUnreadKind(const RoomInfo &r, int mode)
{
    if (baseMode(mode) == kModeMute || !r.unreadThreadsOnly)
        return 0;
    if (r.mentionInThreads)
        return 2;
    return quiet(r, mode) ? 0 : 1;
}

inline bool showsActivityDot(const RoomInfo &r, int mode)
{
    return r.hasUnreadMessages && baseMode(mode) != kModeMute && !quiet(r, mode);
}

inline bool countsAsUnread(const RoomInfo &r, int mode)
{
    if (!rawUnread(r) || baseMode(mode) == kModeMute)
        return false;
    if (!quiet(r, mode))
        return true;
    return r.markedUnread || mentionCount(r, mode) > 0
           || visibleUnreadCount(r, mode) > 0 || threadUnreadKind(r, mode) > 0;
}

inline bool listsAsUnread(const RoomInfo &r, int mode)
{
    return countsAsUnread(r, mode) || (r.markedUnread && baseMode(mode) == kModeMute);
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
