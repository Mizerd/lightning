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
    // Explicit top-level order. Ids not listed sort after it, in the order the
    // model supplies them, so a newly joined Space appears at the bottom
    // rather than jumping into the middle of a hand-made arrangement.
    Q_PROPERTY(QStringList order READ order NOTIFY layoutChanged)

public:
    explicit RailLayoutStore(SettingsManager *settings,
                             QObject *parent = nullptr);

    QVariantList folders() const;
    QStringList order() const;

    // Creates a folder and returns its id, or "" when the limit is reached.
    Q_INVOKABLE QString createFolder(const QString &name);
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
    };

    struct Layout {
        QList<Folder> folders;
        QStringList order;   // top-level entry ids: space ids and folder ids
    };

    const Layout &load() const;
    void save(const Layout &layout);
    static QString makeFolderId(const Layout &layout);

    SettingsManager *m_settings = nullptr;
    mutable Layout m_cache;
    mutable bool m_loaded = false;
};
