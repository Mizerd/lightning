// Personal moderation: ModerationController policy (the account's
// m.ignored_user_list cache and message reporting) against MockMatrixClient,
// plus the pure notification guard. Pins:
//   * ignore/unignore round-trip: the terminal signal, the cached list,
//     isIgnored() and the bump-on-change revision;
//   * self-ignore is refused synchronously and reports a failure;
//   * a remote change lands through the sync push, and an identical pushed
//     list causes no revision bump;
//   * the report prompt opens/submits/cancels correctly, failure categories
//     surface, and reporting is single-flight;
//   * NotificationManager::decide short-circuits an ignored sender (before
//     the server applies the ignore);
//   * sign-out clears the cached list and the prompt.
//
// Policy and wiring only: real account-data round trips and
// /rooms/{roomId}/report against a homeserver are not exercised here.

#include "app/ModerationController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/TimelineEvent.h"
#include "notifications/NotificationManager.h"

#include <QFile>
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

    // Every destructive action asks first: Delete, Remove edits, End poll,
    // Set role, Remove widget and Ignore user. A role change can be one-way
    // (Matrix refuses a change at or above your own level). Asserted per
    // handler: the trigger routes through a confirmation.
    void everyDestructiveActionAsksBeforeItActs()
    {
        struct Case {
            const char *file;
            const char *anchor;   // the action's own objectName or text
            const char *mustNotCallDirectly;
            const char *confirmation;
        };
        const QVector<Case> cases{
            { "/MessageDelegate.qml", "text: qsTr(\"Delete\")",
              "redactEvent(root.menuEventId)", "confirmDestructive" },
            { "/MessageDelegate.qml", "removeEditsMenuItem",
              "app.composer.removeEdits(root.menuEventId)", "confirmDestructive" },
            { "/MessageDelegate.qml", "endPollMenuItem",
              "onTriggered: app.composer.endPoll(", "confirmDestructive" },
            { "/MemberProfilePopover.qml", "profileRoleButton_",
              "onClicked: app.roomInfo.setMemberPowerLevel", "roleConfirm.openFor" },
            { "/RoomInfoPanel.qml", "roomInfoRemoveWidgetButton",
              "onClicked: app.widgets.removeWidget(widgetRow.index)",
              "removeWidgetConfirm.openFor" },
        };
        for (const Case &c : cases) {
            QFile file(QStringLiteral(QML_DIR) + QLatin1String(c.file));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
            const QString source = QString::fromUtf8(file.readAll());
            const int at = source.indexOf(QString::fromLatin1(c.anchor));
            QVERIFY2(at > 0,
                     qPrintable(QStringLiteral("%1: anchor %2 is gone, so this "
                                               "contract guards nothing")
                                    .arg(QLatin1String(c.file),
                                         QLatin1String(c.anchor))));
            // 3000 chars: the role button's objectName and its onClicked sit
            // about forty lines apart, with the contentItem and background
            // between them. Still bounded to one action's own block.
            const QString body = source.mid(at, 3000);
            QVERIFY2(!body.contains(QString::fromLatin1(c.mustNotCallDirectly)),
                     qPrintable(QStringLiteral("%1: %2 still acts straight "
                                               "from its trigger, with no "
                                               "confirmation in front of it")
                                    .arg(QLatin1String(c.file),
                                         QLatin1String(c.anchor))));
            QVERIFY2(body.contains(QString::fromLatin1(c.confirmation)),
                     qPrintable(QStringLiteral("%1: %2 does not route through "
                                               "a confirmation")
                                    .arg(QLatin1String(c.file),
                                         QLatin1String(c.anchor))));
        }
    }

    // A thread-reply notification must not jump to the reply on the room
    // timeline (which hides threaded events), and must not paginate it
    // either: a thread root can be arbitrarily old and the thread panel is
    // already open. revealIfLoaded() takes the root only if already loaded.
    void aThreadNotificationDoesNotJumpToAnEventTheTimelineHides()
    {
        QFile file(QStringLiteral(QML_DIR "/Main.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int at = source.indexOf(QStringLiteral("onNotificationOpenRequested"));
        QVERIFY2(at > 0, "the notification click handler is gone");
        // The handler's own block, by brace matching, not a fixed window.
        const int open = source.indexOf(QLatin1Char('{'), at);
        QVERIFY2(open > at, "the handler has no body");
        int depth = 0;
        int close = -1;
        for (int i = open; i < source.size(); ++i) {
            if (source.at(i) == QLatin1Char('{'))
                ++depth;
            else if (source.at(i) == QLatin1Char('}') && --depth == 0) {
                close = i;
                break;
            }
        }
        QVERIFY2(close > open, "the handler's body is unbalanced");
        const QString body = source.mid(open, close - open + 1);
        // B023: the reply id must never reach the room timeline at all.
        QVERIFY2(!body.contains(QStringLiteral("jumpToEvent(target)")),
                 "the single-target form is back, so a threaded event id can "
                 "reach the room timeline's jump again — it cannot find one");
        QVERIFY2(!body.contains(QStringLiteral("revealIfLoaded(eventId)")),
                 "the reply id is handed to the room timeline, which hides "
                 "threaded events and can never hold it");
        // The root is CONTEXT: free if loaded, never paid for.
        QVERIFY2(body.contains(QStringLiteral("revealIfLoaded(threadRootId)")),
                 "the thread root is not revealed from the loaded timeline");
        QVERIFY2(!body.contains(QStringLiteral("jumpToEvent(threadRootId)")),
                 "the thread root is still handed to jumpToEvent, which is "
                 "allowed to paginate for it: that is the runaway backward "
                 "scroll");
        // And a NON-thread notification still goes to its own message —
        // that one IS what the reader asked for.
        QVERIFY2(body.contains(QStringLiteral("jumpToEvent(eventId)")),
                 "an ordinary notification no longer jumps to the message it "
                 "was raised for");
        // A text scan cannot see branch structure, so pin the shape too: the
        // room-timeline fallback must be the else of the last `if (inThread)`,
        // not an unconditional call after it.
        const int thread = body.lastIndexOf(QStringLiteral("if (inThread)"));
        QVERIFY2(thread >= 0, "the thread branch is gone");
        const int otherwise = body.indexOf(QStringLiteral("} else if ("), thread);
        QVERIFY2(otherwise > thread,
                 "the room-timeline jump is no longer the ELSE of the thread "
                 "branch, so a thread reply can reach it again");
        QVERIFY2(body.indexOf(QStringLiteral("jumpToEvent(eventId)")) > otherwise,
                 "jumpToEvent(eventId) is not inside that else branch");
    }

    // A submitted report must tell the user what happened: submitReport
    // clears the prompt before the server answers, so the dialog closes at
    // once and only the reportFinished consumer in Main.qml can report the
    // outcome. Source contract; the cases above prove the signal's payload.
    void theReportOutcomeReachesAScreen()
    {
        QFile file(QStringLiteral(QML_DIR "/Main.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int handler = source.indexOf(QStringLiteral("onReportFinished"));
        QVERIFY2(handler > 0,
                 "no surface listens to ModerationController::reportFinished, "
                 "so a submitted report never tells the user whether the "
                 "server accepted it, refused it or lost it");
        // Bounded to the handler, so an unrelated notice elsewhere in this
        // very large file cannot satisfy the contract.
        const QString body = source.mid(handler, 400);
        QVERIFY2(body.contains(QStringLiteral("pinNotice.show")),
                 "the report outcome is received and then dropped without "
                 "being shown");
        // Both outcomes are shown, asserted as a positive shape: listing ways
        // to be wrong cannot cover the one nobody thought of.
        QVERIFY2(body.contains(QStringLiteral("pinNotice.show(message, !ok)")),
                 "the handler no longer passes the outcome straight through, "
                 "so it either reports one outcome only (a silent success is "
                 "indistinguishable from a dead menu item) or invents wording "
                 "that can drift from the controller's own");
    }
};

QTEST_MAIN(ModerationTest)
#include "ModerationTest.moc"
