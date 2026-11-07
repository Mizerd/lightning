
#include "matrix/MatrixClient.h"
#include "models/PaginationController.h"
#include "models/TimelineModel.h"

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

    struct State {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
    };
    QHash<QString, State> states;
    bool timelineActive = true;
    bool failureTransient = false;
    bool fullyFiltered = false;
    int loadOlderCalls = 0;
    QString lastLoadRoom;
    QHash<QString, QList<TimelineEvent>> timelines;

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
        QCoreApplication::processEvents();
    }
    void completeEvents(const QString &roomId,
                        const QList<TimelineEvent> &events,
                        bool reachedStart)
    {
        if (!events.isEmpty())
            Q_EMIT eventsPrepended(roomId, events);
        states[roomId].loading = false;
        states[roomId].failed = false;
        states[roomId].reachedStart = reachedStart;
        Q_EMIT paginationStateChanged(roomId);
        QCoreApplication::processEvents();
    }
    void insertUnattributed(const QString &roomId, int count)
    {
        for (int i = 0; i < count; ++i)
            Q_EMIT eventInsertedAt(
                roomId, 1,
                makeEvent(QStringLiteral("$unattributed%1:example.org").arg(i)));
    }
    void failBatch(const QString &roomId, bool transient = false)
    {
        states[roomId].loading = false;
        states[roomId].failed = true;
        failureTransient = transient;
        Q_EMIT paginationStateChanged(roomId);
    }

    void loadOlderMessages(const QString &roomId) override
    {
        ++loadOlderCalls;
        lastLoadRoom = roomId;
        states[roomId].failed = false;
        failureTransient = false;
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
    bool paginationReady(const QString &) const override { return timelineActive; }
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
    bool paginationFailureTransient(const QString &roomId) const override
    {
        return paginationFailed(roomId) && failureTransient;
    }
    bool lastPaginationFullyFiltered(const QString &roomId) const override
    {
        Q_UNUSED(roomId);
        return fullyFiltered;
    }

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
    QList<TimelineEvent> timeline(const QString &roomId) const override
    { return timelines.value(roomId); }
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

}

class PaginationControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roomOpenIntentWaitsForTimelineReadiness()
    {
        FakeClient client;
        client.timelineActive = false;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        QCOMPARE(controller.initialHistoryState(),
                 PaginationController::WaitingForTimeline);
        QCOMPARE(client.loadOlderCalls, 0);

        client.timelineActive = true;
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(client.loadOlderCalls, 1);
        QCOMPARE(controller.initialHistoryState(),
                 PaginationController::LoadingInitialHistory);
    }

    void timelineResetPreservesInitialFillIntent()
    {
        FakeClient client;
        client.timelineActive = false;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        client.timelineActive = true;
        Q_EMIT client.timelineReset(kRoomA);
        QCOMPARE(client.loadOlderCalls, 0);
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void moreHistoryHoldsWhileAPageLoads()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QVERIFY(controller.moreHistory());
        QSignalSpy changed(&controller, &PaginationController::stateChanged);
        client.beginLoading(kRoomA);
        QVERIFY(!client.canPaginate(kRoomA));
        QVERIFY(controller.moreHistory());
        client.completeBatch(kRoomA, 3,false);
        QVERIFY(controller.moreHistory());
        client.beginLoading(kRoomA);
        QVERIFY(controller.moreHistory());
        client.completeBatch(kRoomA, 2,true);
        QVERIFY(!controller.moreHistory());
        QVERIFY(controller.reachedStart());
        QVERIFY(changed.count() > 0);
        client.beginLoading(kRoomA);
        QVERIFY(!controller.moreHistory());
    }

    void whatQmlHoldsForTheHistoryFlagsIsNeverStale()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        bool seenStart = controller.reachedStart();
        bool seenMore = controller.moreHistory();
        connect(&controller, &PaginationController::stateChanged, &controller,
                [&] {
                    seenStart = controller.reachedStart();
                    seenMore = controller.moreHistory();
                });
        const auto inSync = [&] {
            return seenStart == controller.reachedStart()
                && seenMore == controller.moreHistory();
        };
        const auto note = [&](const QString &room) {
            Q_EMIT client.paginationStateChanged(room);
            QCoreApplication::processEvents();
        };

        controller.setRoomId(kRoomA);
        note(kRoomA);
        QVERIFY(inSync());
        QVERIFY(seenMore);

        client.states[kRoomB].reachedStart = true;
        controller.setRoomId(kRoomB);
        QVERIFY(inSync());
        note(kRoomB);
        QVERIFY(inSync());
        QVERIFY(seenStart);
        client.timelineActive = false;
        note(kRoomB);
        QVERIFY2(inSync(), "B stopped knowing and QML still says at the start");
        client.timelineActive = true;
        note(kRoomB);
        QVERIFY(inSync());
        client.states[kRoomB].reachedStart = false;
        note(kRoomB);
        QVERIFY2(inSync(), "B gained history and QML was not told");
        QVERIFY(seenMore);
        controller.setRoomId(kRoomA);
        QVERIFY(inSync());
        client.states[kRoomB].reachedStart = true;
        controller.setRoomId(kRoomB);
        QVERIFY(inSync());
        client.states[kRoomB].reachedStart = false;
        note(kRoomB);
        QVERIFY2(inSync(), "after a switch back, B gained history and QML was "
                           "not told");
        Q_EMIT client.loggedOut();
        QVERIFY(inSync());
    }

    void transientInitialFailureRetriesAutomatically()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setAutomaticRetryPolicyForTest(3, 1);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);
        QCOMPARE(controller.initialHistoryState(),
                 PaginationController::WaitingForAutomaticRetry);
        QVERIFY(!controller.failed());
        QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, 2, 1000);

        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        QVERIFY(!controller.failed());
        QCOMPARE(controller.initialHistoryState(),
                 PaginationController::InitialHistorySettled);
    }

    void automaticRetriesAreBoundedThenManualRetryWorks()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setAutomaticRetryPolicyForTest(2, 1);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);
        QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, 2, 1000);
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);
        QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, 3, 1000);
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);

        QVERIFY(controller.failed());
        QCOMPARE(controller.initialHistoryState(),
                 PaginationController::ManualRetryRequired);
        QTest::qWait(20);
        QCOMPARE(client.loadOlderCalls, 3);

        controller.retry();
        QCOMPARE(client.loadOlderCalls, 4);
    }

    void terminalInitialFailureDoesNotAutoRetry()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setAutomaticRetryPolicyForTest(3, 1);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, false);
        QTest::qWait(20);
        QCOMPARE(client.loadOlderCalls, 1);
        QVERIFY(controller.failed());
    }

    void roomSwitchCancelsAutomaticRetry()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setAutomaticRetryPolicyForTest(3, 20);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);
        controller.setRoomId(kRoomB);
        QTest::qWait(80);
        QCOMPARE(client.loadOlderCalls, 1);
        QCOMPARE(controller.roomId(), kRoomB);
    }

    void signOutCancelsAutomaticRetry()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setAutomaticRetryPolicyForTest(3, 20);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.failBatch(kRoomA, true);
        client.logout();
        QTest::qWait(80);
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void logicalPresentationState()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);

        controller.setRoomId(kRoomA);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
        controller.requestNearTop();
        QCOMPARE(controller.presentationState(), PaginationController::Loading);
        client.beginLoading(kRoomA);
        QCOMPARE(controller.presentationState(), PaginationController::Loading);
        client.failBatch(kRoomA);
        QCOMPARE(controller.presentationState(), PaginationController::Failed);
        controller.retry();
        QCOMPARE(controller.presentationState(), PaginationController::Loading);
        controller.setRoomId(kRoomB);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
    }

    void logicalPresentationStateCoversReadinessAndHistoryEnd()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        client.timelineActive = false;
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
        client.timelineActive = true;
        client.states[kRoomA].reachedStart = true;
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
    }
    void viewportFillDefersUntilTimelineReady()
    {
        FakeClient client;
        client.timelineActive = false;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 0);
        QVERIFY(!controller.failed());
        client.timelineActive = true;
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void roomSwitchClearsDeferredFill()
    {
        FakeClient client;
        client.timelineActive = false;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        controller.setRoomId(kRoomB);
        client.timelineActive = true;
        Q_EMIT client.paginationStateChanged(kRoomA);
        QCOMPARE(client.loadOlderCalls, 0);
    }

    void duplicateStableIdsAreNotCountedTwice()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);
        controller.requestNearTop();
        client.beginLoading(kRoomA);
        const auto events = makeEvents(2);
        Q_EMIT client.eventsPrepended(kRoomA, events);
        Q_EMIT client.eventsPrepended(kRoomA, events);
        client.completeBatch(kRoomA, 0, false);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.first().at(0).toInt(), 2);
    }

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

    void reachedStartWithoutLoadingNotificationClearsPresentation_data()
    {
        QTest::addColumn<int>("inserted");
        QTest::newRow("empty-final-page") << 0;
        QTest::newRow("final-page-with-rows") << 3;
    }

    void reachedStartWithoutLoadingNotificationClearsPresentation()
    {
        QFETCH(int, inserted);
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.setRequestWatchdogForTest(0);
        QSignalSpy completed(&controller,
                             &PaginationController::paginationCompleted);
        QSignalSpy changed(&controller, &PaginationController::stateChanged);

        controller.requestViewportFill();
        QCOMPARE(controller.presentationState(), PaginationController::Loading);
        changed.clear();
        client.completeBatch(kRoomA, inserted, true);
        QVERIFY(!model.paginating());
        QVERIFY(!model.canPaginate());
        QVERIFY(controller.reachedStart());
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 1000);
        QCOMPARE(completed.first().at(0).toInt(), inserted);
        QVERIFY(completed.first().at(1).toBool());
        QVERIFY(!completed.first().at(2).toBool());
        QVERIFY(!controller.busy());
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
        QVERIFY(!changed.isEmpty());
        controller.requestViewportFill();
        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void duplicateRequestsSuppressedBeforeLoadingEvent()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        controller.requestViewportFill();
        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 1);
    }

    void aBatchTheBackendNeverAcknowledgesIsAbandoned()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRequestWatchdogForTest(20);
        controller.setRoomId(kRoomA);

        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 1);
        QVERIFY(controller.busy());

        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);

        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 2);
    }

    void anAbandonedBatchReportsNoCompletion()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRequestWatchdogForTest(20);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller,
                             &PaginationController::paginationCompleted);

        controller.requestNearTop();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(completed.count(), 0);
        QVERIFY(!controller.nearTopRunActive());
    }

    void abandonedFillsEventuallyStopTheFillLoop()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRequestWatchdogForTest(5);
        controller.setRoomId(kRoomA);

        for (int i = 0; i < 12 && !controller.fillStopped(); ++i) {
            controller.requestViewportFill();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        }
        QVERIFY(controller.fillStopped());
        QVERIFY(controller.initialContentSettled());
    }

    void aCompletedBatchIsNeverAbandonedAfterwards()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRequestWatchdogForTest(20);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller,
                             &PaginationController::paginationCompleted);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        QCOMPARE(completed.count(), 1);

        QTest::qWait(120);
        QCOMPARE(completed.count(), 1);
        QVERIFY(!controller.fillStopped());
        QVERIFY(!controller.busy());
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

        const int bound = PaginationController::kMaxFilteredRunStrikes;
        QVERIFY2(bound > 12,
                 "a filtered run must outlast the ordinary no-progress bound");
        for (int i = 0; i < bound - 1; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
            QVERIFY2(!controller.fillStopped(),
                     qPrintable(QStringLiteral(
                         "the fill stopped walking a churn run after %1 empty "
                         "pages, with the start of history not reached")
                         .arg(i + 1)));
        }
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QVERIFY(controller.fillStopped());

        const int dispatched = client.loadOlderCalls;
        QCOMPARE(dispatched, bound);
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, dispatched);

        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, dispatched + 1);
    }

    void aFilteredRunIsWalkedEvenWithAMessageAlreadyOnScreen()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 1, false);
        QCOMPARE(model.eventCount(), 1);

        client.fullyFiltered = true;
        for (int i = 0; i < 20; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        QVERIFY2(!controller.fillStopped(),
                 "twenty filtered pages under a blank viewport stopped the "
                 "automatic fill; one loaded message is not a filled viewport");
        QCOMPARE(client.loadOlderCalls, 21);
        QCOMPARE(controller.emptyFillPages(), 20);
    }

    void emptyFillPagesCountsOnlyPagesThatCompletedWithNothing()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QCOMPARE(controller.emptyFillPages(), 0);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QCOMPARE(controller.emptyFillPages(), 1);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        QCOMPARE(controller.emptyFillPages(), 1);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, true);
        QCOMPARE(controller.emptyFillPages(), 1);

        controller.setRoomId(kRoomB);
        QCOMPARE(controller.emptyFillPages(), 0);
    }

    void aProductiveViewportFillRefundsThePerRoomBudget()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        int dispatched = 0;
        for (int i = 0; i < 20; ++i) {
            const int before = client.loadOlderCalls;
            controller.requestViewportFill();
            if (client.loadOlderCalls == before)
                break;
            ++dispatched;
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 22, false);
        }
        QVERIFY2(dispatched > 8,
                 qPrintable(QStringLiteral(
                     "the per-room fill cap stopped a room whose pages were "
                     "all productive after %1 fills; a collapsed activity run "
                     "is longer than that and the reader is left having to "
                     "expand it by hand").arg(dispatched)));
        QCOMPARE(dispatched, 20);
    }

    void consecutiveEmptyViewportFillsStillStopTheLoop()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        const int bound = PaginationController::kMaxFilteredRunStrikes;
        int dispatched = 0;
        for (int i = 0; i < bound + 10; ++i) {
            const int before = client.loadOlderCalls;
            controller.requestViewportFill();
            if (client.loadOlderCalls == before)
                break;
            ++dispatched;
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        QVERIFY2(dispatched <= bound,
                 qPrintable(QStringLiteral(
                     "a room whose fills add nothing dispatched %1 pages "
                     "against a bound of %2 — it paginates without bound")
                     .arg(dispatched).arg(bound)));
    }

    void automaticNearTopBackfillIsBoundedButUserGestureReArms()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.setNearTopContinuationDelayForTest(0);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        const int afterInitial = client.loadOlderCalls;

        QSignalSpy completions(&controller,
                               &PaginationController::paginationCompleted);
        controller.requestNearTop(true);
        int dispatched = 0;
        for (int guard = 0; guard < 20
                            && client.loadOlderCalls > afterInitial + dispatched;
             ++guard) {
            ++dispatched;
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
            QCoreApplication::processEvents();
        }
        QCOMPARE(dispatched, PaginationController::kMaxNearTopEmptyStrikes);
        const int bound = PaginationController::kMaxNearTopEmptyStrikes;
        QCOMPARE(completions.count(), bound);
        for (int i = 0; i < bound; ++i)
            QCOMPARE(completions.at(i).at(2).toBool(), i < bound - 1);

        const int capped = client.loadOlderCalls;
        controller.requestNearTop(false);
        QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, capped);

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, capped + 1);

        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 2, false);
        QCoreApplication::processEvents();
        const int afterInsert = client.loadOlderCalls;
        controller.requestNearTop(false);
        QCOMPARE(client.loadOlderCalls, afterInsert + 1);
    }

    void zeroProgressNearTopPagesDoNotLoopForever()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.setNearTopContinuationDelayForTest(0);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        const int afterInitial = client.loadOlderCalls;

        controller.requestNearTop(true);
        int pages = 0;
        for (int guard = 0; guard < 50; ++guard) {
            if (!client.states[kRoomA].loading
                && client.loadOlderCalls > afterInitial + pages) {
                ++pages;
                client.beginLoading(kRoomA);
                client.completeBatch(kRoomA, 0, false);
            }
            QCoreApplication::processEvents();
            if (!controller.busy()
                && client.loadOlderCalls == afterInitial + pages)
                break;
        }
        QVERIFY2(pages <= PaginationController::kMaxNearTopEmptyStrikes,
                 qPrintable(QStringLiteral("filtered pages=%1 (bound %2)")
                                .arg(pages)
                                .arg(PaginationController::kMaxNearTopEmptyStrikes)));
        QVERIFY(pages >= 1);

        const int settled = client.loadOlderCalls;
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, settled);
    }

    void nearTopContinuationSurfacesVisibleHistoryAndResets()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.setNearTopContinuationDelayForTest(0);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        const int afterInitial = client.loadOlderCalls;

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, afterInitial + 1);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, afterInitial + 2);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        QCoreApplication::processEvents();
        const int afterVisible = client.loadOlderCalls;
        QCOMPARE(afterVisible, afterInitial + 2);
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, afterVisible);
    }

    void nearTopContinuationKeysOnRealRowGrowthNotPrependSignals()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.setNearTopContinuationDelayForTest(40);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        QCOMPARE(model.rowCount(), 5);
        int calls = client.loadOlderCalls;

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        calls = client.loadOlderCalls;
        client.beginLoading(kRoomA);
        client.insertUnattributed(kRoomA, 20);
        client.completeBatch(kRoomA, 0, false);
        QCOMPARE(model.rowCount(), 25);
        QTest::qWait(80);
        QVERIFY2(client.loadOlderCalls == calls,
                 qPrintable(QStringLiteral(
                     "a page that added 20 rows still bought %1 more batches")
                     .arg(client.loadOlderCalls - calls)));

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        calls = client.loadOlderCalls;
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        client.insertUnattributed(kRoomA, 20);
        QTest::qWait(120);
        QCOMPARE(model.rowCount(), 45);
        QVERIFY2(client.loadOlderCalls == calls,
                 qPrintable(QStringLiteral(
                     "a late-delivered page still bought %1 more batches")
                     .arg(client.loadOlderCalls - calls)));

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        calls = client.loadOlderCalls;
        const int rowsBefore = model.rowCount();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QTRY_COMPARE(client.loadOlderCalls, calls + 1);
        QCOMPARE(model.rowCount(), rowsBefore);
    }

    void continuationDelayDefaultOutlivesALateRowDelivery()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 4, false);
        const int calls = client.loadOlderCalls;

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);

        QCOMPARE(client.loadOlderCalls, calls + 1);
        QVERIFY2(controller.busy(),
                 "a scheduled continuation must keep the loading state up, or "
                 "the pagination overlay blinks once per chained page");

        client.insertUnattributed(kRoomA, 12);
        QTest::qWait(600);
        QCOMPARE(model.rowCount(), 16);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        QVERIFY2(!controller.busy(),
                 "the cancelled continuation must clear the loading state, not "
                 "strand it visible for the rest of the room visit");
    }

    void aFullyFilteredNearTopPageContinuesWithoutTheRowDeliveryWait()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 4, false);
        const int calls = client.loadOlderCalls;

        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, calls + 1);
        client.fullyFiltered = true;
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        for (int i = 0; i < 3 && client.loadOlderCalls == calls + 1; ++i)
            QCoreApplication::processEvents();
        QVERIFY2(client.loadOlderCalls == calls + 2,
                 "a fully filtered page still waited for rows that cannot arrive");
        QCOMPARE(model.rowCount(), 4);
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
        QVERIFY(!controller.fillStopped());
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 4);
    }

    void fillBudgetBoundsConsecutiveUnproductiveFills()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        controller.setMaxViewportFillRequests(2);

        for (int i = 0; i < 5; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 5, false);
        }
        QCOMPARE(client.loadOlderCalls, 5);
        QVERIFY2(!controller.fillStopped(),
                 "productive fills exhausted the cap, so a room whose pages "
                 "are all invisible stops before the reader reaches anything");

        for (int i = 0; i < 2; ++i) {
            controller.requestViewportFill();
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        const int afterEmpties = client.loadOlderCalls;
        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, afterEmpties);
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

    void finishBatchNotifiesPresentationStateSoQmlBindingsDoNotFreeze()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        QList<PaginationController::PresentationState> observed;
        connect(&controller, &PaginationController::stateChanged, &controller,
                [&] { observed.append(controller.presentationState()); });

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);

        QVERIFY(!observed.isEmpty());
        QCOMPARE(observed.last(), PaginationController::Hidden);
        QCOMPARE(controller.presentationState(), PaginationController::Hidden);
    }

    void adoptsExternallyStartedBatch()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);
        QSignalSpy completed(&controller, &PaginationController::paginationCompleted);

        client.beginLoading(kRoomA);
        QVERIFY(controller.busy());
        controller.requestNearTop();
        QCOMPARE(client.loadOlderCalls, 0);

        client.completeBatch(kRoomA, 2, false);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.first().at(0).toInt(), 2);
    }

    void loadedReplyTargetUsesStableEventIdAndHighlights()
    {
        FakeClient client;
        auto target = makeEvent(QStringLiteral("$target:example.org"));
        target.itemId = QStringLiteral("sdk-item-which-is-not-the-event-id");
        client.timelines[kRoomA] = { target };
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setHighlightDurationForTest(5);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        controller.jumpToEvent(QStringLiteral("$target:example.org"));
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.first().at(0).toInt(), 0);
        QVERIFY(located.first().at(2).toBool());
        QCOMPARE(controller.highlightedEventId(),
                 QStringLiteral("$target:example.org"));
        QTRY_VERIFY(controller.highlightedEventId().isEmpty());
    }

    void contextRevealTakesALoadedRowAndNeverPaginatesForOne()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setHighlightDurationForTest(5);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        controller.revealIfLoaded(QStringLiteral("$august-root:example.org"));
        QCOMPARE(client.loadOlderCalls, 0);
        QCOMPARE(located.count(), 0);
        QVERIFY(controller.navigationMessage().isEmpty());

        const QString rootId = QStringLiteral("$root:example.org");
        client.timelines[kRoomA] = { makeEvent(rootId) };
        model.setRoomId(QString());
        model.setRoomId(kRoomA);
        controller.revealIfLoaded(rootId);
        QCOMPARE(client.loadOlderCalls, 0);
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.first().at(0).toInt(), 0);
        QVERIFY(located.first().at(2).toBool());
    }

    void contextRevealYieldsToANavigationTheReaderAskedFor()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        const QString wanted = QStringLiteral("$wanted:example.org");
        controller.jumpToEvent(wanted);
        QCOMPARE(client.loadOlderCalls, 1);

        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$root:example.org")) },
                              false);
        QCOMPARE(located.count(), 0);
        controller.revealIfLoaded(QStringLiteral("$root:example.org"));
        QCOMPARE(located.count(), 0);

        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA, { makeEvent(wanted) }, false);
        QCOMPARE(located.count(), 1);
        QCOMPARE(model.rowForStableId(wanted), located.first().at(0).toInt());
    }

    void replyTargetSearchPaginatesBoundedlyAndCoalescesClicks()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        const QString targetId = QStringLiteral("$old:example.org");
        controller.jumpToEvent(targetId);
        controller.jumpToEvent(targetId);
        QCOMPARE(client.loadOlderCalls, 1);
        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA, { makeEvent(targetId) }, false);

        QCOMPARE(located.count(), 1);
        QCOMPARE(model.rowForStableId(targetId), 0);
        QCOMPARE(located.first().at(0).toInt(), 0);
    }

    void replySearchWaitsForExistingInitialHistoryFlight()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        controller.requestViewportFill();
        QCOMPARE(client.loadOlderCalls, 1);
        controller.jumpToEvent(QStringLiteral("$during-open:example.org"));
        QCOMPARE(client.loadOlderCalls, 1);
        client.beginLoading(kRoomA);
        client.completeEvents(
            kRoomA,
            { makeEvent(QStringLiteral("$during-open:example.org")) }, false);
        QCOMPARE(located.count(), 1);
    }

    void unavailableReplyFailsSafelyAtStart()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);

        controller.jumpToEvent(QStringLiteral("$missing:example.org"));
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.navigationMessage().isEmpty(),
                                 2000);
    }

    void jumpToEventWithoutAClientFailsHonestlyInsteadOfCrashing()
    {
        TimelineModel model;
        PaginationController controller;
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QVERIFY(controller.navigationMessage().isEmpty());

        controller.jumpToEvent(QStringLiteral("$missing:example.org"));

        QCOMPARE(controller.navigationMessage(),
                 PaginationController::unavailableTargetMessage());
        QVERIFY(controller.highlightedEventId().isEmpty());
    }

    void unavailableReplySearchHasFixedBatchBudget()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.setJumpBatchBudgetForTest(8);

        controller.jumpToEvent(QStringLiteral("$missing:example.org"));
        for (int batch = 0; batch < 8; ++batch) {
            QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, batch + 1, 2000);
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.navigationMessage().isEmpty(),
                                 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(client.loadOlderCalls, 8);
    }

    void aJumpReachesAnEventOlderThanTheOldBatchBudget()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        controller.jumpToEvent(QStringLiteral("$weeks-ago:example.org"));
        QVERIFY(controller.navigating());
        for (int batch = 0; batch < 12; ++batch) {
            QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, batch + 1, 2000);
            QVERIFY2(controller.navigationMessage().isEmpty(),
                     "the jump gave up before reaching the event");
            client.beginLoading(kRoomA);
            if (batch < 11) {
                client.completeEvents(
                    kRoomA,
                    { makeEvent(QStringLiteral("$filler%1:example.org")
                                    .arg(batch)) },
                    false);
            } else {
                client.completeEvents(
                    kRoomA,
                    { makeEvent(QStringLiteral("$weeks-ago:example.org")) },
                    false);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(located.count(), 1, 2000);
        QVERIFY(controller.navigationMessage().isEmpty());
    }

    void aJumpPastTheBudgetSaysTheMessageIsTooFarBack()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);

        controller.jumpToEvent(QStringLiteral("$far:example.org"));
        QVERIFY(controller.navigating());
        for (int batch = 0; batch < 12; ++batch) {
            QTRY_COMPARE_WITH_TIMEOUT(client.loadOlderCalls, batch + 1, 2000);
            client.beginLoading(kRoomA);
            client.completeEvents(
                kRoomA,
                { makeEvent(QStringLiteral("$f%1:example.org").arg(batch)) },
                false);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.navigationMessage().isEmpty(),
                                 2000);
        QCOMPARE(controller.navigationMessage(),
                 PaginationController::tooFarTargetMessage());
        QVERIFY(controller.navigationMessage()
                != PaginationController::unavailableTargetMessage());
        QVERIFY(!controller.navigating());
        QCOMPARE(client.loadOlderCalls, 12);
    }

    void cancelNavigationEndsTheProgressState()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.jumpToEvent(QStringLiteral("$far:example.org"));
        QVERIFY(controller.navigating());
        controller.cancelNavigation();
        QVERIFY(!controller.navigating());
    }

    void roomSwitchCancelsReplySearch()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);

        controller.jumpToEvent(QStringLiteral("$old:example.org"));
        client.beginLoading(kRoomA);
        controller.setRoomId(kRoomB);
        model.setRoomId(kRoomB);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$old:example.org")) },
                              false);
        QCOMPARE(located.count(), 0);
    }

    void roomScrollAnchorRestoresEventAndFollowingLatest()
    {
        FakeClient client;
        auto event = makeEvent(QStringLiteral("$anchor:example.org"));
        event.itemId = QStringLiteral("new-sdk-item");
        client.timelines[kRoomA] = { event };
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);

        QCOMPARE(model.eventIdAt(0), QStringLiteral("$anchor:example.org"));
        controller.saveScrollAnchor(kRoomA,
                                    QStringLiteral("$anchor:example.org"),
                                    17.5, false);
        controller.restoreScrollAnchor(kRoomA);
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.first().at(1).toReal(), 17.5);
        QVERIFY(!located.first().at(2).toBool());

        controller.saveFollowingLatest(kRoomA);
        controller.restoreScrollAnchor(kRoomA);
        QCOMPARE(latest.count(), 1);
    }

    void returnModesChooseBetweenAnchorLatestAndSelection()
    {
        FakeClient client;
        auto anchor = makeEvent(QStringLiteral("$anchor:example.org"));
        auto picked = makeEvent(QStringLiteral("$picked:example.org"));
        client.timelines[kRoomA] = { anchor, picked };
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.saveScrollAnchor(kRoomA,
                                    QStringLiteral("$anchor:example.org"),
                                    17.5, false);
        controller.saveSelection(kRoomA, QStringLiteral("$picked:example.org"));
        QCOMPARE(controller.savedSelection(kRoomA),
                 QStringLiteral("$picked:example.org"));

        QVERIFY(controller.hasHistoricalReturn(kRoomA, 0));
        controller.restoreConversation(kRoomA, 0);
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.last().at(0).toInt(),
                 model.rowForStableId(QStringLiteral("$anchor:example.org")));
        QCOMPARE(located.last().at(1).toReal(), 17.5);

        QVERIFY(!controller.hasHistoricalReturn(kRoomA, 1));
        controller.restoreConversation(kRoomA, 1);
        QCOMPARE(latest.count(), 1);
        QCOMPARE(located.count(), 1);
        QCOMPARE(controller.savedSelection(kRoomA),
                 QStringLiteral("$picked:example.org"));

        controller.restoreConversation(kRoomA, 2);
        QCOMPARE(located.count(), 2);
        QCOMPARE(located.last().at(0).toInt(),
                 model.rowForStableId(QStringLiteral("$picked:example.org")));
        QVERIFY(qIsNaN(located.last().at(1).toReal()));
        QVERIFY(!located.last().at(2).toBool());

        controller.saveSelection(kRoomA, QStringLiteral("$anchor:example.org"));
        controller.restoreConversation(kRoomA, 2);
        QCOMPARE(located.last().at(1).toReal(), 17.5);

        controller.saveSelection(kRoomA, QString());
        controller.restoreConversation(kRoomA, 2);
        QCOMPARE(located.count(), 4);
        QCOMPARE(located.last().at(1).toReal(), 17.5);

        controller.saveSelection(kRoomA, QStringLiteral("$picked:example.org"));
        client.logout();
        QCOMPARE(controller.savedSelection(kRoomA),
                 QStringLiteral("$picked:example.org"));
        controller.forgetAccount(QStringLiteral("@me:example.org"));
        QCOMPARE(controller.savedSelection(kRoomA), QString());
    }

    void anUnreachableSelectionFallsBackToThePreviousPosition()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$anchor")) }, false);
        controller.saveScrollAnchor(kRoomA, QStringLiteral("$anchor"), 9.0,
                                    false);
        controller.saveSelection(kRoomA, QStringLiteral("$gone"));
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.restoreConversation(kRoomA, 2);
        QCOMPARE(located.count(), 0);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 2, true);
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(latest.count(), 0);
        QCOMPARE(located.first().at(0).toInt(),
                 model.rowForStableId(QStringLiteral("$anchor")));
        QCOMPARE(located.first().at(1).toReal(), 9.0);
    }

    void forgetAccountClearsThatAccountsRoomScrollAnchors()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.saveScrollAnchor(kRoomA,
                                    QStringLiteral("$anchor:example.org"),
                                    1, false);
        client.logout();
        QVERIFY(controller.hasHistoricalScrollAnchor(kRoomA));
        controller.forgetAccount(QStringLiteral("@someone-else:example.org"));
        QVERIFY(controller.hasHistoricalScrollAnchor(kRoomA));
        controller.forgetAccount(QStringLiteral("@me:example.org"));
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.restoreScrollAnchor(kRoomA);
        QCOMPARE(latest.count(), 1);
    }

    void nearTopRunActiveReflectsRequestAndBoundedContinuationOnly()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.setNearTopContinuationDelayForTest(40);

        QVERIFY(!controller.nearTopRunActive());

        controller.requestViewportFill();
        QVERIFY(!controller.nearTopRunActive());
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        QVERIFY(!controller.nearTopRunActive());
        QCOMPARE(model.rowCount(), 5);

        controller.requestNearTop(true);
        QVERIFY(controller.nearTopRunActive());
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        QVERIFY(!controller.nearTopRunActive());

        controller.requestNearTop(true);
        QVERIFY(controller.nearTopRunActive());
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QVERIFY2(controller.nearTopRunActive(),
                 "a scheduled continuation must still read as an active run");
        client.insertUnattributed(kRoomA, 4);
        QTest::qWait(80);
        QVERIFY2(!controller.nearTopRunActive(),
                 "growth cancelled the continuation; the run must read as "
                 "ended, not still active");
    }

    void aRestoreBeyondThePageCapSaysSoAndLandsOnTheLatest()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);

        controller.saveScrollAnchor(kRoomA, QStringLiteral("$far-anchor"),
                                    40.0, false);
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.restoreScrollAnchor(kRoomA);
        int pages = 0;
        while (latest.isEmpty() && pages < 40) {
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 5, false);
            ++pages;
            QTest::qWait(10);
        }
        QCOMPARE(latest.count(), 1);
        QCOMPARE(located.count(), 0);
        QCOMPARE(controller.navigationMessage(),
                 PaginationController::restoreTooFarMessage());
    }

    void aSuccessfulRestoreShowsNoNotice()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);

        controller.saveScrollAnchor(kRoomA, QStringLiteral("$near-anchor"),
                                    40.0, false);
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.restoreScrollAnchor(kRoomA);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        QTest::qWait(10);
        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$near-anchor")) },
                              false);
        QTRY_COMPARE(located.count(), 1);
        QCOMPARE(latest.count(), 0);
        QCOMPARE(controller.navigationMessage(), QString());
    }

    void cancellingNavigationStopsASlowRestoreFromEverLanding()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);

        controller.saveScrollAnchor(kRoomA, QStringLiteral("$deep-anchor"),
                                    120.0,false);
        QSignalSpy located(&controller, &PaginationController::targetLocated);
        controller.restoreScrollAnchor(kRoomA);
        QCOMPARE(located.count(), 0);

        controller.cancelNavigation();

        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$deep-anchor")) },
                              false);
        QTest::qWait(50);
        QCOMPARE(located.count(), 0);

        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, true);
        QTest::qWait(50);
        QCOMPARE(located.count(), 0);
    }

    void cancellingNavigationDoesNotDisableLaterJumps()
    {
        FakeClient client;
        TimelineModel model;
        model.setClient(&client);
        model.setRoomId(kRoomA);
        PaginationController controller;
        controller.setClient(&client);
        controller.setTimelineModel(&model);
        controller.setRoomId(kRoomA);
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeEvents(kRoomA,
                              { makeEvent(QStringLiteral("$loaded")) }, false);

        controller.cancelNavigation();

        QSignalSpy located(&controller, &PaginationController::targetLocated);
        controller.jumpToEvent(QStringLiteral("$loaded"));
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.at(0).at(2).toBool(), true);
    }
};

QTEST_GUILESS_MAIN(PaginationControllerTest)
#include "PaginationControllerTest.moc"
