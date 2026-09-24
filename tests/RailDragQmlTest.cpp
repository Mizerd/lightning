// The Spaces rail's drag-to-folder gesture, driven by real pointer events.
//
// The rule under test: the tile is the group target, the gap between tiles is
// the reorder target, nothing moves while the pointer is on a tile, and there
// is no dwell. RailLayoutTest drives RailEntryModel directly; this suite sends
// QMouseEvents at tile centres taken from real delegate geometry and asserts
// what a release wrote, because earlier band rules were correct in the model
// yet unreachable from the view.
//
// This proves the gesture is reachable, not how it feels under a real hand.
//
// Measurement rules: assert on the store (RailLayoutStore::folders()/order())
// and the model, not on transient drag flags; read item `y` only after the
// 140 ms move/displaced transition; never sample pixel colours (an offscreen
// grab holds each item's creation-time colour).
//
// Fixture: the real SpacesRail.qml on a real AppController. Only the Spaces are
// substituted (the mock has one, a Space-onto-Space drop needs two):
// RailEntryModel gets a SpaceManager over a local client with three root
// Spaces, while writing to AppController's own RailLayoutStore.

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
#include <QSet>
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

// The rail's geometry, restated so assertions can say what they mean: during
// a drag every row is its tile band tall, the 40 px tile is drawn at y = 4,
// and the group band is the tile's middle 24 px. Every point sent to the
// window still comes from a real delegate's mapToScene().
constexpr int kTileTopInRow = 4;
constexpr int kTileHeight = 40;
constexpr int kTileCentreInRow = kTileTopInRow + kTileHeight / 2;  // 24
constexpr int kGroupBandTopInRow = 12;
constexpr int kGroupBandBottomInRow = 36;

