#include "models/PaginationController.h"

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QLoggingCategory>
#include <QTimer>

Q_LOGGING_CATEGORY(lcPagination, "lightning.timeline.pagination")

PaginationController::PaginationController(QObject *parent)
    : QObject(parent)
{
    m_autoRetryTimer.setSingleShot(true);
    m_completionSettleTimer.setSingleShot(true);
    m_requestWatchdogTimer.setSingleShot(true);
    m_highlightTimer.setSingleShot(true);
    m_navigationMessageTimer.setSingleShot(true);
    connect(&m_highlightTimer, &QTimer::timeout, this, [this] {
        m_highlightedEventId.clear();
        Q_EMIT navigationChanged();
    });
    connect(&m_navigationMessageTimer, &QTimer::timeout, this, [this] {
        m_navigationMessage.clear();
        Q_EMIT navigationChanged();
    });
    connect(&m_autoRetryTimer, &QTimer::timeout, this, [this] {
        if (m_autoRetryGeneration != m_generation || !m_client
            || m_roomId.isEmpty())
            return;
        if (!m_client->paginationFailed(m_roomId)
            || !m_client->paginationFailureTransient(m_roomId)) {
            Q_EMIT stateChanged();
            return;
        }
        request(Reason::AutomaticRetry);
    });
    connect(&m_completionSettleTimer, &QTimer::timeout, this, [this] {
        if (m_requestActive && m_completionPending)
            finishBatch(m_completionReachedStart);
    });
    connect(&m_requestWatchdogTimer, &QTimer::timeout, this, [this] {
        abandonStalledRequest();
    });
}

void PaginationController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    resetPerRoomState();
    if (m_client) {
        connect(m_client, &MatrixClient::paginationStateChanged,
                this, &PaginationController::onPaginationStateChanged);
        connect(m_client, &MatrixClient::eventsPrepended,
                this, &PaginationController::onEventsPrepended);
        connect(m_client, &MatrixClient::eventInsertedAt,
                this, &PaginationController::onEventInsertedAt);
        connect(m_client, &MatrixClient::eventsInsertedAt,
                this, &PaginationController::onEventsInsertedAt);
        connect(m_client, &MatrixClient::timelineReset,
                this, &PaginationController::onTimelineReset);
        connect(m_client, &MatrixClient::loggedOut,
                this, &PaginationController::onLoggedOut);
    }
    Q_EMIT stateChanged();
}

void PaginationController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    clearNavigation();
    resetPerRoomState();
    // Room opening and the Rust snapshot are asynchronous. Record the intent
    // now; the readiness notification dispatches it once the live timeline
    // generation is adopted.
    m_initialHistoryRequested = !roomId.isEmpty();
    m_deferredFill = m_initialHistoryRequested;
    Q_EMIT roomIdChanged();
    Q_EMIT stateChanged();
}

void PaginationController::resetPerRoomState()
{
    m_autoRetryTimer.stop();
    m_completionSettleTimer.stop();
    m_requestWatchdogTimer.stop();
    ++m_generation;
    m_requestActive = false;
    m_activeReason = Reason::None;
    m_batchInserted = 0;
    m_batchStartRows = 0;
    m_continuationPending = false;
    m_batchStableIds.clear();
    m_deferredFill = false;
    m_completionPending = false;
    m_completionReachedStart = false;
    m_seenLoading = false;
    m_initialHistoryRequested = false;
    m_initialHistoryHasSucceeded = false;
    m_autoRetryGeneration = 0;
    m_autoRetryAttempts = 0;
    m_fillRequests = 0;
    m_noProgressStrikes = 0;
    m_fillStopped = false;
    m_emptyFillPages = 0;
    m_nearTopEmptyStrikes = 0;
}

bool PaginationController::busy() const
{
    if (m_requestActive || m_autoRetryTimer.isActive() || m_continuationPending)
        return true;
    return m_client && !m_roomId.isEmpty() && m_client->paginating(m_roomId);
}

