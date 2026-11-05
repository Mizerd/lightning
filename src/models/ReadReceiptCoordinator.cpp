#include "models/ReadReceiptCoordinator.h"

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcReceipts, "lightning.timeline.receipts")

namespace {
constexpr int kDefaultDebounceMs = 800;
}

ReadReceiptCoordinator::ReadReceiptCoordinator(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDefaultDebounceMs);
    connect(&m_debounce, &QTimer::timeout,
            this, &ReadReceiptCoordinator::onDebounceElapsed);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout,
            this, &ReadReceiptCoordinator::onRetryElapsed);
}

void ReadReceiptCoordinator::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client) {
        flushFullyReadMarkerFor(m_client, m_openRoomId);
        m_client->disconnect(this);
    }
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::loggedOut,
                this, &ReadReceiptCoordinator::onLoggedOut);
        connect(m_client, &MatrixClient::roomsChanged,
                this, &ReadReceiptCoordinator::reevaluate);
        connect(m_client, &MatrixClient::loginSucceeded,
                this, &ReadReceiptCoordinator::onLoginSucceeded);
        connect(m_client, &MatrixClient::readReceiptAccepted,
                this, &ReadReceiptCoordinator::onReceiptAccepted);
        connect(m_client, &MatrixClient::readReceiptFailed,
                this, &ReadReceiptCoordinator::onReceiptFailed);
        connect(m_client, &MatrixClient::readMarkerAdvanced,
                this, &ReadReceiptCoordinator::onReadMarkerAdvanced);
        connect(m_client, &MatrixClient::markRoomReadFailed,
                this, &ReadReceiptCoordinator::onMarkRoomReadFailed);
    }
    m_debounce.stop();
    ++m_generation;
    resetReceiptState();
}

void ReadReceiptCoordinator::setTimelineModel(TimelineModel *model)
{
    if (m_model == model)
        return;
    if (m_model)
        m_model->disconnect(this);
    m_model = model;
    if (m_model) {
        connect(m_model, &TimelineModel::roomIdChanged,
                this, &ReadReceiptCoordinator::onRoomChanged);
        connect(m_model, &TimelineModel::countChanged,
                this, &ReadReceiptCoordinator::reevaluate);
        connect(m_model, &TimelineModel::modelReset,
                this, &ReadReceiptCoordinator::reevaluate);
    }
    m_openRoomId = m_model ? m_model->roomId() : QString();
    m_debounce.stop();
    ++m_generation;
}

void ReadReceiptCoordinator::setWindowActive(bool active)
{
    if (m_windowActive == active)
        return;
    m_windowActive = active;
    if (!active) {
        flushFullyReadMarker();
    } else if (!m_openRoomId.isEmpty() && !m_retry.isActive()) {
        m_failures.remove(m_openRoomId);
    }
    Q_EMIT inputsChanged();
    reevaluate();
}

void ReadReceiptCoordinator::setTimelineVisible(bool visible)
{
    if (m_timelineVisible == visible)
        return;
    m_timelineVisible = visible;
    Q_EMIT inputsChanged();
    reevaluate();
}

void ReadReceiptCoordinator::setNearBottom(bool nearBottom)
{
    if (m_nearBottom == nearBottom)
        return;
    m_nearBottom = nearBottom;
    Q_EMIT inputsChanged();
    reevaluate();
}

bool ReadReceiptCoordinator::conditionsHold() const
{
    return m_client && m_model && !m_model->roomId().isEmpty()
        && m_windowActive && m_timelineVisible && m_nearBottom;
}

QString ReadReceiptCoordinator::eligibleEventId(qint64 *timestampMs) const
{
    if (!conditionsHold())
        return {};
    qint64 ts = 0;
    const QString eventId = m_model->latestReadableEventId(&ts);
    if (eventId.isEmpty())
        return {};
    const auto last = m_lastSent.constFind(m_model->roomId());
    if (last != m_lastSent.constEnd()) {
        // Never regress to an older event. Order by POSITION when the
        // last-acked event is still in the model: origin_server_ts is not
        // monotonic with stream order in federated rooms (clock skew), and
        // comparing it silently dropped the receipt for a genuinely newer
        // event, leaving big public rooms unread. The timestamp is only a
        // fallback for a snapshot that no longer contains the acked event.
        const int lastRow = m_model->rowForStableId(last->eventId);
        const int candRow = m_model->rowForStableId(eventId);
        if (lastRow >= 0 && candRow >= 0) {
            if (candRow < lastRow)
                return {};
        } else if (ts > 0 && last->timestampMs > 0 && ts < last->timestampMs) {
            return {};
        }
        if (last->eventId == eventId && !clearsMarkedUnread()
            && !roomCountMovedSinceReceipt(eventId)) {
            return {};
        }
    }
    const auto failure = m_failures.constFind(m_model->roomId());
    if (failure != m_failures.constEnd() && failure->eventId == eventId
        && (failure->waiting || failure->count > kMaxReceiptRetries))
        return {};
    if (timestampMs)
        *timestampMs = ts;
    return eventId;
}

