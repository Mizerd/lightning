// Development-only screenshot-demo dataset contract.
//
// MockMatrixClient::setScreenshotDemoMode(true) serves a richer, fully
// deterministic scene for promotional screenshots. This locks its structure
// (Spaces, required room types, a poll, a thread), its determinism (stable
// event ids + fixed timestamps across launches), and its safety (only
// fictional *.example identities — no real homeserver, no mock.local, no
// network). Tests never enable this mode elsewhere, so the shared fixtures the
// other mock tests assert on stay unchanged.
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QtTest/QtTest>

class ScreenshotDemoDataTest : public QObject
{
    Q_OBJECT

    static RoomInfo roomById(const QList<RoomInfo> &rooms, const QString &id)
    {
        for (const auto &r : rooms)
            if (r.id == id)
                return r;
        return {};
    }

private Q_SLOTS:
    void demoModeIsOffByDefault()
    {
        MockMatrixClient c;
        QVERIFY(!c.screenshotDemoMode());
        // The shared fixtures (used by every other mock test) are the compact
        // mock.local scene until demo mode is explicitly enabled.
        bool sawMockLocal = false;
        for (const auto &r : c.rooms())
            if (r.id.contains(QStringLiteral("mock.local")))
                sawMockLocal = true;
        QVERIFY(sawMockLocal);
    }

    void demoModeSeedsThreeFictionalSpaces()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        QVERIFY(c.screenshotDemoMode());
        QStringList spaceNames;
        for (const auto &r : c.rooms())
            if (r.isSpace)
                spaceNames << r.name;
        spaceNames.sort();
        QCOMPARE(spaceNames, (QStringList{ QStringLiteral("Creative Studio"),
                                           QStringLiteral("Friends"),
                                           QStringLiteral("Lightning Community") }));
    }

    void demoRoomsCoverRequiredTypes()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const auto rooms = c.rooms();

        // Encrypted 1:1 DM.
        const RoomInfo dm = roomById(rooms, QStringLiteral("!dm-maya:lightning.example"));
        QVERIFY(dm.isDirect);
        QVERIFY(dm.encrypted);
        QCOMPARE(dm.directUserId, QStringLiteral("@maya:lightning.example"));

        // A pending invite with an inviter.
        const RoomInfo invite =
            roomById(rooms, QStringLiteral("!invite-founders:lightning.example"));
        QCOMPARE(invite.membership, RoomInfo::Invited);
        QVERIFY(invite.invitePending);
        QVERIFY(!invite.inviterDisplayName.isEmpty());

        // Unread + mention states for the room list.
        const RoomInfo design =
            roomById(rooms, QStringLiteral("!design-lounge:lightning.example"));
        QVERIFY(design.unreadCount > 0);
        QVERIFY(design.highlightCount > 0);
        QVERIFY(!design.typingUserIds.isEmpty());

        // Every non-space room belongs to a Space or is a DM/invite; no room
        // shows a raw id as its name.
        for (const auto &r : rooms)
            QVERIFY2(!r.name.startsWith(QLatin1Char('!')),
                     qUtf8Printable("room name looks like a raw id: " + r.name));
    }

    void pollScenarioHasFourOptionsWithVotes()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const auto tl = c.timeline(QStringLiteral("!feedback:lightning.example"));
        int polls = 0;
        for (const auto &e : tl) {
            if (e.type != TimelineEvent::Poll)
                continue;
            ++polls;
            QCOMPARE(e.pollAnswers.size(), 4);
            QVERIFY(e.pollTotalVoters > 0);
            int mine = 0, total = 0;
            for (const auto &a : e.pollAnswers) { total += a.count; if (a.byMe) ++mine; }
            QVERIFY(total > 0);
            QCOMPARE(mine, 1);   // exactly one selected option
        }
        QCOMPARE(polls, 1);
    }

    void devRoomHasThreadRootAndReplies()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString dev = QStringLiteral("!dev:lightning.example");
        const auto tl = c.timeline(dev);
        QString rootId;
        int replyCount = 0;
        for (const auto &e : tl) {
            if (e.isThreadRoot) {
                rootId = e.eventId;
                QCOMPARE(e.threadReplyCount, 4);
            }
        }
        QVERIFY(!rootId.isEmpty());
        for (const auto &e : tl)
            if (e.threadRootId == rootId)
                ++replyCount;
        QCOMPARE(replyCount, 4);

        // The real thread path rebuilds root + replies from the room timeline.
        QSignalSpy reset(&c, &MockMatrixClient::timelineReset);
        c.openThread(dev, rootId);
        QVERIFY(reset.count() >= 1);
    }

    void timestampsAndEventIdsAreDeterministic()
    {
        MockMatrixClient a, b;
        a.setScreenshotDemoMode(true);
        b.setScreenshotDemoMode(true);
        const QString room = QStringLiteral("!design-lounge:lightning.example");
        const auto ta = a.timeline(room);
        const auto tb = b.timeline(room);
        QVERIFY(!ta.isEmpty());
        QCOMPARE(ta.size(), tb.size());
        for (int i = 0; i < ta.size(); ++i) {
            QCOMPARE(ta[i].eventId, tb[i].eventId);       // stable ids
            QCOMPARE(ta[i].timestamp, tb[i].timestamp);   // fixed clock
        }
        // Anchored to the deterministic demo clock (2026-07-23), never now().
        QCOMPARE(ta.first().timestamp.date(), QDate(2026, 7, 23));
    }

    void onlyFictionalDomainsAppear()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const auto rooms = c.rooms();
        auto safe = [](const QString &s) {
            return !s.contains(QStringLiteral("smetonis"))
                && !s.contains(QStringLiteral("matrix.org"))
                && !s.contains(QStringLiteral("mock.local"))
                && !s.contains(QStringLiteral("bankera"));
        };
        for (const auto &r : rooms) {
            QVERIFY2(safe(r.id), qUtf8Printable("unsafe room id: " + r.id));
            for (const auto &e : c.timeline(r.id)) {
                QVERIFY2(safe(e.sender), qUtf8Printable("unsafe sender: " + e.sender));
                QVERIFY2(safe(e.roomId), qUtf8Printable("unsafe roomId: " + e.roomId));
            }
        }
    }
};

QTEST_GUILESS_MAIN(ScreenshotDemoDataTest)
#include "ScreenshotDemoDataTest.moc"
