// Capture ONE WINDOW on Windows, because nothing we can ship does it.
//
// WHY THIS EXISTS. The screen-share picker could offer whole displays and
// nothing else, which is not what a person expects from a call client —
// Discord, Zoom and Element all let you pick an application. The reason was
// never design: `gdiscreencapsrc`, the only capture element in the shipped
// GStreamer set, takes a `monitor` index and a crop rectangle and has no
// window property at all. The element that does — `d3d11screencapturesrc`,
// with `window-handle` — lives in libgstd3d11.dll, which this project
// deliberately does not ship: the upstream SDK is a UCRT build whose
// `std::codecvt<wchar_t, char, mbstate_t>` mangles differently from this
// msvcrt toolchain's, so it imports libstdc++ symbols our staged copy does
// not export (docs/windows-packaging.md). Verified again rather than assumed:
// the toolchain and the staged libstdc++ both import msvcrt.dll.
//
// So the capture is ours. The precedent is `lightningrtpvp8pay` — Lightning
// already compiles a GStreamer element into the binary and registers it at
// init — and doing it here costs no new dependency: this is plain Win32 GDI,
// which the toolchain has had all along.
//
// WHAT IT CAPTURES, and why not the simpler thing. It asks the WINDOW to draw
// itself (`PrintWindow` with PW_RENDERFULLCONTENT), rather than reading the
// screen where the window happens to be. Cropping the screen to a window's
// rectangle is far less code and was rejected outright: anything stacked on
// top of that window — another app, a password prompt, a notification — would
// be shared too. A share that can leak a window the user did not choose is
// not a feature.
//
// The honest limitation of that choice: a window rendering through its own
// swapchain rather than through the window's DC may print blank. That is a
// property of the API, it is the same one every GDI-based capture has, and it
// is reported as a black frame rather than as a failure to start.
#pragma once

#include <QList>
#include <QString>

typedef struct _GstElement GstElement;

namespace lightning::wincap {

/// One capturable top-level window, as offered in the share picker.
struct WindowInfo {
    /// The HWND, widened so it can cross into QML and a GObject property
    /// without a Windows type leaking into either.
    quint64 handle = 0;
    QString title;
    int width = 0;
    int height = 0;
};

/// Whether this build can capture a window at all. False everywhere but
/// Windows, so the picker offers displays only and says so.
bool available();

/// Top-level windows a person would recognise: visible, non-minimised, titled,
/// on the taskbar, and never one of Lightning's own — sharing the call window
/// into the call is a hall of mirrors, and offering it invites the mistake.
QList<WindowInfo> enumerateWindows();

/// Register `lightningwindowcapturesrc`. Idempotent, safe from any thread,
/// must run after gst_init. A no-op off Windows.
void registerWindowCaptureSrc();

/// The element name to use in a pipeline description.
const char *windowCaptureSrcName();

} // namespace lightning::wincap
