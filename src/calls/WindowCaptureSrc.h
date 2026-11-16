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
//
// On an X11 session (the fallback when there is no ScreenCast portal) the
// same element reads the window's composited pixmap, after redirecting it so
// an uncomposited desktop keeps its contents too.
#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

typedef struct _GstElement GstElement;

namespace lightning::wincap {

struct WindowInfo {
    quint64 handle = 0;
    QString title;
    QString application;
    quint64 pid = 0;
    QString executable;
    int width = 0;
    int height = 0;
};

struct Size {
    int width = 0;
    int height = 0;
};

Size fitInto(int srcW, int srcH, int maxW, int maxH);

struct X11WindowFacts {
    bool viewable = false;
    bool hidden = false;
    bool skipTaskbar = false;
    bool normalType = true;
    bool ours = false;
    int width = 0;
    int height = 0;
    QString title;
};

/// Same rules as the Windows list: top-level, on screen, not minimised, not
/// a tool window, titled, and not one of ours.
bool x11WindowListed(const X11WindowFacts &facts);

bool available();

struct ProcessInfo {
    QString executablePath;
    qint64 startTime = 0;
    bool running = false;
};
ProcessInfo processInfo(quint64 pid);
QString applicationNameForExecutablePath(const QString &path);

QList<WindowInfo> enumerateWindows();

QImage captureThumbnail(quint64 handle, int maxEdge);

QImage captureScreenThumbnail(int displayIndex, int maxEdge);

/// X11: a root-window area in native pixels.
QImage captureAreaThumbnail(const QRect &area, int maxEdge);

/// X11: libxcb and libxcb-composite are dlopen'd; window sharing is offered
/// only when every symbol resolves.
using X11SymbolResolver = void *(*)(const char *library, const char *symbol);
bool x11LibrariesLoaded();
/// The first library that failed to load, empty when all did.
QString x11MissingLibrary();
/// The `--call-media-status` answer.
QString x11WindowSharingStatus(bool built, bool x11Session,
                               const QString &missingLibrary);
/// Test seam: replaces dlopen/dlsym (nullptr restores them) and forgets what
/// was resolved.
void setX11SymbolResolverForTest(X11SymbolResolver resolver);

bool displayForDeviceName(const QString &deviceName, int *index, int *width,
                          int *height);

void registerWindowCaptureSrc();

const char *windowCaptureSrcName();

}
