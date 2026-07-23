// Development-only screenshot-demo control-panel QML contract.
//
// Loads the REAL Main.qml with a demo AppController + its ScreenshotDemoController
// and asserts, in a real (offscreen) QML window, that: the control panel loads
// with no QML warnings and NO layout gap; the boot scenario resizes the actual
// window; a window preset resizes it; hide/restore (controlsVisible) hides the
// panel entirely; and window presets never leave the window below its minimum.

#include "app/AppController.h"
#include "app/ScreenshotDemoController.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class ScreenshotDemoPanelQmlTest : public QObject
{
    Q_OBJECT

    static constexpr int kTimeout = 5000;

    QQuickItem *findPanel() const
    {
        return m_window
            ? m_window->findChild<QQuickItem *>(QStringLiteral("demoControlPanel"))
            : nullptr;
    }
    ScreenshotDemoController *demo() const
    {
        return qobject_cast<ScreenshotDemoController *>(m_controller->demoController());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("screenshot-demo-panel-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend,
                                         /*screenshotDemo=*/true);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        m_controller->beginScreenshotDemo();
        QSignalSpy createdSpy(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kTimeout));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        // Boot restore lands on the scene.
        QTRY_COMPARE(m_controller->currentScreen(), AppController::MainScreen);
        QTRY_COMPARE(m_controller->currentRoomId(),
                     QStringLiteral("!design-lounge:lightning.example"));
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    void panelLoadsWithoutQmlWarnings()
    {
        QVERIFY(demo());
        QQuickItem *panel = findPanel();
        QVERIFY2(panel, "DemoControlPanel did not load");
        QVERIFY(panel->isVisible());
        // No QML warnings during load/boot.
        QVERIFY2(m_warnings.isEmpty(),
                 qUtf8Printable("QML warnings:\n" + m_warnings.join('\n')));
    }

    void bootScenarioResizedTheRealWindow()
    {
        // home-overview requests 1440x900.
        QTRY_COMPARE(m_window->width(), 1440);
        QTRY_COMPARE(m_window->height(), 900);
    }

    void windowPresetResizesTheRealWindow()
    {
        demo()->setWindowSize(QStringLiteral("1600x1000"));
        QTRY_COMPARE(m_window->width(), 1600);
        QTRY_COMPARE(m_window->height(), 1000);
        // Narrow preset.
        demo()->setWindowSize(QStringLiteral("narrow"));
        QTRY_COMPARE(m_window->width(), 760);
        QTRY_COMPARE(m_window->height(), 900);
    }

    void presetNeverGoesBelowWindowMinimum()
    {
        // 900x900 is above the window minimum; the request is honored exactly.
        demo()->setWindowSize(QStringLiteral("900x900"));
        QTRY_COMPARE(m_window->width(), 900);
        QVERIFY(m_window->width() >= m_window->minimumWidth());
        QVERIFY(m_window->height() >= m_window->minimumHeight());
    }

    void hideAndRestoreLeavesNoGap()
    {
        QQuickItem *panel = findPanel();
        QVERIFY(panel);
        // The panel is an overlay that fills its parent (anchors.fill), so it is
        // never part of any layout — hiding it reserves no space (no gap) and
        // restore brings it back. Both the overlay-fill invariant and the
        // visibility toggle are checked.
        QVERIFY(panel->parentItem());
        QTRY_COMPARE(panel->width(), panel->parentItem()->width());
        demo()->setControlsVisible(false);
        QTRY_VERIFY(!panel->isVisible());
        demo()->toggleControls();   // Ctrl+Shift+D equivalent
        QTRY_VERIFY(panel->isVisible());
    }

    void scenarioActivationDrivesTheWindow()
    {
        demo()->activateScenario(QStringLiteral("thread-view"));
        QTRY_COMPARE(m_window->width(), 1600);
        QTRY_COMPARE(m_window->height(), 1000);
        QTRY_COMPARE(m_controller->currentRoomId(),
                     QStringLiteral("!dev:lightning.example"));
    }

private:
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(ScreenshotDemoPanelQmlTest)
#include "ScreenshotDemoPanelQmlTest.moc"
