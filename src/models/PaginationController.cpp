#include "models/PaginationController.h"

#include <QtNumeric>

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
            emitStateChanged();
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
    emitStateChanged();
}

void PaginationController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    clearNavigation();
    resetPerRoomState();
    m_initialHistoryRequested = !roomId.isEmpty();
    m_deferredFill = m_initialHistoryRequested;
    if (m_client && !roomId.isEmpty() && !m_client->paginating(roomId))
        m_moreHistoryKnown = m_client->canPaginate(roomId);
    Q_EMIT roomIdChanged();
    emitStateChanged();
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
    m_moreHistoryKnown = false;
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
    if (!m_client || m_roomId.isEmpty())
        return false;
    return m_client->paginationReady(m_roomId)
        && !m_client->canPaginate(m_roomId) && !m_client->paginating(m_roomId)
        && !m_client->paginationFailed(m_roomId);
}

bool PaginationController::moreHistory() const
{
    if (!m_client || m_roomId.isEmpty())
        return false;
    if (m_client->canPaginate(m_roomId))
        return true;
    // Fetching says nothing about "more" by itself (a viewport fill runs on
    // every open, including a room whose history is all loaded), so while a
    // page loads the answer is the one known before it started.
    return m_client->paginating(m_roomId) && m_moreHistoryKnown;
}

void PaginationController::emitStateChanged()
{
    m_announcedReachedStart = reachedStart();
    m_announcedMoreHistory = moreHistory();
    Q_EMIT stateChanged();
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
        emitStateChanged();
        return;
    }
    m_initialHistoryRequested = true;
    if (failed() || m_autoRetryTimer.isActive())
        return;
    request(Reason::ViewportFill);
}

void PaginationController::requestNearTop(bool userInitiated)
{
    if (failed() || m_autoRetryTimer.isActive())
        return;
    if (userInitiated)
        m_nearTopEmptyStrikes = 0;
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
        if (m_contextCloser)
            m_contextCloser();
        m_navigationPurpose = NavigationPurpose::Reply;
        m_navigationEventId = eventId;
        locateNavigationTarget(loadedRow);
        return;
    }
    if (m_navigationPurpose == NavigationPurpose::Reply
        && m_navigationEventId == eventId)
        return;
    if (m_contextOpener) {
        clearNavigation();
        if (m_contextOpener(eventId))
            return;
    }
    walkToEvent(eventId);
}

void PaginationController::walkToEvent(const QString &eventId)
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
    clearNavigation();
    m_navigationPurpose = NavigationPurpose::Reply;
    m_navigationEventId = eventId;
    m_navigationBatches = 0;
    request(Reason::Navigation);
    Q_EMIT navigationChanged();
    if (!m_requestActive
        && (!m_client || m_client->paginationReady(m_roomId)))
        failNavigation();
}

void PaginationController::revealIfLoaded(const QString &eventId)
{
    if (eventId.isEmpty() || !m_timelineModel || m_roomId.isEmpty())
        return;
    const int row = m_timelineModel->rowForStableId(eventId);
    if (row < 0)
        return;
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
    const QString key = navigationKey(roomId);
    if (m_scrollAnchors.size() >= kMaxScrollAnchors
        && !m_scrollAnchors.contains(key))
        m_scrollAnchors.erase(m_scrollAnchors.begin());
    m_scrollAnchors.insert(key, { eventId, pixelOffset, followingLatest });
}

void PaginationController::saveFollowingLatest(const QString &roomId)
{
    saveScrollAnchor(roomId, {}, 0, true);
}

void PaginationController::cancelNavigation()
{
    if (m_navigationPurpose == NavigationPurpose::None)
        return;
    clearNavigation(false);
}

bool PaginationController::hasHistoricalScrollAnchor(const QString &roomId) const
{
    const auto it = m_scrollAnchors.constFind(navigationKey(roomId));
    return it != m_scrollAnchors.cend() && !it->followingLatest
        && !it->eventId.isEmpty();
}

void PaginationController::saveSelection(const QString &roomId,
                                         const QString &eventId)
{
    if (roomId.isEmpty())
        return;
    if (eventId.isEmpty()) {
        m_savedSelections.remove(navigationKey(roomId));
        return;
    }
    const QString key = navigationKey(roomId);
    if (m_savedSelections.size() >= kMaxScrollAnchors
        && !m_savedSelections.contains(key))
        m_savedSelections.erase(m_savedSelections.begin());
    m_savedSelections.insert(key, eventId);
}

QString PaginationController::savedSelection(const QString &roomId) const
{
    return m_savedSelections.value(navigationKey(roomId));
}

QString PaginationController::navigationKey(const QString &roomId) const
{
    const QString account = m_client ? m_client->currentUserId() : QString{};
    return account + QChar(0x1f) + roomId;
}

