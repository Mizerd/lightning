#pragma once

#include <QHash>
#include <QPointer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QTimer>

#include <functional>

struct TimelineEvent;
class TrayIcon;

// v0.6.0 checkpoint 11: native desktop notifications.
//
// The DECISION (should this event notify, with which title/body under the
// active privacy mode) is pure and unit-tested; DELIVERY uses the
// freedesktop org.freedesktop.Notifications DBus service (the native path
// on Linux — Lightning's first-class platform). Message bodies are shown
// only in the "Sender and message" privacy mode, are never logged, and are
// never persisted; undecryptable events always render a generic encrypted
// placeholder. Click payloads carry only room/event/thread identity —
// never tokens.
class NotificationManager : public QObject
{
    Q_OBJECT
public:
    // SettingsManager's notificationPreview values.
    enum PreviewMode { SenderAndMessage = 0, SenderOnly = 1, Private = 2 };
    Q_ENUM(PreviewMode)
    // Local per-room notification mode (explicitly local-only, not server
    // push rules).
    // FollowDefault means the user removed this room's override, so the
    // ACCOUNT's push rules decide. The server knows that default and applies
    // it to real pushes; this device does not persist it, so locally the row
    // falls through to "notify" — identical to how a never-configured room
    // has always behaved here. Deliberately not fabricating a resolved
    // default: shouldNotify() only special-cases Muted and MentionsOnly.
    enum RoomMode {
        AllMessages = 0, MentionsOnly = 1, Muted = 2, FollowDefault = 3
    };
    Q_ENUM(RoomMode)
    // SettingsManager's notificationSound values.
    enum SoundMode { SoundOff = 0, SoundMentionsAndDirect = 1, SoundAll = 2 };
    Q_ENUM(SoundMode)

    // Everything the decision needs, supplied by the app layer.
    struct Context {
        QString selfUserId;
        QString roomName;
        bool roomIsDirect = false;
        RoomMode roomMode = AllMessages;
        PreviewMode previewMode = SenderOnly;
        bool notificationsEnabled = true;
        // Active-at-latest suppression: the room is on screen, focused, and
        // following the newest message.
        bool roomVisibleAtLatest = false;
        // v0.7.3: the room the user has just opened, still laying out its
        // first screen. Opening a room subscribes it in sliding sync, so the
        // server then delivers that room's recent timeline as ordinary live
        // appends — and roomVisibleAtLatest is false throughout, because it
        // requires a settled view. Without this the room you are looking at
        // raises a notification for every message it just loaded.
        bool roomHydrating = false;
        // v0.6.1: false while the client is still applying its initial sync
        // backlog. Events that arrive before the first sync completes are
        // pre-existing history, not fresh activity, and must never raise a
        // native notification — otherwise every backlog message re-notifies
        // on each launch (the 0.6.0 cold-start storm). Defaults true so the
        // Mock backend and pure-policy tests notify unless told otherwise.
        bool initialSyncComplete = true;
        SoundMode soundMode = SoundMentionsAndDirect;
        // v0.7.x: the sender is on the account's m.ignored_user_list. The
        // server stops SENDING an ignored user's events, so this is only
        // the local belt-and-braces for the race window between the ignore
        // write and the server applying it — but without it that window
        // notifies. Supplied by the app layer to keep decide() pure.
        bool senderIsIgnored = false;
        // Effective room avatar: explicit room avatar, or the unambiguous
        // other user's profile avatar for a strict 1:1 DM.
        QString avatarMxc;
        // Identity key behind the initials disc drawn when there is no
        // avatar to fetch — the same key the room list uses, so the two
        // agree on colour. Empty falls back to the room name.
        QString avatarColorKey;
        // The active SettingsManager::Theme. The identity discs are derived
        // from the theme's accent now, so a notification painted without it
        // would show a different colour from the window it came from.
        int themeId = 0;
    };
    struct Decision {
        bool notify = false;
        QString title;
        QString body;
        // Whether this notification should also play a sound. Always false
        // when notify is false, so muted / active-room / mentions-only
        // suppression suppresses the sound too.
        bool playSound = false;
    };

    explicit NotificationManager(QObject *parent = nullptr);
    // App-layer adapter over MediaBridge. `image(mxc, request)` returns a
    // cached image and optionally starts the normal avatar fetch; `failed`
    // reports a current failure mark. Call avatarCacheChanged when either
    // state changes. Keeping the concrete media bridge out of this class
    // preserves the pure-policy unit-test boundary.
    void setAvatarProvider(
        std::function<QImage(const QString &, bool request)> image,
        std::function<bool(const QString &)> failed);
    void avatarCacheChanged();
    /// The account notifications are currently being raised FOR. Recorded in
    /// every click payload so a reply or mark-as-read taken after an account
    /// switch can be refused rather than sent from the wrong identity.
    void setAccountUserId(const QString &userId) { m_accountUserId = userId; }
    QString accountUserId() const { return m_accountUserId; }
    /// Whether the notification daemon offers an inline reply box. False
    /// until the first delivery has queried GetCapabilities, and false on a
    /// build without DBus.
    bool inlineReplySupported() const { return m_inlineReply; }

