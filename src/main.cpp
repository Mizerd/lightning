#include "app/AppController.h"
#include "app/DesktopEntryQuoting.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include "calls/ShareSourceImageProvider.h"
#include "calls/GstCallMediaBackend.h"
#include "calls/SfuMediaEngine.h"
#endif
#include "i18n/LocalizationManager.h"
#include "app/BackendSelection.h"
#include "app/FontManager.h"
#include "app/StartupChecks.h"
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include "app/ScreenshotDemoController.h"  // demo CLI validation (dev builds only)
#endif
#include "crypto/QrImageProvider.h"
#include "gif/GifBuildKeys.h"
#include "gif/GifProviderSelfTest.h"
#include "app/StormBandPainter.h"
#include "media/ImageFormatSupport.h"
#include "media/MediaImageProvider.h"
#include "media/StagedImageProvider.h"
#include "app/GuiStallTracer.h"
#include "media/VaapiLogGate.h"
#include "text/SpellChecker.h"
#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "smoke/RustSdkSmokeTest.h"
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QFileInfo>
#include <QFontDatabase>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
// Unconditional: the software-renderer fallback below needs all four in
// every build, not only the screenshot-demo one.
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include <QImage>
#include <QTimer>
#endif
#include <QQmlContext>
#include <QQuickStyle>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QDateTime>
#include <QFile>
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

// --log-file: mirror the diagnostic log to a file as well as the console.
//
// WHY A FILE OPTION EXISTS AT ALL. On Windows the app is a GUI-subsystem
// binary that reopens stdout onto the console (`freopen("CONOUT$")`), so a
// shell redirect captures NOTHING — the one thing a person debugging a
// packaged build reaches for first. Getting a log out of an installed build
// meant selecting text in a console window.
//
// SAME STREAM, SAME RULES. This is a mirror of what already goes to stdout,
// so it carries exactly what the console does and nothing more: no tokens, no
// passwords, no recovery keys, no message bodies — §6 governs what may be
// logged and this changes none of it. It APPENDS, so two runs are both kept,
// and a path that cannot be opened is reported once rather than silently
// dropping the option on the floor.
namespace {
QFile *g_logFile = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void logFileHandler(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    if (!g_logFile)
        return;
    const char *level = "info";
    switch (type) {
    case QtDebugMsg:    level = "debug"; break;
    case QtInfoMsg:     level = "info"; break;
    case QtWarningMsg:  level = "warning"; break;
    case QtCriticalMsg: level = "critical"; break;
    case QtFatalMsg:    level = "fatal"; break;
    }
    QTextStream(g_logFile)
        << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ' '
        << level << ' '
        << (context.category ? context.category : "default") << ": "
        << message << '\n';
    g_logFile->flush();   // a crash must not lose the lines that explain it
}
} // namespace

