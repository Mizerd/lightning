// The reaction picker over the real TimelinePane on the mock backend, driven
// by real pointer and key events:
//  1. It opens COMPACT (a quick-reaction bar) and expands to a compact grid,
//     both next to the message that opened it and on screen, at 1366x768 and
//     1920x1080 — even when the composer's picker was enlarged, because the two
//     contexts remember their sizes under separate keys.
//  2. The grid packs its cells at every size: no "Recently used" spread over a
//     sparse grid of 125 px cells.
//  3. Dragging the resize grip resizes the picker by its FREE corner (the one
//     under the pointer) and never scrolls the timeline underneath, including
//     a press a few pixels outside the visible corner.
//  4. Keyboard: arrows and Enter react from the quick bar, typing searches, Esc
//     closes.
// Geometry is asserted, not source text: every defect here passed a scan.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QLineF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "models/EmojiCatalog.h"
#include "models/RoomListModel.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 4000;

QRectF sceneRect(const QQuickItem *item)
{
    return QRectF(item->mapToScene(QPointF(0, 0)),
                  QSizeF(item->width(), item->height()));
}

QString rectText(const QRectF &r)
{
    return QStringLiteral("(%1,%2 %3x%4)")
        .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QQuickItem *findByClass(QQuickItem *root, const char *className)
{
    QList<QQuickItem *> queue{ root };
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (QString::fromLatin1(item->metaObject()->className())
                .startsWith(QLatin1String(className)))
            return item;
        queue.append(item->childItems());
    }
    return nullptr;
}
}

class ReactionPickerQmlTest : public QObject
{
    Q_OBJECT

    // One real pane over a populated mock room.
    struct Scene {
        AppController controller{ AppController::MockBackend };
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *root = nullptr;
        QQuickItem *timeline = nullptr;
        QObject *picker = nullptr;
        QQuickItem *row = nullptr;
        QString eventId;
    };

