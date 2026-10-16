#pragma once

#include <QColor>

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
namespace lightning::windowframe {

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
