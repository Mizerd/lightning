#include "app/AppController.h"
#include "app/DesktopEntryQuoting.h"
#include "calls/CallSoundPlayer.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#ifdef HAVE_LIGHTNING_MAC_SYSTEM_AUDIO
#include "calls/MacSystemAudioSrc.h"
#endif
#include "calls/ShareSourceImageProvider.h"
#include "calls/GstCallMediaBackend.h"
#include "calls/SfuMediaEngine.h"
#include "calls/ShareAudioSources.h"
#include "calls/noise/MicProcessing.h"
#endif
#include "i18n/LocalizationManager.h"
#include "app/BackendSelection.h"
#include "app/SandboxEnvironment.h"
#include "app/UrlLauncher.h"
#include "calls/PulseServerProbe.h"
#include "calls/QtAudioBackendChoice.h"
#include "media/QtAudioGate.h"
#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
#include <QtMultimedia/private/qplatformmediaintegration_p.h>
#endif
#include <functional>
#include <QQuickItem>
#include "app/FontManager.h"
#include "app/QuitSignalBridge.h"
#include "app/SingleInstanceGuard.h"
#include "app/StartupChecks.h"
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include "app/ScreenshotDemoController.h"
#endif
#include "crypto/QrImageProvider.h"
#include "gif/GifBuildKeys.h"
#include "gif/GifProviderSelfTest.h"
#include "app/StormBandPainter.h"
#include "media/ImageFormatSupport.h"
#include "media/MediaImageProvider.h"
#include "media/SvgRasterJob.h"
#include "media/StagedImageProvider.h"
#include "app/AsyncLogSink.h"
#include "app/LogExport.h"
#include "app/GuiStallTracer.h"
#include "app/FrameIntervalProbe.h"
#include "app/WindowFrameTheme.h"
#include "app/AllocatorPolicy.h"
#include "app/IdleTrim.h"
#include "app/LogColor.h"
#include "app/ForeignQtEnvironment.h"
#include "app/QmlGcPolicy.h"
#include "app/RepeatLogLimiter.h"
#include "media/VaapiLogGate.h"
#include "text/SpellChecker.h"
#include "text/SpellBackend.h"
#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "smoke/RustSdkSmokeTest.h"
#include "matrix_rust.h"
#endif

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QAudioDevice>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QThread>
#include <QVersionNumber>
#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QFileInfo>
#include <QFontDatabase>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QPixmapCache>
#include <QTimer>
#include <QSGRendererInterface>
#include <QScreen>
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include <QImage>
#endif
#include <QQmlContext>
#include <QQuickStyle>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QDateTime>
#include <memory>
#include <QFile>
#include "models/ReverseListProxyModel.h"
#include <QTextStream>

#include <atomic>
#include <mutex>
#include <chrono>
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
bool g_qtAudioBackendSetHere = false;
const char *g_qtAudioBackendReason = "";
QString g_qtAudioBackendLine;
Q_LOGGING_CATEGORY(lcQtAudioBackend, "lightning.calls.sound")
}

