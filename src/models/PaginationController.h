#pragma once

#include <QList>
#include <QHash>
#include <QObject>
#include <QString>
#include <QSet>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

class MatrixClient;
class TimelineModel;
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
    QML_ELEMENT
    // Instantiated in C++ and exposed to QML only as the "app.pagination"
    // context-property instance; this registration exists so TimelinePane.qml
    // can name the PresentationState enum as PaginationController.Loading etc.
    QML_UNCREATABLE("PaginationController is exposed via app.pagination")
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool reachedStart READ reachedStart NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(PresentationState presentationState READ presentationState NOTIFY stateChanged)
    Q_PROPERTY(InitialHistoryState initialHistoryState READ initialHistoryState NOTIFY stateChanged)
    // True once automatic viewport filling stopped itself (budget spent or
    // no progress). User-driven NearTop requests remain available.
    Q_PROPERTY(bool fillStopped READ fillStopped NOTIFY stateChanged)
    // v0.7 initial-hydration gate: true once the automatic initial history
    // fill for the open room cannot add more content on its own — a fill
    // batch landed, filling stopped itself, the start of history is loaded,
    // or the fill failed and awaits a user Retry. QML combines this with
    // its own viewport geometry to decide when the room is presentable.
    Q_PROPERTY(bool initialContentSettled READ initialContentSettled
                   NOTIFY stateChanged)
    Q_PROPERTY(QString highlightedEventId READ highlightedEventId NOTIFY navigationChanged)
    Q_PROPERTY(QString navigationMessage READ navigationMessage NOTIFY navigationChanged)

public:
    enum PresentationState { Hidden, Loading, Failed };
    Q_ENUM(PresentationState)
    enum InitialHistoryState {
        InitialInactive,
        WaitingForTimeline,
        LoadingInitialHistory,
        WaitingForAutomaticRetry,
        ManualRetryRequired,
        InitialHistorySettled
    };
    Q_ENUM(InitialHistoryState)

    explicit PaginationController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setTimelineModel(TimelineModel *model) { m_timelineModel = model; }

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool busy() const;
    bool reachedStart() const;
    bool failed() const;
    PresentationState presentationState() const;
    InitialHistoryState initialHistoryState() const;
    bool fillStopped() const { return m_fillStopped; }
    bool initialContentSettled() const;
    QString highlightedEventId() const { return m_highlightedEventId; }
    QString navigationMessage() const { return m_navigationMessage; }

    // Ask for one more batch because the viewport is not filled yet.
    // Budget-limited and no-progress-guarded; safe to call repeatedly from
    // QML size-change handlers.
    Q_INVOKABLE void requestViewportFill();
    // Ask for one more batch because the user scrolled near the top.
    Q_INVOKABLE void requestNearTop();
    // Clear a failure and request again (user pressed Retry).
    Q_INVOKABLE void retry();
    Q_INVOKABLE void jumpToEvent(const QString &eventId);
    Q_INVOKABLE void saveScrollAnchor(const QString &roomId,
                                      const QString &eventId,
                                      qreal pixelOffset,
                                      bool followingLatest);
    Q_INVOKABLE void restoreScrollAnchor(const QString &roomId);
    Q_INVOKABLE void saveFollowingLatest(const QString &roomId);

    // Monotonic controller generation. Bumped on room change, timeline
    // reset, and sign-out; exposed so anchor bookkeeping can reject stale
    // completions.
    quint64 generation() const { return m_generation; }

    // Test hooks.
    void setMaxViewportFillRequests(int count) { m_maxFillRequests = count; }
    void setAutomaticRetryPolicyForTest(int attempts, int baseDelayMs)
    { m_maxAutomaticRetries = attempts; m_autoRetryBaseDelayMs = baseDelayMs; }
    void setHighlightDurationForTest(int durationMs)
    { m_highlightDurationMs = durationMs; }

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    // One completed backward batch for the CURRENT room and generation.
    // insertedCount is the number of events prepended by this batch.
    void paginationCompleted(int insertedCount, bool reachedStart);
    void targetLocated(int row, qreal pixelOffset, bool highlight);
    void restoreLatestRequested();
    void navigationChanged();

private Q_SLOTS:
    void onPaginationStateChanged(const QString &roomId);
    void onEventsPrepended(const QString &roomId,
                           const QList<TimelineEvent> &events);
    void onEventInsertedAt(const QString &roomId, int index,
                           const TimelineEvent &event);
    void onTimelineReset(const QString &roomId);
    void onLoggedOut();

private:
    enum class Reason { None, ViewportFill, AutomaticRetry, NearTop, Retry,
                        Navigation };
    enum class NavigationPurpose { None, Reply, Restore };
    struct ScrollAnchor {
        QString eventId;
        qreal pixelOffset = 0;
        bool followingLatest = true;
    };
    static const char *reasonName(Reason reason);

    void request(Reason reason);
    void resetPerRoomState();
    void finishBatch(bool reachedStart);
    void scheduleAutomaticRetry();
    void continueNavigation(bool reachedStart);
    void failNavigation();
    void locateNavigationTarget(int row);
    void clearNavigation(bool clearMessage = true);

    MatrixClient *m_client = nullptr;
    TimelineModel *m_timelineModel = nullptr;
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
    bool m_seenLoading = false;
    bool m_initialHistoryRequested = false;
    bool m_initialHistoryHasSucceeded = false;
    QTimer m_autoRetryTimer;
    quint64 m_autoRetryGeneration = 0;
    int m_autoRetryAttempts = 0;
    int m_maxAutomaticRetries = 3;
    int m_autoRetryBaseDelayMs = 150;

    // Automatic-fill safety. Both reset on every room (re)open.
    int m_fillRequests = 0;
    int m_maxFillRequests = 8;
    int m_noProgressStrikes = 0;
    bool m_fillStopped = false;

    NavigationPurpose m_navigationPurpose = NavigationPurpose::None;
    QString m_navigationEventId;
    int m_navigationBatches = 0;
    QString m_highlightedEventId;
    QString m_navigationMessage;
    QTimer m_highlightTimer;
    QTimer m_navigationMessageTimer;
    QHash<QString, ScrollAnchor> m_scrollAnchors;
    int m_highlightDurationMs = 1800;

    static constexpr int kMaxNoProgressStrikes = 2;
    static constexpr int kMaxNavigationBatches = 8;
    static constexpr int kMaxScrollAnchors = 64;
};