// A client with a fixed room list: the mock backend's single Space cannot
// express a Space-onto-Space drop.
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

    // The live delegate for `entryId`, matched on its own `entryId` property.
    // findChild cannot reach Repeater delegates (their QObject parent is the
    // Repeater's context), so this walks the visual tree.
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

    // The one shared ToolTip that is actually on screen. Qt Quick Controls
    // uses a single ToolTip per window for every attached `ToolTip.text`; it
    // is a Popup drawn as a QQuickPopupItem in the overlay, with no public
    // C++ handle, and the attached object's x/y report the request, not the
    // result. Walks the visual tree for the visible popup item.
    static QQuickItem *visiblePopupItem(QQuickItem *from)
    {
        if (!from)
            return nullptr;
        const auto children = from->childItems();
        for (QQuickItem *child : children) {
            if (!child)
                continue;
            if (child->isVisible() && child->width() > 0
                && QString::fromLatin1(child->metaObject()->className())
                       .startsWith(QStringLiteral("QQuickPopupItem"))) {
                return child;
            }
            if (QQuickItem *found = visiblePopupItem(child))
                return found;
        }
        return nullptr;
    }

    // The layered group field draws one rectangle per ancestor under one
    // objectName; collect them all.
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

    // Scene y of the top of the row holding `entryId`, from real geometry.
    qreal rowTopScene(const QString &entryId) const
    {
        QQuickItem *item = delegateFor(entryId);
        return item ? item->mapToScene(QPointF(0, 0)).y() : -1;
    }

    // The centre of a tile: where a person aims, and the middle of the group
    // band.
    QPoint tileCentre(const QString &entryId) const
    {
        QQuickItem *item = delegateFor(entryId);
        if (!item)
            return {};
        return item
            ->mapToScene(QPointF(item->width() / 2, kTileCentreInRow))
            .toPoint();
    }

    // A point in the gap below `entryId`'s tile, past the group band, in the
    // 28 px (12 + 4 spacing + 12) between adjacent tiles.
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
    // The sequence matters: earlier rules survived one move and failed on the
    // second sample in the same place.
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

        // Lower the drag threshold (platform default ~10 px) so case 6 can
        // cross it while staying inside a 24 px group band. It only changes
        // how far the pointer travels before the handler grabs, and every
        // case asserts the drag activated.
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
            // row: an ineligible group target directly above the first Space.
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
        // The rail's rows come from this hierarchy; writes still go to
        // AppController's own RailLayoutStore, which the assertions read.
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
        // Tall enough that the rows never make the ListView flickable (which
        // would compete for the grab and arm auto-scroll). If the ListView
        // ever steals the grab under synthesized events, cases fail on "the
        // DragHandler never took the gesture"; under a real pointer that
        // would be a production bug. 160 px is wider than the rail's maximum;
        // theGutterIsWideEnoughForTheGlyphItHolds covers the narrowest width.
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
        // Not scrolled: derived row tops and delegate geometry agree only at
        // contentY 0.
        QCOMPARE(m_list->property("contentY").toReal(), 0.0);
        QVERIFY(m_list->property("contentHeight").toReal()
                <= m_list->property("height").toReal());
    }

    void cleanupTestCase()
    {
        // The engine owns the rail item, so delete it first while the window
        // is still alive.
        delete m_engine;
        delete m_window;
        delete m_controller;
    }

    // Every case starts with no folders, no stored order, and Alpha above
    // Bravo above Charlie.
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
        // Let the previous case's 140 ms transitions finish.
        QTest::qWait(200);
    }

    // Case 1: dropping a Space on a Space creates a folder (not a swap).
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
        // Several small samples on the target, as a hand makes: the second
        // sample is what earlier rules could not survive.
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
        // Target first, then the dropped Space: the folder takes the target's
        // position (RailEntryModel::commitGrouping).
        QCOMPARE(store()->folderMembers(folderId), QStringList({ b, a }));
        QCOMPARE(store()->folderOf(a), folderId);
        QCOMPARE(store()->folderOf(b), folderId);
    }

    // Case 2: the tile being aimed at never moves out from under the pointer.
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

        // Require at least two samples on the tile, or the case could pass
        // without reaching it.
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

    // Case 3: grouping is armed and aimed at the target when the button comes
    // up; endDrag groups on the flag, not the pointer position.
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
        // The release clears the flags, so the rail stops drawing the ring
        // and the drag presentation.
        QVERIFY(!entries()->dragging());
        QVERIFY(!entries()->grouping());
        QVERIFY(entries()->dropTargetId().isEmpty());
    }

    // Case 4: a release in the gap below the target reorders and makes no
    // folder, even after resting on the target. Leaving a tile must disarm
    // grouping.
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

    // Case 5: sweeping through a tile without stopping makes no folder. The
    // geometry replaces a dwell: nothing moves on a tile and a gap is never a
    // group target.
    void sweepingThroughATileWithoutStoppingMakesNoFolder()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QString c = m_spaceIds.at(2);
        const QPoint from = tileCentre(a);
        const QPoint intoGap = gapBelow(b);

        pressAt(from);
        // One continuous run across Bravo's tile and out the far side.
        sweep(from, intoGap, 14);
        QVERIFY(entries()->dragging());
        QVERIFY(!entries()->grouping());
        releaseAt(intoGap);
        QCoreApplication::processEvents();

        QTest::qWait(200);
        QCOMPARE(store()->folders().size(), 0);
        QCOMPARE(store()->order(), QStringList({ b, a, c }));
    }

    // Case 6: a release over the dragged tile's own slot changes nothing.
    void aReleaseOverTheDraggedTilesOwnSlotChangesNothing()
    {
        const QString a = m_spaceIds.at(0);
        const QString b = m_spaceIds.at(1);
        const QString c = m_spaceIds.at(2);
        const QPoint home = tileCentre(a);
        const int rowABefore = entries()->rowForEntry(a);
        const qreal yABefore = rowTopScene(a);

        pressAt(home);
        // 8 px: past the drag threshold, still inside this tile's group band.
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

    // Nesting moves the expander into a fixed gutter lane, not the tiles: the
    // chevron is anchored to the gutter's right edge, a constant distance from
    // its tile at every depth. Geometric, on real delegates.
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
            // Both absolute in the row: the expander belongs to the gutter
            // column, the tile to the shared tile axis.
            gaps << glyphLeft.x();
            // The tile's centre: a nested tile is one step smaller and inset
            // equally, so its left edge moves but its centre does not.
            tileLefts << tileLeft.x() + tile->width() / 2;
        }

        // The tiles do not move with depth; a per-level step walked tiles off
        // a shared axis in a narrow rail. Depth lives in the gutter lanes.
        for (int i = 1; i < tileLefts.size(); ++i) {
            QVERIFY2(qAbs(tileLefts.at(i) - tileLefts.at(0)) < 1.0,
                     qPrintable(QStringLiteral(
                         "a depth-%1 tile is centred on x=%2 and a root on "
                         "x=%3 — the column steps with depth again, which is "
                         "the wave this case exists to prevent")
                         .arg(i).arg(tileLefts.at(i)).arg(tileLefts.at(0))));
        }

        // The expander keeps one x at every depth.
        for (int i = 0; i < gaps.size(); ++i) {
            QVERIFY2(qAbs(gaps.at(i) - gaps.at(0)) < 1.0,
                     qPrintable(QStringLiteral(
                         "the expander sits at x=%1 at depth %2 and x=%3 at "
                         "the root — it moves with the row rather than "
                         "keeping the gutter's one position")
                         .arg(gaps.at(i)).arg(i).arg(gaps.at(0))));
            // In the gutter, left of the tile column (on the tile it clipped
            // the avatar).
            QVERIFY2(gaps.at(i) < tileLefts.at(i),
                     qPrintable(QStringLiteral(
                         "the expander is at x=%1 and the tile's centre is at "
                         "x=%2 — it is not in the gutter it was given")
                         .arg(gaps.at(i)).arg(tileLefts.at(i))));
        }

        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
    }


    // Expanding a leaf Space (rooms, no subspaces) reveals its rooms without
    // anything else rebuilding the rail. `revealed` must depend on the
    // expansion state: read through a Q_INVOKABLE it records no dependency,
    // and a leaf inserts no model rows to rebuild the delegate. Substitutes
    // its own hierarchy and restores the shared one afterwards.
    void expandingALeafSpaceRevealsItsRoomsWithoutRebuildingTheRail()
    {
        // Production wiring: `topRoomsInSpace()` reads `app.spaces`, the
        // controller's own SpaceManager, which is empty until the mock account
        // logs in.
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

        // A leaf, discovered from the live model rather than pinned to a mock
        // id, so a hierarchy change fails loudly.
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

        // The chevron's own toggle; it inserts no model row, so nothing else
        // can rebuild the delegate.
        const int rowsBefore = entries()->rowCount();
        store()->toggleSpaceExpanded(leafId);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCOMPARE(entries()->rowCount(), rowsBefore);

        tile = delegateFor(leafId);
        QVERIFY(tile);
        // The role the chevron reads did update.
        QCOMPARE(tile->property("expanded").toBool(), true);
        QVERIFY2(tile->property("revealed").toInt() > 0,
                 "the chevron opened and the reveal count stayed at its "
                 "creation-time zero: a leaf Space inserts no rows, so "
                 "nothing rebuilds the delegate and the rooms appear only "
                 "after an unrelated toggle or an app restart");
        QCOMPARE(tile->property("revealedRooms").toList().size(), leafRooms);

        // A room is actually drawn: model properties alone stayed correct
        // while the revealed column lost its layout properties. Size is a
        // ratio of the Space tile (a room tier is 0.7), and x is the shared
        // axis.
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

        // Below the Space tile: a room tile keeps its own size and x even when
        // its column has none, so only its y discriminates.
        const qreal bandHeight =
            tile->property("tileBandHeight").toReal();
        const qreal roomTop = roomTile->mapToItem(tile, QPointF(0, 0)).y();
        QVERIFY2(roomTop >= bandHeight - 1,
                 qPrintable(QStringLiteral(
                     "a revealed room starts at y=%1 inside a tile band %2 "
                     "tall — the rooms are drawn over the Space that owns "
                     "them").arg(roomTop).arg(bandHeight)));

        // ...and closing puts them away through the same binding.
        store()->toggleSpaceExpanded(leafId);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        tile = delegateFor(leafId);
        QVERIFY(tile);
        QCOMPARE(tile->property("revealed").toInt(), 0);

        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
    }

    // Every rail tooltip sits beside the row the pointer is on, off the rail.
    // Qt centres an attached tooltip above its attachee, which on the rail is
    // the previous row, so the tips hang off invisible anchors. Revealed rooms
    // need their own hover handler, or the parent Space's tip replaces the
    // room's. Measured on the real shared ToolTip over real delegates. The
    // window is widened because a Popup is kept inside its window, which
    // would measure the clamp instead of the placement.
    void everyRailTooltipSitsBesideTheRowThePointerIsOn()
    {
        // The revealed-room column needs the controller's own SpaceManager,
        // which is empty until login; guarded so case order does not matter.
        if (!m_controller->loggedIn()) {
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
        // The bottom cluster is `visible: app.loggedIn`; without login the cog
        // and avatar would be skipped.
        QVERIFY2(m_controller->loggedIn(),
                 "the fixture reports itself logged out, so the rail's "
                 "bottom cluster is hidden and cannot be pointed at");

        const qreal railWidth = m_rail->width();
        const QSize windowSize = m_window->size();
        const auto restore = qScopeGuard([&] {
            moveTo(QPoint(int(railWidth) + 40, 8));
            QTest::qWait(60);
            m_window->resize(windowSize);
            m_rail->setWidth(railWidth);
            entries()->setSources(m_spaces, store());
            QCoreApplication::processEvents();
        });
        m_window->resize(760, windowSize.height());
        m_rail->setWidth(railWidth);
        QCoreApplication::processEvents();
        QTest::qWait(80);

        QString leafId;
        const QVariantList all = m_controller->spaces()->allSpaces();
        for (const QVariant &entry : all) {
            const QVariantMap row = entry.toMap();
            const QString id =
                row.value(QStringLiteral("spaceId")).toString();
            if (id.isEmpty()
                || row.value(QStringLiteral("childSpaceCount")).toInt() > 0) {
                continue;
            }
            if (!m_controller->spaces()->directChildRoomsDetailed(id)
                     .isEmpty()) {
                leafId = id;
                break;
            }
        }
        QVERIFY2(!leafId.isEmpty(),
                 "the fixture has no Space with rooms and no subspaces, so "
                 "no revealed room can be pointed at");
        store()->setSpaceExpanded(leafId, true);
        QCoreApplication::processEvents();
        QTest::qWait(80);

        QQuickItem *leafRow = delegateFor(leafId);
        QVERIFY(leafRow);
        QQuickItem *spaceTile =
            descendantNamed(leafRow, QStringLiteral("railSpaceTile"));
        QQuickItem *roomTile =
            descendantNamed(leafRow, QStringLiteral("railRevealedRoomTile"));
        QVERIFY2(spaceTile, "the leaf Space draws no tile");
        QVERIFY2(roomTile, "the leaf Space revealed no room tile, so the "
                           "expansion column cannot be pointed at");

        struct Target
        {
            const char *what;
            QQuickItem *item;
        };
        const QList<Target> targets = {
            { "the Space tile", spaceTile },
            { "a revealed room tile", roomTile },
            { "the settings cog",
              m_rail->findChild<QQuickItem *>(
                  QStringLiteral("railSettingsButton")) },
            { "the account avatar",
              m_rail->findChild<QQuickItem *>(
                  QStringLiteral("railAccountTile")) },
            { "the add-Space button",
              m_rail->findChild<QQuickItem *>(
                  QStringLiteral("railAddSpaceButton")) },
        };

        int checked = 0;
        QStringList skipped;
        for (const Target &target : targets) {
            const QString what = QString::fromLatin1(target.what);
            if (!target.item) {
                skipped << what + QStringLiteral(" (no such item)");
                continue;
            }
            if (!target.item->isVisible() || target.item->width() <= 1
                || target.item->height() <= 1) {
                skipped << what
                           + QStringLiteral(" (visible=%1 %2x%3)")
                                 .arg(target.item->isVisible())
                                 .arg(target.item->width())
                                 .arg(target.item->height());
                continue;
            }
            const QRectF hovered = target.item->mapRectToScene(
                QRectF(0, 0, target.item->width(), target.item->height()));
            if (hovered.center().y() < 0
                || hovered.center().y() > m_window->height()) {
                // Scrolled out of the window; nothing to point at.
                skipped << what
                           + QStringLiteral(" (centre y=%1 outside the "
                                            "window)")
                                 .arg(hovered.center().y());
                continue;
            }

            // Park the pointer off the rail and let the previous tip close.
            moveTo(QPoint(int(railWidth) + 200, 8));
            QTest::qWait(120);
            moveTo(hovered.center().toPoint());
            // Past the longest delay any of these carries (500 ms).
            QTRY_VERIFY_WITH_TIMEOUT(
                visiblePopupItem(m_window->contentItem()) != nullptr, 3000);
            QQuickItem *tip = visiblePopupItem(m_window->contentItem());
            QVERIFY(tip);
            const QRectF tipRect =
                tip->mapRectToScene(QRectF(0, 0, tip->width(), tip->height()));
            ++checked;

            // (1) It does not paint on the rail, whose monogram is the only
            // identity cue.
            QVERIFY2(tipRect.left() >= railWidth - 0.5,
                     qPrintable(QStringLiteral(
                         "pointing at %1: the tooltip starts at x=%2 on a "
                         "rail %3 wide — it is painted over the tile column, "
                         "and Qt puts it ABOVE the attachee, so what it "
                         "covers is the tile before the one being pointed at")
                         .arg(QString::fromLatin1(target.what))
                         .arg(tipRect.left()).arg(railWidth)));

            // (2) It sits beside the row being pointed at.
            QVERIFY2(tipRect.center().y() >= hovered.top()
                         && tipRect.center().y() <= hovered.bottom(),
                     qPrintable(QStringLiteral(
                         "pointing at %1 (y %2-%3): the tooltip is centred at "
                         "y=%4, outside the thing it is supposed to be "
                         "describing — it is beside a different row")
                         .arg(QString::fromLatin1(target.what))
                         .arg(hovered.top()).arg(hovered.bottom())
                         .arg(tipRect.center().y())));
        }

        // Assert the count actually measured, not loop iterations. Four: the
        // add-Space "+" needs supportsRoomManagement(), which the mock does not
        // override, so it is never drawn here. Raise this to 5 if the mock
        // gains room management.
        QVERIFY2(checked >= 4,
                 qPrintable(QStringLiteral(
                     "only %1 of the rail's tooltip anchors could be pointed "
                     "at; this case cannot say anything about the rest. "
                     "Skipped: %2")
                     .arg(checked).arg(skipped.join(QStringLiteral("; ")))));
    }

    // `rowTop(i)` drives every drop decision and is derived by accumulating
    // `rowBand()` (delegate `y` animates for 140 ms), so the derivation must
    // match real row heights. The fixture needs a nested row that is not last
    // (a three-level chain): a nested tile is 8 px shorter and a group's last
    // row 8 px taller, so a nested last row cancels out.
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

        // The fixture must contain a nested row, or a constant band would pass.
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


    // A folder container draws behind the hierarchy regions it holds and is
    // wider than them; otherwise it paints over the nesting. Two numbers set
    // in different places, so only geometry can see it.
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

        // Strictly lower z; equal z would fall back to document order.
        QVERIFY2(container->z() < layers.at(0)->z(),
                 qPrintable(QStringLiteral(
                     "the folder container is at z=%1 and the region it "
                     "contains at z=%2 — the container paints over the "
                     "nesting inside it")
                     .arg(container->z()).arg(layers.at(0)->z())));

        // Wider than its contents.
        QVERIFY2(container->width() > layers.at(0)->width(),
                 qPrintable(QStringLiteral(
                     "the folder container is %1px wide and the depth-1 "
                     "region inside it is %2px — the container is narrower "
                     "than the thing it holds")
                     .arg(container->width()).arg(layers.at(0)->width())));

        // The filed tile stays on the shared axis.
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


    // Sibling runs of the same tint are separated by a gap (an inner run
    // ending spends one), while their parent's region stays whole; and a deep
    // row's chevron sits inside its own region. The left run is three deep:
    // at two levels the chevron check passes on the old gutter.
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

        // The last row of A's run, which ends it.
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

        // The inner runs separate.
        const qreal gap = m_rail->property("groupGap").toReal();
        QVERIFY2(gap > 0, "the rail reports no group gap at all");
        const qreal innerGap = topIn(bLayers.at(1)) - bottomIn(a1Layers.at(1));
        QVERIFY2(innerGap >= gap,
                 qPrintable(QStringLiteral(
                     "A's run ends %1px above where B's begins, and the gap "
                     "that separates one run from the next is %2 — the two "
                     "read as one shape with a notch in it")
                     .arg(innerGap).arg(gap)));

        // The parent's region is not split: the depth-1 region owns both runs
        // and must cover the gap between them.
        QVERIFY2(qAbs(topIn(bLayers.at(0)) - bottomIn(a1Layers.at(0))) < 1.0,
                 qPrintable(QStringLiteral(
                     "the depth-1 region ends at y=%1 on A1 and restarts at "
                     "y=%2 on B — the parent has a hole where its child's run "
                     "stopped").arg(bottomIn(a1Layers.at(0)))
                     .arg(topIn(bLayers.at(0)))));

        // The chevron sits inside the innermost region, checked at the
        // minimum rail width where the gutter has the least room.
        const qreal restoreWidth = m_rail->width();
        m_rail->setWidth(m_rail->property("minRailWidth").toReal());
        QCoreApplication::processEvents();
        QTest::qWait(80);

        // On the row with the most layers, where the innermost edge is
        // furthest right.
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
            // Measure the ink, not the em box: an Icon's item is the glyph's
            // advance, about a quarter of which is empty side bearing. The
            // rail places the mark by its ink (see `chevronGlyphX`).
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


    // At the minimum rail width the expander is never nearer the rail's edge
    // than it is to its own tile. Asserted as that relation, which holds at
    // every width and text scale; every other case here runs at 160 px.
    // Activity: a Space with unread and no mention shows the dot, one with
    // mentions shows the count, a quiet one shows neither. Each sits on its
    // tile's top-right corner and inside the rail at its narrowest.
    void activityIndicatorsSitOnTheTileCornerInsideTheRail()
    {
        const QString alpha = m_spaceIds.at(0);
        const QString bravo = m_spaceIds.at(1);
        const QString charlie = m_spaceIds.at(2);
        const QList<RoomInfo> saved = m_client->roomList;
        const qreal savedWidth = m_rail->width();
        auto restore = qScopeGuard([&] {
            m_rail->setWidth(savedWidth);
            entries()->setRoomSources(nullptr, nullptr);
            m_client->roomList = saved;
            Q_EMIT m_client->roomsChanged();
            QCoreApplication::processEvents();
        });

        RoomInfo unread = joinedRoom(QStringLiteral("!alpha-room:example.org"),
                                     QStringLiteral("Alpha room"));
        // No notification count: the case the count badge never showed.
        unread.hasUnreadMessages = true;
        RoomInfo mentioned =
            joinedRoom(QStringLiteral("!bravo-room:example.org"),
                       QStringLiteral("Bravo room"));
        mentioned.hasUnreadMessages = true;
        mentioned.unreadCount = 4;
        mentioned.highlightCount = 2;
        QList<RoomInfo> rooms = saved;
        for (RoomInfo &r : rooms) {
            if (r.id == alpha)
                r.childRoomIds = { unread.id };
            else if (r.id == bravo)
                r.childRoomIds = { mentioned.id };
        }
        rooms << unread << mentioned;
        m_client->roomList = rooms;
        entries()->setRoomSources(m_client, nullptr);
        Q_EMIT m_client->roomsChanged();
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(m_list->property("count").toInt(), 5, 3000);

        const qreal minWidth = m_rail->property("minRailWidth").toReal();
        QVERIFY2(minWidth > 0, "the rail reports no minimum width");
        m_rail->setWidth(minWidth);
        QCoreApplication::processEvents();
        QTest::qWait(60);

        struct Expect { QString id; bool dot; bool badge; };
        const Expect expected[] = {
            { alpha, true, false },
            { bravo, false, true },
            { charlie, false, false },
        };
        int placed = 0;
        for (const Expect &e : expected) {
            QQuickItem *row = delegateFor(e.id);
            QVERIFY2(row, qPrintable(e.id));
            QQuickItem *tile =
                descendantNamed(row, QStringLiteral("railSpaceTile"));
            QQuickItem *dot =
                descendantNamed(row, QStringLiteral("railUnreadDot"));
            QQuickItem *badge =
                descendantNamed(row, QStringLiteral("railMentionBadge"));
            QVERIFY2(tile && dot && badge,
                     "a Space tile carries no activity indicators");
            QTRY_COMPARE(dot->isVisible(), e.dot);
            QCOMPARE(badge->isVisible(), e.badge);

            QQuickItem *shown = e.dot ? dot : e.badge ? badge : nullptr;
            if (!shown)
                continue;
            if (e.badge) {
                QString text;
                for (QQuickItem *child : badge->childItems()) {
                    const QVariant t = child->property("text");
                    if (t.isValid())
                        text = t.toString();
                }
                // The mentions, not the notifications around them.
                QCOMPARE(text, QStringLiteral("2"));
            }
            const QRectF tileRect = tile->mapRectToItem(
                m_rail, QRectF(0, 0, tile->width(), tile->height()));
            const QRectF mark = shown->mapRectToItem(
                m_rail, QRectF(0, 0, shown->width(), shown->height()));
            // The list clips, so that is the visible area.
            const QRectF listRect = m_list->mapRectToItem(
                m_rail, QRectF(0, 0, m_list->width(), m_list->height()));
            QVERIFY2(mark.width() > 0 && mark.height() > 0, "zero-size mark");
            // On the corner: overlapping the tile, centred in its top-right
            // quadrant.
            QVERIFY(mark.intersects(tileRect));
            QVERIFY2(mark.center().x() > tileRect.center().x()
                         && mark.center().y() < tileRect.center().y(),
                     qPrintable(QStringLiteral("%1: mark at %2,%3 is not on "
                                               "the tile's top-right corner")
                                    .arg(e.id)
                                    .arg(mark.center().x())
                                    .arg(mark.center().y())));
            // Inside the rail and the list, so nothing clips it.
            QVERIFY2(mark.left() >= listRect.left()
                         && mark.right() <= listRect.right()
                         && mark.right() <= m_rail->width(),
                     qPrintable(QStringLiteral("%1: mark spans %2..%3 in a "
                                               "%4px rail")
                                    .arg(e.id)
                                    .arg(mark.left())
                                    .arg(mark.right())
                                    .arg(m_rail->width())));
            QVERIFY(mark.top() >= listRect.top());
            ++placed;
        }
        // Both marks were measured, not skipped.
        QCOMPARE(placed, 2);
    }

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
            // Measure the ink (see the sibling case).
            const qreal inkWidth =
                m_rail->property("chevronInkWidth").toReal();
            QVERIFY2(inkWidth > 0, "the rail reports no chevron ink width");
            const qreal inkCentre =
                glyph->mapToItem(m_rail, QPointF(0, 0)).x()
                + glyph->width() / 2;
            const qreal left = inkCentre - inkWidth / 2;
            const qreal right = inkCentre + inkWidth / 2;
            ++checked;
            // The reference is the tile's visible edge, which is the accent
            // ring drawn outside the tile (`tileRingOutset`).
            const qreal ringOutset =
                m_rail->property("tileRingOutset").toReal();
            QVERIFY2(ringOutset > 0, "the rail reports no ring outset");
            const qreal toTile =
                m_rail->property("tileColumnX").toReal() - ringOutset - right;
            QVERIFY2(left >= toTile,
                     qPrintable(QStringLiteral(
                         "at the minimum width of %1 the expander is %2px "
                         "from the rail's outer edge and %3px from the "
                         "visible edge of the tile it acts on — it reads as "
                         "hanging off the edge rather than as belonging to "
                         "the row")
                         .arg(minWidth).arg(left).arg(toTile)));
            QVERIFY2(right <= minWidth,
                     qPrintable(QStringLiteral(
                         "at the minimum width of %1 the expander ends at "
                         "x=%2, past the rail's own right edge")
                         .arg(minWidth).arg(right)));
            // The expander clears the ring, not just the tile: the active ring
            // is drawn outside the tile and paints over anything in that band,
            // only on the selected Space. Reads the real ring item.
            auto *ring = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceActiveRing"));
            QVERIFY2(ring, "the active ring is gone, so nothing here can say "
                           "whether the expander would collide with it");
            const qreal ringLeft = ring->mapToItem(m_rail, QPointF(0, 0)).x();
            // The plate is the control's edge, so the plate must clear the
            // ring (the ink sits `chevronPlatePad` inside it).
            auto *plate = row->findChild<QQuickItem *>(
                QStringLiteral("railSpaceExpandPlate"));
            QVERIFY2(plate, "the expander has no plate, so this measures a "
                            "control that is not the one on screen");
            const qreal plateLeft =
                plate->mapToItem(m_rail, QPointF(0, 0)).x();
            const qreal plateRight = plateLeft + plate->width();
            QVERIFY2(ringLeft - plateRight >= 1.0,
                     qPrintable(QStringLiteral(
                         "the expander's plate ends at x=%1 and the active "
                         "ring starts at x=%2 — on a SELECTED Space they "
                         "overlap")
                         .arg(plateRight).arg(ringLeft)));
            // The plate stays inside its own region (`chevronSlotLeft`).
            QVERIFY2(plateLeft >= m_rail->property("chevronSlotLeft").toReal()
                                  - 0.01,
                     qPrintable(QStringLiteral(
                         "the expander's plate starts at x=%1, left of the "
                         "deepest region inset %2 — at depth it is drawn "
                         "outside the region it acts on")
                         .arg(plateLeft)
                         .arg(m_rail->property("chevronSlotLeft").toReal())));
            QVERIFY2(right <= plateRight && left >= plateLeft,
                     qPrintable(QStringLiteral(
                         "the glyph's ink spans %1..%2 and its plate spans "
                         "%3..%4 — the mark is not inside its own box")
                         .arg(left).arg(right).arg(plateLeft).arg(plateRight)));
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


    // Depth costs the tiles no horizontal room: eight fully expanded levels,
    // every tile on the root's axis. A shallow fixture could not tell a small
    // per-level step from rounding.
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

        // A row draws one region per ancestor, capped (`maxBandLayers`) so
        // the innermost region never becomes narrower than the tile.
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
            // One per ancestor, plus the row's own when it owns the run below
            // it. Every row of this chain but the last owns one.
            const bool owns = row->property("ownsRegion").toBool();
            QCOMPARE(layers.size(),
                     std::min(maxLayers, level + (owns ? 1 : 0)));

            // Strictly nested: each layer starts further in and is narrower
            // than the one containing it.
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

            // The outermost layer has the same shape behind every depth, so it
            // reads as one region behind its descendants.
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

        // No two touching regions share a tint: past the layer cap the
        // innermost layer alternates between the last two rungs, so a parent
        // and the child drawn on it always differ.
        QColor previousInnermost;
        int alternationsChecked = 0;
        for (const QString &id : chain) {
            QQuickItem *row = delegateFor(id);
            if (!row || !row->isVisible())
                continue;
            // Only rows that own a region: a leaf's innermost layer is its
            // parent's by construction.
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

        // Nothing is drawn outside the region that contains it: the cap seam
        // moves a child's band down, and the tile must be positioned from the
        // band, not the row top. Checked on real delegates.
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


    // The region ladder's rungs are even on every theme preset. They are
    // derived from each theme's colours, and one alpha ladder gives smaller
    // luminance steps on light themes than dark ones (sRGB curve). Reads the
    // live singleton, since a text scan cannot evaluate `Qt.tint()`.
    void theRegionLadderIsEvenOnEveryTheme()
    {
        auto *theme = m_engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY2(theme, "no AppTheme singleton — this case cannot read the "
                        "region ladder and so proves nothing");
        auto *settings = m_controller->settings();
        QVERIFY2(settings, "no SettingsManager, so no theme can be selected");
        const int original = settings->property("theme").toInt();

        // WCAG relative luminance: the ladder is about perceptual steps.
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
        // Switch the palette on the singleton too: `AppTheme.mode` is driven
        // by a Binding in Main.qml, which this suite does not load, so writing
        // `settings.theme` alone would measure one palette eleven times.
        QSet<QString> palettesSeen;
        for (int t = 1; t <= 11; ++t) {
            settings->setProperty("theme", t);
            theme->setProperty("mode", t);
            QCoreApplication::processEvents();
            QTest::qWait(30);
            if (settings->property("theme").toInt() != t
                || theme->property("mode").toInt() != t)
                continue;   // a preset this build does not carry

            const QColor rail = theme->property("rail").value<QColor>();
            const QVariantList rungs =
                theme->property("railNestSurfaces").toList();
            // A fingerprint per palette (rail colour plus deepest rung), so
            // "eleven palettes" is checkable.
            palettesSeen.insert(
                rail.name()
                + rungs.at(rungs.size() - 1).value<QColor>().name());
            QVERIFY2(rungs.size() >= 4,
                     qPrintable(QStringLiteral(
                         "theme %1 exposes %2 region rungs")
                         .arg(t).arg(rungs.size())));
            ++themesChecked;

            QColor previous = rail;
            QList<double> steps;
            // Rung 0 is a folder's container and deliberately the quietest;
            // start at hierarchy depth 1.
            for (int i = 1; i < rungs.size(); ++i) {
                const QColor rung = rungs.at(i).value<QColor>();
                const double ratio = contrast(previous, rung);
                // A low floor: the rail is chrome and must recede, so its
                // steps are small. It catches a rung collapsing into its
                // neighbour; the evenness check below is the real assertion.
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
            // Even: steps differing by more than half again make one loud
            // boundary and some whispers.
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
        // They were different palettes, not one measured eleven times.
        QVERIFY2(palettesSeen.size() >= 8,
                 qPrintable(QStringLiteral(
                     "%1 presets were selected but only %2 distinct palettes "
                     "came back, so this case is measuring one theme over and "
                     "over").arg(themesChecked).arg(palettesSeen.size())));

        settings->setProperty("theme", original);
        theme->setProperty("mode", original);
        QCoreApplication::processEvents();
        QTest::qWait(30);
    }


    // Every piece of rail geometry follows the interface size, asserted as
    // ratios to the Space tile (a revealed room is 0.7 of it, the bottom chips
    // exactly one tile), never as pixel counts.
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

        // Restore 100% and the suite's own hierarchy whatever happens; other
        // cases read geometry from both.
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

        // The ratios that make the rail read as one column, with one pixel of
        // slack for Math.round.
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

    // The expander's plate is two rungs off the region it sits on. A region
    // layer at index i draws depth i + 1, so `bandTint(bandLayers - 1)` named
    // the layer outside the innermost one and the plate differed by only one
    // rung (and by two on a collapsed Space). Asserted on the colour property,
    // not a grab.
    void theExpanderPlateIsTwoRungsOffTheRegionItSitsOn()
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
        const QStringList ids = { QStringLiteral("!lv0:example.org"),
                                  QStringLiteral("!lv1:example.org"),
                                  QStringLiteral("!lv2:example.org") };
        for (const QString &id : ids)
            store()->setSpaceExpanded(id, true);
        entries()->setSources(&nestedSpaces, store());
        QCoreApplication::processEvents();
        // Past the plate's 90 ms ColorAnimation.
        QTest::qWait(150);

        const auto lstar = [](const QColor &c) {
            const auto lin = [](qreal v) {
                return v <= 0.04045 ? v / 12.92
                                    : std::pow((v + 0.055) / 1.055, 2.4);
            };
            const qreal y = 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF())
                            + 0.0722 * lin(c.blueF());
            return y > 0.008856 ? 116.0 * std::cbrt(y) - 16.0 : 903.3 * y;
        };

        int measured = 0;
        for (const QString &id : ids) {
            QQuickItem *row = delegateFor(id);
            QVERIFY2(row, qPrintable(QStringLiteral(
                              "no rail row for %1 — the nested fixture did "
                              "not build").arg(id)));
            QQuickItem *plate = descendantNamed(
                row, QStringLiteral("railSpaceExpandPlate"));
            QVERIFY2(plate, qPrintable(QStringLiteral(
                                "row %1 draws no expander plate").arg(id)));
            QList<QQuickItem *> layers;
            collectDescendantsNamed(row, QStringLiteral("railGroupField"),
                                    layers);
            QVERIFY2(!layers.isEmpty(),
                     qPrintable(QStringLiteral(
                         "row %1 draws no region, so there is nothing for "
                         "the plate to step off").arg(id)));
            // The innermost layer is the narrowest.
            QQuickItem *inner = layers.first();
            for (QQuickItem *l : layers) {
                if (l->width() < inner->width())
                    inner = l;
            }
            const QColor plateColour = plate->property("color").value<QColor>();
            const QColor bandColour = inner->property("color").value<QColor>();
            const qreal step = qAbs(lstar(plateColour) - lstar(bandColour));
            QVERIFY2(step >= 5.0,
                     qPrintable(QStringLiteral(
                         "%1: the expander plate is %2 (L* %3) on a region "
                         "of %4 (L* %5) — %6 ΔL* apart. One rung of this "
                         "ladder is ~3.4, which is what separates two "
                         "REGIONS; the plate is a CONTROL and its own source "
                         "asks for two rungs (~6.8), or it reads as another "
                         "band.")
                         .arg(id, plateColour.name())
                         .arg(lstar(plateColour), 0, 'f', 2)
                         .arg(bandColour.name())
                         .arg(lstar(bandColour), 0, 'f', 2)
                         .arg(step, 0, 'f', 2)));
            ++measured;
        }
        // The count, not just the items.
        QCOMPARE(measured, 3);

        entries()->setSources(m_spaces, store());
        QCoreApplication::processEvents();
    }

    // A tooltip wider than its anchor still hangs off the rail. Qt's Basic
    // style centres an attached tooltip on its anchor, so a tip wider than the
    // anchor spills left over the rail; the anchor must be at least as wide as
    // the tip.
    void aTooltipWiderThanItsAnchorStillHangsOffTheRail()
    {
        const qreal railWidth = m_rail->width();
        const QSize windowSize = m_window->size();
        const auto restore = qScopeGuard([&] {
            moveTo(QPoint(int(railWidth) + 40, 8));
            QTest::qWait(60);
            m_window->resize(windowSize);
            m_rail->setWidth(railWidth);
            entries()->setSources(m_spaces, store());
            QCoreApplication::processEvents();
        });
        // Wide enough that Qt's popup positioner never pulls the tip back
        // inside the window, which would hide the defect.
        m_window->resize(900, windowSize.height());
        m_rail->setWidth(railWidth);
        QCoreApplication::processEvents();

        RailFakeClient wide;
        wide.roomList = {
            joinedSpace(QStringLiteral("!wide:example.org"),
                        QStringLiteral("A Space whose name is longer than "
                                       "any anchor floor")),
        };
        SpaceManager wideSpaces;
        wideSpaces.setClient(&wide);
        entries()->setSources(&wideSpaces, store());
        QCoreApplication::processEvents();
        QTest::qWait(80);

        QQuickItem *row = delegateFor(QStringLiteral("!wide:example.org"));
        QVERIFY2(row, "the long-name fixture built no rail row");
        QQuickItem *tile =
            descendantNamed(row, QStringLiteral("railSpaceTile"));
        QVERIFY2(tile, "the long-name Space draws no tile to point at");

        const QRectF hovered = tile->mapRectToScene(
            QRectF(0, 0, tile->width(), tile->height()));
        moveTo(QPoint(int(railWidth) + 300, 8));
        QTest::qWait(120);
        moveTo(hovered.center().toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(
            visiblePopupItem(m_window->contentItem()) != nullptr, 3000);
        QQuickItem *tip = visiblePopupItem(m_window->contentItem());
        QVERIFY(tip);
        const QRectF tipRect =
            tip->mapRectToScene(QRectF(0, 0, tip->width(), tip->height()));

        // Non-vacuity: the tip must be wider than the anchor's floor, or
        // centring cannot spill. Read from the rail (150 is the fallback when
        // the property is absent).
        const QVariant floorProperty = m_rail->property("railTipFloor");
        const qreal floor =
            floorProperty.isValid() ? floorProperty.toReal() : 150.0;
        QVERIFY2(tipRect.width() > floor + 1.0,
                 qPrintable(QStringLiteral(
                     "the fixture's tooltip is only %1px wide against an "
                     "anchor floor of %2 — it cannot spill, so this case "
                     "proves nothing. Lengthen the Space name.")
                     .arg(tipRect.width()).arg(floor)));

        QVERIFY2(tipRect.left() >= railWidth - 0.5,
                 qPrintable(QStringLiteral(
                     "a %1px tooltip on a %2px anchor starts at x=%3 on a "
                     "rail %4 wide: Qt centres an attached tip on its "
                     "anchor, so everything past the anchor's width is "
                     "painted back over the tile column — over the very "
                     "tile the tip is naming")
                     .arg(tipRect.width()).arg(floor)
                     .arg(tipRect.left()).arg(railWidth)));
    }

    // The group ring is drawn outside the avatar that fills the tile. A
    // Rectangle's border paints inside its bounds, under a full-bleed Avatar,
    // so a border-based ring was never visible. Asserted geometrically.
    void theGroupRingIsDrawnOutsideTheAvatarThatFillsTheTile()
    {
        // Driven through the model: the cases above prove a real drag reaches
        // hoverGroup(); this is about where the ring is drawn.
        const int bravoRow = entries()->rowForEntry(m_spaceIds.at(1));
        QVERIFY(bravoRow >= 0);
        QVERIFY(entries()->beginDrag(m_spaceIds.at(0)));
        const auto restore = qScopeGuard([&] {
            entries()->endDrag(false);
            QCoreApplication::processEvents();
        });
        entries()->hoverGroup(bravoRow);
        QCoreApplication::processEvents();
        // Past the tile's 90 ms scale Behavior.
        QTest::qWait(200);
        QVERIFY2(entries()->grouping(),
                 "the model refused to offer a group, so no tile is a drop "
                 "target and there is nothing to measure");

        QQuickItem *row = delegateFor(m_spaceIds.at(1));
        QVERIFY(row);
        QVERIFY2(row->property("dropTarget").toBool(),
                 "the delegate does not know it is the drop target");
        QQuickItem *tile =
            descendantNamed(row, QStringLiteral("railSpaceTile"));
        QVERIFY(tile);
        QQuickItem *ring =
            descendantNamed(row, QStringLiteral("railSpaceDropRing"));
        QVERIFY2(ring,
                 "the drop target draws no group ring at all — the only "
                 "feedback a release-here-to-merge gesture has is the 8% "
                 "scale-up");
        QVERIFY2(ring->isVisible(),
                 "the group ring exists but is not visible on the tile a "
                 "release would file into");

        // Scene rects, so the tile's 1.08 drop-target scale is included: a
        // ring anchored to the tile's unscaled bounds would be grown through.
        const QRectF tileRect =
            tile->mapRectToScene(QRectF(0, 0, tile->width(), tile->height()));
        const QRectF ringRect =
            ring->mapRectToScene(QRectF(0, 0, ring->width(), ring->height()));
        QVERIFY(tileRect.width() > 0 && ringRect.width() > 0);

        const qreal stroke = ring->property("border").value<QObject *>()
                                 ? ring->property("border")
                                       .value<QObject *>()
                                       ->property("width").toReal()
                                 : 0.0;
        QVERIFY2(stroke >= 1.0,
                 qPrintable(QStringLiteral(
                     "the group ring's stroke is %1px wide").arg(stroke)));

        struct Side
        {
            const char *name;
            qreal outside;
        };
        const QList<Side> sides = {
            { "left", tileRect.left() - ringRect.left() },
            { "top", tileRect.top() - ringRect.top() },
            { "right", ringRect.right() - tileRect.right() },
            { "bottom", ringRect.bottom() - tileRect.bottom() },
        };
        int measured = 0;
        for (const Side &side : sides) {
            QVERIFY2(side.outside >= stroke - 0.5,
                     qPrintable(QStringLiteral(
                         "the group ring's %1 edge is only %2px outside the "
                         "tile against a %3px stroke, so that much of it is "
                         "painted inside the bounds `Avatar { anchors.fill: "
                         "parent }` covers — which is the whole defect: a "
                         "ring that is set, drawn, and never seen")
                         .arg(QString::fromLatin1(side.name))
                         .arg(side.outside).arg(stroke)));
            ++measured;
        }
        // The count of sides compared, not the loop's length.
        QCOMPARE(measured, 4);
    }

    // Every derived tile centres on the same axis at every rail width. A
    // derived tile is placed at `tileColumnX + round((railTileSize -
    // rowTileSize) / 2)`, exact only when both sizes have the same parity;
    // otherwise Math.round shifts it half a pixel.
    void everyDerivedTileCentresOnTheSameAxisAtEveryRailWidth()
    {
        const qreal railWidth = m_rail->width();
        const auto restore = qScopeGuard([&] {
            m_rail->setWidth(railWidth);
            QCoreApplication::processEvents();
        });

        const int minWidth = m_rail->property("minRailWidth").toInt();
        const int maxWidth = m_rail->property("maxRailWidth").toInt();
        QVERIFY2(minWidth > 0 && maxWidth >= minWidth,
                 qPrintable(QStringLiteral(
                     "the rail reports a width range of %1..%2")
                     .arg(minWidth).arg(maxWidth)));

        int measured = 0;
        QStringList offenders;
        for (int w = minWidth; w <= maxWidth; ++w) {
            m_rail->setWidth(w);
            QCoreApplication::processEvents();

            const int tile = m_rail->property("railTileSize").toInt();
            const int columnX = m_rail->property("tileColumnX").toInt();
            const QList<QPair<QString, int>> tiers = {
                { QStringLiteral("the nested Space tile"),
                  m_rail->property("railNestedTileSize").toInt() },
                { QStringLiteral("a revealed room's tile"),
                  m_rail->property("railRoomTileSize").toInt() },
            };
            for (const auto &tier : tiers) {
                // The delegate's placement, restated from SpacesRail.qml.
                const qreal derivedX =
                    columnX + qRound((tile - tier.second) / 2.0);
                const qreal derivedCentre = derivedX + tier.second / 2.0;
                const qreal columnCentre = columnX + tile / 2.0;
                if (!qFuzzyCompare(derivedCentre, columnCentre)) {
                    offenders << QStringLiteral(
                        "rail %1 (tile %2): %3 is %4px at x=%5, centre %6 "
                        "against the column's %7")
                        .arg(w).arg(tile).arg(tier.first).arg(tier.second)
                        .arg(derivedX).arg(derivedCentre).arg(columnCentre);
                }
                ++measured;
            }
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "%1 of %2 tier/width pairs put a derived tile off the "
                     "column's axis — the rail's one stated rule is that "
                     "every tile shares one x. A derived size must have the "
                     "SAME PARITY as the Space tile or the halved difference "
                     "rounds away from centre. %3")
                     .arg(offenders.size()).arg(measured)
                     .arg(offenders.join(QStringLiteral("; ")))));
        // The count of pairs measured (nine widths, two derived tiers), not
        // the loop bound.
        QCOMPARE(measured, (maxWidth - minWidth + 1) * 2);
        QVERIFY2(measured >= 18,
                 qPrintable(QStringLiteral(
                     "only %1 tier/width pairs exist, so this case says "
                     "almost nothing about the range a reader can drag to")
                     .arg(measured)));
    }

};

int main(int argc, char *argv[])
{
    // Qt Quick item creation needs a QGuiApplication, as in main.cpp.
    QGuiApplication app(argc, argv);
    RailDragQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "RailDragQmlTest.moc"
