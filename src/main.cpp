#include "app/AppController.h"
#include "app/DesktopEntryQuoting.h"
#include "calls/CallSoundPlayer.h"
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
#include <QAudioDevice>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QThread>
#include <QDir>
#include <QSet>
#include <QFileInfo>
#include <QFontDatabase>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
// Needed by the software-renderer fallback in every build.
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
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
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using lightning::backendFromName;
using lightning::backendNameFor;

#ifdef Q_OS_WIN
// The Windows binary is a GUI-subsystem PE, so it attaches to the parent
// console (when there is one) to keep --version/--help and logging usable;
// --console forces a visible console.
//
// Returns true when the handle is an inherited redirect (file or pipe). Must
// be checked before AttachConsole, which replaces the standard handles.
bool standardHandleAlreadyGoesSomewhere(DWORD which)
{
    const HANDLE h = GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return false;
    // FILE_TYPE_CHAR is a console or NUL; disk files and pipes are redirects.
    const DWORD type = GetFileType(h) & ~DWORD(FILE_TYPE_REMOTE);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

void configureWindowsConsole(bool forceAlloc)
{
    // Sampled before AttachConsole. Reopening a redirected stream onto
    // CONOUT$ would send its output to the console and leave the user's
    // redirect target empty.
    const bool stdoutIsRedirected =
        standardHandleAlreadyGoesSomewhere(STD_OUTPUT_HANDLE);
    const bool stderrIsRedirected =
        standardHandleAlreadyGoesSomewhere(STD_ERROR_HANDLE);

    bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!attached && forceAlloc)
        attached = AllocConsole() != 0;
    if (attached) {
        FILE *f = nullptr;
        if (!stdoutIsRedirected)
            f = freopen("CONOUT$", "w", stdout);
        if (!stderrIsRedirected)
            f = freopen("CONOUT$", "w", stderr);
        (void)f;
        SetConsoleOutputCP(CP_UTF8);
    }
}
#endif

// --log-file: mirror the diagnostic log to a file. On Windows stdout is
// reopened onto the console, so a shell redirect cannot capture it. The file
// carries exactly what the console does (no tokens, keys or message bodies)
// and is appended to.
namespace {
// Qt calls message handlers from arbitrary threads (the GUI-stall watchdog and
// PlayableWriteWorker both log off the GUI thread), and neither QFile nor
// QTextStream is thread-safe. The lock covers the stream write and the flush.
// The previous handler is called outside it to avoid imposing a lock order.
QMutex g_logMutex;
QFile *g_logFile = nullptr;             // guarded by g_logMutex
// Atomic: it is written after logFileHandler is already installed and may be
// running on other threads, and it is read outside g_logMutex.
std::atomic<QtMessageHandler> g_previousHandler{nullptr};

void logFileHandler(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    if (const QtMessageHandler previous = g_previousHandler.load())
        previous(type, context, message);
    const char *level = "info";
    switch (type) {
    case QtDebugMsg:    level = "debug"; break;
    case QtInfoMsg:     level = "info"; break;
    case QtWarningMsg:  level = "warning"; break;
    case QtCriticalMsg: level = "critical"; break;
    case QtFatalMsg:    level = "fatal"; break;
    }
    QMutexLocker locker(&g_logMutex);
    if (!g_logFile)
        return;
    QTextStream(g_logFile)
        << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ' '
        << level << ' '
        << (context.category ? context.category : "default") << ": "
        << message << '\n';
    g_logFile->flush();   // a crash must not lose the lines that explain it
}

// Append program output verbatim to the --log-file, if one is open. The
// status commands print to stdout rather than log, so the message handler
// never sees their output.
void mirrorToLogFile(const QString &text)
{
    if (text.isEmpty())
        return;
    QMutexLocker locker(&g_logMutex);
    if (!g_logFile)
        return;
    QTextStream(g_logFile) << text;
    g_logFile->flush();
}
} // namespace

// Drop-in for `QTextStream out(stdout)` that also mirrors to --log-file, a
// whole line at a time so it interleaves with log lines as on the console.
class DiagnosticStream
{
public:
    explicit DiagnosticStream(FILE *device) : m_device(device) {}
    ~DiagnosticStream() { flush(); }

    Q_DISABLE_COPY_MOVE(DiagnosticStream)

    template <typename T>
    DiagnosticStream &operator<<(const T &value)
    {
        {
            QTextStream into(&m_pending);
            into << value;
        }
        const int lastNewline = m_pending.lastIndexOf(QLatin1Char('\n'));
        if (lastNewline >= 0) {
            emitText(m_pending.left(lastNewline + 1));
            m_pending = m_pending.mid(lastNewline + 1);
        }
        return *this;
    }

    void flush()
    {
        if (m_pending.isEmpty())
            return;
        emitText(m_pending);
        m_pending.clear();
    }

private:
    void emitText(const QString &text)
    {
        {
            QTextStream out(m_device);
            out << text;
        }
        // Flush per line so an abort does not lose buffered output.
        std::fflush(m_device);
        mirrorToLogFile(text);
    }

    FILE *m_device;
    QString m_pending;
};

void installLogFile(const QString &path)
{
    if (path.isEmpty())
        return;
    // Refuse symlinks and create the file owner-only: it is meant to be
    // shared, and a symlink would redirect the append elsewhere.
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
    // Local logs carry account slugs and store paths; say so up front.
    QTextStream(file)
        << "# Lightning debug log. Contains Matrix user ids and local file "
           "paths; never message content, keys or tokens. Review before "
           "sharing.\n";
    file->flush();
    {
        // Publish under the write lock so other threads see it safely.
        QMutexLocker locker(&g_logMutex);
        g_logFile = file;
    }
    g_previousHandler.store(qInstallMessageHandler(logFileHandler));
}

