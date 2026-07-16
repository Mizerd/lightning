#include "threads/ThreadController.h"

#include "matrix/MatrixClient.h"

ThreadController::ThreadController(QObject *parent)
    : QObject(parent)
{
}

void ThreadController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    close();
    m_client = client;
    m_model.setClient(client);
    if (m_client) {
        connect(m_client, &MatrixClient::timelineReset, this,
                [this](const QString &timelineId) {
                    // Only the currently requested thread's reset promotes
                    // to Ready; anything else (rooms, stale threads) is not
                    // ours. Composite-id identity is the staleness gate.
                    if (m_state == Opening && timelineId == this->timelineId())
                        setState(Ready);
                });
        connect(m_client, &MatrixClient::threadTimelineFailed, this,
                [this](const QString &roomId, const QString &rootEventId,
                       const QString &category) {
                    if (m_state != Closed && roomId == m_roomId
                        && rootEventId == m_rootEventId)
                        setState(Failed, category);
                });
        connect(m_client, &MatrixClient::loggedOut, this,
                [this] { close(); });
    }
    Q_EMIT supportedChanged();
}

bool ThreadController::supported() const
{
    return m_client && m_client->supportsThreadTimelines();
}

QString ThreadController::timelineId() const
{
    if (m_roomId.isEmpty() || m_rootEventId.isEmpty())
        return {};
    return MatrixClient::threadTimelineId(m_roomId, m_rootEventId);
}

void ThreadController::openThread(const QString &roomId,
                                  const QString &rootEventId)
{
    if (!supported() || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    if (m_state != Closed && roomId == m_roomId && rootEventId == m_rootEventId
        && m_state != Failed)
        return; // already open/opening — reopening would only reset scroll.

    m_roomId = roomId;
    m_rootEventId = rootEventId;
    m_failureCategory.clear();
    // Bind the model to the new composite id BEFORE dispatching so the
    // arriving snapshot reset is applied, never raced.
    m_model.setRoomId(timelineId());
    setState(Opening);
    m_client->openThread(roomId, rootEventId);
}

void ThreadController::close()
{
    const bool wasActive = m_state != Closed;
    if (m_client && wasActive)
        m_client->closeThread();
    m_roomId.clear();
    m_rootEventId.clear();
    m_failureCategory.clear();
    m_model.setRoomId(QString{});
    if (wasActive)
        setState(Closed);
}

void ThreadController::sendText(const QString &body)
{
    if (!m_client || m_state == Closed || m_roomId.isEmpty()
        || m_rootEventId.isEmpty())
        return;
    const QString trimmed = body.trimmed();
    if (trimmed.isEmpty())
        return;
    // Always the backend's SDK thread path — never sendTextMessage, so a
    // thread reply can never land as an ordinary room message.
    m_client->sendThreadReply(m_roomId, m_rootEventId, trimmed);
}

QStringList ThreadController::participants() const
{
    QStringList result;
    for (int row = 0; row < m_model.rowCount(); ++row) {
        const QModelIndex idx = m_model.index(row, 0);
        if (m_model.data(idx, TimelineModel::IsVirtualRole).toBool())
            continue;
        const QString sender =
            m_model.data(idx, TimelineModel::SenderRole).toString();
        if (!sender.isEmpty() && !result.contains(sender))
            result.append(sender);
    }
    return result;
}

void ThreadController::handleCurrentRoomChanged(const QString &currentRoomId)
{
    if (m_state == Closed)
        return;
    if (currentRoomId != m_roomId)
        close();
}

void ThreadController::setState(State state, const QString &failureCategory)
{
    if (m_state == state && m_failureCategory == failureCategory)
        return;
    m_state = state;
    m_failureCategory = failureCategory;
    Q_EMIT stateChanged();
}
