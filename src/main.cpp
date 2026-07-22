#include "app/AppController.h"
#include "app/BackendSelection.h"
#include "app/StartupChecks.h"
#include "gif/GifBuildKeys.h"
#include "gif/GifProviderSelfTest.h"
#include "media/MediaImageProvider.h"
#include "storage/AppDataPaths.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "smoke/RustSdkSmokeTest.h"
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QTextStream>

#include <cstdlib>
#include <string>

#ifdef Q_OS_WIN
#include <cstdio>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using lightning::backendFromName;
using lightning::backendNameFor;

#ifdef Q_OS_WIN
// The production Windows binary is a GUI-subsystem PE (no console) so a
// double-click never flashes a terminal. To keep --version / --help /
// --build-info and diagnostic logging usable, attach to the parent console
// when launched from cmd/powershell. A double-click has no parent console, so
// this is a harmless no-op and no window appears. --console forces a visible
// console. UTF-8 output so log punctuation (e.g. en-dashes) is not mojibake.
void configureWindowsConsole(bool forceAlloc)
{
    bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!attached && forceAlloc)
        attached = AllocConsole() != 0;
    if (attached) {
        FILE *f = nullptr;
        f = freopen("CONOUT$", "w", stdout);
        f = freopen("CONOUT$", "w", stderr);
        (void)f;
        SetConsoleOutputCP(CP_UTF8);
    }
}
#endif

#ifndef LIGHTNING_BUILD_TYPE
#define LIGHTNING_BUILD_TYPE "unknown"
#endif

