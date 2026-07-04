#include "app/AppController.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

namespace {

// Resolve --backend=NAME (case-insensitive). Falls back to HttpBackend on
// unknown values but reports the ambiguity via *ok.
AppController::Backend backendFromName(const QString &name, bool *ok)
{
    if (ok) *ok = true;
    const QString v = name.trimmed().toLower();
    if (v == QLatin1String("mock"))
        return AppController::MockBackend;
    if (v == QLatin1String("http"))
        return AppController::HttpBackend;
    if (v == QLatin1String("rust"))
        return AppController::RustBackend;
    if (ok) *ok = false;
    return AppController::HttpBackend;
}

QString backendNameFor(AppController::Backend backend)
{
    switch (backend) {
    case AppController::MockBackend: return QStringLiteral("mock");
    case AppController::HttpBackend: return QStringLiteral("http");
    case AppController::RustBackend: return QStringLiteral("rust");
    }
    return QStringLiteral("http");
}

} // namespace

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
        QGuiApplication::translate("main",
            "Native Qt/QML Matrix client. Backend: --backend={mock,http,rust}. "
            "Default: http."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption mockOpt(
        QStringLiteral("mock"),
        QGuiApplication::translate("main",
            "Compatibility alias for --backend=mock."));
    parser.addOption(mockOpt);

    QCommandLineOption backendOpt(
        QStringList{ QStringLiteral("backend") },
        QGuiApplication::translate("main",
            "Backend to run: mock, http, or rust. Default: http."),
        QStringLiteral("name"),
        QStringLiteral("http"));
    parser.addOption(backendOpt);

    parser.process(app);

    AppController::Backend chosen = AppController::HttpBackend;
    bool backendExplicit = false;

    if (parser.isSet(backendOpt)) {
        bool ok = false;
        chosen = backendFromName(parser.value(backendOpt), &ok);
        if (!ok) {
            QTextStream(stderr)
                << "matrix-client: unknown --backend value '"
                << parser.value(backendOpt)
                << "'; expected one of: mock, http, rust\n";
            return 2;
        }
        backendExplicit = true;
    }

    if (parser.isSet(mockOpt)) {
        if (backendExplicit && chosen != AppController::MockBackend) {
            QTextStream(stderr)
                << "matrix-client: --mock conflicts with --backend="
                << backendNameFor(chosen) << "\n";
            return 2;
        }
        chosen = AppController::MockBackend;
    }

    if (!AppController::isBackendCompiled(chosen)) {
        QTextStream(stderr)
            << "matrix-client: backend '" << backendNameFor(chosen)
            << "' was not compiled into this build.\n";
        if (chosen == AppController::RustBackend) {
            QTextStream(stderr)
                << "Reconfigure with -DENABLE_RUST_SDK_BACKEND=ON to enable "
                   "the Matrix Rust SDK backend.\n";
        }
        return 2;
    }

    QQuickStyle::setStyle("Fusion");

    AppController controller(chosen);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &controller);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
