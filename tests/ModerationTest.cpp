// v0.7.x personal moderation — ModerationController policy (the account's
// m.ignored_user_list cache and message reporting) against the scriptable
// MockMatrixClient surface, plus the pure notification guard. Pins:
//   * ignore/unignore round-trip: the terminal signal, the cached list,
//     isIgnored() and the bump-on-change revision;
//   * self-ignore is refused synchronously (the backend returns no op) and
//     reports a failure rather than dispatching or hanging;
//   * a remote change (another client) lands through the sync push, and an
//     identical pushed list is deduplicated — no phantom revision bump;
//   * the report prompt opens/submits/cancels correctly, failure categories
//     surface honestly, and reporting is single-flight;
//   * NotificationManager::decide short-circuits an ignored sender — the
//     local belt-and-braces for the race window before the server applies
//     the ignore and stops delivering their events;
//   * sign-out clears the cached list and the prompt, so one account's
//     ignore list can never bleed into another.
//
// HONEST SCOPE: policy and wiring only. Real m.ignored_user_list account-data
// round trips and /rooms/{roomId}/report against a homeserver are NOT
// exercised here and are NOT TESTED.

#include "app/ModerationController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/TimelineEvent.h"
#include "notifications/NotificationManager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

constexpr int kSignalTimeoutMs = 2000;

// The mock logs in as @alice:mock.local (localpart "alice", host from the
// mock homeserver URL).
const QString kSelf = QStringLiteral("@alice:mock.local");
const QString kTroll = QStringLiteral("@troll:example.org");
const QString kOther = QStringLiteral("@other:example.org");
const QString kRoom = QStringLiteral("!room:mock.local");
const QString kEvent = QStringLiteral("$ev1:mock.local");

TimelineEvent incomingText()
{
    TimelineEvent event;
    event.eventId = QStringLiteral("$ev:example.org");
    event.roomId = QStringLiteral("!room:example.org");
    event.sender = QStringLiteral("@bob:example.org");
    event.senderDisplayName = QStringLiteral("Bob");
    event.body = QStringLiteral("hello");
    event.type = TimelineEvent::TextMessage;
    event.status = TimelineEvent::Sent;
    return event;
}

} // namespace

class ModerationTest : public QObject
{
    Q_OBJECT

private:
    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        return spy.wait(kSignalTimeoutMs);
    }

    static bool ignoreAndWait(ModerationController &ctl, const QString &userId)
    {
        QSignalSpy finished(&ctl, &ModerationController::ignoreActionFinished);
        ctl.ignoreUser(userId);
        return finished.wait(kSignalTimeoutMs)
            && finished.at(0).at(2).toBool();
    }

