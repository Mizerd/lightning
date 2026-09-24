// Renders CallHeaderBar in a real window and saves a PNG for visual review.
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

    // SHOT_THEME renders the bar in another theme (e.g. Storm, 11); a surface
    // that works in one palette only is a theming bug.
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

    // Drive a real active legacy call: invite, then answer.
    auto *mock = controller.findChild<MockMatrixClient *>();
    controller.setCurrentRoomId("!general:mock.local");
    // An outbound call: placeCallWithOffer reaches Inviting without a media
    // engine, and Inviting belongs to the bar (a ringing inbound call belongs
    // to the corner card).
    Q_UNUSED(mock);
    // previewMode renders the bar without a session: the mock implements no
    // call signalling, and faking a call would prove less than showing the
    // real component.
    QTimer::singleShot(2200, [&] {
        QImage shot = win->grabWindow();
        shot.save(qEnvironmentVariable("SHOT_OUT"));
        app.quit();
    });
    return app.exec();
}
