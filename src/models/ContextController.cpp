#include "models/ContextController.h"

#include "matrix/MatrixClient.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcContext, "lightning.timeline.context")

ContextController::ContextController(QObject *parent)
    : QObject(parent)
{
}

void ContextController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    close();
    m_client = client;
    m_model.setClient(client);
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::timelineReset, this,
            [this](const QString &timelineId) { onTimelineReset(timelineId); });
    connect(m_client, &MatrixClient::eventContextFailed, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &category) {
        // Identity is the staleness gate: only the view being opened fails.
        if (m_state != Opening || roomId != m_roomId || eventId != m_eventId)
            return;
        qCWarning(lcContext) << "event context failed category=" << category;
        const QString room = m_roomId;
        const QString event = m_eventId;
        close();
        Q_EMIT openFailed(room, event, category);
    });
    connect(m_client, &MatrixClient::eventContextPagination, this,
            [this](const QString &roomId, const QString &eventId, bool forward,
                   const QString &state, bool reachedEdge) {
        onPagination(roomId, eventId, forward, state, reachedEdge);
    });
    connect(m_client, &MatrixClient::loggedOut, this, [this] { close(); });
}

bool ContextController::supported() const
{
    return m_client && m_client->supportsEventContext();
}

QString ContextController::timelineId() const
{
    return m_roomId.isEmpty() || m_eventId.isEmpty()
        ? QString{}
        : MatrixClient::contextTimelineId(m_roomId, m_eventId);
}

bool ContextController::canLoadOlder() const
{
    return m_state == Ready && !m_loadingOlder && !m_reachedStart
        && m_olderPages < kMaxEdgePages;
}

bool ContextController::canLoadNewer() const
{
    return m_state == Ready && !m_loadingNewer && !m_reachedEnd
        && m_newerPages < kMaxEdgePages;
}

bool ContextController::open(const QString &roomId, const QString &eventId)
{
    if (!supported() || roomId.isEmpty() || eventId.isEmpty())
        return false;
    if (m_state != Closed && m_roomId == roomId && m_eventId == eventId
        && m_state != Failed)
        return true; // already open or opening
    // A previous view is replaced, not stacked: the backend holds one.
    if (m_state != Closed)
        m_client->closeEventContext();
    m_roomId = roomId;
    m_eventId = eventId;
    m_failureCategory.clear();
    resetEdges();
    // Bind the model before dispatching so the snapshot reset is applied,
    // never raced.
    m_model.setRoomId(timelineId());
    setState(Opening);
    m_client->openEventContext(roomId, eventId);
    return true;
}

void ContextController::close()
{
    const bool wasActive = m_state != Closed;
    if (m_client && wasActive)
        m_client->closeEventContext();
    m_roomId.clear();
    m_eventId.clear();
    m_failureCategory.clear();
    m_model.setRoomId(QString{});
    resetEdges();
    if (wasActive)
        setState(Closed);
}

void ContextController::jumpToLatest()
{
    if (m_state == Closed)
        return;
    close();
    Q_EMIT latestRequested();
}

void ContextController::handleCurrentRoomChanged(const QString &currentRoomId)
{
    if (m_state != Closed && currentRoomId != m_roomId)
        close();
}

void ContextController::resetEdges()
{
    const bool changed = m_loadingOlder || m_loadingNewer || m_reachedStart
        || m_reachedEnd || m_olderFailed || m_newerFailed || m_olderPages
        || m_newerPages;
    m_loadingOlder = m_loadingNewer = false;
    m_reachedStart = m_reachedEnd = false;
    m_olderFailed = m_newerFailed = false;
    m_olderPages = m_newerPages = 0;
    if (changed)
        Q_EMIT edgesChanged();
}

void ContextController::setState(State state, const QString &failureCategory)
{
    if (m_state == state && m_failureCategory == failureCategory)
        return;
    m_state = state;
    m_failureCategory = failureCategory;
    Q_EMIT stateChanged();
    Q_EMIT edgesChanged();
}

int ContextController::targetRow() const
{
    return m_eventId.isEmpty() ? -1 : m_model.rowForStableId(m_eventId);
}

void ContextController::onTimelineReset(const QString &timelineId)
{
    // Only the requested view's first reset promotes to Ready; composite-id
    // identity is the staleness gate.
    if (m_state != Opening || timelineId != this->timelineId())
        return;
    const int row = targetRow();
    if (row < 0) {
        // The server answered without the event (redacted, filtered, or not
        // visible to us): nothing to show here, fall back.
        const QString room = m_roomId;
        const QString event = m_eventId;
        close();
        Q_EMIT openFailed(room, event, QStringLiteral("target_missing"));
        return;
    }
    const QString root = m_model.events().at(row).threadRootId;
    if (!root.isEmpty() && root != m_eventId) {
        // A thread reply belongs to its thread panel (CLAUDE.md §8).
        const QString room = m_roomId;
        const QString event = m_eventId;
        close();
        Q_EMIT threadReplyHit(room, root, event);
        return;
    }
    setState(Ready);
    Q_EMIT targetLocated(row);
}

void ContextController::loadOlder()
{
    if (!canLoadOlder() || !m_client)
        return;
    m_loadingOlder = true;
    m_olderFailed = false;
    ++m_olderPages;
    Q_EMIT edgesChanged();
    m_client->paginateEventContext(m_roomId, m_eventId, /*forward=*/false);
}

void ContextController::loadNewer()
{
    if (!canLoadNewer() || !m_client)
        return;
    m_loadingNewer = true;
    m_newerFailed = false;
    ++m_newerPages;
    Q_EMIT edgesChanged();
    m_client->paginateEventContext(m_roomId, m_eventId, /*forward=*/true);
}

void ContextController::onPagination(const QString &roomId,
                                     const QString &eventId, bool forward,
                                     const QString &state, bool reachedEdge)
{
    if (m_state != Ready || roomId != m_roomId || eventId != m_eventId)
        return; // stale: another view, or none
    bool &loading = forward ? m_loadingNewer : m_loadingOlder;
    bool &failed = forward ? m_newerFailed : m_olderFailed;
    bool &reached = forward ? m_reachedEnd : m_reachedStart;
    if (state == QLatin1String("loading")) {
        loading = true;
        failed = false;
    } else if (state == QLatin1String("idle")) {
        loading = false;
        failed = false;
        reached = reachedEdge;
    } else if (state == QLatin1String("failed")) {
        loading = false;
        failed = true;
        // A failed page does not count against the bound.
        int &pages = forward ? m_newerPages : m_olderPages;
        if (pages > 0)
            --pages;
    }
    Q_EMIT edgesChanged();
}
