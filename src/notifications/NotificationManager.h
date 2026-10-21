#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

struct TimelineEvent;
class TrayIcon;

// Native desktop notifications.
//
// The decision (whether to notify, and the title/body under the active
// privacy mode) is pure and unit-tested; delivery uses the freedesktop
// Notifications D-Bus service, or the tray balloon where none exists. Bodies
// appear only in "Sender and message" mode and are never logged or persisted.
// Click payloads carry only room/event/thread identity.
//
// One card per room (and one per thread, and one per keyed notice): a new
// message replaces its room's card through replaces_id rather than stacking a
// new one, so withdrawing a read room is one CloseNotification and the
// bounded click-payload map is bounded by rooms rather than by traffic.
class NotificationManager : public QObject
{
    Q_OBJECT
public:
    // SettingsManager's notificationPreview values.
    enum PreviewMode { SenderAndMessage = 0, SenderOnly = 1, Private = 2 };
    Q_ENUM(PreviewMode)
    // Local per-room notification mode. Muted always silences, so a mute
    // applies before the server echoes it. Otherwise the account's push rules
    // decide when the event carries them (TimelineEvent::pushVerdict); without
    // them the local mode decides alone, FollowDefault as AllMessages.
    enum RoomMode {
        AllMessages = 0, MentionsOnly = 1, Muted = 2, FollowDefault = 3
    };
    Q_ENUM(RoomMode)
    // SettingsManager's notificationSound values.
    enum SoundMode { SoundOff = 0, SoundMentionsAndDirect = 1, SoundAll = 2 };
    Q_ENUM(SoundMode)
    // SettingsManager's notificationSoundSource values: whose sound a
    // delivered notification makes. Lightning's own chime (the platform's is
    // suppressed) or the platform's themed one.
    enum SoundSource { SourceLightning = 0, SourceSystem = 1 };
    Q_ENUM(SoundSource)

    /// What one delivered notification does about sound. Pure, so the hint
    /// the daemon receives and the chime Lightning plays are decided in one
    /// place and tested without a bus.
    struct SoundPlan {
        /// Lightning plays its own chime.
        bool playOwn = false;
        /// The notification asks the daemon for its themed sound.
        bool platformSound = false;
        /// The notification carries `suppress-sound`, so no daemon adds one
        /// of its own. True whenever the platform sound is not wanted.
        bool suppressPlatform = true;
    };
    /// `allowed`: the policy (mode, mute, focus, ...) allows a sound for this
    /// notification. `coalesced`: another sound played within the burst
    /// window.
    static SoundPlan planSound(bool allowed, bool coalesced,
                               SoundSource source);
    /// The freedesktop hints a plan adds to a Notify call.
    static QVariantMap soundHints(const SoundPlan &plan);

    /// The action list and urgency a message or notice card is sent with.
    /// Pure, like callActions(), so the composition is tested without a
    /// daemon. Every message card is normal urgency: critical is reserved for
    /// a live ring, since GNOME and KDE let a critical card through Do Not
    /// Disturb and over full-screen windows and never expire it.
    struct CardShape {
        QStringList actions;
        uchar urgency = 1;
        /// The card offers an inline reply box (KDE placeholder hint).
        bool inlineReply = false;
    };
    /// `actionable`: the card names a real room and event (generic notices do
    /// not, and cannot be read or replied to).
    static CardShape messageCardShape(bool actionable,
                                      bool inlineReplySupported);

    /// Whether message cards carry the `resident` hint, from the daemon's
    /// GetServerInformation (R13). Only Plasma before 6.4 needs it: it expires
    /// a timed-out non-resident popup and strips its actions, so its history
    /// entry cannot be clicked. Nowhere else: GNOME Shell acknowledges a
    /// resident card of the focused app, which then shows no banner and plays
    /// no sound. Pure.
    static bool residentHintWanted(const QString &name, const QString &vendor,
                                   const QString &version);

