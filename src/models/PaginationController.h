#pragma once

#include <QCoreApplication>
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
    // Exposed to QML only as the "app.pagination" context property; registered
    // so TimelinePane.qml can name the PresentationState enum.
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
    // Automatic-fill pages that completed without adding a row or reaching the
    // start of history, as a run of filtered MatrixRTC membership events
    // produces. Monotonic within a room, reset on every (re)open, so QML can
    // compare two fill attempts:
    //
    //   * advanced: the backend really paged through history and nothing was
    //     visible. That is progress (the cursor moved), so the fill may
    //     continue.
    //   * did not advance: the dispatch went nowhere, and the small no-progress
    //     bound applies.
    Q_PROPERTY(int emptyFillPages READ emptyFillPages NOTIFY stateChanged)
    // Initial-hydration gate: true once the automatic history fill cannot add
    // more on its own (a batch landed, filling stopped itself, the start is
    // loaded, or the fill failed and awaits Retry). QML combines it with its
    // viewport geometry to decide when the room is presentable.
    Q_PROPERTY(bool initialContentSettled READ initialContentSettled
                   NOTIFY stateChanged)
    Q_PROPERTY(QString highlightedEventId READ highlightedEventId NOTIFY navigationChanged)
    Q_PROPERTY(QString navigationMessage READ navigationMessage NOTIFY navigationChanged)
    // True while a NearTop backfill run is in flight or a bounded continuation
    // is scheduled. A page that grew the mirror ends the run (finishBatch());
    // an empty page schedules one more try, up to kMaxNearTopEmptyStrikes. Test
    // and diagnostic surface only; production QML does not read it.
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

    // The one wording for "the navigation target could not be reached", shared
    // with ThreadController so the same fact never reads as two failures.
    // QCoreApplication::translate() inline rather than tr(), so
    // ThreadController can use it without linking this class's metaobject. The
    // context and source key match what tr() produced, so translations are
    // unaffected.
    static QString unavailableTargetMessage()
    {
        return QCoreApplication::translate(
            "PaginationController", "Original message is unavailable.");
    }
    // Reply-highlight lifetime and how long the unavailable notice stays,
    // shared with ThreadController so both pulse and expire identically.
    static constexpr int kDefaultHighlightDurationMs = 1800;

    /// The strike bound for a fill walking history the timeline filter empties
    /// while the viewport it exists to fill is still not full. Fills are only
    /// requested while the viewport is short (TimelinePane.qml returns early at
    /// `contentHeight >= height`), so an empty fill page is by construction
    /// spent on a viewport that is not yet full. Stopping with a blank viewport
    /// just hands the user the work.
    ///
    /// Affordable because filtered pages are usually served from the
    /// event-cache store one chunk at a time and skip the completion settle
    /// timer. Still far below a room's whole history.
    static constexpr int kMaxFilteredRunStrikes = 60;
    // Automatic near-top continuations allowed for pages that inserted
    // nothing, matching kMaxNoProgressStrikes and the pane's
    // maxInvisibleFillRetries: all three face long runs of filtered history.
    // Continues only when the backend advanced and the mirror gained nothing,
    // and still stops at the start of history or on any inserted row.
    static constexpr int kMaxNearTopEmptyStrikes = 12;
    static constexpr int kNavigationMessageDurationMs = 3000;

    void setClient(MatrixClient *client);
    void setTimelineModel(TimelineModel *model) { m_timelineModel = model; }

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool busy() const;
    bool nearTopRunActive() const;
    bool reachedStart() const;
    bool failed() const;
    PresentationState presentationState() const;
    InitialHistoryState initialHistoryState() const;
    bool fillStopped() const { return m_fillStopped; }
    int emptyFillPages() const { return m_emptyFillPages; }
    bool initialContentSettled() const;
    QString highlightedEventId() const { return m_highlightedEventId; }
    QString navigationMessage() const { return m_navigationMessage; }

    // Ask for one more batch because the viewport is not filled yet. Budget-
    // and no-progress-guarded; safe to call repeatedly from size handlers.
    Q_INVOKABLE void requestViewportFill();
    // Ask for one more batch because the user scrolled near the top. A real
    // user gesture (userInitiated=true) re-arms the automatic backfill cap;
    // passive geometry-driven calls are bounded so runs of filtered pages
    // cannot spin near the top.
    Q_INVOKABLE void requestNearTop(bool userInitiated = false);
    // Clear a failure and request again (user pressed Retry).
    Q_INVOKABLE void retry();
    Q_INVOKABLE void jumpToEvent(const QString &eventId);
    // Reveal an event only if it is already loaded: no pagination, and no
    // failure message. Unlike jumpToEvent, which may paginate to reach its
    // target, this is for context around a destination already shown (e.g. a
    // thread root when a notification opens the thread panel), where walking
    // months of room history backwards would be wrong.
    Q_INVOKABLE void revealIfLoaded(const QString &eventId);
    Q_INVOKABLE void saveScrollAnchor(const QString &roomId,
                                      const QString &eventId,
                                      qreal pixelOffset,
                                      bool followingLatest);
    Q_INVOKABLE void restoreScrollAnchor(const QString &roomId);
    Q_INVOKABLE void saveFollowingLatest(const QString &roomId);
    // Retire an in-flight navigation because the reader took over the view. A
    // restore from restoreScrollAnchor() can spend up to kMaxNavigationBatches
    // real paginations and is armed after scrolling may have begun; without
    // this it would still land and yank the reader back.
    Q_INVOKABLE void cancelNavigation();

    // Monotonic generation, bumped on room change, timeline reset and sign-out,
    // so anchor bookkeeping can reject stale completions.
    quint64 generation() const { return m_generation; }

    // Test hooks.
    void setMaxViewportFillRequests(int count) { m_maxFillRequests = count; }
    void setAutomaticRetryPolicyForTest(int attempts, int baseDelayMs)
    { m_maxAutomaticRetries = attempts; m_autoRetryBaseDelayMs = baseDelayMs; }
    void setHighlightDurationForTest(int durationMs)
    { m_highlightDurationMs = durationMs; }
    void setNearTopContinuationDelayForTest(int delayMs)
    { m_nearTopContinuationDelayMs = delayMs; }
    void setRequestWatchdogForTest(int timeoutMs)
    { m_requestWatchdogMs = timeoutMs; }

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    // One completed backward batch for the current room and generation.
    // insertedCount is what the reader gained: the larger of the observed
    // prepends and the model's row growth (see batchRowGrowth()).
    // willContinue: this completion scheduled a bounded near-top continuation;
    // that continuation re-checks growth before dispatching, so it means
    // "scheduled", not "will fetch".
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
    // Schedules exactly one more NearTop request after
    // m_nearTopContinuationDelayMs. Only reached for a page that reported no
    // mirror growth and passed the empty-strike gate in finishBatch(). At fire
    // time it re-checks real model growth (batchRowGrowth()), so a page whose
    // rows arrived late cancels the continuation, and re-reads
    // kMaxNearTopEmptyStrikes live: a user-initiated request in the delay
    // window may already have changed m_nearTopEmptyStrikes.
    void scheduleNearTopContinuation();
    // Rows the model gained since the active batch was dispatched; the
    // authoritative progress measure. The signal-counted m_batchInserted is
    // unreliable on the Rust backend, which delivers a batch's diffs from a
    // task independent of the one reporting idle, through a capped 100 ms poll,
    // so a full page can look like "no growth". Uses
    // TimelineModel::eventCount(): this is a backend-delivery question, not a
    // view-geometry one.
    int batchRowGrowth() const;
    void scheduleAutomaticRetry();
    void continueNavigation(bool reachedStart);
    void failNavigation();
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
    // Model row count when the active batch was dispatched. See
    // batchRowGrowth().
    int m_batchStartRows = 0;
    QSet<QString> m_batchStableIds;
    bool m_deferredFill = false;
    /// Duplicate requests suppressed since the last dispatch, and what asked.
    /// Counted rather than logged per call (a viewport fill re-asks on every
    /// layout pass); reported once on the dispatch that ends the run.
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
    // Last resort for a dispatched batch that reports nothing at all (see
    // abandonStalledRequest()). Long enough never to fire on a request that is
    // merely slow.
    QTimer m_requestWatchdogTimer;
    int m_requestWatchdogMs = 30000;
    quint64 m_requestWatchdogGeneration = 0;
    quint64 m_autoRetryGeneration = 0;
    int m_autoRetryAttempts = 0;
    int m_maxAutomaticRetries = 3;
    int m_autoRetryBaseDelayMs = 150;

    // Automatic-fill safety; reset on every room (re)open.
    int m_fillRequests = 0;
    // Consecutive fill requests that inserted nothing before the fill gives up.
    // Equal to kMaxFilteredRunStrikes on purpose: the two count halves of one
    // event (a fill was dispatched; the page came back empty). Filtered
    // MatrixRTC churn inserts zero rows, and a long call can leave hundreds of
    // such events between two messages; the pane's row cap bounds what does get
    // inserted. Any page that inserts a row refunds it. Dispatches that never
    // complete are bounded separately and much sooner by kMaxNoProgressStrikes.
    int m_maxFillRequests = 60;
    int m_noProgressStrikes = 0;
    bool m_fillStopped = false;
    /// See the emptyFillPages property; reset on every room (re)open.
    int m_emptyFillPages = 0;
    // Consecutive automatic NearTop batches that added nothing. Reset by a user
    // gesture, any productive batch, reaching the start, or a room (re)open.
    int m_nearTopEmptyStrikes = 0;
    // Delay before a continuation dispatches: long enough for one more 100 ms
    // bridge poll, so batchRowGrowth() can cancel an unneeded continuation.
    int m_nearTopContinuationDelayMs = 250;
    // A continuation is scheduled but has not run. Folded into busy() so the
    // pagination overlay stays Loading across the gap instead of collapsing and
    // re-expanding per chained page (a cosmetic flicker; the overlay is not
    // list content).
    bool m_continuationPending = false;

    NavigationPurpose m_navigationPurpose = NavigationPurpose::None;
    QString m_navigationEventId;
    int m_navigationBatches = 0;
    QString m_highlightedEventId;
    QString m_navigationMessage;
    QTimer m_highlightTimer;
    QTimer m_navigationMessageTimer;
    QHash<QString, ScrollAnchor> m_scrollAnchors;
    int m_highlightDurationMs = kDefaultHighlightDurationMs;

    // Fill dispatches that never completed, spent by abandonStalledRequest().
    // Much smaller than the filtered-run bound: a page that walked events is
    // progress, a page that never arrived means the backend is not answering.
    static constexpr int kMaxNoProgressStrikes = 12;
    static constexpr int kMaxNavigationBatches = 8;
    static constexpr int kMaxScrollAnchors = 64;
};
