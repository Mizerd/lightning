#pragma once

#include <utility>

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

struct TimelineEvent;
class TrayIcon;

class NotificationManager : public QObject
{
    Q_OBJECT
public:
    bool lastClickHadActivationToken() { return std::exchange(m_lastClickHadToken, false); }

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
    enum SoundMode { SoundOff = 0, SoundMentionsAndDirect = 1, SoundAll = 2 };
    Q_ENUM(SoundMode)
    enum SoundSource { SourceLightning = 0, SourceSystem = 1 };
    Q_ENUM(SoundSource)

    struct SoundPlan {
        bool playOwn = false;
        bool platformSound = false;
        /// The notification carries `suppress-sound`, so no daemon adds one
        /// of its own. True whenever the platform sound is not wanted.
        bool suppressPlatform = true;
    };
    static SoundPlan planSound(bool allowed, bool coalesced,
                               SoundSource source);
    static QVariantMap soundHints(const SoundPlan &plan);

    struct CardShape {
        QStringList actions;
        uchar urgency = 1;
        bool inlineReply = false;
    };
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

    struct DesktopIdentity {
        QString desktopEntry;
        QString appIcon;
    };
    static DesktopIdentity desktopIdentityFor(const QString &flatpakId,
                                              const QString &snapDir,
                                              const QString &snapName);

    struct Context {
        QString selfUserId;
        QString roomName;
        bool roomIsDirect = false;
        RoomMode roomMode = AllMessages;
        PreviewMode previewMode = SenderOnly;
        bool notificationsEnabled = true;
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
        bool senderIsIgnored = false;
        QString senderMemberName;
        QString senderProfileName;
        bool localModeUnsynced = false;
        QString avatarMxc;
        QString avatarColorKey;
        int themeId = 0;
        QString accountUserId;
        QString accountLabel;
        friend bool operator==(const Context &, const Context &) = default;
    };
    struct Decision {
        bool notify = false;
        QString title;
        QString body;
        bool playSound = false;
        // A mention: the mention chime and the card's mention flag. With the
        // account's rules known it is their highlight alone; without them,
        // m.mentions naming this user (never m.mentions.room alone, which
        // carries no power-level check).
        bool mention = false;
    };

    explicit NotificationManager(QObject *parent = nullptr);
    void setAvatarProvider(
        std::function<QImage(const QString &, bool request)> image,
        std::function<bool(const QString &)> failed);
    void avatarCacheChanged();
    /// The account notifications are raised for. Recorded in every click
    /// payload so actions taken after an account switch can be refused.
    void setAccountUserId(const QString &userId)
    {
        m_accountUserId = userId;
        if (m_liveAccountsMode)
            noteAccountUsed(userId);
    }
    QString accountUserId() const { return m_accountUserId; }

    void setLiveAccountsMode(bool on);
    bool liveAccountsMode() const { return m_liveAccountsMode; }
    void setAccountLabelsShown(bool on);
    bool accountLabelsShown() const { return m_accountLabelsShown; }
    void observeRoomUnreadFor(const QString &accountUserId,
                              const QString &roomId, bool unread);
    void setHostCardIntervalMsForTest(int ms) { m_hostCardIntervalMs = ms; }
    int deferredHostCardCountForTest() const
    { return int(m_hostCardQueue.size()); }
    QString eventCardTitleForTest(const QString &roomId,
                                  const QString &eventId) const;
    QStringList eventCardAccountsForTest(const QString &roomId,
                                         const QString &eventId) const;
    void setSoundSource(int source);
    SoundSource soundSource() const { return m_soundSource; }
    bool playOwnSoundForTest(bool mention) const
    { return m_ownSound && m_ownSound(mention); }
    void setOwnSoundPlayer(std::function<bool(bool mention)> player);
    /// The call-state policy for any notification sound, Lightning's or the
    /// desktop's: false while a call or a share that captures the output mix
    /// must stay quiet. Consulted before either source plays. Once installed,
    /// a chime the player then cannot play falls back to the desktop's sound
    /// (without it a refusal by the player could be the policy, not a fault).
    void setSoundAllowedCheck(std::function<bool()> check);
    bool desktopInhibited() const
    { return m_daemonInhibited || m_bannersOff; }
    bool inlineReplySupported() const { return m_inlineReply; }

    static Decision decide(const TimelineEvent &event, const Context &context);
    static QString senderLabel(const TimelineEvent &event,
                               const Context &context);

