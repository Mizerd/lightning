#include "app/AppController.h"
#include "i18n/LocalizationManager.h"
#include "app/BackendSelection.h"
#include "app/StartupChecks.h"
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include "app/ScreenshotDemoController.h"  // demo CLI validation (dev builds only)
#endif
#include "crypto/QrImageProvider.h"
#include "gif/GifBuildKeys.h"
#include "gif/GifProviderSelfTest.h"
#include "app/StormBandPainter.h"
#include "media/MediaImageProvider.h"
#include "media/StagedImageProvider.h"
#include "app/GuiStallTracer.h"
#include "media/VaapiLogGate.h"
#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"

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
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include <QQuickWindow>   // headless --demo-capture (dev builds only)
#include <QImage>
#include <QTimer>
#endif
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
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
    // Development-only screenshot/demo mode. A release build must report false;
    // CI asserts this to prove the demo cannot be reached in a shipped binary.
    const bool screenshotDemoCompiled =
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
        true;
#else
        false;
#endif
    out += QStringLiteral("screenshot_demo_compiled: %1\n").arg(yn(screenshotDemoCompiled));
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
    // Development-only screenshot/demo mode (compile option
    // LIGHTNING_ENABLE_SCREENSHOT_DEMO). Rejected in preflight when the option
    // is not compiled in, so a production binary never reaches it.
    bool screenshotDemo = false;
    // Development-only demo launch options (see --demo-*). Empty/false in every
    // normal build; a production binary rejects the flags as unknown options.
    QString demoScenario;
    QString demoAccount;
    QString demoTheme;
    QString demoAppearance;
    QString demoSize;
    bool demoHideControls = false;
    // Development-only: grab the window to a PNG once the scene settles, then
    // quit — for headless screenshot regeneration/verification.
    QString demoCapture;
    int demoCaptureDelayMs = 1400;
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
                "  --screenshot-demo    Development builds only: boot the real UI on\n"
                "                       the in-memory mock backend with deterministic\n"
                "                       fake accounts/rooms for screenshots. No network,\n"
                "                       isolated storage. Rejected unless compiled with\n"
                "                       -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON (never a\n"
                "                       release build). See docs/screenshot-demo.md.\n"
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
        if (a == QLatin1String("--screenshot-demo")) {
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
            // Development build: boot the real UI on the in-memory mock backend
            // with deterministic fake data. Force the mock backend and mark the
            // run so main() can isolate storage and auto-login.
            r.screenshotDemo = true;
            r.backend = AppController::MockBackend;
            r.backendExplicit = true;
            continue;
#else
            // Production/normal build: the option was not compiled in. Reject
            // here in preflight (before QGuiApplication) so a shipped binary
            // has no reachable path into demo mode, and so the rejection is
            // testable headlessly.
            r.action = PreflightResult::ExitError;
            r.stderrMsg = QStringLiteral(
                "matrix-client: --screenshot-demo is a development-only build "
                "option that is not compiled into this binary.\n"
                "Rebuild with -DLIGHTNING_ENABLE_SCREENSHOT_DEMO=ON (never a "
                "release build) to use it.\n");
            return r;
#endif
        }
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
        // Development-only demo launch options. Parsed (and validated) only in a
        // demo build; in any other build they fall through to QCommandLineParser,
        // which rejects them as unknown options.
        if (a.startsWith(QLatin1String("--demo-scenario="))) {
            r.demoScenario = a.mid(QStringLiteral("--demo-scenario=").size());
            if (!ScreenshotDemoController::isValidScenario(r.demoScenario)) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "matrix-client: unknown --demo-scenario '%1'.\n"
                    "Run scripts/run-screenshot-demo.sh --help for the list.\n")
                    .arg(r.demoScenario);
                return r;
            }
            continue;
        }
        if (a.startsWith(QLatin1String("--demo-account="))) {
            r.demoAccount = a.mid(QStringLiteral("--demo-account=").size());
            continue;
        }
        if (a.startsWith(QLatin1String("--demo-theme="))) {
            r.demoTheme = a.mid(QStringLiteral("--demo-theme=").size());
            continue;
        }
        if (a.startsWith(QLatin1String("--demo-appearance="))) {
            r.demoAppearance = a.mid(QStringLiteral("--demo-appearance=").size());
            continue;
        }
        if (a.startsWith(QLatin1String("--demo-size="))) {
            r.demoSize = a.mid(QStringLiteral("--demo-size=").size());
            int w = 0, h = 0;
            if (!ScreenshotDemoController::sizeForPreset(r.demoSize, &w, &h)) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "matrix-client: invalid --demo-size '%1' (use WxH within "
                    "safe bounds, or a named preset).\n").arg(r.demoSize);
                return r;
            }
            continue;
        }
        if (a == QLatin1String("--demo-hide-controls")) {
            r.demoHideControls = true;
            continue;
        }
        if (a.startsWith(QLatin1String("--demo-capture="))) {
            // --demo-capture=PATH.png[,delayMs]
            const QString v = a.mid(QStringLiteral("--demo-capture=").size());
            const int comma = v.indexOf(QLatin1Char(','));
            if (comma > 0) {
                r.demoCapture = v.left(comma);
                bool ok = false;
                const int d = v.mid(comma + 1).toInt(&ok);
                if (ok && d >= 0) r.demoCaptureDelayMs = d;
            } else {
                r.demoCapture = v;
            }
            continue;
        }
