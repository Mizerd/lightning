// The timeline's non-wheel scroll inputs, over the real TimelinePane on the
// mock backend: the scroll bar, middle-click autoscroll, and a live message
// landing under a reader who is still scrolling. Each case is a defect named by
// a LIGHTNING_SCROLL_TRACE=1 capture on 2026-10-20 of the real client on a
// local homeserver (C26 and open item O3):
//   * a held scroll bar was not user input, so every page landing under it ran
//     the IDLE anchor restore against the drag (`inputs=scrollbar
//     anchorCorrections=91 topDist=7490`), and a drag never reached the settle;
//   * an autoscroll pinned at the top stopped paginating once the per-approach
//     row budget was spent (`topBudgetBlocked=719 reachedStart=0`);
//   * an autoscroll towards newer pinned against the row window's synthetic
//     newest edge (`pinnedSteps=958 winSkip=424 winExtendNew=0`);
//   * a live message landing inside the 250 ms settle tail moved a reader
//     parked at the oldest row by its height (`materializedMaxAbsDelta=23
//     activeDeferrals=1 topDist=23`).
// And from review round 1: the hold for that last one must never touch a glide
// heading for the live edge (it ended a message short, follow-latest off), and
// the pinned-top renewal is bounded per gesture.
// The anchor counters are read from the pane, so the trace gate is set before
// any controller exists (they are inert without it).

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"

namespace {

constexpr int kSignalTimeoutMs = 4000;

class GestureHold
{
public:
    explicit GestureHold(QObject *settleTimer)
    {
        QMetaObject::invokeMethod(settleTimer, "restart");
        QObject::connect(&m_ticker, &QTimer::timeout, settleTimer,
                         [settleTimer] {
                             QMetaObject::invokeMethod(settleTimer, "restart");
                         });
        m_ticker.start(20);
    }
    ~GestureHold() { m_ticker.stop(); }

private:
    QTimer m_ticker;
};

}

class TimelineScrollInputQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

    static QString login(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() == 0; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() == 0)
            return {};
        return controller.roomList()
            ->data(controller.roomList()->index(0, 0), RoomListModel::RoomIdRole)
            .toString();
    }

    static QList<TimelineEvent> textEvents(const QString &roomId, int count,
                                           const QString &prefix,
                                           int baseSecondsAgo = 40000)
    {
        const QDateTime base =
            QDateTime::currentDateTimeUtc().addSecs(-baseSecondsAgo);
        QList<TimelineEvent> events;
        for (int i = 0; i < count; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$%1%2").arg(prefix).arg(i);
            e.itemId = QStringLiteral("uid-%1%2").arg(prefix).arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("line %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        return events;
    }

    static QList<TimelineEvent> shortChunk()
    {
        QList<TimelineEvent> chunk;
        const QDateTime base = QDateTime::currentDateTimeUtc().addDays(-3);
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral("older %1").arg(i);
            e.timestamp = base.addSecs(i * 20);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        return chunk;
    }

    struct Pane {
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *root = nullptr;
        QQuickItem *timeline = nullptr;
        QObject *settleTimer = nullptr;
    };

    static bool openPane(AppController &controller, Pane &pane,
                         const QString &roomId,
                         const QList<TimelineEvent> &events,
                         int paginationPages, int viewportHeight)
    {
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return false;
        controller.setCurrentRoomId(roomId);
        pane.engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&pane.engine,
                              &QQmlApplicationEngine::objectCreated);
        pane.engine.loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        pane.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!pane.root)
            return false;
        pane.window.resize(760, viewportHeight);
        pane.root->setParentItem(pane.window.contentItem());
        pane.root->setSize(QSizeF(pane.window.width(), pane.window.height()));
        pane.window.show();
        (void)QTest::qWaitForWindowExposed(&pane.window, 2000);
        pane.window.requestActivate();
        QCoreApplication::processEvents();
        mock->resetTimelineForTest(roomId, events, paginationPages);
        pane.timeline = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        pane.settleTimer = pane.root->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        if (!pane.timeline || !pane.settleTimer)
            return false;
        return QTest::qWaitFor([&] {
            return pane.timeline->property("presentationReady").toBool()
                && pane.timeline->property("count").toInt() >= events.size();
        }, 8000);
    }

    static QVariant eval(QQuickItem *timeline, const QString &expression)
    {
        QQmlExpression expr(qmlContext(timeline), timeline, expression);
        const QVariant value = expr.evaluate();
        if (expr.hasError())
            qWarning() << expression << expr.error().toString();
        return value;
    }
    static double distanceFromTop(QQuickItem *timeline)
    {
        return eval(timeline, QStringLiteral("distanceFromTop()")).toDouble();
    }
    static int sourceRows(AppController &controller)
    {
        return controller.timeline()->rowCount();
    }
    static void wheel(Pane &pane, int angle)
    {
        const QPointF pos(380, 300);
        QWheelEvent ev(pos, pane.window.mapToGlobal(pos), QPoint(0, 0),
                       QPoint(0, angle), Qt::NoButton, Qt::NoModifier,
                       Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&pane.window, &ev);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }

    void aHeldScrollBarIsInputAndPagesLandingUnderItNeverYankTheReader()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setPaginationChunkForTest(shortChunk());
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 60, QStringLiteral("sb")),
10,620));
        QQuickItem *timeline = pane.timeline;
        auto *bar = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineScrollBar"));
        QVERIFY(bar != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), kSignalTimeoutMs);
        QTest::qWait(400);
        timeline->setProperty("diagAnchorCorrections", 0);

        const double size = bar->property("size").toDouble();
        const double position = bar->property("position").toDouble();
        const QPoint grab = bar->mapToScene(
            QPointF(bar->width() / 2, (position + size / 2) * bar->height()))
                                .toPoint();
        const QPoint top =
            bar->mapToScene(QPointF(bar->width() / 2, 1)).toPoint();
        const int before = sourceRows(controller);
        QTest::mousePress(&pane.window, Qt::LeftButton, Qt::NoModifier, grab);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(&pane.window, grab + (top - grab) * i / 10);
        QTest::qWait(50);
        QVERIFY2(bar->property("pressed").toBool(), "the handle was not grabbed");
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "a held scroll bar is not user input to the timeline");
        QTRY_VERIFY_WITH_TIMEOUT(sourceRows(controller) >= before + 60, 12000);
        QTest::qWait(300);
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "the timeline stopped treating the held bar as input");
        const int corrections =
            timeline->property("diagAnchorCorrections").toInt();
        QVERIFY2(corrections == 0,
                 qPrintable(QStringLiteral("%1 idle anchor restores ran under "
                                           "the held scroll bar")
                                .arg(corrections)));

        QTest::mouseRelease(&pane.window, Qt::LeftButton, Qt::NoModifier, top);
        QVERIFY2(pane.settleTimer->property("running").toBool(),
                 "letting go of the bar did not start the settle");
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 2000);
    }

    // C26: an autoscroll held against the oldest loaded row keeps asking for
    // history after the automatic chain's per-approach budget is spent. Live,
    // a pinned autoscroll never leaves the exit band that renews the budget,
    // so in a room of one-line messages it stopped after 240 rows
    // (`topBudgetBlocked=719`). The mock cannot show that consequence: its
    // geometry lets the passive atYEnd edge (unbudgeted by design) fetch for a
    // reader who re-reaches the top, so a row count would pass on broken code.
    // The STATE is asserted instead: pinned and still steering upwards, the
    // spent budget is renewed whenever no page is in flight.
    void anAutoscrollPinnedAtTheTopRenewsASpentApproachBudget()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setPaginationChunkForTest(shortChunk());
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 300, QStringLiteral("au")),
40,620));
        QQuickItem *timeline = pane.timeline;
        auto *scroller = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const QPoint anchor(380, 400);
        QTest::mouseClick(&pane.window, Qt::MiddleButton, Qt::NoModifier,
                          anchor);
        QVERIFY2(scroller->property("latched").toBool(),
                 "the middle click did not latch");
        QTest::mouseMove(&pane.window, anchor - QPoint(0, 200));
        bool spent = false;
        for (int round = 0; round < 3; ++round) {
            QTRY_VERIFY_WITH_TIMEOUT(distanceFromTop(timeline) <= 1
                                         && !controller.pagination()->busy(),
                                     10000);
            timeline->setProperty("nearTopRowsThisApproach", 100000);
            timeline->setProperty("nearTopRequestsThisApproach", 100000);
            spent = true;
            QTRY_VERIFY2_WITH_TIMEOUT(
                timeline->property("nearTopRowsThisApproach").toInt() < 240,
                qPrintable(QStringLiteral("a pinned autoscroll kept a spent "
                                          "budget (blocked %1 times)")
                               .arg(timeline->property("diagTopBudgetBlocked")
                                        .toInt())),
                4000);
        }
        QVERIFY(spent);

        QTRY_VERIFY_WITH_TIMEOUT(distanceFromTop(timeline) <= 1
                                     && !controller.pagination()->busy(),
                                 10000);
        timeline->setProperty(
            "explicitTopStartRows",
            timeline->property("count").toInt()
                - timeline->property("explicitTopRowCeiling").toInt());
        timeline->setProperty("nearTopRowsThisApproach", 100000);
        timeline->setProperty("nearTopRequestsThisApproach", 100000);
        QTest::qWait(600);
        QVERIFY2(timeline->property("nearTopRowsThisApproach").toInt() >= 240,
                 "the renewal ignored the per-gesture row ceiling");
        QVERIFY(timeline->property("explicitTopCeilingLogged").toBool());
        QVERIFY(scroller->property("latched").toBool());
        QTest::keyClick(&pane.window, Qt::Key_Escape);
    }

    void anAutoscrollTowardsNewerOpensTheRowWindowsNewestEdge()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 420, QStringLiteral("wn")),
