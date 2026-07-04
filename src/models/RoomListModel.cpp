#include "models/RoomListModel.h"

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

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

void RoomListModel::setSpaceManager(SpaceManager *spaces)
{
    if (m_spaces == spaces)
        return;
    if (m_spaces)
        m_spaces->disconnect(this);
    m_spaces = spaces;
    if (m_spaces) {
        connect(m_spaces, &SpaceManager::activeSpaceIdChanged,
                this, &RoomListModel::refresh);
        connect(m_spaces, &SpaceManager::spacesChanged,
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
    case IsSpaceRole:            return r.isSpace;
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
        { IsSpaceRole,            "isSpace" },
    };
}

QVariantMap RoomListModel::findRoom(const QString &roomId) const
{
    // Search the client's full room set so lookups do not depend on the
    // active Space filter.
    if (!m_client)
        return {};
    for (const auto &r : m_client->rooms()) {
        if (r.id == roomId) {
            return {
                { QStringLiteral("id"),        r.id },
                { QStringLiteral("name"),      r.name },
                { QStringLiteral("topic"),     r.topic },
                { QStringLiteral("avatarUrl"), r.avatarUrl },
                { QStringLiteral("encrypted"), r.encrypted },
                { QStringLiteral("unreadCount"), r.unreadCount },
                { QStringLiteral("isSpace"),   r.isSpace },
            };
        }
    }
    return {};
}

bool RoomListModel::passesFilter(const RoomInfo &r) const
{
    // Space rooms themselves belong to the Space chip row, not the room list.
    if (r.isSpace) return false;
    if (!m_spaces) return true;
    const QString active = m_spaces->activeSpaceId();
    if (active.isEmpty()) return true; // "All rooms"
    return m_spaces->includesRoom(active, r.id);
}

void RoomListModel::refresh()
{
    beginResetModel();
    m_rooms.clear();
    if (m_client) {
        for (const auto &r : m_client->rooms()) {
            if (passesFilter(r))
                m_rooms.append(r);
        }
    }
    endResetModel();
}

void RoomListModel::refreshRoom(const QString &roomId)
{
    if (!m_client) return;
    const auto latest = m_client->rooms();
    // If a room's Space membership might have changed, a full refresh is
    // cheaper than trying to reason about position deltas.
    if (m_spaces) {
        refresh();
        return;
    }
    for (int i = 0; i < m_rooms.size(); ++i) {
        if (m_rooms[i].id == roomId) {
            for (const auto &r : latest) {
                if (r.id == roomId) {
                    if (!passesFilter(r)) {
                        // Room fell out of filter — full refresh.
                        refresh();
                        return;
                    }
                    m_rooms[i] = r;
                    const auto idx = index(i);
                    Q_EMIT dataChanged(idx, idx);
                    return;
                }
            }
        }
    }
    refresh();
}
