// Measures state-change floods through the real pipeline (real AppController,
// MockMatrixClient, and the compiled TimelinePane.qml/MessageDelegate.qml).
// TimelineModel widens dataChanged() across a whole contiguous state group on
// each insertion (see TimelineStateFloodPerfTest.cpp), and every loaded row is
// a live delegate, so that signal re-evaluates every group member's bindings.
// Four interactions:
//
//   1. Pagination completion, up to n=500/1000 across several pages.
//   2. Direct large-N hydration (the whole room seeded at once).
//   3. Repeated wheel scrolling over an already-loaded flood.
//   4. Expanding a large collapsed state group (one Label per entry).
//
// Figures are real but headless (QT_QPA_PLATFORM=offscreen), so absolute
// numbers are not the on-screen feel.

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
// A generous absolute bound that only catches a hang or runaway loop, not a
// performance target.
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
    // `seedEvents` and `pages` pending pagination pages, and loads the real
    // TimelinePane.qml. Returns the root item; `timelineOut` receives
    // "timelineListView". Returns nullptr on setup failure.
    //
    // `seed` must already overflow the window at boot, or the pane's automatic
    // viewport fill consumes pages from the mock's budget before the caller
    // stages its chunk; `pages` has headroom, and reachedStart() is treated as
    // a legitimate stop rather than a timeout.
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

    // Runs one near-top pagination request to completion and drains the event
    // loop so the Column relayout is included. Distinguishes the mock running
    // out of history (ReachedStart, not a failure) from a hang (TimedOut).
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
        // Drain a few more turns so this batch's relayout is charged here.
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
        return { PageOutcome::Completed, timer.elapsed() };
    }

    // Sends `count` discrete wheel notches upward over the timeline, timing
    // each. Offscreen wheel delivery is not guaranteed per event and there is
    // no retry, so compare sums/averages rather than single notches.
    // `engagedOut` reports whether any motion was observed at all.
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
    // One page of 100 contiguous state-change rows vs one page of 100
    // ordinary messages, same room, same pipeline.
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

    // One contiguous state group loaded across consecutive pages up to
    // 500-1000 rows, printing per-page wall time. Stops early on a legitimate
    // reachedStart; no pass/fail threshold on the trend.
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

    // Control: the same shape with ordinary messages, to separate row count
    // from state-group size.
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

    // The whole flood seeded as the room's initial content (no pagination),
    // timing boot to presentationReady: delegate creation and relayout for all
    // rows at once, at n=500 and n=1000.
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

    // Repeated wheel-up input over an already-loaded flood (n=500, seeded
    // directly), timing each notch.
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
        // Skip, not fail: offscreen wheel delivery is not guaranteed, and a
        // run without input produced no evidence either way.
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

    // Expanding a large group: RoomActivityDelegate's `expandedColumn`
    // Repeater instantiates one Label per entry only when expanded. Seeds one
    // group, toggles it via toggleStateGroup() (as the summary row does), and
    // times expansion and collapse.
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

    // A contiguous 100-event state group should collapse to a few view rows.
    // Today every loaded event gets its own view row even though only the
    // leader renders content; the fix belongs in ReverseListProxyModel, which
    // must keep TimelineModel's per-event rows for other consumers and handle
    // jumps into suppressed rows, the read marker and virtual rows.
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
        // `count` is the Repeater's own instantiated item count
        // (rowRepeater.count): the real number of view rows built.
        const int viewRows = timeline->property("count").toInt();
        qInfo("viewRowCollapse groupSize=%d modelRows=%d viewRows=%d",
              groupSize, modelRows, viewRows);

        QVERIFY2(modelRows >= groupSize,
                 "fixture assumption: TimelineModel must keep one row per "
                 "real event regardless of any view-level collapsing — a "
                 "row-count fix must never shrink the authoritative model");
        // Allow a small constant for the surviving representative row plus
        // virtual rows. Marked QEXPECT_FAIL until view-row suppression lands;
        // when it does, the XPASS fails the run and forces removing the marker.
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