0,620));
        QQuickItem *timeline = pane.timeline;
        auto *scroller = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            eval(timeline, QStringLiteral("app.timelineView.revealIdle()"))
                .toBool(), 15000);

        eval(timeline, QStringLiteral("app.timelineView.setWindow(120, 300)"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("rowWindowSkip").toInt(), 120, 2000);
        QCoreApplication::processEvents();
        eval(timeline, QStringLiteral("(function() { rowColumn.forceLayout();"
                                      " stickToBottom = false;"
                                      " contentY = wheelMinY() + 40;"
                                      " return true })()"));
        QTest::qWait(400);
        QVERIFY(timeline->property("rowWindowSkip").toInt() > 0);

        const QPoint anchor(380, 200);
        QTest::mouseClick(&pane.window, Qt::MiddleButton, Qt::NoModifier,
                          anchor);
        QVERIFY2(scroller->property("latched").toBool(),
                 "the middle click did not latch");
        QTest::mouseMove(&pane.window, anchor + QPoint(0, 180));
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("rowWindowSkip").toInt() == 0,
            qPrintable(QStringLiteral("autoscroll stopped with %1 newest rows "
                                      "still withheld")
                           .arg(timeline->property("rowWindowSkip").toInt())),
            15000);
        QCOMPARE(timeline->property("diagLiveInsertHolds").toInt(), 0);
        QTest::keyClick(&pane.window, Qt::Key_Escape);
    }

    void aLiveMessageInTheSettleTailDoesNotMoveAReaderAtTheOldestRow()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 80, QStringLiteral("o3")),
0,600));
        QQuickItem *timeline = pane.timeline;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QTest::qWait(100);
        QVERIFY(!timeline->property("stickToBottom").toBool());
        GestureHold hold(pane.settleTimer);
        QTest::qWait(100);
        QVERIFY2(distanceFromTop(timeline) <= 1,
                 qPrintable(QStringLiteral("fixture: reader %1 px from the top")
                                .arg(distanceFromTop(timeline))));
        const QString topBefore = eval(timeline,
            QStringLiteral("eventIdAtViewRow(viewRowAtPhysicalTop())"))
                                      .toString();
        QVERIFY(!topBefore.isEmpty());

        const int rowsBefore = sourceRows(controller);
        TimelineEvent live =
            textEvents(roomId, 1, QStringLiteral("o3live"), 60).first();
        mock->appendEventForTest(roomId, live);
        QTRY_VERIFY_WITH_TIMEOUT(sourceRows(controller) > rowsBefore, 2000);
        QTest::qWait(300);
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "fixture: the scroll session closed before the append");
        const double after = distanceFromTop(timeline);
        QVERIFY2(after <= 1,
                 qPrintable(QStringLiteral("the append moved the reader %1 px "
                                           "off the oldest row").arg(after)));
        QCOMPARE(eval(timeline,
                      QStringLiteral("eventIdAtViewRow(viewRowAtPhysicalTop())"))
                     .toString(),
                 topBefore);
    }

    void aLiveMessageDuringAGlideToTheLatestStillLandsAtTheBottom_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("goToLatest") << 0;
        QTest::newRow("wheelDown") << 1;
    }
    void aLiveMessageDuringAGlideToTheLatestStillLandsAtTheBottom()
    {
        QFETCH(int, mode);
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 120, QStringLiteral("gl")),
0,600));
        QQuickItem *timeline = pane.timeline;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            eval(timeline, QStringLiteral("app.timelineView.revealIdle()"))
                .toBool(), 15000);
        for (int i = 0; i < 3; ++i) {
            wheel(pane, 120);
            QTest::qWait(15);
        }
        QTest::qWait(900);
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "fixture: no anchor captured, the case would be vacuous");
        QVERIFY(!timeline->property("stickToBottom").toBool());
        if (mode == 0) {
            QVERIFY(QMetaObject::invokeMethod(timeline, "goToLatest"));
        } else {
            for (int i = 0; i < 6; ++i) {
                wheel(pane, -120);
                QTest::qWait(15);
            }
        }
        QTest::qWait(30);
        QVERIFY2(timeline->property("wheelAnimating").toBool(),
                 "fixture: the append must land mid-glide");
        mock->appendEventForTest(
            roomId, textEvents(roomId, 1, QStringLiteral("gllive"), 60).first());
        QTest::qWait(1500);
        const double y = timeline->property("contentY").toDouble();
        const double minY = eval(timeline, QStringLiteral("wheelMinY()")).toDouble();
        QVERIFY2(timeline->property("stickToBottom").toBool() && y - minY <= 1,
                 qPrintable(QStringLiteral("the glide to the latest ended %1 px "
                                           "above the bottom (following: %2)")
                                .arg(y - minY)
                                .arg(timeline->property("stickToBottom")
                                         .toBool())));
    }

    void aCaptureBetweenAnAppendAndItsCorrectionStillHoldsTheReader()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId,
                         textEvents(roomId, 80, QStringLiteral("rc")),
