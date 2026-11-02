// The Classic room list on the real presenter, header and sort menu over the
// mock backend (swarm chunk C04, round 1):
//   * the list is ONE tab stop, the arrows / Home / End move between rows, a
//     focused row draws a ring and Enter opens it (C04-F03),
//   * the list keeps its scrollbar clear of the SplitView divider's grab mask,
//     which reaches 4px into the column (C04-F02),
//   * the "..." and "+" header buttons open on Enter, the Menu key and
//     Shift+F10, hide their tooltip while the menu is open, and take the focus
//     ring back when it closes (C04-F05, F06),
//   * the sort menu checks the mode the list is ACTUALLY ordered by while no
//     sort was ever chosen (the model's -1): Space order in a real Space,
//     Activity elsewhere, and an explicit choice wins.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>

#include <QScopeGuard>
#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"

namespace {

constexpr int kTimeoutMs = 4000;

void visualTree(QQuickItem *item, QList<QQuickItem *> &out)
{
    for (QQuickItem *child : item->childItems()) {
        out.append(child);
        visualTree(child, out);
    }
}

QQuickItem *byName(QQuickItem *root, const QString &name)
{
    QList<QQuickItem *> all;
    visualTree(root, all);
    for (QQuickItem *item : all) {
        if (item->objectName() == name)
            return item;
    }
    return nullptr;
}

QList<QQuickItem *> rowsOf(QQuickItem *root)
{
    QList<QQuickItem *> all;
    visualTree(root, all);
    QList<QQuickItem *> rows;
    for (QQuickItem *item : all) {
        if (item->property("showGroupDivider").isValid()
            && item->metaObject()->indexOfProperty("selected") >= 0
            && item->isVisible())
            rows.append(item);
    }
    std::sort(rows.begin(), rows.end(), [](QQuickItem *a, QQuickItem *b) {
        return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
    });
    return rows;
}

bool ringShown(QQuickItem *row)
{
    QQuickItem *ring = byName(row, QStringLiteral("roomRowFocusRing"));
    return ring && ring->isVisible();
}

}

static constexpr const char *kCompactRows = R"QML(
import QtQuick
import MatrixClient
ListView {
    objectName: "compactTestList"
    width: 64; height: 160
    focus: true
    currentIndex: 0
    keyNavigationEnabled: true
    model: ListModel {
        ListElement { title: "Alice"; room: "!alice:test"; unread: 3; mentions: 1 }
        ListElement { title: "General"; room: "!general:test"; unread: 0; mentions: 0 }
    }
    delegate: RoomListCompactRow {
        label: title
        rowId: room
        unreadCount: unread
        highlightCount: mentions
        hasUnread: unread > 0
    }
}
)QML";

class RoomListClassicQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

    struct Fixture {
        std::unique_ptr<AppController> controller;
        QQmlEngine engine;
        QQuickWindow window;
        std::unique_ptr<QQuickItem> item;
    };

    bool load(Fixture &f, const char *qml, int w, int h)
    {
        f.controller = std::make_unique<AppController>(AppController::MockBackend);
        QSignalSpy loginSpy(f.controller->auth(), &AuthManager::loginSucceeded);
        f.controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        if (!loginSpy.wait(kTimeoutMs))
            return false;
        if (!QTest::qWaitFor([&] { return f.controller->roomList()->rowCount() >= 3; },
                             kTimeoutMs))
            return false;
        f.engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                                   f.controller.get());
        QQmlComponent component(&f.engine);
        component.setData(qml, QUrl(QStringLiteral("qrc:/roomlistclassictest.qml")));
        if (!component.errors().isEmpty()) {
            qWarning() << component.errorString();
            return false;
        }
        f.item.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!f.item)
            return false;
        f.window.resize(w, h);
        f.item->setParentItem(f.window.contentItem());
        f.window.show();
        if (!QTest::qWaitForWindowExposed(&f.window))
            return false;
        f.window.requestActivate();
        return QTest::qWaitForWindowActive(&f.window);
    }

    static constexpr const char *kPresenter = R"QML(
import QtQuick
import MatrixClient
RoomListClassicPresenter { objectName: "presenter"; width: 320; height: 400 }
)QML";

