// Thread lifecycle through the deterministic mock backend, which serves thread
// timelines under the same composite timeline-id contract as the Rust backend
// (root first, replies in room order, live reply propagation): open/close,
// root/reply identity, thread and room switches, failure states, and the
// thread-only send path.

#include "matrix/MockMatrixClient.h"
#include "models/AttachmentQueueModel.h"
#include "media/StagedImageStore.h"
#include "models/PaginationController.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"
#include "threads/ThreadManager.h"
#include "crypto/E2eeDiagnostics.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 2000;

const QString kGeneral = QStringLiteral("!general:mock.local");
const QString kDevs = QStringLiteral("!devs:mock.local");
const QString kDm = QStringLiteral("!dm-bob:mock.local");
QStringList *threadTraceLines = nullptr;
QtMessageHandler priorThreadMessageHandler = nullptr;
void captureThreadTrace(QtMsgType type, const QMessageLogContext &context,
                        const QString &message)
{
    if (threadTraceLines && type == QtInfoMsg
        && qstrcmp(context.category, "matrix.thread") == 0)
        threadTraceLines->append(message);
    else if (priorThreadMessageHandler)
        priorThreadMessageHandler(type, context, message);
}
}

/// A mock that accepts a thread voice message and reports how it ended only
/// when the test says so.
class VoiceHoldingClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 sendThreadVoiceMessage(const QString &, const QString &,
                                   const QString &, const QString &, qint64,
                                   const QList<int> &, const QString &) override
    {
        return 777;
    }
};

/// A mock whose thread timeline arrives on the next event-loop turn, as the
/// Rust backend's does, so "before Ready" is observable.
class DeferredThreadClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    void openThread(const QString &roomId, const QString &rootEventId) override
    {
        QTimer::singleShot(0, this, [this, roomId, rootEventId] {
            MockMatrixClient::openThread(roomId, rootEventId);
        });
    }
};

class SpoilerClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    int spoilerSends = 0;
    QString spoilerRoot;
    bool supportsSpoilerMedia() const override { return true; }
    quint64 sendSpoilerMedia(const QString &, const QString &threadRootId,
                             const QString &, const QByteArray &,
                             const QString &, const QString &, const QString &,
                             int, int, qint64, bool, const QByteArray &, int, int,
                             const QString &) override
    {
        ++spoilerSends;
        spoilerRoot = threadRootId;
        return 991;
    }
};

class JoinOpClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 nextJoinOp = 55;
    quint64 joinRoomByIdOrAlias(const QString &, const QStringList &) override
    {
        return nextJoinOp;
    }
};

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
    void completedUploadInParkedThreadReleasesItsStagedImage()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        StagedImageStore staged;
        controller.setClient(&client);
        controller.attachments()->setStagedImages(&staged);
        const QString firstRoot = firstThreadRootId(client, kGeneral);
        const QString secondRoot = firstThreadRootId(client, kDevs);
        QVERIFY(!firstRoot.isEmpty());
        QVERIFY(!secondRoot.isEmpty());
        controller.openThread(kGeneral, firstRoot);
        QVERIFY(controller.attachments()->addImageData("image-bytes", QStringLiteral("image/png"),
                                                        1, 1).isEmpty());
        QCOMPARE(staged.count(), 1);
        auto &entry = controller.attachments()->entries()[0];
        entry.committed = true;
        entry.opId = 991;
        entry.state = QStringLiteral("dispatching");
        entry.threadRootId = firstRoot;
        controller.openThread(kDevs, secondRoot);
        QCOMPARE(controller.attachments()->rowCount(), 0);
        Q_EMIT client.attachmentQueueFinished(991, kGeneral, true, {});
        QCOMPARE(staged.count(), 0);
        controller.openThread(kGeneral, firstRoot);
        QCOMPARE(controller.attachments()->rowCount(), 0);
    }

    void aQueuedPictureStaysWithItsThreadUntilSignOut()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        StagedImageStore staged;
        controller.setClient(&client);
        controller.attachments()->setStagedImages(&staged);
        const QString firstRoot = firstThreadRootId(client, kGeneral);
        const QString secondRoot = firstThreadRootId(client, kDevs);
        controller.openThread(kGeneral, firstRoot);
        QVERIFY(controller.attachments()->addImageData("image-bytes", QStringLiteral("image/png"),
                                                        1, 1).isEmpty());
        controller.attachments()->setSpoilerAt(0, true);

        controller.openThread(kDevs, secondRoot);
        QCOMPARE(controller.attachments()->rowCount(), 0);
        controller.close();
        controller.openThread(kGeneral, firstRoot);
        QCOMPARE(controller.attachments()->rowCount(), 1);
        QCOMPARE(controller.attachments()->entries().first().spoiler,
                 client.supportsSpoilerMedia());
        QCOMPARE(staged.count(), 1);

        controller.close();
        Q_EMIT client.loggedOut();
        QCOMPARE(staged.count(), 0);
    }

    void optInTraceDistinguishesFailureAndStaleResultsWithoutContent()
    {
        QStringList lines;
        threadTraceLines = &lines;
        priorThreadMessageHandler = qInstallMessageHandler(captureThreadTrace);
        const auto restore = qScopeGuard([&] {
            qInstallMessageHandler(priorThreadMessageHandler);
            priorThreadMessageHandler = nullptr;
            threadTraceLines = nullptr;
            qunsetenv("LIGHTNING_THREAD_TRACE");
        });

        MockMatrixClient client;
        QVERIFY(login(client));
        const QString root = firstThreadRootId(client, kGeneral);
        QVERIFY(!root.isEmpty());
        ThreadController ordinary;
        ordinary.setClient(&client);
        ordinary.openThread(kGeneral, root);
        QVERIFY(lines.isEmpty());

        qputenv("LIGHTNING_THREAD_TRACE", "1");
        ThreadController traced;
        traced.setClient(&client);
        traced.openThread(kGeneral, root);
        traced.tracePresentation(1, true, false, 420, 620, 137);
        Q_EMIT client.threadTimelineFailed(kDevs, root,
                                           QStringLiteral("private error /tmp/secret"));
        Q_EMIT client.threadTimelineFailed(kGeneral, root,
                                           QStringLiteral("network"));
        const QString log = lines.join(QLatin1Char('\n'));
        QVERIFY(log.contains(QStringLiteral("reason=stale_failure")));
        QVERIFY(log.contains(QStringLiteral("reason=session_failed")));
        QVERIFY(log.contains(QStringLiteral("generation=1")));
        QVERIFY(log.contains(QStringLiteral("receipt_scope=thread")));
        QVERIFY(log.contains(QStringLiteral(
            "thread_layout generation=1 preset=1 placement=main visible=false viewport=420x620 content_y=137")));
        QVERIFY(log.contains(matrix::e2ee::redactId(kGeneral)));
        QVERIFY(log.contains(matrix::e2ee::redactId(root)));
        QVERIFY(!log.contains(kGeneral));
        QVERIFY(!log.contains(root));
        QVERIFY(!log.contains(QStringLiteral("private error")));
        QVERIFY(!log.contains(QStringLiteral("/tmp/secret")));
    }
    void theRoomThreadUnreadHintFollowsTheOpenRoomAndClearsOnThreadRead()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QCOMPARE(controller.roomThreadUnread(), 0);
        QSignalSpy changed(&controller, &ThreadController::roomThreadUnreadChanged);

        controller.handleCurrentRoomChanged(kGeneral);
        client.setRoomThreadUnreadForTest(kDevs, 2);
        QCOMPARE(controller.roomThreadUnread(), 0);

        client.setRoomThreadUnreadForTest(kGeneral, 1);
        QCOMPARE(controller.roomThreadUnread(), 1);
        client.setRoomThreadUnreadForTest(kGeneral, 2);
        QCOMPARE(controller.roomThreadUnread(), 2);
        QCOMPARE(changed.count(), 2);

        controller.handleCurrentRoomChanged(kDevs);
        QCOMPARE(controller.roomThreadUnread(), 2);
        controller.handleCurrentRoomChanged(kGeneral);
        QCOMPARE(controller.roomThreadUnread(), 2);

        QString root;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &root));
        QTRY_COMPARE(controller.state(), ThreadController::Ready);
        controller.setViewerAtLatest(true);
        controller.markRead();
        QTRY_COMPARE(controller.roomThreadUnread(), 0);
    }

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

    void replyCountIsRepliesNotRows()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));

        int repliesInRoom = 0;
        for (const auto &e : client.timeline(kGeneral))
            if (e.threadRootId == rootId)
                ++repliesInRoom;
        QVERIFY2(repliesInRoom > 0, "fixture assumption: the thread has replies");
        QCOMPARE(controller.replyCount(), repliesInRoom);

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
        QCOMPARE(updated.threadReplyCount, -1);
        updated.threadReplyCount = serverSays;
        client.changeEventAtForTest(composite, rootIndex, updated);

        QCOMPARE(controller.replyCount(), serverSays);
        QCOMPARE(spy.count(), 1);

        client.changeEventAtForTest(composite, rootIndex, updated);
        QCOMPARE(spy.count(), 1);
    }

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

        TimelineEvent stale = thread.at(rootIndex);
        stale.threadReplyCount = loaded - 1;
        client.changeEventAtForTest(composite, rootIndex, stale);

        QCOMPARE(controller.replyCount(), loaded);

        TimelineEvent ahead = stale;
        ahead.threadReplyCount = loaded + 40;
        client.changeEventAtForTest(composite, rootIndex, ahead);
        QCOMPARE(controller.replyCount(), loaded + 40);
    }

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

        TimelineEvent decrypted = thread.at(rootIndex);
        decrypted.body = QStringLiteral("the key finally arrived");
        client.changeEventAtForTest(composite, rootIndex, decrypted);

        QCOMPARE(rootSpy.count(), 1);
        QCOMPARE(controller.rootInfo().value(QStringLiteral("body")).toString(),
                 decrypted.body);
        QCOMPARE(countSpy.count(), 0);

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
        QVERIFY(!controller.supported());
        controller.setClient(&client);
        QVERIFY(controller.supported());
    }

    void aFailedSlashJoinInAThreadIsReportedNotSwallowed()
    {
        JoinOpClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        controller.sendText(QStringLiteral("/join #gone:example.org"));
        QVERIFY(controller.commandError().isEmpty());

        Q_EMIT client.roomJoinFinished(99, false, QString(),
                                       QStringLiteral("not_found"));
        QVERIFY(controller.commandError().isEmpty());

        Q_EMIT client.roomJoinFinished(55, false, QString(),
                                       QStringLiteral("not_found"));
        QVERIFY(controller.commandError().contains(
            QStringLiteral("No room with that address")));

        controller.setText(QString());
        client.nextJoinOp = 0;
        controller.sendText(QStringLiteral("/join #gone:example.org"));
        QVERIFY(controller.commandError().contains(
            QStringLiteral("Could not reach the server")));
    }

    void reopenResubscribesTheOpenThreadInPlace()
    {
        VoiceHoldingClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        controller.reopen();
        QCOMPARE(controller.state(), ThreadController::Closed);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        controller.setText(QStringLiteral("half a reply"));
        QTemporaryFile voice;
        voice.setAutoRemove(false);
        QVERIFY(voice.open());
        voice.write("x");
        voice.close();
        controller.sendVoiceMessage(voice.fileName(),
                                    QStringLiteral("audio/ogg"), 1500, {});
        QVERIFY(QFileInfo::exists(voice.fileName()));

        QSignalSpy stateSpy(&controller, &ThreadController::stateChanged);
        controller.reopen();
        QVERIFY(stateSpy.count() >= 1);
        QCOMPARE(controller.rootEventId(), rootId);
        QCOMPARE(controller.text(), QStringLiteral("half a reply"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(QFileInfo::exists(voice.fileName()));
        Q_EMIT client.attachmentQueueFinished(777, kGeneral, true, QString());
        QVERIFY(!QFileInfo::exists(voice.fileName()));
    }

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
        QVERIFY(stateSpy.count() >= 2);

        auto *model = controller.model();
        QCOMPARE(model->roomId(),
                 MatrixClient::threadTimelineId(kGeneral, rootId));
        QVERIFY(model->rowCount() >= 3);

        int rootRows = 0;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex idx = model->index(row, 0);
            const QString eventId =
                model->data(idx, TimelineModel::EventIdRole).toString();
            const QString threadRootId =
                model->data(idx, TimelineModel::ThreadRootIdRole).toString();
            if (eventId == rootId) {
                ++rootRows;
                QCOMPARE(row, 0);
            } else {
                QCOMPARE(threadRootId, rootId);
            }
        }
        QCOMPARE(rootRows, 1);
    }

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

        controller.handleCurrentRoomChanged(kGeneral);
        QCOMPARE(controller.state(), ThreadController::Ready);

        controller.handleCurrentRoomChanged(kDm);
        QCOMPARE(controller.state(), ThreadController::Closed);
        QCOMPARE(controller.model()->rowCount(), 0);
        QVERIFY(controller.roomId().isEmpty());
    }

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
                QCOMPARE(e.threadRootId, rootId);
            }
        }
        QCOMPARE(roomOccurrences, 1);

        const int rows = controller.model()->rowCount();
        controller.sendText(QStringLiteral("   "));
        QCOMPARE(controller.model()->rowCount(), rows);
    }

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

        controller.beginReply(QStringLiteral("$not-in-thread:mock.local"));
        QVERIFY(!controller.inReply());

        controller.beginReply(rootId);
        QVERIFY(!controller.inReply());

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
        QVERIFY(!controller.inReply());

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
        QVERIFY(!controller.followSupported());
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
    }

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
        QCOMPARE(controller.threadList().size(), 1);
        const QVariantMap entry = controller.threadList().first().toMap();
        QCOMPARE(entry.value(QStringLiteral("rootEventId")).toString(),
                 firstThreadRootId(client, kGeneral));
        QCOMPARE(entry.value(QStringLiteral("replyCount")).toInt(), 2);
        QVERIFY(!entry.value(QStringLiteral("latestPreview"))
                     .toString().isEmpty());

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

        controller.setViewerAtLatest(true);
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

    void markReadNowSendsTheThreadReceiptOnRequest()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        controller.setViewerAtLatest(false);
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 0);
        QVERIFY(QMetaObject::invokeMethod(&controller, "markReadNow"));
        QCOMPARE(client.markThreadReadCallsForTest(), 1);
        QVERIFY(QMetaObject::invokeMethod(&controller, "markReadNow"));
        QCOMPARE(client.markThreadReadCallsForTest(), 1);
    }

    void threadReadReachesTheBellOnlyWhenTheServerAccepts()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setAutoAcceptThreadReadsForTest(false);
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QSignalSpy sent(&controller, &ThreadController::threadReadSent);

        controller.setViewerAtLatest(true);
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 1);
        QCOMPARE(sent.count(), 0);
        const quint64 first = client.threadReadOpIdsForTest().at(0);

        Q_EMIT client.threadMarkReadFailed(kGeneral, rootId, first);
        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, first);
        QCOMPARE(sent.count(), 0);

        controller.sendText(QStringLiteral("advance latest"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 4, kSignalTimeoutMs);
        controller.markRead();
        QCOMPARE(sent.count(), 0);
        const quint64 second = client.threadReadOpIdsForTest().at(1);
        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, second + 100);
        QCOMPARE(sent.count(), 0);
        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, second);
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.first().at(0).toString(), kGeneral);
        QCOMPARE(sent.first().at(1).toString(), rootId);
    }

    void threadReadAcksAreMatchedToTheirOwnRequestInAnyOrder()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setAutoAcceptThreadReadsForTest(false);
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QSignalSpy sent(&controller, &ThreadController::threadReadSent);

        qint64 ms1 = 0;
        controller.model()->latestReadableEventId(&ms1);
        controller.setViewerAtLatest(true);
        controller.markRead();
        controller.sendText(QStringLiteral("second"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 4, kSignalTimeoutMs);
        qint64 ms2 = 0;
        controller.model()->latestReadableEventId(&ms2);
        QVERIFY2(ms2 > ms1, "the fixture's second reply is newer");
        controller.markRead();
        const QList<quint64> ops = client.threadReadOpIdsForTest();
        QCOMPARE(ops.size(), 2);
        QVERIFY(ops.at(0) != ops.at(1));

        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, ops.at(1));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.at(0).at(2).toLongLong(), ms2);
        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, ops.at(0));
        QCOMPARE(sent.count(), 2);
        QCOMPARE(sent.at(1).at(2).toLongLong(), ms1);
    }

    void aFailedThreadReadKeepsTheOtherAndIsRetried()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setAutoAcceptThreadReadsForTest(false);
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QSignalSpy sent(&controller, &ThreadController::threadReadSent);

        controller.setViewerAtLatest(true);
        controller.markRead();
        controller.sendText(QStringLiteral("second"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 4, kSignalTimeoutMs);
        qint64 ms2 = 0;
        controller.model()->latestReadableEventId(&ms2);
        controller.markRead();
        const QList<quint64> ops = client.threadReadOpIdsForTest();
        Q_EMIT client.threadReadMarkerAdvanced(kGeneral, rootId, ops.at(1));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent.at(0).at(2).toLongLong(), ms2);
        Q_EMIT client.threadMarkReadFailed(kGeneral, rootId, ops.at(0));
        QCOMPARE(sent.count(), 1);

        const int callsBefore = client.markThreadReadCallsForTest();
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), callsBefore);
        controller.sendText(QStringLiteral("third"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 5, kSignalTimeoutMs);
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), callsBefore + 1);
        Q_EMIT client.threadMarkReadFailed(
            kGeneral, rootId, client.threadReadOpIdsForTest().last());
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), callsBefore + 2);
    }

    void aBackendWithoutThreadReceiptsAcceptsImmediately()
    {
        MockMatrixClient client;   // auto-accept is the mock's default
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QSignalSpy sent(&controller, &ThreadController::threadReadSent);
        controller.setViewerAtLatest(true);
        controller.markRead();
        QCOMPARE(sent.count(), 1);
    }

    void markReadIsANoOpWhileTheViewerIsNotAtTheLatestReply()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(!controller.viewerAtLatest());
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 0);

        controller.setViewerAtLatest(true);
        QCOMPARE(client.markThreadReadCallsForTest(), 1);

        controller.setViewerAtLatest(false);
        controller.sendText(QStringLiteral("while away"));
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.model()->rowCount() >= 4, kSignalTimeoutMs);
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 1);

        controller.setViewerAtLatest(true);
        QCOMPARE(client.markThreadReadCallsForTest(), 2);
        controller.markRead();
        QCOMPARE(client.markThreadReadCallsForTest(), 2);
    }

    void viewerAtLatestHoldsOnlyWhileTheThreadIsReady()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QSignalSpy changed(&controller, &ThreadController::viewerAtLatestChanged);
        controller.setViewerAtLatest(true);
        QVERIFY(!controller.viewerAtLatest());
        QCOMPARE(changed.count(), 0);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(controller.viewerAtLatest());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(client.markThreadReadCallsForTest(), 1);

        controller.close();
        QVERIFY(!controller.viewerAtLatest());
        QCOMPARE(changed.count(), 2);
    }

    void openThreadAtLocatesTheReplyOnceReady()
    {
        DeferredThreadClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        QString replyId;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.threadRootId == rootId && e.eventId != rootId) {
                replyId = e.eventId;
                break;
            }
        }
        QVERIFY2(!replyId.isEmpty(), "the fixture thread has a reply");
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);

        controller.openThreadAt(kGeneral, rootId, replyId);
        QCOMPARE(located.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(0).toInt(),
                 controller.model()->rowForStableId(replyId));
        QCOMPARE(controller.navigationHighlightEventId(), replyId);

        controller.openThreadAt(kGeneral, rootId, replyId);
        QCOMPARE(located.count(), 2);
    }

    void openThreadAtLandsWhenTheBackendOpensSynchronously()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        QString replyId;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.threadRootId == rootId && e.eventId != rootId) {
                replyId = e.eventId;
                break;
            }
        }
        QVERIFY(!replyId.isEmpty());
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.openThreadAt(kGeneral, rootId, replyId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(controller.navigationHighlightEventId(), replyId);
    }

    void openThreadAtDropsTheLandingWhenTheThreadIsReplaced()
    {
        DeferredThreadClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        QString replyId;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.threadRootId == rootId && e.eventId != rootId) {
                replyId = e.eventId;
                break;
            }
        }
        QVERIFY(!replyId.isEmpty());
        QSignalSpy located(&controller,
                           &ThreadController::navigationTargetLocated);
        controller.openThreadAt(kGeneral, rootId, replyId);
        controller.close();
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QTest::qWait(20);
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationHighlightEventId().isEmpty());
    }

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

        TimelineModel unbound;
        unbound.setClient(&client);
        unbound.retryDecryption();
        QCOMPARE(client.decryptionRetryRoomsForTest().size(), 2);
    }

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
        QVERIFY(MatrixClient::isThreadTimelineId(model->roomId()));

        const QString replyId =
            model->data(model->index(1, 0), TimelineModel::EventIdRole)
                .toString();
        QVERIFY(!replyId.isEmpty());

        const QString link = model->messagePermalink(replyId);
        QVERIFY(link.startsWith(QStringLiteral("https://matrix.to/#/")));
        QVERIFY(!link.contains(QStringLiteral("thread")));
        QVERIFY(!link.contains(QChar(0x1f)));
        QVERIFY(!link.contains(QStringLiteral("%1F")));
        QVERIFY(link.contains(QStringLiteral("general")));

        const QVariantMap details = model->messageDetails(replyId);
        QCOMPARE(details.value(QStringLiteral("roomId")).toString(), kGeneral);
    }

    void aThreadSpoilerTakesTheSpoilerPathWithItsRoot()
    {
        SpoilerClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QTemporaryFile img(QDir::tempPath()
                           + QStringLiteral("/lightning-XXXXXX.png"));
        QVERIFY(img.open());
        QImage(4, 4, QImage::Format_RGB32).save(img.fileName(), "PNG");
        controller.addAttachment(QUrl::fromLocalFile(img.fileName()));
        controller.attachments()->setSpoilerAt(0, true);
        QVERIFY(controller.attachments()->data(controller.attachments()->index(0),
                    AttachmentQueueModel::SpoilerRole).toBool());
        controller.sendText(QString{});
        QCOMPARE(client.spoilerSends, 1);
        QCOMPARE(client.spoilerRoot, rootId);
        QCOMPARE(client.threadAttachmentCallsForTest(), 0);
    }

    void threadAttachmentSendsIntoThread()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QVERIFY(controller.attachmentsSupported() == false);

        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QVERIFY(controller.attachmentsSupported());

        QTemporaryFile img(QDir::tempPath()
                           + QStringLiteral("/lightning-XXXXXX.png"));
        QVERIFY(img.open());
        QImage(4, 4, QImage::Format_RGB32).save(img.fileName(), "PNG");
        controller.addAttachment(QUrl::fromLocalFile(img.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);
        QVERIFY(controller.hasAttachments());

        const int threadRowsBefore = controller.model()->rowCount();
        controller.sendText(QString{});
        QCOMPARE(client.threadAttachmentCallsForTest(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(controller.model()->rowCount(),
                                  threadRowsBefore + 1, kSignalTimeoutMs);
        const QModelIndex last =
            controller.model()->index(controller.model()->rowCount() - 1, 0);
        QCOMPARE(controller.model()
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QVERIFY(controller.model()->data(last, TimelineModel::IsImageRole)
                    .toBool());

        int roomImages = 0;
        for (const auto &e : client.timeline(kGeneral)) {
            if (e.type == TimelineEvent::Image
                && e.body == QFileInfo(img.fileName()).fileName()) {
                ++roomImages;
                QCOMPARE(e.threadRootId, rootId);
            }
        }
        QCOMPARE(roomImages, 1);

        QTRY_COMPARE_WITH_TIMEOUT(controller.attachments()->rowCount(), 0,
                                  kSignalTimeoutMs);
        QVERIFY(!controller.hasAttachments());
    }

    void aThreadPhotoLeavesWithoutItsLocation()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString rootId = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QByteArray jpeg;
        {
            QBuffer buffer(&jpeg);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "JPG", 80);
        }
        const QByteArray marker(24, '\xAB');
        QByteArray t("MM\0*\0\0\0\x08", 8);
        t.append("\0\1", 2);
        t.append(QByteArray("\x88\x25\0\4\0\0\0\1\0\0\0\x1A", 12));
        t.append("\0\0\0\0", 4);
        t.append("\0\1", 2);
        t.append(QByteArray("\0\2\0\5\0\0\0\3\0\0\0\x2C", 12));
        t.append("\0\0\0\0", 4);
        t.append(marker);
        const QByteArray payload = QByteArray("Exif\0\0", 6) + t;
        QByteArray segment("\xFF\xE1", 2);
        segment.append(char((payload.size() + 2) >> 8));
        segment.append(char((payload.size() + 2) & 0xFF));
        segment.append(payload);
        jpeg.insert(2, segment);
        QVERIFY(jpeg.contains(marker));

        QTemporaryFile file(QDir::tempPath()
                            + QStringLiteral("/lightning-XXXXXX.jpg"));
        QVERIFY(file.open());
        file.write(jpeg);
        file.flush();
        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);
        controller.sendText(QString{});
        QCOMPARE(client.threadAttachmentCallsForTest(), 1);
        const QByteArray sent = client.lastThreadAttachmentBytesForTest();
        QVERIFY(!sent.isEmpty());
        QVERIFY(!sent.contains(marker));
        QImage decoded;
        QVERIFY(decoded.loadFromData(sent));

        QTRY_COMPARE_WITH_TIMEOUT(controller.attachments()->rowCount(), 0,
                                  kSignalTimeoutMs);
        controller.setImageMetadataMode(QStringLiteral("keep"));
        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));
        controller.sendText(QString{});
        QCOMPARE(client.threadAttachmentCallsForTest(), 2);
        QVERIFY(client.lastThreadAttachmentBytesForTest().contains(marker));
    }

    void threadAttachmentWithReplyTargetKeepsTheReplyAndTheThread()
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

        QTemporaryFile img(QDir::tempPath()
                           + QStringLiteral("/lightning-XXXXXX.png"));
        QVERIFY(img.open());
        QImage(4, 4, QImage::Format_RGB32).save(img.fileName(), "PNG");
        controller.addAttachment(QUrl::fromLocalFile(img.fileName()));
        controller.beginReply(replyId);
        QVERIFY(controller.inReply());

        const int rowsBefore = model->rowCount();
        controller.sendText(QString{});
        QCOMPARE(client.threadAttachmentCallsForTest(), 1);
        QCOMPARE(client.lastAttachmentReplyToForTest(), replyId);
        QVERIFY(!controller.inReply());

        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), rowsBefore + 1,
                                  kSignalTimeoutMs);
        const QModelIndex last = model->index(model->rowCount() - 1, 0);
        QCOMPARE(model->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QCOMPARE(model->data(last, TimelineModel::ReplyToEventIdRole).toString(),
                 replyId);
    }

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
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        QCOMPARE(controller.attachments()->rowCount(), 1);
        const QModelIndex idx = controller.attachments()->index(0, 0);
        QCOMPARE(controller.attachments()
                     ->data(idx, AttachmentQueueModel::StateRole).toString(),
                 QStringLiteral("failed"));
    }

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

        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        client.sendThreadReply(kGeneral, secondRoot, QStringLiteral("seed"));
        controller.openThread(kGeneral, secondRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        QCOMPARE(controller.attachments()->rowCount(), 0);
        QCOMPARE(client.threadAttachmentCallsForTest(), 0);

        controller.addAttachment(QUrl::fromLocalFile(file.fileName()));
        QCOMPARE(controller.attachments()->rowCount(), 1);
        controller.close();
        QCOMPARE(controller.attachments()->rowCount(), 0);
    }

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

    void threadParticipantsAreCachedDedupedAndAccountScoped()
    {
        MockMatrixClient client;
        ThreadManager threads;
        threads.setClient(&client);

        const QString root = QStringLiteral("$root:example.org");

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

        QVERIFY(threads.participants(
                    kGeneral, QStringLiteral("$other:example.org")).isEmpty());

        const QString root2 = QStringLiteral("$second:example.org");
        Q_EMIT client.threadParticipantsReceived(kGeneral, root2, {}, 0, false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(threads.participants(kGeneral, root2).isEmpty());

        client.logout();
        QVERIFY(threads.participants(kGeneral, root).isEmpty());
    }

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
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.navigationHighlightEventId().isEmpty(), kSignalTimeoutMs);
    }

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
        QCOMPARE(located.count(), 0);
        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(controller.state(), ThreadController::Ready);
        QCOMPARE(controller.roomId(), kGeneral);
        QCOMPARE(controller.rootEventId(), rootId);
        QVERIFY(controller.navigationMessage().isEmpty());
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        QVERIFY(!client.paginating(kGeneral));
    }

    void navigateToAnUnloadedReplyPaginatesTheThreadTimeline()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(10);
        ThreadController controller;
        controller.setClient(&client);
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
        QCOMPARE(located.at(0).at(0).toInt(), 0);
        QCOMPARE(model->rowForStableId(older.eventId), 0);
        QCOMPARE(controller.navigationHighlightEventId(), older.eventId);
        QVERIFY(controller.navigationMessage().isEmpty());
        QVERIFY(!controller.navigating());
    }

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
        QCOMPARE(controller.navigationMessage(),
                 PaginationController::unavailableTargetMessage());
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationHighlightEventId().isEmpty());
        QVERIFY(!controller.navigating());
        QCOMPARE(client.timeline(kGeneral).size(), roomRowsBefore);
        QVERIFY(!client.paginating(kGeneral));
    }

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
        QCOMPARE(model->rowCount(), rowsBefore + 9);
        QVERIFY(client.canPaginate(threadId));
        QVERIFY(!controller.navigating());
    }

    void anOpenThreadMissingItsRootFillsHistoryAndStops()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(5);
        ThreadController controller;
        controller.setClient(&client);

        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        const QString threadId = MatrixClient::threadTimelineId(kGeneral,
                                                               rootId);
        QList<TimelineEvent> replies = client.timeline(threadId);
        QVERIFY(!replies.isEmpty());
        QCOMPARE(replies.first().eventId, rootId);
        replies.removeFirst();
        auto *model = controller.model();

        client.resetTimelineForTest(threadId, replies, 3);
        const int before = model->rowCount();
        QTRY_VERIFY_WITH_TIMEOUT(!client.canPaginate(threadId)
                                     && !client.paginating(threadId),
                                 kSignalTimeoutMs);
        QCOMPARE(model->rowCount(), before + 9);

        client.resetTimelineForTest(threadId, replies, 40);
        const int again = model->rowCount();
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= again + 24,
                                 kSignalTimeoutMs);
        QTest::qWait(150);
        QCOMPARE(model->rowCount(), again + 24);
        QVERIFY(client.canPaginate(threadId));
    }

    void aMissingRootIsPendingWhileTheFillRunsThenUnavailable()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.setPaginationDelayForTest(5);
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        client.resetTimelineForTest(kGeneral, {}, 0);
        const QString threadId = MatrixClient::threadTimelineId(kGeneral,
                                                               rootId);
        QList<TimelineEvent> replies = client.timeline(threadId);
        replies.removeFirst();

        client.resetTimelineForTest(threadId, replies, 40);
        QVERIFY(controller.rootInfo().value(QStringLiteral("pending")).toBool());
        QVERIFY(!controller.rootInfo().value(QStringLiteral("loaded")).toBool());

        QSignalSpy rootSpy(&controller, &ThreadController::rootInfoChanged);
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.rootInfo().value(QStringLiteral("pending")).toBool(),
            kSignalTimeoutMs);
        QVERIFY(rootSpy.count() >= 1);
        QVERIFY(!controller.rootInfo().value(QStringLiteral("loaded")).toBool());
    }

    void theThreadListNamesARootTheTimelinesHaveNotReached()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        controller.openList(kGeneral);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.threadList().isEmpty(),
                                 kSignalTimeoutMs);
        const QVariantMap entry = controller.threadList().first().toMap();
        const QString listed =
            entry.value(QStringLiteral("rootEventId")).toString();
        controller.openThread(kGeneral, listed);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        client.setPaginationDelayForTest(5);
        client.resetTimelineForTest(kGeneral, {}, 0);
        const QString threadId = MatrixClient::threadTimelineId(kGeneral, listed);
        QList<TimelineEvent> replies = client.timeline(threadId);
        replies.removeFirst();
        client.resetTimelineForTest(threadId, replies, 40);

        const QVariantMap info = controller.rootInfo();
        QVERIFY(info.value(QStringLiteral("loaded")).toBool());
        QCOMPARE(info.value(QStringLiteral("body")).toString(),
                 entry.value(QStringLiteral("rootPreview")).toString());
    }

    void anOpenThreadWithItsRootIsNotFilled()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        QString rootId;
        QVERIFY(openFixtureThread(client, controller, kGeneral, &rootId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        const QString threadId = MatrixClient::threadTimelineId(kGeneral,
                                                               rootId);
        client.resetTimelineForTest(threadId, client.timeline(threadId), 5);
        const int rows = controller.model()->rowCount();
        QTest::qWait(150);
        QCOMPARE(controller.model()->rowCount(), rows);
        QVERIFY(!client.paginating(threadId));
        QVERIFY(client.canPaginate(threadId));
    }

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
        QTest::qWait(250);
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationMessage().isEmpty());

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

    void aThreadDropResolvesAndAStaleAnswerIsDropped()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
        });
        std::function<void(bool, const QStringList &)> pending;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &,
                std::function<void(bool, const QStringList &)> done) {
                pending = std::move(done);
            });
        QTemporaryDir dir;
        const QString exported = dir.filePath(QStringLiteral("drop.bin"));
        {
            QFile f(exported);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(16, 'd'));
        }
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("host/drop.bin")));

        MockMatrixClient client;
        QVERIFY(login(client));
        ThreadController controller;
        controller.setClient(&client);
        const QString firstRoot = firstThreadRootId(client, kGeneral);
        controller.openThread(kGeneral, firstRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);

        controller.addDroppedAttachments({hostPath}, QStringLiteral("k1"));
        QVERIFY(pending);
        pending(true, {exported});
        QCOMPARE(controller.attachments()->rowCount(), 1);
        controller.attachments()->clearAll();

        pending = nullptr;
        controller.addDroppedAttachments({hostPath}, QStringLiteral("k2"));
        QVERIFY(pending);
        const QString secondRoot = client.timeline(kGeneral).first().eventId;
        QVERIFY(secondRoot != firstRoot);
        client.sendThreadReply(kGeneral, secondRoot, QStringLiteral("x"));
        controller.openThread(kGeneral, secondRoot);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), ThreadController::Ready,
                                  kSignalTimeoutMs);
        pending(true, {exported});
        QCOMPARE(controller.attachments()->rowCount(), 0);

        pending = nullptr;
        controller.addDroppedAttachments({hostPath}, QStringLiteral("k3"));
        QVERIFY(pending);
        client.logout();
        pending(true, {exported});
        QCOMPARE(controller.attachments()->rowCount(), 0);
    }
};

QTEST_MAIN(ThreadControllerTest)
#include "ThreadControllerTest.moc"
