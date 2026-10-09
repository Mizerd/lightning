#include "models/ReadReceiptCoordinator.h"

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcReceipts, "lightning.timeline.receipts")

namespace {
// Long enough that flicking through a room does not ack it, short enough
// that a read conversation clears its badge promptly.
constexpr int kDefaultDebounceMs = 800;
} // namespace

ReadReceiptCoordinator::ReadReceiptCoordinator(QObject *parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDefaultDebounceMs);
    connect(&m_debounce, &QTimer::timeout,
            this, &ReadReceiptCoordinator::onDebounceElapsed);
}

void ReadReceiptCoordinator::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::loggedOut,
                this, &ReadReceiptCoordinator::onLoggedOut);
        connect(m_client, &MatrixClient::roomsChanged,
                this, &ReadReceiptCoordinator::reevaluate);
    }
    m_debounce.stop();
    ++m_generation;
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
        // Arrivals, resets, prepends and in-place updates all funnel through
        // countChanged or a model reset.
        connect(m_model, &TimelineModel::countChanged,
                this, &ReadReceiptCoordinator::reevaluate);
        connect(m_model, &TimelineModel::modelReset,
                this, &ReadReceiptCoordinator::reevaluate);
    }
    m_debounce.stop();
    ++m_generation;
}

void ReadReceiptCoordinator::setWindowActive(bool active)
{
    if (m_windowActive == active)
        return;
    m_windowActive = active;
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
        if (ts > 0 && last->timestampMs > 0 && ts < last->timestampMs)
            return {}; // never regress to an older event
        if (last->eventId == eventId && !clearsMarkedUnread())
            return {}; // same receipt already sent, no manual flag to clear
    }
    if (timestampMs)
        *timestampMs = ts;
    return eventId;
}

void ReadReceiptCoordinator::reevaluate()
{
    qint64 ts = 0;
    const QString eventId = eligibleEventId(&ts);
    if (eventId.isEmpty()) {
        // Conditions no longer hold (focus lost, scrolled up, room closed,
        // nothing new to ack): a pending receipt must not fire.
        m_debounce.stop();
        return;
    }
    const QString roomId = m_model->roomId();
    if (m_debounce.isActive() && m_armedRoomId == roomId
        && m_armedGeneration == m_generation && m_armedEventId == eventId) {
        return; // duplicate trigger for the same pending receipt
    }
    m_armedRoomId = roomId;
    m_armedGeneration = m_generation;
    m_armedEventId = eventId;
    m_debounce.start();
}

void ReadReceiptCoordinator::onDebounceElapsed()
{
    // Re-validate at fire time: same room, unchanged generation, conditions
    // still hold, and the newest eligible event (possibly newer than when
    // armed) wins.
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
    // Only a flag that was already set when the room was opened: marking the
    // open room unread must not be undone by the next re-evaluation.
    return m_openedMarkedUnread && roomIsMarkedUnread();
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
    // The SDK mark-read action sends the receipt and clears m.marked_unread.
    // A receipt alone leaves that flag set, even if there is nothing new.
    // Keep this behind the same visibility/debounce policy as ordinary reads.
    const bool clearMark = clearsMarkedUnread();
    m_openedMarkedUnread = false;
    if (clearMark && m_client->supportsMarkRoomRead()) {
        m_client->markRoomRead(roomId);
    } else {
        m_client->sendReadReceipt(roomId, eventId);
        if (clearMark)
            m_client->setRoomMarkedUnread(roomId, false);
    }
    m_lastSent.insert(roomId, { eventId, timestampMs });
    qCInfo(lcReceipts) << "read receipt sent event_id=" << eventId;
    Q_EMIT receiptSent(roomId, eventId, timestampMs);
}

void ReadReceiptCoordinator::onRoomChanged()
{
    // Drop any pending receipt from the previous room; the per-room last-sent
    // map still suppresses duplicates on return.
    m_debounce.stop();
    ++m_generation;
    m_openedMarkedUnread = m_client && m_model && roomIsMarkedUnread();
    reevaluate();
}

void ReadReceiptCoordinator::onLoggedOut()
{
    m_debounce.stop();
    ++m_generation;
    m_lastSent.clear();
    qCInfo(lcReceipts) << "read receipts stopped on sign-out";
}