private Q_SLOTS:
    void batchPromptUsesAppDialogAndSelectMenuHasMappedIcon()
    {
        QFile panel(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/RoomsPanel.qml"));
        QVERIFY(panel.open(QIODevice::ReadOnly));
        const QByteArray panelSource = panel.readAll();
        QVERIFY(panelSource.contains("AppDialog {\n            id: batchLeaveDialog"));
        QFile menu(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/RoomActionsMenu.qml"));
        QVERIFY(menu.open(QIODevice::ReadOnly));
        const QByteArray menuSource = menu.readAll();
        const auto selectAt = menuSource.indexOf("objectName: \"roomSelectItem\"");
        QVERIFY(selectAt >= 0);
        QVERIFY(menuSource.mid(selectAt, 160).contains("iconName: \"check_circle\""));
        QFile icons(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/IconGlyphs.js"));
        QVERIFY(icons.open(QIODevice::ReadOnly));
        QVERIFY(icons.readAll().contains("\"check_circle\": "));
    }

    void pointerSelectionKeepsRowsStillAndEscapeClosesIt()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 380; height: 700 }
)QML", 380, 700));
        const auto rows = rowsOf(f.item.get());
        QVERIFY(!rows.isEmpty());
        const qreal before = rows.first()->mapToScene(QPointF()).y();
        const QPoint point = rows.first()->mapToScene(
            QPointF(rows.first()->width() / 2, rows.first()->height() / 2)).toPoint();
        QTest::mouseClick(&f.window, Qt::LeftButton, Qt::ControlModifier, point);
        QTRY_VERIFY(f.controller->roomBatch()->selecting());
        QCOMPARE(rows.first()->mapToScene(QPointF()).y(), before);
        QTest::keyClick(&f.window, Qt::Key_Escape);
        QTRY_VERIFY(!f.controller->roomBatch()->selecting());
    }

    void selectionCanEnterByKeyboardAndExitWithEscape()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 380; height: 700 }
)QML", 380, 700));
        const auto rows = rowsOf(f.item.get());
        QVERIFY(!rows.isEmpty());
        rows.first()->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&f.window, Qt::Key_Space, Qt::ControlModifier);
        QTRY_VERIFY(f.controller->roomBatch()->selecting());
        QCOMPARE(f.controller->roomBatch()->selectionCount(), 1);
        auto *bar = byName(f.item.get(), QStringLiteral("roomBatchActionBar"));
        QVERIFY(bar && bar->isVisible());
        QTest::keyClick(&f.window, Qt::Key_Escape);
        QTRY_VERIFY(!f.controller->roomBatch()->selecting());
        QCOMPARE(f.controller->roomBatch()->selectionCount(), 0);
        QVERIFY(!bar->isVisible());
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
    }

    void theListIsOneTabStop()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        QTRY_VERIFY_WITH_TIMEOUT(rowsOf(f.item.get()).size() >= 3, kTimeoutMs);
        int stops = 0;
        for (QQuickItem *row : rowsOf(f.item.get())) {
            if (row->property("activeFocusOnTab").toBool())
                ++stops;
        }
        QCOMPARE(stops, 1);
    }

    void arrowsHomeEndMoveFocusRingAndEnterOpens()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        QTRY_VERIFY_WITH_TIMEOUT(rowsOf(f.item.get()).size() >= 3, kTimeoutMs);
        auto *list = byName(f.item.get(), QStringLiteral("roomListView"));
        QVERIFY2(list, "the list lost its objectName");
        QSignalSpy activated(f.item.get(), SIGNAL(roomActivated(QString)));
        QSignalSpy pointerOpen(f.item.get(), SIGNAL(roomOpenedByPointer(QString)));

        auto focused = [&]() -> QQuickItem * {
            for (QQuickItem *row : rowsOf(f.item.get())) {
                if (row->hasActiveFocus())
                    return row;
            }
            return nullptr;
        };

        QQuickItem *first = rowsOf(f.item.get()).first();
        first->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(first->hasActiveFocus());
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY2(ringShown(first), "the focused row draws no focus ring");

        QTest::keyClick(&f.window, Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(list->property("currentIndex").toInt(), 1, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(focused() != nullptr, kTimeoutMs);
        QQuickItem *second = focused();
        QVERIFY(second != first);
        QVERIFY(ringShown(second));
        QVERIFY2(!ringShown(first), "two rows draw a ring");

        QTest::keyClick(&f.window, Qt::Key_Up);
        QTRY_COMPARE_WITH_TIMEOUT(list->property("currentIndex").toInt(), 0, kTimeoutMs);

        const int last = f.controller->roomList()->rowCount() - 1;
        QTest::keyClick(&f.window, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(list->property("currentIndex").toInt(), last, kTimeoutMs);
        QTest::keyClick(&f.window, Qt::Key_Home);
        QTRY_COMPARE_WITH_TIMEOUT(list->property("currentIndex").toInt(), 0, kTimeoutMs);

        QTest::keyClick(&f.window, Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(list->property("currentIndex").toInt(), 1, kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(focused() != nullptr, kTimeoutMs);
        const QString id = focused()->property("roomId").toString();
        QVERIFY(!id.isEmpty());
        QTest::keyClick(&f.window, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, kTimeoutMs);
        QCOMPARE(activated.first().first().toString(), id);
        QCOMPARE(pointerOpen.count(), 0);
    }

    void theScrollBarIsClearOfTheDividerGrabMask()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        auto *list = byName(f.item.get(), QStringLiteral("roomListView"));
        QVERIFY(list);
        QVERIFY2(list->x() + list->width() <= f.item->width() - 4,
                 qPrintable(QStringLiteral("list right edge %1 of %2")
                                .arg(list->x() + list->width())
                                .arg(f.item->width())));
    }

    void anEmptyListDrawsNoSectionHeaderOverItsEmptyState()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        auto headers = [&] {
            QList<QQuickItem *> all, out;
            visualTree(f.item.get(), all);
            for (QQuickItem *it : all) {
                if (it->objectName() == QStringLiteral("roomListSectionHeader"))
                    out.append(it);
            }
            return out;
        };
        QTRY_VERIFY_WITH_TIMEOUT(!headers().isEmpty(), kTimeoutMs);
        bool anyShown = false;
        for (QQuickItem *h : headers())
            anyShown = anyShown || h->isVisible();
        QVERIFY2(anyShown, "fixture: a populated list shows its section headers");

        f.controller->roomList()->setSearchQuery(QStringLiteral("zz-no-room-matches-this"));
        QTRY_COMPARE_WITH_TIMEOUT(f.controller->roomList()->rowCount(), 0, kTimeoutMs);
        auto *empty = byName(f.item.get(), QStringLiteral("roomListEmptyState"));
        QVERIFY(empty);
        QTRY_VERIFY_WITH_TIMEOUT(empty->isVisible(), kTimeoutMs);
        QCoreApplication::processEvents();
        for (QQuickItem *h : headers()) {
            QVERIFY2(!h->isVisible() || h->opacity() == 0.0,
                     "a section header is still drawn over an empty list");
        }
        f.controller->roomList()->setSearchQuery(QString());
    }

    void aListThatEmptiesAndRefillsDrawsOnlyItsOwnSectionHeaders()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomListClassicPresenter { objectName: "presenter"; width: 320; height: 900 }
)QML", 320, 900));
        RoomListModel *model = f.controller->roomList();
        auto categories = [&] {
            QStringList out;
            for (int r = 0; r < model->rowCount(); ++r) {
                const QString c = model->data(model->index(r, 0),
                                              RoomListModel::CategoryRole).toString();
                if (!out.contains(c))
                    out.append(c);
            }
            return out;
        };
        auto visibleHeaders = [&] {
            QList<QQuickItem *> all, out;
            visualTree(f.item.get(), all);
            for (QQuickItem *it : all) {
                if (it->objectName() == QStringLiteral("roomListSectionHeader")
                    && it->isVisible() && it->opacity() > 0.0)
                    out.append(it);
            }
            return out;
        };
        // The mock fills the list asynchronously after load; its rooms are
        // all one section, so favourite the first to give a second one.
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 3, kTimeoutMs);
        model->setRoomFavourite(model->data(model->index(0, 0),
                                            RoomListModel::RoomIdRole).toString(),
                                true);
        QTRY_VERIFY2_WITH_TIMEOUT(categories().size() >= 2,
                 "fixture: the mock list needs two sections, so a later one "
                 "has a label below the first", kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(visibleHeaders().size() >= 2, kTimeoutMs);

        QString name;
        const QString lastCategory = categories().last();
        for (int r = 0; r < model->rowCount() && name.isEmpty(); ++r) {
            const QModelIndex i = model->index(r, 0);
            if (model->data(i, RoomListModel::CategoryRole).toString() == lastCategory)
                name = model->data(i, RoomListModel::NameRole).toString();
        }
        QVERIFY(!name.isEmpty());

        model->setSearchQuery(QStringLiteral("zz-no-room-matches-this"));
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 0, kTimeoutMs);
        QCoreApplication::processEvents();
        model->setSearchQuery(name);
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 1, kTimeoutMs);
        QTest::qWait(100);
        const QStringList now = categories();
        auto headerPlaces = [&] {
            QSet<int> ys;
            for (QQuickItem *h : visibleHeaders())
                ys.insert(qRound(h->mapToScene(QPointF(0, 0)).y()));
            return ys.size();
        };
        QTRY_COMPARE_WITH_TIMEOUT(headerPlaces(), int(now.size()), kTimeoutMs);
        auto *list = byName(f.item.get(), QStringLiteral("roomListView"));
        QVERIFY(list);
        QList<QQuickItem *> rows;
        {
            QList<QQuickItem *> all;
            visualTree(list, all);
            for (QQuickItem *it : all) {
                if (it->isVisible() && it->property("roomId").isValid()
                    && !it->property("roomId").toString().isEmpty())
                    rows.append(it);
            }
        }
        for (QQuickItem *h : visibleHeaders()) {
            const qreal bottom = h->mapToScene(QPointF(0, h->height())).y();
            bool heads = false;
            for (QQuickItem *row : rows) {
                if (qAbs(row->mapToScene(QPointF(0, 0)).y() - bottom) <= 1.0)
                    heads = true;
            }
            QVERIFY2(heads, qPrintable(QStringLiteral(
                "a section header at y=%1 heads no row").arg(bottom - h->height())));
        }
        model->setSearchQuery(QString());
    }

    void aParkedSectionHeaderStaysHiddenWhenTheCountChanges()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomListClassicPresenter { objectName: "presenter"; width: 320; height: 900 }
)QML", 320, 900));
        RoomListModel *model = f.controller->roomList();
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 3, kTimeoutMs);
        const QString favourite = model->data(model->index(0, 0),
                                              RoomListModel::NameRole).toString();
        model->setRoomFavourite(model->data(model->index(0, 0),
                                            RoomListModel::RoomIdRole).toString(),
                                true);
        auto visibleHeaders = [&] {
            QList<QQuickItem *> all, out;
            visualTree(f.item.get(), all);
            for (QQuickItem *it : all) {
                if (it->objectName() == QStringLiteral("roomListSectionHeader")
                    && it->isVisible() && it->opacity() > 0.0)
                    out.append(it);
            }
            return out;
        };
        auto headerPlaces = [&] {
            QSet<int> ys;
            for (QQuickItem *h : visibleHeaders())
                ys.insert(qRound(h->mapToScene(QPointF(0, 0)).y()));
            return ys.size();
        };
        QTRY_VERIFY_WITH_TIMEOUT(headerPlaces() >= 2, kTimeoutMs);

        model->setSearchQuery(favourite);
        QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(headerPlaces(), 1, kTimeoutMs);

        model->setRoomFavourite(model->data(model->index(0, 0),
                                            RoomListModel::RoomIdRole).toString(),
                                false);
        model->setSearchQuery(QString());
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 3, kTimeoutMs);
        model->setSearchQuery(model->data(model->index(1, 0),
                                          RoomListModel::NameRole).toString());
        QTRY_VERIFY_WITH_TIMEOUT(model->rowCount() >= 1, kTimeoutMs);
        QTest::qWait(100);
        QCOMPARE(headerPlaces(), 1);
        model->setSearchQuery(QString());
    }

    void favouritesShortcutFitsAndTogglesTheFilter()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 320; height: 700 }
)QML", 320, 700));
        auto *button = byName(f.item.get(), QStringLiteral("favouritesShortcut"));
        QVERIFY(button && button->isVisible());
        QVERIFY(f.controller->roomList()->roomFavouritesSupported());
        QFile panel(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/RoomsPanel.qml"));
        QVERIFY(panel.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(panel.readAll());
        const int at = source.indexOf(QStringLiteral("objectName: \"favouritesShortcut\""));
        QVERIFY(at >= 0);
        QVERIFY(source.mid(at, 300).contains(
            QStringLiteral("app.roomList.roomFavouritesSupported")));
        const auto bounds = button->mapRectToItem(f.item.get(), button->boundingRect());
        QVERIFY(bounds.left() >= 0);
        QVERIFY(bounds.right() <= f.item->width());
        f.controller->settings()->setRoomFilterMode(0);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(f.controller->settings()->roomFilterMode(), 4);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(f.controller->settings()->roomFilterMode(), 0);
    }

    void aRecycledRowUnderAnOpenRoomMenuKeepsItsTargetRoom()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomListClassicPresenter { objectName: "presenter"; width: 320; height: 60 }
)QML", 320, 60));
        auto *host = f.item->findChild<QObject *>(QStringLiteral("sharedRoomMenuHost"));
        QVERIFY(host);
        QTRY_VERIFY(host->property("item").value<QObject *>());
        auto *menu = host->property("item").value<QObject *>();
        QTRY_VERIFY(!rowsOf(f.item.get()).isEmpty());
        QQuickItem *row = rowsOf(f.item.get()).first();
        QVERIFY(QMetaObject::invokeMethod(row, "openRowMenu"));
        QTRY_VERIFY(menu->property("opened").toBool());
        const QString target = menu->property("roomId").toString();
        const QString targetName = menu->property("roomName").toString();
        QVERIFY(!target.isEmpty());

        auto *list = f.item->findChild<QObject *>(QStringLiteral("roomListView"));
        QVERIFY(list);
        list->setProperty("cacheBuffer", 0);
        for (int step = 0; step < 6
             && row->property("rowRoomId").toString() == target; ++step) {
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
            QTest::qWait(50);
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
            QTest::qWait(50);
        }
        QVERIFY2(row->property("rowRoomId").toString() != target,
                 "the fixture never recycled the row");
        QVERIFY(menu->property("opened").toBool());
        QCOMPARE(menu->property("roomId").toString(), target);

        QSignalSpy leave(f.item.get(), SIGNAL(leaveRoomRequested(QString, QString)));
        QVERIFY(leave.isValid());
        QVERIFY(QMetaObject::invokeMethod(menu, "leaveRoomRequested"));
        QCOMPARE(leave.size(), 1);
        QCOMPARE(leave.at(0).at(0).toString(), target);
        QCOMPARE(leave.at(0).at(1).toString(), targetName);
    }

    void aRoomLeavingTheListClosesItsOpenMenu()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        auto *host = f.item->findChild<QObject *>(QStringLiteral("sharedRoomMenuHost"));
        QVERIFY(host);
        QTRY_VERIFY(host->property("item").value<QObject *>());
        auto *menu = host->property("item").value<QObject *>();
        QTRY_VERIFY(rowsOf(f.item.get()).size() >= 2);
        QVERIFY(QMetaObject::invokeMethod(rowsOf(f.item.get()).at(0), "openRowMenu"));
        QTRY_VERIFY(menu->property("opened").toBool());
        f.controller->roomList()->setSearchQuery(QStringLiteral("no room is called this"));
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    void aMenuThatFailedToBuildStillOpens()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        auto *host = f.item->findChild<QObject *>(QStringLiteral("sharedRoomMenuHost"));
        QVERIFY(host);
        QTRY_VERIFY(host->property("item").value<QObject *>());
        QTRY_VERIFY(rowsOf(f.item.get()).size() >= 1);
        QVERIFY(host->setProperty("active", false));
        QTRY_VERIFY(!host->property("item").value<QObject *>());
        QVERIFY(QMetaObject::invokeMethod(rowsOf(f.item.get()).at(0), "openRowMenu"));
        auto *menu = host->property("item").value<QObject *>();
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
    }

    void theSortMenuChecksTheEffectiveDefault()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
