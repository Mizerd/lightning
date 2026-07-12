// Runtime-facing regression test for the 0.5.13 defect where
// TimelinePane.qml referenced `PaginationController.Hidden/Loading/Failed`
// without the type ever being registered for QML — every load of the pane
// threw "ReferenceError: PaginationController is not defined". A text-scan
// test (see QmlBindingContractTest.cpp) cannot catch this class of bug: it
// never actually asks a QQmlEngine to evaluate the bindings. This test does:
// it boots a real AppController on the mock backend, loads the real
// TimelinePane.qml through the real "MatrixClient" QML module, and asserts
// zero engine warnings while driving it through Hidden, Loading, and
// room-switch presentation states.
#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 2000;
}

class TimelinePaneQmlTest : public QObject
{
    Q_OBJECT

private:
    // Logs in on the mock backend and waits for the room list to populate.
    // Returns the room id at `row` of the (client-order) room list.
    static QString loginAndRoomIdAt(AppController &controller, int row)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        // startSync() (called synchronously from onLoginSucceeded) populates
        // the room list via a direct-connection signal, so it should already
        // be non-empty; poll briefly as a safety margin only.
        for (int i = 0; i < 50 && controller.roomList()->rowCount() <= row; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() <= row)
            return {};
        const QModelIndex idx = controller.roomList()->index(row, 0);
        return controller.roomList()
            ->data(idx, RoomListModel::RoomIdRole)
            .toString();
    }

    // The first stateGroupId belonging to a group leader in the currently
    // loaded timeline, or empty if none.
    static QString firstStateGroupId(TimelineModel *timeline)
    {
        for (int i = 0; i < timeline->rowCount(); ++i) {
            const QModelIndex idx = timeline->index(i);
            if (timeline->data(idx, TimelineModel::StateGroupLeaderRole).toBool())
                return timeline->data(idx, TimelineModel::StateGroupIdRole).toString();
        }
        return {};
    }

