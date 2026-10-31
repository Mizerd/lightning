// The status strip's message (B053, B139). It used to stay until another
// message replaced it: "The room was created, but adding it to the Space
// failed." and "The edit could not be applied." were still on screen minutes
// later, across rooms and Settings. Now:
//   * it can be dismissed;
//   * moving to another room or Space withdraws it, except the landing of
//     the operation that reported it (a created room opening);
//   * it expires on its own, later for a long message;
//   * a new create or join withdraws it, but not the create's own wait for
//     its room;
//   * a running call keeps only the call lane's own notice.
// Boots a real AppController on the mock backend; the strip is Main.qml's
// mirror of errorReported(), so the signal is what is asserted. No
// credentials or network.

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "calls/CallController.h"
#include "calls/SfuCallController.h"
#include "matrix/MockMatrixClient.h"
#include "spaces/SpaceManager.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// The mock with the two things it does not do: create a room (answered by
// the case, as the SDK would, with the room listed once `listed` is set)
// and place a legacy call (so a call can be running offline).
class StripClient final : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    quint64 lastOpId = 0;
    QList<RoomInfo> listed;

    quint64 createRoom(const QVariantMap &) override { return ++lastOpId; }
    QList<RoomInfo> rooms() const override
    {
        return MockMatrixClient::rooms() + listed;
    }
    bool supportsCallSignaling() const override { return true; }
    quint64 callInvite(const QString &, const QString &, const QString &,
                       const QString &, const QString &, quint64,
                       const QString &, const QString &) override
    {
        return ++lastOpId;
    }
};

} // namespace