void installLogFile(const QString &path)
{
    if (path.isEmpty())
        return;
    // A symlink would redirect the append at whatever it points to at the
    // moment of opening; the one file this flag exists to produce is one the
    // user is going to hand to someone, so it is also created owner-only.
    if (QFileInfo(path).isSymLink()) {
        QTextStream(stderr)
            << "refusing --log-file: the path is a symbolic link\n";
        return;
    }
    auto *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append
                    | QIODevice::Text)) {
        QTextStream(stderr)
            << "could not open --log-file for writing: " << path << '\n';
        delete file;
        return;
    }
    file->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    // Say what is in it, because every category logs here at debug level
    // and local logs deliberately carry account slugs and store paths.
    QTextStream(file)
        << "# Lightning debug log. Contains Matrix user ids and local file "
           "paths; never message content, keys or tokens. Review before "
           "sharing.\n";
    file->flush();
    g_logFile = file;
    g_previousHandler = qInstallMessageHandler(logFileHandler);
}

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
        RunCallMediaStatus, // --call-media-status: probe the media engines
        RunImageFormatStatus, // --image-format-status: probe the image decoders
        RunSpellStatus, // --spell-status: probe the platform spell checker
        RunDesktopStatus, // --desktop-status: launcher entry + icon association
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
    /// --log-file PATH: mirror the diagnostic log to a file.
    QString logFilePath;
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
                "Lightning — native Qt/QML Matrix desktop client (lightning-matrix).\n"
                "\n"
                "Usage: lightning-matrix [options]\n"
                "\n"
                "Options:\n"
                "  -h, --help           Show this help and exit.\n"
                "  -v, --version        Show version and exit.\n"
                "  --build-info         Print non-secret build metadata (version,\n"
                "                       source, target, backends, default backend,\n"
                "                       secret store, artifact kind) and exit.\n"
                "  --log-file PATH      Mirror the diagnostic log to PATH as well as\n"
                "                       the console. Use this to capture a log from a\n"
                "                       packaged build: --console reopens stdout onto\n"
                "                       the console itself, so a shell redirect cannot\n"
                "                       capture it. Appends; never contains tokens,\n"
                "                       passwords, recovery keys or message bodies.\n"
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
                "  --call-media-status  Probe the call media engines exactly as a\n"
                "                       normal launch does and print the result, then\n"
                "                       exit. Exit 0 only when the group-call (SFU)\n"
                "                       engine is available. Names the first missing\n"
                "                       GStreamer element when it is not. No network,\n"
                "                       no GUI, no window.\n"
                "  --image-format-status\n"
                "                       Print which image formats this build can decode\n"
                "                       and whether that covers what Lightning accepts,\n"
                "                       then exit. Exit 0 only when every required\n"
                "                       format has a decoder. Qt image formats are\n"
                "                       dlopen'd plugins, so this is a property of the\n"
                "                       PACKAGE, not of the source. No network, no GUI.\n"
                "  --spell-status       Print whether this build found the platform's own\n"
                "                       spell checker, which dictionary it resolved and a\n"
                "                       one-word check, then exit. Exit 0 only when a\n"
                "                       dictionary answered. The backend is a runtime\n"
                "                       dlopen (Linux) or COM object (Windows), so this is\n"
                "                       a property of the MACHINE, not of the source. No\n"
                "                       network, no GUI.\n"
                "  --desktop-status     Publish this build's launcher entry the way a\n"
                "                       normal launch does, then print whether this\n"
                "                       session can resolve the app id to an entry\n"
                "                       and an icon, and exit. That association is\n"
                "                       the ONLY route to a window icon on Wayland:\n"
                "                       Qt implements no icon protocol there, so\n"
                "                       setWindowIcon() reaches the compositor\n"
                "                       through nothing. Exit 0 only when both\n"
                "                       resolve. No network, no GUI.\n"
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
            // The product identifies itself; packaging validators pin this exact
            // shape ("Lightning <version>") on every platform.
            r.stdoutMsg = QStringLiteral("Lightning %1\n").arg(QLatin1String(APP_VERSION));
            return r;
        }
        if (a == QLatin1String("--build-info")) {
            r.action = PreflightResult::ExitSuccess;
            r.stdoutMsg = buildInfoString();
            return r;
        }
        if (a.startsWith(QLatin1String("--log-file="))) {
            r.logFilePath = a.mid(QLatin1String("--log-file=").size());
            if (r.logFilePath.isEmpty()) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral("--log-file= needs a path\n");
                return r;
            }
            continue;
        }
        if (a == QLatin1String("--log-file")) {
            if (i + 1 >= argc) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral("--log-file needs a path\n");
                return r;
            }
            r.logFilePath = QString::fromLocal8Bit(argv[++i]);
            continue;
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
                "lightning-matrix: --screenshot-demo is a development-only build "
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
                    "lightning-matrix: unknown --demo-scenario '%1'.\n"
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
                    "lightning-matrix: invalid --demo-size '%1' (use WxH within "
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
        if (a == QLatin1String("--call-media-status")) {
            r.action = PreflightResult::RunCallMediaStatus;
            return r;
        }
        if (a == QLatin1String("--image-format-status")) {
            r.action = PreflightResult::RunImageFormatStatus;
            return r;
        }
        if (a == QLatin1String("--spell-status")) {
            r.action = PreflightResult::RunSpellStatus;
            return r;
        }
        if (a == QLatin1String("--desktop-status")) {
            r.action = PreflightResult::RunDesktopStatus;
            return r;
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
                "lightning-matrix --reset-crypto-store\n"
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
                "lightning-matrix: '%1' is not a supported flag. "
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
                    "lightning-matrix: unknown --backend value '%1'; expected one of: mock, http, rust\n"
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
                    "lightning-matrix: --backend requires a value; expected one of: mock, http, rust\n");
                return r;
            }
            const QString value = QString::fromLocal8Bit(argv[++i]);
            bool ok = false;
            AppController::Backend b = backendFromName(value, &ok);
            if (!ok) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "lightning-matrix: unknown --backend value '%1'; expected one of: mock, http, rust\n").arg(value);
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
                "lightning-matrix: --mock conflicts with --backend=%1\n"
                ).arg(backendNameFor(r.backend));
            return r;
        }
        r.backend = AppController::MockBackend;
    }

    if (!AppController::isBackendCompiled(r.backend)) {
        r.action = PreflightResult::ExitError;
        QString msg = QStringLiteral(
            "lightning-matrix: backend '%1' was not compiled into this build.\n"
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
                "lightning-matrix: --rust-sdk-smoke-test requires "
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
            "lightning-matrix: --rust-sdk-smoke-test needs a Rust-enabled "
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

// ── THE WINDOW ICON, AND WHY AN APPIMAGE HAS TO PUBLISH A LAUNCHER ENTRY
//    TO HAVE ONE ─────────────────────────────────────────────────────────
//
// Reported against the AppImage: after updating, the window and taskbar icon
// is a generic placeholder. The evidence, established 2026-09-08:
//
//  * QT'S WAYLAND CLIENT IMPLEMENTS NO ICON PROTOCOL. `xdg_toplevel_icon`
//    appears ZERO times in libQt6WaylandClient (measured on 6.11.0; the
//    AppImage bundles Debian's OLDER 6.8.2, so it cannot have it either). On
//    a native Wayland session QGuiApplication::setWindowIcon() therefore
//    reaches the compositor through NOTHING — it is inert. Under X11 and
//    XWayland the same call sets _NET_WM_ICON and the icon is correct, which
//    is why this was never seen before.
//  * The compositor's only remaining route is the toplevel's app id. Qt takes
//    that from QGuiApplication::desktopFileName() — "lightning" — and the
//    session resolves it by looking for `lightning.desktop` in XDG_DATA_HOME
//    and XDG_DATA_DIRS, then reading its Icon= key.
//  * AN APPIMAGE INSTALLS NOTHING, so that lookup finds nothing. The
//    reporter's log carries the very same lookup failing in a second consumer:
//    `qt.qpa.services: Failed to register with host portal ... Could not
//    register app ID: App info not found for 'lightning'`.
//  * WHY IT APPEARED ON AN UPDATE. AppImages up to 0.9.0 shipped without
//    wayland-shell-integration, so Qt refused its own Wayland plugin and ran
//    under XWayland — where setWindowIcon works. Staging that plugin (the fix
//    for the black screen share, asserted by validate-appimage.sh since) moved
//    the client onto native Wayland, and the icon association went with it.
//    Nothing about the icon payload changed; the protocol under it did.
//
// So an AppImage that wants an icon has to publish a launcher entry, and the
// icons it names, where the session can see them. That is what every
// self-integrating AppImage does and it is the only route Wayland offers.
//
// SCOPED HARD. It runs only when the AppImage runtime's APPIMAGE **and**
// APPDIR are both set and both resolve, so a deb, rpm, flatpak, snap, macOS
// or source run never writes a byte — those install a real launcher entry
// through their own packaging and already work. It never overwrites a
// `lightning.desktop` it did not write (the X-Lightning-Generated marker), and
// LIGHTNING_NO_DESKTOP_INTEGRATION=1 turns it off entirely.
//
// NOT CLAIMED: that the icon appears on the FIRST run of a new AppImage. The
// entry is written while this process starts, and a session that has already
// cached its application index may only pick it up on the next launch.
namespace {

// The ONE name that has to agree in three places or the icon is generic: the
// app id Qt stamps on every Wayland toplevel (setDesktopFileName, below), the
// basename of the launcher entry the compositor looks that id up in, and the
// icon name that entry's Icon= key carries.
constexpr QLatin1String kAppId("lightning");
// X11/XWayland association: Qt's xcb plugin takes the WM_CLASS instance from
// argv[0], i.e. the binary name.
constexpr QLatin1String kWmClass("lightning-matrix");

struct LauncherEntryReport {
    bool appImageRun = false;
    QString appImagePath;
    QString appDir;
    QString payloadEntry;     // launcher entry inside the AppImage payload
    QString payloadIconName;  // its Icon= value
    int payloadIcons = 0;     // icon files in the payload's hicolor tree
    QString userEntry;        // launcher entry under the user's data dir
    int iconsCopied = 0;
    QString outcome = QStringLiteral("not an AppImage run");
};

#if defined(Q_OS_LINUX)
// Everything between here and the publication function is reached only from
// the AppImage path, so it is compiled only where that path exists. Windows
// and macOS are guarded builds this repository cannot run locally (the QtDBus
// lesson); leaving four unused static functions in them is exactly the noise
// that hides a real warning.

// The Exec quoting lives in src/app/DesktopEntryQuoting.{h,cpp} so it can be
// CALLED by a test. main.cpp cannot be linked into one (it defines main), so
// everything here could only ever be guarded by source scans, and a review
// found several of those would pass on broken code.
using lightning::desktop_entry::quoteExecArgument;

QStringList launcherEntrySource(const QString &payloadEntry)
{
    QFile file(payloadEntry);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    // The payload's entry is asserted by validate-appimage.sh, so this is the
    // belt to that braces: an icon is worth more than fidelity to a file that
    // is not there.
    return QStringList{
        QStringLiteral("[Desktop Entry]"),
        QStringLiteral("Type=Application"),
        QStringLiteral("Name=Lightning"),
        QStringLiteral("GenericName=Matrix Client"),
        QStringLiteral("Comment=Native Qt Matrix chat client"),
        QStringLiteral("Terminal=false"),
        QStringLiteral("Categories=Network;Chat;InstantMessaging;"),
        QStringLiteral("StartupNotify=true"),
    };
}

/// The launcher entry to publish: the payload's own entry — so Name, Comment,
/// Categories, Keywords and any translations stay in the one tracked place,
/// data/lightning.desktop — with the keys a single-file bundle gets wrong
/// rewritten.
QString launcherEntryText(const QString &payloadEntry,
                          const QString &appImagePath)
{
    const QStringList source = launcherEntrySource(payloadEntry);

    // Arguments come from the payload's own Exec line, so `--backend=rust`
    // (or whatever a future entry passes) is not duplicated here to drift.
    QString execArguments;
    QStringList kept;
    bool seenHeader = false;
    for (const QString &raw : source) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            // Our keys are appended at the end, so they must land in the FIRST
            // group. A second group (a Desktop Action) ends the copy.
            if (seenHeader)
                break;
            seenHeader = true;
            kept.append(line);
            continue;
        }
        if (line.startsWith(QLatin1String("Exec="))) {
            const QString value = line.mid(5).trimmed();
            const int space = value.indexOf(QLatin1Char(' '));
            if (space > 0)
                execArguments = value.mid(space + 1).trimmed();
            continue;
        }
        if (line.startsWith(QLatin1String("TryExec="))
            || line.startsWith(QLatin1String("Icon="))
            || line.startsWith(QLatin1String("StartupWMClass="))
            || line.startsWith(QLatin1String("X-AppImage-"))
            || line.startsWith(QLatin1String("X-Lightning-")))
            continue;
        if (line.isEmpty())
            continue;
        kept.append(line);
    }
    if (!seenHeader)
        kept.prepend(QStringLiteral("[Desktop Entry]"));

    QString exec = quoteExecArgument(appImagePath);
    // Refused rather than mangled: a path carrying a control character has no
    // honest Exec representation, and writing one anyway would inject a key
    // into the file. An empty entry text is the caller's signal to skip.
    if (exec.isEmpty())
        return {};
    if (!execArguments.isEmpty())
        exec += QLatin1Char(' ') + execArguments;
    kept.append(QStringLiteral("Exec=") + exec);
    // TryExec is what makes the entry disappear from menus once the AppImage
    // is deleted — a single-file bundle has no uninstall step to do it. It is
    // a bare path with no quoting in the spec, so a path containing whitespace
    // would read as "not installed" and HIDE a working entry: omit it there
    // rather than trade a stale menu item for no icon at all.
    if (!appImagePath.contains(QLatin1Char(' '))
        && !appImagePath.contains(QLatin1Char('\t')))
        kept.append(QStringLiteral("TryExec=") + appImagePath);
    kept.append(QStringLiteral("Icon=") + kAppId);
    kept.append(QStringLiteral("StartupWMClass=") + kWmClass);
    kept.append(QStringLiteral("X-AppImage-Version=")
                + QLatin1String(APP_VERSION));
    // The marker that makes this file ours. Without it we cannot tell our own
    // entry from one the user or a distribution wrote, and overwriting theirs
    // would be a data-loss defect wearing an icon fix's clothes.
    kept.append(QStringLiteral("X-Lightning-Generated=true"));
    return kept.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

/// Copy the payload's hicolor icons into the user's own icon theme, so
/// `Icon=lightning` resolves for the compositor, for the launcher and for
/// QIcon::fromTheme alike. Returns the number of files actually written.
/// The manifest of icon files THIS code wrote, one relative path per line.
///
/// It exists because the entry path honours "never overwrite what we did not
/// write" and the icon path did not: replacing
/// `~/.local/share/icons/hicolor/<size>/apps/lightning.png` is how a user
/// deliberately re-icons an application, and we clobbered it on every launch.
/// It also makes the copies reclaimable, so deferring to an installed package
/// can take them away instead of leaving a user-level icon that SHADOWS the
/// package's own for good. Caught in review.
QString userIconManifestPath(const QString &dataHome)
{
    return dataHome
           + QStringLiteral("/icons/hicolor/.lightning-appimage-icons");
}

QStringList readUserIconManifest(const QString &dataHome)
{
    QFile file(userIconManifestPath(dataHome));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

/// Remove every icon we recorded, and the manifest. Used when an installed
/// package's entry wins, so our copies stop shadowing its artwork.
void removeUserIcons(const QString &dataHome)
{
    const QString root = dataHome + QStringLiteral("/icons/hicolor/");
    for (const QString &relative : readUserIconManifest(dataHome)) {
        // Never follow a path out of the icon tree, whatever the file says.
        if (relative.contains(QLatin1String("..")))
            continue;
        QFile::remove(root + relative);
    }
    QFile::remove(userIconManifestPath(dataHome));
}

int installUserIcons(const QString &appDir, const QString &dataHome,
                     int *payloadIcons)
{
    int copied = 0;
    const QStringList recorded = readUserIconManifest(dataHome);
    QStringList written;
    const QDir hicolor(appDir + QStringLiteral("/usr/share/icons/hicolor"));
    if (!hicolor.exists())
        return 0;
    const QStringList sizes =
        hicolor.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &size : sizes) {
        const QDir apps(hicolor.filePath(size + QStringLiteral("/apps")));
        if (!apps.exists())
            continue;
        const QStringList names =
            apps.entryList(QStringList{ kAppId + QStringLiteral(".*") },
                           QDir::Files, QDir::Name);
        for (const QString &name : names) {
            if (payloadIcons)
                ++*payloadIcons;
            const QString from = apps.filePath(name);
            const QString toDir = dataHome + QStringLiteral("/icons/hicolor/")
                + size + QStringLiteral("/apps");
            const QString to = toDir + QLatin1Char('/') + name;
            // Size is the cheap discriminator on a startup path, and this
            // artwork only changes when the icons themselves do. Re-reading
            // nine files on every launch to catch a same-size redraw is not
            // worth it.
            const QString relative =
                size + QStringLiteral("/apps/") + name;
            // A FILE WE DID NOT WRITE IS THE USER'S. Overwriting one is how
            // a deliberate icon override was being undone on every launch.
            if (QFileInfo::exists(to) && !recorded.contains(relative)) {
                written.append(relative);   // keep any earlier record honest
                continue;
            }
            written.append(relative);
            if (QFileInfo(to).size() == QFileInfo(from).size())
                continue;
            if (!QDir().mkpath(toDir))
                continue;
            // Copy beside and rename, so a full disk leaves the old icon in
            // place instead of no icon at all.
            const QString staging = to + QStringLiteral(".new");
            QFile::remove(staging);
            if (!QFile::copy(from, staging))
                continue;
            QFile::remove(to);
            if (QFile::rename(staging, to))
                ++copied;
            else
                QFile::remove(staging);
        }
    }
    if (!written.isEmpty()) {
        QDir().mkpath(dataHome + QStringLiteral("/icons/hicolor"));
        QSaveFile manifest(userIconManifestPath(dataHome));
        if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            manifest.write(written.join(QLatin1Char('\n')).toUtf8());
            manifest.commit();
        }
    }
    return copied;
}

/// The launcher entry an installed PACKAGE published, if any: XDG_DATA_DIRS
/// only, never XDG_DATA_HOME, because the point is to notice somebody else's
/// copy of the same basename before shadowing it.
///
/// THE BUNDLE'S OWN SHARE DIRECTORY DOES NOT COUNT, and forgetting that would
/// turn the whole publication into a no-op: linuxdeploy's AppRun PREPENDS
/// `$APPDIR/usr/share` to XDG_DATA_DIRS (which is why the AppImage hook saves
/// the session's value as XDG_DATA_DIRS_APPIMAGE — see UrlLauncher). Our own
/// payload entry would then look like an installed package to the loop below.
/// It is not one: that directory exists only inside this process's
/// environment, on a mount that disappears when it exits, and the compositor
/// asking "which desktop entry is app id lightning?" has never heard of it.
QString systemLauncherEntry()
{
    QString dirs = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS"));
    if (dirs.isEmpty())
        dirs = QStringLiteral("/usr/local/share:/usr/share");
    const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
    const QStringList entries =
        dirs.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : entries) {
        if (!appDir.isEmpty() && dir.startsWith(appDir))
            continue;
        const QString candidate = dir + QStringLiteral("/applications/")
            + kAppId + QStringLiteral(".desktop");
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return {};
}

#endif // Q_OS_LINUX

/// Publish the launcher entry and the icons it names for an AppImage run; a
/// no-op everywhere else. A handful of small local writes, done once — the
/// second launch finds everything current and writes nothing.
LauncherEntryReport publishAppImageLauncherEntry()
{
    LauncherEntryReport report;
#ifndef Q_OS_LINUX
    report.outcome = QStringLiteral("skipped: not Linux");
    return report;
#else
    report.appImagePath = QString::fromLocal8Bit(qgetenv("APPIMAGE"));
    report.appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
    report.appImageRun = !report.appImagePath.isEmpty()
        && !report.appDir.isEmpty()
        && QFileInfo(report.appImagePath).isFile()
        && QFileInfo(report.appDir).isDir();
    if (!report.appImageRun) {
        report.outcome = QStringLiteral("skipped: not an AppImage run");
        return report;
    }
    report.payloadEntry = report.appDir
        + QStringLiteral("/usr/share/applications/") + kAppId
        + QStringLiteral(".desktop");
    if (!QFileInfo(report.payloadEntry).isFile()) {
        report.payloadEntry.clear();
    } else {
        QFile payload(report.payloadEntry);
        if (payload.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList lines =
                QString::fromUtf8(payload.readAll()).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                if (line.trimmed().startsWith(QLatin1String("Icon="))) {
                    report.payloadIconName = line.trimmed().mid(5);
                    break;
                }
            }
        }
    }

    const QString optOut =
        QString::fromLocal8Bit(qgetenv("LIGHTNING_NO_DESKTOP_INTEGRATION"));
    if (!optOut.isEmpty() && optOut != QLatin1String("0")) {
        report.outcome =
            QStringLiteral("skipped: LIGHTNING_NO_DESKTOP_INTEGRATION is set");
        return report;
    }

    const QString dataHome = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    if (dataHome.isEmpty()) {
        report.outcome = QStringLiteral("failed: no writable data location");
        return report;
    }
    report.userEntry = dataHome + QStringLiteral("/applications/") + kAppId
        + QStringLiteral(".desktop");

    QByteArray existing;
    {
        QFile current(report.userEntry);
        if (current.open(QIODevice::ReadOnly))
            existing = current.readAll();
    }
    if (!existing.isEmpty()
        && !existing.contains("X-Lightning-Generated=true")) {
        report.outcome = QStringLiteral(
            "skipped: an existing launcher entry was not written by Lightning");
        return report;
    }

    // DEFER TO AN INSTALLED PACKAGE, and remove our own copy if one is there.
    //
    // A deb, rpm, flatpak or snap installs `lightning.desktop` into a system
    // data directory, and a file of the same basename under XDG_DATA_HOME
    // SHADOWS it. Publishing ours over that would repoint the shared name at
    // this AppImage, and the TryExec below would then HIDE the entry the day
    // the AppImage file is deleted — taking the installed package's launcher
    // with it. There is nothing to gain by it either: the system entry
    // already carries Icon=lightning and the package already installed the
    // hicolor icons, which is the whole thing this publication exists to
    // achieve.
    if (const QString installed = systemLauncherEntry(); !installed.isEmpty()) {
        // Take our icons back too. A user-level hicolor icon shadows the
        // package's, so leaving ours behind would freeze the artwork at
        // whatever this AppImage shipped, for good, even after the AppImage
        // is deleted. Only files we recorded are removed.
        removeUserIcons(dataHome);
        if (!existing.isEmpty() && QFile::remove(report.userEntry))
            report.outcome = QStringLiteral(
                "skipped: %1 is installed; removed our own shadowing copy")
                                 .arg(installed);
        else
            report.outcome =
                QStringLiteral("skipped: %1 is installed").arg(installed);
        return report;
    }

    // BUILD THE ENTRY BEFORE TOUCHING THE FILESYSTEM. The text can be
    // refused (a path with no honest Exec representation), and installing
    // icons for an entry that is never written would leave artwork behind
    // with nothing pointing at it.
    const QByteArray wanted =
        launcherEntryText(report.payloadEntry, report.appImagePath).toUtf8();
    if (wanted.isEmpty()) {
        report.outcome = QStringLiteral(
            "skipped: the AppImage path cannot be represented in Exec=");
        return report;
    }

    report.iconsCopied =
        installUserIcons(report.appDir, dataHome, &report.payloadIcons);

    if (existing == wanted) {
        report.outcome = QStringLiteral("already current");
        return report;
    }
    if (!QDir().mkpath(dataHome + QStringLiteral("/applications"))) {
        report.outcome = QStringLiteral("failed: cannot create %1/applications")
                             .arg(dataHome);
        return report;
    }
    QSaveFile out(report.userEntry);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || out.write(wanted) != wanted.size() || !out.commit()) {
        report.outcome =
            QStringLiteral("failed: could not write the launcher entry");
        return report;
    }
    report.outcome = QStringLiteral("written");
    return report;