bool PaginationController::nearTopRunActive() const
{
    return (m_requestActive && m_activeReason == Reason::NearTop)
        || m_continuationPending;
}

bool PaginationController::reachedStart() const
{
    // The backend exposes reached-start only through canPaginate(); derive
    // it without conflating "loading right now" with "no more history".
    if (!m_client || m_roomId.isEmpty())
        return false;
    return m_client->paginationReady(m_roomId)
        && !m_client->canPaginate(m_roomId) && !m_client->paginating(m_roomId)
        && !m_client->paginationFailed(m_roomId);
}

bool PaginationController::failed() const
{
    return m_client && !m_roomId.isEmpty()
        && !m_autoRetryTimer.isActive()
        && m_client->paginationFailed(m_roomId);
}

PaginationController::PresentationState PaginationController::presentationState() const
{
    if (m_roomId.isEmpty())
        return Hidden;
    if (busy())
        return Loading;
    if (failed())
        return Failed;
    return Hidden;
}

PaginationController::InitialHistoryState PaginationController::initialHistoryState() const
{
    if (m_roomId.isEmpty() || !m_initialHistoryRequested)
        return InitialInactive;
    if (m_deferredFill && m_client
        && !m_client->paginationReady(m_roomId))
        return WaitingForTimeline;
    if (m_autoRetryTimer.isActive())
        return WaitingForAutomaticRetry;
    if (failed())
        return ManualRetryRequired;
    if (m_requestActive && (m_activeReason == Reason::ViewportFill
                            || m_activeReason == Reason::AutomaticRetry))
        return LoadingInitialHistory;
    return InitialHistorySettled;
}

bool PaginationController::initialContentSettled() const
{
    if (m_roomId.isEmpty() || !m_initialHistoryRequested)
        return false;
    // Not settled while the timeline is still being built or while an
    // automatic fill/retry can still add content without user input.
    if (m_deferredFill && m_client && !m_client->paginationReady(m_roomId))
        return false;
    return m_initialHistoryHasSucceeded || m_fillStopped || reachedStart()
        || failed();
}

const char *PaginationController::reasonName(Reason reason)
{
    switch (reason) {
    case Reason::ViewportFill: return "viewport_fill";
    case Reason::AutomaticRetry: return "automatic_retry";
    case Reason::NearTop:      return "near_top";
    case Reason::Retry:        return "retry";
    case Reason::Navigation:   return "navigation";
    case Reason::None:         break;
    }
    return "none";
}

void PaginationController::requestViewportFill()
{
    if (m_fillStopped)
        return;
    // One cap, honoured as configured; see m_maxFillRequests.
    if (m_fillRequests >= m_maxFillRequests) {
        m_fillStopped = true;
        qCInfo(lcPagination)
            << "timeline pagination fill budget exhausted requests="
            << m_fillRequests << "generation=" << m_generation;
        Q_EMIT stateChanged();
        return;
    }
    // A persistent/exhausted failure stays a user gesture. A backend-marked
    // transient initial failure is handled only by the bounded timer policy.
    m_initialHistoryRequested = true;
    if (failed() || m_autoRetryTimer.isActive())
        return;
    request(Reason::ViewportFill);
}

void PaginationController::requestNearTop(bool userInitiated)
{
    if (failed() || m_autoRetryTimer.isActive())
        return;
    // A deliberate new approach to the top re-arms the continuation. QML
    // edge-latches this (once per approach, not per scroll signal). It does not
    // bypass the bound below.
    if (userInitiated)
        m_nearTopEmptyStrikes = 0;
    // ListView reports atYBeginning during its first empty frame; treat that as
    // the initial-history fill so a transient failure gets the automatic retry
    // policy. Not once the fill has stopped: requestViewportFill() then returns
    // immediately and would swallow the user's gesture.
    if (m_initialHistoryRequested && !m_initialHistoryHasSucceeded
        && !m_fillStopped) {
        requestViewportFill();
        return;
    }
    // Bound consecutive zero-growth pages regardless of who asked: the SDK
    // keeps advancing its cursor through hidden history with
    // reached_start=false, and without a bound the timeline hammers the server
    // while the reader sits at the top. This also stops finishBatch()'s
    // continuation and stale geometry re-triggers.
    if (m_nearTopEmptyStrikes >= kMaxNearTopEmptyStrikes)
        return;
    request(Reason::NearTop);
}

