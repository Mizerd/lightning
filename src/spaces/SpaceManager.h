#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.4.1: real space model. Rebuilt from the active MatrixClient every time
// `roomsChanged` fires. Exposes:
//   - a list model of Space rooms for QML (name, id, unread total, avatar);
//   - `activeSpaceId` — selection state driving the RoomListModel filter;
//   - `roomsInSpace(spaceId)` — hierarchy lookup;
//   - `includesRoom(spaceId, roomId)` — filter helper for RoomListModel.
//
// The pseudo-space id "" (empty string) means "All rooms" and matches every
// room. The pseudo-space id "@orphans" matches every room that is not a
// child of any Space.
//
// Space *editing* (creating a Space, adding a room to a Space, power-level
// checks) is out of scope for v0.4.1 and documented as v0.5 work.
class SpaceManager : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString activeSpaceId READ activeSpaceId WRITE setActiveSpaceId NOTIFY activeSpaceIdChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY spacesChanged)
    Q_PROPERTY(bool hasSpaces READ hasSpaces NOTIFY spacesChanged)

public:
    enum Roles {
        SpaceIdRole = Qt::UserRole + 1,
        NameRole,
        TopicRole,
        AvatarUrlRole,
        ChildCountRole,
        UnreadTotalRole,
        HighlightTotalRole,
        LevelRole,
    };

    // Well-known pseudo-space ids surfaced through the model as extra rows
    // so QML can render them consistently with real Spaces.
    static QString allRoomsId()   { return QStringLiteral(""); }
    static QString orphansId()    { return QStringLiteral("@orphans"); }

    explicit SpaceManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString activeSpaceId() const { return m_activeSpaceId; }
    void    setActiveSpaceId(const QString &spaceId);
    bool    hasSpaces() const { return !m_spaces.isEmpty(); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Hierarchy queries. Return the set of room ids that belong to
    // `spaceId`. `allRoomsId()` returns every room; `orphansId()` returns
    // rooms not referenced by any Space.
    Q_INVOKABLE QStringList roomsInSpace(const QString &spaceId) const;
    Q_INVOKABLE bool includesRoom(const QString &spaceId, const QString &roomId) const;
    // v0.7 design shell: display name for the room-list workspace header.
    Q_INVOKABLE QString spaceName(const QString &spaceId) const;

    // v0.7 Space Home. Presentation snapshot of one Space (name, topic,
    // avatar, child/unread totals) and its children in authoritative
    // m.space.child order with unread/mention state. `addableRooms` lists
    // joined, non-Space rooms that are NOT yet children (name-filtered) for
    // the "Add existing room" picker.
    Q_INVOKABLE QVariantMap spaceInfo(const QString &spaceId) const;
    Q_INVOKABLE QVariantList childRoomsDetailed(const QString &spaceId) const;
    Q_INVOKABLE QVariantList addableRooms(const QString &spaceId,
                                          const QString &filter) const;
    // Sends the real m.space.child state event through the backend. The
    // authoritative hierarchy update arrives via sync (roomsChanged →
    // rebuild); childAddFinished only reports the send outcome.
    Q_INVOKABLE void addRoomToSpace(const QString &spaceId,
                                    const QString &roomId);
    // MSC1772 removal (empty-via m.space.child); the room itself is never
    // left or deleted. Permission failures surface as ok=false.
    Q_INVOKABLE void removeRoomFromSpace(const QString &spaceId,
                                         const QString &roomId);

Q_SIGNALS:
    void activeSpaceIdChanged();
    void spacesChanged();
    // v0.7: outcome of one addRoomToSpace call (send result, not sync).
    void childAddFinished(const QString &spaceId, const QString &roomId,
                          bool ok);
    void childRemoveFinished(const QString &spaceId, const QString &roomId,
                             bool ok);

private Q_SLOTS:
    void rebuild();

private:
    struct SpaceEntry {
        RoomInfo info;              // The Space room itself.
        QStringList childRoomIds;   // Ordered list of children.
        int unreadTotal = 0;        // Sum of children's unread counts.
        int highlightTotal = 0;
        int level = 0;
    };

    void recomputeOrphans();

    MatrixClient *m_client = nullptr;

    QList<SpaceEntry> m_spaces;             // Rows: [All rooms] [orphans?] [space1] [space2] ...
    QHash<QString, QSet<QString>> m_membership;    // spaceId → set(roomId)
    QSet<QString> m_allRoomIds;             // Every non-space room id.
    QSet<QString> m_orphanRoomIds;          // Rooms not in any Space.
    int m_homeUnreadTotal = 0;
    int m_homeHighlightTotal = 0;
    // v0.7: pending m.space.child sends, opId → (spaceId, roomId).
    QHash<quint64, QPair<QString, QString>> m_pendingChildAdds;
    QHash<quint64, QPair<QString, QString>> m_pendingChildRemovals;

    QString m_activeSpaceId; // Empty means "All rooms".
};
