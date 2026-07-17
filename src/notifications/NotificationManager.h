#pragma once

#include <QHash>
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
    };
    struct Decision {
        bool notify = false;
        QString title;
        QString body;
    };

    explicit NotificationManager(QObject *parent = nullptr);

    // Pure policy — no I/O, no logging. Exposed for tests.
    static Decision decide(const TimelineEvent &event, const Context &context);

    // Decide and (when positive) deliver natively. The click payload is
    // {roomId, eventId, threadRootId} only.
    void processEvent(const TimelineEvent &event, const Context &context);

    // Generic non-message notifications (invites, verification requests).
    // The body must already be safe — callers never pass message content.
    void showGeneric(const QString &title, const QString &safeBody,
                     const QString &roomId = QString());

    // Logout/account switch: forget queued click payloads.
    void clearPending();

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
                 const QVariantMap &payload);

    QHash<quint32, QVariantMap> m_pendingPayloads;
};