void PaginationController::retry()
{
    if (!failed())
        return;
    m_autoRetryAttempts = 0;
    // An explicit user gesture; loadOlderMessages clears the backend failure
    // flag on the next dispatch.
    request(Reason::Retry);
}

void PaginationController::jumpToEvent(const QString &eventId)
{
    if (eventId.isEmpty() || !m_timelineModel || m_roomId.isEmpty()) {
        failNavigation();
        return;
    }
    const int loadedRow = m_timelineModel->rowForStableId(eventId);
    if (loadedRow >= 0) {
        m_navigationPurpose = NavigationPurpose::Reply;
        m_navigationEventId = eventId;
        locateNavigationTarget(loadedRow);
        return;
    }
    if (m_navigationPurpose == NavigationPurpose::Reply
        && m_navigationEventId == eventId)
        return; // coalesce repeated activation of the same reply
    clearNavigation();
    m_navigationPurpose = NavigationPurpose::Reply;
    m_navigationEventId = eventId;
    m_navigationBatches = 0;
    request(Reason::Navigation);
    // A room-open fill may own the single flight, or the timeline may not have
    // adopted its generation yet; keep the target pending for finishBatch() or
    // the readiness-driven fill. Without a client, request() dispatches nothing
    // and no completion can arrive, so fail the target honestly.
    if (!m_requestActive
        && (!m_client || m_client->paginationReady(m_roomId)))
        failNavigation();
}

// See the header for why this is not jumpToEvent with a flag.
void PaginationController::revealIfLoaded(const QString &eventId)
{
    if (eventId.isEmpty() || !m_timelineModel || m_roomId.isEmpty())
        return;
    const int row = m_timelineModel->rowForStableId(eventId);
    if (row < 0)
        return;
    // A navigation the reader asked for may be in flight; context must never
    // displace it.
    if (m_navigationPurpose != NavigationPurpose::None)
        return;
    m_navigationPurpose = NavigationPurpose::Reply;
    m_navigationEventId = eventId;
    locateNavigationTarget(row);
}

void PaginationController::saveScrollAnchor(const QString &roomId,
                                            const QString &eventId,
                                            qreal pixelOffset,
                                            bool followingLatest)
{
    if (roomId.isEmpty())
        return;
    if (m_scrollAnchors.size() >= kMaxScrollAnchors
        && !m_scrollAnchors.contains(roomId))
        m_scrollAnchors.erase(m_scrollAnchors.begin());
    m_scrollAnchors.insert(roomId, { eventId, pixelOffset, followingLatest });
}

void PaginationController::saveFollowingLatest(const QString &roomId)
{
    saveScrollAnchor(roomId, {}, 0, true);
}

void PaginationController::cancelNavigation()
{
    if (m_navigationPurpose == NavigationPurpose::None)
        return;
    // Keep the message (clearMessage = false): replacing reported progress with
    // silence reads as the click having done nothing. In-flight pagination is
    // left alone since its rows are still useful; clearing the purpose is what
    // stops locateNavigationTarget() from emitting targetLocated().
    clearNavigation(/*clearMessage=*/false);
}