    static bool shouldNotifyInvite(bool initialSyncComplete, bool alreadyKnown,
                                   bool notificationsEnabled);

    // Decide and (when positive) deliver natively. The click payload is
    // {roomId, eventId, threadRootId} only. A message that could not be
    // decrypted yet is held briefly for its key; see processDecrypted().
    void processEvent(const TimelineEvent &event, const Context &context);

    void processDecrypted(const TimelineEvent &event, const Context &context);

    /// Refresh an existing card from an SDK-aggregated row, silently. Never
    /// raises a card for an event this manager has not already announced.
    /// (A room with no open timeline has no such row: its edits reach
    /// processEvent() raw, which refreshes the card the same way.)
    void processUpdated(const TimelineEvent &event, const Context &context);

    void withdrawEvent(const QString &eventId);

    // Generic non-message notifications (invites, verification requests).
    // The body must already be safe — callers never pass message content.
    // `sound` asks for the message sound plan (the same coalescing, policy
    // and source). A non-empty `key` names the card: showing the same key
    // again replaces it, and withdrawGeneric() withdraws it.
    // `accountUserId` stamps the card for that account (a live account's
    // notice: a click opens that account); empty is the foreground's, as
    // every caller before live accounts.
    void showGeneric(const QString &title, const QString &safeBody,
                     const QString &roomId = QString(),
                     const QString &avatarMxc = QString(),
                     bool sound = false, const QString &key = QString(),
                     const QString &accountUserId = QString());
    void withdrawGeneric(const QString &key);

    void showIncomingCall(const QString &roomId, const QString &callId,
                          const QString &title, const QString &safeBody,
                          bool sound, int ringSeconds,
                          bool acceptOffered = false, bool rtcLane = false,
                          bool silenceOffered = false);

    static QStringList callActions(bool acceptOffered, bool rtcLane,
                                   bool silenceOffered = false);
    static QVariantMap callSoundHints(bool sound);
    void setCallAcceptOffered(const QString &callId, bool offered,
                              bool rtcLane);

    void silenceIncomingCall(const QString &callId);

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
    void clearPending() { clearPendingFor(QString()); }

    void recordPayloadForTest(quint32 id, const QVariantMap &payload)
    { recordPayload(QString(), id, payload); }
    void recordPayloadForTest(const QString &accountUserId, quint32 id,
                              const QVariantMap &payload)
    { recordPayload(accountUserId, id, payload); }
    int hostUnreadPassesForTest() const { return m_hostUnreadPasses; }
    int pendingPayloadCountForTest() const
    { return pendingPayloadCountForTest(QString()); }
    int pendingPayloadCountForTest(const QString &accountUserId) const;
    quint64 deliveryGenerationForTest(const QString &accountUserId) const;
    void notifyReplyForTest(const QString &accountUserId, quint64 generation,
                            quint32 id, const QVariantMap &payload);
    bool callRingActiveForTest() const { return m_callRingTimer.isActive(); }
    bool callSoundActiveForTest() const { return m_activeCallSound; }
    bool callSilenceOfferedForTest() const
    { return m_activeCallSilenceOffered; }
    QString activeCallIdForTest() const { return m_activeCallId; }
    quint32 activeCallNotificationIdForTest() const
    { return m_activeCallNotificationId; }
    void setActiveCallNotificationIdForTest(quint32 id)
    { m_activeCallNotificationId = id; m_activeCallCardExpired = false; }
    int genericNoticeCountForTest() const { return m_genericNoticeCount; }
    void setFallbackTray(TrayIcon *tray);
    void deliverThroughTrayForTest(const QVariantMap &payload)
    { deliverThroughTrayForTest(QString(), payload); }
    void deliverThroughTrayForTest(const QString &accountUserId,
                                   const QVariantMap &payload);
    void setTrayShowsForTest(bool shows) { m_trayShowsForTest = shows; }
    void setBalloonWithdrawableForTest(bool withdrawable)
    { m_balloonWithdrawableOverride = withdrawable ? 1 : 0; }
    void setBalloonHoldMsForTest(int ms) { m_balloonHoldOverrideMs = ms; }
    bool balloonOnScreenForTest() const { return m_balloonHold.isActive(); }
    int pendingBalloonEntriesForTest() const
    { return int(m_balloonQueue.size()); }
    int pendingBalloonMessagesForTest() const;
    QVariantMap lastTrayPayloadForTest() const { return m_lastTrayPayload; }
    int callTrayAttemptsForTest() const { return m_callTrayAttempts; }
    QVariantMap lastCallTrayPayloadForTest() const
    { return m_lastCallTrayPayload; }
    int trayAttemptsForTest() const { return m_trayAttempts; }
    bool lastTrayPlatformSoundForTest() const
    { return m_lastTrayPlatformSound; }
    QString lastTrayTitleForTest() const { return m_lastTrayTitle; }
    QString lastTrayBodyForTest() const { return m_lastTrayBody; }
    void setTrayCarriesSoundForTest(bool carries)
    { m_trayCarriesSoundOverride = carries ? 1 : 0; }
    int avatarWaitCountForTest() const
    { return avatarWaitCount(QString()); }
    int avatarWaitCountForTest(const QString &accountUserId) const
    { return avatarWaitCount(accountUserId); }
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
    int droppedReplacementsForTest() const { return m_droppedReplacements; }
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
    bool avatarWaitHasImageForTest(int index) const
    {
        const Card *card = foregroundAvatarWait(index);
        return card && (!card->avatarMxc.isEmpty() || !card->fallback.isNull()
                        || !card->avatar.isNull());
    }
    int heldUndecryptableCountForTest() const { return m_heldUtds.size(); }
    void setUndecryptableHoldMsForTest(int ms) { m_utdHoldMs = ms; }
    void setDesktopInhibitedForTest(bool inhibited)
    { m_daemonInhibited = inhibited; }
    int notifyRepliesProcessedForTest() const { return m_notifyReplies; }
    void setAssumeWaylandForTest(bool wayland) { m_assumeWayland = wayland; }

