#pragma once

#include <QProcessEnvironment>
#include <QUrl>

namespace lightning::urls {

bool isOpenableExternally(const QUrl &url);

// Open a URL in the user's browser. Inside an AppImage the inherited
// environment points into the bundle (LD_LIBRARY_PATH, GStreamer, PipeWire),
// so the browser would load the bundle's libraries and fail to start; the
// child gets the session's original environment instead. Returns false only
// when the URL is refused or the launch could not be started.
bool openExternally(const QUrl &url);

QProcessEnvironment childEnvironment();

}
