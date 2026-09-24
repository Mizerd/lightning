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

// Native desktop notifications.
//
// The decision (whether to notify, and the title/body under the active
// privacy mode) is pure and unit-tested; delivery uses the freedesktop
// Notifications D-Bus service, or the tray balloon where none exists. Bodies
// appear only in "Sender and message" mode and are never logged or persisted.
// Click payloads carry only room/event/thread identity.
class NotificationManager : public QObject
{
    Q_OBJECT
public:
    // SettingsManager's notificationPreview values.
    enum PreviewMode { SenderAndMessage = 0, SenderOnly = 1, Private = 2 };
    Q_ENUM(PreviewMode)
    // Local per-room notification mode. FollowDefault means the account's push
    // rules decide; this device does not resolve that default, so locally it
    // behaves like AllMessages.
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
        // The room is on screen, focused and following the newest message.
        bool roomVisibleAtLatest = false;
        // The room was just opened and is loading its first screen. Sliding
        // sync delivers its recent timeline as live appends, which must not
        // notify.
        bool roomHydrating = false;
        // False while the initial sync backlog is applied, so existing history
        // does not re-notify on every launch. Defaults true for the mock
        // backend and policy tests.
        bool initialSyncComplete = true;
        SoundMode soundMode = SoundMentionsAndDirect;
        // The sender is on m.ignored_user_list. Covers the window before the
        // server stops delivering their events.
        bool senderIsIgnored = false;
        // Effective room avatar: explicit room avatar, or the unambiguous
        // other user's profile avatar for a strict 1:1 DM.
        QString avatarMxc;
        // Identity key for the initials disc when there is no avatar; matches
        // the room list's colour. Empty falls back to the room name.
        QString avatarColorKey;
        // Active SettingsManager::Theme; identity disc colours derive from it.
        int themeId = 0;
    };
    struct Decision {
        bool notify = false;
        QString title;
        QString body;
        // Whether to also play a sound. Always false when notify is false.
        bool playSound = false;
    };

    explicit NotificationManager(QObject *parent = nullptr);
    // App-layer adapter over MediaBridge, keeping this class free of it.
    // `image(mxc, request)` returns a cached image and optionally starts a
    // fetch; `failed` reports a failure mark. Call avatarCacheChanged() when
    // either changes.
    void setAvatarProvider(
        std::function<QImage(const QString &, bool request)> image,
        std::function<bool(const QString &)> failed);
    void avatarCacheChanged();
    /// The account notifications are raised for. Recorded in every click
    /// payload so actions taken after an account switch can be refused.
    void setAccountUserId(const QString &userId) { m_accountUserId = userId; }
    QString accountUserId() const { return m_accountUserId; }
    /// Whether the daemon offers an inline reply box. False until the first
    /// delivery has queried GetCapabilities, and without D-Bus.
    bool inlineReplySupported() const { return m_inlineReply; }

    // Pure policy — no I/O, no logging. Exposed for tests.
    static Decision decide(const TimelineEvent &event, const Context &context);

    // Invites notify only after the initial sync, once per session, and only
    // when notifications are enabled.
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

    // Incoming call. While ringing it is re-delivered every few seconds,
    // replacing itself, so the themed call sound repeats. `sound` false shows a
    // silent card. Bounded by `ringSeconds`; stopped by stopIncomingCall().
    //
    // `acceptOffered`, `rtcLane` and `silenceOffered` are inputs: whether a
    // call is joinable is decided once by the gate IncomingCallPrompt,
    // RoomCallBanner and CallEventDelegate share, and the caller knows which
    // ringer is sounding. Silence emits callSilenceRequested and ends nothing.
    void showIncomingCall(const QString &roomId, const QString &callId,
                          const QString &title, const QString &safeBody,
                          bool sound, int ringSeconds,
                          bool acceptOffered = false, bool rtcLane = false,
                          bool silenceOffered = false);

    /// The action list the freedesktop card is delivered with. Public and
    /// static so tests can check it without a daemon.
    static QStringList callActions(bool acceptOffered, bool rtcLane,
                                   bool silenceOffered = false);
    /// Update whether the showing call card offers an answer, and redraw it.
    /// The joinability answer usually arrives after the card is raised (it
    /// needs an asynchronous session read). Unlike showIncomingCall(), this
    /// keeps the ring deadline and sound timer. Mismatched or absent call ids
    /// are a no-op.
    void setCallAcceptOffered(const QString &callId, bool offered,
                              bool rtcLane);

    /// The user silenced this call's ring. The card stays; the themed sound and
    /// its re-post stop and the Silence action is dropped. Mismatched ids and
    /// repeat calls are no-ops. Never touches the ring deadline.
    void silenceIncomingCall(const QString &callId);

    // Retire the incoming-call notification (call ended or was handled on
    // another device). Safe to call when nothing is showing.
    void stopIncomingCall(const QString &callId);

    // Logout/account switch: forget queued click payloads.
    void clearPending();

    // Test hook: record a click payload as deliver() would, exercising the
    // bounded FIFO eviction without D-Bus.
    void recordPayloadForTest(quint32 id, const QVariantMap &payload)
    { recordPayload(id, payload); }
    int pendingPayloadCountForTest() const { return m_pendingPayloads.size(); }
    bool callRingActiveForTest() const { return m_callRingTimer.isActive(); }
    /// Whether the showing card would be delivered with the themed call sound
    /// (setCallAcceptOffered redraws with it even when the timer is idle), and
    /// with a Silence action.
    bool callSoundActiveForTest() const { return m_activeCallSound; }
    bool callSilenceOfferedForTest() const
    { return m_activeCallSilenceOffered; }
    QString activeCallIdForTest() const { return m_activeCallId; }
    // Stands in for the Notify() reply so the id-matched decline/closed
    // branches can be driven without a daemon.
    void setActiveCallNotificationIdForTest(quint32 id)
    { m_activeCallNotificationId = id; }
    // Count of generic notices raised, for tests without a daemon.
    int genericNoticeCountForTest() const { return m_genericNoticeCount; }
    // Tray balloon delivery where no freedesktop daemon exists (Windows,
    // macOS). Only one balloon shows at a time, so the manager keeps the single
    // payload it carries.
    void setFallbackTray(TrayIcon *tray);
    void deliverThroughTrayForTest(const QVariantMap &payload)
    { m_lastFallbackPayload = payload; }
    // Tray balloon attempts by the call ring, and their payload. Counts
    // attempts rather than deliveries because offscreen tests have no tray.
    int callTrayAttemptsForTest() const { return m_callTrayAttempts; }
    QVariantMap lastCallTrayPayloadForTest() const
    { return m_lastCallTrayPayload; }
    // Deliveries parked waiting for an avatar fetch. They have no notification
    // id yet, so tests observe them here.
    int avatarWaitCountForTest() const { return m_avatarWaits.size(); }
    // The payload a parked delivery carries.
    QVariantMap avatarWaitPayloadForTest(int index) const
    {
        return index >= 0 && index < m_avatarWaits.size()
            ? m_avatarWaits.at(index).payload : QVariantMap{};
    }

    // ── Composite thread-timeline ids never leave this class ──
    // Thread replies arrive with the composite timeline id in
    // TimelineEvent::roomId. These reduce it to routable identity; both are
    // no-ops for real room ids.
    static QString routableRoomId(const QString &roomId);
    static QString routableThreadRootId(const QString &roomId,
                                        const QString &threadRootId);

