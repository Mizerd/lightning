// The Spaces rail's own arrangement: drag order and folders.
//
// All of it is pure over a model snapshot, which is the point — the rail can
// be rearranged, filed and reloaded without a homeserver, a ListView or a
// gesture. What these cases pin is the behaviour a user would notice: a new
// Space does not barge into a hand-made order, a folder that empties does not
// vanish, deleting a folder puts its Spaces back where the folder was, and a
// pseudo row can never be dragged or filed.

#include "app/SettingsManager.h"
#include "spaces/RailLayoutStore.h"

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

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(RailLayoutTest)
#include "RailLayoutTest.moc"
