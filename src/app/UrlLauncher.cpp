#include "app/UrlLauncher.h"

#include <QDesktopServices>
#include <QProcess>

namespace lightning::urls {

QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Not in an AppImage: hand back the environment untouched, so a normal
    // install behaves exactly as it always has.
    if (!env.contains(QStringLiteral("APPDIR")))
        return env;

    // Restore the loader path the user's session had. The hook saved it; an
    // empty saved value means the session had none, in which case the variable
    // must be REMOVED rather than set to "" — an empty LD_LIBRARY_PATH entry
    // is read as the current directory.
    const QString original =
        env.value(QStringLiteral("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH"));
    if (original.isEmpty())
        env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    else
        env.insert(QStringLiteral("LD_LIBRARY_PATH"), original);

    // Everything else that points into the mount. These are set by the AppRun
    // hook for Lightning's own GStreamer and PipeWire clients; a browser
    // inheriting them would look for plugins in a directory that disappears
    // when Lightning exits.
    for (const char *key : { "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_PLUGIN_PATH_1_0",
                             "GST_PLUGIN_SCANNER", "GST_REGISTRY_1_0",
                             "SPA_PLUGIN_DIR", "PIPEWIRE_MODULE_DIR",
                             "PIPEWIRE_CONFIG_DIR", "QT_PLUGIN_PATH",
                             "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                             "PYTHONHOME", "PERLLIB", "GSETTINGS_SCHEMA_DIR",
                             "XDG_DATA_DIRS_APPIMAGE" }) {
        env.remove(QString::fromLatin1(key));
    }
    return env;
}

bool openExternally(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty())
        return false;

    const QProcessEnvironment env = childEnvironment();
    if (!env.contains(QStringLiteral("APPDIR")))
        return QDesktopServices::openUrl(url);

    // xdg-open rather than QDesktopServices, because only a QProcess lets the
    // child's environment be set. QDesktopServices would pass this process's.
    QProcess opener;
    opener.setProgram(QStringLiteral("xdg-open"));
    opener.setArguments({ url.toString(QUrl::FullyEncoded) });
    opener.setProcessEnvironment(env);
    if (opener.startDetached())
        return true;
    // A host with no xdg-open at all: better a browser started in the wrong
    // environment than no browser. It may still work, and it is what every
    // previous release did.
    return QDesktopServices::openUrl(url);
}

} // namespace lightning::urls
