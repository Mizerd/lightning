// The Spaces rail's own arrangement: drag order, folders, and the drag model.
//
// The store half is pure over a model snapshot: a new Space does not barge
// into a hand-made order, a folder emptied by a write goes away with it
// (RailFolderLifecycleTest covers the rest), deleting a folder puts its Spaces
// back where it was, and pseudo rows cannot be dragged or filed.
//
// The gesture half lives in RailEntryModel: preview reorder, reorder versus
// group, and what a release writes are model operations. The view's half
// (pointer bands, auto-scroll) is covered by RailDragQmlTest, not here.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QVariantMap space(const QString &id, const QString &name, int unread = 0)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("spaceId"), id);
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("unreadTotal"), unread);
    entry.insert(QStringLiteral("highlightTotal"), 0);
    return entry;
}

// The model always leads with the two pseudo rows.
QVariantList withPseudo(const QVariantList &spaces)
{
    QVariantList out;
    out.append(space(QString(), QStringLiteral("All rooms")));
    out.append(space(QStringLiteral("@orphans"), QStringLiteral("Other rooms")));
    out.append(spaces);
    return out;
}

QStringList idsOf(const QVariantList &arranged)
{
    QStringList out;
    for (const QVariant &value : arranged) {
        const QVariantMap entry = value.toMap();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("folder"))
            out << QStringLiteral("[") + entry.value(QStringLiteral("name")).toString()
                    + QStringLiteral("]");
        else
            out << entry.value(QStringLiteral("spaceId")).toString();
    }
    return out;
}

// A client answering with whatever rooms a case hands it: RailEntryModel
// needs a real SpaceManager for subspace rows, and that needs a client.
class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<RoomInfo> roomList;
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
    void announce() { Q_EMIT roomsChanged(); }
};

RoomInfo spaceRoom(const QString &id, const QString &name,
                   const QStringList &children = {},
                   const QStringList &parents = {})
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = true;
    info.membership = RoomInfo::Joined;
    info.childRoomIds = children;
    info.parentSpaceIds = parents;
    return info;
}

QStringList modelIds(const RailEntryModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i) {
        out.append(model.data(model.index(i, 0),
                              RailEntryModel::EntryIdRole).toString());
    }
    return out;
}

} // namespace

class RailLayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("rail-layout-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void anUntouchedRailIsTheModelsOwnOrder()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
        }));
        QCOMPARE(idsOf(arranged),
                 (QStringList{ QString(), QStringLiteral("@orphans"),
                               QStringLiteral("!a:x"), QStringLiteral("!b:x") }));
    }

    void dragOrderIsKeptAndNewSpacesGoToTheEnd()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setTopLevelOrder({ QStringLiteral("!b:x"), QStringLiteral("!a:x") });

        QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("!b:x"), QStringLiteral("!a:x") }));

        // A Space joined later does not land in the middle of a hand-made
        // arrangement.
        arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("!b:x"), QStringLiteral("!a:x"),
                               QStringLiteral("!c:x") }));

        // A left Space stops appearing; its stored slot is kept, since the
        // account may just not have synced yet.
        arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
        }));
        QCOMPARE(idsOf(arranged).mid(2), (QStringList{ QStringLiteral("!a:x") }));
    }

    void aPseudoRowCanNeverBeOrderedOrFiled()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        QVERIFY(!folder.isEmpty());

        // "All rooms" and "Other rooms" are views of everything, not Spaces.
        store.setSpaceFolder(QString(), folder);
        store.setSpaceFolder(QStringLiteral("@orphans"), folder);
        store.setTopLevelOrder({ QStringLiteral("@orphans"), QString(),
                                 QStringLiteral("!a:x") });
        QCOMPARE(store.order(), QStringList{ QStringLiteral("!a:x") });

        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
        }));
        // Both keep their place at the top, ahead of everything arrangeable.
        QCOMPARE(idsOf(arranged).mid(0, 2),
                 (QStringList{ QString(), QStringLiteral("@orphans") }));
    }

    void aFolderCarriesItsSpacesAndTheirUnreadTotals()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);

        QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A"), 3),
            space(QStringLiteral("!b:x"), QStringLiteral("B"), 4),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
        }));
        // Open: the folder, then its members, then everything else.
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("[Work]"), QStringLiteral("!a:x"),
                               QStringLiteral("!b:x"), QStringLiteral("!c:x") }));
        const QVariantMap folderRow = arranged.at(2).toMap();
        QCOMPARE(folderRow.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("folder"));
        QCOMPARE(folderRow.value(QStringLiteral("childCount")).toInt(), 2);
        // A collapsed folder carries what it hides, so it cannot hide unread
        // messages.
        QCOMPARE(folderRow.value(QStringLiteral("unreadTotal")).toInt(), 7);

        store.setFolderCollapsed(folder, true);
        arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A"), 3),
            space(QStringLiteral("!b:x"), QStringLiteral("B"), 4),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("[Work]"), QStringLiteral("!c:x") }));
    }

    // A Space is in at most one folder, and a folder emptied by a write is
    // removed.
    void aSpaceIsInAtMostOneFolderAndAnEmptiedFolderGoes()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        const QString play = store.createFolder(QStringLiteral("Play"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        QCOMPARE(store.folderOf(QStringLiteral("!a:x")), work);

        // Moving it leaves the old folder as part of joining the new one.
        store.setSpaceFolder(QStringLiteral("!a:x"), play);
        QCOMPARE(store.folderOf(QStringLiteral("!a:x")), play);

        // Work held a Space and now holds none, so it is gone. A folder created
        // empty and never filled stays ("New folder…" makes one to drag Spaces
        // into).
        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("[Play]"),
                               QStringLiteral("!a:x") }));
    }

    void deletingAFolderPutsItsSpacesBackWhereItWas()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        store.setSpaceFolder(QStringLiteral("!c:x"), folder);
        // Put the folder between two loose Spaces.
        store.setTopLevelOrder({ QStringLiteral("!a:x"), folder,
                                 QStringLiteral("!d:x") });

        store.deleteFolder(folder);
        // Deleting undoes the grouping in place, not at the bottom.
        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
            space(QStringLiteral("!d:x"), QStringLiteral("D")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x"),
                               QStringLiteral("!c:x"), QStringLiteral("!d:x") }));
        QVERIFY(store.folders().isEmpty());
    }

    void theArrangementSurvivesAReload()
    {
        SettingsManager settings;
        QString folder;
        {
            RailLayoutStore store(&settings);
            folder = store.createFolder(QStringLiteral("Work"));
            store.setSpaceFolder(QStringLiteral("!a:x"), folder);
            store.setFolderCollapsed(folder, true);
            store.setTopLevelOrder({ QStringLiteral("!c:x"), folder,
                                     QStringLiteral("!b:x") });
        }
        RailLayoutStore reopened(&settings);
        QCOMPARE(reopened.folderOf(QStringLiteral("!a:x")), folder);
        QCOMPARE(reopened.order(),
                 (QStringList{ QStringLiteral("!c:x"), folder,
                               QStringLiteral("!b:x") }));
        QCOMPARE(reopened.folders().size(), 1);
        QCOMPARE(reopened.folders().first().toMap()
                     .value(QStringLiteral("collapsed")).toBool(), true);
    }

    void everyMutationNotifies()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        QSignalSpy changed(&store, &RailLayoutStore::layoutChanged);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        QCOMPARE(changed.count(), 1);
        store.renameFolder(folder, QStringLiteral("Home"));
        QCOMPARE(changed.count(), 2);
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        QCOMPARE(changed.count(), 3);
        store.setFolderCollapsed(folder, true);
        QCOMPARE(changed.count(), 4);
        // A no-op write must not churn the rail.
        store.setFolderCollapsed(folder, true);
        store.renameFolder(folder, QStringLiteral("Home"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        QCOMPARE(changed.count(), 4);
        store.deleteFolder(folder);
        QCOMPARE(changed.count(), 5);
    }

    // The stored format.

    // A layout written before the "expanded" key existed loads with its
    // folders and order intact; the missing key is a default, not a migration.
    void anOlderStoredLayoutKeepsItsFoldersAndOrder()
    {
        SettingsManager settings;
        {
            // The older stored shape.
            RailLayoutStore writer(&settings);
            const QString folder = writer.createFolder(QStringLiteral("Work"));
            writer.setSpaceFolder(QStringLiteral("!a:x"), folder);
            writer.setSpaceFolder(QStringLiteral("!b:x"), folder);
            writer.setFolderCollapsed(folder, true);
            writer.setTopLevelOrder({ QStringLiteral("!c:x"), folder });
        }
        // Strip the key newer builds add.
        QSettings raw;
        const QString key = QStringLiteral("appearance/shell/railLayout");
        QString json = raw.value(key).toString();
        if (json.isEmpty()) {
            // The value is account-scoped with a global fallback; find it.
            for (const QString &candidate : raw.allKeys()) {
                if (candidate.endsWith(QStringLiteral("shell/railLayout"))) {
                    json = raw.value(candidate).toString();
                    if (!json.isEmpty())
                        break;
                }
            }
        }
        QVERIFY2(!json.isEmpty(), "the layout was never written");
        QVERIFY2(json.contains(QStringLiteral("\"folders\"")),
                 qPrintable(json));

        RailLayoutStore reopened(&settings);
        QCOMPARE(reopened.folders().size(), 1);
        const QVariantMap folder = reopened.folders().first().toMap();
        QCOMPARE(folder.value(QStringLiteral("spaceIds")).toStringList(),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));
        QCOMPARE(folder.value(QStringLiteral("collapsed")).toBool(), true);
        QCOMPARE(reopened.order().first(), QStringLiteral("!c:x"));
        // The new state defaults to nothing expanded, which is what an older
        // layout meant.
        QVERIFY(reopened.expandedSpaceIds().isEmpty());
    }

    void expansionIsPersistedAndNeverAppliesToAPseudoRow()
    {
        SettingsManager settings;
        {
            RailLayoutStore store(&settings);
            store.setSpaceExpanded(QStringLiteral("!a:x"), true);
            store.setSpaceExpanded(QString(), true);
            store.setSpaceExpanded(QStringLiteral("@orphans"), true);
            QVERIFY(store.spaceExpanded(QStringLiteral("!a:x")));
            QVERIFY(!store.spaceExpanded(QString()));
            QVERIFY(!store.spaceExpanded(QStringLiteral("@orphans")));
            store.toggleSpaceExpanded(QStringLiteral("!a:x"));
            QVERIFY(!store.spaceExpanded(QStringLiteral("!a:x")));
            store.toggleSpaceExpanded(QStringLiteral("!a:x"));
        }
        // Expansion is persisted, as Element does for its Space panel.
        RailLayoutStore reopened(&settings);
        QVERIFY(reopened.spaceExpanded(QStringLiteral("!a:x")));
    }

    // The atomic arrangement write.

    void oneArrangementWriteReplacesTheWholePicture()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setSpaceFolder(QStringLiteral("!b:x"), work);
        store.setTopLevelOrder({ work, QStringLiteral("!c:x") });

        QSignalSpy changed(&store, &RailLayoutStore::layoutChanged);
        // b leaves the folder and lands after c; a stays.
        store.applyArrangement({ work, QStringLiteral("!c:x"),
                                 QStringLiteral("!b:x") },
                               { { work, QStringList{ QStringLiteral("!a:x") } } });
        QCOMPARE(changed.count(), 1);   // ONE write, not three
        QCOMPARE(store.folderMembers(work),
                 QStringList{ QStringLiteral("!a:x") });
        QCOMPARE(store.order(), (QStringList{ work, QStringLiteral("!c:x"),
                                              QStringLiteral("!b:x") }));
        QVERIFY(store.folderOf(QStringLiteral("!b:x")).isEmpty());
    }

    void aCollapsedFolderIsNotEmptiedByADragThatNeverShowedIt()
    {
        // The rail only renders an open folder's members, so a collapsed
        // folder left out of the call keeps what it holds rather than being
        // emptied.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString shut = store.createFolder(QStringLiteral("Shut"));
        store.setSpaceFolder(QStringLiteral("!a:x"), shut);
        store.setSpaceFolder(QStringLiteral("!b:x"), shut);
        store.setFolderCollapsed(shut, true);

        store.applyArrangement({ shut, QStringLiteral("!c:x") }, {});
        QCOMPARE(store.folderMembers(shut),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));

        // ...but a member the call placed elsewhere leaves it.
        store.applyArrangement({ shut, QStringLiteral("!a:x") }, {});
        QCOMPARE(store.folderMembers(shut),
                 QStringList{ QStringLiteral("!b:x") });
    }

    void aFolderTileCarriesItsMembersForTheCompositePreview()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        store.setSpaceFolder(QStringLiteral("!c:x"), folder);
        store.setFolderCollapsed(folder, true);

        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("Alpha")),
            space(QStringLiteral("!b:x"), QStringLiteral("Beta")),
            space(QStringLiteral("!c:x"), QStringLiteral("Gamma")),
        }));
        const QVariantMap row = arranged.at(2).toMap();
        const QVariantList preview =
            row.value(QStringLiteral("memberPreview")).toList();
        QCOMPARE(preview.size(), 3);
        QCOMPARE(preview.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alpha"));
        // A collapsed folder is identified by its contents (the preview).
        QCOMPARE(preview.at(2).toMap().value(QStringLiteral("spaceId")).toString(),
                 QStringLiteral("!c:x"));
    }

    void anOpenFoldersLastMemberKnowsItIsTheLast()
    {
        // The last member of an open folder carries the rounded bottom of the
        // container drawn behind the rows.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
        }));
        QCOMPARE(arranged.at(3).toMap()
                     .value(QStringLiteral("folderLast")).toBool(), false);
        QCOMPARE(arranged.at(4).toMap()
                     .value(QStringLiteral("folderLast")).toBool(), true);
    }

    void everySpaceIsOrderedForTheChannelsLayoutEvenInsideAShutFolder()
    {
        // orderedSpaceIds answers an ordering question, so a collapsed folder
        // keeps its Spaces in the answer (unlike arrange()).
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        store.setFolderCollapsed(folder, true);
        store.setTopLevelOrder({ QStringLiteral("!c:x"), folder,
                                 QStringLiteral("!a:x") });

        const QVariantList spaces = withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
        });
        QCOMPARE(store.orderedSpaceIds(spaces),
                 (QStringList{ QStringLiteral("!c:x"), QStringLiteral("!b:x"),
                               QStringLiteral("!a:x") }));
        // And no pseudo row leaks into it.
        QVERIFY(!store.orderedSpaceIds(spaces).contains(QString()));
    }

    // "Other rooms" is offered only when it narrows something: in Channels,
    // Home already lists exactly the rooms no Space does. Driven through the
    // property the rail binds; the model knows nothing of layouts.
    void otherRoomsIsOfferedOnlyWhenItNarrowsSomething()
    {
        FakeClient client;
        RoomInfo orphan;
        orphan.id = QStringLiteral("!lonely:x");
        orphan.name = QStringLiteral("Lonely");
        orphan.membership = RoomInfo::Joined;
        client.roomList = {
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A"),
                      { QStringLiteral("!inside:x") }),
            orphan,
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        // Classic is the default, and the tile is shown there.
        QVERIFY2(model.orphansEntryVisible(),
                 "the tile defaults to hidden, so Classic loses it");
        QVERIFY2(modelIds(model).contains(SpaceManager::orphansId()),
                 qPrintable(modelIds(model).join(QLatin1Char(','))));

        model.setOrphansEntryVisible(false);
        QVERIFY2(!modelIds(model).contains(SpaceManager::orphansId()),
                 "Channels still offers a tile that opens Home");
        // Nothing else moved.
        QVERIFY(modelIds(model).contains(SpaceManager::allRoomsId()));
        QVERIFY(modelIds(model).contains(QStringLiteral("!a:x")));

        model.setOrphansEntryVisible(true);
        QVERIFY2(modelIds(model).contains(SpaceManager::orphansId()),
                 "switching back to Classic did not restore the tile");
    }

    // A Space whose direct children are rooms (no subspaces) can be expanded:
    // the chevron is the only expansion trigger. Using the transitive
    // `childCount` instead would let an umbrella that owns nothing directly
    // expand into nothing, so a separate count is asserted both ways.
    void aSpaceWithRoomsButNoSubspacesCanStillBeExpanded()
    {
        RoomInfo channel;
        channel.id = QStringLiteral("!chan:x");
        channel.name = QStringLiteral("general");
        channel.isSpace = false;
        channel.membership = RoomInfo::Joined;

        FakeClient client;
        client.roomList = {
            // A leaf category: one room, no subspaces.
            spaceRoom(QStringLiteral("!cat:x"), QStringLiteral("Category"),
                      { QStringLiteral("!chan:x") }),
            // An umbrella with neither rooms nor subspaces of its own.
            spaceRoom(QStringLiteral("!empty:x"), QStringLiteral("Empty")),
            channel,
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        auto expandableOf = [&model](const QString &id) {
            for (int row = 0; row < model.rowCount(); ++row) {
                const QModelIndex idx = model.index(row, 0);
                if (model.data(idx, RailEntryModel::EntryIdRole).toString()
                    == id) {
                    return model.data(idx, RailEntryModel::ExpandableRole)
                        .toBool();
                }
            }
            return false;
        };

        QVERIFY2(expandableOf(QStringLiteral("!cat:x")),
                 "a Space with a joined room and no subspaces is not "
                 "expandable, so its chevron never appears and its rooms "
                 "cannot be reached from the rail at all");
        QVERIFY2(!expandableOf(QStringLiteral("!empty:x")),
                 "a Space with nothing in it offers a chevron that opens "
                 "nothing");
    }

    // A bridged three-level tree (category > server > category > channels):
    // each channel appears only under the category that owns it directly, and
    // those categories are expandable. One fixture, because two accessors can
    // each be right while the composed tree is wrong.
    void duskTreeListsEachChannelOnlyUnderItsOwnCategory()
    {
        auto room = [](const QString &id, const QString &name) {
            RoomInfo r;
            r.id = id;
            r.name = name;
            r.isSpace = false;
            r.membership = RoomInfo::Joined;
            return r;
        };

        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!umbrella:x"),
                      QStringLiteral("Discord Category"),
                      { QStringLiteral("!server1:x"),
                        QStringLiteral("!server2:x") }),
            spaceRoom(QStringLiteral("!server1:x"), QStringLiteral("Server 1"),
                      { QStringLiteral("!cat1:x"), QStringLiteral("!cat2:x") },
                      { QStringLiteral("!umbrella:x") }),
            spaceRoom(QStringLiteral("!server2:x"), QStringLiteral("Server 2"),
                      { QStringLiteral("!cat3:x") },
                      { QStringLiteral("!umbrella:x") }),
            spaceRoom(QStringLiteral("!cat1:x"), QStringLiteral("category 1"),
                      { QStringLiteral("!chanA:x"), QStringLiteral("!chanB:x") },
                      { QStringLiteral("!server1:x") }),
            spaceRoom(QStringLiteral("!cat2:x"), QStringLiteral("category 2"),
                      { QStringLiteral("!chanC:x") },
                      { QStringLiteral("!server1:x") }),
            spaceRoom(QStringLiteral("!cat3:x"), QStringLiteral("category 3"),
                      { QStringLiteral("!chanD:x") },
                      { QStringLiteral("!server2:x") }),
            room(QStringLiteral("!chanA:x"), QStringLiteral("channel a")),
            room(QStringLiteral("!chanB:x"), QStringLiteral("channel b")),
            room(QStringLiteral("!chanC:x"), QStringLiteral("channel c")),
            room(QStringLiteral("!chanD:x"), QStringLiteral("channel d")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);

        auto directIds = [&spaces](const QString &spaceId) {
            QStringList out;
            const QVariantList rows = spaces.directChildRoomsDetailed(spaceId);
            for (const QVariant &v : rows)
                out << v.toMap().value(QStringLiteral("roomId")).toString();
            out.sort();
            return out;
        };

        // Neither the umbrella nor either server owns a channel directly, so
        // the rail reveals none under them.
        QCOMPARE(directIds(QStringLiteral("!umbrella:x")), QStringList{});
        QCOMPARE(directIds(QStringLiteral("!server1:x")), QStringList{});
        QCOMPARE(directIds(QStringLiteral("!server2:x")), QStringList{});

        // Each channel appears under exactly the category that owns it.
        QCOMPARE(directIds(QStringLiteral("!cat1:x")),
                 (QStringList{ QStringLiteral("!chanA:x"),
                               QStringLiteral("!chanB:x") }));
        QCOMPARE(directIds(QStringLiteral("!cat2:x")),
                 QStringList{ QStringLiteral("!chanC:x") });
        QCOMPARE(directIds(QStringLiteral("!cat3:x")),
                 QStringList{ QStringLiteral("!chanD:x") });

        // The categories (rooms, no subspaces) are openable.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        // Reports "absent" separately from "not expandable": nested rows only
        // exist while their ancestors are expanded.
        auto rowOf = [&model](const QString &id) {
            for (int row = 0; row < model.rowCount(); ++row) {
                if (model.data(model.index(row, 0),
                               RailEntryModel::EntryIdRole).toString() == id) {
                    return row;
                }
            }
            return -1;
        };
        auto expandableOf = [&model, &rowOf](const QString &id) {
            const int row = rowOf(id);
            return row >= 0
                && model.data(model.index(row, 0),
                              RailEntryModel::ExpandableRole).toBool();
        };

        QVERIFY2(rowOf(QStringLiteral("!umbrella:x")) >= 0,
                 "the root space is not in the rail at all");
        QVERIFY2(expandableOf(QStringLiteral("!umbrella:x")),
                 "the umbrella has subspaces and cannot be opened");

        // Nested rows exist only once their ancestors are open; walk down one
        // chevron at a time.
        QVERIFY2(rowOf(QStringLiteral("!server1:x")) < 0,
                 "a subspace is listed while its parent is collapsed");
        store.setSpaceExpanded(QStringLiteral("!umbrella:x"), true);
        QVERIFY2(rowOf(QStringLiteral("!server1:x")) >= 0,
                 "expanding the umbrella did not reveal its servers");
        QVERIFY2(expandableOf(QStringLiteral("!server1:x")),
                 "a server holding categories cannot be opened");

        store.setSpaceExpanded(QStringLiteral("!server1:x"), true);
        QVERIFY2(rowOf(QStringLiteral("!cat1:x")) >= 0,
                 "expanding the server did not reveal its categories");
        QVERIFY2(expandableOf(QStringLiteral("!cat1:x")),
                 "a category holding channels cannot be opened, so those "
                 "channels are unreachable from the rail");

        // The transitive accessor is unchanged; the Channels column and the
        // Classic filter depend on it.
        QStringList transitive;
        const QVariantList all =
            spaces.childRoomsDetailed(QStringLiteral("!umbrella:x"));
        for (const QVariant &v : all)
            transitive << v.toMap().value(QStringLiteral("roomId")).toString();
        transitive.sort();
        QCOMPARE(transitive,
                 (QStringList{ QStringLiteral("!chanA:x"),
                               QStringLiteral("!chanB:x"),
                               QStringLiteral("!chanC:x"),
                               QStringLiteral("!chanD:x") }));
    }

    // The gesture.

    void aPreviewDragMovesRowsWithoutWritingAnything()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        const QStringList before = modelIds(model);
        QVERIFY2(before.contains(QStringLiteral("!a:x")), qPrintable(before.join(QLatin1Char(','))));
        const int aRow = model.rowForEntry(QStringLiteral("!a:x"));
        const int cRow = model.rowForEntry(QStringLiteral("!c:x"));
        QVERIFY(aRow >= 0 && cRow > aRow);

        QSignalSpy moves(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy written(&store, &RailLayoutStore::layoutChanged);
        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.hoverGap(cRow + 1);
        // The neighbours have already moved, as a real rowsMoved the view can
        // animate.
        QVERIFY2(moves.count() >= 1, "the preview reorder was a reset, so the "
                                     "rows cannot animate and the delegate "
                                     "holding the gesture was destroyed");
        QCOMPARE(model.rowForEntry(QStringLiteral("!a:x")), cRow);
        QVERIFY2(written.count() == 0,
                 "the drag wrote settings while the pointer was still down");

        model.endDrag(true);
        QCOMPARE(written.count(), 1);
        QCOMPARE(store.order(), (QStringList{ QStringLiteral("!b:x"),
                                              QStringLiteral("!c:x"),
                                              QStringLiteral("!a:x") }));
    }

    void anAbandonedDragRestoresTheStoredArrangement()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        const QStringList before = modelIds(model);
        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.hoverGap(model.rowForEntry(QStringLiteral("!b:x")) + 1);
        QVERIFY(modelIds(model) != before);
        model.endDrag(false);
        QCOMPARE(modelIds(model), before);
        QVERIFY(store.order().isEmpty());
    }

    void droppingOneSpaceOntoAnotherCreatesAFolderWhereTheTargetWas()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY(model.beginDrag(QStringLiteral("!c:x")));
        model.hoverGroup(model.rowForEntry(QStringLiteral("!a:x")));
        QVERIFY2(model.grouping(),
                 "holding one Space over another offered a reorder, not a group");
        QCOMPARE(model.dropTargetId(), QStringLiteral("!a:x"));
        model.endDrag(true);

        QCOMPARE(store.folders().size(), 1);
        const QString folder =
            store.folders().first().toMap().value(QStringLiteral("id")).toString();
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!c:x") }));
        // The folder takes the target's position, so the gesture reads as a
        // merge.
        QCOMPARE(store.order().indexOf(folder), 0);
    }

    void droppingOntoAFiledSpaceJoinsThatFolderRatherThanNestingOne()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY(model.beginDrag(QStringLiteral("!c:x")));
        model.hoverGroup(model.rowForEntry(QStringLiteral("!a:x")));
        model.endDrag(true);
        QCOMPARE(store.folders().size(), 1);   // no second, nested folder
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!c:x") }));
    }

    void droppingOntoAFolderFilesTheSpaceThere()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setFolderCollapsed(folder, true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY(model.beginDrag(QStringLiteral("!b:x")));
        model.hoverGroup(model.rowForEntry(folder));
        QCOMPARE(model.dropTargetId(), folder);
        model.endDrag(true);
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));
    }

    void aSpaceCanBeReorderedInsideAFolderAndDraggedBackOut()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        store.setTopLevelOrder({ folder, QStringLiteral("!z:x") });
        RailEntryModel model;
        model.setSources(&spaces, &store);
        // [All rooms][folder][a][b][z]: no rooms outside a Space here, so no
        // orphans row.
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ folder, QStringLiteral("!a:x"),
                               QStringLiteral("!b:x"), QStringLiteral("!z:x") }));

        // Reorder inside the folder: b above a.
        QVERIFY(model.beginDrag(QStringLiteral("!b:x")));
        model.hoverGap(model.rowForEntry(QStringLiteral("!a:x")));
        model.endDrag(true);
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!b:x"), QStringLiteral("!a:x") }));

        // Drag one back out, to the end of the rail.
        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.hoverGap(model.rowCount());
        model.endDrag(true);
        QVERIFY2(store.folderOf(QStringLiteral("!a:x")).isEmpty(),
                 "a Space dragged past everything stayed filed");
        QCOMPARE(store.folderMembers(folder),
                 QStringList{ QStringLiteral("!b:x") });
        QVERIFY(store.order().contains(QStringLiteral("!a:x")));
    }

    void aFolderMovesWithItsMembersAndNeverLandsInsideAnother()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        const QString play = store.createFolder(QStringLiteral("Play"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setSpaceFolder(QStringLiteral("!b:x"), play);
        store.setTopLevelOrder({ work, play, QStringLiteral("!z:x") });
        RailEntryModel model;
        model.setSources(&spaces, &store);
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ work, QStringLiteral("!a:x"), play,
                               QStringLiteral("!b:x"), QStringLiteral("!z:x") }));

        // Drag Work to the end; its member travels with it.
        QVERIFY(model.beginDrag(work));
        model.hoverGap(model.rowCount());
        const QStringList preview = modelIds(model).mid(1);
        const int workAt = preview.indexOf(work);
        QVERIFY(workAt >= 0);
        QCOMPARE(preview.at(workAt + 1), QStringLiteral("!a:x"));
        model.endDrag(true);
        QCOMPARE(store.folderMembers(work),
                 QStringList{ QStringLiteral("!a:x") });
        QCOMPARE(store.folderMembers(play),
                 QStringList{ QStringLiteral("!b:x") });
        // Folders do not nest.
        QVERIFY(!store.folderMembers(play).contains(work));
        QVERIFY(store.order().contains(work));
    }

    void aFolderIsNeverOfferedAsSomethingToDropAFolderInto()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        const QString play = store.createFolder(QStringLiteral("Play"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setSpaceFolder(QStringLiteral("!b:x"), play);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY(model.beginDrag(work));
        model.hoverGroup(model.rowForEntry(play));
        QVERIFY2(!model.grouping(),
                 "dragging a folder onto a folder offered to nest them, which "
                 "the store cannot represent");
        model.endDrag(true);
        QCOMPARE(store.folders().size(), 2);
    }

    // A subspace reorder is remembered under its parent's key and does not
    // go through applyArrangement, which would rewrite the top-level order as
    // a side effect.
    void reorderingASubspaceIsRememberedAndTouchesNothingElse()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!other:x"), QStringLiteral("Other"), {}),
            spaceRoom(QStringLiteral("!parent:x"), QStringLiteral("Parent"),
                      { QStringLiteral("!a:x"), QStringLiteral("!b:x"),
                        QStringLiteral("!c:x") }),
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A"), {},
                      { QStringLiteral("!parent:x") }),
            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B"), {},
                      { QStringLiteral("!parent:x") }),
            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C"), {},
                      { QStringLiteral("!parent:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!parent:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        const auto childRows = [&model] {
            QStringList out;
            for (int i = 0; i < model.rowCount(); ++i) {
                const QModelIndex idx = model.index(i, 0);
                if (!model.data(idx, RailEntryModel::HierarchyChildRole)
                         .toBool()) {
                    continue;
                }
                out << model.data(idx, RailEntryModel::SpaceIdRole).toString();
            }
            return out;
        };
        const QStringList before = childRows();
        QCOMPARE(before, (QStringList{ QStringLiteral("!a:x"),
                                       QStringLiteral("!b:x"),
                                       QStringLiteral("!c:x") }));
        const QStringList topBefore = store.order();

        // Drag C to the front of its own run.
        const int aRow = model.rowForEntry(QStringLiteral("!a:x"));
        QVERIFY(model.beginDrag(QStringLiteral("!c:x")));
        model.hoverGap(aRow);
        model.endDrag(true);

        QCOMPARE(childRows(), (QStringList{ QStringLiteral("!c:x"),
                                            QStringLiteral("!a:x"),
                                            QStringLiteral("!b:x") }));
        QCOMPARE(store.orderedChildren(QStringLiteral("!parent:x"),
                                       (QStringList{ QStringLiteral("!a:x"),
                                                     QStringLiteral("!b:x"),
                                                     QStringLiteral("!c:x") })),
                 (QStringList{ QStringLiteral("!c:x"), QStringLiteral("!a:x"),
                               QStringLiteral("!b:x") }));

        // It survives a fresh store over the same settings.
        RailLayoutStore reloaded(&settings);
        QCOMPARE(reloaded.orderedChildren(
                     QStringLiteral("!parent:x"),
                     (QStringList{ QStringLiteral("!a:x"),
                                   QStringLiteral("!b:x"),
                                   QStringLiteral("!c:x") })),
                 (QStringList{ QStringLiteral("!c:x"), QStringLiteral("!a:x"),
                               QStringLiteral("!b:x") }));

        // A child that appears later joins the end, as at the top level.
        QCOMPARE(reloaded.orderedChildren(
                     QStringLiteral("!parent:x"),
                     (QStringList{ QStringLiteral("!a:x"),
                                   QStringLiteral("!b:x"),
                                   QStringLiteral("!c:x"),
                                   QStringLiteral("!new:x") })),
                 (QStringList{ QStringLiteral("!c:x"), QStringLiteral("!a:x"),
                               QStringLiteral("!b:x"),
                               QStringLiteral("!new:x") }));

        // The top level is untouched.
        QCOMPARE(store.order(), topBefore);
    }

    // Pseudo rows cannot be dragged; a subspace can, but only among its
    // siblings. Reparenting needs power in a Space the user may not own, and a
    // folder is a top-level grouping, so a subspace cannot be filed either.
    void aPseudoRowCannotBeDraggedAndASubspaceOnlyAmongItsSiblings()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!parent:x"), QStringLiteral("Parent"),
                      { QStringLiteral("!child:x") }),
            spaceRoom(QStringLiteral("!child:x"), QStringLiteral("Child"), {},
                      { QStringLiteral("!parent:x") }),
            // A room, so the orphans pseudo row exists.
            [] {
                RoomInfo info;
                info.id = QStringLiteral("!loose:x");
                info.name = QStringLiteral("loose");
                info.membership = RoomInfo::Joined;
                return info;
            }(),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!parent:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY2(!model.beginDrag(QString()),
                 "the All rooms pseudo row is draggable");
        QVERIFY2(!model.beginDrag(QStringLiteral("@orphans")),
                 "the Other rooms pseudo row is draggable");

        // The subspace is shown (its parent is expanded) and draggable.
        const int childRow = model.rowForEntry(QStringLiteral("!child:x"));
        QVERIFY(childRow >= 0);
        QVERIFY2(model.beginDrag(QStringLiteral("!child:x")),
                 "a subspace refuses to be dragged, so the rail still "
                 "arranges its top level and nothing under it");

        // It cannot leave its parent: every gap resolves to a slot inside the
        // parent's own run.
        for (int gap = 0; gap <= model.rowCount() + 2; ++gap) {
            const int legal = model.legalGapForTest(gap);
            QVERIFY2(legal >= childRow && legal <= childRow + 1,
                     qPrintable(QStringLiteral(
                         "a gap at %1 resolves to %2, outside this "
                         "subspace's only sibling slot (%3..%4) — the drag "
                         "can reparent")
                         .arg(gap).arg(legal).arg(childRow)
                         .arg(childRow + 1)));
        }

        // It cannot be filed into a rail folder.
        model.hoverGroup(model.rowForEntry(QStringLiteral("!parent:x")));
        QVERIFY2(model.dropTargetId().isEmpty(),
                 "a subspace offers to group into a folder, which would "
                 "either detach it from its parent or claim a nesting the "
                 "store has no way to write");
        model.endDrag(false);
    }

    // A top-level entry may not land between a parent and its children; the
    // refusal resolves to the nearer end of that run, not always the top. A
    // release in the top half lands above the run, in the bottom half below
    // it.
    void aTopLevelDropInsideASubspaceRunTakesTheNearerBoundary()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!drag:x"), QStringLiteral("Drag me")),
            spaceRoom(QStringLiteral("!owner:x"), QStringLiteral("Owner"),
                      { QStringLiteral("!c1:x"), QStringLiteral("!c2:x"),
                        QStringLiteral("!c3:x"), QStringLiteral("!c4:x") }),
            spaceRoom(QStringLiteral("!c1:x"), QStringLiteral("C1"), {},
                      { QStringLiteral("!owner:x") }),
            spaceRoom(QStringLiteral("!c2:x"), QStringLiteral("C2"), {},
                      { QStringLiteral("!owner:x") }),
            spaceRoom(QStringLiteral("!c3:x"), QStringLiteral("C3"), {},
                      { QStringLiteral("!owner:x") }),
            spaceRoom(QStringLiteral("!c4:x"), QStringLiteral("C4"), {},
                      { QStringLiteral("!owner:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!owner:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        // Real row indices: the pseudo rows at the top are what `firstMovable`
        // counts.
        const int ownerRow = model.rowForEntry(QStringLiteral("!owner:x"));
        const int runStart = ownerRow + 1;
        QVERIFY(ownerRow > 0);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c1:x")), runStart);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c4:x")), runStart + 3);
        const int runEnd = runStart + 4;   // the gap PAST the whole run
        QCOMPARE(model.rowCount(), runEnd);

        QVERIFY(model.beginDrag(QStringLiteral("!drag:x")));

        int landedBelow = 0;
        int landedAbove = 0;
        for (int gap = runStart; gap < runEnd; ++gap) {
            const int legal = model.legalGapForTest(gap);
            // (1) The refusal holds: nothing resolves strictly inside the run.
            QVERIFY2(legal == ownerRow || legal == runEnd,
                     qPrintable(QStringLiteral(
                         "a gap at %1 resolves to %2, which is inside the "
                         "subspace run %3..%4 — a top-level entry would land "
                         "between a parent and its own children")
                         .arg(gap).arg(legal).arg(runStart).arg(runEnd)));
            // (2) It is the nearer end.
            const int other = legal == ownerRow ? runEnd : ownerRow;
            QVERIFY2(qAbs(legal - gap) <= qAbs(other - gap),
                     qPrintable(QStringLiteral(
                         "a gap at %1 resolves to %2 (%3 rows away) when the "
                         "run's other boundary %4 is only %5 rows away — the "
                         "drop lands further from the pointer than it needs "
                         "to, which is what 'where the tile sits is where it "
                         "will land' forbids")
                         .arg(gap).arg(legal).arg(qAbs(legal - gap))
                         .arg(other).arg(qAbs(other - gap))));
            if (legal == runEnd)
                ++landedBelow;
            else
                ++landedAbove;
        }
        // The counts: a rule that always answered `runEnd` would pass (1) and
        // (2) at the bottom of the run. Four gaps, split 2/2.
        QCOMPARE(landedAbove, 2);
        QCOMPARE(landedBelow, 2);

        // What the release writes: hoverGap() must move the block to the
        // resolved slot.
        model.hoverGap(runEnd - 1);   // the last gap inside the run
        const QStringList ids = modelIds(model);
        const int draggedNow = ids.indexOf(QStringLiteral("!drag:x"));
        const int c4Now = ids.indexOf(QStringLiteral("!c4:x"));
        QVERIFY2(draggedNow > c4Now,
                 qPrintable(QStringLiteral(
                     "released below the run, the dragged Space sits at %1 "
                     "and the run's last child at %2 — it jumped back above "
                     "the whole subtree. Rail: %3")
                     .arg(draggedNow).arg(c4Now)
                     .arg(ids.join(QLatin1Char(',')))));
        model.endDrag(false);
    }

    // Matrix subspaces in the rail.

    void onlyRootSpacesSitAtTheTopLevelAndSubspacesNestWhenExpanded()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!root:x"), QStringLiteral("Root"),
                      { QStringLiteral("!mid:x") }),
            spaceRoom(QStringLiteral("!mid:x"), QStringLiteral("Mid"),
                      { QStringLiteral("!leaf:x") },
                      { QStringLiteral("!root:x") }),
            spaceRoom(QStringLiteral("!leaf:x"), QStringLiteral("Leaf"), {},
                      { QStringLiteral("!mid:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        // Collapsed: only the root. A subspace is reached by opening its
        // parent, as in Element.
        QStringList ids = modelIds(model);
        QVERIFY(ids.contains(QStringLiteral("!root:x")));
        QVERIFY2(!ids.contains(QStringLiteral("!mid:x")),
                 "a subspace is listed at the top level as well as under its "
                 "parent");

        store.setSpaceExpanded(QStringLiteral("!root:x"), true);
        ids = modelIds(model);
        QCOMPARE(ids.indexOf(QStringLiteral("!mid:x")),
                 ids.indexOf(QStringLiteral("!root:x")) + 1);
        // Real depth, not a two-level approximation.
        const int mid = model.rowForEntry(QStringLiteral("!mid:x"));
        QCOMPARE(model.data(model.index(mid, 0),
                            RailEntryModel::LevelRole).toInt(), 1);
        QVERIFY(model.data(model.index(mid, 0),
                           RailEntryModel::HierarchyChildRole).toBool());
        QVERIFY(model.data(model.index(mid, 0),
                           RailEntryModel::ExpandableRole).toBool());

        store.setSpaceExpanded(QStringLiteral("!mid:x"), true);
        const int leaf = model.rowForEntry(QStringLiteral("!leaf:x"));
        QVERIFY(leaf > mid);
        QCOMPARE(model.data(model.index(leaf, 0),
                            RailEntryModel::LevelRole).toInt(), 2);
        QVERIFY(!model.data(model.index(leaf, 0),
                            RailEntryModel::ExpandableRole).toBool());
    }

    // `folderLast` marks the last row of a folder's run, including nested
    // rows of an expanded last member, not the last member itself. The view
    // reads it for the container's rounded bottom. With every member collapsed
    // the two coincide, so the fixture expands the last member.
    void folderLastMarksTheLastRowOfTheRunNotTheLastMember()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!p:x"), QStringLiteral("P"),
                      { QStringLiteral("!pc:x") }),
            spaceRoom(QStringLiteral("!pc:x"), QStringLiteral("PC"), {},
                      { QStringLiteral("!p:x") }),
            spaceRoom(QStringLiteral("!q:x"), QStringLiteral("Q")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        // P is the last member and is expanded, so its child is the run's
        // last row.
        const QString folder = store.createFolderWithSpaces(
            { QStringLiteral("!q:x"), QStringLiteral("!p:x") }, -1,
            QStringLiteral("Work"));
        QVERIFY(!folder.isEmpty());
        store.setSpaceExpanded(QStringLiteral("!p:x"), true);

        RailEntryModel model;
        model.setSources(&spaces, &store);

        int rowP = -1;
        int rowPC = -1;
        for (int i = 0; i < model.rowCount(); ++i) {
            const QString id = model.data(model.index(i, 0),
                                          RailEntryModel::EntryIdRole)
                                   .toString();
            if (id == QStringLiteral("!p:x")) rowP = i;
            if (id == QStringLiteral("!pc:x")) rowPC = i;
        }
        QVERIFY2(rowP >= 0 && rowPC >= 0,
                 "the fixture produced no folder with an expanded member and "
                 "a nested row, so this case measures nothing");
        QVERIFY2(rowPC > rowP, "the child must follow its parent");

        const auto lastAt = [&](int row) {
            return model.data(model.index(row, 0),
                              RailEntryModel::FolderLastRole).toBool();
        };
        QVERIFY2(!lastAt(rowP),
                 "the folder's last MEMBER carries folderLast while a nested "
                 "row still follows it — the container squares its bottom a "
                 "row early and pinches there");
        QVERIFY2(lastAt(rowPC),
                 "the last ROW of the folder's run is not marked last, so the "
                 "container squares its bottom and overshoots into the gap "
                 "below it");
    }

    // The group field's bounds come from the rows' level sequence, not the
    // Space graph, so they follow a drag preview. The comparison is `<`, not
    // `<=`: siblings must not close a region. Fixture:
    //
    //   Root
    //   ├── A            A, B and C are adjacent siblings, so the region under
    //   ├── B            Root runs through all five rows and closes at C1a.
    //   └── C
    //       └── C1
    //           └── C1a
    void theGroupFieldFollowsTheRowsRatherThanTheGraph()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!root:x"), QStringLiteral("Root"),
                      { QStringLiteral("!a:x"), QStringLiteral("!b:x"),
                        QStringLiteral("!c:x") }),
            // Leaves: two adjacent siblings are the only arrangement where `<`
            // and `<=` disagree.
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A"), {},
                      { QStringLiteral("!root:x") }),
            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B"), {},
                      { QStringLiteral("!root:x") }),
            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C"),
                      { QStringLiteral("!c1:x") },
                      { QStringLiteral("!root:x") }),
            spaceRoom(QStringLiteral("!c1:x"), QStringLiteral("C1"),
                      { QStringLiteral("!c1a:x") },
                      { QStringLiteral("!c:x") }),
            // The third level: C1a closes a run deeper than the one above it,
            // proving the bottom is read from the next row, and stacks three
            // layers.
            spaceRoom(QStringLiteral("!c1a:x"), QStringLiteral("C1a"), {},
                      { QStringLiteral("!c1:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);
        for (const QString &id : { QStringLiteral("!root:x"),
                                   QStringLiteral("!c:x"),
                                   QStringLiteral("!c1:x") }) {
            store.setSpaceExpanded(id, true);
        }

        // What the view asks per layer: the region at depth `d` opens when the
        // row above is shallower than d and closes when the row below is.
        const auto bandAt = [&model](const QString &id, int depth) {
            const int row = model.rowForEntry(id);
            const int prev = model.data(model.index(row, 0),
                                        RailEntryModel::BandPrevLevelRole)
                                 .toInt();
            const int next = model.data(model.index(row, 0),
                                        RailEntryModel::BandNextLevelRole)
                                 .toInt();
            return QPair<bool, bool>(prev < depth, next < depth);
        };
        const auto bandOf = [&bandAt, &model](const QString &id) {
            const int row = model.rowForEntry(id);
            return bandAt(id, model.data(model.index(row, 0),
                                         RailEntryModel::LevelRole).toInt());
        };

        // The fixture has the shape described above.
        const int rootRow = model.rowForEntry(QStringLiteral("!root:x"));
        QCOMPARE(model.rowForEntry(QStringLiteral("!a:x")), rootRow + 1);
        QCOMPARE(model.rowForEntry(QStringLiteral("!b:x")), rootRow + 2);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c:x")), rootRow + 3);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c1:x")), rootRow + 4);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c1a:x")), rootRow + 5);

        // A opens the run under Root.
        QVERIFY2(bandOf(QStringLiteral("!a:x")).first,
                 "the region does not open at A, so Root's children sit on "
                 "bare rail");

        // A is followed by its sibling and must not close; B is preceded by one
        // and must not open. `<=` would make each child its own pill.
        QVERIFY2(!bandOf(QStringLiteral("!a:x")).second,
                 "A closes the region although B is its sibling — a Space's "
                 "children are one group, not one pill each");
        QVERIFY2(!bandOf(QStringLiteral("!b:x")).first,
                 "B opens a NEW region although it is A's sibling");
        QVERIFY2(!bandOf(QStringLiteral("!b:x")).second,
                 "B closes the region although C follows it at the same "
                 "depth");
        QVERIFY2(!bandOf(QStringLiteral("!c:x")).second,
                 "C closes the region although C1 is nested inside it");

        // The bottom is read from the row below: C1 has C1a under it.
        QVERIFY2(!bandOf(QStringLiteral("!c1:x")).second,
                 "C1 closes its region although C1a is deeper");
        QVERIFY2(bandOf(QStringLiteral("!c1a:x")).second,
                 "the deepest row does not close its region, so the field "
                 "runs off the end of the tree");

        // The layers nest: each row draws one region per ancestor, so the
        // outer region stays open across deeper rows.
        QVERIFY2(!bandAt(QStringLiteral("!c:x"), 1).second,
                 "the depth-1 region closes at C although C1 is inside it — "
                 "a parent's region must run behind its descendants");
        QVERIFY2(!bandAt(QStringLiteral("!c1:x"), 1).first
                     && !bandAt(QStringLiteral("!c1:x"), 1).second,
                 "C1 opens or closes the depth-1 region, so the outer layer "
                 "is broken by a row that is merely deeper than it");
        QVERIFY2(bandAt(QStringLiteral("!c1a:x"), 1).second,
                 "the depth-1 region does not close at the last row inside "
                 "it");
        // The inner region is bounded by its own depth.
        QVERIFY2(bandAt(QStringLiteral("!c1:x"), 2).first,
                 "the depth-2 region does not open at C1");
        QVERIFY2(!bandAt(QStringLiteral("!c1:x"), 2).second,
                 "the depth-2 region closes at C1 although C1a is deeper");
        QVERIFY2(bandAt(QStringLiteral("!c1a:x"), 3).first
                     && bandAt(QStringLiteral("!c1a:x"), 3).second,
                 "C1a's own depth-3 region is not a single row");
    }

    void aCyclicHierarchyKeepsEverySpaceReachableAndTerminates()
    {
        // A -> B -> A is legal: the walk must terminate and keep every joined
        // Space.
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A"),
                      { QStringLiteral("!b:x") }, { QStringLiteral("!b:x") }),
            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B"),
                      { QStringLiteral("!a:x") }, { QStringLiteral("!a:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!a:x"), true);
        store.setSpaceExpanded(QStringLiteral("!b:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        const QStringList ids = modelIds(model);
        QVERIFY2(ids.contains(QStringLiteral("!a:x")), "a cycle lost a Space");
        QVERIFY2(ids.contains(QStringLiteral("!b:x")), "a cycle lost a Space");
        // Each Space appears exactly once, however the cycle is entered.
        QCOMPARE(ids.count(QStringLiteral("!a:x")), 1);
        QCOMPARE(ids.count(QStringLiteral("!b:x")), 1);
    }

    void aSubspaceWithTwoJoinedParentsNestsUnderExactlyOne()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!p1:x"), QStringLiteral("P1"),
                      { QStringLiteral("!shared:x") }),
            spaceRoom(QStringLiteral("!p2:x"), QStringLiteral("P2"),
                      { QStringLiteral("!shared:x") }),
            spaceRoom(QStringLiteral("!shared:x"), QStringLiteral("Shared"), {},
                      { QStringLiteral("!p1:x"), QStringLiteral("!p2:x") }),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!p1:x"), true);
        store.setSpaceExpanded(QStringLiteral("!p2:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        const QStringList ids = modelIds(model);
        QCOMPARE(ids.count(QStringLiteral("!shared:x")), 1);
        // The choice is stable across rebuilds.
        const int first = ids.indexOf(QStringLiteral("!shared:x"));
        client.announce();
        QCOMPARE(modelIds(model).indexOf(QStringLiteral("!shared:x")), first);
    }

    // The flat (Classic) rail lists top-level Spaces only, with the nested
    // rows absent from the model rather than hidden, since drag, bands and
    // drop targets all index into this list.
    void aFlatRailListsTopLevelSpacesAndNothingUnderThem()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!top:x"), QStringLiteral("Top"),
                      { QStringLiteral("!mid:x") }),
            spaceRoom(QStringLiteral("!mid:x"), QStringLiteral("Mid"),
                      { QStringLiteral("!deep:x") },
                      { QStringLiteral("!top:x") }),
            spaceRoom(QStringLiteral("!deep:x"), QStringLiteral("Deep"), {},
                      { QStringLiteral("!mid:x") }),
            spaceRoom(QStringLiteral("!other:x"), QStringLiteral("Other"), {}),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!top:x"), true);
        store.setSpaceExpanded(QStringLiteral("!mid:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        // Regions: the whole chain is listed.
        const QStringList nested = modelIds(model);
        QVERIFY2(nested.contains(QStringLiteral("!mid:x")), "mid is missing "
                                                            "from the nested rail");
        QVERIFY2(nested.contains(QStringLiteral("!deep:x")), "deep is missing "
                                                             "from the nested rail");

        model.setFlat(true);
        const QStringList flat = modelIds(model);
        QVERIFY2(flat.contains(QStringLiteral("!top:x")), "a TOP-LEVEL Space "
                                                          "went missing on the flat rail");
        QVERIFY2(flat.contains(QStringLiteral("!other:x")),
                 "a second top-level Space went missing on the flat rail");
        QVERIFY2(!flat.contains(QStringLiteral("!mid:x")),
                 "a subspace is still listed on the flat rail");
        QVERIFY2(!flat.contains(QStringLiteral("!deep:x")),
                 "a deep subspace is still listed on the flat rail");

        // Nothing expands: `expandable` draws a chevron, and `expanded` is read
        // by the region code.
        for (int i = 0; i < model.rowCount(); ++i) {
            const QModelIndex idx = model.index(i, 0);
            const QString id =
                model.data(idx, RailEntryModel::EntryIdRole).toString();
            QVERIFY2(!model.data(idx, RailEntryModel::ExpandableRole).toBool(),
                     qPrintable(QStringLiteral("%1 is expandable on the flat "
                                               "rail").arg(id)));
            QVERIFY2(!model.data(idx, RailEntryModel::ExpandedRole).toBool(),
                     qPrintable(QStringLiteral("%1 is expanded on the flat "
                                               "rail").arg(id)));
            QVERIFY2(!model.data(idx,
                                 RailEntryModel::HierarchyChildRole).toBool(),
                     qPrintable(QStringLiteral("%1 is a hierarchy child on "
                                               "the flat rail").arg(id)));
            QCOMPARE(model.data(idx, RailEntryModel::LevelRole).toInt(), 0);
        }

        // The round trip is lossless: Classic ignores the expansion state
        // rather than clearing it.
        model.setFlat(false);
        QCOMPARE(modelIds(model), nested);
    }

    // A drag on the flat rail moves one tile over the shorter row list and
    // stores the top-level order.
    void aDragOnTheFlatRailMovesOneTileAndStoresTheTopLevelOrder()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!p:x"), QStringLiteral("Parent"),
                      { QStringLiteral("!c:x") }),
            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("Child"), {},
                      { QStringLiteral("!p:x") }),
            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        // Expanded, and it stays expanded: the drag must not disturb it.
        store.setSpaceExpanded(QStringLiteral("!p:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);
        model.setFlat(true);

        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ QStringLiteral("!p:x"),
                               QStringLiteral("!z:x") }));

        // The subspace is not in the list, so the drag carries one tile;
        // `hoverGap(rowCount())` is the end of the shorter list.
        QVERIFY(model.beginDrag(QStringLiteral("!p:x")));
        model.hoverGap(model.rowCount());
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ QStringLiteral("!z:x"),
                               QStringLiteral("!p:x") }));
        model.endDrag(true);
        QCOMPARE(store.order(), (QStringList{ QStringLiteral("!z:x"),
                                              QStringLiteral("!p:x") }));
        QVERIFY2(store.spaceExpanded(QStringLiteral("!p:x")),
                 "a flat-rail drag cleared the expansion state it is only "
                 "supposed to be ignoring");

        // The stored order is what Regions then draws, unscrambled.
        model.setFlat(false);
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ QStringLiteral("!z:x"), QStringLiteral("!p:x"),
                               QStringLiteral("!c:x") }));
    }

    // The flat rail keeps folders and all their members: folders are the
    // user's grouping of top-level Spaces, not Matrix hierarchy.
    void theFlatRailKeepsFoldersAndEveryOneOfTheirMembers()
    {
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        RailEntryModel model;
        model.setSources(&spaces, &store);
        model.setFlat(true);

        const QStringList ids = modelIds(model);
        for (const char *id : { "!a:x", "!b:x", "!c:x" }) {
            QVERIFY2(ids.contains(QLatin1String(id)),
                     qPrintable(QStringLiteral("%1 vanished from the flat "
                                               "rail: %2")
                                    .arg(QLatin1String(id),
                                         ids.join(QLatin1Char(',')))));
        }
        // `modelIds` reads EntryIdRole, the id createFolder returned
        // ("[Work]" is store.arrange()'s spelling).
        QVERIFY2(ids.contains(folder),
                 qPrintable(QStringLiteral("the folder itself vanished: %1")
                                .arg(ids.join(QLatin1Char(',')))));

        // A collapsed folder still hides its members in Classic.
        store.setFolderCollapsed(folder, true);
        model.refresh();
        const QStringList collapsed = modelIds(model);
        QVERIFY(collapsed.contains(folder));
        QVERIFY(collapsed.contains(QStringLiteral("!c:x")));
        QVERIFY2(!collapsed.contains(QStringLiteral("!a:x")),
                 "a collapsed folder is showing its members on the flat rail");
    }

    void draggingASpaceCarriesItsExpandedSubspacesWithIt()
    {
        // Those rows are Matrix's arrangement under this Space; moving the
        // header alone would strand them.
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!p:x"), QStringLiteral("Parent"),
                      { QStringLiteral("!c:x") }),
            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("Child"), {},
                      { QStringLiteral("!p:x") }),
            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        store.setSpaceExpanded(QStringLiteral("!p:x"), true);
        RailEntryModel model;
        model.setSources(&spaces, &store);
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ QStringLiteral("!p:x"), QStringLiteral("!c:x"),
                               QStringLiteral("!z:x") }));

        QVERIFY(model.beginDrag(QStringLiteral("!p:x")));
        model.hoverGap(model.rowCount());
        const QStringList preview = modelIds(model).mid(1);
        QCOMPARE(preview,
                 (QStringList{ QStringLiteral("!z:x"), QStringLiteral("!p:x"),
                               QStringLiteral("!c:x") }));
        model.endDrag(true);
        // The subspace is not a top-level entry, so only the parent is stored.
        QCOMPARE(store.order(), (QStringList{ QStringLiteral("!z:x"),
                                              QStringLiteral("!p:x") }));
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ QStringLiteral("!z:x"), QStringLiteral("!p:x"),
                               QStringLiteral("!c:x") }));
    }

    void thePreviewSaysExactlyWhatTheReleaseWillDo()
    {
        // A Space dragged out of a folder stops claiming its folderId at once,
        // so the preview matches what the release will produce.
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString folder = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), folder);
        store.setSpaceFolder(QStringLiteral("!b:x"), folder);
        store.setTopLevelOrder({ folder, QStringLiteral("!z:x") });
        RailEntryModel model;
        model.setSources(&spaces, &store);

        auto folderIdAt = [&model](const QString &entryId) {
            const int row = model.rowForEntry(entryId);
            return row < 0 ? QString()
                           : model.data(model.index(row, 0),
                                        RailEntryModel::FolderIdRole).toString();
        };
        auto lastAt = [&model](const QString &entryId) {
            const int row = model.rowForEntry(entryId);
            return row >= 0
                   && model.data(model.index(row, 0),
                                 RailEntryModel::FolderLastRole).toBool();
        };
        QCOMPARE(folderIdAt(QStringLiteral("!b:x")), folder);
        QVERIFY(lastAt(QStringLiteral("!b:x")));
        QVERIFY(!lastAt(QStringLiteral("!a:x")));

        // Drag b past Z: it leaves the folder and A becomes the run's last.
        QVERIFY(model.beginDrag(QStringLiteral("!b:x")));
        model.hoverGap(model.rowCount());
        QVERIFY2(folderIdAt(QStringLiteral("!b:x")).isEmpty(),
                 "the dragged Space still draws its old folder's container, so "
                 "the preview promises a grouping the release will not make");
        QVERIFY2(lastAt(QStringLiteral("!a:x")),
                 "the folder's container has no rounded bottom any more");
        model.endDrag(true);
        QCOMPARE(store.folderMembers(folder),
                 QStringList{ QStringLiteral("!a:x") });
        QVERIFY(store.folderOf(QStringLiteral("!b:x")).isEmpty());
    }

    // Three exclusive verbs: `hoverGroup(row)` (on a tile: arm, move
    // nothing), `hoverGap(gap)` (between tiles: disarm, move) and
    // `clearDropTarget()` (over the dragged block's own slot: neither).
    void restingOnATileClearsTheTargetWithoutMovingAnything()
    {
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
                            spaceRoom(QStringLiteral("!c:x"), QStringLiteral("C")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        auto order = [&model] {
            QStringList out;
            for (int i = 0; i < model.rowCount(); ++i) {
                out << model.data(model.index(i, 0),
                                  RailEntryModel::EntryIdRole).toString();
            }
            return out;
        };

        QVERIFY(model.beginDrag(QStringLiteral("!c:x")));
        model.hoverGroup(model.rowForEntry(QStringLiteral("!a:x")));
        QVERIFY(model.grouping());
        QCOMPARE(model.dropTargetId(), QStringLiteral("!a:x"));
        const QStringList armed = order();

        model.clearDropTarget();
        QVERIFY2(!model.grouping(),
                 "the group target survived the pointer leaving the tile");
        QVERIFY2(model.dropTargetId().isEmpty(),
                 "a stale drop target is still lit");
        QVERIFY2(order() == armed,
                 "clearing the target moved rows, so the tile being aimed at "
                 "steps aside and can never be grouped with");
        QVERIFY2(model.dragging(),
                 "clearing the target ended the gesture");

        // Still groupable afterwards.
        model.hoverGroup(model.rowForEntry(QStringLiteral("!b:x")));
        QVERIFY(model.grouping());
        QCOMPARE(model.dropTargetId(), QStringLiteral("!b:x"));
        model.endDrag(true);
        QCOMPARE(store.folders().size(), 1);
    }

    void aRefreshDuringADragIsDeferredRatherThanApplied()
    {
        // Rebuilding under the pointer would destroy the delegate holding the
        // gesture.
        FakeClient client;
        client.roomList = { spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
                            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")) };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        RailEntryModel model;
        model.setSources(&spaces, &store);

        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.hoverGap(model.rowForEntry(QStringLiteral("!b:x")) + 1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        // A new Space arrives mid-gesture.
        client.roomList.append(spaceRoom(QStringLiteral("!c:x"),
                                         QStringLiteral("C")));
        client.announce();
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.rowForEntry(QStringLiteral("!c:x")), -1);
        QVERIFY(model.dragging());

        model.endDrag(true);
        QVERIFY2(model.rowForEntry(QStringLiteral("!c:x")) >= 0,
                 "the deferred refresh never happened, so the rail is stale");
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(RailLayoutTest)
#include "RailLayoutTest.moc"
