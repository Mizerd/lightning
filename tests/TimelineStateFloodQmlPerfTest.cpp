// Follow-up to TimelineStateFloodPerfTest.cpp (pure-C++ TimelineModel-level
// measurement, now confirmed: hydration linear in row count, cumulative
// incremental-append cost O(n^2) but only ~4.5ms of C++ work at n=100 —
// far too small on its own to explain a reported "scrolling up kinda dies").
//
// TimelineModel::emitPresentationGroupingChanged still widens dataChanged()
// to cover the WHOLE contiguous state-change run on every insertion into
// that run (proven directly in TimelineStateFloodPerfTest.cpp), and the room
// timeline instantiates every loaded row with no virtualization
// (TimelinePane.qml's rotated Flickable + Column, since 1e50f6a) — so that
// widened signal reaches every one of the group's REAL, already-created
// MessageDelegate items and re-evaluates their `model.stateGroupEntries` /
// `model.stateGroupId` / `model.stateGroupLeader` bindings
// (MessageDelegate.qml:50,511,513). This file measures that layer directly
// through the real pipeline (real AppController, real MockMatrixClient, the
// real compiled TimelinePane.qml/MessageDelegate.qml loaded through the
// "MatrixClient" QML module, real synchronous delegate creation and Column
// relayout), across four different interactions:
//
//   1. Pagination completion, at n=100 (first pass) and now pushed to
//      n=500/1000 (contiguous state group loaded across several pages).
//   2. Direct large-N hydration (the whole room seeded at once, no
//      pagination at all) — isolates "how expensive is N loaded state rows"
//      from "how expensive is loading them incrementally".
//   3. Repeated wheel-scroll events over an ALREADY-LOADED flood — the
//      interaction the report actually names ("scrolling up"), as opposed
//      to pagination completion.
//   4. Expanding a large collapsed state group (RoomActivityDelegate.qml's
//      `expandedColumn` Repeater instantiates one Label per entry the
//      moment `expanded` becomes true — a materially different cost profile
//      from the collapsed one-line summary).
//
// FIRST PASS RESULT (n=100, pagination only): singlePage state=n/a
// (see completion report), repeatedPages state 8/6/16ms vs message 6/5/9ms
// at rows 53/73/93 — a modest difference, not a reproduction. This revision
// fixes a harness bug found in that pass (see fixupHarness note below) and
// extends to n=500/1000 and to the wheel-scroll and expanded-group
// interactions the maintainer's report actually describes.
//
// Every millisecond figure printed here is a genuine measurement of the real
// pipeline, not a proxy or a prediction. It is still a HEADLESS/offscreen
// measurement (QT_QPA_PLATFORM=offscreen, see CMakeLists.txt), so absolute
// numbers are not the physical on-screen feel — see the completion report
// for exactly what is and is not covered by this test.

#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QWheelEvent>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 2000;
// A generous absolute bound, not a performance target: only meant to fail
// loudly on a genuine hang/runaway loop, not to characterize normal timing
// (which is inherently machine-dependent — see the completion report).
constexpr int kHangGuardMs = 15000;

QList<TimelineEvent> makeStateChangeChunk(int n, int startOffsetMinutes,
                                          const QString &tag = {})
{
    QList<TimelineEvent> chunk;
    chunk.reserve(n);
    for (int i = 0; i < n; ++i) {
        TimelineEvent e;
        e.sender = QStringLiteral("@alice:mock.local");
        e.senderDisplayName = QStringLiteral("Alice");
        e.stateKind = QStringLiteral("membership");
        e.body = QStringLiteral(
            "Alice changed their display name to Alice%1%2.").arg(tag).arg(i);
        e.type = TimelineEvent::StateChange;
        e.status = TimelineEvent::Sent;
        e.timestamp = QDateTime::currentDateTimeUtc()
                          .addSecs(-(startOffsetMinutes * 60 + i));
        chunk.append(e);
    }
    return chunk;
}

