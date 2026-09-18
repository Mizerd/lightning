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

#include <algorithm>
#include <functional>

#include <QGuiApplication>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "auth/AuthManager.h"
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
    // `findChild` DOES NOT REACH A REPEATER'S DELEGATES — their QObject
    // parent is the Repeater's context, not the item they are laid out in —
    // so the revealed-rooms column is invisible to it. Walk the VISUAL tree.
    static QQuickItem *descendantNamed(QQuickItem *root, const QString &name)
    {
        if (!root)
            return nullptr;
        const auto children = root->childItems();
        for (QQuickItem *child : children) {
            if (child->objectName() == name)
                return child;
            if (QQuickItem *found = descendantNamed(child, name))
                return found;
        }
        return nullptr;
    }

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
        // 160, NOT the production MINIMUM of 68. The rail's indent budget is
        // half the leftover after the tile, so at 68 a step of 12 affords ONE
        // level — and the nesting cases below need three distinct indents to
        // be measuring anything. A user who wants that depth drags the rail
        // out to the stop that affords it; this window is that stop.
        m_window->resize(160, 700);
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

    // ── 2026-09-18: the expander sat nowhere near the tile it expands ───
    //
    // Reported in those words — "way too far off on the left, and unevenly
    // distanced" — and measured on a real build at the 112px stop before
    // touching anything: the gap between the chevron and its own tile ran
    // 20, 23, 26, 29, 32px down five levels of nesting, and even a top-level
    // Space sat 20px clear of its tile against the rail's edge.
    //
    // The glyph was `anchors.centerIn` its gutter, and the gutter is the
    // whole width left of the tile — so it sat at HALF the tile's own offset
    // and drifted half as fast as the thing it belongs to. It is anchored to
    // the gutter's right edge now, which is already a constant 4px from the
    // tile at every level.
    //
    // GEOMETRIC, on real delegates, because nothing else can see this: the
    // old arrangement was correct QML and read perfectly well as source.
    void theExpanderSitsTheSameDistanceFromItsTileAtEveryLevel()
    {
        RailFakeClient nested;
        RoomInfo lv0 = joinedSpace(QStringLiteral("!lv0:example.org"),
                                   QStringLiteral("Level 0"));
        lv0.childRoomIds = { QStringLiteral("!lv1:example.org") };
        RoomInfo lv1 = joinedSpace(QStringLiteral("!lv1:example.org"),
                                   QStringLiteral("Level 1"));
        lv1.childRoomIds = { QStringLiteral("!lv2:example.org") };
        RoomInfo lv2 = joinedSpace(QStringLiteral("!lv2:example.org"),
                                   QStringLiteral("Level 2"));
        lv2.childRoomIds = { QStringLiteral("!chan:example.org") };
        nested.roomList = { lv0, lv1, lv2,
                            joinedRoom(QStringLiteral("!chan:example.org"),
                                       QStringLiteral("Channel")) };
        SpaceManager nestedSpaces;
        nestedSpaces.setClient(&nested);
        for (const QString &id : { QStringLiteral("!lv0:example.org"),
                                   QStringLiteral("!lv1:example.org"),
                                   QStringLiteral("!lv2:example.org") }) {
            store()->setSpaceExpanded(id, true);
        }
        entries()->setSources(&nestedSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);

        QList<qreal> gaps;
        QList<qreal> tileLefts;
        for (const QString &id : { QStringLiteral("!lv0:example.org"),
                                   QStringLiteral("!lv1:example.org"),
                                   QStringLiteral("!lv2:example.org") }) {
            QQuickItem *row = delegateFor(id);
            QVERIFY2(row, qPrintable(QStringLiteral("no rail row for %1 — the "
                                                    "nested fixture did not "
                                                    "build").arg(id)));
            auto *glyph = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceExpandGlyph"));
            auto *tile = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceTile"));
            QVERIFY2(glyph && tile,
                     qPrintable(QStringLiteral("row %1 has no expander or no "
                                               "tile").arg(id)));
            QVERIFY2(glyph->width() > 0,
                     "the expander glyph has no width, so its position says "
                     "nothing");
            const QPointF glyphRight =
                glyph->mapToItem(row, QPointF(glyph->width(), 0));
            const QPointF tileLeft = tile->mapToItem(row, QPointF(0, 0));
            gaps << tileLeft.x() - glyphRight.x();
            tileLefts << tileLeft.x();
        }

        // The fixture has to actually NEST, or a constant gap is trivial.
        QVERIFY2(tileLefts.at(1) > tileLefts.at(0)
                     && tileLefts.at(2) > tileLefts.at(1),
                 "the three rows are at the same indent, so this case cannot "
                 "see a gap that grows with depth");

        for (int i = 0; i < gaps.size(); ++i) {
            QVERIFY2(gaps.at(i) >= 0,
                     qPrintable(QStringLiteral("the expander overlaps its own "
                                               "tile at level %1 (gap %2)")
                                    .arg(i).arg(gaps.at(i))));
            QVERIFY2(qAbs(gaps.at(i) - gaps.at(0)) < 1.0,
                     qPrintable(QStringLiteral(
                         "the expander is %1px from its tile at level %2 and "
                         "%3px at level 0 — it drifts at a different rate "
                         "from the tile it expands, so no two levels are "
                         "spaced alike")
                         .arg(gaps.at(i)).arg(i).arg(gaps.at(0))));
        }
        // ...and it is CLOSE to it, not parked against the rail's edge. Half
        // a tile is generous and still catches the old arrangement, whose
        // gap was 20px against a 40px tile at the very first level.
        QVERIFY2(gaps.at(0) < 20.0,
                 qPrintable(QStringLiteral("the expander sits %1px from its "
                                           "tile — far enough to read as "
                                           "belonging to the rail rather than "
                                           "to the Space").arg(gaps.at(0))));

        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
    }


    // ── 2026-09-17: expanding a LEAF Space revealed nothing until the rail
    //    was rebuilt by something else ─────────────────────────────────────
    //
    // Reproduced on a real account: a Discord-style category — rooms, no
    // subspaces — got its chevron from 143abb07, flipped it to "open" on a
    // click, and listed none of its rooms. Collapsing and re-expanding an
    // UNRELATED Space above it made them appear; so did restarting the app,
    // which is how it was first reported ("a room did not appear under its
    // space until Lightning restarted") and why it was filed as a sync
    // staleness bug. It is not one. The state was in the store the whole
    // time: Space Home listed both rooms while the rail showed neither.
    //
    // `revealed` called `root.revealCount(spaceId)`, which asks
    // `app.railLayout.spaceExpanded(spaceId)` — a Q_INVOKABLE, so the binding
    // records NO dependency on the expansion state and never re-evaluates
    // when it changes. Expanding a Space that HAS subspaces inserts model
    // rows, which rebuilds delegates and hides the defect; a leaf inserts
    // none, so only its `expanded` ROLE changes — the chevron reads that role
    // and flips, and the reveal, which did not, stays at its creation-time
    // zero. Exactly the shape the space-identity suite was written for:
    // "neither half can see a binding that never re-evaluates, which is what
    // `root.info` (a spaceInfo() CALL) was".
    //
    // Substitutes its own hierarchy and puts the shared one back, so the drag
    // cases above are untouched whatever order this file is run in.
    void expandingALeafSpaceRevealsItsRoomsWithoutRebuildingTheRail()
    {
        // Production wiring, not the substituted hierarchy the drag cases
        // use: `topRoomsInSpace()` reads `app.spaces`, which is the
        // controller's OWN SpaceManager, so a locally-fed model would leave
        // the reveal empty for a reason that has nothing to do with the bug.
        // That manager is empty until the mock account is logged in — the
        // drag cases never needed it and so never did.
        QSignalSpy loginSpy(m_controller->auth(),
                            &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTest::qWait(200);
        entries()->setSources(m_controller->spaces(), store());
        QCoreApplication::processEvents();
        QTest::qWait(50);

        // A LEAF: direct rooms to reveal, and no subspaces whose insertion
        // would rebuild the delegate for us. Discovered from the live model
        // rather than pinned to a mock id, so a change to the mock's
        // hierarchy fails this loudly instead of silently testing nothing.
        SpaceManager *spaces = m_controller->spaces();
        QString leafId;
        int leafRooms = 0;
        const QVariantList all = spaces->allSpaces();
        for (const QVariant &entry : all) {
            const QVariantMap row = entry.toMap();
            const QString id =
                row.value(QStringLiteral("spaceId")).toString();
            if (id.isEmpty()
                || row.value(QStringLiteral("childSpaceCount")).toInt() > 0) {
                continue;
            }
            const int rooms = spaces->directChildRoomsDetailed(id).size();
            if (rooms > 0) {
                leafId = id;
                leafRooms = rooms;
                break;
            }
        }
        QVERIFY2(!leafId.isEmpty(),
                 "the fixture has no Space with rooms and no subspaces, so "
                 "this case cannot exercise the leaf path at all");

        store()->setSpaceExpanded(leafId, false);
        QCoreApplication::processEvents();
        QTest::qWait(50);

        QQuickItem *tile = delegateFor(leafId);
        QVERIFY2(tile, "the leaf Space has no delegate in the rail at all");
        QCOMPARE(tile->property("expandable").toBool(), true);
        QCOMPARE(tile->property("revealed").toInt(), 0);

        // Production's own toggle — the chevron's TapHandler calls exactly
        // this. It inserts and removes no model row, which is the whole
        // point: nothing else can rebuild the delegate for us.
        const int rowsBefore = entries()->rowCount();
        store()->toggleSpaceExpanded(leafId);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCOMPARE(entries()->rowCount(), rowsBefore);

        tile = delegateFor(leafId);
        QVERIFY(tile);
        // The role the chevron reads DID update — that is why the control
        // looked like it worked.
        QCOMPARE(tile->property("expanded").toBool(), true);
        QVERIFY2(tile->property("revealed").toInt() > 0,
                 "the chevron opened and the reveal count stayed at its "
                 "creation-time zero: a leaf Space inserts no rows, so "
                 "nothing rebuilds the delegate and the rooms appear only "
                 "after an unrelated toggle or an app restart");
        QCOMPARE(tile->property("revealedRooms").toList().size(), leafRooms);

        // ...and closing it puts them away again, through the same binding.
        store()->toggleSpaceExpanded(leafId);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        tile = delegateFor(leafId);
        QVERIFY(tile);
        QCOMPARE(tile->property("revealed").toInt(), 0);

        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
    }

    // ── 2026-09-18: the rail scaled its Space tiles and NOTHING ELSE ──────
    //
    // The Space tile started following the interface size earlier the same
    // day — it had been a flat 40 while `normalRowBand`, `indentStep` and
    // `minRailWidth` around it were already scaled, which is exactly why a
    // 140% rail grew wider while the tiles inside it did not. Fixing that
    // exposed the other half: every REMAINING piece of rail geometry was
    // still a literal, so at 140% a 56px Space tile sat above 28px room
    // tiles, a 40px settings cog and a 40px account avatar, and the column
    // read as three unrelated controls stacked on one another.
    //
    // WHAT IS ASSERTED IS A RATIO, NEVER A PIXEL COUNT. "56 at 140%" is a
    // number someone edits to match whatever the build produces; that a
    // revealed room's tile stays 0.7 of the Space tile above it, and that
    // the bottom cluster's chips stay exactly one Space tile, is the rule
    // the rail is supposed to obey and no frozen literal can satisfy it.
    //
    // Production wiring, for the reason the leaf case above documents: the
    // revealed-rooms column reads `app.spaces`, the CONTROLLER's manager, so
    // a locally-fed hierarchy would leave it empty for a reason that has
    // nothing to do with scaling.
    // ── 2026-09-18: a tree deeper than the rail can draw ─────────────────
    //
    // The tree costs one indent step a level, so a narrow rail runs out of
    // depth long before the hierarchy does. What used to happen then was the
    // worst available option: `tileIndent` clamps at `indentBudget`, so every
    // row past the budget was drawn at the SAME indent as its own parent —
    // two rows side by side claiming to be siblings when one contains the
    // other.
    //
    // The rail dives instead: the ancestor that brings the deepest row back
    // inside the budget becomes the trunk, its subtree is drawn from there,
    // and a chip at the top says what to click to come back.
    //
    // THE INVARIANT IS WHAT IS ASSERTED, not the choice of trunk: whatever
    // the rail is showing, no visible row may be drawn deeper than the rail
    // can draw. A screenshot can suggest that; only this can hold it.
    void aTreeTooDeepToDrawDivesInsteadOfPilingUp()
    {
        RailFakeClient deep;
        QList<RoomInfo> rooms;
        QStringList chain;
        // Six levels, each the only child of the one above — the shape the
        // maintainer's own fixture has and the one that overflows soonest.
        for (int i = 0; i < 6; ++i)
            chain << QStringLiteral("!lvl%1:example.org").arg(i);
        for (int i = 0; i < chain.size(); ++i) {
            RoomInfo info = joinedSpace(chain.at(i),
                                        QStringLiteral("Level %1").arg(i));
            if (i + 1 < chain.size())
                info.childRoomIds = { chain.at(i + 1) };
            rooms << info;
        }
        deep.roomList = rooms;
        SpaceManager deepSpaces;
        deepSpaces.setClient(&deep);
        for (const QString &id : chain)
            store()->setSpaceExpanded(id, true);
        entries()->setSources(&deepSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(120);

        const int drawable = m_rail->property("drawableLevels").toInt();
        QVERIFY2(drawable >= 1 && drawable < 5,
                 qPrintable(QStringLiteral(
                     "the rail claims it can draw %1 levels at this width; "
                     "the fixture needs it to run out before six or this "
                     "case is not exercising the dive at all").arg(drawable)));

        // The rail must have dived, and the fixture must be deep enough to
        // have forced it.
        QTRY_VERIFY_WITH_TIMEOUT(
            !m_rail->property("treeFocusId").toString().isEmpty(),
            kSignalTimeoutMs);

        // Every row still on screen is inside the budget. `drawnTreeLevel`
        // is the clamp itself, so this reads `drawnLevel` — what the row
        // WANTS to be drawn at — and requires the clamp never to be needed.
        auto *content =
            m_list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        int visibleRows = 0;
        int deepest = 0;
        const auto children = content->childItems();
        for (QQuickItem *child : children) {
            if (!child->isVisible() || child->height() <= 0)
                continue;
            const QVariant level = child->property("drawnLevel");
            if (!level.isValid())
                continue;
            ++visibleRows;
            deepest = std::max(deepest, level.toInt());
        }
        QVERIFY2(visibleRows > 1,
                 "the dive left one row or none on screen, which is not a "
                 "view of a subtree — it is an empty rail");
        QVERIFY2(deepest <= drawable,
                 qPrintable(QStringLiteral(
                     "a row is drawn %1 levels deep in a rail that can draw "
                     "%2, so it shares an indent with its own parent and the "
                     "two read as siblings").arg(deepest).arg(drawable)));

        m_rail->setProperty("treeFocusId", QString());
        for (const QString &id : chain)
            store()->setSpaceExpanded(id, false);
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }

    void everyRailChipFollowsTheInterfaceSize()
    {
        auto *theme = m_engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY2(theme, "no AppTheme singleton — this case cannot change the "
                        "interface size and so proves nothing");
        const qreal originalScale = theme->property("textScale").toReal();

        if (!m_controller->property("loggedIn").toBool()) {
            QSignalSpy loginSpy(m_controller->auth(),
                                &AuthManager::loginSucceeded);
            m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                        QStringLiteral("alice"),
                                        QStringLiteral("unused"));
            QVERIFY(loginSpy.wait(kSignalTimeoutMs));
            QTest::qWait(200);
        }
        entries()->setSources(m_controller->spaces(), store());
        QCoreApplication::processEvents();
        QTest::qWait(50);

        // Whatever happens below, the shared engine goes back to 100% and the
        // suite's own hierarchy goes back on the rail — every other case here
        // reads geometry from both.
        const auto restore = qScopeGuard([&] {
            theme->setProperty("textScale", originalScale);
            entries()->setSources(m_spaces, store());
            QCoreApplication::processEvents();
            QTest::qWait(60);
        });

        // Discovered from the live model, never pinned to a mock id.
        SpaceManager *spaces = m_controller->spaces();
        QString hostId;
        const QVariantList all = spaces->allSpaces();
        for (const QVariant &entry : all) {
            const QString id =
                entry.toMap().value(QStringLiteral("spaceId")).toString();
            if (!id.isEmpty()
                && !spaces->directChildRoomsDetailed(id).isEmpty()) {
                hostId = id;
                break;
            }
        }
        QVERIFY2(!hostId.isEmpty(),
                 "the fixture has no Space with direct rooms, so the "
                 "expansion column cannot be measured at all");
        store()->setSpaceExpanded(hostId, true);
        QCoreApplication::processEvents();
        QTest::qWait(60);

        // Four chips, read from the live item tree at each size.
        const auto measure = [&](const char *when) -> QList<qreal> {
            QQuickItem *row = delegateFor(hostId);
            if (!row) {
                qWarning("no rail row for the scaling fixture (%s)", when);
                return {};
            }
            auto *tile = descendantNamed(
                row, QStringLiteral("railSpaceTile"));
            auto *roomTile = descendantNamed(
                row, QStringLiteral("railRevealedRoomTile"));
            auto *cog = descendantNamed(
                m_rail, QStringLiteral("railSettingsButton"));
            auto *account = descendantNamed(
                m_rail, QStringLiteral("railAccountTile"));
            if (!tile || !roomTile || !cog || !account) {
                qWarning("rail chips missing at %s: tile=%d room=%d cog=%d "
                         "account=%d", when, tile != nullptr,
                         roomTile != nullptr, cog != nullptr,
                         account != nullptr);
                return {};
            }
            return { tile->width(), roomTile->width(), cog->width(),
                     account->width() };
        };

        const QList<qreal> at100 = measure("100%");
        QCOMPARE(at100.size(), 4);
        for (const qreal w : at100)
            QVERIFY2(w > 0, "a rail chip has no width at 100%");

        theme->setProperty("textScale", 1.4);
        QCoreApplication::processEvents();
        QTest::qWait(100);

        const QList<qreal> at140 = measure("140%");
        QCOMPARE(at140.size(), 4);

        static const char *const kNames[] = { "the Space tile",
                                              "a revealed room's tile",
                                              "the settings cog",
                                              "the account avatar" };
        for (int i = 0; i < 4; ++i) {
            QVERIFY2(at140.at(i) > at100.at(i),
                     qPrintable(QStringLiteral(
                         "%1 is %2px at 100%% and %3px at 140%% — it does not "
                         "follow the interface size, so it sits beside chips "
                         "that do")
                         .arg(QString::fromLatin1(kNames[i]))
                         .arg(at100.at(i)).arg(at140.at(i))));
        }

        // The ratios that make the rail read as one column. One pixel of
        // slack for Math.round, and no more.
        for (const QList<qreal> &m : { at100, at140 }) {
            const qreal tile = m.at(0);
            QVERIFY2(qAbs(m.at(1) - qRound(tile * 0.7)) <= 1.0,
                     qPrintable(QStringLiteral(
                         "a revealed room's tile is %1px under a %2px Space "
                         "tile — expected %3, so the expansion column no "
                         "longer reads as that Space's contents")
                         .arg(m.at(1)).arg(tile).arg(qRound(tile * 0.7))));
            QVERIFY2(qAbs(m.at(2) - tile) <= 1.0,
                     qPrintable(QStringLiteral(
                         "the settings cog is %1px against a %2px Space tile")
                         .arg(m.at(2)).arg(tile)));
            QVERIFY2(qAbs(m.at(3) - tile) <= 1.0,
                     qPrintable(QStringLiteral(
                         "the account avatar is %1px against a %2px Space "
                         "tile").arg(m.at(3)).arg(tile)));
        }
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
