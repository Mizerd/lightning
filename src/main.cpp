#include "app/AppController.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName("MatrixClient");
    QCoreApplication::setOrganizationDomain("matrix-client.local");
    QCoreApplication::setApplicationName("matrix-client");
    QCoreApplication::setApplicationVersion(APP_VERSION);

    QGuiApplication app(argc, argv);
    QGuiApplication::setWindowIcon(QIcon::fromTheme("network-server"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QGuiApplication::translate("main", "Native Qt/QML Matrix client"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption mockOpt(
        QStringLiteral("mock"),
        QGuiApplication::translate(
            "main", "Use the in-memory mock backend instead of real HTTP."));
    parser.addOption(mockOpt);
    parser.process(app);
    const bool useMock = parser.isSet(mockOpt);

    QQuickStyle::setStyle("Fusion");

    AppController controller(useMock ? AppController::MockBackend
                                     : AppController::HttpBackend);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &controller);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
