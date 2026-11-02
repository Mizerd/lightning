#pragma once

#include "matrix/RoomInfo.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>
#include <deque>
#include <utility>

namespace notices {

inline constexpr int kPreviewPrivate = 2;

inline bool isPrivate(int preview) { return preview >= kPreviewPrivate; }

inline bool showsRoomAvatar(int preview) { return !isPrivate(preview); }

/// An invitation's picture is chosen by a stranger: it also needs the
/// account's MSC4278 invite_avatars to be known and on
/// (AccountPrefsController::inviteAvatarsAllowed, fail-closed).
inline bool showsInviteAvatar(int preview, bool inviteAvatarsAllowed)
{
    return showsRoomAvatar(preview) && inviteAvatarsAllowed;
}

inline QString incomingCallTitle(bool video)
{
    return video ? QCoreApplication::translate("AppController",
                                               "Incoming video call")
                 : QCoreApplication::translate("AppController",
                                               "Incoming call");
}

/// `caller`: a display name or localpart, never a full MXID. A direct room's
/// name is the peer's own name, so it is not repeated ("Alice is calling in
/// Alice").
inline QString incomingCallBody(int preview, const QString &caller,
                                const QString &roomName, bool direct,
                                bool video)
{
    if (isPrivate(preview)) {
        return video ? QCoreApplication::translate("AppController",
                                                   "Incoming video call")
                     : QCoreApplication::translate("AppController",
                                                   "Incoming voice call");
    }
    if (direct || roomName.isEmpty())
        return QCoreApplication::translate("AppController", "%1 is calling")
            .arg(caller);
    return QCoreApplication::translate("AppController", "%1 is calling in %2")
        .arg(caller, roomName);
}

inline QString missedCallBody(int preview, const QString &caller,
                              const QString &roomName, bool direct, bool video)
{
    if (isPrivate(preview) || caller.isEmpty()) {
        if (!isPrivate(preview) && !roomName.isEmpty() && !direct) {
            return video
                ? QCoreApplication::translate("AppController",
                                              "You missed a video call in %1")
                      .arg(roomName)
                : QCoreApplication::translate("AppController",
                                              "You missed a voice call in %1")
                      .arg(roomName);
        }
        return video ? QCoreApplication::translate("AppController",
                                                   "You missed a video call")
                     : QCoreApplication::translate("AppController",
                                                   "You missed a voice call");
    }
    if (direct || roomName.isEmpty()) {
        return video
            ? QCoreApplication::translate("AppController",
                                          "Missed video call from %1")
                  .arg(caller)
            : QCoreApplication::translate("AppController",
                                          "Missed voice call from %1")
                  .arg(caller);
    }
    return video
        ? QCoreApplication::translate("AppController",
                                      "Missed video call from %1 in %2")
              .arg(caller, roomName)
        : QCoreApplication::translate("AppController",
                                      "Missed voice call from %1 in %2")
              .arg(caller, roomName);
}

struct RingPlan {
    bool ring = false;
    bool ownRinger = false;
    bool cardSound = false;
};

inline RingPlan ringPlan(bool ringForCalls, bool silenced, bool ringerAvailable,
                         bool platformLoopsCallSound,
                         bool desktopDoNotDisturb = false)
{
    RingPlan plan;
    plan.ring = ringForCalls && !silenced && !desktopDoNotDisturb;
    if (!plan.ring)
        return plan;
    if (platformLoopsCallSound) {
        plan.ownRinger = ringerAvailable;
        plan.cardSound = !ringerAvailable;
    } else {
        plan.ownRinger = true;
        plan.cardSound = !ringerAvailable;
    }
    return plan;
}

class RingAnnounceLimiter
{
public:
    static constexpr qint64 kWindowMs = 30000;
    static constexpr int kMaxNewCallsPerWindow = 3;
    static constexpr int kMaxSenders = 64;

    enum class Verdict { Announce, Refresh, Refuse };

