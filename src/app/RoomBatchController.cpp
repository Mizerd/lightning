#include "app/RoomBatchController.h"
#include "app/RoomInfoController.h"
#include "models/RoomListModel.h"
#include "matrix/MatrixClient.h"
#include <QTimer>
#include <utility>

RoomBatchController::RoomBatchController(QObject *parent) : QObject(parent) {}
bool RoomBatchController::markReadSupported() const { return m_client && m_client->supportsMarkRoomRead(); }

void RoomBatchController::configure(MatrixClient *client, RoomListModel *rooms,
                                    RoomInfoController *info,
                                    std::function<void(const QString &, int)> write,
                                    std::function<int(const QString &)> read)
{
    m_client = client;
    m_rooms = rooms;
    m_info = info;
    m_notificationWrite = std::move(write);
    m_notificationRead = std::move(read);
    connect(client, &MatrixClient::loggedOut, this, [this] { setAccount({}); });
    connect(rooms, &RoomListModel::roomMarkedRead, this, [this](const QString &id) {
        if (m_action == QLatin1String("read")) finish(id, true);
    });
    connect(client, &MatrixClient::markRoomReadFailed, this, [this](const QString &id) {
        if (m_action == QLatin1String("read")) finish(id, false);
    });
    const auto checkTag = [this] {
        if (m_active.isEmpty() || (m_action != QLatin1String("favourite")
                                   && m_action != QLatin1String("low_priority"))) return;
        for (const auto &room : m_client->rooms())
            if (room.id == m_active
                && (m_action == QLatin1String("favourite")
                    ? room.isFavourite : room.lowPriority) == (m_mode == 1)) {
                const QString id = m_active;
                finish(id, true);
                return;
            }
    };
    connect(client, &MatrixClient::roomUpdated, this,
            [checkTag](const QString &) { checkTag(); });
    connect(client, &MatrixClient::roomsChanged, this, checkTag);
    connect(client, &MatrixClient::roomNotificationModeChanged, this,
            [this](const QString &id, int mode, bool defined) {
        if (m_action == QLatin1String("notification") && m_mode != 3
            && defined && mode == m_mode) finish(id, true);
    });
    connect(client, &MatrixClient::roomNotificationModeCleared, this, [this](const QString &id) {
        if (m_action == QLatin1String("notification") && m_mode == 3) finish(id, true);
    });
    connect(client, &MatrixClient::roomNotificationModeWriteFailed, this, [this](const QString &id) {
        if (m_action == QLatin1String("notification")) finish(id, false);
    });
    connect(info, &RoomInfoController::roomLeft, this, [this](const QString &id) {
        if (m_action == QLatin1String("leave")) finish(id, true);
    });
    connect(info, &RoomInfoController::roomLeaveFailed, this,
            [this](const QString &id, const QString &) {
        if (m_action == QLatin1String("leave")) finish(id, false);
    });
}

void RoomBatchController::setAccount(const QString &id)
{
    if (id == m_account && !id.isEmpty()) return;
    m_account = id;
    clear();
}

void RoomBatchController::clear()
{
    ++m_generation;
    disconnect(m_tagFailureConnection);
    m_selected.clear(); m_order.clear(); m_results.clear(); m_queue.clear();
    m_active.clear(); m_action.clear(); m_mode = -1;
    m_selecting = false; m_confirmLeave = false;
    emit changed();
}

bool RoomBatchController::validRoom(const QString &id) const
{
    if (m_account.isEmpty() || id.isEmpty() || !m_rooms) return false;
    return m_rooms->findRoom(id).value(QStringLiteral("membership")).toString()
           == QLatin1String("joined");
}

void RoomBatchController::select(const QString &id)
{
    if (!validRoom(id) || busy()) return;
    m_selecting = true;
    if (!m_selected.contains(id)) { m_selected.insert(id); m_order.append(id); }
    emit changed();
}
void RoomBatchController::toggle(const QString &id)
{
    if (!validRoom(id) || busy()) return;
    m_selecting = true;
    if (m_selected.remove(id)) m_order.removeAll(id);
    else { m_selected.insert(id); m_order.append(id); }
    emit changed();
}
void RoomBatchController::selectRange(const QStringList &ids)
{
    if (busy()) return;
    bool changedSelection = false;
    for (const auto &id : ids) {
        if (!validRoom(id) || m_selected.contains(id)) continue;
        m_selected.insert(id);
        m_order.append(id);
        changedSelection = true;
    }
    if (changedSelection) {
        m_selecting = true;
        emit changed();
    }
}
void RoomBatchController::exitSelection() { clear(); }

