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

#include <QImage>
#include <QList>
#include <QString>

typedef struct _GstElement GstElement;

namespace lightning::wincap {

/// One capturable top-level window, as offered in the share picker.
struct WindowInfo {
    /// The HWND, widened so it can cross into QML and a GObject property
    /// without a Windows type leaking into either.
    quint64 handle = 0;
    /// The window's own caption.
    QString title;
    /// WHICH APPLICATION it belongs to, and it is not decoration. Chromium
    /// browsers put only the tab's title in the caption, so a Brave window
    /// offered by its title alone says nothing about being a browser at all
    /// — reported as "with brave it listed my tab name but didnt even say
    /// brave anywhere". Taken from the executable's VERSIONINFO description,
    /// which is the name Task Manager shows and therefore the one the user
    /// already associates with the window; the executable's own base name is
    /// the fallback. Empty when neither can be read.
    QString application;
    /// The window's VISIBLE size — the frame DWM actually paints, which on
    /// Windows 10/11 is smaller than the window rect by an invisible resize
    /// border. It is the size the CAPTURE delivers, from the same helper, so
    /// a row in the picker cannot advertise a shape the share does not send.
    int width = 0;
    int height = 0;
};

/// A frame size, so the fitting rule below can be tested without Windows.
struct Size {
    int width = 0;
    int height = 0;
};

/// The largest size no larger than `maxW` x `maxH` that keeps `srcW`:`srcH`,
/// with both edges even.
///
/// EVEN because VP8 encodes in 16x16 macroblocks over chroma subsampled by
/// two; an odd edge reaches `videoconvert` and costs a copy per frame to fix
/// something we can simply never produce.
///
/// NEVER UPSCALES. A window smaller than the ceiling is published at its own
/// size; enlarging it spends bitrate on invented pixels and makes text
/// softer, not sharper.
///
/// KEEPS THE ASPECT, which the element used to leave to a pair of independent
/// clamps: a 3840x2100 window fixated to 1920x1080 is a 16:9 rectangle
/// holding a 1.83:1 picture, so everything in it is stretched. Returns a zero
/// size for a degenerate input rather than inventing one.
///
/// Compiled on every platform deliberately. This is the rule that decides
/// what resolution a shared window is published at, and it is the one part of
/// a Windows-only file that a test on any machine can hold to account.
Size fitInto(int srcW, int srcH, int maxW, int maxH);

/// Whether this build can capture a window at all. False everywhere but
/// Windows, so the picker offers displays only and says so.
bool available();

/// Top-level windows a person would recognise: visible, non-minimised, titled,
/// on the taskbar, and never one of Lightning's own — sharing the call window
/// into the call is a hall of mirrors, and offering it invites the mistake.
QList<WindowInfo> enumerateWindows();

/// A still of one window, for the share picker's preview tile.
///
/// Same PrintWindow path the capture uses, so what the tile shows is what
/// the call would send — including, honestly, a blank one for a window that
/// refuses to print. A preview that flattered the capture would be worse
/// than none: the whole point is to let someone confirm what they are about
/// to broadcast BEFORE they broadcast it.
///
/// Scaled so the longest edge is at most `maxEdge`. Returns a null image
/// when the window is gone or cannot be drawn; the picker then shows its
/// glyph, which is the pre-preview behaviour.
QImage captureThumbnail(quint64 handle, int maxEdge);

/// The same, for a whole display. `displayIndex` is the index Windows itself
/// reports the monitor at — see `displayForDeviceName`, which is the only way
/// a caller should obtain one.
QImage captureScreenThumbnail(int displayIndex, int maxEdge);

/// Resolve a display by the platform's own device name (`\\.\DISPLAY1`), as
/// `QScreen::name()` reports it on Windows.
///
/// WHY THIS EXISTS. Three enumerations were being treated as one: Qt's screen
/// list, the order `EnumDisplayMonitors` walks, and whatever
/// `gdiscreencapsrc`'s `monitor` property counts. Nothing maps between them,
/// so on a multi-monitor machine the row a user picked could name one
/// display, preview a second and capture a third — the same class of failure
/// the picker was built to close. Matching on the device NAME is exact, and
/// it also yields the monitor's real framebuffer rectangle, which is what
/// gets captured and therefore the only honest thing to put on the row.
///
/// Returns false off Windows and for a name Windows does not know; the caller
/// then falls back to Qt's own index and geometry.
bool displayForDeviceName(const QString &deviceName, int *index, int *width,
                          int *height);

/// Register `lightningwindowcapturesrc`. Idempotent, safe from any thread,
/// must run after gst_init. A no-op off Windows.
void registerWindowCaptureSrc();

/// The element name to use in a pipeline description.
const char *windowCaptureSrcName();

} // namespace lightning::wincap
