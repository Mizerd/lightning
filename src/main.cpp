#include "app/AppController.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QTextStream>

namespace {

// Resolve --backend=NAME (case-insensitive). Returns HttpBackend on unknown
// values and reports the ambiguity via *ok.
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

// Simple pre-flight CLI parser that runs *before* QGuiApplication is
// constructed. Bad --backend values or --help are handled here so a Qt
// platform-plugin abort (e.g. no display available) cannot mask a clear
// user error. Only recognises the small surface we own — everything else
// is delegated to QCommandLineParser after QGuiApplication exists.
struct PreflightResult {
    enum Action {
        Continue,      // proceed with normal startup
        ExitSuccess,   // e.g. --help emitted, exit 0
        ExitError,     // bad argument, exit 2
    };
    Action action = Continue;
    AppController::Backend backend = AppController::HttpBackend;
    bool backendExplicit = false;
    bool mockAliasUsed = false;
    QString stderrMsg;
    QString stdoutMsg;
};

PreflightResult preflightParse(int argc, char *argv[])
{
    PreflightResult r;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            r.action = PreflightResult::ExitSuccess;
            r.stdoutMsg = QStringLiteral(
                "matrix-client — native Qt/QML Matrix desktop client.\n"
                "\n"
                "Usage: matrix-client [options]\n"
                "\n"
                "Options:\n"
                "  -h, --help           Show this help and exit.\n"
                "  -v, --version        Show version and exit.\n"
                "  --mock               Alias for --backend=mock.\n"
                "  --backend=NAME       Backend to use. NAME is one of:\n"
                "                         mock  — in-memory, hardcoded rooms\n"
                "                         http  — Matrix Client-Server HTTP API (default)\n"
                "                         rust  — Matrix Rust SDK scaffold (v0.4;\n"
                "                                 requires -DENABLE_RUST_SDK_BACKEND=ON\n"
                "                                 at build time; login not wired yet)\n"
                "\n"
                "See docs/build-and-test.md and docs/backend-contract.md for details.\n");
            return r;
        }
        if (a == QLatin1String("-v") || a == QLatin1String("--version")) {
            r.action = PreflightResult::ExitSuccess;
            r.stdoutMsg = QStringLiteral("matrix-client %1\n").arg(QLatin1String(APP_VERSION));
            return r;
        }
        if (a == QLatin1String("--mock")) {
            r.mockAliasUsed = true;
            continue;
        }
        if (a.startsWith(QLatin1String("--backend="))) {
            const QString value = a.mid(QStringLiteral("--backend=").size());
            bool ok = false;
            AppController::Backend b = backendFromName(value, &ok);
            if (!ok) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "matrix-client: unknown --backend value '%1'; expected one of: mock, http, rust\n"
                    "Run with --help for details.\n").arg(value);
                return r;
            }
            r.backend = b;
            r.backendExplicit = true;
            continue;
        }
        if (a == QLatin1String("--backend")) {
            if (i + 1 >= argc) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "matrix-client: --backend requires a value; expected one of: mock, http, rust\n");
                return r;
            }
            const QString value = QString::fromLocal8Bit(argv[++i]);
            bool ok = false;
            AppController::Backend b = backendFromName(value, &ok);
            if (!ok) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "matrix-client: unknown --backend value '%1'; expected one of: mock, http, rust\n").arg(value);
                return r;
            }
            r.backend = b;
            r.backendExplicit = true;
            continue;
        }
        // Any other flag is deferred to QCommandLineParser after Qt is up.
    }

    if (r.mockAliasUsed) {
        if (r.backendExplicit && r.backend != AppController::MockBackend) {
            r.action = PreflightResult::ExitError;
            r.stderrMsg = QStringLiteral(
                "matrix-client: --mock conflicts with --backend=%1\n"
                ).arg(backendNameFor(r.backend));
            return r;
        }
        r.backend = AppController::MockBackend;
    }

    if (!AppController::isBackendCompiled(r.backend)) {
        r.action = PreflightResult::ExitError;
        QString msg = QStringLiteral(
            "matrix-client: backend '%1' was not compiled into this build.\n"
            ).arg(backendNameFor(r.backend));
        if (r.backend == AppController::RustBackend) {
            msg += QStringLiteral(
                "Reconfigure with -DENABLE_RUST_SDK_BACKEND=ON to enable "
                "the Matrix Rust SDK backend.\n");
        }
        r.stderrMsg = msg;
        return r;
    }

    return r;
}

} // namespace

int main(int argc, char *argv[])
{
    // Parse and validate our own flags before QGuiApplication constructs.
    // This keeps --help / --version / bad --backend from being masked by a
    // Qt platform-plugin abort when no display is available.
    const PreflightResult pf = preflightParse(argc, argv);
    if (pf.action == PreflightResult::ExitSuccess) {
        QTextStream(stdout) << pf.stdoutMsg;
        return 0;
    }
    if (pf.action == PreflightResult::ExitError) {
        QTextStream(stderr) << pf.stderrMsg;
        return 2;
    }

    QCoreApplication::setOrganizationName("MatrixClient");
    QCoreApplication::setOrganizationDomain("matrix-client.local");
    QCoreApplication::setApplicationName("matrix-client");
    QCoreApplication::setApplicationVersion(APP_VERSION);

    QGuiApplication app(argc, argv);
    QGuiApplication::setWindowIcon(QIcon::fromTheme("network-server"));

    // Re-run through QCommandLineParser so --help / --version behave when a
    // user passes them alongside another Qt flag we do not know about, and
    // so that unrecognised args produce the standard Qt error message.
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

    QQuickStyle::setStyle("Fusion");

    AppController controller(pf.backend);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &controller);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
