#pragma once

#include <QProcessEnvironment>
#include <QUrl>

namespace lightning::urls {

// True for the URLs openExternally() will hand to the desktop: http(s) with a
// host and no credentials, and mailto. openExternally() enforces this itself.
bool isOpenableExternally(const QUrl &url);

// Open a URL in the user's browser. Inside an AppImage the inherited
// environment points into the bundle (LD_LIBRARY_PATH, GStreamer, PipeWire),
// so the browser would load the bundle's libraries and fail to start; the
// child gets the session's original environment instead. Returns false only
// when the URL is refused or the launch could not be started.
bool openExternally(const QUrl &url);

// The current environment with the AppImage bundle's variables removed and the
// session's LD_LIBRARY_PATH restored. Keyed off APPDIR. Exposed for testing.
QProcessEnvironment childEnvironment();

} // namespace lightning::urls