QList<TimelineEvent> makeMessageChunk(int n, int startOffsetMinutes)
{
    QList<TimelineEvent> chunk;
    chunk.reserve(n);
    for (int i = 0; i < n; ++i) {
        TimelineEvent e;
        e.sender = QStringLiteral("@carol:mock.local");
        e.senderDisplayName = QStringLiteral("Carol");
        e.body = QStringLiteral("older backfilled message %1").arg(i);
        e.type = TimelineEvent::TextMessage;
        e.status = TimelineEvent::Sent;
        e.timestamp = QDateTime::currentDateTimeUtc()
                          .addSecs(-(startOffsetMinutes * 60 + i));
        chunk.append(e);
    }
    return chunk;
}

QList<TimelineEvent> makeSeedMessages(int n)
{
    QList<TimelineEvent> events;
    events.reserve(n);
    for (int i = 0; i < n; ++i) {
        TimelineEvent e;
        e.sender = QStringLiteral("@bob:mock.local");
        e.senderDisplayName = QStringLiteral("Bob");
        e.body = QStringLiteral("seed message %1").arg(i);
        e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
        e.type = TimelineEvent::TextMessage;
        e.status = TimelineEvent::Sent;
        events.append(e);
    }
    return events;
}

} // namespace

class TimelineStateFloodQmlPerfTest : public QObject
{
    Q_OBJECT

private:
    static QString loginAndRoomIdAt(AppController &controller, int row)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() <= row; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() <= row)
            return {};
        const QModelIndex idx = controller.roomList()->index(row, 0);
        return controller.roomList()
            ->data(idx, RoomListModel::RoomIdRole)
            .toString();
    }

    // Boots a real AppController on the mock backend, seeds a room with
    // `seedEvents` (either ordinary messages or the whole flood, caller's
    // choice) and `pages` pending pagination pages, and loads the real
    // compiled TimelinePane.qml (through the "MatrixClient" QML module)
    // against it. Returns the root item; `timelineOut` receives the
    // "timelineListView" child. Returns nullptr on any setup failure.
    //
    // fixupHarness (found from the coordinator's first-pass run): `seed`
    // must be large enough that contentHeight already exceeds the window's
    // height at boot. TimelinePane.qml's maybeFillViewport() (and the
    // separate initial-history-gate near-top request) otherwise fire
    // automatically before this function's caller ever calls
    // setPaginationChunkForTest(), silently consuming 1-2 units of the
    // mock's paginationRemaining budget with the mock's own tiny default
    // filler — which is exactly what made explicit page 4 fail in the
    // first pass (reached_start had already gone true one page early).
    // Giving `pages` generous headroom above what the caller explicitly
    // requests, and treating reachedStart() as a legitimate stop rather
    // than a timeout (see PageResult below), covers the rest.
    QQuickItem *bootRoomTimeline(AppController &controller,
                                 QQmlApplicationEngine &engine,
                                 QQuickWindow &window,
                                 QStringList &warnings,
                                 MockMatrixClient *&mockOut,
                                 QQuickItem *&timelineOut,
                                 const QList<TimelineEvent> &seedEvents,
                                 int pages)
    {
        if (loginAndRoomIdAt(controller, 0).isEmpty())
            return nullptr;
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return nullptr;
        mockOut = mock;
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(0);
        mock->resetTimelineForTest(roomId, seedEvents, pages);

        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        if (createdSpy.isEmpty())
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!timeline)
            return nullptr;
        timelineOut = timeline;

        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) {
            QCoreApplication::processEvents();
            ready = timeline->property("presentationReady").toBool()
                    && timeline->property("count").toInt() >= seedEvents.size()
                    && !controller.pagination()->busy();
            if (!ready)
                QTest::qWait(10);
        }
        return ready ? root : nullptr;
    }

    QQuickItem *bootRoomTimeline(AppController &controller,
                                 QQmlApplicationEngine &engine,
                                 QQuickWindow &window,
                                 QStringList &warnings,
                                 MockMatrixClient *&mockOut,
                                 QQuickItem *&timelineOut,
                                 int seed, int pages)
    {
        return bootRoomTimeline(controller, engine, window, warnings, mockOut,
                                timelineOut, makeSeedMessages(seed), pages);
    }

    enum class PageOutcome { Completed, ReachedStart, TimedOut };
    struct PageResult {
        PageOutcome outcome;
        qint64 elapsedMs = -1;
    };

    // Runs one near-top pagination request to completion (the mock's
    // currently staged chunk), then lets the QML event loop drain so the
    // Column's relayout from this batch is included in the measurement.
    // Distinguishes "the mock legitimately ran out of history"
    // (PageOutcome::ReachedStart — not a failure, just nothing more to
    // measure) from an actual hang (PageOutcome::TimedOut).
    PageResult timeOnePage(AppController &controller)
    {
        if (controller.pagination()->reachedStart())
            return { PageOutcome::ReachedStart, 0 };

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        QElapsedTimer timer;
        timer.start();
        controller.pagination()->requestNearTop();
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kHangGuardMs;
        while (completedSpy.isEmpty()
               && QDateTime::currentMSecsSinceEpoch() < deadline) {
            QCoreApplication::processEvents();
            QTest::qWait(5);
        }
        if (completedSpy.isEmpty()) {
            if (controller.pagination()->reachedStart())
                return { PageOutcome::ReachedStart, 0 };
            return { PageOutcome::TimedOut, -1 };
        }
        // Drain the event loop a few more turns so the Column's relayout
        // triggered by this batch (and any deferred property re-evaluation)
        // is charged to this page's measurement, not the next one's.
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        return { PageOutcome::Completed, timer.elapsed() };
    }

    // Drives `count` discrete mouse-wheel notches (angleDelta, the pattern
    // already proven reliable offscreen in TimelinePaneQmlTest.cpp) upward
    // over the timeline's centre, one per iteration, timing each with
    // QElapsedTimer. This is the interaction the report actually names:
    // repeated wheel-up input over an ALREADY-LOADED flood, not pagination.
    // Returns per-notch elapsed times in milliseconds.
    //
    // Honesty note: synthesized wheel delivery is not guaranteed to register
    // on every single offscreen pass (existing tests in
    // TimelinePaneQmlTest.cpp resend until it registers for exactly this
    // reason). This helper sends exactly one event per notch with no retry,
    // so an individual notch's timing can include a "not actually delivered"
    // fast outlier on either variant equally — the SUM/average across all
    // `count` notches is the trustworthy comparison, not any single notch.
    // `engagedOut` reports whether the sequence produced observable motion
    // at all (stickToBottom left true would mean the harness never actually
    // exercised the handler, which would make the timing meaningless).
    QList<qint64> timeWheelNotches(QQuickWindow &window, QQuickItem *timeline,
                                   int count, bool &engagedOut)
    {
        QList<qint64> timings;
        const QPointF pos(380, 300);
        for (int i = 0; i < count; ++i) {
            QElapsedTimer timer;
            timer.start();
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            timings.append(timer.elapsed());
        }
        engagedOut = timeline->property("stickToBottom").toBool() == false;
        return timings;
    }

