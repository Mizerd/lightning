#pragma once

#include <QColor>
#include <QEvent>

// The title bar of a top-level window on Windows belongs to DWM, which draws
// it in the SYSTEM's light or dark colours and none of the app's, so under a
// dark Lightning theme every caption stayed white (live Windows test
// 2026-10-16). DWM takes one request per window, DWMWA_USE_IMMERSIVE_DARK_MODE
// (attribute 20; 19 on Windows 10 before 20H1), and this keeps it in step with
// the application palette, which Main.qml's syncControlPalette() sets from the
// theme on load and on every theme change. A light theme asks for the light
// frame again.
//
// dwmapi.dll is loaded at run time, as WindowCaptureSrc does, so the package
// gains no import and an older Windows without the attribute just keeps its
// frame. Elsewhere install() does nothing: the window manager decorates.
//
// WHY IT IS DEFERRED (live FAIL, Windows, pipeline 326). Qt's own Windows
// plugin sets the SAME attribute from the SYSTEM colour scheme: when it creates
// a window, on QEvent::ApplicationPaletteChange it delivers to each window
// (QWindowsWindow::windowEvent -> setDarkBorder(system scheme == dark)), and
// when the system scheme changes. An application-level event filter runs
// BEFORE the event reaches the window, so a request made inside the filter was
// overwritten a moment later with the system's answer: a light system under a
// dark Lightning theme kept a light caption, on a fresh launch (Main.qml pushes
// the palette on load) and on every theme switch. So the request is made AFTER
// Qt, from a zero-delay timer, on every event that makes Qt touch the frame.
// QStyleHints::setColorScheme was considered and REFUSED: it would also rewrite
// styleHints().colorScheme(), which the "System" theme reads
// (AppController::systemDarkMode), making the app's choice the system's.
namespace lightning::windowframe {

/// What the filter does about an event: nothing, or put the frame request
/// back once Qt has finished with its own.
enum class Reapply { Ignore, AfterQt };

/// The events after which Qt may have rewritten a window's caption mode:
/// palette and system theme changes, the native window's creation, showing
/// and state changes. Never "now": the request always follows Qt's.
Reapply reapplyFor(QEvent::Type type);

/// "dark" or "light", for the lightning.window log line.
const char *frameModeName(bool dark);

/// Whether a window colour reads as dark: relative luminance under the
/// L* 50 midpoint (Y < 0.184), so a saturated dark colour counts as dark
/// where HSL lightness would not.
bool prefersDarkFrame(const QColor &window);

/// Follows the application palette on every top-level window: the ones that
/// exist, each new one as its native window is created, and all of them
/// again on every palette or system theme change. Call once, after the
/// QApplication exists; a second call does nothing.
void install();

} // namespace lightning::windowframe
