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
#include <QFont>
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
#include "matrix/MockMatrixClient.h"
#include "models/ActivityModel.h"
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

    void anEmptyArchivedListExplainsArchivingInsteadOfJoiningRooms()
    {
        Fixture f;
        QVERIFY(load(f, kPresenter, 320, 400));
        f.controller->roomList()->setFilterMode(5);
        auto *empty = byName(f.item.get(), QStringLiteral("roomListEmptyState"));
        QVERIFY(empty);
        QTRY_VERIFY_WITH_TIMEOUT(empty->isVisible(), kTimeoutMs);
        auto *hint = byName(f.item.get(), QStringLiteral("roomListEmptyHint"));
        QVERIFY(hint);
        const QString text = hint->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("Archive a chat")), qPrintable(text));
        f.controller->roomList()->setFilterMode(0);
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

    // Selection must not borrow the unread signal: a selected, fully-read
    // row stays Medium weight, and an unselected unread row stays Bold.
    void boldNameTracksUnreadNotSelection()
    {
        Fixture f;
        f.controller = std::make_unique<AppController>(AppController::MockBackend);
        auto *mock = f.controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setScreenshotDemoMode(true);
        QSignalSpy loginSpy(f.controller->auth(), &AuthManager::loginSucceeded);
        f.controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kTimeoutMs));
        QVERIFY(QTest::qWaitFor([&] { return f.controller->roomList()->rowCount() >= 3; },
                               kTimeoutMs));
        f.engine.rootContext()->setContextProperty(QStringLiteral("app"), f.controller.get());
        QQmlComponent component(&f.engine);
        component.setData(kPresenter, QUrl(QStringLiteral("qrc:/roomlistboldtest.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        f.item.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(f.item);
        f.window.resize(320, 400);
        f.item->setParentItem(f.window.contentItem());
        f.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&f.window));
        f.window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.window));

        // Weekend Plans has no unread of any kind; Design Lounge does
        // (seedScreenshotDemoData, both in the @alex:lightning.example account).
        const QString readRoomId = QStringLiteral("!weekend:lightning.example");
        const QString unreadRoomId = QStringLiteral("!design-lounge:lightning.example");
        f.controller->setCurrentRoomId(readRoomId);

        QQuickItem *readSelected = nullptr;
        QQuickItem *unreadRow = nullptr;
        QVERIFY(QTest::qWaitFor([&] {
            readSelected = nullptr;
            unreadRow = nullptr;
            for (auto *row : rowsOf(f.item.get())) {
                const QString id = row->property("roomId").toString();
                if (id == readRoomId)
                    readSelected = row;
                else if (id == unreadRoomId)
                    unreadRow = row;
            }
            return readSelected != nullptr && unreadRow != nullptr;
        }, kTimeoutMs));

        QVERIFY(readSelected->property("selected").toBool());
        QVERIFY(!readSelected->property("isUnread").toBool());
        auto *readName = byName(readSelected, QStringLiteral("roomNameLabel"));
        QVERIFY(readName);
        QCOMPARE(int(readName->property("font").value<QFont>().weight()),
                 int(QFont::Medium));

        QVERIFY(!unreadRow->property("selected").toBool());
        QVERIFY(unreadRow->property("isUnread").toBool());
        auto *unreadName = byName(unreadRow, QStringLiteral("roomNameLabel"));
        QVERIFY(unreadName);
        QCOMPARE(int(unreadName->property("font").value<QFont>().weight()),
                 int(QFont::Bold));
    }

    // A Mentions-only room must still bold for a plain message: the
    // notification gate that hides isUnread must not also hide hasActivity.
    // The mention badge has its own separate gate and is unaffected.
    void boldNameSurvivesAMentionsOnlyNotificationMode()
    {
        Fixture f;
        f.controller = std::make_unique<AppController>(AppController::MockBackend);
        auto *mock = f.controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setScreenshotDemoMode(true);
        QSignalSpy loginSpy(f.controller->auth(), &AuthManager::loginSucceeded);
        f.controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(kTimeoutMs));
        QVERIFY(QTest::qWaitFor([&] { return f.controller->roomList()->rowCount() >= 3; },
                               kTimeoutMs));

        const QString roomId = QStringLiteral("!design-lounge:lightning.example");
        f.controller->setRoomNotificationMode(roomId, 1); // Mentions only

        f.engine.rootContext()->setContextProperty(QStringLiteral("app"), f.controller.get());
        QQmlComponent component(&f.engine);
        component.setData(kPresenter, QUrl(QStringLiteral("qrc:/roomlistrawunreadtest.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        f.item.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(f.item);
        f.window.resize(320, 400);
        f.item->setParentItem(f.window.contentItem());
        f.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&f.window));
        f.window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&f.window));

        QQuickItem *row = nullptr;
        QVERIFY(QTest::qWaitFor([&] {
            row = nullptr;
            for (auto *r : rowsOf(f.item.get())) {
                if (r->property("roomId").toString() == roomId) {
                    row = r;
                    break;
                }
            }
            return row != nullptr;
        }, kTimeoutMs));

        // Confirms the mode really does gate the old signal, or this test
        // would pass without exercising the fix at all.
        QVERIFY2(!row->property("isUnread").toBool(),
                 "Mentions-only mode should have gated the activity-dot signal");
        QVERIFY(row->property("hasActivity").toBool());

        auto *name = byName(row, QStringLiteral("roomNameLabel"));
        QVERIFY(name);
        QCOMPARE(int(name->property("font").value<QFont>().weight()),
                 int(QFont::Bold));
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

        auto *archived = byName(f.item.get(), QStringLiteral("compactRoomListArchived"));
        QVERIFY(archived);
        QVERIFY(!archived->isVisible());
        RoomListModel *rooms = f.controller->roomList();
        int unreadRow = -1;
        for (int row = 0; row < rooms->rowCount() && unreadRow < 0; ++row) {
            if (rooms->data(rooms->index(row), RoomListModel::UnreadCountRole).toInt() > 0)
                unreadRow = row;
        }
        QVERIFY2(unreadRow >= 0, "fixture: the mock list has an unread room");
        const QString roomId = rooms->data(rooms->index(unreadRow),
                                           RoomListModel::RoomIdRole).toString();
        f.controller->roomList()->setRoomArchived(roomId, true);
        QTRY_VERIFY(archived->isVisible());
        QTRY_VERIFY(rooms->archivedUnreadCount() > 0);
        auto *dot = byName(f.item.get(), QStringLiteral("compactRoomListArchivedDot"));
        QVERIFY2(dot, "the compact archive button has no unread dot");
        QTRY_VERIFY(dot->isVisible());
        const QRectF button(archived->mapToItem(f.item.get(), QPointF()), archived->size());
        QVERIFY2(button.left() >= -0.1 && button.right() <= 64.1 && button.top() >= -0.1
                     && button.bottom() <= 700.1,
                 qPrintable(QStringLiteral("the archive button %1..%2 escapes the 64 px column")
                                .arg(button.left()).arg(button.right())));
        const QRectF dotIn(dot->mapToItem(archived, QPointF()), dot->size());
        QVERIFY2(QRectF(0, 0, archived->width(), archived->height()).contains(dotIn),
                 qPrintable(QStringLiteral("the unread dot %1,%2 %3x%4 leaves its %5x%6 button")
                                .arg(dotIn.x()).arg(dotIn.y()).arg(dotIn.width())
                                .arg(dotIn.height()).arg(archived->width())
                                .arg(archived->height())));
        const int filterBefore = f.controller->settings()->roomFilterMode();
        QVERIFY(QMetaObject::invokeMethod(archived, "clicked"));
        QCOMPARE(f.controller->settings()->roomFilterMode(), 5);
        QTRY_COMPARE(f.controller->roomList()->rowCount(), 1);
        f.controller->settings()->setRoomFilterMode(filterBefore);
        f.controller->roomList()->setRoomArchived(roomId, false);
    }

    void theCompactColumnKeepsTheActivityButtonAndItsUnseenState()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 64; height: 700 }
)QML", 64, 700));
        QVERIFY(f.item->property("compact").toBool());
        auto *activity = qobject_cast<ActivityModel *>(f.controller->activity());
        QVERIFY(activity);
        auto *button = byName(f.item.get(), QStringLiteral("compactActivityCenterButton"));
        QVERIFY2(button, "the compact column has no Activity button");
        QVERIFY(button->isVisible());
        const QRectF bounds(button->mapToItem(f.item.get(), QPointF()), button->size());
        QVERIFY2(bounds.left() >= -0.1 && bounds.top() >= -0.1 && bounds.right() <= 64.1
                     && bounds.bottom() <= 700.1,
                 qPrintable(QStringLiteral("the Activity button %1,%2 %3x%4 escapes the column")
                                .arg(bounds.x()).arg(bounds.y()).arg(bounds.width())
                                .arg(bounds.height())));
        auto *list = byName(f.item.get(), QStringLiteral("compactClassicList"));
        QVERIFY(list);
        QVERIFY2(bounds.bottom() <= list->mapToItem(f.item.get(), QPointF()).y() + 0.1,
                 "the Activity button overlaps the room avatars");

        auto *dot = byName(button, QStringLiteral("compactActivityCenterDot"));
        QVERIFY(dot);
        activity->markAllSeen();
        QTRY_COMPARE(activity->unseenCount(), 0);
        QTRY_VERIFY(!dot->isVisible());
        QVERIFY(!button->property("active").toBool());

        RoomListModel *rooms = f.controller->roomList();
        TimelineEvent mention;
        mention.eventId = QStringLiteral("$compact-activity-mention");
        mention.roomId = rooms->data(rooms->index(0), RoomListModel::RoomIdRole).toString();
        mention.sender = QStringLiteral("@bob:mock.local");
        mention.senderDisplayName = QStringLiteral("bob");
        mention.body = QStringLiteral("@alice look");
        mention.type = TimelineEvent::TextMessage;
        mention.timestamp = QDateTime::currentDateTime();
        mention.mentionsMe = true;
        QVERIFY(activity->ingest(mention, QStringLiteral("Room")));
        QTRY_VERIFY(activity->unseenCount() > 0);
        QTRY_VERIFY(dot->isVisible());
        QVERIFY(button->property("active").toBool());
        const QRectF dotIn(dot->mapToItem(button, QPointF()), dot->size());
        QVERIFY(QRectF(0, 0, button->width(), button->height()).contains(dotIn));

        auto *dialog = f.item->findChild<QObject *>(QStringLiteral("activityCenterDialog"));
        QVERIFY(dialog);
        QVERIFY(!dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY2(f.item->property("compact").toBool(), "opening Activity expanded the list");
        QMetaObject::invokeMethod(dialog, "close");
    }

    void aCompactRowsRightClickOffersTheRoomMenuAndArchives()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 64; height: 700 }
)QML", 400, 700));
        QVERIFY(f.item->property("compact").toBool());
        RoomListModel *rooms = f.controller->roomList();
        QQuickItem *row = nullptr;
        QString roomId;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QList<QQuickItem *> all;
            visualTree(f.item.get(), all);
            for (auto *item : all) {
                if (item->objectName() != QStringLiteral("compactRoomRow") || !item->isVisible())
                    continue;
                const QString id = item->property("rowId").toString();
                for (int r = 0; r < rooms->rowCount(); ++r) {
                    const QModelIndex idx = rooms->index(r);
                    if (rooms->data(idx, RoomListModel::RoomIdRole).toString() == id
                        && rooms->data(idx, RoomListModel::MembershipRole).toString()
                               == QStringLiteral("joined")) {
                        row = item;
                        roomId = id;
                        return true;
                    }
                }
            }
            return false;
        }()), kTimeoutMs);
        const QPointF centre = row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseClick(&f.window, Qt::RightButton, Qt::NoModifier, centre.toPoint());
        auto *menu = f.item->findChild<QObject *>(QStringLiteral("compactRoomContextMenu"));
        QVERIFY2(menu, "the compact list has no room menu");
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(), kTimeoutMs);
        QCOMPARE(menu->property("roomId").toString(), roomId);
        auto *select = menu->findChild<QQuickItem *>(QStringLiteral("roomSelectItem"));
        auto *archive = menu->findChild<QQuickItem *>(QStringLiteral("roomArchiveItem"));
        QVERIFY(select && archive);
        QVERIFY2(!select->isVisible(), "the compact menu offers a selection it cannot show");
        QVERIFY(archive->isVisible());
        QVERIFY(QMetaObject::invokeMethod(archive, "triggered"));
        QTRY_VERIFY(rooms->isRoomArchived(roomId));
        rooms->setRoomArchived(roomId, false);
    }

    void theFilterStripArrowsNeverCoverAChip()
    {
        Fixture f;
        QVERIFY(load(f, R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: 260; height: 700 }
)QML", 260, 700));
        RoomListModel *rooms = f.controller->roomList();
        const QString roomId = rooms->data(rooms->index(rooms->rowCount() - 1),
                                           RoomListModel::RoomIdRole).toString();
        rooms->setRoomArchived(roomId, true);
        auto *flick = byName(f.item.get(), QStringLiteral("roomFilterChipsFlick"));
        auto *right = byName(f.item.get(), QStringLiteral("roomFilterChipsMoreRight"));
        QVERIFY(flick && right);
        QTRY_VERIFY2(right->isVisible(), "fixture: the strip overflows at this width");
        const QRectF strip(flick->mapToItem(f.item.get(), QPointF()), flick->size());
        const QRectF arrow(right->mapToItem(f.item.get(), QPointF()), right->size());
        QVERIFY2(!strip.intersects(arrow),
                 qPrintable(QStringLiteral("arrow %1..%2 sits on the chips %3..%4")
                                .arg(arrow.left()).arg(arrow.right())
                                .arg(strip.left()).arg(strip.right())));
        rooms->setRoomArchived(roomId, false);
    }

    void theFilterStripKeepsEveryChipInsideItsGutters_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("240") << 240;
        QTest::newRow("300") << 300;
        QTest::newRow("420") << 420;
    }

    void theFilterStripKeepsEveryChipInsideItsGutters()
    {
        QFETCH(int, width);
        Fixture f;
        const QByteArray qml = QStringLiteral(R"QML(
import QtQuick
import MatrixClient
RoomsPanel { width: %1; height: 700 }
)QML").arg(width).toUtf8();
        QVERIFY(load(f, qml.constData(), width, 700));
        RoomListModel *rooms = f.controller->roomList();
        auto *settings = f.controller->settings();
        const int filterBefore = settings->roomFilterMode();
        const QString roomId = rooms->data(rooms->index(rooms->rowCount() - 1),
                                           RoomListModel::RoomIdRole).toString();
        const auto restore = qScopeGuard([&] {
            settings->setRoomFilterMode(filterBefore);
            rooms->setRoomArchived(roomId, false);
        });
        rooms->setRoomArchived(roomId, true);
        auto *flick = byName(f.item.get(), QStringLiteral("roomFilterChipsFlick"));
        auto *left = byName(f.item.get(), QStringLiteral("roomFilterChipsMoreLeft"));
        auto *right = byName(f.item.get(), QStringLiteral("roomFilterChipsMoreRight"));
        QVERIFY(flick && left && right);
        QTRY_VERIFY(byName(f.item.get(), QStringLiteral("roomFilterChips_5")));
        const auto inPanel = [&](QQuickItem *i) {
            return QRectF(i->mapToItem(f.item.get(), QPointF()), i->size());
        };
        const auto fmt = [](const QRectF &r) {
            return QStringLiteral("%1..%2 x %3..%4").arg(r.left()).arg(r.right())
                .arg(r.top()).arg(r.bottom());
        };
        const QRectF panel(0, 0, width, 700);
        const QRectF strip = inPanel(flick);
        QVERIFY2(panel.contains(strip), qPrintable(fmt(strip)));
        for (QQuickItem *arrow : { left, right }) {
            if (!arrow->isVisible())
                continue;
            const QRectF a = inPanel(arrow);
            QVERIFY2(panel.contains(a), qPrintable(arrow->objectName() + QLatin1Char(' ') + fmt(a)));
            QVERIFY2(!strip.intersects(a),
                     qPrintable(arrow->objectName() + QStringLiteral(" %1 on the strip %2")
                                                          .arg(fmt(a), fmt(strip))));
        }
        QList<QQuickItem *> chips;
        for (int value : { 0, 1, 2, 4, 3, 5 }) {
            auto *chip = byName(f.item.get(), QStringLiteral("roomFilterChips_%1").arg(value));
            QVERIFY2(chip && chip->isVisible(), qPrintable(QString::number(value)));
            chips << chip;
        }
        QString overlap;
        const auto chipsApart = [&] {
            overlap.clear();
            for (int i = 1; i < chips.size(); ++i) {
                const QRectF a = inPanel(chips.at(i - 1)), b = inPanel(chips.at(i));
                if (a.right() > b.left() + 0.5)
                    overlap = QStringLiteral("chips %1 and %2 overlap: %3 / %4")
                                  .arg(i - 1).arg(i).arg(fmt(a), fmt(b));
            }
            return overlap.isEmpty();
        };
        QTRY_VERIFY2(chipsApart(), qPrintable(overlap));
        const qreal fade = 16;
        for (int value : { 5, 0, 3 }) {
            settings->setRoomFilterMode(value);
            auto *chip = byName(f.item.get(), QStringLiteral("roomFilterChips_%1").arg(value));
            const auto visibleClear = [&] {
                const QRectF c = inPanel(chip);
                const qreal lo = strip.left() + (left->isVisible() && left->property("more").toBool() ? fade : 0);
                const qreal hi = strip.right() - (right->isVisible() && right->property("more").toBool() ? fade : 0);
                return c.left() >= lo - 0.5 && c.right() <= hi + 0.5;
            };
            QTRY_VERIFY2(visibleClear(),
                         qPrintable(QStringLiteral("selected chip %1 at %2 is clipped or "
                                                   "under a fade of the strip %3")
                                        .arg(value).arg(fmt(inPanel(chip)), fmt(strip))));
        }
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
