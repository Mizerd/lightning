#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class MatrixClient;
class TimelineModel;

// One selected timeline and one in-flight Matrix request. A result, not a
// timeline diff, advances the queue: sync may arrive much later or not at all.
class BulkRedactionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool selecting READ selecting NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString timelineId READ timelineId NOTIFY changed)
public:
    explicit BulkRedactionController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);
    bool selecting() const { return m_selecting; }
    bool running() const { return m_running; }
    int selectedCount() const { return m_ids.size(); }
    int completed() const { return m_completed; }
    int total() const { return m_total; }
    int failed() const { return m_failed; }
    bool cancelled() const { return m_cancelled; }
    QString error() const { return m_error; }
    QString timelineId() const { return m_timelineId; }

    Q_INVOKABLE void begin(TimelineModel *model);
    Q_INVOKABLE void toggle(const QString &eventId, bool canRedactOthers);
    Q_INVOKABLE void toggleFromPointer(const QString &eventId,
                                       bool canRedactOthers);
    Q_INVOKABLE void selectRange(const QString &first, const QString &last,
                                  bool canRedactOthers);
    Q_INVOKABLE bool isSelected(const QString &eventId) const;
    Q_INVOKABLE void start(const QString &reason);
    Q_INVOKABLE void cancel();

    /// A request with no result within this window counts as failed and the
    /// run moves on; its late result is ignored. Default 60 s.
    void setRequestTimeoutForTest(int ms) { m_requestTimeoutMs = ms; }
    /// How long an abandoned request (cancelled or timed out while on the
    /// wire) keeps blocking a re-selection of its message. Default 10 min.
    void setAbandonedExpiryForTest(qint64 ms) { m_abandonedExpiryMs = ms; }

signals:
    void changed();

private:
    void add(const QString &eventId, bool canRedactOthers);
    void sendNext();
    void finish();
    void onResult(const QString &roomId, const QString &eventId,
                  bool ok, const QString &error);
    void onRequestTimeout();
    void abandon(const QString &key, bool alreadyCounted);
    bool isAbandoned(const QString &key);
    void pruneAbandoned();

    QPointer<MatrixClient> m_client;
    QPointer<TimelineModel> m_model;
    QString m_roomId;
    QString m_timelineId;
    QStringList m_ids;
    QStringList m_queue;
    QString m_inFlight;
    QString m_reason;
    QString m_anchor;
    QString m_error;
    /// Request key -> when it was abandoned (ms since epoch). Expired by
    /// pruneAbandoned(): a result that never comes must not block that
    /// message for the rest of the session.
    struct Abandoned {
        qint64 atMs = 0;
        /// A timed-out request was already counted as failed; its late
        /// result must not be counted again.
        bool counted = false;
    };
    QHash<QString, Abandoned> m_abandonedRequests;
    QTimer m_requestTimer;
    int m_requestTimeoutMs = 60000;
    qint64 m_abandonedExpiryMs = 10 * 60 * 1000;
    bool m_selecting = false;
    bool m_running = false;
    bool m_cancelled = false;
    int m_completed = 0;
    int m_total = 0;
    int m_failed = 0;
    quint64 m_generation = 0;
};