namespace {

using lightning::backendFromName;
using lightning::backendNameFor;

#ifdef Q_OS_WIN
bool standardHandleAlreadyGoesSomewhere(DWORD which)
{
    const HANDLE h = GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return false;
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

namespace {
using lightning::logging::AsyncLogSink;
using lightning::logging::processLogSink;

// Qt calls message handlers on arbitrary threads, including GStreamer
// streaming threads (pad probes log the call counters) and the GUI thread.
// The handler only queues: a writer thread owns the file (AsyncLogSink), so
// a log file that stops accepting writes costs dropped lines, never a
// blocked caller. It used to write and flush here under one global mutex,
// and a stalled log pipe silenced call audio both ways.
//
// The previous handler (Qt's stderr or journald output) is still called
// synchronously, after the line is queued, and that is deliberate: it is
// where a crash is read when no --log-file was asked for, and a queue would
// lose exactly the lines before the crash; Qt formats QT_MESSAGE_PATTERN
// (%{threadid}, %{backtrace}) on the calling thread. It can still block if
// whatever reads stderr stops reading: a throttled journald, `2> file` on a
// throttled disk, or a Windows console with text selected in QuickEdit.
//
// Atomic: it is written after logFileHandler is already installed and may be
// running on other threads.
std::atomic<QtMessageHandler> g_previousHandler{nullptr};

void logFileHandler(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    if (AsyncLogSink *rolling = lightning::logexport::rollingLogSink()) {
        if (type == QtFatalMsg) {
            rolling->logFatal(context.category, message,
                              AsyncLogSink::kFatalDrainMs);
        } else {
            rolling->log(type, context.category, message);
        }
    }
    if (AsyncLogSink *sink = processLogSink()) {
        if (type == QtFatalMsg) {
            // Qt aborts when the handlers return: the line is queued past
            // the bound and the writer is waited for, bounded.
            sink->logFatal(context.category, message,
                           AsyncLogSink::kFatalDrainMs);
        } else {
            sink->log(type, context.category, message);
        }
    }
    if (const QtMessageHandler previous = g_previousHandler.load())
        previous(type, context, message);
}

void mirrorToLogFile(const QString &text)
{
    if (text.isEmpty())
        return;
    if (AsyncLogSink *sink = processLogSink())
        sink->writeVerbatim(text);
}

void flushLogFileAtExit()
{
    if (AsyncLogSink *sink = processLogSink())
        sink->shutdown(AsyncLogSink::kShutdownMs);
    if (AsyncLogSink *rolling = lightning::logexport::rollingLogSink())
        rolling->shutdown(AsyncLogSink::kShutdownMs);
}

void colourLogHandler(QtMsgType type, const QMessageLogContext &context,
                      const QString &message)
{
    const QByteArray line = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fprintf(stderr, "%s%s\033[0m\n", lightning::logColorFor(type),
                 line.constData());
    std::fflush(stderr);
}

void installLogColours()
{
    if (lightning::logColorWanted(lightning::stderrTakesLogColor(),
                                  qgetenv("NO_COLOR"), qgetenv("TERM"),
                                  qgetenv("QT_MESSAGE_PATTERN"))) {
        qInstallMessageHandler(colourLogHandler);
    }
}

void ensureLogHandlerInstalled()
{
    static std::atomic<bool> installed{false};
    if (installed.exchange(true))
        return;
    g_previousHandler.store(qInstallMessageHandler(logFileHandler));
}
}

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
    auto *sink = new AsyncLogSink(std::unique_ptr<QIODevice>(file));
    sink->writeVerbatim(QStringLiteral(
        "# Lightning debug log. Contains Matrix user ids and local file "
        "paths; never message content, keys or tokens. Review before "
        "sharing.\n"));
    lightning::logging::setProcessLogSink(sink);
    std::atexit(flushLogFileAtExit);
    ensureLogHandlerInstalled();
}

void installRollingLog()
{
    static std::once_flag hooks;
    std::call_once(hooks, [] { std::atexit(flushLogFileAtExit); });
    ensureLogHandlerInstalled();

    const QString cache = matrix::app_data::cacheRoot();
    if (cache.isEmpty())
        return;
    const QString dir = cache + QStringLiteral("/logs");
    lightning::logexport::setRollingLogDirectory(dir);
    if (!lightning::logexport::keepLogEnabled())
        return;
    lightning::logexport::startRollingLog(dir);
}

#ifndef LIGHTNING_BUILD_TYPE
#define LIGHTNING_BUILD_TYPE "unknown"
#endif

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
    out += QStringLiteral("matrix_backend: %1\n").arg(backendName);
    out += QStringLiteral("default_backend: %1\n").arg(backendName);
    out += QStringLiteral("backends: %1\n").arg(backends.join(QLatin1Char(',')));
    out += QStringLiteral("rust_backend_compiled: %1\n").arg(yn(rustCompiled));
    out += QStringLiteral("http_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("mock_backend_compiled: %1\n").arg(yn(httpMockCompiled));
    out += QStringLiteral("gif_keys_embedded: %1\n").arg(yn(gifKeysEmbedded));
    const bool screenshotDemoCompiled =
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
        true;
#else
        false;
#endif
    out += QStringLiteral("screenshot_demo_compiled: %1\n").arg(yn(screenshotDemoCompiled));
#ifdef LIGHTNING_TEST_LOOPBACK_FOCI
    out += QStringLiteral("test_loopback_foci_compiled: true\n");
#endif
    out += QStringLiteral("secret_store: %1\n").arg(secretStore);
    out += QStringLiteral("artifact_kind: %1\n").arg(QLatin1String(LIGHTNING_ARTIFACT_KIND));
    return out;
}

struct PreflightResult {
    enum Action {
        Continue,
        ExitSuccess,
        ExitError,
        ExitResetError,
        ExitResetRefused,
        RunSmokeTest,
        RunGifStatus,
        RunGifSelfTest,
        RunCallMediaStatus,
        RunCallQueueSelfTest,
        RunCallSoundsStatus,
        RunCallSoundsDemo,
        RunImageFormatStatus,
        RunSpellStatus,
        RunSpellSelfTest,
        RunDesktopStatus,
        RunRasterizeSvg,
    };
    Action action = Continue;
    AppController::Backend backend = lightning::defaultBackend();
    bool backendExplicit = false;
    bool mockAliasUsed = false;
    bool smokeTestRequested = false;
    bool consoleRequested = false;
    QString logFilePath;
    bool screenshotDemo = false;
    QString demoScenario;
    QString demoAccount;
    QString demoTheme;
    QString demoAppearance;
    QString demoSize;
    bool demoHideControls = false;
    QString demoCapture;
    int demoCaptureDelayMs = 1400;
    QString svgIn;
    QString svgOut;
    int svgMinEdge = 0;
    int svgMaxEdge = 0;
    bool svgNoText = false;
    QString stderrMsg;
    QString stdoutMsg;
};

void applyTextRenderingPreference(const QVariant &stored)
{
    QQuickWindow::setTextRenderType(
        SettingsManager::textRenderingFromValue(stored)
                == SettingsManager::kTextRenderingNative
            ? QQuickWindow::NativeTextRendering
            : QQuickWindow::QtTextRendering);
}

PreflightResult preflightParse(int argc, char *argv[])
{
    PreflightResult r;

    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QLatin1String("--log-file="))) {
            const QString path = a.mid(QLatin1String("--log-file=").size());
            if (!path.isEmpty())
                r.logFilePath = path;
            continue;
        }
        if (a == QLatin1String("--log-file")) {
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
                "                       Refuses, with exit code 6, while a running Lightning\n"
                "                       instance still owns this profile's data root.\n"
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
                "  --spell-selftest     Check English spelling end to end (engine, a\n"
                "                       misspelling, a correct word, a suggestion, an\n"
                "                       autocorrection and, on Linux, the dictionaries\n"
                "                       bundled in this package) and print booleans and\n"
                "                       a VERDICT line only. Exit 0 only on pass.\n"
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
            r.stdoutMsg = QStringLiteral("Lightning %1\n").arg(QLatin1String(APP_VERSION));
#ifdef LIGHTNING_TEST_LOOPBACK_FOCI
            r.stdoutMsg += QStringLiteral("WARNING: TEST BUILD permits loopback MatrixRTC foci\n");
#endif
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
        if (a == QLatin1String("--rasterize-svg")) {
            if (i + 4 >= argc) {
                r.action = PreflightResult::ExitError;
                r.stderrMsg = QStringLiteral(
                    "lightning-matrix: --rasterize-svg needs IN OUT MIN MAX\n");
                return r;
            }
            r.svgIn = QString::fromLocal8Bit(argv[i + 1]);
            r.svgOut = QString::fromLocal8Bit(argv[i + 2]);
            r.svgMinEdge = QString::fromLocal8Bit(argv[i + 3]).toInt();
            r.svgMaxEdge = QString::fromLocal8Bit(argv[i + 4]).toInt();
            r.svgNoText = i + 5 < argc
                && QString::fromLocal8Bit(argv[i + 5]) == QLatin1String("--no-text");
            r.action = PreflightResult::RunRasterizeSvg;
            return r;
        }
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
        if (a == QLatin1String("--spell-selftest")) {
            r.action = PreflightResult::RunSpellSelfTest;
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

            lightning::SingleInstanceGuard::RootLock rootLock(
                roots.first(), QStringLiteral("matrix-client"));
            if (rootLock.result()
                == lightning::SingleInstanceGuard::LockProbe::HeldByOther) {
                r.action = PreflightResult::ExitResetRefused;
                r.stderrMsg = QStringLiteral(
                    "Lightning is already running with this profile "
                    "(%1). Quit it first: deleting its Rust SDK store while "
                    "it is open can corrupt Olm sessions and one-time "
                    "keys.\n").arg(roots.first());
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
        if (a == QLatin1String("--http") || a == QLatin1String("--rust")) {
            const QString value = a.mid(2);
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

QtMessageHandler g_beforeRepeatLimiter = nullptr;
lightning::logging::RepeatLogLimiter *g_repeatLimiter = nullptr;

qint64 steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void repeatLimitedMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                                 const QString &msg)
{
    const QtMessageHandler next = g_beforeRepeatLimiter;
    if (!next)
        return;
    if (type == QtFatalMsg || !g_repeatLimiter) {
        next(type, ctx, msg);
        return;
    }
    const lightning::logging::RepeatLogLimiter::Decision decision =
        g_repeatLimiter->admit(type, ctx.category, msg, steadyNowMs());
    for (const auto &summary : decision.summaries) {
        const QMessageLogContext summaryContext(nullptr, 0, nullptr,
                                                summary.category.constData());
        next(summary.type, summaryContext, summary.text);
    }
    if (decision.pass)
        next(type, ctx, msg);
}

void flushRepeatLimiterAtExit()
{
    const QtMessageHandler next = g_beforeRepeatLimiter;
    if (!next || !g_repeatLimiter)
        return;
    for (const auto &summary : g_repeatLimiter->takePending(steadyNowMs())) {
        const QMessageLogContext summaryContext(nullptr, 0, nullptr,
                                                summary.category.constData());
        next(summary.type, summaryContext, summary.text);
    }
}

void installRepeatLimiter()
{
    g_repeatLimiter = new lightning::logging::RepeatLogLimiter();
    g_beforeRepeatLimiter = qInstallMessageHandler(repeatLimitedMessageHandler);
    std::atexit(flushRepeatLimiterAtExit);
}

}

namespace {

constexpr QLatin1String kAppId("lightning");
constexpr QLatin1String kWmClass("lightning-matrix");

QString resolvedAppId()
{
    const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
    return flatpakId.isEmpty() ? QString(kAppId) : flatpakId;
}

struct LauncherEntryReport {
    bool appImageRun = false;
    QString appImagePath;
    QString appDir;
    QString payloadEntry;
    QString payloadIconName;
    int payloadIcons = 0;
    QString userEntry;
    int iconsCopied = 0;
    QString outcome = QStringLiteral("not an AppImage run");
};

#if defined(Q_OS_LINUX)

using lightning::desktop_entry::quoteExecArgument;

QStringList launcherEntrySource(const QString &payloadEntry)
{
    QFile file(payloadEntry);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
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

QString launcherEntryText(const QString &payloadEntry,
                          const QString &appImagePath)
{
    const QStringList source = launcherEntrySource(payloadEntry);

    QString execArguments;
    QStringList kept;
    bool seenHeader = false;
    for (const QString &raw : source) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
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
    if (exec.isEmpty())
        return {};
    if (!execArguments.isEmpty())
        exec += QLatin1Char(' ') + execArguments;
    kept.append(QStringLiteral("Exec=") + exec);
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
    kept.append(QStringLiteral("X-Lightning-Generated=true"));
    return kept.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

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

void removeUserIcons(const QString &dataHome)
{
    const QString root = dataHome + QStringLiteral("/icons/hicolor/");
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    for (const QString &relative : readUserIconManifest(dataHome)) {
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
                    written.append(relative);
                continue;
            }
            written.append(relative);
            if (QFileInfo(to).size() == QFileInfo(from).size())
                continue;
            if (!QDir().mkpath(toDir))
                continue;
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
    for (const QString &earlier : recorded) {
        if (!written.contains(earlier))
            written.append(earlier);
    }
    if (!written.isEmpty()) {
        QDir().mkpath(dataHome + QStringLiteral("/lightning"));
        QSaveFile manifest(userIconManifestPath(dataHome));
        if (manifest.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            manifest.write(written.join(QLatin1Char('\n')).toUtf8());
            if (!manifest.commit())
                qWarning("lightning: could not record the icons written");
        }
    }
    return copied;
}

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

#endif

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

    if (const QString installed = systemLauncherEntry(); !installed.isEmpty()) {
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

    const QByteArray wanted =
        launcherEntryText(report.payloadEntry, report.appImagePath).toUtf8();
    if (wanted.isEmpty()) {
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

}

static int printDesktopStatus()
{
    DiagnosticStream out(stdout);

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

static int runRasterizeSvg(int &argc, char *argv[], const QString &in,
                           const QString &out, int minEdge, int maxEdge,
                           bool noText)
{
#ifdef Q_OS_UNIX
    struct rlimit cpu { 20, 25 };
    setrlimit(RLIMIT_CPU, &cpu);
#endif
    std::unique_ptr<QCoreApplication> app;
    bool allowText = !noText;
#ifdef Q_OS_LINUX
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
#ifdef Q_OS_MACOS
    qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
#endif
    if (allowText)
        app = std::make_unique<QGuiApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    namespace svg = lightning::svgraster;
    QByteArray bytes;
    {
        QFile file(in);
        if (file.open(QIODevice::ReadOnly))
            bytes = file.read(svg::kMaxSourceBytes + 1);
    }
    const svg::Result result =
        svg::rasterize(bytes, { minEdge, maxEdge }, allowText);
    QFile target(out);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 4;
    if (!result.refusal.isEmpty()) {
        target.write(result.refusal.toLatin1());
        return 3;
    }
    return target.write(result.png) == result.png.size() ? 0 : 4;
}

static int printImageFormatStatus()
{
    namespace ifmt = lightning::imagefmt;
    DiagnosticStream out(stdout);

    out << "qt version: " << QLatin1String(qVersion()) << "\n";
    out << "plugin path (compiled-in): "
        << QLibraryInfo::path(QLibraryInfo::PluginsPath) << "\n";
    const QStringList libraryPaths = QCoreApplication::libraryPaths();
    int dirsWithPlugins = 0;
    QString pluginLines;
    {
        QTextStream ps(&pluginLines);
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

    const QStringList missingRequired = ifmt::undecodable(true);
    const QStringList missingAny = ifmt::undecodable(false);
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
static int printCallMediaStatus()
{
    DiagnosticStream out(stdout);
    out << "call media engine built in: yes\n";

    QString whyNot;
    const bool inited = lightning::gst::ensureInitialised(&whyNot);
#if defined(HAVE_LIGHTNING_MAC_SYSTEM_AUDIO)
    if (inited && qEnvironmentVariableIsSet("LIGHTNING_TEST_MAC_CAPTURE"))
        return lightning::macaudio::captureDiagnostic();
#endif
    const QString bundled = lightning::gst::bundledPluginPath();
    out << "bundled plugin directory: "
        << (bundled.isEmpty()
                ? QStringLiteral("<none - using system GStreamer>")
                : bundled)
        << "\n";
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
    out << "gstreamer: initialised, " << lightning::gst::versionString()
        << "\n";

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

    {
        QStringList modes;
        for (const calls::noise::Mode mode :
             {calls::noise::Mode::Off, calls::noise::Mode::WebRtc,
              calls::noise::Mode::RNNoise,
              calls::noise::Mode::DeepFilterNet}) {
            const auto reason = calls::noise::availability(mode, sfu);
            modes << QStringLiteral("%1=%2").arg(
                QLatin1String(calls::noise::modeKey(mode)),
                reason == calls::noise::Unavailable::None
                    ? QStringLiteral("available")
                    : QString::fromLatin1(calls::noise::unavailableKey(reason)));
        }
        out << "microphone noise suppression: " << modes.join(QLatin1Char(' '))
            << "\n";
    }

    out << "camera compressed (MJPG) chain: "
        << (SfuMediaEngine::jpegCameraChainAvailable()
                ? QStringLiteral("available")
                : QStringLiteral("unavailable — cameras will use the raw "
                                 "entry and may be rate-limited"))
        << "\n";

#if defined(Q_OS_LINUX)
    out << "X11 screen capture (" << SfuMediaEngine::x11ScreenCaptureElementName()
        << "): "
        << (SfuMediaEngine::elementAvailable(
                SfuMediaEngine::x11ScreenCaptureElementName())
                ? QStringLiteral("available")
                : QStringLiteral("unavailable — screen sharing on X11 needs "
                                 "the desktop's portal"))
        << "\n";
#endif

    out << lightning::shareaudio::systemExclusionStatusLine(
               lightning::shareaudio::systemExclusion())
        << "\n";

    out << "\nRESULT: "
        << (sfu ? QStringLiteral("calls can be placed and answered.")
                : QStringLiteral("calls will be refused by this build."))
        << "\n";
    return sfu ? 0 : 1;
}
#endif

int main(int argc, char *argv[])
{
    lightning::applyAllocatorPolicy();

    lightning::applyQmlGcPolicy();

    const QStringList foreignQtReport =
        lightning::sanitizeForeignQtEnvironment();

    const PreflightResult pf = preflightParse(argc, argv);

#ifdef Q_OS_WIN
    configureWindowsConsole(pf.consoleRequested);
#endif
    installLogColours();
    installLogFile(pf.logFilePath);
    for (const QString &line : foreignQtReport)
        qInfo().noquote() << "lightning: Qt environment:" << line;

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
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");

    {
        using namespace lightning::calls;
        QtAudioBackendInputs in;
#if defined(Q_OS_LINUX)
        in.platform = AudioPlatform::Linux;
#elif defined(Q_OS_WIN)
        in.platform = AudioPlatform::Windows;
#elif defined(Q_OS_MACOS)
        in.platform = AudioPlatform::MacOS;
#endif
        in.flatpak = sandboxenv::isFlatpak();
        in.userSetBackend = qEnvironmentVariableIsSet("QT_AUDIO_BACKEND");
        const QVersionNumber running =
            QVersionNumber::fromString(QLatin1String(qVersion()));
        in.qtMajor = running.majorVersion();
        in.qtMinor = running.minorVersion();
        if (in.platform == AudioPlatform::Linux && !in.flatpak
            && !in.userSetBackend && running >= QVersionNumber(6, 10)) {
            in.qtHasPulseBackend = qtMultimediaHasPulseBackend();
            in.pulseServerReachable =
                in.qtHasPulseBackend && pulseServerReachable();
        }
        const QtAudioBackendDecision decision = decideQtAudioBackend(in);
        if (!decision.value.isEmpty()) {
            qputenv("QT_AUDIO_BACKEND", decision.value);
            g_qtAudioBackendSetHere = true;
        }
        g_qtAudioBackendReason = decision.reason;
        g_qtAudioBackendLine =
            QStringLiteral("qt audio backend choice: %1 set-by-lightning=%2 (%3, Qt %4)")
                .arg(in.userSetBackend
                         ? QStringLiteral("QT_AUDIO_BACKEND=")
                             + QString::fromLocal8Bit(qgetenv("QT_AUDIO_BACKEND"))
                         : decision.value.isEmpty()
                         ? QStringLiteral("Qt's default")
                         : QString::fromLatin1(decision.value),
                     g_qtAudioBackendSetHere ? QStringLiteral("yes")
                                             : QStringLiteral("no"),
                     QString::fromLatin1(decision.reason),
                     QString::fromLatin1(qVersion()));
    }

    installRepeatLimiter();
    installVaapiLogGate();

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
    if (pf.action == PreflightResult::ExitResetRefused) {
        DiagnosticStream(stdout) << pf.stdoutMsg;
        DiagnosticStream(stderr) << pf.stderrMsg;
        return 6;
    }
    if (pf.action == PreflightResult::RunCallMediaStatus) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
#ifdef HAVE_LIGHTNING_WEBRTC
        QCoreApplication probeApp(argc, argv);
        return printCallMediaStatus();
#else
        DiagnosticStream(stdout)
            << "call media engine built in: no\n"
            << "\nRESULT: calls will be refused by this build "
               "(configured without GStreamer).\n";
        return 1;
#endif
    }
    if (pf.action == PreflightResult::RunCallQueueSelfTest) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
#ifdef HAVE_LIGHTNING_WEBRTC
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
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication probeApp(argc, argv);
        DiagnosticStream out(stdout);
        const QAudioDevice output = QMediaDevices::defaultAudioOutput();
        out << "default audio output: "
            << (output.isNull() ? QStringLiteral("none")
                                : output.description())
            << "\n"
            << "qt audio backend requested: "
            << (qEnvironmentVariableIsSet("QT_AUDIO_BACKEND")
                    ? QString::fromLocal8Bit(qgetenv("QT_AUDIO_BACKEND"))
                    : QStringLiteral("(unset: Qt's default)"))
            << " set-by-lightning="
            << (g_qtAudioBackendSetHere ? "yes" : "no") << " reason: "
            << g_qtAudioBackendReason << "\n"
            << "call sounds voiced by: " << CallSoundPlayer::cueBackend()
            << "\n";
#if defined(LIGHTNING_HAVE_QT_AUDIO_BACKEND_NAME)
        if (qstrcmp(qVersion(), QT_VERSION_STR) == 0) {
            out << "qt audio backend: "
                << QString(QPlatformMediaIntegration::audioBackendName())
                << "\n";
        } else {
            out << "qt audio backend: (not asked: running Qt "
                << qVersion() << " is not the Qt " QT_VERSION_STR
                   " this was built against)\n";
        }
#endif
        out << "qt audio backend fallback (inferred, not proof): libpipewire "
               "loaded="
            << (lightning::calls::sharedObjectLoaded("libpipewire") ? "yes"
                                                                    : "no")
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
                player.play(sound, 0.7,false);
                const bool bar = sound == QLatin1String("ring")
                    || sound == QLatin1String("call-waiting")
                    || sound == QLatin1String("ringback");
                pump(bar ? 3600 : 1500);
            }
        }
        const int status = loaded == sounds.size() ? 0 : 1;
        CallSoundPlayer::setExitStatus(status);
        return status;
    }
    if (pf.action == PreflightResult::RunRasterizeSvg) {
        return runRasterizeSvg(argc, argv, pf.svgIn, pf.svgOut, pf.svgMinEdge,
                               pf.svgMaxEdge, pf.svgNoText);
    }
    if (pf.action == PreflightResult::RunImageFormatStatus) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication probeApp(argc, argv);
        return printImageFormatStatus();
    }
    if (pf.action == PreflightResult::RunSpellStatus) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication spellProbeApp(argc, argv);
        SpellChecker checker;
        checker.initialize();
        DiagnosticStream out(stdout);
        out << "spell checker available: "
            << (checker.available() ? "yes" : "no") << "\n";
        if (!checker.available()) {
            const QString reason = checker.unavailableReason();
            out << "reason: " << reason << "\n";
            if (reason == QLatin1String("no-library"))
                out << "\nRESULT: no spelling engine could be loaded (Linux: "
                       "neither libenchant-2 nor libhunspell-1.7; Windows: the "
                       "spell-checking service), so the composer will not "
                       "underline anything.\n";
            else if (reason == QLatin1String("no-platform"))
                out << "\nRESULT: this build has no spelling engine for this "
                       "operating system.\n";
            else
                out << "\nRESULT: the spelling engine loaded but has no "
                       "dictionary for the system language or English.\n";
            return 1;
        }
        out << "backend: " << checker.backendName() << "\n"
            << "dictionary: " << checker.language() << "\n"
            << "languages: " << checker.availableLanguages().join(QStringLiteral(" "))
            << "\n";
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        QStringList shownDirs;
        QStringList homes{ QDir::homePath() };
        const QString canonicalHome = QFileInfo(homes.first()).canonicalFilePath();
        if (!canonicalHome.isEmpty() && canonicalHome != homes.first())
            homes << canonicalHome;
        static const QRegularExpression perUser(
            QStringLiteral("^/etc/profiles/per-user/[^/]+"));
        for (QString dir : spellDictionaryDirectories()) {
            for (const QString &home : std::as_const(homes)) {
                if (!home.isEmpty() && home != QLatin1String("/")
                    && (dir == home || dir.startsWith(home + QLatin1Char('/')))) {
                    dir = QLatin1Char('~') + dir.mid(home.size());
                    break;
                }
            }
            dir.replace(perUser, QStringLiteral("/etc/profiles/per-user/$USER"));
            shownDirs << dir;
        }
        out << "dictionary directories: " << shownDirs.join(QStringLiteral(" "))
            << "\n";
#endif
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
        const QStringList ideas = checker.suggestions(QStringLiteral("teh"));
        out << "sample suggestions: " << ideas.size() << "\n";
        out << "\nRESULT: spell checking works in this build on this "
               "machine.\n";
        return 0;
    }
    if (pf.action == PreflightResult::RunSpellSelfTest) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication spellProbeApp(argc, argv);
        DiagnosticStream out(stdout);
        auto yn = [](bool v) { return v ? "yes" : "no"; };
        const bool compiledIn = platformSpellEngineCompiledIn();
        SpellChecker checker;
        checker.configure(QStringLiteral("en-US"), {}, QString());
        const bool loaded = checker.available()
            || checker.unavailableReason() == QLatin1String("no-dictionary");
        const bool english = checker.available();
        const bool rejects = english
            && !checker.misspelledRanges(QStringLiteral("teh")).isEmpty();
        const bool accepts = english
            && checker.misspelledRanges(QStringLiteral("the receive message")).isEmpty();
        const bool suggests = english
            && checker.suggestions(QStringLiteral("teh")).contains(QStringLiteral("the"));
        checker.setAutocorrectEnabled(true);
        const bool corrects = english
            && checker.autocorrection(QStringLiteral("teh "), 3)
                       .value(QStringLiteral("replacement")).toString()
                == QLatin1String("the");
        out << "spell engine compiled in: " << yn(compiledIn) << "\n"
            << "spell engine loaded: " << yn(loaded) << "\n"
            << "english dictionary found: " << yn(english) << "\n"
            << "misspelling rejected: " << yn(rejects) << "\n"
            << "correct words accepted: " << yn(accepts) << "\n"
            << "suggestion offered: " << yn(suggests) << "\n"
            << "autocorrect fixed a known typo: " << yn(corrects) << "\n";
        bool bundledOk = true;
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        const QStringList bundled = bundledSpellDictionaryDirectories();
        bool bundledWorks = false;
        int usable = 0;
        QStringList bundledLanguages;
        bool lithuanianWorks = false;
        if (!bundled.isEmpty()) {
            auto own = createHunspellSpellBackend(QStringLiteral("en-US"), bundled,
                                                  nullptr, &bundledLanguages);
            bundledWorks = own && !own->isCorrect(QStringLiteral("teh"))
                && own->isCorrect(QStringLiteral("the"));
            for (const QString &tag : std::as_const(bundledLanguages)) {
                auto backend = createHunspellSpellBackend(tag, bundled, nullptr, nullptr);
                if (backend && !backend->isCorrect(QStringLiteral("qzxjvkwq")))
                    ++usable;
            }
            auto lt = createHunspellSpellBackend(QStringLiteral("lt-LT"), bundled,
                                                 nullptr, nullptr);
            lithuanianWorks = lt && lt->isCorrect(QString::fromUtf8("\u0105\u017euolas"))
                && !lt->isCorrect(QString::fromUtf8("\u0105\u017euolsa"));
        }
        out << "bundled dictionaries present: " << yn(!bundled.isEmpty()) << "\n"
            << "bundled english dictionary works: " << yn(bundledWorks) << "\n"
            << "bundled languages usable: " << usable << "/"
            << bundledLanguages.size() << "\n"
            << "bundled lithuanian dictionary works: " << yn(lithuanianWorks) << "\n";
        bundledOk = bundled.isEmpty()
            || (bundledWorks && usable == bundledLanguages.size());
#endif
        const bool pass = compiledIn && english && rejects && accepts && suggests
            && corrects && bundledOk;
        out << "VERDICT: "
            << (pass ? "pass" : !compiledIn || !loaded ? "unavailable" : "fail")
            << "\n";
        return pass ? 0 : 1;
    }
    if (pf.action == PreflightResult::RunDesktopStatus) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication desktopProbeApp(argc, argv);
        return printDesktopStatus();
    }
    if (pf.action == PreflightResult::RunGifStatus) {
        return gif::printProviderStatus();
    }
    if (pf.action == PreflightResult::RunGifSelfTest) {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClient"));
        QCoreApplication::setApplicationName(QStringLiteral("matrix-client"));
        QCoreApplication selftestApp(argc, argv);
        return gif::runProviderSelfTest();
    }
#ifdef ENABLE_RUST_SDK_BACKEND
    if (pf.action == PreflightResult::RunSmokeTest) {
        std::unique_ptr<lightning::SingleInstanceGuard::RootLock> profileLock;
        if (qgetenv("LIGHTNING_TEST_PERSISTENT_STORE") == "1") {
            const QString root = matrix::app_data::primaryRoot();
            const bool created = !QFileInfo::exists(root);
            if (root.isEmpty() || !QDir().mkpath(root)) {
                DiagnosticStream(stderr) << "persistent smoke test: profile lock unavailable\n";
                return 5;
            }
            if (created)
                QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            profileLock = std::make_unique<lightning::SingleInstanceGuard::RootLock>(
                root, QStringLiteral("matrix-client"));
            if (profileLock->result() != lightning::SingleInstanceGuard::LockProbe::Acquired) {
                DiagnosticStream(stderr) << "persistent smoke test: profile lock unavailable or held by another process\n";
                return 5;
            }
            profileLock->holdLockUntilProcessExit();
        }
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClient"));
        QCoreApplication::setOrganizationDomain(
            QStringLiteral("matrix-client.local"));
        return runRustSdkSmokeTest(argc, argv);
    }
#endif

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

    QCoreApplication::setOrganizationName("MatrixClient");
    QCoreApplication::setOrganizationDomain("matrix-client.local");
    QCoreApplication::setApplicationName("matrix-client");
    QCoreApplication::setApplicationVersion(APP_VERSION);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
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
            const QString text = portableProblem + QStringLiteral("\n\n") + advice;
            MessageBoxW(nullptr,
                        reinterpret_cast<const wchar_t *>(text.utf16()),
                        L"Lightning", MB_OK | MB_ICONERROR);
#endif
            return 4;
        }
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           lightning::portable::configDir());

        qputenv("QML_DISK_CACHE_PATH",
                QDir::toNativeSeparators(
                    lightning::portable::cacheDir()
                    + QStringLiteral("/qmlcache")).toLocal8Bit());
        QDir().mkpath(lightning::portable::cacheDir()
                      + QStringLiteral("/qmlcache"));
        qputenv("QT_DISABLE_SHADER_DISK_CACHE", "1");
    }

    {
        const int swept = lightning::portable::cleanStaleTempDirs();
        if (swept > 0) {
            QTextStream(stderr)
                << "removed " << swept
                << " stale media scratch director"
                << (swept == 1 ? "y" : "ies") << "\n";
        }
    }

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

    applyTextRenderingPreference(
        QSettings().value(QLatin1String(SettingsManager::kTextRenderingKey)));

