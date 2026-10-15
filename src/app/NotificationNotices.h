#pragma once

// Wording and gating for Lightning's own non-message notices (the call card,
// the missed-call notice, invitations, verification requests, the summary for
// mentions the sync never delivered) and the small decisions AppController
// makes around them. Pure, so every rule is tested without a bus, a window or
// a session (tests/NotificationNoticesTest.cpp).
//
// Strings are translated in the "AppController" context, where they lived
// before, so existing translations keep applying.

#include "matrix/RoomInfo.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>
#include <deque>
#include <utility>

namespace notices {

/// SettingsManager's preview values: 0 sender and message, 1 sender only,
/// 2 private. Callers resolve the level for the room first
/// (SettingsManager::effectiveNotificationPreview, the stricter level while a
/// room's encryption is unknown or the room is not listed yet).
inline constexpr int kPreviewPrivate = 2;

inline bool isPrivate(int preview) { return preview >= kPreviewPrivate; }

/// Private withholds the room as well, and its picture would name it.
inline bool showsRoomAvatar(int preview) { return !isPrivate(preview); }

// ── Calls ────────────────────────────────────────────────────────────────

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

/// What an announced ring does about sound.
struct RingPlan {
    /// The call rings at all (the user's ring switch, not silenced).
    bool ring = false;
    /// Lightning's own looping ringer starts.
    bool ownRinger = false;
    /// The desktop card asks the platform for its call sound.
    bool cardSound = false;
};

/// The ring follows "Ring for incoming calls" alone: the message
/// notification sound setting is about chat chimes and must not silence
/// calls. `platformLoopsCallSound`: the delivery path re-posts the card with
/// the desktop's call sound until the call ends (freedesktop notifications).
/// Without it (the tray balloon on Windows and macOS) the desktop gives one
/// cue at most, so Lightning's own ringer is always started there, and the
/// card's single cue is used only while that ringer is not ready.
inline RingPlan ringPlan(bool ringForCalls, bool silenced, bool ringerAvailable,
                         bool platformLoopsCallSound)
{
    RingPlan plan;
    plan.ring = ringForCalls && !silenced;
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

/// Bounds critical-urgency call announcements per sender without dropping a
/// real redial. A room member must not be able to pump call cards with fresh
/// call ids, but a caller who gives up and tries again a few seconds later is
/// the most ordinary thing a call does: the first `kMaxNewCallsPerWindow`
/// distinct calls from one sender inside `kWindowMs` are announced, a call
/// already announced is a refresh (never counted), and only the burst beyond
/// the cap is refused.
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
        // Still over: the window is full of distinct senders. Any entry may
        // go; the bound is a heuristic, not a ledger.
        while (m_bySender.size() > kMaxSenders) {
            auto it = m_bySender.begin();
            if (it.key() == keep)
                ++it;
            m_bySender.erase(it);
        }
    }

    QHash<QString, std::deque<std::pair<qint64, QString>>> m_bySender;
};

// ── Invitations ──────────────────────────────────────────────────────────

/// Push-rule levels as PushRulesController reports them: -1 the server has no
/// such rule, 0 off, 1 notify, 2 notify with sound.
///
/// The account's rules are what every other client follows: master mute
/// ("Mute all notifications") and "Invitations to you: Off" silence the
/// invitation card too. Unknown rules keep the local behaviour.
inline bool inviteNoticeAllowed(bool notificationsEnabled, bool rulesKnown,
                                bool masterMute, int inviteRuleLevel)
{
    if (!notificationsEnabled)
        return false;
    if (rulesKnown && (masterMute || inviteRuleLevel == 0))
        return false;
    return true;
}

/// An invitation is a conversation starting (a new DM arrives as one), so it
/// sounds under both "Mentions and direct messages" and "All". When the rules
/// are known, the invite rule's own sound tweak decides, as on other clients.
inline bool inviteSound(int soundMode, bool rulesKnown, int inviteRuleLevel)
{
    if (soundMode <= 0)
        return false;
    if (rulesKnown && inviteRuleLevel >= 0)
        return inviteRuleLevel >= 2;
    return true;
}

