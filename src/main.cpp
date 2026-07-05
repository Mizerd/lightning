#include "app/AppController.h"
#include "storage/AppDataPaths.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "smoke/RustSdkSmokeTest.h"
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QTextStream>

#include <cstdlib>
#include <string>

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
        ExitResetError, // --reset-crypto-store failed, exit 3
        RunSmokeTest,  // --rust-sdk-smoke-test (Rust build only)
    };
    Action action = Continue;
    AppController::Backend backend = AppController::HttpBackend;
    bool backendExplicit = false;
    bool mockAliasUsed = false;
    bool smokeTestRequested = false;
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
                "                       Note: --http and --rust are NOT accepted;\n"
                "                       use --backend=http / --backend=rust instead.\n"
                "  --backend=NAME       Backend to use. NAME is one of:\n"
                "                         mock  — in-memory, hardcoded rooms\n"
                "                         http  — Matrix Client-Server HTTP API (default)\n"
                "                         rust  — Matrix Rust SDK backend foundation;\n"
                "                                 requires -DENABLE_RUST_SDK_BACKEND=ON\n"
                "                                 at build time)\n"
                "  --reset-crypto-store Delete per-account Rust SDK stores under\n"
                "                       ${XDG_DATA_HOME}/matrix-client/*/matrix-rust-sdk-store.\n"
                "                       It never touches cache.sqlite or SecretStore tokens.\n"
                "                       Exit code 0 when nothing is found; exit code 3 on error.\n"
                "  --rust-sdk-smoke-test\n"
                "                       Headless verification harness for --backend=rust\n"
                "                       (Rust-enabled build only). Reads credentials from\n"
                "                       LIGHTNING_TEST_HOMESERVER / LIGHTNING_TEST_USER /\n"
                "                       LIGHTNING_TEST_PASSWORD. See docs/build-and-test.md.\n"
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
        if (a == QLatin1String("--rust-sdk-smoke-test")) {
            r.smokeTestRequested = true;
            continue;
        }
        if (a == QLatin1String("--reset-crypto-store")) {
            r.action = PreflightResult::ExitSuccess;

            // Scan the SAME app-data roots that RustSdkMatrixClient uses at
            // runtime PLUS any legacy roots earlier v0.5.0-prep builds might
            // have populated. The primary root matches
            // QStandardPaths::AppLocalDataLocation with
            // OrganizationName=MatrixClient, ApplicationName=matrix-client,
            // resolved without constructing a QCoreApplication.
            const QStringList roots = matrix::app_data::allRoots();

            r.stdoutMsg = QStringLiteral(
                "matrix-client --reset-crypto-store\n"
                "\n");
            if (roots.isEmpty()) {
                r.stdoutMsg += QStringLiteral(
                    "No app data root available "
                    "(neither $HOME nor $XDG_DATA_HOME is set).\n");
                return r;
            }

            QString deleted;
            QString failed;
            int scanned = 0;
            for (const auto &root : roots) {
                r.stdoutMsg += QStringLiteral("Scanning: %1\n").arg(root);
                const QStringList stores = matrix::app_data::findRustStoresIn(root);
                for (const QString &cryptoPath : stores) {
                    ++scanned;
                    QDir storeDir(cryptoPath);
                    if (storeDir.removeRecursively()) {
                        deleted += QStringLiteral("    %1\n").arg(cryptoPath);
                    } else {
                        failed += QStringLiteral("    %1\n").arg(cryptoPath);
                    }
                }
            }
            r.stdoutMsg += QStringLiteral("\n");

            if (!deleted.isEmpty())
                r.stdoutMsg += QStringLiteral("Deleted:\n%1\n").arg(deleted);
            if (scanned == 0) {
                r.stdoutMsg += QStringLiteral(
                    "No Rust SDK store directories found.\n");
            }
            if (!failed.isEmpty()) {
                r.action = PreflightResult::ExitResetError;
                r.stderrMsg = QStringLiteral(
                    "Failed to delete Rust SDK store directories:\n%1").arg(failed);
            }
            return r;
        }
        // v0.4.3: catch the user-friendly-looking shortcuts before Qt sees
        // them. QCommandLineParser would otherwise treat them as unknown
        // options AFTER QGuiApplication is constructed — that path can
        // abort on a Qt platform-plugin problem before the error message
        // reaches the user. Reject cleanly with a hint.
        if (a == QLatin1String("--http") || a == QLatin1String("--rust")) {
            const QString value = a.mid(2); // strip leading "--"
            r.action = PreflightResult::ExitError;
            r.stderrMsg = QStringLiteral(
                "matrix-client: '%1' is not a supported flag. "
                "Use '--backend=%2' instead.\n"
                "Run with --help for the full list.\n").arg(a, value);
            return r;
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

    if (r.smokeTestRequested) {
        if (r.backend != AppController::RustBackend || !r.backendExplicit) {
            r.action = PreflightResult::ExitError;
            r.stderrMsg = QStringLiteral(
                "matrix-client: --rust-sdk-smoke-test requires "
                "'--backend=rust'.\n"
                "See docs/build-and-test.md for the environment variables "
                "the harness reads.\n");
            return r;
        }
#ifdef ENABLE_RUST_SDK_BACKEND
        r.action = PreflightResult::RunSmokeTest;
#else
        r.action = PreflightResult::ExitError;
        r.stderrMsg = QStringLiteral(
            "matrix-client: --rust-sdk-smoke-test needs a Rust-enabled "
            "build (reconfigure with -DENABLE_RUST_SDK_BACKEND=ON).\n");
#endif
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
    if (pf.action == PreflightResult::ExitResetError) {
        QTextStream(stdout) << pf.stdoutMsg;
        QTextStream(stderr) << pf.stderrMsg;
        return 3;
    }
#ifdef ENABLE_RUST_SDK_BACKEND
    if (pf.action == PreflightResult::RunSmokeTest) {
        // Headless: no QGuiApplication, no QML engine, no display probe.
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClient"));
        QCoreApplication::setOrganizationDomain(
            QStringLiteral("matrix-client.local"));
        return runRustSdkSmokeTest(argc, argv);
    }
#endif

    // Second preflight: refuse to construct QGuiApplication when no display
    // can be reached. Otherwise Qt calls qFatal → abort() from its platform
    // plugin initialiser and the process SIGABRTs (this is the same stack
    // trace as the coredump reported for v0.4.0). Users can still opt into
    // headless execution by setting QT_QPA_PLATFORM=offscreen (used by the
    // smoke tests).
    {
        const bool hasDisplay = std::getenv("DISPLAY") != nullptr
                             || std::getenv("WAYLAND_DISPLAY") != nullptr;
        const bool platformForced = std::getenv("QT_QPA_PLATFORM") != nullptr;
        if (!hasDisplay && !platformForced) {
            QTextStream(stderr) <<
                "matrix-client: no graphical display available "
                "(DISPLAY / WAYLAND_DISPLAY unset).\n"
                "Run this app inside a graphical session, or export "
                "QT_QPA_PLATFORM=offscreen for a headless smoke test.\n"
                "See docs/build-and-test.md for the exact commands.\n";
            return 3;
        }
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
