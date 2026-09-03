#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class MatrixClient;
class TimelineModel;

// v0.5.11: automatic SDK-backed read receipts.
//
// The UI reports coarse visibility state (window active, timeline visible,
// user near the bottom); the coordinator combines that with the newest
// receipt-eligible event from the TimelineModel and sends ONE receipt
// through MatrixClient::sendReadReceipt — but only after the state has held
// for a brief debounce period and every condition still holds at fire time.
//
// A receipt is eligible only when, simultaneously:
//   1. a room is open (model has a room id);
//   2. the application window is active;
//   3. the timeline is visible;
//   4. the user is at/near the newest messages;
//   5. the newest readable event has a real remote event id (no local
//      echoes without one, no failed sends, no virtual rows);
//   6. that event has not already received the same or a newer receipt.
//
// Room switches, focus loss, upward scrolling, timeline resets and sign-out
// during the debounce all cancel or re-validate the pending receipt; a
// bumped generation guarantees a stale timer can never ack into a
// different room. Receipts go through the Matrix SDK (Rust bridge) — this
// class never merely clears a local badge.
//
// Nothing here logs event bodies; only room-free diagnostic state.
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
    // A receipt was handed to the backend. Carries the room, the event id
    // (safe to log by existing convention) and the event's timestamp — a
    // consumer that wants to mirror "read up to here" needs all three, and
    // sendNow() already holds them.
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