#ifdef Q_OS_WIN
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
#endif

    // QApplication, not QGuiApplication: without a StatusNotifierItem
    // watcher, QSystemTrayIcon on X11 falls back to the XEmbed tray, which is
    // a QWidget.
    QApplication app(argc, argv);
    qCInfo(lcQtAudioBackend).noquote() << g_qtAudioBackendLine;
    lightning::svgraster::hooks().scratchRoot = [] {
        return lightning::portable::mediaScratchRoot();
    };
    lightning::svgraster::hooks().holdScratchDir = [](const QString &dir) {
        lightning::portable::holdScratchDirLive(dir);
    };
    lightning::svgraster::hooks().releaseScratchDir = [](const QString &dir) {
        lightning::portable::releaseScratchDir(dir);
    };
    stalltrace::install();
    lightning::windowframe::install();

    lightning::SingleInstanceGuard instanceGuard(
        matrix::app_data::primaryRoot(), QCoreApplication::applicationName());
    // Held until the OS reclaims the fd/handle at real process exit, never
    // merely at main() unwinding: RustSdkMatrixClient's teardown can still be
    // closing the store on a background thread (its own retirement pool, a
    // leaked static that can outlive main() returning) for up to
    // kStoreCloseBudgetMs after aboutToQuit. Releasing any earlier would let
    // a fast relaunch open the same store while that close is still in
    // flight.
    instanceGuard.holdLockUntilProcessExit();
    switch (instanceGuard.claim()) {
    case lightning::SingleInstanceGuard::Claim::Primary:
#ifdef ENABLE_RUST_SDK_BACKEND
        mx_rust_profile_lock_held();
#endif
        break;
    case lightning::SingleInstanceGuard::Claim::Unguarded:
        break;
    case lightning::SingleInstanceGuard::Claim::Deferred:
        return 0;
    case lightning::SingleInstanceGuard::Claim::Unresponsive: {
        const QString text = QStringLiteral(
            "Lightning is already running with this profile but did not "
            "respond. If no window appears, quit the running Lightning (or end "
            "it in the task manager) and start it again.");
        QTextStream(stderr) << "lightning-matrix: " << text << "\n";
#ifdef Q_OS_WIN
        MessageBoxW(nullptr, reinterpret_cast<const wchar_t *>(text.utf16()),
                    L"Lightning", MB_OK | MB_ICONWARNING);
#endif
        return 5;
    }
    }
    installRollingLog();
