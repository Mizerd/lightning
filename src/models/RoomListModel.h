#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QVariantMap>

class MatrixClient;
class SpaceManager;

class RoomListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        RoomIdRole = Qt::UserRole + 1,
        NameRole,
        TopicRole,
        AvatarUrlRole,
        LastMessagePreviewRole,
        LastActivityRole,
        UnreadCountRole,
        EncryptedRole,
        IsSpaceRole,
    };

    explicit RoomListModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    // Optional Space filter. When bound, only rooms belonging to
    // SpaceManager::activeSpaceId() are shown. Space rooms themselves are
    // always filtered out (they render in the Space chip row, not the
    // room list).
    void setSpaceManager(SpaceManager *spaces);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Convenience for QML: look up a room by id and get a small map with the
    // fields the UI actually needs (name, topic, encrypted). Returns an
    // empty map if the room is not present.
    Q_INVOKABLE QVariantMap findRoom(const QString &roomId) const;

private Q_SLOTS:
    void refresh();
    void refreshRoom(const QString &roomId);

private:
    bool passesFilter(const RoomInfo &r) const;

    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    QList<RoomInfo> m_rooms; // Filtered subset actually shown.
};
