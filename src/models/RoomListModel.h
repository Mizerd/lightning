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
    // Element-style list filter: 0 = All, 1 = People (DMs), 2 = Rooms,
    // 3 = Unreads. Invites always pass (they need action), and in Unreads
    // mode the pinned (currently open) room stays visible so reading a
    // room does not yank its row out from under the selection.
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode NOTIFY filterModeChanged)
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
        CanonicalAliasRole,
        // identityColorKey(RoomInfo): partner MXID for unambiguous 1:1
        // DMs, room id otherwise — the single fallback-colour policy.
        IdentityColorKeyRole,
        // v0.7.x room upgrades. The successor of a tombstoned room, empty
        // otherwise, and whether that successor is one the user can
        // ACTUALLY reach — the row is de-emphasized only on the latter.
        SuccessorRoomIdRole,
        SupersededByAccessibleSuccessorRole,
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
    // v0.7.1: the most recent joined conversations (Spaces excluded) for the
    // Home surface, as a bounded list of {roomId,name,avatarUrl,isDirect,
    // hasUnread,unreadCount} maps, reusing the model's activity ordering.
    Q_INVOKABLE QVariantList recentRooms(int max = 6) const;
    // v0.7 Home: joined Spaces (presentation fields only) for the Spaces
    // shortcut strip.
    Q_INVOKABLE QVariantList spacesSummary(int max = 8) const;
    Q_INVOKABLE void acceptInvite(const QString &roomId);
    Q_INVOKABLE void rejectInvite(const QString &roomId);
    Q_INVOKABLE void markRoomRead(const QString &roomId);
    Q_INVOKABLE void markRoomUnread(const QString &roomId);
    // v0.6.5 (SPEC 1d): pure formatting helper for "Copy room link" — prefers
    // the canonical alias over the bare room id, matching
    // TimelineModel::messagePermalink's existing percent-encoding convention
    // (! $ : @ excluded) so room and message links read consistently. No
    // server behavior; static so it is trivially unit-testable.
    Q_INVOKABLE static QString roomPermalink(const QString &roomId,
                                             const QString &canonicalAlias = QString());
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    quint64 filterGeneration() const { return m_filterGeneration; }
    int filterMode() const { return m_filterMode; }
    void setFilterMode(int mode);
    // The currently open room; kept visible in Unreads mode (see the
    // filterMode property comment). Set by AppController on room switch.
    void setPinnedRoomId(const QString &roomId);

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
    bool passesScopeFilter(const RoomInfo &r) const;
    bool passesFilter(const RoomInfo &r) const;
    QList<RoomInfo> desiredRooms(const QSet<QString> &superseded) const;
    void reconcileRooms();
    // Rooms that have been replaced by a successor the user can actually
    // reach. "Actually reach" is the maintainer's condition for
    // de-emphasizing the old room, and it means all three of: a successor
    // exists, we HOLD a record for it with membership Joined or Invited,
    // and that record's predecessor points BACK at this room. An
    // unverifiable chain leaves the row alone.
    QSet<QString> computeSupersededRoomIds() const;
    void resolveMissingDirectAvatars();

    MatrixClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    QList<RoomInfo> m_rooms; // Filtered subset actually shown.
    // Rooms whose successor the user can actually reach. Recomputed once
    // per reconcile from the client's FULL room set (the successor may be
    // filtered out of the visible list, or in another Space), because
    // deriving it per data() call would be quadratic in the room count.
    QSet<QString> m_supersededRoomIds;
    QString m_searchQuery;
    QString m_pendingSearchQuery;
    int m_filterMode = 0;
    QString m_pinnedRoomId;
    quint64 m_filterGeneration = 1;
    QTimer m_searchDebounce;
    // Coalesces per-event refreshRoom() calls into one reconcile per turn.
    QTimer m_reconcileCoalesce;
    QHash<QString, QString> m_profileAvatars;
    QHash<quint64, QString> m_profileOps;
    QSet<QString> m_profilePending;

Q_SIGNALS:
    void searchQueryChanged();
    void filterGenerationChanged();
    void filterModeChanged();
};