#ifdef LIGHTNING_TEST_LOOPBACK_FOCI
    qWarning("WARNING: TEST BUILD permits loopback MatrixRTC foci; never distribute this binary");
#endif
    lightning::logexport::setBuildInfo(buildInfoString());
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &instanceGuard,
                     &lightning::SingleInstanceGuard::stopListening);

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
            qWarning("lightning: no usable OpenGL context on the \"%s\" "
                     "platform - falling back to the software renderer. "
                     "CALL AND SCREEN-SHARE VIDEO WILL NOT BE DISPLAYED on "
                     "this renderer; audio is unaffected. Set "
                     "QT_QUICK_BACKEND to override.",
                     qUtf8Printable(QGuiApplication::platformName()));
#endif
        }
    }
    if (zoomEnvSetHere)
        qunsetenv("QT_SCALE_FACTOR");
    QGuiApplication::setDesktopFileName(resolvedAppId());
    {
        const LauncherEntryReport entry = publishAppImageLauncherEntry();
        if (entry.appImageRun)
            qInfo("lightning: AppImage launcher entry: %s (%d icon file(s) copied)",
                  qUtf8Printable(entry.outcome), entry.iconsCopied);
    }

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

    for (const char *font : { "Manrope[wght].ttf", "JetBrainsMono[wght].ttf",
                              "Inter[wght].ttf", "IBMPlexSans[wght].ttf",
                              "SourceSans3[wght].ttf",
                              "PlusJakartaSans[wght].ttf",
                              "SpaceGrotesk[wght].ttf",
                              "MaterialSymbolsRounded-subset.ttf" }) {
        // From data, not from the resource path: by path Qt keeps one copy
        // here and FreeType reads another for every weight it opens.
        QFile file(QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
                   + QLatin1String(font));
        if (file.open(QIODevice::ReadOnly))
            QFontDatabase::addApplicationFontFromData(file.readAll());
    }
    QGuiApplication::setFont(
        FontManager::withEmojiFallback(QStringLiteral("Manrope"), 14));
    if (FontManager::emojiFallbackNeeded(QLibraryInfo::version()))
        FontManager::installEmojiFallback(FontManager::emojiFamily());

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

    QQuickStyle::setStyle("Basic");

    AppController controller(pf.backend, pf.screenshotDemo);
    controller.enableCallMediaEngine();
    controller.enableCallSounds();

    lightning::IdleTrim idleTrim(&app);
    app.installEventFilter(&idleTrim);
    QObject::connect(&controller, &AppController::currentRoomIdChanged,
                     &idleTrim, [&idleTrim] { idleTrim.noteActivity(); });

    QObject::connect(
        &instanceGuard, &lightning::SingleInstanceGuard::activationRequested,
        &controller, [&controller](const QString &activationToken) {
            if (!activationToken.isEmpty()
                && QGuiApplication::platformName().startsWith(
                    QLatin1String("wayland"))) {
                qputenv("XDG_ACTIVATION_TOKEN", activationToken.toLatin1());
            }
            Q_EMIT controller.trayShowRequested();
        });

    lightning::quitsignals::install(&controller, [&controller](int) {
        Q_EMIT controller.applicationQuitIntended();
        QCoreApplication::quit();
    });

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Auto-login into the deterministic demo account on the mock backend.
    if (pf.screenshotDemo) {
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

    FontManager fontManager(controller.settings());
    fontManager.loadImportedFonts();

    const auto applyUiFont = [](const QString &family) {
        QGuiApplication::setFont(FontManager::withEmojiFallback(family, 14));
    };
    applyUiFont(fontManager.uiFamily());
    QObject::connect(&fontManager, &FontManager::selectionChanged,
                     &app, [&fontManager, applyUiFont] {
                         applyUiFont(fontManager.uiFamily());
                     });

    controller.localization()->applyStoredLanguage();

    QQmlApplicationEngine engine;
    QObject::connect(controller.localization(),
                     &LocalizationManager::retranslateRequested,
                     &engine, [&engine] { engine.retranslate(); });
    engine.rootContext()->setContextProperty("app", &controller);
    engine.rootContext()->setContextProperty(
        "qtAudio", &lightning::media::QtAudioGate::install());
    lightning::calls::releaseQtAudioBackendVariable(g_qtAudioBackendSetHere);
    engine.rootContext()->setContextProperty("fonts", &fontManager);
    engine.addImageProvider(QStringLiteral("lightning-media"),
                            new MediaImageProvider(controller.mediaBridge()));
    engine.addImageProvider(QStringLiteral("lightning-qr"),
                            new QrImageProvider(controller.qrCodeStore()));
    engine.addImageProvider(QStringLiteral("storm-band"),
                            new StormBandImageProvider());
#ifdef HAVE_LIGHTNING_WEBRTC
    engine.addImageProvider(QStringLiteral("lightning-sharesource"),
                            new ShareSourceImageProvider());
#endif
    engine.addImageProvider(QStringLiteral("lightning-staged"),
                            new StagedImageProvider(controller.stagedImages()));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    if (pf.screenshotDemo && !pf.demoCapture.isEmpty()) {
        const QString capturePath = pf.demoCapture;
        const int captureDelay = pf.demoCaptureDelayMs;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
            [capturePath, captureDelay](QObject *obj, const QUrl &) {
                auto *win = qobject_cast<QQuickWindow *>(obj);
                if (!win)
                    return;
                auto settled = std::make_shared<int>(0);
                auto *poll = new QTimer(win);
                poll->setInterval(200);
                QObject::connect(poll, &QTimer::timeout, win, [win, settled] {
                    const QQuickItem *ci = win->contentItem();
                    if (ci && qFuzzyCompare(ci->width() + 1, win->width() + 1)
                        && qFuzzyCompare(ci->height() + 1, win->height() + 1))
                        ++*settled;
                    else
                        *settled = 0;
                });
                poll->start();
                QTimer::singleShot(captureDelay, win, [win, capturePath, settled] {
                    for (int i = 0; i < 100 && *settled < 5; ++i)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
                    if (qEnvironmentVariableIntValue("LIGHTNING_DEMO_AUDIT")
                        != 0) {
                        const QRectF wr(0, 0, win->width(), win->height());
                        QTextStream(stdout) << "AUDIT window " << win->width()
                                            << "x" << win->height() << "\n";
                        std::function<void(QQuickItem *)> walk =
                            [&](QQuickItem *it) {
                            if (!it->isVisible() || it->opacity() <= 0.01)
                                return;
                            const QByteArray cls = it->metaObject()->className();
                            if (qEnvironmentVariableIsSet("LIGHTNING_DEMO_AUDIT_NAMES")
                                && qEnvironmentVariable("LIGHTNING_DEMO_AUDIT_NAMES")
                                       .split(QLatin1Char(','))
                                       .contains(it->objectName()))
                                QTextStream(stdout) << "AUDIT item "
                                    << it->objectName() << " "
                                    << it->mapToScene(QPointF(0, 0)).x() << ","
                                    << it->mapToScene(QPointF(0, 0)).y() << " "
                                    << it->width() << "x" << it->height() << "\n";
                            const bool control = cls.contains("Button")
                                || cls.contains("TextField")
                                || cls.contains("ComboBox")
                                || cls.contains("CheckBox")
                                || cls.contains("Switch")
                                || cls.contains("PopupItem");
                            if (control && it->width() > 0 && it->height() > 0) {
                                QRectF r = it->mapRectToScene(
                                    QRectF(0, 0, it->width(), it->height()));
                                for (QQuickItem *a = it->parentItem(); a;
                                     a = a->parentItem())
                                    if (a->clip())
                                        r = r.intersected(a->mapRectToScene(
                                            QRectF(0, 0, a->width(), a->height())));
                                const bool popup = cls.contains("PopupItem");
                                const QRectF full = it->mapRectToScene(
                                    QRectF(0, 0, it->width(), it->height()));
                                if (popup && !wr.adjusted(-1, -1, 1, 1).contains(full))
                                    QTextStream(stdout) << "AUDIT popup-outside "
                                        << cls << " " << it->objectName() << " "
                                        << full.x() << "," << full.y() << " "
                                        << full.width() << "x" << full.height()
                                        << "\n";
                                else if (!popup && !r.isEmpty()
                                         && !wr.adjusted(-1, -1, 1, 1).contains(r))
                                    QTextStream(stdout) << "AUDIT control-outside "
                                        << cls << " " << it->objectName() << " "
                                        << r.x() << "," << r.y() << " "
                                        << r.width() << "x" << r.height() << "\n";
                            }
                            for (QQuickItem *c : it->childItems())
                                walk(c);
                        };
                        walk(win->contentItem());
                    }
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

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [&controller](QObject *obj, const QUrl &) {
            auto *win = qobject_cast<QQuickWindow *>(obj);
            if (!win)
                return;
            if (FrameIntervalProbe::enabledByEnvironment())
                new FrameIntervalProbe(win, win);
            QObject::connect(
                win, &QQuickWindow::sceneGraphInitialized, win,
                [&controller] {
                    auto *ctx = QOpenGLContext::currentContext();
                    if (!ctx || !ctx->functions())
                        return;
                    const auto *vendor = reinterpret_cast<const char *>(
                        ctx->functions()->glGetString(GL_VENDOR));
                    const auto *renderer = reinterpret_cast<const char *>(
                        ctx->functions()->glGetString(GL_RENDERER));
                    qInfo("lightning: GL_RENDERER=%s GL_VENDOR=%s",
                          renderer ? renderer : "?", vendor ? vendor : "?");
                    if (renderer) {
                        const QString glRenderer =
                            QString::fromLatin1(renderer);
                        QMetaObject::invokeMethod(
                            &controller, [&controller, glRenderer] {
                                controller.setGlRenderer(glRenderer);
                            },
                            Qt::QueuedConnection);
                    }
                },
                Qt::DirectConnection);
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
                    controller.setSoftwareRenderer(software);
                },
                Qt::SingleShotConnection);
        });

    engine.loadFromModule("MatrixClient", "Main");

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &engine,
                     [&engine, &controller] {
        QElapsedTimer clock;
        clock.start();
        qInfo("quit: hiding windows and clearing timeline delegates");
        for (QWindow *window : QGuiApplication::allWindows())
            window->hide();
        if (auto *view = qobject_cast<ReverseListProxyModel *>(
                controller.timelineView()))
            view->setSourceModel(nullptr);
        qInfo("quit: timeline delegates cleared elapsedMs=%lld",
              static_cast<long long>(clock.elapsed()));
    });

    QTimer scrollMemoryTimer;
    if (qEnvironmentVariableIsSet("LIGHTNING_SCROLL_MEMORY_TRACE")) {
        QObject::connect(&scrollMemoryTimer, &QTimer::timeout, &app,
                         [&controller] {
            qint64 rssKiB = -1;
#ifdef Q_OS_LINUX
            QFile status(QStringLiteral("/proc/self/status"));
            if (status.open(QIODevice::ReadOnly)) {
                while (!status.atEnd()) {
                    const QByteArray line = status.readLine();
                    if (line.startsWith("VmRSS:")) {
                        const QList<QByteArray> fields = line.simplified().split(' ');
                        if (fields.size() >= 2)
                            rssKiB = fields.at(1).toLongLong();
                        break;
                    }
                }
            }
#endif
            qInfo("scroll-memory rssKiB=%lld bridgeBytes=%lld "
                  "qtPixmapLimitKiB=%d rows=%d sourceRows=%d",
                  static_cast<long long>(rssKiB),
                  static_cast<long long>(controller.mediaBridge()->cacheBytesUsed()),
                  QPixmapCache::cacheLimit(),
                  controller.timelineView()->rowCount(),
                  controller.timeline()->rowCount());
        });
        scrollMemoryTimer.setInterval(5000);
        scrollMemoryTimer.start();
    }

    const int status = app.exec();
    CallSoundPlayer::setExitStatus(status);
    return status;
}