Item {
    width: 400; height: 300
    property alias menu: m
    RoomListSortMenu { id: m }
}
)QML", 400, 300));
        auto *menu = f.item->property("menu").value<QObject *>();
        QVERIFY(menu);
        auto *explanation = menu->findChild<QObject *>(QStringLiteral("roomSortExplanation"));
        QVERIFY(explanation);
        auto checked = [&](const char *name) {
            auto *entry = menu->findChild<QObject *>(QString::fromLatin1(name));
            return entry && entry->property("radioSelected").toBool();
        };
        f.controller->roomList()->setSortMode(-1);

        f.controller->spaces()->setActiveSpaceId(SpaceManager::allRoomsId());
        QVERIFY2(checked("roomSortActivityItem"), "Home: Activity must be checked");
        QVERIFY(!checked("roomSortSpaceItem"));

        f.controller->spaces()->setActiveSpaceId(QStringLiteral("!space:mock.local"));
        QVERIFY2(checked("roomSortSpaceItem"),
                 qPrintable(QStringLiteral("a real Space with no choice made: Space order must be checked "
                                           "(currentSort=%1 sortMode=%2 space=%3)")
                                .arg(menu->property("currentSort").toInt())
                                .arg(f.controller->roomList()->sortMode())
                                .arg(f.controller->spaces()->activeSpaceId())));
        QVERIFY(!checked("roomSortActivityItem"));

        f.controller->roomList()->setSortMode(0);
        QVERIFY(checked("roomSortActivityItem"));
        QVERIFY(!checked("roomSortSpaceItem"));

        f.controller->roomList()->setSortMode(1);
        QVERIFY(checked("roomSortNameItem"));
        QVERIFY(explanation->property("text").toString().contains(QStringLiteral("alphabetical")));
        f.controller->roomList()->setSortMode(3);
        QVERIFY(explanation->property("text").toString().contains(QStringLiteral("unread rooms first")));
    }

    void theHeaderButtonsOpenOnEnterAndTheMenuKeyAndGiveTheRingBack()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { objectName: "panel"; width: 380; height: 700 }
)QML", 380, 700));
        auto *button = byName(f.item.get(), QStringLiteral("roomListOptionsButton"));
        QVERIFY2(button && button->isVisible(), "no sort button");
        auto *menu = byName(f.item.get(), QStringLiteral("roomListSortMenu"));
        QObject *popup = menu ? menu : f.item->findChild<QObject *>(
                                           QStringLiteral("roomListSortMenu"));
        QVERIFY2(popup, "no sort menu");

        for (const int key : { int(Qt::Key_Return), int(Qt::Key_Enter),
                               int(Qt::Key_Menu) }) {
            button->forceActiveFocus(Qt::TabFocusReason);
            QVERIFY(button->hasActiveFocus());
            QTest::keyClick(&f.window, Qt::Key(key));
            QTRY_VERIFY2_WITH_TIMEOUT(popup->property("opened").toBool(),
                                      qPrintable(QStringLiteral("key %1 opened nothing").arg(key)),
                                      kTimeoutMs);
            QTest::keyClick(&f.window, Qt::Key_Escape);
            QTRY_VERIFY_WITH_TIMEOUT(!popup->property("opened").toBool(), kTimeoutMs);
            QTRY_VERIFY2_WITH_TIMEOUT(button->hasActiveFocus()
                                          && button->property("visualFocus").toBool(),
                                      qPrintable(QStringLiteral("no focus ring on the button after Escape (key %1 focus=%2 visualFocus=%3)")
                                                     .arg(key).arg(button->hasActiveFocus())
                                                     .arg(button->property("visualFocus").toBool())),
                                      kTimeoutMs);
        }
        button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&f.window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("opened").toBool(), kTimeoutMs);
        QTest::keyClick(&f.window, Qt::Key_Escape);
    }

    void theOptionsTooltipHidesWhileItsMenuIsOpen()
    {
        QFile file(QStringLiteral(QML_DIR "/RoomsPanel.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString src = QString::fromUtf8(file.readAll());
        QVERIFY(src.contains(
            QStringLiteral("ToolTip.visible: hovered && !roomListSortMenu.opened")));
    }

    void compactRowsKeepAvatarBadgeAndTooltipInsideColumn()
    {
        Fixture f;
        QVERIFY(load(f, kCompactRows, 64, 160));
        auto compactRows = [&]() {
            QList<QQuickItem *> all;
            visualTree(f.item.get(), all);
            QList<QQuickItem *> rows;
            for (auto *item : all)
                if (item->objectName() == QStringLiteral("compactRoomRow"))
                    rows.append(item);
            return rows;
        };
        QTRY_COMPARE_WITH_TIMEOUT(compactRows().size(), 2, kTimeoutMs);
        const auto rows = compactRows();
        for (auto *row : rows) {
            QCOMPARE(row->width(), 64.0);
            QCOMPARE(row->property("tooltipText").toString().isEmpty(), false);
            for (const QString &name : { QStringLiteral("compactRoomAvatar"),
                                         QStringLiteral("compactRoomBadge") }) {
                auto *child = byName(row, name);
                QVERIFY(child);
                if (!child->isVisible()) continue;
                const QRectF bounds(child->mapToItem(row, QPointF()), child->size());
                QVERIFY2(bounds.left() >= -0.1 && bounds.top() >= -0.1
                         && bounds.right() <= row->width() + 0.1
                         && bounds.bottom() <= row->height() + 0.1,
                         qPrintable(name + QStringLiteral(" escapes compact row")));
            }
        }
        auto *badge = byName(rows.first(), QStringLiteral("compactRoomBadge"));
        QVERIFY(badge->isVisible());
        QCOMPARE(badge->property("mention").toBool(), true);
        QCOMPARE(rows.first()->property("tooltipText").toString(), QStringLiteral("Alice"));
    }

    void theRowHighlightHoldsAcrossTheWholeRow()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        const auto rows = rowsOf(f.item.get());
        QVERIFY(rows.size() >= 2);
        QQuickItem *row = rows.first();
        auto *chip = byName(row, QStringLiteral("roomRowChip"));
        auto *name = byName(row, QStringLiteral("roomNameLabel"));
        QVERIFY(chip && name);
        const QColor rest = chip->property("color").value<QColor>();
        QTest::mouseMove(&f.window, rows.at(1)->mapToScene(
            QPointF(row->width() / 2, row->height() / 2)).toPoint());
        QTRY_VERIFY(chip->property("color").value<QColor>() == rest);

        QQuickItem *avatar = nullptr;
        QList<QQuickItem *> all;
        visualTree(row, all);
        for (auto *it : all)
            if (it->property("mxc").isValid() && it->property("size").isValid())
                { avatar = it; break; }
        QVERIFY(avatar);

        struct Spot { const char *what; QPointF at; };
        const QList<Spot> spots = {
            { "avatar", row->mapFromItem(avatar, QPointF(avatar->width() / 2,
                                                         avatar->height() / 2)) },
            { "title", row->mapFromItem(name, QPointF(4, name->height() / 2)) },
            { "blank", QPointF(row->width() - 12, row->height() / 2) },
        };
        for (const auto &s : spots) {
            QTest::mouseMove(&f.window, rows.at(1)->mapToScene(QPointF(4, 4)).toPoint());
            QTest::mouseMove(&f.window, row->mapToScene(s.at).toPoint());
            QTRY_VERIFY2(chip->property("color").value<QColor>() != rest,
                         qPrintable(QStringLiteral("no highlight over the %1")
                                    .arg(QLatin1String(s.what))));
        }
    }

    void compactRowsNavigateWithKeyboard()
    {
        Fixture f;
        QVERIFY(load(f, kCompactRows, 64, 160));
        f.item->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&f.window, Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(f.item->property("currentIndex").toInt(), 1, kTimeoutMs);
        QTest::keyClick(&f.window, Qt::Key_Up);
        QTRY_COMPARE_WITH_TIMEOUT(f.item->property("currentIndex").toInt(), 0, kTimeoutMs);
    }

    void compactPresenterSwitchesBetweenClassicAndChannelsModels()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomListCompactPresenter { width: 64; height: 320 }
)QML", 64, 320));
        auto *classic = byName(f.item.get(), QStringLiteral("compactClassicList"));
        auto *channels = byName(f.item.get(), QStringLiteral("compactChannelsList"));
        QVERIFY(classic);
        QVERIFY(channels);
        QVERIFY(classic->isVisible());
        QVERIFY(!channels->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(classic->property("count").toInt() >= 3, kTimeoutMs);
        auto *settings = f.controller->settings();
        const int savedLayout = settings->roomNavigationLayout();
        const auto restoreLayout = qScopeGuard(
            [settings, savedLayout] { settings->setRoomNavigationLayout(savedLayout); });
        settings->setRoomNavigationLayout(1);
        QTRY_VERIFY_WITH_TIMEOUT(channels->isVisible(), kTimeoutMs);
        QVERIFY(!classic->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(channels->property("count").toInt() > 0, kTimeoutMs);
        channels->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&f.window, Qt::Key_Down);
        QTRY_VERIFY_WITH_TIMEOUT(channels->property("currentIndex").toInt() >= 0,
                                 kTimeoutMs);
    }

    void compactPanelKeepsItsControlsInsideTheSavedWidth()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 64; height: 700 }
)QML", 64, 700));
        QVERIFY(f.item->property("compact").toBool());
        QCOMPARE(f.item->width(), 64.0);
        for (const QString &name : { QStringLiteral("compactRoomListHeader"),
                                     QStringLiteral("compactRoomListSearch"),
                                     QStringLiteral("compactRoomListFilter"),
                                     QStringLiteral("compactClassicList") }) {
            auto *child = byName(f.item.get(), name);
            QVERIFY2(child, qPrintable(name + QStringLiteral(" missing")));
            QVERIFY(child->isVisible());
            const QRectF bounds(child->mapToItem(f.item.get(), QPointF()), child->size());
            QVERIFY2(bounds.left() >= -0.1 && bounds.top() >= -0.1
                     && bounds.right() <= 64.1 && bounds.bottom() <= 700.1,
                     qPrintable(name + QStringLiteral(" escapes panel")));
        }
        auto *fullSearch = byName(f.item.get(), QStringLiteral("roomSearchCard"));
        QVERIFY(fullSearch);
        QVERIFY(!fullSearch->isVisible());
    }

    void theRoomListDividerClicksDoubleClicksTabsAndKeys_data()
    {
        QTest::addColumn<int>("navigationLayout");
        QTest::newRow("Classic") << 0;
        QTest::newRow("Channels") << 1;
    }

    void theRoomListDividerClicksDoubleClicksTabsAndKeys()
    {
        QFETCH(int, navigationLayout);
        auto controller = std::make_unique<AppController>(AppController::MockBackend);
        QSignalSpy loginSpy(controller->auth(), &AuthManager::loginSucceeded);
        controller->auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"), QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kTimeoutMs));
        SettingsManager *settings = controller->settings();
        const int layoutBefore = settings->roomNavigationLayout();
        settings->setRoomNavigationLayout(navigationLayout);
        const int widthBefore = settings->roomListWidth();
        const int expandedBefore = settings->roomListExpandedWidth();
        const auto restore = qScopeGuard([&] {
            settings->setRoomListExpandedWidth(expandedBefore);
            settings->setRoomListWidth(widthBefore);
            settings->setRoomNavigationLayout(layoutBefore);
        });
        settings->setRoomListExpandedWidth(320);
        settings->setRoomListWidth(320);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), controller.get());
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 1100; height: 720; visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("qrc:/roomlistdivider.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        QQuickItem *rooms = nullptr;
        QQuickItem *divider = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (rooms = byName(window->contentItem(), QStringLiteral("roomsPanel")))
                && (divider = byName(window->contentItem(),
                                     QStringLiteral("roomListDivider"))),
            kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(), 320.0, kTimeoutMs);
        const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
        const auto at = [&] {
            return divider->mapToScene(QPointF(divider->width() / 2,
                                               divider->height() / 2)).toPoint();
        };
        const auto click = [&] {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, at());
        };

        click();
        QTest::qWait(qMin(60, interval / 4));
        click();
        QTRY_VERIFY_WITH_TIMEOUT(rooms->property("compact").toBool(), kTimeoutMs);
        QCOMPARE(settings->roomListWidth(), settings->roomListMinWidth());
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(), 64.0, kTimeoutMs);

        QTest::qWait(interval + 100);
        click();
        QTRY_VERIFY_WITH_TIMEOUT(!rooms->property("compact").toBool(), kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(settings->roomListWidth(), 320, kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(), 320.0, kTimeoutMs);

        QTest::qWait(interval + 100);
        click();
        QTest::qWait(interval + 300);
        QVERIFY(!rooms->property("compact").toBool());
        QCOMPARE(settings->roomListWidth(), 320);

        const QPoint from = at();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int dx = 8; dx <= 48; dx += 8)
            QTest::mouseMove(window, from + QPoint(dx, 0));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, from + QPoint(48, 0));
        QTRY_VERIFY_WITH_TIMEOUT(settings->roomListWidth() >= 350, kTimeoutMs);
        QVERIFY(!rooms->property("compact").toBool());

        const QPoint narrowFrom = at();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, narrowFrom);
        for (int dx = -40; dx >= -280; dx -= 40)
            QTest::mouseMove(window, narrowFrom + QPoint(dx, 0));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                            narrowFrom + QPoint(-280, 0));
        QTRY_COMPARE_WITH_TIMEOUT(settings->roomListWidth(), 64, kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(), 64.0, kTimeoutMs);
        click();
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(),
                                  double(settings->roomListExpandedWidth()), kTimeoutMs);

        rooms->forceActiveFocus();
        bool reached = false;
        for (int i = 0; i < 200 && !reached; ++i) {
            QTest::keyClick(window, Qt::Key_Tab);
            reached = divider->hasActiveFocus();
        }
        QVERIFY2(reached, "Tab never reached the room-list divider");
        QQuickItem *ring = byName(divider, QStringLiteral("roomListDividerFocusRing"));
        QVERIFY2(ring && ring->isVisible(), "the focused divider draws no focus ring");

        QTest::keyClick(window, Qt::Key_Home);
        QTRY_VERIFY_WITH_TIMEOUT(rooms->property("compact").toBool(), kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(), 64.0, kTimeoutMs);
        QTest::keyClick(window, Qt::Key_End);
        QTRY_VERIFY_WITH_TIMEOUT(!rooms->property("compact").toBool(), kTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(rooms->width(),
                                  double(settings->roomListExpandedWidth()), kTimeoutMs);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(rooms->property("compact").toBool(), kTimeoutMs);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!rooms->property("compact").toBool(), kTimeoutMs);
    }
};

QTEST_MAIN(RoomListClassicQmlTest)
#include "RoomListClassicQmlTest.moc"
