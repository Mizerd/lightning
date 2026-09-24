// Single-window capture on Windows, as a GStreamer element of our own.
//
// gdiscreencapsrc, the only capture element we ship there, has no window
// property. d3d11screencapturesrc does, but libgstd3d11.dll is not shipped:
// the upstream SDK is a UCRT build whose libstdc++ imports do not match this
// msvcrt toolchain (docs/windows-packaging.md). Like lightningrtpvp8pay, the
// element is compiled into the binary; it is plain Win32 GDI and adds no
// dependency.
//
// It asks the window to draw itself (PrintWindow with PW_RENDERFULLCONTENT)
// rather than cropping the screen, which would also share anything stacked on
// top of it (another app, a password prompt). Limitation: a window rendering
// through its own swapchain may print blank; that is reported as a black
// frame, not a failure to start.
#pragma once

#include <QImage>
#include <QList>
#include <QString>

typedef struct _GstElement GstElement;

namespace lightning::wincap {

/// One capturable top-level window, as offered in the share picker.
struct WindowInfo {
    /// The HWND, widened so no Windows type crosses into QML or GObject.
    quint64 handle = 0;
    /// The window's own caption.
    QString title;
    /// The owning application, from the executable's VERSIONINFO description
    /// (as Task Manager shows it), falling back to its base name; empty when
    /// neither can be read. Needed because browser captions are just the tab
    /// title.
    QString application;
    /// The window's visible size (the frame DWM paints, excluding the invisible
    /// resize border), computed by the same helper as the capture.
    int width = 0;
    int height = 0;
};

/// A frame size, so the fitting rule can be tested without Windows.
struct Size {
    int width = 0;
    int height = 0;
};

/// The largest size within `maxW` x `maxH` that keeps `srcW`:`srcH`, with
/// both edges even (VP8 chroma subsampling). Never upscales. Returns a zero
/// size for degenerate input. Compiled on every platform so it is testable.
Size fitInto(int srcW, int srcH, int maxW, int maxH);

/// Whether this build can capture a window at all (Windows only).
bool available();

/// Top-level windows a person would recognise: visible, not minimised,
/// titled, on the taskbar, and never Lightning's own.
QList<WindowInfo> enumerateWindows();

/// A still of one window for the picker's preview, via the same PrintWindow
/// path as the capture, so it shows what would be sent (blank included).
/// Longest edge at most `maxEdge`; a null image when the window is gone or
/// cannot be drawn.
QImage captureThumbnail(quint64 handle, int maxEdge);

/// The same for a whole display. Obtain `displayIndex` from
/// displayForDeviceName().
QImage captureScreenThumbnail(int displayIndex, int maxEdge);

/// Resolves a display by its device name (`\\.\DISPLAY1`, as QScreen::name()
/// reports it). Qt's screen order, EnumDisplayMonitors and gdiscreencapsrc's
/// `monitor` index are independent enumerations; matching the name is exact
/// and also yields the real framebuffer size. Returns false off Windows or
/// for an unknown name.
bool displayForDeviceName(const QString &deviceName, int *index, int *width,
                          int *height);

/// Registers `lightningwindowcapturesrc`. Idempotent, thread-safe, must run
/// after gst_init; a no-op off Windows.
void registerWindowCaptureSrc();

/// The element name to use in a pipeline description.
const char *windowCaptureSrcName();

} // namespace lightning::wincap
