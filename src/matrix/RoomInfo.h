#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>

struct MemberInfo {
    QString userId;
    QString displayName;
    QString avatarMxcUrl;
};

struct RoomInfo {
    enum Membership {
        Joined,
        Invited,
        Knocked,
        Left,
    };

    QString id;
    QString name;
    QString topic;
    QString avatarUrl;
    QString lastMessagePreview;
    // The room list's sort key. Write it only through raiseActivity().
    QDateTime lastActivity;

    // Moves lastActivity forward, never backwards; returns true when it moved.
    // Events only get newer, so a decrease is an artefact (a lagging summary,
    // or state events from a timeline that was loaded and unloaded) and would
    // make the room jump. The one genuine decrease, redacting the newest
    // message, just leaves the room in place.
    bool raiseActivity(const QDateTime &when)
    {
        if (!when.isValid())
            return false;
        if (lastActivity.isValid() && when <= lastActivity)
            return false;
        lastActivity = when;
        return true;
    }
    int unreadCount = 0;
    int highlightCount = 0;
    bool markedUnread = false;
    bool hasUnreadMessages = false;
    bool encrypted = false;
    // Whether `encrypted` is known rather than a not-yet-synced default. The
    // Rust bridge sets it from the SDK's tri-state EncryptionState; Mock/HTTP
    // rooms are definitive, hence the default. Security-relevant decisions
    // (draft persistence, server-search offers) must fail closed while false.
    bool encryptionKnown = true;
    bool isSpace = false;
    // Favourites backed by the Matrix `m.favourite` room tag; Lightning keeps
    // no list of its own. False on backends without tags.
    bool isFavourite = false;
    bool isDirect = false;
    QString directUserId;
    // Every target user this room's m.direct mapping lists; authoritative for
    // "is this an unambiguous 1:1 DM". A group DM or ambiguous mapping lists
    // more than one and must not take an arbitrary member's avatar. Empty on
    // backends that only provide directUserId.
    QStringList directUserIds;
    QString canonicalAlias;
    QString inviterUserId;
    QString inviterDisplayName;
    QString roomType;
    Membership membership = Joined;
    bool invitePending = false;
    QString inviteError;
    QString spaceId;

    // Pagination token from the most recent /sync for backfill via
    // GET /rooms/{id}/messages?dir=b. Empty means "unknown / never seen".
    QString prevBatchToken;
    bool paginationExhausted = false;

    // Per-room member cache: display names can be room-specific.
    QHash<QString, MemberInfo> members;

    // Users currently typing, from the ephemeral m.typing event.
    QStringList typingUserIds;

    // The direct children of this Space (rooms and subspaces) in the Space's
    // own m.space.child order (order key, then room id). Only set when isSpace.
    // Direct, not transitive: the rail's nesting and the Channels layout depend
    // on it. SpaceManager::rebuild() derives transitive membership; nothing
    // else should assume it. A room may be in several Spaces; SpaceManager
    // picks one primary parent for display.
    QStringList childRoomIds;
    QStringList parentSpaceIds;

    // Room upgrades. Both are room ids from the SDK's typed successor_room() /
    // predecessor_room(), never free text; empty means not upgraded / replaces
    // nothing. The tombstone body is deliberately absent: it is attacker-chosen
    // text for a banner the user is invited to click, so it never crosses the
    // bridge.
    QString successorRoomId;
    QString predecessorRoomId;
};

inline bool operator==(const MemberInfo &a, const MemberInfo &b)
{
    return a.userId == b.userId && a.displayName == b.displayName
        && a.avatarMxcUrl == b.avatarMxcUrl;
}

// Full-field equality so RoomListModel::replaceRoom can skip dataChanged for
// an unchanged room. Cheap: unchanged copies share d-pointers.
inline bool operator==(const RoomInfo &a, const RoomInfo &b)
{
    return a.id == b.id && a.name == b.name && a.topic == b.topic
        && a.avatarUrl == b.avatarUrl
        && a.lastMessagePreview == b.lastMessagePreview
        && a.lastActivity == b.lastActivity
        && a.unreadCount == b.unreadCount
        && a.highlightCount == b.highlightCount
        && a.markedUnread == b.markedUnread
        && a.hasUnreadMessages == b.hasUnreadMessages
        && a.encrypted == b.encrypted
        && a.encryptionKnown == b.encryptionKnown
        && a.isSpace == b.isSpace
        // Without this a changed m.favourite tag compares equal, replaceRoom
        // skips dataChanged, and the row moves section while reporting its old
        // category.
        && a.isFavourite == b.isFavourite
        && a.isDirect == b.isDirect && a.directUserId == b.directUserId
        && a.directUserIds == b.directUserIds
        && a.canonicalAlias == b.canonicalAlias
        && a.inviterUserId == b.inviterUserId
        && a.inviterDisplayName == b.inviterDisplayName
        && a.roomType == b.roomType && a.membership == b.membership
        && a.invitePending == b.invitePending
        && a.inviteError == b.inviteError && a.spaceId == b.spaceId
        && a.prevBatchToken == b.prevBatchToken
        && a.paginationExhausted == b.paginationExhausted
        && a.members == b.members && a.typingUserIds == b.typingUserIds
        && a.childRoomIds == b.childRoomIds
        && a.parentSpaceIds == b.parentSpaceIds
        // Without these a newly tombstoned room compares equal and the upgrade
        // banner does not appear until something else changes.
        && a.successorRoomId == b.successorRoomId
        && a.predecessorRoomId == b.predecessorRoomId;
}

inline bool operator!=(const RoomInfo &a, const RoomInfo &b)
{
    return !(a == b);
}

// The identity key a room's fallback-avatar colour hashes from, matching
// effectiveAvatarUrl: an unambiguous 1:1 DM is coloured as the person (their
// MXID, like every user-keyed surface), everything else as the room. A group
// DM or ambiguous m.direct mapping must not adopt a member's identity.
inline QString identityColorKey(const RoomInfo &r)
{
    if (r.isDirect && r.directUserIds.size() <= 1 && !r.directUserId.isEmpty())
        return r.directUserId;
    return r.id;
}
