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
    QString id;
    QString name;
    QString topic;
    QString avatarUrl;
    QString lastMessagePreview;
    QDateTime lastActivity;
    int unreadCount = 0;
    bool encrypted = false;
    bool isSpace = false;
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
};
