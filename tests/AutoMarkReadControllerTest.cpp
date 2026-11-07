#include "app/AutoMarkReadController.h"
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

#include <QtTest/QtTest>

class AutoReadClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override {}
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return user; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
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
    bool supportsMarkRoomRead() const override { return true; }
    QList<RoomInfo> rooms() const override
    {
        ++roomsCalls;
        return rows;
    }
    RoomInfo roomInfo(const QString &id) const override
    {
        for (const RoomInfo &r : rows) {
            if (r.id == id)
                return r;
        }
        return {};
    }
    void markRoomRead(const QString &id) override { readRequests.append(id); }

    QString user = QStringLiteral("@me:x");
    QList<RoomInfo> rows;
    QStringList readRequests;
    mutable int roomsCalls = 0;
};

namespace {

RoomInfo room(const QString &id, bool unread)
{
    RoomInfo r;
    r.id = id;
    r.name = id;
    r.membership = RoomInfo::Joined;
    r.hasUnreadMessages = unread;
    return r;
}

const QString A = QStringLiteral("!a:x");
const QString B = QStringLiteral("!b:x");
const QString C = QStringLiteral("!c:x");

struct Fixture {
    AutoReadClient client;
    RoomListModel rooms;
    AutoMarkReadController controller;
    QStringList open;

    explicit Fixture(int minutes)
    {
        client.rows = { room(A, true), room(B, true), room(C, false) };
        rooms.setClient(&client);
        controller.setSources(&client, &rooms);
        controller.setExcludedRooms([this] { return open; });
        controller.setIntervalMinutes(minutes);
        controller.setIntervalMsForTest(40);
        controller.setAccount(client.user);
    }
};

}

class AutoMarkReadControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void offNeverArmsAndNeverMarks()
    {
        Fixture f(0);
        QVERIFY(!f.controller.armed());
        QTest::qWait(250);
        QVERIFY(f.client.readRequests.isEmpty());
    }

    void theIntervalIsTheChosenMinutes()
    {
        AutoMarkReadController controller;
        controller.setIntervalMinutes(15);
        controller.setAccount(QStringLiteral("@me:x"));
        QVERIFY(controller.armed());
        QCOMPARE(controller.intervalMs(), 15 * 60000);
        controller.setIntervalMinutes(90);
        QCOMPARE(controller.intervalMs(), 90 * 60000);
    }

    void aPassWaitsForTheIntervalAndSkipsTheOpenRoom()
    {
        Fixture f(15);
        f.open = { B };
        f.controller.setIntervalMsForTest(400);
        QTest::qWait(100);
        QVERIFY2(f.client.readRequests.isEmpty(), "a pass ran before the interval");
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTest::qWait(150);
        QVERIFY2(!f.client.readRequests.contains(B), "the open room was marked read");
        QVERIFY(!f.client.readRequests.contains(C));
    }

    void oneRoomIsInFlightAtATimeAndAPassNeverOverlapsTheNext()
    {
        Fixture f(15);
        QSignalSpy passes(&f.controller, &AutoMarkReadController::passStarted);
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        QTest::qWait(200);
        QCOMPARE(f.client.readRequests, QStringList{ A });
        QCOMPARE(passes.count(), 1);
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTRY_COMPARE(f.client.readRequests, (QStringList{ A, B }));
    }

    void anAccountSwitchAbandonsThePassAndArmsNothingForTheOldClient()
    {
        Fixture f(15);
        QSignalSpy passes(&f.controller, &AutoMarkReadController::passStarted);
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        f.controller.setAccount(QStringLiteral("@bob:x"));
        QVERIFY(!f.controller.passRunning());
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTest::qWait(250);
        QCOMPARE(f.client.readRequests, QStringList{ A });
        QCOMPARE(passes.count(), 1);
    }

    void signingOutStopsTheTimer()
    {
        Fixture f(15);
        QVERIFY(f.controller.armed());
        Q_EMIT f.client.loggedOut();
        QVERIFY(!f.controller.armed());
        QTest::qWait(250);
        QVERIFY(f.client.readRequests.isEmpty());
    }

    void turningItOffMidPassStopsThePass()
    {
        Fixture f(15);
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        f.controller.setIntervalMinutes(0);
        QVERIFY(!f.controller.armed());
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTest::qWait(250);
        QCOMPARE(f.client.readRequests, QStringList{ A });
    }

    void aTimedOutStepHoldsThePassThenGivesItUp()
    {
        Fixture f(15);
        f.controller.setStepTimeoutMsForTest(80);
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        f.controller.setIntervalMsForTest(600000);
        QTest::qWait(120);
        QCOMPARE(f.client.readRequests, QStringList{ A });
        QVERIFY(f.controller.passRunning());
        QTRY_VERIFY(!f.controller.passRunning());
        QCOMPARE(f.client.readRequests, QStringList{ A });
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTest::qWait(60);
        QCOMPARE(f.client.readRequests, QStringList{ A });
    }

    void anOverdueStepThatSettlesLetsThePassGoOn()
    {
        Fixture f(15);
        f.controller.setStepTimeoutMsForTest(80);
        QTRY_COMPARE(f.client.readRequests, QStringList{ A });
        f.controller.setIntervalMsForTest(600000);
        QTest::qWait(120);
        QCOMPARE(f.client.readRequests, QStringList{ A });
        Q_EMIT f.client.readMarkerAdvanced(A);
        QTRY_COMPARE(f.client.readRequests, (QStringList{ A, B }));
    }

    void aPassReadsTheRoomListOnceNotOncePerRoom()
    {
        Fixture f(15);
        f.controller.setIntervalMinutes(0);
        f.client.rows.clear();
        for (int i = 0; i < 30; ++i)
            f.client.rows.append(room(QStringLiteral("!r%1:x").arg(i), true));
        f.controller.setIntervalMinutes(15);
        f.controller.setIntervalMsForTest(40);
        QTRY_COMPARE(f.client.readRequests.size(), 1);
        f.controller.setIntervalMsForTest(600000);
        const int before = f.client.roomsCalls;
        for (int i = 0; i < 30; ++i) {
            const QString id = f.client.readRequests.last();
            Q_EMIT f.client.readMarkerAdvanced(id);
            if (i < 29)
                QTRY_COMPARE(f.client.readRequests.size(), i + 2);
        }
        QTRY_VERIFY(!f.controller.passRunning());
        QVERIFY2(f.client.roomsCalls - before < 5,
                 qPrintable(QStringLiteral("rooms() read %1 times in one pass")
                                .arg(f.client.roomsCalls - before)));
    }

    void aMarkedUnreadRoomIsLeftAlone()
    {
        Fixture f(15);
        f.client.rows[0].markedUnread = true;
        QTRY_COMPARE(f.client.readRequests, QStringList{ B });
    }
};

QTEST_GUILESS_MAIN(AutoMarkReadControllerTest)
#include "AutoMarkReadControllerTest.moc"