int RoomBatchController::succeeded() const { return m_results.values().count(QStringLiteral("succeeded")); }
int RoomBatchController::failed() const { return m_results.values().count(QStringLiteral("failed")); }
int RoomBatchController::pending() const { return m_results.values().count(QStringLiteral("pending")); }
QVariantList RoomBatchController::outcomes() const
{
    QVariantList rows;
    for (const auto &id : m_order)
        if (m_results.contains(id)) rows.append(QVariantMap{{QStringLiteral("roomId"), id},
            {QStringLiteral("name"), m_rooms ? m_rooms->findRoom(id).value(QStringLiteral("name")) : QVariant()},
            {QStringLiteral("status"), m_results.value(id)}});
    return rows;
}

void RoomBatchController::run(const QString &action, int mode)
{
    if (busy() || m_confirmLeave || m_account.isEmpty() || m_order.isEmpty()) return;
    if (action != QLatin1String("read") && action != QLatin1String("favourite")
        && action != QLatin1String("low_priority")
        && action != QLatin1String("notification")) return;
    if (((action == QLatin1String("favourite") || action == QLatin1String("low_priority"))
         && (!m_client->supportsRoomFavourites() || (mode != 0 && mode != 1)))
        || (action == QLatin1String("read") && !m_client->supportsMarkRoomRead())
        || (action == QLatin1String("notification") && (mode < 0 || mode > 3))) return;
    ++m_generation;
    m_action = action; m_mode = mode; m_results.clear();
    for (const auto &id : m_order) {
        m_results.insert(id, QStringLiteral("pending"));
        m_queue.append(id);
    }
    emit changed(); advance();
}
void RoomBatchController::retryFailed()
{
    if (busy() || m_confirmLeave || m_account.isEmpty() || !canRetry()) return;
    ++m_generation;
    for (const auto &id : m_order) {
        if (m_results.value(id) != QLatin1String("failed")) continue;
        m_results[id] = QStringLiteral("pending");
        m_queue.append(id);
    }
    emit changed(); advance();
}
void RoomBatchController::requestLeave()
{
    if (busy() || m_order.isEmpty() || m_account.isEmpty()) return;
    m_confirmLeave = true; emit changed();
}
void RoomBatchController::cancelLeave() { m_confirmLeave = false; emit changed(); }
void RoomBatchController::confirmLeave()
{
    if (!m_confirmLeave || busy() || m_account.isEmpty()) return;
    ++m_generation;
    m_confirmLeave = false; m_action = QStringLiteral("leave"); m_results.clear();
    for (const auto &id : m_order) { m_results.insert(id, QStringLiteral("pending")); m_queue.append(id); }
    emit changed(); advance();
}
void RoomBatchController::advance()
{
    if (!m_active.isEmpty() || m_queue.isEmpty()) { emit changed(); return; }
    m_active = m_queue.takeFirst();
    const auto id = m_active;
    if (!validRoom(id)) { finish(id, false); return; }
    if (m_action == QLatin1String("favourite") || m_action == QLatin1String("low_priority")) {
        for (const auto &room : m_client->rooms())
            if (room.id == id
                && (m_action == QLatin1String("favourite")
                    ? room.isFavourite : room.lowPriority) == (m_mode == 1)) {
                finish(id, true);
                return;
            }
    }
    if (m_action == QLatin1String("favourite")
        || m_action == QLatin1String("low_priority")) {
        const auto generation = m_generation;
        const QString action = m_action;
        m_tagFailureConnection = connect(m_client, &MatrixClient::roomActionFailed,
            this, [this, generation, id, action](const QString &failedAction,
                                                  const QString &failedRoom) {
                if (generation == m_generation && id == m_active
                    && failedAction == action && failedRoom == id)
                    finish(id, false);
            });
    }
    if (m_action == QLatin1String("read")) m_rooms->markRoomRead(id);
    else if (m_action == QLatin1String("favourite")) m_rooms->setRoomFavourite(id, m_mode == 1);
    else if (m_action == QLatin1String("low_priority")) m_rooms->setRoomLowPriority(id, m_mode == 1);
    else if (m_action == QLatin1String("notification")) {
        m_notificationWrite(id, m_mode);
        if (!m_client->supportsServerNotificationModes() && m_notificationRead
            && m_notificationRead(id) == m_mode) {
            finish(id, true);
            return;
        }
    }
    else if (m_action == QLatin1String("leave")) m_info->leaveRoom(id);
    // A backend that never reports an outcome must not block the rest forever.
    // Timeout is failure, never inferred success. Generation prevents old work
    // from landing in a newly selected account.
    const auto generation = m_generation;
    QTimer::singleShot(30000, this, [this, generation, id] {
        if (generation == m_generation && id == m_active) finish(id, false);
    });
    emit changed();
}
void RoomBatchController::finish(const QString &id, bool ok)
{
    if (id != m_active || m_active.isEmpty()) return;
    disconnect(m_tagFailureConnection);
    m_results[id] = ok ? QStringLiteral("succeeded") : QStringLiteral("failed");
    m_active.clear();
    emit changed();
    // Defer the next dispatch so a synchronous mock acknowledgement cannot
    // recurse through the entire selection.
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] { if (generation == m_generation) advance(); });
}
