// The Spaces rail's drag-to-make-a-folder gesture, driven by a REAL POINTER.
//
// WHY THIS FILE EXISTS. Dropping a Space onto a Space had never once made a
// folder. It was "fixed" twice and was structurally unreachable both times,
// while fifteen cases in tests/RailLayoutTest.cpp passed throughout — because
// every one of them calls RailEntryModel directly and hands it a state
// production could not produce. The model was right; the VIEW could not reach
// it.
//
//   v1 of the band rule: "the middle 24 px of a row is the group zone".
//   Reaching that middle means first crossing the row's near edge, which
//   REORDERED — so the tile being aimed at stepped aside, the row under the
//   pointer became the dragged entry, and a dragged entry is never a group
//   target.
//
//   v2: "short of the row's midpoint you are resting, past it you have pushed
//   through". Geometry right, dispatch wrong: the resting branch ended in
//   `updateDrag(row, !dwellTimer.running)`, and `running` is TRUE for the
//   whole 250 ms the dwell is being served, so the second pointer sample
//   inside the target's near half reordered anyway — and the branch that then
//   fired stopped the very dwell it was waiting for.
//
// Both moved things while the user was still aiming. The rule under test here
// is the third: THE TILE IS THE GROUP TARGET, THE GAP BETWEEN TILES IS THE
// REORDER TARGET, nothing moves while the pointer is on a tile, and there is
// no dwell. What that costs is exactly the half a model test cannot see, so
// this suite sends real QMouseEvents at tile centres resolved from real
// delegate geometry and asserts on what a release WROTE.
//
// WHAT IT PROVES, AND WHAT IT DOES NOT. An offscreen QTest::mouseMove is a
// synthesized pointer, not a hand on a mouse. This suite proves the gesture is
// REACHABLE — that a plausible sequence of pointer events arrives at
// hoverGroup()/hoverGap() and that a release writes the folder. It proves
// nothing about FEEL: whether 24 px is the right band, whether the ring
// appearing with no dwell reads as responsive or twitchy, whether the dragged
// tile parking on its target reads as a merge, whether the auto-scroll is
// usable. The standing open item "DRIVE THE RAIL'S DRAG WITH A REAL POINTER"
// (CLAUDE.md §16) therefore stays OPEN, and this round is NOT TESTED live
// until the maintainer runs it.
//
// TWO MEASUREMENT RULES, both learned the expensive way:
//   * State is asserted on the STORE (RailLayoutStore::folders()/order()) and
//     on the model, never on a transient drag flag alone — a flag is what the
//     gesture is doing, the store is what it did.
//   * Item `y` is only read after a QTest::qWait past the 140 ms move/displaced
//     transition, and NO pixel colour is ever sampled: an offscreen grab holds
//     each item's creation-time colour because its Behavior animation has not
//     advanced (four rounds of false readings, 2026-08-25).
//
// FIXTURE NOTE. The rail is the real compiled SpacesRail.qml on a real
// AppController, so every `app.*` binding in it resolves exactly as in
// production. Only the SPACES are substituted: the mock backend ships a single
// Space, and a Space-onto-Space drop needs two. The test therefore hands
// AppController's own RailEntryModel a SpaceManager fed by a local client with
// three root Spaces (RailEntryModel::setSources), while leaving the store it
// writes to — AppController's own RailLayoutStore — untouched. The gesture,
// the view, the model and the store under test are all production code.

#include <QtTest/QtTest>

#include <functional>

#include <QGuiApplication>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "matrix/MatrixClient.h"
#include "spaces/RailEntryModel.h"
#include "spaces/RailLayoutStore.h"
#include "spaces/SpaceManager.h"

namespace {

constexpr int kSignalTimeoutMs = 5000;

// The rail's own geometry, restated here so the assertions can say what they
// mean. Every row is exactly its tile band tall while a drag is live (the
// revealed-rooms column is hidden for the duration), the tile is 40 px drawn
// at y = 4 inside it, and the group band is the middle 24 px of that tile.
// These are READ, never used to fabricate a coordinate: every point sent to
// the window comes from a real delegate's mapToScene().
constexpr int kTileTopInRow = 4;
constexpr int kTileHeight = 40;
constexpr int kTileCentreInRow = kTileTopInRow + kTileHeight / 2;  // 24
constexpr int kGroupBandTopInRow = 12;
constexpr int kGroupBandBottomInRow = 36;

// A client that answers with a fixed room list. A real SpaceManager needs a
// real MatrixClient, and the mock backend's single Space cannot express
// "drop this Space onto that one".
class RailFakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<RoomInfo> roomList;
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

RoomInfo joinedSpace(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.isSpace = true;
    info.membership = RoomInfo::Joined;
    return info;
}

RoomInfo joinedRoom(const QString &id, const QString &name)
{
    RoomInfo info;
    info.id = id;
    info.name = name;
    info.membership = RoomInfo::Joined;
    return info;
}

} // namespace

class RailDragQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    RailFakeClient *m_client = nullptr;
    SpaceManager *m_spaces = nullptr;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_rail = nullptr;
    QQuickItem *m_list = nullptr;
    QStringList m_spaceIds;

    RailEntryModel *entries() const { return m_controller->railEntries(); }
    RailLayoutStore *store() const { return m_controller->railLayout(); }

    // The live delegate for `entryId`, found by walking the ListView's
    // contentItem and matching the delegate's own `entryId` property. The
    // footer and any non-delegate children simply do not carry it.
    QQuickItem *delegateFor(const QString &entryId) const
    {
        auto *content = m_list->property("contentItem").value<QQuickItem *>();
        if (!content)
            return nullptr;
        const auto children = content->childItems();
        for (QQuickItem *child : children) {
            const QVariant id = child->property("entryId");
            if (id.isValid() && id.toString() == entryId)
                return child;
        }
        return nullptr;
    }

    // Scene y of the TOP of the row that holds `entryId`. Real geometry, so a
    // change to the rail's margins, spacing or band heights cannot silently
    // move every point this suite sends somewhere meaningless.
    qreal rowTopScene(const QString &entryId) const
    {
        QQuickItem *item = delegateFor(entryId);
        return item ? item->mapToScene(QPointF(0, 0)).y() : -1;
    }

    // The centre of a tile, which is where a person aims and — under the rule
    // being tested, and under neither of the two that preceded it — the middle
    // of the group band.
    QPoint tileCentre(const QString &entryId) const
    {
        QQuickItem *item = delegateFor(entryId);
        if (!item)
            return {};
        return item
            ->mapToScene(QPointF(item->width() / 2, kTileCentreInRow))
            .toPoint();
    }

    // A point in the GAP below `entryId`'s tile: past the group band's bottom
    // edge, in the 28 px of dead space (12 + 4 spacing + 12) that separates two
    // adjacent tiles.
    QPoint gapBelow(const QString &entryId) const
    {
        QQuickItem *item = delegateFor(entryId);
        if (!item)
            return {};
        return item
            ->mapToScene(QPointF(item->width() / 2,
                                 kGroupBandBottomInRow + 8))
            .toPoint();
    }

    bool pointerInsideTile(const QString &entryId, const QPoint &scenePos) const
    {
        const qreal top = rowTopScene(entryId);
        if (top < 0)
            return false;
        return scenePos.y() >= top + kGroupBandTopInRow
               && scenePos.y() < top + kGroupBandBottomInRow;
    }

    void pressAt(const QPoint &p)
    {
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, p);
        QCoreApplication::processEvents();
    }

    void moveTo(const QPoint &p)
    {
        QTest::mouseMove(m_window, p);
        QCoreApplication::processEvents();
    }

    void releaseAt(const QPoint &p)
    {
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, p);
        QCoreApplication::processEvents();
    }

    // `steps` interpolated moves from `from` (exclusive) to `to` (inclusive).
    // The SEQUENCE is the point: both retired rules survived a single move to
    // the destination and died on the second sample in the same place, so a
    // test that jumps straight to the target proves nothing.
    void sweep(const QPoint &from, const QPoint &to, int steps,
               const std::function<void(const QPoint &)> &afterEach = {})
    {
        for (int i = 1; i <= steps; ++i) {
            const QPoint p(from.x() + (to.x() - from.x()) * i / steps,
                           from.y() + (to.y() - from.y()) * i / steps);
            moveTo(p);
            if (afterEach)
                afterEach(p);
        }
    }

    QString folderIdOfOnlyFolder() const
    {
        const QVariantList folders = store()->folders();
        if (folders.size() != 1)
            return {};
        return folders.first().toMap().value(QStringLiteral("id")).toString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("rail-drag-qml-test"));
        QSettings().clear();

        // DragHandler's threshold defaults to the platform's start-drag
        // distance (10 px on most). Case 6 has to stay INSIDE a 24 px group
        // band while still crossing that threshold, which leaves no honest
        // margin at 10. Lowering it changes only how far the synthesized
        // pointer must travel before the handler takes the grab — never which
        // branch of the rail's reading a given position lands in, which is the
        // thing under test. Every case asserts the drag actually activated, so
        // a platform where this does not apply fails loudly rather than
        // passing vacuously.
        QGuiApplication::styleHints()->setStartDragDistance(4);

        m_client = new RailFakeClient(this);
        m_client->roomList = {
            joinedSpace(QStringLiteral("!space-alpha:example.org"),
                        QStringLiteral("Alpha")),
            joinedSpace(QStringLiteral("!space-bravo:example.org"),
                        QStringLiteral("Bravo")),
            joinedSpace(QStringLiteral("!space-charlie:example.org"),
                        QStringLiteral("Charlie")),
            // One room in no Space, so the rail also renders the "Other rooms"
            // pseudo row — production's second row, and an INELIGIBLE group
            // target sitting directly above the first Space.
            joinedRoom(QStringLiteral("!loose:example.org"),
                       QStringLiteral("Loose room")),
        };
        m_spaceIds = { QStringLiteral("!space-alpha:example.org"),
                       QStringLiteral("!space-bravo:example.org"),
                       QStringLiteral("!space-charlie:example.org") };

        m_spaces = new SpaceManager(this);
        m_spaces->setClient(m_client);
        QCOMPARE(m_spaces->spaceCount(), 3);

        m_controller = new AppController(AppController::MockBackend);
        // The rail's rows now come from THIS hierarchy; everything it writes
        // still goes to AppController's own RailLayoutStore, which is what the
        // assertions read.
        entries()->setSources(m_spaces, store());
        QCOMPARE(entries()->rowCount(), 5);   // Home, Other rooms, 3 Spaces

        m_engine = new QQmlApplicationEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine,
                              &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("SpacesRail"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_rail = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_rail);

        m_window = new QQuickWindow;
        // 68 px is the production rail width; 700 px is tall enough that the
        // five rows never make the ListView flickable — a scrollable list
        // would let the Flickable compete for the grab and would arm the
        // rail's auto-scroll, neither of which belongs in a drop test.
        //
        // If a future Qt lets the ListView steal the grab from the tile's
        // DragHandler under synthesized events, every case here fails on its
        // "the DragHandler never took the gesture" assertion rather than
        // quietly passing, and the harness fix is
        // `m_list->setProperty("interactive", false)` — which disables only
        // the rail's own flick-scrolling. A steal under a REAL pointer would
        // be a production finding, not a harness one, and must not be papered
        // over here.
        m_window->resize(68, 700);
        m_rail->setParentItem(m_window->contentItem());
        m_rail->setSize(QSizeF(m_window->width(), m_window->height()));
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window, 5000));
        m_window->requestActivate();
        QCoreApplication::processEvents();

        m_list = m_rail->findChild<QQuickItem *>(
            QStringLiteral("spacesRailList"));
        QVERIFY(m_list);
        QTRY_COMPARE_WITH_TIMEOUT(m_list->property("count").toInt(), 5, 5000);
        // Not scrolled, and not scrollable: the derived row tops the rail
        // computes and the real delegate geometry this suite reads are the
        // same coordinates only while contentY is 0.
        QCOMPARE(m_list->property("contentY").toReal(), 0.0);
        QVERIFY(m_list->property("contentHeight").toReal()
                <= m_list->property("height").toReal());
    }

    void cleanupTestCase()
    {
        // Order matters: the rail item's QObject owner is the engine (the
        // window only holds it as a visual child), so the engine goes first
        // and the item detaches itself from a window that is still alive.
        delete m_engine;
        delete m_window;
        delete m_controller;
    }

    // Every case starts from the canonical arrangement: no folders, no stored
    // order, Alpha directly above Bravo directly above Charlie.
    void init()
    {
        const QVariantList folders = store()->folders();
        for (const QVariant &value : folders) {
            store()->deleteFolder(
                value.toMap().value(QStringLiteral("id")).toString());
        }
        store()->setTopLevelOrder({});
        QCoreApplication::processEvents();
        QTRY_COMPARE(store()->folders().size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(m_list->property("count").toInt(), 5, 3000);

        const int rowA = entries()->rowForEntry(m_spaceIds.at(0));
        QVERIFY(rowA > 0);
        QCOMPARE(entries()->rowForEntry(m_spaceIds.at(1)), rowA + 1);
        QCOMPARE(entries()->rowForEntry(m_spaceIds.at(2)), rowA + 2);
        // Let the 140 ms move/displaced transitions from the previous case
        // finish before any geometry is read.
        QTest::qWait(200);
    }

    // CASE 1 — the gesture the whole feature exists for.
    //
    // ON THE UNFIXED TREE (0b38f8c and every revision before it) this produced
    // a SWAP and no folder: the second pointer sample inside Bravo's near half
    // called updateDrag(row, false), Alpha took Bravo's slot, and the release
    // committed a reorder. That swap IS the maintainer's report.
    void droppingASpaceOnASpaceCreatesAFolder()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QPoint from = tileCentre(a);
        const QPoint to = tileCentre(b);
        QVERIFY(!from.isNull() && !to.isNull());

        bool sawDragging = false;
        pressAt(from);
        sweep(from, to, 10, [&](const QPoint &) {
            sawDragging = sawDragging || entries()->dragging();
        });
        // A few small samples ON the target, because that is what a hand does
        // and because it is exactly what both retired rules could not survive:
        // one sample inside the tile was harmless, the second reordered.
        for (int i = 0; i < 4; ++i) {
            moveTo(QPoint(to.x(), to.y() + (i % 2 ? 2 : -2)));
            sawDragging = sawDragging || entries()->dragging();
        }
        QTest::qWait(300);
        QVERIFY2(sawDragging, "the DragHandler never took the gesture");
        releaseAt(to);
        QCoreApplication::processEvents();

        QTRY_COMPARE_WITH_TIMEOUT(store()->folders().size(), 1, 3000);
        const QString folderId = folderIdOfOnlyFolder();
        QVERIFY(!folderId.isEmpty());
        // TARGET FIRST, then the Space that was dropped: the folder takes over
        // the position the user was pointing at, and the dragged Space joins it
        // there (RailEntryModel::commitGrouping).
        QCOMPARE(store()->folderMembers(folderId), QStringList({ b, a }));
        QCOMPARE(store()->folderOf(a), folderId);
        QCOMPARE(store()->folderOf(b), folderId);
    }

    // CASE 2 — the direct assertion of the defect, and the one a model test can
    // never make: the tile being aimed at must not move out from under the
    // pointer. On the unfixed tree Bravo's row index changed on the SECOND
    // pointer sample inside its near half.
    void theTargetRowNeverMovesWhileThePointerIsOnItsTile()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const int rowBBefore = entries()->rowForEntry(b);
        const QPoint from = tileCentre(a);
        const QPoint to = tileCentre(b);

        int samplesInsideTile = 0;
        QList<int> readings;
        pressAt(from);
        sweep(from, to, 10, [&](const QPoint &p) {
            readings.append(entries()->rowForEntry(b));
            if (pointerInsideTile(b, p))
                ++samplesInsideTile;
        });
        for (int i = 0; i < 4; ++i) {
            const QPoint p(to.x(), to.y() + (i % 2 ? 2 : -2));
            moveTo(p);
            readings.append(entries()->rowForEntry(b));
            if (pointerInsideTile(b, p))
                ++samplesInsideTile;
        }
        QVERIFY2(entries()->dragging(),
                 "the DragHandler never took the gesture");
        releaseAt(to);
        QCoreApplication::processEvents();

        // Without this the case could pass by never reaching the target at
        // all — and it is the SECOND sample in the same place that killed both
        // earlier rules, so one is not enough.
        QVERIFY2(samplesInsideTile >= 2,
                 qPrintable(QStringLiteral("only %1 pointer samples landed "
                                           "inside the target tile")
                                .arg(samplesInsideTile)));
        for (qsizetype i = 0; i < readings.size(); ++i) {
            QVERIFY2(readings.at(i) == rowBBefore,
                     qPrintable(QStringLiteral("target row moved from %1 to %2 "
                                               "on pointer sample %3")
                                    .arg(rowBBefore)
                                    .arg(readings.at(i))
                                    .arg(i)));
        }
    }

    // CASE 3 — the decision survives to the release. `endDrag` groups on the
    // FLAG, not on where the pointer is, so what matters is the state the model
    // is holding at the instant the button comes up.
    void groupingIsArmedAndAimedAtTheTargetWhenTheButtonComesUp()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QPoint from = tileCentre(a);
        const QPoint to = tileCentre(b);

        pressAt(from);
        sweep(from, to, 10);
        moveTo(to);
        QTest::qWait(50);

        QVERIFY(entries()->dragging());
        QVERIFY2(entries()->grouping(),
                 "a pointer resting on a tile did not arm the group gesture");
        QCOMPARE(entries()->dropTargetId(), b);

        releaseAt(to);
        QCoreApplication::processEvents();
        // And the flags are cleared by the release itself — the rail draws the
        // ring off dropTargetId, and a released tile that keeps its drag
        // presentation was a real reported defect.
        QVERIFY(!entries()->dragging());
        QVERIFY(!entries()->grouping());
        QVERIFY(entries()->dropTargetId().isEmpty());
    }

    // CASE 4 — a release in the GAP below the target reorders and makes no
    // folder, EVEN AFTER the pointer has rested on that target on the way.
    // Leaving a tile has to disarm the grouping as well as unlight it, or a
    // stale flag turns a reorder into a folder.
    void aReleaseInTheGapBelowReordersAndMakesNoFolder()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QString c = m_spaceIds.at(2);
        const QPoint from = tileCentre(a);
        const QPoint onB = tileCentre(b);
        const QPoint intoGap = gapBelow(b);

        pressAt(from);
        sweep(from, onB, 10);
        QTest::qWait(300);              // rest on Bravo: grouping arms here
        QVERIFY(entries()->grouping());
        sweep(onB, intoGap, 4);
        QVERIFY2(!entries()->grouping(),
                 "leaving the tile left the group gesture armed");
        QVERIFY(entries()->dragging());
        releaseAt(intoGap);
        QCoreApplication::processEvents();

        QTest::qWait(200);              // past the 140 ms move transition
        QCOMPARE(store()->folders().size(), 0);
        // Alpha lands after Bravo; Charlie is untouched.
        QCOMPARE(store()->order(), QStringList({ b, a, c }));
        QCOMPARE(entries()->rowForEntry(b) + 1, entries()->rowForEntry(a));
    }

    // CASE 5 — sweeping THROUGH the target without stopping leaves no folder.
    // This is what replaces the 250 ms dwell: the dwell existed to stop a
    // pass-through from making a folder, and the geometry now carries that on
    // its own, because nothing moves while the pointer is on a tile and a gap
    // is never a group target.
    void sweepingThroughATileWithoutStoppingMakesNoFolder()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QString c = m_spaceIds.at(2);
        const QPoint from = tileCentre(a);
        const QPoint intoGap = gapBelow(b);

        pressAt(from);
        // One continuous run straight across Bravo's tile and out the far side,
        // with no pause anywhere.
        sweep(from, intoGap, 14);
        QVERIFY(entries()->dragging());
        QVERIFY(!entries()->grouping());
        releaseAt(intoGap);
        QCoreApplication::processEvents();

        QTest::qWait(200);
        QCOMPARE(store()->folders().size(), 0);
        QCOMPARE(store()->order(), QStringList({ b, a, c }));
    }

    // CASE 6 — a release over the dragged block's own slot changes nothing.
    // There is nothing to group with and nowhere new to go, so the rail must
    // hold everything still rather than pick the nearest verb.
    void aReleaseOverTheDraggedTilesOwnSlotChangesNothing()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QString c = m_spaceIds.at(2);
        const QPoint home = tileCentre(a);
        const int rowABefore = entries()->rowForEntry(a);
        const qreal yABefore = rowTopScene(a);

        pressAt(home);
        // 8 px of travel: past the drag threshold set in initTestCase, and
        // still inside the 24 px group band centred on this tile.
        moveTo(QPoint(home.x(), home.y() + 8));
        moveTo(QPoint(home.x(), home.y() + 4));
        moveTo(home);
        QVERIFY2(entries()->dragging(),
                 "the DragHandler never took the gesture");
        QVERIFY2(!entries()->grouping(),
                 "the tile in the user's own hand was offered as a group "
                 "target");
        QVERIFY(entries()->dropTargetId().isEmpty());
        releaseAt(home);
        QCoreApplication::processEvents();

        QTest::qWait(200);
        QCOMPARE(store()->folders().size(), 0);
        QCOMPARE(entries()->rowForEntry(a), rowABefore);
        QCOMPARE(entries()->rowForEntry(b), rowABefore + 1);
        QCOMPARE(entries()->rowForEntry(c), rowABefore + 2);
        QCOMPARE(rowTopScene(a), yABefore);
    }
};

int main(int argc, char *argv[])
{
    // Real Qt Quick item creation (even offscreen) needs a QGuiApplication,
    // matching main.cpp's application class exactly.
    QGuiApplication app(argc, argv);
    RailDragQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "RailDragQmlTest.moc"