void PaginationController::restoreScrollAnchor(const QString &roomId)
{
    if (roomId.isEmpty() || roomId != m_roomId)
        return;
    const auto it = m_scrollAnchors.constFind(roomId);
    if (it == m_scrollAnchors.cend() || it->followingLatest
        || it->eventId.isEmpty()) {
        Q_EMIT restoreLatestRequested();
        return;
    }
    const int row = m_timelineModel
        ? m_timelineModel->rowForStableId(it->eventId) : -1;
    if (row >= 0) {
        Q_EMIT targetLocated(row, it->pixelOffset, false);
        return;
    }
    clearNavigation();
    m_navigationPurpose = NavigationPurpose::Restore;
    m_navigationEventId = it->eventId;
    m_navigationBatches = 0;
    request(Reason::Navigation);
    if (!m_requestActive && m_client
        && m_client->paginationReady(m_roomId))
        failNavigation();
}

void PaginationController::request(Reason reason)
{
    if (!m_client || m_roomId.isEmpty())
        return;
    if (!m_client->paginationReady(m_roomId)) {
        if (reason == Reason::ViewportFill
            || reason == Reason::AutomaticRetry)
            m_deferredFill = true;
        return; // initialization is not a dispatch failure
    }
    if (m_requestActive || m_client->paginating(m_roomId)
        || (m_autoRetryTimer.isActive()
            && reason != Reason::AutomaticRetry)) {
        // Counted and reported once by the dispatch that ends the run; a
        // viewport fill re-asks on every layout pass.
        ++m_suppressedSinceDispatch;
        m_suppressedReason = reasonName(reason);
        return;
    }
    // canPaginate() is false with no live timeline, while loading, or at the
    // start of history; requesting then is pointless or would be dropped,
    // leaving the controller stuck busy.
    if (!m_client->canPaginate(m_roomId) && !m_client->paginationFailed(m_roomId))
        return;

    if (m_suppressedSinceDispatch > 0) {
        qCDebug(lcPagination)
            << "timeline pagination duplicates suppressed count="
            << m_suppressedSinceDispatch << "reason=" << m_suppressedReason;
        m_suppressedSinceDispatch = 0;
        m_suppressedReason.clear();
    }
    m_requestActive = true;
    m_activeReason = reason;
    m_batchInserted = 0;
    m_batchStartRows = m_timelineModel ? m_timelineModel->eventCount() : 0;
    m_batchStableIds.clear();
    m_completionSettleTimer.stop();
    m_completionPending = false;
    m_completionReachedStart = false;
    m_seenLoading = false;
    if (reason == Reason::ViewportFill)
        ++m_fillRequests;

    qCInfo(lcPagination) << "timeline pagination requested reason="
                         << reasonName(reason)
                         << "generation=" << m_generation;
    const quint64 generationAtDispatch = m_generation;
    m_client->loadOlderMessages(m_roomId);
    // A synchronous dispatch failure re-enters via onPaginationStateChanged and
    // clears m_requestActive; only re-check while this request is still ours.
    if (m_generation == generationAtDispatch && m_requestActive
        && m_client->paginationFailed(m_roomId)) {
        m_requestActive = false;
        // Keep the reason until the failure callback/log has observed it.
    }
    // Arm the watchdog only for a flight actually in the air; every path that
    // ends the flight stops it.
    if (m_requestActive && m_requestWatchdogMs > 0) {
        m_requestWatchdogGeneration = m_generation;
        m_requestWatchdogTimer.start(m_requestWatchdogMs);
    }
    Q_EMIT stateChanged();
}

void PaginationController::onEventsPrepended(const QString &roomId,
                                             const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId || !m_requestActive)
        return;
    for (const auto &event : events) {
        const QString stable = !event.itemId.isEmpty()
            ? event.itemId : event.eventId;
        if (!stable.isEmpty() && !m_batchStableIds.contains(stable)) {
            m_batchStableIds.insert(stable);
            ++m_batchInserted;
        }
    }
    if (m_completionPending)
        m_completionSettleTimer.start(0);
}

