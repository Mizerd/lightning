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
//   6. that event has not received a newer receipt, and either has not received
//      the same receipt or the room's manual unread flag needs clearing.
//
// Room switches, focus loss, scrolling up, resets and sign-out cancel or
// re-validate a pending receipt; a generation counter keeps a stale timer
// from acking into another room. Never logs event bodies.
//
// A receipt is only ANNOUNCED (receiptSent) once the backend reports the
// server accepted it (MatrixClient::readReceiptAccepted, or readMarkerAdvanced
// for the mark-as-read path). A failure (readReceiptFailed /
// markRoomReadFailed) forgets the receipt so the same event is eligible again,
// and re-arms it with a bounded backoff; past the bound it waits for a newer
// event, the window becoming active again, or the room being reopened.
//
// The fully-read marker (m.fully_read, the "New messages" divider's anchor)
// is NOT moved by the receipts themselves: it is advanced to the newest event
// the reader was shown only when they LEAVE the room (room switch, window
// deactivation, account change, or flushFullyReadMarker()). Moving it with
// every receipt removed the divider and the first-unread pill about a second
// after a room opened.
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

    // Advance m.fully_read for the open room to the newest event the reader
    // was shown, if that moved since the last write. Called internally when
    // the reader leaves; exposed for sign-out, which must flush BEFORE the
    // session ends (loggedOut arrives too late to write anything).
    Q_INVOKABLE void flushFullyReadMarker();

    // A failed receipt is retried at most this many times per event, with a
    // doubling delay starting at the retry base.
    static constexpr int kMaxReceiptRetries = 4;

    // Test hooks.
    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }
    void setRetryBaseMs(int ms) { m_retryBaseMs = ms; }
    bool receiptPending() const { return m_debounce.isActive(); }
    bool retryPending() const { return m_retry.isActive(); }

Q_SIGNALS:
    void inputsChanged();
    // The server ACCEPTED a receipt: room, event id and timestamp, all a
    // consumer mirroring "read up to here" needs. Never emitted for a receipt
    // that was merely queued, and never for one that failed.
    void receiptSent(const QString &roomId, const QString &eventId,
                     qint64 timestampMs);
    // This coordinator wrote m.fully_read for `roomId` (the reader left it,
    // or the window lost focus). Whatever unread lay above the loaded rows
    // when the room was opened is read now: the timeline's first-unread pill
    // stops offering a jump to it. Room and event ids only.
    void fullyReadMarkerMoved(const QString &roomId, const QString &eventId);

private Q_SLOTS:
    void onDebounceElapsed();
    void onRetryElapsed();
    void onRoomChanged();
    void onLoggedOut();
    void onLoginSucceeded(const QString &userId);
    void onReceiptAccepted(const QString &roomId, const QString &eventId);
    void onReceiptFailed(const QString &roomId, const QString &eventId);
    void onReadMarkerAdvanced(const QString &roomId);
    void onMarkRoomReadFailed(const QString &roomId);

private:
    struct SentReceipt {
        QString eventId;
        qint64 timestampMs = 0;
    };
    // The receipt handed to the backend for a room and not yet answered.
    struct InFlightReceipt {
        QString eventId;
        qint64 timestampMs = 0;
        // Sent through markRoomRead() (clearing m.marked_unread), which
        // answers with readMarkerAdvanced / markRoomReadFailed.
        bool viaMarkRead = false;
    };
    // Consecutive failures of one event in one room.
    struct ReceiptFailure {
        QString eventId;
        int count = 0;
        // A backoff retry is scheduled; the event is held back until it fires.
        bool waiting = false;
    };

    // Newest eligible event, or empty when conditions do not allow one.
    QString eligibleEventId(qint64 *timestampMs) const;
    bool conditionsHold() const;
    bool roomIsMarkedUnread() const;
    bool clearsMarkedUnread() const;
    bool m_openedMarkedUnread = false;
    void sendNow(const QString &eventId, qint64 timestampMs);
    void acceptInFlight(const QString &roomId);
    void failInFlight(const QString &roomId);
    void flushFullyReadMarkerFor(MatrixClient *client, const QString &roomId);
    void resetReceiptState();

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
    // timestamp so an older event can never overwrite a newer receipt. Holds
    // an in-flight receipt too, so it is not sent twice while unanswered; a
    // failure rolls it back to m_lastAccepted.
    QHash<QString, SentReceipt> m_lastSent;
    QHash<QString, SentReceipt> m_lastAccepted;
    QHash<QString, InFlightReceipt> m_inFlight;
    QHash<QString, ReceiptFailure> m_failures;
    QTimer m_retry;
    QString m_retryRoomId;
    int m_retryBaseMs = 2000;

    // The room the coordinator last saw open (the model has already moved on
    // when roomIdChanged arrives, so the room being LEFT is kept here).
    QString m_openRoomId;
    // Newest event the reader was shown, per room (what a receipt was sent
    // for), with the account that was shown it, and the last value written to
    // m.fully_read from here. The account check refuses a write into another
    // account's session when one client object serves an account switch.
    struct SeenEvent {
        QString eventId;
        QString userId;
    };
    QHash<QString, SeenEvent> m_seenUpTo;
    QHash<QString, QString> m_fullyReadWritten;
    QString m_userId;
};
