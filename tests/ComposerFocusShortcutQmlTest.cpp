#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"
#include "models/MessageComposer.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"

class ComposerFocusShortcutQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_window->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

    QObject *switcher() const
    {
        const auto objects = m_window->findChildren<QObject *>();
        for (QObject *object : objects) {
            if (QByteArray(object->metaObject()->className())
                    .startsWith("QuickSwitcher"))
                return object;
        }
        return nullptr;
    }

    void openSwitcher()
    {
        QTest::keyClick(m_window, Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(switcher()->property("opened").toBool());
        QTRY_VERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    int resultRow(bool invite = false, bool space = false) const
    {
        auto *model = m_controller->quickSwitcher();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap result = model->resultAt(row);
            if (result.value(QStringLiteral("isInvite")).toBool() == invite
                && result.value(QStringLiteral("isSpace")).toBool() == space)
                return row;
        }
        return -1;
    }

private:
    void openJoinedRoom()
    {
        openSwitcher();
        const int row = resultRow();
        QVERIFY(row >= 0);
        QVERIFY(item("quickSwitcherList")->setProperty("currentIndex", row));
        const QString roomId = m_controller->quickSwitcher()->resultAt(row)
                                   .value(QStringLiteral("roomId")).toString();
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_controller->currentRoomId(), roomId);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);
        QTRY_VERIFY(m_controller->timeline()->realEventCount() > 2);
        // Let upstream's one-shot automatic composer focus settle. It must
        // not make an absent explicit shortcut look as if it worked.
        QTest::qWait(300);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + "/config").toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + "/data").toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("composer-focus-test"));
    }

    void init()
    {
        QSettings().clear();
        // In-memory secrets and deterministic rooms, Spaces and invitations;
        // the screenshot UI itself is not enabled.
        m_controller = new AppController(AppController::MockBackend, true);
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setScreenshotDemoMode(true);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://lightning.example"),
                                    QStringLiteral("alex"),
                                    QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(4000));
        QTRY_VERIFY(m_controller->roomList()->rowCount() > 0);
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        QQmlComponent scene(m_engine);
        scene.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 1100
    height: 800
    visible: true
    MainScreen { anchors.fill: parent }
}
)QML", QUrl(QStringLiteral("composer-focus-scene.qml")));
        QObject *root = scene.create();
        QVERIFY2(root, qPrintable(scene.errorString()));
        m_window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QVERIFY(switcher());
    }

    void cleanup()
    {
        delete m_window;
        delete m_engine;
        delete m_controller;
        m_window = nullptr;
        m_engine = nullptr;
        m_controller = nullptr;
    }

    void focusShortcutCanBeReboundAndDoesNotStealSwitcherFocus()
    {
        openSwitcher();
        const int row = resultRow();
        QVERIFY(row >= 0);
        const QString roomId = m_controller->quickSwitcher()->resultAt(row)
                                   .value(QStringLiteral("roomId")).toString();
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!switcher()->property("visible").toBool());
        m_controller->openRoom(roomId);
        QVERIFY(item("timelinePane"));
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);

        auto *registry = m_controller->shortcuts();
        QCOMPARE(registry->setBinding(QStringLiteral("composer.focus"),
                                      QStringLiteral("Ctrl+J")), QString());
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(item("composerInput")->hasActiveFocus(), 1500);
        openSwitcher();
        QTest::keyClick(m_window, Qt::Key_J, Qt::ControlModifier);
        QVERIFY(item("quickSwitcherField")->hasActiveFocus());
    }

    void focusShortcutUsesActiveEditor_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("markdown") << QStringLiteral("markdown");
        QTest::newRow("rich") << QStringLiteral("rich");
    }

    void focusShortcutUsesActiveEditor()
    {
        QFETCH(QString, mode);
        openJoinedRoom();
        m_controller->settings()->setProperty("composerMode", mode);
        m_controller->composer()->setText(QStringLiteral("keep this draft"));
        auto *editor = item(mode == QStringLiteral("rich")
                                ? "composerRichInput" : "composerInput");
        QTRY_VERIFY(editor->property("text").toString().contains("keep this draft"));
        const QString draft = m_controller->composer()->text();
        item("timelinePane")->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(editor->hasActiveFocus(), 1500);
        QCOMPARE(m_controller->composer()->text(), draft);
    }

    void focusShortcutRefusesHiddenComposer()
    {
        openJoinedRoom();
        auto *composer = item("messageComposer");
        QVERIFY(composer);
        composer->setVisible(false);
        item("timelinePane")->forceActiveFocus();
        auto *before = m_window->activeFocusItem();
        QTest::keyClick(m_window, Qt::Key_L, Qt::ControlModifier);
        QTest::qWait(50);
        QCOMPARE(m_window->activeFocusItem(), before);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ComposerFocusShortcutQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ComposerFocusShortcutQmlTest.moc"