#endif
}

/// Every directory a session searches for launcher entries and icons:
/// $XDG_DATA_HOME (or ~/.local/share) first, then $XDG_DATA_DIRS. This is the
/// lookup the compositor and xdg-desktop-portal perform on the app id, so what
/// it finds IS the diagnosis of a generic window icon.
///
/// $APPDIR is excluded for the reason systemLauncherEntry() gives: AppRun puts
/// the bundle's own share directory on XDG_DATA_DIRS, and counting it here
/// would make --desktop-status report the reported defect as fixed on every
/// AppImage ever built.
QStringList xdgDataDirs()
{
    QStringList dirs;
    const QString home = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    if (!home.isEmpty())
        dirs << home;
    const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
    QString system = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS"));
    if (system.isEmpty())
        system = QStringLiteral("/usr/local/share:/usr/share");
    const QStringList entries =
        system.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        if (!appDir.isEmpty() && entry.startsWith(appDir))
            continue;
        if (!dirs.contains(entry))
            dirs << entry;
    }
    return dirs;
}

} // namespace

/// `--desktop-status`: ask the RUNNING BUILD whether this session can resolve
/// its app id to a launcher entry and an icon — the association that decides
/// the window and taskbar icon on Wayland, where setWindowIcon() reaches the
/// compositor through nothing at all (see the block above).
///
/// Same shape and same reason as --call-media-status and
/// --image-format-status: no file listing can answer it. The AppImage's
/// payload has always carried a perfectly good desktop entry and a full
/// hicolor icon set; what it never had was a copy of them anywhere the SESSION
/// looks, and every check in the pipeline passed while the icon was generic.
///
/// It performs the same publication a normal launch does, deliberately: a
/// probe that skips the write cannot prove the write works.
static int printDesktopStatus()
{
    QTextStream out(stdout);

    QGuiApplication::setDesktopFileName(kAppId);
    out << "qt version: " << QLatin1String(qVersion()) << "\n";
    out << "app id (desktop file name): "
        << QGuiApplication::desktopFileName() << "\n";
    out << "launcher entry basename: " << kAppId << ".desktop\n";
    out << "wm class: " << kWmClass << "\n";

    const LauncherEntryReport report = publishAppImageLauncherEntry();
    out << "appimage runtime: " << (report.appImageRun ? "yes" : "no") << "\n";
    if (report.appImageRun) {
        out << "appimage: " << report.appImagePath << "\n";
        out << "appdir: " << report.appDir << "\n";
        out << "payload launcher entry: "
            << (report.payloadEntry.isEmpty() ? QStringLiteral("MISSING")
                                              : report.payloadEntry)
            << "\n";
        out << "payload entry icon name: "
            << (report.payloadIconName.isEmpty() ? QStringLiteral("MISSING")
                                                 : report.payloadIconName)
            << "\n";
        out << "payload icon files: " << report.payloadIcons << "\n";
        out << "user icon files copied: " << report.iconsCopied << "\n";
        out << "user launcher entry: " << report.userEntry << "\n";
    }
    out << "launcher entry: " << report.outcome << "\n";

    // What the session itself would find, searched exactly as it searches.
    QString visibleEntry;
    QString visibleIcon;
    const QStringList dirs = xdgDataDirs();
    for (const QString &dir : dirs) {
        const QString candidate = dir + QStringLiteral("/applications/")
            + kAppId + QStringLiteral(".desktop");
        if (visibleEntry.isEmpty() && QFileInfo(candidate).isFile())
            visibleEntry = candidate;
        const QDir hicolor(dir + QStringLiteral("/icons/hicolor"));
        if (visibleIcon.isEmpty() && hicolor.exists()) {
            const QStringList sizes = hicolor.entryList(
                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &size : sizes) {
                const QDir apps(
                    hicolor.filePath(size + QStringLiteral("/apps")));
                const QStringList hits = apps.entryList(
                    QStringList{ kAppId + QStringLiteral(".*") }, QDir::Files);
                if (!hits.isEmpty()) {
                    visibleIcon = apps.filePath(hits.first());
                    break;
                }
            }
        }
    }
    out << "data dirs searched: " << dirs.size() << " ("
        << dirs.join(QLatin1Char(' ')) << ")\n";
    out << "visible launcher entry: "
        << (visibleEntry.isEmpty() ? QStringLiteral("NONE") : visibleEntry)
        << "\n";
    out << "visible icon: "
        << (visibleIcon.isEmpty() ? QStringLiteral("NONE") : visibleIcon)
        << "\n";

    if (visibleEntry.isEmpty() || visibleIcon.isEmpty()) {
        out << "\nRESULT: this session cannot resolve the app id \"" << kAppId
            << "\" to a launcher entry and an icon, so the window and taskbar "
               "icon is a generic placeholder on Wayland. Expected for a source "
               "build, which installs neither; a packaging defect for any "
               "package.\n";
        return 1;
    }
    out << "\nRESULT: the app id \"" << kAppId
        << "\" resolves to a launcher entry and an icon this session can "
           "find.\n";
    return 0;
}