void PaginationController::onEventInsertedAt(const QString &roomId, int index,
                                             const TimelineEvent &event)
{
    if (roomId != m_roomId || !m_requestActive)
        return;
    const QString stable = !event.itemId.isEmpty()
        ? event.itemId : event.eventId;
    if (!stable.isEmpty() && !m_batchStableIds.contains(stable)) {
        m_batchStableIds.insert(stable);
        ++m_batchInserted;
    }
    if (m_completionPending)
        m_completionSettleTimer.start(0);
}

void PaginationController::onEventsInsertedAt(
    const QString &roomId, int, const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId || !m_requestActive)
        return;
    for (const auto &event : events) {
        const QString stable = !event.itemId.isEmpty()
            ? event.itemId : event.eventId;
        if (!stable.isEmpty() && !m_batchStableIds.contains(stable)) {
            m_batchStableIds.insert(stable);
            ++m_batchInserted;
        }
    }
    // If the idle notification won the race, this is the page it was waiting
    // for. Finish after the current drain, once this range and the model
    // mutation are observable.
    if (m_completionPending)
        m_completionSettleTimer.start(0);
}

void PaginationController::onPaginationStateChanged(const QString &roomId)
{
    if (roomId != m_roomId || !m_client)
        return;

    if (m_deferredFill && m_client->paginationReady(m_roomId)
        && !m_requestActive && !m_client->paginating(m_roomId)) {
        m_deferredFill = false;
        requestViewportFill();
        return;
    }

    if (m_client->paginating(m_roomId)) {
        // Adopt an externally started batch (e.g. requestOlder()) so busy() and
        // duplicate suppression stay truthful.
        if (!m_requestActive) {
            m_requestActive = true;
            m_activeReason = Reason::None;
            m_batchInserted = 0;
            // Re-baseline the row count too, or batchRowGrowth() measures
            // against the previous dispatch and inflates insertedCount.
            m_batchStartRows = m_timelineModel ? m_timelineModel->eventCount() : 0;
        }
        m_seenLoading = true;
        Q_EMIT stateChanged();
        return;
    }

    if (m_client->paginationFailed(m_roomId)) {
        const Reason failedReason = m_activeReason;
        if (m_requestActive) {
            qCWarning(lcPagination)
                << "timeline pagination failed retryable=true reason="
                << reasonName(m_activeReason) << "generation=" << m_generation;
        }
        m_requestActive = false;
        m_activeReason = Reason::None;
        m_completionSettleTimer.stop();
        m_requestWatchdogTimer.stop();
        m_completionPending = false;
        m_seenLoading = false;
        if (failedReason == Reason::Navigation) {
            failNavigation();
            return;
        }
        if ((failedReason == Reason::ViewportFill
             || failedReason == Reason::AutomaticRetry)
            && m_initialHistoryRequested
            && m_client->paginationFailureTransient(m_roomId)
            && m_autoRetryAttempts < m_maxAutomaticRetries) {
            scheduleAutomaticRetry();
            return;
        }
        Q_EMIT stateChanged();
        return;
    }

    // Neither loading nor failed: the batch completed (or state was reset).
    // Only a tracked batch counts.
    if (m_requestActive && m_seenLoading && !m_completionPending) {
        m_completionPending = true;
        m_completionReachedStart = reachedStart();
        // If rows are already present, one drain suffices. If idle arrived
        // first, stay active across the bridge's independent 100 ms poll lane;
        // the first landed range shortens this timer to zero.
        const bool rowsAlreadyLanded = m_batchInserted > 0
            || batchRowGrowth() > 0;
        // Nothing is in flight after a page the filter emptied, so skip the
        // wait.
        const bool nothingCanArrive = m_client
            && m_client->lastPaginationFullyFiltered(m_roomId);
        const int settleDelay =
            rowsAlreadyLanded || nothingCanArrive || !m_timelineModel
                ? 0 : m_completionSettleDelayMs;
        m_completionSettleTimer.start(settleDelay);
    }
    Q_EMIT stateChanged();
}

