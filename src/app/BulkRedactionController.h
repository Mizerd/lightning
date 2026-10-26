#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

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

signals:
    void changed();

private:
    void add(const QString &eventId, bool canRedactOthers);
    void sendNext();
    void finish();
    void onResult(const QString &roomId, const QString &eventId,
                  bool ok, const QString &error);

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
    QSet<QString> m_abandonedRequests;
    bool m_selecting = false;
    bool m_running = false;
    bool m_cancelled = false;
    int m_completed = 0;
    int m_total = 0;
    int m_failed = 0;
    quint64 m_generation = 0;
};
