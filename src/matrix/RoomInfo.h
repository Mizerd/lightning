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
    // The room list's sort key. Write it ONLY through raiseActivity().
    QDateTime lastActivity;

    // Moves lastActivity forward, never backwards. Returns true when it
    // actually moved.
    //
    // Four different places used to assign this field, and a room jumped
    // whenever any two of them disagreed — the repeatedly reported "clicking
    // an older room moves it upwards in the room order, then it drops back
    // down to where it was". Events only ever get newer, so a DECREASE is
    // always an artefact: a summary that has not caught up, or a room whose
    // timeline was loaded (bringing state events into the SDK's "latest
    // event of any kind") and then unloaded again. The one genuine decrease
    // — redacting the newest message in a room — costs nothing but the room
    // keeping its place.
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
    // v0.7.x: whether `encrypted` is a KNOWN fact rather than a not-yet-
    // synced default. The Rust bridge sets it from the SDK's tri-state
    // EncryptionState; Mock/HTTP construct rooms with definitive state, so
    // the default is true and the Rust payload overrides it explicitly.
    // Anything security-relevant (draft persistence, server-search offers)
    // must fail closed while this is false.
    bool encryptionKnown = true;
    bool isSpace = false;
    // Element-parity favourites, backed by the Matrix `m.favourite` room
    // tag. Lightning keeps NO list of its own — a room favourited in
    // Element is favourite here and vice versa. False on a backend that
    // does not carry tags, which is honest: absent, not "not favourite by
    // decision".
    bool isFavourite = false;
    bool isDirect = false;
    QString directUserId;
    // Every target user this room's m.direct account-data mapping lists —
    // authoritative for "is this an unambiguous 1:1 DM". A group DM (or a
    // stale/ambiguous m.direct mapping) lists more than one and must not
    // get an arbitrary member's avatar. Populated alongside directUserId;
    // empty on backends that only ever provide the singular field.
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

    // v0.3: per-room member cache (display name / avatar). Matrix display
    // names can be room-specific, so this belongs to the room, not to a
    // global address book.
    QHash<QString, MemberInfo> members;

    // v0.3: users currently typing, sourced from the ephemeral m.typing
    // event in /sync.
    QStringList typingUserIds;

    // v0.4.1: the DIRECT children of this room as a Space — rooms AND
    // subspaces — in the order the Space's own `m.space.child` state declares
    // (order key first, room id as the tiebreak). Only populated when
    // isSpace == true. Sourced from m.space.child state events on every
    // backend: read from state (HTTP), hardcoded (Mock), or read from the
    // SDK's state store (Rust).
    //
    // DIRECT, not transitive, and that distinction is load-bearing: the
    // Rust backend used to fill this from its payload's `descendants` list
    // (the transitive closure), which is why anything reading it as the
    // direct list — the rail's subspace nesting, the Channels layout —
    // listed a subspace's rooms twice and showed no structure at all.
    // SpaceManager::rebuild() walks these to derive the transitive
    // membership it needs; nothing else should assume transitivity.
    //
    // Rooms may appear in multiple Spaces (Matrix allows it) — SpaceManager
    // assigns one deterministic PRIMARY parent per Space for display.
    QStringList childRoomIds;
    QStringList parentSpaceIds;

    // v0.7.x room upgrades. Both are ROOM IDS, never free text: the Rust
    // bridge fills them from the SDK's typed successor_room() /
    // predecessor_room(), which parse through ruma. Empty means "this room
    // has not been upgraded" / "this room replaces nothing".
    //
    // The tombstone's own body deliberately does not exist on this struct.
    // It is attacker-chosen text destined for a banner the user is invited
    // to click, so it never crosses the bridge at all.
    QString successorRoomId;
    QString predecessorRoomId;
};

inline bool operator==(const MemberInfo &a, const MemberInfo &b)
{
    return a.userId == b.userId && a.displayName == b.displayName
        && a.avatarMxcUrl == b.avatarMxcUrl;
}

// Full-field equality so RoomListModel::replaceRoom can skip the
// dataChanged storm for a room nothing actually changed on. Cheap in
// practice: unchanged copies flow from the same mirror objects, so the
// implicitly-shared members short-circuit on d-pointer identity.
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
        // Omitting this had exactly the failure the tombstone note below
        // describes: a room whose m.favourite tag just changed compared
        // equal to its pre-tag self, replaceRoom skipped the dataChanged,
        // and the row MOVED into the Favourites section while still
        // reporting its old category — so the section header and the rows
        // under it disagreed until something else about the room changed.
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
        // Omitting these would mean a room that JUST got tombstoned
        // compares equal to its pre-tombstone self, replaceRoom skips the
        // dataChanged, and the upgrade banner does not appear until
        // something unrelated about the room happens to change.
        && a.successorRoomId == b.successorRoomId
        && a.predecessorRoomId == b.predecessorRoomId;
}

inline bool operator!=(const RoomInfo &a, const RoomInfo &b)
{
    return !(a == b);
}

// The ONE identity key a room's fallback-avatar colour hashes from — the
// same policy as effectiveAvatarUrl's mxc choice: an unambiguous 1:1 DM is
// coloured as the PERSON (their MXID, matching every user-keyed surface —
// message rows, receipt chips, member list), everything else as the room.
// A group DM or ambiguous m.direct mapping (more than one target) must not
// adopt an arbitrary member's identity. Live report 2026-08-14: the same
// person rendered purple in one surface and green in another because DM
// rows hashed the room id while user rows hashed the MXID.
inline QString identityColorKey(const RoomInfo &r)
{
    if (r.isDirect && r.directUserIds.size() <= 1 && !r.directUserId.isEmpty())
        return r.directUserId;
    return r.id;
}
