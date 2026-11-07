#include "app/AutoMarkReadController.h"

#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcAutoMarkRead, "lightning.rooms.automarkread")

namespace {
constexpr int kStepTimeoutMs = 15000;
}

AutoMarkReadController::AutoMarkReadController(QObject *parent)
    : QObject(parent)
{
    m_interval.setSingleShot(false);
    connect(&m_interval, &QTimer::timeout, this, &AutoMarkReadController::startPass);
    m_stepTimeout.setSingleShot(true);
    m_stepTimeout.setInterval(kStepTimeoutMs);
    connect(&m_stepTimeout, &QTimer::timeout, this, &AutoMarkReadController::stepTimedOut);
}

void AutoMarkReadController::setSources(MatrixClient *client, RoomListModel *rooms)
{
    for (const auto &connection : std::as_const(m_clientConnections))
        disconnect(connection);
    m_clientConnections.clear();
    m_client = client;
    m_rooms = rooms;
    if (m_client) {
        m_clientConnections.append(connect(m_client, &MatrixClient::loggedOut, this,
                                           [this] { setAccount({}); }));
        m_clientConnections.append(connect(m_client, &MatrixClient::markRoomReadFailed,
                                           this, &AutoMarkReadController::finishRoom));
    }
    if (m_rooms)
        m_clientConnections.append(connect(m_rooms, &RoomListModel::roomMarkedRead,
                                           this, &AutoMarkReadController::finishRoom));
    setAccount({});
}

void AutoMarkReadController::setExcludedRooms(std::function<QStringList()> excluded)
{
    m_excluded = std::move(excluded);
}

void AutoMarkReadController::setIntervalMinutes(int minutes)
{
    minutes = qMax(0, minutes);
    if (minutes == m_minutes)
        return;
    m_minutes = minutes;
    if (m_minutes == 0)
        stopPass();
    rearm();
}

void AutoMarkReadController::setAccount(const QString &userId)
{
    if (userId == m_account && !userId.isEmpty())
        return;
    m_account = userId;
    stopPass();
    rearm();
}

void AutoMarkReadController::rearm()
{
    m_interval.stop();
    const int ms = m_testIntervalMs > 0 ? m_testIntervalMs : m_minutes * 60000;
    if (m_minutes <= 0 || m_account.isEmpty() || ms <= 0)
        return;
    m_interval.start(ms);
}

void AutoMarkReadController::startPass()
{
    if (passRunning() || !m_client || !m_rooms || m_account.isEmpty()
        || m_client->currentUserId() != m_account)
        return;
    m_queue = m_rooms->roomsToMarkRead();
    qCInfo(lcAutoMarkRead) << "pass rooms=" << m_queue.size();
    Q_EMIT passStarted(int(m_queue.size()));
    next();
}

void AutoMarkReadController::next()
{
    while (!m_queue.isEmpty() && m_active.isEmpty()) {
        if (!m_client || !m_rooms || m_client->currentUserId() != m_account) {
            stopPass();
            return;
        }
        const QString roomId = m_queue.takeFirst();
        if (!m_rooms->roomNeedsMarkRead(roomId))
            continue;
        if (m_excluded && m_excluded().contains(roomId))
            continue;
        m_active = roomId;
        m_stepTimeout.start();
        m_rooms->markRoomRead(roomId);
    }
}

void AutoMarkReadController::finishRoom(const QString &roomId)
{
    if (m_active.isEmpty() || roomId != m_active)
        return;
    m_stepTimeout.stop();
    m_active.clear();
    m_overdue = false;
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == m_generation)
            next();
    }, Qt::QueuedConnection);
}

// A timed-out request is still in flight: wait for it once more rather than
// start another, then give the pass up.
void AutoMarkReadController::stepTimedOut()
{
    if (m_active.isEmpty())
        return;
    if (!m_overdue) {
        m_overdue = true;
        qCInfo(lcAutoMarkRead) << "step overdue; holding the pass";
        m_stepTimeout.start();
        return;
    }
    qCInfo(lcAutoMarkRead) << "step abandoned; pass stopped left=" << m_queue.size();
    stopPass();
}

void AutoMarkReadController::stopPass()
{
    ++m_generation;
    m_stepTimeout.stop();
    m_queue.clear();
    m_active.clear();
    m_overdue = false;
}
