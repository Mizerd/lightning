// GitHub #45: with "Show the Spaces rail" off, the Settings button and the
// account menu vanished, because they live only on the rail's bottom cluster.
// RailHiddenActions sits in the room list's header and must be visible ONLY
// while the rail is hidden, so neither is ever drawn twice.
//
// Real AppController on the mock backend; the real SpacesRail and the real
// RailHiddenActions in one scene, visibility driven by the real setting.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"

namespace {
constexpr auto kScene = R"QML(
import QtQuick
import MatrixClient
Window {
    visible: true
    width: 800
    height: 600
    SpacesRail {
        objectName: "scenRail"
        x: 0; y: 0; width: 68; height: 600
        visible: app.settings.spacesRailVisible
    }
    RailHiddenActions {
        objectName: "scenActions"
        x: 300; y: 20
    }
}
)QML";
}

class RailHiddenActionsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_root->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("rail-hidden-actions-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(10000));
        QVERIFY(m_controller->loggedIn());

        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("railhiddenscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        auto *win = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(win);
        QVERIFY(QTest::qWaitForWindowExposed(win));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    // Rail shown: its cog and account tile are the only ones.
    void railShownMeansTheHeaderActionsAreAbsent()
    {
        m_controller->settings()->setSpacesRailVisible(true);
        QTRY_VERIFY(item("railSettingsButton")->isVisible());
        QVERIFY(item("railAccountTile")->isVisible());
        QVERIFY(!item("scenActions")->isVisible());
        QVERIFY(!item("headerSettingsButton")->isVisible());
        QVERIFY(!item("headerAccountTile")->isVisible());
    }

    // Rail hidden: the header carries Settings and the account menu.
    void railHiddenMeansTheHeaderCarriesSettingsAndTheAccountMenu()
    {
        m_controller->settings()->setSpacesRailVisible(false);
        QTRY_VERIFY(item("scenActions")->isVisible());
        QVERIFY(!item("railSettingsButton")->isVisible());
        auto *cog = item("headerSettingsButton");
        auto *tile = item("headerAccountTile");
        QVERIFY(cog && tile);
        QVERIFY(cog->isVisible());
        QVERIFY(tile->isVisible());
        QVERIFY(cog->width() > 0 && tile->width() > 0);

        // The cog really opens Settings, and again returns to chat.
        QMetaObject::invokeMethod(cog, "clicked");
        QTRY_COMPARE(m_controller->currentScreen(), AppController::SettingsScreen);
        QMetaObject::invokeMethod(cog, "clicked");
        QTRY_COMPARE(m_controller->currentScreen(), AppController::MainScreen);

        // The account tile opens the switcher popover.
        auto *popup = tile->findChild<QObject *>(
            QStringLiteral("accountSwitcherPopover"));
        QVERIFY(popup);
        auto *railPopup = item("scenRail")->findChild<QObject *>(
            QStringLiteral("accountSwitcherPopover"));
        QVERIFY(railPopup && railPopup != popup);
        QVERIFY(!popup->property("opened").toBool());
        QVERIFY(!railPopup->property("opened").toBool());
        auto *win = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(win);
        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier,
                          tile->mapToScene(QPointF(tile->width() / 2,
                                                  tile->height() / 2)).toPoint());
        QTRY_VERIFY(popup->property("opened").toBool());
        QVERIFY(!railPopup->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY(!popup->property("opened").toBool());
    }

    // The account tile was a TapHandler only: Tab skipped it and no key opened
    // the switcher. It is a Tab stop now, and Return opens the popover.
    void theHeaderAccountTileIsReachableFromTheKeyboard()
    {
        m_controller->settings()->setSpacesRailVisible(false);
        QTRY_VERIFY(item("scenActions")->isVisible());
        auto *tile = item("headerAccountTile");
        QVERIFY(tile);
        QVERIFY2(tile->property("activeFocusOnTab").toBool(),
                 "the header account tile is not a Tab stop");
        auto *win = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(win);
        win->requestActivate();
        tile->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(tile->hasActiveFocus());
        auto *popup = tile->findChild<QObject *>(
            QStringLiteral("accountSwitcherPopover"));
        QVERIFY(popup);
        QVERIFY(!popup->property("opened").toBool());
        QTest::keyClick(win, Qt::Key_Return);
        QTRY_VERIFY(popup->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY(!popup->property("opened").toBool());
    }

    // The host really instantiates it in the room list header.
    void theRoomListHeaderHostsIt()
    {
        QFile f(QStringLiteral(QML_DIR "/RoomsPanel.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(f.readAll())
                    .contains(QStringLiteral("RailHiddenActions {")));
    }
};

QTEST_MAIN(RailHiddenActionsTest)
#include "RailHiddenActionsTest.moc"
