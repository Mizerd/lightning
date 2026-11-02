#include <QSignalSpy>
#include <QtTest>

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

namespace {

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
    QList<RoomInfo> rooms() const override { return fakeRooms; }
    QList<TimelineEvent> timeline(const QString &) const override
    { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &,
                   const QString &) override {}
    void editMessage(const QString &, const QString &,
                     const QString &) override {}
    void redactEvent(const QString &, const QString &,
                     const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    quint64 setSpaceChildSuggested(const QString &spaceId,
                                   const QString &roomId,
                                   bool suggested) override
    {
        lastSpaceId = spaceId;
        lastRoomId = roomId;
        lastSuggested = suggested;
        if (refuse)
            return 0;
        return ++opCounter;
    }
    quint64 removeRoomFromSpace(const QString &spaceId,
                                const QString &roomId) override
    {
        removals.append(spaceId + QLatin1Char('|') + roomId);
        return ++opCounter;
    }
    QStringList removals;

    void finishSuggested(quint64 opId, const QString &spaceId,
                         const QString &roomId, bool suggested, bool ok)
    {
        Q_EMIT spaceChildSuggestedFinished(opId, spaceId, roomId, suggested,
                                           ok);
    }

    QList<RoomInfo> fakeRooms;
    QString lastSpaceId;
    QString lastRoomId;
    bool lastSuggested = false;
    bool refuse = false;
    quint64 opCounter = 0;
};

const QString kSpace = QStringLiteral("!space:example.org");
const QString kRoom = QStringLiteral("!room:example.org");

const QString kHome = QStringLiteral("!home:example.org");
const QString kGeneral = QStringLiteral("!general:example.org");
const QString kSubA = QStringLiteral("!subA:example.org");
const QString kA1 = QStringLiteral("!a1:example.org");
const QString kA2 = QStringLiteral("!a2:example.org");
const QString kA3 = QStringLiteral("!a3:example.org");
const QString kGrand = QStringLiteral("!grand:example.org");
const QString kG1 = QStringLiteral("!g1:example.org");
const QString kAnnounce = QStringLiteral("!announce:example.org");
const QString kSubB = QStringLiteral("!subB:example.org");
const QString kOfferSpace = QStringLiteral("!offerSpace:example.org");
const QString kOfferRoom = QStringLiteral("!offerRoom:example.org");
const QString kFormerSub = QStringLiteral("!formerSub:example.org");

RoomInfo lobbyRoom(const QString &id, const QString &name,
                   const QString &topic, bool space = false,
                   const QStringList &children = {})
{
    RoomInfo r;
    r.id = id;
    r.name = name;
    r.topic = topic;
    r.isSpace = space;
    r.childRoomIds = children;
    r.membership = RoomInfo::Joined;
    return r;
}

QList<RoomInfo> lobbyRooms()
{
    QList<RoomInfo> rooms{
        lobbyRoom(kHome, QStringLiteral("Home"), QStringLiteral("The Space"),
                  true,
                  { kGeneral, kSubA, kAnnounce, kSubB, kOfferSpace,
                    kOfferRoom }),
        lobbyRoom(kGeneral, QStringLiteral("General"),
                  QStringLiteral("Say hello")),
        lobbyRoom(kSubA, QStringLiteral("Comunidade"),
                  QStringLiteral("Community rooms"), true,
                  { kA1, kA2, kGrand, kHome }),
        lobbyRoom(kA1, QStringLiteral("Off topic"),
                  QStringLiteral("Anything goes")),
        lobbyRoom(kA2, QStringLiteral("Memes"), QString()),
        lobbyRoom(kGrand, QStringLiteral("Deep"), QString(), true, { kG1 }),
        lobbyRoom(kG1, QStringLiteral("Deep room"),
                  QStringLiteral("Grandchild")),
        lobbyRoom(kAnnounce, QStringLiteral("Announcements"),
                  QStringLiteral("Read only")),
        lobbyRoom(kSubB, QStringLiteral("Voz"), QString(), true, {}),
        lobbyRoom(kFormerSub, QStringLiteral("Former"), QString(), true, {}),
    };
    // A count with no "unread" flag: the case the mock produces.
    for (RoomInfo &r : rooms) {
        if (r.id == kA1)
            r.unreadCount = 3;
    }
    return rooms;
}

QVariantMap hierRow(const QString &id, const QString &name,
                    const QString &topic, const QString &membership,
                    bool space = false, bool suggested = false,
                    qlonglong members = 0)
{
    return QVariantMap{
        { QStringLiteral("roomId"), id },
        { QStringLiteral("name"), name },
        { QStringLiteral("topic"), topic },
        { QStringLiteral("avatarUrl"), QString() },
        { QStringLiteral("members"), members },
        { QStringLiteral("joinRule"), QStringLiteral("public") },
        { QStringLiteral("membership"), membership },
        { QStringLiteral("isSpace"), space },
        { QStringLiteral("childrenCount"), space ? 4 : 0 },
        { QStringLiteral("suggested"), suggested },
        { QStringLiteral("via"), QStringList{ QStringLiteral("example.org") } },
    };
}

QVariantMap lobbyHierarchy()
{
    const QVariantList home{
        hierRow(kOfferRoom, QStringLiteral("Unjoined room"),
                QStringLiteral("Visible before joining"),
                QStringLiteral("left"), false, true, 7),
        hierRow(kGeneral, QStringLiteral("General"), QStringLiteral("stale"),
                QStringLiteral("joined"), false, true, 12),
        hierRow(kSubA, QStringLiteral("Comunidade"), QString(),
                QStringLiteral("joined"), true, true),
        hierRow(kOfferSpace, QStringLiteral("Offered space"),
                QStringLiteral("A space to join"), QString(), true),
        hierRow(QStringLiteral("!late:example.org"), QStringLiteral("Late"),
                QString(), QString()),
        hierRow(QStringLiteral("!ghost:example.org"), QStringLiteral("Ghost"),
                QString(), QStringLiteral("joined")),
        hierRow(kA1, QStringLiteral("Off topic"), QString(),
                QStringLiteral("joined")),
        hierRow(kFormerSub, QStringLiteral("Former"), QString(),
                QStringLiteral("joined"), true),
    };
    const QVariantList subA{
        hierRow(kA2, QStringLiteral("Memes"),
                QStringLiteral("Topic only the server has"),
                QStringLiteral("joined")),
        hierRow(kA3, QStringLiteral("Voice lounge"),
                QStringLiteral("Only via hierarchy"), QStringLiteral("")),
    };
    return QVariantMap{ { kHome, home }, { kSubA, subA } };
}

QStringList ids(const QVariantList &rows)
{
    QStringList out;
    for (const QVariant &v : rows)
        out.append(v.toMap().value(QStringLiteral("roomId")).toString());
    return out;
}

QHash<QString, RoomInfo> byId(const QList<RoomInfo> &rooms)
{
    QHash<QString, RoomInfo> out;
    for (const RoomInfo &r : rooms)
        out.insert(r.id, r);
    return out;
}

QVariantList sections(const QString &filter = {},
                      const QSet<QString> &collapsed = {})
{
    return SpaceManager::buildLobbySections(kHome, byId(lobbyRooms()),
                                            lobbyHierarchy(), filter,
                                            collapsed);
}

}

class SpaceChildSuggestTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void forwardsAndReportsOnlyOwnOps()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, true);
        QCOMPARE(client.lastSpaceId, kSpace);
        QCOMPARE(client.lastRoomId, kRoom);
        QCOMPARE(client.lastSuggested, true);
        QCOMPARE(done.count(), 0);

        client.finishSuggested(9999, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);

        client.finishSuggested(client.opCounter, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), kSpace);
        QCOMPARE(done.at(0).at(1).toString(), kRoom);
        QCOMPARE(done.at(0).at(2).toBool(), true);
        QCOMPARE(done.at(0).at(3).toBool(), true);

        client.finishSuggested(client.opCounter, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 1);
    }

    void refusedSendFailsImmediately()
    {
        FakeClient client;
        client.refuse = true;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, false);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(2).toBool(), false);
        QCOMPARE(done.at(0).at(3).toBool(), false);
    }

    void emptyArgumentsAreIgnored()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);
        mgr.setSpaceChildSuggested(QString(), kRoom, true);
        mgr.setSpaceChildSuggested(kSpace, QString(), true);
        QCOMPARE(client.opCounter, quint64(0));
        QCOMPARE(done.count(), 0);
    }

    void logoutClearsPendingOps()
    {
        FakeClient client;
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childSuggestedFinished);

        mgr.setSpaceChildSuggested(kSpace, kRoom, true);
        const quint64 op = client.opCounter;
        Q_EMIT client.loggedOut();
        client.finishSuggested(op, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);
    }

    void lobbyGroupsEachRoomUnderItsOwnSpace()
    {
        const QVariantList secs = sections();
        QCOMPARE(ids(secs), (QStringList{ kHome, kSubA, kSubB }));
        const QVariantMap root = secs.at(0).toMap();
        QVERIFY(root.value(QStringLiteral("isRoot")).toBool());
        const QStringList rootRows =
            ids(root.value(QStringLiteral("rows")).toList());
        QVERIFY2(!rootRows.contains(kA1), "a subspace room leaked into Rooms");
        QVERIFY(!rootRows.contains(kSubA));
        QVERIFY(!rootRows.contains(kSubB));
        const QVariantMap subA = secs.at(1).toMap();
        QVERIFY(!subA.value(QStringLiteral("isRoot")).toBool());
        QCOMPARE(subA.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Comunidade"));
        const QStringList aRows =
            ids(subA.value(QStringLiteral("rows")).toList());
        QCOMPARE(aRows, (QStringList{ kA1, kA2, kGrand, kA3 }));
        QVERIFY(!aRows.contains(kG1));
        const QVariantMap subB = secs.at(2).toMap();
        QCOMPARE(subB.value(QStringLiteral("rows")).toList().size(), 0);
        QCOMPARE(subB.value(QStringLiteral("roomCount")).toInt(), 0);
    }

    void aSubspaceTheHomeNoLongerListsIsNoSection()
    {
        const QVariantList secs = sections();
        QVERIFY2(!ids(secs).contains(kFormerSub),
                 "a stale /hierarchy answer kept a removed subspace as a "
                 "(selectable) section");
        QCOMPARE(ids(secs), (QStringList{ kHome, kSubA, kSubB }));
    }

    void lobbyFollowsSpaceChildOrderThenHierarchy()
    {
        const QVariantMap root = sections().at(0).toMap();
        QCOMPARE(ids(root.value(QStringLiteral("rows")).toList()),
                 (QStringList{ kGeneral, kAnnounce, kOfferSpace, kOfferRoom,
                               QStringLiteral("!late:example.org") }));
    }

    void lobbyRowsCarryTopicsJoinStateAndSelectability()
    {
        const QVariantList secs = sections();
        QHash<QString, QVariantMap> row;
        for (const QVariant &s : secs)
            for (const QVariant &r :
                 s.toMap().value(QStringLiteral("rows")).toList())
                row.insert(r.toMap().value(QStringLiteral("roomId")).toString(),
                           r.toMap());
        QCOMPARE(row[kGeneral].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Say hello"));
        QCOMPARE(row[kGeneral].value(QStringLiteral("members")).toLongLong(),
                 12);
        QVERIFY(row[kGeneral].value(QStringLiteral("joined")).toBool());
        QVERIFY(row[kGeneral].value(QStringLiteral("suggested")).toBool());
        QCOMPARE(row[kA2].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Topic only the server has"));
        QCOMPARE(row[kOfferRoom].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Visible before joining"));
        QVERIFY(!row[kOfferRoom].value(QStringLiteral("joined")).toBool());
        QCOMPARE(row[kOfferRoom].value(QStringLiteral("via")).toStringList(),
                 QStringList{ QStringLiteral("example.org") });
        QVERIFY(row[kOfferSpace].value(QStringLiteral("isSpace")).toBool());
        QCOMPARE(row[kA3].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Only via hierarchy"));
        QVERIFY(row[kGeneral].value(QStringLiteral("selectable")).toBool());
        QVERIFY(row[kOfferRoom].value(QStringLiteral("selectable")).toBool());
        QVERIFY(!row[kA1].value(QStringLiteral("selectable")).toBool());
        QVERIFY(!row[kA3].value(QStringLiteral("selectable")).toBool());
        QCOMPARE(row[kA1].value(QStringLiteral("parentId")).toString(), kSubA);
        QVERIFY(!row[QStringLiteral("!late:example.org")]
                     .value(QStringLiteral("selectable")).toBool());
        QVERIFY(row[kA1].value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(!row[kA2].value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("hasUnread")).toBool());
        QCOMPARE(secs.at(1).toMap().value(QStringLiteral("unreadTotal")).toInt(),
                 3);
        QVERIFY(!secs.at(2).toMap().value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("selectable")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("suggested")).toBool());
        QCOMPARE(row[kGrand].value(QStringLiteral("childCount")).toInt(), 1);
    }

    void aMutedRoomIsSilentInTheLobbyRollup()
    {
        const auto muteA1 = [](const QString &id) { return id == kA1 ? 2 : 0; };
        const QVariantList secs = SpaceManager::buildLobbySections(
            kHome, byId(lobbyRooms()), lobbyHierarchy(), QString(), {}, muteA1);
        QVariantMap row;
        for (const QVariant &sec : secs)
            for (const QVariant &r : sec.toMap().value(QStringLiteral("rows")).toList())
                if (r.toMap().value(QStringLiteral("roomId")).toString() == kA1)
                    row = r.toMap();
        QVERIFY(!row.isEmpty());
        QVERIFY(!row.value(QStringLiteral("hasUnread")).toBool());
        QCOMPARE(row.value(QStringLiteral("unreadCount")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("highlightCount")).toInt(), 0);
        QVERIFY(!secs.at(1).toMap().value(QStringLiteral("hasUnread")).toBool());
        QCOMPARE(secs.at(1).toMap().value(QStringLiteral("unreadTotal")).toInt(), 0);
    }

    void lobbySectionCountsRoomsAndSpacesSeparately()
    {
        const QVariantList secs = sections();
        const QVariantMap root = secs.at(0).toMap();
        QCOMPARE(root.value(QStringLiteral("roomCount")).toInt(), 4);
        QCOMPARE(root.value(QStringLiteral("spaceCount")).toInt(), 1);
        const QVariantMap subA = secs.at(1).toMap();
        QCOMPARE(subA.value(QStringLiteral("roomCount")).toInt(), 3);
        QCOMPARE(subA.value(QStringLiteral("spaceCount")).toInt(), 1);
        QCOMPARE(subA.value(QStringLiteral("matchCount")).toInt(), 4);
    }

    void lobbySearchFiltersWithinSectionsAndDropsEmptyOnes()
    {
        QVariantList secs = sections(QStringLiteral("  ONLY VIA "));
        QCOMPARE(ids(secs), QStringList{ kSubA });
        QCOMPARE(ids(secs.at(0).toMap().value(QStringLiteral("rows")).toList()),
                 QStringList{ kA3 });
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("matchCount")).toInt(),
                 1);
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("roomCount")).toInt(),
                 3);
        secs = sections(QStringLiteral("comunidade"));
        QCOMPARE(ids(secs), QStringList{ kSubA });
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("rows")).toList()
                     .size(),
                 4);
        secs = sections(QStringLiteral("announ"));
        QCOMPARE(ids(secs), QStringList{ kHome });
        QCOMPARE(ids(secs.at(0).toMap().value(QStringLiteral("rows")).toList()),
                 QStringList{ kAnnounce });
        QVERIFY(sections(QStringLiteral("zzz-nothing")).isEmpty());
    }

    void lobbyCollapseHidesRowsButNotCountsAndSearchOverridesIt()
    {
        const QSet<QString> folded{ kSubA };
        QVariantMap subA = sections({}, folded).at(1).toMap();
        QVERIFY(subA.value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(subA.value(QStringLiteral("rows")).toList().size(), 0);
        QCOMPARE(subA.value(QStringLiteral("roomCount")).toInt(), 3);
        const QVariantList secs = sections(QStringLiteral("memes"), folded);
        QCOMPARE(ids(secs), QStringList{ kSubA });
        subA = secs.at(0).toMap();
        QVERIFY(!subA.value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(ids(subA.value(QStringLiteral("rows")).toList()),
                 QStringList{ kA2 });
    }

    void lobbyWithoutHierarchyStillListsJoinedRooms()
    {
        const QVariantList secs = SpaceManager::buildLobbySections(
            kHome, byId(lobbyRooms()), {}, {}, {});
        QCOMPARE(ids(secs), (QStringList{ kHome, kSubA, kSubB }));
        QCOMPARE(ids(secs.at(0).toMap().value(QStringLiteral("rows")).toList()),
                 (QStringList{ kGeneral, kAnnounce }));
        QVERIFY(!secs.at(0).toMap().value(QStringLiteral("rows")).toList()
                     .at(0).toMap().value(QStringLiteral("suggestedKnown"))
                     .toBool());
    }

    void lobbyManagerApiUsesTheClientAndSessionCollapseState()
    {
        FakeClient client;
        client.fakeRooms = lobbyRooms();
        SpaceManager mgr;
        mgr.setClient(&client);
        QCOMPARE(mgr.lobbySubspaceIds(kHome), (QStringList{ kSubA, kSubB }));
        QCOMPARE(ids(mgr.lobbySections(kHome, lobbyHierarchy(), {})),
                 (QStringList{ kHome, kSubA, kSubB }));

        QSignalSpy changed(&mgr, &SpaceManager::lobbyCollapseChanged);
        mgr.setLobbySectionCollapsed(kHome, kSubA, true);
        mgr.setLobbySectionCollapsed(kHome, kSubA, true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(mgr.lobbySectionCollapsed(kHome, kSubA));
        QVERIFY(!mgr.lobbySectionCollapsed(kSubA, kSubA));
        QVERIFY(mgr.lobbySections(kHome, lobbyHierarchy(), {}).at(1).toMap()
                    .value(QStringLiteral("rows")).toList().isEmpty());
        Q_EMIT client.loggedOut();
        QVERIFY(!mgr.lobbySectionCollapsed(kHome, kSubA));
    }

    void removeActsOnDirectChildrenOnly()
    {
        FakeClient client;
        client.fakeRooms = lobbyRooms();
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childRemoveFinished);

        mgr.removeRoomFromSpace(kHome, kSubA);
        mgr.removeRoomFromSpace(kHome, kOfferRoom);
        mgr.removeRoomFromSpace(kHome, kGeneral);
        QCOMPARE(client.removals,
                 (QStringList{ kHome + QLatin1Char('|') + kSubA,
                               kHome + QLatin1Char('|') + kOfferRoom,
                               kHome + QLatin1Char('|') + kGeneral }));
        QCOMPARE(done.count(), 0);

        mgr.removeRoomFromSpace(kHome, kA1);
        QCOMPARE(client.removals.size(), 3);
        QCOMPARE(done.count(), 1);
    }
};

QTEST_GUILESS_MAIN(SpaceChildSuggestTest)
#include "SpaceChildSuggestTest.moc"
