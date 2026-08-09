// v0.7 timeline-stability suite. Reproduces the live room-open defects with
// the real TimelinePane.qml + TimelineModel + PaginationController stack on
// the mock backend, driven through the staged Rust-SDK-shaped sequence:
// a small initial snapshot, asynchronous history fill batches, and in-place
// Set updates that change delegate heights after presentation.
//
// Gates covered:
//   * the initial-hydration presentation gate (no one-item partial room,
//     open at the latest event once coherently presentable);
//   * bottom-pinned stability while rows above grow (media/text hydration
//     must not push the newest message out of the viewport);
//   * scrolled-up anchor preservation while rows above/below the anchor
//     grow and while live events append;
//   * row-scoped (not full-range) model updates for in-place Set diffs.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
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
} // namespace

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

    // Loads the real TimelinePane into a shown offscreen window so the
    // ListView lays out real delegates with real heights.
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

    // How far the view sits from the newest message. The timeline is rotated
    // so that proxy row 0 — the newest — is at the physical bottom, which puts
    // it at the LOW end of the scroll range; following the latest therefore
    // means contentY resting on wheelMinY(), not on contentHeight.
    static qreal bottomDistance(QQuickItem *timeline)
    {
        return timelineReal(timeline, "contentY")
            - callQml(timeline, "wheelMinY").toReal();
    }

    // The y position of the row inside the viewport, or NaN when that row is
    // not currently held by the view (the proxy paces newly paginated history
    // out over several frames).
    static qreal viewportYForRow(QQuickItem *timeline, int row)
    {
        auto *quickItem = callQml(timeline, "itemAtViewRow", row)
                              .value<QQuickItem *>();
        if (!quickItem)
            return qQNaN();
        return quickItem->y() - timelineReal(timeline, "contentY");
    }

private Q_SLOTS:
    // The live defect: a room whose event cache starts with one event and
    // hydrates through fill batches must not present the partial snapshot.
    // It presents once coherent, positioned at the latest event, with the
    // loading surface gone.
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

        // Stage the live shape: one event in the snapshot, two more history
        // pages available. The reset re-engages the presentation gate.
        mock->resetTimelineForTest(
            kRoom, { makeText(QStringLiteral("@matas:mock.local"),
                              QStringLiteral("newest message"), 60) },
            /*paginationPages=*/2);
        QCOMPARE(pane.timeline->property("presentationReady").toBool(), false);
        auto *loading = pane.root->findChild<QQuickItem *>(
            QStringLiteral("timelineLoadingSurface"));
        QVERIFY(loading != nullptr);
        QVERIFY(loading->isVisible());
        QCOMPARE(pane.timeline->opacity(), 0.0);

        // The automatic viewport fill settles the gate without user input.
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        QVERIFY(controller.timeline()->rowCount() > 1);
        QTRY_COMPARE_WITH_TIMEOUT(pane.timeline->opacity(), 1.0, 5000);
        QVERIFY(!loading->isVisible());

        // Presented at the latest event: the view is bottom-pinned and the
        // remaining distance below the viewport is (near) zero.
        QVERIFY(pane.timeline->property("stickToBottom").toBool());
        if (timelineReal(pane.timeline, "contentHeight")
            > timelineReal(pane.timeline, "height")) {
            QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0,
                                     5000);
        }
        QCOMPARE(pane.warnings, QStringList{});
    }

    // While pinned to the bottom, asynchronous row growth above the newest
    // message (image hydration, late decryption, link previews) must keep
    // the newest message visible — the regression was a growing gap that
    // pushed the conversation out of the viewport.
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
        mock->resetTimelineForTest(kRoom, events, /*paginationPages=*/0);
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);

        // Grow several rows above the newest message in randomized-ish
        // order, as media/decryption completion does.
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
        // One coalesced correction per batch keeps the newest event pinned.
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);
        QVERIFY(pane.timeline->property("stickToBottom").toBool());
        QCOMPARE(pane.warnings, QStringList{});
    }

    // While the user reads older history, growth above the anchored message
    // must not move it inside the viewport, growth below must not either,
    // and a live append must not force the view back to the bottom.
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
        mock->resetTimelineForTest(kRoom, events, /*paginationPages=*/0);
        QTRY_VERIFY_WITH_TIMEOUT(
            pane.timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bottomDistance(pane.timeline) < 48.0, 5000);

        // Scroll up to the middle of history the way programmatic
        // navigation does, then let the pane capture its reading anchor.
        const int anchorRow = 15;
        pane.timeline->setProperty("stickToBottom", false);
        // Release the paced backlog first: a jump must be able to address any
        // loaded row, which is exactly why the navigation paths do the same.
        QMetaObject::invokeMethod(pane.timeline, "releasePendingRows");
        QMetaObject::invokeMethod(pane.timeline, "positionViewAtViewRow",
                                  Q_ARG(QVariant, anchorRow),
                                  Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(pane.timeline, "captureViewAnchor");
        QTRY_VERIFY_WITH_TIMEOUT(
            !pane.timeline->property("viewAnchorId").toString().isEmpty(),
            2000);
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

        // Growth above the anchor.
        grow(2);
        grow(5);
        QTest::qWait(60);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(viewportYForRow(pane.timeline, anchorRow) - anchorViewportY)
                <= 2.0,
            5000);

        // Growth below the anchor.
        grow(24);
        QTest::qWait(60);
        QVERIFY(qAbs(viewportYForRow(pane.timeline, anchorRow)
                     - anchorViewportY) <= 2.0);

        // A live append below must not force the reader to the bottom.
        mock->appendEventForTest(
            kRoom, makeText(QStringLiteral("@bob:mock.local"),
                            QStringLiteral("new live message"), 0));
        QTest::qWait(60);
        QVERIFY(!pane.timeline->property("stickToBottom").toBool());
        QVERIFY(qAbs(viewportYForRow(pane.timeline, anchorRow)
                     - anchorViewportY) <= 2.0);
        QCOMPARE(pane.warnings, QStringList{});
    }

    // An in-place Set diff re-reads exactly one row. The previous full-range
    // dataChanged forced every delegate to re-bind and re-measure on every
    // profile/decryption/reaction update — the relayout storm behind the
    // room-open jumping.
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

        // A grouping-relevant change (new sender) also refreshes the
        // neighbourhood presentation roles. That refresh is now emitted
        // synchronously and scoped to the mutation boundary, rather than
        // coalesced onto the next turn as one whole-model span: at 600-900
        // rows the whole-model form rebound all of loaded history once per
        // page. The row-scoped change still lands, and every span stays local.
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

    // The visible sender label never falls back to the complete Matrix ID
    // when a safer representation exists: unresolved profiles show the
    // localpart, and a late profile resolution replaces it in place on the
    // same row (the live "@matas:matrix.smetonis.net" regression).
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
        unresolved.senderDisplayName.clear(); // profile not yet available
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
        // Full MXID stays available on the identity role for tooltips.
        QCOMPARE(model->data(idx, TimelineModel::SenderRole).toString(),
                 QStringLiteral("@matas:matrix.smetonis.net"));

        // Late member-profile resolution (an SDK Set diff) updates the same
        // row in place.
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