private Q_SLOTS:
    // Defect A (0.5.14 checkpoint 1): instantiating the real TimelinePane.qml
    // must not throw "PaginationController is not defined", with no room
    // open at all (the header binding evaluates unconditionally on load).
    void timelinePaneInstantiatesWithoutReferenceError()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);

        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        // loadFromModule() completes synchronously for a compiled qrc
        // module, so objectCreated may already have fired; only wait if it
        // genuinely has not.
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        QVERIFY(createdSpy.at(0).at(0).value<QObject *>() != nullptr);
        QCOMPARE(warnings, QStringList{});

        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(header != nullptr);
        // Hidden state: no room open, so the pagination header collapses.
        QCOMPARE(header->height(), 0.0);
    }

    // Defect A: loading presentation state must render the header visibly,
    // and it must collapse again once the batch completes — proven through
    // the real object graph, not the isolated controller unit test.
    void loadingStateExpandsHeaderThenCollapses()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        // "!general:mock.local" is seeded with 2 pages remaining — pick it
        // explicitly so the request is guaranteed to be accepted.
        const QString generalId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(generalId);
        QCOMPARE(controller.pagination()->roomId(), generalId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(header != nullptr);

        // TimelinePane.qml's own ListView wiring (maybeFillViewport() on
        // Component.onCompleted, requestNearTop() on atYBeginning) already
        // requests a batch as soon as the pane loads, for a seeded room
        // shorter than the viewport — exactly the "automatic viewport
        // filling" behavior this header must reflect. So the pane may
        // already be Loading the instant it is created; assert the header
        // tracks whichever state that leaves it in rather than assuming
        // Hidden.
        auto expectedHeight = [&] {
            return controller.pagination()->presentationState()
                           == PaginationController::Loading
                       ? 32.0
                       : 0.0;
        };
        QCOMPARE(header->height(), expectedHeight());
        QVERIFY(controller.pagination()->presentationState()
                != PaginationController::Failed);

        // Let every automatically triggered batch resolve (the mock backend
        // settles each one ~300ms later, and a freshly prepended page can
        // immediately trigger another viewport-fill request) until the pane
        // stops requesting more — "!general:mock.local" has exactly 2 pages
        // of seeded history, so this always terminates well within the
        // timeout. The header height is asserted here too (not just the
        // controller state) because it is bound to a QML property that only
        // re-evaluates on PaginationController::stateChanged(); a regression
        // that drops that signal on batch completion leaves the header
        // frozen on "Loading" forever even though the C++ getter already
        // reports Hidden — exactly what a C++-only unit test cannot catch.
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.pagination()->presentationState(),
                                  PaginationController::Hidden, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 0.0, 5000);
        QCOMPARE(warnings, QStringList{});
    }

    // Defect A: switching rooms must not leak the previous room's
    // presentation state into the newly opened room (generation isolation).
    void roomSwitchResetsPresentationState()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());

        const QString generalId = QStringLiteral("!general:mock.local");
        const QString dmId = QStringLiteral("!dm-bob:mock.local");

        controller.setCurrentRoomId(generalId);
        controller.pagination()->requestNearTop();
        QCOMPARE(controller.pagination()->presentationState(),
                 PaginationController::Loading);

        // Switching away mid-request must not leave the new room "busy".
        controller.setCurrentRoomId(dmId);
        QCOMPARE(controller.pagination()->roomId(), dmId);
        QCOMPARE(controller.pagination()->presentationState(),
                 PaginationController::Hidden);
    }

    // Defect B (0.5.14 checkpoint 2): clicking Expand on a room-activity
    // group did nothing. Root cause: the summary row referenced the bare
    // `ListView.view` attached property, which is only populated on the
    // delegate's own root item — not on a nested child — so it silently
    // resolved to null (fixed to `root.ListView.view`; pinned by
    // QmlBindingContractTest::stateActivityQualifiesListViewViewOnNestedControls).
    //
    // This test drives the real expand/collapse STATE MACHINE — the exact
    // `stateGroupExpanded`/`toggleStateGroup` functions the summary row's
    // TapHandler and Keys.onPressed call — through the real compiled
    // TimelinePane.qml and the real seeded "!devs:mock.local" state-change
    // group, via QMetaObject::invokeMethod rather than a synthesized mouse
    // click. A genuine end-to-end click/keyboard simulation was attempted
    // but had to be abandoned: this sandbox's offscreen QPA platform never
    // drives ListView's polish-based delegate incubation (confirmed with a
    // trivial `model: 5` / `Text` delegate ListView, which also never
    // populated), so no MessageDelegate — state-activity or otherwise —
    // ever becomes a real, clickable item here. Given that hard
    // environment limit, this is the strongest check available: it proves
    // the actual QML function wiring (not a re-implementation of it)
    // toggles correctly and resets across a room switch.
    void stateGroupExpansionTogglesAndResetsOnRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        // "!devs:mock.local" is seeded with two consecutive state-change
        // events (a membership join + a profile change) forming one group.
        const QString devsId = QStringLiteral("!devs:mock.local");
        const QString generalId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(devsId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *listView = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(listView != nullptr);

        const QString groupId = firstStateGroupId(controller.timeline());
        QVERIFY(!groupId.isEmpty());

        auto isExpanded = [&] {
            QVariant result;
            QMetaObject::invokeMethod(listView, "stateGroupExpanded",
                                      Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, groupId));
            return result.toBool();
        };
        auto toggle = [&] {
            QMetaObject::invokeMethod(listView, "toggleStateGroup",
                                      Q_ARG(QVariant, groupId));
        };

        QVERIFY(!isExpanded());
        toggle();
        QVERIFY(isExpanded());
        toggle();
        QVERIFY(!isExpanded());
        toggle();
        QVERIFY(isExpanded());

        // Room switch must not leak expansion into (or out of) another
        // room's identically-keyed lookup — TimelinePane.qml's
        // onModelReset handler resets expandedStateGroups to {}.
        controller.setCurrentRoomId(generalId);
        QVERIFY(!isExpanded());

        QCOMPARE(warnings, QStringList{});
    }
};

int main(int argc, char *argv[])
{
    // Real Qt Quick item creation (even offscreen) needs a QGuiApplication,
    // matching main.cpp's application class exactly.
    QGuiApplication app(argc, argv);
    TimelinePaneQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "TimelinePaneQmlTest.moc"
