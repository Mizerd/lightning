#include "app/UrlLauncher.h"

#include <QDesktopServices>
#include <QProcess>

namespace lightning::urls {

QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Not in an AppImage.
    if (!env.contains(QStringLiteral("APPDIR")))
        return env;

    // Restore the session's loader path, saved by the AppRun hook. Remove it
    // rather than set "", which would mean the current directory.
    const QString original =
        env.value(QStringLiteral("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH"));
    if (original.isEmpty())
        env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    else
        env.insert(QStringLiteral("LD_LIBRARY_PATH"), original);

    // Everything else the AppRun hook points into the mount. The hook saves the
    // session's value as APPIMAGE_ORIGINAL_<NAME>; restore it, or remove the
    // variable (never set ""). Both scanner spellings are needed: GStreamer
    // reads the versioned one first. This list must match the hook's.
    for (const char *key : { "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SCANNER_1_0", "GST_PLUGIN_SCANNER",
                             "GST_REGISTRY_1_0",
                             "SPA_PLUGIN_DIR", "PIPEWIRE_MODULE_DIR",
                             "PIPEWIRE_CONFIG_DIR", "QT_PLUGIN_PATH",
                             "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                             "PYTHONHOME", "PERLLIB", "GSETTINGS_SCHEMA_DIR",
                             "XDG_DATA_DIRS_APPIMAGE" }) {
        const QString name = QString::fromLatin1(key);
        const QString saved =
            env.value(QStringLiteral("APPIMAGE_ORIGINAL_") + name);
        if (saved.isEmpty())
            env.remove(name);
        else
            env.insert(name, saved);
        env.remove(QStringLiteral("APPIMAGE_ORIGINAL_") + name);
    }
    return env;
}

bool isOpenableExternally(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return false;
    // Only web and mail links. Everything else (file:, javascript:, data:, and
    // Windows handlers that have been RCE vectors such as ms-msdt:) is refused
    // here, at the single exit to ShellExecute / xdg-open.
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return !url.host().isEmpty() && url.userInfo().isEmpty();
    if (scheme == QLatin1String("mailto"))
        return !url.path().isEmpty();
    return false;
}

bool openExternally(const QUrl &url)
{
    if (!isOpenableExternally(url))
        return false;

    const QProcessEnvironment env = childEnvironment();
    if (!env.contains(QStringLiteral("APPDIR")))
        return QDesktopServices::openUrl(url);

    // Only a QProcess lets the child's environment be set.
    QProcess opener;
    opener.setProgram(QStringLiteral("xdg-open"));
    opener.setArguments({ url.toString(QUrl::FullyEncoded) });
    opener.setProcessEnvironment(env);
    if (opener.startDetached())
        return true;
    // No xdg-open: a browser in the wrong environment beats no browser.
    return QDesktopServices::openUrl(url);
}

} // namespace lightning::urls
