#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class SettingsManager;

// How the Spaces rail is arranged: the order the user dragged their Spaces
// into, and the folders they grouped them in.
//
// Device-local by design: Matrix has no standard for ordering or grouping
// Spaces, so storing it on the server would be a private invention in the
// user's account. Other clients keep their own order.
//
// Everything is referential: a folder holds space ids, not copies. An id
// that no longer resolves is ignored rather than cleaned up eagerly, since
// the account may not have synced yet.
class RailLayoutStore : public QObject
{
    Q_OBJECT

    // [{ id, name, collapsed, spaceIds }] in rail order.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY layoutChanged)
    // Spaces whose subspace hierarchy is expanded. Persisted, like Element's
    // Space-panel expansion.
    Q_PROPERTY(QStringList expandedSpaceIds READ expandedSpaceIds
                   NOTIFY layoutChanged)
    // Explicit top-level order. Unlisted ids follow in model order, so a
    // newly joined Space appears at the bottom.
    Q_PROPERTY(QStringList order READ order NOTIFY layoutChanged)

public:
    explicit RailLayoutStore(SettingsManager *settings,
                             QObject *parent = nullptr);

    // The arrangement is account-scoped, so sign-out and account switch must
    // drop the in-memory cache. Optional: the constructor already listens to
    // SettingsManager::sessionChanged; this makes the drop happen as soon as
    // the session ends. Invalidation is idempotent.
    void setClient(MatrixClient *client);

    QVariantList folders() const;
    QStringList order() const;
    QStringList expandedSpaceIds() const;
    Q_INVOKABLE bool spaceExpanded(const QString &spaceId) const;
    Q_INVOKABLE void setSpaceExpanded(const QString &spaceId, bool expanded);
    Q_INVOKABLE void toggleSpaceExpanded(const QString &spaceId);
    // The members of one folder, in order. Empty for an unknown id, which
    // folders() can distinguish from an empty folder.
    Q_INVOKABLE QStringList folderMembers(const QString &folderId) const;

    // Creates a folder and returns its id, or "" when the limit is reached.
    Q_INVOKABLE QString createFolder(const QString &name);
    // One Space dropped onto another: creates the folder at `atIndex` in the
    // top-level order (clamped; -1 appends) with `spaceIds` as members, in a
    // single write so the rail does not rearrange mid-gesture. Returns the
    // new id, or "" when the limit is reached or nothing valid was supplied.
    Q_INVOKABLE QString createFolderWithSpaces(const QStringList &spaceIds,
                                               int atIndex,
                                               const QString &name);
    // Files `spaceId` into `folderId` at a member position (clamped; -1
    // appends), for a drag that lands between members.
    Q_INVOKABLE void moveSpaceToFolder(const QString &spaceId,
                                       const QString &folderId, int index);
    Q_INVOKABLE void renameFolder(const QString &folderId, const QString &name);
    // The folder goes; its Spaces return to the top level, in place.
    Q_INVOKABLE void deleteFolder(const QString &folderId);
    Q_INVOKABLE void setFolderCollapsed(const QString &folderId, bool collapsed);

    // Moves a Space into a folder, or back to the top level with an empty
    // folder id. A Space is in at most one folder.
    Q_INVOKABLE void setSpaceFolder(const QString &spaceId,
                                    const QString &folderId);
    // Reorders one top-level entry (space id or folder id). `toIndex` is
    // clamped.
    Q_INVOKABLE void moveEntry(const QString &entryId, int toIndex);
    // Replaces the top-level order with the ids the rail is showing, so a
    // drop lands exactly where it was made. Pseudo ids and duplicates are
    // dropped; folder members are ignored.
    Q_INVOKABLE void setTopLevelOrder(const QStringList &entryIds);

