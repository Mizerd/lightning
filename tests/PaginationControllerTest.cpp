// v0.5.11: deterministic tests for the backward-pagination controller —
// single-flight (including the dispatch-to-loading window), viewport-fill
// budget and no-progress stop, reached-start behavior, retry after failure,
// and stale-result isolation across room switches, timeline resets and
// sign-out.

#include "matrix/MatrixClient.h"
#include "models/PaginationController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

TimelineEvent makeEvent(const QString &eventId)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.roomId = QStringLiteral("!a:example.org");
    e.sender = QStringLiteral("@other:example.org");
    e.body = QStringLiteral("x");
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1000);
    return e;
}

QList<TimelineEvent> makeEvents(int count)
{
    QList<TimelineEvent> events;
    for (int i = 0; i < count; ++i)
        events.append(makeEvent(QStringLiteral("$e%1:example.org").arg(i)));
    return events;
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // Scripted per-room pagination state, mirroring RustSdkMatrixClient.
    struct State {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
    };
    QHash<QString, State> states;
    bool timelineActive = true;
    int loadOlderCalls = 0;
    QString lastLoadRoom;

    // Simulation helpers: the async Rust events, condensed.
    void beginLoading(const QString &roomId)
    {
        states[roomId].loading = true;
        states[roomId].failed = false;
        Q_EMIT paginationStateChanged(roomId);
    }
    void completeBatch(const QString &roomId, int eventCount, bool reachedStart)
    {
        if (eventCount > 0)
            Q_EMIT eventsPrepended(roomId, makeEvents(eventCount));
        states[roomId].loading = false;
        states[roomId].failed = false;
        states[roomId].reachedStart = reachedStart;
        Q_EMIT paginationStateChanged(roomId);
    }
    void failBatch(const QString &roomId)
    {
        states[roomId].loading = false;
        states[roomId].failed = true;
        Q_EMIT paginationStateChanged(roomId);
    }

    // Pagination interface under test.
    void loadOlderMessages(const QString &roomId) override
    {
        ++loadOlderCalls;
        lastLoadRoom = roomId;
    }
    bool canPaginate(const QString &roomId) const override
    {
        if (!timelineActive)
            return false;
        const auto it = states.constFind(roomId);
        if (it == states.constEnd())
            return true;
        return !it->loading && !it->reachedStart;
    }
    bool paginating(const QString &roomId) const override
    {
        const auto it = states.constFind(roomId);
        return it != states.constEnd() && it->loading;
    }
    bool paginationFailed(const QString &roomId) const override
    {
        const auto it = states.constFind(roomId);
        return it != states.constEnd() && it->failed;
    }

    // Remaining pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
};

const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");

} // namespace

class PaginationControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void viewportFillDispatchesOneRequest()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 1);
        QCOMPARE(client.lastLoadRoom, kRoomA);
        QVERIFY(controller.busy());
    }

    void duplicateRequestsSuppressedBeforeLoadingEvent()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        // The "loading" poll event has not arrived yet; the controller's
        // own single-flight must still hold.
        controller.requestViewportFill();
        controller.requestViewportFill();
        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void duplicateRequestsSuppressedWhileLoading()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        controller.requestNearTop();
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void completionReportsInsertedCountAndClearsBusy()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);

        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.first().at(0).toInt(), 3);
        QCOMPARE(completed.first().at(1).toBool(), false);
        QVERIFY(!controller.busy());
    }

    void reachedStartStopsFurtherRequests()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 2, true);
        QVERIFY(controller.reachedStart());

        controller.requestNearTop();
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void emptyBatchesStopAutomaticFillButNotUserRequests()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        for (int i = 0; i < 2; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        QVERIFY(controller.fillStopped());

        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 2); // fill refused

        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 3); // user gesture still allowed
    }

    void progressResetsNoProgressStrikes()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        const int batches[] = { 0, 3, 0 };
        for (int count : batches) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, count, false);
        }
        // Strikes never reached two in a row.
        QVERIFY(!controller.fillStopped());
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 4);
    }

    void fillBudgetIsBounded()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.setMaxViewportFillRequests(2);

        for (int i = 0; i < 2; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 5, false);
        }
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 2);
        QVERIFY(controller.fillStopped());
    }

    void retryAfterTransientFailure()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA);
        QVERIFY(controller.failed());
        QVERIFY(!controller.busy());

        // Automatic fill never retries a failure.
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 1);

        controller.retry();
        QCOMPARE(client.loadOlderCalls, 2);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 4, false);
        QVERIFY(!controller.failed());
    }

    void staleRoomResultIgnoredAfterSwitch()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        controller.setRoomId(kRoomB);
        QVERIFY(!controller.busy());

        // The old room's completion must not surface into the new room.
        client.completeBatch(kRoomA, 7, false);
        QCOMPARE(completed.count(), 0);

        controller.requestNearTop();
        QCOMPARE(client.lastLoadRoom, kRoomB);
        QCOMPARE(client.loadOlderCalls, 2);
    }

    void timelineResetInvalidatesActiveBatch()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        const quint64 generationBefore = controller.generation();
        Q_EMIT client.timelineReset(kRoomA);
        QVERIFY(controller.generation() > generationBefore);

        client.completeBatch(kRoomA, 5, false);
        QCOMPARE(completed.count(), 0);
    }

    void signOutStopsPaginationSafely()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.logout();
        QVERIFY(!controller.busy() || client.paginating(kRoomA));

        // A stale completion after sign-out emits nothing.
        client.completeBatch(kRoomA, 5, false);
        QCOMPARE(completed.count(), 0);
    }

    void refusesRequestsWithoutLiveTimeline()
    {
        FakeClient client;
        client.timelineActive = false;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 0);
        QVERIFY(!controller.busy());
    }

    void adoptsExternallyStartedBatch()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        // Legacy path (TimelineModel::requestOlder) started a batch without
        // the controller; it must still report busy and completion.
        client.beginLoading(kRoomA);
        QVERIFY(controller.busy());
        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 0);

        client.completeBatch(kRoomA, 2, false);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.first().at(0).toInt(), 2);
    }
};

QTEST_GUILESS_MAIN(PaginationControllerTest)
#include "PaginationControllerTest.moc"
