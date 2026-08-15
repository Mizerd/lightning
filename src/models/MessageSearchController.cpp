#include "models/MessageSearchController.h"

#include "matrix/MatrixClient.h"

namespace {
constexpr int kDebounceMs = 400;
constexpr int kPageSize = 25;
} // namespace

MessageSearchController::MessageSearchController(QObject *parent)
    : QAbstractListModel(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this,
            [this] { dispatch(/*nextPage=*/false); });
}

void MessageSearchController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client) {
        Q_EMIT stateChanged();
        return;
    }
    connect(m_client, &MatrixClient::messageSearchFinished, this,
            &MessageSearchController::onSearchFinished);
    // One account's search results must never surface under another's.
    connect(m_client, &MatrixClient::loggedOut, this,
            &MessageSearchController::clear);
    Q_EMIT stateChanged();
}

bool MessageSearchController::supported() const
{
    return m_client && m_client->supportsMessageSearch();
}

void MessageSearchController::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    invalidatePending();
    if (m_query.trimmed().isEmpty()) {
        m_debounce.stop();
        clear();
        return;
    }
    m_debounce.start();
}

void MessageSearchController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    Q_EMIT roomIdChanged();
    // A different scope answers a different question: drop everything the
    // old scope produced rather than letting it repaint under a new label.
    clear();
}

int MessageSearchController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MessageSearchController::data(const QModelIndex &index,
                                       int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case RoomIdRole: return row.value(QStringLiteral("roomId"));
    case RoomNameRole: return row.value(QStringLiteral("roomName"));
    case EventIdRole: return row.value(QStringLiteral("eventId"));
    case SenderRole: return row.value(QStringLiteral("sender"));
    case SenderDisplayNameRole:
        return row.value(QStringLiteral("senderDisplayName"));
    case SenderAvatarUrlRole:
        return row.value(QStringLiteral("senderAvatarUrl"));
    case TimestampMsRole: return row.value(QStringLiteral("timestampMs"));
    case MsgtypeRole: return row.value(QStringLiteral("msgtype"));
    case BodyRole: return row.value(QStringLiteral("body"));
    }
    return {};
}

QHash<int, QByteArray> MessageSearchController::roleNames() const
{
    return {
        { RoomIdRole, QByteArrayLiteral("roomId") },
        { RoomNameRole, QByteArrayLiteral("roomName") },
        { EventIdRole, QByteArrayLiteral("eventId") },
        { SenderRole, QByteArrayLiteral("sender") },
        { SenderDisplayNameRole, QByteArrayLiteral("senderDisplayName") },
        { SenderAvatarUrlRole, QByteArrayLiteral("senderAvatarUrl") },
        { TimestampMsRole, QByteArrayLiteral("timestampMs") },
        { MsgtypeRole, QByteArrayLiteral("msgtype") },
        { BodyRole, QByteArrayLiteral("body") },
    };
}

void MessageSearchController::search()
{
    m_debounce.stop();
    if (m_query.trimmed().isEmpty())
        return;
    dispatch(/*nextPage=*/false);
}

void MessageSearchController::loadMore()
{
    if (m_pendingOp != 0 || m_nextBatch.isEmpty())
        return;
    dispatch(/*nextPage=*/true);
}

void MessageSearchController::clear()
{
    m_debounce.stop();
    invalidatePending();
    beginResetModel();
    m_rows.clear();
    endResetModel();
    m_nextBatch.clear();
    m_totalCount = 0;
    setState(QStringLiteral("idle"));
}

QVariantMap MessageSearchController::rowAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

void MessageSearchController::dispatch(bool nextPage)
{
    if (!supported() || m_query.trimmed().isEmpty())
        return;
    const QString since = nextPage ? m_nextBatch : QString();
    const quint64 opId = m_client->searchMessages(m_query.trimmed(), m_roomId,
                                                  since, kPageSize);
    if (opId == 0) {
        setState(QStringLiteral("error"));
        return;
    }
    m_pendingOp = opId;
    m_pendingIsNextPage = nextPage;
    setState(nextPage ? QStringLiteral("loading_more")
                      : QStringLiteral("loading"));
}

void MessageSearchController::onSearchFinished(quint64 opId, bool ok,
                                               const QVariantList &results,
                                               const QString &nextBatch,
                                               quint64 count,
                                               const QString &category)
{
    Q_UNUSED(category);
    if (opId == 0 || opId != m_pendingOp)
        return; // superseded — stale pages never repaint a newer query
    m_pendingOp = 0;
    if (!ok) {
        if (!m_pendingIsNextPage) {
            beginResetModel();
            m_rows.clear();
            endResetModel();
            m_nextBatch.clear();
        }
        setState(QStringLiteral("error"));
        return;
    }
    // Room display names resolve locally — the account is in every room
    // the server searched for it.
    QList<QVariantMap> rows;
    rows.reserve(results.size());
    for (const QVariant &value : results) {
        QVariantMap row = value.toMap();
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        QString roomName;
        if (m_client)
            roomName = m_client->roomInfo(roomId).name;
        row.insert(QStringLiteral("roomName"),
                   roomName.isEmpty() ? roomId : roomName);
        rows.append(row);
    }
    if (!m_pendingIsNextPage) {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    } else if (!rows.isEmpty()) {
        beginInsertRows({}, m_rows.size(), m_rows.size() + rows.size() - 1);
        m_rows.append(rows);
        endInsertRows();
    }
    m_nextBatch = nextBatch;
    m_totalCount = count;
    setState(m_rows.isEmpty() ? QStringLiteral("no_results")
                              : QStringLiteral("results"));
}

void MessageSearchController::setState(const QString &state)
{
    if (m_state == state) {
        Q_EMIT stateChanged(); // count/canLoadMore/totalCount ride this
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void MessageSearchController::invalidatePending()
{
    m_pendingOp = 0;
    m_pendingIsNextPage = false;
    m_nextBatch.clear();
}
