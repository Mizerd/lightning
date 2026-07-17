#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

struct TimelineEvent;

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
    enum RoomMode { AllMessages = 0, MentionsOnly = 1, Muted = 2 };
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
        // v0.6.1: false while the client is still applying its initial sync
        // backlog. Events that arrive before the first sync completes are
        // pre-existing history, not fresh activity, and must never raise a
        // native notification — otherwise every backlog message re-notifies
        // on each launch (the 0.6.0 cold-start storm). Defaults true so the
        // Mock backend and pure-policy tests notify unless told otherwise.
        bool initialSyncComplete = true;
        SoundMode soundMode = SoundMentionsAndDirect;
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
                     const QString &roomId = QString());

    // Logout/account switch: forget queued click payloads.
    void clearPending();

    // Test hook: record a delivered notification's click payload exactly as
    // deliver() would, exercising the bounded FIFO eviction without a live
    // DBus service. Not used in production paths.
    void recordPayloadForTest(quint32 id, const QVariantMap &payload)
    { recordPayload(id, payload); }
    int pendingPayloadCountForTest() const { return m_pendingPayloads.size(); }

Q_SIGNALS:
    // The user activated a notification. Identity only — no tokens.
    void openRequested(const QString &roomId, const QString &eventId,
                       const QString &threadRootId);

private Q_SLOTS:
    // DBus signal receivers (freedesktop Notifications).
    void onActionInvoked(quint32 id, const QString &action);
    void onNotificationClosed(quint32 id, quint32 reason);

private:
    void deliver(const QString &title, const QString &body,
                 const QVariantMap &payload, bool sound = false);
    // Store one click payload, evicting the oldest when the bounded cap is
    // exceeded (a desktop only keeps a handful visible). FIFO eviction keeps
    // the most recent notifications clickable instead of dropping them all.
    void recordPayload(quint32 id, const QVariantMap &payload);
    void forgetPayload(quint32 id);

    // Bounded number of click payloads retained for routing.
    static constexpr int kMaxPendingPayloads = 64;

    QHash<quint32, QVariantMap> m_pendingPayloads;
    // Insertion order of the ids in m_pendingPayloads, oldest first, so the
    // eviction can drop the least-recent entry (QHash is unordered).
    QList<quint32> m_payloadOrder;
    // Monotonic time (ms) of the last sound played; a short window coalesces
    // notification bursts into a single alert.
    qint64 m_lastSoundMs = 0;
};