int PaginationController::batchRowGrowth() const
{
    if (!m_timelineModel)
        return 0;
    return m_timelineModel->eventCount() - m_batchStartRows;
}

void PaginationController::abandonStalledRequest()
{
    // The generation guard makes a queued timeout from an earlier room, reset
    // or sign-out harmless; it must never clear a newer room's flight.
    if (m_requestWatchdogGeneration != m_generation || !m_requestActive)
        return;

    const Reason reason = m_activeReason;
    qCWarning(lcPagination)
        << "timeline pagination abandoned: the backend never reported a"
        << "terminal state reason=" << reasonName(reason)
        << "seen_loading=" << m_seenLoading
        << "timeout_ms=" << m_requestWatchdogMs
        << "generation=" << m_generation;

    m_requestActive = false;
    m_activeReason = Reason::None;
    m_batchInserted = 0;
    m_batchStableIds.clear();
    m_completionSettleTimer.stop();
    m_completionPending = false;
    m_completionReachedStart = false;
    m_seenLoading = false;

    // A navigation that can never land must say so rather than leave the
    // reader waiting.
    if (reason == Reason::Navigation) {
        failNavigation();
        return;
    }
    // An automatic fill whose pages never arrive must stop like one whose pages
    // arrive empty; otherwise the room never becomes presentable
    // (initialContentSettled waits for the fill to stop).
    if (reason == Reason::ViewportFill
        && ++m_noProgressStrikes >= kMaxNoProgressStrikes) {
        m_fillStopped = true;
        qCInfo(lcPagination)
            << "timeline pagination fill stopped no_progress_strikes="
            << m_noProgressStrikes << "generation=" << m_generation;
    }
    Q_EMIT stateChanged();
}

void PaginationController::finishBatch(bool hitStart)
{
    m_completionSettleTimer.stop();
    m_requestWatchdogTimer.stop();
    const Reason reason = m_activeReason;
    // Signal-counted inserts under-report on the Rust backend (see
    // batchRowGrowth()); take the larger of that and the model's growth.
    const int signalled = m_batchInserted;
    const int inserted = qMax(signalled, batchRowGrowth());
    m_requestActive = false;
    m_activeReason = Reason::None;
    m_batchInserted = 0;
    m_batchStableIds.clear();
    m_completionPending = false;
    m_completionReachedStart = false;
    m_seenLoading = false;
    m_autoRetryAttempts = 0;
    // A page that delivered nothing is not a successful initial history:
    // initialContentSettled() returns this flag, and the pane would then show
    // "No messages here yet" over a room whose history the fill has not reached
    // (e.g. a long filtered MatrixRTC run). Rows already on screen count as
    // success, so a cache-served room settles immediately. The other exits
    // (m_fillStopped, reachedStart(), failed()) and the pane's 2500 ms guard
    // still open the gate.
    if ((reason == Reason::ViewportFill || reason == Reason::AutomaticRetry)
        && (inserted > 0
            || (m_timelineModel && m_timelineModel->eventCount() > 0)))
        m_initialHistoryHasSucceeded = true;

    qCInfo(lcPagination) << "timeline pagination completed added=" << inserted
                         << "signalled=" << signalled
                         << "reached_start=" << hitStart
                         << "reason=" << reasonName(reason)
                         << "generation=" << m_generation;

    // No-progress protection for the automatic fill only. An empty batch is
    // legal mid-history (filtered events), so it takes two consecutive empty
    // automatic batches to stop.
    if (reason == Reason::ViewportFill) {
        // A productive fill refunds the budget, so m_maxFillRequests bounds
        // consecutive unproductive fills, not fills per room. Rooms whose
        // recent history is routine state insert rows of zero height and need
        // many pages to fill the viewport; pages that add nothing still stop
        // the loop.
        if (inserted > 0)
            m_fillRequests = 0;
        if (inserted == 0 && !hitStart) {
            // An empty page with the start not reached still moved the cursor
            // toward the next real message, and fills are only requested while
            // the viewport is short, so it gets the larger filtered-run bound.
            // Deliberately not gated on lastPaginationFullyFiltered(): that is
            // an optimisation hint ("false is always safe") only one backend
            // implements, while `inserted == 0 && !hitStart` is reported by
            // every backend.
            ++m_emptyFillPages;
            if (++m_noProgressStrikes >= kMaxFilteredRunStrikes) {
                m_fillStopped = true;
                qCInfo(lcPagination)
                    << "timeline pagination fill stopped no_progress_strikes="
                    << m_noProgressStrikes << "generation=" << m_generation;
            }
        } else {
            m_noProgressStrikes = 0;
        }
    }

    // NearTop progress classification and bounded continuation:
    //   * reached start -> EndOfHistory: clear strikes, stop;
    //   * the mirror grew -> Progress: clear strikes, stop. The reader has the
    //     new page and is pushed off the top edge; QML re-arms on the next
    //     deliberate approach;
    //   * no growth, not at start -> BackendProgressWithoutGrowth: the SDK
    //     advanced through filtered history. Continue a strictly bounded number
    //     of times, then latch until the reader re-approaches the top.
    bool willContinue = false;
    if (reason == Reason::NearTop) {
        if (hitStart) {
            m_nearTopEmptyStrikes = 0;
        } else if (inserted > 0) {
            m_nearTopEmptyStrikes = 0;
        } else {
            ++m_nearTopEmptyStrikes;
            if (m_nearTopEmptyStrikes < kMaxNearTopEmptyStrikes) {
                scheduleNearTopContinuation();
                willContinue = true;
            } else {
                qCInfo(lcPagination)
                    << "timeline pagination near-top continuation latched after"
                    << m_nearTopEmptyStrikes
                    << "filtered pages generation=" << m_generation;
            }
        }
    }

    // busy() and presentationState() just changed; QML bindings on them only
    // re-evaluate on this signal and would otherwise stay on "Loading".
    Q_EMIT stateChanged();
    Q_EMIT paginationCompleted(inserted, hitStart, willContinue);
    if (m_navigationPurpose != NavigationPurpose::None)
        continueNavigation(hitStart);
}