0,600));
        QQuickItem *timeline = pane.timeline;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QTest::qWait(100);
        GestureHold hold(pane.settleTimer);
        QTest::qWait(100);
        QVERIFY(distanceFromTop(timeline) <= 1);

        TimelineEvent live =
            textEvents(roomId, 1, QStringLiteral("rclive"), 60).first();
        mock->appendEventForTest(roomId, live);
        eval(timeline, QStringLiteral("(function() { rowColumn.forceLayout();"
                                      " captureViewAnchor(); return true })()"));
        QTest::qWait(300);
        const double after = distanceFromTop(timeline);
        QVERIFY2(after <= 1,
                 qPrintable(QStringLiteral("a capture in between left the "
                                           "reader %1 px off the oldest row")
                                .arg(after)));
    }

    void aMessageBodyShowsTheIBeamAndItsLinkThePointingHand()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = login(controller);
        QVERIFY(!roomId.isEmpty());
        QList<TimelineEvent> events =
            textEvents(roomId, 6, QStringLiteral("cur"));
        events.last().body = QStringLiteral("plain words example link");
        events.last().formattedBody = QStringLiteral(
            "plain words <a href=\"https://example.org/probe\">example link</a>");
        Pane pane;
        QVERIFY(openPane(controller, pane, roomId, events, 0, 620));

        QQuickItem *body = nullptr;
        QVERIFY(QTest::qWaitFor([&] {
            for (const char *name : {"messageBody", "messageSegmentText"}) {
                const auto bodies = pane.root->findChildren<QQuickItem *>(
                    QLatin1String(name));
                for (QQuickItem *candidate : bodies) {
                    if (candidate->property("text").toString().contains(
                            QStringLiteral("example.org/probe")))
                        body = candidate;
                }
            }
            return body && body->width() > 0;
        }, kSignalTimeoutMs));

        const qreal y = body->height() / 2;
        qreal linkX = -1;
        for (qreal x = 2; x < body->width() && linkX < 0; x += 2) {
            QString href;
            QVERIFY(QMetaObject::invokeMethod(body, "linkAt",
                                              Q_RETURN_ARG(QString, href),
                                              Q_ARG(qreal, x), Q_ARG(qreal, y)));
            if (!href.isEmpty())
                linkX = x + 6;
        }
        QVERIFY2(linkX > 20, "premise: the body has no plain text before a link");
        const QPoint plain = body->mapToScene(QPointF(6, y)).toPoint();
        const QPoint link = body->mapToScene(QPointF(linkX, y)).toPoint();

        QTest::mouseMove(&pane.window, plain + QPoint(0, -200));
        QTest::mouseMove(&pane.window, plain);
        QTRY_COMPARE_WITH_TIMEOUT(pane.window.cursor().shape(),
                                  Qt::IBeamCursor, kSignalTimeoutMs);
        QTest::mouseMove(&pane.window, link + QPoint(-1, 0));
        QTest::mouseMove(&pane.window, link);
        QTRY_COMPARE_WITH_TIMEOUT(pane.window.cursor().shape(),
                                  Qt::PointingHandCursor, kSignalTimeoutMs);
        QTest::mouseMove(&pane.window, plain);
        QTRY_COMPARE_WITH_TIMEOUT(pane.window.cursor().shape(),
                                  Qt::IBeamCursor, kSignalTimeoutMs);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TimelineScrollInputQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "TimelineScrollInputQmlTest.moc"
