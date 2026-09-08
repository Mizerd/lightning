#include "calls/GstBootstrap.h"

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

/// Point GStreamer at the plugins shipped beside the executable.
///
/// A GStreamer plugin is dlopen'd, never linked, so nothing in the import
/// table names one and a packaged layout has to be pointed at explicitly: the
/// path compiled into the library is the BUILDER's sysroot, which does not
/// exist on a user's machine.
///
/// Layout contract, matched by both packaging scripts:
///   Windows   <install dir>/gstreamer-1.0/
///   macOS     Lightning.app/Contents/MacOS/gstreamer-1.0  (a SYMLINK to
///             ../PlugIns/gstreamer-plugins — codesign refuses a plain
///             directory of dylibs inside MacOS/, and QFileInfo::isDir
///             follows the link)
///   Linux     absent; the system GStreamer is used and nothing is touched.
/// The AppImage layout, checked against what the AppRun hook actually
/// exported. Filesystem side of the pure rule in the header.
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

void applyBundledPluginPath()
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .absoluteFilePath(QStringLiteral("gstreamer-1.0"));
    if (!QFileInfo(bundled).isDir()) {
        // Not the Windows/macOS layout, where the plugins sit beside the
        // binary. It may still be an AppImage, whose AppRun hook pointed
        // GStreamer at the bundle before this process began. Nothing to set;
        // recording it is the difference between a diagnostic that names the
        // runtime and one that names the wrong one.
        g_bundledPath = appImageBundle();
        return;   // otherwise a development build: leave the system alone.
    }
    // An explicit override wins. Someone debugging a plugin against a packaged
    // build has said what they want, and silently ignoring it would make the
    // override look broken.
    if (!qEnvironmentVariableIsEmpty("GST_PLUGIN_PATH"))
        return;
    qputenv("GST_PLUGIN_PATH", QFile::encodeName(bundled));
    // The bundle is COMPLETE, so the system path must not be consulted: a user
    // with their own GStreamer installed would otherwise load a mixture of two
    // builds into one process, which is a crash rather than a fallback.
    qputenv("GST_PLUGIN_SYSTEM_PATH", QByteArray());
    g_bundledPath = bundled;
}

} // namespace

bool ensureInitialised(QString *whyNot)
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        // BEFORE gst_init, always. The environment is read during init and
        // never again, so a caller that inits first and sets the path second
        // gets an empty registry — which is exactly the defect this unit was
        // created for.
        applyBundledPluginPath();
        GError *error = nullptr;
        ok = gst_init_check(nullptr, nullptr, &error) == TRUE;
        if (error) {
            // The message is upstream text about the local machine. Category
            // only; never the string, and never the path.
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

} // namespace lightning::gst

