// Loads the real TimelinePane.qml through the "MatrixClient" QML module over a
// real AppController on the mock backend, and asserts zero engine warnings
// across its presentation states. Binding and registration errors only show
// up when a QQmlEngine evaluates the component, which a text scan cannot do.
#include <QtTest/QtTest>

#include <functional>

#include <limits>

#include <QGuiApplication>
#include <QClipboard>
#include <QWheelEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <cmath>
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
#include "models/ReverseListProxyModel.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "threads/ThreadController.h"
#include "matrix/MockMatrixClient.h"

namespace {
// Every QTRY here waits for a state that must eventually hold, so this is a
// starvation budget for parallel ctest runs, not a latency assertion. Keep it
// well below PaginationController's 30 s stall watchdog: a failing case stacks
// several waits, and the watchdog firing mid-test adds an unrelated failure.
constexpr int kSignalTimeoutMs = 4000;
// How long the anchor's row may take to be built (ReverseListProxyModel paces
// its reveal). Generous because it ends as soon as the row exists, and a slow
// machine must not look like a compensation defect.
constexpr int kAnchorRowRevealTimeoutMs = 30000;
}

namespace {
// Keeps a scroll session open across an asynchronous wait, as a reader who
// keeps swiping does: restarts the 250 ms settle timer periodically until
// destroyed. Restarting once and then waiting races the timer.
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
        // 20 ms leaves enough margin against the 250 ms settle timer under
        // parallel-run CPU contention; at 50 ms the timer occasionally fired
        // mid-run.
        m_ticker.start(20);
    }
    ~GestureHold() { m_ticker.stop(); }

private:
    QTimer m_ticker;
};

// Captures every message logged while alive, to check the
// LIGHTNING_SCROLL_TRACE lines: QML console.info() is not otherwise
// observable from a test.
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
        // startSync() populates the room list synchronously; poll briefly as a
        // safety margin only.
        for (int i = 0; i < 50 && controller.roomList()->rowCount() <= row; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() <= row)
            return {};
        const QModelIndex idx = controller.roomList()->index(row, 0);
        return controller.roomList()
            ->data(idx, RoomListModel::RoomIdRole)
            .toString();
    }

    // The first loaded event in the current room that other events name as
    // their thread root (the mock thread fixture).
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

    // A Repeater reparents its delegates with setParentItem() only, so they
    // are not QObject-tree descendants and findChildren() cannot see them.
    // Walks childItems() instead; only needed for Repeater-created content.
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

    // Row addressing on the rotated Flickable + Column timeline: every loaded
    // row is instantiated and the row API is view-row based (view row 0 = the
    // newest message, at content y 0). ListView's positionViewAtIndex /
    // itemAtIndex do not exist here. Model (source) row 0 is the oldest
    // message; these helpers take source rows and convert in one place.

    // Source row -> view row, through the pane's own mapping (anchored on the
    // model total, not `count`, which lags while a page is still draining).
    static int viewRowForSourceRow(QQuickItem *timeline, int sourceRow)
    {
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "viewRowForSourceRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(sourceRow))))
            return -1;
        return out.toInt();
    }

    // The fixture must stop growing before an anchor is captured:
    // ReverseListProxyModel keeps revealing rows after pagination reports
    // idle, so an anchor captured then picks a different row run to run. Ask
    // the proxy (`revealIdle()`, the condition it stops its own timer on)
    // rather than polling contentHeight, since the reveal interval is adaptive.
    static bool waitForRowsToStopArriving(AppController &controller)
    {
        auto *view = qobject_cast<ReverseListProxyModel *>(
            controller.timelineView());
        if (!view)
            return false;
        return QTest::qWaitFor([view] { return view->revealIdle(); },
                               kAnchorRowRevealTimeoutMs);
    }

    // Waits for the anchor's row to be built, then reads its offset once with
    // no grace period. Two reasons for "not yet" (row not built vs. reader
    // moved) are separated so a failure says which one it was, and only
    // construction is waited for, keeping "compensation is immediate" strict.
    //
    // These prepend cases guard the geometric identity that a backfill
    // prepend lands beyond the reader (older rows sit at higher y on the
    // rotated view); they do not exercise anchor compensation, which the diag*
    // cases cover.
    //
    // One layout flush is allowed: the Column assigns `y` in a polish pass, so
    // a freshly created delegate can briefly read y 0. `neededFlush` is
    // reported so routine reliance on it shows up.
    struct AnchorSettle {
        bool sawRow = false;
        bool neededFlush = false;
        bool rightRow = true;
        QString measuredId;
        QString wantedId;
        QString counters;
        double offset = 0;
        bool moved(double offsetBefore) const
        {
            return !sawRow || !rightRow
                || qAbs(offset - offsetBefore) >= 2.0;
        }
        QString detail(double offsetBefore) const
        {
            if (!sawRow)
                return QStringLiteral("the anchor's row was never built "
                                      "within %1 ms — a paced-reveal "
                                      "timeout, NOT a compensation failure")
                    .arg(kAnchorRowRevealTimeoutMs);
            if (!rightRow)
                return QStringLiteral("the row measured was %1, not the "
                                      "anchor %2 — a source-row/view-row "
                                      "MAPPING failure, NOT a compensation "
                                      "failure (offset read %3)")
                    .arg(measuredId.isEmpty() ? QStringLiteral("(none)")
                                              : measuredId)
                    .arg(wantedId).arg(offset);
            return QStringLiteral("viewport offset %1 -> %2, read %3 a "
                                  "layout flush; %4")
                .arg(offsetBefore)
                .arg(offset)
                .arg(neededFlush ? QStringLiteral("after")
                                 : QStringLiteral("without"))
                .arg(counters);
        }
    };
    // Checks the delegate's event id too: a view row is `count - 1 -
    // sourceRow - rowWindowSkip`, so a momentary count disagreement resolves
    // to another row's delegate, which would be misreported as the reader
    // moving.
    static AnchorSettle anchorOffsetOnceItsRowExists(QQuickItem *timeline,
                                                     int rowAfter,
                                                     double offsetBefore,
                                                     const QString &anchorId)
    {
        AnchorSettle settle;
        QQuickItem *item = nullptr;
        settle.sawRow = QTest::qWaitFor(
            [&] {
                item = itemForSourceRow(timeline, rowAfter);
                return item != nullptr;
            },
            kAnchorRowRevealTimeoutMs);
        if (!settle.sawRow)
            return settle;
        const auto read = [&] {
            settle.offset =
                item->y() - timeline->property("contentY").toDouble();
        };
        const auto identify = [&] {
            settle.wantedId = anchorId;
            QVariant out;
            const int viewRow = viewRowForSourceRow(timeline, rowAfter);
            if (viewRow >= 0
                && QMetaObject::invokeMethod(
                       timeline, "stableIdAtViewRow", Q_RETURN_ARG(QVariant, out),
                       Q_ARG(QVariant, QVariant(viewRow))))
                settle.measuredId = out.toString();
            settle.rightRow = settle.measuredId == anchorId;
        };
        // On failure, report the anchor counters a further anchor fix would
        // need, instead of leaving the next reader to reproduce it.
        const auto snapshot = [&] {
            static const char *kNames[] = {
                // Did the machinery correct anything?
                "diagAnchorCorrections", "diagGrowthCorrections",
                // Did it run and take an early return?
                "diagNoAnchorReturns", "diagStickToBottomReturns",
                // The prepend branch, which these cases exercise.
                "diagPrependFirings", "diagPrependOriginShiftSum",
                "diagPrependMaxAbsOriginShift",
                // The displaced/materialized branches and their magnitudes.
                "diagDisplacedFirings", "diagMaterializedFirings",
                "diagMaterializedMaxAbsDelta", "diagActiveDeferrals",
                "diagUnresolvedIdFallbacks", "diagEvictedNoInsertFallbacks",
                // originY tells "the row moved within the content" apart from
                // "the content origin moved under a stationary contentY".
                "originY", "contentY", "contentHeight", "rowWindowSkip",
            };
            QStringList parts;
            QStringList missing;
            for (const char *name : kNames) {
                const QVariant v = timeline->property(name);
                if (v.isValid())
                    parts << QStringLiteral("%1=%2").arg(
                        QString::fromLatin1(name).startsWith(QLatin1String("diag"))
                            ? QString::fromLatin1(name).mid(4)
                            : QString::fromLatin1(name),
                        v.toString());
                else
                    missing << QString::fromLatin1(name);
            }
            // A renamed property reads as UNREADABLE, not a silent zero.
            if (!missing.isEmpty())
                parts << QStringLiteral("UNREADABLE[%1]")
                             .arg(missing.join(QLatin1Char(',')));
            settle.counters = parts.join(QLatin1Char(' '));
        };
        read();
        identify();
        snapshot();
        if (settle.moved(offsetBefore)) {
            settle.neededFlush = true;
            if (QQuickWindow *window = timeline->window())
                window->requestUpdate();
            QTest::qWait(0);
            if (QQuickItem *again = itemForSourceRow(timeline, rowAfter))
                item = again;
            read();
            identify();
            snapshot();
        }
        return settle;
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

    // Park a source row at the viewport's physical top.
    static bool positionAtSourceRow(QQuickItem *timeline, int sourceRow)
    {
        const int viewRow = viewRowForSourceRow(timeline, sourceRow);
        if (viewRow < 0)
            return false;
        return QMetaObject::invokeMethod(timeline, "positionViewAtViewRow",
                                         Q_ARG(QVariant, QVariant(viewRow)),
                                         Q_ARG(QVariant, QVariant(false)));
    }

    // Park the reader at the top edge (the oldest loaded row, where near-top
    // backfill fires). On the rotated view that is wheelMaxY(). A direct
    // contentY write rather than goToEarliestLoaded(), which also re-runs
    // pagination and restarts the settle timer.
    static bool positionAtTopEdge(QQuickItem *timeline)
    {
        QVariant maxY;
        if (!QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                       Q_RETURN_ARG(QVariant, maxY)))
            return false;
        return timeline->setProperty("contentY", maxY.toDouble());
    }

    // The pane's own wheelMinY()/wheelMaxY(). A hand-rolled range narrower
    // than production's would clamp simulated glides against a floor the app
    // does not have.
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

    // Filters the one warning that belongs to the mock fixture: `mediaThumbUrl`
    // is a plain http URL there, so image rows make Qt fail to resolve
    // `mock.local`. Pinned to that host so a fixture reaching a real host still
    // fails.
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

    // Shared fixture: the real pane over `roomId` with an explicit event list
    // and pagination budget (for short, tall and zero-page rooms).
    static QQuickItem *paneWithEvents(AppController &controller,
                                      QQmlApplicationEngine &engine,
                                      QQuickWindow &window,
                                      const QString &roomId,
                                      const QList<TimelineEvent> &events,
                                      int paginationPages,
                                      int viewportHeight,
                                      QQuickItem **timelineOut)
    {
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return nullptr;
        controller.setCurrentRoomId(roomId);

        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(700, viewportHeight);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        // Exposure and activation, so window-context Shortcuts are delivered.
        // Not asserted: the offscreen platform may decline.
        QTest::qWaitForWindowExposed(&window, 2000);
        window.requestActivate();
        QCoreApplication::processEvents();

        mock->resetTimelineForTest(roomId, events, paginationPages);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!timeline)
            return nullptr;
        if (!QTest::qWaitFor([&] {
                return timeline->property("presentationReady").toBool();
            }, 5000))
            return nullptr;
        if (timelineOut)
            *timelineOut = timeline;
        return root;
    }

    // The pane root that owns `item`: the last QQuickItem ancestor (the top
    // QObject parent is the engine).
    static QQuickItem *paneRootOf(QQuickItem *item)
    {
        QQuickItem *root = item;
        while (auto *parent = qobject_cast<QQuickItem *>(root->parent()))
            root = parent;
        return root;
    }

    static QList<TimelineEvent> textFixture(const QString &roomId, int count,
                                            const QString &prefix,
                                            const QString &body,
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
            e.body = QStringLiteral("%1 %2").arg(body).arg(i);
            e.timestamp = base.addSecs(i * 30);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        return events;
    }

    static void sendWheelNotch(QQuickWindow &window, const QPointF &pos,
                               int angleY, bool inverted)
    {
        QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                          QPoint(0, 0), QPoint(0, angleY), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, inverted);
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