    /// `known` in the user's order: the stored arrangement first, then the
    /// rest in Matrix's order, so a new Space goes to the end of its run.
    Q_INVOKABLE QStringList orderedChildren(const QString &parentId,
                                            const QStringList &known) const;
    /// Records the order of `parentId`'s subspaces. Ids no longer in `known`
    /// are simply not returned, so a departed child is ignored rather than
    /// cleaned up eagerly: an unfinished hierarchy load is not a change.
    Q_INVOKABLE void setChildOrder(const QString &parentId,
                                   const QStringList &childIds);

    /// The same policy for a Space's revealed rooms. Unarranged rooms keep
    /// their arrival order (most recently active first).
    Q_INVOKABLE QStringList orderedRooms(const QString &spaceId,
                                         const QStringList &known) const;
    Q_INVOKABLE void setRoomOrder(const QString &spaceId,
                                  const QStringList &roomIds);

    // One atomic write of a finished drag's whole arrangement, instead of a
    // sequence of writes whose intermediate states each reach the rail.
    //
    // `topLevel` is the ordered top-level ids (spaces and folders).
    // `folderMembers` maps folder id -> ordered member ids and must name only
    // folders the caller rendered: an omitted folder keeps its members (minus
    // any placed elsewhere), so a collapsed folder cannot be emptied.
    //
    // Pseudo ids, unknown folder ids and duplicates are dropped. A Space
    // named as a folder member is removed from the top level.
    Q_INVOKABLE void applyArrangement(const QStringList &topLevel,
                                      const QVariantMap &folderMembers);

    // Every Space id in rail order, with each folder's members inline whether
    // or not it is collapsed. The Channels layout's Space order; arrange()
    // hides collapsed members and cannot answer this.
    Q_INVOKABLE QStringList orderedSpaceIds(const QVariantList &spaces) const;

    // Presentation: takes the model's Spaces (each carrying `spaceId`) and
    // returns the rail's rows: pseudo rows first as given, then folders and
    // Spaces in the user's order, with open folders' members after them.
    // Pure, so it is testable without a rail or homeserver.
    Q_INVOKABLE QVariantList arrange(const QVariantList &spaces) const;

    Q_INVOKABLE QString folderOf(const QString &spaceId) const;

    // Bounded: a rail is a strip down the side of a window.
    static constexpr int kMaxFolders = 32;
    static constexpr int kMaxNameLength = 40;

Q_SIGNALS:
    void layoutChanged();

private:
    struct Folder {
        QString id;
        QString name;
        bool collapsed = false;
        QStringList spaceIds;

        // Lets a mutation skip the write (and layoutChanged) when nothing
        // changed.
        bool operator==(const Folder &other) const
        {
            return id == other.id && name == other.name
                   && collapsed == other.collapsed
                   && spaceIds == other.spaceIds;
        }
        bool operator!=(const Folder &other) const { return !(*this == other); }
    };

    struct Layout {
        QList<Folder> folders;
        QStringList order;   // top-level entry ids: space ids and folder ids
        QStringList expanded;   // space ids whose subspaces are revealed
        /// Parent space id -> local order of its subspaces. Local like
        /// `order`: writing m.space.child `order` would need power in
        /// someone else's Space and reorder it for every member.
        QHash<QString, QStringList> childOrder;
        /// Space id -> order of its revealed rooms. Separate from
        /// `childOrder`: subspaces are rail rows, rooms are drawn inside the
        /// owning row.
        QHash<QString, QStringList> roomOrder;
    };

    const Layout &load() const;
    void save(const Layout &layout);
    /// Removes every folder this write emptied (empty now, non-empty when
    /// loaded). Never removes a folder created empty (see the .cpp).
    Layout dropEmptiedFolders(const Layout &layout) const;
    static QString makeFolderId(const Layout &layout);

    /// Drops the cached arrangement so the next read re-resolves it against
    /// whichever account is active now.
    void invalidate();

    SettingsManager *m_settings = nullptr;
    MatrixClient *m_client = nullptr;
    mutable Layout m_cache;
    mutable bool m_loaded = false;
};
