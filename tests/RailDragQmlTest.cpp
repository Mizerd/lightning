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
#include <QColor>
#include <cmath>
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

    // The layered group field draws one rectangle PER ANCESTOR under the
    // same objectName, so `descendantNamed` — which stops at the first — can
    // only ever see the outermost. Collect them all.
    static void collectDescendantsNamed(QQuickItem *root, const QString &name,
                                        QList<QQuickItem *> &out)
    {
        if (!root)
            return;
        const auto children = root->childItems();
        for (QQuickItem *child : children) {
            if (child->objectName() == name)
                out << child;
            collectDescendantsNamed(child, name, out);
        }
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
        // 160, which is WIDER than the rail's own maximum. That is deliberate
        // and it is also a blind spot: geometry that only goes wrong when the
        // gutter is at its narrowest cannot be seen at 160, which is how a
        // clipped chevron shipped. `theGutterIsWideEnoughForTheGlyphItHolds`
        // below narrows the rail to its real minimum for exactly that reason,
        // and every other case here is about proportion rather than fit.
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
    void nestingMovesTheTreeAndNotTheTiles()
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
            const QPointF glyphLeft = glyph->mapToItem(row, QPointF(0, 0));
            const QPointF tileLeft = tile->mapToItem(row, QPointF(0, 0));
            // Both absolute in the row, because both belong to a COLUMN now:
            // the expander to the gutter, the tile to the one axis every
            // tile shares. Measuring the expander from the tile would have
            // been the right question while it was a badge on the tile, and
            // it is the wrong one once the two are separate columns.
            gaps << glyphLeft.x();
            // The tile's CENTRE. A nested tile is drawn one step smaller and
            // inset equally on both sides, so its LEFT edge legitimately moves
            // by half the size difference — the centre is what "one column"
            // means when the things in it are not all the same size.
            tileLefts << tileLeft.x() + tile->width() / 2;
        }

        // ── THE TILES DO NOT MOVE ────────────────────────────────────
        //
        // This case used to require the OPPOSITE — that each level's tile sat
        // further right than the one above — and that requirement was the
        // defect. Reported as "they keep sticking out more and more and create
        // like a wave pattern": a per-level step walks a 40px tile off its own
        // axis inside a rail that starts at 68px, and then walks it back, so
        // the column has no baseline anywhere. Depth moved into lanes in a
        // fixed gutter and the tiles stay put. Element renders no nesting at
        // all while narrow and Nheko multiplies its indent by zero — the same
        // conclusion reached twice by people who shipped it.
        for (int i = 1; i < tileLefts.size(); ++i) {
            QVERIFY2(qAbs(tileLefts.at(i) - tileLefts.at(0)) < 1.0,
                     qPrintable(QStringLiteral(
                         "a depth-%1 tile is centred on x=%2 and a root on "
                         "x=%3 — the column steps with depth again, which is "
                         "the wave this case exists to prevent")
                         .arg(i).arg(tileLefts.at(i)).arg(tileLefts.at(0))));
        }

        // And the expander keeps ONE x at every depth — the other half of the
        // original report, which was that the chevrons were "unevenly
        // distanced". It has the gutter to itself, so there is nothing left
        // for it to move for.
        for (int i = 0; i < gaps.size(); ++i) {
            QVERIFY2(qAbs(gaps.at(i) - gaps.at(0)) < 1.0,
                     qPrintable(QStringLiteral(
                         "the expander sits at x=%1 at depth %2 and x=%3 at "
                         "the root — it moves with the row rather than "
                         "keeping the gutter's one position")
                         .arg(gaps.at(i)).arg(i).arg(gaps.at(0))));
            // In the gutter, which is to say LEFT of the tile column. This is
            // the assertion that would fail if the expander were ever moved
            // back on top of the tile, where it clipped the avatar.
            QVERIFY2(gaps.at(i) < tileLefts.at(i),
                     qPrintable(QStringLiteral(
                         "the expander is at x=%1 and the tile's centre is at "
                         "x=%2 — it is not in the gutter it was given")
                         .arg(gaps.at(i)).arg(tileLefts.at(i))));
        }

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

        // ── AND A ROOM IS ACTUALLY DRAWN ────────────────────────────────
        //
        // Everything above this reads MODEL properties, and that is how the
        // whole revealed-rooms column went missing in a shipped build while
        // this case — and a full CTest run — stayed green. A careless edit
        // deleted the column's `visible`, `y`, `width` and `spacing`; a
        // Column with no width lays out nothing, so no Space revealed any
        // room anywhere, and `revealed` went on reporting a happy number the
        // whole time.
        //
        // The size is asserted as a RATIO of the Space tile, not a literal:
        // a room tier is 0.7 of it, which is what says "this is a room and
        // that is a Space" once the indent was taken away. And the x, because
        // one shared axis is the rail's whole layout rule.
        QQuickItem *roomTile = descendantNamed(
            tile, QStringLiteral("railRevealedRoomTile"));
        QVERIFY2(roomTile, "the reveal count is non-zero and NO room tile "
                           "exists in the delegate — the column is laying out "
                           "nothing");
        QVERIFY2(roomTile->width() > 1 && roomTile->height() > 1,
                 qPrintable(QStringLiteral(
                     "a revealed room tile is %1x%2 — it is in the tree and "
                     "has no size, so nothing is on screen")
                     .arg(roomTile->width()).arg(roomTile->height())));
        const qreal spaceTileSize =
            m_rail->property("railTileSize").toReal();
        const qreal ratio = roomTile->width() / spaceTileSize;
        QVERIFY2(qAbs(ratio - 0.7) < 0.04,
                 qPrintable(QStringLiteral(
                     "a revealed room tile is %1 against a Space tile of %2 — "
                     "a ratio of %3 where the rail's one size cue is 0.7")
                     .arg(roomTile->width()).arg(spaceTileSize).arg(ratio)));
        const qreal roomCentre =
            roomTile->mapToItem(m_rail, QPointF(0, 0)).x()
            + roomTile->width() / 2;
        const qreal columnCentre =
            m_rail->property("tileColumnX").toReal() + spaceTileSize / 2;
        QVERIFY2(qAbs(roomCentre - columnCentre) < 1.0,
                 qPrintable(QStringLiteral(
                     "a revealed room is centred on x=%1 and every other tile "
                     "on x=%2 — the column has two axes")
                     .arg(roomCentre).arg(columnCentre)));

        // AND IT IS BELOW THE SPACE TILE, which is the assertion that
        // actually discriminates. A first version checked the tile's size and
        // x and PASSED on the broken code: a room tile carries its own width
        // and its own absolute x, so it keeps both even when the column
        // around it has neither. What the column owns is WHERE the run
        // starts — strip its `y` and every revealed room is drawn on top of
        // the Space tile it belongs to.
        const qreal bandHeight =
            tile->property("tileBandHeight").toReal();
        const qreal roomTop = roomTile->mapToItem(tile, QPointF(0, 0)).y();
        QVERIFY2(roomTop >= bandHeight - 1,
                 qPrintable(QStringLiteral(
                     "a revealed room starts at y=%1 inside a tile band %2 "
                     "tall — the rooms are drawn over the Space that owns "
                     "them").arg(roomTop).arg(bandHeight)));

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
    // ── 2026-09-18: a drop lands where `rowTop` says it does ─────────────
    //
    // `rowTop(i)` is what every drop decision is made against, and it is
    // DERIVED by accumulating `rowBand()` rather than read off the delegates,
    // because the move and displaced transitions interpolate a delegate's `y`
    // for 140ms — a pointer held still over an animating list would map to one
    // row, then its neighbour, then back.
    //
    // THE PRICE OF DERIVING IT is that the derivation has to keep up with the
    // rows. It returned one constant for every row but the first, which was
    // exactly true while every tile was `railTileSize`, and stopped being true
    // the moment a nested Space's tile became a step smaller and the last row
    // of a group started carrying the gap below it. Nothing would have said
    // so: the error is 6px per nested row and 8px per group ABOVE the pointer,
    // so shallow trees are unaffected and a deep one drops a slot off.
    //
    // THE FIXTURE NEEDS A ROW THAT IS NESTED AND NOT LAST, and a first
    // version of it did not have one. Two roots each with a single nested
    // child passed on a constant band by arithmetic accident: a nested tile
    // is 8px shorter and the last row of a group is 8px taller, so at every
    // width those two cancel and a nested LAST row is exactly the constant.
    // The middle row of a three-level chain is the one that cannot cancel.
    void everyRowTopMatchesTheRowThatIsActuallyThere()
    {
        RailFakeClient mixed;
        RoomInfo r1 = joinedSpace(QStringLiteral("!r1:example.org"),
                                  QStringLiteral("Root One"));
        r1.childRoomIds = { QStringLiteral("!c1:example.org") };
        RoomInfo c1 = joinedSpace(QStringLiteral("!c1:example.org"),
                                  QStringLiteral("Child One"));
        c1.childRoomIds = { QStringLiteral("!c1a:example.org") };
        RoomInfo r2 = joinedSpace(QStringLiteral("!r2:example.org"),
                                  QStringLiteral("Root Two"));
        r2.childRoomIds = { QStringLiteral("!c2:example.org") };
        mixed.roomList = { r1, c1,
                           joinedSpace(QStringLiteral("!c1a:example.org"),
                                       QStringLiteral("Grandchild")),
                           r2,
                           joinedSpace(QStringLiteral("!c2:example.org"),
                                       QStringLiteral("Child Two")) };
        SpaceManager mixedSpaces;
        mixedSpaces.setClient(&mixed);
        store()->setSpaceExpanded(QStringLiteral("!r1:example.org"), true);
        store()->setSpaceExpanded(QStringLiteral("!c1:example.org"), true);
        store()->setSpaceExpanded(QStringLiteral("!r2:example.org"), true);
        entries()->setSources(&mixedSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(80);

        auto *content = m_list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const int count = m_list->property("count").toInt();
        QVERIFY2(count >= 5,
                 qPrintable(QStringLiteral(
                     "the rail built %1 rows, so the mixed fixture is not "
                     "there").arg(count)));

        // The fixture must actually contain a nested row, or a constant band
        // would be correct and this case would prove nothing.
        int nestedRows = 0;
        for (QQuickItem *row : content->childItems()) {
            if (row && row->isVisible()
                && row->property("hierarchyChild").toBool())
                ++nestedRows;
        }
        QVERIFY2(nestedRows >= 3,
                 qPrintable(QStringLiteral(
                     "only %1 nested rows are on screen — a constant row band "
                     "would pass this case").arg(nestedRows)));

        for (QQuickItem *row : content->childItems()) {
            if (!row || !row->isVisible() || row->height() <= 0)
                continue;
            bool ok = false;
            const int index = row->property("index").toInt(&ok);
            if (!ok || index < 0)
                continue;
            QVariant predicted;
            QMetaObject::invokeMethod(m_rail, "rowTop",
                                      Q_RETURN_ARG(QVariant, predicted),
                                      Q_ARG(QVariant, index));
            QVERIFY2(qAbs(predicted.toReal() - row->y()) < 1.0,
                     qPrintable(QStringLiteral(
                         "row %1 is at y=%2 and `rowTop` predicts %3 — every "
                         "drop decision on this row is made against the wrong "
                         "slot").arg(index).arg(row->y())
                         .arg(predicted.toReal())));
        }

        store()->setSpaceExpanded(QStringLiteral("!r1:example.org"), false);
        store()->setSpaceExpanded(QStringLiteral("!c1:example.org"), false);
        store()->setSpaceExpanded(QStringLiteral("!r2:example.org"), false);
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }


    // ── 2026-09-18: a folder painted its own contents flat ───────────────
    //
    // The folder container was drawn at `z: -2` while the hierarchy regions
    // live at -21..-17, in a colour those regions already used — so a Space
    // tree filed into a folder had its whole nesting painted OVER. Measured
    // on a capture: inside a folder, exactly one region tint appeared in the
    // entire rail, on a three-deep tree with four open chevrons proving the
    // app knew it was a tree. Nothing was wrong with the model.
    //
    // Two things had to be true and neither was: the container has to be
    // BEHIND the regions it contains, and it has to be WIDER than them. It
    // was inset by a raw literal against a scaled ladder, which put it
    // between depth 1 and depth 2 — a container narrower than its contents.
    //
    // A COLOUR CENSUS IS THE ONLY THING THAT SEES THE FIRST HALF from a
    // screenshot, and no source scan sees either: both bindings read
    // perfectly well, and the defect is entirely in how two numbers written
    // in different places compare.
    void aFolderDrawsBehindTheHierarchyItHolds()
    {
        RailFakeClient tree;
        RoomInfo top = joinedSpace(QStringLiteral("!filed:example.org"),
                                   QStringLiteral("Filed"));
        top.childRoomIds = { QStringLiteral("!sub:example.org") };
        RoomInfo sub = joinedSpace(QStringLiteral("!sub:example.org"),
                                   QStringLiteral("Sub"));
        sub.childRoomIds = { QStringLiteral("!leaf:example.org") };
        tree.roomList = { top, sub,
                          joinedSpace(QStringLiteral("!leaf:example.org"),
                                      QStringLiteral("Leaf")) };
        SpaceManager treeSpaces;
        treeSpaces.setClient(&tree);
        store()->setSpaceExpanded(QStringLiteral("!filed:example.org"), true);
        store()->setSpaceExpanded(QStringLiteral("!sub:example.org"), true);
        entries()->setSources(&treeSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(80);

        const QString folderId = store()->createFolderWithSpaces(
            { QStringLiteral("!filed:example.org") }, -1, QString());
        QVERIFY2(!folderId.isEmpty(), "the fixture could not create a folder");
        QCoreApplication::processEvents();
        QTest::qWait(120);

        QQuickItem *subRow = delegateFor(QStringLiteral("!sub:example.org"));
        QVERIFY2(subRow, "the filed Space's subspace has no row, so the "
                         "fixture is not a tree inside a folder");
        QVERIFY2(subRow->property("inFolder").toBool(),
                 "the subspace row does not consider itself filed, so this "
                 "case is not measuring the folder path at all");

        QList<QQuickItem *> layers;
        collectDescendantsNamed(subRow, QStringLiteral("railGroupField"),
                                layers);
        QVERIFY2(!layers.isEmpty(),
                 "a filed Space's subspace draws no hierarchy region at all");

        auto *container = subRow->findChild<QQuickItem *>(
            QStringLiteral("railFolderContainer"));
        QVERIFY2(container && container->isVisible(),
                 "the filed row draws no folder container");

        // BEHIND. Equal z would not do either: same-z siblings paint in
        // document order, and the container is declared first.
        QVERIFY2(container->z() < layers.at(0)->z(),
                 qPrintable(QStringLiteral(
                     "the folder container is at z=%1 and the region it "
                     "contains at z=%2 — the container paints over the "
                     "nesting inside it")
                     .arg(container->z()).arg(layers.at(0)->z())));

        // AND WIDER. A container narrower than its contents is backwards,
        // and it is what turned the boundary between two groups into four
        // corner arcs and a hairline.
        QVERIFY2(container->width() > layers.at(0)->width(),
                 qPrintable(QStringLiteral(
                     "the folder container is %1px wide and the depth-1 "
                     "region inside it is %2px — the container is narrower "
                     "than the thing it holds")
                     .arg(container->width()).arg(layers.at(0)->width())));

        // AND THE TILE DOES NOT MOVE. The folder path had the one horizontal
        // offset left in this file, 7.5px measured, in a rail whose whole
        // rule is that tiles share an axis.
        QQuickItem *unfiled = delegateFor(QStringLiteral("!leaf:example.org"));
        auto *filedTile = subRow->findChild<QQuickItem *>(
            QStringLiteral("railSpaceTile"));
        QQuickItem *unfiledTile =
            unfiled ? unfiled->findChild<QQuickItem *>(
                          QStringLiteral("railSpaceTile"))
                    : nullptr;
        if (filedTile && unfiledTile) {
            const qreal filedCentre =
                filedTile->mapToItem(m_rail, QPointF(0, 0)).x()
                + filedTile->width() / 2;
            const qreal freeCentre =
                unfiledTile->mapToItem(m_rail, QPointF(0, 0)).x()
                + unfiledTile->width() / 2;
            QVERIFY2(qAbs(filedCentre - freeCentre) < 1.0,
                     qPrintable(QStringLiteral(
                         "a filed tile is centred on x=%1 and an unfiled one "
                         "on x=%2 — the folder path has its own indent again")
                         .arg(filedCentre).arg(freeCentre)));
        }

        store()->deleteFolder(folderId);
        store()->setSpaceExpanded(QStringLiteral("!filed:example.org"), false);
        store()->setSpaceExpanded(QStringLiteral("!sub:example.org"), false);
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }


    // ── 2026-09-18: sibling runs touched, and the chevron sat outside ─────
    //
    // Reported against a capture, in two parts. "The lowest level runs out of
    // color, there are small gaps between them where the color should end, we
    // want to separate it cleanly": two sibling runs of the same tint,
    // separated by nothing but `ListView.spacing`, read as ONE shape with a
    // hairline notch through it — because the trailing gap was keyed on the
    // TOP-LEVEL group ending and an inner run ending spent nothing. And
    // "chevrons should be repositioned so they are in the color shape, not in
    // between them": a deep row's innermost region begins further right than
    // a glyph right-anchored to the tile does, so the chevron sat in the
    // PARENT's band beside its own box.
    //
    // BOTH ARE GEOMETRY AND NOTHING ELSE CAN SEE EITHER. A source scan reads
    // the same correct-looking bindings before and after, and the second one
    // is a two-pixel disagreement between two numbers written in different
    // files.
    //
    // The fixture is two sibling runs under one Root — the smallest
    // arrangement in which an inner run ends with something after it — and
    // the left one is THREE deep on purpose. At two levels the innermost
    // region's edge is still left of a tile-anchored glyph, so the chevron
    // half of this case passes on the old gutter; only the deepest inset the
    // rail can draw puts the two numbers in conflict.
    void siblingRunsSeparateWhileTheirParentStaysWhole()
    {
        RailFakeClient tree;
        RoomInfo root = joinedSpace(QStringLiteral("!root:example.org"),
                                    QStringLiteral("Root"));
        root.childRoomIds = { QStringLiteral("!a:example.org"),
                              QStringLiteral("!b:example.org") };
        RoomInfo a = joinedSpace(QStringLiteral("!a:example.org"),
                                 QStringLiteral("A"));
        a.childRoomIds = { QStringLiteral("!a1:example.org") };
        RoomInfo aChild = joinedSpace(QStringLiteral("!a1:example.org"),
                                      QStringLiteral("A1"));
        aChild.childRoomIds = { QStringLiteral("!a1a:example.org") };
        RoomInfo b = joinedSpace(QStringLiteral("!b:example.org"),
                                 QStringLiteral("B"));
        b.childRoomIds = { QStringLiteral("!b1:example.org") };
        tree.roomList = { root, a, aChild,
                          joinedSpace(QStringLiteral("!a1a:example.org"),
                                      QStringLiteral("A1a")),
                          b,
                          joinedSpace(QStringLiteral("!b1:example.org"),
                                      QStringLiteral("B1")) };
        SpaceManager treeSpaces;
        treeSpaces.setClient(&tree);
        for (const QString &id : { QStringLiteral("!root:example.org"),
                                   QStringLiteral("!a:example.org"),
                                   QStringLiteral("!a1:example.org"),
                                   QStringLiteral("!b:example.org") }) {
            store()->setSpaceExpanded(id, true);
        }
        entries()->setSources(&treeSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(120);

        auto *content = m_list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const auto layersOf = [&content](QQuickItem *row) {
            QList<QQuickItem *> out;
            collectDescendantsNamed(row, QStringLiteral("railGroupField"),
                                    out);
            Q_UNUSED(content);
            return out;
        };

        // The LAST row of A's run, which is the one that ends it.
        QQuickItem *a1 = delegateFor(QStringLiteral("!a1a:example.org"));
        QQuickItem *bRow = delegateFor(QStringLiteral("!b:example.org"));
        QVERIFY2(a1 && bRow, "the two-sibling fixture did not build");

        const QList<QQuickItem *> a1Layers = layersOf(a1);
        const QList<QQuickItem *> bLayers = layersOf(bRow);
        QVERIFY2(a1Layers.size() >= 3 && bLayers.size() >= 2,
                 qPrintable(QStringLiteral(
                     "A1a draws %1 layers and B draws %2 — the fixture did "
                     "not reach the rail's deepest inset, where the chevron "
                     "has least room")
                     .arg(a1Layers.size()).arg(bLayers.size())));

        const auto bottomIn = [&content](QQuickItem *layer) {
            return layer->mapToItem(content, QPointF(0, layer->height())).y();
        };
        const auto topIn = [&content](QQuickItem *layer) {
            return layer->mapToItem(content, QPointF(0, 0)).y();
        };

        // ── THE INNER RUNS SEPARATE ──────────────────────────────────
        const qreal gap = m_rail->property("groupGap").toReal();
        QVERIFY2(gap > 0, "the rail reports no group gap at all");
        const qreal innerGap = topIn(bLayers.at(1)) - bottomIn(a1Layers.at(1));
        QVERIFY2(innerGap >= gap,
                 qPrintable(QStringLiteral(
                     "A's run ends %1px above where B's begins, and the gap "
                     "that separates one run from the next is %2 — the two "
                     "read as one shape with a notch in it")
                     .arg(innerGap).arg(gap)));

        // ── AND THE PARENT DOES NOT ──────────────────────────────────
        //
        // The depth-1 region owns both runs, so it has to cover the gap
        // between them: a layer that stops at its own delegate would put a
        // hole in the parent exactly where its child happened to end, which
        // is the other way to get this wrong.
        QVERIFY2(qAbs(topIn(bLayers.at(0)) - bottomIn(a1Layers.at(0))) < 1.0,
                 qPrintable(QStringLiteral(
                     "the depth-1 region ends at y=%1 on A1 and restarts at "
                     "y=%2 on B — the parent has a hole where its child's run "
                     "stopped").arg(bottomIn(a1Layers.at(0)))
                     .arg(topIn(bLayers.at(0)))));

        // ── AND THE CHEVRON IS INSIDE THE INNERMOST REGION ───────────
        //
        // AT THE MINIMUM RAIL WIDTH, and it still narrows the rail even
        // though it no longer has to. `tileColumnX` WAS a centring
        // calculation, so a wide rail handed the glyph tens of pixels of
        // slack and this assertion passed on a gutter that could not
        // actually hold it — a first version measured at this suite's 160px
        // and proved nothing. The gutter is a constant now (the rail's two
        // margins differ, so the tile is placed rather than centred), which
        // makes every width the worst case. Narrowing is kept because it
        // costs nothing and it is the case that would come back if the
        // placement ever went back to centring.
        const qreal restoreWidth = m_rail->width();
        m_rail->setWidth(m_rail->property("minRailWidth").toReal());
        QCoreApplication::processEvents();
        QTest::qWait(80);

        // Asserted on the row with the MOST layers, because that is where the
        // innermost edge is furthest right and the glyph has least room.
        int chevronsChecked = 0;
        for (QQuickItem *row : { a1, bRow,
                                 delegateFor(QStringLiteral("!a:example.org")),
                                 delegateFor(QStringLiteral("!a1:example.org")),
                                 delegateFor(QStringLiteral("!root:example.org")) }) {
            if (!row || !row->isVisible())
                continue;
            auto *glyph = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceExpandGlyph"));
            const QList<QQuickItem *> layers = layersOf(row);
            if (!glyph || !glyph->isVisible() || layers.isEmpty())
                continue;
            QQuickItem *innermost = layers.last();
            ++chevronsChecked;
            // THE INK, NOT THE EM BOX. An `Icon`'s item is the glyph's
            // ADVANCE — for this chevron about 0.45 of the font size — and
            // roughly a quarter of that is the font's own empty side
            // bearing. The rail places the mark by its ink for exactly that
            // reason (see `chevronGlyphX`), so measuring the box here would
            // fail a chevron that is drawn perfectly inside its region. The
            // ink is centred in the box to within a quarter-pixel; that is
            // measured, not assumed.
            const qreal inkWidth =
                m_rail->property("chevronInkWidth").toReal();
            QVERIFY2(inkWidth > 0, "the rail reports no chevron ink width");
            const qreal inkCentre =
                glyph->mapToItem(row, QPointF(0, 0)).x() + glyph->width() / 2;
            const qreal glyphLeft = inkCentre - inkWidth / 2;
            const qreal glyphRight = inkCentre + inkWidth / 2;
            const qreal fieldLeft =
                innermost->mapToItem(row, QPointF(0, 0)).x();
            const qreal fieldRight = fieldLeft + innermost->width();
            QVERIFY2(glyphLeft >= fieldLeft && glyphRight <= fieldRight,
                     qPrintable(QStringLiteral(
                         "the expander spans %1..%2 and the innermost region "
                         "it belongs to spans %3..%4 — the chevron is drawn "
                         "outside its own box")
                         .arg(glyphLeft).arg(glyphRight)
                         .arg(fieldLeft).arg(fieldRight)));
        }
        QVERIFY2(chevronsChecked >= 3,
                 qPrintable(QStringLiteral(
                     "only %1 expanders were measurable, so this says nothing "
                     "about the deepest row").arg(chevronsChecked)));

        m_rail->setWidth(restoreWidth);
        QCoreApplication::processEvents();
        QTest::qWait(60);

        for (const QString &id : { QStringLiteral("!root:example.org"),
                                   QStringLiteral("!a:example.org"),
                                   QStringLiteral("!a1:example.org"),
                                   QStringLiteral("!b:example.org") }) {
            store()->setSpaceExpanded(id, false);
        }
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }


    // ── 2026-09-18: the expander hung off the rail's edge when narrow ─────
    //
    // FOUND IN A CAPTURE AT THE MINIMUM WIDTH, and it could not have been
    // found anywhere else: every case in this file runs at 160px, where the
    // gutter is 52 and nothing is near an edge. The gutter's width was a
    // literal 14 with nothing tying it to the glyph it carries, which left
    // the chevron 2.8px from the rail's outer edge and 4px from its tile —
    // closer to the window frame than to the thing it belongs to.
    //
    // WHAT IS ASSERTED IS THE RELATION, not a pixel count: the expander is
    // never nearer the rail's edge than it is to its own tile. That is the
    // property that makes it read as part of the row, it holds at every
    // width and every text scale, and no literal can express it.
    //
    // THE FIRST VERSION OF THIS CASE ASSERTED `left >= 0` — clipping — and
    // PASSED ON THE OLD CODE, because the glyph is 7.2px wide and not the 12
    // it is given (an `Icon` sizes by font pixel size; a chevron's advance is
    // narrower than its em). The defect was real and the description of it
    // was arithmetic, not measurement.
    void theGutterIsWideEnoughForTheGlyphItHolds()
    {
        RailFakeClient nested;
        RoomInfo lv0 = joinedSpace(QStringLiteral("!lv0:example.org"),
                                   QStringLiteral("Level 0"));
        lv0.childRoomIds = { QStringLiteral("!lv1:example.org") };
        RoomInfo lv1 = joinedSpace(QStringLiteral("!lv1:example.org"),
                                   QStringLiteral("Level 1"));
        lv1.childRoomIds = { QStringLiteral("!chan:example.org") };
        nested.roomList = { lv0, lv1,
                            joinedRoom(QStringLiteral("!chan:example.org"),
                                       QStringLiteral("Channel")) };
        SpaceManager nestedSpaces;
        nestedSpaces.setClient(&nested);
        store()->setSpaceExpanded(QStringLiteral("!lv0:example.org"), true);
        entries()->setSources(&nestedSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);

        const qreal restore = m_rail->width();
        const qreal minWidth = m_rail->property("minRailWidth").toReal();
        QVERIFY2(minWidth > 0, "the rail reports no minimum width");
        m_rail->setWidth(minWidth);
        QCoreApplication::processEvents();
        QTest::qWait(60);

        auto *content = m_list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        int checked = 0;
        const auto rows = content->childItems();
        for (QQuickItem *row : rows) {
            if (!row || !row->isVisible() || row->height() <= 0)
                continue;
            auto *glyph = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceExpandGlyph"));
            if (!glyph || !glyph->isVisible() || glyph->width() <= 0)
                continue;
            // THE INK, for the reason the sibling case spells out: an
            // `Icon`'s item is the glyph's advance and a quarter of that is
            // empty side bearing, so the box says nothing about where the
            // mark is drawn.
            const qreal inkWidth =
                m_rail->property("chevronInkWidth").toReal();
            QVERIFY2(inkWidth > 0, "the rail reports no chevron ink width");
            const qreal inkCentre =
                glyph->mapToItem(m_rail, QPointF(0, 0)).x()
                + glyph->width() / 2;
            const qreal left = inkCentre - inkWidth / 2;
            const qreal right = inkCentre + inkWidth / 2;
            ++checked;
            const qreal toTile =
                m_rail->property("tileColumnX").toReal() - right;
            QVERIFY2(left >= toTile,
                     qPrintable(QStringLiteral(
                         "at the minimum width of %1 the expander is %2px "
                         "from the rail's outer edge and %3px from its tile — "
                         "it reads as hanging off the edge rather than as "
                         "belonging to the row")
                         .arg(minWidth).arg(left).arg(toTile)));
            QVERIFY2(right <= minWidth,
                     qPrintable(QStringLiteral(
                         "at the minimum width of %1 the expander ends at "
                         "x=%2, past the rail's own right edge")
                         .arg(minWidth).arg(right)));
        }
        QVERIFY2(checked > 0,
                 "no row showed an expander at the minimum width, so this "
                 "case measured nothing — the fixture must contain a Space "
                 "with children");

        m_rail->setWidth(restore);
        store()->setSpaceExpanded(QStringLiteral("!lv0:example.org"), false);
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }


    // ── 2026-09-18: a tree deeper than the rail can draw ─────────────────
    //
    // THE CONDITION THIS CASE WAS WRITTEN FOR NO LONGER EXISTS, and how it
    // stopped existing is the point. Depth used to cost an indent step, so a
    // narrow rail ran out of room long before a hierarchy did, and the rail
    // answered that by DIVING — picking an ancestor as a trunk and hiding
    // everything above it behind a chip. That was a second thing for a reader
    // to learn, invented to pay for the first.
    //
    // Depth costs no horizontal room now, so there is nothing to run out of
    // and nothing to dive for. What replaces this case is the invariant that
    // makes the dive unnecessary, asserted at a depth no rail could ever have
    // drawn: EIGHT levels, every one expanded, every tile on the root's axis.
    //
    // A shallow fixture cannot discriminate here — two levels of a per-level
    // step are a few pixels and a rounding argument away from passing. Eight
    // are not.
    void depthCostsTheTilesNoHorizontalRoomAtAll()
    {
        RailFakeClient deep;
        QList<RoomInfo> rooms;
        QStringList chain;
        for (int i = 0; i < 8; ++i)
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

        qreal rootCentre = -1;
        int measured = 0;
        int deepest = 0;
        for (int i = 0; i < chain.size(); ++i) {
            QQuickItem *row = delegateFor(chain.at(i));
            if (!row || !row->isVisible())
                continue;
            auto *tile = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceTile"));
            if (!tile || tile->width() <= 0)
                continue;
            const qreal centre =
                tile->mapToItem(m_rail, QPointF(0, 0)).x() + tile->width() / 2;
            ++measured;
            deepest = std::max(deepest, row->property("level").toInt());
            if (rootCentre < 0) {
                rootCentre = centre;
                continue;
            }
            QVERIFY2(qAbs(centre - rootCentre) < 1.0,
                     qPrintable(QStringLiteral(
                         "%1 is centred on x=%2 and the root on x=%3 — depth "
                         "is buying horizontal room again, which is the wave "
                         "the whole redesign removed")
                         .arg(chain.at(i)).arg(centre).arg(rootCentre)));
        }
        QVERIFY2(measured >= 6,
                 qPrintable(QStringLiteral(
                     "only %1 rows of the eight-deep chain were measurable, "
                     "so this proves nothing about depth").arg(measured)));
        QVERIFY2(deepest >= 5,
                 qPrintable(QStringLiteral(
                     "the deepest measured row is level %1 — the fixture did "
                     "not nest").arg(deepest)));

        // ── THE LAYERS NEST, AND THEY SATURATE ──────────────────────
        //
        // A row draws one region per ANCESTOR, so the count is its depth,
        // capped. The cap is what keeps a deep tree drawable at all: each
        // layer is inset inside the one containing it, so without one the
        // innermost region would end up narrower than the tile it holds.
        const int maxLayers = m_rail->property("maxBandLayers").toInt();
        QVERIFY2(maxLayers >= 2 && maxLayers <= 6,
                 qPrintable(QStringLiteral(
                     "the rail claims %1 layers — a rail this narrow cannot "
                     "inset that many and still hold a tile").arg(maxLayers)));

        qreal outerX = -1;
        qreal outerWidth = -1;
        int deepRowsChecked = 0;
        for (const QString &id : chain) {
            QQuickItem *row = delegateFor(id);
            if (!row || !row->isVisible())
                continue;
            const int level = row->property("level").toInt();
            if (level < 1)
                continue;
            QList<QQuickItem *> layers;
            collectDescendantsNamed(row, QStringLiteral("railGroupField"),
                                    layers);
            // One per ancestor, PLUS the row's own when it owns the run
            // below it — a tile is inside the region it owns, or the region
            // reads as a band that begins underneath it. Every row of this
            // chain but the last owns one.
            const bool owns = row->property("ownsRegion").toBool();
            QCOMPARE(layers.size(),
                     std::min(maxLayers, level + (owns ? 1 : 0)));

            // Strictly nested: each layer starts further in and is narrower
            // than the one containing it. This is the property the previous
            // design could not have — there was one region per row, tinted by
            // that row's own depth, so a deeper run REPLACED its parent's
            // tint instead of sitting on it.
            for (int i = 1; i < layers.size(); ++i) {
                const qreal outer =
                    layers.at(i - 1)->mapToItem(row, QPointF(0, 0)).x();
                const qreal inner =
                    layers.at(i)->mapToItem(row, QPointF(0, 0)).x();
                QVERIFY2(inner > outer,
                         qPrintable(QStringLiteral(
                             "%1's layer %2 starts at x=%3 and the one "
                             "containing it at x=%4 — they are not nested")
                             .arg(id).arg(i).arg(inner).arg(outer)));
                QVERIFY2(layers.at(i)->width() < layers.at(i - 1)->width(),
                         qPrintable(QStringLiteral(
                             "%1's layer %2 is not narrower than the one "
                             "containing it").arg(id).arg(i)));
            }

            // AND THE OUTERMOST DOES NOT MOVE. The top-level Space's region
            // has to be the same shape behind a depth-1 row and behind a
            // depth-6 one, or it is not one region running behind its
            // descendants — it is a per-row band wearing a parent's colour.
            ++deepRowsChecked;
            const qreal x = layers.at(0)->mapToItem(row, QPointF(0, 0)).x();
            if (outerX < 0) {
                outerX = x;
                outerWidth = layers.at(0)->width();
                continue;
            }
            QVERIFY2(qAbs(x - outerX) < 1.0
                         && qAbs(layers.at(0)->width() - outerWidth) < 1.0,
                     qPrintable(QStringLiteral(
                         "%1's outermost region is %2px wide at x=%3 where a "
                         "shallower row's is %4px at x=%5 — the top-level "
                         "region narrows as its contents get deeper")
                         .arg(id).arg(layers.at(0)->width()).arg(x)
                         .arg(outerWidth).arg(outerX)));
        }
        QVERIFY2(deepRowsChecked >= 5,
                 qPrintable(QStringLiteral(
                     "only %1 nested rows were measurable, so nothing here "
                     "says anything about deep nesting")
                     .arg(deepRowsChecked)));

        // ── NO TWO REGIONS THAT TOUCH WEAR ONE TINT ─────────────────
        //
        // The stack is capped, so every row past the cap draws its own region
        // as "the innermost layer" — which meant a depth-5 region was drawn
        // directly inside a depth-4 one at the same inset AND the same
        // colour, and the two were one picture. Asked in those words: "are
        // these supposed to be the same color?"
        //
        // A CAP CANNOT BE ALLOWED TO STOP DISTINGUISHING. Past it the
        // innermost layer alternates between the last two rungs, so a parent
        // and the child drawn on top of it always differ. Eight levels is
        // several rows past the cap, which is what makes this measurable.
        QColor previousInnermost;
        int alternationsChecked = 0;
        for (const QString &id : chain) {
            QQuickItem *row = delegateFor(id);
            if (!row || !row->isVisible())
                continue;
            // ONLY ROWS THAT OWN A REGION. A leaf draws its ancestors'
            // layers and none of its own, so its "innermost" IS its
            // parent's — identical by construction, and comparing them
            // asserts that a row differs from itself. The first version of
            // this did exactly that and failed on correct code.
            if (!row->property("ownsRegion").toBool())
                continue;
            QList<QQuickItem *> rowLayers;
            collectDescendantsNamed(row, QStringLiteral("railGroupField"),
                                    rowLayers);
            if (rowLayers.isEmpty())
                continue;
            const QColor innermost =
                rowLayers.last()->property("color").value<QColor>();
            if (previousInnermost.isValid()) {
                ++alternationsChecked;
                QVERIFY2(innermost != previousInnermost,
                         qPrintable(QStringLiteral(
                             "%1's own region is %2 and the region it is "
                             "drawn directly inside is the same colour — two "
                             "different regions, one picture")
                             .arg(id).arg(innermost.name())));
            }
            previousInnermost = innermost;
        }
        QVERIFY2(alternationsChecked >= 4,
                 qPrintable(QStringLiteral(
                     "only %1 nested pairs were comparable, so nothing here "
                     "reaches past the cap").arg(alternationsChecked)));

        // ── NOTHING IS DRAWN OUTSIDE THE REGION THAT CONTAINS IT ────
        //
        // Reported as "blue DL looks very bad, the whole region", and the
        // cause was a shape escaping its container: the cap seam moves a
        // child's BAND down, and the tile drawn on that band was still
        // positioned from the row's top — so on every row that opens a seam
        // the tile stuck out through the top edge of its own region.
        //
        // GEOMETRIC, ON REAL DELEGATES, and nothing else can see it. The
        // bindings read correctly either way; the defect is entirely in two
        // numbers that are supposed to move together and did not. The same
        // shape of mistake has now produced three separate defects in this
        // file, so it is worth pinning rather than fixing again.
        int containmentChecks = 0;
        for (const QString &id : chain) {
            QQuickItem *row = delegateFor(id);
            if (!row || !row->isVisible())
                continue;
            QList<QQuickItem *> rowLayers;
            collectDescendantsNamed(row, QStringLiteral("railGroupField"),
                                    rowLayers);
            if (rowLayers.isEmpty())
                continue;
            auto *tile = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceTile"));
            if (!tile || tile->width() <= 0)
                continue;
            QQuickItem *innermost = rowLayers.last();
            const QPointF tileTL = tile->mapToItem(row, QPointF(0, 0));
            const QPointF bandTL = innermost->mapToItem(row, QPointF(0, 0));
            ++containmentChecks;
            QVERIFY2(tileTL.y() >= bandTL.y() - 0.5,
                     qPrintable(QStringLiteral(
                         "%1's tile starts at y=%2 and the region that holds "
                         "it starts at y=%3 — the tile is drawn outside its "
                         "own region")
                         .arg(id).arg(tileTL.y()).arg(bandTL.y())));
            QVERIFY2(tileTL.y() + tile->height()
                         <= bandTL.y() + innermost->height() + 0.5,
                     qPrintable(QStringLiteral(
                         "%1's tile ends at y=%2 and its region ends at y=%3")
                         .arg(id).arg(tileTL.y() + tile->height())
                         .arg(bandTL.y() + innermost->height())));
            QVERIFY2(tileTL.x() >= bandTL.x() - 0.5
                         && tileTL.x() + tile->width()
                                <= bandTL.x() + innermost->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "%1's tile spans x %2..%3 and its region spans "
                         "%4..%5").arg(id).arg(tileTL.x())
                         .arg(tileTL.x() + tile->width())
                         .arg(bandTL.x())
                         .arg(bandTL.x() + innermost->width())));
        }
        QVERIFY2(containmentChecks >= 5,
                 qPrintable(QStringLiteral(
                     "only %1 rows were checked for containment")
                     .arg(containmentChecks)));

        for (const QString &id : chain)
            store()->setSpaceExpanded(id, false);
        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(60);
    }


    // ── 2026-09-18: the region ladder, on every preset ───────────────────
    //
    // The rail's whole hierarchy cue is a ladder of tinted regions, and its
    // rungs are DERIVED from each theme's own rail and text colours — so
    // there are eleven of them and nobody looks at more than one.
    //
    // MEASURED ACROSS ALL ELEVEN and they were not equal: the dark presets
    // landed at 1.40-1.53 per boundary and the LIGHT ones at 1.22-1.36 on the
    // same mix steps. That is the sRGB transfer curve rather than a palette
    // problem — equal 8-bit steps are far smaller luminance steps near white
    // than near black — so one alpha ladder cannot serve both directions and
    // a ladder tuned on a dark preset arrives washed out on a light one.
    //
    // THIS READS THE LIVE SINGLETON, not the file. `railNestSurfaces` is a
    // list of `Qt.tint()` results; a text scan of AppTheme.qml sees the
    // alphas and cannot evaluate them, which is exactly how eleven presets
    // came to share one ramp.
    void theRegionLadderIsEvenOnEveryTheme()
    {
        auto *theme = m_engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY2(theme, "no AppTheme singleton — this case cannot read the "
                        "region ladder and so proves nothing");
        auto *settings = m_controller->settings();
        QVERIFY2(settings, "no SettingsManager, so no theme can be selected");
        const int original = settings->property("theme").toInt();

        // Relative luminance, WCAG. Written out because the ladder's whole
        // point is a PERCEPTUAL step and an 8-bit difference is not one.
        const auto luminance = [](const QColor &c) {
            const auto ch = [](double v) {
                return v <= 0.04045 ? v / 12.92
                                    : std::pow((v + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF())
                   + 0.0722 * ch(c.blueF());
        };
        const auto contrast = [&luminance](const QColor &a, const QColor &b) {
            const double la = luminance(a) + 0.05;
            const double lb = luminance(b) + 0.05;
            return la > lb ? la / lb : lb / la;
        };

        int themesChecked = 0;
        // 1..11: every preset. 0 is "follow the system" and 12 is a
        // user-authored palette, and neither is a palette of its own.
        for (int t = 1; t <= 11; ++t) {
            settings->setProperty("theme", t);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            if (settings->property("theme").toInt() != t)
                continue;   // a preset this build does not carry

            const QColor rail = theme->property("rail").value<QColor>();
            const QVariantList rungs =
                theme->property("railNestSurfaces").toList();
            QVERIFY2(rungs.size() >= 4,
                     qPrintable(QStringLiteral(
                         "theme %1 exposes %2 region rungs")
                         .arg(t).arg(rungs.size())));
            ++themesChecked;

            QColor previous = rail;
            QList<double> steps;
            // Rung 0 is a FOLDER's container and is deliberately the quietest
            // step of the ladder, so the assertion starts at hierarchy depth
            // 1 — the rung a reader actually has to see against bare rail.
            for (int i = 1; i < rungs.size(); ++i) {
                const QColor rung = rungs.at(i).value<QColor>();
                const double ratio = contrast(previous, rung);
                    // 1.20, and it was 1.30. The ladder was given a CEILING on
                // 2026-09-18 — without one it climbed to L*62 in a theme
                // whose base is L*6 and made the rail the brightest band in
                // the window, 5.25:1 against the room-list column beside it.
                // A receding column affords about 2.1:1 in total, so three
                // rungs inside it are ~1.26-1.30 steps and no threshold
                // written against the unbounded ladder can survive that.
                // What this still pins is the thing that matters: the rungs
                // are EVEN, and none of them collapses into its neighbour.
                // 1.05, AND THE REAL ASSERTION IS THE EVENNESS BELOW.
                // This threshold has now been re-keyed twice, both times
                // because the ladder was made QUIETER on purpose, and a
                // moving absolute floor pins nothing. A rail is chrome: it
                // has to recede, so its whole range is small and its steps
                // are small with it — Discord's entire three-plane chrome
                // spans 9.5 ΔL*. What must never happen is a rung COLLAPSING
                // into its neighbour, which is what this floor catches, and
                // the spread check afterwards is what catches a ladder that
                // has stopped being a ladder.
                QVERIFY2(ratio >= 1.05,
                         qPrintable(QStringLiteral(
                             "theme %1: region rung %2 (%3) is %4:1 against "
                             "the one outside it (%5) — below the step a 2px "
                             "band can carry, so the nesting stops reading")
                             .arg(t).arg(i).arg(rung.name())
                             .arg(ratio, 0, 'f', 2).arg(previous.name())));
                steps << ratio;
                previous = rung;
            }
            // EVEN, which is the property that actually matters and the one
            // no absolute number can express. A ladder whose steps differ by
            // more than half again is not a ladder — it is one loud boundary
            // and some whispers, which is exactly what the first version of
            // this ramp was before it was rebuilt off the rail's own
            // background.
            double lo = steps.first();
            double hi = steps.first();
            for (double v : std::as_const(steps)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            QVERIFY2(hi <= lo * 1.5,
                     qPrintable(QStringLiteral(
                         "theme %1: the region steps run %2:1 to %3:1 — the "
                         "ladder is uneven, so one boundary shouts and the "
                         "rest whisper")
                         .arg(t).arg(lo, 0, 'f', 3).arg(hi, 0, 'f', 3)));
        }
        QVERIFY2(themesChecked >= 8,
                 qPrintable(QStringLiteral(
                     "only %1 presets were selectable, so this says little "
                     "about the fleet").arg(themesChecked)));

        settings->setProperty("theme", original);
        QCoreApplication::processEvents();
        QTest::qWait(30);
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