/// `inviter`: a display name or localpart; empty when unknown.
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

// ── Verification ─────────────────────────────────────────────────────────

/// Master mute silences this notice too; it is a notification like any other.
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

/// One key per flow, so the card can be withdrawn when the flow leaves
/// "requested" (accepted, cancelled or finished, here or on another device).
inline QString verificationNoticeKey(const QString &flowId)
{
    return QStringLiteral("verification:") + flowId;
}

// ── A call announced without ringing us ──────────────────────────────────
//
// A MatrixRTC `notification` (a group call starting), or a `ring` that
// addresses neither this user nor the room (MSC4075). CallController never
// rings for it; Element shows a quiet notice, and so does this.

inline bool callStartedNoticeAllowed(bool notificationsEnabled,
                                     bool initialSyncDone, bool roomMuted,
                                     bool roomVisible, bool masterMute,
                                     bool senderIgnored, bool ownSender)
{
    return notificationsEnabled && initialSyncDone && !roomMuted
        && !roomVisible && !masterMute && !senderIgnored && !ownSender;
}

/// `caller`: a display name or localpart, never a full MXID.
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

/// One card per room: a newer announcement replaces the older one.
inline QString callStartedKey(const QString &roomId)
{
    return QStringLiteral("call-started:") + roomId;
}

// ── Mentions the sync never delivered ────────────────────────────────────

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

/// One card per room: a newer summary replaces the older one.
inline QString highlightSummaryKey(const QString &roomId)
{
    return QStringLiteral("highlights:") + roomId;
}

// ── Small decisions ──────────────────────────────────────────────────────

/// Event ids already handed to the notification and Activity paths. Two
/// producers can deliver one event (a thread copy and the room's sync, an
/// SDK insert and a later append), and a sliding-sync reset re-delivers. A
/// set wiped wholesale at its cap forgets everything at once, so a reply
/// notified hours ago could notify again; this one forgets the oldest only.
class RecentIds
{
public:
    explicit RecentIds(int capacity = 2048) : m_capacity(std::max(1, capacity))
    {}

    /// False when the id was already present (nothing changes then).
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

/// The read-receipt coordinator's "the user can see the window": the MAIN
/// window focused and on screen. Application activation is not enough: the
/// picture-in-picture and full-screen call windows are top-level windows of
/// the same application.
inline bool mainWindowCountsAsActive(bool windowActive, bool minimized,
                                     bool hidden)
{
    return windowActive && !minimized && !hidden;
}

/// "Mark as read" on a Space, with Home's filter (RoomListModel::
/// markAllRoomsRead): never a room the user marked unread to come back to,
/// never an invite, a Space, or a room with nothing unread (a receipt there
/// is a request for nothing).
inline bool roomWantsMarkRead(const RoomInfo &room)
{
    if (room.membership != RoomInfo::Joined || room.isSpace || room.markedUnread)
        return false;
    return room.unreadCount > 0 || room.highlightCount > 0
        || room.hasUnreadMessages;
}

/// The tray tooltip names the account by display name, else its localpart;
/// empty while signed out.
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

/// The troubleshooter's account default for one room's kind: -1 unknown,
/// 0 all messages, 1 mentions and keywords.
inline int accountDefaultLevel(bool rulesKnown, bool isDirect, bool dmAll,
                               bool roomsAll)
{
    if (!rulesKnown)
        return -1;
    return (isDirect ? dmAll : roomsAll) ? 0 : 1;
}

/// The Activity seed after a failed attempt: 5 s, doubling, at most 5 min;
/// -1 once `attempt` (0-based) reaches the cap. A later Syncing edge still
/// tries again on its own.
inline int activitySeedRetryDelayMs(int attempt)
{
    constexpr int kMaxAttempts = 6;
    if (attempt < 0 || attempt >= kMaxAttempts)
        return -1;
    return std::min(5000 << attempt, 300000);
}

} // namespace notices
