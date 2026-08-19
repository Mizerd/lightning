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

#include <limits>

#include <QGuiApplication>
#include <QClipboard>
#include <QWheelEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTimer>

#include <QBuffer>
#include <QImage>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "media/MediaImageProvider.h"
#include "models/PaginationController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "threads/ThreadController.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 2000;
}

namespace {
// Keeps a scroll session open across an asynchronous wait the way a reader
// who keeps swiping does: the 250ms settle timer is restarted periodically
// until the caller destroys this. Restarting ONCE and then waiting races the
// timer against the round trip and fails intermittently.
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
        // 20ms, not 50ms: the multi-batch near-top tests hold this across
        // several real network round trips, several times longer than the
        // single-batch tests this helper was written for.
        // 50ms leaves only 5x margin against the 250ms settle timeout, and
        // under the CPU contention of a full parallel test/build run that
        // margin was observed to slip often enough to fire the settle timer
        // MID-RUN — a real scheduling race in this harness, not a
        // production bug (the mechanism under test reacts correctly either
        // way; what breaks is a test asserting exactly one reconcile after
        // an unintentionally early one landed). More headroom, not a
        // capability change.
        m_ticker.start(20);
    }
    ~GestureHold() { m_ticker.stop(); }

private:
    QTimer m_ticker;
};

// Captures every message logged while alive (same pattern as
// GifKeyConfigTest.cpp's LogCapture) — used here to verify the
// LIGHTNING_SCROLL_TRACE "scroll-gesture" line actually renders every
// field, since console.info() in QML does not otherwise surface to a
// QSignalSpy or a property the test can read directly.
class LogCapture
{
public:
    LogCapture()
    {
        s_messages.clear();
        m_previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &msg) {
                s_messages.append(msg);
            });
    }
    ~LogCapture() { qInstallMessageHandler(m_previous); }
    static QStringList messages() { return s_messages; }

private:
    QtMessageHandler m_previous = nullptr;
    inline static QStringList s_messages;
};
} // namespace

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

    // v0.6.0: first loaded event in the current room model that other
    // events name as their thread root (the mock thread fixture).
    static QString fixtureThreadRootId(AppController &controller)
    {
        auto *timeline = controller.timeline();
        for (int row = 0; row < timeline->rowCount(); ++row) {
            const QString rootId = timeline
                ->data(timeline->index(row, 0), TimelineModel::ThreadRootIdRole)
                .toString();
            if (!rootId.isEmpty())
                return rootId;
        }
        return {};
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

    // A Repeater reparents its delegate items into ITS OWN parent item
    // (here, the reactions Flow) for correct positioner layout — that
    // reparenting is QQuickItem::setParentItem() only, not QObject::
    // setParent(), so Repeater-created delegates never become proper
    // QObject-tree descendants of `root` and QObject::findChildren() can
    // never see them (confirmed by direct inspection: the two chip
    // Rectangles exist, both correctly objectName'd and correctly sized,
    // as childItems() siblings of the Repeater under the Flow — just
    // unreachable via the QObject tree). Every OTHER findChild/findChildren
    // use elsewhere in this file targets items declared directly in QML,
    // which stay QObject-tree reachable; this helper is only needed for
    // Repeater-instantiated content.
    static QList<QQuickItem *> findVisualChildren(QQuickItem *parent,
                                                  const QString &name)
    {
        QList<QQuickItem *> result;
        if (!parent)
            return result;
        const auto kids = parent->childItems();
        for (auto *child : kids) {
            if (child->objectName() == name)
                result.append(child);
            result.append(findVisualChildren(child, name));
        }
        return result;
    }

    // ── Row addressing on the solid timeline (1e50f6a) ──────────────────
    //
    // The timeline stopped being a ListView: it is a rotated Flickable +
    // Column, every loaded row is instantiated, and its row API is VIEW-row
    // based (view row 0 = the newest message, at content y 0, which the
    // rotation puts at the physical bottom). The old ListView API these
    // tests used — positionViewAtIndex / positionViewAtBeginning /
    // itemAtIndex — no longer exists, and invoking it silently returned
    // false, which is what these cases were actually failing on.
    //
    // Model (SOURCE) row 0 is the OLDEST message; the pre-rewrite ListView
    // bound `model: app.timeline` directly, so its indices WERE source rows.
    // These helpers keep that meaning at the call sites and do the one
    // conversion in a single place.

    // Source row -> view row, through the pane's own mapping (which is
    // anchored on the model total, not on `count` — the two differ while a
    // paginated page is still draining out).
    static int viewRowForSourceRow(QQuickItem *timeline, int sourceRow)
    {
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "viewRowForSourceRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(sourceRow))))
            return -1;
        return out.toInt();
    }

    // The instantiated delegate for a SOURCE row, or nullptr.
    static QQuickItem *itemForSourceRow(QQuickItem *timeline, int sourceRow)
    {
        const int viewRow = viewRowForSourceRow(timeline, sourceRow);
        if (viewRow < 0)
            return nullptr;
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(viewRow))))
            return nullptr;
        return out.value<QQuickItem *>();
    }

    // Park a SOURCE row at the viewport's physical top — the intent the old
    // positionViewAtIndex(row, ListView.Beginning) carried.
    static bool positionAtSourceRow(QQuickItem *timeline, int sourceRow)
    {
        const int viewRow = viewRowForSourceRow(timeline, sourceRow);
        if (viewRow < 0)
            return false;
        return QMetaObject::invokeMethod(timeline, "positionViewAtViewRow",
                                         Q_ARG(QVariant, QVariant(viewRow)),
                                         Q_ARG(QVariant, QVariant(false)));
    }

    // Park the reader at the TOP EDGE — the oldest loaded row — which is
    // where near-top backfill fires. This is what positionViewAtBeginning()
    // meant on the old top-to-bottom ListView (contentY 0). On the rotated
    // view the oldest end is the HIGH end of the scroll range, so it is
    // wheelMaxY(). Deliberately a direct contentY write rather than
    // goToEarliestLoaded(), which additionally re-runs pagination and
    // restarts the settle timer — side effects the old call did not have
    // and which several of these fixtures are specifically controlling.
    static bool positionAtTopEdge(QQuickItem *timeline)
    {
        QVariant maxY;
        if (!QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                       Q_RETURN_ARG(QVariant, maxY)))
            return false;
        return timeline->setProperty("contentY", maxY.toDouble());
    }

    // The pane's own wheelMinY()/wheelMaxY() (originY -+ topMargin/
    // bottomMargin), NOT a hand-rolled originY/contentHeight-height guess.
    // A wheel-glide fixture that feeds the controller a narrower range than
    // production's real bound clamps the simulated glide against a floor
    // that does not exist in the app, which silently hides whatever the
    // fixture meant to exercise past that point.
    static bool wheelBounds(QQuickItem *timeline, double *minY, double *maxY)
    {
        QVariant minV, maxV;
        if (!QMetaObject::invokeMethod(timeline, "wheelMinY",
                                       Q_RETURN_ARG(QVariant, minV)))
            return false;
        if (!QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                       Q_RETURN_ARG(QVariant, maxV)))
            return false;
        *minY = minV.toDouble();
        *maxY = maxV.toDouble();
        return true;
    }

    // QML warnings that are properties of the MOCK FIXTURE, not of the code
    // under test. On the mock backend `mediaThumbUrl` is a plain http URL
    // (the media bridge is the Rust path), so any row carrying an image asks
    // Qt to resolve `mock.local` and Qt logs one host-not-found warning.
    // That is a DNS fact about the test host, not a QML defect — and every
    // other warning still fails the assertion it appears in.
    // Pinned to the MOCK host specifically: a fixture that accidentally
    // reached a real remote host must still fail, so this cannot be a
    // blanket "any unresolvable image host" filter.
    static QStringList realWarnings(const QStringList &warnings)
    {
        QStringList out;
        for (const QString &w : warnings) {
            if (w.contains(QLatin1String("QQuickImage: Host mock.local"))
                && w.contains(QLatin1String("not found"))) {
                continue;
            }
            out << w;
        }
        return out;
    }

