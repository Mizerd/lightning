#include "calls/GstBootstrap.h"

#include <atomic>
#include <mutex>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <gst/gst.h>
#ifdef HAVE_LIGHTNING_MAC_SYSTEM_AUDIO
#include "calls/MacSystemAudioSrc.h"
#endif

Q_LOGGING_CATEGORY(lcGstBoot, "lightning.calls.gst")

namespace lightning::gst {
namespace {

QString g_bundledPath;
QString g_scannerPath;

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
        g_bundledPath = appImageBundle();
        g_scannerPath = appImageScanner();
        return;
    }
    if (!qEnvironmentVariableIsEmpty("GST_PLUGIN_PATH"))
        return;
    qputenv("GST_PLUGIN_PATH", QFile::encodeName(bundled));
    // The bundle is complete, so skip the system path: mixing two GStreamer
    // builds in one process crashes.
    qputenv("GST_PLUGIN_SYSTEM_PATH", QByteArray());
    g_bundledPath = bundled;
}

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

}

bool ensureInitialised(QString *whyNot)
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        applyBundledPluginPath();
#ifdef Q_OS_MACOS
        applyBundledScannerPath(QCoreApplication::applicationDirPath());
#endif
        installDeviceProbeNoiseCollapse();
        GError *error = nullptr;
        ok = gst_init_check(nullptr, nullptr, &error) == TRUE;
#ifdef HAVE_LIGHTNING_MAC_SYSTEM_AUDIO
        if (ok)
            lightning::macaudio::registerSource();
#endif
        if (error) {
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
    if (!qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER_1_0")
        || !qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER"))
        return false;
    const QString scanner = scannerPathBesideExecutable(applicationDirPath);
    if (scanner.isEmpty())
        return false;
    const QFileInfo info(scanner);
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
    for (const QString &value : { scannerVersioned, scannerPlain }) {
        if (!value.isEmpty() && QDir::cleanPath(value) == wanted)
            return wanted;
    }
    return {};
}

}