    // Pure policy — no I/O, no logging. Exposed for tests.
    static Decision decide(const TimelineEvent &event, const Context &context);

    // v0.6.1: pure policy for pending-invite notifications. An invite raises a
    // native notification only once the initial sync backlog has been applied
    // (so a restart does not re-announce every existing invite), only for an
    // invite not already announced this session, and only when notifications
    // are enabled. Exposed for tests.
    static bool shouldNotifyInvite(bool initialSyncComplete, bool alreadyKnown,
                                   bool notificationsEnabled);

    // Decide and (when positive) deliver natively. The click payload is
    // {roomId, eventId, threadRootId} only.
    void processEvent(const TimelineEvent &event, const Context &context);

    // Generic non-message notifications (invites, verification requests).
    // The body must already be safe — callers never pass message content.
    void showGeneric(const QString &title, const QString &safeBody,
                     const QString &roomId = QString(),
                     const QString &avatarMxc = QString());

    // 2026-08-18 round 2: incoming voice call. One notification with a
    // Decline action; while ringing it is re-delivered every few seconds
    // (replacing itself) so the themed call sound repeats — the closest
    // honest "ring" the freedesktop notification API offers, since
    // Lightning bundles no audio and plays none itself. `sound` false
    // shows a silent card (ringForCalls off / sound mode off). Bounded by
    // `ringSeconds`, and stopped by stopIncomingCall().
    void showIncomingCall(const QString &roomId, const QString &callId,
                          const QString &title, const QString &safeBody,
                          bool sound, int ringSeconds);
    // Retire the incoming-call notification (call ended or was handled on
    // another device). Safe to call when nothing is showing.
    void stopIncomingCall(const QString &callId);

    // Logout/account switch: forget queued click payloads.
    void clearPending();

    // Test hook: record a delivered notification's click payload exactly as
    // deliver() would, exercising the bounded FIFO eviction without a live
    // DBus service. Not used in production paths.
    void recordPayloadForTest(quint32 id, const QVariantMap &payload)
    { recordPayload(id, payload); }
    int pendingPayloadCountForTest() const { return m_pendingPayloads.size(); }
    bool callRingActiveForTest() const { return m_callRingTimer.isActive(); }
    QString activeCallIdForTest() const { return m_activeCallId; }
    // The DBus daemon is absent under offscreen tests: this stands in for
    // the Notify() reply so the id-matched decline/closed branches can be
    // driven (they key on the delivered notification id).
    void setActiveCallNotificationIdForTest(quint32 id)
    { m_activeCallNotificationId = id; }
    // Count of generic notices raised (missed calls, invites, ...): lets
    // integration tests observe the AppController gating without a daemon.
    int genericNoticeCountForTest() const { return m_genericNoticeCount; }
    // The tray balloon is the delivery where no freedesktop daemon exists
    // (Windows, macOS): see deliverThroughTray. The manager keeps the ONE
    // payload the balloon carries — the platforms show one balloon at a
    // time and a new one replaces it — so a click routes to that room.
    void setFallbackTray(TrayIcon *tray);
    void deliverThroughTrayForTest(const QVariantMap &payload)
    { m_lastFallbackPayload = payload; }

Q_SIGNALS:
    // The user activated a notification. Identity only — no tokens.
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);
    // The user pressed Decline on the incoming-call notification.
    void callDeclineRequested(const QString &callId);
    // v0.9.0 notification actions. Both carry the ACCOUNT the notification
    // was raised for, because a desktop notification outlives the account
    // that produced it: the user can switch accounts, or sign out entirely,
    // while the card is still on screen. Acting on it under whichever
    // account happens to be current would send a reply from the wrong
    // identity into a room the new account may not even be in — a mistake
    // that is invisible until someone else points it out. The app layer
    // compares this against the live account and refuses a mismatch.
    void markReadRequested(const QString &accountUserId, const QString &roomId,
                           const QString &eventId);
    void replyRequested(const QString &accountUserId, const QString &roomId,
                        const QString &threadRootId, const QString &text);