private Q_SLOTS:
    // The room-activity component materializes typed child rows, not just an
    // expansion flag.
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

    // The pane instantiates with no room open without a ReferenceError for
    // PaginationController (the header binding evaluates on load).
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
        // loadFromModule() may complete synchronously; only wait if needed.
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

    // With no room selected the Home surface shows and the composer is
    // hidden; selecting a room reverses both.
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

    // The pagination header expands while loading and collapses once the
    // batch completes, through the real object graph.
    void loadingStateExpandsHeaderThenCollapses()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        // "!general:mock.local" is seeded with 2 pages remaining, so the
        // request is accepted.
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

        // The pane may already be Loading on creation (it fills the viewport
        // for a short room), so assert the header tracks whichever state that
        // leaves.
        auto expectedHeight = [&] {
            return controller.pagination()->presentationState()
                           == PaginationController::Loading
                       ? 32.0
                       : 0.0;
        };
        QCOMPARE(header->height(), expectedHeight());
        QVERIFY(controller.pagination()->presentationState()
                != PaginationController::Failed);

        // Let every automatic batch resolve (the mock settles each ~300 ms
        // later; 2 seeded pages, so this terminates). The header height is
        // asserted too: it only re-evaluates on stateChanged(), which a C++
        // test cannot observe.
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.pagination()->presentationState(),
                                  PaginationController::Hidden, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 0.0, 5000);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The reader-list opener lives on the Flickable (`timelineView`), the only
    // pane object delegates can reach.
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

        // The opener exists on the view the delegates hold...
        const QVariant opener = timeline->property("openReceiptList");
        QVERIFY(opener.isValid());
        QVERIFY(qvariant_cast<QJSValue>(opener).isCallable());

        // ...and invoking it as the delegate does opens the shared popover.
        // First-open case: rows materialize after placement. With thirty
        // readers and a point at the bottom edge, the card must grow, cap at
        // half the window, scroll inside and stay within the window.
        auto *popover =
            root->findChild<QObject *>(QStringLiteral("receiptListPopover"));
        QVERIFY(popover != nullptr);
        QVERIFY(!popover->property("visible").toBool());
        QQmlExpression bottomCall(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "openReceiptList([{userId: \"@u0:mock.local\","
                " displayName: \"User 0\", avatarMxc: \"\","
                " tsMs: 1000}], 30,"
                " Qt.point(40, Overlay.overlay.height - 12))"));
        bottomCall.evaluate();
        QVERIFY2(!bottomCall.hasError(),
                 bottomCall.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(popover->property("visible").toBool(), 5000);
        QCOMPARE(popover->property("totalOthers").toInt(), 30);
        const qreal placedHeight = popover->property("height").toReal();
        // Grow in place after placement, as the first desktop open does; the
        // fixture materializes synchronously, so drive the growth explicitly.
        QQmlExpression grow(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "receiptListPopover.readers ="
                " Array.from({length: 30}, function(v, i) {"
                " return {userId: \"@u\" + i + \":mock.local\","
                " displayName: \"User \" + i, avatarMxc: \"\","
                " tsMs: 1000 + i}; })"));
        grow.evaluate();
        QVERIFY2(!grow.hasError(),
                 grow.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("height").toReal() > placedHeight + 40, 5000);
        const qreal manyHeight = popover->property("height").toReal();
        QVERIFY(manyHeight <= window.height() * 0.5 + 1.0);
        auto *readerList = popover->findChild<QQuickItem *>(
            QStringLiteral("receiptReaderList"));
        QVERIFY(readerList != nullptr);
        // Capped: the rows overflow and scroll inside.
        QTRY_VERIFY_WITH_TIMEOUT(
            readerList->property("contentHeight").toReal()
                > readerList->height() + 1.0, 5000);
        // Fully inside the window even though it grew after placement.
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("y").toReal()
                    + popover->property("height").toReal()
                <= window.height() + 0.5, 5000);

        // Reopened with one reader, the card hugs its single row (<140).
        QQmlExpression closeCall(qmlContext(timeline), timeline,
                                 QStringLiteral("receiptListPopover.close()"));
        closeCall.evaluate();
        QVERIFY(!closeCall.hasError());
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
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("height").toReal() < 140, 5000);
    }

    // A QQuickText is created with ItemObservesViewport and clears it only
    // after laying out non-empty text, so an empty Label observes the viewport
    // forever and makes every contentY change walk the whole item tree. The
    // timeline's rows must contain no such observers.
    void timelineRowsCarryNoPermanentViewportObservers()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

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

        // A mixed timeline covering every row kind that had an empty-text
        // Label: plain, own (meta label), date divider and read marker.
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QList<TimelineEvent> events;
        for (int i = 0; i < 40; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$vp%1").arg(i);
            e.itemId = QStringLiteral("uid-vp%1").arg(i);
            e.roomId = roomId;
            e.sender = (i % 5 == 0)
                           ? controller.settings()->userId()
                           : QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("viewport observer probe %1").arg(i);
            e.timestamp = base.addSecs(i * 60);
            events.append(e);
        }
        // A live thread card with no latest timestamp: its timeLabel() is ""
        // in exactly that state, inside an active Loader.
        TimelineEvent threadRoot;
        threadRoot.eventId = QStringLiteral("$vp-threadroot");
        threadRoot.itemId = QStringLiteral("uid-vp-threadroot");
        threadRoot.roomId = roomId;
        threadRoot.sender = QStringLiteral("@alice:mock.local");
        threadRoot.senderDisplayName = QStringLiteral("Alice");
        threadRoot.body = QStringLiteral("thread root with no latest ts");
        threadRoot.timestamp = base.addSecs(2500);
        threadRoot.isThreadRoot = true;
        threadRoot.threadReplyCount = 3;
        // threadLatestTimestamp deliberately left invalid.
        events.append(threadRoot);
        // An edited row and an ambiguous-name row, so the meta label and the
        // disambiguator are walked materialized.
        TimelineEvent editedRow;
        editedRow.eventId = QStringLiteral("$vp-edited");
        editedRow.itemId = QStringLiteral("uid-vp-edited");
        editedRow.roomId = roomId;
        editedRow.sender = QStringLiteral("@bob:mock.local");
        editedRow.senderDisplayName = QStringLiteral("Bob");
        editedRow.body = QStringLiteral("an edited message");
        editedRow.timestamp = base.addSecs(2560);
        editedRow.edited = true;
        events.append(editedRow);
        TimelineEvent ambiguous;
        ambiguous.eventId = QStringLiteral("$vp-ambiguous");
        ambiguous.itemId = QStringLiteral("uid-vp-ambiguous");
        ambiguous.roomId = roomId;
        ambiguous.sender = QStringLiteral("@bob2:mock.local");
        ambiguous.senderDisplayName = QStringLiteral("Bob");
        ambiguous.senderNameAmbiguous = true;
        ambiguous.body = QStringLiteral("same display name as Bob");
        ambiguous.timestamp = base.addSecs(2620);
        events.append(ambiguous);
        TimelineEvent divider;
        divider.itemId = QStringLiteral("uid-vp-divider");
        divider.roomId = roomId;
        divider.type = TimelineEvent::DateDivider;
        divider.timestamp = base;
        events.append(divider);
        TimelineEvent marker;
        marker.itemId = QStringLiteral("uid-vp-marker");
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 20,
                                 5000);

        int totalItems = 0;
        QStringList observers;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            ++totalItems;
            if (item->flags() & QQuickItem::ItemObservesViewport) {
                QStringList chain;
                const QQuickItem *up = item;
                for (int d = 0; d < 6 && up; ++d) {
                    QString seg = QString::fromLatin1(
                                      up->metaObject()->className())
                                      .section(QLatin1Char('_'), 0, 0);
                    if (!up->objectName().isEmpty())
                        seg += QLatin1Char('#') + up->objectName();
                    chain.prepend(seg);
                    up = up->parentItem();
                }
                observers << QStringLiteral("[vis=%1 text='%2'] %3")
                                 .arg(item->isVisible())
                                 .arg(item->property("text").toString()
                                          .left(20))
                                 .arg(chain.join(QStringLiteral(" > ")));
            }
            const auto children = item->childItems();
            for (QQuickItem *c : children)
                walk(c);
        };
        walk(timeline);

        // The Labels became Loaders; prove they still materialize, or "zero
        // observers" could be satisfied by deleting the UI.
        const auto names =
            findVisualChildren(timeline, QStringLiteral("senderName"));
        QVERIFY2(!names.isEmpty(), "no senderName Label was created at all");
        int visibleNamed = 0;
        for (QQuickItem *n : names) {
            if (n->isVisible()
                && n->property("text").toString() == QStringLiteral("Alice"))
                ++visibleNamed;
        }
        QVERIFY2(visibleNamed > 0,
                 "the identity header Loader produced no visible sender name");
        // ...and the identity header is a real, sized item.
        const auto headers =
            findVisualChildren(timeline, QStringLiteral("senderIdentityHeader"));
        QVERIFY(!headers.isEmpty());
        QQuickItem *header = headers.first();
        QVERIFY2(header->width() > 0 && header->height() > 0,
                 qPrintable(QStringLiteral("header collapsed: %1x%2")
                                .arg(header->width()).arg(header->height())));

        QVERIFY2(totalItems > 200,
                 qPrintable(QStringLiteral("only %1 items instantiated — the "
                                           "fixture did not build real rows")
                                .arg(totalItems)));
        QVERIFY2(observers.isEmpty(),
                 qPrintable(QStringLiteral("%1 permanent viewport observers:\n%2")
                                .arg(observers.size())
                                .arg(observers.join(QStringLiteral("\n")))));
    }

    // Jump-to-latest glides from nearby (motion engine engaged, follow-latest
    // arrival pending) and lands immediately from far away.
    void jumpToLatestGlidesFromNearbyAndJumpsFromFar()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

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
        window.resize(700, 400);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        // Enough rows that the content is many viewports tall.
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-9000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 220; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$jump%1").arg(i);
            e.itemId = QStringLiteral("uid-jump%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("jump probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 200,
                                 5000);

        const qreal viewportHeight = timeline->property("height").toReal();
        QVERIFY(viewportHeight > 0);
        const int smoothViewports =
            timeline->property("smoothJumpViewports").toInt();
        QVERIFY(smoothViewports > 0);
        QQmlExpression minY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMinY()"));
        const qreal bottomY = minY.evaluate().toReal();
        QVERIFY(!minY.hasError());
        QQmlExpression maxY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMaxY()"));
        const qreal topY = maxY.evaluate().toReal();
        QVERIFY(!maxY.hasError());
        // The content must be taller than the smooth threshold, or the "far"
        // half is vacuous.
        QVERIFY2(topY - bottomY > viewportHeight * (smoothViewports + 1),
                 qPrintable(QStringLiteral("content only %1px for a %2px "
                                           "threshold")
                                .arg(topY - bottomY)
                                .arg(viewportHeight * smoothViewports)));

        // Near: one viewport up. Leave follow-latest first, or the bottom pin
        // undoes the position before goToLatest() reads it.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty("contentY", bottomY + viewportHeight);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toReal() > bottomY + 1.0, 5000);
        QQmlExpression jumpNear(qmlContext(timeline), timeline,
                                QStringLiteral("goToLatest()"));
        jumpNear.evaluate();
        QVERIFY2(!jumpNear.hasError(),
                 jumpNear.error().toString().toUtf8().constData());
        QVERIFY(controller.timelineScroll()->motionActive());
        QVERIFY(timeline->property("followLatestOnArrival").toBool());
        // ...and it arrives and re-pins without a teleport.
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.timelineScroll()->motionActive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                 5000);
        QVERIFY(!timeline->property("followLatestOnArrival").toBool());

        // Far: beyond the threshold, no glide.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty(
            "contentY",
            bottomY + viewportHeight * (smoothViewports + 0.5));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toReal()
                > bottomY + viewportHeight * smoothViewports, 5000);
        QQmlExpression jumpFar(qmlContext(timeline), timeline,
                               QStringLiteral("goToLatest()"));
        jumpFar.evaluate();
        QVERIFY2(!jumpFar.hasError(),
                 jumpFar.error().toString().toUtf8().constData());
        QVERIFY(!controller.timelineScroll()->motionActive());
        QVERIFY(!timeline->property("followLatestOnArrival").toBool());
        QVERIFY(timeline->property("stickToBottom").toBool());
    }

    // The row window bounds instantiated rows around the reader; releasing
    // rows must never move the message the reader is looking at.
    //
    // Builds a pane over `rows` plain rows with the reader parked deep in
    // history. Returns the timeline item; the caller owns the lifetimes.
    static QQuickItem *deepHistoryPane(AppController &controller,
                                       QQmlApplicationEngine &engine,
                                       QQuickWindow &window,
                                       int rows,
                                       double parkFraction,
                                       int viewportHeight = 420)
    {
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        if (roomId.isEmpty())
            return nullptr;
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return nullptr;
        controller.setCurrentRoomId(roomId);

        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(700, viewportHeight);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-40000);
        QList<TimelineEvent> events;
        for (int i = 0; i < rows; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$win%1").arg(i);
            e.itemId = QStringLiteral("uid-win%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("window probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!timeline)
            return nullptr;
        if (!QTest::qWaitFor([&] {
                return timeline->property("presentationReady").toBool()
                       && timeline->property("count").toInt() > rows - 100;
            }, 5000))
            return nullptr;

        timeline->setProperty("stickToBottom", false);
        double minY = 0.0;
        double maxY = 0.0;
        if (!wheelBounds(timeline, &minY, &maxY))
            return nullptr;
        timeline->setProperty("contentY", maxY * parkFraction);
        QCoreApplication::processEvents();
        return timeline;
    }

    void rowWindowBoundsRowsWithoutMovingTheReadersMessage()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

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
        window.resize(700, 420);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        // More rows than the window keeps, so a release is required.
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-40000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 900; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$win%1").arg(i);
            e.itemId = QStringLiteral("uid-win%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("window probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 800,
                                 5000);
        const int rowsBefore = timeline->property("count").toInt();

        // Park the reader deep in history.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QQmlExpression maxY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMaxY()"));
        const qreal topY = maxY.evaluate().toReal();
        QVERIFY(!maxY.hasError());
        timeline->setProperty("contentY", topY * 0.6);
        QCoreApplication::processEvents();

        // The reader's message and its screen position. The probe recomputes
        // the visible range itself, since the pane's update is timer-driven.

        QQmlExpression probe(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0]; })()"));
        const QVariantList before = probe.evaluate().toList();
        QVERIFY2(!probe.hasError(),
                 probe.error().toString().toUtf8().constData());
        const QString anchorId = before.value(0).toString();
        const qreal anchorOffset = before.value(1).toReal();
        QVERIFY2(!anchorId.isEmpty(), "no anchor event under the viewport");

        // Apply the window as the settle timer does.
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        // (1) Rows are bounded.
        const int rowsAfter = timeline->property("count").toInt();
        QVERIFY2(rowsAfter < rowsBefore,
                 qPrintable(QStringLiteral("no rows released: %1 -> %2")
                                .arg(rowsBefore).arg(rowsAfter)));
        QVERIFY2(rowsAfter <= 520,
                 qPrintable(QStringLiteral("window too loose: %1 rows")
                                .arg(rowsAfter)));

        // (2) The reader's message did not move on screen.
        QQmlExpression after(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + anchorId + QStringLiteral("')"));
        const qreal afterOffset = after.evaluate().toReal();
        QVERIFY2(!after.hasError(),
                 after.error().toString().toUtf8().constData());
        QVERIFY2(qAbs(afterOffset - anchorOffset) <= 2.0,
                 qPrintable(QStringLiteral("reader moved %1 px (%2 -> %3)")
                                .arg(afterOffset - anchorOffset)
                                .arg(anchorOffset).arg(afterOffset)));

        // (3) A window hiding the live edge never claims "at bottom", or
        //     follow-latest would latch to a false newest message.
        const int skipAfter = timeline->property("rowWindowSkip").toInt();
        QVERIFY2(skipAfter > 0,
                 "the window only trimmed the oldest end — the skip path, "
                 "which is the one that shifts the reader, was never taken");
        QQmlExpression atBottom(qmlContext(timeline), timeline,
                                QStringLiteral("atBottomEdge()"));
        QCOMPARE(atBottom.evaluate().toBool(), false);
        QVERIFY(!timeline->property("stickToBottom").toBool());

        // (4) Restore direction: move toward the live edge and re-apply. The
        //     skip comes down and the reader still does not move.
        QQmlExpression probe2(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ contentY = wheelMinY() + 40;"
                           " updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0]; })()"));
        const QVariantList back = probe2.evaluate().toList();
        QVERIFY2(!probe2.hasError(),
                 probe2.error().toString().toUtf8().constData());
        const QString backId = back.value(0).toString();
        const qreal backOffset = back.value(1).toReal();
        QVERIFY(!backId.isEmpty());
        QCoreApplication::processEvents();

        QQmlExpression apply2(qmlContext(timeline), timeline,
                              QStringLiteral("applyRowWindow()"));
        apply2.evaluate();
        QVERIFY2(!apply2.hasError(),
                 apply2.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        QVERIFY2(timeline->property("rowWindowSkip").toInt() < skipAfter,
                 qPrintable(QStringLiteral("skip did not come back down: %1")
                                .arg(timeline->property("rowWindowSkip")
                                         .toInt())));
        QQmlExpression after2(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + backId + QStringLiteral("')"));
        const qreal after2Offset = after2.evaluate().toReal();
        QVERIFY2(!after2.hasError(),
                 after2.error().toString().toUtf8().constData());
        QVERIFY2(qAbs(after2Offset - backOffset) <= 2.0,
                 qPrintable(QStringLiteral("reader moved %1 px on restore "
                                           "(%2 -> %3)")
                                .arg(after2Offset - backOffset)
                                .arg(backOffset).arg(after2Offset)));
        // The offscreen harness lays restored rows out before this reads
        // them, so it cannot reproduce the unmeasured-row hazard the flush in
        // applyRowWindow() guards against; a pass here does not prove absence.
        QCOMPARE(timeline->property("diagWindowUnmeasuredRows").toInt(), 0);
    }

    // Trimming the oldest end shrinks wheelMaxY() and so moves the reader's
    // measured distance from the top without any visible movement. The trim
    // must not leave the reader inside the near-top band, or the follow-up
    // checkNearTopEdge() refetches exactly what was released.
    void rowWindowTrimNeverFeedsTheNearTopPaginationBand()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        // A tall viewport on purpose: the window's kept margin is a fixed
        // ~4020 px while the enter band is 2.5 viewports, so the guard is only
        // reachable above ~1600 px. 2160 approximates a 4K fullscreen window.
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6, 2160);
        QVERIFY(timeline != nullptr);
        const int rowsBefore = timeline->property("count").toInt();

        // Arm the latch so a dispatch is possible.
        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        QVERIFY2(timeline->property("count").toInt() < rowsBefore,
                 "nothing was released, so the hazard was never exercised");
        // A trim proceeds only if it leaves the reader outside the enter band;
        // the exit distance (3.25 viewports) is wider than enter (2.5).
        QQmlExpression fromTop(
            qmlContext(timeline), timeline,
            QStringLiteral("[distanceFromTop(), nearTopEnterDistance]"));
        const QVariantList d = fromTop.evaluate().toList();
        QVERIFY2(!fromTop.hasError(),
                 fromTop.error().toString().toUtf8().constData());
        QVERIFY2(d.value(0).toReal() > d.value(1).toReal(),
                 qPrintable(QStringLiteral("trim left the reader INSIDE the "
                                           "near-top band: %1 <= %2")
                                .arg(d.value(0).toReal())
                                .arg(d.value(1).toReal())));
        // Not asserting on nearTopArmed: the dispatch is coalesced to the next
        // turn and the latch reads true with or without the guard.
    }

    // With a window active, jump-to-latest restores the live edge first rather
    // than landing on the window's synthetic newest row.
    void jumpToLatestRestoresTheLiveEdgeWhenAWindowIsActive()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QVERIFY2(timeline->property("rowWindowSkip").toInt() > 0,
                 "no window was established, so the bug is unreachable here");

        QQmlExpression jump(qmlContext(timeline), timeline,
                            QStringLiteral("goToLatest()"));
        jump.evaluate();
        QVERIFY2(!jump.hasError(),
                 jump.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        // The mock has no event cache, so the history trim refuses and this
        // falls through to settleAtLatest(), which must restore the live edge.
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
        QVERIFY(timeline->property("stickToBottom").toBool());
        QQmlExpression newest(
            qmlContext(timeline), timeline,
            QStringLiteral("[eventIdAtViewRow(0), atBottomEdge()]"));
        const QVariantList landed = newest.evaluate().toList();
        QVERIFY2(!newest.hasError(),
                 newest.error().toString().toUtf8().constData());
        QCOMPARE(landed.value(0).toString(), QStringLiteral("$win899"));
        QCOMPARE(landed.value(1).toBool(), true);
    }

    // A row whose height settles late after a window restore (link previews,
    // media without dimensions) is absorbed by the contentHeight anchor
    // mechanism the window hands off to.
    void lateHeightChangeAfterAWindowRestoreIsAbsorbedByTheAnchor()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QVERIFY(timeline->property("rowWindowSkip").toInt() > 0);

        // The reader's position, and a newer row (physically below) whose
        // growth pushes the reader's row.
        QQmlExpression probe(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0, r]; })()"));
        const QVariantList before = probe.evaluate().toList();
        QVERIFY2(!probe.hasError(),
                 probe.error().toString().toUtf8().constData());
        const QString anchorId = before.value(0).toString();
        const qreal anchorOffset = before.value(1).toReal();
        const int anchorRow = before.value(2).toInt();
        QVERIFY(!anchorId.isEmpty());
        QVERIFY2(anchorRow > 4, "reader too close to the newest row to have "
                                "a newer row to grow");

        QQmlExpression grow(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ var it = itemAtViewRow(%1);"
                           " if (!it) return false;"
                           " it.height = it.height + 140; return true; })()")
                .arg(anchorRow - 3));
        QVERIFY2(grow.evaluate().toBool(), "could not grow a newer row");

        QQmlExpression after(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + anchorId + QStringLiteral("')"));
        // The correction is coalesced on a timer.
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(after.evaluate().toReal() - anchorOffset) <= 2.0, 3000);
    }

    // At the window's oldest exposed row, the pane re-exposes rows it already
    // holds instead of asking the homeserver for older history.
    void reachingTheWindowsOldEdgeExposesLocalRowsInsteadOfAskingTheServer()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        const int skip = timeline->property("rowWindowSkip").toInt();
        const int exposed = timeline->property("count").toInt();
        const int sourceTotal = controller.timeline()->rowCount();
        QVERIFY2(skip > 0, "no window established");
        QVERIFY2(skip + exposed < sourceTotal,
                 qPrintable(QStringLiteral(
                     "the window holds nothing back at the OLD end "
                     "(skip %1 + exposed %2 vs total %3), so the boundary "
                     "this test is about does not exist here")
                                .arg(skip).arg(exposed).arg(sourceTotal)));

        // Drive the reader to the window's oldest row and run the near-top
        // check as a gesture does.
        QQmlExpression toEdge(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ contentY = wheelMaxY();"
                           " nearTopArmed = true;"
                           " nearTopRequestDistance = Infinity;"
                           " checkNearTopEdge(true); return true; })()"));
        QVERIFY(toEdge.evaluate().toBool());
        QVERIFY2(!toEdge.hasError(),
                 toEdge.error().toString().toUtf8().constData());

        // Paced reveal. The source total must not change: no server page was
        // consumed.
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("count").toInt() > exposed, 4000);
        QCOMPARE(controller.timeline()->rowCount(), sourceTotal);
        // The newest end must not move: this path does no contentY correction.
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), skip);
    }

    // End to end through the real trigger: real wheel notches deep into
    // history, then nothing. The settle timer, fed by the pane's own
    // visible-row tracking, must apply the window by itself.
    void wheelScrollingIntoHistoryEventuallyBoundsRowsThroughTheSettleTimer()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.0);
        QVERIFY(timeline != nullptr);
        const int rowsBefore = timeline->property("count").toInt();
        QVERIFY(rowsBefore > 800);

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        const qreal startY = timeline->property("contentY").toReal();
        for (int i = 0; i < 240; ++i) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        }
        const qreal deepY = timeline->property("contentY").toReal();
        QVERIFY2(deepY > startY + 2000,
                 qPrintable(QStringLiteral(
                     "the wheel notches did not travel into history "
                     "(contentY %1 -> %2); this test proves nothing")
                                .arg(startY).arg(deepY)));

        // Do nothing; the settle timer is 250 ms.
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("rowWindowSkip").toInt() > 0, 5000);
        QVERIFY2(timeline->property("count").toInt() < rowsBefore,
                 qPrintable(QStringLiteral(
                     "a window was established but bounded nothing: %1 rows")
                                .arg(timeline->property("count").toInt())));
    }

    // `speculativeMediaAllowed` is false while the reader's input owns the
    // viewport and true again once it settles, so rows sweeping past do not
    // arm full-payload prefetches. Thumbnails are not gated.
    void speculativeMediaIsBlockedDuringAGestureAndResumesOnSettle()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

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
        window.resize(700, 420);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-6000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 160; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$spec%1").arg(i);
            e.itemId = QStringLiteral("uid-spec%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("speculative probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 100,
                                 5000);

        // Settled: speculative media is allowed.
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("speculativeMediaAllowed").toBool(), 5000);

        // During real wheel notches the gate is shut: a row sweeping past is
        // not worth a full-payload prefetch.
        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        bool blockedDuringGesture = false;
        for (int i = 0; i < 12; ++i) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, -120),
                              Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            if (!timeline->property("speculativeMediaAllowed").toBool())
                blockedDuringGesture = true;
        }
        QVERIFY2(blockedDuringGesture,
                 "speculative media was never gated during a wheel gesture");

        // ...and it reopens, or rows the reader stops on never get media.
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("speculativeMediaAllowed").toBool(), 5000);
    }

    // The history trim's refusal policy, every clause. The trim itself (an
    // explicit jump-to-live that rebuilds at the live edge, like Element's)
    // is only reachable on the Rust backend and is not covered offline.
    void historyTrimPolicyRefusesEveryUnsafeCombination()
    {
        const int rows = 500;
        const int threshold = 400;
        // The one combination that may proceed.
        QVERIFY(AppController::historyTrimAllowed(
            /*rustBackend=*/true, /*roomOpen=*/true, /*paginationBusy=*/false,
            /*threadOpen=*/false, rows, threshold));
        // ...and each clause on its own must veto it.
        QVERIFY(!AppController::historyTrimAllowed(false, true, false, false,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, false, false, false,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, true, true, false,
                                                  rows, threshold));
        // An open thread panel holds its own event-cache subscriber; the SDK
        // could not shrink and the reload would break that subscription.
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, true,
                                                  rows, threshold));
        // The threshold is exclusive: exactly-at is not "more than".
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, false,
                                                  threshold, threshold));
        QVERIFY(AppController::historyTrimAllowed(true, true, false, false,
                                                 threshold + 1, threshold));
        // A short timeline is never worth a reset.
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, false,
                                                  12, threshold));
    }

    // On a backend with no event cache the trim refuses, and a refused trim is
    // a complete no-op: the far jump still lands at the newest row.
    void jumpToLiveTrimIsRefusedByABackendWithNoEventCacheAndIsThenANoOp()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        // One value, read by both QML and this test.
        QVERIFY(controller.historyTrimRowThreshold() > 0);

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
        window.resize(700, 400);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-9000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 220; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$trim%1").arg(i);
            e.itemId = QStringLiteral("uid-trim%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("trim probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 200,
                                 5000);
        const int rowsBefore = controller.timeline()->rowCount();
        QVERIFY(rowsBefore > 200);

        // Refuses on a backend with no event cache. The no-room call refuses
        // for the same reason here; it only pins that a missing room never
        // dispatches.
        QVERIFY(!controller.trimHistoryAndJumpToLive());
        const QString openRoom = controller.currentRoomId();
        controller.setCurrentRoomId(QString());
        QVERIFY(!controller.trimHistoryAndJumpToLive());
        controller.setCurrentRoomId(openRoom);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.timeline()->rowCount() > 0, 5000);

        // A refused trim leaves the timeline untouched, and the far jump still
        // lands at the newest row.
        const int smoothViewports =
            timeline->property("smoothJumpViewports").toInt();
        const qreal viewportHeight = timeline->property("height").toReal();
        QQmlExpression minY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMinY()"));
        const qreal bottomY = minY.evaluate().toReal();
        QVERIFY(!minY.hasError());
        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty(
            "contentY",
            bottomY + viewportHeight * (smoothViewports + 0.5));
        QCoreApplication::processEvents();
        QQmlExpression jumpFar(qmlContext(timeline), timeline,
                               QStringLiteral("goToLatest()"));
        jumpFar.evaluate();
        QVERIFY2(!jumpFar.hasError(),
                 jumpFar.error().toString().toUtf8().constData());
        QVERIFY(timeline->property("stickToBottom").toBool());
        QCOMPARE(controller.timeline()->rowCount(), rowsBefore);
    }

    // Header height changes move contentHeight; viewport-fill checks triggered
    // from that geometry must be queued and coalesced, settling through
    // initial fill, loading, reached-start and resizes without a binding loop
    // or a request storm. Failed/Retry are covered by PaginationControllerTest.
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
                        // The offscreen host cannot resolve the mock media
                        // origin; the image fixture row's fetch logs this.
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
        // Request the second page explicitly: headless delegates are not
        // polished, so contentHeight never asks for it on its own.
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

    // A transient first viewport-fill failure retries internally through the
    // real pane/controller path, without calling retry() from the test.
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

    // A populated encrypted timeline with a long (>4K) decrypted body stays
    // responsive through resize and room switches: a delegate created at a
    // transient 1 px text width grew huge and churned the view forever.
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

    // The offscreen QPA does not polish list delegates, so create the real
    // MessageDelegate directly with the real role schema. Completion happens
    // at width zero, the phase that measured the long body at 1 px.
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

    // Reaction chips share one height regardless of the emoji they hold: the
    // labels are pinned to a fixed content height so glyphs with divergent
    // font metrics cannot grow a chip. Compares sibling chips, not a constant.
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
        // Emoji shapes whose font metrics tend to diverge: single codepoint,
        // base+VS16, ZWJ sequence, skin-tone modifier, keycap and flag.
        fixture.insert(QStringLiteral("reactions"), QVariantList{
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("👍") },
                        { QStringLiteral("count"), 1 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"),
                          QStringLiteral("❤️") },
                        { QStringLiteral("count"), 12 },
                        { QStringLiteral("byMe"), true } },
            QVariantMap{ { QStringLiteral("key"),
                          QStringLiteral("👨‍👩‍👧‍👦") },
                        { QStringLiteral("count"), 2 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("👍🏽") },
                        { QStringLiteral("count"), 3 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("1️⃣") },
                        { QStringLiteral("count"), 4 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("🇱🇹") },
                        { QStringLiteral("count"), 5 },
                        { QStringLiteral("byMe"), false } },
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

        // findChildren() cannot see Repeater delegates; walk the item tree.
        const auto chips = findVisualChildren(root, QStringLiteral("reactionChip"));
        QCOMPARE(chips.size(), 6);
        // 20 px floor, matching Element's pill.
        QVERIFY2(chips.at(0)->height() >= 20.0,
                 "chip height below the 20px design floor");
        for (int i = 1; i < chips.size(); ++i) {
            QVERIFY2(qFuzzyCompare(chips.at(i)->height() + 1.0,
                                   chips.at(0)->height() + 1.0),
                     qPrintable(QStringLiteral("chip %1 is %2px tall against "
                                               "%3px for the first — a glyph "
                                               "grew its pill")
                                    .arg(i)
                                    .arg(chips.at(i)->height())
                                    .arg(chips.at(0)->height())));
        }

        // Vertical glyph alignment is not covered: this environment's fonts
        // give identical metrics for all six keys, so an assertion would pass
        // on broken code. The fixed line box needs checking on a real desktop.
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

    // resetForReuse() scrubs every transient, non-model field so a pooled
    // delegate never carries a stale popup target or dialog body to the next
    // message.
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

        // Stale transient state as if the previous row had an open context
        // menu and an inspected details payload.
        QVERIFY(root->setProperty("menuEventId",
                                  QStringLiteral("$stale-menu:mock.local")));
        QCOMPARE(root->property("menuEventId").toString(),
                 QStringLiteral("$stale-menu:mock.local"));
        QVERIFY(!root->property("reactionEventId").isValid());

        // Simulate the pool handing this delegate to a new row.
        QVERIFY(QMetaObject::invokeMethod(root, "resetForReuse"));

        QCOMPARE(root->property("menuEventId").toString(), QString{});
        // No warnings: resetForReuse() resolved every id it touches.
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
        // Delegate layout completes asynchronously offscreen; wait for it.
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

    // The Settings wheel-speed control reflects and updates the persisted
    // value, and the setting drives the shared TimelineScrollController.
    void settingsControlTracksWheelSpeedPreference()
    {
        AppController controller(AppController::MockBackend);
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        // QSettings persists across runs, so normalise to the default (Fast).
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
        // Fast (value 1) is index 1; the ComboBox resolves its index on
        // completion.
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
        // Restore the persisted default for other runs.
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

    // The timeline wheel handler exists, is scoped to the timeline view, and
    // reaches the shared TimelineScrollController without warnings.
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
        // The handler lives inside the timeline's own subtree, so wheel input
        // elsewhere cannot move the timeline.
        QObject *handler = timeline->findChild<QObject *>(
            QStringLiteral("timelineWheelHandler"));
        QVERIFY(handler != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Upward wheel motion via beginWheelTo() leaves follow-latest at once.
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
        // Physically upward (older history) increases contentY on the rotated
        // timeline, as keyboardPage() does.
        QVERIFY(QMetaObject::invokeMethod(timeline, "beginWheelTo",
                                          Q_ARG(QVariant, startY + 200.0)));
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Jump to latest cancels an in-flight coalesced wheel motion.
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

    // Switching rooms cancels the previous room's wheel motion.
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

    // A real discrete wheel event over the pane is routed through the
    // timeline WheelHandler into TimelineScrollController. Asserts the side
    // effects (motion engaged, follow-latest left), not pixel distances.
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

        // One +120 notch upward over the viewport centre, resent until it
        // registers (offscreen wheel delivery is not guaranteed in one pass).
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
        // Ignore the benign mock.local DNS warning from image rows.
        warnings.removeIf([](const QString &w) {
            return w.contains(QStringLiteral("Host mock.local not found"));
        });
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // A touchpad (pixelDelta) upward scroll leaves follow-latest like the
    // mouse wheel does; otherwise the next content-height change snaps the
    // reader back to the newest message.
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

        // Touchpad: pixelDelta upward, no angle notch, scroll phase set.
        // Resent until it registers (offscreen delivery is not guaranteed).
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

    // Slow Wayland touchpad swipes are mostly `pixelDelta 0, angleDelta ±1,
    // ScrollUpdate` frames (Qt rounds to whole pixels). Those must move
    // nothing and never take the notch branch, while a phase-less notch (a
    // real mouse wheel, also labelled TouchPad on Wayland) keeps its glide.
    // The branch is read from the trace counters.
    void touchpadZeroPixelFramesNeverEngageTheNotchGlide()
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
        QVERIFY(timeline->setProperty("stickToBottom", false));

        const QPointF pos(320, 300);
        auto send = [&](QPoint pixel, QPoint angle, Qt::ScrollPhase phase) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()), pixel,
                              angle, Qt::NoButton, Qt::NoModifier, phase,
                              /*inverted=*/false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        auto counter = [&](const char *name) {
            return timeline->property(name).toInt();
        };

        // Zero-pixel frames of a phased (continuous) gesture, both directions.
        const double before = timeline->property("contentY").toDouble();
        for (int attempt = 0; attempt < 50 && counter("diagEvents") < 6;
             ++attempt) {
            send(QPoint(0, 0), QPoint(0, 1), Qt::ScrollUpdate);
            send(QPoint(0, 0), QPoint(0, -1), Qt::ScrollUpdate);
            if (counter("diagEvents") < 6)
                QTest::qWait(10);
        }
        QVERIFY2(counter("diagEvents") >= 6,
                 "the fixture never delivered a wheel event to the handler");
        QCOMPARE(counter("diagAngleEvents"), 0);
        QCOMPARE(counter("diagPixelEvents"), counter("diagEvents"));
        QCOMPARE(controller.timelineScroll()->motionActive(), false);
        QCOMPARE(timeline->property("contentY").toDouble(), before);

        // Control: a phase-less notch still takes the notch branch, so the
        // assertions above are not vacuous.
        const int anglesBefore = counter("diagAngleEvents");
        for (int attempt = 0;
             attempt < 50 && counter("diagAngleEvents") == anglesBefore;
             ++attempt) {
            send(QPoint(0, 0), QPoint(0, 120), Qt::NoScrollPhase);
            if (counter("diagAngleEvents") == anglesBefore)
                QTest::qWait(10);
        }
        QVERIFY2(counter("diagAngleEvents") > anglesBefore,
                 "a phase-less wheel notch must still take the notch branch");
    }

    // A touchpad gesture opens a scroll session (userScrollActive) that gates
    // the absolute anchor restore, and the session clears ~250 ms after input
    // stops. QML's WheelEvent has no phase or device type, so the session is
    // inferred from the settle timer. Relative growth corrections still run
    // mid-gesture (see the growth tests).
    void touchpadGestureOpensScrollSessionThatGatesCorrections()
    {
        // Enable the per-gesture diagnostics (read at controller construction)
        // so diagAnchorCorrections, the absolute-restore count, is observable.
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

        // Touchpad pixel deltas as KDE Wayland delivers them, resent until the
        // session opens.
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

        // No anchor captured yet, so maintainViewAnchor() returns early and
        // contentY is untouched. Mid-gesture growth correction is covered by
        // maintainViewAnchorDefersGrowthDeltaMidGestureWithoutGlide.
        const double before = timeline->property("contentY").toDouble();
        QMetaObject::invokeMethod(timeline, "maintainViewAnchor");
        QCOMPARE(timeline->property("contentY").toDouble(), before);

        // More deltas keep the session open while hydration churns
        // contentHeight.
        for (int i = 0; i < 5; ++i)
            sendDelta();
        QVERIFY(timeline->property("userScrollActive").toBool());

        // Upward intent left follow-latest.
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        // The session clears once input stops, so deferred anchor maintenance
        // can resume.
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 3000);

        // No absolute restore while the gesture owned the view (relative
        // corrections are counted separately in diagGrowthCorrections). No
        // warning check: scrolling through incubating delegates can log
        // transient offscreen binding warnings.
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    // After scrolling up, content growth (appends and late delegate heights)
    // never re-pins to the bottom: follow-latest is latched to user intent.
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
        // Several appends exercise onCountChanged and the coalesced
        // onContentHeightChanged; none may re-pin the reader.
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

    // Keyboard navigation. Delegate incubation does not run offscreen, so
    // these assert key routing and follow-latest/motion side effects.
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

        // Home is programmatic navigation like End: it bypasses the motion
        // engine and lands on the earliest loaded position.
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QTest::keyClick(&window, Qt::Key_Home, Qt::NoModifier);
        QVERIFY2(!timeline->property("wheelAnimating").toBool(),
                 "Home must jump instantly, not start wheel motion");
        // The earliest end is wheelMaxY() on the rotated timeline. Pagination
        // may move it asynchronously, so repeat the jump and read contentY and
        // wheelMaxY in the same event-loop turn.
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

        // With the composer focused, End must not resume follow-latest and no
        // motion may start.
        QTest::keyClick(&window, Qt::Key_End);
        QTest::keyClick(&window, Qt::Key_PageUp);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The find bar is a detached floating card but stays a Layout child, so
    // opening or closing it only resizes the timeline and never moves the
    // composer. Kept next to the other window-showing test: placing one
    // directly before keyboardNavigationKeysStartTimelineMotion() triggers an
    // offscreen-QPA flake.
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

        // Inset from both pane edges.
        const QPointF findBarTopLeft = findBar->mapToScene(QPointF(0, 0));
        const QPointF findBarTopRight =
            findBar->mapToScene(QPointF(findBar->width(), 0));
        QVERIFY2(findBarTopLeft.x() > 0.0,
                 "find bar must not touch the pane's left edge");
        QVERIFY2(findBarTopRight.x() < root->width(),
                 "find bar must not touch the pane's right edge");

        // Opening find shrinks only the timeline; the composer does not move.
        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);

        auto *findField = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(findField != nullptr);
        QVERIFY(findField->hasActiveFocus());

        QVERIFY(QMetaObject::invokeMethod(root, "closeFind"));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(!findBar->isVisible(), kSignalTimeoutMs);
        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);
        // closeFind() hands focus back to the timeline explicitly.
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Switching rooms does not leak the previous room's presentation state.
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

    // Drives the state-group expand/collapse functions the summary row's
    // TapHandler and Keys call, on the real pane and the seeded
    // "!devs:mock.local" group, and checks the reset on room switch. Invoked
    // directly because the offscreen QPA never incubates list delegates, so
    // there is nothing to click.
    void stateGroupExpansionTogglesAndResetsOnRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        // "!devs:mock.local" has two consecutive state events forming one
        // group.
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

        // A room switch resets expandedStateGroups (onModelReset).
        controller.setCurrentRoomId(generalId);
        QVERIFY(!isExpanded());

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Thread panel: controller-driven state, visibility, composer send path
    // and narrow layout on the real pane (no reply geometry offscreen).
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

    // The panel composer sends through ThreadController.sendText: the reply
    // lands in the thread model and never as an ordinary room message.
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

    // Below 660 px the open panel takes the whole pane and the room column
    // hides; from 660 up the thread is a 340 px side panel.
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

        // Wide again: side by side, panel at 340 px.
        root->setWidth(800);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  true, kSignalTimeoutMs);
        QCOMPARE(panel->property("visible").toBool(), true);
        auto *panelItem = qobject_cast<QQuickItem *>(panel);
        QVERIFY(panelItem);
        // Offscreen items get no layout polish, so read the preferred width
        // the RowLayout would apply.
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

    // The right side is member panel XOR thread panel (rightPanelState).
    // Closing the thread collapses it to "none"; it never restores Room
    // Information or People.
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

        // Opening a thread replaces the member panel and flips the header
        // chips.
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

        // Reopening the member panel closes the thread.
        QVERIFY(root->setProperty("infoOpen", true));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QCOMPARE(groupButton->property("active").toBool(), true);
        QCOMPARE(forumButton->property("active").toBool(), false);

        // Reopen the thread and close with X: the right side collapses to
        // none and both header chips go inactive.
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

    // Message layouts on the production delegate: Compact drops the avatar
    // gutter; Bubbles colours DM rows only and right-aligns own messages; the
    // text-size setting scales the body font.
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

        // Text scale reaches the body font. Main.qml binds AppTheme.textScale
        // in production; here the singleton is driven directly.
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

    // The room header band grows with its scaled text instead of spilling its
    // content (and action icons) under the timeline, which paints on top.
    void theRoomHeaderKeepsItsContentInsideItsOwnBandAtEveryTextScale()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        LogCapture capture;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *band = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderBand"));
        auto *actions = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderActions"));
        QVERIFY(band);
        QVERIFY(actions);

        const auto actionsBottomInBand = [&] {
            const QPointF topLeft = actions->mapToItem(band, QPointF(0, 0));
            return topLeft.y() + actions->height();
        };
        const auto report = [&](double scale) {
            return qPrintable(QStringLiteral(
                "at textScale %1 the header band is %2px tall and its action "
                "row runs to %3px: the icons are drawn outside the band, over "
                "the timeline")
                .arg(scale).arg(band->height()).arg(actionsBottomInBand()));
        };

        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5, report(1.0));

        // The text-size slider's top end, with no font optical factor.
        for (const double scale : { 1.4, 1.8, 2.0 }) {
            QQmlExpression set(qmlContext(root), root,
                               QStringLiteral("AppTheme.textScale = %1").arg(scale));
            set.evaluate();
            QCoreApplication::processEvents();
            QVERIFY2(actionsBottomInBand() <= band->height() + 0.5, report(scale));
            // The band grows rather than clipping or spilling.
            QVERIFY2(band->height() >= 60.0, report(scale));
        }
        QQmlExpression reset(qmlContext(root), root,
                             QStringLiteral("AppTheme.textScale = 1.0"));
        reset.evaluate();
        QCoreApplication::processEvents();

        // A room topic containing newlines (server text; Label breaks on them
        // regardless of elide). `currentRoom` is set as MainScreen does, or
        // the header would be empty.
        const auto setRoom = [&](const QString &topic) {
            QVariantMap room;
            room.insert(QStringLiteral("name"), QStringLiteral("Minecraft"));
            room.insert(QStringLiteral("topic"), topic);
            root->setProperty("currentRoom", room);
            // Layouts settle on the polish pass, which an offscreen window
            // runs only when it updates.
            QTest::qWait(60);
            QCoreApplication::processEvents();
        };
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        QVERIFY(identity);

        setRoom(QStringLiteral("Cutefunny minecraft server"));
        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                 "a one-line topic already overflows the header band");

        setRoom(QStringLiteral("Instructions:\nhttps://polymc.org\ndrag and drop "
                               "the zip into it\nit will already be listed\nfour"));
        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                 qPrintable(QStringLiteral(
                     "a multi-line room topic pushed the header's action row "
                     "to %1px inside a %2px band: the icons are drawn over the "
                     "message list").arg(actionsBottomInBand()).arg(band->height())));

        // A shorter viewport must not squeeze the band below its content.
        for (const int h : { 520, 400, 300, 240, 180 }) {
            root->setSize(QSizeF(700, h));
            QCoreApplication::processEvents();
            QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                     qPrintable(QStringLiteral(
                         "at pane height %1 the action row runs to %2px inside "
                         "a %3px band").arg(h).arg(actionsBottomInBand())
                             .arg(band->height())));
        }
        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
        // No warning assertion: waiting for real polish passes lets the host
        // audio stack log unrelated noise into the same sink.
    }

    // The room title outranks the header icon row: the row yields (folding
    // into an overflow menu) before the title elides below its floor, and an
    // elided title never sits beside an empty spacer. Measured geometrically,
    // since elision is invisible to a source scan.
    void theRoomTitleOutranksTheHeaderIconRowAtEveryWidth()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        auto *actions = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderActions"));
        auto *spacer = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderSpacer"));
        auto *title = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderTitle"));
        QVERIFY(identity);
        QVERIFY(actions);
        QVERIFY(spacer);
        QVERIFY(title);
        auto *header = identity->parentItem();
        QVERIFY(header);

        // The floor is in the title's own font, capped by the text's natural
        // width so a short name that fits is never a failure.
        QQmlExpression titlePxExpr(qmlContext(root), root,
                                   QStringLiteral(
                                       "AppTheme.scaled(AppTheme.textTitle)"));
        const double titlePx = titlePxExpr.evaluate().toDouble();
        QVERIFY2(titlePx > 0, "the title's scaled font size did not evaluate");
        // Fifteen characters at ~0.53 x pixel size each for this bold face.
        const double inkFloor = 15.0 * 0.53 * titlePx;

        struct Variant { const char *label; bool topic; bool lock; };
        const Variant variants[] = {
            { "topic+lock", true, true },
            { "no topic, no lock", false, false },
        };
        // IconButton's "lg" rung: one overflow control.
        const double kOneIconSlot = 34.0;
        QStringList failures;
        int measured = 0;
        int sawTruncated = 0;
        int sawStarved = 0;
        double worstInk = -1.0;

        // Both ends of the text-size slider: icons are a constant 34 px while
        // the title floor grows with the font, so 1.4 is the tighter case.
        for (const double scale : { 1.0, 1.4 }) {
        QQmlExpression setScale(qmlContext(root), root,
                                QStringLiteral("AppTheme.textScale = %1")
                                    .arg(scale));
        setScale.evaluate();
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const double scaledTitlePx = titlePxExpr.evaluate().toDouble();
        const double scaledInkFloor = 15.0 * 0.53 * scaledTitlePx;

        for (const Variant &v : variants) {
            QVariantMap room;
            // Longer than any header here, so the title can always elide.
            room.insert(QStringLiteral("name"),
                        QStringLiteral("Lightning development and release chat"));
            room.insert(QStringLiteral("topic"),
                        v.topic ? QStringLiteral("Where the work happens")
                                : QString());
            room.insert(QStringLiteral("encrypted"), v.lock);
            root->setProperty("currentRoom", room);
            // Layouts settle on the polish pass.
            QTest::qWait(60);
            QCoreApplication::processEvents();

            for (const int w : { 320, 360, 400, 460, 520, 600, 640, 760,
                                 900, 1200 }) {
                root->setSize(QSizeF(w, 700));
                QTest::qWait(60);
                QCoreApplication::processEvents();
                ++measured;

                const double ink = title->width();
                const double natural =
                    std::ceil(title->property("implicitWidth").toDouble());
                const bool truncated = title->property("truncated").toBool();
                if (truncated)
                    ++sawTruncated;
                if (worstInk < 0.0 || ink < worstInk)
                    worstInk = ink;

                // Invariant 1: the icon row yields before the title does. A
                // short title is acceptable only once the row is down to one
                // control.
                const bool titleStarved =
                    ink + 0.5 < std::min(scaledInkFloor, natural);
                if (titleStarved && actions->width() > kOneIconSlot + 0.5)
                    failures.append(QStringLiteral(
                        "at text scale %10: %1 at pane %2 (header %3): the "
                        "room title is %4 px of ink where its own text wants "
                        "%5 and fifteen "
                        "characters are %6 — while the icon row still holds "
                        "%7 px, more than the one slot an overflow costs. "
                        "identity %8, spacer %9.")
                        .arg(QLatin1String(v.label)).arg(w)
                        .arg(header->width()).arg(ink).arg(natural)
                        .arg(int(scaledInkFloor)).arg(actions->width())
                        .arg(identity->width()).arg(spacer->width())
                        .arg(scale));
                if (titleStarved)
                    ++sawStarved;

                // Invariant 2: an elided title means the spacer is empty.
                if (truncated && spacer->width() > 0.5)
                    failures.append(QStringLiteral(
                        "at text scale %9: %1 at pane %2 (header %3): the "
                        "room title elided at %4 px of a %5 px name while %6 "
                        "px of header sat EMPTY beside it (identity %7, "
                        "icons %8)")
                        .arg(QLatin1String(v.label)).arg(w)
                        .arg(header->width()).arg(ink).arg(natural)
                        .arg(spacer->width()).arg(identity->width())
                        .arg(actions->width()).arg(scale));
            }
        }

        }

        QQmlExpression resetScale(qmlContext(root), root,
                                  QStringLiteral("AppTheme.textScale = 1.0"));
        resetScale.evaluate();
        QCoreApplication::processEvents();

        qInfo("room header sweep: %d widths measured, %d elided, %d below the "
              "ink floor (each with the row already down to one control); "
              "title font %.0f px at scale 1.0, floor %.0f px, narrowest "
              "title %.0f px",
              measured, sawTruncated, sawStarved, titlePx, inkFloor, worstInk);
        // A sweep in which the title never elided proves neither invariant.
        QVERIFY2(sawTruncated > 0,
                 "no width in this sweep elided the room title, so neither "
                 "invariant was actually exercised");
        QVERIFY2(failures.isEmpty(),
                 qPrintable(failures.join(QStringLiteral("\n  "))));

        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
    }

    // Every action folded out of the header row is still reachable in the
    // overflow menu, which costs the row a single slot. A forward contract,
    // not a regression proof.
    void everyFoldedHeaderActionIsStillReachable()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *actions = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderActions"));
        auto *overflow = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderOverflowButton"));
        QVERIFY(actions);
        QVERIFY(overflow);

        QVariantMap room;
        room.insert(QStringLiteral("name"),
                    QStringLiteral("Lightning development and release chat"));
        root->setProperty("currentRoom", room);
        QTest::qWait(60);
        QCoreApplication::processEvents();

        // Wide: nothing folds and the overflow button costs nothing.
        root->setSize(QSizeF(1200, 700));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        QCOMPARE(actions->property("foldedActions").toStringList().size(), 0);
        QVERIFY2(!overflow->isVisible(),
                 "the overflow button is drawn on a header with room to spare");

        // Narrowest supported pane: every folded action has a menu row, and no
        // menu row duplicates a visible icon.
        root->setSize(QSizeF(320, 700));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const QStringList folded =
            actions->property("foldedActions").toStringList();
        QVERIFY2(!folded.isEmpty(),
                 "the narrowest supported pane folded nothing, so this case "
                 "measured a header that was never under pressure");
        QVERIFY2(overflow->isVisible(),
                 "actions folded out of the row with no overflow button to "
                 "reach them through");

        auto *menu = root->findChild<QObject *>(
            QStringLiteral("roomHeaderOverflowMenu"));
        QVERIFY(menu);
        // Open the menu first: `visible` is effective visibility, so every row
        // of a closed popup reads false.
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QCoreApplication::processEvents();
        const QMap<QString, QString> rowFor = {
            { QStringLiteral("startVoiceCallButton"),
              QStringLiteral("overflowStartVoiceCall") },
            { QStringLiteral("pinnedMessagesButton"),
              QStringLiteral("overflowPinnedMessages") },
            { QStringLiteral("threadsViewButton"),
              QStringLiteral("overflowThreads") },
            { QStringLiteral("timelineSearchButton"),
              QStringLiteral("overflowSearch") },
            { QStringLiteral("memberPanelButton"),
              QStringLiteral("overflowMembers") },
            { QStringLiteral("roomInfoButton"),
              QStringLiteral("overflowRoomInfo") },
        };
        QStringList unreachable;
        for (auto it = rowFor.cbegin(); it != rowFor.cend(); ++it) {
            auto *button = root->findChild<QObject *>(it.key());
            QVERIFY2(button, qPrintable(it.key()));
            auto *row = menu->findChild<QObject *>(it.value());
            QVERIFY2(row, qPrintable(it.value()));
            const bool isFolded = button->property("folded").toBool();
            if (isFolded != row->property("visible").toBool())
                unreachable.append(QStringLiteral(
                    "%1 is %2 but its menu row is %3")
                    .arg(it.key(),
                         isFolded ? QStringLiteral("folded out of the row")
                                  : QStringLiteral("still an icon"),
                         row->property("visible").toBool()
                             ? QStringLiteral("shown") : QStringLiteral("hidden")));
            if (isFolded && row->property("text").toString().isEmpty())
                unreachable.append(QStringLiteral(
                    "%1 folded into a menu row with no label").arg(it.key()));
        }
        QVERIFY2(unreachable.isEmpty(),
                 qPrintable(unreachable.join(QStringLiteral("\n  "))));

        // A folded action runs the button's own handler.
        auto *searchRow = menu->findChild<QObject *>(
            QStringLiteral("overflowSearch"));
        QVERIFY(searchRow);
        if (searchRow->property("visible").toBool()) {
            const bool before = root->property("searchOpen").toBool();
            QMetaObject::invokeMethod(searchRow, "triggered");
            QCoreApplication::processEvents();
            QVERIFY2(root->property("searchOpen").toBool() != before,
                     "the overflow row for Search messages did not open the "
                     "search panel");
            if (root->property("searchOpen").toBool() != before)
                root->setProperty("searchOpen", before);
        }
        QMetaObject::invokeMethod(menu, "close");
        QCoreApplication::processEvents();

        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
    }

    // Read-receipt chips: an empty list adds no footprint; a populated one
    // renders at most 4 avatar chips plus a "+N" chip and one summary line.
    // Two separate engine loads: swapping the "model" context property after
    // load instantiates the Repeater mid-cascade, where ids do not resolve.
    void readReceiptChipsRenderBoundedAndCollapseWhenEmpty()
    {
        // Repeater delegates are not QObject children; walk childItems().
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

        // No receipts: invisible strip, no chips, empty summary. An invisible
        // child adds no ColumnLayout height.
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

        // Six readers: 4 chips + "+2", newest first, one summary line. Loaded
        // wide (1400 px) so the right-edge rail placement is a real
        // assertion.
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
            // The chip stack rides the strip's right edge (see
            // readReceiptChipsRideTheRightEdgeRail).
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

    // Read-receipt chips ride one fixed right-edge rail (the strip's right
    // edge) on every row, however wide the body or sender header renders.
    void readReceiptChipsRideTheRightEdgeRail()
    {
        // Delegate-level fixture: the full-pane rows never hydrate offscreen.
        // Three row shapes that once placed chips differently must agree.
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

        // Short body + short name, a wide unbroken body near the column cap,
        // and a header wider than a two-word body.
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
            // Chips ride the strip's right edge regardless of content width.
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
        // One rail: every shape has the same offset from the strip edge.
        QCOMPARE(railOffsets.size(), 3);
        QVERIFY2(qAbs(railOffsets[0] - railOffsets[1]) < 1.0
                     && qAbs(railOffsets[1] - railOffsets[2]) < 1.0,
                 qPrintable(QStringLiteral(
                     "shapes disagree on the receipt rail: %1 / %2 / %3")
                     .arg(railOffsets[0]).arg(railOffsets[1])
                     .arg(railOffsets[2])));

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Receipt chip avatars load through the real chain: member cache ->
    // delegate projection -> Avatar -> MediaBridge -> MediaImageProvider.
    // Carol's avatar is cached before the room opens and must reach "ready";
    // Dave's arrives after the receipt renders, so his chip shows initials and
    // then promotes when membersChanged re-announces ReadReceiptsRole.
    void readReceiptChipAvatarsLoadThroughRealProviderPath()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller, /*row=*/0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        // Enable the mock media bridge and register the readers' avatar bytes.
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

        // Carol: avatar known before the room opens.
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
        // The real image provider, as main.cpp registers it.
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

        // The chip's Avatar root: the item inside readReceiptChip carrying the
        // avatarImage child.
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

        // Carol: the chip reaches the decoded bitmap.
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

        // Dave: initials first, then member-cache hydration alone promotes
        // the chip.
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

        // Hydration rebuilds the chip; re-resolve the Avatar under the same
        // row.
        QTRY_VERIFY_WITH_TIMEOUT(
            (daveAvatar = chipAvatar(daveRow)) != nullptr
                && daveAvatar->property("mxc").toString() == daveMxc,
            kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            daveAvatar->property("presentationState").toString(),
            QStringLiteral("ready"), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The SDK's ReadMarker row renders collapsed while pinned to the bottom,
    // or the own-receipt ack cycle bounces the "New messages" divider under
    // every incoming message. It appears only for a reader who scrolled up.
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

        // A short timeline with the read marker before one unread message.
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

        // Pinned to the bottom: the marker row renders as nothing.
        QVERIFY(!divider->isVisible());
        QCOMPARE(markerItem->implicitHeight(), 0.0);

        // Scrolled up: the divider is back.
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

        // Closing the thread cancels the panel's in-flight wheel motion.
        threadScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(threadScroll->motionActive());
        controller.thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(threadScroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // A backward-pagination prepend is handled by the persistent view anchor.
    // A prepend landing while the reader keeps scrolling must not touch
    // contentY at all while the gesture is live, so the user's own motion is
    // preserved.
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
        // Two pages: the startup viewport fill consumes one.
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

        // Row 5 leaves headroom above; deeper rows land at wheelMinY() on the
        // un-virtualized layout and the clamp would cap the simulated scroll.
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

        // The reader keeps scrolling during the request: a contentY write plus
        // a settle-timer restart, as the pixelDelta branch does. The restart
        // makes userScrollActive true.
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

        // anchorItem resolving shows the anchor delegate survived the prepend,
        // so the "materialized" no-write branch is the one that ran.
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

    // An in-flight wheel glide is translated, not cancelled, across a real
    // pagination completion.
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

        // Row 5 leaves headroom (see paginationPrependPreservesConcurrentScroll).
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
        // The real bounds (see wheelBounds()).
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

    // A prepend at the top edge with a gesture in flight keeps the reader's
    // row at the same viewport offset. At the top edge a prepend pushes the
    // reader's row beyond cacheBuffer and destroys its delegate; mid-list the
    // expected delta is zero and a test there proves nothing.
    void topEdgePrependKeepsReaderOnTheSameRowMidGesture()
    {
    {
        // The diag* counters only increment with LIGHTNING_SCROLL_TRACE, which
        // is read once at controller construction.
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
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
        // A production-sized batch (20 rows): the default 3-row mock page
        // stays inside cacheBuffer (800) and the test would not discriminate.
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            // Long enough to wrap: 20 one-line rows sit right at the
            // cacheBuffer boundary.
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

        // Put the reader at the top edge, where near-top backfill fires.
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
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

        // A gesture is in flight.
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

        // Enforce the premise: the batch must displace the anchor beyond the
        // cache buffer, or the test goes vacuous if metrics change.
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
        const AnchorSettle settle =
            anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
        QVERIFY2(!settle.moved(offsetBefore),
                 qPrintable(QStringLiteral(
                     "a top-edge prepend moved the reader off their row "
                     "mid-gesture (the teleport cascade): %1")
                     .arg(settle.detail(offsetBefore))));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Controller-driven near-top batches (requestNearTop() per batch,
    // bypassing the QML edge latch) are each compensated immediately on
    // completion, and a productive batch ends the run rather than chaining.
    void nearTopControllerDrivenBatchesCompensateImmediatelyNotChained()
    {
    {
        // The diag* counters only increment with LIGHTNING_SCROLL_TRACE, which
        // is read once at controller construction.
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
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
        // One page for the initial fill, one spare, then the batches driven
        // here while a gesture is held.
        constexpr int kBatches = 3;
        // Generous page supply on purpose: this measures anchor compensation,
        // not the near-top continuation bound, and must not depend on it.
        mock->resetTimelineForTest(roomId, events,
                                   /*paginationPages=*/2 + kBatches
                                       + PaginationController::
                                             kMaxNearTopEmptyStrikes);
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
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
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
            // One productive batch ends the run.
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

            // Sample right after this batch settles, before the next request:
            // compensation must be immediate.
            const AnchorSettle settle =
                anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
            QVERIFY2(!settle.moved(offsetBefore),
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a near-top prepend during a held "
                         "gesture moved the reader off their row: %3")
                         .arg(batch + 1).arg(kBatches)
                         .arg(settle.detail(offsetBefore))));
            const double offsetAfter = settle.offset;

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // The same immediate per-batch compensation, driven through
    // ViewportFill rather than NearTop: several consecutive batches with a
    // gesture held, never touching requestNearTop() or nearTopArmed.
    void viewportFillRunCompensatesEveryBatchImmediately()
    {
    {
        // The diag* counters only increment with LIGHTNING_SCROLL_TRACE, which
        // is read once at controller construction.
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
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
        // One page for the initial fill, then kBatches driven explicitly via
        // requestViewportFill().
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
        // The proxy paces newly loaded rows, so release them as every real
        // navigation path does; delegates appear on the next event-loop turn.
        QVERIFY(QMetaObject::invokeMethod(timeline, "releasePendingRows"));
        QCoreApplication::processEvents();
        // Not the exact top: reaching atYBeginning dispatches its own
        // requestNearTop() and would contaminate the premise. Row 8 is near
        // enough for a big prepend to displace it past the cache buffer.
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
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

            // Sample right after this batch settles: must be immediate.
            const AnchorSettle settle =
                anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
            QVERIFY2(!settle.moved(offsetBefore),
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a prepend during a held gesture "
                         "moved the reader off their row before the next "
                         "batch was even requested: %3")
                         .arg(batch + 1).arg(kBatches)
                         .arg(settle.detail(offsetBefore))));
            const double offsetAfter = settle.offset;

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Near-top proximity is measured from the top of loaded history
    // (distanceFromTop()), never raw contentY: originY moves as history loads,
    // so contentY is not a distance. The dispatch gate refuses downward
    // samples and must still dispatch on continued upward progress.
    // QmlBindingContractTest's text scan is the primary guard for the
    // mechanism; this checks geometry and behaviour agree.
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
        // A production-sized page of wrapping rows, so the batch moves originY
        // by much more than the band width.
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

        // The top of loaded history is wheelMaxY() on the rotated view (what
        // distanceFromTop() measures from), read from the Flickable directly.
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
        // Wrapped rows can keep nudging content height after insertion; read
        // topmostY() only once it stops moving.
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

        // At the top of loaded history the edge latches exactly once.
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

        // A whole batch now sits above the reader: not near the top.
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

        // The progress gate lives at the dispatch site (checkNearTopEdge());
        // the settle re-arm is unconditional within the band. No page may land
        // during this phase (it would move the top), but pages stay available.
        mock->setPaginationDelayForTest(60000);
        QVERIFY2(!controller.pagination()->reachedStart(),
                 "premise: backfill must still be available");
        QVERIFY(positionAtTopEdge(timeline));
        setContentYAtDistanceFromTop(0.0);
        // Every probe stays inside the band (~232 px here), including the
        // +40 downward excursion.
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
        // checkNearTopEdge() returns early while stickToBottom is true, which
        // would make every "did not consume" check vacuous; assert it.
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
            // Consumed means dispatched; still armed means the gate refused.
            return !timeline->property("nearTopArmed").toBool();
        };

        // A first approach with no baseline consumes the latch and records
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

        // Downward, still inside the band: the settle re-arms, but the
        // dispatch must refuse.
        setContentYAtDistanceFromTop(probeBase + probeStep);
        QVERIFY(QMetaObject::invokeMethod(settleTimer, "restart"));
        QTRY_VERIFY_WITH_TIMEOUT(!settleTimer->property("running").toBool(),
                                 kSignalTimeoutMs);
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "the settle re-arm is deliberately unconditional in the band");
        // The settle may have flipped stickToBottom; re-assert it.
        QVERIFY2(notFollowingBottom(),
                 "the settle re-pinned follow-latest; the refusal check below "
                 "would pass vacuously");
        QVERIFY2(!armAndCheck(),
                 "a DOWNWARD sample inside the band consumed the latch and "
                 "fetched a page — history loads while scrolling down");

        // Continuing upward past the last request dispatches again.
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

        // Pinned at the exact top with the baseline already there: "came
        // closer" is unsatisfiable, so the pinned-at-top clause must still
        // allow loading. The controller's strike bound throttles from here.
        setContentYAtDistanceFromTop(0.0);
        QVariant atTopDistance;
        QVERIFY(QMetaObject::invokeMethod(timeline, "distanceFromTop",
                                          Q_RETURN_ARG(QVariant, atTopDistance)));
        QVERIFY2(atTopDistance.toDouble() <= 1.0,
                 qPrintable(QStringLiteral(
                     "premise: the probe must be pinned at the top (distance %1)")
                     .arg(atTopDistance.toDouble())));
        QVERIFY(timeline->setProperty("nearTopRequestDistance", 0.0));
        // Re-pin right at the call site: this probe has zero tolerance, and a
        // dispatch can run queued work that nudges contentY.
        timeline->setProperty("contentY", topmostY());
        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "a reader pinned at the exact top could not dispatch — history "
                 "is unreachable at the one place they most want it");

        // The baseline ratchets to the closest approach, including samples
        // that did not dispatch. Otherwise a downward gesture after a deep
        // upward one fetches.
        const double entry = 200;
        const double deep = 40;
        const double backOff = 45;   // closer than `entry`, farther than `deep`
        QVERIFY2(entry < bandWidth(),
                 "fixture: the entry probe must sit inside the band");
        QVERIFY(timeline->setProperty("nearTopRequestDistance",
                                      std::numeric_limits<double>::infinity()));
        setContentYAtDistanceFromTop(entry);
        QVERIFY2(armAndCheck(), "band entry must dispatch");

        // The same gesture continues up: no dispatch (latch spent), but the
        // baseline must drop.
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

        // Downward: closer than the last dispatch (200) but farther than the
        // closest approach (40), so it must refuse.
        setContentYAtDistanceFromTop(backOff);
        QVERIFY2(!armAndCheck(),
                 "a downward sample inside the region already traversed "
                 "consumed the latch — 'scroll up near the top, then scroll "
                 "down a little' still loads history");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // One approach to the top loads a bounded amount, not the whole room. The
    // anchor keeps a top-edge reader at distanceFromTop() ~0 while history
    // piles up, the `fromTop <= 1` clause bypasses the ratchet, and each page
    // re-arms the latch; nearTopApproachRowBudget ends that chain. The
    // request is suppressed via the pane's own re-entrancy guard, so only the
    // dispatch decision is counted.
    void oneApproachToTheTopDoesNotPaginateToTheStartOfTheRoom()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        // Tall, wrapping rows, so the reader can genuinely leave the exit band
        // (3.25 viewports).
        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "history message %1 - deliberately long enough to wrap over "
                "several lines so the loaded room is much taller than the "
                "near-top exit band").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events, /*paginationPages=*/6);

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
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const auto wheelMaxY = [timeline] {
            QVariant v;
            QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                      Q_RETURN_ARG(QVariant, v));
            return v.toDouble();
        };
        const auto wheelMinY = [timeline] {
            QVariant v;
            QMetaObject::invokeMethod(timeline, "wheelMinY",
                                      Q_RETURN_ARG(QVariant, v));
            return v.toDouble();
        };

        const double exitBand =
            timeline->property("nearTopExitDistance").toDouble();
        QVERIFY2(wheelMaxY() - wheelMinY() > exitBand + 100.0,
                 qPrintable(QStringLiteral(
                     "fixture too short: scroll range %1 cannot express a "
                     "departure past the %2px exit band, so the re-arm half "
                     "of this test would assert nothing")
                     .arg(wheelMaxY() - wheelMinY()).arg(exitBand)));

        const int budget =
            timeline->property("nearTopApproachRowBudget").toInt();
        QVERIFY2(budget > 0, "the approach row budget must be a real bound");

        // Hold the re-entrancy guard so maybeRequestNearTop() schedules
        // nothing; the latch, ratchet and budget run as in the app.
        QVERIFY(timeline->setProperty("nearTopCheckScheduled", true));
        QVERIFY(timeline->setProperty("stickToBottom", false));

        int dispatches = 0;
        const int kPages = 40;
        const int kRowsPerPage = 20;
        for (int page = 0; page < kPages; ++page) {
            // The reader stays pinned at the top, as maintainViewAnchor keeps a
            // reader whose row does not move.
            QVERIFY(positionAtTopEdge(timeline));
            QCoreApplication::processEvents();
            QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                              Q_ARG(QVariant, QVariant(true))));
            if (!timeline->property("nearTopArmed").toBool())
                ++dispatches;
            // The page lands; production's handler re-arms the latch and spends
            // the budget.
            emit controller.pagination()->paginationCompleted(
                kRowsPerPage, /*reachedStart=*/false, /*willContinue=*/false);
            QCoreApplication::processEvents();
        }

        QVERIFY2(dispatches > 0,
                 "the fixture never dispatched at all - it is not exercising "
                 "the near-top path and cannot fail on the old code either");
        const int allowedPages = budget / kRowsPerPage + 1;
        QVERIFY2(dispatches <= allowedPages,
                 qPrintable(QStringLiteral(
                     "one uninterrupted approach to the top dispatched %1 "
                     "pages (~%2 rows) against a budget of %3 rows - the "
                     "chain runs to the start of the room, which is the "
                     "reported 'it loads media slow and jumps' behaviour")
                     .arg(dispatches)
                     .arg(dispatches * kRowsPerPage)
                     .arg(budget)));
        QVERIFY2(timeline->property("nearTopRowsThisApproach").toInt() >= budget,
                 "the approach did not actually spend its budget, so the "
                 "bound above was not what stopped the chain");

        // The budget bounds one approach: leaving the band and returning
        // earns a fresh one.
        timeline->setProperty("contentY", wheelMaxY() - (exitBand + 60.0));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QCOMPARE(timeline->property("nearTopRowsThisApproach").toInt(), 0);
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "leaving the exit band must re-arm the latch as before");

        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "a NEW approach after a real departure must dispatch again - "
                 "the budget bounds the automatic chain, it does not stop a "
                 "reader who keeps scrolling into history");

        // Returning to the live edge also resets it.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(false))));
        QCOMPARE(timeline->property("nearTopRowsThisApproach").toInt(), 0);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // A filtered run (pages that insert nothing, e.g. MatrixRTC membership
    // churn dropped by the event filter) must not end the automatic fill
    // while real history is further back: the viewport ends up full. Pins the
    // user-visible outcome, not a specific budget.
    void aFilteredHistoryRunStillFillsTheViewport()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(1);

        // One message on screen, not an empty room.
        QList<TimelineEvent> seed;
        {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("the one message that did load");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            seed.append(e);
        }

        // 20 filtered pages without reaching the start: longer than the old
        // budget (8), shorter than the current one (60).
        constexpr int kFilteredPages = 20;
        QList<TimelineEvent> realChunk;
        for (int i = 0; i < 25; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "a real message from beyond the call-churn run, long enough "
                "that a page or two of them is taller than the viewport %1")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            realChunk.append(e);
        }
        mock->setPaginationChunkForTest(realChunk);
        mock->setFilteredPaginationPagesForTest(kFilteredPages);
        mock->resetTimelineForTest(roomId, seed,
                                   /*paginationPages=*/kFilteredPages + 6);

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

        // Nothing is touched after this line: only the pane's automatic fill
        // may produce the result.
        const qreal viewportHeight = timeline->property("height").toReal();
        QVERIFY2(viewportHeight > 0, "the fixture never laid the pane out");

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentHeight").toReal() >= viewportHeight,
            20000);

        const qreal filled = timeline->property("contentHeight").toReal();
        QVERIFY2(filled >= viewportHeight,
                 qPrintable(QStringLiteral(
                     "the room opened with one message and %1 px of content "
                     "under a %2 px viewport, and the automatic fill stopped "
                     "there — the reader is left scrolling by hand through a "
                     "run of filtered history, which is the report")
                     .arg(filled).arg(viewportHeight)));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // A filtered run that never ends still stops: the budget is larger, not
    // absent, and the controller's strike bound still applies.
    void anEndlessFilteredRunStillStopsTheAutomaticFill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(1);

        QList<TimelineEvent> seed;
        {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("the one message that did load");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            seed.append(e);
        }
        // More filtered pages than any budget allows and a start that is never
        // reached, so only a bound can end the run.
        mock->setFilteredPaginationPagesForTest(5000);
        mock->resetTimelineForTest(roomId, seed, /*paginationPages=*/5000);

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

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);

        // Either the controller's strike bound or the pane's empty-page budget
        // must stop it.
        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->fillStopped(), 30000);
        QVERIFY2(controller.pagination()->fillStopped(),
                 "an unending filtered run must still stop the automatic fill");
    }

    // Empty pages still cost a request and the row budget cannot see them,
    // so one approach is also bounded by a request budget.
    void anApproachThatKeepsGettingEmptyPagesStillStops()
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
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const int requestBudget =
            timeline->property("nearTopApproachRequestBudget").toInt();
        QVERIFY2(requestBudget > 0, "the request budget must be a real bound");

        // Re-entrancy guard held, so only the dispatch decision is measured.
        QVERIFY(timeline->setProperty("nearTopCheckScheduled", true));
        QVERIFY(timeline->setProperty("stickToBottom", false));

        int dispatches = 0;
        for (int page = 0; page < 40; ++page) {
            QVERIFY(positionAtTopEdge(timeline));
            QCoreApplication::processEvents();
            QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                              Q_ARG(QVariant, QVariant(true))));
            if (!timeline->property("nearTopArmed").toBool())
                ++dispatches;
            // An empty page: the row budget cannot charge for it, yet a
            // filtered run re-arms the latch just the same.
            QVERIFY(timeline->setProperty("nearTopArmed", true));
            QCoreApplication::processEvents();
        }

        QVERIFY2(dispatches > 0, "the fixture never dispatched");
        QVERIFY2(dispatches <= requestBudget,
                 qPrintable(QStringLiteral(
                     "one approach issued %1 near-top requests against a "
                     "budget of %2, every one of them returning no rows — the "
                     "row budget cannot bound an empty page")
                     .arg(dispatches).arg(requestBudget)));

        // A genuine departure still starts a new approach.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(false))));
        QCOMPARE(timeline->property("nearTopRequestsThisApproach").toInt(), 0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // Back-to-back prepends are each compensated exactly once. Anchored
    // mid-list, where positions do not move, so this asserts "no spurious
    // write"; topEdgePrependKeepsReaderOnTheSameRowMidGesture proves the
    // compensation itself.
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

        // The per-batch capture bookkeeping no longer exists.
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


    // Growth above the anchor during a live gesture is measured but not
    // written: contentY stays put and the measurement is re-based. Simulated
    // by biasing viewAnchorLastY below the row's real y (the offscreen QPA
    // does not reliably resize delegates). Writing mid-gesture pulled the
    // reader in both directions.
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

        // Open a touchpad session (pixelDelta; never engages the wheel engine)
        // without letting it settle.
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

        // Bias the baseline as 270 px of growth above the anchor would.
        constexpr double simulatedGrowth = 270.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        const double afterY = timeline->property("contentY").toDouble();
        // No write while the gesture is live.
        QVERIFY2(qAbs(afterY - beforeY) < 1.0,
                 qPrintable(QStringLiteral(
                     "growth above the anchor was written into contentY "
                     "during an active gesture, which the deliberate "
                     "no-write design forbids: before=%1 after=%2")
                     .arg(beforeY).arg(afterY)));
        // The branch re-bases via captureViewAnchor(): check for a fresh
        // anchor id rather than an exact y, since it re-derives from whatever
        // row is at the physical top.
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the branch must leave a resolved anchor behind, not a "
                 "stale/empty one, even though it performs no write");
        QVERIFY2(!controller.timelineScroll()->motionActive(),
                 "no glide was active — nothing should have been engaged");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // An in-flight wheel glide is left completely undisturbed by a growth
    // measurement landing mid-flight: same position, coalesced target and
    // remaining distance, still moving.
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

        // Row 5 and the real bounds (see wheelBounds()), so the glide has
        // headroom.
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
        // No write while the glide is live: contentY, target and remaining
        // distance are unchanged.
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

    // During a native drag (Flickable owns contentY) nothing writes contentY,
    // and the anchor is still re-derived from live geometry, exactly as on the
    // self-driven path.
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

        // Preconditions, so a geometry change fails here rather than
        // confusingly below.
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

        // A real native drag, so Flickable's `moving` turns true.
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
        // Release before asserting, so a failure does not leave the button
        // pressed for later tests.
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                            QPoint(360, 512));

        Q_UNUSED(baselineAfter);
        // contentY does not move during the drag.
        QCOMPARE(afterY, beforeY);
        // The anchor is re-derived, not stale or empty. The drag moved the
        // viewport, so a different row may legitimately be at the top.
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the drag path must leave a resolved anchor behind, not a "
                 "stale/empty one");
    }

    // Per-branch scroll-trace instrumentation for maintainViewAnchor(). These
    // pin only the trace plumbing (each counter increments on its own branch,
    // the line renders every field, tracing off costs nothing), not scroll
    // behaviour. Counters carry across a flush so post-settle reconciles
    // appear on the next line.

    // With LIGHTNING_SCROLL_TRACE unset, the same growth scenario moves no
    // counter and emits no line.
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

    // The emitted trace line contains every per-branch field; values are
    // covered by the per-branch tests.
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
            // No "materializedApplied=": the branch no longer writes, so it
            // would always read 0; the magnitude is materializedMaxAbsDelta.
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

    // The materialized (self-driven) branch's counters: it only measures
    // (diagMaterializedFirings, diagMaterializedMaxAbsDelta) and never
    // applies, so diagMaterializedAppliedSum stays unchanged.
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
        // maintainViewAnchor() measures against anchorPositionForItem(), i.e.
        // y + height (the rotated view's physical top edge), not y alone.
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
        // Baseline rather than zero: under heavy load one extra firing has
        // been seen during setup (cause not identified). The assertions stay
        // exact in the quiescent case.
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
        // The branch performs no write, so the applied sum stays at baseline.
        QCOMPARE(timeline->property("diagMaterializedAppliedSum").toDouble(),
                 baseApplied);
        // Exact when quiescent; with an earlier baseline firing it is only a
        // lower bound (the field keeps the largest |delta|).
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

    // diagDragDeferrals counts a native drag engaging the defer path. A pure
    // drag never opens the diag session, so one touchpad delta opens it
    // first, then a real drag follows.
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

        // Open the diag session (a pure drag does not).
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

        // Then a real native drag, so Flickable's `moving` turns true.
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

        // Baselines captured right before the driven call: the touchpad
        // session and the drag can each run this branch under load, and
        // diagDragDeferrals increments inside diagMaterializedFirings' branch.
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

    // A stable id that resolves to no row increments diagUnresolvedIdFallbacks,
    // not diagEvictedNoInsertFallbacks: "the id is gone" and "the delegate was
    // evicted" are different causes.
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

        // A stable id that resolves to no row cannot enter the displaced
        // branch (it needs row >= 0) and falls to the capture fallback.
        QVERIFY(timeline->setProperty(
            "viewAnchorId", QStringLiteral("$this-event-id-does-not-exist")));

        // Baseline the other counters right before the call: setup can
        // legitimately fire the materialized branch, so assert deltas.
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

    // The un-virtualized Column never evicts a delegate, so an anchor whose
    // event is loaded always resolves and never takes the evicted-no-insert
    // or unresolved-id fallback. If delegate eviction returns, this fails;
    // the pre-8f84d18 eviction fixtures are the starting point for re-porting.
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

        // Jump far away; the anchor delegate must survive.
        QVERIFY(positionAtSourceRow(timeline, 50));
        QCoreApplication::processEvents();
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the un-virtualized Column must keep every loaded row's "
                 "delegate alive — eviction has been reintroduced");
        QVERIFY2(!timeline->property("moving").toBool(),
                 "programmatic positioning must not leave Flickable.moving");
        // Pin the id back in case a queued maintainViewAnchorCoalesced()
        // re-captured another row.
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
        // A coalesced re-capture may have moved the anchor; re-pin it and
        // confirm its delegate is alive right before the driven call.
        QVERIFY(timeline->setProperty("viewAnchorId", anchorId));
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the anchor's delegate must be alive for this invariant");
        // Setup can drive counters; assert no increase across the call.
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
        // With the delegate alive and input active, the materialized branch
        // handles this. A coalesced call may fire alongside, so assert growth.
        QVERIFY2(timeline->property("diagMaterializedFirings").toInt()
                     > baseMaterialized,
                 "the driven call must take the materialized branch");
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
    }

    // With no eviction, the displaced branch's `!it` precondition can never
    // hold for a loaded row. Even when the bookkeeping claims rows were
    // inserted above the reader, a live delegate routes the correction
    // through the idle absolute restore.
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
        // Ask the pane for the view row: the paced proxy can make a
        // hand-computed mapping wrong while rows are releasing.
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

        // Inject the displaced branch's bookkeeping: the recorded row three
        // below the real one, with a matching content-height delta. Idle with
        // the delegate alive, the displaced arithmetic must not run.
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
        // The prepend diagnostic still records the firing, attributed to the
        // idle path.
        QCOMPARE(timeline->property("diagPrependFirings").toInt(),
                 basePrepend + 1);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftPath").toString(),
                 QStringLiteral("idle"));
        // The idle branch re-based the recorded row to the real view row.
        QVERIFY(QMetaObject::invokeMethod(
            timeline, "viewRowForStableId", Q_RETURN_ARG(QVariant, viewRowOut),
            Q_ARG(QVariant, anchorId)));
        QCOMPARE(timeline->property("viewAnchorRow").toInt(),
                 viewRowOut.toInt());
    }


    // diagNoAnchorReturns increments on the early return (empty viewAnchorId
    // or stickToBottom), so "never engaged" is distinguishable from "nothing
    // to correct".
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

        // stickToBottom forces the early return; viewAnchorId also starts
        // empty on a fresh room.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        // Nothing else ran. An empty viewAnchorId takes precedence over
        // stickToBottom, so the sibling counter stays 0.
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    // diagStickToBottomReturns: a correction dropped because the reader
    // returned to the bottom. Needs a non-empty viewAnchorId, since the empty
    // arm takes precedence.
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

        // A real anchor first, then the reader lands back at the bottom.
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

    // Outcome counters incremented while no gesture session is open (a
    // reconcile landing after the settle flush) survive to the next printed
    // line. Driven directly for determinism.
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

        // Two early returns before any session opens.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagActive").toBool(), false);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        // Opening a gesture resets only the gesture-local event group, not
        // the carried outcome count. viewAnchorId stays empty, so this delta
        // triggers no further outcome.
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

        // Drained at print: the next early return starts from 1.
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    // With a row window active, real wheel notches toward the newest end
    // reach the true live edge: the window must shrink during motion, and a
    // skip under the 40-row hysteresis must still close to zero.
    void wheelMotionIntoHistoryAndBackReachesTheTrueLiveEdge()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQuickItem *root = paneRootOf(timeline);
        QVERIFY(root != nullptr);

        // Establish a window as the settle timer does.
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        const int skip = timeline->property("rowWindowSkip").toInt();
        QVERIFY2(skip > 0,
                 "no window was established, so the synthetic newest edge "
                 "this test is about does not exist here");

        // Scroll down using only the wheel.
        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        // Notch until follow-latest engages, which implies skip == 0
        // (atBottomEdge() is false while a window hides the live edge).
        // skip == 0 alone is not enough: the reader can still be a screen
        // above the newest message. Generous budget: each extension adds
        // runway to traverse.
        for (int i = 0; i < 6000; ++i) {
            sendWheelNotch(window, pos, -120, /*inverted=*/false);
            if (timeline->property("stickToBottom").toBool())
                break;
        }

        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
        QVERIFY2(timeline->property("diagWindowNewEndExtensions").toInt() > 0,
                 "the live edge was reached, but no newest-end extension ever "
                 "ran — the motion path is not what restored it");
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("stickToBottom").toBool(), 3000);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);
        QVERIFY2(!pill->isVisible(),
                 "the reader is at the true live edge and the jump pill is "
                 "still demanding a press");
    }

    // A window whose skip is smaller than the hysteresis step still closes to
    // the live edge.
    void rowWindowWithASmallSkipStillClosesToTheLiveEdge()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        // Small skip and an exposed count near windowMinRows: both hysteresis
        // terms under 40.
        QQmlExpression setWindow(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ app.timelineView.setWindow(12, 330);"
                           " contentY = wheelMinY();"
                           " updateVisibleRowRange(); return true; })()"));
        QVERIFY(setWindow.evaluate().toBool());
        QVERIFY2(!setWindow.hasError(),
                 setWindow.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 12);
        const int rows = timeline->property("count").toInt();
        QVERIFY2(qAbs(rows - 320) < 40,
                 qPrintable(QStringLiteral(
                     "fixture exposed %1 rows; the row term of the hysteresis "
                     "must also be under 40 or this proves nothing")
                                .arg(rows)));

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
    }

    // In a room too short to scroll, a wheel event is a no-op and must leave
    // follow-latest engaged (a jump pill there could never be cleared).
    // WheelEvent::inverted is ignored, so both states behave the same.
    void clampedNoOpWheelEventLeavesFollowLatestEngaged()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 3, QStringLiteral("short"),
                        QStringLiteral("tiny")),
            /*paginationPages=*/0, /*viewportHeight=*/600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(qFuzzyCompare(minY + 1.0, maxY + 1.0),
                 qPrintable(QStringLiteral(
                     "fixture is scrollable (%1..%2); the clamped no-op this "
                     "test is about cannot occur").arg(minY).arg(maxY)));
        QVERIFY(timeline->setProperty("stickToBottom", true));

        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        for (bool inverted : { false, true }) {
            QVERIFY(timeline->setProperty("stickToBottom", true));
            sendWheelNotch(window, pos, 120, inverted);
            QVERIFY2(timeline->property("stickToBottom").toBool(),
                     inverted ? "an unappliable inverted wheel event "
                                "disengaged follow-latest"
                              : "an unappliable wheel event disengaged "
                                "follow-latest");
            QVERIFY(!pill->isVisible());
        }

        // Downward at the live edge, in both inverted states.
        for (bool inverted : { false, true }) {
            QVERIFY(timeline->setProperty("stickToBottom", true));
            sendWheelNotch(window, pos, -120, inverted);
            QVERIFY(timeline->property("stickToBottom").toBool());
            QVERIFY(!pill->isVisible());
        }
    }

    // Turning smooth scrolling off must not reverse the wheel: both settings
    // move the reader the same way for the same notch. A comparison rather
    // than an absolute direction, which depends on the view's rotation.
    void turningSmoothScrollingOffKeepsTheWheelDirection()
    {
        // Measure first, assert after: this binary shares one QSettings file,
        // and a failure while the setting is flipped would leave it flipped
        // on disk for later cases.
        double moved[2] = { 0.0, 0.0 };
        const bool wanted[2] = { true, false };
        bool previous = true;
        for (int i = 0; i < 2; ++i) {
            AppController controller(AppController::MockBackend);
            QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
            const QString roomId = QStringLiteral("!general:mock.local");
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = paneWithEvents(
                controller, engine, window, roomId,
                textFixture(roomId, 60, QStringLiteral("line"),
                            QStringLiteral("body")),
                /*paginationPages=*/0, /*viewportHeight=*/600, &timeline);
            QVERIFY(root != nullptr);
            QVERIFY(timeline != nullptr);
            if (i == 0)
                previous = controller.settings()->smoothScrolling();
            controller.settings()->setSmoothScrolling(wanted[i]);

            double minY = 0.0;
            double maxY = 0.0;
            QVERIFY(wheelBounds(timeline, &minY, &maxY));
            QVERIFY2(maxY > minY + 1.0, "fixture is not scrollable");

            // Park in the middle so no edge clamp swallows the notch.
            const double start = (minY + maxY) / 2.0;
            QVERIFY(timeline->setProperty("contentY", start));
            QVERIFY(timeline->setProperty("stickToBottom", false));
            QTest::qWait(60);

            const QPointF pos(window.width() / 2.0, window.height() / 2.0);
            // One notch up (toward older messages).
            sendWheelNotch(window, pos, 120, /*inverted=*/false);
            QTest::qWait(400);   // let a glide, if any, settle
            moved[i] = timeline->property("contentY").toDouble() - start;

            controller.settings()->setSmoothScrolling(previous);
        }

        for (int i = 0; i < 2; ++i) {
            QVERIFY2(qAbs(moved[i]) > 1.0,
                     qPrintable(QStringLiteral("smooth=%1 moved nothing")
                                    .arg(wanted[i])));
        }
        // Both settings move the same way.
        QCOMPARE(moved[1] > 0 ? 1 : -1, moved[0] > 0 ? 1 : -1);
    }

    // The jump pill lands the reader on the first unread message (the SDK's
    // read marker row), driven through the real pill and landing machinery.
    void thePillLandsTheReaderOnTheFirstUnreadMessage()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        // 60 messages with the marker a third of the way from the newest end,
        // so landing is a real move.
        QList<TimelineEvent> events =
            textFixture(roomId, 60, QStringLiteral("u"), QStringLiteral("body"));
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.insert(40, marker);

        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events, /*paginationPages=*/0,
                                          /*viewportHeight=*/600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        // The model found the marker, so the pill is offered.
        QCOMPARE(controller.timeline()->readMarkerRow(), 40);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY2(pill != nullptr, "the pill does not exist");
        QVERIFY2(pill->isVisible(),
                 "the marker is loaded and the pill is still hidden");

        // Start at the live edge.
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QMetaObject::invokeMethod(timeline, "goToLatest");
        QTest::qWait(120);
        const double before = timeline->property("contentY").toDouble();

        QMetaObject::invokeMethod(pill, "click");
        // The landing may wait for the Column to lay the row out.
        QVERIFY2(QTest::qWaitFor([&] {
                     return timeline->property("navigationPendingRow").toInt() < 0
                         && timeline->property("contentY").toDouble() > before + 1.0;
                 }, 4000),
                 "the pill did not move the reader toward the marker");

        // The marker is above the live edge: the reader moved into history
        // and stopped following the bottom.
        QVERIFY(!timeline->property("stickToBottom").toBool());
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
    }

    // No marker, no pill.
    void noReadMarkerMeansNoPill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("q"), QStringLiteral("body")),
            /*paginationPages=*/0, /*viewportHeight=*/600, &timeline);
        QVERIFY(root != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), -1);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QVERIFY2(!pill->isVisible(),
                 "the pill is offered with nothing to jump to");
    }

    // The find bar's History segment searches the local index (which covers
    // encrypted rooms), driven through the real bar: open Find, switch to
    // History, type, read the rows.
    void theFindBarSearchesTheLocalIndex()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        QList<TimelineEvent> events =
            textFixture(roomId, 12, QStringLiteral("s"), QStringLiteral("body"));
        // A distinctive needle, and a decoy that must NOT match it.
        events[3].body = QStringLiteral("the zephyrine protocol landed today");
        events[7].body = QStringLiteral("nothing to do with that word");
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events, /*paginationPages=*/0,
                                          /*viewportHeight=*/700, &timeline);
        QVERIFY(root != nullptr);

        auto *search = controller.messageSearch();
        QVERIFY(search != nullptr);
        QVERIFY2(search->localAvailable(),
                 "the backend reports no local index, so the bar cannot be "
                 "exercised at all");

        // Open Find and switch to History through the real controls.
        QMetaObject::invokeMethod(root, "openFind");
        QTest::qWait(60);
        auto *toggle = root->findChild<QQuickItem *>(
            QStringLiteral("findModeToggle"));
        QVERIFY2(toggle != nullptr, "the History segment is gone");
        QVERIFY2(toggle->isVisible(),
                 "History is not offered even though a local index exists");
        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("history")));
        QTest::qWait(60);
        QCOMPARE(root->property("findHistoryMode").toBool(), true);
        QCOMPARE(search->source(), QStringLiteral("local"));

        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(field != nullptr);
        field->setProperty("text", QStringLiteral("zephyrine"));
        QVERIFY2(QTest::qWaitFor([&] {
                     return search->state() == QLatin1String("results");
                 }, 4000),
                 qPrintable(QStringLiteral("search state stuck at %1")
                                .arg(search->state())));
        QCOMPARE(search->rowCount({}), 1);
        QCOMPARE(search->rowAt(0).value(QStringLiteral("body")).toString(),
                 QStringLiteral("the zephyrine protocol landed today"));

        // The coverage line says what is being searched, so "no results" is
        // not read as "never said".
        auto *coverage = root->findChild<QQuickItem *>(
            QStringLiteral("findLocalCoverage"));
        QVERIFY2(coverage != nullptr, "the coverage line is gone");
        QVERIFY(coverage->isVisible());

        // A query below the tokenizer minimum is its own state, not an error
        // or "no results".
        field->setProperty("text", QStringLiteral("ze"));
        QVERIFY2(QTest::qWaitFor([&] {
                     return search->state() == QLatin1String("too_short");
                 }, 4000),
                 qPrintable(QStringLiteral("short query reported %1")
                                .arg(search->state())));
        QVERIFY(search->minLocalChars() >= 3);

        // A needle nobody said returns nothing.
        field->setProperty("text", QStringLiteral("borogoves"));
        QVERIFY(QTest::qWaitFor([&] {
            return search->state() == QLatin1String("no_results");
        }, 4000));
    }

    // "Index this room" beside the coverage line reaches the backend.
    void indexingThisRoomReachesTheBackend()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 6, QStringLiteral("d"), QStringLiteral("body")),
            /*paginationPages=*/0, /*viewportHeight=*/700, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QMetaObject::invokeMethod(root, "openFind");
        QTest::qWait(50);
        auto *toggle = root->findChild<QQuickItem *>(
            QStringLiteral("findModeToggle"));
        QVERIFY(toggle != nullptr);
        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("history")));
        QTest::qWait(50);

        auto *button = root->findChild<QQuickItem *>(
            QStringLiteral("findIndexRoomButton"));
        QVERIFY2(button != nullptr, "the index-this-room action is gone");
        QVERIFY(button->isVisible());
        const int before = mock->deepIndexedRooms.size();
        QMetaObject::invokeMethod(button, "click");
        QVERIFY2(QTest::qWaitFor([&] {
                     return mock->deepIndexedRooms.size() > before;
                 }, 3000),
                 "pressing Index this room asked the backend for nothing");
        QCOMPARE(mock->deepIndexedRooms.last(), roomId);
    }

    // A fresh short room becomes scrollable without a resize. A batch of rows
    // the timeline does not render (hidden room activity) changes the model
    // but not contentHeight, so no geometry signal re-runs the level-triggered
    // viewport fill.
    void freshShortRoomBecomesScrollableWithoutAResize()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        controller.settings()->setShowRoomActivity(false);
        // Enough fill budget for the retries; this is about the loop being fed,
        // not its ceiling.
        controller.pagination()->setMaxViewportFillRequests(24);

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");

        // Every page until told otherwise is routine room activity: real rows,
        // zero rendered height.
        QList<TimelineEvent> hidden;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.roomId = roomId;
            e.type = TimelineEvent::StateChange;
            e.stateKind = QStringLiteral("membership");
            e.body = QStringLiteral("membership churn");
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-9000 - i * 60);
            hidden.append(e);
        }
        mock->setPaginationChunkForTest(hidden);

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 2, QStringLiteral("seed"),
                        QStringLiteral("seed"), /*baseSecondsAgo=*/7200),
            /*paginationPages=*/24, /*viewportHeight=*/420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        // Precondition: the invisible page landed and the viewport is still
        // not filled.
        QTRY_VERIFY_WITH_TIMEOUT(controller.timeline()->rowCount() > 2, 4000);
        QVERIFY2(timeline->property("contentHeight").toReal()
                     < timeline->property("height").toReal(),
                 "the hidden page rendered after all; the deadlock this test "
                 "is about does not exist here");

        // From here the timeline can fill, but only if something asks again.
        // Nothing is resized, scrolled or clicked below.
        mock->setPaginationChunkForTest(textFixture(
            roomId, 4, QStringLiteral("older"),
            QStringLiteral("a deliberately long older message body so that "
                           "each page adds several wrapped lines of real "
                           "rendered height to the column"),
            /*baseSecondsAgo=*/20000));

        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                double lo = 0.0;
                double hi = 0.0;
                return wheelBounds(timeline, &lo, &hi) && hi > lo;
            }(), 8000);
    }

    // A quick middle click starts no autoscroll and leaves no marker;
    // press-and-hold is the whole gesture.
    void quickMiddleClickStartsNoAutoscrollAndLeavesNoMarker()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 40, QStringLiteral("mid"),
                        QStringLiteral("middle click probe")),
            /*paginationPages=*/0, /*viewportHeight=*/420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);
        auto *marker = scroller->findChild<QQuickItem *>(
            QStringLiteral("autoscrollAnchorMarker"));
        QVERIFY(marker != nullptr);

        const QPoint at = scroller
                              ->mapToScene(QPointF(scroller->width() / 2.0,
                                                   scroller->height() / 2.0))
                              .toPoint();
        QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        // The press alone engages it.
        QVERIFY(scroller->property("active").toBool());
        QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();

        QVERIFY2(!scroller->property("active").toBool(),
                 "a quick middle click latched autoscroll on");
        QVERIFY2(!marker->isVisible(),
                 "the autoscroll anchor marker survived the release");
    }

    // An active middle-drag scroll writes contentY directly, so Escape,
    // programmatic navigation and a room switch must each end it.
    void activeMiddleDragIsCancelledByEscapeRoomSwitchAndNavigation()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 60, QStringLiteral("drag"),
                        QStringLiteral("drag probe")),
            /*paginationPages=*/0, /*viewportHeight=*/420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);

        const QPointF centre(scroller->width() / 2.0,
                             scroller->height() / 2.0);
        const QPoint anchor = scroller->mapToScene(centre).toPoint();
        const QPoint dragged =
            scroller->mapToScene(centre + QPointF(0, 90)).toPoint();

        auto beginDrag = [&] {
            QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier,
                              anchor);
            QTest::mouseMove(&window, dragged);
            QCoreApplication::processEvents();
            return scroller->property("active").toBool();
        };
        auto endDrag = [&] {
            QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier,
                                dragged);
            QCoreApplication::processEvents();
        };

        // 1. Escape.
        QVERIFY(beginDrag());
        QTest::keyClick(&window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "Escape did not end an active autoscroll");
        endDrag();

        // 2. Programmatic navigation: jump to latest owns contentY from the
        //    moment it is pressed.
        QVERIFY(beginDrag());
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToLatest"));
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "jump to latest did not end an active autoscroll");
        endDrag();

        // 3. Room switch (the scroller also hides itself on a switch).
        QVERIFY(beginDrag());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QCoreApplication::processEvents();
        QVERIFY(!scroller->property("active").toBool());
        endDrag();
    }

    // One owner of transient row interaction: opening the reaction picker's
    // skin-tone popup clears the pinned/hovered action keys (the bar is gone,
    // not covered), and hover cannot reclaim while an owner is set.
    void tonePopupOwnsRowInteractionSoNoActionBarShows()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, /*row=*/0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 12, QStringLiteral("tone"),
                        QStringLiteral("tone probe")),
            /*paginationPages=*/0, /*viewportHeight=*/420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 0, 4000);

        // The newest view row may be virtual (date divider, read marker) with
        // no actionKey; find the first real message row.
        QQuickItem *messageRow = nullptr;
        QString actionKey;
        for (int row = 0; row < 8 && actionKey.isEmpty(); ++row) {
            QVariant rowVar;
            if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                           Q_RETURN_ARG(QVariant, rowVar),
                                           Q_ARG(QVariant, QVariant(row))))
                continue;
            auto *candidate = rowVar.value<QQuickItem *>();
            if (!candidate)
                continue;
            const QString key = candidate->property("actionKey").toString();
            if (key.isEmpty())
                continue;
            messageRow = candidate;
            actionKey = key;
        }
        QVERIFY2(messageRow != nullptr, "no real message row was instantiated");

        // A pinned row shows its bar.
        QVERIFY(timeline->setProperty("pinnedActionsKey", actionKey));
        QTRY_VERIFY_WITH_TIMEOUT(
            messageRow->property("actionsVisible").toBool(), 2000);

        auto *picker = root->findChild<QObject *>(
            QStringLiteral("sharedReactionPicker"));
        QVERIFY(picker != nullptr);
        auto *tonePopup =
            picker->findChild<QObject *>(QStringLiteral("emojiTonePopup"));
        QVERIFY(tonePopup != nullptr);

        QQmlExpression openPicker(
            qmlContext(timeline), timeline,
            QStringLiteral("openReactionPicker('$tone3', Qt.point(10, 10))"));
        openPicker.evaluate();
        QVERIFY2(!openPicker.hasError(),
                 openPicker.error().toString().toUtf8().constData());
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("picker"), 2000);

        QVariantList variants;
        variants.append(QVariantMap{
            { QStringLiteral("emoji"), QStringLiteral("\xF0\x9F\x91\x8D") },
            { QStringLiteral("name"), QStringLiteral("thumbs up") },
            { QStringLiteral("tone"), 1 },
        });
        QVERIFY(tonePopup->setProperty("variants", variants));
        QVERIFY(QMetaObject::invokeMethod(tonePopup, "open"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("tone"), 2000);

        // Both keys are cleared and no row draws a bar.
        QCOMPARE(timeline->property("pinnedActionsKey").toString(), QString());
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), QString());
        QVERIFY2(!messageRow->property("actionsVisible").toBool(),
                 "a row still showed its action bar under the tone popup");

        // Hover cannot reclaim while an owner is set (the HoverHandler's
        // write).
        QVERIFY(timeline->setProperty("hoveredActionsKey", actionKey));
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), QString());
        QVERIFY(!messageRow->property("actionsVisible").toBool());

        // Closing the tone popup returns ownership to the picker, which is
        // still open.
        QVERIFY(QMetaObject::invokeMethod(tonePopup, "close"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("picker"), 2000);

        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QString(), 2000);
        // Ownership released: ordinary hover works again.
        QVERIFY(timeline->setProperty("hoveredActionsKey", actionKey));
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), actionKey);
    }

    // Reply navigation to a target outside the exposed window positions the
    // view once real geometry exists; released rows have no positioned
    // delegates until the Column re-lays out.
    void replyNavigationToAnUnexposedTargetActuallyPositionsTheView()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QVERIFY2(timeline->property("rowWindowSkip").toInt() > 0,
                 "no window established");

        // An old event, outside the window's exposed range.
        const QString targetId = QStringLiteral("$win50");
        QQmlExpression exposed(
            qmlContext(timeline), timeline,
            QStringLiteral("viewRowForStableId('") + targetId
                + QStringLiteral("')"));
        QCOMPARE(exposed.evaluate().toInt(), -1);

        // Stretch the highlight timer so a loaded machine cannot expire it
        // before the geometry assertion below.
        controller.pagination()->setHighlightDurationForTest(120000);
        controller.pagination()->jumpToEvent(targetId);
        // Locating the target needs a real pagination round trip; wait for
        // the contract. The delegate's highlight source is the view.
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("navigationHighlightEventId").toString(),
            targetId, 10000);

        // The row ends up on screen, comfortably inside the edges.
        QQmlExpression onScreen(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = r >= 0 ? itemAtViewRow(r) : null;"
                           " if (!it) return 1e9;"
                           " return (contentY + height)"
                           "        - (it.y + it.height); })('")
                + targetId + QStringLiteral("')"));
        double offset = 1e9;
        const bool landed = QTest::qWaitFor([&] {
            const QVariant v = onScreen.evaluate();
            if (onScreen.hasError())
                return false;
            offset = v.toDouble();
            return offset >= 0.0
                   && offset <= timeline->property("height").toReal();
        }, 5000);
        const double height = timeline->property("height").toReal();
        // Report geometry on failure: offset 1e9 means the row never resolved;
        // a large negative offset means the view landed at the wrong end.
        QVERIFY2(landed, qPrintable(QStringLiteral(
                     "reply target never reached the viewport "
                     "(offset %1, viewport %2, landings %3, unresolved %4)")
                     .arg(offset).arg(height)
                     .arg(timeline->property("diagNavigationLandings").toInt())
                     .arg(timeline->property("diagNavigationUnresolved")
                              .toInt())
                     + QStringLiteral(" abandoned %1 pendingRow %2 pendingId %3"
                                      " ticks %4 count %5 laidOut %6 winSkip %7")
                           .arg(timeline->property("diagNavigationAbandoned").toInt())
                           .arg(timeline->property("navigationPendingRow").toInt())
                           .arg(timeline->property("navigationPendingId").toString())
                           .arg(timeline->property("navigationTotalAttempts").toInt())
                           .arg(timeline->property("count").toInt())
                           .arg(timeline->property("layoutRowsAtLastPass").toInt())
                           .arg(timeline->property("rowWindowSkip").toInt())));
        QVERIFY2(offset > 1.0 && offset < height - 1.0,
                 qPrintable(QStringLiteral(
                     "reply target landed flush against a viewport edge "
                     "(offset %1 of %2)").arg(offset).arg(height)));
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
        // Exactly one write: the landing waits for geometry and positions
        // once. A land-then-correct loop would fight the anchor machinery.
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 1);
    }

    // A pending jump must not fire later mid-gesture. Its retry budget is
    // re-armed whenever the view's shape changes, which during scrolling is
    // always. A wheel notch abandons the jump.
    void aWheelNotchAbandonsAJumpThatHasNotLandedYet()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        // The precondition is only that the target is not exposed.
        QQmlExpression exposed(
            qmlContext(timeline), timeline,
            QStringLiteral("viewRowForStableId('$win50')"));
        QCOMPARE(exposed.evaluate().toInt(), -1);

        // Source row 50 is deep history, so the landing stays pending.
        QQmlExpression arm(
            qmlContext(timeline), timeline,
            QStringLiteral("beginNavigationLanding(50, 0, false)"));
        arm.evaluate();
        QVERIFY2(!arm.hasError(),
                 arm.error().toString().toUtf8().constData());
        QVERIFY2(!timeline->property("navigationPendingId").toString()
                      .isEmpty(),
                 "the landing did not stay pending, so this case would pass "
                 "vacuously");
        const qreal parked = timeline->property("contentY").toReal();

        // The reader takes the view.
        sendWheelNotch(window, QPointF(350, 200), 120, false);

        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QCOMPARE(timeline->property("navigationPendingRow").toInt(), -1);
        QCOMPARE(timeline->property("diagNavigationAbandoned").toInt(), 1);

        // And it stays abandoned while the event loop runs.
        QTest::qWait(400);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
        QVERIFY2(!qFuzzyCompare(timeline->property("contentY").toReal() + 1.0,
                                parked + 1.0),
                 "the wheel notch did not move the view, so a teleport back "
                 "to `parked` could not be distinguished from doing nothing");
    }

    // A landing whose view keeps changing shape gives up after a bounded
    // number of retries instead of waiting forever.
    void aLandingWhoseViewNeverStopsChangingGivesUpInsteadOfWaitingForever()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QCoreApplication::processEvents();

        QQmlExpression arm(
            qmlContext(timeline), timeline,
            QStringLiteral("beginNavigationLanding(50, 0, false)"));
        arm.evaluate();
        QVERIFY2(!arm.hasError(),
                 arm.error().toString().toUtf8().constData());
        QVERIFY(!timeline->property("navigationPendingId").toString().isEmpty());

        // Retry by hand with the shape changing every time, as a live scroll
        // does. layoutRowsAtLastPass is a plain property, so moving it
        // reproduces the churn without a gesture.
        QQmlExpression retry(qmlContext(timeline), timeline,
                             QStringLiteral("tryLandNavigationTarget()"));
        // A fixed, generous count rather than reading the ceiling property:
        // still pending after 400 attempts means it would wait forever.
        for (int i = 0; i < 400; ++i) {
            timeline->setProperty("layoutRowsAtLastPass", i + 1);
            retry.evaluate();
            QVERIFY2(!retry.hasError(),
                     retry.error().toString().toUtf8().constData());
            if (timeline->property("navigationPendingId").toString().isEmpty())
                break;
        }

        // It gave up, and said so, rather than landing on a stale target.
        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QCOMPARE(timeline->property("navigationPendingRow").toInt(), -1);
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 1);
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
    }
};

int main(int argc, char *argv[])
{
    // Qt Quick item creation needs a QGuiApplication, as in main.cpp.
    QGuiApplication app(argc, argv);
    TimelinePaneQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "TimelinePaneQmlTest.moc"