    // ── Composite thread-timeline ids never leave this class ──
    // Thread replies arrive with the composite timeline id in
    // TimelineEvent::roomId. These reduce it to routable identity; both are
    // no-ops for real room ids.
    static QString routableRoomId(const QString &roomId);
    static QString routableThreadRootId(const QString &roomId,
                                        const QString &threadRootId);

Q_SIGNALS:
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);
    void openRequestedForAccount(const QString &accountUserId,
                                 const QString &roomId, const QString &eventId,
                                 const QString &threadRootId);
    void callDeclineRequested(const QString &callId);
    void callAcceptRequested(const QString &callId);
    void callSilenceRequested(const QString &callId);
    /// The user dismissed the ringing call card (freedesktop reason 2). Not a
    /// decline: other devices keep ringing; the app stops its own ringer.
    void callCardDismissed(const QString &callId);
    void messageDelivered(const QString &roomId, bool mention);
    void markReadRequested(const QString &accountUserId, const QString &roomId,
                           const QString &eventId);
    void replyRequested(const QString &accountUserId, const QString &roomId,
                        const QString &threadRootId, const QString &text);

private Q_SLOTS:
    void onActionInvoked(quint32 id, const QString &action);
    void onNotificationReplied(quint32 id, const QString &text);
    void onNotificationClosed(quint32 id, quint32 reason);
    void onActivationToken(quint32 id, const QString &token);
    void onDaemonPropertiesChanged(const QString &interfaceName,
                                   const QVariantMap &changed,
                                   const QStringList &invalidated);
    // The notification service changed hands (daemon restart): every id it
    // gave out is meaningless now.
    void onDaemonOwnerChanged(const QString &service, const QString &oldOwner,
                              const QString &newOwner);
    void onFallbackMessageClicked();
    void onCallRingTick();

private:
    struct Card {
        QString title;
        QString body;
        QVariantMap payload;
        bool sound = false;
        QImage avatar;
        QString avatarMxc;
        QImage fallback;
        bool replacement = false;
        QString accountKey;
    };
    void deliver(Card card);
    bool replacementStillOwned(const Card &card) const;
    void deliverNow(Card card);
    void refreshCard(const TimelineEvent &event, const Context &context);
    bool refreshFromEdit(const TimelineEvent &edit, const Context &context);
    void deliverDecision(const TimelineEvent &event, const Context &context,
                         const Decision &decision, bool undecryptable,
                         bool replacement);
    void deliverCallNotification();
    void flushAvatarWaits(bool fallbackAll);
    void flushHeldUndecryptable();
    void recordPayload(const QString &accountKey, quint32 id,
                       const QVariantMap &payload);
    bool takeNotifyReply(const Card &card, const QString &groupKey,
                         quint64 generation, std::optional<quint32> id);
    int avatarWaitCount(const QString &accountKey) const;
    const Card *foregroundAvatarWait(int index) const;

    QString accountOfKey(const QString &accountKey) const
    { return accountKey.isEmpty() ? m_accountUserId : accountKey; }
    static QString accountLabelOr(const QString &label, const QString &userId);
    QString accountSuffix(const QStringList &labels) const;
    QString labelledTitleForRefresh(const TimelineEvent &event, const Context &context,
                                    const QString &baseTitle);
    bool mergeIntoEventCard(const TimelineEvent &event, const Context &context,
                            const QString &roomId, const Decision &decision);
    bool hasCardsFor(const QString &accountKey) const;
    void recordEventCard(const QString &eventKey, const QString &accountKey,
                         const QString &groupKey, const QString &label,
                         const QString &baseTitle, const Card &card);
    void offerHostCard(Card card, const QString &groupKey, bool groupWasEmpty);
    void flushHostCards();
    void armHostCardTimer();
    bool deliveryReplacesInPlace() const;
    void noteAccountUsed(const QString &userId);
    QString openAccountFor(const QVariantMap &payload) const;
    void forgetLiveAccountsStateFor(const QString &accountKey);
