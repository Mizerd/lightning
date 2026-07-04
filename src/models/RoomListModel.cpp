#include "models/RoomListModel.h"

#include "matrix/MatrixClient.h"

RoomListModel::RoomListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void RoomListModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomsChanged,
                this, &RoomListModel::refresh);
        connect(m_client, &MatrixClient::roomUpdated,
                this, &RoomListModel::refreshRoom);
        connect(m_client, &MatrixClient::loggedOut,
                this, &RoomListModel::refresh);
    }
    refresh();
}

int RoomListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rooms.size());
}

QVariant RoomListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rooms.size())
        return {};
    const auto &r = m_rooms.at(index.row());
    switch (role) {
    case RoomIdRole:             return r.id;
    case NameRole:               return r.name;
    case TopicRole:              return r.topic;
    case AvatarUrlRole:          return r.avatarUrl;
    case LastMessagePreviewRole: return r.lastMessagePreview;
    case LastActivityRole:       return r.lastActivity;
    case UnreadCountRole:        return r.unreadCount;
    case EncryptedRole:          return r.encrypted;
    default:                     return {};
    }
}

QHash<int, QByteArray> RoomListModel::roleNames() const
{
    return {
        { RoomIdRole,             "roomId" },
        { NameRole,               "name" },
        { TopicRole,              "topic" },
        { AvatarUrlRole,          "avatarUrl" },
        { LastMessagePreviewRole, "lastMessagePreview" },
        { LastActivityRole,       "lastActivity" },
        { UnreadCountRole,        "unreadCount" },
        { EncryptedRole,          "encrypted" },
    };
}

QVariantMap RoomListModel::findRoom(const QString &roomId) const
{
    for (const auto &r : m_rooms) {
        if (r.id == roomId) {
            return {
                { QStringLiteral("id"),        r.id },
                { QStringLiteral("name"),      r.name },
                { QStringLiteral("topic"),     r.topic },
                { QStringLiteral("avatarUrl"), r.avatarUrl },
                { QStringLiteral("encrypted"), r.encrypted },
                { QStringLiteral("unreadCount"), r.unreadCount },
            };
        }
    }
    return {};
}

void RoomListModel::refresh()
{
    beginResetModel();
    m_rooms = m_client ? m_client->rooms() : QList<RoomInfo>{};
    endResetModel();
}

void RoomListModel::refreshRoom(const QString &roomId)
{
    if (!m_client)
        return;
    const auto latest = m_client->rooms();
    for (int i = 0; i < m_rooms.size(); ++i) {
        if (m_rooms[i].id == roomId) {
            for (const auto &r : latest) {
                if (r.id == roomId) {
                    m_rooms[i] = r;
                    const auto idx = index(i);
                    Q_EMIT dataChanged(idx, idx);
                    return;
                }
            }
        }
    }
    // Not previously in the list — fall back to a full refresh.
    refresh();
}
