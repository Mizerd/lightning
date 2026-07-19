#pragma once

#include "matrix/RoomInfo.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVariantMap>
#include <QTimer>

class MatrixClient;
class SpaceManager;

class RoomListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(quint64 filterGeneration READ filterGeneration NOTIFY filterGenerationChanged)
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
        MemberCountRole, // display/diagnostics only; never used for DM classification
        CategoryRole,    // "invite" | "dm" | "room"; DM is authoritative m.direct
        HighlightCountRole,
        MarkedUnreadRole,
        HasUnreadRole,
        MembershipRole,
        IsDirectRole,
        DirectUserIdRole,
        InviterRole,
        InvitePendingRole,
        InviteErrorRole,
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
    Q_INVOKABLE void acceptInvite(const QString &roomId);
    Q_INVOKABLE void rejectInvite(const QString &roomId);
    Q_INVOKABLE void markRoomRead(const QString &roomId);
    Q_INVOKABLE void markRoomUnread(const QString &roomId);
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    quint64 filterGeneration() const { return m_filterGeneration; }

    // v0.7 account switching: drop DM profile lookups resolved under the
    // previous account's authority, then rebuild from the client.
    void clearProfileCaches();

    void resetRooms(const QList<RoomInfo> &rooms);
    bool appendRooms(const QList<RoomInfo> &rooms);
    bool prependRooms(const QList<RoomInfo> &rooms);
    bool insertRoom(int index, const RoomInfo &room);
    bool replaceRoom(int index, const RoomInfo &room);
    bool removeRoom(int index);
    bool removeRange(int index, int count);
    bool truncate(int length);
    void clearRooms();

private Q_SLOTS:
    void refresh();
    void refreshRoom(const QString &roomId);
    void onUserProfileFinished(quint64 opId, bool ok, const QString &userId,
                               const QString &displayName,
                               const QString &avatarUrl,
                               const QString &category);

private:
    QString effectiveAvatarUrl(const RoomInfo &room) const;
    bool passesFilter(const RoomInfo &r) const;
    QList<RoomInfo> desiredRooms() const;
    void reconcileRooms();
    void resolveMissingDirectAvatars();

    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    QList<RoomInfo> m_rooms; // Filtered subset actually shown.
    QString m_searchQuery;
    QString m_pendingSearchQuery;
    quint64 m_filterGeneration = 1;
    QTimer m_searchDebounce;
    QHash<QString, QString> m_profileAvatars;
    QHash<quint64, QString> m_profileOps;
    QSet<QString> m_profilePending;

Q_SIGNALS:
    void searchQueryChanged();
    void filterGenerationChanged();
};