#ifndef LIGHTNING_BUILD_TYPE
#define LIGHTNING_BUILD_TYPE "unknown"
#endif

// Non-secret build metadata for --build-info, as `key: value` lines that CI
// asserts on. Never prints keys, tokens, URLs or account data;
// gif_keys_embedded reports only whether the compiled keys are non-empty.
QString buildInfoString()
{
    const bool rustCompiled =
#ifdef ENABLE_RUST_SDK_BACKEND
        true;
#else
        false;
#endif
    // HTTP and mock are compiled (or excluded by LIGHTNING_RUST_ONLY) together.
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
    // default_backend is a legacy alias of matrix_backend kept for CI checks.
    out += QStringLiteral("matrix_backend: %1\n").arg(backendName);
    out += QStringLiteral("default_backend: %1\n").arg(backendName);
    out += QStringLiteral("backends: %1\n").arg(backends.join(QLatin1Char(',')));
    out += QStringLiteral("rust_backend_compiled: %1\n").arg(yn(rustCompiled));
    out += QStringLiteral("http_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("mock_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("gif_keys_embedded: %1\n").arg(yn(gifKeysEmbedded));
    // CI asserts this is false in release builds.
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

// Pre-flight CLI parser that runs before QGuiApplication exists, so --help
// and bad --backend values are reported even when the platform plugin would
// abort (e.g. no display). Everything else is left to QCommandLineParser.
// A flag handled here that does not exit must also be registered with
// QCommandLineParser, or process() rejects it as unknown.
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
        RunCallQueueSelfTest, // --call-queue-selftest: the voice-delay check
        RunCallSoundsStatus, // --call-sounds-status: are the sounds loadable
        RunCallSoundsDemo,   // --call-sounds-demo: play every call sound once
        RunImageFormatStatus, // --image-format-status: probe the image decoders
        RunSpellStatus, // --spell-status: probe the platform spell checker
        RunDesktopStatus, // --desktop-status: launcher entry + icon association
    };
    Action action = Continue;
    // Compile-time default (Rust when built, else HTTP); --backend overrides.
    AppController::Backend backend = lightning::defaultBackend();
    bool backendExplicit = false;
    bool mockAliasUsed = false;
    bool smokeTestRequested = false;
    bool consoleRequested = false;   // Windows: force a visible console.
    /// --log-file PATH: mirror the diagnostic log to a file.
    QString logFilePath;
    // Development-only; rejected unless LIGHTNING_ENABLE_SCREENSHOT_DEMO is on.
    bool screenshotDemo = false;
    // Development-only --demo-* options; unknown flags in a normal build.
    QString demoScenario;
    QString demoAccount;
    QString demoTheme;
    QString demoAppearance;
    QString demoSize;
    bool demoHideControls = false;
    // Grab the window to a PNG once the scene settles, then quit.
    QString demoCapture;
    int demoCaptureDelayMs = 1400;
    QString stderrMsg;
    QString stdoutMsg;
};

PreflightResult preflightParse(int argc, char *argv[])
{
    PreflightResult r;

    // First pass: --log-file and --console decide where output goes, so they
    // are read from the whole command line before any terminating flag
    // (e.g. `--call-media-status --log-file x.log`) can return early.
    // Malformed values are still diagnosed by the main loop.
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QLatin1String("--log-file="))) {
            const QString path = a.mid(QLatin1String("--log-file=").size());
            if (!path.isEmpty())
                r.logFilePath = path;
            continue;
        }
        if (a == QLatin1String("--log-file")) {
            // Skip the value so a path spelled like a flag is not parsed.
            if (i + 1 < argc)
                r.logFilePath = QString::fromLocal8Bit(argv[++i]);
            continue;
        }
        if (a == QLatin1String("--console")) {
            r.consoleRequested = true;
            continue;
        }
    }

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
                "  --call-queue-selftest\n"
                "                       Measure the voice-delay property directly: run\n"
                "                       every queue this build's publish pipeline uses\n"
                "                       through a starved consumer, beside a plain\n"
                "                       GStreamer queue as a control, and report what\n"
                "                       each one held. No network, no GUI, no account,\n"
                "                       no sound card. Exit 0 when none of the shipped\n"
                "                       queues kept a backlog its consumer had caught\n"
                "                       up from. Takes about half a minute.\n"
                "  --call-sounds-status Load every bundled call sound through the same\n"
                "                       player a call uses and print which loaded, then\n"
                "                       exit. Exit 0 only when all of them did. Plays\n"
                "                       nothing. No network, no GUI, no account.\n"
                "  --call-sounds-demo   Play every call sound once, in order, on the\n"
                "                       default output, naming each as it plays, then\n"
                "                       exit. For hearing the set; takes ~30 seconds.\n"
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
            // Packaging validators pin this exact shape on every platform.
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
            // Accepted everywhere; only acted on under Q_OS_WIN.
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
            // Force the mock backend; main() isolates storage and auto-logs in.
            r.screenshotDemo = true;
            r.backend = AppController::MockBackend;
            r.backendExplicit = true;
            continue;