void ReadReceiptCoordinator::reevaluate()
{
    forgetClearedCounts();
    qint64 ts = 0;
    const QString eventId = eligibleEventId(&ts);
    if (eventId.isEmpty()) {
        m_debounce.stop();
        return;
    }
    const QString roomId = m_model->roomId();
    if (m_debounce.isActive() && m_armedRoomId == roomId
        && m_armedGeneration == m_generation && m_armedEventId == eventId) {
        return;
    }
    m_armedRoomId = roomId;
    m_armedGeneration = m_generation;
    m_armedEventId = eventId;
    m_debounce.start();
}

void ReadReceiptCoordinator::onDebounceElapsed()
{
    if (!conditionsHold())
        return;
    if (m_armedGeneration != m_generation
        || m_armedRoomId != m_model->roomId())
        return;
    qint64 ts = 0;
    const QString eventId = eligibleEventId(&ts);
    if (eventId.isEmpty())
        return;
    sendNow(eventId, ts);
}

bool ReadReceiptCoordinator::clearsMarkedUnread() const
{
    return m_openedMarkedUnread && roomIsMarkedUnread();
}

int ReadReceiptCoordinator::roomCount(const QString &roomId) const
{
    for (const auto &room : m_client->rooms()) {
        if (room.id == roomId)
            return qMax(0, room.unreadCount) + qMax(0, room.highlightCount)
                + (room.hasUnreadMessages ? 1 : 0);
    }
    return 0;
}

// A reaction or an ended poll is not a timeline row, so the newest READABLE
// event does not move when one arrives, yet the server or matrix-sdk counts it
// and the receipt at that older event never covered it. The same event is
// therefore sent again (the backend moves it past such events), once per
// distinct non-zero count; a count that does not clear is not asked again.
bool ReadReceiptCoordinator::roomCountMovedSinceReceipt(const QString &eventId) const
{
    const QString roomId = m_model->roomId();
    const int now = roomCount(roomId);
    if (now <= 0)
        return false;
    const auto at = m_countAtSend.constFind(roomId);
    return at == m_countAtSend.constEnd() || at->eventId != eventId
        || at->count != now;
}

void ReadReceiptCoordinator::forgetClearedCounts()
{
    for (auto it = m_countAtSend.begin(); it != m_countAtSend.end();) {
        if (roomCount(it.key()) <= 0)
            it = m_countAtSend.erase(it);
        else
            ++it;
    }
}

bool ReadReceiptCoordinator::roomIsMarkedUnread() const
{
    for (const auto &room : m_client->rooms()) {
        if (room.id == m_model->roomId())
            return room.markedUnread;
    }
    return false;
}

void ReadReceiptCoordinator::sendNow(const QString &eventId, qint64 timestampMs)
{
    const QString roomId = m_model->roomId();
    const bool clearMark = clearsMarkedUnread();
    m_openedMarkedUnread = false;
    const bool viaMarkRead = clearMark && m_client->supportsMarkRoomRead();
    // Recorded BEFORE the call: a backend may answer synchronously. The
    // in-flight entry also keeps the same receipt from being sent twice while
    // it is unanswered. receiptSent waits for the server (onReceiptAccepted).
    m_lastSent.insert(roomId, { eventId, timestampMs });
    m_countAtSend.insert(roomId, { eventId, roomCount(roomId) });
    m_inFlight.insert(roomId, { eventId, timestampMs, viaMarkRead });
    m_seenUpTo.insert(roomId, { eventId, m_client->currentUserId() });
    qCInfo(lcReceipts) << "read receipt queued event_id=" << eventId;
    if (viaMarkRead) {
        m_fullyReadWritten.insert(roomId, eventId);
        m_client->markRoomRead(roomId);
    } else {
        m_client->sendReadReceipt(roomId, eventId);
        if (clearMark)
            m_client->setRoomMarkedUnread(roomId, false);
    }
}

void ReadReceiptCoordinator::onReceiptAccepted(const QString &roomId,
                                               const QString &eventId)
{
    const auto it = m_inFlight.constFind(roomId);
    if (it == m_inFlight.constEnd() || it->viaMarkRead || it->eventId != eventId)
        return;
    acceptInFlight(roomId);
}

void ReadReceiptCoordinator::onReceiptFailed(const QString &roomId,
                                             const QString &eventId)
{
    const auto it = m_inFlight.constFind(roomId);
    if (it == m_inFlight.constEnd() || it->viaMarkRead || it->eventId != eventId)
        return;
    failInFlight(roomId);
}