    static QString loginAndRoomIdAt(AppController &controller, int row)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() <= row; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() <= row)
            return {};
        const QModelIndex idx = controller.roomList()->index(row, 0);
        return controller.roomList()
            ->data(idx, RoomListModel::RoomIdRole)
            .toString();
    }

    static QList<TimelineEvent> textFixture(const QString &roomId, int count)
    {
        const QDateTime base =
            QDateTime::currentDateTimeUtc().addSecs(-40000);
        QList<TimelineEvent> events;
        for (int i = 0; i < count; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$react%1").arg(i);
            e.itemId = QStringLiteral("uid-react%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("reaction picker fixture line %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        return events;
    }

    static QQuickItem *itemAtViewRow(QQuickItem *timeline, int viewRow)
    {
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(viewRow))))
            return nullptr;
        return out.value<QQuickItem *>();
    }

    static bool buildScene(Scene &s, const QSize &size)
    {
        const QString roomId = loginAndRoomIdAt(s.controller, 0);
        if (roomId.isEmpty())
            return false;
        auto *mock = s.controller.findChild<MockMatrixClient *>();
        if (!mock)
            return false;
        s.controller.setCurrentRoomId(roomId);
        s.engine.rootContext()->setContextProperty("app", &s.controller);
        QSignalSpy createdSpy(&s.engine, &QQmlApplicationEngine::objectCreated);
        s.engine.loadFromModule(QStringLiteral("MatrixClient"),
                                QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        s.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!s.root)
            return false;
        s.window.resize(size);
        s.root->setParentItem(s.window.contentItem());
        s.root->setSize(QSizeF(size));
        s.window.show();
        QTest::qWaitForWindowExposed(&s.window, 2000);
        s.window.requestActivate();
        QCoreApplication::processEvents();

        mock->resetTimelineForTest(roomId, textFixture(roomId, 80), 0);
        s.timeline = s.root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!s.timeline)
            return false;
        if (!QTest::qWaitFor([&] {
                return s.timeline->property("presentationReady").toBool();
            }, 5000))
            return false;
        s.picker = s.root->findChild<QObject *>(
            QStringLiteral("sharedReactionPicker"));
        return s.picker != nullptr;
    }

    static QQuickItem *rowNear(Scene &s, double fraction, QString *eventId)
    {
        const int count = s.timeline->property("count").toInt();
        const double wantY = s.window.height() * fraction;
        QQuickItem *best = nullptr;
        double bestDist = 1e9;
        for (int row = 0; row < count; ++row) {
            QQuickItem *item = itemAtViewRow(s.timeline, row);
            if (!item || item->height() <= 0 || !item->isVisible())
                continue;
            const QString id = item->property("eventId").toString().isEmpty()
                ? QString()
                : item->property("eventId").toString();
            if (item->property("actionKey").toString().isEmpty())
                continue;
            const QRectF r = sceneRect(item);
            if (r.bottom() < 0 || r.top() > s.window.height())
                continue;
            const double d = std::abs(r.center().y() - wantY);
            if (d < bestDist) {
                bestDist = d;
                best = item;
                if (eventId)
                    *eventId = id;
            }
        }
        return best;
    }

    static bool openFromRow(Scene &s, QQuickItem *row)
    {
        s.row = row;
        QQmlContext anchorContext(qmlContext(row));
        anchorContext.setContextProperty(QStringLiteral("testAnchorRow"), row);
        QQmlExpression open(&anchorContext, row,
                            QStringLiteral("openReactionPickerFor("
                                           "eventIdForActions(), testAnchorRow)"));
        open.evaluate();
        if (open.hasError()) {
            qWarning("%s", qPrintable(open.error().toString()));
            return false;
        }
        return QTest::qWaitFor([&] {
            return s.picker->property("opened").toBool();
        }, kSignalTimeoutMs);
    }

    static QQuickItem *panelOf(QObject *picker)
    {
        if (auto *p = picker->findChild<QQuickItem *>(
                QStringLiteral("emojiPickerPanel")))
            if (p->isVisible())
                return p;
        auto *content = picker->property("contentItem").value<QQuickItem *>();
        return content ? content->parentItem() : nullptr;
    }

    static void expand(QObject *picker)
    {
        QMetaObject::invokeMethod(picker, "expand",
                                  Q_ARG(QVariant, QVariant(QString())));
        QCoreApplication::processEvents();
    }

    static QString placementProblem(const QRectF &r, const QSize &win,
                                    const QPointF &anchor)
    {
        if (r.left() < -0.5 || r.top() < -0.5 || r.right() > win.width() + 0.5
            || r.bottom() > win.height() + 0.5)
            return QStringLiteral("%1 is not inside the %2x%3 window")
                .arg(rectText(r)).arg(win.width()).arg(win.height());
        const double below = std::abs(r.top() - anchor.y());
        const double above = std::abs(anchor.y() - r.bottom());
        if (std::min(below, above) > 12.0)
            return QStringLiteral("%1 is not next to its anchor y=%2 "
                                  "(gap below %3, above %4)")
                .arg(rectText(r)).arg(anchor.y()).arg(below).arg(above);
        if (anchor.x() < r.left() - 0.5 || anchor.x() > r.right() + 0.5) {
            const bool pushed = r.left() <= 12.5
                || r.right() >= win.width() - 12.5;
            if (!pushed)
                return QStringLiteral("%1 does not cover its anchor x=%2")
                    .arg(rectText(r)).arg(anchor.x());
        }
        return {};
    }

