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
    // Room opening and the Rust timeline snapshot are asynchronous. Record
    // initial-history intent immediately; the backend readiness notification
    // will dispatch it only after the live timeline generation is adopted.
    m_initialHistoryRequested = !roomId.isEmpty();
    m_deferredFill = m_initialHistoryRequested;
    Q_EMIT roomIdChanged();
    Q_EMIT stateChanged();
}

void PaginationController::resetPerRoomState()
{
    m_autoRetryTimer.stop();
    m_completionSettleTimer.stop();
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
    // A deliberate NEW approach to the top re-arms the bounded continuation.
    // QML edge-latches this (one request per genuine top-approach, not per
    // contentY/settle signal), so it is not re-sent while the reader merely
    // sits near the top. It does NOT bypass the bound below — reaching the top
    // must not itself spin.
    if (userInitiated)
        m_nearTopEmptyStrikes = 0;
    // ListView reports atYBeginning during its first empty/incubating frame.
    // Treat that first signal as initial-history fill so a transient failure
    // receives the bounded automatic policy instead of requiring a click.
    if (m_initialHistoryRequested && !m_initialHistoryHasSucceeded) {
        requestViewportFill();
        return;
    }
    // Bound consecutive filtered (zero-growth) pages regardless of who
    // asked. matrix-rust-sdk keeps advancing its back-pagination cursor through
    // thread-only / hidden history, returning reached_start=false with no
    // mirror growth; without this bound the timeline hammers the homeserver
    // for as long as the reader stays at the top — the reported near_top loop.
    // finishBatch() drives the bounded continuation itself, so this guard is
    // what stops it and what a stale geometry re-trigger hits.
    if (m_nearTopEmptyStrikes >= kMaxNearTopEmptyStrikes)
        return;
    request(Reason::NearTop);
}

