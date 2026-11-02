#pragma once

// Startup predicates extracted from main() for unit testing.
//
// DISPLAY / WAYLAND_DISPLAY only matter on X11/Wayland; Windows and macOS reach
// their platform plugin without them, so the "no graphical display" check must
// never fire there. main() resolves the platform via Q_OS_* and passes a bool.

namespace lightning::startup {

inline bool shouldRejectForNoDisplay(bool platformRequiresDisplayServer,
                                     bool hasDisplay,
                                     bool platformForced)
{
    if (!platformRequiresDisplayServer)
        return false;
    return !hasDisplay && !platformForced;
}

}
