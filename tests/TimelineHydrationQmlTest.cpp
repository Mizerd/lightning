// Timeline-stability suite: room-open behaviour with the real
// TimelinePane.qml + TimelineModel + PaginationController stack on the mock
// backend, driven through a Rust-SDK-shaped sequence (small initial snapshot,
// asynchronous history fill batches, in-place Set updates that change
// delegate heights after presentation).
//
// Gates covered:
//   * the initial-hydration presentation gate (no one-item partial room,
//     open at the latest event once coherently presentable);
//   * bottom-pinned stability while rows above grow;
//   * scrolled-up anchor preservation while rows above/below the anchor
//     grow and while live events append;
//   * row-scoped (not full-range) model updates for in-place Set diffs.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
const QString kRoom = QStringLiteral("!general:mock.local");

TimelineEvent makeText(const QString &sender, const QString &body,
                       int secondsAgo)
{
    TimelineEvent e;
    e.sender = sender;
    e.senderDisplayName = sender.mid(1).section(QLatin1Char(':'), 0, 0);
    e.body = body;
    e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-secondsAgo);
    e.type = TimelineEvent::TextMessage;
    e.status = TimelineEvent::Sent;
    return e;
}
}

class TimelineHydrationQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Pane {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QQuickItem *timeline = nullptr;
        QStringList warnings;
    };

    static bool login(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return false;
        for (int i = 0; i < 50 && controller.roomList()->rowCount() < 1; ++i)
            QTest::qWait(20);
        return controller.roomList()->rowCount() >= 1;
    }

    bool createPane(AppController &controller, Pane &pane)
    {
        pane.engine = std::make_unique<QQmlApplicationEngine>();
        connect(pane.engine.get(), &QQmlEngine::warnings, this,
                [&pane](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        pane.warnings << e.toString();
                });
        pane.engine->rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(pane.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        pane.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                    QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        pane.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!pane.root)
            return false;
        pane.window = std::make_unique<QQuickWindow>();
        pane.window->resize(820, 560);
        pane.root->setParentItem(pane.window->contentItem());
        pane.root->setSize(QSizeF(820, 560));
        pane.window->show();
        pane.timeline = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        return pane.timeline != nullptr;
    }

    static qreal timelineReal(QQuickItem *timeline, const char *prop)
    {
        return timeline->property(prop).toReal();
    }

    // Call a QML-declared function. QML methods return through QVariant, not
    // a typed C++ return, so Q_RETURN_ARG must name QVariant.
    static QVariant callQml(QQuickItem *o, const char *fn,
                            QVariant a = {}, QVariant b = {})
    {
        QVariant out;
        if (b.isValid())
            QMetaObject::invokeMethod(o, fn, Q_RETURN_ARG(QVariant, out),
                                      Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        else if (a.isValid())
            QMetaObject::invokeMethod(o, fn, Q_RETURN_ARG(QVariant, out),
                                      Q_ARG(QVariant, a));
        else
            QMetaObject::invokeMethod(o, fn, Q_RETURN_ARG(QVariant, out));
        return out;
    }

    static qreal bottomDistance(QQuickItem *timeline)
    {
        return timelineReal(timeline, "contentY")
            - callQml(timeline, "wheelMinY").toReal();
    }

    static qreal viewportYForRow(QQuickItem *timeline, int row)
    {
        auto *quickItem = callQml(timeline, "itemAtViewRow", row)
                              .value<QQuickItem *>();
        if (!quickItem)
            return qQNaN();
        return quickItem->y() - timelineReal(timeline, "contentY");
    }

private Q_SLOTS:
    void initialHydrationGateHoldsThenOpensAtLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setPaginationDelayForTest(40);

        controller.setCurrentRoomId(kRoom);
        Pane pane;
        QVERIFY(createPane(controller, pane));

        mock->resetTimelineForTest(
            kRoom, { makeText(QStringLiteral("@matas:mock.local"),
                              QStringLiteral("newest message"), 60) },
2);
        QCOMPARE(pane.timeline->property("presentationReady").toBool(), false);
        auto *loading = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineLoadingSurface"));
        QVERIFY(loading != nullptr);
        QVERIFY(loading->isVisible());
        QCOMPARE(pane.timeline->opacity(), 0.0);

        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        const int gatedRows = controller.timeline()->rowCount();
        QVERIFY2(gatedRows > 1,
                 qPrintable(
                     QStringLiteral("presentation gate opened on a partial "
                                    "snapshot: modelRows=%1 viewRows=%2 "
                                    "contentHeight=%3 height=%4 "
                                    "geometryStale=%5")
                         .arg(gatedRows)
                         .arg(pane.timeline->property("count").toInt())
                         .arg(timelineReal(pane.timeline, "contentHeight"))
                         .arg(timelineReal(pane.timeline, "height"))
                         .arg(pane.timeline
                                  ->property("presentationGeometryStale")
                                  .toBool()
                              ? QStringLiteral("true")
                              : QStringLiteral("false"))));
        QTRY_COMPARE_WITH_TIMEOUT(pane.timeline->opacity(), 1.0, 5000);
        QVERIFY(!loading->isVisible());

        QVERIFY(pane.timeline->property("stickToBottom").toBool());
        if (timelineReal(pane.timeline, "contentHeight")
            > timelineReal(pane.timeline, "height")) {
            QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0,
                                     5000);
        }
        QCOMPARE(pane.warnings, QStringList{});
    }

    void firstRoomOpenWithWarmContentPresentsFast()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        QList<TimelineEvent> events;
        for (int i = 0; i < 16; ++i) {
            events.append(makeText(QStringLiteral("@alice:mock.local"),
                                   QStringLiteral("warm message %1\nsecond line")
                                       .arg(i),
                                   (30 - i) * 60));
        }
        mock->resetTimelineForTest(kRoom, events,1);
        mock->setPaginationDelayForTest(3000);

        Pane pane;
        QVERIFY(createPane(controller, pane));
        QElapsedTimer clock;
        clock.start();
        controller.setCurrentRoomId(kRoom);
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        const qint64 openedAfterMs = clock.elapsed();
        QVERIFY(controller.timeline()->rowCount() >= 16);
        QVERIFY2(openedAfterMs < 1500,
                 qPrintable(QStringLiteral(
                     "gate opened only after %1 ms — the fast fillsViewport "
                     "path did not run (guard fires at 2500, settle at 3000)")
                                .arg(openedAfterMs)));
    }

    void bottomPinnedStaysAtLatestWhileRowsGrow()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        controller.setCurrentRoomId(kRoom);
        QList<TimelineEvent> events;
        for (int i = 0; i < 14; ++i) {
            events.append(makeText(QStringLiteral("@alice:mock.local"),
                                   QStringLiteral("message %1").arg(i),
                                   (20 - i) * 60));
        }
        Pane pane;
        QVERIFY(createPane(controller, pane));
        mock->resetTimelineForTest(kRoom, events,0);
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);

        const QList<int> growth = { 3, 9, 1, 6 };
        for (int row : growth) {
            TimelineEvent grown = events.at(row);
            grown.eventId = controller.timeline()
                                ->data(controller.timeline()->index(row),
                                       TimelineModel::EventIdRole)
                                .toString();
            grown.body = QStringLiteral("hydrated tall content line\n")
                             .repeated(6 + row % 3);
            mock->changeEventAtForTest(kRoom, row, grown);
            QTest::qWait(30);
        }
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);
        QVERIFY(pane.timeline->property("stickToBottom").toBool());
        QCOMPARE(pane.warnings, QStringList{});
    }

    void scrolledUpAnchorHoldsThroughGrowthAndAppends()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        controller.setCurrentRoomId(kRoom);
        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            events.append(makeText(QStringLiteral("@alice:mock.local"),
                                   QStringLiteral("history message %1\nsecond line")
                                       .arg(i),
                                   (40 - i) * 60));
        }
        Pane pane;
        QVERIFY(createPane(controller, pane));
        mock->resetTimelineForTest(kRoom, events,0);
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);

        const int anchorRow = 15;
        pane.timeline->setProperty("stickToBottom", false);
        QMetaObject::invokeMethod(pane.timeline, "releasePendingRows");
        QMetaObject::invokeMethod(pane.timeline, "positionViewAtViewRow",
                                  Q_ARG(QVariant, anchorRow),
                                  Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(pane.timeline, "captureViewAnchor");
        QTRY_VERIFY_WITH_TIMEOUT(
            !pane.timeline->property("viewAnchorId").toString().isEmpty(),
            2000);
        const int anchorSourceRow =
            callQml(pane.timeline, "sourceRowForViewRow", anchorRow).toInt();
        QVERIFY(anchorSourceRow >= 0);
        auto anchorViewRow = [&]() {
            return callQml(pane.timeline, "viewRowForSourceRow",
                           anchorSourceRow).toInt();
        };
        const qreal anchorViewportY = viewportYForRow(pane.timeline, anchorRow);
        QVERIFY(!qIsNaN(anchorViewportY));

        auto grow = [&](int row) {
            TimelineEvent grown = events.at(row);
            grown.eventId = controller.timeline()
                                ->data(controller.timeline()->index(row),
                                       TimelineModel::EventIdRole)
                                .toString();
            grown.body = QStringLiteral("late media/preview growth line\n")
                             .repeated(8);
            mock->changeEventAtForTest(kRoom, row, grown);
        };

        grow(2);
        grow(5);
        QTest::qWait(60);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(viewportYForRow(pane.timeline, anchorRow) - anchorViewportY)
                <= 2.0,
            5000);

        grow(24);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(viewportYForRow(pane.timeline, anchorRow) - anchorViewportY)
                <= 2.0,
            5000);

        mock->appendEventForTest(
            kRoom, makeText(QStringLiteral("@bob:mock.local"),
                            QStringLiteral("new live message"), 0));
        QTest::qWait(60);
        QVERIFY(!pane.timeline->property("stickToBottom").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(viewportYForRow(pane.timeline, anchorViewRow())
                 - anchorViewportY)
                <= 2.0,
            5000);
        QCOMPARE(pane.warnings, QStringList{});
    }

    void setDiffEmitsRowScopedChange()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(kRoom);

        QList<TimelineEvent> events;
        for (int i = 0; i < 6; ++i) {
            events.append(makeText(QStringLiteral("@alice:mock.local"),
                                   QStringLiteral("m%1").arg(i),
                                   (10 - i) * 60));
        }
        mock->resetTimelineForTest(kRoom, events, 0);
        auto *model = controller.timeline();
        QCOMPARE(model->rowCount(), 6);

        QSignalSpy changed(model, &QAbstractItemModel::dataChanged);
        TimelineEvent updated = events.at(3);
        updated.eventId = model->data(model->index(3),
                                      TimelineModel::EventIdRole).toString();
        updated.senderDisplayName = QStringLiteral("Resolved Name");
        updated.senderAvatarUrl = QStringLiteral("mxc://mock.local/avatar3");
        mock->changeEventAtForTest(kRoom, 3, updated);

        QCOMPARE(changed.count(), 1);
        const auto topLeft = changed.at(0).at(0).toModelIndex();
        const auto bottomRight = changed.at(0).at(1).toModelIndex();
        QCOMPARE(topLeft.row(), 3);
        QCOMPARE(bottomRight.row(), 3);
        QCOMPARE(model->data(model->index(3),
                             TimelineModel::SenderDisplayNameRole).toString(),
                 QStringLiteral("Resolved Name"));

        changed.clear();
        TimelineEvent senderChange = updated;
        senderChange.sender = QStringLiteral("@carol:mock.local");
        mock->changeEventAtForTest(kRoom, 3, senderChange);
        QVERIFY(changed.count() >= 1);
        bool sawRowScoped = false;
        const int rowCount = model->rowCount();
        for (const auto &sig : changed) {
            const int first = sig.at(0).toModelIndex().row();
            const int last = sig.at(1).toModelIndex().row();
            if (first == 3 && last == 3)
                sawRowScoped = true;
            QVERIFY2(last - first + 1 <= 4,
                     "presentation refresh must stay local to its boundary");
            QVERIFY(last < rowCount);
        }
        QVERIFY(sawRowScoped);
    }

    void displayNameFallsBackToLocalpartThenResolvesInPlace()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(kRoom);

        TimelineEvent unresolved =
            makeText(QStringLiteral("@matas:matrix.smetonis.net"),
                     QStringLiteral("labas"), 60);
        unresolved.senderDisplayName.clear();
        mock->resetTimelineForTest(kRoom, { unresolved }, 0);

        auto *model = controller.timeline();
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex idx = model->index(0);
        QCOMPARE(model->data(idx, TimelineModel::SenderDisplayNameRole)
                     .toString(),
                 QStringLiteral("matas"));
        QCOMPARE(model->data(idx, TimelineModel::SenderInitialsRole)
                     .toString(),
                 QStringLiteral("M"));
        QCOMPARE(model->data(idx, TimelineModel::SenderRole).toString(),
                 QStringLiteral("@matas:matrix.smetonis.net"));

        TimelineEvent resolved = unresolved;
        resolved.eventId = model->data(idx, TimelineModel::EventIdRole)
                               .toString();
        resolved.senderDisplayName = QStringLiteral("Matas");
        resolved.senderAvatarUrl = QStringLiteral("mxc://x/matas");
        mock->changeEventAtForTest(kRoom, 0, resolved);
        QCOMPARE(model->data(idx, TimelineModel::SenderDisplayNameRole)
                     .toString(),
                 QStringLiteral("Matas"));
        QCOMPARE(model->data(idx, TimelineModel::SenderAvatarMxcRole)
                     .toString(),
                 QStringLiteral("mxc://x/matas"));
    }
};

QTEST_MAIN(TimelineHydrationQmlTest)
#include "TimelineHydrationQmlTest.moc"