/// `--image-format-status`: ask the RUNNING BUILD which image formats it can
/// decode, and say plainly whether that covers what Lightning accepts.
///
/// The same shape, and the same reason, as `--call-media-status`. A Qt image
/// format is a dlopen'd plugin, so what a build decodes is decided by
/// packaging: every Linux package up to 0.8.0 shipped exactly libqgif, libqico
/// and libqjpeg while the client's own byte sniffers accepted image/webp — it
/// accepted, forwarded and re-uploaded a format it could not draw, and no
/// check anywhere looked. A file listing in the build job cannot answer this
/// (a plugin present is not a plugin that loads, the sctp lesson), and the dev
/// shell cannot either: it reports 92 formats because the maintainer's system
/// profile carries kimageformats, not because this repository ships it.
///
/// Exit 0 only when every REQUIRED format decodes. Optional formats — JPEG XL
/// today — are reported and never fail the check, because they are genuinely
/// unavailable on Windows and macOS (no Qt JXL plugin exists, and neither
/// Fedora's mingw64 repository nor Homebrew packages KDE's kimageformats).
static int printImageFormatStatus()
{
    namespace ifmt = lightning::imagefmt;
    QTextStream out(stdout);

    // The paths first: when the answer is "the plugins are not where I look",
    // these are the lines that say so.
    out << "qt version: " << QLatin1String(qVersion()) << "\n";
    // EVERY search path, not just the compiled-in prefix. Printing
    // QLibraryInfo::PluginsPath alone is actively misleading, and it misled me
    // while writing this: in the dev shell that single path holds exactly
    // libqgif/libqico/libqjpeg — the shipped AppImage's three — beside a
    // "decodable formats (92)" line, because the other 89 arrive from OTHER
    // entries in libraryPaths(). A transcript that names one directory and
    // reports decoders from another cannot be used to diagnose the very defect
    // this flag exists for. Qt searches all of these, so all of them are
    // printed, in order.
    out << "plugin path (compiled-in): "
        << QLibraryInfo::path(QLibraryInfo::PluginsPath) << "\n";
    // Only the search paths that actually CONTAIN an imageformats directory,
    // each with the plugin files in it. The full libraryPaths() list is the
    // honest input but not a readable answer — in the nix dev shell it is 115
    // entries, one per package in the shell, because a packaged artifact has
    // two or three and a developer shell has one of everything. What a reader
    // needs is which directories supplied the decoders, so that is what is
    // printed, together with the total so nothing looks hidden.
    const QStringList libraryPaths = QCoreApplication::libraryPaths();
    int dirsWithPlugins = 0;
    QString pluginLines;
    {
        QTextStream ps(&pluginLines);
        // Deduplicated: libraryPaths() legitimately repeats an entry (the
        // compiled-in prefix is both the default and, here, the head of
        // QT_PLUGIN_PATH), and the same directory listed twice reads as a bug
        // in the report rather than as a fact about Qt.
        QSet<QString> seen;
        for (const QString &path : libraryPaths) {
            QDir dir(path + QStringLiteral("/imageformats"));
            if (!dir.exists())
                continue;
            const QString canonical = dir.canonicalPath();
            if (seen.contains(canonical))
                continue;
            seen.insert(canonical);
            const QStringList files =
                dir.entryList(QDir::Files, QDir::Name);
            if (files.isEmpty())
                continue;
            ++dirsWithPlugins;
            ps << "  " << dir.path() << " (" << files.size() << "): "
               << files.join(QLatin1Char(' ')) << "\n";
        }
    }
    out << "plugin search paths: " << libraryPaths.size() << " searched, "
        << dirsWithPlugins << " containing image-format plugins\n";
    out << pluginLines;

    QStringList decodable(ifmt::decodableQtFormats().begin(),
                          ifmt::decodableQtFormats().end());
    decodable.sort();
    out << "decodable formats (" << decodable.size() << "): "
        << decodable.join(QLatin1Char(' ')) << "\n";

    int n = 0;
    const ifmt::RasterFormat *table = ifmt::rasterFormats(&n);
    for (int i = 0; i < n; ++i) {
        const bool ok = ifmt::canDecode(QString::fromLatin1(table[i].qtFormat));
        out << (table[i].required ? "required " : "optional ")
            << QLatin1String(table[i].mime) << ": "
            << (ok ? QStringLiteral("decodable")
                   : QStringLiteral("NOT DECODABLE (no plugin in this build)"))
            << "\n";
    }

    const QStringList missingRequired = ifmt::undecodable(/*requiredOnly=*/true);
    const QStringList missingAny = ifmt::undecodable(/*requiredOnly=*/false);
    if (!missingRequired.isEmpty()) {
        out << "\nRESULT: this build ACCEPTS image formats it cannot decode: "
            << missingRequired.join(QStringLiteral(", ")) << "\n";
        return 1;
    }
    if (!missingAny.isEmpty()) {
        out << "\nRESULT: every required format decodes; not available on this "
               "platform: "
            << missingAny.join(QStringLiteral(", ")) << "\n";
        return 0;
    }
    out << "\nRESULT: every image format Lightning accepts can be decoded by "
           "this build.\n";
    return 0;
}