void PaginationController::forgetAccount(const QString &userId)
{
    const QString prefix = userId + QChar(0x1f);
    const auto drop = [&prefix](auto &map) {
        for (auto it = map.begin(); it != map.end();) {
            if (it.key().startsWith(prefix))
                it = map.erase(it);
            else
                ++it;
        }
    };
    drop(m_scrollAnchors);
    drop(m_savedSelections);
}

bool PaginationController::hasHistoricalReturn(const QString &roomId,
                                               int mode) const
{
    if (mode == 1)
        return false;
    if (mode == 2 && !m_savedSelections.value(navigationKey(roomId)).isEmpty())
        return true;
    return hasHistoricalScrollAnchor(roomId);
}

void PaginationController::restoreConversation(const QString &roomId,
                                               int mode)
{
    if (roomId.isEmpty() || roomId != m_roomId)
        return;
    if (mode == 1) {
        Q_EMIT restoreLatestRequested();
        return;
    }
    const QString selection = mode == 2
        ? m_savedSelections.value(navigationKey(roomId))
                                        : QString{};
    if (selection.isEmpty()) {
        restoreScrollAnchor(roomId);
        return;
    }
    const auto anchor = m_scrollAnchors.constFind(navigationKey(roomId));
    const qreal offset =
        (anchor != m_scrollAnchors.cend() && !anchor->followingLatest
         && anchor->eventId == selection)
            ? anchor->pixelOffset : qQNaN();
    const int row = m_timelineModel
        ? m_timelineModel->rowForStableId(selection) : -1;
    if (row >= 0) {
        Q_EMIT targetLocated(row, offset, false);
        return;
    }
    clearNavigation();
    m_navigationPurpose = NavigationPurpose::Restore;
    m_navigationEventId = selection;
    m_navigationBatches = 0;
    m_restoreIsSelection = true;
    m_restoreOffset = offset;
    request(Reason::Navigation);
    if (!m_requestActive && m_client
        && m_client->paginationReady(m_roomId))
        failNavigation();
}

void PaginationController::restoreScrollAnchor(const QString &roomId)
{
    if (roomId.isEmpty() || roomId != m_roomId)
        return;
    const auto it = m_scrollAnchors.constFind(navigationKey(roomId));
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
        return;
    }
    if (m_requestActive || m_client->paginating(m_roomId)
        || (m_autoRetryTimer.isActive()
            && reason != Reason::AutomaticRetry)) {
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
    if (m_generation == generationAtDispatch && m_requestActive
        && m_client->paginationFailed(m_roomId)) {
        m_requestActive = false;
    }
    if (m_requestActive && m_requestWatchdogMs > 0) {
        m_requestWatchdogGeneration = m_generation;
        m_requestWatchdogTimer.start(m_requestWatchdogMs);
    }
    emitStateChanged();
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
    if (m_completionPending)
        m_completionSettleTimer.start(0);
}

void PaginationController::onPaginationStateChanged(const QString &roomId)
{
    if (roomId == m_roomId && m_client && !m_client->paginating(m_roomId))
        m_moreHistoryKnown = m_client->canPaginate(m_roomId);
    handlePaginationStateChanged(roomId);
    if (roomId != m_roomId || !m_client)
        return;
    if (reachedStart() != m_announcedReachedStart
        || moreHistory() != m_announcedMoreHistory)
        emitStateChanged();
}

void PaginationController::handlePaginationStateChanged(const QString &roomId)
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
        if (!m_requestActive) {
            m_requestActive = true;
            m_activeReason = Reason::None;
            m_batchInserted = 0;
            m_batchStartRows = m_timelineModel ? m_timelineModel->eventCount() : 0;
        }
        m_seenLoading = true;
        emitStateChanged();
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
        emitStateChanged();
        return;
    }

    if (m_requestActive && (m_seenLoading || reachedStart())
        && !m_completionPending) {
        m_completionPending = true;
        m_completionReachedStart = reachedStart();
        const bool rowsAlreadyLanded = m_batchInserted > 0
            || batchRowGrowth() > 0;
        const bool nothingCanArrive = m_client
            && m_client->lastPaginationFullyFiltered(m_roomId);
        const int settleDelay =
            rowsAlreadyLanded || nothingCanArrive || !m_timelineModel
                ? 0 : m_completionSettleDelayMs;
        m_completionSettleTimer.start(settleDelay);
    }
    emitStateChanged();
}

int PaginationController::batchRowGrowth() const
{
    if (!m_timelineModel)
        return 0;
    return m_timelineModel->eventCount() - m_batchStartRows;
}

