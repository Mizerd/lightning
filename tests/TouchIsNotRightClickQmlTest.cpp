// A finger is not a right button, in the real shell on the mock backend:
//   * a touch tap on a room-list row opens the room and nothing else (Classic
//     and Channels). A TapHandler limited to Qt.RightButton does not check
//     buttons for touch at all, so before this every tap also popped the room
//     menu over the room it had just opened;
//   * a touch long press opens the room menu and does not open the room;
//   * a mouse right click still opens the menu;
//   * every TapHandler in qml/ that leaves the left button out (right, and
//     Back/Forward or any other mouse-only button) keeps touch out by
//     declaring its devices, so a new one cannot bring the bug back.

#include <QtTest/QtTest>

#include <QDirIterator>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 720
    visible: true
    color: AppTheme.background

    MainScreen {
        objectName: "mainScreen"
        anchors.fill: parent
    }
}
)QML";

const QString kRoom = QStringLiteral("!dm-bob:mock.local"); // a DM: shown at Home in both layouts
const QString kOther = QStringLiteral("!devs:mock.local");

} // namespace

class TouchIsNotRightClickQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QPointingDevice *m_touch = nullptr;

    // The visible room-list row for `roomId`: a ChannelDelegate (has
    // channelName) or a RoomDelegate (has showGroupDivider).
    QQuickItem *row(const QString &roomId) const
    {
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            const bool isRow = it->metaObject()->indexOfProperty("channelName") >= 0
                || it->metaObject()->indexOfProperty("showGroupDivider") >= 0;
            if (isRow && it->isVisible() && it->width() > 0
                && it->property("roomId").toString() == roomId)
                return it;
            stack.append(it->childItems());
        }
        return nullptr;
    }

    QPoint centreOf(const QString &roomId) const
    {
        QQuickItem *target = row(roomId);
        if (!target)
            return {};
        return target->mapToScene(QPointF(target->width() / 2, target->height() / 2))
            .toPoint();
    }

    // Classic declares one menu per row, Channels loads one on first use;
    // either way the popup is a QObject child of something in its row.
    // QObject::findChildren from the window cannot reach view delegates, so
    // walk the item tree and look at each item's own children.
    QList<QObject *> roomMenus() const
    {
        QList<QObject *> menus;
        QList<QQuickItem *> stack{ m_window->contentItem() };
        while (!stack.isEmpty()) {
            QQuickItem *it = stack.takeLast();
            const auto children = it->children();
            for (QObject *o : children) {
                const QString name = o->objectName();
                if (name == QStringLiteral("roomContextMenu")
                    || name == QStringLiteral("channelContextMenu"))
                    menus << o;
                // The Channels menu sits inside its Loader.
                const auto inner = o->findChildren<QObject *>(
                    QStringLiteral("channelContextMenu"));
                menus << inner;
            }
            stack.append(it->childItems());
        }
        // A menu is reachable both as a child and through its parent's walk.
        const QSet<QObject *> unique(menus.cbegin(), menus.cend());
        return QList<QObject *>(unique.cbegin(), unique.cend());
    }

    int openRoomMenus() const
    {
        int open = 0;
        const auto menus = roomMenus();
        for (QObject *o : menus) {
            if (o->property("opened").toBool())
                ++open;
        }
        return open;
    }

    void closeRoomMenus()
    {
        const auto menus = roomMenus();
        for (QObject *o : menus) {
            if (o->property("opened").toBool())
                QMetaObject::invokeMethod(o, "close");
        }
        QTest::qWait(250); // past the menu's exit transition
    }

    // A sweep that finds no menus would pass every "no menu opened" check.
    void verifyMenusAreVisible() const
    {
        QVERIFY2(!roomMenus().isEmpty(), "the harness found no room menus at all");
    }

    // Another room open, every menu closed: the start of every case.
    void reset(int layout)
    {
        m_controller->settings()->setRoomNavigationLayout(layout);
        m_controller->openSpaceHome(QString());
        m_controller->openRoom(kOther);
        QTest::qWait(150);
        closeRoomMenus();
        QCOMPARE(m_controller->currentRoomId(), kOther);
        QCOMPARE(openRoomMenus(), 0);
    }

    static void addLayouts()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("channels") << 1;
        QTest::newRow("classic") << 0;
    }

    // Every TapHandler whose acceptedButtons leaves the left button out must
    // declare acceptedDevices without TouchScreen. TapHandler checks buttons
    // for mouse events only, so a finger fires every such handler: a
    // right-button one pops a menu, and a Back/Forward-only one (mouse room
    // history, D2) would navigate on every tap. Direct properties only:
    // nested blocks are blanked before the check, so a child's
    // acceptedDevices cannot satisfy its parent.
    struct TapHandlerSweep
    {
        int rightOnly = 0; // non-left handlers naming the right button
        int otherOnly = 0; // non-left handlers without it: Back, Forward, ...
        QList<int> offendingLines; // 1-based
    };
    static TapHandlerSweep sweepTapHandlers(const QString &src)
    {
        static const QRegularExpression open(QStringLiteral("\\bTapHandler\\s*\\{"));
        static const QRegularExpression nested(QStringLiteral("\\{[^{}]*\\}"));
        static const QRegularExpression buttons(
            QStringLiteral("acceptedButtons\\s*:\\s*([^\\n]+)"));
        static const QRegularExpression devices(
            QStringLiteral("acceptedDevices\\s*:\\s*([^\\n]+)"));
        // Every button but the left one (AllButtons includes it).
        static const QRegularExpression nonLeft(QStringLiteral(
            "\\b(RightButton|MiddleButton|MidButton|BackButton|ForwardButton"
            "|XButton[12]|ExtraButton\\d+|TaskButton)\\b"));
        TapHandlerSweep out;
        auto m = open.globalMatch(src);
        while (m.hasNext()) {
            const auto hit = m.next();
            qsizetype i = hit.capturedEnd();
            int depth = 1;
            while (depth > 0 && i < src.size()) {
                if (src.at(i) == u'{')
                    ++depth;
                else if (src.at(i) == u'}')
                    --depth;
                ++i;
            }
            QString flat = src.mid(hit.capturedEnd(), i - 1 - hit.capturedEnd());
            while (flat.contains(nested))
                flat.replace(nested, QString());
            const auto b = buttons.match(flat);
            if (!b.hasMatch())
                continue;
            const QString named = b.captured(1);
            if (named.contains(QStringLiteral("LeftButton"))
                || named.contains(QStringLiteral("AllButtons"))
                || !named.contains(nonLeft))
                continue;
            if (named.contains(QStringLiteral("RightButton")))
                ++out.rightOnly;
            else
                ++out.otherOnly;
            const auto d = devices.match(flat);
            if (!d.hasMatch() || d.captured(1).contains(QStringLiteral("TouchScreen")))
                out.offendingLines << int(src.left(hit.capturedStart()).count(u'\n')) + 1;
        }
        return out;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("touch-is-not-right-click-qml-test"));

        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(3000));

        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("touchisnotrightclick.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QTest::qWait(80);

        m_touch = QTest::createTouchDevice();
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void aTouchTapOpensTheRoomAndNoMenu_data() { addLayouts(); }
    void aTouchTapOpensTheRoomAndNoMenu()
    {
        QFETCH(int, layout);
        reset(layout);
        const QPoint at = centreOf(kRoom);
        QVERIFY2(!at.isNull(), "no visible row for the room");

        QTest::touchEvent(m_window, m_touch).press(0, at);
        QTest::qWait(30);
        QTest::touchEvent(m_window, m_touch).release(0, at);
        QTest::qWait(200);

        QCOMPARE(m_controller->currentRoomId(), kRoom);
        // Classic builds its menus with the rows, so an empty find means the
        // walk is broken. Channels builds one only when it opens.
        if (layout == 0)
            verifyMenusAreVisible();
        QVERIFY2(openRoomMenus() == 0,
                 "a touch tap on a room row also opened the room menu");
    }

    void aTouchLongPressOpensTheMenuNotTheRoom_data() { addLayouts(); }
    void aTouchLongPressOpensTheMenuNotTheRoom()
    {
        QFETCH(int, layout);
        reset(layout);
        const QPoint at = centreOf(kRoom);
        QVERIFY2(!at.isNull(), "no visible row for the room");

        QTest::touchEvent(m_window, m_touch).press(0, at);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300);
        QTest::touchEvent(m_window, m_touch).release(0, at);
        QTest::qWait(200);

        QCOMPARE(openRoomMenus(), 1);
        QVERIFY2(m_controller->currentRoomId() == kOther,
                 "a long press that opened the menu also opened the room");
        closeRoomMenus();
    }

    void aMouseRightClickStillOpensTheMenu_data() { addLayouts(); }
    void aMouseRightClickStillOpensTheMenu()
    {
        QFETCH(int, layout);
        reset(layout);
        const QPoint at = centreOf(kRoom);
        QVERIFY2(!at.isNull(), "no visible row for the room");

        QTest::mouseClick(m_window, Qt::RightButton, {}, at);
        QTest::qWait(200);

        QCOMPARE(openRoomMenus(), 1);
        QCOMPARE(m_controller->currentRoomId(), kOther);
        closeRoomMenus();
    }

    void everyNonLeftButtonTapHandlerKeepsTouchOut()
    {
        const QString qmlDir = QStringLiteral(QML_DIR);
        QDirIterator it(qmlDir, { QStringLiteral("*.qml") }, QDir::Files,
                        QDirIterator::Subdirectories);
        int rightOnly = 0;
        int otherOnly = 0;
        QStringList offenders;
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const TapHandlerSweep sweep = sweepTapHandlers(QString::fromUtf8(f.readAll()));
            rightOnly += sweep.rightOnly;
            otherOnly += sweep.otherOnly;
            for (int line : sweep.offendingLines) {
                offenders << QStringLiteral("%1:%2")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(line);
            }
        }
        // Twelve right-button handlers when this was written; a sweep that
        // finds none is broken. Back/Forward ones may legitimately be none
        // (mouse history is a C++ event filter); the sweep is proven able to
        // see them by theSweepSeesEveryNonLeftButton below.
        qInfo() << "non-left TapHandlers: right" << rightOnly << "other" << otherOnly;
        QVERIFY2(rightOnly >= 10, qPrintable(QStringLiteral("only %1 found").arg(rightOnly)));
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("TapHandlers without the left button "
                                           "that touch also fires: ")
                            + offenders.join(QStringLiteral(", "))));
    }

    // The sweep's own rule, on handlers the tree may not contain today. It
    // checked only right-button handlers before, so a Back/Forward-only
    // TapHandler with no acceptedDevices passed it while every finger tap
    // fired it.
    void theSweepSeesEveryNonLeftButton_data()
    {
        QTest::addColumn<QString>("handler");
        QTest::addColumn<bool>("offends");
        QTest::newRow("back, no devices")
            << QStringLiteral("acceptedButtons: Qt.BackButton\n") << true;
        QTest::newRow("forward, no devices")
            << QStringLiteral("acceptedButtons: Qt.ForwardButton\n") << true;
        QTest::newRow("back and forward, no devices")
            << QStringLiteral("acceptedButtons: Qt.BackButton | Qt.ForwardButton\n") << true;
        QTest::newRow("extra button, no devices")
            << QStringLiteral("acceptedButtons: Qt.ExtraButton4\n") << true;
        QTest::newRow("middle, no devices")
            << QStringLiteral("acceptedButtons: Qt.MiddleButton\n") << true;
        QTest::newRow("back, touch screen named")
            << QStringLiteral("acceptedButtons: Qt.BackButton\n"
                              "acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchScreen\n")
            << true;
        QTest::newRow("back, devices only in a nested block")
            << QStringLiteral("acceptedButtons: Qt.BackButton\n"
                              "onTapped: { const acceptedDevices: PointerDevice.Mouse }\n")
            << true;
        QTest::newRow("right, no devices")
            << QStringLiteral("acceptedButtons: Qt.RightButton\n") << true;
        QTest::newRow("back and forward, mouse and touchpad")
            << QStringLiteral("acceptedButtons: Qt.BackButton | Qt.ForwardButton\n"
                              "acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad\n")
            << false;
        QTest::newRow("left and back: a tap is meant to fire it")
            << QStringLiteral("acceptedButtons: Qt.LeftButton | Qt.BackButton\n") << false;
        QTest::newRow("all buttons")
            << QStringLiteral("acceptedButtons: Qt.AllButtons\n") << false;
        QTest::newRow("default buttons")
            << QStringLiteral("onTapped: root.open()\n") << false;
    }
    void theSweepSeesEveryNonLeftButton()
    {
        QFETCH(QString, handler);
        QFETCH(bool, offends);
        const QString src = QStringLiteral("Item {\n    TapHandler {\n") + handler
            + QStringLiteral("    }\n}\n");
        const TapHandlerSweep sweep = sweepTapHandlers(src);
        QCOMPARE(sweep.offendingLines, offends ? QList<int>{ 2 } : QList<int>{});
    }
};

QTEST_MAIN(TouchIsNotRightClickQmlTest)
#include "TouchIsNotRightClickQmlTest.moc"