void ReadReceiptCoordinator::onReadMarkerAdvanced(const QString &roomId)
{
    const auto it = m_inFlight.constFind(roomId);
    if (it != m_inFlight.constEnd() && it->viaMarkRead)
        acceptInFlight(roomId);
}

void ReadReceiptCoordinator::onMarkRoomReadFailed(const QString &roomId)
{
    const auto it = m_inFlight.constFind(roomId);
    if (it != m_inFlight.constEnd() && it->viaMarkRead)
        failInFlight(roomId);
}

void ReadReceiptCoordinator::acceptInFlight(const QString &roomId)
{
    const InFlightReceipt done = m_inFlight.take(roomId);
    m_lastAccepted.insert(roomId, { done.eventId, done.timestampMs });
    m_failures.remove(roomId);
    if (m_retryRoomId == roomId) {
        m_retry.stop();
        m_retryRoomId.clear();
    }
    qCInfo(lcReceipts) << "read receipt accepted event_id=" << done.eventId;
    Q_EMIT receiptSent(roomId, done.eventId, done.timestampMs);
}

void ReadReceiptCoordinator::failInFlight(const QString &roomId)
{
    const InFlightReceipt failed = m_inFlight.take(roomId);
    const auto accepted = m_lastAccepted.constFind(roomId);
    if (accepted != m_lastAccepted.constEnd())
        m_lastSent.insert(roomId, accepted.value());
    else
        m_lastSent.remove(roomId);
    if (failed.viaMarkRead) {
        m_fullyReadWritten.remove(roomId);
        if (roomId == m_openRoomId)
            m_openedMarkedUnread = true;
    }
    ReceiptFailure &failure = m_failures[roomId];
    if (failure.eventId != failed.eventId) {
        failure.eventId = failed.eventId;
        failure.count = 0;
    }
    ++failure.count;
    failure.waiting = false;
    if (failure.count <= kMaxReceiptRetries && roomId == m_openRoomId) {
        failure.waiting = true;
        m_retryRoomId = roomId;
        m_retry.start(m_retryBaseMs << (failure.count - 1));
        qCInfo(lcReceipts) << "read receipt failed; retrying attempt="
                           << failure.count;
    } else {
        qCInfo(lcReceipts) << "read receipt failed; waiting for the reader"
                           << "attempts=" << failure.count;
    }
}

void ReadReceiptCoordinator::onRetryElapsed()
{
    const QString roomId = m_retryRoomId;
    m_retryRoomId.clear();
    const auto failure = m_failures.find(roomId);
    if (failure != m_failures.end())
        failure->waiting = false;
    reevaluate();
}

void ReadReceiptCoordinator::flushFullyReadMarker()
{
    flushFullyReadMarkerFor(m_client, m_openRoomId);
}

void ReadReceiptCoordinator::flushFullyReadMarkerFor(MatrixClient *client,
                                                     const QString &roomId)
{
    if (!client || roomId.isEmpty())
        return;
    const SeenEvent seen = m_seenUpTo.value(roomId);
    if (seen.eventId.isEmpty() || m_fullyReadWritten.value(roomId) == seen.eventId)
        return;
    if (seen.userId != client->currentUserId()) {
        m_seenUpTo.remove(roomId);
        return;
    }
    m_fullyReadWritten.insert(roomId, seen.eventId);
    qCInfo(lcReceipts) << "fully-read marker advanced on leave event_id="
                       << seen.eventId;
    client->setFullyReadMarker(roomId, seen.eventId);
    Q_EMIT fullyReadMarkerMoved(roomId, seen.eventId);
}

void ReadReceiptCoordinator::resetReceiptState()
{
    m_retry.stop();
    m_retryRoomId.clear();
    m_lastSent.clear();
    m_countAtSend.clear();
    m_lastAccepted.clear();
    m_inFlight.clear();
    m_failures.clear();
    m_seenUpTo.clear();
    m_fullyReadWritten.clear();
}

void ReadReceiptCoordinator::onRoomChanged()
{
    m_debounce.stop();
    m_retry.stop();
    m_retryRoomId.clear();
    flushFullyReadMarkerFor(m_client, m_openRoomId);
    m_openRoomId = m_model ? m_model->roomId() : QString();
    if (!m_openRoomId.isEmpty())
        m_failures.remove(m_openRoomId);
    ++m_generation;
    m_openedMarkedUnread = m_client && m_model && roomIsMarkedUnread();
    reevaluate();
}

void ReadReceiptCoordinator::onLoginSucceeded(const QString &userId)
{
    if (!m_userId.isEmpty() && userId != m_userId) {
        m_debounce.stop();
        ++m_generation;
        resetReceiptState();
    }
    m_userId = userId;
}

void ReadReceiptCoordinator::onLoggedOut()
{
    m_debounce.stop();
    ++m_generation;
    resetReceiptState();
    qCInfo(lcReceipts) << "read receipts stopped on sign-out";
}