private Q_SLOTS:
    void ignoreUserLandsInTheList()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.supported());
        const int revBefore = ctl.revision();

        QSignalSpy finished(&ctl, &ModerationController::ignoreActionFinished);
        ctl.ignoreUser(kTroll);
        QVERIFY(ctl.busy());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), kTroll);
        QVERIFY(finished.at(0).at(1).toBool());  // ignored
        QVERIFY(finished.at(0).at(2).toBool());  // ok
        QVERIFY(!finished.at(0).at(3).toString().isEmpty());

        QVERIFY(ctl.ignoredUsers().contains(kTroll));
        QVERIFY(ctl.isIgnored(kTroll));
        QVERIFY(!ctl.busy());
        // The write bumped the revision once; the authoritative sync push
        // that follows carries the SAME list and must not bump it again.
        QTest::qWait(80);
        QCOMPARE(ctl.revision(), revBefore + 1);
    }

    void unignoreReversesIt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);
        QVERIFY(ignoreAndWait(ctl, kTroll));
        QVERIFY(ctl.isIgnored(kTroll));

        QSignalSpy finished(&ctl, &ModerationController::ignoreActionFinished);
        ctl.unignoreUser(kTroll);
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(1).toBool()); // ignored == false
        QVERIFY(finished.at(0).at(2).toBool());  // ok
        QVERIFY(!ctl.isIgnored(kTroll));
        QVERIFY(ctl.ignoredUsers().isEmpty());
    }

    void selfIgnoreIsRefusedSynchronously()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QCOMPARE(client.currentUserId(), kSelf);
        ModerationController ctl;
        ctl.setClient(&client);

        QSignalSpy finished(&ctl, &ModerationController::ignoreActionFinished);
        ctl.ignoreUser(kSelf);
        // The backend refuses with no op id, and the controller reports the
        // failure immediately — no dispatch, no hang, no busy state.
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), kSelf);
        QVERIFY(!finished.at(0).at(2).toBool()); // ok == false
        QVERIFY(!finished.at(0).at(3).toString().isEmpty());
        QVERIFY(!ctl.busy());
        QTest::qWait(80);
        QVERIFY(!ctl.isIgnored(kSelf));
        QVERIFY(ctl.ignoredUsers().isEmpty());
    }

    void remoteChangeFollowsAndIdenticalPushIsDeduped()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);

        // Another client changed the account data: the sync push is
        // authoritative for local AND remote changes.
        client.mockIgnoredUsers = QStringList{ kTroll, kOther };
        Q_EMIT client.ignoredUsersChanged(client.mockIgnoredUsers);
        QCOMPARE(ctl.ignoredUsers(), (QStringList{ kTroll, kOther }));
        QVERIFY(ctl.isIgnored(kOther));
        const int revAfterFirst = ctl.revision();

        // The identical list pushed again is a no-op: no revision bump, no
        // stateChanged churn for QML to re-evaluate.
        QSignalSpy state(&ctl, &ModerationController::stateChanged);
        Q_EMIT client.ignoredUsersChanged(client.mockIgnoredUsers);
        QCOMPARE(state.size(), 0);
        QCOMPARE(ctl.revision(), revAfterFirst);

        // The explicit refresh path answers with the same truth — still no
        // phantom change.
        ctl.refreshIgnoredUsers();
        QTest::qWait(80);
        QCOMPARE(ctl.ignoredUsers(), (QStringList{ kTroll, kOther }));
        QCOMPARE(ctl.revision(), revAfterFirst);
    }

    void reportFlowPromptSubmitAndCancel()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.reportSupported());
        QVERIFY(!ctl.reportPromptActive());

        ctl.beginReport(kRoom, kEvent);
        QVERIFY(ctl.reportPromptActive());

        QSignalSpy finished(&ctl, &ModerationController::reportFinished);
        ctl.submitReport(QStringLiteral("spam"));
        QVERIFY(!ctl.reportPromptActive()); // closes on submit
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool()); // ok
        QVERIFY(!finished.at(0).at(1).toString().isEmpty());
        QVERIFY(!ctl.busy());

        // Cancel clears the prompt without dispatching anything.
        ctl.beginReport(kRoom, kEvent);
        QVERIFY(ctl.reportPromptActive());
        ctl.cancelReport();
        QVERIFY(!ctl.reportPromptActive());
        QVERIFY(!ctl.busy());
        QTest::qWait(80);
        QCOMPARE(finished.size(), 1); // no second outcome ever arrives
    }

    void reportFailureCategorySurfaces()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);
        client.mockReportFailCategory = QStringLiteral("not_found");

        QSignalSpy finished(&ctl, &ModerationController::reportFinished);
        ctl.beginReport(kRoom, kEvent);
        ctl.submitReport(QString());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(0).toBool()); // ok == false
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("That message no longer exists on the "
                                "server."));
    }

    void reportIsSingleFlight()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);

        ctl.beginReport(kRoom, kEvent);
        QSignalSpy finished(&ctl, &ModerationController::reportFinished);
        ctl.submitReport(QStringLiteral("spam"));
        // While the submission is in flight, a second report cannot open.
        ctl.beginReport(kRoom, QStringLiteral("$ev2:mock.local"));
        QVERIFY(!ctl.reportPromptActive());

        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        // Once the outcome lands, reporting is available again.
        ctl.beginReport(kRoom, QStringLiteral("$ev2:mock.local"));
        QVERIFY(ctl.reportPromptActive());
    }

    // Pure policy: an ignored sender never notifies, even for an event that
    // would otherwise notify. This is the local belt-and-braces for the race
    // window between the ignore write and the server applying it.
    void ignoredSenderNeverNotifies()
    {
        NotificationManager::Context context;
        context.selfUserId = kSelf;
        context.roomName = QStringLiteral("Lightning Dev");
        context.previewMode = NotificationManager::SenderOnly;
        context.notificationsEnabled = true;

        // Sanity: this event notifies when the sender is not ignored.
        const auto allowed =
            NotificationManager::decide(incomingText(), context);
        QVERIFY(allowed.notify);

        context.senderIsIgnored = true;
        const auto suppressed =
            NotificationManager::decide(incomingText(), context);
        QVERIFY(!suppressed.notify);
        QVERIFY(!suppressed.playSound);
        QVERIFY(suppressed.title.isEmpty());
        QVERIFY(suppressed.body.isEmpty());
    }

    void loggedOutClearsListAndPrompt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ModerationController ctl;
        ctl.setClient(&client);
        QVERIFY(ignoreAndWait(ctl, kTroll));
        ctl.beginReport(kRoom, kEvent);
        QVERIFY(ctl.reportPromptActive());
        const int revBefore = ctl.revision();

        // One account's ignore list must never bleed into another.
        client.logout();
        QVERIFY(ctl.ignoredUsers().isEmpty());
        QVERIFY(!ctl.isIgnored(kTroll));
        QVERIFY(!ctl.reportPromptActive());
        QVERIFY(!ctl.busy());
        QVERIFY(ctl.revision() > revBefore);
    }
};

QTEST_MAIN(ModerationTest)
#include "ModerationTest.moc"
