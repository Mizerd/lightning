#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class MatrixClient;
class TimelineModel;

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

    Q_INVOKABLE void reevaluate();

    // Advance m.fully_read for the open room to the newest event the reader
    // was shown, if that moved since the last write. Called internally when
    // the reader leaves; exposed for sign-out, which must flush BEFORE the
    // session ends (loggedOut arrives too late to write anything).
    Q_INVOKABLE void flushFullyReadMarker();

    static constexpr int kMaxReceiptRetries = 4;

    void setDebounceMs(int ms) { m_debounce.setInterval(ms); }
    void setRetryBaseMs(int ms) { m_retryBaseMs = ms; }
    bool receiptPending() const { return m_debounce.isActive(); }
    bool retryPending() const { return m_retry.isActive(); }

Q_SIGNALS:
    void inputsChanged();
    void receiptSent(const QString &roomId, const QString &eventId,
                     qint64 timestampMs);
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
    struct CountAtSend {
        QString eventId;
        int count = 0;
    };
    int roomCount(const QString &roomId) const;
    bool roomCountMovedSinceReceipt(const QString &eventId) const;
    void forgetClearedCounts();
    struct InFlightReceipt {
        QString eventId;
        qint64 timestampMs = 0;
        bool viaMarkRead = false;
    };
    struct ReceiptFailure {
        QString eventId;
        int count = 0;
        bool waiting = false;
    };

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
    QString m_armedRoomId;
    quint64 m_generation = 0;
    quint64 m_armedGeneration = 0;
    QString m_armedEventId;

    QHash<QString, SentReceipt> m_lastSent;
    QHash<QString, CountAtSend> m_countAtSend;
    QHash<QString, SentReceipt> m_lastAccepted;
    QHash<QString, InFlightReceipt> m_inFlight;
    QHash<QString, ReceiptFailure> m_failures;
    QTimer m_retry;
    QString m_retryRoomId;
    int m_retryBaseMs = 2000;

    QString m_openRoomId;
    struct SeenEvent {
        QString eventId;
        QString userId;
    };
    QHash<QString, SeenEvent> m_seenUpTo;
    QHash<QString, QString> m_fullyReadWritten;
    QString m_userId;
};
