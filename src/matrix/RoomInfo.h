#pragma once

#include "matrix/PreviewToken.h"

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
    matrix::preview::Kind previewKind = matrix::preview::Kind::Text;
    QString previewArg;

    void setPreviewText(const QString &text)
    {
        lastMessagePreview = text;
        previewKind = matrix::preview::Kind::Text;
        previewArg.clear();
    }
    void setPreview(matrix::preview::Kind kind, const QString &arg = QString())
    {
        if (kind == matrix::preview::Kind::Text) {
            setPreviewText(arg);
            return;
        }
        previewKind = kind;
        previewArg = arg;
        lastMessagePreview = matrix::preview::render(kind, arg);
    }
    bool retranslatePreview()
    {
        if (previewKind == matrix::preview::Kind::Text)
            return false;
        const QString now = matrix::preview::render(previewKind, previewArg);
        if (now == lastMessagePreview)
            return false;
        lastMessagePreview = now;
        return true;
    }
    QDateTime lastActivity;

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
    qint64 countsWrittenAtMs = 0;
    bool markedUnread = false;
    bool hasUnreadMessages = false;
    bool unreadThreadsOnly = false;
    bool mentionInThreads = false;
    bool encrypted = false;
    // Whether `encrypted` is known rather than a not-yet-synced default. The
    // Rust bridge sets it from the SDK's tri-state EncryptionState; Mock/HTTP
    // rooms are definitive, hence the default. Security-relevant decisions
    // (draft persistence, server-search offers) must fail closed while false.
    bool encryptionKnown = true;
    bool isSpace = false;
    bool isFavourite = false;
    bool lowPriority = false;
    bool isDirect = false;
    QString directUserId;
    QStringList directUserIds;
    int activeMemberCount = 0;
    int joinedMemberCount = 0;
    QString joinRule;
    QString canonicalAlias;
    QString inviterUserId;
    QString inviterDisplayName;
    QString roomType;
    Membership membership = Joined;
    bool invitePending = false;
    QString inviteError;
    QString spaceId;

    QString prevBatchToken;
    bool paginationExhausted = false;

    QHash<QString, MemberInfo> members;

    QStringList typingUserIds;

    QStringList childRoomIds;
    QStringList orderedChildIds;
    bool childOrderKnown = false;
    QStringList parentSpaceIds;

    QString successorRoomId;
    QString predecessorRoomId;
};

inline bool operator==(const MemberInfo &a, const MemberInfo &b)
{
    return a.userId == b.userId && a.displayName == b.displayName
        && a.avatarMxcUrl == b.avatarMxcUrl;
}

inline bool operator==(const RoomInfo &a, const RoomInfo &b)
{
    return a.id == b.id && a.name == b.name && a.topic == b.topic
        && a.avatarUrl == b.avatarUrl
        && a.lastMessagePreview == b.lastMessagePreview
        && a.previewKind == b.previewKind && a.previewArg == b.previewArg
        && a.lastActivity == b.lastActivity
        && a.unreadCount == b.unreadCount
        && a.highlightCount == b.highlightCount
        && a.markedUnread == b.markedUnread
        && a.hasUnreadMessages == b.hasUnreadMessages
        && a.unreadThreadsOnly == b.unreadThreadsOnly
        && a.mentionInThreads == b.mentionInThreads
        && a.encrypted == b.encrypted
        && a.encryptionKnown == b.encryptionKnown
        && a.isSpace == b.isSpace
        // Without this a changed m.favourite tag compares equal, replaceRoom
        // skips dataChanged, and the row moves section while reporting its old
        // category.
        && a.isFavourite == b.isFavourite
        && a.lowPriority == b.lowPriority
        && a.isDirect == b.isDirect && a.directUserId == b.directUserId
        && a.directUserIds == b.directUserIds
        && a.activeMemberCount == b.activeMemberCount
        && a.joinedMemberCount == b.joinedMemberCount
        && a.joinRule == b.joinRule
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
        && a.orderedChildIds == b.orderedChildIds
        && a.childOrderKnown == b.childOrderKnown
        && a.parentSpaceIds == b.parentSpaceIds
        && a.successorRoomId == b.successorRoomId
        && a.predecessorRoomId == b.predecessorRoomId;
}

inline bool operator!=(const RoomInfo &a, const RoomInfo &b)
{
    return !(a == b);
}

inline QString identityColorKey(const RoomInfo &r)
{
    if (r.isDirect && r.directUserIds.size() <= 1 && !r.directUserId.isEmpty())
        return r.directUserId;
    return r.id;
}
