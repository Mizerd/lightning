// v0.5.11: deterministic tests for the backward-pagination controller —
// single-flight (including the dispatch-to-loading window), viewport-fill
// budget and no-progress stop, reached-start behavior, retry after failure,
// and stale-result isolation across room switches, timeline resets and
// sign-out.

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

    // Scripted per-room pagination state, mirroring RustSdkMatrixClient.
    struct State {
        bool loading = false;
        bool reachedStart = false;
        bool failed = false;
    };
    QHash<QString, State> states;
    bool timelineActive = true;
    bool failureTransient = false;
    int loadOlderCalls = 0;
    QString lastLoadRoom;
    QHash<QString, QList<TimelineEvent>> timelines;

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
        QCoreApplication::processEvents(); // settle async diff accounting
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
    void failBatch(const QString &roomId, bool transient = false)
    {
        states[roomId].loading = false;
        states[roomId].failed = true;
        failureTransient = transient;
        Q_EMIT paginationStateChanged(roomId);
    }

    // Pagination interface under test.
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

} // namespace

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

    void automaticNearTopBackfillIsBoundedButUserGestureReArms()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        // One completed initial fill so later NearTop is not redirected to fill.
        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false);
        const int afterInitial = client.loadOlderCalls; // == 1

        // A SINGLE near-top approach now drives a controller-owned, strictly
        // bounded continuation through filtered (zero-visible-row) pages: each
        // completed empty page schedules exactly one more, up to
        // kMaxNearTopEmptyStrikes, then latches. Complete each dispatched page
        // empty and count them; the loop terminates when no further page is
        // auto-dispatched.
        QSignalSpy completions(&controller,
                               &PaginationController::paginationCompleted);
        controller.requestNearTop(/*userInitiated=*/true);
        int dispatched = 0;
        for (int guard = 0; guard < 20
                            && client.loadOlderCalls > afterInitial + dispatched;
             ++guard) {
            ++dispatched;
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false); // empty (filtered) page
            QCoreApplication::processEvents();      // fire the continuation
        }
        QCOMPARE(dispatched, 4); // kMaxNearTopEmptyStrikes: one approach, 4 pages
        // willContinue tells the anchor logic whether THIS empty completion
        // already scheduled the run's next batch: true for the first three
        // strikes, false for the latching fourth — the release signal that
        // keeps a latched run from stranding a scroll-anchor capture.
        QCOMPARE(completions.count(), 4);
        for (int i = 0; i < 4; ++i)
            QCOMPARE(completions.at(i).at(2).toBool(), i < 3);

        // The continuation has latched: no further automatic dispatch spins.
        const int capped = client.loadOlderCalls;
        controller.requestNearTop(false);
        QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, capped);

        // A genuine user scroll gesture (a deliberate NEW approach to the top)
        // re-arms the bound and dispatches again.
        controller.requestNearTop(true);
        QCOMPARE(client.loadOlderCalls, capped + 1);

        // An inserting page re-arms the bound for subsequent automatic use.
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 2, false); // added content -> strikes reset
        QCoreApplication::processEvents();
        const int afterInsert = client.loadOlderCalls;
        controller.requestNearTop(false);
        QCOMPARE(client.loadOlderCalls, afterInsert + 1);
    }

    // The reported defect: reaching the top starts a rapid loop of
    // "completed added=0 reached_start=false -> requested reason=near_top".
    // A single approach must dispatch a BOUNDED number of filtered pages and
    // then STOP on its own, without any further user or geometry input.
    void zeroProgressNearTopPagesDoNotLoopForever()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false); // initial visible history
        const int afterInitial = client.loadOlderCalls;

        // One near-top approach, then let the controller drive itself: every
        // dispatched page returns zero visible rows and is NOT at the start
        // (the SDK is paging through thread-only history). Simulate the async
        // completion of whatever the controller dispatches, but issue NO
        // further requestNearTop — the loop, if any, must be the controller's.
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
                break; // settled: no new dispatch pending
        }
        // Strictly bounded — it stopped on its own well under a spin.
        QVERIFY2(pages <= 4,
                 qPrintable(QStringLiteral("filtered pages=%1").arg(pages)));
        QVERIFY(pages >= 1); // it did try at least once

        // And it stays stopped with no further input.
        const int settled = client.loadOlderCalls;
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, settled);
    }

    // Bounded continuation still SURFACES visible history: a run of filtered
    // pages followed by a page that adds visible events resets the bound and
    // stops the continuation (the reader now sees older messages).
    void nearTopContinuationSurfacesVisibleHistoryAndResets()
    {
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        controller.requestViewportFill();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 5, false);
        const int afterInitial = client.loadOlderCalls;

        controller.requestNearTop(true);
        // First dispatched page is filtered (0 visible)...
        QCOMPARE(client.loadOlderCalls, afterInitial + 1);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 0, false);
        QCoreApplication::processEvents();
        // ...the controller auto-continues once...
        QCOMPARE(client.loadOlderCalls, afterInitial + 2);
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false); // visible history surfaces
        QCoreApplication::processEvents();
        // ...and stops: visible progress cleared the bound, no auto-continue.
        const int afterVisible = client.loadOlderCalls;
        QCOMPARE(afterVisible, afterInitial + 2);
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(client.loadOlderCalls, afterVisible);
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

    void finishBatchNotifiesPresentationStateSoQmlBindingsDoNotFreeze()
    {
        // Regression test: a QML binding (e.g. TimelinePane.qml's
        // pagination header) only re-reads presentationState() when
        // stateChanged() fires — it never polls. finishBatch() used to
        // drop m_requestActive to false (the input to busy()/
        // presentationState()) without emitting stateChanged() itself,
        // so the last value any such binding observed stayed "Loading"
        // forever even though a fresh presentationState() call already
        // returned "Hidden". Verified against the real QML in
        // TimelinePaneQmlTest.cpp; this pins the underlying signal.
        FakeClient client;
        PaginationController controller;
        controller.setClient(&client);
        controller.setRoomId(kRoomA);

        QList<PaginationController::PresentationState> observed;
        connect(&controller, &PaginationController::stateChanged, &controller,
                [&] { observed.append(controller.presentationState()); });

        controller.requestNearTop();
        client.beginLoading(kRoomA);
        client.completeBatch(kRoomA, 3, false); // flushes the deferred finishBatch()

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
        QVERIFY(!controller.navigationMessage().isEmpty());
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

        controller.jumpToEvent(QStringLiteral("$missing:example.org"));
        for (int batch = 0; batch < 8; ++batch) {
            client.beginLoading(kRoomA);
            client.completeBatch(kRoomA, 0, false);
        }
        QCOMPARE(client.loadOlderCalls, 8);
        QVERIFY(!controller.navigationMessage().isEmpty());
        QVERIFY(!controller.busy());
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

    void logoutClearsRoomScrollAnchors()
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
        QSignalSpy latest(&controller,
                          &PaginationController::restoreLatestRequested);
        controller.restoreScrollAnchor(kRoomA);
        QCOMPARE(latest.count(), 1);
    }
};

QTEST_GUILESS_MAIN(PaginationControllerTest)
#include "PaginationControllerTest.moc"