#endif
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

// Chained message handler bounding the VAAPI texture-export warning storm
// (see VaapiLogGate). Qt calls message handlers from arbitrary threads; the
// gate's counter is atomic and the previous handler does its own locking.
QtMessageHandler g_previousMessageHandler = nullptr;
VaapiLogGate g_vaapiLogGate;

void vaapiGatedMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                              const QString &msg)
{
    switch (g_vaapiLogGate.classify(msg)) {
    case VaapiLogGate::Action::Drop:
        return;
    case VaapiLogGate::Action::Summary:
        if (g_previousMessageHandler)
            g_previousMessageHandler(QtWarningMsg, ctx,
                                     g_vaapiLogGate.summaryLine());
        return;
    case VaapiLogGate::Action::Print:
        break;
    }
    if (g_previousMessageHandler)
        g_previousMessageHandler(type, ctx, msg);
}

void installVaapiLogGate()
{
    g_previousMessageHandler = qInstallMessageHandler(vaapiGatedMessageHandler);
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

    // Deliberately NOT setting QT_DISABLE_HW_TEXTURES_CONVERSION here.
    // It was briefly defaulted to 1 to skip the per-frame
    // vaExportSurfaceHandle failure storm, but on the very Mesa stack it
    // was meant to help it broke playback outright — black frames and a
    // frozen UI (maintainer live test, 2026-08-12). The noisy
    // export-attempt-then-CPU-fallback path, bounded by VaapiLogGate,
    // actually plays; users on stacks where the variable helps can still
    // set it in their environment.
    //
    // Software video decoding IS defaulted, with live evidence: a rotated
    // (display-matrix) H.264 video hard-deadlocked the render pipeline
    // through VAAPI twice on the maintainer's desktop — audio running,
    // frames black, the main thread unresponsive to SIGINT — and the same
    // video plays correctly with software decoding. Chat-sized clips do
    // not need hardware decode, and a deadlock is strictly worse than a
    // few percent CPU. An explicitly set value (including "" to restore
    // Qt's own default probing) always wins.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "none");

    // Bound the per-frame VAAPI texture-export warning spam from Qt's
    // FFmpeg video backend (thousands of identical lines per minute on
    // hardware whose driver cannot export decoded surfaces — see
    // VaapiLogGate). Everything else chains to the previous handler
    // untouched. Installed before QGuiApplication so the earliest decoder
    // warnings are already gated.
    installVaapiLogGate();

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

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Storage isolation: a distinct applicationName redirects EVERY default
    // QSettings store (theme/appearance/account registry, GIF favourites, the
    // insecure token fallback) to a separate file, so the demo never reads or
    // writes the developer's real Lightning configuration. The mock backend
    // touches no other store (no cache.sqlite, Rust store, or SecretStore).
    // run-screenshot-demo.sh additionally points XDG_{DATA,CONFIG,CACHE}_HOME at
    // a dedicated demo directory for belt-and-suspenders full isolation.
    if (pf.screenshotDemo)
        QCoreApplication::setApplicationName(
            QStringLiteral("matrix-client-screenshot-demo"));
#endif

    // ── Portable mode ─────────────────────────────────────────────────
    // This is the last moment the decision can be made. The very next block
    // default-constructs a QSettings for the interface-zoom value, and on
    // Windows a default-constructed QSettings is REGISTRY-backed (HKCU) —
    // once one exists in the process the storage backend is settled and
    // there is no retracting it. The screenshot-demo block above is the same
    // pattern for the same reason, which is why this sits after it: the
    // application name it may have changed feeds the INI file name below.
    //
    // Note the ordering relative to QGuiApplication (constructed further
    // down): there is no QCoreApplication instance here, so
    // lightning::portable resolves the executable directory from the platform
    // API rather than applicationDirPath(). See storage/PortableMode.h.
    if (lightning::portable::isPortable()) {
        const QString portableProblem = lightning::portable::prepareDataRoot();
        if (!portableProblem.isEmpty()) {
            const QString advice =
                QStringLiteral("Lightning portable needs write access to its "
                               "data directory. Move the extracted folder to "
                               "a writable location and try again.");
            QTextStream(stderr) << portableProblem << "\n" << advice << "\n";
#ifdef Q_OS_WIN
            // Lightning.exe is a GUI-subsystem PE, so a user who
            // double-clicked it has no console and would see NOTHING — the
            // application would simply fail to appear, which is the worst
            // possible outcome for the one error this mode is most likely to
            // produce (extracted into Program Files, or onto read-only
            // media). MessageBoxW is used directly rather than QMessageBox
            // because this runs before QGuiApplication exists, by design: the
            // portable decision has to happen before the first QSettings.
            const QString text = portableProblem + QStringLiteral("\n\n") + advice;
            MessageBoxW(nullptr,
                        reinterpret_cast<const wchar_t *>(text.utf16()),
                        L"Lightning", MB_OK | MB_ICONERROR);
#endif
            // Deliberately NOT a fallback to the installed locations. A
            // portable copy that quietly starts writing to %LOCALAPPDATA%,
            // the registry and the Credential Manager is the exact defect
            // this mode exists to fix: the user would sign in, copy the
            // folder to another PC, and be asked to sign in again with no
            // indication of why. Refusing loudly is the honest outcome.
            return 4;
        }
        // Every default-constructed QSettings in the process — here,
        // SettingsManager, GifSearchController, the GIF models, the insecure
        // token fallback — lands in
        // <dataRoot>/config/MatrixClient/matrix-client.ini. Nothing is
        // written to, or synchronised back to, the registry.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           lightning::portable::configDir());

        // ── Qt's OWN caches ────────────────────────────────────────────────
        //
        // Lightning's cache abstraction was already redirected; Qt's was not,
        // and Qt writes on its own initiative. The QML disk cache and the Qt
        // Quick graphics/shader pipeline cache both default to
        // QStandardPaths::CacheLocation, which on Windows is under
        // %LOCALAPPDATA% — outside the folder, with nothing in this codebase
        // pointing at it. Neither holds anything private, but "portable" here
        // means Lightning writes nothing persistent outside its own tree, and
        // these were two quiet exceptions.
        //
        // The QML cache is REDIRECTED (documented env var, keeps the startup
        // win). The shader cache is DISABLED rather than redirected: its path
        // is not settable by environment, only through a per-window
        // QQuickGraphicsConfiguration, and a portable copy that silently keeps
        // writing outside the folder because one window was constructed
        // without that call is the failure mode this mode exists to prevent.
        // The cost is recompiling pipelines on each launch — a first-frame
        // cost, not a running one. Refusing beats leaking.
        //
        // Set before QGuiApplication, because Qt reads both at construction.
        qputenv("QML_DISK_CACHE_PATH",
                QDir::toNativeSeparators(
                    lightning::portable::cacheDir()
                    + QStringLiteral("/qmlcache")).toLocal8Bit());
        QDir().mkpath(lightning::portable::cacheDir()
                      + QStringLiteral("/qmlcache"));
        qputenv("QT_DISABLE_SHADER_DISK_CACHE", "1");

        // Scratch directories from a run that crashed before its
        // QTemporaryDir destructor executed. Ours only, by name prefix, and
        // never a symlink.
        const int swept = lightning::portable::cleanStaleTempDirs();
        if (swept > 0) {
            QTextStream(stderr)
                << "portable: removed " << swept
                << " stale media scratch director"
                << (swept == 1 ? "y" : "ies") << "\n";
        }
    }

    // Interface zoom (Settings → Appearance, Ctrl+= / Ctrl+-): Qt reads
    // QT_SCALE_FACTOR exactly once at startup, so the persisted percent is
    // applied here, pre-QGuiApplication — that is why zoom changes take
    // effect on the next launch. An explicitly set user env always wins
    // (and is then left untouched below).
    bool zoomEnvSetHere = false;
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        const int zoom =
            QSettings().value(QStringLiteral("ui/interfaceZoom"), 100).toInt();
        if (zoom != 100 && zoom >= SettingsManager::kMinInterfaceZoom
            && zoom <= SettingsManager::kMaxInterfaceZoom) {
            qputenv("QT_SCALE_FACTOR",
                    QByteArray::number(zoom / 100.0, 'f', 2));
            zoomEnvSetHere = true;
        }
    }

