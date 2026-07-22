#pragma once

// Pure, platform-independent startup predicates, extracted from main() so they
// can be unit-tested without constructing a QGuiApplication or reaching a real
// display server.
//
// The DISPLAY / WAYLAND_DISPLAY environment variables are an X11 / Wayland
// concept, i.e. specific to Unix-like desktops other than macOS. Windows (the
// "windows" QPA plugin) and macOS ("cocoa") reach their native platform plugin
// without either variable, so the "no graphical display" preflight must never
// fire on those platforms. Firing it unconditionally is the bug that made a
// normal double-click on native Windows exit with
// "no graphical display available (DISPLAY / WAYLAND_DISPLAY unset)".
//
// The concrete platform is resolved at compile time in main() via Q_OS_* and
// passed to shouldRejectForNoDisplay() as a plain bool, so the decision table
// can be exercised for every platform/env combination in a unit test.

namespace lightning::startup {

// Decide whether startup must refuse to construct a GUI application because no
// graphical display can be reached.
//
//   platformRequiresDisplayServer — true only on X11/Wayland platforms
//                                   (Unix, excluding macOS) whose Qt GUI plugin
//                                   needs DISPLAY / WAYLAND_DISPLAY. Windows and
//                                   macOS pass false and are never rejected.
//   hasDisplay                    — DISPLAY or WAYLAND_DISPLAY is set.
//   platformForced                — QT_QPA_PLATFORM is set (e.g. offscreen /
//                                   minimal for headless smoke tests).
//
// Returns true only when the running platform genuinely needs a display server
// and none is reachable and the caller has not forced a QPA platform.
inline bool shouldRejectForNoDisplay(bool platformRequiresDisplayServer,
                                     bool hasDisplay,
                                     bool platformForced)
{
    if (!platformRequiresDisplayServer)
        return false;
    return !hasDisplay && !platformForced;
}

} // namespace lightning::startup
