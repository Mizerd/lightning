#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class MatrixClient;
class TimelineModel;

// Automatic SDK-backed read receipts.
//
// The UI reports coarse visibility (window active, timeline visible, near the
// bottom); the coordinator combines it with the newest receipt-eligible event
// and sends one receipt through MatrixClient::sendReadReceipt after a short
// debounce, if every condition still holds at fire time:
//   1. a room is open;
//   2. the window is active;
//   3. the timeline is visible;
//   4. the user is at or near the newest messages;
//   5. the newest readable event has a real remote event id (no local echoes,
//      failed sends or virtual rows);
//   6. that event has not already received the same or a newer receipt.
//
// Room switches, focus loss, scrolling up, resets and sign-out cancel or
// re-validate a pending receipt; a generation counter keeps a stale timer
// from acking into another room. Never logs event bodies.
class ReadReceiptCoordinator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool windowActive READ windowActive WRITE setWindowActive
                   NOTIFY inputsChanged)
    Q_PROPERTY(bool timelineVisible READ timelineVisible WRITE setTimelineVisible
                   NOTIFY inputsChanged)
    Q_PROPERTY(bool nearBottom READ nearBottom WRITE setNearBottom
                   NOTIFY inputsChanged)

public:
    explicit ReadReceiptCoordinator(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setTimelineModel(TimelineModel *model);

    bool windowActive() const { return m_windowActive; }
    void setWindowActive(bool active);
    bool timelineVisible() const { return m_timelineVisible; }
    void setTimelineVisible(bool visible);
    bool nearBottom() const { return m_nearBottom; }
    void setNearBottom(bool nearBottom);

    // Re-run the eligibility check now (e.g. after an explicit user
    // action). Still debounced and validated like any other trigger.
    Q_INVOKABLE void reevaluate();

    // Test hooks.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }
    bool receiptPending() const { return m_debounce.isActive(); }

Q_SIGNALS:
    void inputsChanged();
    // A receipt was handed to the backend: room, event id and timestamp, all a
    // consumer mirroring "read up to here" needs.
    void receiptSent(const QString &roomId, const QString &eventId,
                     qint64 timestampMs);

private Q_SLOTS:
    void onDebounceElapsed();
    void onRoomChanged();
    void onLoggedOut();

private:
    struct SentReceipt {
        QString eventId;
        qint64 timestampMs = 0;
    };

    // Newest eligible event, or empty when conditions do not allow one.
    QString eligibleEventId(qint64 *timestampMs) const;
    bool conditionsHold() const;
    void sendNow(const QString &eventId, qint64 timestampMs);

    MatrixClient *m_client = nullptr;
    TimelineModel *m_model = nullptr;

    bool m_windowActive = false;
    bool m_timelineVisible = false;
    bool m_nearBottom = false;

    QTimer m_debounce;
    // Room + generation captured when the debounce was armed; the timer
    // refuses to fire into anything else.
    QString m_armedRoomId;
    quint64 m_generation = 0;
    quint64 m_armedGeneration = 0;
    QString m_armedEventId;

    // Last receipt handed to the backend, per room, with its origin
    // timestamp so an older event can never overwrite a newer receipt.
    QHash<QString, SentReceipt> m_lastSent;
};
