
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

}

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
        QVERIFY(finished.at(0).at(1).toBool());
        QVERIFY(finished.at(0).at(2).toBool());
        QVERIFY(!finished.at(0).at(3).toString().isEmpty());

        QVERIFY(ctl.ignoredUsers().contains(kTroll));
        QVERIFY(ctl.isIgnored(kTroll));
        QVERIFY(!ctl.busy());
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
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(finished.at(0).at(2).toBool());
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
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), kSelf);
        QVERIFY(!finished.at(0).at(2).toBool());
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

        client.mockIgnoredUsers = QStringList{ kTroll, kOther };
        Q_EMIT client.ignoredUsersChanged(client.mockIgnoredUsers);
        QCOMPARE(ctl.ignoredUsers(), (QStringList{ kTroll, kOther }));
        QVERIFY(ctl.isIgnored(kOther));
        const int revAfterFirst = ctl.revision();

        QSignalSpy state(&ctl, &ModerationController::stateChanged);
        Q_EMIT client.ignoredUsersChanged(client.mockIgnoredUsers);
        QCOMPARE(state.size(), 0);
        QCOMPARE(ctl.revision(), revAfterFirst);

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
        QVERIFY(!ctl.reportPromptActive());
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(0).toBool());
        QVERIFY(!finished.at(0).at(1).toString().isEmpty());
        QVERIFY(!ctl.busy());

        ctl.beginReport(kRoom, kEvent);
        QVERIFY(ctl.reportPromptActive());
        ctl.cancelReport();
        QVERIFY(!ctl.reportPromptActive());
        QVERIFY(!ctl.busy());
        QTest::qWait(80);
        QCOMPARE(finished.size(), 1);
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
        QVERIFY(!finished.at(0).at(0).toBool());
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
        ctl.beginReport(kRoom, QStringLiteral("$ev2:mock.local"));
        QVERIFY(!ctl.reportPromptActive());

        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(finished.size(), 1);
        ctl.beginReport(kRoom, QStringLiteral("$ev2:mock.local"));
        QVERIFY(ctl.reportPromptActive());
    }

    void ignoredSenderNeverNotifies()
    {
        NotificationManager::Context context;
        context.selfUserId = kSelf;
        context.roomName = QStringLiteral("Lightning Dev");
        context.previewMode = NotificationManager::SenderOnly;
        context.notificationsEnabled = true;

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

        client.logout();
        QVERIFY(ctl.ignoredUsers().isEmpty());
        QVERIFY(!ctl.isIgnored(kTroll));
        QVERIFY(!ctl.reportPromptActive());
        QVERIFY(!ctl.busy());
        QVERIFY(ctl.revision() > revBefore);
    }

    void everyDestructiveActionAsksBeforeItActs()
    {
        struct Case {
            const char *file;
            const char *anchor;
            const char *mustNotCallDirectly;
            const char *confirmation;
        };
        const QVector<Case> cases{
            { "/MessageDelegate.qml", "text: qsTr(\"Delete\")",
              "redactEvent(root.menuEventId)", "root.deleteThisMessage()" },
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

        QFile del(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY2(del.open(QIODevice::ReadOnly), qPrintable(del.fileName()));
        const QString src = QString::fromUtf8(del.readAll());
        const int fn = src.indexOf(QStringLiteral("function deleteThisMessage()"));
        QVERIFY2(fn > 0, "deleteThisMessage() is gone, so Delete's route is unchecked");
        const int fnOpen = src.indexOf(QLatin1Char('{'), fn);
        int d = 0, fnClose = -1;
        for (int i = fnOpen; i < src.size(); ++i) {
            if (src.at(i) == QLatin1Char('{'))
                ++d;
            else if (src.at(i) == QLatin1Char('}') && --d == 0) {
                fnClose = i;
                break;
            }
        }
        QVERIFY2(fnClose > fnOpen, "deleteThisMessage() is unbalanced");
        const QString fnBody = src.mid(fnOpen, fnClose - fnOpen + 1);
        const int redactions = fnBody.count(QStringLiteral("redactEvent("));
        QVERIFY2(redactions >= 1, "deleteThisMessage() no longer redacts");
        QCOMPARE(fnBody.count(QStringLiteral("function() { root.timelineModel.redactEvent(")),
                 redactions);
        QCOMPARE(fnBody.count(QStringLiteral("confirmDestructive(")), redactions);
        QVERIFY2(src.contains(QStringLiteral(
                     "case \"timeline.deleteMessage\":\n"
                     "            if (root.bulkSelectionMode)\n"
                     "                app.bulkRedaction.toggle(root.menuEventId, root.canRedactOthers)\n"
                     "            else\n"
                     "                deleteThisMessage()")),
                 "the Delete hotkey no longer goes through deleteThisMessage()");
        QVERIFY2(!src.contains(QStringLiteral("case \"timeline.deleteMessage\": root.timelineModel.redactEvent")),
                 "the Delete hotkey redacts without asking");
    }

    void aThreadNotificationDoesNotJumpToAnEventTheTimelineHides()
    {
        QFile file(QStringLiteral(QML_DIR "/Main.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int at = source.indexOf(QStringLiteral("onNotificationOpenRequested"));
        QVERIFY2(at > 0, "the notification click handler is gone");
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
        QVERIFY2(!body.contains(QStringLiteral("jumpToEvent(target)")),
                 "the single-target form is back, so a threaded event id can "
                 "reach the room timeline's jump again — it cannot find one");
        QVERIFY2(!body.contains(QStringLiteral("revealIfLoaded(eventId)")),
                 "the reply id is handed to the room timeline, which hides "
                 "threaded events and can never hold it");
        QVERIFY2(body.contains(QStringLiteral("revealIfLoaded(threadRootId)")),
                 "the thread root is not revealed from the loaded timeline");
        QVERIFY2(!body.contains(QStringLiteral("jumpToEvent(threadRootId)")),
                 "the thread root is still handed to jumpToEvent, which is "
                 "allowed to paginate for it: that is the runaway backward "
                 "scroll");
        QVERIFY2(body.contains(QStringLiteral("jumpToEvent(eventId)")),
                 "an ordinary notification no longer jumps to the message it "
                 "was raised for");
        const int inlineAt = body.indexOf(QStringLiteral("threadRepliesInline === true"));
        QVERIFY2(inlineAt >= 0, "the inline-thread notification branch is gone");
        const int narrow = body.indexOf(QStringLiteral("app.showThread("), inlineAt);
        const int inlineJump = body.indexOf(QStringLiteral("jumpToEvent(eventId)"), inlineAt);
        const int inlineRet = body.indexOf(QStringLiteral("return"), inlineJump);
        QVERIFY2(narrow > inlineAt && inlineJump > narrow && inlineRet > inlineJump,
                 "the inline branch must narrow to the thread, then jump, then return");
        const int panelAt = body.indexOf(QStringLiteral("if (inThread)"), inlineRet);
        QVERIFY2(panelAt > inlineRet, "the panel-mode thread branch is gone");
        const int thread = body.lastIndexOf(QStringLiteral("if (inThread)"));
        QVERIFY2(thread >= 0, "the thread branch is gone");
        const int otherwise = body.indexOf(QStringLiteral("} else if ("), thread);
        QVERIFY2(otherwise > thread,
                 "the room-timeline jump is no longer the ELSE of the thread "
                 "branch, so a thread reply can reach it again");
        QVERIFY2(body.lastIndexOf(QStringLiteral("jumpToEvent(eventId)")) > otherwise,
                 "jumpToEvent(eventId) is not inside that else branch");
    }

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
        const QString body = source.mid(handler, 400);
        QVERIFY2(body.contains(QStringLiteral("pinNotice.show")),
                 "the report outcome is received and then dropped without "
                 "being shown");
        QVERIFY2(body.contains(QStringLiteral("pinNotice.show(message, !ok)")),
                 "the handler no longer passes the outcome straight through, "
                 "so it either reports one outcome only (a silent success is "
                 "indistinguishable from a dead menu item) or invents wording "
                 "that can drift from the controller's own");
    }
};

QTEST_MAIN(ModerationTest)
#include "ModerationTest.moc"
