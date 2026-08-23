// Renders CallHeaderBar in a real window and saves a PNG, so the maintainer's
// visual complaint can be checked visually rather than argued about.
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QImage>
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "matrix/MockMatrixClient.h"
#include <QSignalSpy>
#include <QDateTime>
#include <QFontDatabase>
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Load the bundled fonts the way main.cpp does, or every icon renders as
    // tofu and the screenshot says nothing about the real appearance.
    for (const char *font : {"Manrope[wght].ttf",
                             "MaterialSymbolsRounded-subset.ttf"}) {
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
            + QLatin1String(font));
    }
    AppController controller(AppController::MockBackend);
    QSignalSpy login(controller.auth(), &AuthManager::loginSucceeded);
    controller.auth()->login("https://mock.local", "alice", "unused");
    login.wait(4000);

    // SHOT_THEME lets the same harness prove the bar in a dark theme as well
    // as a light one — the maintainer's report came from Storm (11), and a
    // surface that only works in one palette is a theming bug.
    if (qEnvironmentVariableIsSet("SHOT_THEME")) {
        controller.settings()->setTheme(static_cast<SettingsManager::Theme>(
            qEnvironmentVariableIntValue("SHOT_THEME")));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &controller);
    // A window wrapping the bar, sized like the real conversation column.
    engine.loadData(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient
Window {
    id: win
    visible: true
    // Main.qml pushes the persisted theme into AppTheme; this harness loads
    // the component directly, so it has to do the same or every capture
    // renders in the System default rather than the theme under test.
    Component.onCompleted: {
        AppTheme.mode = app.settings.theme
        AppTheme.textScale = app.settings.textScale / 100
    }
    width: 760; height: 130
    color: AppTheme.background
    Column {
        anchors.fill: parent
        Rectangle {  // stand-in for the room header above the bar
            width: parent.width; height: 60
            color: AppTheme.sidebar
            Text {
                anchors.verticalCenter: parent.verticalCenter
                x: 20; text: "general"
                color: AppTheme.textPrimary
                font.pixelSize: 15; font.weight: Font.DemiBold
            }
        }
        Rectangle { width: parent.width; height: 1; color: AppTheme.border }
        CallHeaderBar { id: bar; width: parent.width; previewMode: true }
    }
    property alias barItem: bar
}
)QML");
    if (engine.rootObjects().isEmpty()) return 2;
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win) return 3;

    // Drive a real ACTIVE legacy call: invite -> answer, exactly the state
    // the maintainer photographed.
    auto *mock = controller.findChild<MockMatrixClient *>();
    controller.setCurrentRoomId("!general:mock.local");
    // An OUTBOUND call: placeCallWithOffer is the seam that reaches
    // Inviting without a media engine, and Inviting is a state the bar owns
    // (a RINGING inbound call belongs to the corner card, because the user
    // may not be looking at that room).
    Q_UNUSED(mock);
    // previewMode renders the bar's appearance without a session: the mock
    // backend implements no call signalling, so there is no honest way to
    // reach a live call here, and faking one would prove less than showing
    // the real component.
    QTimer::singleShot(2200, [&] {
        QImage shot = win->grabWindow();
        shot.save(qEnvironmentVariable("SHOT_OUT"));
        app.quit();
    });
    return app.exec();
}