Q_SIGNALS:
    // The user activated a notification. Identity only — no tokens.
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);
    // The user pressed Decline on the incoming-call notification.
    void callDeclineRequested(const QString &callId);
    /// The user pressed Answer/Join on the incoming-call notification.
    ///
    ///
    /// Signals name no D-Bus type, so they stay outside the QtDBus guard and
    /// compile on Windows and macOS.
    void callAcceptRequested(const QString &callId);
    /// The user pressed Silence. Only a request:
    /// CallSoundController::silenceRing decides, and quietens the card through
    /// silenceIncomingCall().
    void callSilenceRequested(const QString &callId);
    // Notification actions. Both carry the account the notification was raised
    // for, since the card can outlive an account switch or sign-out; the app
    // layer refuses a mismatch.
    void markReadRequested(const QString &accountUserId, const QString &roomId,
                           const QString &eventId);
    void replyRequested(const QString &accountUserId, const QString &roomId,
                        const QString &threadRootId, const QString &text);

private Q_SLOTS:
    // DBus signal receivers (freedesktop Notifications).
    void onActionInvoked(quint32 id, const QString &action);
    // Inline reply submitted. This is the KDE `inline-reply` extension (also
    // implemented by GNOME), offered only when GetCapabilities advertises it.
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
    // Re-deliver the incoming-call notification, replacing the previous one.
    void deliverCallNotification();
    void flushAvatarWaits(bool fallbackAll);
    // Store a click payload, evicting the oldest past the cap so the most
    // recent notifications stay clickable.
    void recordPayload(quint32 id, const QVariantMap &payload);
public:
    /// Withdraw every still-showing notification for a room that has been read
    /// (here or elsewhere), so the notification centre does not keep stale
    /// entries.
    void closeRoomNotifications(const QString &roomId);
private:
    void forgetPayload(quint32 id);
    /// The one place openRequested is emitted; normalises the payload identity.
    void emitOpenFor(const QVariantMap &payload);
    // The incoming-call ring's tray delivery; see the definition.
    bool deliverCallThroughTray();
    /// Balloon delivery for sessions with no freedesktop daemon. True when the
    /// tray showed it.
    bool deliverThroughTray(const QString &title, const QString &body,
                            const QVariantMap &payload, const QImage &avatar);
    QPointer<TrayIcon> m_fallbackTray;
    QVariantMap m_lastFallbackPayload;

    // Bounded number of click payloads retained for routing.
    static constexpr int kMaxPendingPayloads = 64;

    QHash<quint32, QVariantMap> m_pendingPayloads;
    // Insertion order of m_pendingPayloads, oldest first (QHash is unordered).
    QList<quint32> m_payloadOrder;
    // Monotonic time of the last sound; coalesces bursts into one alert.
    qint64 m_lastSoundMs = 0;
    // Escapes a body iff the daemon advertises body-markup. The only place a
    // notification body is escaped; callers pass raw text.
    QString bodyForServer(class QDBusInterface &notifications,
                          const QString &body);

    // Whether the server renders body markup. Queried once and cached.
    bool m_bodyMarkup = false;
    bool m_bodyMarkupKnown = false;
    // Whether the daemon advertises `inline-reply` (same GetCapabilities call).
    bool m_inlineReply = false;
    QString m_accountUserId;

    // Incoming-call ring state: the active call, its notification id (for
    // replaces_id and CloseNotification), the repeat timer and deadline.
    QString m_activeCallId;
    QString m_activeCallRoomId;
    QString m_activeCallTitle;
    QString m_activeCallBody;
    bool m_activeCallSound = false;
    // Supplied by the caller; see showIncomingCall.
    bool m_activeCallAcceptOffered = false;
    bool m_activeCallRtcLane = false;
    bool m_activeCallSilenceOffered = false;
    quint32 m_activeCallNotificationId = 0;
    // Whether this call was already announced through the tray. Reset on every
    // raise and stop.
    bool m_activeCallTrayDelivered = false;
    int m_callTrayAttempts = 0;
    QVariantMap m_lastCallTrayPayload;
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
