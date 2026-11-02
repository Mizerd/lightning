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

bool displayForDeviceName(const QString &deviceName, int *index, int *width,
                          int *height);

void registerWindowCaptureSrc();

const char *windowCaptureSrcName();

}