private Q_SLOTS:
    // DBus signal receivers (freedesktop Notifications).
    void onActionInvoked(quint32 id, const QString &action);
    // The daemon's inline reply box was submitted. Not part of the
    // freedesktop spec proper — it is the KDE/Plasma `inline-reply`
    // extension, which GNOME's daemon also implements — so the action is
    // only ever offered when GetCapabilities advertises it.
    void onNotificationReplied(quint32 id, const QString &text);
    void onNotificationClosed(quint32 id, quint32 reason);
    // The tray balloon (Windows / macOS delivery) was clicked.
    void onFallbackMessageClicked();
    void onCallRingTick();

private:
    // `fallback` is shown when the avatar is absent or cannot be fetched;
    // an empty image reverts to the daemon's own generic icon.
    void deliver(const QString &title, const QString &body,
                 const QVariantMap &payload, bool sound = false,
                 const QString &avatarMxc = QString(),
                 const QImage &fallback = QImage());
    void deliverNow(const QString &title, const QString &body,
                    const QVariantMap &payload, bool sound,
                    const QImage &avatar);
    // One re-delivery of the active incoming-call notification (replacing
    // the previous one via replaces_id so cards never stack).
    void deliverCallNotification();
    void flushAvatarWaits(bool fallbackAll);
    // Store one click payload, evicting the oldest when the bounded cap is
    // exceeded (a desktop only keeps a handful visible). FIFO eviction keeps
    // the most recent notifications clickable instead of dropping them all.
    void recordPayload(quint32 id, const QVariantMap &payload);
public:
    /// Withdraw every still-showing notification for a room the user has now
    /// read.
    ///
    /// GHOST NOTIFICATIONS. Only the call ring was ever closed; a message
    /// notification stayed in the notification centre after its room had
    /// been read — here, or in another client, or on a phone. So the desktop
    /// kept asserting that something was waiting when nothing was, which
    /// teaches a person to stop believing the notification area, and that is
    /// how a real message then gets missed.
    ///
    /// Cheap when there is nothing to do: the payload map is small and
    /// bounded, and a room with no live notification scans a few entries and
    /// returns.
    void closeRoomNotifications(const QString &roomId);
private:
    void forgetPayload(quint32 id);
    /// The balloon delivery, for builds and sessions with no freedesktop
    /// daemon. True when the tray showed it.
    bool deliverThroughTray(const QString &title, const QString &body,
                            const QVariantMap &payload, const QImage &avatar);
    QPointer<TrayIcon> m_fallbackTray;
    QVariantMap m_lastFallbackPayload;

    // Bounded number of click payloads retained for routing.
    static constexpr int kMaxPendingPayloads = 64;

    QHash<quint32, QVariantMap> m_pendingPayloads;
    // Insertion order of the ids in m_pendingPayloads, oldest first, so the
    // eviction can drop the least-recent entry (QHash is unordered).
    QList<quint32> m_payloadOrder;
    // Monotonic time (ms) of the last sound played; a short window coalesces
    // notification bursts into a single alert.
    qint64 m_lastSoundMs = 0;
    // Escapes a body iff the daemon advertises body-markup. THE ONLY place
    // a notification body is escaped; callers pass raw text. See the
    // definition for why the summary is never escaped.
    QString bodyForServer(class QDBusInterface &notifications,
                          const QString &body);

    // Whether this notification server renders markup in `body`.
    // Queried once (GetCapabilities) and cached: a daemon cannot
    // change it without restarting. Decides whether the body is
    // HTML-escaped before it is sent. See deliverNow().
    bool m_bodyMarkup = false;
    bool m_bodyMarkupKnown = false;
    // Whether the daemon advertises `inline-reply`. Queried in the same
    // GetCapabilities round trip as body-markup, because there is no reason
    // to make two.
    bool m_inlineReply = false;
    QString m_accountUserId;

    // Incoming-call ring state: the active call, its notification id (for
    // replaces_id and CloseNotification), the repeat timer and deadline.
    QString m_activeCallId;
    QString m_activeCallRoomId;
    QString m_activeCallTitle;
    QString m_activeCallBody;
    bool m_activeCallSound = false;
    quint32 m_activeCallNotificationId = 0;
    qint64 m_callRingDeadlineMs = 0;
    QTimer m_callRingTimer;
    int m_genericNoticeCount = 0;
    struct WaitingDelivery {
        QString title;
        QString body;
        QVariantMap payload;
        bool sound = false;
        QString avatarMxc;
        QImage fallback;
    };
    std::function<QImage(const QString &, bool)> m_avatarImage;
    std::function<bool(const QString &)> m_avatarFailed;
    QList<WaitingDelivery> m_avatarWaits;
    QTimer m_avatarWaitTimer;
};
