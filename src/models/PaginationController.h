#pragma once

#include <QCoreApplication>
#include <QList>
#include <QHash>
#include <QObject>
#include <functional>
#include <QString>
#include <QSet>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

class MatrixClient;
class TimelineModel;
struct TimelineEvent;

// Backward-pagination policy for the live SDK timeline.
//
// The Rust bridge single-flights `paginate_backwards` and reports loading /
// idle / failed / reached_start through generation-stamped events, which
// RustSdkMatrixClient mirrors per room. This controller adds the request
// policy:
//
//   * two request reasons: filling a short initial viewport (ViewportFill)
//     and the user approaching the top (NearTop);
//   * single-flight that also covers the gap between dispatch and the first
//     "loading" event;
//   * a bounded automatic-fill budget with no-progress detection, so a fill
//     can never loop forever;
//   * stale-result isolation by room and controller generation, so a room
//     switch, reset or sign-out never completes into the new timeline;
//   * explicit retry after a transient failure.
//
// QML drives it with requestViewportFill() / requestNearTop() / retry(). It
// does not listen to paginationCompleted: the timeline has one
// position-preserving mechanism (TimelinePane.qml's view anchor), so there is
// no pagination-specific restore step. The signal remains the completion
// contract for tests.
//
// Never logs message bodies, room ids or URLs; only reasons, counts and
// generations.
class PaginationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PaginationController is exposed via app.pagination")
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool reachedStart READ reachedStart NOTIFY stateChanged)
    Q_PROPERTY(bool moreHistory READ moreHistory NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(PresentationState presentationState READ presentationState NOTIFY stateChanged)
    Q_PROPERTY(InitialHistoryState initialHistoryState READ initialHistoryState NOTIFY stateChanged)
    Q_PROPERTY(bool fillStopped READ fillStopped NOTIFY stateChanged)
    Q_PROPERTY(int emptyFillPages READ emptyFillPages NOTIFY stateChanged)
    Q_PROPERTY(bool initialContentSettled READ initialContentSettled
                   NOTIFY stateChanged)
    Q_PROPERTY(QString highlightedEventId READ highlightedEventId NOTIFY navigationChanged)
    Q_PROPERTY(QString navigationMessage READ navigationMessage NOTIFY navigationChanged)
    Q_PROPERTY(bool navigating READ navigating NOTIFY navigationChanged)
    Q_PROPERTY(bool nearTopRunActive READ nearTopRunActive NOTIFY stateChanged)

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

    static QString unavailableTargetMessage()
    {
        return QCoreApplication::translate(
            "PaginationController", "Original message is unavailable.");
    }
    static constexpr int kDefaultHighlightDurationMs = 1800;
    static QString tooFarTargetMessage()
    {
        return QCoreApplication::translate(
            "PaginationController",
            "This message is too far back to open here yet.");
    }

    static constexpr int kMaxFilteredRunStrikes = 60;
    static constexpr int kMaxNearTopEmptyStrikes = 12;
    static constexpr int kNavigationMessageDurationMs = 3000;

    void setClient(MatrixClient *client);
    void setTimelineModel(TimelineModel *model) { m_timelineModel = model; }

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool busy() const;
    bool nearTopRunActive() const;
    bool reachedStart() const;
    bool moreHistory() const;
    bool failed() const;
    PresentationState presentationState() const;
    InitialHistoryState initialHistoryState() const;
    bool fillStopped() const { return m_fillStopped; }
    int emptyFillPages() const { return m_emptyFillPages; }
    bool initialContentSettled() const;
    QString highlightedEventId() const { return m_highlightedEventId; }
    QString navigationMessage() const { return m_navigationMessage; }
    bool navigating() const
    { return m_navigationPurpose == NavigationPurpose::Reply; }

    Q_INVOKABLE void requestViewportFill();
    Q_INVOKABLE void requestNearTop(bool userInitiated = false);
    Q_INVOKABLE void retry();
    Q_INVOKABLE void jumpToEvent(const QString &eventId);
    using ContextOpener = std::function<bool(const QString &eventId)>;
    void setContextOpener(ContextOpener opener)
    { m_contextOpener = std::move(opener); }
    void setContextCloser(std::function<void()> closer)
    { m_contextCloser = std::move(closer); }
    void walkToEvent(const QString &eventId);
    Q_INVOKABLE void revealIfLoaded(const QString &eventId);
    Q_INVOKABLE void saveScrollAnchor(const QString &roomId,
                                      const QString &eventId,
                                      qreal pixelOffset,
                                      bool followingLatest);
    Q_INVOKABLE void restoreScrollAnchor(const QString &roomId);
    Q_INVOKABLE void restoreConversation(const QString &roomId, int mode);
    Q_INVOKABLE bool hasHistoricalReturn(const QString &roomId,
                                         int mode) const;
    Q_INVOKABLE void saveSelection(const QString &roomId,
                                   const QString &eventId);
    Q_INVOKABLE QString savedSelection(const QString &roomId) const;
    void forgetAccount(const QString &userId);
    Q_INVOKABLE bool hasHistoricalScrollAnchor(const QString &roomId) const;
    Q_INVOKABLE void saveFollowingLatest(const QString &roomId);
    Q_INVOKABLE void cancelNavigation();

    quint64 generation() const { return m_generation; }

    void setMaxViewportFillRequests(int count) { m_maxFillRequests = count; }
    void setAutomaticRetryPolicyForTest(int attempts, int baseDelayMs)
    { m_maxAutomaticRetries = attempts; m_autoRetryBaseDelayMs = baseDelayMs; }
    void setJumpBatchBudgetForTest(int batches) { m_jumpBatchBudget = batches; }
    void setHighlightDurationForTest(int durationMs)
    { m_highlightDurationMs = durationMs; }
    void setNearTopContinuationDelayForTest(int delayMs)
    { m_nearTopContinuationDelayMs = delayMs; }
    void setRequestWatchdogForTest(int timeoutMs)
    { m_requestWatchdogMs = timeoutMs; }

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    void paginationCompleted(int insertedCount, bool reachedStart,
                             bool willContinue);
    void targetLocated(int row, qreal pixelOffset, bool highlight);
    void restoreLatestRequested();
    void navigationChanged();

private Q_SLOTS:
    void onPaginationStateChanged(const QString &roomId);
    void onEventsPrepended(const QString &roomId,
                           const QList<TimelineEvent> &events);
    void onEventInsertedAt(const QString &roomId, int index,
                           const TimelineEvent &event);
    void onEventsInsertedAt(const QString &roomId, int index,
                            const QList<TimelineEvent> &events);
    void onTimelineReset(const QString &roomId);
    void onLoggedOut();

private:
    void handlePaginationStateChanged(const QString &roomId);
    bool m_announcedReachedStart = false;
    bool m_announcedMoreHistory = false;
    bool m_moreHistoryKnown = false;
    void emitStateChanged();
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
    // Give up on a dispatched batch that never reached any terminal state.
    // Rust's `paginate_back` can return Ok without enqueuing anything (start
    // already reached, or another request holds its single-flight), and the
    // bounded event queue can drop a terminal event, which would leave busy()
    // latched and the room stuck on "Loading" with no Retry.
    //
    // Not a completion: paginationCompleted is not emitted and no continuation
    // is scheduled. It clears the flight so normal triggers can dispatch again,
    // and counts a fill strike so an unresponsive backend cannot keep the fill
    // asking forever.
    void abandonStalledRequest();
    void scheduleNearTopContinuation();
    int batchRowGrowth() const;
    void scheduleAutomaticRetry();
    void continueNavigation(bool reachedStart);
    void failNavigation(bool tooFar = false);
    void locateNavigationTarget(int row);
    void clearNavigation(bool clearMessage = true);

    MatrixClient *m_client = nullptr;
    TimelineModel *m_timelineModel = nullptr;
    QString m_roomId;
    quint64 m_generation = 0;

    // Controller-level single flight, from dispatch until a terminal state
    // (idle / failed / room switch / sign-out).
    bool m_requestActive = false;
    Reason m_activeReason = Reason::None;
    int m_batchInserted = 0;
    int m_batchStartRows = 0;
    QSet<QString> m_batchStableIds;
    bool m_deferredFill = false;
    int m_suppressedSinceDispatch = 0;
    QString m_suppressedReason;
    bool m_completionPending = false;
    bool m_completionReachedStart = false;
    bool m_seenLoading = false;
    bool m_initialHistoryRequested = false;
    bool m_initialHistoryHasSucceeded = false;
    QTimer m_autoRetryTimer;
    // The SDK reports pagination idle independently of the diff stream. Stay
    // single-flight until the batch's rows arrive or this bounded window
    // expires, or a second request can start before the first one's rows land.
    QTimer m_completionSettleTimer;
    int m_completionSettleDelayMs = 250;
    QTimer m_requestWatchdogTimer;
    int m_requestWatchdogMs = 30000;
    quint64 m_requestWatchdogGeneration = 0;
    quint64 m_autoRetryGeneration = 0;
    int m_autoRetryAttempts = 0;
    int m_maxAutomaticRetries = 3;
    int m_autoRetryBaseDelayMs = 150;

    int m_fillRequests = 0;
    int m_maxFillRequests = 60;
    int m_noProgressStrikes = 0;
    bool m_fillStopped = false;
    int m_emptyFillPages = 0;
    int m_nearTopEmptyStrikes = 0;
    int m_nearTopContinuationDelayMs = 250;
    bool m_continuationPending = false;

    NavigationPurpose m_navigationPurpose = NavigationPurpose::None;
    QString m_navigationEventId;
    int m_navigationBatches = 0;
    QString m_highlightedEventId;
    QString m_navigationMessage;
    QTimer m_highlightTimer;
    QTimer m_navigationMessageTimer;
    QString navigationKey(const QString &roomId) const;
    QHash<QString, ScrollAnchor> m_scrollAnchors;
    QHash<QString, QString> m_savedSelections;
    bool m_restoreIsSelection = false;
    qreal m_restoreOffset = 0;
    int m_highlightDurationMs = kDefaultHighlightDurationMs;

    static constexpr int kMaxNoProgressStrikes = 12;
    static constexpr int kMaxNavigationBatches = 8;
    static constexpr int kMaxJumpBatches = 12;
    int m_jumpBatchBudget = kMaxJumpBatches;
    static constexpr int kMaxScrollAnchors = 64;
    ContextOpener m_contextOpener;
    std::function<void()> m_contextCloser;
};