#ifdef HAVE_LIGHTNING_WEBRTC
/// `--call-media-status`: probe both media engines the way a real launch does.
///
/// Lives in main.cpp, NOT in GstBootstrap, because it names BOTH engines and
/// the bootstrap is linked by test targets that carry only one of them —
/// putting it there made call-media-loopback-test and sfu-media-engine-test
/// fail to link against an engine they deliberately do not build.
///
/// It exists because none of this was answerable from a package: the build log
/// said the engine was compiled in, the packaging said 25 plugins were
/// bundled, and calls were still refused — the plugin path was applied AFTER
/// gst_init had already run, and nothing shipped could say so.
static int printCallMediaStatus()
{
    QTextStream out(stdout);
    out << "call media engine built in: yes\n";

    QString whyNot;
    const bool inited = lightning::gst::ensureInitialised(&whyNot);
    const QString bundled = lightning::gst::bundledPluginPath();
    // The PATH, not its contents: it is the app's own install directory and
    // the single most useful line when the answer is "the plugins are not
    // where I look".
    out << "bundled plugin directory: "
        << (bundled.isEmpty()
                ? QStringLiteral("<none - using system GStreamer>")
                : bundled)
        << "\n";
    if (!inited) {
        out << "gstreamer: FAILED (" << whyNot << ")\n"
            << "\nRESULT: calls will be refused by this build.\n";
        return 1;
    }
    // THE VERSION, not just "it started". The receive path depends on what
    // webrtcbin fills in on a src pad, and that has moved between releases:
    // the dev shell is 1.26.x while the packaged Windows runtime is 1.28.x
    // and the macOS bundle 1.28.x again. A defect that reproduces on a
    // tester's machine and nowhere here begins with knowing which runtime
    // they actually loaded, and asking for it afterwards costs a round trip.
    out << "gstreamer: initialised, " << lightning::gst::versionString()
        << "\n";

    // BOTH engines, through the SAME functions AppController probes, in the
    // same order. A status command with its own check could pass while the
    // application refused.
    QString oneToOneWhy;
    const bool oneToOne = GstCallMediaBackend::runtimeAvailable(&oneToOneWhy);
    out << "1:1 call engine: "
        << (oneToOne ? QStringLiteral("available")
                     : QStringLiteral("unavailable (%1)").arg(oneToOneWhy))
        << "\n";

    QString sfuWhy;
    const bool sfu = SfuMediaEngine::runtimeAvailable(&sfuWhy);
    out << "group call (SFU) engine: "
        << (sfu ? QStringLiteral("available")
                : QStringLiteral("unavailable (%1)").arg(sfuWhy))
        << "\n";

    // The SFU engine alone decides the exit code: it is the one every
    // MatrixRTC call runs through, and the 1:1 lane's button is disabled in
    // the UI regardless.
    out << "\nRESULT: "
        << (sfu ? QStringLiteral("calls can be placed and answered.")
                : QStringLiteral("calls will be refused by this build."))
        << "\n";
    return sfu ? 0 : 1;
}
#endif

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
    // NOT inside the Windows guard. Windows is why the option exists, but a
    // packaged macOS bundle launched from Finder has no terminal either, and
    // asking a tester to reproduce a call bug through Console.app is the same
    // problem wearing a different name.
    installLogFile(pf.logFilePath);

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
    if (pf.action == PreflightResult::RunCallMediaStatus) {
        // THE ONE COMMAND THAT ANSWERS "why can I not call from this build".
        //
        // A packaged Windows or macOS build reaches this through exactly the
        // path a normal launch does — the shared GStreamer bootstrap, the
        // bundled plugin directory beside the executable, and both engines'
        // own runtimeAvailable() probes. So it can distinguish, without a
        // GUI and without a homeserver, between "the engine was never built
        // into this binary", "the plugins are not where the app looks" and
        // "one specific element is missing".
        //
        // It exists because none of that was answerable from a package: the
        // build log said the engine was compiled in, the packaging said 25
        // plugins were bundled, and calls still refused — the plugin path was
        // applied AFTER gst_init had already run. Nothing shipped could say
        // so. A QCoreApplication is needed because the bootstrap resolves the
        // plugin directory from applicationDirPath().
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
#ifdef HAVE_LIGHTNING_WEBRTC
        QCoreApplication probeApp(argc, argv);
        return printCallMediaStatus();
#else
        // The honest answer for a build configured without the engine: the
        // CMake pkg-config probe found no GStreamer, so there is nothing to
        // ask. This is what EVERY packaged Windows and macOS build printed
        // before 2026-08-26.
        QTextStream(stdout)
            << "call media engine built in: no\n"
            << "\nRESULT: calls will be refused by this build "
               "(configured without GStreamer).\n";
        return 1;
#endif
    }
    if (pf.action == PreflightResult::RunImageFormatStatus) {
        // A QCoreApplication is enough and is what makes this askable of EVERY
        // packaged artifact. QImageReader resolves its plugins through
        // QFactoryLoader over QCoreApplication::libraryPaths() — no QPA
        // platform plugin, no display, no window. Measured: a bare
        // QCoreApplication probe lists all 92 formats in the dev shell,
        // kimg_jxl.so included.
        //
        // Deliberately NOT a QGuiApplication forced to offscreen: the Windows
        // package stages only qwindows.dll, so forcing offscreen there would
        // qFatal on a platform plugin that is not in the bundle — turning the
        // one command that reports packaging into a packaging-dependent
        // command.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication probeApp(argc, argv);
        return printImageFormatStatus();
    }
    if (pf.action == PreflightResult::RunSpellStatus) {
        // THE CHECK THAT ASKS THE SHIPPED ARTIFACT WHETHER THE FEATURE WORKS.
        //
        // Nothing about spell checking is decided at build time on any
        // platform: Linux resolves libenchant-2 with dlopen and Windows
        // creates a COM object, and either can be absent on a machine whose
        // package is otherwise perfect. That is the exact shape of defect
        // this project has paid for twice (a Windows/macOS package with no
        // media engine; an AppImage whose staged plugins could not dlopen),
        // and the lesson recorded both times is that the check has to run
        // the artifact and ask.
        //
        // A QCoreApplication is enough: no QPA plugin, no display, no window.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication spellProbeApp(argc, argv);
        SpellChecker checker;
        checker.initialize();
        QTextStream out(stdout);
        out << "spell checker available: "
            << (checker.available() ? "yes" : "no") << "\n";
        if (!checker.available()) {
            out << "\nRESULT: this machine has no dictionary Lightning can "
                   "reach, so the composer will not underline anything.\n";
            return 1;
        }
        out << "backend: " << checker.backendName() << "\n"
            << "dictionary: " << checker.language() << "\n";
        // A deliberately misspelled word, so the output distinguishes "a
        // dictionary loaded" from "a dictionary loaded and answers". A
        // backend that says every word is correct is indistinguishable from
        // no backend at all in the composer.
        const QVariantList wrong =
            checker.misspelledRanges(QStringLiteral("teh"));
        out << "sample check (\"teh\"): "
            << (wrong.isEmpty() ? "accepted (suspicious)" : "rejected")
            << "\n";
        if (wrong.isEmpty()) {
            out << "\nRESULT: a dictionary loaded but accepted a misspelling; "
                   "treat spell checking as not working.\n";
            return 1;
        }
        // The suggestion call is exercised too, deliberately. It is the one
        // that hands memory back across the boundary — a string list the
        // backend then frees, or a COM enumerator it releases — so a wrong
        // signature shows up here rather than under a user's right-click.
        const QStringList ideas = checker.suggestions(QStringLiteral("teh"));
        out << "sample suggestions: " << ideas.size() << "\n";
        out << "\nRESULT: spell checking works in this build on this "
               "machine.\n";
        return 0;
    }
    if (pf.action == PreflightResult::RunDesktopStatus) {
        // A QCoreApplication is enough and is what makes this askable of
        // EVERY packaged artifact: the lookup is XDG_DATA_HOME and
        // XDG_DATA_DIRS on disk, not a windowing-system round trip. No QPA
        // platform plugin, no display, no window — the Windows package
        // stages only qwindows.dll, and forcing offscreen would turn the
        // one command that reports packaging into a packaging-dependent
        // command.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication desktopProbeApp(argc, argv);
        return printDesktopStatus();
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
                "lightning-matrix: no graphical display available "
                "(DISPLAY / WAYLAND_DISPLAY unset).\n"
                "Run this app inside a graphical session, or export "
                "QT_QPA_PLATFORM=offscreen for a headless smoke test.\n"
                "See docs/build-and-test.md for the exact commands.\n";
            return 3;
        }
    }

    // The PERSISTENT application identity. These literals name the settings
    // file, the QStandardPaths data/cache roots, the credential-store
    // namespace and the Rust SDK store roots of every existing install; they
    // deliberately did NOT follow the binary rename to `lightning-matrix`
    // (renaming them would sign every user out of a session they still
    // hold). The product name shown to people is "Lightning".
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
    }

    // Scratch directories from a run that crashed before its QTemporaryDir
    // destructor executed. Ours only, by name prefix, our own uid, never a
    // symlink -- and on EVERY install type: they hold decrypted
    // encrypted-room media, and this used to run only for a portable copy,
    // so every other install left them in /tmp for good after a crash.
    {
        const int swept = lightning::portable::cleanStaleTempDirs();
        if (swept > 0) {
            QTextStream(stderr)
                << "removed " << swept
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

    // QApplication, not QGuiApplication, since 0.9.1. QSystemTrayIcon on
    // X11 has two implementations: a StatusNotifierItem over D-Bus when a
    // watcher is on the session bus, and the legacy XEmbed tray otherwise —
    // and the legacy one is a QWidget. Enabling the tray on a desktop whose
    // bar speaks XEmbed only (i3bar, polybar, tint2 …) therefore aborted a
    // QGuiApplication process with "QWidget: Cannot create a QWidget without
    // QApplication" (reported from NixOS on 0.9.0). Nothing else here is a
    // widget; QtWidgets was already linked for the tray, and a QML
    // application under QApplication renders exactly as before.
    QApplication app(argc, argv);
    // Opt-in GUI-thread stall tracing (LIGHTNING_GUI_STALL_TRACE): a
    // heartbeat + watchdog that logs any event-loop stall over the
    // threshold with a coarse category. Duration and category literal
    // only — see GuiStallTracer.h.
    stalltrace::install();

    // ── A missing GL stack must degrade, not refuse to start ─────────────
    //
    // Qt Quick's default RHI backend is OpenGL, and when no context can be
    // created Qt prints "Failed to create RHI (backend 2)" and the process
    // exits before a window ever appears. The 0.9.1 AppImage did exactly
    // that on a Wayland session (2026-09-06): the bundle carries the Qt
    // Wayland platform plugin and its EGL hardware integration but, like
    // every AppImage, no libEGL of its own — that has to come from the host,
    // and under `appimage-run` on this NixOS box it could not be reached, so
    // `EGL not available` was followed by a dead process. 0.9.0 had survived
    // the same machine only by accident: its AppImage was missing the
    // xdg-shell plugin, so Qt refused the Wayland platform entirely and fell
    // back to XWayland, where GLX worked. Fixing the plugin removed the
    // accident and left the client unable to start at all.
    //
    // So probe once, here, before any QQuickWindow exists, and fall back to
    // the software renderer rather than not starting. It is a real
    // degradation — the scene is rasterised on the CPU, which matters for
    // video and screen sharing — hence the warning, and hence it is a LAST
    // resort that never overrides an explicit choice by the user or by a
    // test harness.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")
        && qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")
        && QGuiApplication::platformName() != QLatin1String("offscreen")
        && QGuiApplication::platformName() != QLatin1String("minimal")) {
        bool glUsable = false;
        {
            QOpenGLContext probe;
            QOffscreenSurface surface;
            surface.setFormat(probe.format());
            surface.create();
            if (surface.isValid() && probe.create()
                && probe.makeCurrent(&surface)) {
                glUsable = true;
                probe.doneCurrent();
            }
        }
        if (!glUsable) {
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
            qWarning("lightning: no usable OpenGL context on the \"%s\" "
                     "platform - falling back to the software renderer. "
                     "Video and screen sharing will be slower. Set "
                     "QT_QUICK_BACKEND to override.",
                     qUtf8Printable(QGuiApplication::platformName()));
        }
    }
    // Qt has read the scale factor now; drop it from the environment so it
    // does not leak into child processes (the OAuth system browser,
    // xdg-open) and zoom THEIR UI too (review L2). A user-set env var is
    // deliberately left alone.
    if (zoomEnvSetHere)
        qunsetenv("QT_SCALE_FACTOR");
    // Wayland compositors match the window to its launcher entry through
    // the desktop-file name (app_id "lightning" ↔ lightning.desktop); X11
    // matches WM_CLASS (the binary name, "lightning-matrix") through
    // StartupWMClass.
    QGuiApplication::setDesktopFileName(kAppId);
    // ...and, for an AppImage, put a launcher entry carrying that id
    // where the session can find it. Without one there is no window icon
    // on Wayland at all: Qt has no icon protocol there, so the compositor
    // resolves the app id against installed desktop entries or shows a
    // generic placeholder. A no-op for every other install type.
    {
        const LauncherEntryReport entry = publishAppImageLauncherEntry();
        if (entry.appImageRun)
            qInfo("lightning: AppImage launcher entry: %s (%d icon file(s) copied)",
                  qUtf8Printable(entry.outcome), entry.iconsCopied);
    }

    // ONE LINE IN THE LOG when this build accepts an image format it cannot
    // draw. Warn, not debug: the failure it names is otherwise a blank box in
    // a timeline with no diagnostic anywhere, which is how every Linux package
    // up to 0.8.0 shipped without a WebP decoder. `--image-format-status`
    // prints the same finding without starting the UI. Names format strings
    // only — never a file, a room or a payload.
    {
        const QStringList missing = lightning::imagefmt::undecodable(true);
        if (!missing.isEmpty())
            qWarning("image decoders missing from this build: %s — these "
                     "formats are accepted by Lightning and will not render. "
                     "Run lightning-matrix --image-format-status for detail.",
                     qUtf8Printable(missing.join(QStringLiteral(", "))));
    }
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
    // The emoji face rides along as the second family: a tooltip or a
    // native menu that never names a face then draws colour emoji too. See
    // FontManager::emojiFamily().
    QGuiApplication::setFont(
        FontManager::withEmojiFallback(QStringLiteral("Manrope"), 14));

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
    // Preflight already CONSUMED these two, and unlike every other preflight
    // flag they do not exit — the app goes on to run. So they must be
    // registered here or `process()` rejects them as unknown and quits.
    //
    // `--console` shipped broken for exactly this reason: it was reported as
    // "matrix-client: Unknown option 'console'." from an installed build, on
    // the one flag whose entire job is getting a log out of an installed
    // build. It has never worked in any build.
    //
    // `parseTimeFlagsSurviveIntoTheQtParser` pins it, so a third flag of this
    // shape fails at build time rather than in a tester's hands.
    QCommandLineOption consoleOpt(
        QStringLiteral("console"),
        QGuiApplication::translate("main",
            "Windows: open a visible diagnostic console."));
    parser.addOption(consoleOpt);
    QCommandLineOption logFileOpt(
        QStringLiteral("log-file"),
        QGuiApplication::translate("main",
            "Mirror the diagnostic log to this file as well as the console."),
        QStringLiteral("path"));
    parser.addOption(logFileOpt);
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
            << "lightning-matrix: screenshot demo mode active — mock backend, "
               "isolated storage, no network.\n";
    }