void PaginationController::scheduleNearTopContinuation()
{
    const quint64 generation = m_generation;
    const int rowsAtDispatch = m_batchStartRows;
    m_continuationPending = true;
    // Deferred so this batch's completion and anchor restore settle first. The
    // generation/active/room/bound re-checks cancel it cleanly on a room
    // switch, sign-out or re-arm. The delay allows one more bridge poll so the
    // growth re-check can see late item diffs.
    QTimer::singleShot(m_nearTopContinuationDelayMs, this,
                       [this, generation, rowsAtDispatch] {
        if (generation != m_generation) {
            // A room switch, reset or sign-out already cleared the flag; this
            // timer belongs to the previous generation and must touch nothing.
            return;
        }
        // Cleared on every path before anything can return, or busy() stays
        // true and the loading overlay is stranded.
        m_continuationPending = false;
        // Notify unconditionally: busy() changed, and request() has silent
        // early returns that would leave the overlay latched on Loading. A
        // duplicate notify only re-evaluates bindings.
        Q_EMIT stateChanged();
        if (m_requestActive || m_roomId.isEmpty())
            return;
        if (m_nearTopEmptyStrikes >= kMaxNearTopEmptyStrikes)
            return;
        // The page did add rows; the strike was an artefact of the asynchronous
        // bridge (see batchRowGrowth()). Clear it and stop; the next deliberate
        // approach re-arms through QML. Accepted false positive: a live message
        // appended in this window also reads as growth and cancels one page.
        const int growth = m_timelineModel
            ? m_timelineModel->eventCount() - rowsAtDispatch : 0;
        if (growth > 0) {
            m_nearTopEmptyStrikes = 0;
            qCInfo(lcPagination)
                << "timeline pagination continuation cancelled rows_added="
                << growth << "generation=" << m_generation;
            return; // already notified above
        }
        request(Reason::NearTop);
    });
}

