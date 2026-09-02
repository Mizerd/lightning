#pragma once

#include <QProcessEnvironment>
#include <QUrl>

namespace lightning::urls {

// Open a URL in the user's browser, with an environment the browser can
// actually start in.
//
// QDesktopServices::openUrl() spawns `xdg-open`, and a spawned child INHERITS
// this process's environment. Inside an AppImage that environment points at the
// bundle: LD_LIBRARY_PATH puts $APPDIR/usr/lib first so the app's own Qt and
// GStreamer resolve, and the GStreamer/PipeWire variables point into a mount
// that only exists while Lightning runs. The browser then loads the BUNDLE's
// glib/gio/dbus ahead of the host's and fails to start — reported as "clicking
// links doesn't open them in browser in appimage", with no error anywhere.
//
// The AppRun hook preserves the caller's LD_LIBRARY_PATH as
// APPIMAGE_ORIGINAL_LD_LIBRARY_PATH exactly so this can be undone. Outside an
// AppImage nothing is stripped and this is QDesktopServices unchanged.
//
// Returns false only when the URL is unusable or the launch could not be
// started; a browser that starts and then fails is not observable here.
// True for the schemes openExternally() will hand to the desktop: http and
// https with a host and no credentials, and mailto. Exposed so a caller can
// decide what to SAY when a link is refused; the refusal itself does not
// depend on the caller checking.
bool isOpenableExternally(const QUrl &url);
bool openExternally(const QUrl &url);

// The environment a spawned child should get, exposed for testing: the current
// environment with the bundle's variables removed and LD_LIBRARY_PATH restored
// to whatever the user's session had. Keyed off APPDIR, which the AppImage
// runtime sets and nothing else does.
QProcessEnvironment childEnvironment();

} // namespace lightning::urls
