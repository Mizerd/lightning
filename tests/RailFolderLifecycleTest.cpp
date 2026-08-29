// When a rail folder exists, and when it stops existing.
//
// Rokas asked for one rule — "if a Space folder is empty, delete it" — and the
// whole difficulty is in the word EMPTY. A folder holds SPACE IDS, and the
// rail draws the intersection of those ids with the Spaces the account
// currently knows about. During a cold start, an initial sync or an account
// switch that intersection is legitimately empty for a folder that is full, so
// a rule keyed on what the rail RENDERED would delete a hand-made arrangement
// every time the app opened. Emptiness is therefore a fact about the STORE,
// and it is judged only at the moment of a write that took the last member
// away.
//
// The other half of this suite is the defect that made empty folders appear in
// the first place. `RailEntryModel::commitReorder` named EVERY folder row in
// the arrangement it committed, including collapsed ones whose members it had
// never rendered — and a folder that is named with an empty list is REPLACED,
// which is exactly the guarantee `applyArrangement` documents in the other
// direction. So any reorder drag emptied every collapsed folder: its Spaces
// fell out to the end of the rail and an empty tile stayed behind. With the
// deletion rule above, that same drag would have DELETED the folder outright,
// which is why the two land together.
//
// The gesture cases drive the real drag through the model — beginDrag /
// hoverGap / endDrag — rather than calling the store, because the store has
// always behaved correctly here and it was its CALLER that broke the contract.

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QVariantMap space(const QString &id, const QString &name)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("spaceId"), id);
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("unreadTotal"), 0);
    entry.insert(QStringLiteral("highlightTotal"), 0);
    return entry;
}

QVariantList withPseudo(const QVariantList &spaces)
{
    QVariantList out;
    out.append(space(QString(), QStringLiteral("All rooms")));
    out.append(spaces);
    return out;
}

QStringList idsOf(const QVariantList &arranged)
{
    QStringList out;
    for (const QVariant &value : arranged) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("kind")).toString()
            == QLatin1String("folder")) {
            out << QStringLiteral("[")
                       + entry.value(QStringLiteral("name")).toString()
                       + QStringLiteral("]");
        } else {
            out << entry.value(QStringLiteral("spaceId")).toString();
        }
    }
    return out;
}

QStringList folderIds(const RailLayoutStore &store)
{
    QStringList out;
    for (const QVariant &value : store.folders())
        out << value.toMap().value(QStringLiteral("id")).toString();
    return out;
}

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
};