private Q_SLOTS:
    // Direct comparison at equal row count: one pagination page of 100
    // contiguous state-change rows vs. one page of 100 ordinary messages,
    // against the SAME seeded room, through the SAME real pipeline.
    void singlePageStateFloodVsMessageFloodWallTime()
    {
        {
            AppController controller(AppController::MockBackend);
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QStringList warnings;
            MockMatrixClient *mock = nullptr;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = bootRoomTimeline(controller, engine, window,
                                               warnings, mock, timeline,
                                               /*seed=*/60, /*pages=*/3);
            QVERIFY(root != nullptr);
            mock->setPaginationChunkForTest(makeStateChangeChunk(100, 600));

            const PageResult result = timeOnePage(controller);
            QVERIFY2(result.outcome == PageOutcome::Completed,
                     "state-flood page did not complete (unexpected reached_start "
                     "or a real hang)");
            qInfo("singlePage stateFlood n=100 elapsedMs=%lld",
                  static_cast<long long>(result.elapsedMs));
            QVERIFY(controller.timeline()->rowCount() >= 160);
        }
        {
            AppController controller(AppController::MockBackend);
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QStringList warnings;
            MockMatrixClient *mock = nullptr;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = bootRoomTimeline(controller, engine, window,
                                               warnings, mock, timeline,
                                               /*seed=*/60, /*pages=*/3);
            QVERIFY(root != nullptr);
            mock->setPaginationChunkForTest(makeMessageChunk(100, 600));

            const PageResult result = timeOnePage(controller);
            QVERIFY2(result.outcome == PageOutcome::Completed,
                     "message-flood page did not complete (unexpected reached_start "
                     "or a real hang)");
            qInfo("singlePage messageFlood n=100 elapsedMs=%lld",
                  static_cast<long long>(result.elapsedMs));
            QVERIFY(controller.timeline()->rowCount() >= 160);
        }
    }

    // Loads the SAME contiguous state group across several consecutive
    // pagination pages, PUSHED TO 500-1000 rows (the maintainer's "50-100"
    // was an estimate of what they noticed, not a ceiling — a long-lived
    // room can hold far more, and nothing here virtualizes). Prints the
    // per-page wall time; stops early and reports (not fails) on a
    // legitimate reachedStart. No pass/fail threshold on the timing trend
    // itself — see the completion report for why.
    void repeatedPagesStateFloodScalesToLargeN()
    {
        const int pageSize = 100;
        const int targetPages = 10; // 60 seed + up to 1000 more, 100 at a time.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QStringList warnings;
        MockMatrixClient *mock = nullptr;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = bootRoomTimeline(controller, engine, window,
                                           warnings, mock, timeline,
                                           /*seed=*/60, /*pages=*/targetPages + 5);
        QVERIFY(root != nullptr);
        mock->setPaginationChunkForTest(
            makeStateChangeChunk(pageSize, 600, QStringLiteral("g")));

        int completedPages = 0;
        for (int page = 1; page <= targetPages; ++page) {
            const PageResult result = timeOnePage(controller);
            if (result.outcome == PageOutcome::ReachedStart) {
                qInfo("repeatedPages stateFlood page=%d reachedStart — stopping "
                      "(legitimate terminal condition, not a failure)", page);
                break;
            }
            QVERIFY2(result.outcome != PageOutcome::TimedOut,
                     qPrintable(QStringLiteral(
                         "state-flood page %1 hung (not reached_start)").arg(page)));
            ++completedPages;
            qInfo("repeatedPages stateFlood page=%d totalRows=%d elapsedMs=%lld",
                  page, controller.timeline()->rowCount(),
                  static_cast<long long>(result.elapsedMs));
        }
        QVERIFY2(completedPages >= 5,
                 "fixture did not deliver enough pages to reach a large N — "
                 "harness regression, not a perf result");
        qInfo("repeatedPages stateFlood finalRows=%d",
              controller.timeline()->rowCount());
    }

    // Control: the same shape, but ordinary messages instead of state
    // changes — whether per-page cost grows with accumulated ROW COUNT in
    // general or specifically with accumulated STATE-GROUP size.
    void repeatedPagesMessageFloodScalesToLargeN()
    {
        const int pageSize = 100;
        const int targetPages = 10;
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QStringList warnings;
        MockMatrixClient *mock = nullptr;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = bootRoomTimeline(controller, engine, window,
                                           warnings, mock, timeline,
                                           /*seed=*/60, /*pages=*/targetPages + 5);
        QVERIFY(root != nullptr);
        mock->setPaginationChunkForTest(makeMessageChunk(pageSize, 600));

        int completedPages = 0;
        for (int page = 1; page <= targetPages; ++page) {
            const PageResult result = timeOnePage(controller);
            if (result.outcome == PageOutcome::ReachedStart) {
                qInfo("repeatedPages messageFlood page=%d reachedStart — "
                      "stopping (legitimate terminal condition)", page);
                break;
            }
            QVERIFY2(result.outcome != PageOutcome::TimedOut,
                     qPrintable(QStringLiteral(
                         "message-flood page %1 hung (not reached_start)").arg(page)));
            ++completedPages;
            qInfo("repeatedPages messageFlood page=%d totalRows=%d elapsedMs=%lld",
                  page, controller.timeline()->rowCount(),
                  static_cast<long long>(result.elapsedMs));
        }
        QVERIFY2(completedPages >= 5,
                 "fixture did not deliver enough pages to reach a large N — "
                 "harness regression, not a perf result");
        qInfo("repeatedPages messageFlood finalRows=%d",
              controller.timeline()->rowCount());
    }

    // Isolates "how expensive is N loaded contiguous state-change rows"
    // from "how expensive is loading them incrementally": the WHOLE flood
    // is seeded as the room's initial content (no pagination at all), and
    // this measures wall time from boot start to presentationReady/settled
    // — i.e. real delegate creation + Column relayout for the whole set in
    // one shot, at n=500 and n=1000.
    void directLargeHydrationWallTime()
    {
        for (const int n : { 500, 1000 }) {
            {
                AppController controller(AppController::MockBackend);
                QQmlApplicationEngine engine;
                QQuickWindow window;
                QStringList warnings;
                MockMatrixClient *mock = nullptr;
                QQuickItem *timeline = nullptr;
                QElapsedTimer timer;
                timer.start();
                QQuickItem *root = bootRoomTimeline(
                    controller, engine, window, warnings, mock, timeline,
                    makeStateChangeChunk(n, 600, QStringLiteral("h")),
                    /*pages=*/0);
                const qint64 elapsedMs = timer.elapsed();
                QVERIFY2(root != nullptr,
                         qPrintable(QStringLiteral(
                             "direct hydration setup failed at n=%1 (state)").arg(n)));
                qInfo("directHydration stateFlood n=%d bootToReadyMs=%lld",
                      n, static_cast<long long>(elapsedMs));
                QVERIFY(controller.timeline()->rowCount() >= n);
            }
            {
                AppController controller(AppController::MockBackend);
                QQmlApplicationEngine engine;
                QQuickWindow window;
                QStringList warnings;
                MockMatrixClient *mock = nullptr;
                QQuickItem *timeline = nullptr;
                QElapsedTimer timer;
                timer.start();
                QQuickItem *root = bootRoomTimeline(
                    controller, engine, window, warnings, mock, timeline,
                    makeMessageChunk(n, 600), /*pages=*/0);
                const qint64 elapsedMs = timer.elapsed();
                QVERIFY2(root != nullptr,
                         qPrintable(QStringLiteral(
                             "direct hydration setup failed at n=%1 (message)").arg(n)));
                qInfo("directHydration messageFlood n=%d bootToReadyMs=%lld",
                      n, static_cast<long long>(elapsedMs));
                QVERIFY(controller.timeline()->rowCount() >= n);
            }
        }
    }

    // THE interaction the report names: repeated wheel-up input over an
    // ALREADY-LOADED flood (n=500, seeded directly — no pagination in
    // flight to confound the measurement), timing each notch individually.
    void wheelScrollOverLoadedStateFloodVsMessages()
    {
        constexpr int n = 500;
        constexpr int notches = 20;
        QList<qint64> stateTimings;
        QList<qint64> messageTimings;
        bool stateEngaged = false;
        bool messageEngaged = false;
        {
            AppController controller(AppController::MockBackend);
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QStringList warnings;
            MockMatrixClient *mock = nullptr;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = bootRoomTimeline(
                controller, engine, window, warnings, mock, timeline,
                makeStateChangeChunk(n, 600, QStringLiteral("w")), /*pages=*/0);
            QVERIFY(root != nullptr);
            QVERIFY(timeline->setProperty("stickToBottom", true));
            stateTimings = timeWheelNotches(window, timeline, notches, stateEngaged);
        }
        {
            AppController controller(AppController::MockBackend);
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QStringList warnings;
            MockMatrixClient *mock = nullptr;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = bootRoomTimeline(
                controller, engine, window, warnings, mock, timeline,
                makeMessageChunk(n, 600), /*pages=*/0);
            QVERIFY(root != nullptr);
            QVERIFY(timeline->setProperty("stickToBottom", true));
            messageTimings = timeWheelNotches(window, timeline, notches, messageEngaged);
        }
        // SKIP, not fail. Synthesized wheel delivery is not guaranteed on
        // every offscreen pass -- this measured cleanly once (state 1.10 ms
        // vs message 8.95 ms per notch) and failed to deliver on a later
        // run of the identical binary. A measurement that could not obtain
        // its input has produced no evidence either way; failing on it
        // would make the suite flaky, which is worse than saying so.
        if (!stateEngaged || !messageEngaged) {
            QSKIP("wheel notches were not delivered in this offscreen run; "
                  "no timing evidence produced (not a regression)");
        }

        qint64 stateTotal = 0, messageTotal = 0;
        for (int i = 0; i < notches; ++i) {
            stateTotal += stateTimings.at(i);
            messageTotal += messageTimings.at(i);
            qInfo("wheelNotch i=%d stateMs=%lld messageMs=%lld",
                  i, static_cast<long long>(stateTimings.at(i)),
                  static_cast<long long>(messageTimings.at(i)));
        }
        qInfo("wheelScroll n=%d notches=%d stateTotalMs=%lld messageTotalMs=%lld "
              "stateAvgMs=%.2f messageAvgMs=%.2f",
              n, notches, static_cast<long long>(stateTotal),
              static_cast<long long>(messageTotal),
              double(stateTotal) / notches, double(messageTotal) / notches);
    }

    // The EXPANDED-group hypothesis: RoomActivityDelegate.qml's
    // `expandedColumn` Repeater (`model: expandedColumn.visible ?
    // root.entries : []`) instantiates one Label PER ENTRY only when the
    // group is expanded — a materially different cost from the collapsed
    // one-line summary every other test here measures. Seeds one
    // contiguous group of `groupSize` state changes, toggles it open via
    // the same toggleStateGroup() the summary row's TapHandler calls, and
    // times the expansion and the subsequent collapse.
    void expandingLargeStateGroupWallTime()
    {
        constexpr int groupSize = 300;
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QStringList warnings;
        MockMatrixClient *mock = nullptr;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = bootRoomTimeline(
            controller, engine, window, warnings, mock, timeline,
            makeStateChangeChunk(groupSize, 600, QStringLiteral("e")),
            /*pages=*/0);
        QVERIFY(root != nullptr);

        QString groupId;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            const QModelIndex idx = controller.timeline()->index(row);
            if (controller.timeline()
                    ->data(idx, TimelineModel::StateGroupLeaderRole)
                    .toBool()) {
                groupId = controller.timeline()
                              ->data(idx, TimelineModel::StateGroupIdRole)
                              .toString();
                break;
            }
        }
        QVERIFY2(!groupId.isEmpty(), "fixture must yield one state group");

        QElapsedTimer expandTimer;
        expandTimer.start();
        QVERIFY(QMetaObject::invokeMethod(timeline, "toggleStateGroup",
                                          Q_ARG(QVariant, groupId)));
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();
        const qint64 expandMs = expandTimer.elapsed();

        QElapsedTimer collapseTimer;
        collapseTimer.start();
        QVERIFY(QMetaObject::invokeMethod(timeline, "toggleStateGroup",
                                          Q_ARG(QVariant, groupId)));
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();
        const qint64 collapseMs = collapseTimer.elapsed();

        qInfo("expandGroup groupSize=%d expandMs=%lld collapseMs=%lld",
              groupSize, static_cast<long long>(expandMs),
              static_cast<long long>(collapseMs));
    }

    // REGRESSION GUARD for the proposed fix (not yet implemented — see the
    // completion report for the design and its risks: it belongs in
    // ReverseListProxyModel, not TimelineModel, must preserve TimelineModel's
    // full per-event row space for every non-view consumer, and must handle
    // jump-to-event/scroll-anchor redirects into a suppressed row, read-marker
    // positioning, and virtual-row transparency).
    //
    // THIS TEST IS EXPECTED TO FAIL ON TODAY'S CODE, DELIBERATELY. Today every
    // loaded event — leader or not — gets its own Repeater-instantiated view
    // row (TimelinePane.qml's `Repeater { model: app.timelineView }`), so a
    // contiguous 100-event state-change group costs ~100 view rows even
    // though only the leader's row ever renders visible content. Once
    // non-leader state rows are filtered out of the view's row space, this
    // should assert a small bounded count instead of one proportional to the
    // group size — that is the regression this guards.
    void hundredEventStateGroupShouldCollapseToFewViewRows()
    {
        constexpr int groupSize = 100;
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QStringList warnings;
        MockMatrixClient *mock = nullptr;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = bootRoomTimeline(
            controller, engine, window, warnings, mock, timeline,
            makeStateChangeChunk(groupSize, 600, QStringLiteral("r")),
            /*pages=*/0);
        QVERIFY(root != nullptr);

        const int modelRows = controller.timeline()->rowCount();
        // TimelinePane.qml exposes the Repeater's own instantiated item
        // count as `count` (see TimelinePane.qml: `readonly property int
        // count: rowRepeater.count`) — the real number of view rows built,
        // not an estimate.
        const int viewRows = timeline->property("count").toInt();
        qInfo("viewRowCollapse groupSize=%d modelRows=%d viewRows=%d",
              groupSize, modelRows, viewRows);

        QVERIFY2(modelRows >= groupSize,
                 "fixture assumption: TimelineModel must keep one row per "
                 "real event regardless of any view-level collapsing — a "
                 "row-count fix must never shrink the authoritative model");
        // THE regression guard. Allow a small constant for the one
        // surviving representative row plus any virtual rows (date
        // dividers etc.) the fixture may legitimately add.
        //
        // EXPECTED TO FAIL until the view-row suppression lands in
        // ReverseListProxyModel. Measured today: modelRows=100, viewRows=100
        // -- 100 delegates for a group that draws ONE summary line, at a
        // measured ~4.8 ms per row of hydration cost.
        //
        // Marked expected-fail rather than left red on purpose: a
        // permanently-failing case trains people to ignore the suite (see
        // the two stale timeline suites in CLAUDE.md 16). QEXPECT_FAIL also
        // works in our favour here -- when the fix lands this reports XPASS,
        // which FAILS the run and forces the marker to be removed rather
        // than silently rotting.
        QEXPECT_FAIL("", "view-row suppression not implemented yet: a 100-event "
                         "state group still materialises ~100 view rows",
                     Abort);
        QVERIFY2(viewRows <= 10,
                 qPrintable(QStringLiteral(
                     "view row count %1 is proportional to the group size "
                     "(%2) — non-leader state rows are still being "
                     "materialized as separate view rows")
                     .arg(viewRows).arg(groupSize)));
    }
};

QTEST_MAIN(TimelineStateFloodQmlPerfTest)
#include "TimelineStateFloodQmlPerfTest.moc"
