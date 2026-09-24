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
        if (last->eventId == eventId)
            return {}; // same receipt already sent
        if (ts > 0 && last->timestampMs > 0 && ts < last->timestampMs)
            return {}; // never regress to an older event
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

void ReadReceiptCoordinator::sendNow(const QString &eventId, qint64 timestampMs)
{
    const QString roomId = m_model->roomId();
    m_client->sendReadReceipt(roomId, eventId);
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
    reevaluate();
}

void ReadReceiptCoordinator::onLoggedOut()
{
    m_debounce.stop();
    ++m_generation;
    m_lastSent.clear();
    qCInfo(lcReceipts) << "read receipts stopped on sign-out";
}
