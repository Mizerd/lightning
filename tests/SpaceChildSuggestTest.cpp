// 2026-08-19 Element-parity round: SpaceManager::setSpaceChildSuggested
// plumbing — pending-op tracking against MatrixClient's
// spaceChildSuggestedFinished, foreign-op rejection, the refused-send
// (opId 0) immediate failure, and account isolation on logout. The wire
// behavior (state-event read-modify-send preserving via/order) is Rust
// and is not what this suite proves.
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

    // MatrixClient pure virtuals (inert).
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

// ---- Space Home lobby fixture (2026-09-23) --------------------------------
//
//   !home                      (the Space whose Home is open)
//     !general   joined room    topic from sync
//     !subA      joined Space   -> its own section
//       !a1  joined room
//       !a2  joined room, empty local topic -> /hierarchy's topic
//       !grand joined Space     -> a ROW in subA's section, never flattened
//         !g1 joined room
//       (!a3 unjoined, known only through subA's /hierarchy)
//     !announce  joined room
//     !subB      joined Space, no rooms yet -> an empty section
//     !offerSpace  unjoined Space (hierarchy only) -> a root ROW with Join
//     !offerRoom   unjoined room  (hierarchy only)
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
// A JOINED Space the Home no longer lists (just removed from it).
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
    // The SDK returns /hierarchy in SPEC order, which need not match the
    // local child list's; the root's rows are deliberately shuffled so the
    // test can tell which order the lobby used.
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
        // Known ONLY to /hierarchy: its m.space.child has not synced.
        hierRow(QStringLiteral("!late:example.org"), QStringLiteral("Late"),
                QString(), QString()),
        // /hierarchy says joined, sync has never heard of it: skipped.
        hierRow(QStringLiteral("!ghost:example.org"), QStringLiteral("Ghost"),
                QString(), QStringLiteral("joined")),
        // A stale answer naming a JOINED room the Home's synced state no
        // longer lists (just removed): not a child, skipped.
        hierRow(kA1, QStringLiteral("Off topic"), QString(),
                QStringLiteral("joined")),
        // ...and a stale answer naming a JOINED Space the Home no longer
        // lists: never a section (it would offer Remove on a non-child).
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

} // namespace

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
        QCOMPARE(done.count(), 0); // pending, not finished

        // A foreign op (another manager, another surface) must not be
        // reported as this call's outcome.
        client.finishSuggested(9999, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);

        client.finishSuggested(client.opCounter, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.at(0).at(0).toString(), kSpace);
        QCOMPARE(done.at(0).at(1).toString(), kRoom);
        QCOMPARE(done.at(0).at(2).toBool(), true);
        QCOMPARE(done.at(0).at(3).toBool(), true);

        // The op is consumed — a duplicate result is dropped.
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
        QCOMPARE(done.at(0).at(2).toBool(), false); // suggested arg echoed
        QCOMPARE(done.at(0).at(3).toBool(), false); // ok=false
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
        // A late result from the signed-out account's op must not report
        // into the next session (account isolation).
        client.finishSuggested(op, kSpace, kRoom, true, true);
        QCOMPARE(done.count(), 0);
    }

    // ---- The Space Home lobby (2026-09-23) --------------------------------

    // THE REPORT: "you cant tell which rooms belong to each space". The flat
    // list read childRoomsDetailed(), which is transitive.
    void lobbyGroupsEachRoomUnderItsOwnSpace()
    {
        const QVariantList secs = sections();
        QCOMPARE(ids(secs), (QStringList{ kHome, kSubA, kSubB }));
        const QVariantMap root = secs.at(0).toMap();
        QVERIFY(root.value(QStringLiteral("isRoot")).toBool());
        const QStringList rootRows =
            ids(root.value(QStringLiteral("rows")).toList());
        // A subspace's rooms are NOT in the root, and a joined subspace is a
        // section, never also a root row.
        QVERIFY2(!rootRows.contains(kA1), "a subspace room leaked into Rooms");
        QVERIFY(!rootRows.contains(kSubA));
        QVERIFY(!rootRows.contains(kSubB));
        const QVariantMap subA = secs.at(1).toMap();
        QVERIFY(!subA.value(QStringLiteral("isRoot")).toBool());
        QCOMPARE(subA.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Comunidade"));
        const QStringList aRows =
            ids(subA.value(QStringLiteral("rows")).toList());
        // One level of sections: the grandchild Space is a ROW, and its own
        // room is not flattened into this section. The cycle back to the
        // Home is dropped.
        QCOMPARE(aRows, (QStringList{ kA1, kA2, kGrand, kA3 }));
        QVERIFY(!aRows.contains(kG1));
        // An empty joined subspace still gets its section (it can be opened).
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
        // The Home's own m.space.child order, NOT /hierarchy's shuffled one;
        // a row only /hierarchy knows goes last; a /hierarchy "joined" row
        // sync never delivered is not drawn at all; nor is a JOINED room the
        // Home's state no longer lists (kA1 in a stale answer).
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
        // Joined: sync's topic wins over a stale /hierarchy copy...
        QCOMPARE(row[kGeneral].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Say hello"));
        QCOMPARE(row[kGeneral].value(QStringLiteral("members")).toLongLong(),
                 12);
        QVERIFY(row[kGeneral].value(QStringLiteral("joined")).toBool());
        QVERIFY(row[kGeneral].value(QStringLiteral("suggested")).toBool());
        // ...and /hierarchy fills a topic sync does not have.
        QCOMPARE(row[kA2].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Topic only the server has"));
        // Unjoined rooms show their topic too — from /hierarchy.
        QCOMPARE(row[kOfferRoom].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Visible before joining"));
        QVERIFY(!row[kOfferRoom].value(QStringLiteral("joined")).toBool());
        QCOMPARE(row[kOfferRoom].value(QStringLiteral("via")).toStringList(),
                 QStringList{ QStringLiteral("example.org") });
        QVERIFY(row[kOfferSpace].value(QStringLiteral("isSpace")).toBool());
        QCOMPARE(row[kA3].value(QStringLiteral("topic")).toString(),
                 QStringLiteral("Only via hierarchy"));
        // Only the Home's DIRECT children can be selected for its Remove /
        // Mark as suggested; a subspace's rooms belong to another Space.
        QVERIFY(row[kGeneral].value(QStringLiteral("selectable")).toBool());
        QVERIFY(row[kOfferRoom].value(QStringLiteral("selectable")).toBool());
        QVERIFY(!row[kA1].value(QStringLiteral("selectable")).toBool());
        QVERIFY(!row[kA3].value(QStringLiteral("selectable")).toBool());
        QCOMPARE(row[kA1].value(QStringLiteral("parentId")).toString(), kSubA);
        // A row only /hierarchy knows has no m.space.child in the Home for
        // Remove to change: not selectable.
        QVERIFY(!row[QStringLiteral("!late:example.org")]
                     .value(QStringLiteral("selectable")).toBool());
        // ONE unread rule: a count alone makes a row unread, and the section
        // says so too (it used to draw a folded total with no row badge).
        QVERIFY(row[kA1].value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(!row[kA2].value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("hasUnread")).toBool());
        QCOMPARE(secs.at(1).toMap().value(QStringLiteral("unreadTotal")).toInt(),
                 3);
        QVERIFY(!secs.at(2).toMap().value(QStringLiteral("hasUnread")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("selectable")).toBool());
        QVERIFY(secs.at(1).toMap().value(QStringLiteral("suggested")).toBool());
        // A nested Space row counts its own DIRECT joined rooms.
        QCOMPARE(row[kGrand].value(QStringLiteral("childCount")).toInt(), 1);
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
        // A topic-only match, case-insensitive, inside a subspace: only that
        // section survives and only that row in it.
        QVariantList secs = sections(QStringLiteral("  ONLY VIA "));
        QCOMPARE(ids(secs), QStringList{ kSubA });
        QCOMPARE(ids(secs.at(0).toMap().value(QStringLiteral("rows")).toList()),
                 QStringList{ kA3 });
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("matchCount")).toInt(),
                 1);
        // The counts are the section's, not the filter's.
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("roomCount")).toInt(),
                 3);
        // A subspace whose own NAME matches keeps every one of its rows.
        secs = sections(QStringLiteral("comunidade"));
        QCOMPARE(ids(secs), QStringList{ kSubA });
        QCOMPARE(secs.at(0).toMap().value(QStringLiteral("rows")).toList()
                     .size(),
                 4);
        // A name match in the root.
        secs = sections(QStringLiteral("announ"));
        QCOMPARE(ids(secs), QStringList{ kHome });
        QCOMPARE(ids(secs.at(0).toMap().value(QStringLiteral("rows")).toList()),
                 QStringList{ kAnnounce });
        // Nothing matches: no sections at all (the view says so).
        QVERIFY(sections(QStringLiteral("zzz-nothing")).isEmpty());
    }

    void lobbyCollapseHidesRowsButNotCountsAndSearchOverridesIt()
    {
        const QSet<QString> folded{ kSubA };
        QVariantMap subA = sections({}, folded).at(1).toMap();
        QVERIFY(subA.value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(subA.value(QStringLiteral("rows")).toList().size(), 0);
        QCOMPARE(subA.value(QStringLiteral("roomCount")).toInt(), 3);
        // A search never hides a match inside a folded section.
        const QVariantList secs = sections(QStringLiteral("memes"), folded);
        QCOMPARE(ids(secs), QStringList{ kSubA });
        subA = secs.at(0).toMap();
        QVERIFY(!subA.value(QStringLiteral("collapsed")).toBool());
        QCOMPARE(ids(subA.value(QStringLiteral("rows")).toList()),
                 QStringList{ kA2 });
    }

    void lobbyWithoutHierarchyStillListsJoinedRooms()
    {
        // Before /hierarchy answers (or on a backend without it) the lobby is
        // built from sync alone: no placeholders, no unjoined rows.
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
        mgr.setLobbySectionCollapsed(kHome, kSubA, true); // no-op
        QCOMPARE(changed.count(), 1);
        QVERIFY(mgr.lobbySectionCollapsed(kHome, kSubA));
        // Per Space: the same section id under another Home is independent.
        QVERIFY(!mgr.lobbySectionCollapsed(kSubA, kSubA));
        QVERIFY(mgr.lobbySections(kHome, lobbyHierarchy(), {}).at(1).toMap()
                    .value(QStringLiteral("rows")).toList().isEmpty());
        // Folded sections name one account's Spaces: gone on sign-out.
        Q_EMIT client.loggedOut();
        QVERIFY(!mgr.lobbySectionCollapsed(kHome, kSubA));
    }

    // Remove may only send into the Space the room is a DIRECT child of.
    // The lobby lets a manager select a subspace (a direct child that is a
    // Space) and an unjoined child; the old transitive pre-check reported
    // both "removed" without sending anything, and would have sent an
    // empty-via m.space.child into the Home for a SUBSPACE's room.
    void removeActsOnDirectChildrenOnly()
    {
        FakeClient client;
        client.fakeRooms = lobbyRooms();
        SpaceManager mgr;
        mgr.setClient(&client);
        QSignalSpy done(&mgr, &SpaceManager::childRemoveFinished);

        mgr.removeRoomFromSpace(kHome, kSubA);       // a child Space
        mgr.removeRoomFromSpace(kHome, kOfferRoom);  // an unjoined child
        mgr.removeRoomFromSpace(kHome, kGeneral);    // a joined room
        QCOMPARE(client.removals,
                 (QStringList{ kHome + QLatin1Char('|') + kSubA,
                               kHome + QLatin1Char('|') + kOfferRoom,
                               kHome + QLatin1Char('|') + kGeneral }));
        QCOMPARE(done.count(), 0); // all three pending on the backend

        // A SUBSPACE's room is not the Home's to remove: nothing is sent.
        mgr.removeRoomFromSpace(kHome, kA1);
        QCOMPARE(client.removals.size(), 3);
        QCOMPARE(done.count(), 1);
    }
};

QTEST_GUILESS_MAIN(SpaceChildSuggestTest)
#include "SpaceChildSuggestTest.moc"
