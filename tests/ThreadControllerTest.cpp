// v0.6.0 checkpoint 2: SDK-backed thread foundation, exercised through the
// deterministic mock backend. The mock serves thread timelines under the
// same composite timeline-id contract as the Rust backend (root first,
// replies in room order, live reply propagation), so these tests pin the
// lifecycle rules — open/close, root/reply identity, thread and room
// switches, failure states, and the thread-only send path — without a
// homeserver.

#include "matrix/MockMatrixClient.h"
#include "models/AttachmentQueueModel.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

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

    // ── v0.6.0 checkpoint 5: follow state, thread list, threaded read ───

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

        // Persistence across reopen (mock's map is the stand-in for the
        // MSC4306 server state).
        controller.setFollowed(true);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
        controller.close();
        QVERIFY(!controller.followSupported());   // reset while closed
        controller.openThread(kGeneral, rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.followed(), true,
                                  kSignalTimeoutMs);
    }

    // The Threads view lists the room's threads sorted by latest activity,
    // updates live on new replies, and closes on room switch.
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

    // markRead sends exactly one threaded receipt per new latest reply —
    // repeated calls (delegate churn, repeated settle events) deduplicate.
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

        // Repeated calls with an unchanged latest reply send exactly ONE
        // threaded receipt; a new reply re-arms exactly one more.
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

    // ── v0.6.0 checkpoint 8: manual decryption retry dispatch ───────────
    // Both the room model and the thread model dispatch retryDecryption to
    // the backend with their own timeline id (the Rust backend maps a
    // composite thread id onto its parent room and retries both timelines).
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

    // v0.6.1: Copy message link / message details on a thread reply must use
    // the REAL room id, never the internal composite thread-timeline id (which
    // embeds a unit separator + "thread" marker and would break the permalink
    // and leak the internal scheme into the details dialog).
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
        // The model is bound to the composite thread timeline id …
        QVERIFY(MatrixClient::isThreadTimelineId(model->roomId()));

        // … but a reply's permalink and details resolve the real room.
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

    // ── v0.6.1: thread attachment sending ───────────────────────────────

    // A queued file attachment sends into the OPEN thread: it lands in the
    // thread timeline AND the room timeline with the correct thread root, as
    // an Image/File (never an ordinary room message), and the tray clears once
    // the SDK send queue accepts it.
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

        // Local echo appears in the thread with the correct root and an image.
        QTRY_COMPARE_WITH_TIMEOUT(controller.model()->rowCount(),
                                  threadRowsBefore + 1, kSignalTimeoutMs);
        const QModelIndex last =
            controller.model()->index(controller.model()->rowCount() - 1, 0);
        QCOMPARE(controller.model()
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QVERIFY(controller.model()->data(last, TimelineModel::IsImageRole)
                    .toBool());

        // The room timeline saw exactly one copy, threaded (never a room msg).
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

    // A failed queue attempt leaves the entry as a retryable "failed" tray
    // item — never an immortal spinner, never a room-timeline fallback.
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

    // Switching threads or closing the panel before sending discards queued
    // attachments so they can never reach the new thread or the room.
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

        // Open a different thread — queued attachment is dropped, not resent.
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

    // v0.6.1 (E2EE): an undecryptable thread reply recovers IN PLACE when the
    // key arrives — the SDK emits an item update (eventChangedAt) on the
    // thread timeline, and the same row becomes decrypted with no duplicate,
    // stable identity, and its thread root preserved. No room-timeline
    // fallback, no reopen.
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

        // The key arrives: the SDK replaces the item in place. Build the
        // decrypted event from the mock's thread copy, same identity.
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

        // Same row, decrypted in place — no duplicate, identity preserved,
        // thread root intact.
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
};

QTEST_MAIN(ThreadControllerTest)
#include "ThreadControllerTest.moc"