    /// The desktop-entry hint and app icon for this install. Pure: tests pass
    /// the environment. An empty appIcon means "the themed name if it
    /// resolves, else the bundled icon written to the cache".
    struct DesktopIdentity {
        QString desktopEntry;
        QString appIcon;
    };
    static DesktopIdentity desktopIdentityFor(const QString &flatpakId,
                                              const QString &snapDir,
                                              const QString &snapName);

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
        // The sender's name in this room's member list and their global
        // profile name, where the app layer knows them. Either may be empty,
        // or the bare user id (what a member lookup answers for a user it
        // does not know); see senderLabel().
        QString senderMemberName;
        QString senderProfileName;
        // The server does not have this room's mode: a failed write, or a
        // device-only mode from before modes were saved to the server. The
        // local mode then decides alone.
        bool localModeUnsynced = false;
        // Effective room avatar: explicit room avatar, or the unambiguous
        // other user's profile avatar for a strict 1:1 DM.
        QString avatarMxc;
        // Identity key for the initials disc when there is no avatar; matches
        // the room list's colour. Empty falls back to the room name.
        QString avatarColorKey;
        // Active SettingsManager::Theme; identity disc colours derive from it.
        int themeId = 0;
        // The account the event belongs to. Empty = the foreground account
        // (today): the payload is stamped with accountUserId() and the card
        // lives in the "" table. Non-empty: the payload is stamped with it and
        // the card lives in that account's own table.
        QString accountUserId;
        // Display label for titles once more than one account is live. Not
        // read anywhere yet.
        QString accountLabel;
        friend bool operator==(const Context &, const Context &) = default;
    };
    struct Decision {
        bool notify = false;
        QString title;
        QString body;
        // Whether to also play a sound. Always false when notify is false.
        bool playSound = false;
        // A mention: the mention chime and the card's mention flag. With the
        // account's rules known it is their highlight alone; without them,
        // m.mentions naming this user (never m.mentions.room alone, which
        // carries no power-level check).
        bool mention = false;
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
    /// Whose sound a notification makes; see SoundSource.
    void setSoundSource(int source);
    SoundSource soundSource() const { return m_soundSource; }
    /// Test hook: ask the installed chime player directly, as a delivery does.
    bool playOwnSoundForTest(bool mention) const
    { return m_ownSound && m_ownSound(mention); }
    /// Plays Lightning's chime for a delivered notification (`mention`
    /// selects the mention variant) and returns whether one played.
    void setOwnSoundPlayer(std::function<bool(bool mention)> player);
    /// The call-state policy for any notification sound, Lightning's or the
    /// desktop's: false while a call or a share that captures the output mix
    /// must stay quiet. Consulted before either source plays. Once installed,
    /// a chime the player then cannot play falls back to the desktop's sound
    /// (without it a refusal by the player could be the policy, not a fault).
    void setSoundAllowedCheck(std::function<bool()> check);
    /// The desktop is in Do Not Disturb: the daemon's `Inhibited` property
    /// (KDE), or GNOME's show-banners off. Lightning's own chime stays quiet
    /// while it holds. False where it cannot be read.
    bool desktopInhibited() const
    { return m_daemonInhibited || m_bannersOff; }
    /// Whether the daemon offers an inline reply box. False until the first
    /// delivery has queried GetCapabilities, and without D-Bus.
    bool inlineReplySupported() const { return m_inlineReply; }

    // Pure policy — no I/O, no logging. Exposed for tests.
    static Decision decide(const TimelineEvent &event, const Context &context);
    /// The name a card gives the sender, the same in every card whichever
    /// path delivered the event: the room member's display name (the event's
    /// own, else the context's), then the global profile name, then the
    /// localpart. A "name" that is just the sender's user id counts as none,
    /// so the full MXID appears only when the id has no localpart to show.
    static QString senderLabel(const TimelineEvent &event,
                               const Context &context);

    // Invites notify only after the initial sync, once per session, and only
    // when notifications are enabled.
    static bool shouldNotifyInvite(bool initialSyncComplete, bool alreadyKnown,
                                   bool notificationsEnabled);

    // Decide and (when positive) deliver natively. The click payload is
    // {roomId, eventId, threadRootId} only. A message that could not be
    // decrypted yet is held briefly for its key; see processDecrypted().
    void processEvent(const TimelineEvent &event, const Context &context);

    /// An event this class saw live while undecryptable has been decrypted.
    /// Decides again on the plaintext: a card still held is delivered with
    /// it, an "Encrypted message" card is replaced in place (silently), and
    /// a message whose ciphertext did not notify (a mention in a mentions-only
    /// room) notifies now. Never a second card for one event; events never
    /// seen live are ignored, so a history backfill raises nothing.
    void processDecrypted(const TimelineEvent &event, const Context &context);

    /// Refresh an existing card from an SDK-aggregated row, silently. Never
    /// raises a card for an event this manager has not already announced.
    /// (A room with no open timeline has no such row: its edits reach
    /// processEvent() raw, which refreshes the card the same way.)
    void processUpdated(const TimelineEvent &event, const Context &context);

    /// The event was redacted (or otherwise must disappear): withdraw a card
    /// showing it, and drop one not delivered yet.
    void withdrawEvent(const QString &eventId);

    // Generic non-message notifications (invites, verification requests).
    // The body must already be safe — callers never pass message content.
    // `sound` asks for the message sound plan (the same coalescing, policy
    // and source). A non-empty `key` names the card: showing the same key
    // again replaces it, and withdrawGeneric() withdraws it.
    void showGeneric(const QString &title, const QString &safeBody,
                     const QString &roomId = QString(),
                     const QString &avatarMxc = QString(),
                     bool sound = false, const QString &key = QString());
    /// Withdraw the generic card raised with `key`, shown or still on its way.
    void withdrawGeneric(const QString &key);

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
    /// The call card's sound hints: the themed ring while it rings, and
    /// `suppress-sound` otherwise, so no daemon adds a default sound to a
    /// silent card or under Lightning's own ringer. Pure.
    static QVariantMap callSoundHints(bool sound);
    /// Update whether the showing call card offers an answer, and redraw it.
    /// The joinability answer usually arrives after the card is raised (it
    /// needs an asynchronous session read). Unlike showIncomingCall(), this
    /// keeps the ring deadline and sound timer. Mismatched or absent call ids
    /// are a no-op, and a card that is not showing is not raised again.
    void setCallAcceptOffered(const QString &callId, bool offered,
                              bool rtcLane);

    /// The user silenced this call's ring. The card stays; the themed sound and
    /// its re-post stop and the Silence action is dropped. Mismatched ids and
    /// repeat calls are no-ops. Never touches the ring deadline.
    void silenceIncomingCall(const QString &callId);

    // Retire the incoming-call notification (call ended or was handled on
    // another device). Safe to call when nothing is showing.
    void stopIncomingCall(const QString &callId);

    /// Logout/account switch/removal: withdraw every card the account raised
    /// (their previews must not outlive it) and forget their click payloads.
    /// An action on one that still arrives reaches the different-account
    /// refusal through a tombstone rather than vanishing.
    ///
    /// An empty `accountUserId` is the foreground account and is exactly the
    /// clear this class has always done: the "" table plus every process-wide
    /// piece of state (the call card, capability waits, held and live
    /// undecryptable messages, rooms awaiting unread, activation tokens, the
    /// balloon's payload and rooms). A non-empty one touches only what is
    /// tagged with that account: its table and delivery generation, its
    /// avatar and capability waits, its held messages, and the balloon's
    /// payload when that account raised it. It never stops the call ring.
    void clearPendingFor(const QString &accountUserId);
    /// The foreground account's clear; see clearPendingFor().
    void clearPending() { clearPendingFor(QString()); }

    // Test hook: record a click payload as deliver() would, exercising the
    // bounded FIFO eviction without D-Bus. The "" (foreground) table.
    void recordPayloadForTest(quint32 id, const QVariantMap &payload)
    { recordPayload(QString(), id, payload); }
    /// The same, into `accountUserId`'s table ("" is the foreground's).
    void recordPayloadForTest(const QString &accountUserId, quint32 id,
                              const QVariantMap &payload)
    { recordPayload(accountUserId, id, payload); }
    /// Click payloads held in the "" (foreground) table, or in one account's.
    int pendingPayloadCountForTest() const
    { return pendingPayloadCountForTest(QString()); }
    int pendingPayloadCountForTest(const QString &accountUserId) const;
    /// The generation a Notify reply for `accountUserId`'s table must carry.
    quint64 deliveryGenerationForTest(const QString &accountUserId) const;
    /// Stands in for a Notify reply naming `id`, for a card raised under
    /// `accountUserId`'s table at `generation`: what the D-Bus reply runs
    /// after its daemon check, so the generation rule is tested without one.
    void notifyReplyForTest(const QString &accountUserId, quint64 generation,
                            quint32 id, const QVariantMap &payload);
    bool callRingActiveForTest() const { return m_callRingTimer.isActive(); }
    /// Whether the showing card would be delivered with the themed call sound
    /// (setCallAcceptOffered redraws with it even when the timer is idle), and
    /// with a Silence action.
    bool callSoundActiveForTest() const { return m_activeCallSound; }
    bool callSilenceOfferedForTest() const
    { return m_activeCallSilenceOffered; }
    QString activeCallIdForTest() const { return m_activeCallId; }
    quint32 activeCallNotificationIdForTest() const
    { return m_activeCallNotificationId; }
    // Stands in for the Notify() reply so the id-matched decline/closed
    // branches can be driven without a daemon.
    void setActiveCallNotificationIdForTest(quint32 id)
    { m_activeCallNotificationId = id; m_activeCallCardExpired = false; }
    // Count of generic notices raised, for tests without a daemon.
    int genericNoticeCountForTest() const { return m_genericNoticeCount; }
    // Tray balloon delivery where no freedesktop daemon exists (Windows,
    // macOS). A balloon's click carries no id. Where the platform can take a
    // balloon down (Windows, TrayIcon::canWithdrawBalloon), at most ONE is on
    // screen and the click is the one it shows: what arrives meanwhile waits
    // as one pending summary (offerBalloon()). Elsewhere every balloon is
    // shown and a click opens a room only when every balloon raised since the
    // last click was that room's.
    void setFallbackTray(TrayIcon *tray);
    /// ONE message for the balloon, through the same path a delivery takes,
    /// shown as if the tray had shown it (offscreen tests have none). Not
    /// sticky: a later show (a summary, the next balloon after a click) needs
    /// setTrayShowsForTest(true). Breaks through like a mention when the
    /// payload is one or names no room.
    void deliverThroughTrayForTest(const QVariantMap &payload)
    { deliverThroughTrayForTest(QString(), payload); }
    /// The same, for a balloon raised from `accountUserId`'s table.
    void deliverThroughTrayForTest(const QString &accountUserId,
                                   const QVariantMap &payload);
    /// Treat EVERY show with no tray icon as shown, until set back.
    void setTrayShowsForTest(bool shows) { m_trayShowsForTest = shows; }
    /// Pretend the platform can (or cannot) take a balloon down, instead of
    /// asking TrayIcon::canWithdrawBalloon().
    void setBalloonWithdrawableForTest(bool withdrawable)
    { m_balloonWithdrawableOverride = withdrawable ? 1 : 0; }
    /// How long a balloon holds the screen before the pending summary may
    /// replace it, instead of TrayIcon::balloonHoldMs().
    void setBalloonHoldMsForTest(int ms) { m_balloonHoldOverrideMs = ms; }
    bool balloonOnScreenForTest() const { return m_balloonHold.isActive(); }
    /// Rooms (and threads) waiting in the pending summary, and the messages
    /// they stand for.
    int pendingBalloonEntriesForTest() const
    { return int(m_balloonQueue.size()); }
    int pendingBalloonMessagesForTest() const;
    /// The payload of the last balloon attempt (messages, notices and calls).
    QVariantMap lastTrayPayloadForTest() const { return m_lastTrayPayload; }
    // Tray balloon attempts by the call ring, and their payload. Counts
    // attempts rather than deliveries because offscreen tests have no tray.
    int callTrayAttemptsForTest() const { return m_callTrayAttempts; }
    QVariantMap lastCallTrayPayloadForTest() const
    { return m_lastCallTrayPayload; }
    // Every tray balloon attempt (messages and calls), its title and body, and
    // whether the last one allowed the platform's own sound. Attempts, as
    // above.
    int trayAttemptsForTest() const { return m_trayAttempts; }
    bool lastTrayPlatformSoundForTest() const
    { return m_lastTrayPlatformSound; }
    QString lastTrayTitleForTest() const { return m_lastTrayTitle; }
    QString lastTrayBodyForTest() const { return m_lastTrayBody; }
    /// Whether the balloon path can carry the platform's sound: Windows can
    /// (TrayIcon sets or clears NIIF_NOSOUND), macOS and a Linux tray cannot.
    void setTrayCarriesSoundForTest(bool carries)
    { m_trayCarriesSoundOverride = carries ? 1 : 0; }
    // Deliveries parked waiting for an avatar fetch. They have no notification
    // id yet, so tests observe them here. The "" (foreground) account's, or
    // one account's.
    int avatarWaitCountForTest() const
    { return avatarWaitCount(QString()); }
    int avatarWaitCountForTest(const QString &accountUserId) const
    { return avatarWaitCount(accountUserId); }
    // The payload a parked "" delivery carries, and its body; `index` counts
    // the foreground account's waits only, oldest first.
    QVariantMap avatarWaitPayloadForTest(int index) const
    {
        const Card *card = foregroundAvatarWait(index);
        return card ? card->payload : QVariantMap{};
    }
    QString avatarWaitBodyForTest(int index) const
    {
        const Card *card = foregroundAvatarWait(index);
        return card ? card->body : QString();
    }
    /// Replacements dropped because their card was gone or superseded.
    int droppedReplacementsForTest() const { return m_droppedReplacements; }
    /// Cards parked behind the capability query, and the message count and
    /// body a group shows (the foreground account's).
    int capabilityWaitCountForTest() const { return m_capabilityWaits.size(); }
    int groupCountForTest(const QString &groupKey) const
    { return m_tables.value(QString()).groups.value(groupKey).count; }
    QString groupBodyForTest(const QString &groupKey) const
    { return m_tables.value(QString()).groups.value(groupKey).body; }
    QVariantMap firstGroupPayloadForTest() const
    {
        const auto &groups = m_tables.value(QString()).groups;
        return groups.isEmpty() ? QVariantMap{} : groups.cbegin()->payload;
    }
    int groupTableSizeForTest() const
    { return m_tables.value(QString()).groups.size(); }
    /// Whether a parked "" delivery has an avatar or a fallback image.
    bool avatarWaitHasImageForTest(int index) const
    {
        const Card *card = foregroundAvatarWait(index);
        return card && (!card->avatarMxc.isEmpty() || !card->fallback.isNull()
                        || !card->avatar.isNull());
    }
    /// Messages held for their decryption key, and the hold.
    int heldUndecryptableCountForTest() const { return m_heldUtds.size(); }
    void setUndecryptableHoldMsForTest(int ms) { m_utdHoldMs = ms; }
    /// Stand-in for the desktop's Do Not Disturb state.
    void setDesktopInhibitedForTest(bool inhibited)
    { m_daemonInhibited = inhibited; }
    /// Notify replies this manager has processed, current or stale, so a test
    /// can wait for evidence rather than for time.
    int notifyRepliesProcessedForTest() const { return m_notifyReplies; }
    /// Pretend the session is Wayland, so the activation token is exported.
    void setAssumeWaylandForTest(bool wayland) { m_assumeWayland = wayland; }

    // ── Composite thread-timeline ids never leave this class ──
    // Thread replies arrive with the composite timeline id in
    // TimelineEvent::roomId. These reduce it to routable identity; both are
    // no-ops for real room ids.
    static QString routableRoomId(const QString &roomId);
    static QString routableThreadRootId(const QString &roomId,
                                        const QString &threadRootId);

Q_SIGNALS:
    // The user activated a notification. Identity only — no tokens. Empty ids
    // mean "raise the window" (a card whose room is no longer known, or a
    // balloon that cannot say which of several rooms was clicked).
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);
    /// Emitted immediately before every openRequested, with the same
    /// normalised ids and the account the card was raised for (the payload's
    /// "accountUserId"). Empty for a raise-window open: an empty payload, or
    /// a balloon click with more than one room outstanding.
    void openRequestedForAccount(const QString &accountUserId,
                                 const QString &roomId, const QString &eventId,
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
    /// The user dismissed the ringing call card (freedesktop reason 2). Not a
    /// decline: other devices keep ringing; the app stops its own ringer.
    void callCardDismissed(const QString &callId);
    /// A message card left for the desktop (daemon or balloon). For window
    /// attention hints; a silent in-place update is not a delivery.
    void messageDelivered(const QString &roomId, bool mention);
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
    // Spec 1.2: the xdg-activation token for an action that follows.
    void onActivationToken(quint32 id, const QString &token);
    // The daemon's `Inhibited` property changed (KDE Do Not Disturb).
    void onDaemonPropertiesChanged(const QString &interfaceName,
                                   const QVariantMap &changed,
                                   const QStringList &invalidated);
    // The notification service changed hands (daemon restart): every id it
    // gave out is meaningless now.
    void onDaemonOwnerChanged(const QString &service, const QString &oldOwner,
                              const QString &newOwner);
    // The tray balloon (Windows / macOS delivery) was clicked.
    void onFallbackMessageClicked();
    void onCallRingTick();

private:
    // One card on its way to the desktop.
    struct Card {
        QString title;
        QString body;
        QVariantMap payload;
        bool sound = false;
        // The resolved image (deliverNow), or what deliver() resolves it from.
        QImage avatar;
        QString avatarMxc;
        QImage fallback;
        // A silent in-place update (a decrypted "Encrypted message"): no
        // sound and no messageDelivered.
        bool replacement = false;
        // The table the card belongs to: Context::accountUserId, "" for the
        // foreground account, generic notices and the call card.
        QString accountKey;
    };
    // `fallback` is shown when the avatar is absent or cannot be fetched;
    // an empty image reverts to the daemon's own generic icon.
    void deliver(Card card);
    /// A replacement is still wanted: its group shows (or is raising) its
    /// event for its account. See deliverNow().
    bool replacementStillOwned(const Card &card) const;
    void deliverNow(Card card);
    // processUpdated() past its gate: refresh every copy of the card showing
    // `event` (waiting, in flight, on screen) silently, or withdraw it when
    // the update would not have notified.
    void refreshCard(const TimelineEvent &event, const Context &context);
    // A raw m.replace from the live sync (a room with no open timeline has no
    // SDK aggregate): refreshes the card showing its target when the edit is
    // valid for it (same room, same sender as the original, new content
    // present, not older than an edit already applied). True when it
    // matched such a card.
    bool refreshFromEdit(const TimelineEvent &edit, const Context &context);
    // Builds a message card from a decision and delivers it.
    void deliverDecision(const TimelineEvent &event, const Context &context,
                         const Decision &decision, bool undecryptable,
                         bool replacement);
    // Re-deliver the incoming-call notification, replacing the previous one.
    void deliverCallNotification();
    void flushAvatarWaits(bool fallbackAll);
    void flushHeldUndecryptable();
    // Store a click payload in `accountKey`'s table, evicting that table's
    // oldest past the cap so its most recent notifications stay clickable.
    // An evicted card is withdrawn.
    void recordPayload(const QString &accountKey, quint32 id,
                       const QVariantMap &payload);
    // A Notify reply for `card`, after the daemon check. `id` is the
    // daemon's answer, nullopt for an error. A card whose table was cleared
    // since it left is closed and tombstoned. True when the caller still has
    // the error's fallback to run.
    bool takeNotifyReply(const Card &card, const QString &groupKey,
                         quint64 generation, std::optional<quint32> id);
    int avatarWaitCount(const QString &accountKey) const;
    const Card *foregroundAvatarWait(int index) const;
public:
    /// Withdraw every still-showing notification for a room that has been read
    /// (here or elsewhere), so the notification centre does not keep stale
    /// entries. For an explicit read (mark-read, reply, an invite resolving);
    /// the room list's walk uses observeRoomUnread().
    void closeRoomNotifications(const QString &roomId);
    /// A thread was read: withdraw the cards its replies raised.
    void closeThreadNotifications(const QString &roomId,
                                  const QString &rootEventId);
    /// The room list's unread state for a room. Read withdraws its message
    /// cards, but only once the list has shown the room unread since its
    /// latest card: the card can come first, and a "read" that predates the
    /// message must not withdraw it. Thread replies and notices (a missed
    /// call, an invite) are not what the room's unread fields count, so the
    /// walk never withdraws them; they go with an explicit read.
    void observeRoomUnread(const QString &roomId, bool unread);
    bool roomAwaitsUnreadForTest(const QString &roomId) const
    { return m_roomsAwaitingUnread.contains(roomId); }
    void setAwaitUnreadMsForTest(int ms) { m_awaitUnreadMs = ms; }
    void setCapabilityRetryMsForTest(int ms) { m_capabilityRetryMs = ms; }
    int capabilityRequestsForTest() const { return m_capabilityRequests; }
private:
    // Remove a payload, whichever table holds it, and the group mapping that
    // points at it.
    void forgetPayload(quint32 id);
    // CloseNotification without waiting for an answer.
    void closeNotification(quint32 id);
    // Withdraw and forget `accountKey`'s cards matching `match`, sparing the
    // call card; also drops its matching deliveries not shown yet. Returns
    // cards withdrawn.
    int withdrawWhere(const QString &accountKey,
                      const std::function<bool(const QVariantMap &)> &match);
    // The same over every account's table.
    int withdrawEverywhere(
        const std::function<bool(const QVariantMap &)> &match);
    // Remember that `id` was ours and for which account, after its payload
    // went (sign-out, eviction).
    void tombstone(quint32 id, const QString &accountUserId);
    // An action arrived for a card we raised but can no longer route.
    void handleStaleAction(quint32 id, const QString &action,
                           const QString &text);
    // Our own short notice for an action that did nothing.
    void showStaleActionNotice();
    // GetCapabilities, asynchronously; delivers what waited for it. True while
    // a request is in flight; false in the pause after a failed one, when the
    // caller goes on with the fallback (escaped body, no inline reply).
    bool requestCapabilities();
    // GetServerInformation, asynchronously, beside GetCapabilities: decides
    // m_residentCards. Cards wait for both answers.
    void requestServerInformation();
    // Releases the cards (and the call card) that waited for the daemon's
    // answers, once neither question is in flight.
    void releaseCapabilityWaits();
    // Do Not Disturb: the daemon's Inhibited, and GNOME's show-banners through
    // the settings portal. Asked once per daemon, before the first delivery.
    void queryDesktopInhibition();
    /// The one place openRequested is emitted; normalises the payload identity.
    /// An empty payload raises the window without naming a room.
    void emitOpenFor(const QVariantMap &payload);
    // Export the activation token the daemon gave for `id` (Wayland), so the
    // window raise that follows may take focus.
    void applyActivationToken(quint32 id);
    // The incoming-call ring's tray delivery; see the definition.
    bool deliverCallThroughTray();
    /// Balloon delivery for sessions with no freedesktop daemon. True when the
    /// tray showed it. `platformSound` is the sound plan's: the balloon may
    /// make the platform's own sound only when the plan asks for it (System
    /// default chosen), never on top of or instead of silence by Lightning.
    /// `accountKey`: the table the card was raised from.
    bool deliverThroughTray(const QString &title, const QString &body,
                            const QVariantMap &payload, const QImage &avatar,
                            bool platformSound, const QString &accountKey);
    void noteBalloon(const QString &accountKey, const QVariantMap &payload);
    // One message for the balloon. Shown now when no balloon holds the screen
    // or when it breaks through (a mention, a notice with no room); otherwise
    // merged into the pending summary. True when shown or queued.
    struct Balloon {
        QString title;
        QString body;
        QVariantMap payload;
        QImage avatar;
        bool platformSound = false;
        QString accountKey;
        // Messages this entry stands for (merged from one room or thread).
        int messages = 1;
    };
    bool offerBalloon(Balloon balloon, bool breakThrough);
    // Whether the one-balloon gate applies: the platform can take a balloon
    // down (Windows), or a test says so.
    bool balloonGate() const;
    // Merge into the pending summary, one entry per account and card group.
    // `older`: the entry predates what is queued (a displaced balloon), so a
    // queued entry of its group keeps its newer content.
    void enqueueBalloon(Balloon balloon, bool older);
    // Before a balloon replaces one still holding the screen (gate only):
    // put what the displaced one stood for back into the summary.
    void requeueDisplacedBalloon();
    // The pending summary, shown once the balloon on screen has had its time
    // (or was clicked): the latest message when every entry is one room's,
    // otherwise "N new messages in M rooms", whose click opens no room.
    void showPendingBalloons();
    void onBalloonHoldExpired();
    // Forget what the balloon on screen would route to, and take it down
    // where the platform can (Windows); used when its account is cleared.
    void retireBalloonOnScreen();
    bool trayCarriesSound() const;
    // Monotonic milliseconds; immune to wall-clock steps.
    qint64 nowMs() const { return m_clock.elapsed(); }

    QPointer<TrayIcon> m_fallbackTray;
    QVariantMap m_lastFallbackPayload;
    // The table the balloon in m_lastFallbackPayload was raised from.
    QString m_lastFallbackAccountKey;
    QVariantMap m_lastTrayPayload;
    // Active while a balloon holds the screen. Windows queues a second
    // balloon behind the first and reports a click on either as the same
    // signal, so nothing new is shown until this runs out (or a click or a
    // break-through ends it).
    QTimer m_balloonHold;
    int m_balloonHoldOverrideMs = -1;
    int m_balloonWithdrawableOverride = -1;
    // What the balloon on screen stands for (itself, or a summary's
    // entries), re-queued if a break-through displaces it unclicked.
    QList<Balloon> m_shownEntries;
    // Without the gate: distinct rooms with a balloon raised since the last
    // click. A click cannot say which balloon it was, so with more than one
    // the window is raised without guessing.
    QSet<QString> m_balloonRooms;
    // A balloon was shown and not clicked since: the platform may still be
    // showing it, so the next one takes it down first.
    bool m_balloonMayBeVisible = false;
    // The balloon on screen announces a ringing call: it is raised once per
    // call and cannot be raised again, so not even a mention replaces it.
    bool m_balloonIsCall = false;
    // What arrived while a balloon held the screen, one entry per account
    // and card group (room, thread or keyed notice), oldest first.
    QList<Balloon> m_balloonQueue;
    bool m_trayShowsForTest = false;
    int m_droppedReplacements = 0;
    int m_trayAttempts = 0;
    bool m_lastTrayPlatformSound = false;
    QString m_lastTrayTitle;
    QString m_lastTrayBody;
    int m_trayCarriesSoundOverride = -1;

    // Bounded number of click payloads retained for routing, per account.
    static constexpr int kMaxPendingPayloads = 64;
    static constexpr int kMaxTombstones = 64;

    // Cards we raised and can no longer route, and the account each was for.
    // Process-wide: a tombstone records its own account.
    QHash<quint32, QString> m_tombstones;
    QList<quint32> m_tombstoneOrder;
    // One card per group (room, thread, or keyed notice): its id once the
    // daemon answered, whether a Notify is in flight, the newest card waiting
    // for that answer (a second Notify with replaces_id 0 would raise a
    // duplicate), and how many messages it stands for.
    struct CardGroup {
        quint32 id = 0;
        bool inFlight = false;
        bool withdrawn = false;
        int count = 0;
        // The newest card's payload: what a withdrawal matches before (or
        // while) the daemon answers.
        QVariantMap payload;
        QString title;
        QString body;
        std::optional<Card> next;
        // Who sent the event the card shows: only their own edit may change
        // its text (refreshFromEdit). Empty for generic notices.
        QString sender;
        // The newest edit applied to this card (origin_server_ts), so an
        // older one delivered late cannot bring back superseded text.
        qint64 editStampMs = 0;
    };
    // One account's cards. "" is the foreground account, which is the only
    // table there has ever been: every path without an account argument
    // works on it exactly as before. Daemon ids are unique, so an id is in
    // at most one table.
    struct AccountTable {
        QHash<quint32, QVariantMap> pendingPayloads;
        // Insertion order of pendingPayloads, oldest first (QHash is
        // unordered).
        QList<quint32> payloadOrder;
        QHash<QString, CardGroup> groups;
        // Bumped by clearPendingFor(), so a reply for a card raised before
        // the clear is not routed (and its card is closed).
        quint64 deliveryGeneration = 0;
    };
    // Never shrinks: a cleared table keeps its generation, or a reply from
    // before the clear would match a fresh table's 0. Bounded by accounts.
    // References into it are held only across code that adds no key.
    QHash<QString, AccountTable> m_tables;
    AccountTable &table(const QString &accountKey)
    { return m_tables[accountKey]; }
    // The table holding `id`, or null.
    AccountTable *tableHolding(quint32 id);
    // The click payload `id` names in any table; empty when none does.
    QVariantMap payloadFor(quint32 id) const;
    // Activation tokens by notification id, until the action they precede.
    QHash<quint32, QString> m_activationTokens;
    bool m_assumeWayland = false;

    QElapsedTimer m_clock;
    // Monotonic time of the last sound and of the last mention sound;
    // coalesces bursts into one alert, though a mention breaks through an
    // ordinary burst.
    qint64 m_lastSoundMs = 0;
    qint64 m_lastMentionSoundMs = 0;
    bool m_soundPlayed = false;
    bool m_mentionSoundPlayed = false;
    SoundSource m_soundSource = SourceLightning;
    std::function<bool(bool)> m_ownSound;
    std::function<bool()> m_soundAllowed;
    bool m_daemonInhibited = false;
    bool m_bannersOff = false;
    bool m_inhibitionQueried = false;
    qint64 m_inhibitionQueriedAtMs = 0;
    /// Decides and, for Lightning's chime, plays: called once per delivery,
    /// at the point it leaves for the daemon or the tray. `pathCarriesSound`:
    /// the delivery can make the platform's sound at all.
    SoundPlan takeSoundPlan(bool sound, bool mention, bool pathCarriesSound);
    // Escapes a body iff the daemon advertises body-markup. The only place a
    // notification body is escaped; callers pass raw text. Needs the
    // capabilities.
    QString bodyForServer(const QString &body) const;

    // Whether the server renders body markup. Queried once and cached.
    bool m_bodyMarkup = false;
    bool m_bodyMarkupKnown = false;
    // Whether the daemon advertises `inline-reply` (same GetCapabilities call).
    bool m_inlineReply = false;
    bool m_capabilityRequestInFlight = false;
    int m_capabilityRequests = 0;
    qint64 m_capabilityRetryAtMs = 0;
    int m_capabilityRetryMs = 30000;
    QList<Card> m_capabilityWaits;
    bool m_callWaitsForCapabilities = false;
    // GetServerInformation (R13). Asked again with each capability request
    // until it answers; an answer that is not Plasma's leaves cards plain.
    bool m_serverInfoInFlight = false;
    bool m_serverInfoKnown = false;
    bool m_residentCards = false;
    // The call card's Notify is in flight; a redraw meanwhile waits for it.
    bool m_callNotifyInFlight = false;
    bool m_callRedrawPending = false;
    // Bumped when the daemon changes hands: a reply from the old one names an
    // id the new one may have given to another application.
    quint64 m_daemonGeneration = 0;
    int m_notifyReplies = 0;
    // Rooms with a card raised since the room list last showed them unread,
    // and until when (monotonic) a "read" from the list is not believed.
    QHash<QString, qint64> m_roomsAwaitingUnread;
    int m_awaitUnreadMs = 5000;
    QString m_accountUserId;

    // Undecryptable messages held for their key, and the undecryptable events
    // seen live this session (so a late decryption is decided once, and a
    // history backfill is not).
    struct HeldUtd {
        std::shared_ptr<const TimelineEvent> event;
        Context context;
        qint64 dueMs = 0;
    };
    QList<HeldUtd> m_heldUtds;
    QTimer m_utdHoldTimer;
    int m_utdHoldMs = 3000;
    // Quiet: did not notify; Held: waiting for its key; Delivered: shown as
    // "Encrypted message"; Decided: its plaintext was decided, never again.
    enum class UtdState : quint8 { Quiet, Held, Delivered, Decided };
    struct LiveUtd {
        UtdState state = UtdState::Quiet;
        qint64 seenMs = 0;
    };
    QHash<QString, LiveUtd> m_liveUtds;
    QList<QString> m_liveUtdOrder;

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
    // The daemon expired the card (reason 1): it is in the history, not on
    // screen. Its id is kept so the end of the call can withdraw it, but it
    // is never redrawn.
    bool m_activeCallCardExpired = false;
    // Whether this call was already announced through the tray. Reset when
    // the call changes, never by a re-announcement of the same call.
    bool m_activeCallTrayDelivered = false;
    int m_callTrayAttempts = 0;
    QVariantMap m_lastCallTrayPayload;
    qint64 m_callRingDeadlineMs = 0;
    QTimer m_callRingTimer;
    int m_genericNoticeCount = 0;
    std::function<QImage(const QString &, bool)> m_avatarImage;
    std::function<bool(const QString &)> m_avatarFailed;
    // Arrival order across accounts; each card carries its accountKey, and
    // the cap (kMaxPendingPayloads) and its oldest-first flush are per
    // account.
    QList<Card> m_avatarWaits;
    QTimer m_avatarWaitTimer;
};
