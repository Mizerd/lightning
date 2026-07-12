#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QSet>

class MatrixClient;
struct TimelineEvent;

// v0.5.11: backward-pagination policy for the live SDK timeline.
//
// The Rust bridge already single-flights `paginate_backwards` and reports
// loading / idle / failed / reached_start through generation-stamped events;
// RustSdkMatrixClient mirrors that per room. This controller adds the
// missing request POLICY on top of that state:
//
//   * two request reasons — filling a too-short initial viewport
//     (ViewportFill) and the user approaching the top (NearTop);
//   * controller-level single-flight that also covers the window between
//     dispatch and the first "loading" poll event;
//   * a bounded automatic-fill budget plus no-progress detection so an
//     initial viewport fill can never loop forever;
//   * stale-result isolation by room and by controller generation, so a
//     room switch, timeline reset, or sign-out during a request can never
//     complete into the newly shown timeline;
//   * explicit retry after a transient failure.
//
// QML drives it with requestViewportFill() / requestNearTop() / retry() and
// listens to paginationCompleted(insertedCount, reachedStart) to restore the
// scroll anchor after a prepend (final anchor QML belongs to 0.5.11 UI work).
//
// Never logs message bodies, room ids, or URLs — only reasons, counts and
// generations.
class PaginationController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool reachedStart READ reachedStart NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(PresentationState presentationState READ presentationState NOTIFY stateChanged)
    // True once automatic viewport filling stopped itself (budget spent or
    // no progress). User-driven NearTop requests remain available.
    Q_PROPERTY(bool fillStopped READ fillStopped NOTIFY stateChanged)

public:
    enum PresentationState { Hidden, Loading, Failed };
    Q_ENUM(PresentationState)

    explicit PaginationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool busy() const;
    bool reachedStart() const;
    bool failed() const;
    PresentationState presentationState() const;
    bool fillStopped() const { return m_fillStopped; }

    // Ask for one more batch because the viewport is not filled yet.
    // Budget-limited and no-progress-guarded; safe to call repeatedly from
    // QML size-change handlers.
    Q_INVOKABLE void requestViewportFill();
    // Ask for one more batch because the user scrolled near the top.
    Q_INVOKABLE void requestNearTop();
    // Clear a failure and request again (user pressed Retry).
    Q_INVOKABLE void retry();

    // Monotonic controller generation. Bumped on room change, timeline
    // reset, and sign-out; exposed so anchor bookkeeping can reject stale
    // completions.
    quint64 generation() const { return m_generation; }

    // Test hooks.
    void setMaxViewportFillRequests(int count) { m_maxFillRequests = count; }

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    // One completed backward batch for the CURRENT room and generation.
    // insertedCount is the number of events prepended by this batch.
    void paginationCompleted(int insertedCount, bool reachedStart);

private Q_SLOTS:
    void onPaginationStateChanged(const QString &roomId);
    void onEventsPrepended(const QString &roomId,
                           const QList<TimelineEvent> &events);
    void onEventInsertedAt(const QString &roomId, int index,
                           const TimelineEvent &event);
    void onTimelineReset(const QString &roomId);
    void onLoggedOut();

private:
    enum class Reason { None, ViewportFill, NearTop, Retry };
    static const char *reasonName(Reason reason);

    void request(Reason reason);
    void resetPerRoomState();
    void finishBatch(bool reachedStart);

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_generation = 0;

    // Controller-level single flight. True from dispatch until the batch
    // reaches a terminal state (idle / failed / room switch / sign-out).
    bool m_requestActive = false;
    Reason m_activeReason = Reason::None;
    int m_batchInserted = 0;
    QSet<QString> m_batchStableIds;
    bool m_deferredFill = false;
    bool m_completionPending = false;

    // Automatic-fill safety. Both reset on every room (re)open.
    int m_fillRequests = 0;
    int m_maxFillRequests = 8;
    int m_noProgressStrikes = 0;
    bool m_fillStopped = false;

    static constexpr int kMaxNoProgressStrikes = 2;
};
