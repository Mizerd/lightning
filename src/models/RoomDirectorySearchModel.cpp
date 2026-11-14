#include "models/RoomDirectorySearchModel.h"

#include "matrix/MatrixClient.h"

#include <QStringList>

namespace {
constexpr int kDebounceMs = 300;
constexpr int kPageSize = 30;

// A room listed by another directory is usually unknown to our homeserver,
// and a join by bare room id then has nowhere to go.
QVariantMap withVia(QVariantMap row, const QString &directoryServer)
{
    QStringList via;
    const auto add = [&via](const QString &server) {
        if (!server.isEmpty() && !via.contains(server))
            via.append(server);
    };
    // Newer room ids carry no server name, so the alias is tried too.
    for (const char *key : { "roomId", "alias" }) {
        const QString id = row.value(QLatin1String(key)).toString();
        const int colon = id.indexOf(QLatin1Char(':'));
        if (colon > 0)
            add(id.mid(colon + 1));
    }
    add(directoryServer);
    row.insert(QStringLiteral("via"), via);
    return row;
}
}

RoomDirectorySearchModel::RoomDirectorySearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this,
            [this] { dispatch(false); });
}

void RoomDirectorySearchModel::setClient(MatrixClient *client)
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
    connect(m_client, &MatrixClient::publicRoomsReceived, this,
            &RoomDirectorySearchModel::onPublicRooms);
    // One account's directory results must never survive into another's.
    connect(m_client, &MatrixClient::loggedOut, this,
            &RoomDirectorySearchModel::clear);
    Q_EMIT stateChanged();
}

bool RoomDirectorySearchModel::supported() const
{
    return m_client && m_client->supportsRoomDiscovery();
}

void RoomDirectorySearchModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    invalidatePending();
    m_debounce.start();
}

void RoomDirectorySearchModel::setServer(const QString &server)
{
    const QString trimmed = server.trimmed();
    if (m_server == trimmed)
        return;
    m_server = trimmed;
    Q_EMIT serverChanged();
    invalidatePending();
    m_debounce.start();
}

int RoomDirectorySearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant RoomDirectorySearchModel::data(const QModelIndex &index,
                                        int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case RoomIdRole: return row.value(QStringLiteral("roomId"));
    case NameRole: return row.value(QStringLiteral("name"));
    case AliasRole: return row.value(QStringLiteral("alias"));
    case TopicRole: return row.value(QStringLiteral("topic"));
    case AvatarUrlRole: return row.value(QStringLiteral("avatarUrl"));
    case MembersRole: return row.value(QStringLiteral("members"));
    case JoinRuleRole: return row.value(QStringLiteral("joinRule"));
    case MembershipRole: return row.value(QStringLiteral("membership"));
    case IsSpaceRole: return row.value(QStringLiteral("isSpace"));
    case ViaRole: return row.value(QStringLiteral("via"));
    }
    return {};
}

QHash<int, QByteArray> RoomDirectorySearchModel::roleNames() const
{
    return {
        { RoomIdRole, QByteArrayLiteral("roomId") },
        { NameRole, QByteArrayLiteral("name") },
        { AliasRole, QByteArrayLiteral("alias") },
        { TopicRole, QByteArrayLiteral("topic") },
        { AvatarUrlRole, QByteArrayLiteral("avatarUrl") },
        { MembersRole, QByteArrayLiteral("members") },
        { JoinRuleRole, QByteArrayLiteral("joinRule") },
        { MembershipRole, QByteArrayLiteral("membership") },
        { IsSpaceRole, QByteArrayLiteral("isSpace") },
        { ViaRole, QByteArrayLiteral("via") },
    };
}

void RoomDirectorySearchModel::refresh()
{
    m_debounce.stop();
    dispatch(false);
}

void RoomDirectorySearchModel::loadMore()
{
    if (m_pendingOp != 0 || m_nextBatch.isEmpty())
        return;
    dispatch(true);
}

void RoomDirectorySearchModel::clear()
{
    m_debounce.stop();
    invalidatePending();
    beginResetModel();
    m_rows.clear();
    endResetModel();
    m_nextBatch.clear();
    m_errorCategory.clear();
    setState(QStringLiteral("idle"));
}

QVariantMap RoomDirectorySearchModel::rowAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

void RoomDirectorySearchModel::dispatch(bool nextPage)
{
    if (!supported())
        return;
    const QString since = nextPage ? m_nextBatch : QString();
    const quint64 opId =
        m_client->searchPublicRooms(m_query.trimmed(), m_server, since,
                                    kPageSize);
    if (opId == 0) {
        m_errorCategory = m_server.isEmpty()
                              ? QStringLiteral("network")
                              : QStringLiteral("invalid_server");
        setState(QStringLiteral("error"));
        return;
    }
    m_pendingOp = opId;
    m_pendingServer = m_server;
    m_pendingIsNextPage = nextPage;
    m_errorCategory.clear();
    setState(nextPage ? QStringLiteral("loading_more")
                      : QStringLiteral("loading"));
}

void RoomDirectorySearchModel::onPublicRooms(quint64 opId, bool ok,
                                             const QVariantList &rooms,
                                             const QString &nextBatch,
                                             quint64 totalEstimate,
                                             const QString &category)
{
    Q_UNUSED(totalEstimate);
    if (opId == 0 || opId != m_pendingOp)
        return;
    m_pendingOp = 0;
    if (!ok) {
        m_errorCategory = category;
        if (!m_pendingIsNextPage) {
            beginResetModel();
            m_rows.clear();
            endResetModel();
            m_nextBatch.clear();
        }
        setState(QStringLiteral("error"));
        return;
    }
    if (!m_pendingIsNextPage) {
        beginResetModel();
        m_rows.clear();
        for (const QVariant &value : rooms)
            m_rows.append(withVia(value.toMap(), m_pendingServer));
        endResetModel();
    } else if (!rooms.isEmpty()) {
        beginInsertRows({}, m_rows.size(), m_rows.size() + rooms.size() - 1);
        for (const QVariant &value : rooms)
            m_rows.append(withVia(value.toMap(), m_pendingServer));
        endInsertRows();
    }
    m_nextBatch = nextBatch;
    setState(m_rows.isEmpty() ? QStringLiteral("no_results")
                              : QStringLiteral("results"));
}

void RoomDirectorySearchModel::setState(const QString &state)
{
    if (m_state == state) {
        Q_EMIT stateChanged();
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void RoomDirectorySearchModel::invalidatePending()
{
    m_pendingOp = 0;
    m_pendingIsNextPage = false;
    m_nextBatch.clear();
}