#else
            // Not compiled in: reject before QGuiApplication exists.
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
        // In any other build these fall through and are rejected as unknown.
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
        if (a == QLatin1String("--call-queue-selftest")) {
            r.action = PreflightResult::RunCallQueueSelfTest;
            return r;
        }
        if (a == QLatin1String("--call-sounds-status")) {
            r.action = PreflightResult::RunCallSoundsStatus;
            return r;
        }
        if (a == QLatin1String("--call-sounds-demo")) {
            r.action = PreflightResult::RunCallSoundsDemo;
            return r;
        }
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

            // The same roots RustSdkMatrixClient uses, plus legacy ones,
            // resolved without a QCoreApplication.
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
        // Reject these plausible-looking shortcuts here with a hint, before
        // a platform-plugin abort could hide the error.
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

// Chained handler that rate-limits the VAAPI texture-export warnings (see
// VaapiLogGate). Called from arbitrary threads; the gate's counter is atomic.
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

// Window icon and the AppImage launcher entry.
//
// Qt's Wayland client implements no icon protocol, so setWindowIcon() does
// nothing on native Wayland. The compositor resolves the icon from the
// toplevel's app id (desktopFileName()) via an installed `<app id>.desktop`.
// An AppImage installs nothing, so it publishes its own entry and icons under
// XDG_DATA_HOME.
//
// Only when both APPIMAGE and APPDIR are set and resolve; other package types
// install their own entry. Never overwrites an entry without the
// X-Lightning-Generated marker, and LIGHTNING_NO_DESKTOP_INTEGRATION=1
// disables it. A session with a cached application index may only pick the
// entry up on the next launch.
namespace {

// The app id, the launcher entry basename and the Icon= name; they must agree.
constexpr QLatin1String kAppId("lightning");
// Qt's xcb plugin takes the WM_CLASS instance from argv[0] (the binary name),
// independent of resolvedAppId().
constexpr QLatin1String kWmClass("lightning-matrix");

/// The app id this installation's launcher entry is published under.
///
/// A Flatpak exports only app-id-prefixed files, so its entry is
/// `$FLATPAK_ID.desktop` rather than `lightning.desktop`. Same resolution as
/// the notification desktop-entry hint in NotificationManager. Everywhere
/// else FLATPAK_ID is unset and kAppId is used.
QString resolvedAppId()
{
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
    return flatpakId.isEmpty() ? QString(kAppId) : flatpakId;
}

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
// AppImage-only helpers; compiled on Linux only to avoid unused-function
// warnings elsewhere.

// Lives in DesktopEntryQuoting so tests can call it; main.cpp cannot be linked
// into a test.
using lightning::desktop_entry::quoteExecArgument;

QStringList launcherEntrySource(const QString &payloadEntry)
{
    QFile file(payloadEntry);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    // Fallback only; validate-appimage.sh asserts the payload entry exists.
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

/// The launcher entry to publish: the payload's own entry (so translations
/// and metadata stay in data/lightning.desktop) with the keys a single-file
/// bundle gets wrong rewritten.
QString launcherEntryText(const QString &payloadEntry,
                          const QString &appImagePath)
{
    const QStringList source = launcherEntrySource(payloadEntry);

    // Exec arguments are taken from the payload entry, not duplicated here.
    QString execArguments;
    QStringList kept;
    bool seenHeader = false;
    for (const QString &raw : source) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            // Our keys are appended, so stop before a second group.
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
    // A path with a control character cannot be represented safely (it could
    // inject a key); an empty result tells the caller to skip.
    if (exec.isEmpty())
        return {};
    if (!execArguments.isEmpty())
        exec += QLatin1Char(' ') + execArguments;
    kept.append(QStringLiteral("Exec=") + exec);
    // TryExec hides the entry once the AppImage is deleted. It cannot be
    // quoted, so omit it for paths with whitespace (it would hide a working
    // entry). It is still a desktop-entry string, so backslashes must be
    // escaped; `%` and `$` have no meaning here.
    if (!appImagePath.contains(QLatin1Char(' '))
        && !appImagePath.contains(QLatin1Char('\t'))) {
        QString tryExec = appImagePath;
        tryExec.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        kept.append(QStringLiteral("TryExec=") + tryExec);
    }
    kept.append(QStringLiteral("Icon=") + kAppId);
    kept.append(QStringLiteral("StartupWMClass=") + kWmClass);
    kept.append(QStringLiteral("X-AppImage-Version=")
                + QLatin1String(APP_VERSION));
    // Marks the file as ours; entries without it are never overwritten.
    kept.append(QStringLiteral("X-Lightning-Generated=true"));
    return kept.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

/// The manifest of icon files this code wrote, one relative path per line.
/// It keeps user-placed icons from being overwritten and lets our copies be
/// removed when an installed package's entry takes over.
QString userIconManifestPath(const QString &dataHome)
{
    return dataHome
           + QStringLiteral("/lightning/appimage-icons.list");
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
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    for (const QString &relative : readUserIconManifest(dataHome)) {
        // Check containment on canonical paths so a symlinked directory
        // cannot redirect the removal outside the icon tree.
        const QString target = root + relative;
        const QString canonicalTarget = QFileInfo(target).canonicalFilePath();
        if (canonicalRoot.isEmpty() || canonicalTarget.isEmpty())
            continue;
        if (!canonicalTarget.startsWith(canonicalRoot + QLatin1Char('/')))
            continue;
        QFile::remove(target);
    }
    QFile::remove(userIconManifestPath(dataHome));
}

/// Count the payload's icons without writing anything, for the diagnostic
/// when publication is skipped.
void countPayloadIcons(const QString &appDir, int *payloadIcons)
{
    if (!payloadIcons)
        return;
    const QDir hicolor(appDir + QStringLiteral("/usr/share/icons/hicolor"));
    if (!hicolor.exists())
        return;
    for (const QString &size :
         hicolor.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir apps(hicolor.filePath(size + QStringLiteral("/apps")));
        if (!apps.exists())
            continue;
        *payloadIcons += apps.entryList(
            QStringList{ kAppId + QStringLiteral(".*") }, QDir::Files).size();
    }
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
            const QString relative =
                size + QStringLiteral("/apps/") + name;
            // An untracked file is the user's. Adopt it into the manifest
            // only if it is byte-identical to our payload icon (a copy from
            // before the manifest existed); otherwise leave it alone.
            if (QFileInfo::exists(to) && !recorded.contains(relative)) {
                QFile ours(from);
                QFile theirs(to);
                bool identical = QFileInfo(to).size() == QFileInfo(from).size()
                                 && ours.open(QIODevice::ReadOnly)
                                 && theirs.open(QIODevice::ReadOnly)
                                 && ours.readAll() == theirs.readAll();
                if (identical)
                    written.append(relative);   // a copy of ours from before
                continue;
            }
            written.append(relative);
            // Size is a cheap enough change check for a startup path.
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
    // Keep earlier entries so sizes a newer release dropped stay reclaimable.
    for (const QString &earlier : recorded) {
        if (!written.contains(earlier))
            written.append(earlier);
    }
    if (!written.isEmpty()) {
        QDir().mkpath(dataHome + QStringLiteral("/lightning"));
        QSaveFile manifest(userIconManifestPath(dataHome));
        if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            manifest.write(written.join(QLatin1Char('\n')).toUtf8());
            // Not fatal, but the next run would treat our copies as the user's.
            if (!manifest.commit())
                qWarning("lightning: could not record the icons written");
        }
    }
    return copied;
}

/// The launcher entry an installed package published, if any (XDG_DATA_DIRS
/// only), so we do not shadow it.
///
/// Directories under $APPDIR are skipped: linuxdeploy's AppRun prepends
/// `$APPDIR/usr/share` to XDG_DATA_DIRS, and the payload's own entry is not
/// visible to the compositor.
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

/// Publish the launcher entry and its icons for an AppImage run; a no-op
/// everywhere else, and when everything is already current.
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

    // Defer to an installed package: a user-level entry of the same name
    // would shadow it, and our TryExec would hide it once the AppImage is
    // deleted. Remove our own copy if one exists.
    if (const QString installed = systemLauncherEntry(); !installed.isEmpty()) {
        // Our user-level icons would shadow the package's; remove the ones
        // we recorded.
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

    // Build the entry first: it can be refused, and icons without an entry
    // would be left orphaned.
    const QByteArray wanted =
        launcherEntryText(report.payloadEntry, report.appImagePath).toUtf8();
    if (wanted.isEmpty()) {
        // Still count the payload icons so --desktop-status is accurate.
        countPayloadIcons(report.appDir, &report.payloadIcons);
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

/// The directories a session searches for launcher entries and icons:
/// $XDG_DATA_HOME first, then $XDG_DATA_DIRS, excluding $APPDIR (see
/// systemLauncherEntry()).
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

/// `--desktop-status`: report whether this session can resolve the app id to
/// a launcher entry and an icon, which decides the window icon on Wayland.
/// Performs the same publication a normal launch does, so the write itself is
/// exercised.
static int printDesktopStatus()
{
    DiagnosticStream out(stdout);

    // Must match the app id the startup path applies.
    const QString appId = resolvedAppId();
    QGuiApplication::setDesktopFileName(resolvedAppId());
    out << "qt version: " << QLatin1String(qVersion()) << "\n";
    out << "app id (desktop file name): "
        << QGuiApplication::desktopFileName() << "\n";
    out << "app id source: "
        << (qEnvironmentVariableIsEmpty("FLATPAK_ID") ? "built in"
                                                      : "FLATPAK_ID")
        << "\n";
    out << "launcher entry basename: " << appId << ".desktop\n";
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

    // Search the way the session does.
    QString visibleEntry;
    QString visibleIcon;
    const QStringList dirs = xdgDataDirs();
    for (const QString &dir : dirs) {
        const QString candidate = dir + QStringLiteral("/applications/")
            + appId + QStringLiteral(".desktop");
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
                    QStringList{ appId + QStringLiteral(".*") }, QDir::Files);
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
        out << "\nRESULT: this session cannot resolve the app id \"" << appId
            << "\" to a launcher entry and an icon, so the window and taskbar "
               "icon is a generic placeholder on Wayland. Expected for a source "
               "build, which installs neither; a packaging defect for any "
               "package.\n";
        return 1;
    }
    out << "\nRESULT: the app id \"" << appId
        << "\" resolves to a launcher entry and an icon this session can "
           "find.\n";
    return 0;
}

/// `--image-format-status`: report which image formats this build can decode
/// and whether that covers what Lightning accepts. Qt image formats are
/// dlopen'd plugins, so this is a property of the package, not the source.
///
/// Exit 0 only when every required format decodes. Optional formats (JPEG XL)
/// are reported but never fail: no plugin is available on Windows or macOS.
static int printImageFormatStatus()
{
    namespace ifmt = lightning::imagefmt;
    DiagnosticStream out(stdout);

    out << "qt version: " << QLatin1String(qVersion()) << "\n";
    // Qt searches every libraryPaths() entry, not just the compiled-in
    // prefix, so report where the decoders actually come from.
    out << "plugin path (compiled-in): "
        << QLibraryInfo::path(QLibraryInfo::PluginsPath) << "\n";
    // List only paths with image-format plugins (a dev shell has ~100
    // paths), plus the total searched.
    const QStringList libraryPaths = QCoreApplication::libraryPaths();
    int dirsWithPlugins = 0;
    QString pluginLines;
    {
        QTextStream ps(&pluginLines);
        // libraryPaths() can repeat a directory.
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
/// Kept out of GstBootstrap because it names both engines, and test targets
/// that link the bootstrap build only one of them.
static int printCallMediaStatus()
{
    DiagnosticStream out(stdout);
    out << "call media engine built in: yes\n";

    QString whyNot;
    const bool inited = lightning::gst::ensureInitialised(&whyNot);
    const QString bundled = lightning::gst::bundledPluginPath();
    out << "bundled plugin directory: "
        << (bundled.isEmpty()
                ? QStringLiteral("<none - using system GStreamer>")
                : bundled)
        << "\n";
    // GStreamer silently falls back to an in-process registry scan when
    // gst-plugin-scanner is missing, so report which one is used.
    const QString scanner = lightning::gst::bundledScannerPath();
    out << "plugin scanner: "
        << (scanner.isEmpty()
                ? QStringLiteral("<GStreamer's own - in-process fallback if "
                                 "it is not installed>")
                : scanner)
        << "\n";
    if (!inited) {
        out << "gstreamer: FAILED (" << whyNot << ")\n"
            << "\nRESULT: calls will be refused by this build.\n";
        return 1;
    }
    // The loaded version: webrtcbin behaviour differs between releases and
    // packaged runtimes differ from the dev shell.
    out << "gstreamer: initialised, " << lightning::gst::versionString()
        << "\n";

    // Same probes, same order as AppController, so the answer matches.
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

    // Asks the registry for the JPEG elements rather than checking for the
    // plugin file. Reported, not required: without it cameras fall back to
    // the raw chain at a lower frame rate.
    out << "camera compressed (MJPG) chain: "
        << (SfuMediaEngine::jpegCameraChainAvailable()
                ? QStringLiteral("available")
                : QStringLiteral("unavailable — cameras will use the raw "
                                 "entry and may be rate-limited"))
        << "\n";

    // Only the SFU engine decides the exit code; every MatrixRTC call uses it.
    out << "\nRESULT: "
        << (sfu ? QStringLiteral("calls can be placed and answered.")
                : QStringLiteral("calls will be refused by this build."))
        << "\n";
    return sfu ? 0 : 1;
}
#endif

int main(int argc, char *argv[])
{
    // Before QGuiApplication, so a platform-plugin abort cannot mask errors.
    const PreflightResult pf = preflightParse(argc, argv);

#ifdef Q_OS_WIN
    configureWindowsConsole(pf.consoleRequested);
#endif
    // All platforms: a macOS bundle launched from Finder has no terminal.
    installLogFile(pf.logFilePath);

    // Qt logs to the journal when stderr is not a TTY; headless and self-test
    // runs need stderr so a harness can capture them. An explicit value wins.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
        const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
        const bool headless = platform.startsWith("offscreen")
                           || platform.startsWith("minimal")
                           || pf.action == PreflightResult::RunSmokeTest
                           || pf.action == PreflightResult::RunGifSelfTest;
        if (headless)
            qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }

    // QT_DISABLE_HW_TEXTURES_CONVERSION is deliberately left alone: setting
    // it broke playback (black frames, frozen UI) on Mesa.
    //
    // Default to software video decoding: rotated H.264 deadlocked the render
    // pipeline through VAAPI, and chat-sized clips do not need hardware
    // decode. An explicit value (including "") wins.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "none");

    // Rate-limit Qt FFmpeg's per-frame VAAPI export warnings. Installed
    // before QGuiApplication so the earliest warnings are gated too.
    installVaapiLogGate();

    // Through DiagnosticStream so preflight output also reaches --log-file.
    if (pf.action == PreflightResult::ExitSuccess) {
        DiagnosticStream(stdout) << pf.stdoutMsg;
        return 0;
    }
    if (pf.action == PreflightResult::ExitError) {
        DiagnosticStream(stderr) << pf.stderrMsg;
        return 2;
    }
    if (pf.action == PreflightResult::ExitResetError) {
        DiagnosticStream(stdout) << pf.stdoutMsg;
        DiagnosticStream(stderr) << pf.stderrMsg;
        return 3;
    }
    if (pf.action == PreflightResult::RunCallMediaStatus) {
        // Uses the same bootstrap and probes as a normal launch, so it tells
        // "not built in", "plugins not found" and "element missing" apart.
        // The bootstrap needs a QCoreApplication for applicationDirPath().
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
#ifdef HAVE_LIGHTNING_WEBRTC
        QCoreApplication probeApp(argc, argv);
        return printCallMediaStatus();
#else
        // Configured without GStreamer.
        DiagnosticStream(stdout)
            << "call media engine built in: no\n"
            << "\nRESULT: calls will be refused by this build "
               "(configured without GStreamer).\n";
        return 1;
#endif
    }
    if (pf.action == PreflightResult::RunCallQueueSelfTest) {
        // Measures the publish pipeline's queue latency directly, without a
        // sound card or a second machine.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
#ifdef HAVE_LIGHTNING_WEBRTC
        // The bootstrap needs a QCoreApplication for applicationDirPath().
        QCoreApplication probeApp(argc, argv);
        DiagnosticStream out(stdout);
        QString whyNot;
        if (!lightning::gst::ensureInitialised(&whyNot)) {
            out << "gstreamer: FAILED (" << whyNot << ")\n"
                << "\nRESULT: nothing was measured — GStreamer did not "
                   "initialise.\n"
                << "VERDICT: unmeasurable\n";
            return 2;
        }
        QString report;
        const int rc = SfuMediaEngine::runQueueSelfTest(&report);
        out << report;
        return rc;
#else
        DiagnosticStream out(stdout);
        out << "call media engine built in: no\n"
            << "\nRESULT: nothing was measured — this build has no media "
               "engine (configured\nwithout GStreamer).\n"
            << "VERDICT: unmeasurable\n";
        return 2;
#endif
    }
    if (pf.action == PreflightResult::RunCallSoundsStatus
        || pf.action == PreflightResult::RunCallSoundsDemo) {
        // Whether the sounds play depends on the packaged Qt Multimedia
        // backend, so load each through the real CallSoundPlayer and report
        // which reached QSoundEffect::Ready.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication probeApp(argc, argv);
        DiagnosticStream out(stdout);
        const QAudioDevice output = QMediaDevices::defaultAudioOutput();
        out << "default audio output: "
            << (output.isNull() ? QStringLiteral("none")
                                : output.description())
            << "\n";
        CallSoundPlayer player([] { return QString(); });
        const QStringList &sounds = CallSoundPlayer::knownSounds();
        const auto loadedCount = [&] {
            int n = 0;
            for (const QString &sound : sounds)
                n += player.canPlay(sound) ? 1 : 0;
            return n;
        };
        const auto pump = [](int ms) {
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < ms) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(10);
            }
        };
        QElapsedTimer loading;
        loading.start();
        while (loadedCount() < sounds.size() && loading.elapsed() < 5000)
            pump(50);
        for (const QString &sound : sounds) {
            out << "  " << sound << ": "
                << (player.canPlay(sound) ? "loaded" : "NOT LOADED") << "\n";
        }
        const int loaded = loadedCount();
        out << "\nRESULT: " << loaded << " of " << sounds.size()
            << " call sounds loaded\n";
        if (pf.action == PreflightResult::RunCallSoundsDemo) {
            for (const QString &sound : sounds) {
                out << "playing " << sound << "\n";
                player.play(sound, 0.7, /*inCall=*/false);
                const bool bar = sound == QLatin1String("ring")
                    || sound == QLatin1String("call-waiting")
                    || sound == QLatin1String("ringback");
                pump(bar ? 3600 : 1500);
            }
        }
        return loaded == sounds.size() ? 0 : 1;
    }
    if (pf.action == PreflightResult::RunImageFormatStatus) {
        // QImageReader needs only a QCoreApplication. Not an offscreen
        // QGuiApplication: the Windows package ships no offscreen plugin.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication probeApp(argc, argv);
        return printImageFormatStatus();
    }
    if (pf.action == PreflightResult::RunSpellStatus) {
        // The backend is resolved at runtime (dlopen on Linux, COM on
        // Windows), so only running the artifact can answer this.
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication spellProbeApp(argc, argv);
        SpellChecker checker;
        checker.initialize();
        DiagnosticStream out(stdout);
        out << "spell checker available: "
            << (checker.available() ? "yes" : "no") << "\n";
        if (!checker.available()) {
            out << "\nRESULT: this machine has no dictionary Lightning can "
                   "reach, so the composer will not underline anything.\n";
            return 1;
        }
        out << "backend: " << checker.backendName() << "\n"
            << "dictionary: " << checker.language() << "\n";
        // A misspelling proves the dictionary actually answers.
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
        // Exercise suggestions too: they hand memory across the backend
        // boundary, so a wrong signature shows up here.
        const QStringList ideas = checker.suggestions(QStringLiteral("teh"));
        out << "sample suggestions: " << ideas.size() << "\n";
        out << "\nRESULT: spell checking works in this build on this "
               "machine.\n";
        return 0;
    }
    if (pf.action == PreflightResult::RunDesktopStatus) {
        // A filesystem lookup; a QCoreApplication is enough.
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

    // Refuse to construct QGuiApplication without a display, where Qt would
    // otherwise abort in the platform plugin. QT_QPA_PLATFORM overrides.
    // Only meaningful on X11/Wayland platforms, not Windows or macOS.
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

    // The persistent identity: these name the settings file, data/cache
    // roots, credential namespace and SDK stores of every existing install.
    // Changing them would sign every user out.
    QCoreApplication::setOrganizationName("MatrixClient");
    QCoreApplication::setOrganizationDomain("matrix-client.local");
    QCoreApplication::setApplicationName("matrix-client");
    QCoreApplication::setApplicationVersion(APP_VERSION);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // A distinct application name isolates every default QSettings store
    // from the developer's real configuration.
    if (pf.screenshotDemo)
        QCoreApplication::setApplicationName(
            QStringLiteral("matrix-client-screenshot-demo"));
#endif

    // Portable mode. Must be decided before the first QSettings is
    // constructed (the next block), which on Windows would settle on the
    // registry. After the demo block, whose application name feeds the INI
    // file name. No QCoreApplication exists yet; see storage/PortableMode.h.
    if (lightning::portable::isPortable()) {
        const QString portableProblem = lightning::portable::prepareDataRoot();
        if (!portableProblem.isEmpty()) {
            const QString advice =
                QStringLiteral("Lightning portable needs write access to its "
                               "data directory. Move the extracted folder to "
                               "a writable location and try again.");
            QTextStream(stderr) << portableProblem << "\n" << advice << "\n";
#ifdef Q_OS_WIN
            // A double-clicked GUI-subsystem binary has no console. No
            // QGuiApplication exists yet, so use MessageBoxW directly.
            const QString text = portableProblem + QStringLiteral("\n\n") + advice;
            MessageBoxW(nullptr,
                        reinterpret_cast<const wchar_t *>(text.utf16()),
                        L"Lightning", MB_OK | MB_ICONERROR);
#endif
            // No fallback to the installed locations: a portable copy must
            // never silently write outside its folder.
            return 4;
        }
        // Every default QSettings lands in
        // <dataRoot>/config/MatrixClient/matrix-client.ini, never the registry.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           lightning::portable::configDir());

        // Qt's own caches default to %LOCALAPPDATA%. Redirect the QML disk
        // cache; disable the shader cache, whose path can only be set per
        // window. Qt reads both when QGuiApplication is constructed.
        qputenv("QML_DISK_CACHE_PATH",
                QDir::toNativeSeparators(
                    lightning::portable::cacheDir()
                    + QStringLiteral("/qmlcache")).toLocal8Bit());
        QDir().mkpath(lightning::portable::cacheDir()
                      + QStringLiteral("/qmlcache"));
        qputenv("QT_DISABLE_SHADER_DISK_CACHE", "1");
    }

    // Remove scratch directories left by a crashed run (ours only: name
    // prefix, own uid, no symlinks). They can hold decrypted media.
    {
        const int swept = lightning::portable::cleanStaleTempDirs();
        if (swept > 0) {
            QTextStream(stderr)
                << "removed " << swept
                << " stale media scratch director"
                << (swept == 1 ? "y" : "ies") << "\n";
        }
    }

    // Interface zoom: Qt reads QT_SCALE_FACTOR once at startup, so zoom
    // changes take effect on the next launch. A user-set value wins.
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
    // The Media Foundation backend stalls the video surface after the first
    // frames; the package ships the FFmpeg plugin. A user override wins.
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
#endif

    // QApplication, not QGuiApplication: without a StatusNotifierItem
    // watcher, QSystemTrayIcon on X11 falls back to the XEmbed tray, which is
    // a QWidget.
    QApplication app(argc, argv);
    // Opt-in GUI-thread stall tracing (LIGHTNING_GUI_STALL_TRACE).
    stalltrace::install();

    // A missing GL stack must degrade rather than exit: Qt Quick fails to
    // create its RHI and quits before any window appears (e.g. an AppImage
    // whose host libEGL is unreachable). Probe before any QQuickWindow exists
    // and pick another backend. Never overrides an explicit choice.
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
            // Windows and macOS have native backends that need no OpenGL
            // (Direct3D 11 falls back to WARP without a GPU). The software
            // renderer cannot draw video nodes, so it is used only where no
            // such backend exists.
#if defined(Q_OS_WIN)
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
            qInfo("lightning: no usable OpenGL context on the \"%s\" platform "
                  "- using Direct3D 11, which renders video normally.",
                  qUtf8Printable(QGuiApplication::platformName()));
#elif defined(Q_OS_MACOS)
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
            qInfo("lightning: no usable OpenGL context on the \"%s\" platform "
                  "- using Metal, which renders video normally.",
                  qUtf8Printable(QGuiApplication::platformName()));
#else
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
            // The software adaptation cannot draw VideoOutput at all, even
            // though frames keep arriving and decrypting.
            qWarning("lightning: no usable OpenGL context on the \"%s\" "
                     "platform - falling back to the software renderer. "
                     "CALL AND SCREEN-SHARE VIDEO WILL NOT BE DISPLAYED on "
                     "this renderer; audio is unaffected. Set "
                     "QT_QUICK_BACKEND to override.",
                     qUtf8Printable(QGuiApplication::platformName()));
#endif
        }
    }
    // Qt has read the scale factor; unset it so child processes (the OAuth
    // browser, xdg-open) are not zoomed. A user-set value is left alone.
    if (zoomEnvSetHere)
        qunsetenv("QT_SCALE_FACTOR");
    // Wayland matches the window to its launcher entry via the desktop-file
    // name; X11 matches WM_CLASS via StartupWMClass. resolvedAppId() covers
    // the Flatpak case.
    QGuiApplication::setDesktopFileName(resolvedAppId());
    // For an AppImage, publish a launcher entry for that id (the only route
    // to a window icon on Wayland). A no-op for other install types.
    {
        const LauncherEntryReport entry = publishAppImageLauncherEntry();
        if (entry.appImageRun)
            qInfo("lightning: AppImage launcher entry: %s (%d icon file(s) copied)",
                  qUtf8Printable(entry.outcome), entry.iconsCopied);
    }

    // Warn once when a required image format has no decoder; otherwise it
    // is only a blank box in the timeline. Names formats only.
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

    // Bundled UI fonts (OFL), all loaded up front so switching the font in
    // Settings is instant. A failed load falls back to platform fonts.
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
    // Default application font, including native menus that never read
    // AppTheme, with the emoji face as a fallback family. The selected family
    // is applied once the controller exists, before the first frame.
    QGuiApplication::setFont(
        FontManager::withEmojiFallback(QStringLiteral("Manrope"), 14));
    // Also Qt's own fallback: a QML `font.family` drops the families list,
    // and Qt 6.8 then falls back to a monochrome emoji face.
    FontManager::installEmojiFallback(FontManager::emojiFamily());

    // Second pass through QCommandLineParser for Qt's own flags and its
    // standard unknown-option error.
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
    // Consumed by preflight but non-exiting, so they must be registered here
    // or process() rejects them (DesktopIntegrationTest checks this).
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
    // Consumed by preflight; registered so process() accepts it.
    QCommandLineOption screenshotDemoOpt(
        QStringLiteral("screenshot-demo"),
        QGuiApplication::translate("main",
            "Development-only: boot the mock backend with deterministic demo "
            "data for screenshots."));
    parser.addOption(screenshotDemoOpt);
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

    // Basic: flat, palette-driven stock controls; our own controls draw the
    // rest.
    QQuickStyle::setStyle("Basic");

    AppController controller(pf.backend, pf.screenshotDemo);
    // Here rather than in the AppController constructor so tests do not
    // initialise GStreamer.
    controller.enableCallMediaEngine();
    controller.enableCallSounds();

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Auto-login into the deterministic demo account on the mock backend.
    if (pf.screenshotDemo) {
        // Prefer the scenario's own account so no visible switch happens.
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

    // Imported fonts must be registered before the UI font is resolved, or
    // an imported family reads as missing.
    FontManager fontManager(controller.settings());
    fontManager.loadImportedFonts();

    // The resolved UI family (a missing family falls back to the bundled
    // face, keeping the stored value), applied now and on every change.
    const auto applyUiFont = [](const QString &family) {
        QGuiApplication::setFont(FontManager::withEmojiFallback(family, 14));
    };
    applyUiFont(fontManager.uiFamily());
    QObject::connect(&fontManager, &FontManager::selectionChanged,
                     &app, [&fontManager, applyUiFont] {
                         applyUiFont(fontManager.uiFamily());
                     });

    // Before the engine loads, so the first frame is translated.
    controller.localization()->applyStoredLanguage();

    QQmlApplicationEngine engine;
    // Live language switching. retranslate() does not reach strings already
    // stored in C++ models or JS variables (see docs/localization.md).
    QObject::connect(controller.localization(),
                     &LocalizationManager::retranslateRequested,
                     &engine, [&engine] { engine.retranslate(); });
    engine.rootContext()->setContextProperty("app", &controller);
    // Separate from the controller: FontManager holds no session state.
    // Main.qml guards it with `typeof` for test harnesses.
    engine.rootContext()->setContextProperty("fonts", &fontManager);
    // Decrypted media from the in-memory bridge cache. The engine owns the
    // provider; the bridge outlives it.
    engine.addImageProvider(QStringLiteral("lightning-media"),
                            new MediaImageProvider(controller.mediaBridge()));
    // Show-QR verification: memory-only module grid, cleared when the flow
    // ends. The QR payload itself never reaches this side.
    engine.addImageProvider(QStringLiteral("lightning-qr"),
                            new QrImageProvider(controller.qrCodeStore()));
    // About page's procedural storm art; stateless, parameters are in the id.
    engine.addImageProvider(QStringLiteral("storm-band"),
                            new StormBandImageProvider());
    // Live preview grabs for the screen-share picker (Windows only; null
    // elsewhere). Never written to disk. Only compiled with the media engine.
#ifdef HAVE_LIGHTNING_WEBRTC
    engine.addImageProvider(QStringLiteral("lightning-sharesource"),
                            new ShareSourceImageProvider());
#endif
    // Queued composer images (e.g. clipboard pastes) that have no file URL.
    engine.addImageProvider(QStringLiteral("lightning-staged"),
                            new StagedImageProvider(controller.stagedImages()));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Grab the settled demo window to a PNG and quit.
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

    // Log which backend the scene graph actually got (setGraphicsApi() is
    // only a request), with the screen refresh rate for context. Plain qInfo
    // so it appears without QT_LOGGING_RULES.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [&controller](QObject *obj, const QUrl &) {
            auto *win = qobject_cast<QQuickWindow *>(obj);
            if (!win)
                return;
            // Emitted on the render thread; `win` as context queues it to
            // the GUI thread.
            QObject::connect(
                win, &QQuickWindow::sceneGraphInitialized, win,
                [win, &controller] {
                    const char *name = "unknown";
                    bool software = false;
                    if (auto *ri = win->rendererInterface()) {
                        switch (ri->graphicsApi()) {
                        case QSGRendererInterface::Software:
                            name = "software"; software = true; break;
                        case QSGRendererInterface::OpenGL:  name = "opengl"; break;
                        case QSGRendererInterface::Vulkan:  name = "vulkan"; break;
                        case QSGRendererInterface::Metal:   name = "metal"; break;
                        case QSGRendererInterface::Direct3D11: name = "d3d11"; break;
                        case QSGRendererInterface::Direct3D12: name = "d3d12"; break;
                        default: break;
                        }
                    }
                    const QScreen *screen = win->screen();
                    qInfo("lightning: scene graph backend=%s software=%d "
                          "platform=%s refreshHz=%.1f dpr=%.2f",
                          name, software ? 1 : 0,
                          qUtf8Printable(QGuiApplication::platformName()),
                          screen ? screen->refreshRate() : 0.0,
                          win->effectiveDevicePixelRatio());
                    // The software backend cannot draw video; let the UI say so.
                    controller.setSoftwareRenderer(software);
                },
                Qt::SingleShotConnection);
        });

    engine.loadFromModule("MatrixClient", "Main");

    return app.exec();
}