class StatusStripTest : public QObject
{
    Q_OBJECT

private:
    // What the strip shows now: the last errorReported(), "" before any.
    static QString shown(const QSignalSpy &spy)
    {
        return spy.isEmpty() ? QString()
                             : spy.constLast().at(0).toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("status-strip-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void theDismissControlWithdrawsTheMessage()
    {
        AppController app(AppController::MockBackend);
        QSignalSpy reported(&app, &AppController::errorReported);
        Q_EMIT app.errorReported(QStringLiteral("The edit could not be applied."));
        app.dismissStatusError();
        QCOMPARE(shown(reported), QString());
        // Nothing shown: a second dismiss reports nothing.
        const int count = reported.count();
        app.dismissStatusError();
        QCOMPARE(reported.count(), count);
    }

    void movingToAnotherRoomOrSpaceWithdrawsIt()
    {
        AppController app(AppController::MockBackend);
        QSignalSpy reported(&app, &AppController::errorReported);

        Q_EMIT app.errorReported(QStringLiteral("The edit could not be applied."));
        app.openRoom(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(shown(reported), QString());

        Q_EMIT app.errorReported(QStringLiteral("Something failed."));
        app.spaces()->setActiveSpaceId(SpaceManager::orphansId());
        QCOMPARE(shown(reported), QString());
    }

    // The create's own landing is not the user moving on: the message it
    // reported on the way is what the user needs to see in that room.
    void aCreatedRoomOpeningKeepsTheMessageItReported()
    {
        AppController app(AppController::MockBackend);
        QSignalSpy reported(&app, &AppController::errorReported);
        const QString placement = QStringLiteral(
            "The room was created, but adding it to the Space failed.");
        Q_EMIT app.errorReported(placement);
        Q_EMIT app.conversations()->conversationReady(
            QStringLiteral("!created:example.org"));
        QCOMPARE(app.currentRoomId(), QStringLiteral("!created:example.org"));
        QCOMPARE(shown(reported), placement);
        Q_EMIT app.conversations()->spaceReady(
            QStringLiteral("!space-created:example.org"));
        QCOMPARE(shown(reported), placement);
        // The next move is the user's.
        app.openRoom(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(shown(reported), QString());
    }

    // B053 through the real create: ConversationController reports the
    // Space failure, THEN goes busy again to wait for the room, THEN opens
    // it. The busy hook used to withdraw the message on that second busy,
    // before a frame was drawn, so the fix made it invisible.
    void aCreatesSpaceFailureSurvivesItsWaitAndLanding()
    {
        StripClient client;
        AppController app(AppController::MockBackend);
        app.conversations()->setClient(&client);
        QSignalSpy reported(&app, &AppController::errorReported);

        // A new create withdraws the last attempt's message.
        Q_EMIT app.errorReported(QStringLiteral("Old message."));
        app.conversations()->createRoom(
            {{QStringLiteral("name"), QStringLiteral("Room")}});
        QVERIFY(app.conversations()->busy());
        QCOMPARE(shown(reported), QString());

        const QString room = QStringLiteral("!created:example.org");
        Q_EMIT client.roomCreateFinished(client.lastOpId, true, room,
                                         QString(),
                                         QStringLiteral("space_add_failed"));
        const QString placement = QStringLiteral(
            "The room was created, but adding it to the Space failed.");
        // Waiting for the room to be listed is the same create.
        QVERIFY(app.conversations()->busy());
        QCOMPARE(shown(reported), placement);

        RoomInfo info;
        info.id = room;
        info.name = room;
        client.listed = {info};
        Q_EMIT client.roomsChanged();
        QVERIFY(!app.conversations()->busy());
        QCOMPARE(app.currentRoomId(), room);
        QCOMPARE(shown(reported), placement);
    }

    // A running call keeps ITS notice ("nobody hears you") on the strip
    // across the timer and room switches; any other message shown during
    // the call expires and goes on a switch as it would without one.
    void aRunningCallKeepsOnlyItsOwnNotice()
    {
        StripClient client;
        AppController app(AppController::MockBackend);
        app.setStatusErrorExpiryMsForTest(300);
        app.calls()->setClient(&client);
        QVERIFY(app.calls()->placeCallWithOffer(
            QStringLiteral("!call:example.org"), QStringLiteral("v=0 sdp")));
        QVERIFY(app.calls()->sessionLive());
        QSignalSpy reported(&app, &AppController::errorReported);

        const QString notice =
            QStringLiteral("Nobody in the call can hear you.");
        Q_EMIT app.groupCall()->callFailed(notice);
        QCOMPARE(shown(reported), notice);
        QTest::qWait(1000);
        QCOMPARE(shown(reported), notice);
        app.openRoom(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(shown(reported), notice);

        // Not the call's: it expires during the call...
        Q_EMIT app.errorReported(
            QStringLiteral("The edit could not be applied."));
        QTRY_COMPARE_WITH_TIMEOUT(shown(reported), QString(), 3000);
        // ...and goes on a room switch.
        app.setStatusErrorExpiryMsForTest(60000);
        Q_EMIT app.errorReported(QStringLiteral("Something failed."));
        app.openRoom(QStringLiteral("!other:example.org"));
        QCOMPARE(shown(reported), QString());
        app.calls()->hangup();
    }

    // A long message (the keyring ones run to several sentences) is not
    // gone before it can be read; a short one keeps the 8 s.
    void aLongMessageStaysLonger()
    {
        QCOMPARE(AppController::statusErrorExpiryMs(0), 8000);
        QCOMPARE(AppController::statusErrorExpiryMs(40), 8000);
        QCOMPARE(AppController::statusErrorExpiryMs(300), 22000);
        QCOMPARE(AppController::statusErrorExpiryMs(5000), 30000);
    }

    void theMessageExpiresOnItsOwn()
    {
        AppController app(AppController::MockBackend);
        QSignalSpy reported(&app, &AppController::errorReported);
        Q_EMIT app.errorReported(QStringLiteral("First."));
        // A newer message restarts the clock: the first one's timer must not
        // take the second down early.
        QTest::qWait(3000);
        Q_EMIT app.errorReported(QStringLiteral("Second."));
        QTest::qWait(6000);
        QCOMPARE(shown(reported), QStringLiteral("Second."));
        QTRY_COMPARE_WITH_TIMEOUT(shown(reported), QString(), 6000);
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(StatusStripTest)
#include "StatusStripTest.moc"
