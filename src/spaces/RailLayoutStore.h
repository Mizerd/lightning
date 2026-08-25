#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class SettingsManager;

// How the Spaces rail is arranged: the order the user dragged their Spaces
// into, and the folders they grouped them in.
//
// This is DEVICE-LOCAL and deliberately so. Matrix has no standard for
// ordering or grouping Spaces — no state event, no account-data key that any
// other client reads — so anything stored on the server would be a private
// invention only Lightning could see, which is exactly what a client should
// not put in someone's account. Ordering is a view preference, like a window
// width; it stays here, and every other client keeps showing its own order.
//
// Everything is REFERENTIAL: a folder holds space ids, never a copy of a
// Space. A Space that has been left simply stops appearing, and an id in the
// stored layout that no longer resolves is ignored rather than cleaned up
// eagerly — the account may just not have synced yet.
class RailLayoutStore : public QObject
{
    Q_OBJECT

    // [{ id, name, collapsed, spaceIds }] in rail order.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY layoutChanged)
    // The Spaces whose subspace hierarchy is expanded in the rail. Persisted,
    // as Element persists its own Space-panel expansion: an expansion is a
    // statement about how you want to navigate, not a transient glance, and
    // losing it on every restart is what made the rail feel flat.
    Q_PROPERTY(QStringList expandedSpaceIds READ expandedSpaceIds
                   NOTIFY layoutChanged)
    // Explicit top-level order. Ids not listed sort after it, in the order the
    // model supplies them, so a newly joined Space appears at the bottom
    // rather than jumping into the middle of a hand-made arrangement.
    Q_PROPERTY(QStringList order READ order NOTIFY layoutChanged)

public:
    explicit RailLayoutStore(SettingsManager *settings,
                             QObject *parent = nullptr);

    QVariantList folders() const;
    QStringList order() const;
    QStringList expandedSpaceIds() const;
    Q_INVOKABLE bool spaceExpanded(const QString &spaceId) const;
    Q_INVOKABLE void setSpaceExpanded(const QString &spaceId, bool expanded);
    Q_INVOKABLE void toggleSpaceExpanded(const QString &spaceId);
    // The members of one folder, in its own order. Empty for an unknown id —
    // which is not the same answer as "an empty folder", so callers that need
    // to tell them apart ask `folders()`.
    Q_INVOKABLE QStringList folderMembers(const QString &folderId) const;

    // Creates a folder and returns its id, or "" when the limit is reached.
    Q_INVOKABLE QString createFolder(const QString &name);
    // The drag gesture's folder creation: one Space dropped onto another.
    // Atomic on purpose — a create, two files and a reposition as four
    // separate writes is four saves, four layoutChanged signals and four
    // chances for the rail to re-arrange under the pointer mid-gesture.
    // The folder takes `atIndex` in the top-level order (clamped; -1 appends)
    // and `spaceIds` become its members in the given order. Returns the new
    // folder id, or "" when the folder limit is reached or nothing valid was
    // supplied.
    Q_INVOKABLE QString createFolderWithSpaces(const QStringList &spaceIds,
                                               int atIndex,
                                               const QString &name);
    // Files `spaceId` into `folderId` AT a position among its members
    // (clamped; -1 appends). setSpaceFolder always appends, which is right
    // for the context menu and wrong for a drag that landed between two
    // members.
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
    // Reorders one entry within the TOP LEVEL. `entryId` is either a space id
    // or a folder id — the rail interleaves both, so dragging has to move
    // either. `toIndex` is clamped.
    Q_INVOKABLE void moveEntry(const QString &entryId, int toIndex);
    // Replaces the top-level order outright with the ids the rail is showing.
    // The rail already knows the arrangement it is displaying, and pushing
    // the whole list is what makes a drop land exactly where it was dropped:
    // positioning against `order` alone is guesswork while entries that have
    // never been dragged are still implicit. Pseudo ids and duplicates are
    // dropped; ids belonging to a folder are ignored.
    Q_INVOKABLE void setTopLevelOrder(const QStringList &entryIds);

    // ONE atomic write of the whole arrangement, which is what a finished
    // drag actually produces: the rail knows every top-level entry it is
    // showing and every member of every OPEN folder, so committing that
    // picture in one call is both simpler and safer than a sequence of
    // unfile / file / reorder writes whose intermediate states are each
    // published to the rail.
    //
    // `topLevel` is the ordered list of top-level entry ids (space ids and
    // folder ids). `folderMembers` maps folder id -> ordered member space
    // ids, and must name ONLY folders whose members the caller actually
    // rendered: a folder left out keeps its members (minus anything the call
    // placed elsewhere), so a COLLAPSED folder cannot be emptied by a drag
    // that never showed its contents.
    //
    // Pseudo ids, unknown folder ids and duplicates are dropped. A space
    // named as a folder member is removed from the top level even if
    // `topLevel` also lists it, because a Space is in at most one place.
    Q_INVOKABLE void applyArrangement(const QStringList &topLevel,
                                      const QVariantMap &folderMembers);

    // Every Space id in rail order — the user's order, with each folder's
    // members inline where the folder sits, whether or not it is collapsed.
    //
    // This is the Channels layout's Space order. `arrange()` cannot answer
    // it: that is a PRESENTATION list and deliberately hides a collapsed
    // folder's members, which for an ordering question would silently drop
    // Spaces.
    Q_INVOKABLE QStringList orderedSpaceIds(const QVariantList &spaces) const;

    // Presentation. Takes the model's Spaces (each a map carrying at least
    // `spaceId`) and returns the rail's rows: pseudo rows first, exactly as
    // given, then folders and Spaces in the user's order, with a folder's
    // members following it when it is open.
    //
    // Pure, so the arrangement is testable without a rail, a model or a
    // homeserver.
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

        // Needed so a mutation can compare the whole layout it produced
        // against the one it loaded and decline to write when nothing moved.
        // A no-op save is a QSettings write plus a layoutChanged the rail
        // rebuilds itself for.
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
    };

    const Layout &load() const;
    void save(const Layout &layout);
    static QString makeFolderId(const Layout &layout);

    SettingsManager *m_settings = nullptr;
    mutable Layout m_cache;
    mutable bool m_loaded = false;
};