private Q_SLOTS:
    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void reactionPickerOpensCompactNextToItsMessage_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<double>("rowFraction");
        QTest::newRow("1366x768 upper row") << QSize(1366, 768) << 0.25;
        QTest::newRow("1366x768 lower row") << QSize(1366, 768) << 0.85;
        QTest::newRow("1920x1080 upper row") << QSize(1920, 1080) << 0.25;
        QTest::newRow("1920x1080 lower row") << QSize(1920, 1080) << 0.85;
    }

    void reactionPickerOpensCompactNextToItsMessage()
    {
        QFETCH(QSize, size);
        QFETCH(double, rowFraction);
        {
            QSettings settings;
            settings.setValue(QStringLiteral("pickers/picker/widthShare"), 900);
            settings.setValue(QStringLiteral("pickers/picker/heightShare"), 900);
            settings.sync();
        }
        Scene s;
        QVERIFY(buildScene(s, size));
        QCOMPARE(s.controller.settings()->pickerWidthShare(
                     QStringLiteral("picker")), 900);

        QQuickItem *row = rowNear(s, rowFraction, nullptr);
        QVERIFY2(row != nullptr, "no message row on screen");
        const QPointF anchor = row->mapToScene(
            QPointF(row->width() / 2, row->height()));
        QVERIFY(openFromRow(s, row));

        QQuickItem *panel = panelOf(s.picker);
        QVERIFY(panel != nullptr);
        QRectF r = sceneRect(panel);
        QVERIFY2(r.width() <= 420 && r.height() <= 80,
                 qPrintable(QStringLiteral("the reaction picker opened at %1, "
                                           "not as a compact quick bar")
                                .arg(rectText(r))));
        QString problem = placementProblem(r, size, anchor);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        auto *quickBar = s.picker->findChild<QQuickItem *>(
            QStringLiteral("reactionQuickBar"));
        QVERIFY(quickBar != nullptr && quickBar->isVisible());
        QVERIFY(quickBar->property("quickCount").toInt() >= 6);

        expand(s.picker);
        QTRY_VERIFY(s.picker->property("expanded").toBool());
        QTRY_VERIFY(panel->height() > 200);
        r = sceneRect(panel);
        QVERIFY2(r.width() >= 300 && r.width() <= 400 && r.height() >= 340
                     && r.height() <= 460,
                 qPrintable(QStringLiteral("the expanded reaction grid is %1, "
                                           "not compact (about 360x420)")
                                .arg(rectText(r))));
        problem = placementProblem(r, size, anchor);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        auto *grip = s.picker->findChild<QQuickItem *>(
            QStringLiteral("popupResizeGrip"));
        QVERIFY(grip != nullptr);
        QTRY_VERIFY(grip->isVisible());
        QCOMPARE(grip->width(), grip->height());
        const QPointF g = sceneRect(grip).center();
        double nearest = 1e9;
        for (const QPointF &c : { r.topLeft(), r.topRight(), r.bottomLeft(),
                                  r.bottomRight() })
            nearest = std::min(nearest, QLineF(g, c).length());
        QVERIFY2(nearest <= grip->width(),
                 qPrintable(QStringLiteral("the grip %1 is %2 px from the "
                                           "nearest corner of the panel %3")
                                .arg(rectText(sceneRect(grip))).arg(nearest)
                                .arg(rectText(r))));
    }

    void theGridHasNoSparseGapsAtAnySize_data()
    {
        QTest::addColumn<QSize>("pickerSize");
        QTest::newRow("compact") << QSize(0, 0);
        QTest::newRow("dragged large") << QSize(1000, 700);
    }

    void theGridHasNoSparseGapsAtAnySize()
    {
        QFETCH(QSize, pickerSize);
        Scene s;
        QVERIFY(buildScene(s, QSize(1366, 768)));
        const QStringList recents = {
            QString::fromUtf8("\xF0\x9F\x91\x80"), QString::fromUtf8("\xF0\x9F\x91\x8D"),
            QString::fromUtf8("\xF0\x9F\x99\x8F"), QString::fromUtf8("\xF0\x9F\x98\x85"),
            QString::fromUtf8("\xF0\x9F\x98\x84"), QString::fromUtf8("\xF0\x9F\x91\x8C"),
            QString::fromUtf8("\xF0\x9F\x98\x9C"), QString::fromUtf8("\xF0\x9F\x92\xAF"),
            QString::fromUtf8("\xF0\x9F\x8F\x93"), QString::fromUtf8("\xE2\x9C\x8B"),
            QString::fromUtf8("\xF0\x9F\x98\x83"), QString::fromUtf8("\xF0\x9F\xA4\xA3"),
        };
        QVERIFY2(s.controller.emojiCatalog()->catalogueCount() > 0,
                 "the emoji catalogue did not load");
        for (const QString &e : recents)
            s.controller.emojiCatalog()->recordUse(e);
        QVERIFY2(s.controller.emojiCatalog()->recentEmoji().size()
                     == recents.size(),
                 qPrintable(QStringLiteral("%1 of %2 recents recorded")
                                .arg(s.controller.emojiCatalog()
                                         ->recentEmoji().size())
                                .arg(recents.size())));
        s.controller.emojiCatalog()->setCategory(QStringLiteral("Recently Used"));

        QQuickItem *row = rowNear(s, 0.3, nullptr);
        QVERIFY(row != nullptr);
        QVERIFY(openFromRow(s, row));
        expand(s.picker);
        if (pickerSize.width() > 0)
            QVERIFY(QMetaObject::invokeMethod(
                s.picker, "resizeTo",
                Q_ARG(QVariant, QVariant(double(pickerSize.width()))),
                Q_ARG(QVariant, QVariant(double(pickerSize.height())))));
        QCoreApplication::processEvents();

        auto *content =
            s.picker->property("contentItem").value<QQuickItem *>();
        QVERIFY(content != nullptr);
        QQuickItem *grid = findByClass(content, "QQuickGridView");
        QVERIFY2(grid != nullptr, "the picker has no grid");
        QTRY_VERIFY(grid->width() > 0);
        QTRY_COMPARE(grid->property("count").toInt(), recents.size());

        const double cellWidth = grid->property("cellWidth").toDouble();
        const double cellHeight = grid->property("cellHeight").toDouble();
        QVERIFY(cellWidth > 0 && cellHeight > 0);
        const int columns = int(grid->width() / cellWidth);
        QVERIFY2(cellWidth <= cellHeight * 1.34,
                 qPrintable(QStringLiteral("a %1 px wide grid lays out %2 "
                                           "columns of %3x%4 px cells")
                                .arg(grid->width()).arg(columns)
                                .arg(cellWidth).arg(cellHeight)));
        QVERIFY2(grid->width() - columns * cellWidth < cellWidth,
                 qPrintable(QStringLiteral("%1 columns of %2 px leave %3 px "
                                           "unused in a %4 px grid")
                                .arg(columns).arg(cellWidth)
                                .arg(grid->width() - columns * cellWidth)
                                .arg(grid->width())));
    }

    void draggingTheGripResizesWithoutScrollingTheTimeline_data()
    {
        QTest::addColumn<int>("offset");
        QTest::addColumn<int>("direction");
        QTest::newRow("on the arc, shrink") << 8 << 1;
        QTest::newRow("on the arc, grow") << 8 << -1;
        QTest::newRow("on the corner, shrink") << 1 << 1;
        QTest::newRow("3px outside the corner, shrink") << -3 << 1;
        QTest::newRow("3px outside the corner, grow") << -3 << -1;
    }

    void draggingTheGripResizesWithoutScrollingTheTimeline()
    {
        QFETCH(int, offset);
        QFETCH(int, direction);
        Scene s;
        QVERIFY(buildScene(s, QSize(1366, 768)));
        QQuickItem *row = rowNear(s, 0.2, nullptr);
        QVERIFY(row != nullptr);

        double minY = 0, maxY = 0;
        {
            QVariant a, b;
            QVERIFY(QMetaObject::invokeMethod(s.timeline, "wheelMinY",
                                              Q_RETURN_ARG(QVariant, a)));
            QVERIFY(QMetaObject::invokeMethod(s.timeline, "wheelMaxY",
                                              Q_RETURN_ARG(QVariant, b)));
            minY = a.toDouble();
            maxY = b.toDouble();
        }
        QVERIFY2(maxY - minY > 600, "the fixture room cannot scroll");
        s.timeline->setProperty("stickToBottom", false);
        s.timeline->setProperty("contentY", (minY + maxY) / 2);
        QTest::qWait(350);
        row = rowNear(s, 0.2, nullptr);
        QVERIFY(row != nullptr);
        QVERIFY(openFromRow(s, row));
        expand(s.picker);
        QTest::qWait(50);
        const double contentYBefore =
            s.timeline->property("contentY").toDouble();

        auto *grip = s.picker->findChild<QQuickItem *>(
            QStringLiteral("popupResizeGrip"));
        QVERIFY(grip != nullptr);
        QTRY_VERIFY(grip->isVisible() && grip->width() > 0);
        QQuickItem *panel = panelOf(s.picker);
        QVERIFY(panel != nullptr);
        const QRectF before = sceneRect(panel);

        const QPointF gripCentre = sceneRect(grip).center();
        const bool left = gripCentre.x() < before.center().x();
        const bool top = gripCentre.y() < before.center().y();
        const QPointF corner(left ? before.left() : before.right(),
                             top ? before.top() : before.bottom());
        const QPointF inward(left ? 1 : -1, top ? 1 : -1);
        const QPoint start = (corner + inward * offset).toPoint();
        const QPointF travel = inward * (direction > 0 ? 30.0 : -48.0);

        QTest::mousePress(&s.window, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&s.window,
                             (QPointF(start) + travel * (step / 8.0)).toPoint());
            QCoreApplication::processEvents();
        }
        const QPoint end = (QPointF(start) + travel).toPoint();
        QTest::mouseRelease(&s.window, Qt::LeftButton, Qt::NoModifier, end);
        QTest::qWait(300);

        const double contentYAfter =
            s.timeline->property("contentY").toDouble();
        QVERIFY2(std::abs(contentYAfter - contentYBefore) < 0.5,
                 qPrintable(QStringLiteral("the timeline under the picker "
                                           "scrolled from %1 to %2 while the "
                                           "resize grip was dragged")
                                .arg(contentYBefore).arg(contentYAfter)));
        QVERIFY(!s.timeline->property("moving").toBool());
        QVERIFY2(s.picker->property("opened").toBool(),
                 "the press meant for the resize grip dismissed the picker");

        const QRectF after = sceneRect(panel);
        const QPointF cornerAfter(left ? after.left() : after.right(),
                                  top ? after.top() : after.bottom());
        const QPointF expected = corner + travel;
        QVERIFY2(std::abs(cornerAfter.x() - expected.x()) <= 4
                     && std::abs(cornerAfter.y() - expected.y()) <= 4,
                 qPrintable(QStringLiteral("the grabbed corner went from "
                                           "(%1,%2) to (%3,%4), the pointer "
                                           "travelled to (%5,%6): %7 -> %8")
                                .arg(corner.x()).arg(corner.y())
                                .arg(cornerAfter.x()).arg(cornerAfter.y())
                                .arg(expected.x()).arg(expected.y())
                                .arg(rectText(before), rectText(after))));
        const QPointF far(left ? before.right() : before.left(),
                          top ? before.bottom() : before.top());
        const QPointF farAfter(left ? after.right() : after.left(),
                               top ? after.bottom() : after.top());
        QVERIFY2(std::abs(far.x() - farAfter.x()) <= 1
                     && std::abs(far.y() - farAfter.y()) <= 1,
                 qPrintable(QStringLiteral("the pinned corner moved: %1 -> %2")
                                .arg(rectText(before), rectText(after))));
    }

    void reactionAndComposerSizesArePersistedSeparately()
    {
        Scene s;
        QVERIFY(buildScene(s, QSize(1366, 768)));
        s.controller.settings()->setPickerShare(QStringLiteral("picker"), 600,
                                                700);
        QQuickItem *row = rowNear(s, 0.2, nullptr);
        QVERIFY(row != nullptr);
        QVERIFY(openFromRow(s, row));
        expand(s.picker);
        QQuickItem *panel = panelOf(s.picker);
        QVERIFY(panel != nullptr);
        auto *grip = s.picker->findChild<QQuickItem *>(
            QStringLiteral("popupResizeGrip"));
        QVERIFY(grip != nullptr);
        QTRY_VERIFY(grip->isVisible());

        const QRectF before = sceneRect(panel);
        const QPointF gripCentre = sceneRect(grip).center();
        const bool left = gripCentre.x() < before.center().x();
        const bool top = gripCentre.y() < before.center().y();
        const QPointF outward(left ? -1 : 1, top ? -1 : 1);
        const QPoint start = gripCentre.toPoint();
        QTest::mousePress(&s.window, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&s.window,
                             (QPointF(start) + outward * (6.0 * step)).toPoint());
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(&s.window, Qt::LeftButton, Qt::NoModifier,
                            (QPointF(start) + outward * 48.0).toPoint());
        QCoreApplication::processEvents();
        const QSizeF dragged = sceneRect(panel).size();
        QVERIFY2(dragged.width() > before.width() + 20,
                 qPrintable(QStringLiteral("the drag did not resize: %1 -> %2")
                                .arg(before.width()).arg(dragged.width())));

        QVERIFY(s.controller.settings()->pickerWidthShare(
                    QStringLiteral("reaction")) > 0);
        QCOMPARE(s.controller.settings()->pickerWidthShare(
                     QStringLiteral("picker")), 600);
        QCOMPARE(s.controller.settings()->pickerHeightShare(
                     QStringLiteral("picker")), 700);

        QVERIFY(QMetaObject::invokeMethod(s.picker, "close"));
        QTRY_VERIFY(!s.picker->property("visible").toBool());
        QVERIFY(openFromRow(s, row));
        expand(s.picker);
        panel = panelOf(s.picker);
        QTRY_VERIFY(std::abs(sceneRect(panel).width() - dragged.width()) <= 2);
        QVERIFY(std::abs(sceneRect(panel).height() - dragged.height()) <= 2);
    }

    void theQuickBarIsKeyboardOperable()
    {
        Scene s;
        QVERIFY(buildScene(s, QSize(1366, 768)));
        QQuickItem *row = rowNear(s, 0.3, nullptr);
        QVERIFY(row != nullptr);
        QVERIFY(openFromRow(s, row));
        QSignalSpy chosen(s.picker, SIGNAL(emojiChosen(QString)));
        auto *quickBar = s.picker->findChild<QQuickItem *>(
            QStringLiteral("reactionQuickBar"));
        QVERIFY(quickBar != nullptr);
        QTRY_COMPARE(s.window.activeFocusItem(), quickBar);
        const QStringList quick =
            quickBar->property("quickEmoji").toStringList();
        QVERIFY(quick.size() >= 6);

        QTest::keyClick(&s.window, Qt::Key_Right);
        QTest::keyClick(&s.window, Qt::Key_Right);
        QTest::keyClick(&s.window, Qt::Key_Left);
        QTest::keyClick(&s.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.at(0).at(0).toString(), quick.at(1));
        QTRY_VERIFY(!s.picker->property("visible").toBool());

        QVERIFY(openFromRow(s, row));
        QVERIFY(!s.picker->property("expanded").toBool());
        QTRY_COMPARE(s.window.activeFocusItem(), quickBar);
        for (const char c : { 's', 'm', 'i' })
            QTest::keyClick(&s.window, c);
        QTRY_VERIFY(s.picker->property("expanded").toBool());
        QTRY_COMPARE(s.controller.emojiCatalog()->searchText(),
                     QStringLiteral("smi"));
        QVERIFY(s.controller.emojiCatalog()->rowCount() > 0);
        QTest::keyClick(&s.window, Qt::Key_Down);
        QTest::keyClick(&s.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 2);
        QTRY_VERIFY(!s.picker->property("visible").toBool());

        QVERIFY(openFromRow(s, row));
        QTRY_COMPARE(s.window.activeFocusItem(), quickBar);
        QTest::keyClick(&s.window, Qt::Key_Escape);
        QTRY_VERIFY(!s.picker->property("visible").toBool());
        QCOMPARE(chosen.size(), 2);
    }
};

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    if (!configHome.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("reaction-picker-qml-test"));
    ReactionPickerQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "ReactionPickerQmlTest.moc"
