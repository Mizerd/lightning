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
// QML drives it with requestViewportFill() / requestNearTop() / retry().
// It does NOT listen to paginationCompleted: since v0.7.2 the timeline keeps
// ONE position-preserving mechanism (TimelinePane.qml's view anchor, driven by
// coalesced content-height changes regardless of why height changed), so there
// is no pagination-specific restore step to trigger. The signal survives as the
// controller's completion contract for tests and any future consumer.
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
    // v0.7.x: true while a NearTop-driven backfill run is in flight or a
    // bounded continuation to one is scheduled. A page that grew the mirror
    // ends the run (see finishBatch()); only a page that delivered nothing
    // new schedules one more bounded try, up to kMaxNearTopEmptyStrikes. Test
    // hook / diagnostic surface only — nothing in production QML reads this
    // any more. (v0.6.6: TimelineModel's near-top "virtual scrolling"
    // staging window, which this property used to help gate from
    // TimelinePane.qml, was removed outright — see TimelineModel's history.
    // A held gesture at one bounded page per approach coalesced nothing
    // (there was rarely more than one page in flight to hold), while the
    // staging window cost a hard wall for the reader between "one page
    // landed" and "the gesture physically ends" — the loaded page sat
    // invisible and contentY could not advance, the opposite of the
    // maintainer's ask. The whole hidden-prefix mechanism, its
    // backfillStagingActive plumbing, and the navigation flush hooks that
    // existed only to see past it are gone; every landed batch is now an
    // ordinary immediate prepend again, exactly like the un-staged path
    // already was for ViewportFill/Retry.)
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

    // The ONE wording for "the navigation target could not be reached".
    // ThreadController shows the same sentence when a thread-local reply
    // cannot be paginated into the thread panel: a second phrasing for the
    // same fact would read to the user as a different failure, and lupdate
    // still sees exactly one translatable source.
    // QCoreApplication::translate() rather than tr(), and inline, so
    // ThreadController can show it without linking this class's metaobject
    // (its own suites do not build PaginationController.cpp). The context
    // string and the source key are exactly what tr() produced here before,
    // so existing translations are unaffected.
    static QString unavailableTargetMessage()
    {
        return QCoreApplication::translate(
            "PaginationController", "Original message is unavailable.");
    }
    // Reply-highlight lifetime, and how long the unavailable notice stays.
    // Shared with ThreadController so the room timeline and the thread panel
    // pulse and expire identically.
    static constexpr int kDefaultHighlightDurationMs = 1800;
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
    bool initialContentSettled() const;
    QString highlightedEventId() const { return m_highlightedEventId; }
    QString navigationMessage() const { return m_navigationMessage; }

    // Ask for one more batch because the viewport is not filled yet.
    // Budget-limited and no-progress-guarded; safe to call repeatedly from
    // QML size-change handlers.
    Q_INVOKABLE void requestViewportFill();
    // Ask for one more batch because the user scrolled near the top. A genuine
    // user scroll gesture (userInitiated=true) re-arms the automatic backfill
    // cap; passive geometry-driven calls (userInitiated=false) are bounded so a
    // long run of no-op (filtered thread-only) pages cannot spin near the top.
    Q_INVOKABLE void requestNearTop(bool userInitiated = false);
    // Clear a failure and request again (user pressed Retry).
    Q_INVOKABLE void retry();
    Q_INVOKABLE void jumpToEvent(const QString &eventId);
    Q_INVOKABLE void saveScrollAnchor(const QString &roomId,
                                      const QString &eventId,
                                      qreal pixelOffset,
                                      bool followingLatest);
    Q_INVOKABLE void restoreScrollAnchor(const QString &roomId);
    Q_INVOKABLE void saveFollowingLatest(const QString &roomId);
    // Retire an in-flight navigation because the reader took the view.
    //
    // A Restore started by restoreScrollAnchor() can spend up to
    // kMaxNavigationBatches REAL backward paginations — comfortably five to
    // fifteen seconds — before it locates its target and emits
    // targetLocated(). Cancelling the landing in the view is not enough on
    // its own: the view can only retire a landing that is already armed, and
    // this one gets armed AFTER the reader started scrolling. Without a way
    // to reach the controller, the restore still lands and yanks the reader
    // back to the position the room was opened at.
    Q_INVOKABLE void cancelNavigation();

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
    void setNearTopContinuationDelayForTest(int delayMs)
    { m_nearTopContinuationDelayMs = delayMs; }