private Q_SLOTS:
    // The actual room-activity component must materialize typed child rows,
    // not merely toggle an expansion bit in the containing ListView.
    void roomActivityComponentExpandsVisibleTypedEntries()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("RoomActivityDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(420, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();

        QVariantList entries;
        entries.append(QVariantMap{
            { QStringLiteral("stableEventId"), QStringLiteral("item-join") },
            { QStringLiteral("eventKind"), QStringLiteral("membership") },
            { QStringLiteral("actorDisplayName"), QStringLiteral("Alice") },
            { QStringLiteral("affectedMemberDisplayName"), QStringLiteral("Bob") },
            { QStringLiteral("description"), QStringLiteral("Bob joined the room.") },
        });
        entries.append(QVariantMap{
            { QStringLiteral("stableEventId"), QStringLiteral("item-topic") },
            { QStringLiteral("eventKind"), QStringLiteral("m.room.topic") },
            { QStringLiteral("actorDisplayName"), QStringLiteral("Alice") },
            { QStringLiteral("description"), QStringLiteral("Alice changed the room topic.") },
        });
        QVERIFY(root->setProperty("entries", entries));

        auto *expanded = root->findChild<QQuickItem *>(
            QStringLiteral("stateActivityExpandedContent"));
        QVERIFY(expanded != nullptr);
        QCOMPARE(root->property("entryCount").toInt(), 2);
        QCOMPARE(expanded->isVisible(), false);
        QCOMPARE(expanded->height(), 0.0);

        QVERIFY(root->setProperty("expanded", true));
        QTRY_VERIFY_WITH_TIMEOUT(expanded->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("renderedEntryCount").toInt(),
                                  2, kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(root->property("expandedContentHeight").toReal() > 0.0,
                                 kSignalTimeoutMs);

        QVERIFY(root->setProperty("expanded", false));
        QTRY_VERIFY_WITH_TIMEOUT(!expanded->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(expanded->height(), 0.0, kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

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
        QCOMPARE(realWarnings(warnings), QStringList{});

        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(header != nullptr);
        // Hidden state: no room open, so the pagination header collapses.
        QCOMPARE(header->height(), 0.0);
    }

    // v0.7.1: with no room selected the Home surface replaces the bare
    // "select a room" placeholder and the composer is hidden; selecting a
    // room hides Home and restores the composer.
    void homeSurfaceShownWithNoRoomAndComposerHidden()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        controller.setCurrentRoomId(QString()); // no room selected

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
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});

        auto *home = root->findChild<QQuickItem *>(QStringLiteral("homePane"));
        auto *composer =
            root->findChild<QQuickItem *>(QStringLiteral("composerCard"));
        QVERIFY(home != nullptr);
        QVERIFY(composer != nullptr);
        QVERIFY(home->isVisible());       // Home replaces the empty state
        QVERIFY(!composer->isVisible());  // composer hidden with no room

        // Selecting a room flips both.
        controller.setCurrentRoomId(roomId);
        QCoreApplication::processEvents();
        QVERIFY(!home->isVisible());
        QVERIFY(composer->isVisible());
        QCOMPARE(realWarnings(warnings), QStringList{});
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
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // 2026-08-19 regression: delegates reach the pane ONLY through their
    // `timelineView` (the rotated Flickable), so the reader-list opener
    // must be a property-function ON that Flickable — as a pane-root
    // function it was unreachable and the delegate's existence guard
    // silently swallowed every click on the receipt facepile. This case
    // fails on that tree: the property did not exist on the view.
    void receiptListOpenerIsReachableFromDelegatesAndOpens()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        // A Popup needs a real window/overlay to open.
        QQuickWindow window;
        window.resize(680, 480);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        // The opener exists on the view the delegates actually hold...
        const QVariant opener = timeline->property("openReceiptList");
        QVERIFY(opener.isValid());
        QVERIFY(qvariant_cast<QJSValue>(opener).isCallable());

        // ...and invoking it exactly as the delegate does opens the ONE
        // shared popover with the handed data.
        auto *popover =
            root->findChild<QObject *>(QStringLiteral("receiptListPopover"));
        QVERIFY(popover != nullptr);
        QVERIFY(!popover->property("visible").toBool());
        QQmlExpression call(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "openReceiptList([{userId: \"@a:mock.local\","
                " displayName: \"A\", avatarMxc: \"\", tsMs: 0}],"
                " 3, Qt.point(40, 40))"));
        call.evaluate();
        QVERIFY2(!call.hasError(),
                 call.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(popover->property("visible").toBool(), 5000);
        QCOMPARE(popover->property("totalOthers").toInt(), 3);
        QCOMPARE(popover->property("readers").toList().size(), 1);

        // 2026-08-19 feedback: the card is content-sized, never a fixed
        // window share — one reader gets a small card (the share-sized
        // version measured >=160 here), and a long list caps at half the
        // window with the list scrolling inside.
        const qreal oneReaderHeight = popover->property("height").toReal();
        QVERIFY2(oneReaderHeight < 140,
                 qPrintable(QStringLiteral("h=%1").arg(oneReaderHeight)));

        QQmlExpression closeCall(qmlContext(timeline), timeline,
                                 QStringLiteral("receiptListPopover.close()"));
        closeCall.evaluate();
        QVERIFY(!closeCall.hasError());
        QQmlExpression bigCall(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "openReceiptList(Array.from({length: 30}, function(v, i) {"
                " return {userId: \"@u\" + i + \":mock.local\","
                " displayName: \"User \" + i, avatarMxc: \"\","
                " tsMs: 1000 + i}; }), 30, Qt.point(40, 40))"));
        bigCall.evaluate();
        QVERIFY2(!bigCall.hasError(),
                 bigCall.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(popover->property("visible").toBool(), 5000);
        // The 30 delegates instantiate asynchronously; the content-driven
        // height follows the list's contentHeight, so poll for it.
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("height").toReal() > oneReaderHeight, 5000);
        const qreal manyHeight = popover->property("height").toReal();
        QVERIFY(manyHeight <= window.height() * 0.5 + 1.0);
        auto *readerList = popover->findChild<QQuickItem *>(
            QStringLiteral("receiptReaderList"));
        QVERIFY(readerList != nullptr);
        // Capped: the rows genuinely overflow and scroll inside.
        QTRY_VERIFY_WITH_TIMEOUT(
            readerList->property("contentHeight").toReal()
                > readerList->height() + 1.0, 5000);
    }

    // 0.5.17: controller state changes alter the pagination header height,
    // which alters ListView contentHeight. Dispatching viewport fill directly
    // from that geometry notification re-entered the header state binding.
    // Exercise the real pane through initial fill, loading, reached-start and
    // repeated resizes; queued/coalesced geometry checks must settle without a
    // binding-loop warning or an uncontrolled request storm. Failed/Retry are
    // driven deterministically by PaginationControllerTest's FakeClient while
    // this runtime test pins the QML geometry side of the cycle.
    void paginationGeometryChangesAreQueuedAndBounded()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->failNextPaginationForTest();

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) {
                        // The offscreen test host has no resolver for the
                        // mock media origin; the bottom-anchored view now
                        // legitimately instantiates the image fixture row,
                        // whose fetch attempt reports this environmental
                        // (non-QML) warning.
                        if (e.toString().contains(
                                QLatin1String("Host mock.local not found")))
                            continue;
                        warnings << e.toString();
                    }
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy stateSpy(controller.pagination(),
                            &PaginationController::stateChanged);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(timeline != nullptr);
        QVERIFY(header != nullptr);

        for (const QSize size : { QSize(680, 420), QSize(900, 720),
                                  QSize(560, 360), QSize(720, 640) }) {
            root->setSize(QSizeF(size));
            QCoreApplication::processEvents();
        }

        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->failed(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 32.0, 5000);
        controller.pagination()->retry();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QVERIFY(!controller.pagination()->failed());
        // Drive the second seeded page explicitly. The headless test does
        // not polish delegates, so contentHeight cannot legitimately ask for
        // this page on its own; the real header still traverses Loading back
        // to Hidden/reached-start around the request.
        controller.pagination()->requestNearTop();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->reachedStart(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 0.0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("viewportFillCheckScheduled").toBool(), 5000);
        QVERIFY2(stateSpy.count() < 40,
                 qPrintable(QStringLiteral("pagination state storm: %1")
                                .arg(stateSpy.count())));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // A transient first viewport-fill failure must stay internal and retry
    // through the real pane/controller interaction. Existing messages then
    // appear without invoking PaginationController::retry() from the test.
    void transientInitialHistoryFailureRetriesWithoutUserAction()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->failNextPaginationForTest(/*transient=*/true);
        controller.pagination()->setAutomaticRetryPolicyForTest(3, 1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(createdSpy.at(0).at(0).value<QObject *>() != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), 5000);
        QVERIFY(!controller.pagination()->failed());
        QVERIFY(controller.timeline()->rowCount() > 0);
        QVERIFY2(completedSpy.count() < 10, "initial history request storm");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // 0.5.17: a populated encrypted timeline containing a long decrypted
    // body used to create that delegate at a transient 1px text width. Its
    // enormous temporary height made ListView discard/recreate the visible
    // range forever and starved the GUI event loop. Load the actual
    // encrypted mock room (decrypted/undecryptable/missing-profile/reply/media
    // pending plus a >4K body), resize it, switch away and back, and prove
    // timers and the window survive without QML warnings.
    void populatedEncryptedTimelineRemainsResponsive()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString encryptedId = QStringLiteral("!devs:mock.local");
        controller.setCurrentRoomId(encryptedId);

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
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        QTimer heartbeat;
        heartbeat.setSingleShot(true);
        QSignalSpy heartbeatSpy(&heartbeat, &QTimer::timeout);
        heartbeat.start(100);
        QVERIFY2(heartbeatSpy.wait(kSignalTimeoutMs),
                 "GUI event loop was starved by timeline delegate layout");
        QVERIFY(window.isVisible());

        for (const QSize size : { QSize(520, 360), QSize(940, 720),
                                  QSize(760, 620) }) {
            window.resize(size);
            root->setSize(QSizeF(size));
            QCoreApplication::processEvents();
        }
        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QCoreApplication::processEvents();
        controller.setCurrentRoomId(encryptedId);

        heartbeatSpy.clear();
        heartbeat.start(100);
        QVERIFY2(heartbeatSpy.wait(kSignalTimeoutMs),
                 "room switch left timeline layout unresponsive");
        QVERIFY(window.isVisible());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The offscreen test QPA does not polish ListView delegates, so create
    // the real MessageDelegate directly with the real TimelineModel role
    // schema. Component completion happens at width zero, exactly the phase
    // that formerly measured the long body at one pixel wide.
    void longEncryptedMessageDelegateUsesBoundedStartupWidth()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QString{});
        fixture.insert(QStringLiteral("senderInitials"),
                       QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Large encrypted fixture line.\n")
                           .repeated(160));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("isEncrypted"), true);
        fixture.insert(QStringLiteral("isDecrypted"), true);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(640);
        QCoreApplication::processEvents();

        auto *body = root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QVERIFY(body->property("text").toString().size() > 4000);
        QVERIFY(body->width() > 100.0);
        QVERIFY(body->height() > 0.0);
        QVERIFY(body->height() < 20000.0);
        QVERIFY(root->implicitHeight() < 20000.0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.6.5 (C6, reviewer M4): a runtime guard for the reaction-chip
    // height defect — before the fix, each chip's Rectangle derived its
    // implicitHeight from reactionRow.implicitHeight, and a plain Row does
    // not vertically centre children of different natural heights, so a
    // taller-metric emoji glyph grew the WHOLE chip while a shorter one
    // stayed at the 22px floor. Two reactions with genuinely different
    // Unicode composition (a single-codepoint emoji vs. one combined with a
    // variation selector — a well-known source of divergent font-reported
    // metrics even at the identical pixel size) exercise that gap. On the
    // fixed code both labels are pinned to a deterministic 16px content
    // height (MessageDelegate.qml's reactionRow), so every chip lands on
    // the same height regardless of which glyph it holds — this is a
    // static-vs-static comparison of two SIBLING chips actually rendered
    // side by side, not a comparison against a hardcoded pixel constant,
    // so it stays valid across any future spacing/padding retune.
    void reactionChipsShareOneHeightAcrossDifferentEmoji()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("Reaction fixture"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        // Thumbs-up: a single codepoint. Heart: base + U+FE0F variation
        // selector-16 (emoji presentation) — the two-codepoint combination
        // historically diverges from single-codepoint glyphs in reported
        // font metrics on some font stacks, which is exactly the class of
        // difference the old Row-based layout let leak into chip height.
        fixture.insert(QStringLiteral("reactions"), QVariantList{
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("👍") },
                        { QStringLiteral("count"), 1 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"),
                          QStringLiteral("❤️") },
                        { QStringLiteral("count"), 12 },
                        { QStringLiteral("byMe"), true } },
        });

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(640, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        // Root cause of the earlier "0 chips" failure: QObject::
        // findChildren() cannot see Repeater-instantiated delegates (see
        // findVisualChildren's comment) — root.reactionsList() and the
        // Repeater's own `count` property were always correct (confirmed
        // by direct inspection during triage). Walk the actual QQuickItem
        // scene-graph tree instead.
        const auto chips = findVisualChildren(root, QStringLiteral("reactionChip"));
        QCOMPARE(chips.size(), 2);
        QVERIFY2(chips.at(0)->height() >= 22.0,
                 "chip height below the 22px design floor");
        QCOMPARE(chips.at(0)->height(), chips.at(1)->height());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void rightClickMenuSnapshotsStableEventAndClosesOnRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));

        int messageRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            const QModelIndex idx = controller.timeline()->index(row);
            if (!controller.timeline()->data(
                    idx, TimelineModel::IsVirtualRole).toBool()
                && !controller.timeline()->data(
                    idx, TimelineModel::IsStateActivityRole).toBool()) {
                messageRow = row;
                break;
            }
        }
        QVERIFY(messageRow >= 0);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            fixture.insert(QString::fromUtf8(it.value()),
                           controller.timeline()->data(
                               controller.timeline()->index(messageRow), it.key()));
        }
        const QString eventId = fixture.value(QStringLiteral("eventId")).toString();
        QVERIFY(!eventId.isEmpty());

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(640, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier,
                          QPoint(180, qMax(4, qRound(root->height() / 2))));
        QTRY_COMPARE_WITH_TIMEOUT(root->property("menuEventId").toString(),
                                  eventId, kSignalTimeoutMs);
        auto *menu = root->findChild<QObject *>(
            QStringLiteral("messageContextMenu"));
        QVERIFY(menu != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(),
                                 kSignalTimeoutMs);

        const QString visibleText = controller.timeline()->visibleTextForEvent(eventId);
        QVERIFY(!visibleText.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root, "copyToClipboard",
                                          Q_ARG(QVariant, visibleText)));
        QCOMPARE(QGuiApplication::clipboard()->text(), visibleText);
        const QString permalink = controller.timeline()->messagePermalink(eventId);
        QVERIFY(QMetaObject::invokeMethod(root, "copyToClipboard",
                                          Q_ARG(QVariant, permalink)));
        QCOMPARE(QGuiApplication::clipboard()->text(), permalink);
        QVERIFY(!permalink.contains(QStringLiteral("access_token"),
                                    Qt::CaseInsensitive));

        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY_WITH_TIMEOUT(!menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        root->forceActiveFocus();
        QVERIFY(root->hasActiveFocus());
        QTest::keyClick(&window, Qt::Key_Menu);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        QCOMPARE(root->property("menuEventId").toString(), eventId);

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(!menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        QCOMPARE(root->property("menuEventId").toString(), QString{});
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.7: the timeline ListView pools MessageDelegates (reuseItems). This
    // guards the reuse contract: resetForReuse() (wired to ListView.onReused)
    // must scrub every transient, non-model-bound field so a pooled row can
    // never carry a stale popup target or dialog body onto the next message.
    void pooledDelegateReuseScrubsTransientState()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));

        int messageRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            const QModelIndex idx = controller.timeline()->index(row);
            if (!controller.timeline()->data(idx, TimelineModel::IsVirtualRole).toBool()
                && !controller.timeline()->data(
                       idx, TimelineModel::IsStateActivityRole).toBool()) {
                messageRow = row;
                break;
            }
        }
        QVERIFY(messageRow >= 0);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            fixture.insert(QString::fromUtf8(it.value()),
                           controller.timeline()->data(
                               controller.timeline()->index(messageRow), it.key()));
        }

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        // Stale the transient state as if the previous row had an open
        // context menu and an inspected details payload. (Reaction targets
        // no longer live on the delegate at all — the view-shared picker
        // snapshots the event id at open, so a recycled delegate cannot
        // carry one.)
        QVERIFY(root->setProperty("menuEventId",
                                  QStringLiteral("$stale-menu:mock.local")));
        QCOMPARE(root->property("menuEventId").toString(),
                 QStringLiteral("$stale-menu:mock.local"));
        QVERIFY(!root->property("reactionEventId").isValid());

        // Simulate the pool handing this delegate to a new row.
        QVERIFY(QMetaObject::invokeMethod(root, "resetForReuse"));

        QCOMPARE(root->property("menuEventId").toString(), QString{});
        // No engine warnings means resetForReuse() resolved every id it
        // touches (details dialog, popups, preview refresh) cleanly.
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomActivitySettingCollapsesOnlyActivityDelegates()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        controller.settings()->setShowRoomActivity(true);

        int activityRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            if (controller.timeline()->data(
                    controller.timeline()->index(row),
                    TimelineModel::StateGroupLeaderRole).toBool()) {
                activityRow = row;
                break;
            }
        }
        QVERIFY(activityRow >= 0);
        const int underlyingCount = controller.timeline()->rowCount();

        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            fixture.insert(QString::fromUtf8(it.value()),
                           controller.timeline()->data(
                               controller.timeline()->index(activityRow),
                               it.key()));
        }

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *activity = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(activity != nullptr);
        QQuickWindow window;
        window.resize(640, 320);
        activity->setParentItem(window.contentItem());
        activity->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(activity->isVisible());
        // The activity delegate's height is produced by delegate layout, which
        // the offscreen platform completes asynchronously; wait for it rather
        // than assuming a single processEvents() sufficed (the later checks in
        // this test already use QTRY_VERIFY for the same reason).
        QTRY_VERIFY_WITH_TIMEOUT(activity->implicitHeight() > 0.0,
                                 kSignalTimeoutMs);

        controller.settings()->setShowRoomActivity(false);
        QTRY_VERIFY_WITH_TIMEOUT(!activity->isVisible(), kSignalTimeoutMs);
        QCOMPARE(activity->implicitHeight(), 0.0);
        QCOMPARE(controller.timeline()->rowCount(), underlyingCount);

        controller.settings()->setShowRoomActivity(true);
        QTRY_VERIFY_WITH_TIMEOUT(activity->isVisible(), kSignalTimeoutMs);
        QVERIFY(activity->implicitHeight() > 0.0);
        QCOMPARE(controller.timeline()->rowCount(), underlyingCount);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void settingsControlTracksRoomActivityPreference()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setShowRoomActivity(true);
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
                              QStringLiteral("SettingsScreen"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *check = root->findChild<QObject *>(
            QStringLiteral("showRoomActivityCheck"));
        QVERIFY(check != nullptr);
        QCOMPARE(check->property("checked").toBool(), true);

        controller.settings()->setShowRoomActivity(false);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), false,
                                  kSignalTimeoutMs);
        controller.settings()->setShowRoomActivity(true);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), true,
                                  kSignalTimeoutMs);
        QSignalSpy settingSpy(controller.settings(),
                              &SettingsManager::showRoomActivityChanged);
        QVERIFY(QMetaObject::invokeMethod(check, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.settings()->showRoomActivity(),
                                  false, kSignalTimeoutMs);
        QCOMPARE(settingSpy.count(), 1);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: the Settings mouse-wheel-speed control reflects the persisted
    // value, selecting a value updates the setting, and the setting drives the
    // shared TimelineScrollController (default Fast) without QML warnings.
    void settingsControlTracksWheelSpeedPreference()
    {
        AppController controller(AppController::MockBackend);
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        // QSettings persists across test runs, so normalise to the documented
        // default (Fast) rather than assuming a pristine store. The default
        // value itself is covered in the isolated SettingsSessionTest.
        controller.settings()->setTimelineWheelSpeed(1);
        QCOMPARE(controller.settings()->timelineWheelSpeed(), 1);
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::Fast);

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
                              QStringLiteral("SettingsScreen"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *combo = root->findChild<QObject *>(
            QStringLiteral("timelineWheelSpeedCombo"));
        QVERIFY(combo != nullptr);
        // Fast (value 1) is the second entry (index 1). The ComboBox resolves
        // its index from the model on completion, so allow it to settle.
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 1,
                                  kSignalTimeoutMs);

        // Changing the setting updates the control …
        controller.settings()->setTimelineWheelSpeed(2);       // Very fast
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 2,
                                  kSignalTimeoutMs);
        // … and drives the controller immediately (no timeline restart).
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::VeryFast);

        controller.settings()->setTimelineWheelSpeed(0);       // Standard
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 0,
                                  kSignalTimeoutMs);
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::Standard);
        QCOMPARE(realWarnings(warnings), QStringList{});
        // Leave the persisted store back at the default for other runs/tests.
        controller.settings()->setTimelineWheelSpeed(1);
    }

    void sdkReadMarkerRendersNewMessagesDivider()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("itemId"),
                       QStringLiteral("read-marker-stable"));
        fixture.insert(QStringLiteral("isVirtual"), true);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("isRoutineActivity"), false);
        fixture.insert(QStringLiteral("eventType"), 8);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"), QString{});
        fixture.insert(QStringLiteral("senderDisplayName"), QString{});
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("?"));
        fixture.insert(QStringLiteral("showSenderIdentity"), false);
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), false);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(640, 160);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        auto *divider = root->findChild<QQuickItem *>(
            QStringLiteral("unreadDivider"));
        auto *label = root->findChild<QObject *>(
            QStringLiteral("unreadDividerLabel"));
        QVERIFY(divider != nullptr);
        QVERIFY(divider->isVisible());
        QVERIFY(root->implicitHeight() >= 28.0);
        QVERIFY(label != nullptr);
        QCOMPARE(label->property("text").toString(),
                 QStringLiteral("New messages"));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void jumpToLatestPreservesReaderUntilExplicitClick()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

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
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const int before = controller.timeline()->rowCount();
        mock->sendTextMessage(roomId, QStringLiteral("new fixture event"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        QVERIFY(QMetaObject::invokeMethod(jump, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                  true, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: the timeline wheel handler exists, is scoped to the timeline
    // ListView (so wheel input elsewhere cannot move the timeline), and the
    // pane reaches the shared TimelineScrollController through app.timelineScroll
    // without QML warnings/binding loops.
    void wheelHandlerIsPresentAndScopedToTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

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
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        // The handler must live inside the timeline view's own subtree, not on
        // some ancestor — this is what keeps its wheel input from moving
        // anything else (settings, dialogs, sidebars).
        QObject *handler = timeline->findChild<QObject *>(
            QStringLiteral("timelineWheelHandler"));
        QVERIFY(handler != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: a wheel movement upward through beginWheelTo() must leave
    // follow-latest mode immediately (a reader scrolling up is not dragged
    // back down by new messages).
    void wheelUpwardLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

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
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", true));
        const double startY = timeline->property("contentY").toDouble();
        // Target above the current position. The timeline is ROTATED since
        // 1e50f6a: view row 0 (the newest message) sits at content y 0, so
        // moving physically UPWARD — toward older history — INCREASES
        // contentY. The pane's own keyboardPage() encodes the same sign
        // (`beginWheelTo(contentY - direction * height * 0.9)` with
        // direction -1 for up). The old `startY - 200.0` was the
        // pre-rotation direction and now scrolls toward the newest end,
        // which correctly leaves follow-latest engaged — hence the failure.
        QVERIFY(QMetaObject::invokeMethod(timeline, "beginWheelTo",
                                          Q_ARG(QVariant, startY + 200.0)));
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: Jump to latest must cancel an in-flight coalesced wheel motion
    // (no animation fighting the programmatic jump).
    void jumpToLatestCancelsWheelMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

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
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        // Put the shared controller into an active coalesced-motion state.
        scroll->wheelTargetY(120.0, 5000.0, 0.0, 10000.0, 900.0);
        QVERIFY(scroll->motionActive());

        QVERIFY(QMetaObject::invokeMethod(jump, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(scroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: switching rooms cancels the previous room's wheel motion.
    void roomSwitchCancelsWheelMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

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
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        scroll->wheelTargetY(120.0, 5000.0, 0.0, 10000.0, 900.0);
        QVERIFY(scroll->motionActive());

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(scroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.5.19: a REAL discrete wheel event delivered over the pane must route
    // through the timeline WheelHandler into TimelineScrollController — i.e.
    // the handler actually intercepts wheel input (rather than Flickable's
    // default handling silently owning it). Delegate incubation never runs
    // under the offscreen platform, so this asserts the wiring/side effects
    // (motion engaged, follow-latest left) rather than a pixel distance.
    void realWheelEventEngagesControllerAndLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

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
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(!scroll->motionActive());

        // Discrete mouse wheel: no pixelDelta, one +120 notch upward, over the
        // centre of the timeline viewport. Resend until the notch registers
        // (synthesized wheel delivery is not guaranteed in one offscreen pass).
        const QPointF pos(320, 300);
        auto sendNotch = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool engaged = false;
        for (int attempt = 0; attempt < 50 && !engaged; ++attempt) {
            sendNotch();
            engaged = scroll->motionActive();
            if (!engaged)
                QTest::qWait(10);
        }

        // The handler ran, engaged the controller, and left follow-latest.
        QVERIFY2(engaged, "a mouse-wheel notch must engage the motion engine");
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        // A MessageDelegate QQuickImage occasionally tries to resolve a
        // mock.local HTTP URL and logs a benign "Host ... not found" DNS
        // warning (a mock-backend/offscreen artifact, not a QML defect). Ignore
        // that one known-benign line; any other engine warning still fails.
        warnings.removeIf([](const QString &w) {
            return w.contains(QStringLiteral("Host mock.local not found"));
        });
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Regression: a TOUCHPAD (pixelDelta) upward scroll must leave
    // follow-latest, exactly like the mouse-wheel path. The touchpad branch
    // previously only recomputed a wide 40px "near bottom" test and never set
    // stickToBottom=false on upward intent, so a small trackpad nudge near the
    // bottom could not disengage — the next async content-height change then
    // teleported the reader to the newest message (the reported "can't scroll
    // up, it jumps back down" defect on KDE Wayland laptops).
    void touchpadWheelUpwardLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));

        // Touchpad: non-zero pixelDelta upward (+y), no angle notch, scroll
        // phase set — routed through the pixelDelta branch of the handler.
        // Synthesized wheel delivery to the WheelHandler is not guaranteed in a
        // single pass under the offscreen QPA, so resend until the upward
        // intent registers rather than trusting one send.
        const QPointF pos(320, 300);
        auto sendUp = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 48), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate, /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool left = false;
        for (int attempt = 0; attempt < 50 && !left; ++attempt) {
            sendUp();
            left = !timeline->property("stickToBottom").toBool();
            if (!left)
                QTest::qWait(10);
        }
        QVERIFY2(left, "an upward touchpad delta must leave follow-latest");
    }

    // Native-touchpad architecture pass: a high-resolution touchpad gesture
    // must open a scroll SESSION (userScrollActive) that gates every deferred
    // position correction, and that session must CLEAR once input stops so
    // corrections resume. Qt's QML WheelEvent exposes no scroll phase and no
    // device type (phase begin/end is macOS-only even in C++), so the session
    // is inferred from the settle timer restarted on each delta — this proves
    // that heuristic works: active during input, cleared ~250ms after the last
    // event. What the session gates is the ABSOLUTE restore in
    // maintainViewAnchor(); the RELATIVE growth-delta path deliberately runs
    // during a self-driven gesture (see the growth tests below), and is
    // itself deferred while Flickable owns a native drag.
    void touchpadGestureOpensScrollSessionThatGatesCorrections()
    {
        // Enable the per-gesture diagnostics (read once at controller
        // construction) so this test can assert the load-bearing number
        // directly: diagAnchorCorrections — the count of ABSOLUTE restores
        // (the idle path) — must stay 0 while the gesture owns the view,
        // even when contentHeight churns (delegate hydration).
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        // Reader browsing history: not pinned to the bottom, no input yet.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("userScrollActive").toBool(), false);

        // Send touchpad pixel deltas (as KDE Wayland delivers them: pixelDelta
        // set, no angle notch). Delivery of a synthesized wheel event to the
        // WheelHandler is not guaranteed in a single pass under the offscreen
        // QPA, so RESEND until the session opens rather than trusting one send.
        const QPointF pos(320, 300);
        auto sendDelta = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate, /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            sendDelta();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");

        // No viewAnchorId has been captured yet (no settle has happened),
        // so maintainViewAnchor() returns at the `viewAnchorId === ""` guard
        // regardless of userScrollActive — contentY must stay untouched. A
        // real mid-gesture RELATIVE growth correction (the fix for "an image
        // pops up while scrolling and the view jumps") is covered by
        // maintainViewAnchorAppliesGrowthDeltaMidGestureWithoutGlide, which
        // establishes a real anchor first.
        const double before = timeline->property("contentY").toDouble();
        QMetaObject::invokeMethod(timeline, "maintainViewAnchor");
        QCOMPARE(timeline->property("contentY").toDouble(), before);

        // More deltas keep the session open and drive some scroll (delegate
        // hydration → contentHeight churn), the exact condition that used to
        // trigger the mid-gesture correction.
        for (int i = 0; i < 5; ++i)
            sendDelta();
        QVERIFY(timeline->property("userScrollActive").toBool());

        // Upward intent left follow-latest, exactly like the mouse path.
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        // The session clears once input stops (settle timer, ~250ms), so
        // deferred anchor maintenance can resume for later async growth.
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 3000);

        // diagAnchorCorrections counts ONLY the idle absolute-restore path
        // (see maintainViewAnchor()) — it must stay 0 for as long as a
        // gesture owns the view; a non-zero value would be an absolute write
        // fighting the gesture, the defect that pass eliminated. The separate
        // relative-delta path that DOES run mid-gesture is counted by
        // diagGrowthCorrections. (The strict engine-warning
        // check is intentionally omitted here: this test deliberately scrolls
        // through incubating delegates, which can emit transient offscreen
        // binding warnings; warning-freedom of static content is covered by the
        // other pane tests.)
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    // Regression: once the reader has scrolled up (follow-latest left), later
    // content growth — new events AND asynchronous delegate-height settles —
    // must NOT re-pin to the bottom. Bottom-follow is latched to user intent
    // and is never re-asserted from a content/count change, so the reader
    // stays put and Jump-to-latest stays offered.
    void contentGrowthWhileBrowsingDoesNotResnap()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        // Reader browses history: disengaged from the bottom.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        // Several appends in a row exercise onCountChanged + the coalesced
        // onContentHeightChanged reaction. None may re-pin the reader.
        for (int i = 0; i < 4; ++i) {
            const int before = controller.timeline()->rowCount();
            mock->sendTextMessage(
                roomId, QStringLiteral("growth event %1").arg(i));
            QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(),
                                      before + 1, kSignalTimeoutMs);
            QCoreApplication::processEvents();
            QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        }
        QCOMPARE(jump->property("visible").toBool(), true);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // ── v0.5.19 checkpoint 3: keyboard timeline navigation ───────────────
    // Loads the pane into a shown window and focuses the timeline. Delegate
    // incubation never runs offscreen, so these assert the key ROUTING and
    // follow-latest/motion side effects rather than pixel distances.
    void keyboardEndKeyReturnsToLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QVERIFY(timeline->setProperty("stickToBottom", false));

        QTest::keyClick(&window, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                  true, kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Page Up, Home, Space and Shift+Space route to the timeline and start
    // scroll motion (wheelAnimating) when the timeline holds focus.
    void keyboardNavigationKeysStartTimelineMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        const QList<QPair<Qt::Key, Qt::KeyboardModifiers>> keys = {
            {Qt::Key_PageUp, Qt::NoModifier},
            {Qt::Key_PageDown, Qt::NoModifier},
            {Qt::Key_Space, Qt::ShiftModifier},
            {Qt::Key_Space, Qt::NoModifier},
        };
        for (const auto &k : keys) {
            QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
            timeline->forceActiveFocus();
            QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(),
                                     kSignalTimeoutMs);
            QVERIFY(!timeline->property("wheelAnimating").toBool());
            QTest::keyClick(&window, k.first, k.second);
            QVERIFY2(timeline->property("wheelAnimating").toBool(),
                     "navigation key did not start timeline motion");
        }

        // v0.6.0: Home is programmatic navigation like End — it must BYPASS
        // the wheel motion engine (no smooth motion) and land directly on the
        // earliest loaded position.
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QTest::keyClick(&window, Qt::Key_Home, Qt::NoModifier);
        QVERIFY2(!timeline->property("wheelAnimating").toBool(),
                 "Home must jump instantly, not start wheel motion");
        // Home lands on the earliest loaded position. On the ROTATED
        // timeline (1e50f6a) the earliest — oldest — end is the HIGH end of
        // the scroll range, so that position is wheelMaxY(), not wheelMinY()
        // as it was on the old top-to-bottom ListView. goToEarliestLoaded()
        // is literally `contentY = wheelMaxY()`.
        // Pagination can grow content asynchronously (moving wheelMaxY)
        // between the keypress and a later read, so assert race-free: re-run
        // the exact jump goToEarliestLoaded performs and read contentY +
        // wheelMaxY in the SAME event-loop turn (neither call spins the
        // loop), which cannot race an async prepend.
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QVariant maxY;
        QVERIFY(QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                          Q_RETURN_ARG(QVariant, maxY)));
        QCOMPARE(timeline->property("contentY").toDouble(), maxY.toDouble());
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
    }

    // A focused composer keeps its keys: the timeline must not act on them.
    void composerFocusPreventsTimelineKeyHandling()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("composerInput"));
        QVERIFY(timeline != nullptr);
        QVERIFY(composer != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        composer->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(composer->hasActiveFocus(), kSignalTimeoutMs);

        // End would resume follow-latest if the timeline handled it; with the
        // composer focused it must not, and no scroll motion may start.
        QTest::keyClick(&window, Qt::Key_End);
        QTest::keyClick(&window, Qt::Key_PageUp);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.6.5 (C7): the find bar is now a floating composer-family card,
    // detached from the pane's edges by outer Layout margins, instead of a
    // flush full-width strip. It stays an ordinary Layout child rather than
    // an absolute overlay specifically so the timeline ListView's existing
    // find-bar-driven height compensation keeps working — which means the
    // composer, pinned below the timeline's fillHeight Item in the same
    // roomColumn ColumnLayout, must never move when find opens or closes;
    // only the flexible timeline Item between them may resize.
    //
    // Deliberately placed directly after composerFocusPreventsTimelineKey-
    // Handling(): both create and show a real QQuickWindow, and this suite
    // has a documented offscreen-QPA flake when a window-showing test sits
    // immediately before keyboardNavigationKeysStartTimelineMotion() (see
    // the comment on paginationAnchorRestorePreservesConcurrentScroll at
    // the bottom of this file). Staying adjacent to the OTHER already-safe
    // window-showing test, rather than introducing a new adjacency to that
    // one, avoids reproducing it.
    void findBarIsDetachedAndNeverMovesTheComposer()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("composerCard"));
        QVERIFY(composer != nullptr);
        const qreal composerYBeforeOpen =
            composer->mapToScene(QPointF(0, 0)).y();

        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        QCoreApplication::processEvents();

        auto *findBar = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindBar"));
        QVERIFY(findBar != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(findBar->isVisible(), kSignalTimeoutMs);

        // Detached: the card is inset from both the pane's left and right
        // edges — not a flush, full-width strip touching either side.
        const QPointF findBarTopLeft = findBar->mapToScene(QPointF(0, 0));
        const QPointF findBarTopRight =
            findBar->mapToScene(QPointF(findBar->width(), 0));
        QVERIFY2(findBarTopLeft.x() > 0.0,
                 "find bar must not touch the pane's left edge");
        QVERIFY2(findBarTopRight.x() < root->width(),
                 "find bar must not touch the pane's right edge");

        // Opening find only shrinks the flexible timeline Item between the
        // find bar and the composer — the composer itself must not move.
        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);

        auto *findField = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(findField != nullptr);
        QVERIFY(findField->hasActiveFocus());

        QVERIFY(QMetaObject::invokeMethod(root, "closeFind"));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(!findBar->isVisible(), kSignalTimeoutMs);
        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);
        // closeFind() hands focus back to the timeline explicitly rather
        // than leaving the focus scope with no active item.
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
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

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // ── v0.6.0 checkpoint 3: thread panel ─────────────────────────────────
    // Delegate incubation never runs under the offscreen platform, so these
    // assert the panel's controller-driven state machine, visibility wiring,
    // composer send path, and narrow-layout behaviour on the REAL compiled
    // TimelinePane.qml — not pixel geometry of individual replies.
    void threadPanelOpensAndClosesWithController()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->property("visible").toBool(), false);

        const QString rootId = fixtureThreadRootId(controller);
        QVERIFY(!rootId.isEmpty());
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), true,
                                  kSignalTimeoutMs);

        // The panel's pinned root header resolves the fixture root.
        QObject *rootHeader = root->findChild<QObject *>(
            QStringLiteral("threadRootHeader"));
        QVERIFY(rootHeader != nullptr);
        const QVariantMap rootInfo = controller.thread()->rootInfo();
        QCOMPARE(rootInfo.value(QStringLiteral("loaded")).toBool(), true);
        QCOMPARE(rootInfo.value(QStringLiteral("eventId")).toString(), rootId);

        // The close button closes through the controller.
        QObject *closeButton = root->findChild<QObject *>(
            QStringLiteral("threadCloseButton"));
        QVERIFY(closeButton != nullptr);
        QVERIFY(QMetaObject::invokeMethod(closeButton, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomSwitchClosesThreadPanel()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panel != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QCOMPARE(controller.thread()->state(), ThreadController::Closed);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The panel composer sends through ThreadController.sendText — the
    // reply lands in the thread model with the correct root and exactly
    // once in the room timeline, never as an ordinary room message.
    void threadComposerSendsThreadReply()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        auto *threadModel = controller.thread()->model();
        const int rowsBefore = threadModel->rowCount();

        QObject *input = root->findChild<QObject *>(
            QStringLiteral("threadComposerInput"));
        QObject *send = root->findChild<QObject *>(
            QStringLiteral("threadSendButton"));
        QVERIFY(input != nullptr);
        QVERIFY(send != nullptr);
        QVERIFY(input->setProperty("text",
                                   QStringLiteral("panel thread reply")));
        QVERIFY(QMetaObject::invokeMethod(send, "click"));

        QTRY_COMPARE_WITH_TIMEOUT(threadModel->rowCount(), rowsBefore + 1,
                                  kSignalTimeoutMs);
        const QModelIndex last =
            threadModel->index(threadModel->rowCount() - 1, 0);
        QCOMPARE(threadModel->data(last, TimelineModel::BodyRole).toString(),
                 QStringLiteral("panel thread reply"));
        QCOMPARE(threadModel
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QCOMPARE(input->property("text").toString(), QString{});
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Deliberate narrow fallback (< 660 pane width): the open panel takes
    // the whole pane and the room column hides — and returns when the panel
    // closes or the pane widens. From 660 up the thread is ALWAYS a 340px
    // side panel next to the visible timeline.
    void narrowWindowThreadPanelReplacesRoomColumn()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(640);   // narrow
        root->setHeight(480);
        QObject *roomColumn = root->findChild<QObject *>(
            QStringLiteral("roomColumn"));
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(roomColumn != nullptr);
        QVERIFY(panel != nullptr);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  false, kSignalTimeoutMs);

        // Wide again: both are visible side by side, panel at exactly
        // 340px (correction spec §4). 800px sits inside the range the old
        // 900px breakpoint wrongly turned into a full-pane takeover.
        root->setWidth(800);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  true, kSignalTimeoutMs);
        QCOMPARE(panel->property("visible").toBool(), true);
        auto *panelItem = qobject_cast<QQuickItem *>(panel);
        QVERIFY(panelItem);
        // Offscreen root items get no layout polish, so read the attached
        // preferred width the RowLayout applies in a real window; the
        // windowed acceptance snapshot asserts the rendered 340px too.
        QQmlExpression widthAt800(qmlContext(panelItem), panelItem,
                                  QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt800.evaluate().toReal(), 340.0);
        root->setWidth(1200);
        QQmlExpression widthAt1200(qmlContext(panelItem), panelItem,
                                   QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt1200.evaluate().toReal(), 340.0);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        controller.thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // ── v0.6.0 checkpoint 6: isolated thread scroll motion ───────────────
    // The thread panel has its OWN wheel engine: separate instance, both
    // track the persisted speed, motion on one never engages the other,
    // and closing the thread cancels the panel's in-flight motion.
    // The right side is member panel XOR thread panel, owned by one derived
    // state (rightPanelState). Closing the thread with X collapses the right
    // side to "none" — it never restores Room Information or People.
    void threadPanelIsExclusiveWithMemberPanel()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(1200);
        root->setHeight(700);

        QObject *forumButton = root->findChild<QObject *>(
            QStringLiteral("threadsViewButton"));
        QObject *infoPanel = root->findChild<QObject *>(
            QStringLiteral("roomInfoPanel"));
        QVERIFY(forumButton && infoPanel);

        // Member panel showing (simulated at the state level).
        QVERIFY(infoPanel->setProperty("section", QStringLiteral("people")));
        QVERIFY(root->setProperty("infoOpen", true));
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QObject *groupButton = root->findChild<QObject *>(
            QStringLiteral("memberPanelButton"));
        QVERIFY(groupButton);
        QCOMPARE(groupButton->property("active").toBool(), true);
        QCOMPARE(forumButton->property("active").toBool(), false);

        // Opening a thread replaces it (never layers over it) and flips the
        // header chips.
        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("infoOpen").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(forumButton->property("active").toBool(), true);
        QCOMPARE(groupButton->property("active").toBool(), false);
        // The room timeline stays instantiated and visible next to it.
        QObject *roomColumn = root->findChild<QObject *>(
            QStringLiteral("roomColumn"));
        QVERIFY(roomColumn);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        // Thread and main drafts are isolated.
        QObject *threadInput = root->findChild<QObject *>(
            QStringLiteral("threadComposerInput"));
        QVERIFY(threadInput);
        controller.composer()->setText(QStringLiteral("main draft"));
        QVERIFY(threadInput->setProperty("text",
                                         QStringLiteral("thread draft")));
        QCOMPARE(controller.composer()->text(), QStringLiteral("main draft"));
        QCOMPARE(threadInput->property("text").toString(),
                 QStringLiteral("thread draft"));
        controller.composer()->setText(QString{});

        // Reopening the member panel while the thread shows switches
        // directly — the same property mechanism closes the thread.
        QVERIFY(root->setProperty("infoOpen", true));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QCOMPARE(groupButton->property("active").toBool(), true);
        QCOMPARE(forumButton->property("active").toBool(), false);

        // Reopen the thread and close with X: the right side collapses to
        // NONE — Room Information / People are never restored implicitly,
        // and both header chips go inactive.
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("infoOpen").toBool(), false,
                                  kSignalTimeoutMs);
        QObject *closeButton = root->findChild<QObject *>(
            QStringLiteral("threadCloseButton"));
        QVERIFY(closeButton);
        QVERIFY(QMetaObject::invokeMethod(closeButton, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QCOMPARE(root->property("rightPanelState").toString(),
                 QStringLiteral("none"));
        QCOMPARE(root->property("infoOpen").toBool(), false);
        QObject *panelObject = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panelObject);
        QCOMPARE(panelObject->property("visible").toBool(), false);
        QCOMPARE(forumButton->property("active").toBool(), false);
        QCOMPARE(groupButton->property("active").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Settings → Appearance → Message layout: Compact tightens the row and
    // drops the avatar gutter; Bubbles colors DM rows only (never ordinary
    // rooms) and right-aligns own messages; the text-size setting scales the
    // body font. All against the production delegate.
    void messageLayoutModesReshapeTheDelegate()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Layout fixture body"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(640);
        QCoreApplication::processEvents();

        auto *content = root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *avatar = root->findChild<QQuickItem *>(
            QStringLiteral("senderAvatar"));
        auto *body = root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(content && avatar && body);

        // Modern: 40px gutter, visible avatar, transparent row.
        QCOMPARE(controller.settings()->messageLayout(), 0);
        QCOMPARE(root->property("avatarGutterWidth").toReal(), 40.0);
        QVERIFY(avatar->isVisible());
        QCOMPARE(content->property("color").value<QColor>().alpha(), 0);

        // Compact: gutter collapses, avatar hides, body tightens.
        controller.settings()->setMessageLayout(2);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("avatarGutterWidth").toReal(), 8.0);
        QVERIFY(!avatar->isVisible());
        QCOMPARE(body->property("font").value<QFont>().pixelSize(), 13);

        // Bubbles in an ordinary room: NO bubble background.
        controller.settings()->setMessageLayout(1);
        QCoreApplication::processEvents();
        QVERIFY(!root->property("bubbleMode").toBool());
        QCOMPARE(content->property("color").value<QColor>().alpha(), 0);

        // Bubbles in a DM: incoming rows take the neutral bubble...
        root->setProperty("isDirectRoom", true);
        QCoreApplication::processEvents();
        QVERIFY(root->property("bubbleMode").toBool());
        QQmlExpression otherExpr(qmlContext(root), root,
                                 QStringLiteral("AppTheme.otherBubble"));
        QCOMPARE(content->property("color").value<QColor>(),
                 otherExpr.evaluate().value<QColor>());
        const qreal incomingX = content->x();

        // ...and own rows right-align in the accent-dark bubble.
        fixture.insert(QStringLiteral("isOwn"), true);
        engine.rootContext()->setContextProperty("model", fixture);
        QCoreApplication::processEvents();
        QQmlExpression ownExpr(qmlContext(root), root,
                               QStringLiteral("AppTheme.ownBubble"));
        QCOMPARE(content->property("color").value<QColor>(),
                 ownExpr.evaluate().value<QColor>());
        QVERIFY(content->x() > incomingX);
        QVERIFY(content->x() + content->width() <= 640.0 + 1.0);

        // Text scale reaches the body font (Modern, 140%). Main.qml binds
        // AppTheme.textScale to the setting in production; this scene drives
        // the singleton directly.
        controller.settings()->setMessageLayout(0);
        QQmlExpression setScale(qmlContext(root), root,
                                QStringLiteral("AppTheme.textScale = 1.4"));
        setScale.evaluate();
        QCoreApplication::processEvents();
        QCOMPARE(body->property("font").value<QFont>().pixelSize(),
                 qRound(14 * 1.4));
        QQmlExpression resetScale(qmlContext(root), root,
                                  QStringLiteral("AppTheme.textScale = 1.0"));
        resetScale.evaluate();

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Read-receipt chips: empty list = zero footprint (the strip stays
    // invisible and adds no height), a populated list renders a bounded
    // stack of 4 avatar chips + a "+N" overflow chip, and the strip carries
    // one accessible/tooltip summary line. Two independent engine loads —
    // swapping the "model" context property after load triggers Repeater
    // instantiation inside the context-replacement cascade, where document
    // ids do not resolve (a harness artifact; the live path delivers role
    // updates through dataChanged inside a real ListView, the same
    // mechanism reaction chips already use).
    void readReceiptChipsRenderBoundedAndCollapseWhenEmpty()
    {
        // Repeater-created delegates have a VISUAL parent but no QObject
        // parent in the item tree, so findChildren() cannot see them —
        // count them by walking childItems() instead.
        const auto visualChildrenByName =
            [](QQuickItem *root, const QString &name) {
                QList<QQuickItem *> out;
                QList<QQuickItem *> stack{root};
                while (!stack.isEmpty()) {
                    QQuickItem *item = stack.takeLast();
                    const auto kids = item->childItems();
                    for (QQuickItem *k : kids) {
                        if (k->objectName() == name)
                            out.append(k);
                        stack.append(k);
                    }
                }
                return out;
            };

        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Receipt fixture body"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QStringList warnings;
        const auto loadDelegate =
            [this, &controller, &warnings](
                QQmlApplicationEngine &engine,
                const QVariantMap &model, qreal width) -> QQuickItem * {
            connect(&engine, &QQmlEngine::warnings, this,
                    [&warnings](const QList<QQmlError> &errors) {
                        for (const auto &e : errors)
                            warnings << e.toString();
                    });
            engine.rootContext()->setContextProperty("app", &controller);
            engine.rootContext()->setContextProperty("model", model);
            QSignalSpy createdSpy(&engine,
                                  &QQmlApplicationEngine::objectCreated);
            engine.loadFromModule(QStringLiteral("MatrixClient"),
                                  QStringLiteral("MessageDelegate"));
            if (createdSpy.isEmpty()
                && !createdSpy.wait(kSignalTimeoutMs))
                return nullptr;
            auto *root = qobject_cast<QQuickItem *>(
                createdSpy.at(0).at(0).value<QObject *>());
            if (root == nullptr)
                return nullptr;
            root->setWidth(width);
            QCoreApplication::processEvents();
            return root;
        };

        // Load 1 — no receipts: invisible strip, no chips, empty summary.
        // An invisible child adds no ColumnLayout height, so an unread
        // message keeps exactly its previous geometry.
        {
            QQmlApplicationEngine engine;
            QVariantMap empty = fixture;
            empty.insert(QStringLiteral("readReceipts"), QVariantList{});
            QQuickItem *root = loadDelegate(engine, empty, 1400);
            QVERIFY(root != nullptr);
            auto *strip = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
            QVERIFY(strip != nullptr);
            QVERIFY(!strip->isVisible());
            QVERIFY(visualChildrenByName(root,
                                         QStringLiteral("readReceiptChip"))
                        .isEmpty());
            QCOMPARE(strip->property("summary").toString(), QString{});
            QVERIFY(root->implicitHeight() > 0.0);
        }

        // Load 2 — six readers (uncapped total 6): 4 avatar chips + "+2"
        // overflow, newest-first list as the model delivers it, one
        // summary line for the tooltip and the accessible name. Loaded
        // WIDE (1400px) so the strip is much wider than the chip stack
        // and the right-edge rail placement is a real assertion (see
        // readReceiptChipsRideTheRightEdgeRail for the full multi-row
        // contract).
        QVariantList receipts;
        const QStringList names = {
            QStringLiteral("Alice"), QStringLiteral("Bob"),
            QStringLiteral("Carol"), QStringLiteral("Dave"),
            QStringLiteral("Erin"), QStringLiteral("Frank"),
        };
        for (int i = 0; i < names.size(); ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"),
                     QStringLiteral("@u%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"), names.at(i));
            r.insert(QStringLiteral("avatarMxc"), QString{});
            r.insert(QStringLiteral("tsMs"),
                     Q_INT64_C(1700000000000) - i * 1000);
            receipts.append(r);
        }
        {
            QQmlApplicationEngine engine;
            QVariantMap populated = fixture;
            populated.insert(QStringLiteral("readReceipts"), receipts);
            populated.insert(QStringLiteral("readReceiptsTotal"), 6);
            QQuickItem *root = loadDelegate(engine, populated, 1400);
            QVERIFY(root != nullptr);
            auto *strip = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
            QVERIFY(strip != nullptr);
            QVERIFY(strip->isVisible());
            QCOMPARE(visualChildrenByName(root,
                                          QStringLiteral("readReceiptChip"))
                         .size(),
                     4);
            auto *overflow = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptOverflow"));
            QVERIFY(overflow != nullptr);
            QVERIFY(overflow->isVisible());
            QCOMPARE(overflow->childItems().first()
                         ->property("text").toString(),
                     QStringLiteral("+2"));
            QCOMPARE(strip->property("summary").toString(),
                     QStringLiteral("Read by Alice, Bob and 4 others"));
            auto *presentation = root->findChild<QQuickItem *>(
                QStringLiteral("messagePresentationRow"));
            QVERIFY(presentation != nullptr);
            const qreal messageBottom = presentation->mapToScene(
                QPointF(0, presentation->height())).y();
            auto *chipRow = strip->findChild<QQuickItem *>(
                QStringLiteral("readReceiptRow"));
            QVERIFY(chipRow != nullptr);
            const qreal receiptBottom = chipRow->mapToScene(
                QPointF(0, chipRow->height())).y();
            QVERIFY2(qAbs(receiptBottom - messageBottom) < 1.5,
                     qPrintable(QStringLiteral(
                         "receiptBottom=%1 messageBottom=%2")
                         .arg(receiptBottom).arg(messageBottom)));
            // Geometry (2026-08-14, Element parity): the chip stack rides
            // the strip's own RIGHT EDGE — one fixed receipt rail per
            // pane — see readReceiptChipsRideTheRightEdgeRail for the
            // full multi-row contract.
            const qreal stripRight =
                strip->mapToScene(QPointF(strip->width(), 0)).x();
            const qreal chipsRight =
                chipRow->mapToScene(QPointF(chipRow->width(), 0)).x();
            QVERIFY2(qAbs(chipsRight - stripRight) < 1.5,
                     qPrintable(QStringLiteral(
                         "chipsRight=%1 stripRight=%2")
                         .arg(chipsRight).arg(stripRight)));
        }

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // 2026-08-14 (maintainer request, Element parity): read-receipt chips
    // ride ONE fixed right-edge rail — the strip's own right edge, i.e.
    // the far right of the timeline row — for every row, regardless of
    // how wide the message body or the sender identity header renders.
    // This deliberately REPLACES the 2026-08-06 "trail the rendered
    // content" contract (whose per-row anchor walked the widest visible
    // bubbleContent child); the three fixture rows below are kept from
    // that era precisely because they exercise the row shapes that used
    // to produce different chip positions — under the rail contract all
    // three must agree on a single right edge.
    void readReceiptChipsRideTheRightEdgeRail()
    {
        // Delegate-level fixture (the same harness the bounded-chips test
        // above uses): the full-pane fixture cannot exercise this — its
        // rows never hydrate into app.timelineView offscreen, the same
        // stale-fixture failure the rest of this suite carries. Three row
        // shapes that produced three DIFFERENT chip positions under the
        // retired trail-the-content contract must all place their chips on
        // the strip's own right edge.
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        QVariantList receipts;
        for (const auto &reader :
             { QStringLiteral("@carol:mock.local"),
               QStringLiteral("@dave:mock.local") }) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"), reader);
            r.insert(QStringLiteral("displayName"),
                     reader.mid(1, reader.indexOf(QLatin1Char(':')) - 1));
            r.insert(QStringLiteral("avatarMxc"), QString{});
            receipts.append(r);
        }
        fixture.insert(QStringLiteral("readReceipts"), receipts);
        fixture.insert(QStringLiteral("readReceiptsTotal"), receipts.size());

        QStringList warnings;
        const auto loadDelegate =
            [this, &controller, &warnings](
                QQmlApplicationEngine &engine,
                const QVariantMap &model, qreal width) -> QQuickItem * {
            connect(&engine, &QQmlEngine::warnings, this,
                    [&warnings](const QList<QQmlError> &errors) {
                        for (const auto &e : errors)
                            warnings << e.toString();
                    });
            engine.rootContext()->setContextProperty("app", &controller);
            engine.rootContext()->setContextProperty("model", model);
            QSignalSpy createdSpy(&engine,
                                  &QQmlApplicationEngine::objectCreated);
            engine.loadFromModule(QStringLiteral("MatrixClient"),
                                  QStringLiteral("MessageDelegate"));
            if (createdSpy.isEmpty()
                && !createdSpy.wait(kSignalTimeoutMs))
                return nullptr;
            auto *root = qobject_cast<QQuickItem *>(
                createdSpy.at(0).at(0).value<QObject *>());
            if (root == nullptr)
                return nullptr;
            root->setWidth(width);
            QCoreApplication::processEvents();
            return root;
        };

        // The three shapes: short body + short name (the 2026-08-06
        // screenshot row), a wide unbroken body near the column cap, and a
        // header rendering wider than a two-word body ("SpongeMan"/"Fr
        // fr", the reviewer's counterexample row).
        struct Shape {
            const char *sender;
            const char *name;
            const char *body;
        };
        const Shape shapes[] = {
            { "@bob:mock.local", "Bob", "Fr fr" },
            { "@bob:mock.local", "Bob",
              "https://www.example.com/a/very/long/unbroken/path/segment/"
              "that/cannot/wrap/anywhere/because/it/has/no/spaces/at/all" },
            { "@sponge:mock.local", "SpongeMan", "Fr fr" },
        };

        QVector<qreal> railOffsets;
        for (const Shape &shape : shapes) {
            QQmlApplicationEngine engine;
            QVariantMap model = fixture;
            model.insert(QStringLiteral("sender"),
                         QString::fromLatin1(shape.sender));
            model.insert(QStringLiteral("senderDisplayName"),
                         QString::fromLatin1(shape.name));
            model.insert(QStringLiteral("senderInitials"),
                         QString::fromLatin1(shape.name).left(1));
            model.insert(QStringLiteral("body"),
                         QString::fromLatin1(shape.body));
            QQuickItem *root = loadDelegate(engine, model, 1400);
            QVERIFY(root != nullptr);
            auto *strip = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
            QVERIFY(strip != nullptr);
            QVERIFY(strip->isVisible());
            auto *chipRow = strip->findChild<QQuickItem *>(
                QStringLiteral("readReceiptRow"));
            QVERIFY(chipRow != nullptr);

            const qreal stripRight =
                strip->mapToScene(QPointF(strip->width(), 0)).x();
            const qreal chipsRight =
                chipRow->mapToScene(QPointF(chipRow->width(), 0)).x();
            // The rail: chips ride the strip's own right edge no matter
            // how wide the body or the identity header rendered. The
            // retired contract fails this on the short-body rows by
            // hundreds of px (chips near the body, strip edge far right).
            QVERIFY2(qAbs(chipsRight - stripRight) < 1.5,
                     qPrintable(QStringLiteral(
                         "chips must ride the right-edge rail: sender=%1 "
                         "chipsRight=%2 stripRight=%3")
                         .arg(QString::fromLatin1(shape.sender))
                         .arg(chipsRight).arg(stripRight)));
            // The avatar-gutter floor survives the rail contract.
            QVERIFY(chipRow->x() >= root->property("avatarGutterWidth")
                                        .toReal() - 0.5);
            railOffsets.append(stripRight - chipsRight);
        }
        // One rail, not three: every shape agrees on the same offset from
        // the strip edge.
        QCOMPARE(railOffsets.size(), 3);
        QVERIFY2(qAbs(railOffsets[0] - railOffsets[1]) < 1.0
                     && qAbs(railOffsets[1] - railOffsets[2]) < 1.0,
                 qPrintable(QStringLiteral(
                     "shapes disagree on the receipt rail: %1 / %2 / %3")
                     .arg(railOffsets[0]).arg(railOffsets[1])
                     .arg(railOffsets[2])));

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Live-bug reproduction (2026-08 report: "receipts dont show avatars" —
    // the chips render, but as initials/coloured circles only; sender
    // avatars on the same account load fine). Drives the REAL chain the
    // live app uses: TimelineModel::readReceiptsVariant() resolving
    // avatarMxc through the member cache → the delegate's content-guarded
    // `shown` projection → the chip Avatar → MediaBridge →
    // MediaImageProvider (real provider, fake bytes). Two shapes:
    //   * Carol's avatar is in the member cache BEFORE the room opens —
    //     the chip must reach presentationState "ready" with a non-empty
    //     provider-backed image source;
    //   * Dave's receipt renders BEFORE his member entry carries an avatar
    //     (the live Rust-backend timing: room_members hydration resolves
    //     after the timeline reset) — the chip must show initials first,
    //     then promote to "ready" when the member cache hydrates and
    //     membersChanged re-announces ReadReceiptsRole. No manual poke, no
    //     room switch.
    void readReceiptChipAvatarsLoadThroughRealProviderPath()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        // Real provider path: enable the mock media bridge and register the
        // readers' avatar bytes (64px solid PNGs, distinct colours).
        const auto pngBytes = [](const QColor &color) {
            QImage image(64, 64, QImage::Format_ARGB32);
            image.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const QString carolMxc = QStringLiteral("mxc://mock.local/carol-av");
        const QString daveMxc = QStringLiteral("mxc://mock.local/dave-av");
        mock->setSupportsMediaBridgeForTest(true);
        mock->setAvatarBytesForTest(carolMxc, pngBytes(QColor(200, 40, 40)),
                                    QStringLiteral("image/png"));
        mock->setAvatarBytesForTest(daveMxc, pngBytes(QColor(40, 40, 200)),
                                    QStringLiteral("image/png"));

        // Carol: avatar known BEFORE the room opens (hydrated cache).
        mock->setRoomMemberForTest(
            roomId, { QStringLiteral("@carol:mock.local"),
                      QStringLiteral("Carol"), carolMxc });

        controller.setCurrentRoomId(roomId);

        TimelineEvent readByCarol;
        readByCarol.eventId = QStringLiteral("$receipt-av-carol");
        readByCarol.itemId = QStringLiteral("uid-receipt-av-carol");
        readByCarol.roomId = roomId;
        readByCarol.sender = QStringLiteral("@bob:mock.local");
        readByCarol.senderDisplayName = QStringLiteral("Bob");
        readByCarol.body = QStringLiteral("read by carol");
        readByCarol.timestamp = QDateTime::currentDateTimeUtc();
        readByCarol.type = TimelineEvent::TextMessage;
        readByCarol.status = TimelineEvent::Sent;
        readByCarol.readBy = { { QStringLiteral("@carol:mock.local"),
                                 Q_INT64_C(1700000002000) } };
        readByCarol.readByTotal = 1;

        TimelineEvent readByDave = readByCarol;
        readByDave.eventId = QStringLiteral("$receipt-av-dave");
        readByDave.itemId = QStringLiteral("uid-receipt-av-dave");
        readByDave.body = QStringLiteral("read by dave");
        readByDave.timestamp = QDateTime::currentDateTimeUtc().addSecs(1);
        readByDave.readBy = { { QStringLiteral("@dave:mock.local"),
                                Q_INT64_C(1700000003000) } };
        readByDave.readByTotal = 1;

        mock->resetTimelineForTest(roomId, { readByCarol, readByDave },
                                   /*paginationPages=*/0);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        // The REAL image provider, exactly as main.cpp registers it — the
        // chip's Image resolves "image://lightning-media/..." through it.
        engine.addImageProvider(
            QStringLiteral("lightning-media"),
            new MediaImageProvider(controller.mediaBridge()));
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(1200, 900);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(),
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 2,
                                 kSignalTimeoutMs);
        QMetaObject::invokeMethod(timeline, "forceLayout");
        QCoreApplication::processEvents();

        // The chip's Avatar root: the item inside readReceiptChip that
        // carries the avatarImage child (Avatar.qml has no objectName of
        // its own in the chip).
        const auto chipAvatar = [](QQuickItem *rowItem) -> QQuickItem * {
            const auto chips = findVisualChildren(
                rowItem, QStringLiteral("readReceiptChip"));
            if (chips.size() != 1)
                return nullptr;
            const auto images = findVisualChildren(
                chips.first(), QStringLiteral("avatarImage"));
            if (images.size() != 1)
                return nullptr;
            return images.first()->parentItem();
        };

        // --- Shape 1 (Carol, cache hydrated before open): the chip must
        // reach the decoded bitmap through the real provider path.
        QQuickItem *carolRow = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((carolRow = itemForSourceRow(timeline, 0)) != nullptr),
             carolRow != nullptr),
            kSignalTimeoutMs);
        QQuickItem *carolAvatar = chipAvatar(carolRow);
        QVERIFY2(carolAvatar != nullptr,
                 "carol's receipt chip has no Avatar with an avatarImage");
        QCOMPARE(carolAvatar->property("mxc").toString(), carolMxc);
        QTRY_COMPARE_WITH_TIMEOUT(
            carolAvatar->property("presentationState").toString(),
            QStringLiteral("ready"), kSignalTimeoutMs);
        {
            const auto images = findVisualChildren(
                carolAvatar, QStringLiteral("avatarImage"));
            QCOMPARE(images.size(), 1);
            const QString source =
                images.first()->property("source").toUrl().toString();
            QVERIFY2(source.startsWith(
                         QStringLiteral("image://lightning-media/")),
                     qPrintable(QStringLiteral(
                         "chip avatar source is not provider-backed: '%1'")
                         .arg(source)));
            QVERIFY(images.first()->isVisible());
        }

        // --- Shape 2 (Dave, live hydration timing): receipts rendered
        // BEFORE his member entry knows an avatar → honest initials; the
        // member-cache hydration alone must then promote the chip.
        QQuickItem *daveRow = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((daveRow = itemForSourceRow(timeline, 1)) != nullptr),
             daveRow != nullptr),
            kSignalTimeoutMs);
        QQuickItem *daveAvatar = chipAvatar(daveRow);
        QVERIFY2(daveAvatar != nullptr,
                 "dave's receipt chip has no Avatar with an avatarImage");
        QCOMPARE(daveAvatar->property("mxc").toString(), QString{});
        QCOMPARE(daveAvatar->property("presentationState").toString(),
                 QStringLiteral("missing"));

        mock->setRoomMemberForTest(
            roomId, { QStringLiteral("@dave:mock.local"),
                      QStringLiteral("Dave"), daveMxc });

        // The hydration must rebuild the chip (the projection guard sees
        // the avatarMxc change) — re-resolve the Avatar under the SAME row.
        QTRY_VERIFY_WITH_TIMEOUT(
            (daveAvatar = chipAvatar(daveRow)) != nullptr
                && daveAvatar->property("mxc").toString() == daveMxc,
            kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            daveAvatar->property("presentationState").toString(),
            QStringLiteral("ready"), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // SDK receipt tracking (required for the receipt chips) also revives
    // the SDK's ReadMarker virtual row. While the reader is pinned to the
    // bottom, the own-receipt ack cycle would bounce the 28px "New
    // messages" divider in and out under every incoming message — so the
    // divider must render COLLAPSED while stickToBottom holds, and appear
    // only for a reader who scrolled up.
    void newMessagesDividerCollapsesWhilePinnedToBottom()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

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
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        // Stage a short timeline with the SDK-style read marker between
        // the read part and one unread message (all rows fit on screen).
        const QDateTime base =
            QDateTime::currentDateTimeUtc().addSecs(-600);
        QList<TimelineEvent> events;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$m%1").arg(i);
            e.itemId = QStringLiteral("uid-m%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.body = QStringLiteral("message %1").arg(i);
            e.timestamp = base.addSecs(i * 60);
            events.append(e);
        }
        TimelineEvent marker;
        marker.itemId = QStringLiteral("uid-marker");
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        TimelineEvent unread;
        unread.eventId = QStringLiteral("$unread");
        unread.itemId = QStringLiteral("uid-unread");
        unread.roomId = roomId;
        unread.sender = QStringLiteral("@bob:mock.local");
        unread.body = QStringLiteral("the unread one");
        unread.timestamp = base.addSecs(300);
        events.append(unread);
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QVERIFY(timeline->property("stickToBottom").toBool());

        QQuickItem *markerItem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((markerItem = itemForSourceRow(timeline, 3)) != nullptr),
             markerItem != nullptr),
            5000);
        auto *divider = markerItem->findChild<QQuickItem *>(
            QStringLiteral("unreadDivider"));
        QVERIFY(divider != nullptr);

        // Pinned to the bottom: the marker row exists but renders as
        // nothing — no divider, zero height, no layout bounce when the
        // ack cycle inserts/removes it.
        QVERIFY(!divider->isVisible());
        QCOMPARE(markerItem->implicitHeight(), 0.0);

        // A reader who scrolled up (bottom-follow disengaged) gets the
        // divider back.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_VERIFY_WITH_TIMEOUT(divider->isVisible(), 2000);
        QVERIFY(markerItem->implicitHeight() >= 28.0);

        // Returning to the bottom collapses it again.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QTRY_VERIFY_WITH_TIMEOUT(!divider->isVisible(), 2000);
        QCOMPARE(markerItem->implicitHeight(), 0.0);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void threadScrollMotionIsIsolatedFromRoomTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *roomScroll = controller.timelineScroll();
        auto *threadScroll = controller.threadScroll();
        QVERIFY(roomScroll != nullptr);
        QVERIFY(threadScroll != nullptr);
        QVERIFY(roomScroll != threadScroll);

        // Both follow the persisted wheel-speed setting.
        controller.settings()->setTimelineWheelSpeed(2);   // Very fast
        QCOMPARE(roomScroll->wheelSpeed(), TimelineScrollController::VeryFast);
        QCOMPARE(threadScroll->wheelSpeed(),
                 TimelineScrollController::VeryFast);
        controller.settings()->setTimelineWheelSpeed(1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);

        // Thread wheel motion engages ONLY the thread engine.
        threadScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(threadScroll->motionActive());
        QVERIFY(!roomScroll->motionActive());

        // Room wheel motion engages ONLY the room engine.
        roomScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(roomScroll->motionActive());
        threadScroll->cancel();
        QVERIFY(roomScroll->motionActive());   // untouched by the other panel
        roomScroll->cancel();

        // Closing the thread cancels the panel's in-flight wheel motion
        // (the panel's state-change handler owns this).
        threadScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(threadScroll->motionActive());
        controller.thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(threadScroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.7.2: the pagination-specific anchor (anchorStableId, captureAnchor/
    // restoreCapturedAnchor/restoreAnchor, anchorCaptureToken) is gone — a
    // backward-pagination prepend is now just another cause of the persistent
    // view anchor's onContentHeightChanged reaction. These three tests keep
    // the guarantees the deleted mechanism's tests protected, driven through
    // REAL near-top round trips rather than the old per-batch bookkeeping.
    //
    // Concurrent scroll: a prepend landing while the reader keeps scrolling
    // must NOT recompute an absolute position from the last settle (which
    // would silently discard the in-flight scroll — the "jump / reverse
    // while history is loading" defect). The mechanism that satisfies this
    // changed since this test was written: maintainViewAnchor()'s
    // "materialized" branch (qml/TimelinePane.qml, userScrollActive path)
    // used to apply the anchor's measured y-delta to contentY; that was
    // tried twice, reviewed, and rejected by physical testing both times —
    // see the comment on that branch — because it pulled the reader in both
    // directions during loading and during ordinary scrolling with nothing
    // loading at all. Production now performs NO WRITE while a gesture is
    // active; it only re-bases its measurement for the next call. So the
    // invariant this proves is now the plain, stronger form: an in-flight
    // prepend must not touch contentY at all while the gesture is live —
    // the exact position the user's own motion produced is preserved
    // untouched, not recomputed from a captured delta.
    void paginationPrependPreservesConcurrentScroll()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        // Two pages: the unavoidable startup viewport-fill consumes one
        // before this test's own request ever runs.
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/2);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // Deeper into history than the old virtualized fixture used: the
        // un-virtualized timeline renders every row's real height, so row 15
        // of ~30 was landing the reader at (or past) wheelMinY() with no
        // headroom left to simulate an additional upward scroll — the
        // production clamp then legitimately capped the write, which is not
        // what this case means to exercise. Row 5 leaves real headroom.
        const int anchorRow = 5;
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, anchorRow));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QCoreApplication::processEvents();

        // The reader keeps scrolling while the request is in flight: a direct
        // contentY write PLUS restarting the settle timer — the two things
        // the WheelHandler's pixelDelta branch does. The timer restart is
        // what makes userScrollActive true, routing the correction to the
        // RELATIVE branch instead of the absolute idle restore.
        const double capturedContentY = timeline->property("contentY").toDouble();
        const double anchorLastY =
            timeline->property("viewAnchorLastY").toDouble();
        constexpr double simulatedScrollDelta = 40.0;
        const double scrolledContentY = capturedContentY - simulatedScrollDelta;
        QVERIFY(timeline->setProperty("contentY", scrolledContentY));
        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "the simulated gesture must read as an active scroll "
                 "session, or the correction takes the wrong branch");

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        const int newRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(newRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((anchorItem = itemForSourceRow(timeline, newRow)) != nullptr),
             anchorItem != nullptr),
            kSignalTimeoutMs);

        // anchorItem resolving proves the anchor row's delegate survived the
        // prepend (still instantiated, not evicted) — a real precondition
        // for the "materialized" no-write branch to have been the one that
        // ran at all, since an unresolved id takes a different branch
        // entirely (diagUnresolvedIdFallbackCountsGenuinelyUnresolvableAnchor
        // covers that one).
        Q_UNUSED(anchorItem);
        Q_UNUSED(anchorLastY);
        double actual = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (actual = timeline->property("contentY").toDouble(),
             qAbs(actual - scrolledContentY) < 0.5),
            kSignalTimeoutMs);
        QVERIFY2(qAbs(actual - scrolledContentY) < 0.5,
                 qPrintable(QStringLiteral(
                     "pagination prepend wrote over an active gesture's "
                     "position: actual=%1 scrolledContentY=%2 (a stale "
                     "absolute restore would move it toward %3)")
                     .arg(actual).arg(scrolledContentY)
                     .arg(capturedContentY)));
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "the gesture must still read active at the assertion "
                 "point, or an idle restore — not the mechanism under "
                 "test — could be the reason nothing moved");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Discrete-wheel path end to end: the old restore cancelled an in-flight
    // glide unconditionally, freezing it partway and discarding the reader's
    // remaining distance ("it snaps me half the distance back"). The unified
    // path translates the glide instead — proven here through a REAL
    // pagination completion, not a direct function call.
    void paginationPrependPreservesWheelGlide()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(80);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/2);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // Row 5, not 15: on the un-virtualized layout row 15 of ~30 lands
        // the reader AT wheelMinY() already (no headroom above), which
        // would clamp this glide before it ever moves regardless of the
        // mechanism under test. See wheelBounds() and the sibling comment
        // on paginationPrependPreservesConcurrentScroll.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 5));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        QVERIFY(!timeline->property("viewAnchorId").toString().isEmpty());

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QCoreApplication::processEvents();

        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        const double glideStartY = timeline->property("contentY").toDouble();
        // The real bounds, not a hand-rolled originY/contentHeight-height
        // guess (see wheelBounds()): row 15 of ~30 lands the reader AT
        // wheelMinY() already in the un-virtualized layout, so a floor
        // computed without topMargin would clamp the simulated glide before
        // it ever moves, which is exactly what silently made this case
        // vacuous until the bound helper matched production's.
        double minY = 0, maxY = 0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        for (int notch = 0; notch < 6; ++notch)
            scroll->wheelNotch(120.0,
                               timeline->property("contentY").toDouble(),
                               minY, maxY, timeline->height());
        QTRY_VERIFY_WITH_TIMEOUT(scroll->motionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble() < glideStartY - 10.0,
            2000);
        QVERIFY(scroll->motionActive());
        const double beforeCompletionY =
            timeline->property("contentY").toDouble();

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        QVERIFY2(scroll->motionActive(),
                 "a real pagination completion killed the in-flight wheel "
                 "glide (the 'snaps me back partway' defect)");
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble()
                < beforeCompletionY - 20.0,
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!scroll->motionActive(), kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // THE load-bearing test for the unification, driven from the TOP EDGE
    // where the reader actually is when backfill fires. Review probes showed
    // Qt behaves differently there than mid-list: a prepend gives the new
    // rows the low positions and pushes the reader's row down by the whole
    // batch — far outside cacheBuffer, destroying its delegate — whereas
    // deeper in history row positions do not move at all. A mid-list test
    // therefore proves nothing: its expected delta is zero and it passes
    // whether or not compensation happens. This one forces the real case,
    // with a gesture in flight, and asserts the reader's tracked row keeps
    // the SAME viewport offset across the batch. It fails if the displaced
    // anchor is not resolved (the view re-anchors onto the newly loaded
    // content and ratifies the jump — the teleport cascade).
    void topEdgePrependKeepsReaderOnTheSameRowMidGesture()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/2);
        // A REAL-SIZED batch. The default mock page is 3 short rows, whose
        // displacement stays inside cacheBuffer (800) — the anchor delegate
        // survives, the resolve branch never runs, and the test would pass
        // whether or not the fix is present (verified: it did). Production
        // pages are PAGINATION_BATCH = 20, which pushes the reader's row far
        // outside the buffer and destroys its delegate — the only geometry
        // in which this defect exists. Reproduce that here.
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            // Deliberately long enough to WRAP to several lines: 20 short
            // rows land right at the cacheBuffer boundary (verified — the
            // delegate survives and the test stops discriminating). Real
            // messages are taller than one line; this reproduces the
            // production geometry where the reader's row is pushed clear of
            // the buffer and its delegate is destroyed.
            e.body = QStringLiteral(
                "older backfilled message %1 — this body is deliberately "
                "long so the row wraps to several lines and the prepended "
                "batch displaces the reader's anchor well beyond the "
                "ListView cache buffer, reproducing the real top-edge "
                "geometry rather than a compact synthetic one").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // Put the reader AT THE TOP EDGE, which is where near-top backfill
        // fires and where an upward glide parks against StopAtBounds.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");

        // The reader's row and where it sits in the viewport right now.
        const int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        const double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        // A gesture is in flight (the case the resolve branch used to skip).
        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");

        const double heightBefore =
            timeline->property("contentHeight").toDouble();
        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        // ENFORCE the premise that makes this test discriminating: the batch
        // must displace the anchor beyond the cache buffer, or the delegate
        // survives, the resolve branch never runs, and this test would pass
        // on broken code (which an earlier version of it did). Fail loudly
        // here rather than silently going vacuous if fonts or delegate
        // metrics change.
        const double cacheBufferPx = 800.0;
        double heightGrowth = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (heightGrowth = timeline->property("contentHeight").toDouble()
                            - heightBefore,
             heightGrowth > cacheBufferPx + timeline->height()),
            kSignalTimeoutMs);
        QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                 qPrintable(QStringLiteral(
                     "fixture no longer displaces the anchor past the cache "
                     "buffer (growth %1 <= %2) — this test would pass on "
                     "broken code")
                     .arg(heightGrowth).arg(cacheBufferPx + timeline->height())));

        // The reader's row must still sit at the same place in the viewport.
        const int rowAfter = controller.timeline()->rowForStableId(anchorId);
        QVERIFY2(rowAfter > rowBefore,
                 "fixture assumption: the prepend must shift the row index");
        double offsetAfter = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&] {
                QQuickItem *itemAfter = nullptr;
                ((itemAfter = itemForSourceRow(timeline, rowAfter)) != nullptr);
                if (!itemAfter)
                    return false;
                offsetAfter = itemAfter->y()
                    - timeline->property("contentY").toDouble();
                return qAbs(offsetAfter - offsetBefore) < 2.0;
            }()),
            kSignalTimeoutMs);
        QVERIFY2(qAbs(offsetAfter - offsetBefore) < 2.0,
                 qPrintable(QStringLiteral(
                     "a top-edge prepend moved the reader off their row "
                     "mid-gesture: viewport offset %1 -> %2 (the teleport "
                     "cascade)").arg(offsetBefore).arg(offsetAfter)));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // v0.6.6 regression fix: reverted to its pre-M5 shape (formerly
    // nearTopHeldRunCompensatesEveryBatchNotJustTheFinalOne). M5 made a
    // genuinely held run auto-chain regardless of growth while a since-
    // removed TimelineModel staging window held (see
    // nearTopKeepsChainingBatchesWhileStagingHoldsTheGesture, removed from
    // PaginationControllerTest.cpp), and this test's assertions were
    // rewritten to match that chained/staged behavior. M5 is now withdrawn,
    // and the staging window it interacted with is gone entirely (see
    // TimelinePane.qml's near-top backfill comment) — a live
    // LIGHTNING_SCROLL_TRACE capture showed the auto-chain turn into an
    // unbounded prefetch of the room (~30 near_top requests in one session,
    // signalled=0 on every completion, multi-thousand-pixel
    // displacedApplied corrections on release), and staging itself then
    // turned out to cost a hard wall for the reader once the chain was
    // bounded — see finishBatch(). This test is restored to proving what it
    // originally proved: driving requestNearTop() explicitly for EACH of
    // kBatches — bypassing the QML edge latch entirely, exactly like
    // scheduleNearTopContinuation() or any future controller-internal
    // caller — compensates each batch IMMEDIATELY once it completes, never
    // deferred. FAIL-ON-OLD: the nearTopRunActive() assertion right after
    // each completion is false on the withdrawn M5 mechanism, which would
    // have left a continuation scheduled (nearTopRunActive() still true)
    // instead of ending the run.
    void nearTopControllerDrivenBatchesCompensateImmediatelyNotChained()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        // One page for the initial viewport fill plus one spare (so a
        // startup fill that ever needs a second page cannot starve the
        // run), then three near-top batches this test drives explicitly
        // while a gesture is held. The spare also keeps the last sampled
        // batch away from the reached-start transition.
        constexpr int kBatches = 3;
        mock->resetTimelineForTest(roomId, events,
                                   /*paginationPages=*/2 + kBatches);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long so the row "
                "wraps to several lines and each prepended batch displaces "
                "the reader's anchor well beyond the ListView cache buffer, "
                "reproducing the real top-edge geometry of a sustained "
                "near-top loading run rather than a compact synthetic one")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");

        int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "this test exercises the nearTopRunActive branch alone — "
                 "nearTopArmed must stay un-consumed throughout");

        const double cacheBufferPx = 800.0;
        for (int batch = 0; batch < kBatches; ++batch) {
            const double heightBeforeBatch =
                timeline->property("contentHeight").toDouble();
            QSignalSpy completedSpy(controller.pagination(),
                                   &PaginationController::paginationCompleted);
            controller.pagination()->requestNearTop();
            QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
            QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                     "fixture assumption: each near-top page must insert rows");
            // FAIL-ON-OLD: the withdrawn M5 mechanism left a continuation
            // scheduled here whenever staging was still held (regardless of
            // this batch's own growth), so nearTopRunActive() would still
            // read true immediately after the completion above. It must
            // read false — this ONE productive batch ended the run.
            QVERIFY2(!controller.pagination()->nearTopRunActive(),
                     "a productive batch must end the run immediately, not "
                     "leave a continuation scheduled");

            double heightGrowth = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                (heightGrowth = timeline->property("contentHeight").toDouble()
                                - heightBeforeBatch,
                 heightGrowth > cacheBufferPx + timeline->height()),
                kSignalTimeoutMs);
            QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                     qPrintable(QStringLiteral(
                         "batch %1: fixture no longer displaces the anchor "
                         "past the cache buffer (growth %2 <= %3) — this "
                         "test would pass on broken code")
                         .arg(batch).arg(heightGrowth)
                         .arg(cacheBufferPx + timeline->height())));

            const int rowAfter = controller.timeline()->rowForStableId(anchorId);
            QVERIFY2(rowAfter > rowBefore,
                     "fixture assumption: the prepend must shift the row index");

            // Sample the offset RIGHT AFTER this single batch settles its own
            // onContentHeightChanged reaction — before the next batch is even
            // requested. Compensation must be IMMEDIATE, never deferred.
            double offsetAfter = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                ([&] {
                    QQuickItem *itemAfter = nullptr;
                    ((itemAfter = itemForSourceRow(timeline, rowAfter)) != nullptr);
                    if (!itemAfter)
                        return false;
                    offsetAfter = itemAfter->y()
                        - timeline->property("contentY").toDouble();
                    return qAbs(offsetAfter - offsetBefore) < 2.0;
                }()),
                kSignalTimeoutMs);
            QVERIFY2(qAbs(offsetAfter - offsetBefore) < 2.0,
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a near-top prepend during a held "
                         "gesture moved the reader off their row: viewport "
                         "offset %3 -> %4")
                         .arg(batch + 1).arg(kBatches)
                         .arg(offsetBefore).arg(offsetAfter)));

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The IMMEDIATE per-batch correction path — maintainViewAnchor()'s
    // mid-gesture relative-delta branch, applied the moment each batch's
    // onContentHeightChanged fires — is now the ONLY path (v0.6.6: the
    // near-top backfill staging window this test used to distinguish itself
    // from is gone outright; see TimelinePane.qml's near-top backfill
    // comment). Driven through ViewportFill rather than NearTop, so it
    // stays a distinct regression guard from
    // nearTopControllerDrivenBatchesCompensateImmediatelyNotChained (which
    // drives the same invariant through requestNearTop()): this test never
    // calls requestNearTop() or otherwise touches nearTopArmed. Same shape
    // topEdgePrependKeepsReaderOnTheSameRowMidGesture already proves for a
    // SINGLE batch, repeated here for several CONSECUTIVE batches while a
    // gesture stays held, sampling the offset after each.
    void viewportFillRunCompensatesEveryBatchImmediately()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        constexpr int kBatches = 3;
        // One page for the automatic initial fill, then exactly kBatches
        // MORE pages this test drives explicitly via requestViewportFill()
        // — never requestNearTop(), so nearTopRunActive can never become
        // true, and nearTopArmed is never touched either.
        mock->resetTimelineForTest(roomId, events,
                                   /*paginationPages=*/1 + kBatches);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long so the row "
                "wraps to several lines and each prepended batch displaces "
                "the reader's anchor well beyond the ListView cache buffer, "
                "reproducing the real top-edge geometry of a sustained "
                "near-top loading run rather than a compact synthetic one")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        // The reverse-list proxy paces newly loaded rows out over a few
        // frames (ReverseListProxyModel), so `count` can already report the
        // full model total while a row's delegate has not been released
        // yet. Every real navigation path calls releasePendingRows() first
        // for exactly this reason (see qml/TimelinePane.qml); this fixture
        // must too, or positionAtSourceRow() addresses a row the proxy has
        // not exposed yet and fails. releaseAll() inserts the model rows
        // synchronously, but the Column's delegates for them are created on
        // the next event-loop turn, so a processEvents() must follow before
        // addressing one of the newly released rows by item.
        QVERIFY(QMetaObject::invokeMethod(timeline, "releasePendingRows"));
        QCoreApplication::processEvents();
        // Deliberately NOT positionViewAtBeginning(): landing exactly at
        // atYBeginning fires TimelinePane.qml's onAtYBeginningChanged,
        // which dispatches its OWN passive (userInitiated=false)
        // requestNearTop() — a real, separate mechanism (the initial-
        // history-fill kick-off), not one this test can suppress, and it
        // would contaminate the "un-staged" premise below with a genuine
        // NearTop dispatch. Position a few rows down instead — still near
        // enough the top for a big prepend to displace it past the cache
        // buffer, but with enough headroom that atYBeginning never
        // triggers. Row 8, not lower: by the time this fixture reaches
        // here the automatic initial viewport-fill has already run (this
        // test's own room primes a full extra page via
        // setPaginationChunkForTest — count is 60, not 30, before this
        // call), so headroom is measured against that larger total.
        QVERIFY(positionAtSourceRow(timeline, 8));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        QVERIFY2(!timeline->property("atYBeginning").toBool(),
                 "fixture assumption: positioned with headroom, not at the "
                 "exact top edge");

        int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "never touched by this test — a ViewportFill dispatch "
                 "must not consume it");

        const double cacheBufferPx = 800.0;
        for (int batch = 0; batch < kBatches; ++batch) {
            const double heightBeforeBatch =
                timeline->property("contentHeight").toDouble();
            QSignalSpy completedSpy(controller.pagination(),
                                   &PaginationController::paginationCompleted);
            controller.pagination()->requestViewportFill();
            QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
            QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                     "fixture assumption: each fill page must insert rows");

            double heightGrowth = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                (heightGrowth = timeline->property("contentHeight").toDouble()
                                - heightBeforeBatch,
                 heightGrowth > cacheBufferPx + timeline->height()),
                kSignalTimeoutMs);
            QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                     qPrintable(QStringLiteral(
                         "batch %1: fixture no longer displaces the anchor "
                         "past the cache buffer (growth %2 <= %3) — this "
                         "test would pass on broken code")
                         .arg(batch).arg(heightGrowth)
                         .arg(cacheBufferPx + timeline->height())));

            const int rowAfter = controller.timeline()->rowForStableId(anchorId);
            QVERIFY2(rowAfter > rowBefore,
                     "fixture assumption: the prepend must shift the row "
                     "index");

            // Sample the offset RIGHT AFTER this single batch settles its
            // own onContentHeightChanged reaction — before the next batch
            // is even requested. This must be IMMEDIATE.
            double offsetAfter = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                ([&] {
                    QQuickItem *itemAfter = nullptr;
                    ((itemAfter = itemForSourceRow(timeline, rowAfter)) != nullptr);
                    if (!itemAfter)
                        return false;
                    offsetAfter = itemAfter->y()
                        - timeline->property("contentY").toDouble();
                    return qAbs(offsetAfter - offsetBefore) < 2.0;
                }()),
                kSignalTimeoutMs);
            QVERIFY2(qAbs(offsetAfter - offsetBefore) < 2.0,
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a prepend during a held gesture "
                         "moved the reader off their row before the next "
                         "batch was even requested: viewport offset %3 -> %4")
                         .arg(batch + 1).arg(kBatches)
                         .arg(offsetBefore).arg(offsetAfter)));

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The reported loading storm: "it keeps loading old messages each time I
    // scroll up ... and down", with the lag and jitter that come with it. A
    // live trace showed one deliberate upward gesture producing a burst of four
    // near_top batches, and the SAME burst after a DOWNWARD gesture.
    //
    // Root cause: near-top proximity was measured as `contentY <= height/2`.
    // contentY is not a distance from anything — it is an offset from originY,
    // and originY is arbitrary and MOVES as history loads. So the comparison
    // was not a weak proximity test, it was not a proximity test at all, and it
    // failed in either direction depending on where originY sat. Live it
    // answered "near the top" everywhere once a page had landed: the enter test
    // was permanently true, the exit test unreachable, and the gesture-settle
    // re-arm therefore fired after EVERY gesture in either direction, each one
    // resetting the controller's filtered-page bound to buy four more batches.
    //
    // What this test can and cannot prove, stated plainly because the fixture's
    // originY is what decides it:
    //   * measured here, originY sits POSITIVE (~2484 with the reader at the top
    //     of loaded history), so the old comparison under-triggered in this
    //     fixture rather than over-triggering as it did live. The
    //     "downward gesture must not re-arm" assertion is therefore a
    //     regression guard here, not a reproduction of the live defect;
    //   * the final assertion — continued UPWARD movement still re-arms — DOES
    //     fail on the old code in this fixture, because the old comparison never
    //     re-armed at a positive originY at all;
    //   * the mechanism itself (every band compared against distanceFromTop(),
    //     never raw contentY) is pinned by a text scan in
    //     QmlBindingContractTest, which no choice of fixture geometry can make
    //     vacuous. That scan is the primary guard; this test proves the geometry
    //     and the behaviour agree with it.
    void nearTopProximityIsMeasuredFromLoadedHistoryNotAbsoluteContentY()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/6);
        // A production-sized page (PAGINATION_BATCH = 20) of WRAPPING rows, so
        // the batch moves originY by much more than the band width. The default
        // 3-short-row mock page would leave the two measures within noise of
        // each other and the test could not tell them apart.
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long enough to "
                "wrap to several lines so the prepended batch moves originY by "
                "far more than one near-top band width").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // wheelMaxY(), i.e. the topMOST-of-loaded-HISTORY reachable contentY
        // — the OLDEST end, not wheelMinY(). The view is rotated: the newest
        // message sits at contentY 0 and older content sits at increasing
        // contentY, so "the top of loaded history" is the FAR/high end of
        // the scroll range, exactly what production's own distanceFromTop()
        // measures from (qml/TimelinePane.qml: `wheelMaxY() - contentY`).
        // Read from the Flickable's own properties so the test measures
        // Qt's geometry rather than trusting a QML helper to agree with it.
        const auto topmostY = [timeline] {
            const double maxY = timeline->property("originY").toDouble()
                + timeline->property("contentHeight").toDouble()
                + timeline->property("bottomMargin").toDouble()
                - timeline->height();
            const double minY = timeline->property("originY").toDouble()
                - timeline->property("topMargin").toDouble();
            return maxY < minY ? minY : maxY;
        };
        const auto bandWidth = [timeline] {
            return timeline->property("nearTopEnterDistance").toDouble();
        };
        // Wrapped, multi-line delegate text (deliberately used above for
        // realistic geometry) can keep nudging measured content height —
        // and therefore topmostY() — for a little while after insertion or
        // after a settle-timer round trip, well past a single
        // processEvents() call. Every probe below reads topmostY() only
        // after it has stopped moving across several consecutive drains, so
        // the distance a probe is set at is the distance production's own
        // live distanceFromTop() will actually see a moment later.
        const auto settledTopmostY = [&timeline, &topmostY] {
            double stable = topmostY();
            for (int attempt = 0; attempt < 30; ++attempt) {
                QCoreApplication::processEvents();
                const double next = topmostY();
                if (qAbs(next - stable) < 0.5)
                    return next;
                stable = next;
            }
            return stable;
        };
        // Sets contentY the given distance below the SETTLED top.
        const auto setContentYAtDistanceFromTop =
            [timeline, &settledTopmostY](double distance) {
            const double target = settledTopmostY() - distance;
            timeline->setProperty("contentY", target);
            QCoreApplication::processEvents();
        };

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        const double topBefore = topmostY();

        // At the top of loaded history: both measures agree that the reader is
        // near the top, and the edge latches exactly once.
        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "an approach to the top must consume the latch");

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop(/*userInitiated=*/true);
        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            topmostY() - timeline->property("contentY").toDouble()
                > bandWidth(),
            kSignalTimeoutMs);

        const double contentY = timeline->property("contentY").toDouble();
        const double fromTop = topmostY() - contentY;
        qInfo("top-edge batch geometry: topmostY %g -> %g, contentY %g, "
              "distance %g, band %g",
              topBefore, topmostY(), contentY, fromTop, bandWidth());

        // The reader now has a whole batch of history above them, so they are
        // NOT near the top — whichever coordinate Qt moved to absorb the
        // batch. This is what the fix reads.
        QVERIFY2(fromTop > bandWidth(),
                 qPrintable(QStringLiteral(
                     "a landed batch left the reader classified 'near the top' "
                     "(distance %1 <= band %2) — every further gesture would "
                     "re-arm and buy four more batches")
                     .arg(fromTop).arg(bandWidth())));
        QVariant reported;
        QVERIFY(QMetaObject::invokeMethod(timeline, "distanceFromTop",
                                          Q_RETURN_ARG(QVariant, reported)));
        const double reportedDistance = reported.toDouble();
        QVERIFY2(qAbs(reportedDistance - fromTop) < 1.0,
                 qPrintable(QStringLiteral(
                     "distanceFromTop() (%1) must agree with Qt's own geometry "
                     "(%2)").arg(reportedDistance).arg(fromTop)));

        // ── the progress gate, at the DISPATCH site ──────────────────────
        // The gate lives in checkNearTopEdge(), not on the gesture-settle
        // re-arm. Putting it on the re-arm was wrong twice over: an upward
        // gesture re-armed the latch and the next DOWNWARD gesture consumed it
        // and fetched anyway, and a reader parked at the exact top could never
        // re-arm at all (contentY is at its minimum there, so "must have moved
        // further up" is unsatisfiable). So the settle re-arm is now
        // unconditional within the band — an armed latch that a downward sample
        // cannot consume is harmless — and these assertions target consumption.
        //
        // No further page may LAND during this phase: one would move the top and
        // invalidate the probe positions. Pages stay AVAILABLE (the settle
        // re-arm is suppressed once history is exhausted, which would make this
        // vacuous); they simply never complete.
        mock->setPaginationDelayForTest(60000);
        QVERIFY2(!controller.pagination()->reachedStart(),
                 "premise: backfill must still be available");
        QVERIFY(positionAtTopEdge(timeline));
        setContentYAtDistanceFromTop(0.0);
        // Every probe below must stay INSIDE the band, including the downward
        // one — the band is only ~232 px here (half the ListView height, not
        // half the window), so leave room for the +40 excursion.
        const double probeBase = 120;
        const double probeStep = 40;
        QVERIFY2(probeBase + probeStep < bandWidth(),
                 qPrintable(QStringLiteral(
                     "fixture: every probe must sit inside the band (max %1 vs "
                     "band %2)").arg(probeBase + probeStep).arg(bandWidth())));
        setContentYAtDistanceFromTop(probeBase);

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        // checkNearTopEdge() short-circuits to "re-arm and return" when
        // stickToBottom is true, which would make every "did not consume"
        // assertion below pass vacuously. atBottomEdge() is known to be
        // frame-confused in the same way this round fixes (it omits originY —
        // recorded as a follow-up, not touched here), so with the positive
        // originY this fixture has, assert the premise rather than trust it.
        const auto notFollowingBottom = [timeline] {
            return !timeline->property("stickToBottom").toBool();
        };
        QVERIFY2(notFollowingBottom(),
                 "premise: these probes require stickToBottom == false");
        const auto armAndCheck = [timeline] {
            timeline->setProperty("nearTopArmed", true);
            QCoreApplication::processEvents();
            QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                      Q_ARG(QVariant, QVariant(true)));
            // Consumed == dispatched. Still armed == the gate refused.
            return !timeline->property("nearTopArmed").toBool();
        };

        // A first approach with no baseline yet consumes the latch and records
        // its distance.
        QVERIFY(timeline->setProperty("nearTopRequestDistance",
                                      std::numeric_limits<double>::infinity()));
        QVERIFY2(armAndCheck(), "a fresh approach must dispatch");
        QVERIFY2(qAbs(timeline->property("nearTopRequestDistance").toDouble()
                     - probeBase) < 1.0,
                 qPrintable(QStringLiteral(
                     "recorded dispatch distance %1 does not match the driven "
                     "probe %2")
                     .arg(timeline->property("nearTopRequestDistance")
                              .toDouble())
                     .arg(probeBase)));

        // Now DOWNWARD (toward the newest end, i.e. decreasing contentY in
        // this rotated view), still inside the band. The settle re-arms
        // (expected and harmless), but the dispatch must refuse — this is
        // the reported "it keeps loading old messages ... when I scroll
        // down".
        setContentYAtDistanceFromTop(probeBase + probeStep);
        QVERIFY(QMetaObject::invokeMethod(settleTimer, "restart"));
        QTRY_VERIFY_WITH_TIMEOUT(!settleTimer->property("running").toBool(),
                                 kSignalTimeoutMs);
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "the settle re-arm is deliberately unconditional in the band");
        // The settle ran updateStickAndPaginate(), the one thing here that can
        // flip stickToBottom. Re-assert before the refusal check.
        QVERIFY2(notFollowingBottom(),
                 "the settle re-pinned follow-latest; the refusal check below "
                 "would pass vacuously");
        QVERIFY2(!armAndCheck(),
                 "a DOWNWARD sample inside the band consumed the latch and "
                 "fetched a page — history loads while scrolling down");

        // Continuing UP (toward the oldest end, i.e. increasing contentY)
        // past the last request dispatches again, so "keep scrolling up"
        // still means "keep loading".
        setContentYAtDistanceFromTop(probeBase - probeStep);
        QVERIFY2(armAndCheck(), "continued upward progress must still dispatch");
        QVERIFY2(qAbs(timeline->property("nearTopRequestDistance").toDouble()
                     - (probeBase - probeStep)) < 1.0,
                 qPrintable(QStringLiteral(
                     "recorded dispatch distance %1 does not match the driven "
                     "probe %2")
                     .arg(timeline->property("nearTopRequestDistance")
                              .toDouble())
                     .arg(probeBase - probeStep)));

        // Pinned against the exact top with the baseline already AT the top:
        // "must have come closer" is unsatisfiable there, so without the
        // pinned-at-top clause this reader can never load again — the stranding
        // case the reviewer caught. Being unable to scroll further up IS the
        // intent; the controller's strike bound is what throttles from here.
        setContentYAtDistanceFromTop(0.0);
        QVariant atTopDistance;
        QVERIFY(QMetaObject::invokeMethod(timeline, "distanceFromTop",
                                          Q_RETURN_ARG(QVariant, atTopDistance)));
        QVERIFY2(atTopDistance.toDouble() <= 1.0,
                 qPrintable(QStringLiteral(
                     "premise: the probe must be pinned at the top (distance %1)")
                     .arg(atTopDistance.toDouble())));
        QVERIFY(timeline->setProperty("nearTopRequestDistance", 0.0));
        // Re-pin immediately adjacent to the check, with no intervening
        // processEvents(): "pinned at the exact top" is the one probe in
        // this test with zero tolerance by construction (a margin would
        // defeat the point), so it cannot absorb the same residual drift
        // the wider-margin probes above are deliberately immune to. A
        // dispatch can itself run queued work when armAndCheck() drains the
        // event queue, so re-reading and re-writing contentY right at the
        // call site (rather than relying on a settle that happened one
        // event-queue drain earlier) is what keeps this exact-zero probe
        // reliable.
        timeline->setProperty("contentY", topmostY());
        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "a reader pinned at the exact top could not dispatch — history "
                 "is unreachable at the one place they most want it");

        // ── the baseline must RATCHET, not record the last dispatch ───────
        // The ordinary sequence, and the one the first version of this gate
        // still got wrong: an upward gesture dispatches on band ENTRY, carries
        // on much closer to the top WITHOUT dispatching again (the latch is
        // already consumed), settles, and is followed by a downward gesture.
        // If the baseline only remembered the DISPATCH distance, everything
        // between the top and that point stayed unpaid and the downward sample
        // fetched — the reported defect surviving its own fix. The baseline must
        // therefore track the CLOSEST approach, including on samples that did
        // not dispatch.
        const double entry = 200;
        const double deep = 40;
        const double backOff = 45;   // closer than `entry`, farther than `deep`
        QVERIFY2(entry < bandWidth(),
                 "fixture: the entry probe must sit inside the band");
        QVERIFY(timeline->setProperty("nearTopRequestDistance",
                                      std::numeric_limits<double>::infinity()));
        setContentYAtDistanceFromTop(entry);
        QVERIFY2(armAndCheck(), "band entry must dispatch");

        // Same gesture continues up. The latch is spent, so this must NOT
        // dispatch — but it MUST lower the baseline.
        setContentYAtDistanceFromTop(deep);
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        const double ratcheted =
            timeline->property("nearTopRequestDistance").toDouble();
        QVERIFY2(qAbs(ratcheted - deep) < 1.0,
                 qPrintable(QStringLiteral(
                     "the baseline did not ratchet to the closest approach "
                     "(%1, expected %2) — the region between the top and the "
                     "last dispatch stays unpaid, so a later downward sample "
                     "fetches").arg(ratcheted).arg(deep)));

        // Now the downward gesture. Its sample is closer to the top than the
        // last DISPATCH (200) but farther than the closest approach (40), so it
        // must refuse. Without the ratchet it would fetch.
        setContentYAtDistanceFromTop(backOff);
        QVERIFY2(!armAndCheck(),
                 "a downward sample inside the region already traversed "
                 "consumed the latch — 'scroll up near the top, then scroll "
                 "down a little' still loads history");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The multi-batch guarantee the deleted stale-token bookkeeping existed
    // for: back-to-back prepends must each be compensated exactly once — no
    // lost batch (the "teleports toward the top" cascade) and no double
    // application. Also asserts the old per-batch state is genuinely gone.
    // NOTE: anchored mid-list, where a prepend leaves row positions
    // unchanged, so this asserts "no spurious write" rather than proving
    // compensation — topEdgePrependKeepsReaderOnTheSameRowMidGesture above
    // is the test that proves the compensation itself.
    void consecutivePaginationBatchesEachCompensateWithoutDoubleCounting()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(30);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        // Three pages: startup fill, then this test's two requests.
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/3);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // The per-batch capture bookkeeping is gone from the live object.
        QVERIFY(!timeline->property("anchorStableId").isValid());

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        const double yBefore = itemBefore->y();
        const double contentYBefore = timeline->property("contentY").toDouble();

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, kSignalTimeoutMs);
        QCoreApplication::processEvents();
        controller.pagination()->requestNearTop();
        QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 2, kSignalTimeoutMs);
        QCoreApplication::processEvents();

        const int rowAfter = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowAfter >= 0);
        QQuickItem *itemAfter = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((itemAfter = itemForSourceRow(timeline, rowAfter)) != nullptr),
             itemAfter != nullptr),
            kSignalTimeoutMs);
        const double expected = contentYBefore + (itemAfter->y() - yBefore);
        double actual = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (actual = timeline->property("contentY").toDouble(),
             qAbs(actual - expected) < 1.0),
            kSignalTimeoutMs);
        QVERIFY2(qAbs(actual - expected) < 1.0,
                 qPrintable(QStringLiteral(
                     "two consecutive prepends were not each compensated "
                     "exactly once: actual=%1 expected=%2")
                     .arg(actual).arg(expected)));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }


    // Round-3 growth fix: the deterministic proof for "an image pops up
    // while scrolling up and the view jumps by a lot". No real delegate
    // resize is forced (the offscreen QPA does not reliably drive one);
    // instead this uses the technique paginationAnchorRestorePreservesWheel-
    // Glide already established: bias the tracked baseline (viewAnchorLastY)
    // below the anchor row's real, UNCHANGED y, so maintainViewAnchor() sees
    // exactly the delta a real growth event above it would have produced.
    // No wheel glide is engaged here (pixelDelta cancels it).
    //
    // Ported: this used to prove growth is compensated by a relative
    // contentY shift DURING the gesture (fails on 9e505d2, where
    // maintainViewAnchor() returned unconditionally at the userScrollActive
    // guard, doing nothing at all). That relative-shift behavior was tried
    // again since and reverted a second time — see the "materialized: NO
    // WRITE" comment on this exact branch in qml/TimelinePane.qml: applying
    // it pulled the reader both up and down during loading, and down during
    // ordinary scrolling with nothing loading at all, in two separate
    // physical tests. Unlike 9e505d2, the current branch is NOT a no-op: it
    // measures the delta (viewAnchorLastY re-bases below to the row's real
    // position, proven below) and defers it — it simply must not WRITE
    // contentY while the gesture is live. That is the invariant this proves
    // now.
    void maintainViewAnchorDefersGrowthDeltaMidGestureWithoutGlide()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        // Open a touchpad scroll session (pixelDelta — never engages the
        // wheel engine) without letting it settle.
        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY2(!controller.timelineScroll()->motionActive(),
                 "the pixel path must never engage the wheel engine");

        // Bias the baseline below the anchor's real, unchanged y — exactly
        // what 270px of growth above it (an image row resolving) produces.
        constexpr double simulatedGrowth = 270.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        const double afterY = timeline->property("contentY").toDouble();
        // NO WRITE while the gesture is live (see the class comment above
        // and the "materialized" branch comment in TimelinePane.qml): the
        // growth is measured, not applied, so contentY must be exactly
        // unchanged rather than shifted by simulatedGrowth.
        QVERIFY2(qAbs(afterY - beforeY) < 1.0,
                 qPrintable(QStringLiteral(
                     "growth above the anchor was written into contentY "
                     "during an active gesture, which the deliberate "
                     "no-write design forbids: before=%1 after=%2")
                     .arg(beforeY).arg(afterY)));
        // The branch still re-bases its measurement via captureViewAnchor()
        // (see the "materialized" comment) — proven by a fresh, non-empty
        // anchor id, not by an exact Y match: captureViewAnchor() re-derives
        // from the row at the viewport's PHYSICAL top, which is not
        // necessarily the same row positionAtSourceRow() centered earlier,
        // so pinning an exact pixel value here would assert this fixture's
        // incidental geometry rather than the real postcondition.
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the branch must leave a resolved anchor behind, not a "
                 "stale/empty one, even though it performs no write");
        QVERIFY2(!controller.timelineScroll()->motionActive(),
                 "no glide was active — nothing should have been engaged");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Companion for the DISCRETE-WHEEL path, ORIGINALLY: growth compensation
    // must not just shift contentY, it must also translate an in-flight
    // glide's coalesced target via TimelineScrollController::
    // translateActiveMotion() — otherwise the glide's next integrated frame
    // would overwrite the correction with its stale pre-growth position.
    //
    // Ported for the "materialized: NO WRITE" reversal (see that branch's
    // comment in qml/TimelinePane.qml): a wheel glide is userScrollActive
    // (wheelAnimating) exactly like a touchpad session or a drag, so growth
    // landing mid-flight now takes the same NO-WRITE path as everywhere
    // else — translateActiveMotion() is consequently no longer called from
    // this branch at all (left as dead code in TimelineScrollController;
    // not this file's to remove). The invariant this now proves is the
    // glide's own: an in-flight wheel motion must be completely undisturbed
    // by a growth measurement that lands mid-flight — same position, same
    // coalesced target, same remaining distance, still moving.
    void maintainViewAnchorTranslatesActiveGlideWhenGrowthLandsMidFlight()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        // Row 5, not 15, and the real wheelMinY()/wheelMaxY() bounds, not a
        // hand-rolled originY/contentHeight-height guess: see wheelBounds()
        // and the comment on paginationPrependPreservesWheelGlide. Row 15 of
        // ~30 lands the reader AT the true minimum already in the
        // un-virtualized layout, leaving no headroom for the glide.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 5));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        const double glideStartY = timeline->property("contentY").toDouble();
        double minY = 0, maxY = 0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        for (int notch = 0; notch < 6; ++notch)
            scroll->wheelNotch(120.0,
                               timeline->property("contentY").toDouble(),
                               minY, maxY, timeline->height());
        QTRY_VERIFY_WITH_TIMEOUT(scroll->motionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble() < glideStartY - 10.0,
            2000);
        QVERIFY(scroll->motionActive());

        const double targetBefore = scroll->targetYForTest();
        const double positionBefore = scroll->positionYForTest();
        const double remainingBefore = targetBefore - positionBefore;

        constexpr double simulatedGrowth = 300.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QVERIFY2(scroll->motionActive(),
                 "an in-flight glide must survive a growth correction");
        const double afterY = timeline->property("contentY").toDouble();
        // NO WRITE while the glide is live: contentY, the coalesced target,
        // and the remaining distance must all be exactly as they were —
        // the measurement is deferred, never applied, so nothing about the
        // glide's motion may change.
        QVERIFY2(qAbs(afterY - beforeY) < 1.0,
                 qPrintable(QStringLiteral(
                     "contentY moved during an active glide, which the "
                     "deliberate no-write design forbids: before=%1 "
                     "after=%2")
                     .arg(beforeY).arg(afterY)));
        QVERIFY2(qAbs(scroll->targetYForTest() - targetBefore) < 1.0,
                 "the glide's coalesced target moved even though nothing "
                 "should have written to it");
        QVERIFY2(qAbs((scroll->targetYForTest() - scroll->positionYForTest())
                     - remainingBefore) < 1.0,
                 "growth correction changed the glide's remaining distance");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Blocking review finding, ORIGINALLY: userScrollActive also covers a
    // NATIVE drag / kinetic flick, where QQuickFlickable owns contentY and
    // recomputes it from the recorded press position on every move — a
    // growth correction written there is discarded by construction, and
    // (on the design of that era, where the self-driven path DID write a
    // relative delta) re-basing the baseline during a drag would have hidden
    // that growth from settle-time re-anchoring.
    //
    // Ported for the later "materialized: NO WRITE" reversal (see that
    // branch's comment in qml/TimelinePane.qml): the self-driven path no
    // longer writes a relative delta either, so there is no longer a
    // drag-only special case to protect — captureViewAnchor() unconditionally
    // re-bases from the LIVE, real anchorPositionForItem() on every path,
    // drag included. That is not "hiding" anything: nothing here tracks a
    // persistent delta across calls any more. A REAL height change (e.g. an
    // image resolving) is picked up fresh on whichever later call reads the
    // live geometry, whether that call happened during a drag or not. This
    // proves contentY is not written during a drag (unchanged from before
    // this reversal) and that the anchor is still validly re-derived (not
    // left stale or empty) — the drag path behaves exactly like the
    // self-driven one, with no special case left to regress independently.
    void growthDeltaIsDeferredWhileFlickableOwnsTheDrag()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        // Preconditions, so a future geometry change fails loudly here
        // instead of as a confusing "growth not compensated".
        QVERIFY(!controller.pagination()->busy());

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        // A real native drag: press and move, so Flickable's own `moving`
        // turns true and IT owns contentY.
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(360, 400));
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&window, QPoint(360, 400 + step * 14));
            QCoreApplication::processEvents();
        }
        QVERIFY2(timeline->property("moving").toBool(),
                 "a native drag must set Flickable.moving");
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "a drag is still a user scroll session");
        QVERIFY2(!timeline->property("selfDrivenScrollActive").toBool(),
                 "a drag is NOT a self-driven (Lightning-owned) scroll");

        constexpr double simulatedGrowth = 250.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        const double afterY = timeline->property("contentY").toDouble();
        const double baselineAfter =
            timeline->property("viewAnchorLastY").toDouble();
        // Release before asserting: a failing QVERIFY must not leave the
        // left button pressed for later tests in this binary.
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                            QPoint(360, 512));

        Q_UNUSED(baselineAfter);
        // contentY must not move during the drag (Flickable owns it).
        QCOMPARE(afterY, beforeY);
        // The anchor must still be validly re-derived from live geometry —
        // not left pointing at the pre-drag id/position, and not emptied
        // out. The drag genuinely moved the viewport (8 real mouse-move
        // steps), so the physical-top row after it is legitimately a
        // DIFFERENT row than before; asserting a specific id or position
        // here would pin this fixture's incidental geometry, not the real
        // contract.
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the drag path must leave a resolved anchor behind, not a "
                 "stale/empty one");
    }

    // ── v0.7.x round-2 review: per-branch scroll-trace instrumentation ──
    // A proposed fix to the displaced-anchor branch (bounding/symmetrizing
    // its correction) was reviewed and WITHDRAWN: the single combined
    // diagGrowthCorrections counter cannot tell WHICH of the five distinct
    // outcomes in maintainViewAnchor() actually ran during a real
    // reported jitter/teleport gesture, nor whether a correction's
    // magnitude was proportionate to real inserted content. The tests
    // below pin ONLY the trace plumbing itself (each counter increments on
    // its own branch, the emitted line renders every field, tracing off
    // costs nothing) — none of them claim any scroll POSITION behavior
    // changed, because none did: maintainViewAnchor()'s actual corrections
    // are byte-for-byte the pre-existing logic, with diagnostic-only
    // `if (scrollTrace)` bookkeeping added alongside (carry-bucket: the
    // counters reset at print, so post-settle reconciles land on the
    // next line instead of vanishing).

    // Guard verification: with LIGHTNING_SCROLL_TRACE unset, scrollTrace
    // is false (bound to the CONSTANT scrollTraceEnabled, read once from
    // the environment), so every `if (scrollTrace)` increment site
    // short-circuits — this drives the exact growth scenario that WOULD
    // move diagMaterializedFirings/diagMaterializedAppliedSum/
    // diagMaterializedMaxAbsDelta if tracing were on, with it explicitly
    // off, and proves nothing moved and no line was emitted.
    void scrollTraceDisabledAddsNoDiagnosticCost()
    {
        qunsetenv("LIGHTNING_SCROLL_TRACE");

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(!controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QCOMPARE(timeline->property("scrollTrace").toBool(), false);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));

        LogCapture capture;

        const QPointF pos(320, 300);
        for (int attempt = 0; attempt < 50; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            if (timeline->property("userScrollActive").toBool())
                break;
            QTest::qWait(10);
        }
        QVERIFY(timeline->setProperty(
            "viewAnchorLastY",
            timeline->property("viewAnchorLastY").toDouble() - 270.0));
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagActive").toBool(), false);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedAppliedSum").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagDisplacedMaxAbsGrew").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagDisplacedMaxAbsGrewRows").toInt(), 0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsGrewOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShiftContentDelta").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShiftRows").toInt(),
                 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedMaxAbsDelta").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(timeline->property("diagPrependFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagPrependOriginShiftSum").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagPrependMaxAbsOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property("diagPrependMaxAbsOriginShiftRows").toInt(),
                 0);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftContentDelta").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftPath").toString(),
                 QStringLiteral("none"));

        for (const QString &m : capture.messages()) {
            QVERIFY2(!m.contains(QStringLiteral("scroll-gesture")),
                     qPrintable(QStringLiteral(
                         "a scroll-gesture line was emitted with tracing "
                         "off: %1").arg(m)));
        }
    }

    // "The line renders all fields" (lead instruction 3): capture the real
    // console.info() emission through a genuine gesture reaching settle,
    // and assert every per-branch field name from the extended line is
    // present alongside the pre-existing ones. Deliberately loose about
    // VALUES — that is covered by the dedicated per-branch tests below —
    // this only pins that the line's shape cannot silently drop a field.
    void scrollTraceLineIncludesAllPerBranchFields()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QCoreApplication::processEvents();

        LogCapture capture;

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");

        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 3000);

        QString line;
        for (const QString &m : capture.messages()) {
            if (m.contains(QStringLiteral("scroll-gesture"))) {
                line = m;
                break;
            }
        }
        QVERIFY2(!line.isEmpty(), "no scroll-gesture line was emitted at settle");

        const QStringList expectedFields = {
            QStringLiteral("events="),
            QStringLiteral("pixel="),
            QStringLiteral("angle="),
            QStringLiteral("netY="),
            QStringLiteral("dContentH="),
            QStringLiteral("originY="),
            QStringLiteral("dOriginY="),
            QStringLiteral("noAnchorReturns="),
            QStringLiteral("stickToBottomReturns="),
            QStringLiteral("anchorCorrections="),
            QStringLiteral("growthCorrections="),
            QStringLiteral("displacedFirings="),
            QStringLiteral("displacedApplied="),
            QStringLiteral("displacedMaxAbsGrew="),
            QStringLiteral("displacedMaxAbsGrewRows="),
            QStringLiteral("displacedMaxAbsGrewOriginShift="),
            QStringLiteral("displacedMaxAbsOriginShift="),
            QStringLiteral("displacedMaxAbsOriginShiftDContentH="),
            QStringLiteral("displacedMaxAbsOriginShiftRows="),
            QStringLiteral("materializedFirings="),
            QStringLiteral("materializedMaxAbsDelta="),
            // NOTE: no "materializedApplied=". That field was declared,
            // reset and printed but never incremented once the materialized
            // branch was reversed to NO WRITE, so it always read 0 — a
            // trace reader would take that as "no growth was measured" when
            // the real magnitude is in materializedMaxAbsDelta. A
            // diagnostic that reports a constant is worse than one that
            // reports nothing, so it was removed rather than left in.
            QStringLiteral("unresolvedId="),
            QStringLiteral("evictedNoInsert="),
            QStringLiteral("dragDeferrals="),
            QStringLiteral("prependFirings="),
            QStringLiteral("prependOriginShift="),
            QStringLiteral("prependMaxAbsOriginShift="),
            QStringLiteral("prependMaxAbsOriginShiftRows="),
            QStringLiteral("prependMaxAbsOriginShiftDContentH="),
            QStringLiteral("prependMaxAbsOriginShiftPath="),
            QStringLiteral("stick="),
            QStringLiteral("topDist="),
            QStringLiteral("nearTop="),
        };
        for (const QString &field : expectedFields) {
            QVERIFY2(line.contains(field),
                     qPrintable(QStringLiteral(
                         "scroll-gesture line is missing field %1: %2")
                         .arg(field, line)));
        }
    }

    // Per-branch counter (materialized path): discriminates whether the
    // already-tested, already-symmetric self-driven growth-delta branch —
    // NOT the displaced branch under review — is where a reported jitter
    // is actually coming from. Reuses
    // maintainViewAnchorAppliesGrowthDeltaMidGestureWithoutGlide's exact
    // drive; asserts ONLY the new counters, not scroll correctness (already
    // covered there, unchanged by this round).
    //
    // Ported for the "materialized: NO WRITE" reversal (see the comment on
    // that branch in qml/TimelinePane.qml): applying the measured delta
    // here was tried twice and rejected by physical testing both times, so
    // the branch now only MEASURES (diagMaterializedFirings,
    // diagMaterializedMaxAbsDelta) and never applies. diagMaterializedFirings
    // and diagMaterializedMaxAbsDelta still pin the branch's real, current
    // contract; diagMaterializedAppliedSum pins that the branch stays a
    // pure measurement (asserted to stay unchanged, not to grow — see the
    // inline comment at the assertion for why that field can never move).
    void diagMaterializedCountersTrackTheSelfDrivenGrowthBranch()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        // maintainViewAnchor() measures against anchorPositionForItem() —
        // item.y + item.height (the rotated view's "physical top edge" is a
        // row's logical BOTTOM edge), not item.y() alone. Using plain y()
        // here would bias viewAnchorLastY by one row height and desync the
        // simulated growth from what the branch actually measures.
        const double realAnchorY = anchorItem->y() + anchorItem->height();

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY(timeline->property("diagActive").toBool());
        // Baseline capture instead of an absolute zero gate. The cause is
        // NOT fully identified: one full-suite run on a busy machine read
        // diagMaterializedFirings == 1 here (both trees, same run;
        // isolated reruns and six idle full-suite runs read 0), and this
        // fixture has no receipts and no media bridge, so no avatar or
        // receipt mechanism can explain it — the diag test family is
        // load-timing sensitive (sibling diag tests fail under deliberate
        // 24-way CPU saturation with this hunk reverted). The warning
        // below keeps any recurrence visible; the assertions stay EXACT
        // in the quiescent (normal) case via the captured baseline.
        const int baseFirings =
            timeline->property("diagMaterializedFirings").toInt();
        const double baseApplied =
            timeline->property("diagMaterializedAppliedSum").toDouble();
        if (baseFirings != 0)
            qWarning("diag baseline not quiescent: firings=%d applied=%f "
                     "(load-timing; see comment)",
                     baseFirings, baseApplied);

        constexpr double simulatedGrowth = 270.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realAnchorY - simulatedGrowth));

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseFirings + 1);
        // diagMaterializedAppliedSum is a stale field: production's
        // "materialized" branch (qml/TimelinePane.qml) performs NO WRITE —
        // see the comment on that branch — so nothing is ever "applied"
        // here to sum. It stays exactly at its baseline. The magnitude this
        // branch actually measured lives in diagMaterializedMaxAbsDelta,
        // asserted below; that is the field a physical trace should read.
        QCOMPARE(timeline->property("diagMaterializedAppliedSum").toDouble(),
                 baseApplied);
        // Magnitude: EXACT in the quiescent case (the normal one); only a
        // non-zero baseline firing of unknown magnitude degrades this to
        // the at-least bound (a larger earlier |delta| legitimately
        // keeps the max — the field is selected by |x|).
        const double maxAbs =
            qAbs(timeline->property("diagMaterializedMaxAbsDelta").toDouble());
        if (baseFirings == 0) {
            QVERIFY2(qAbs(maxAbs - simulatedGrowth) < 1.0,
                     "diagMaterializedMaxAbsDelta did not record the exact "
                     "driven magnitude");
        } else {
            QVERIFY2(maxAbs >= simulatedGrowth - 1.0,
                     "diagMaterializedMaxAbsDelta lost the driven magnitude");
        }
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Per-branch counter (drag-deferral): discriminates H-A — whether a
    // native drag/flick ever actually engages maintainViewAnchor()'s defer
    // path. A pure click-drag never opens the diag session by itself (only
    // a wheel/pixel delta calls diagNoteEvent()), so this opens the session
    // with one touchpad delta first — the realistic "mixed gesture" case —
    // then immediately performs a real native drag, exactly like
    // growthDeltaIsDeferredWhileFlickableOwnsTheDrag's proven drive.
    void diagDragDeferralsCountsNativeDragEngagements()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        // Open the diag session first (a pure drag never calls
        // diagNoteEvent() on its own).
        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY(timeline->property("diagActive").toBool());

        // Immediately (same as growthDeltaIsDeferredWhileFlickableOwnsThe-
        // Drag): a real native drag, so Flickable's own `moving` turns true.
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(360, 400));
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&window, QPoint(360, 400 + step * 14));
            QCoreApplication::processEvents();
        }
        QVERIFY2(timeline->property("moving").toBool(),
                 "a native drag must set Flickable.moving");
        QVERIFY2(!timeline->property("selfDrivenScrollActive").toBool(),
                 "a drag is NOT a self-driven (Lightning-owned) scroll");

        // Baselines, not absolute zero/one gates, captured as LATE as
        // possible (right before the one deterministic call this test
        // drives): opening the touchpad session and the real mouse-move
        // sequence above can each independently run maintainViewAnchor()
        // through this same branch under load (diagMaterializedFirings is
        // the outer counter for the whole userScrollActive branch, and
        // diagDragDeferrals increments INSIDE it — see the field comments
        // in qml/TimelinePane.qml — so a load-timing firing during the
        // simulated drag would inflate diagDragDeferrals too, not just
        // diagMaterializedFirings). Same load-timing-sensitive family as
        // the sibling diagMaterializedCountersTrackTheSelfDrivenGrowthBranch;
        // capturing here instead of before the drag keeps the asserted
        // delta exactly +1 regardless of how many firings preceded it.
        const int baseFirings =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDragDeferrals =
            timeline->property("diagDragDeferrals").toInt();

        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - 250.0));
        const double beforeY = timeline->property("contentY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        const double afterY = timeline->property("contentY").toDouble();

        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                            QPoint(360, 512));

        QCOMPARE(afterY, beforeY);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(),
                 baseDragDeferrals + 1);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseFirings + 1);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
    }

    // Per-branch counter (L1 split, unresolved-id half): a stable id that no
    // longer resolves to any row (redaction, local-echo id change, or — as
    // this fixture drives it — a fabricated id) must increment
    // diagUnresolvedIdFallbacks specifically, NOT diagEvictedNoInsertFallbacks
    // — that is the whole point of the split (see the L1 comment on the two
    // declarations): "the id is gone" and "the id is fine but the delegate
    // was merely evicted" are different causes with different implications,
    // and conflating them is exactly what the old single counter did.
    void diagUnresolvedIdFallbackCountsGenuinelyUnresolvableAnchor()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        // Open the diag session.
        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);

        // A stable id that resolves to no row at all — rowForStableId()
        // returns -1, so `it` is null and `row >= 0` is false: this cannot
        // enter the displaced branch (which requires row >= 0) and falls
        // straight to the capture fallback.
        QVERIFY(timeline->setProperty(
            "viewAnchorId", QStringLiteral("$this-event-id-does-not-exist")));

        // Baseline the OTHER branch counters immediately before the call.
        // The invariant this test names is about THIS maintainViewAnchor()
        // call — "it cannot enter the displaced branch and falls straight to
        // the capture fallback" — not about the whole fixture's history. The
        // touchpad loop above drives real geometry, so the self-driven
        // (materialized) branch may legitimately have fired during setup;
        // asserting the ABSOLUTE counter was zero made this case depend on
        // that incidental timing and it failed intermittently (measured 4
        // pass / 4 fail in isolation). Deltas assert the real thing. Same
        // baseline idiom the sibling diag tests already use.
        const int baseEvicted =
            timeline->property("diagEvictedNoInsertFallbacks").toInt();
        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();
        const int baseMaterialized =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDragDeferrals =
            timeline->property("diagDragDeferrals").toInt();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 1);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(),
                 baseEvicted);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseMaterialized);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(),
                 baseDragDeferrals);
    }

    // INVERTED from the pre-1e50f6a suite (2026-08-18). The original test
    // asserted the evicted-no-insert fallback FIRING after real ListView
    // cache eviction. The rotated Flickable + Column instantiates every
    // loaded row and never evicts a delegate, so that fixture aborted on its
    // own precondition ("fixture no longer evicts...") from 8f84d18 onward.
    // The preserved invariant is the structural fact itself: a loaded row's
    // delegate SURVIVES arbitrary scrolling, so maintainViewAnchor() must
    // always resolve the anchor's item and must never take the
    // evicted-no-insert fallback (nor the unresolved-id one) for an anchor
    // whose event is still loaded. If a future timeline change reintroduces
    // delegate eviction, this fails and the ported eviction fixtures in Git
    // history (pre-8f84d18) are the starting point for re-porting.
    void anchorDelegateSurvivesDistantScrollNeverEvictedFallback()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "message %1 — long enough to wrap across several lines so "
                "sixty rows vastly exceed any plausible delegate cache, the "
                "geometry that USED to evict the anchor delegate").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);

        // Jump far away. Under the old virtualized view this destroyed the
        // anchor delegate; the whole point now is that it MUST NOT.
        QVERIFY(positionAtSourceRow(timeline, 50));
        QCoreApplication::processEvents();
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the un-virtualized Column must keep every loaded row's "
                 "delegate alive — eviction has been reintroduced");
        QVERIFY2(!timeline->property("moving").toBool(),
                 "programmatic positioning must not leave Flickable.moving");
        // Pin the id back in case a queued maintainViewAnchorCoalesced()
        // re-captured onto the row now at the top of the viewport.
        QVERIFY(timeline->setProperty("viewAnchorId", anchorId));

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        // A coalesced maintainViewAnchorCoalesced() during the wheel loop
        // can legitimately re-capture the anchor onto the row now at the
        // top of the viewport. The invariant under test does not depend on
        // WHICH loaded event is the anchor — re-pin the row-15 id and
        // verify its delegate is (still) alive right before the driven
        // call.
        QVERIFY(timeline->setProperty("viewAnchorId", anchorId));
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the anchor's delegate must be alive for this invariant");
        // The fixture's own setup can legitimately drive counters; assert
        // no INCREASE across the driven call, never a hardcoded total.
        const int baseEvicted =
            timeline->property("diagEvictedNoInsertFallbacks").toInt();
        const int baseUnresolved =
            timeline->property("diagUnresolvedIdFallbacks").toInt();
        const int baseMaterialized =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(),
                 baseEvicted);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(),
                 baseUnresolved);
        // With the delegate alive and input active, the materialized
        // measurement branch is the one that must own this geometry. A
        // coalesced maintainViewAnchor can legitimately fire alongside the
        // driven call, so assert growth, not an exact total.
        QVERIFY2(timeline->property("diagMaterializedFirings").toInt()
                     > baseMaterialized,
                 "the driven call must take the materialized branch");
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
    }

    // INVERTED from the pre-1e50f6a suite (2026-08-18). The original test
    // injected displaced anchor bookkeeping onto an evicted delegate and
    // asserted the displaced branch's counters recorded the exact grew/
    // origin-shift pairs. Without eviction the displaced branch requires a
    // precondition (`!it`) that a loaded row can never satisfy, so the
    // preserved invariant is the inverse: even when the anchor bookkeeping
    // CLAIMS rows were inserted above the reader (row > viewAnchorRow), a
    // live delegate must route the correction through the idle absolute
    // restore — the estimate-based displaced arithmetic must not fire.
    // This is the same invariant the 2026-08-12 physical capture recorded
    // as displacedFirings=0/evictedNoInsert=0 across ~28 real pagination
    // batches; here it is pinned deterministically.
    void displacedBranchDoesNotFireWhileAnchorDelegateAlive()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "message %1 — long enough to wrap so the geometry matches "
                "the displaced fixture this test inverts").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        const int sourceRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(sourceRow >= 0);
        // Ask the pane for the anchor's view row: the paced proxy means a
        // hand-computed count-1-source mapping can be off while rows are
        // still releasing.
        QVariant viewRowOut;
        QVERIFY(QMetaObject::invokeMethod(
            timeline, "viewRowForStableId", Q_RETURN_ARG(QVariant, viewRowOut),
            Q_ARG(QVariant, anchorId)));
        const int anchorViewRow = viewRowOut.toInt();
        QVERIFY(anchorViewRow >= 0);
        QVERIFY2(itemForSourceRow(timeline, sourceRow) != nullptr,
                 "the anchor's delegate must be alive for this invariant");
        QVERIFY(!timeline->property("moving").toBool());
        QVERIFY(!timeline->property("userScrollActive").toBool());

        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();
        const int basePrepend =
            timeline->property("diagPrependFirings").toInt();

        // Inject the exact bookkeeping the displaced branch keys on: the
        // anchor's recorded row three below its real one (as if three rows
        // were inserted above the reader) with a matching content-height
        // delta. Idle, delegate alive: the displaced arithmetic must NOT
        // run — the idle branch restores from the live measurement instead.
        constexpr double injectedContentDelta = -3582.0;
        constexpr int injectedInsertedRows = 3;
        const double contentHeightNow =
            timeline->property("contentHeight").toDouble();
        QVERIFY(timeline->setProperty("viewAnchorRow",
                                      anchorViewRow - injectedInsertedRows));
        QVERIFY(timeline->setProperty("viewAnchorContentHeight",
                                      contentHeightNow
                                          - injectedContentDelta));

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
        // The prepend DIAGNOSTIC still records the firing (row rose above
        // the recorded anchor row) and must attribute it to the idle path,
        // proving which branch actually handled it.
        QCOMPARE(timeline->property("diagPrependFirings").toInt(),
                 basePrepend + 1);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftPath").toString(),
                 QStringLiteral("idle"));
        // The idle branch re-based the bookkeeping to reality: the recorded
        // row is the anchor's real view row again (re-resolved after the
        // call — the paced proxy may have released rows in between).
        QVERIFY(QMetaObject::invokeMethod(
            timeline, "viewRowForStableId", Q_RETURN_ARG(QVariant, viewRowOut),
            Q_ARG(QVariant, anchorId)));
        QCOMPARE(timeline->property("viewAnchorRow").toInt(),
                 viewRowOut.toInt());
    }


    // M2: an all-zero line does not distinguish "the mechanism ran and had
    // nothing to correct" from "the mechanism never engaged at all"
    // (viewAnchorId empty, or stickToBottom). diagNoAnchorReturns must
    // increment on that early-return path specifically.
    void diagNoAnchorReturnsCountsTheEarlyReturn()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        // stickToBottom true is the cheapest way to force the early return
        // regardless of viewAnchorId's own state (which starts "" anyway on
        // a fresh room, itself already satisfying the OTHER half of the
        // guard).
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        // Every other counter must stay at 0 — nothing else ran. In
        // particular the SIBLING early-return counter: an empty
        // viewAnchorId wins over stickToBottom when both hold, and that
        // precedence is the thing a maintainer reading a trace depends on.
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    // The sibling early-return arm, which the counter split exists to make
    // legible: a correction scheduled while scrolled up and then dropped
    // because the reader returned to the bottom. Requires a NON-EMPTY
    // viewAnchorId — with an empty one the other arm wins (pinned above),
    // and that asymmetry is exactly what a trace reader must be able to
    // trust. Without this test the split ships unverified: nothing else in
    // the suite ever increments diagStickToBottomReturns.
    void diagStickToBottomReturnsCountsTheDroppedCorrection()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        // A real anchor first (scrolled up), so the empty-id arm cannot
        // claim the return; then the reader lands back at the bottom.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        QVERIFY(!timeline->property("viewAnchorId").toString().isEmpty());
        QVERIFY(timeline->setProperty("stickToBottom", true));

        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 1);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 2);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        // No correction path ran.
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    // M1, the structural fix itself: scrollSettleTimer.onTriggered calls
    // diagFlushGesture() as its LAST statement, but a post-settle
    // correction (a media hydration or late-decryption height change, say)
    // lands via Qt.callLater chains AFTER that handler returns — so every
    // outcome counter must survive being
    // incremented while diagActive is false (before any gesture has ever
    // opened a session, exactly like a reconcile call landing after a flush)
    // and must still appear on the NEXT line that gets printed, not be lost.
    // Drives maintainViewAnchor()/diagNoteEvent()/diagFlushGesture()
    // directly (bypassing real WheelEvent/Timer scheduling, which the other
    // tests already cover) so this is deterministic and cannot be polluted
    // by incidental delegate/layout churn between "before settle" and
    // "after settle".
    void diagOutcomeCountersSurviveAcrossAFlushBoundary()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        // Two early-returns BEFORE any gesture has ever opened a session —
        // diagActive is false throughout, standing in for a reconcile call
        // landing after a prior flush already printed and reset.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagActive").toBool(), false);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        // Opening a gesture (diagNoteEvent(), via a real touchpad delta —
        // the same proven pattern the other tests in this file use) must
        // NOT reset the carried outcome count — only the event/pixel/angle
        // group is gesture-local. viewAnchorId stays "" throughout (nothing
        // here ever calls captureViewAnchor()), so this single delta cannot
        // itself trigger any further maintainViewAnchor() outcome.
        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QCOMPARE(timeline->property("diagActive").toBool(), true);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        LogCapture capture;
        QVERIFY(QMetaObject::invokeMethod(timeline, "diagFlushGesture"));

        QString line;
        for (const QString &m : capture.messages()) {
            if (m.contains(QStringLiteral("scroll-gesture"))) {
                line = m;
                break;
            }
        }
        QVERIFY2(!line.isEmpty(), "no scroll-gesture line was emitted");
        QVERIFY2(line.contains(QStringLiteral("noAnchorReturns=2")),
                 qPrintable(QStringLiteral(
                     "the pre-gesture outcome count was not carried into "
                     "the flushed line: %1").arg(line)));

        // Drained exactly at print, not before and not left to accumulate
        // forever: a fresh early-return after this line starts back at 1.
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);
        QCOMPARE(realWarnings(warnings), QStringList{});
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
