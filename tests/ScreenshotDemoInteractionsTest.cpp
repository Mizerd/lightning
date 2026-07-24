// Development-only screenshot-demo local-interactions contract.
//
// Locks that the demo scene supports local-only interactions through the real
// backend interface — poll vote change, invite accept/reject, mark unread,
// reactions and message send — with no network, and that reset restores the
// deterministic state. These are all gated on demo mode, so the shared mock
// fixtures (and every other backend) are unchanged.

#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class ScreenshotDemoInteractionsTest : public QObject
{
    Q_OBJECT

    static QString findPoll(const MockMatrixClient &c, const QString &room,
                            QString *midnightId = nullptr, QString *oceanId = nullptr)
    {
        for (const TimelineEvent &e : c.timeline(room)) {
            if (e.type != TimelineEvent::Poll)
                continue;
            for (const PollAnswer &a : e.pollAnswers) {
                if (a.text == QLatin1String("Midnight") && midnightId) *midnightId = a.id;
                if (a.text == QLatin1String("Ocean") && oceanId) *oceanId = a.id;
            }
            return e.eventId;
        }
        return {};
    }
    static RoomInfo room(const MockMatrixClient &c, const QString &id)
    {
        for (const RoomInfo &r : c.rooms())
            if (r.id == id)
                return r;
        return {};
    }

private Q_SLOTS:
    void pollsSupportedOnlyInDemoMode()
    {
        MockMatrixClient plain;
        QVERIFY(!plain.supportsPolls());
        MockMatrixClient demo;
        demo.setScreenshotDemoMode(true);
        QVERIFY(demo.supportsPolls());
    }

    void pollVoteChangesTallyLocally()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString feedback = QStringLiteral("!feedback:lightning.example");
        QString midnightId, oceanId;
        const QString pollId = findPoll(c, feedback, &midnightId, &oceanId);
        QVERIFY(!pollId.isEmpty());
        QVERIFY(!oceanId.isEmpty());

        // Seed state: Midnight selected (4), Ocean (3).
        auto answer = [&](const QString &id) {
            for (const TimelineEvent &e : c.timeline(feedback))
                if (e.eventId == pollId)
                    for (const PollAnswer &a : e.pollAnswers)
                        if (a.id == id) return a;
            return PollAnswer{};
        };
        QVERIFY(answer(midnightId).byMe);
        QCOMPARE(answer(midnightId).count, 4);
        QCOMPARE(answer(oceanId).count, 3);

        // Change the vote to Ocean; a change signal fires and the tally moves.
        QSignalSpy spy(&c, &MatrixClient::eventChangedAt);
        c.sendPollResponse(feedback, QString(), pollId, { oceanId });
        QVERIFY(spy.count() >= 1);
        QVERIFY(!answer(midnightId).byMe);
        QCOMPARE(answer(midnightId).count, 3);
        QVERIFY(answer(oceanId).byMe);
        QCOMPARE(answer(oceanId).count, 4);

        // Reset restores the deterministic initial tally.
        c.resetDemoData();
        QVERIFY(answer(midnightId).byMe);
        QCOMPARE(answer(midnightId).count, 4);
    }

    void inviteAcceptJoinsLocally()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString invite = QStringLiteral("!invite-founders:lightning.example");
        QCOMPARE(room(c, invite).membership, RoomInfo::Invited);
        QVERIFY(room(c, invite).invitePending);

        c.acceptInvite(invite);
        QCOMPARE(room(c, invite).membership, RoomInfo::Joined);
        QVERIFY(!room(c, invite).invitePending);

        c.resetDemoData();
        QCOMPARE(room(c, invite).membership, RoomInfo::Invited);
    }

    void inviteRejectRemovesLocally()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString invite = QStringLiteral("!invite-founders:lightning.example");
        QVERIFY(!room(c, invite).id.isEmpty());
        c.rejectInvite(invite);
        QVERIFY(room(c, invite).id.isEmpty());   // gone from the room list
        c.resetDemoData();
        QVERIFY(!room(c, invite).id.isEmpty());   // restored
    }

    void markUnreadTogglesLocally()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString music = QStringLiteral("!music:lightning.example");
        QVERIFY(!room(c, music).markedUnread);
        c.setRoomMarkedUnread(music, true);
        QVERIFY(room(c, music).markedUnread);
        c.setRoomMarkedUnread(music, false);
        QVERIFY(!room(c, music).markedUnread);
    }

    void reactionsAndSendWorkLocally()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString design = QStringLiteral("!design-lounge:lightning.example");
        const int before = c.timeline(design).size();
        c.sendTextMessage(design, QStringLiteral("hi from the demo"));
        QCOMPARE(c.timeline(design).size(), before + 1);
        QCOMPARE(c.timeline(design).last().body,
                 QStringLiteral("hi from the demo"));

        // Reaction toggle on the first message updates its reactions.
        const QString target = c.timeline(design).first().eventId;
        QSignalSpy spy(&c, &MatrixClient::reactionsChanged);
        c.toggleReaction(design, target, QStringLiteral("🎯"));
        QVERIFY(spy.count() >= 1);
    }

    void interactionsAreNoOpsOutsideDemoMode()
    {
        MockMatrixClient c;   // not in demo mode
        // These must not touch the shared fixtures.
        c.acceptInvite(QStringLiteral("!whatever:mock.local"));
        c.setRoomMarkedUnread(QStringLiteral("!whatever:mock.local"), true);
        c.sendPollResponse(QStringLiteral("!whatever:mock.local"), QString(),
                           QStringLiteral("$x"), { QStringLiteral("a") });
        QVERIFY(!c.supportsPolls());   // still false
    }
};

QTEST_GUILESS_MAIN(ScreenshotDemoInteractionsTest)
#include "ScreenshotDemoInteractionsTest.moc"
