#include "app/BulkRedactionController.h"

#include "matrix/MatrixClient.h"
#include "models/TimelineModel.h"

#include <QTimer>
#include <QGuiApplication>

namespace {
QString requestKey(const QString &roomId, const QString &eventId)
{
    return roomId + QChar(0x1e) + eventId;
}
}

BulkRedactionController::BulkRedactionController(QObject *parent)
    : QObject(parent)
{}

void BulkRedactionController::setClient(MatrixClient *client)
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    ++m_generation;
    m_client = client;
    m_model = nullptr;
    m_ids.clear();
    m_anchor.clear();
    m_queue.clear();
    m_inFlight.clear();
    m_reason.clear();
    m_selecting = false;
    m_running = false;
    m_cancelled = false;
    m_completed = m_total = m_failed = 0;
    m_error.clear();
    m_abandonedRequests.clear();
    if (client)
        connect(client, &MatrixClient::redactionFinished, this,
                &BulkRedactionController::onResult);
    Q_EMIT changed();
}

void BulkRedactionController::begin(TimelineModel *model)
{
    if (m_running || !model || !m_client || model->roomId().isEmpty())
        return;
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    ++m_generation;
    m_model = model;
    connect(model, &TimelineModel::roomIdChanged, this,
            &BulkRedactionController::cancel);
    m_roomId.clear();
    m_timelineId = model->roomId();
    m_ids.clear();
    m_anchor.clear();
    m_queue.clear();
    m_inFlight.clear();
    m_selecting = true;
    m_cancelled = false;
    m_completed = m_total = m_failed = 0;
    m_error.clear();
    Q_EMIT changed();
}

void BulkRedactionController::add(const QString &eventId,
                                   bool canRedactOthers)
{
    if (!m_model || eventId.isEmpty())
        return;
    const QString roomId = m_model->realRoomIdForEvent(eventId);
    if (roomId.isEmpty() || (!m_roomId.isEmpty() && roomId != m_roomId))
        return;
    if (m_abandonedRequests.contains(requestKey(roomId, eventId))) {
        m_error = tr("A previous deletion of this message is still in progress.");
        return;
    }
    if (!m_model->canRedactEvent(eventId, canRedactOthers)) {
        m_error = tr("That message cannot be deleted with your current permissions.");
        return;
    }
    if (m_ids.size() >= 50) {
        m_error = tr("Select up to 50 messages at once.");
        return;
    }
    m_roomId = roomId;
    m_ids.append(eventId);
    m_error.clear();
}

void BulkRedactionController::toggle(const QString &eventId,
                                      bool canRedactOthers)
{
    if (!m_selecting || m_running)
        return;
    if (m_ids.removeOne(eventId)) {
        m_error.clear();
    } else {
        add(eventId, canRedactOthers);
    }
    m_anchor = eventId;
    Q_EMIT changed();
}

void BulkRedactionController::toggleFromPointer(const QString &eventId,
                                                bool canRedactOthers)
{
    if ((QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
        && !m_anchor.isEmpty()) {
        selectRange(m_anchor, eventId, canRedactOthers);
        return;
    }
    toggle(eventId, canRedactOthers);
}

void BulkRedactionController::selectRange(const QString &first,
                                           const QString &last,
                                           bool canRedactOthers)
{
    if (!m_selecting || m_running || !m_model)
        return;
    int from = -1, to = -1;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QString id = m_model->eventIdAt(row);
        if (id == first) from = row;
        if (id == last) to = row;
    }
    if (from < 0 || to < 0)
        return;
    int skipped = 0;
    for (int row = qMin(from, to); row <= qMax(from, to); ++row) {
        const QString id = m_model->eventIdAt(row);
        if (m_ids.contains(id))
            continue;
        const QModelIndex index = m_model->index(row, 0);
        if (id.isEmpty()
            || m_model->data(index, TimelineModel::IsVirtualRole).toBool()
            || m_model->data(index, TimelineModel::IsStateActivityRole).toBool()
            || m_model->data(index, TimelineModel::IsCallEventRole).toBool())
            continue;
        if (!m_model->canRedactEvent(id, canRedactOthers)) {
            ++skipped;
            continue;
        }
        add(id, canRedactOthers);
        if (m_ids.size() == 50)
            break;
    }
    if (skipped)
        m_error = tr("%n message(s) in the range cannot be deleted.", nullptr,
                     skipped);
    Q_EMIT changed();
}

bool BulkRedactionController::isSelected(const QString &eventId) const
{
    return m_ids.contains(eventId);
}

void BulkRedactionController::start(const QString &reason)
{
    if (!m_selecting || m_running || m_ids.isEmpty() || !m_model || !m_client)
        return;
    // A power-level change or a redaction by another client may have landed
    // since selection. The server is the final authority on each request.
    m_queue = m_ids;
    m_total = m_queue.size();
    m_completed = m_failed = 0;
    m_reason = reason;
    m_error.clear();
    m_running = true;
    m_selecting = false;
    m_cancelled = false;
    Q_EMIT changed();
    sendNext();
}

void BulkRedactionController::sendNext()
{
    if (!m_running || !m_client)
        return;
    if (m_cancelled || m_queue.isEmpty()) {
        finish();
        return;
    }
    m_inFlight = m_queue.takeFirst();
    // The selected model knows whether this is a thread; MatrixClient's
    // redaction path translates the composite id before the SDK call.
    m_client->redactEvent(m_timelineId, m_inFlight, m_reason);
}

void BulkRedactionController::onResult(const QString &roomId,
                                       const QString &eventId, bool ok,
                                       const QString &error)
{
    if (m_abandonedRequests.remove(requestKey(roomId, eventId))) {
        if (m_cancelled && !m_running && roomId == m_roomId) {
            ++m_completed;
            if (!ok) {
                ++m_failed;
                m_error = error.isEmpty()
                              ? tr("The server refused a redaction.") : error;
            } else {
                m_error.clear();
            }
            Q_EMIT changed();
        }
        return;
    }
    if (!m_running || roomId != m_roomId || eventId != m_inFlight)
        return;
    m_inFlight.clear();
    ++m_completed;
    if (!ok) {
        ++m_failed;
        m_error = error.isEmpty() ? tr("The server refused a redaction.") : error;
        // Do not continue flooding a room after a 429. A retry belongs to a
        // later, deliberate batch once the homeserver's limit has passed.
        if (error.contains(QLatin1String("M_LIMIT_EXCEEDED"))
            || error.contains(QLatin1String("rate limit"), Qt::CaseInsensitive)
            || error.contains(QLatin1String("429"))) {
            m_cancelled = true;
            m_queue.clear();
        }
    }
    Q_EMIT changed();
    const quint64 generation = m_generation;
    QTimer::singleShot(300, this, [this, generation] {
        if (generation == m_generation)
            sendNext();
    });
}

void BulkRedactionController::finish()
{
    m_running = false;
    m_queue.clear();
    m_inFlight.clear();
    m_ids.clear();
    m_reason.clear();
    ++m_generation;
    Q_EMIT changed();
}

void BulkRedactionController::cancel()
{
    if (m_running) {
        m_cancelled = true;
        m_queue.clear();
        if (!m_inFlight.isEmpty()) {
            m_abandonedRequests.insert(requestKey(m_roomId, m_inFlight));
            m_error = tr("One redaction was already in progress; its result is unknown.");
        }
        finish();
        return;
    }
    ++m_generation;
    m_selecting = false;
    m_ids.clear();
    m_error.clear();
    m_reason.clear();
    m_completed = m_total = m_failed = 0;
    m_cancelled = false;
    Q_EMIT changed();
}
