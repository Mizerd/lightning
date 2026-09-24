#include "calls/GstBootstrap.h"

#include <atomic>
#include <mutex>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <gst/gst.h>

Q_LOGGING_CATEGORY(lcGstBoot, "lightning.calls.gst")

namespace lightning::gst {
namespace {

QString g_bundledPath;
QString g_scannerPath;

/// The AppImage's plugin directory, checked against what the AppRun hook
/// exported. Filesystem side of appImageBundledPluginPath().
QString appImageBundle()
{
    const QByteArray appDir = qgetenv("APPDIR");
    if (appDir.isEmpty())
        return {};
    const QString named = appImageBundledPluginPath(
        QFile::decodeName(appDir),
        QFile::decodeName(qgetenv("GST_PLUGIN_SYSTEM_PATH_1_0")),
        QFile::decodeName(qgetenv("GST_PLUGIN_PATH_1_0")));
    if (named.isEmpty() || !QFileInfo(named).isDir())
        return {};
    return named;
}

/// The AppImage's scanner, checked against what the AppRun hook exported.
/// Recorded, never set: the hook already exported it.
QString appImageScanner()
{
    const QByteArray appDir = qgetenv("APPDIR");
    if (appDir.isEmpty())
        return {};
    const QString named = appImageBundledScannerPath(
        QFile::decodeName(appDir),
        QFile::decodeName(qgetenv("GST_PLUGIN_SCANNER_1_0")),
        QFile::decodeName(qgetenv("GST_PLUGIN_SCANNER")));
    if (named.isEmpty())
        return {};
    // Executable as well as present, as applyBundledScannerPath() requires.
    const QFileInfo info(named);
    if (!info.isFile() || !info.isExecutable())
        return {};
    return named;
}

void applyBundledPluginPath()
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .absoluteFilePath(QStringLiteral("gstreamer-1.0"));
    if (!QFileInfo(bundled).isDir()) {
        // Plugins are dlopen'd, and the compiled-in path is the builder's
        // sysroot, so a packaged layout must be pointed at explicitly:
        //   Windows   <install dir>/gstreamer-1.0/
        //   macOS     Contents/MacOS/gstreamer-1.0, a symlink to
        //             ../PlugIns/gstreamer-plugins (codesign refuses a plain
        //             directory of dylibs in MacOS/)
        // Not found here: possibly an AppImage, whose AppRun hook already
        // configured GStreamer; record it for diagnostics.
        g_bundledPath = appImageBundle();
        g_scannerPath = appImageScanner();
        return;   // otherwise a development build: leave the system alone
    }
    // An explicit override wins.
    if (!qEnvironmentVariableIsEmpty("GST_PLUGIN_PATH"))
        return;
    qputenv("GST_PLUGIN_PATH", QFile::encodeName(bundled));
    // The bundle is complete, so skip the system path: mixing two GStreamer
    // builds in one process crashes.
    qputenv("GST_PLUGIN_SYSTEM_PATH", QByteArray());
    g_bundledPath = bundled;
}

/// Collapses a known-harmless upstream assertion. GStreamer's device
/// providers probe every ALSA PCM, and a device reporting a degenerate rate
/// range triggers a CRITICAL `GstIntRange` pair per probe (dozens per call
/// join). Not ours: gst-device-monitor-1.0 prints the same, and devices
/// enumerate fine.
///
/// The first occurrence is always logged in full (we must still see any bad
/// range we create ourselves), and repeats are only counted, with the count
/// logged at milestones. Every other message is forwarded untouched.
void installDeviceProbeNoiseCollapse()
{
    static std::once_flag once;
    std::call_once(once, [] {
        g_log_set_handler(
            "GStreamer",
            GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING
                           | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION),
            [](const gchar *domain, GLogLevelFlags level, const gchar *message,
               gpointer user) {
                static std::atomic<quint64> seen{0};
                const bool isProbeNoise =
                    message
                    && (g_strrstr(message, "GstIntRange") != nullptr
                        || g_strrstr(message, "gst_value_collect_int_range")
                            != nullptr);
                if (isProbeNoise) {
                    const quint64 n = ++seen;
                    // First occurrence in full; afterwards only milestones.
                    if (n == 1) {
                        qCWarning(lcGstBoot).nospace()
                            << "gstreamer device probe: " << message
                            << " — this is GStreamer's own device provider "
                               "reading a degenerate rate range from an audio "
                               "device, not Lightning, and nothing fails. "
                               "Repeats are collapsed from here.";
                        return;
                    }
                    if (n == 100 || n == 1000 || n % 10000 == 0) {
                        qCWarning(lcGstBoot)
                            << "gstreamer device probe: the same harmless "
                               "range assertion has now fired"
                            << n << "times";
                    }
                    return;
                }
                g_log_default_handler(domain, level, message, user);
            },
            nullptr);
    });
}

} // namespace