    Verdict admit(const QString &senderId, const QString &callId, qint64 nowMs)
    {
        auto &calls = m_bySender[senderId];
        while (!calls.empty() && nowMs - calls.front().first >= kWindowMs)
            calls.pop_front();
        for (const auto &entry : calls) {
            if (entry.second == callId)
                return Verdict::Refresh;
        }
        if (int(calls.size()) >= kMaxNewCallsPerWindow)
            return Verdict::Refuse;
        calls.emplace_back(nowMs, callId);
        prune(nowMs, senderId);
        return Verdict::Announce;
    }

    void clear() { m_bySender.clear(); }
    int senderCountForTest() const { return int(m_bySender.size()); }

private:
    void prune(qint64 nowMs, const QString &keep)
    {
        if (m_bySender.size() <= kMaxSenders)
            return;
        for (auto it = m_bySender.begin(); it != m_bySender.end();) {
            auto &calls = it.value();
            while (!calls.empty() && nowMs - calls.front().first >= kWindowMs)
                calls.pop_front();
            if (calls.empty() && it.key() != keep)
                it = m_bySender.erase(it);
            else
                ++it;
        }
        while (m_bySender.size() > kMaxSenders) {
            auto it = m_bySender.begin();
            if (it.key() == keep)
                ++it;
            m_bySender.erase(it);
        }
    }

    QHash<QString, std::deque<std::pair<qint64, QString>>> m_bySender;
};

inline bool inviteNoticeAllowed(bool notificationsEnabled, bool rulesKnown,
                                bool masterMute, int inviteRuleLevel)
{
    if (!notificationsEnabled)
        return false;
    if (rulesKnown && (masterMute || inviteRuleLevel == 0))
        return false;
    return true;
}

inline bool inviteSound(int soundMode, bool rulesKnown, int inviteRuleLevel)
{
    if (soundMode <= 0)
        return false;
    if (rulesKnown && inviteRuleLevel >= 0)
        return inviteRuleLevel >= 2;
    return true;
}

inline QString inviteBody(int preview, const QString &inviter,
                          const QString &roomName, bool direct)
{
    if (isPrivate(preview))
        return QCoreApplication::translate("AppController",
                                           "New Matrix notification");
    if (!inviter.isEmpty()) {
        if (direct)
            return QCoreApplication::translate("AppController",
                                               "%1 wants to chat with you")
                .arg(inviter);
        if (roomName.isEmpty())
            return QCoreApplication::translate("AppController",
                                               "%1 invited you to a room")
                .arg(inviter);
        return QCoreApplication::translate("AppController",
                                           "%1 invited you to %2")
            .arg(inviter, roomName);
    }
    if (roomName.isEmpty())
        return QCoreApplication::translate("AppController",
                                           "You were invited to a room");
    return QCoreApplication::translate("AppController", "You were invited to %1")
        .arg(roomName);
}

inline bool verificationNoticeAllowed(bool notificationsEnabled, bool rulesKnown,
                                      bool masterMute)
{
    return notificationsEnabled && !(rulesKnown && masterMute);
}

/// `otherName`: a display name or localpart, never the full MXID (the body is
/// written to the desktop's notification history in plaintext).
inline QString verificationBody(int preview, bool isSelf,
                                const QString &otherName)
{
    if (isPrivate(preview))
        return QCoreApplication::translate(
            "AppController",
            "A session verification request is waiting. Open Lightning to "
            "review it.");
    if (isSelf || otherName.isEmpty())
        return QCoreApplication::translate(
            "AppController",
            "Another of your sessions wants to verify. Open Lightning to "
            "review it.");
    return QCoreApplication::translate(
               "AppController",
               "%1 wants to verify a session. Open Lightning to review it.")
        .arg(otherName);
}

inline QString verificationNoticeKey(const QString &flowId)
{
    return QStringLiteral("verification:") + flowId;
}

inline bool callStartedNoticeAllowed(bool notificationsEnabled,
                                     bool initialSyncDone, bool roomMuted,
                                     bool roomVisible, bool masterMute,
                                     bool senderIgnored, bool ownSender)
{
    return notificationsEnabled && initialSyncDone && !roomMuted
        && !roomVisible && !masterMute && !senderIgnored && !ownSender;
}

inline QString callStartedBody(int preview, const QString &caller,
                               const QString &roomName)
{
    if (isPrivate(preview) || caller.isEmpty())
        return QCoreApplication::translate("AppController",
                                           "New Matrix notification");
    if (roomName.isEmpty())
        return QCoreApplication::translate("AppController",
                                           "%1 started a call")
            .arg(caller);
    return QCoreApplication::translate("AppController",
                                       "%1 started a call in %2")
        .arg(caller, roomName);
}

inline QString callStartedKey(const QString &roomId)
{
    return QStringLiteral("call-started:") + roomId;
}

inline bool highlightSummaryAllowed(bool notificationsEnabled,
                                    bool initialSyncDone, bool roomMuted,
                                    bool roomVisibleAtLatest, bool masterMute,
                                    int count)
{
    return notificationsEnabled && initialSyncDone && !roomMuted
        && !roomVisibleAtLatest && !masterMute && count > 0;
}

inline QString highlightSummaryBody(int preview, int count,
                                    const QString &roomName)
{
    if (isPrivate(preview) || roomName.isEmpty())
        return QCoreApplication::translate("AppController",
                                           "New Matrix notification");
    return QCoreApplication::translate("AppController",
                                       "%n new mention(s) in %1", nullptr,
                                       count)
        .arg(roomName);
}

inline QString highlightSummaryKey(const QString &roomId)
{
    return QStringLiteral("highlights:") + roomId;
}

class RecentIds
{
public:
    explicit RecentIds(int capacity = 2048) : m_capacity(std::max(1, capacity))
    {}