void PaginationController::retry()
{
    if (!failed())
        return;
    m_autoRetryAttempts = 0;
    // NearTop semantics: an explicit user gesture. loadOlderMessages clears
    // the backend failure flag on the next dispatch.
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
    // A room-open fill may already own the single flight, or the timeline
    // may not have adopted its SDK generation yet. Keep the target pending;
    // finishBatch()/the readiness-driven initial fill will continue it.
    //
    // The m_client guard is not decoration: request() returns early without
    // dispatching when there is no client, so this line used to dereference a
    // null pointer on exactly that path (its sibling in restoreScrollAnchor()
    // has always guarded it). A missing client can never deliver a completion
    // either, so the pending target is failed honestly rather than left to sit
    // forever behind a click that appeared to do nothing.
    if (!m_requestActive
        && (!m_client || m_client->paginationReady(m_roomId)))
        failNavigation();
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
    // The message is deliberately KEPT (clearMessage = false): if a jump was
    // reporting progress to the reader, replacing that with silence reads as
    // the click having done nothing. Only the navigation itself stops.
    //
    // In-flight pagination is left alone on purpose. It is already paid for,
    // the rows it returns are useful to whoever is now reading, and
    // cancelling it would make a gesture during a load look like a stall.
    // What must not happen is the RESULT being applied to the view, and
    // clearing the purpose is exactly what prevents locateNavigationTarget()
    // from emitting targetLocated().
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
        // Counted, and reported once by the dispatch that ends the run. A
        // viewport fill re-asks on every layout pass while a batch is in
        // flight, so one line per suppression is O(layout passes).
        ++m_suppressedSinceDispatch;
        m_suppressedReason = reasonName(reason);
        return;
    }
    // canPaginate() is false when there is no live timeline for the room,
    // a batch is loading, or the start of history was reached. Requesting
    // in any of those states would either be dropped silently (leaving the
    // controller stuck busy) or is pointless.
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
    // A synchronous dispatch failure flips the backend failed flag and
    // re-enters this object via onPaginationStateChanged, which clears
    // m_requestActive. Only re-check when this request is still ours.
    if (m_generation == generationAtDispatch && m_requestActive
        && m_client->paginationFailed(m_roomId)) {
        m_requestActive = false;
        // Preserve the reason until the failure callback/log has observed it.
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
    // If the backend's idle notification won the race, this is the page it was
    // waiting for. Finish after the current event-loop drain, when all signals
    // from this atomic range and the TimelineModel mutation are observable.
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
        // Adopt an externally started batch (e.g. legacy requestOlder())
        // so busy() and duplicate suppression stay truthful.
        if (!m_requestActive) {
            m_requestActive = true;
            m_activeReason = Reason::None;
            m_batchInserted = 0;
            // Also re-baseline the row count. Without this an externally
            // started batch measures batchRowGrowth() against the PREVIOUS
            // dispatch's baseline and reports an arbitrarily inflated
            // insertedCount — a lie in the completion signal this class
            // documents as "what the reader actually gained".
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

    // Neither loading nor failed: the batch completed (or the state was
    // reset underneath us). Only a batch this controller tracked counts.
    if (m_requestActive && m_seenLoading && !m_completionPending) {
        m_completionPending = true;
        m_completionReachedStart = reachedStart();
        // If rows are already present, one event-loop drain is sufficient. If
        // idle arrived first, keep the request active across the Rust bridge's
        // independent 100ms poll lane. The first landed atomic range shortens
        // this timer to zero in the insertion handlers above.
        const bool rowsAlreadyLanded = m_batchInserted > 0
            || batchRowGrowth() > 0;
        const int settleDelay = rowsAlreadyLanded || !m_timelineModel
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

void PaginationController::finishBatch(bool hitStart)
{
    m_completionSettleTimer.stop();
    const Reason reason = m_activeReason;
    // Signal-counted inserts UNDER-report on the real backend (see
    // batchRowGrowth()); the model is the ground truth for whether the reader
    // gained anything. Take the larger so a backend that does report its
    // prepends faithfully is never made to look worse.
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
    if (reason == Reason::ViewportFill || reason == Reason::AutomaticRetry)
        m_initialHistoryHasSucceeded = true;

    qCInfo(lcPagination) << "timeline pagination completed added=" << inserted
                         << "signalled=" << signalled
                         << "reached_start=" << hitStart
                         << "reason=" << reasonName(reason)
                         << "generation=" << m_generation;

    // No-progress protection for the automatic fill loop only. An empty
    // batch is legal mid-history (e.g. filtered state events), so two
    // consecutive empty automatic batches stop the loop instead of one.
    if (reason == Reason::ViewportFill) {
        if (inserted == 0 && !hitStart) {
            if (++m_noProgressStrikes >= kMaxNoProgressStrikes) {
                m_fillStopped = true;
                qCInfo(lcPagination)
                    << "timeline pagination fill stopped no_progress_strikes="
                    << m_noProgressStrikes << "generation=" << m_generation;
            }
        } else {
            m_noProgressStrikes = 0;
        }
    }

    // NearTop progress classification and BOUNDED continuation:
    //   * reached start -> EndOfHistory: clear strikes, stop;
    //   * the mirror grew (inserted > 0) -> Progress: clear strikes, stop
    //     auto-continuation. One near-top trigger yields at most one
    //     dispatched page once that page is productive. (v0.6.6 regression
    //     fix: an earlier "the view is staged/frozen, so keep loading
    //     regardless of growth" branch here turned one held gesture into an
    //     unbounded chain that silently paginated the rest of the room —
    //     chains of near_top requests with signalled=0 on every completion,
    //     and multi-thousand-pixel displacedApplied corrections flushed on
    //     release. That branch, and the TimelineModel staging/freezing
    //     window it existed to serve, are both gone — see git history.) The
    //     reader now has the new page in front of them and is pushed off
    //     the top edge, so QML re-arms on the next deliberate approach;
    //   * no growth, not at start -> BackendProgressWithoutGrowth: the SDK
    //     advanced its cursor through filtered (thread-only / hidden)
    //     history with nothing new in the mirror. Continue a STRICTLY
    //     bounded number of times to surface real content, then latch and
    //     stop until the reader deliberately re-approaches the top. This is
    //     the 2735bb3 storm bound.
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

    // m_requestActive just dropped to false above, which is what busy() and
    // therefore presentationState() key off — QML bindings on those
    // properties (e.g. TimelinePane.qml's pagination header) only
    // re-evaluate on this NOTIFY signal, so without it they stay frozen on
    // whatever state was current the instant the batch finished (typically
    // "Loading", forever) even though direct C++ reads already see Hidden.
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
    // Deferred so this batch's completion, anchor restore, and stateChanged
    // settle first; network latency then paces the bounded continuation (it is
    // not a tight spin). The generation / active / room / bound re-checks make
    // a room switch, sign-out, or a re-arm that landed meanwhile cancel it
    // cleanly. The delay is long enough for one more bridge poll, so the
    // model-growth re-check below can see item diffs that arrived after
    // finishBatch() already classified the page.
    QTimer::singleShot(m_nearTopContinuationDelayMs, this,
                       [this, generation, rowsAtDispatch] {
        if (generation != m_generation) {
            // A room switch / reset / sign-out already cleared the flag through
            // resetPerRoomState(); this timer belongs to the previous
            // generation and must touch nothing.
            return;
        }
        // Cleared on EVERY remaining path, before anything can return: leaving
        // it set would pin busy() true and strand the loading overlay visible
        // for the rest of the room visit.
        m_continuationPending = false;
        // Notify UNCONDITIONALLY, before any decision. busy() just changed, and
        // request() below has silent early returns (timeline not ready, cannot
        // paginate) that would otherwise leave the overlay latched on Loading
        // with no notify to bring it down. request() emits again on the paths
        // that proceed; a duplicate notify only re-evaluates bindings, and the
        // overlay is a sibling of the ListView so it cannot perturb geometry.
        Q_EMIT stateChanged();
        if (m_requestActive || m_roomId.isEmpty())
            return;
        if (m_nearTopEmptyStrikes >= kMaxNearTopEmptyStrikes)
            return;
        // The page DID add rows; the strike was a measurement artefact of the
        // asynchronous bridge (see batchRowGrowth()). Continuing here is what
        // turned one deliberate approach to the top into four network batches
        // and made a big room feel like it "keeps loading". Clear the strike
        // and stop — the reader now has older history in front of them, and a
        // further deliberate approach re-arms through QML.
        //
        // Deliberately accepted false positive: a LIVE message appended below
        // the reader inside this window also reads as growth and cancels the
        // continuation. That costs one page of genuinely filtered history and
        // errs toward loading less, which is the direction the defect reports
        // all point in.
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
    // The reset is the readiness boundary for a newly built Rust timeline.
    // Keep one initial fill pending; the following backend state notification
    // dispatches it against the adopted generation.
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