void PaginationController::abandonStalledRequest()
{
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

    if (reason == Reason::Navigation) {
        failNavigation();
        return;
    }
    if (reason == Reason::ViewportFill
        && ++m_noProgressStrikes >= kMaxNoProgressStrikes) {
        m_fillStopped = true;
        qCInfo(lcPagination)
            << "timeline pagination fill stopped no_progress_strikes="
            << m_noProgressStrikes << "generation=" << m_generation;
    }
    emitStateChanged();
}

void PaginationController::finishBatch(bool hitStart)
{
    m_completionSettleTimer.stop();
    m_requestWatchdogTimer.stop();
    const Reason reason = m_activeReason;
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
    if ((reason == Reason::ViewportFill || reason == Reason::AutomaticRetry)
        && (inserted > 0
            || (m_timelineModel && m_timelineModel->eventCount() > 0)))
        m_initialHistoryHasSucceeded = true;

    qCInfo(lcPagination) << "timeline pagination completed added=" << inserted
                         << "signalled=" << signalled
                         << "reached_start=" << hitStart
                         << "reason=" << reasonName(reason)
                         << "generation=" << m_generation;

    if (reason == Reason::ViewportFill) {
        if (inserted > 0)
            m_fillRequests = 0;
        if (inserted == 0 && !hitStart) {
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
    emitStateChanged();
    Q_EMIT paginationCompleted(inserted, hitStart, willContinue);
    if (m_navigationPurpose != NavigationPurpose::None)
        continueNavigation(hitStart);
}

void PaginationController::scheduleNearTopContinuation()
{
    const quint64 generation = m_generation;
    const int rowsAtDispatch = m_batchStartRows;
    m_continuationPending = true;
    const bool nothingCanArrive = m_client
        && m_client->lastPaginationFullyFiltered(m_roomId);
    QTimer::singleShot(nothingCanArrive ? 0 : m_nearTopContinuationDelayMs, this,
                       [this, generation, rowsAtDispatch] {
        if (generation != m_generation) {
            return;
        }
        // Cleared on every path before anything can return, or busy() stays
        // true and the loading overlay is stranded.
        m_continuationPending = false;
        emitStateChanged();
        if (m_requestActive || m_roomId.isEmpty())
            return;
        if (m_nearTopEmptyStrikes >= kMaxNearTopEmptyStrikes)
            return;
        const int growth = m_timelineModel
            ? m_timelineModel->eventCount() - rowsAtDispatch : 0;
        if (growth > 0) {
            m_nearTopEmptyStrikes = 0;
            qCInfo(lcPagination)
                << "timeline pagination continuation cancelled rows_added="
                << growth << "generation=" << m_generation;
            return;
        }
        request(Reason::NearTop);
    });
}

void PaginationController::onTimelineReset(const QString &roomId)
{
    if (roomId != m_roomId)
        return;
    clearNavigation();
    resetPerRoomState();
    m_initialHistoryRequested = !m_roomId.isEmpty();
    m_deferredFill = m_initialHistoryRequested;
    emitStateChanged();
}

void PaginationController::onLoggedOut()
{
    qCInfo(lcPagination) << "timeline pagination stopped on sign-out generation="
                         << m_generation;
    resetPerRoomState();
    clearNavigation();
    emitStateChanged();
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
    const int budget = m_navigationPurpose == NavigationPurpose::Reply
        ? m_jumpBatchBudget : kMaxNavigationBatches;
    if (hitStart || ++m_navigationBatches >= budget) {
        failNavigation(!hitStart);
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
    if (!highlight && m_restoreIsSelection) {
        offset = m_restoreOffset;
    } else if (!highlight) {
        const auto it = m_scrollAnchors.constFind(navigationKey(m_roomId));
        if (it != m_scrollAnchors.cend())
            offset = it->pixelOffset;
    }
    m_restoreIsSelection = false;
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

void PaginationController::failNavigation(bool tooFar)
{
    const bool wasRestore = m_navigationPurpose == NavigationPurpose::Restore;
    const bool wasSelection = wasRestore && m_restoreIsSelection;
    clearNavigation(false);
    if (wasSelection) {
        qCInfo(lcPagination)
            << "selected-message return unreachable; previous position";
        restoreScrollAnchor(m_roomId);
        return;
    }
    if (wasRestore) {
        Q_EMIT restoreLatestRequested();
        return;
    }
    m_navigationMessage = tooFar ? tooFarTargetMessage()
                                 : unavailableTargetMessage();
    m_navigationMessageTimer.start(kNavigationMessageDurationMs);
    Q_EMIT navigationChanged();
}

void PaginationController::clearNavigation(bool clearMessage)
{
    bool changed = m_navigationPurpose == NavigationPurpose::Reply;
    m_navigationPurpose = NavigationPurpose::None;
    m_restoreIsSelection = false;
    m_navigationEventId.clear();
    m_navigationBatches = 0;
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
    emitStateChanged();
}