// Non-secret build metadata for --build-info: version, source revision, target
// triple, build type, the shipped Matrix backend, which backends are compiled
// in, whether GIF provider keys are embedded, the secret store, and the
// artifact kind. Machine-checkable (anchored `key: value` lines) so CI can
// assert release invariants. Never prints keys, tokens, URLs, or account data —
// gif_keys_embedded is derived from whether the compiled key is non-empty, not
// from its value.
QString buildInfoString()
{
    const bool rustCompiled =
#ifdef ENABLE_RUST_SDK_BACKEND
        true;
#else
        false;
#endif
    // The HTTP and mock backends are compiled together (or excluded together in
    // a LIGHTNING_RUST_ONLY release).
    const bool httpMockCompiled =
#ifdef LIGHTNING_RUST_ONLY
        false;
#else
        true;
#endif

    QStringList backends;
    if (rustCompiled)
        backends << QStringLiteral("rust");
    if (httpMockCompiled)
        backends << QStringLiteral("http") << QStringLiteral("mock");

    const bool gifKeysEmbedded =
        !gif::buildKeyFor(QStringLiteral("giphy")).isEmpty()
        && !gif::buildKeyFor(QStringLiteral("klipy")).isEmpty();

    const QString secretStore =
#if defined(HAVE_WINCRED)
        QStringLiteral("windows-credential-manager");
#elif defined(HAVE_LIBSECRET)
        QStringLiteral("libsecret");
#else
        QStringLiteral("insecure-qsettings");
#endif

    const auto yn = [](bool b) {
        return b ? QStringLiteral("true") : QStringLiteral("false");
    };
    const QString backendName = backendNameFor(lightning::defaultBackend());

    QString out;
    out += QStringLiteral("version: %1\n").arg(QLatin1String(APP_VERSION));
    out += QStringLiteral("source: %1\n").arg(QLatin1String(LIGHTNING_SOURCE_SHA));
    out += QStringLiteral("target: %1\n").arg(QLatin1String(LIGHTNING_BUILD_TARGET));
    out += QStringLiteral("build_type: %1\n").arg(QLatin1String(LIGHTNING_BUILD_TYPE));
    // matrix_backend is the single shipped/default backend; default_backend is
    // retained as its historical alias so existing CI checks keep matching.
    out += QStringLiteral("matrix_backend: %1\n").arg(backendName);
    out += QStringLiteral("default_backend: %1\n").arg(backendName);
    out += QStringLiteral("backends: %1\n").arg(backends.join(QLatin1Char(',')));
    out += QStringLiteral("rust_backend_compiled: %1\n").arg(yn(rustCompiled));
    out += QStringLiteral("http_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("mock_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("gif_keys_embedded: %1\n").arg(yn(gifKeysEmbedded));
    out += QStringLiteral("secret_store: %1\n").arg(secretStore);
    out += QStringLiteral("artifact_kind: %1\n").arg(QLatin1String(LIGHTNING_ARTIFACT_KIND));
    return out;
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
        RunGifStatus,  // --gif-status: print provider-configured booleans
        RunGifSelfTest, // --gif-selftest: bounded live provider request
    };
    Action action = Continue;
    // Compile-time default (Rust when the SDK backend is built, else HTTP). A
    // packaged desktop launcher passes no --backend flag, so this is what a
    // normal double-click selects; --backend=... overrides it.
    AppController::Backend backend = lightning::defaultBackend();
    bool backendExplicit = false;
    bool mockAliasUsed = false;
    bool smokeTestRequested = false;
    bool consoleRequested = false;   // Windows: force a visible console.
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
                "  --build-info         Print non-secret build metadata (version,\n"
                "                       source, target, backends, default backend,\n"
                "                       secret store, artifact kind) and exit.\n"
                "  --console            Windows: open a visible diagnostic console\n"
                "                       for the GUI-subsystem binary. No effect\n"
                "                       elsewhere.\n"
                "  --mock               Alias for --backend=mock.\n"
                "                       Note: --http and --rust are NOT accepted;\n"
                "                       use --backend=http / --backend=rust instead.\n"
                "  --backend=NAME       Backend to use. NAME is one of:\n"
                "                         mock  — in-memory, hardcoded rooms\n"
                "                         http  — Matrix Client-Server HTTP API\n"
                "                                 (development/diagnostic; no E2EE)\n"
                "                         rust  — Matrix Rust SDK backend (E2EE);\n"
                "                                 requires -DENABLE_RUST_SDK_BACKEND=ON\n"
                "                                 at build time)\n"
                "                       Default for this build: %1.\n"
                "  --reset-crypto-store Delete per-account Rust SDK stores under\n"
                "                       ${XDG_DATA_HOME}/MatrixClient/matrix-client/*/matrix-rust-sdk-store.\n"
                "                       It never touches cache.sqlite or SecretStore tokens.\n"
                "                       Exit code 0 when nothing is found; exit code 3 on error.\n"
                "  --rust-sdk-smoke-test\n"
                "                       Headless verification harness for --backend=rust\n"
                "                       (Rust-enabled build only). Reads credentials from\n"
                "                       LIGHTNING_TEST_HOMESERVER / LIGHTNING_TEST_USER /\n"
                "                       LIGHTNING_TEST_PASSWORD. See docs/build-and-test.md.\n"
                "  --gif-status         Print 'GIPHY/KLIPY configured: yes|no' and exit.\n"
                "                       Booleans only; never prints keys. No network.\n"
                "  --gif-selftest       As --gif-status plus a bounded live trending\n"
                "                       request per configured provider. Exit 0 only if\n"
                "                       every provider is configured and responds.\n"
                "\n"
                "See docs/build-and-test.md and docs/backend-contract.md for details.\n")
                .arg(backendNameFor(lightning::defaultBackend()));
            return r;
        }
        if (a == QLatin1String("-v") || a == QLatin1String("--version")) {
            r.action = PreflightResult::ExitSuccess;
            r.stdoutMsg = QStringLiteral("matrix-client %1\n").arg(QLatin1String(APP_VERSION));
            return r;
        }
        if (a == QLatin1String("--build-info")) {
            r.action = PreflightResult::ExitSuccess;
            r.stdoutMsg = buildInfoString();
            return r;
        }
        if (a == QLatin1String("--console")) {
            // Windows: request a visible diagnostic console. Parsed on every
            // platform so it is never treated as an unknown flag; only acted
            // on under Q_OS_WIN.
            r.consoleRequested = true;
            continue;
        }
        if (a == QLatin1String("--mock")) {
            r.mockAliasUsed = true;
            continue;
        }
        if (a == QLatin1String("--rust-sdk-smoke-test")) {
            r.smokeTestRequested = true;
            continue;
        }
        if (a == QLatin1String("--gif-status")) {
            r.action = PreflightResult::RunGifStatus;
            return r;
        }
        if (a == QLatin1String("--gif-selftest")) {
            r.action = PreflightResult::RunGifSelfTest;
            return r;
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

#ifdef Q_OS_WIN
    // Attach to a parent console (if any) so the GUI-subsystem binary can still
    // print --version / --help / --build-info and diagnostic logs where it was
    // launched; a double-click has no parent console and stays window-only.
    configureWindowsConsole(pf.consoleRequested);
#endif

    // Qt's default message handler routes qCDebug/qCInfo/qCWarning to the
    // systemd journal instead of stderr whenever stderr is not a TTY, which
    // silently hides every category log from piped/offscreen harness runs.
    // Headless and self-test runs need the logs on stderr where a harness
    // can capture them; a real desktop launch keeps the default routing.
    // An explicitly set QT_FORCE_STDERR_LOGGING always wins.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
        const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
        const bool headless = platform.startsWith("offscreen")
                           || platform.startsWith("minimal")
                           || pf.action == PreflightResult::RunSmokeTest
                           || pf.action == PreflightResult::RunGifSelfTest;
        if (headless)
            qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }

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
    if (pf.action == PreflightResult::RunGifStatus) {
        // Booleans only; no Qt application or network needed.
        return gif::printProviderStatus();
    }
    if (pf.action == PreflightResult::RunGifSelfTest) {
        // Needs an event loop for the bounded network request, but no GUI.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication selftestApp(argc, argv);
        return gif::runProviderSelfTest();
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
    //
    // DISPLAY / WAYLAND_DISPLAY are an X11/Wayland concept, so this probe is
    // meaningful ONLY on Unix-like platforms other than macOS. On Windows the
    // "windows" QPA plugin and on macOS the "cocoa" plugin reach a native
    // display with neither variable set — probing there is the bug that made a
    // normal double-click on Windows exit "no graphical display available".
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        constexpr bool kPlatformRequiresDisplayServer = true;
#else
        constexpr bool kPlatformRequiresDisplayServer = false;
#endif
        const bool hasDisplay = std::getenv("DISPLAY") != nullptr
                             || std::getenv("WAYLAND_DISPLAY") != nullptr;
        const bool platformForced = std::getenv("QT_QPA_PLATFORM") != nullptr;
        if (lightning::startup::shouldRejectForNoDisplay(
                kPlatformRequiresDisplayServer, hasDisplay, platformForced)) {
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
    // Wayland compositors match the window to its launcher entry through
    // the desktop-file name (app_id "lightning" ↔ lightning.desktop); X11
    // matches WM_CLASS ("matrix-client") through StartupWMClass.
    QGuiApplication::setDesktopFileName(QStringLiteral("lightning"));
    QGuiApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("lightning"),
        QIcon(QStringLiteral(
            ":/qt/qml/MatrixClient/data/icons/hicolor/192x192/apps/lightning.png"))));

    // Bundled UI fonts (OFL). AppTheme's family lists put them first;
    // failure to load only means the platform fallbacks apply. The v0.7
    // selectable families load alongside the default so Settings →
    // Appearance → Font switches instantly with no disk access.
    for (const char *font : { "Manrope[wght].ttf", "JetBrainsMono[wght].ttf",
                              "Inter[wght].ttf", "IBMPlexSans[wght].ttf",
                              "SourceSans3[wght].ttf",
                              "PlusJakartaSans[wght].ttf",
                              "MaterialSymbolsRounded-subset.ttf" }) {
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
            + QLatin1String(font));
    }
    // Handoff typography everywhere, including native control chrome
    // (menus, popups) that never reads AppTheme's font tokens. The
    // persisted per-account family is applied after the controller exists
    // (before the QML engine loads), so the first rendered frame already
    // uses the selected font.
    QFont uiFont(QStringLiteral("Manrope"));
    uiFont.setPixelSize(14);
    QGuiApplication::setFont(uiFont);

    // Re-run through QCommandLineParser so --help / --version behave when a
    // user passes them alongside another Qt flag we do not know about, and
    // so that unrecognised args produce the standard Qt error message.
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QGuiApplication::translate("main",
            "Native Qt/QML Matrix client. Backend: --backend={mock,http,rust}. "
            "Default: rust (http in builds without the Rust SDK)."));
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
            "Backend to run: mock, http, or rust. "
            "Default: rust (http in builds without the Rust SDK)."),
        QStringLiteral("name"),
        backendNameFor(lightning::defaultBackend()));
    parser.addOption(backendOpt);
    parser.process(app);

    // Basic style: flat, palette-driven controls with no native bevels or
    // gradients. Lightning's shared controls (IconButton, AppButton,
    // SegmentedControl, AppComboBox, AppTextField) own the chrome of every
    // primary surface; Basic keeps any remaining stock control flat and
    // themed instead of Fusion's beveled desktop look.
    QQuickStyle::setStyle("Basic");

    AppController controller(pf.backend);

    // v0.7: the selected UI font applies before the first frame and follows
    // the setting live (font choices are validated inside SettingsManager;
    // mono/icon/emoji font roles are never affected).
    const auto applyUiFont = [](const QString &family) {
        QFont font(family);
        font.setPixelSize(14);
        QGuiApplication::setFont(font);
    };
    applyUiFont(controller.settings()->uiFont());
    QObject::connect(controller.settings(), &SettingsManager::uiFontChanged,
                     &app, [&controller, applyUiFont] {
                         applyUiFont(controller.settings()->uiFont());
                     });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("app", &controller);
    // v0.5.9: serve decrypted media images from the in-memory bridge cache.
    // The engine takes ownership of the provider; the bridge outlives it.
    engine.addImageProvider(QStringLiteral("lightning-media"),
                            new MediaImageProvider(controller.mediaBridge()));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
