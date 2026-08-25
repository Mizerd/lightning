// The Spaces rail's own arrangement — drag order, folders, and the DRAG
// ITSELF.
//
// The store half is pure over a model snapshot, which is the point: the rail
// can be rearranged, filed and reloaded without a homeserver, a ListView or a
// gesture. What those cases pin is the behaviour a user would notice — a new
// Space does not barge into a hand-made order, a folder that empties does not
// vanish, deleting a folder puts its Spaces back where the folder was, and a
// pseudo row can never be dragged or filed.
//
// The gesture half lives in RailEntryModel, which is exactly why it is
// testable at all: the preview reorder, the reorder-versus-group decision and
// what a release WRITES are model operations, not pointer events. A policy
// test that drives the model proves the outcome; it does not prove the view
// reaches it, and the view's half (the pointer bands, the dwell, the
// auto-scroll) is stated in SpacesRail.qml and NOT covered here — that is an
// honest gap, not a claim.

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

// A client that answers with whatever rooms a case hands it. The drag half
// needs a real SpaceManager (the model reads the hierarchy for its subspace
// rows), and a real SpaceManager needs a client.
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

        // A Space joined later must not land in the middle of an arrangement
        // somebody made by hand.
        arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
            space(QStringLiteral("!b:x"), QStringLiteral("B")),
            space(QStringLiteral("!c:x"), QStringLiteral("C")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("!b:x"), QStringLiteral("!a:x"),
                               QStringLiteral("!c:x") }));

        // A Space that has been left simply stops appearing; its slot in the
        // stored order is not cleaned up, because the account may just not
        // have synced yet.
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
        // A collapsed folder has to carry what it is hiding, or a folder
        // would be a way to lose track of unread messages.
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

    void aSpaceIsInAtMostOneFolderAndAnEmptyFolderStays()
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

        // The now-empty folder is still there: it is a place the user made,
        // and one that vanished when its last Space left would be a bug.
        const QVariantList arranged = store.arrange(withPseudo({
            space(QStringLiteral("!a:x"), QStringLiteral("A")),
        }));
        QCOMPARE(idsOf(arranged).mid(2),
                 (QStringList{ QStringLiteral("[Work]"), QStringLiteral("[Play]"),
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
        // Undoing the grouping, not scattering its contents to the bottom.
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

    // ── The stored format ────────────────────────────────────────────────

    // A layout written by 0.7.6 has folders and an order and NO "expanded"
    // key. It must load with its folders and its order intact; the missing key
    // is a default, not a migration. Losing someone's grouping to a format
    // change is the one unrecoverable failure in this file.
    void anOlderStoredLayoutKeepsItsFoldersAndOrder()
    {
        SettingsManager settings;
        {
            // Exactly the shape 0.7.6 wrote.
            RailLayoutStore writer(&settings);
            const QString folder = writer.createFolder(QStringLiteral("Work"));
            writer.setSpaceFolder(QStringLiteral("!a:x"), folder);
            writer.setSpaceFolder(QStringLiteral("!b:x"), folder);
            writer.setFolderCollapsed(folder, true);
            writer.setTopLevelOrder({ QStringLiteral("!c:x"), folder });
        }
        // Strip the key the newer build adds, leaving a genuinely older value.
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
        // And the new state defaults to "nothing expanded", which is what an
        // older layout meant.
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
        // Element persists its own Space-panel expansion, and so does this:
        // an expansion is how the user wants to navigate, not a glance.
        RailLayoutStore reopened(&settings);
        QVERIFY(reopened.spaceExpanded(QStringLiteral("!a:x")));
    }

    // ── The atomic arrangement write ─────────────────────────────────────

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
        // The rail only renders an OPEN folder's members, so a drag can only
        // ever report those. A collapsed folder left out of the call must keep
        // what it holds — replacing it with an empty list is how a drag past a
        // collapsed folder would silently dissolve it.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString shut = store.createFolder(QStringLiteral("Shut"));
        store.setSpaceFolder(QStringLiteral("!a:x"), shut);
        store.setSpaceFolder(QStringLiteral("!b:x"), shut);
        store.setFolderCollapsed(shut, true);

        store.applyArrangement({ shut, QStringLiteral("!c:x") }, {});
        QCOMPARE(store.folderMembers(shut),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));

        // ...but a member the call placed elsewhere DOES leave it, or the
        // Space would be in two places at once.
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
        // A collapsed folder is identified by its CONTENTS, which is the whole
        // reason the preview exists rather than a generic letter tile.
        QCOMPARE(preview.at(2).toMap().value(QStringLiteral("spaceId")).toString(),
                 QStringLiteral("!c:x"));
    }

    void anOpenFoldersLastMemberKnowsItIsTheLast()
    {
        // The container behind an open folder is drawn per row, so the last
        // member carries the rounded bottom. Without the flag the container
        // reads as a band that ran off the end of the group.
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
        // orderedSpaceIds answers an ORDERING question, so a collapsed folder
        // must not drop its Spaces from the answer — arrange() legitimately
        // hides them, which is why this is a separate accessor.
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

    // ── The gesture ──────────────────────────────────────────────────────

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
        model.updateDrag(cRow, false);
        // The neighbours have ALREADY moved — that is the whole point, and it
        // is a real rowsMoved so the view can animate it.
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
        model.updateDrag(model.rowForEntry(QStringLiteral("!b:x")), false);
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
        model.updateDrag(model.rowForEntry(QStringLiteral("!a:x")), true);
        QVERIFY2(model.grouping(),
                 "holding one Space over another offered a reorder, not a group");
        QCOMPARE(model.dropTargetId(), QStringLiteral("!a:x"));
        model.endDrag(true);

        QCOMPARE(store.folders().size(), 1);
        const QString folder =
            store.folders().first().toMap().value(QStringLiteral("id")).toString();
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!c:x") }));
        // The folder took the TARGET's position, so the gesture reads as the
        // two tiles merging rather than as one being moved somewhere.
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
        model.updateDrag(model.rowForEntry(QStringLiteral("!a:x")), true);
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
        model.updateDrag(model.rowForEntry(folder), true);
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
        // [All rooms][folder][a][b][z] — there are no rooms outside a Space
        // in this fixture, so the orphans pseudo row does not exist.
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ folder, QStringLiteral("!a:x"),
                               QStringLiteral("!b:x"), QStringLiteral("!z:x") }));

        // Reorder INSIDE the folder: b above a.
        QVERIFY(model.beginDrag(QStringLiteral("!b:x")));
        model.updateDrag(model.rowForEntry(QStringLiteral("!a:x")), false);
        model.endDrag(true);
        QCOMPARE(store.folderMembers(folder),
                 (QStringList{ QStringLiteral("!b:x"), QStringLiteral("!a:x") }));

        // Drag one back OUT, to the end of the rail.
        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.updateDrag(model.rowCount() - 1, false);
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

        // Drag Work to the end. Its member has to travel with it.
        QVERIFY(model.beginDrag(work));
        model.updateDrag(model.rowCount() - 1, false);
        const QStringList preview = modelIds(model).mid(1);
        const int workAt = preview.indexOf(work);
        QVERIFY(workAt >= 0);
        QCOMPARE(preview.at(workAt + 1), QStringLiteral("!a:x"));
        model.endDrag(true);
        QCOMPARE(store.folderMembers(work),
                 QStringList{ QStringLiteral("!a:x") });
        QCOMPARE(store.folderMembers(play),
                 QStringList{ QStringLiteral("!b:x") });
        // Folders do not nest, so a folder never becomes another's member.
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
        model.updateDrag(model.rowForEntry(play), true);
        QVERIFY2(!model.grouping(),
                 "dragging a folder onto a folder offered to nest them, which "
                 "the store cannot represent");
        model.endDrag(true);
        QCOMPARE(store.folders().size(), 2);
    }

    void aPseudoRowAndASubspaceCannotBeDragged()
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
        // The subspace is SHOWN (its parent is expanded) but its position
        // belongs to Matrix, not to the user.
        QVERIFY(model.rowForEntry(QStringLiteral("!child:x")) >= 0);
        QVERIFY2(!model.beginDrag(QStringLiteral("!child:x")),
                 "a subspace can be dragged, which would let a local folder "
                 "look like it changes the Matrix hierarchy");
    }

    // ── Matrix subspaces in the rail ─────────────────────────────────────

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

        // Collapsed: only the root. Element Classic's Space panel does exactly
        // this — a subspace is reached by opening its parent.
        QStringList ids = modelIds(model);
        QVERIFY(ids.contains(QStringLiteral("!root:x")));
        QVERIFY2(!ids.contains(QStringLiteral("!mid:x")),
                 "a subspace is listed at the top level as well as under its "
                 "parent");

        store.setSpaceExpanded(QStringLiteral("!root:x"), true);
        ids = modelIds(model);
        QCOMPARE(ids.indexOf(QStringLiteral("!mid:x")),
                 ids.indexOf(QStringLiteral("!root:x")) + 1);
        // Real DEPTH, not a two-level approximation: level was hardcoded to
        // "0 or 1", so a three-deep tree rendered as a flat pair of indents.
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

    void aCyclicHierarchyKeepsEverySpaceReachableAndTerminates()
    {
        // A -> B -> A is legal state. A naive walk never returns; a walk that
        // simply drops what it cannot place loses a Space the user has joined.
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
        // And the choice is STABLE: rebuilding must not move it between the
        // two parents.
        const int first = ids.indexOf(QStringLiteral("!shared:x"));
        client.announce();
        QCOMPARE(modelIds(model).indexOf(QStringLiteral("!shared:x")), first);
    }

    void draggingASpaceCarriesItsExpandedSubspacesWithIt()
    {
        // Those rows are Matrix's arrangement UNDER this Space. Moving the
        // header alone strands them under whatever the drag moved into their
        // place, which reads as the hierarchy having changed.
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
        model.updateDrag(model.rowCount() - 1, false);
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
        // The container band behind an open folder is drawn from each row's
        // own folderId, so a Space dragged OUT of a folder has to stop
        // claiming the folder immediately — otherwise the preview promises a
        // grouping the release will not produce.
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

        // Drag b past Z: it leaves the folder, and A becomes the run's last.
        QVERIFY(model.beginDrag(QStringLiteral("!b:x")));
        model.updateDrag(model.rowCount() - 1, false);
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

    void aRefreshDuringADragIsDeferredRatherThanApplied()
    {
        // Rebuilding under the pointer destroys the delegate holding the
        // gesture — the defect the whole model exists to avoid.
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
        model.updateDrag(model.rowForEntry(QStringLiteral("!b:x")), false);
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