Q_SIGNALS:
    void roomIdChanged();
    void stateChanged();
    // One completed backward batch for the CURRENT room and generation.
    // insertedCount is what the reader actually gained: the larger of the
    // prepends this controller observed and the rows the model grew by (see
    // batchRowGrowth() for why the two disagree on the real backend).
    // willContinue: THIS completion scheduled a bounded near-top continuation
    // (zero rows gained, start not reached, strike budget left). That
    // continuation still re-checks row growth before dispatching, so
    // willContinue=true means "scheduled", not "will certainly fetch".
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
    // Schedules exactly one more NearTop request, paced by
    // m_nearTopContinuationDelayMs (never a tight loop). Only reached for a
    // page that reported no mirror growth (the backend delivered nothing new
    // — never "no visible rows": there is no separate visible/hidden
    // distinction any more, see TimelineModel::eventCount()) and has not hit
    // the empty-strike bound yet — that gate is decided once, synchronously,
    // in finishBatch(); this function is only ever called when it already
    // passed. (v0.6.6 regression fix: an earlier "becauseStagingHeld" branch
    // kept dispatching unconditionally while a since-removed staging window
    // was held, effectively prefetching the rest of the room behind a held
    // gesture — see the 52cf6ca-round trace: chains of ~30 near_top requests
    // per session with signalled=0 on every completion and multi-thousand-
    // pixel displacedApplied corrections on release. Removed; see git
    // history for the withdrawn mechanism and for TimelineModel's staging
    // window, removed outright in the same round.)
    // At FIRE time (after the delay), this re-checks real model growth
    // before dispatching — see batchRowGrowth() — so a page that DID add
    // rows (just reported late by the async backend) cancels the
    // continuation instead of fetching one the reader did not need, and
    // re-checks the SAME kMaxNearTopEmptyStrikes bound one more time: not
    // redundant with finishBatch()'s own gate — a fresh userInitiated
    // requestNearTop() landing in the delay window can dispatch and
    // complete its OWN batch (resetting or further incrementing
    // m_nearTopEmptyStrikes) before this stale continuation fires, so the
    // bound must be live-read here too, not just captured at schedule time.
    void scheduleNearTopContinuation();
    // Rows the timeline model actually gained since the active batch was
    // dispatched. THE authoritative progress measure for the near-top
    // continuation, because the signal-counted m_batchInserted is not
    // trustworthy on the real backend: it only counts eventsPrepended /
    // eventInsertedAt(index 0) seen inside the request window, and the Rust
    // bridge delivers a batch's item diffs from a task independent of the one
    // that reports pagination idle, through a 100 ms poll that is capped per
    // drain. A page that really did deliver twenty messages can therefore
    // still be classified "no growth" by the signal count alone — and four
    // such pages per approach is the reported "it keeps loading old messages
    // each time I scroll up". Deliberately TimelineModel::eventCount()
    // (the mirror the backend actually delivered into), never rowCount() —
    // there is only one row space now that staging is gone, so the two
    // happen to read identically today, but this is a progress question
    // about backend delivery, not a view-geometry one, and the distinction
    // is the whole reason this helper exists rather than a bare eventCount()
    // read at each call site.
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

    // Controller-level single flight. True from dispatch until the batch
    // reaches a terminal state (idle / failed / room switch / sign-out).
    bool m_requestActive = false;
    Reason m_activeReason = Reason::None;
    int m_batchInserted = 0;
    // Model row count when the active batch was dispatched. See
    // batchRowGrowth().
    int m_batchStartRows = 0;
    QSet<QString> m_batchStableIds;
    bool m_deferredFill = false;
    bool m_completionPending = false;
    bool m_completionReachedStart = false;
    bool m_seenLoading = false;
    bool m_initialHistoryRequested = false;
    bool m_initialHistoryHasSucceeded = false;
    QTimer m_autoRetryTimer;
    // The Rust SDK reports pagination idle independently from the timeline
    // diff stream. Keep the request single-flight until either its atomic row
    // range arrives or this bounded late-delivery window expires; otherwise a
    // second request can start between `complete` and the first request's rows.
    QTimer m_completionSettleTimer;
    int m_completionSettleDelayMs = 250;
    quint64 m_autoRetryGeneration = 0;
    int m_autoRetryAttempts = 0;
    int m_maxAutomaticRetries = 3;
    int m_autoRetryBaseDelayMs = 150;

    // Automatic-fill safety. Both reset on every room (re)open.
    int m_fillRequests = 0;
    int m_maxFillRequests = 8;
    int m_noProgressStrikes = 0;
    bool m_fillStopped = false;
    // Consecutive automatic (non-user) NearTop batches that added no visible
    // events. Bounds passive geometry-driven backfill; reset by a user gesture,
    // any batch that adds content, reaching the start, or a room (re)open.
    int m_nearTopEmptyStrikes = 0;
    // How long the bounded continuation waits before dispatching. Long enough
    // for one more 100 ms bridge poll to deliver a batch's item diffs, so
    // batchRowGrowth() can cancel a continuation the page did not need.
    int m_nearTopContinuationDelayMs = 250;
    // A bounded continuation is scheduled and has not yet run. Folded into
    // busy() so presentationState() stays Loading across the gap: without it
    // the pagination overlay collapses to 0 px and re-expands once per chained
    // page, which is a new visible flicker in a round about jitter. (The
    // overlay is a sibling of the ListView, never list content, so it cannot
    // move timeline geometry either way — this is purely cosmetic.)
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

    static constexpr int kMaxNoProgressStrikes = 2;
    static constexpr int kMaxNearTopEmptyStrikes = 4;
    static constexpr int kMaxNavigationBatches = 8;
    static constexpr int kMaxScrollAnchors = 64;
};