bool ensureInitialised(QString *whyNot)
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        // Before gst_init, always: the environment is read only during init.
        applyBundledPluginPath();
#ifdef Q_OS_MACOS
        // macOS only, guarded at the call so the function stays testable
        // everywhere. Windows uses GStreamer's in-process fallback and Linux a
        // system GStreamer with a correct compiled-in path.
        applyBundledScannerPath(QCoreApplication::applicationDirPath());
#endif
        installDeviceProbeNoiseCollapse();
        GError *error = nullptr;
        ok = gst_init_check(nullptr, nullptr, &error) == TRUE;
        if (error) {
            // Upstream text about the local machine: category only, never the
            // string or path.
            g_error_free(error);
        }
        if (ok) {
            qCInfo(lcGstBoot) << "gstreamer initialised bundled="
                              << !g_bundledPath.isEmpty();
        } else {
            qCWarning(lcGstBoot) << "gstreamer init failed";
        }
    });
    if (!ok && whyNot)
        *whyNot = QStringLiteral("gstreamer_init_failed");
    return ok;
}

QString versionString()
{
    gchar *version = gst_version_string();
    if (!version)
        return {};
    const QString text = QString::fromUtf8(version);
    g_free(version);
    return text;
}

QString bundledPluginPath()
{
    return g_bundledPath;
}

bool applyBundledScannerPath(const QString &applicationDirPath)
{
    // GStreamer reads the versioned name first, then the plain one; an
    // override in either wins.
    if (!qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER_1_0")
        || !qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER"))
        return false;
    const QString scanner = scannerPathBesideExecutable(applicationDirPath);
    if (scanner.isEmpty())
        return false;
    const QFileInfo info(scanner);
    // Executable as well as present. An unsigned helper is SIGKILLed on Apple
    // Silicon without a message, so this cannot prove it will run; the
    // in-process fallback covers that, and `--call-media-status` reports what
    // was found.
    if (!info.isFile() || !info.isExecutable())
        return false;
    const QByteArray encoded = QFile::encodeName(scanner);
    qputenv("GST_PLUGIN_SCANNER_1_0", encoded);
    qputenv("GST_PLUGIN_SCANNER", encoded);
    g_scannerPath = scanner;
    return true;
}

QString scannerPathBesideExecutable(const QString &applicationDirPath)
{
    if (applicationDirPath.isEmpty())
        return {};
    return QDir(applicationDirPath)
        .absoluteFilePath(QStringLiteral("gst-plugin-scanner"));
}

QString bundledScannerPath()
{
    return g_scannerPath;
}

QString appImageBundledPluginPath(const QString &appDir,
                                  const QString &systemPath,
                                  const QString &pluginPath)
{
    if (appDir.isEmpty())
        return {};
    const QString wanted = QDir::cleanPath(
        QDir(appDir).absoluteFilePath(QStringLiteral("usr/lib/gstreamer-1.0")));
    for (const QString &value : { systemPath, pluginPath }) {
        const auto parts = value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (QDir::cleanPath(part) == wanted)
                return wanted;
        }
    }
    return {};
}

QString appImageBundledScannerPath(const QString &appDir,
                                   const QString &scannerVersioned,
                                   const QString &scannerPlain)
{
    if (appDir.isEmpty())
        return {};
    const QString wanted = QDir::cleanPath(QDir(appDir).absoluteFilePath(
        QStringLiteral("usr/libexec/gstreamer-1.0/gst-plugin-scanner")));
    // A single path, not a list: GStreamer runs GST_PLUGIN_SCANNER verbatim.
    for (const QString &value : { scannerVersioned, scannerPlain }) {
        if (!value.isEmpty() && QDir::cleanPath(value) == wanted)
            return wanted;
    }
    return {};
}

} // namespace lightning::gst