RoomInfo spaceRoom(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = true;
    info.membership = RoomInfo::Joined;
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

class RailFolderLifecycleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("rail-folder-lifecycle-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ── The deletion rule ────────────────────────────────────────────────

    void aFolderTheUserEmptiesGoesAwayWithTheWriteThatEmptiedIt()
    {
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        QCOMPARE(folderIds(store), QStringList{ work });

        // Its last Space leaves. There is nothing left for the folder to be.
        store.setSpaceFolder(QStringLiteral("!a:x"), QString());
        QVERIFY2(folderIds(store).isEmpty(),
                 "an emptied folder survived the write that emptied it");
        // ...and its slot in the top-level order goes with it, or the rail
        // keeps a place for something nothing can draw.
        QVERIFY(!store.order().contains(work));
        QCOMPARE(idsOf(store.arrange(withPseudo({
                     space(QStringLiteral("!a:x"), QStringLiteral("A")) }))),
                 (QStringList{ QString(), QStringLiteral("!a:x") }));
    }

    void aFolderCreatedEmptyIsNotDeletedBeforeItCanBeFilled()
    {
        // "New folder…" (SpacesRail.qml) makes an empty folder for the user
        // to drag Spaces into. Deleting it because it is empty is not a
        // cleanup, it is the action silently failing.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString made = store.createFolder(QStringLiteral("Later"));
        QVERIFY(!made.isEmpty());
        QCOMPARE(folderIds(store), QStringList{ made });

        // It survives unrelated writes too — a rename, a collapse, and a
        // reorder of the Spaces around it.
        store.setFolderCollapsed(made, true);
        store.renameFolder(made, QStringLiteral("Soon"));
        store.setTopLevelOrder({ QStringLiteral("!a:x"), made });
        QCOMPARE(folderIds(store), QStringList{ made });

        // And across a reload: the layout on disk still has it.
        RailLayoutStore reopened(&settings);
        QCOMPARE(folderIds(reopened), QStringList{ made });
    }

    void aFolderWhoseSpacesHaveNotResolvedYetIsNeverDeleted()
    {
        // THE TRAP. `arrange()` shows the stored ids INTERSECTED with the
        // Spaces the account knows about, and that intersection is empty for
        // every folder during a cold start. Judging emptiness there would
        // delete the whole arrangement before the first sync landed.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setSpaceFolder(QStringLiteral("!b:x"), work);

        // Nothing has synced: the rail draws the folder with no members.
        const QVariantList arranged = store.arrange(withPseudo({}));
        QCOMPARE(idsOf(arranged),
                 (QStringList{ QString(), QStringLiteral("[Work]") }));

        // A write happens anyway (the user collapses it, or drags a Space
        // that has arrived). The folder still holds both ids.
        store.setFolderCollapsed(work, true);
        store.setTopLevelOrder({ work, QStringLiteral("!z:x") });
        QCOMPARE(store.folderMembers(work),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));
        QCOMPARE(folderIds(store), QStringList{ work });
    }

    void anArrangementThatEmptiesAnOpenFolderDeletesIt()
    {
        // The atomic write a finished drag produces: the folder is RENDERED
        // (so the caller may speak for it) and the drag took its only member
        // to the top level.
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setTopLevelOrder({ work, QStringLiteral("!z:x") });

        store.applyArrangement({ work, QStringLiteral("!a:x"),
                                 QStringLiteral("!z:x") },
                               { { work, QStringList{} } });
        QVERIFY(folderIds(store).isEmpty());
        QCOMPARE(store.order(),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!z:x") }));
    }

    // ── The defect that produced empty folders ───────────────────────────

    void aCollapsedFolderSurvivesAReorderDragThatNeverShowedIt()
    {
        // FAILS ON THE UNFIXED TREE. commitReorder named the collapsed
        // folder in the arrangement with an empty member list — the rail
        // cannot render a collapsed folder's members, so nothing could ever
        // fill that list — and applyArrangement takes a named folder's list
        // as the whole truth. The folder was emptied by a drag that never
        // touched it, and with the deletion rule above it would be gone.
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
            spaceRoom(QStringLiteral("!b:x"), QStringLiteral("B")),
            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")),
            spaceRoom(QStringLiteral("!p:x"), QStringLiteral("P")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString shut = store.createFolder(QStringLiteral("Shut"));
        store.setSpaceFolder(QStringLiteral("!a:x"), shut);
        store.setSpaceFolder(QStringLiteral("!b:x"), shut);
        store.setFolderCollapsed(shut, true);
        store.setTopLevelOrder({ shut, QStringLiteral("!z:x"),
                                 QStringLiteral("!p:x") });

        RailEntryModel model;
        model.setSources(&spaces, &store);
        // [All rooms][Shut][z][p] — the folder is collapsed, so its members
        // have no rows at all. That is the whole hazard.
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ shut, QStringLiteral("!z:x"),
                               QStringLiteral("!p:x") }));

        // A drag that has nothing to do with the folder: z moves past p.
        QVERIFY(model.beginDrag(QStringLiteral("!z:x")));
        model.hoverGap(model.rowCount());
        model.endDrag(true);

        QCOMPARE(store.folderMembers(shut),
                 (QStringList{ QStringLiteral("!a:x"), QStringLiteral("!b:x") }));
        QVERIFY2(folderIds(store).contains(shut),
                 "a drag past a collapsed folder emptied it, and the empty "
                 "folder was then deleted");
        // The reorder the user actually asked for still happened.
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ shut, QStringLiteral("!p:x"),
                               QStringLiteral("!z:x") }));
    }

    void anOpenFolderKeepsAMemberThatHasNotResolvedYet()
    {
        // The same defect in its quiet costume: an OPEN folder holding one
        // Space that exists and one that has not synced renders a single
        // member row, so an arrangement that spoke for the folder with only
        // the rendered ids destroyed the other one.
        FakeClient client;
        client.roomList = {
            spaceRoom(QStringLiteral("!a:x"), QStringLiteral("A")),
            spaceRoom(QStringLiteral("!z:x"), QStringLiteral("Z")),
        };
        SpaceManager spaces;
        spaces.setClient(&client);
        SettingsManager settings;
        RailLayoutStore store(&settings);
        const QString work = store.createFolder(QStringLiteral("Work"));
        store.setSpaceFolder(QStringLiteral("!a:x"), work);
        store.setSpaceFolder(QStringLiteral("!unsynced:x"), work);
        store.setTopLevelOrder({ work, QStringLiteral("!z:x") });

        RailEntryModel model;
        model.setSources(&spaces, &store);
        QCOMPARE(modelIds(model).mid(1),
                 (QStringList{ work, QStringLiteral("!a:x"),
                               QStringLiteral("!z:x") }));

        // Drag the one member that exists out to the end of the rail.
        QVERIFY(model.beginDrag(QStringLiteral("!a:x")));
        model.hoverGap(model.rowCount());
        model.endDrag(true);

        QVERIFY(store.folderOf(QStringLiteral("!a:x")).isEmpty());
        QCOMPARE(store.folderMembers(work),
                 QStringList{ QStringLiteral("!unsynced:x") });
        QVERIFY2(folderIds(store).contains(work),
                 "a folder still holding an unsynced Space was deleted as "
                 "though it were empty");
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(RailFolderLifecycleTest)
#include "RailFolderLifecycleTest.moc"
