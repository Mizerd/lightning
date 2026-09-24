// Thread lifecycle through the deterministic mock backend, which serves thread
// timelines under the same composite timeline-id contract as the Rust backend
// (root first, replies in room order, live reply propagation): open/close,
// root/reply identity, thread and room switches, failure states, and the
// thread-only send path.

#include "matrix/MockMatrixClient.h"
#include "models/AttachmentQueueModel.h"
#include "models/PaginationController.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"
#include "threads/ThreadManager.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QUrl>
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

    // Opens a room's first fixture thread. A private helper, not a slot:
    // everything in the Q_SLOTS block is a test case.
    static bool openFixtureThread(MockMatrixClient &client,
                                  ThreadController &controller,
                                  const QString &roomId, QString *rootOut)
    {
        const QString rootId = firstThreadRootId(client, roomId);
        if (rootId.isEmpty())
            return false;
        controller.openThread(roomId, rootId);
        if (rootOut)
            *rootOut = rootId;
        return true;
    }

private Q_SLOTS:
    // An edit addressed to the open thread lands in the thread's own list: the
    // mock keeps a separate copy under the composite thread id, which is what
    // TimelineModel::onEventEdited re-reads for a thread model. Editing only
    // the room copy would be overwritten by the stale thread copy.
    void anEditThroughTheOpenThreadLandsInTheThreadsOwnList()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        const QString composite =
            MatrixClient::threadTimelineId(kGeneral, rootId);
        const auto before = client.timeline(composite);
        QVERIFY2(before.size() > 1,
                 "fixture assumption: the thread holds the root and a reply");
        // A reply, not the root: the root also lives in the room list, so
        // editing it could pass by coincidence.
        const QString replyId = before.at(1).eventId;
        QVERIFY(!replyId.isEmpty());

        const QString edited = QStringLiteral("edited through the thread");
        client.editMessage(composite, replyId, edited);

        const auto after = client.timeline(composite);
        QCOMPARE(after.size(), before.size());
        const auto row = std::find_if(
            after.cbegin(), after.cend(),
            [&](const TimelineEvent &e) { return e.eventId == replyId; });
        QVERIFY2(row != after.cend(),
                 "the edited reply left the thread timeline entirely");
        QCOMPARE(row->body, edited);
    }

    // The divider's number counts replies, not rows (rows include date
    // dividers, the read marker and the timeline-start row).
    void replyCountIsRepliesNotRows()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        // What the room says the thread holds, computed independently.
        int repliesInRoom = 0;
        for (const auto &e : client.timeline(kGeneral))
            if (e.threadRootId == rootId)
                ++repliesInRoom;
        QVERIFY2(repliesInRoom > 0, "fixture assumption: the thread has replies");
        QCOMPARE(controller.replyCount(), repliesInRoom);

        // Make the row count and reply count disagree: a virtual row is not a
        // reply.
        const int rowsBefore = controller.model()->property("count").toInt();
        const int realBefore = controller.model()->property("realCount").toInt();
        QCOMPARE(controller.replyCount(), realBefore - 1);

        TimelineEvent divider;
        divider.type = TimelineEvent::DateDivider;
        divider.itemId = QStringLiteral("uid-divider");
        divider.roomId = MatrixClient::threadTimelineId(kGeneral, rootId);
        divider.timestamp = QDateTime::currentDateTimeUtc();
        QVERIFY(divider.isVirtual());
        client.appendEventForTest(divider.roomId, divider);

        QCOMPARE(controller.model()->property("count").toInt(), rowsBefore + 1);
        QCOMPARE(controller.model()->property("realCount").toInt(), realBefore);
        QCOMPARE(controller.replyCount(), repliesInRoom);
    }

    // The SDK's reply count wins over a loaded count (thread timelines are
    // windowed), and its arrival is announced: the summary lands as an
    // in-place Set on the root, which does not emit countChanged.
    void theSdkSummaryWinsAndItsArrivalIsAnnounced()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        const int loaded = controller.replyCount();
        QVERIFY2(loaded > 0, "fixture assumption: the thread has loaded replies");

        // A windowed thread: the server knows about far more replies.
        const int serverSays = loaded + 40;
        QSignalSpy spy(&controller, &ThreadController::replyCountChanged);
        const QString composite =
            MatrixClient::threadTimelineId(kGeneral, rootId);
        const auto thread = client.timeline(composite);
        const int rootIndex = static_cast<int>(std::distance(
            thread.cbegin(),
            std::find_if(thread.cbegin(), thread.cend(),
                         [&](const TimelineEvent &e) {
                             return e.eventId == rootId;
                         })));
        QVERIFY2(rootIndex < thread.size(),
                 "fixture assumption: the root is a row of the thread");
        TimelineEvent updated = thread.at(rootIndex);
        QCOMPARE(updated.threadReplyCount, -1);   // the unknown sentinel
        updated.threadReplyCount = serverSays;
        client.changeEventAtForTest(composite, rootIndex, updated);

        QCOMPARE(controller.replyCount(), serverSays);
        QCOMPARE(spy.count(), 1);

        // An identical second Set does not re-announce.
        client.changeEventAtForTest(composite, rootIndex, updated);
        QCOMPARE(spy.count(), 1);
    }

    // The divider never shows fewer replies than are on screen: a stale server
    // summary can lag behind the loaded replies.
    void aStaleSummaryNeverUndercutsTheRepliesOnScreen()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        const QString composite =
            MatrixClient::threadTimelineId(kGeneral, rootId);
        const auto thread = client.timeline(composite);
        const int rootIndex = static_cast<int>(std::distance(
            thread.cbegin(),
            std::find_if(thread.cbegin(), thread.cend(),
                         [&](const TimelineEvent &e) {
                             return e.eventId == rootId;
                         })));
        QVERIFY2(rootIndex < thread.size(), "fixture assumption: the root is a row");

        const int loaded = controller.replyCount();
        QVERIFY2(loaded >= 2, "fixture assumption: at least two replies");

        // The server is behind what is loaded.
        TimelineEvent stale = thread.at(rootIndex);
        stale.threadReplyCount = loaded - 1;
        client.changeEventAtForTest(composite, rootIndex, stale);

        QCOMPARE(controller.replyCount(), loaded);

        // It still yields to a summary that is ahead (a windowed thread).
        TimelineEvent ahead = stale;
        ahead.threadReplyCount = loaded + 40;
        client.changeEventAtForTest(composite, rootIndex, ahead);
        QCOMPARE(controller.replyCount(), loaded + 40);
    }

    // An in-place change to the root row (late decryption, edit, redaction,
    // name resolution) is announced, so the panel's root card snapshot
    // refreshes; such Sets change no row count.
    void aRootRowChangedInPlaceAnnouncesItself()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        const QString composite =
            MatrixClient::threadTimelineId(kGeneral, rootId);
        const auto thread = client.timeline(composite);
        const int rootIndex = static_cast<int>(std::distance(
            thread.cbegin(),
            std::find_if(thread.cbegin(), thread.cend(),
                         [&](const TimelineEvent &e) {
                             return e.eventId == rootId;
                         })));
        QVERIFY2(rootIndex < thread.size(), "fixture assumption: the root is a row");

        QSignalSpy rootSpy(&controller, &ThreadController::rootInfoChanged);
        QSignalSpy countSpy(controller.model(), &TimelineModel::countChanged);

        // A late decryption: same row, new body, no row added.
        TimelineEvent decrypted = thread.at(rootIndex);
        decrypted.body = QStringLiteral("the key finally arrived");
        client.changeEventAtForTest(composite, rootIndex, decrypted);

        QCOMPARE(rootSpy.count(), 1);
        QCOMPARE(controller.rootInfo().value(QStringLiteral("body")).toString(),
                 decrypted.body);
        // countChanged did not fire, so the panel's old trigger could not
        // have caught this.
        QCOMPARE(countSpy.count(), 0);

        // A Set on a reply is not a root change.
        const int replyIndex = rootIndex == 0 ? 1 : 0;
        TimelineEvent reply = thread.at(replyIndex);
        QVERIFY(reply.eventId != rootId);
        reply.body = QStringLiteral("a reply changed, not the root");
        client.changeEventAtForTest(composite, replyIndex, reply);
        QCOMPARE(rootSpy.count(), 1);
    }

    void mockBackendSupportsThreadTimelines()
    {
        MockMatrixClient client;
        ThreadController controller;
        QVERIFY(!controller.supported());   // no client yet
        controller.setClient(&client);
        QVERIFY(controller.supported());
    }

    // Opening a fixture thread goes Closed -> Opening -> Ready and loads
    // exactly the root (pinned first, never duplicated) plus its replies.
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

    // The room timeline model reports thread roles for the same fixtures.
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

    // Switching threads replaces the panel's content entirely.
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

    // A room switch closes the panel; switching to the same room does not.
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

    // sendText uses the backend's thread send path: the reply lands in the
    // thread and room timelines with the thread root, once each, never as an
    // ordinary room message.
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

    // Encrypted-thread fixtures: the decrypted root and reply keep their
    // encryption metadata, and the undecryptable reply stays a placeholder.
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

    // Reply-within-thread compose state.
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

    // Sending with a reply target produces a rich reply within the thread
    // (threadRootId and replyToEventId set) and clears the target.
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

    // Follow state, thread list, threaded read receipts.
    //
    // Follow state round-trips through the backend; stale answers for other
    // threads are ignored; unfollow works; close resets it.
    void followStateTracksSubscription()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        // The mock answers synchronously: supported, not subscribed.
        QVERIFY(controller.followSupported());
        QVERIFY(!controller.followed());
        QVERIFY(!controller.followBusy());

        controller.setFollowed(true);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
        QVERIFY(!controller.followBusy());

        // A stale answer for a different thread must not disturb state.
        Q_EMIT client.threadSubscriptionState(
            kGeneral, QStringLiteral("$other:mock.local"), true, false, false);
        QVERIFY(controller.followed());

        controller.setFollowed(false);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), false,
                                  kSignalTimeoutMs);

        // Persists across reopen (the mock's map stands in for MSC4306 server
        // state).
        controller.setFollowed(true);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
        controller.close();
        QVERIFY(!controller.followSupported());   // reset while closed
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
    }

    // The Threads view lists threads by latest activity, updates live, and
    // closes on room switch.
    void threadListListsAndFollowsActivity()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        controller.openList(kGeneral);
        QTRY_COMPARE_WITH_TIMEOUT(controller.listLoading(), false,
                                  kSignalTimeoutMs);
        QVERIFY(controller.listOpen());
        QVERIFY(controller.listEndReached());
        QVERIFY(!controller.listFailed());
        QCOMPARE(controller.threadList().size(), 1);   // one fixture thread
        const QVariantMap entry = controller.threadList().first().toMap();
        QCOMPARE(entry.value(QStringLiteral("rootEventId")).toString(),
                 firstThreadRootId(client, kGeneral));
        QCOMPARE(entry.value(QStringLiteral("replyCount")).toInt(), 2);
        QVERIFY(!entry.value(QStringLiteral("latestPreview"))
                     .toString().isEmpty());

        // A new thread appears in the live list, newest activity first.
        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        client.sendThreadReply(kGeneral, secondRoot,
                               QStringLiteral("fresh thread"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.threadList().size(), 2,
                                  kSignalTimeoutMs);
        QCOMPARE(controller.threadList().first().toMap()
                     .value(QStringLiteral("rootEventId")).toString(),
                 secondRoot);

        controller.handleCurrentRoomChanged(kDm);
        QVERIFY(!controller.listOpen());
        QVERIFY(controller.threadList().isEmpty());
    }

    // markRead sends one threaded receipt per new latest reply; repeated calls
    // deduplicate.
    void markReadDeduplicatesPerLatestReply()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        // Repeated calls with an unchanged latest reply send one receipt; a new
        // reply re-arms one more.
        controller.markRead();
        controller.markRead();
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 1);

        controller.sendText(QStringLiteral("advance latest"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 4, kSignalTimeoutMs);
        controller.markRead();
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 2);
        QCOMPARE(controller.state(), ThreadController::Ready);
    }

    // Manual decryption retry: both the room and thread models dispatch
    // retryDecryption with their own timeline id (the Rust backend maps a
    // composite thread id onto its room and retries both).
    void retryDecryptionDispatchesFromBothModels()
    {
        MockMatrixClient client;
        QVERIFY(login(client));

        TimelineModel room;
        room.setClient(&client);
        room.setRoomId(kDevs);
        room.retryDecryption();
        QCOMPARE(client.decryptionRetryRoomsForTest(),
                 QStringList{ kDevs });

        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kDevs);
        controller.openThread(kDevs, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        controller.model()->retryDecryption();
        QCOMPARE(client.decryptionRetryRoomsForTest().size(), 2);
        QCOMPARE(client.decryptionRetryRoomsForTest().at(1),
                 MatrixClient::threadTimelineId(kDevs, rootId));

        // An unbound model never dispatches.
        TimelineModel unbound;
        unbound.setClient(&client);
        unbound.retryDecryption();
        QCOMPARE(client.decryptionRetryRoomsForTest().size(), 2);
    }

    // A thread reply's permalink and details use the real room id, never the
    // composite thread-timeline id.
    void threadReplyPermalinkUsesRealRoomId()
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
        // The model is bound to the composite thread timeline id...
        QVERIFY(MatrixClient::isThreadTimelineId(model->roomId()));

        // ...but a reply's permalink and details resolve the real room.
        const QString replyId =
            model->data(model->index(1, 0), TimelineModel::EventIdRole)
                .toString();
        QVERIFY(!replyId.isEmpty());

        const QString link = model->messagePermalink(replyId);
        QVERIFY(link.startsWith(QStringLiteral("https://matrix.to/#/")));
        QVERIFY(!link.contains(QStringLiteral("thread")));
        QVERIFY(!link.contains(QChar(0x1f)));
        QVERIFY(!link.contains(QStringLiteral("%1F"))); // encoded separator
        QVERIFY(link.contains(QStringLiteral("general"))); // the real room

        const QVariantMap details = model->messageDetails(replyId);
        QCOMPARE(details.value(QStringLiteral("roomId")).toString(), kGeneral);
    }

    // Thread attachments.
    //
    // A queued attachment sends into the open thread (thread and room
    // timelines, with the thread root, as Image/File), and the tray clears
    // once the SDK send queue accepts it.
    void threadAttachmentSendsIntoThread()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QVERIFY(controller.attachmentsSupported() == false); // no thread yet

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(controller.attachmentsSupported());

        // Prepare a real temporary image file for validation.
        QTemporaryFile img(QDir::tempPath()
                           + QStringLiteral("/lightning-XXXXXX.png"));
        QVERIFY(img.open());
        QImage(4, 4, QImage::Format_RGB32).save(img.fileName(), "PNG");
        controller.addAttachment(QUrl::fromLocalFile(img.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);
        QVERIFY(controller.hasAttachments());

        const int threadRowsBefore = controller.model()->rowCount();
        controller.sendText(QString{});   // attachment-only send
        QCOMPARE(client.threadAttachmentCallsForTest(), 1);

        // The local echo appears in the thread with the root and an image.
        QTRY_COMPARE_WITH_TIMEOUT(controller.model()->rowCount(),
                                  threadRowsBefore + 1, kSignalTimeoutMs);
        const QModelIndex last =
            controller.model()->index(controller.model()->rowCount() - 1, 0);
        QCOMPARE(controller.model()
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QVERIFY(controller.model()->data(last, TimelineModel::IsImageRole)
                    .toBool());

        // The room timeline has exactly one threaded copy.
        int roomImages = 0;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.type == TimelineEvent::Image
                && e.body == QFileInfo(img.fileName()).fileName()) {
                ++roomImages;
                QCOMPARE(e.threadRootId, rootId);
            }
        }
        QCOMPARE(roomImages, 1);

        // The tray clears once the queue accepts the upload.
        QTRY_COMPARE_WITH_TIMEOUT(controller.attachments()->rowCount(), 0,
                                  kSignalTimeoutMs);
        QVERIFY(!controller.hasAttachments());
    }

    // A failed queue attempt leaves a retryable "failed" tray item, never a
    // permanent spinner or a room-timeline fallback.
    void threadAttachmentFailureIsRetryable()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        QTemporaryFile file(QDir::tempPath()
                            + QStringLiteral("/lightning-XXXXXX.bin"));
        QVERIFY(file.open());
        file.write("payload");
        file.flush();
        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));

        client.failNextThreadAttachmentForTest();
        const int roomRowsBefore = client.timeline(kGeneral).size();
        controller.sendText(QString{});
        // No room-timeline event was produced by the failed thread send.
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        // The entry is marked failed (retryable), not stuck dispatching.
        QCOMPARE(controller.attachments()->rowCount(), 1);
        const QModelIndex idx = controller.attachments()->index(0, 0);
        QCOMPARE(controller.attachments()
                     ->data(idx, AttachmentQueueModel::StateRole).toString(),
                 QStringLiteral("failed"));
    }

    // Switching threads or closing the panel discards queued attachments.
    void threadSwitchDiscardsQueuedAttachments()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        QTemporaryFile file(QDir::tempPath()
                            + QStringLiteral("/lightning-XXXXXX.bin"));
        QVERIFY(file.open());
        file.write("payload");
        file.flush();
        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);

        // Open a different thread: the queued attachment is dropped.
        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        client.sendThreadReply(kGeneral, secondRoot, QStringLiteral("seed"));
        controller.openThread(kGeneral, secondRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QCOMPARE(controller.attachments()->rowCount(), 0);
        QCOMPARE(client.threadAttachmentCallsForTest(), 0);

        // Closing also clears; logout too.
        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);
        controller.close();
        QCOMPARE(controller.attachments()->rowCount(), 0);
    }

    // An undecryptable thread reply recovers in place when the key arrives
    // (eventChangedAt on the thread timeline): same row, same identity, root
    // preserved, no duplicate and no reopen.
    void threadUndecryptableReplyRecoversInPlace()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);

        const QString rootId = firstThreadRootId(client, kDevs);
        controller.openThread(kDevs, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        auto *model = controller.model();
        const QString timelineId = MatrixClient::threadTimelineId(kDevs, rootId);

        // Locate the undecryptable reply row.
        int utdRow = -1;
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->data(model->index(row, 0),
                            TimelineModel::UndecryptableRole).toBool()) {
                utdRow = row;
                break;
            }
        }
        QVERIFY(utdRow >= 0);
        const int rowsBefore = model->rowCount();
        const QString stableId =
            model->data(model->index(utdRow, 0),
                        TimelineModel::EventIdRole).toString();

        // The key arrives: the item is replaced in place with the decrypted
        // event (same identity).
        const auto threadEvents = client.timeline(timelineId);
        TimelineEvent decrypted;
        for (const auto &e : threadEvents) {
            if (e.eventId == stableId) { decrypted = e; break; }
        }
        QCOMPARE(decrypted.eventId, stableId);
        decrypted.undecryptable = false;
        decrypted.isDecrypted = true;
        decrypted.body = QStringLiteral("now readable");
        decrypted.type = TimelineEvent::TextMessage;
        Q_EMIT client.eventChangedAt(timelineId, utdRow, decrypted);

        // Same row, decrypted: no duplicate, identity and root intact.
        QCOMPARE(model->rowCount(), rowsBefore);
        const QModelIndex idx = model->index(utdRow, 0);
        QVERIFY(!model->data(idx, TimelineModel::UndecryptableRole).toBool());
        QVERIFY(model->data(idx, TimelineModel::IsDecryptedRole).toBool());
        QCOMPARE(model->data(idx, TimelineModel::EventIdRole).toString(),
                 stableId);
        QCOMPARE(model->data(idx, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QCOMPARE(model->data(idx, TimelineModel::BodyRole).toString(),
                 QStringLiteral("now readable"));
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

    // ThreadManager's participant cache: requests are idempotent per (room,
    // root), an unknown thread reads empty, and sign-out drops the cache so
    // faces never cross accounts.
    void threadParticipantsAreCachedDedupedAndAccountScoped()
    {
        MockMatrixClient client;
        ThreadManager threads;
        threads.setClient(&client);

        const QString root = QStringLiteral("$root:example.org");

        // Unknown thread: empty means unknown, not "nobody".
        QVERIFY(threads.participants(kGeneral, root).isEmpty());

        QSignalSpy changed(&threads, &ThreadManager::participantsChanged);
        const QVariantList people{
            QVariantMap{ { QStringLiteral("userId"),
                           QStringLiteral("@alice:example.org") },
                         { QStringLiteral("displayName"),
                           QStringLiteral("Alice") },
                         { QStringLiteral("avatarUrl"), QString{} } },
            QVariantMap{ { QStringLiteral("userId"),
                           QStringLiteral("@bob:example.org") },
                         { QStringLiteral("displayName"),
                           QStringLiteral("Bob") },
                         { QStringLiteral("avatarUrl"), QString{} } },
        };
        Q_EMIT client.threadParticipantsReceived(kGeneral, root, people, 2,
                                                 false);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(threads.participants(kGeneral, root).size(), 2);

        // Scoped by room and root.
        QVERIFY(threads.participants(
                    kGeneral, QStringLiteral("$other:example.org")).isEmpty());

        // A failed lookup is not cached, so a transient failure can retry.
        const QString root2 = QStringLiteral("$second:example.org");
        Q_EMIT client.threadParticipantsReceived(kGeneral, root2, {}, 0, false);
        QCOMPARE(changed.count(), 1);          // no "changed" for a failure
        QVERIFY(threads.participants(kGeneral, root2).isEmpty());

        // Sign-out drops everything.
        client.logout();
        QVERIFY(threads.participants(kGeneral, root).isEmpty());
    }

    // Reply navigation inside the thread panel resolves against this thread's
    // timeline, never the room history loader.
    //
    // A loaded target resolves synchronously: one located signal with its row,
    // and a highlight that pulses rather than staying on.
    void navigateToLoadedThreadReplyLocatesItsRowAndPulses()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 150, 500);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        auto *model = controller.model();
        QVERIFY(model->rowCount() >= 3);

        const int targetRow = model->rowCount() - 1;
        const QString targetId =
            model->data(model->index(targetRow, 0),
                        TimelineModel::EventIdRole).toString();
        QVERIFY(!targetId.isEmpty());
        QVERIFY(targetId != rootId);

        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.navigateToEvent(targetId);

        QCOMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(0).toInt(), targetRow);
        QCOMPARE(controller.navigationHighlightEventId(), targetId);
        QVERIFY(controller.navigationMessage().isEmpty());
        QVERIFY(!controller.navigating());
        // The pulse expires on its own.
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.navigationHighlightEventId().isEmpty(), kSignalTimeoutMs);
    }

    // The highlight is observably on before it clears (the case above would
    // pass if it were never set).
    void theReplyHighlightIsVisibleBeforeItExpires()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 400, 500);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        auto *model = controller.model();
        const QString targetId =
            model->data(model->index(model->rowCount() - 1, 0),
                        TimelineModel::EventIdRole).toString();

        QSignalSpy navChanged(&controller, &ThreadController::navigationChanged);
        controller.navigateToEvent(targetId);
        QVERIFY(navChanged.count() >= 1);
        QCOMPARE(controller.navigationHighlightEventId(), targetId);
        QTest::qWait(80);
        QCOMPARE(controller.navigationHighlightEventId(), targetId);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.navigationHighlightEventId().isEmpty(), kSignalTimeoutMs);
    }

    // The thread root is pinned as a card, not a list row, so navigating to it
    // pulses the card: no row landing, no close, no room jump.
    void navigateToThreadRootPulsesTheCardWithoutLeavingTheThread()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 300, 500);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        const int roomRowsBefore = client.timeline(kGeneral).size();
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        QSignalSpy stateSpy(&controller, &ThreadController::stateChanged);

        controller.navigateToEvent(rootId);

        QCOMPARE(controller.navigationHighlightEventId(), rootId);
        QCOMPARE(located.count(), 0);          // the root owns no list row
        QCOMPARE(stateSpy.count(), 0);         // the thread stayed open
        QCOMPARE(controller.state(), ThreadController::Ready);
        QCOMPARE(controller.roomId(), kGeneral);
        QCOMPARE(controller.rootEventId(), rootId);
        QVERIFY(controller.navigationMessage().isEmpty());
        // Nothing asked the room for history.
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        QVERIFY(!client.paginating(kGeneral));
    }

    // A target reachable only through the thread's own backward pagination is
    // found by the bounded search.
    void navigateToAnUnloadedReplyPaginatesTheThreadTimeline()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(10);
        ThreadController controller;
        controller.setClient(&client);
        // A long highlight lifetime, since this waits on an async page.
        controller.setNavigationPolicyForTest(4, 1500, 3000);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        // Give the open thread timeline paginable history: the mock keys
        // pagination by timeline id, so seed it with its current rows plus a
        // page budget, and stage the page carrying the target.
        const QString threadId = MatrixClient::threadTimelineId(kGeneral,
                                                               rootId);
        TimelineEvent older;
        older.eventId = QStringLiteral("$older-thread-reply:mock.local");
        older.sender = QStringLiteral("@bob:mock.local");
        older.senderDisplayName = QStringLiteral("Bob");
        older.body = QStringLiteral("An older reply in this thread.");
        older.timestamp = QDateTime::currentDateTimeUtc().addSecs(-7200);
        older.type = TimelineEvent::TextMessage;
        older.status = TimelineEvent::Sent;
        older.threadRootId = rootId;
        client.setPaginationChunkForTest({ older });
        client.resetTimelineForTest(threadId, client.timeline(threadId), 2);

        auto *model = controller.model();
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() > 0, kSignalTimeoutMs);
        QCOMPARE(model->rowForStableId(older.eventId), -1);

        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.navigateToEvent(older.eventId);
        QVERIFY(controller.navigating());

        QTRY_COMPARE_WITH_TIMEOUT(located.count(), 1, kSignalTimeoutMs);
        // A backward page lands at the top of the thread timeline.
        QCOMPARE(located.at(0).at(0).toInt(), 0);
        QCOMPARE(model->rowForStableId(older.eventId), 0);
        QCOMPARE(controller.navigationHighlightEventId(), older.eventId);
        QVERIFY(controller.navigationMessage().isEmpty());
        QVERIFY(!controller.navigating());
    }

    // With no reachable history the search refuses immediately and never
    // falls back to the room history loader.
    void navigationForAnUnreachableTargetFailsHonestlyAndNeverJumpsToTheRoom()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 200, 500);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        const int roomRowsBefore = client.timeline(kGeneral).size();
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.navigateToEvent(QStringLiteral("$nowhere:mock.local"));

        QTRY_VERIFY_WITH_TIMEOUT(!controller.navigationMessage().isEmpty(),
                                 kSignalTimeoutMs);
        // The same message the room timeline shows.
        QCOMPARE(controller.navigationMessage(),
                 PaginationController::unavailableTargetMessage());
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationHighlightEventId().isEmpty());
        QVERIFY(!controller.navigating());
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        QVERIFY(!client.paginating(kGeneral));
    }

    // With history available but no target, the search stops at its own
    // budget, not the end of the thread, and reports the message.
    void navigationStopsAtItsBoundedBudgetAndReportsHonestly()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(5);
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 200, 3000);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        const QString threadId = MatrixClient::threadTimelineId(kGeneral,
                                                               rootId);
        // Far more pages than the budget, none carrying the target.
        client.resetTimelineForTest(threadId, client.timeline(threadId), 20);

        auto *model = controller.model();
        const int rowsBefore = model->rowCount();
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.navigateToEvent(QStringLiteral("$never-here:mock.local"));

        QTRY_VERIFY_WITH_TIMEOUT(!controller.navigationMessage().isEmpty(),
                                 kSignalTimeoutMs);
        QCOMPARE(controller.navigationMessage(),
                 PaginationController::unavailableTargetMessage());
        QCOMPARE(located.count(), 0);
        // Exactly three pages of three rows: the budget stopped it, not the
        // end of history.
        QCOMPARE(model->rowCount(), rowsBefore + 9);
        QVERIFY(client.canPaginate(threadId));
        QVERIFY(!controller.navigating());
    }

    // A thread or room change abandons an in-flight search silently: no
    // message about a thread the reader left, no landing in the new one.
    void aThreadOrRoomChangeAbandonsAnInFlightReplySearch()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(120);
        ThreadController controller;
        controller.setClient(&client);
        controller.setNavigationPolicyForTest(3, 200, 3000);

        QString generalRoot;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &generalRoot));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        const QString generalThreadId =
            MatrixClient::threadTimelineId(kGeneral, generalRoot);
        client.resetTimelineForTest(generalThreadId,
                                    client.timeline(generalThreadId), 20);

        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);

        // (1) Switching to a thread in ANOTHER room.
        controller.navigateToEvent(QStringLiteral("$gone-a:mock.local"));
        QVERIFY(controller.navigating());
        const QString devsRoot = firstThreadRootId(client, kDevs);
        QVERIFY(!devsRoot.isEmpty());
        controller.openThread(kDevs, devsRoot);
        QVERIFY(!controller.navigating());
        QVERIFY(controller.navigationHighlightEventId().isEmpty());
        QVERIFY(controller.navigationMessage().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QTest::qWait(250);            // let the abandoned page land
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationMessage().isEmpty());

        // (2) The active room changing out from under the panel.
        const QString devsThreadId =
            MatrixClient::threadTimelineId(kDevs, devsRoot);
        client.resetTimelineForTest(devsThreadId,
                                    client.timeline(devsThreadId), 20);
        controller.navigateToEvent(QStringLiteral("$gone-b:mock.local"));
        QVERIFY(controller.navigating());
        controller.handleCurrentRoomChanged(kGeneral);
        QCOMPARE(controller.state(), ThreadController::Closed);
        QVERIFY(!controller.navigating());
        QVERIFY(controller.navigationMessage().isEmpty());
        QTest::qWait(250);
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationMessage().isEmpty());
    }
};

QTEST_MAIN(ThreadControllerTest)
#include "ThreadControllerTest.moc"
