#pragma once

#include "matrix/RoomInfo.h"

#include <QList>

// The one rule for "does this room count as unread", shared by the Spaces rail
// and the tray badge (GitHub #18: the tray ignored the notification mode, so a
// muted room painted a red dot the rail correctly did not).
namespace roomunread {

// SettingsManager::roomNotificationMode(): 0 is All messages, 2 is mute.
constexpr int kModeAllMessages = 0;
constexpr int kModeMentions = 1;
constexpr int kModeMute = 2;
// roomUnreadMode() only: kModeMentions that this device cannot vouch for.
constexpr int kModeMentionsUnverified = 4;

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

// The room list's Unreads FILTER: countsAsUnread, plus a muted room the user
// explicitly marked unread. The row draws a grey dot for that room, so the
// filter must list it; a count, a mention, the rail and the tray stay silent
// (they use countsAsUnread, which this does NOT replace).
inline bool listsAsUnread(const RoomInfo &r, int mode)
{
    return countsAsUnread(r, mode) || (r.markedUnread && mode == kModeMute);
}

/// The mention count a badge may show: zero for a muted room.
inline int mentionCount(const RoomInfo &r, int mode)
{
    return mode == kModeMute ? 0 : r.highlightCount;
}

/// The plain-message count a badge may show. A muted room shows none. A
/// "mentions & keywords" room keeps its count (a keyword hit raises it without
/// a highlight, as Element's grey count) EXCEPT where the number cannot be
/// trusted: the mode is held only on this device or was never accepted by the
/// server (kModeMentionsUnverified), or encryption is not yet known (the
/// backend then takes the higher of two counts, one of which may not have
/// applied the room's push rule). Such a room is still unread (a dot) and its
/// mentions still count; inventing a number is what CLAUDE.md section 8 forbids.
/// ONE function for every surface that shows a room's count.
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

/// Why a room is still unread when only a thread holds it: 0 = not that,
/// 1 = unread in a thread, 2 = mention in a thread. A muted room is silent,
/// so it reports 0.
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

// The tray's walk. `modeOf(roomId)` is read only for a room that is raw-unread;
// `onRoom(room, countsAsUnread)` sees every joined room, a Space room included
// (as not unread: no other surface counts a Space room and the user has no row
// to open to clear it, but a notification already raised for one is withdrawn).
// A pending invitation is not an unread message and gets no `onRoom` call (it
// has its own notification, withdrawn when answered), but it needs action, so
// it adds one to the total and lights the badge, as Element's does.
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
        // Ask for the mode once, and only for a room with something unread:
        // the lookup is a settings read per room.
        const bool raw = rawUnread(room);
        const int mode = raw ? modeOf(room.id) : 0;
        const bool unread = raw && countsAsUnread(room, mode);
        if (unread) {
            out.any = true;
            // The count the mode allows; when it clamps the plain number to
            // nothing the mentions are still what the tray reports.
            const int shown = visibleUnreadCount(room, mode);
            out.total += shown > 0 ? shown : mentionCount(room, mode);
        }
        onRoom(room, unread);
    }
    return out;
}

} // namespace roomunread