void PaginationController::onTimelineReset(const QString &roomId)
{
    if (roomId != m_roomId)
        return;
    clearNavigation();
    // A fresh snapshot restarts anchor bookkeeping and the fill budget;
    // any batch that was in flight belongs to the previous generation.
    resetPerRoomState();
    // The reset is the readiness boundary for a new Rust timeline. Keep one
    // initial fill pending for the following state notification.
    m_initialHistoryRequested = !m_roomId.isEmpty();
    m_deferredFill = m_initialHistoryRequested;
    Q_EMIT stateChanged();
}

void PaginationController::onLoggedOut()
{
    qCInfo(lcPagination) << "timeline pagination stopped on sign-out generation="
                         << m_generation;
    resetPerRoomState();
    clearNavigation();
    m_scrollAnchors.clear();
    Q_EMIT stateChanged();
}

void PaginationController::continueNavigation(bool hitStart)
{
    if (m_navigationPurpose == NavigationPurpose::None || !m_timelineModel)
        return;
    const int row = m_timelineModel->rowForStableId(m_navigationEventId);
    if (row >= 0) {
        locateNavigationTarget(row);
        return;
    }
    if (hitStart || ++m_navigationBatches >= kMaxNavigationBatches) {
        failNavigation();
        return;
    }
    request(Reason::Navigation);
    if (!m_requestActive)
        failNavigation();
}

void PaginationController::locateNavigationTarget(int row)
{
    const bool highlight = m_navigationPurpose == NavigationPurpose::Reply;
    qreal offset = 0;
    if (!highlight) {
        const auto it = m_scrollAnchors.constFind(m_roomId);
        if (it != m_scrollAnchors.cend())
            offset = it->pixelOffset;
    }
    const QString eventId = m_navigationEventId;
    m_navigationPurpose = NavigationPurpose::None;
    m_navigationEventId.clear();
    m_navigationBatches = 0;
    if (highlight) {
        m_highlightedEventId = eventId;
        m_highlightTimer.start(m_highlightDurationMs);
        Q_EMIT navigationChanged();
    }
    Q_EMIT targetLocated(row, offset, highlight);
}

void PaginationController::failNavigation()
{
    const bool wasRestore = m_navigationPurpose == NavigationPurpose::Restore;
    clearNavigation(false);
    if (wasRestore) {
        Q_EMIT restoreLatestRequested();
        return;
    }
    m_navigationMessage = unavailableTargetMessage();
    m_navigationMessageTimer.start(kNavigationMessageDurationMs);
    Q_EMIT navigationChanged();
}

void PaginationController::clearNavigation(bool clearMessage)
{
    m_navigationPurpose = NavigationPurpose::None;
    m_navigationEventId.clear();
    m_navigationBatches = 0;
    bool changed = false;
    if (!m_highlightedEventId.isEmpty()) {
        m_highlightTimer.stop();
        m_highlightedEventId.clear();
        changed = true;
    }
    if (clearMessage && !m_navigationMessage.isEmpty()) {
        m_navigationMessageTimer.stop();
        m_navigationMessage.clear();
        changed = true;
    }
    if (changed)
        Q_EMIT navigationChanged();
}

void PaginationController::scheduleAutomaticRetry()
{
    const int shift = qMin(m_autoRetryAttempts, 4);
    const int delay = m_autoRetryBaseDelayMs * (1 << shift);
    ++m_autoRetryAttempts;
    m_autoRetryGeneration = m_generation;
    qCInfo(lcPagination)
        << "timeline pagination automatic retry scheduled attempt="
        << m_autoRetryAttempts << "delay_ms=" << delay
        << "generation=" << m_generation;
    m_autoRetryTimer.start(delay);
    Q_EMIT stateChanged();
}
