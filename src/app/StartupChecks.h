#pragma once

// Startup predicates extracted from main() for unit testing.
//
// DISPLAY / WAYLAND_DISPLAY only matter on X11/Wayland; Windows and macOS reach
// their platform plugin without them, so the "no graphical display" check must
// never fire there. main() resolves the platform via Q_OS_* and passes a bool.

namespace lightning::startup {

// True when startup must refuse to create a GUI application: the platform
// needs a display server (X11/Wayland), none is set, and QT_QPA_PLATFORM has
// not been forced (e.g. offscreen for headless tests).
inline bool shouldRejectForNoDisplay(bool platformRequiresDisplayServer,
                                     bool hasDisplay,
                                     bool platformForced)
{
    if (!platformRequiresDisplayServer)
        return false;
    return !hasDisplay && !platformForced;
}

} // namespace lightning::startup