    bool insert(const QString &id)
    {
        if (m_set.contains(id))
            return false;
        m_set.insert(id);
        m_order.push_back(id);
        while (int(m_order.size()) > m_capacity) {
            m_set.remove(m_order.front());
            m_order.pop_front();
        }
        return true;
    }
    bool contains(const QString &id) const { return m_set.contains(id); }
    int size() const { return int(m_set.size()); }
    void clear()
    {
        m_set.clear();
        m_order.clear();
    }

private:
    int m_capacity;
    QSet<QString> m_set;
    std::deque<QString> m_order;
};

inline bool mainWindowCountsAsActive(bool windowActive, bool minimized,
                                     bool hidden)
{
    return windowActive && !minimized && !hidden;
}

inline bool windowAlertAllowed(const QString &platformName)
{
    return !platformName.startsWith(QLatin1String("wayland"),
                                    Qt::CaseInsensitive);
}

inline bool clickNeedsAlertActivation(const QString &platformName,
                                      bool daemonTokenApplied)
{
    return !daemonTokenApplied
        && platformName.startsWith(QLatin1String("wayland"),
                                   Qt::CaseInsensitive);
}

inline bool roomWantsMarkRead(const RoomInfo &room)
{
    if (room.membership != RoomInfo::Joined || room.isSpace || room.markedUnread)
        return false;
    return room.unreadCount > 0 || room.highlightCount > 0
        || room.hasUnreadMessages;
}

inline QString trayAccountLabel(const QString &userId,
                                const QString &displayName)
{
    if (userId.isEmpty())
        return {};
    const QString name = displayName.trimmed();
    if (!name.isEmpty())
        return name;
    QString local = userId.startsWith(QLatin1Char('@')) ? userId.mid(1) : userId;
    const qsizetype colon = local.indexOf(QLatin1Char(':'));
    if (colon > 0)
        local = local.left(colon);
    return local.isEmpty() ? userId : local;
}

inline int accountDefaultLevel(bool rulesKnown, bool isDirect, bool dmAll,
                               bool roomsAll)
{
    if (!rulesKnown)
        return -1;
    return (isDirect ? dmAll : roomsAll) ? 0 : 1;
}

inline int activitySeedRetryDelayMs(int attempt)
{
    constexpr int kMaxAttempts = 6;
    if (attempt < 0 || attempt >= kMaxAttempts)
        return -1;
    return std::min(5000 << attempt, 300000);
}

}