#endif

    // Fonts. The user may pick any family the host has and may import a font
    // file by hand; FontManager owns both, and owns the resolution of a
    // stored family against what this machine actually has.
    //
    // ORDER MATTERS HERE. loadImportedFonts() registers the user's imported
    // faces with QFontDatabase, and it must run BEFORE the window font is
    // applied — otherwise a UI font that IS an imported family resolves as
    // missing on the very launch that stored it, and the first frame draws
    // the fallback.
    FontManager fontManager(controller.settings());
    fontManager.loadImportedFonts();

    // The selected UI font applies before the first frame and follows the
    // setting live. It is FontManager's RESOLVED family: a stored family the
    // host no longer has renders as the bundled face, and the stored value is
    // left alone so re-installing the font brings it back. Mono is pushed
    // into AppTheme from Main.qml; icon and emoji roles are never affected.
    const auto applyUiFont = [](const QString &family) {
        QGuiApplication::setFont(FontManager::withEmojiFallback(family, 14));
    };
    applyUiFont(fontManager.uiFamily());
    QObject::connect(&fontManager, &FontManager::selectionChanged,
                     &app, [&fontManager, applyUiFont] {
                         applyUiFont(fontManager.uiFamily());
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
    // Fonts are their own context property rather than a member of the
    // controller: FontManager holds no MatrixClient, no network object and no
    // session state, and keeping it off the controller is what keeps that
    // true. Main.qml guards the name with `typeof`, so a QML test harness
    // that does not install it still loads.
    engine.rootContext()->setContextProperty("fonts", &fontManager);
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
    // Preview tiles in the screen-share picker: a live grab of one window or
    // display, taken when the row is drawn. Stateless — the id names what to
    // grab — and nothing is written to disk, because a still of whatever the
    // user has on screen must not outlive the dialog that asked for it. Null
    // everywhere but Windows, where the picker falls back to its glyph.
    //
    // GUARDED, because its header is: `ShareSourceImageProvider.cpp` is only
    // compiled under `HAVE_LIGHTNING_WEBRTC`, so a build without a media
    // engine has no such type. Registering it unconditionally compiled fine
    // on every machine that has GStreamer — which is every developer machine
    // here — and broke `build-deb`, where Debian's job builds without it.
    // A build with no media engine has no call, so no picker, so nothing ever
    // asks this provider for an image.
#ifdef HAVE_LIGHTNING_WEBRTC
    engine.addImageProvider(QStringLiteral("lightning-sharesource"),
                            new ShareSourceImageProvider());
#endif
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
