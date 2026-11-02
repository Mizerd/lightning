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

    Q_PROPERTY(QVariantList folders READ folders NOTIFY layoutChanged)
    Q_PROPERTY(QStringList expandedSpaceIds READ expandedSpaceIds
                   NOTIFY layoutChanged)
    Q_PROPERTY(QStringList order READ order NOTIFY layoutChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode
                   NOTIFY layoutChanged)

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
    int sortMode() const { return m_sortMode; }
    void setSortMode(int mode);
    QStringList sortedForMode(const QStringList &ids,
                              const QHash<QString, QVariantMap> &byId) const;
    QStringList expandedSpaceIds() const;
    Q_INVOKABLE bool spaceExpanded(const QString &spaceId) const;
    Q_INVOKABLE void setSpaceExpanded(const QString &spaceId, bool expanded);
    Q_INVOKABLE void toggleSpaceExpanded(const QString &spaceId);
    Q_INVOKABLE QStringList folderMembers(const QString &folderId) const;

    Q_INVOKABLE QString createFolder(const QString &name);
    Q_INVOKABLE QString createFolderWithSpaces(const QStringList &spaceIds,
                                               int atIndex,
                                               const QString &name);
    Q_INVOKABLE void moveSpaceToFolder(const QString &spaceId,
                                       const QString &folderId, int index);
    Q_INVOKABLE void renameFolder(const QString &folderId, const QString &name);
    Q_INVOKABLE void deleteFolder(const QString &folderId);
    Q_INVOKABLE void setFolderCollapsed(const QString &folderId, bool collapsed);

    Q_INVOKABLE void setSpaceFolder(const QString &spaceId,
                                    const QString &folderId);
    Q_INVOKABLE void moveEntry(const QString &entryId, int toIndex);
    Q_INVOKABLE void setTopLevelOrder(const QStringList &entryIds);

    Q_INVOKABLE QStringList orderedChildren(const QString &parentId,
                                            const QStringList &known) const;
    Q_INVOKABLE void setChildOrder(const QString &parentId,
                                   const QStringList &childIds);

    Q_INVOKABLE QStringList orderedRooms(const QString &spaceId,
                                         const QStringList &known) const;
    Q_INVOKABLE void setRoomOrder(const QString &spaceId,
                                  const QStringList &roomIds);

    Q_INVOKABLE void applyArrangement(const QStringList &topLevel,
                                      const QVariantMap &folderMembers);

    Q_INVOKABLE QStringList orderedSpaceIds(const QVariantList &spaces) const;

    Q_INVOKABLE QVariantList arrange(const QVariantList &spaces) const;

    Q_INVOKABLE QString folderOf(const QString &spaceId) const;

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
        QStringList order;
        QStringList expanded;
        /// Parent space id -> local order of its subspaces. Local like
        /// `order`: writing m.space.child `order` would need power in
        /// someone else's Space and reorder it for every member.
        QHash<QString, QStringList> childOrder;
        QHash<QString, QStringList> roomOrder;
    };

    QHash<QString, QVariantMap> sortKeys(
        const Layout &layout, const QHash<QString, QVariantMap> &byId) const;
    const Layout &load() const;
    void save(const Layout &layout);
    Layout dropEmptiedFolders(const Layout &layout) const;
    static QString makeFolderId(const Layout &layout);

    void invalidate();

    SettingsManager *m_settings = nullptr;
    MatrixClient *m_client = nullptr;
    mutable Layout m_cache;
    mutable bool m_loaded = false;
    int m_sortMode = 0;
};
