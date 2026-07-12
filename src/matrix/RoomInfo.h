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
    QDateTime lastActivity;
    int unreadCount = 0;
    int highlightCount = 0;
    bool markedUnread = false;
    bool hasUnreadMessages = false;
    bool encrypted = false;
    bool isSpace = false;
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

    // v0.4.1: rooms that this room contains as a Space. Only populated when
    // isSpace == true. Sourced from m.space.child state events (HTTP) or
    // hardcoded (Mock). Rooms may appear in multiple spaces (Matrix allows
    // it) — RoomInfo::spaceId stays as a "primary parent" hint.
    QStringList childRoomIds;
    QStringList parentSpaceIds;
};