public:
    void closeRoomNotifications(const QString &roomId);
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
    void forgetPayload(quint32 id);
    void closeNotification(quint32 id);
    int withdrawWhere(const QString &accountKey,
                      const std::function<bool(const QVariantMap &)> &match);
    int withdrawEverywhere(
        const std::function<bool(const QVariantMap &)> &match);
    void tombstone(quint32 id, const QString &accountUserId);
    void handleStaleAction(quint32 id, const QString &action,
                           const QString &text);
    void showStaleActionNotice();
    bool requestCapabilities();
    void requestServerInformation();
    void releaseCapabilityWaits();
    void queryDesktopInhibition();
    void emitOpenFor(const QVariantMap &payload);
    void applyActivationToken(quint32 id);
    bool deliverCallThroughTray();
    bool deliverThroughTray(const QString &title, const QString &body,
                            const QVariantMap &payload, const QImage &avatar,
                            bool platformSound, const QString &accountKey);
    void noteBalloon(const QString &accountKey, const QVariantMap &payload);
    struct Balloon {
        QString title;
        QString body;
        QVariantMap payload;
        QImage avatar;
        bool platformSound = false;
        QString accountKey;
        int messages = 1;
    };
    bool offerBalloon(Balloon balloon, bool breakThrough);
    bool balloonGate() const;
    void enqueueBalloon(Balloon balloon, bool older);
    void requeueDisplacedBalloon();
    void showPendingBalloons();
    void onBalloonHoldExpired();
    void retireBalloonOnScreen();
    bool trayCarriesSound() const;
    qint64 nowMs() const { return m_clock.elapsed(); }

    QPointer<TrayIcon> m_fallbackTray;
    QVariantMap m_lastFallbackPayload;
    QString m_lastFallbackAccountKey;
    QVariantMap m_lastTrayPayload;
    QTimer m_balloonHold;
    int m_balloonHoldOverrideMs = -1;
    int m_balloonWithdrawableOverride = -1;
    QList<Balloon> m_shownEntries;
    QSet<QString> m_balloonRooms;
    bool m_balloonMayBeVisible = false;
    bool m_balloonIsCall = false;
    QList<Balloon> m_balloonQueue;
    bool m_trayShowsForTest = false;
    int m_droppedReplacements = 0;
    int m_trayAttempts = 0;
    bool m_lastTrayPlatformSound = false;
    QString m_lastTrayTitle;
    QString m_lastTrayBody;
    int m_trayCarriesSoundOverride = -1;

    static constexpr int kMaxPendingPayloads = 64;
    static constexpr int kMaxTombstones = 64;

    QHash<quint32, QString> m_tombstones;
    QList<quint32> m_tombstoneOrder;
    struct CardGroup {
        quint32 id = 0;
        bool inFlight = false;
        bool withdrawn = false;
        int count = 0;
        QVariantMap payload;
        QString title;
        QString body;
        std::optional<Card> next;
        QString sender;
        // The newest edit applied to this card (origin_server_ts), so an
        // older one delivered late cannot bring back superseded text.
        qint64 editStampMs = 0;
    };
    struct AccountTable {
        QHash<quint32, QVariantMap> pendingPayloads;
        QList<quint32> payloadOrder;
        QHash<QString, CardGroup> groups;
        quint64 deliveryGeneration = 0;
    };
    // Never shrinks: a cleared table keeps its generation, or a reply from
    // before the clear would match a fresh table's 0. Bounded by accounts.
    // References into it are held only across code that adds no key.
    QHash<QString, AccountTable> m_tables;
    AccountTable &table(const QString &accountKey)
    { return m_tables[accountKey]; }
    AccountTable *tableHolding(quint32 id);
    QVariantMap payloadFor(quint32 id) const;
    QHash<quint32, QString> m_activationTokens;
    bool m_lastClickHadToken = false;
    bool m_assumeWayland = false;

    QElapsedTimer m_clock;
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
    SoundPlan takeSoundPlan(bool sound, bool mention, bool pathCarriesSound);
    // Escapes a body iff the daemon advertises body-markup. The only place a
    // notification body is escaped; callers pass raw text. Needs the
    // capabilities.
    QString bodyForServer(const QString &body) const;

    bool m_bodyMarkup = false;
    bool m_bodyMarkupKnown = false;
    bool m_inlineReply = false;
    bool m_capabilityRequestInFlight = false;
    int m_capabilityRequests = 0;
    qint64 m_capabilityRetryAtMs = 0;
    int m_capabilityRetryMs = 30000;
    QList<Card> m_capabilityWaits;
    bool m_callWaitsForCapabilities = false;
    bool m_serverInfoInFlight = false;
    bool m_serverInfoKnown = false;
    bool m_residentCards = false;
    bool m_callNotifyInFlight = false;
    bool m_callRedrawPending = false;
    // Bumped when the daemon changes hands: a reply from the old one names an
    // id the new one may have given to another application.
    quint64 m_daemonGeneration = 0;
    int m_notifyReplies = 0;
    QHash<QString, qint64> m_roomsAwaitingUnread;
    int m_awaitUnreadMs = 5000;
    QString m_accountUserId;

    struct HeldUtd {
        std::shared_ptr<const TimelineEvent> event;
        Context context;
        qint64 dueMs = 0;
    };
    QList<HeldUtd> m_heldUtds;
    QTimer m_utdHoldTimer;
    int m_utdHoldMs = 3000;
    enum class UtdState : quint8 { Quiet, Held, Delivered, Decided };
    struct LiveUtd {
        UtdState state = UtdState::Quiet;
        qint64 seenMs = 0;
    };
    QHash<QString, LiveUtd> m_liveUtds;
    QList<QString> m_liveUtdOrder;

    QString m_activeCallId;
    QString m_activeCallRoomId;
    QString m_activeCallTitle;
    QString m_activeCallBody;
    bool m_activeCallSound = false;
    bool m_activeCallAcceptOffered = false;
    bool m_activeCallRtcLane = false;
    bool m_activeCallSilenceOffered = false;
    quint32 m_activeCallNotificationId = 0;
    // The daemon expired the card (reason 1): it is in the history, not on
    // screen. Its id is kept so the end of the call can withdraw it, but it
    // is never redrawn.
    bool m_activeCallCardExpired = false;
    bool m_activeCallTrayDelivered = false;
    int m_callTrayAttempts = 0;
    QVariantMap m_lastCallTrayPayload;
    qint64 m_callRingDeadlineMs = 0;
    QTimer m_callRingTimer;
    int m_genericNoticeCount = 0;
    std::function<QImage(const QString &, bool)> m_avatarImage;
    std::function<bool(const QString &)> m_avatarFailed;
    QList<Card> m_avatarWaits;
    QTimer m_avatarWaitTimer;

    bool m_liveAccountsMode = false;
    bool m_accountLabelsShown = false;
    int m_hostCardIntervalMs = 2000;
    qint64 m_lastHostNewCardMs = -1;
    struct DeferredHostCard {
        QString groupKey;
        Card card;
    };
    QList<DeferredHostCard> m_hostCardQueue;
    std::unique_ptr<QTimer> m_hostCardTimer;
    struct EventCard {
        QString ownerKey;
        QString groupKey;
        QStringList accounts;
        QStringList labels;
        QString baseTitle;
        Card card;
    };
    QHash<QString, EventCard> m_eventCards;
    QList<QString> m_eventCardOrder;
    QStringList m_accountRecency;
    QHash<QString, qint64> m_hostRoomsAwaitingUnread;
    QHash<QString, quint64> m_cardGeneration;
    QHash<QString, quint64> m_hostRoomsCheckedAt;
    int m_hostUnreadPasses = 0;
    void noteCardFor(const QString &accountKey)
    {
        if (m_liveAccountsMode && !accountKey.isEmpty())
            ++m_cardGeneration[accountKey];
    }
};
