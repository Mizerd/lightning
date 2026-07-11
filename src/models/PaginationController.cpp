#include "models/PaginationController.h"

#include "matrix/MatrixClient.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcPagination, "lightning.timeline.pagination")

PaginationController::PaginationController(QObject *parent)
    : QObject(parent)
{
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
    resetPerRoomState();
    Q_EMIT roomIdChanged();
    Q_EMIT stateChanged();
}

void PaginationController::resetPerRoomState()
{
    ++m_generation;
    m_requestActive = false;
    m_activeReason = Reason::None;
    m_batchInserted = 0;
    m_fillRequests = 0;
    m_noProgressStrikes = 0;
    m_fillStopped = false;
}

bool PaginationController::busy() const
{
    if (m_requestActive)
        return true;
    return m_client && !m_roomId.isEmpty() && m_client->paginating(m_roomId);
}

bool PaginationController::reachedStart() const
{
    // The backend exposes reached-start only through canPaginate(); derive
    // it without conflating "loading right now" with "no more history".
    if (!m_client || m_roomId.isEmpty())
        return false;
    return !m_client->canPaginate(m_roomId) && !m_client->paginating(m_roomId)
        && !m_client->paginationFailed(m_roomId);
}

bool PaginationController::failed() const
{
    return m_client && !m_roomId.isEmpty() && m_client->paginationFailed(m_roomId);
}

const char *PaginationController::reasonName(Reason reason)
{
    switch (reason) {
    case Reason::ViewportFill: return "viewport_fill";
    case Reason::NearTop:      return "near_top";
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
    // A viewport fill never auto-retries a failed batch; that would loop on
    // a persistent error. Retry stays a user gesture.
    if (failed())
        return;
    request(Reason::ViewportFill);
}

void PaginationController::requestNearTop()
{
    if (failed())
        return;
    request(Reason::NearTop);
}

void PaginationController::retry()
{
    if (!failed())
        return;
    // NearTop semantics: an explicit user gesture. loadOlderMessages clears
    // the backend failure flag on the next dispatch.
    request(Reason::NearTop);
}

void PaginationController::request(Reason reason)
{
    if (!m_client || m_roomId.isEmpty())
        return;
    if (m_requestActive || m_client->paginating(m_roomId)) {
        qCDebug(lcPagination) << "timeline pagination duplicate suppressed reason="
                              << reasonName(reason);
        return;
    }
    // canPaginate() is false when there is no live timeline for the room,
    // a batch is loading, or the start of history was reached. Requesting
    // in any of those states would either be dropped silently (leaving the
    // controller stuck busy) or is pointless.
    if (!m_client->canPaginate(m_roomId) && !m_client->paginationFailed(m_roomId))
        return;

    m_requestActive = true;
    m_activeReason = reason;
    m_batchInserted = 0;
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
        m_activeReason = Reason::None;
    }
    Q_EMIT stateChanged();
}

void PaginationController::onEventsPrepended(const QString &roomId,
                                             const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId)
        return;
    m_batchInserted += static_cast<int>(events.size());
}

void PaginationController::onPaginationStateChanged(const QString &roomId)
{
    if (roomId != m_roomId || !m_client)
        return;

    if (m_client->paginating(m_roomId)) {
        // Adopt an externally started batch (e.g. legacy requestOlder())
        // so busy() and duplicate suppression stay truthful.
        if (!m_requestActive) {
            m_requestActive = true;
            m_activeReason = Reason::None;
            m_batchInserted = 0;
        }
        Q_EMIT stateChanged();
        return;
    }

    if (m_client->paginationFailed(m_roomId)) {
        if (m_requestActive) {
            qCWarning(lcPagination)
                << "timeline pagination failed retryable=true reason="
                << reasonName(m_activeReason) << "generation=" << m_generation;
        }
        m_requestActive = false;
        m_activeReason = Reason::None;
        Q_EMIT stateChanged();
        return;
    }

    // Neither loading nor failed: the batch completed (or the state was
    // reset underneath us). Only a batch this controller tracked counts.
    if (m_requestActive)
        finishBatch(reachedStart());
    Q_EMIT stateChanged();
}

void PaginationController::finishBatch(bool hitStart)
{
    const Reason reason = m_activeReason;
    const int inserted = m_batchInserted;
    m_requestActive = false;
    m_activeReason = Reason::None;
    m_batchInserted = 0;

    qCInfo(lcPagination) << "timeline pagination completed added=" << inserted
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

    Q_EMIT paginationCompleted(inserted, hitStart);
}

void PaginationController::onTimelineReset(const QString &roomId)
{
    if (roomId != m_roomId)
        return;
    // A fresh snapshot restarts anchor bookkeeping and the fill budget;
    // any batch that was in flight belongs to the previous generation.
    resetPerRoomState();
    Q_EMIT stateChanged();
}

void PaginationController::onLoggedOut()
{
    qCInfo(lcPagination) << "timeline pagination stopped on sign-out generation="
                         << m_generation;
    resetPerRoomState();
    Q_EMIT stateChanged();
}