#ifdef Q_OS_WIN
    // Pin the Qt Multimedia FFmpeg backend on Windows so inline video decodes
    // reliably. The Windows Media Foundation backend delivers the first
    // buffered frames and then stalls the video surface; the packaged build
    // ships ffmpegmediaplugin.dll + the FFmpeg runtime DLLs, and packaging
    // validation fails closed if they are absent. Respect an explicit user
    // override. No effect on other platforms (FFmpeg is already Qt's default
    // on the pinned Linux qtmultimedia).
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
#endif

    QGuiApplication app(argc, argv);
    // Opt-in GUI-thread stall tracing (LIGHTNING_GUI_STALL_TRACE): a
    // heartbeat + watchdog that logs any event-loop stall over the
    // threshold with a coarse category. Duration and category literal
    // only — see GuiStallTracer.h.
    stalltrace::install();
    // Qt has read the scale factor now; drop it from the environment so it
    // does not leak into child processes (the OAuth system browser,
    // xdg-open) and zoom THEIR UI too (review L2). A user-set env var is
    // deliberately left alone.
    if (zoomEnvSetHere)
        qunsetenv("QT_SCALE_FACTOR");
    // Wayland compositors match the window to its launcher entry through
    // the desktop-file name (app_id "lightning" ↔ lightning.desktop); X11
    // matches WM_CLASS ("matrix-client") through StartupWMClass.
    QGuiApplication::setDesktopFileName(QStringLiteral("lightning"));
    QGuiApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("lightning"),
        QIcon(QStringLiteral(
            ":/qt/qml/MatrixClient/data/icons/hicolor/256x256/apps/lightning.png"))));

    // Bundled UI fonts (OFL). AppTheme's family lists put them first;
    // failure to load only means the platform fallbacks apply. The v0.7
    // selectable families load alongside the default so Settings →
    // Appearance → Font switches instantly with no disk access.
    for (const char *font : { "Manrope[wght].ttf", "JetBrainsMono[wght].ttf",
                              "Inter[wght].ttf", "IBMPlexSans[wght].ttf",
                              "SourceSans3[wght].ttf",
                              "PlusJakartaSans[wght].ttf",
                              "SpaceGrotesk[wght].ttf",
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
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Preflight already consumed --screenshot-demo (a development-only build);
    // register it here so QCommandLineParser::process does not reject it as an
    // unknown option. In a normal/release build the flag never reaches this
    // parser — preflight exits first.
    QCommandLineOption screenshotDemoOpt(
        QStringLiteral("screenshot-demo"),
        QGuiApplication::translate("main",
            "Development-only: boot the mock backend with deterministic demo "
            "data for screenshots."));
    parser.addOption(screenshotDemoOpt);
    // Development-only demo launch options — registered so process() accepts
    // them (preflight already consumed and validated them). Never reach this
    // parser in a normal build (preflight exits on --screenshot-demo first).
    for (const char *name : { "demo-scenario", "demo-account", "demo-theme",
                              "demo-appearance", "demo-size", "demo-capture" }) {
        parser.addOption(QCommandLineOption(
            QString::fromLatin1(name),
            QGuiApplication::translate("main",
                "Development-only screenshot-demo option."),
            QStringLiteral("value")));
    }
    parser.addOption(QCommandLineOption(
        QStringLiteral("demo-hide-controls"),
        QGuiApplication::translate("main",
            "Development-only: start with the demo controls hidden.")));
#endif
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

    AppController controller(pf.backend, pf.screenshotDemo);
    // The real WebRTC voice-call engine (webrtcbin), when built + its
    // runtime elements resolve. Deliberately here and not in the
    // AppController constructor: the offscreen test fleet must not
    // gst_init or register a media engine it never asked for.
    controller.enableCallMediaEngine();

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Development screenshot mode: enrich the mock scene and auto-login into the
    // deterministic demo account so the app opens straight into the real chat
    // UI (no login form, no network). beginScreenshotDemo() is a no-op unless
    // the mock backend is active.
    if (pf.screenshotDemo) {
        // Restore the launch scenario's own account directly (a cross-account
        // scenario then lands instantly, with no visible switch), falling back
        // to the explicit --demo-account, then the default.
        QString initialAccount = pf.demoAccount;
        if (!pf.demoScenario.isEmpty()) {
            const QString scenarioAcct =
                ScreenshotDemoController::scenarioAccount(pf.demoScenario);
            if (!scenarioAcct.isEmpty())
                initialAccount = scenarioAcct;
        }
        controller.beginScreenshotDemo(initialAccount);
        controller.applyDemoLaunchOptions(pf.demoScenario, pf.demoTheme,
                                          pf.demoAppearance, pf.demoSize,
                                          pf.demoHideControls);
        QTextStream(stdout)
            << "matrix-client: screenshot demo mode active — mock backend, "
               "isolated storage, no network.\n";
    }
#endif

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

    // UI language, applied BEFORE the engine loads so the first frame is
    // already translated. English is the source language and installs no
    // catalog at all; a stored "system" resolves against the desktop's
    // ordered preference list each time the app starts, so moving a machine
    // to another locale follows without touching the setting.
    controller.localization()->applyStoredLanguage();

    QQmlApplicationEngine engine;
    // Live language switching. QQmlEngine::retranslate() re-evaluates every
    // binding that reads qsTr(), which covers the whole declarative UI. It
    // does NOT reach strings a C++ model already turned into data, nor a
    // JavaScript variable assigned once — those are listed as the known
    // limitation in docs/localization.md rather than papered over.
    QObject::connect(controller.localization(),
                     &LocalizationManager::retranslateRequested,
                     &engine, [&engine] { engine.retranslate(); });
    engine.rootContext()->setContextProperty("app", &controller);
    // v0.5.9: serve decrypted media images from the in-memory bridge cache.
    // The engine takes ownership of the provider; the bridge outlives it.
    engine.addImageProvider(QStringLiteral("lightning-media"),
                            new MediaImageProvider(controller.mediaBridge()));
    // Show-QR verification. Memory-only and single-slot; the store lives on
    // the AppController (declared before the engine, so it outlives it) and
    // is cleared whenever the flow ends. Never the QR payload — only the
    // module grid reaches this side at all.
    engine.addImageProvider(QStringLiteral("lightning-qr"),
                            new QrImageProvider(controller.qrCodeStore()));
    // Storm Band: the About page's procedurally generated pixel-art storm
    // landscape. Stateless — colors and layer identity travel in the image
    // id, so a plain parameterless provider suffices.
    engine.addImageProvider(QStringLiteral("storm-band"),
                            new StormBandImageProvider());
    // Composer chips for images that are queued but not sent. A clipboard
    // paste never becomes a file, so there is no file:// URL to point an
    // Image at; the bytes live on the controller and are served from here.
    engine.addImageProvider(QStringLiteral("lightning-staged"),
                            new StagedImageProvider(controller.stagedImages()));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Development-only: once the demo window is up and the scene has settled
    // (media fetched, layout done), grab it to a PNG and quit. Headless
    // screenshot regeneration for the visual-review workflow. Never in a
    // release binary (the whole block is compiled out).
    if (pf.screenshotDemo && !pf.demoCapture.isEmpty()) {
        const QString capturePath = pf.demoCapture;
        const int captureDelay = pf.demoCaptureDelayMs;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
            [capturePath, captureDelay](QObject *obj, const QUrl &) {
                auto *win = qobject_cast<QQuickWindow *>(obj);
                if (!win)
                    return;
                QTimer::singleShot(captureDelay, win, [win, capturePath] {
                    const QImage img = win->grabWindow();
                    if (!img.isNull() && img.save(capturePath))
                        QTextStream(stdout) << "demo-capture: wrote "
                                            << capturePath << "\n";
                    else
                        QTextStream(stderr) << "demo-capture: FAILED "
                                            << capturePath << "\n";
                    QCoreApplication::quit();
                });
            });
    }
#endif

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
