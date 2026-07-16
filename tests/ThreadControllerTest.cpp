// v0.6.0 checkpoint 2: SDK-backed thread foundation, exercised through the
// deterministic mock backend. The mock serves thread timelines under the
// same composite timeline-id contract as the Rust backend (root first,
// replies in room order, live reply propagation), so these tests pin the
// lifecycle rules — open/close, root/reply identity, thread and room
// switches, failure states, and the thread-only send path — without a
// homeserver.

#include "matrix/MockMatrixClient.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 2000;

const QString kGeneral = QStringLiteral("!general:mock.local");
const QString kDevs = QStringLiteral("!devs:mock.local");
const QString kDm = QStringLiteral("!dm-bob:mock.local");
} // namespace

class ThreadControllerTest : public QObject
{
    Q_OBJECT

private:
    // Logged-in mock client; returns false on login failure.
    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("unused"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

    // First fixture thread root of a room: the first event some other event
    // names as its threadRootId.
    static QString firstThreadRootId(MockMatrixClient &client,
                                     const QString &roomId)
    {
        const auto events = client.timeline(roomId);
        for (const auto &e : events) {
            if (!e.threadRootId.isEmpty())
                return e.threadRootId;
        }
        return {};
    }

    static QStringList bodies(const TimelineModel &model)
    {
        QStringList out;
        for (int row = 0; row < model.rowCount(); ++row)
            out << model.data(model.index(row, 0), TimelineModel::BodyRole)
                       .toString();
        return out;
    }

private Q_SLOTS:
    void mockBackendSupportsThreadTimelines()
    {
        MockMatrixClient client;
        ThreadController controller;
        QVERIFY(!controller.supported());   // no client yet
        controller.setClient(&client);
        QVERIFY(controller.supported());
    }

    // Opening a fixture thread promotes Closed → Opening → Ready and loads
    // exactly the root (pinned first, never duplicated) plus its replies —
    // no unrelated room events.
    void openThreadLoadsRootFirstAndRepliesOnly()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        QVERIFY(!rootId.isEmpty());

        QSignalSpy stateSpy(&controller, &ThreadController::stateChanged);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(stateSpy.count() >= 2);   // Opening, then Ready

        auto *model = controller.model();
        QCOMPARE(model->roomId(),
                 MatrixClient::threadTimelineId(kGeneral, rootId));
        QVERIFY(model->rowCount() >= 3);  // root + two fixture replies

        int rootRows = 0;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex idx = model->index(row, 0);
            const QString eventId =
                model->data(idx, TimelineModel::EventIdRole).toString();
            const QString threadRootId =
                model->data(idx, TimelineModel::ThreadRootIdRole).toString();
            if (eventId == rootId) {
                ++rootRows;
                QCOMPARE(row, 0);         // root pinned first
            } else {
                QCOMPARE(threadRootId, rootId);  // every other row is a reply
            }
        }
        QCOMPARE(rootRows, 1);            // present exactly once
    }

    // The ROOM timeline model reports thread roles for the same fixtures:
    // the root is recognized and the reply count matches the loaded replies.
    void roomModelReportsThreadRoles()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        const QString rootId = firstThreadRootId(client, kGeneral);
        QVERIFY(!rootId.isEmpty());

        TimelineModel room;
        room.setClient(&client);
        room.setRoomId(kGeneral);

        bool sawRoot = false;
        for (int row = 0; row < room.rowCount(); ++row) {
            const QModelIndex idx = room.index(row, 0);
            if (room.data(idx, TimelineModel::EventIdRole).toString() != rootId)
                continue;
            sawRoot = true;
            QVERIFY(room.data(idx, TimelineModel::IsThreadRootRole).toBool());
            QCOMPARE(room.data(idx, TimelineModel::ThreadReplyCountRole).toInt(),
                     2);
        }
        QVERIFY(sawRoot);
    }

    void unknownRootFails()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        controller.openThread(kGeneral, QStringLiteral("$missing:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Failed,
                                  kSignalTimeoutMs);
        QCOMPARE(controller.failureCategory(), QStringLiteral("unknown_root"));
        QCOMPARE(controller.model()->rowCount(), 0);
    }

    // Switching to another thread replaces the panel: the model holds only
    // the new thread's events; nothing from the old thread leaks through.
    void threadSwitchReplacesContent()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString firstRoot = firstThreadRootId(client, kGeneral);
        QVERIFY(!firstRoot.isEmpty());
        controller.openThread(kGeneral, firstRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        // Create a second thread by replying to a plain fixture message.
        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        QVERIFY(secondRoot != firstRoot);
        client.sendThreadReply(kGeneral, secondRoot,
                               QStringLiteral("new thread reply"));

        controller.openThread(kGeneral, secondRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QCOMPARE(controller.rootEventId(), secondRoot);
        QCOMPARE(controller.model()->roomId(),
                 MatrixClient::threadTimelineId(kGeneral, secondRoot));
        const QStringList loaded = bodies(*controller.model());
        QVERIFY(loaded.contains(QStringLiteral("new thread reply")));
        for (int row = 0; row < controller.model()->rowCount(); ++row) {
            const QString threadRootId = controller.model()
                ->data(controller.model()->index(row, 0),
                       TimelineModel::ThreadRootIdRole)
                .toString();
            QVERIFY(threadRootId.isEmpty() || threadRootId == secondRoot);
        }
    }

    // A room switch always closes the panel; switching "to" the same room
    // does not.
    void roomSwitchClosesThread()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        controller.handleCurrentRoomChanged(kGeneral);   // same room: keep
        QCOMPARE(controller.state(), ThreadController::Ready);

        controller.handleCurrentRoomChanged(kDm);        // other room: close
        QCOMPARE(controller.state(), ThreadController::Closed);
        QCOMPARE(controller.model()->rowCount(), 0);
        QVERIFY(controller.roomId().isEmpty());
    }

    // sendText goes through the backend's THREAD send path: the reply lands
    // in the open thread timeline AND the room timeline with the correct
    // thread root, exactly once each — never as an ordinary room message.
    void sendTextCreatesThreadReplyOnly()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        const int threadRowsBefore = controller.model()->rowCount();

        controller.sendText(QStringLiteral("  sent from panel  "));

        QTRY_COMPARE_WITH_TIMEOUT(controller.model()->rowCount(),
                                  threadRowsBefore + 1, kSignalTimeoutMs);
        const QModelIndex last =
            controller.model()->index(controller.model()->rowCount() - 1, 0);
        QCOMPARE(controller.model()
                     ->data(last, TimelineModel::BodyRole).toString(),
                 QStringLiteral("sent from panel"));
        QCOMPARE(controller.model()
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);

        int roomOccurrences = 0;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.body == QLatin1String("sent from panel")) {
                ++roomOccurrences;
                QCOMPARE(e.threadRootId, rootId);   // never an ordinary message
            }
        }
        QCOMPARE(roomOccurrences, 1);

        // Empty/whitespace bodies never dispatch.
        const int rows = controller.model()->rowCount();
        controller.sendText(QStringLiteral("   "));
        QCOMPARE(controller.model()->rowCount(), rows);
    }

    // Encrypted-thread fixtures: the decrypted root and reply load with
    // their encryption metadata, and the undecryptable reply stays a safe
    // placeholder inside the thread.
    void encryptedThreadLoadsWithUndecryptableReply()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kDevs);
        QVERIFY(!rootId.isEmpty());
        controller.openThread(kDevs, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        auto *model = controller.model();
        QCOMPARE(model->rowCount(), 3);
        QVERIFY(model->data(model->index(0, 0),
                            TimelineModel::IsEncryptedRole).toBool());
        bool sawUndecryptable = false;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex idx = model->index(row, 0);
            if (model->data(idx, TimelineModel::UndecryptableRole).toBool()) {
                sawUndecryptable = true;
                QCOMPARE(model->data(idx, TimelineModel::ErrorKindRole)
                             .toString(),
                         QStringLiteral("session_missing"));
            }
        }
        QVERIFY(sawUndecryptable);
    }

    void participantsAreDeduplicated()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        controller.sendText(QStringLiteral("one"));
        controller.sendText(QStringLiteral("two"));

        const QStringList participants = controller.participants();
        QCOMPARE(participants.count(QStringLiteral("@alice:mock.local")), 1);
        QVERIFY(participants.contains(QStringLiteral("@carol:mock.local")));
        for (const auto &p : participants)
            QCOMPARE(participants.count(p), 1);
    }

    // ── v0.6.0 checkpoint 4: reply-within-thread compose state ──────────
    void replyStateTargetsLoadedThreadEventsOnly()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(!controller.inReply());

        // Unloaded/foreign events are not valid targets.
        controller.beginReply(QStringLiteral("$not-in-thread:mock.local"));
        QVERIFY(!controller.inReply());

        // Replying to the root is a plain thread message (no rich target).
        controller.beginReply(rootId);
        QVERIFY(!controller.inReply());

        // A loaded reply is a valid target and resolves its presentation.
        auto *model = controller.model();
        const QString replyId = model
            ->data(model->index(1, 0), TimelineModel::EventIdRole).toString();
        QSignalSpy replySpy(&controller, &ThreadController::replyStateChanged);
        controller.beginReply(replyId);
        QVERIFY(controller.inReply());
        QCOMPARE(controller.replyToEventId(), replyId);
        QVERIFY(!controller.replyToSender().isEmpty());
        QVERIFY(!controller.replyToPreview().isEmpty());
        QCOMPARE(replySpy.count(), 1);

        controller.cancelReply();
        QVERIFY(!controller.inReply());
        QCOMPARE(replySpy.count(), 2);
    }

    // Sending with an active reply target produces a rich reply WITHIN the
    // thread (both threadRootId and replyToEventId set) and clears the
    // target; the next send is a plain thread message again.
    void sendWithReplyTargetCreatesRichThreadReply()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        auto *model = controller.model();
        const QString replyId = model
            ->data(model->index(1, 0), TimelineModel::EventIdRole).toString();
        const int rowsBefore = model->rowCount();

        controller.beginReply(replyId);
        controller.sendText(QStringLiteral("rich reply body"));
        QVERIFY(!controller.inReply());   // cleared after dispatch

        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), rowsBefore + 1,
                                  kSignalTimeoutMs);
        const QModelIndex last = model->index(model->rowCount() - 1, 0);
        QCOMPARE(model->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QCOMPARE(model->data(last, TimelineModel::ReplyToEventIdRole).toString(),
                 replyId);
        QVERIFY(!model->data(last, TimelineModel::ReplyToPreviewRole)
                     .toString().isEmpty());

        controller.sendText(QStringLiteral("plain follow-up"));
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), rowsBefore + 2,
                                  kSignalTimeoutMs);
        const QModelIndex plain = model->index(model->rowCount() - 1, 0);
        QCOMPARE(model->data(plain, TimelineModel::ReplyToEventIdRole)
                     .toString(), QString{});
        QCOMPARE(model->data(plain, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
    }

    // Close, room switch, and thread switch all clear compose-reply state.
    void lifecycleTransitionsClearReplyState()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        auto arm = [&] {
            controller.openThread(kGeneral, rootId);
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(),
                                      ThreadController::Ready,
                                      kSignalTimeoutMs);
            auto *model = controller.model();
            controller.beginReply(model
                ->data(model->index(1, 0), TimelineModel::EventIdRole)
                .toString());
            QVERIFY(controller.inReply());
        };

        arm();
        controller.close();
        QVERIFY(!controller.inReply());

        arm();
        controller.handleCurrentRoomChanged(kDm);
        QVERIFY(!controller.inReply());

        arm();
        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        client.sendThreadReply(kGeneral, secondRoot, QStringLiteral("seed"));
        controller.openThread(kGeneral, secondRoot);
        QVERIFY(!controller.inReply());
    }

    void logoutClosesThread()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        client.logout();
        QCOMPARE(controller.state(), ThreadController::Closed);
        QCOMPARE(controller.model()->rowCount(), 0);
    }
};

QTEST_MAIN(ThreadControllerTest)
#include "ThreadControllerTest.moc"
