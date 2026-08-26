#include "calls/WindowCaptureSrc.h"

#ifdef Q_OS_WIN

#include <algorithm>
#include <cstring>
#include <mutex>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include <windows.h>

namespace {

// PW_RENDERFULLCONTENT asks the window to render its whole content even where
// another window covers it. Declared here because the mingw headers guard it
// behind a newer _WIN32_WINNT than the rest of the build needs, and raising
// that for one constant would change every other header in the project.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// The capture rate. FIXED, and deliberately not derived from the window.
//
// A desktop capture on Linux delivers ON DAMAGE, which is what makes
// `videorate` hold the first buffer until something moves — the startup
// stall documented in §16. This source runs on its own clock and always
// produces a frame, so a perfectly still window still streams and there is
// no hold to wait out.
constexpr int kFramerate = 30;
constexpr GstClockTime kFrameDuration = GST_SECOND / kFramerate;

struct LightningWindowCaptureSrc {
    GstPushSrc parent;

    /// The HWND, as a plain integer property so nothing Windows-shaped has to
    /// cross a GObject boundary.
    guint64 hwnd;

    /// NEGOTIATED ONCE, then held for the life of the element.
    ///
    /// The encoder and the SFU agreed a resolution when the share started; a
    /// window the user then resizes must not renegotiate underneath them, so
    /// later frames are scaled into this size instead. Capture caps that
    /// change mid-stream are how a share turns into a stall.
    gint width;
    gint height;

    HDC memoryDc;
    HBITMAP bitmap;
    void *pixels;          // BGRA, top-down, owned by `bitmap`
    GstClockTime nextPts;
    gboolean windowGone;
};

struct LightningWindowCaptureSrcClass {
    GstPushSrcClass parent;
};

enum { PROP_0, PROP_HWND };

#define LIGHTNING_TYPE_WINDOW_CAPTURE_SRC \
    (lightning_window_capture_src_get_type())

G_DEFINE_TYPE(LightningWindowCaptureSrc, lightning_window_capture_src,
              GST_TYPE_PUSH_SRC)

GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)BGRA, "
                    "width=(int)[1,16384], height=(int)[1,16384], "
                    "framerate=(fraction)30/1"));

GST_DEBUG_CATEGORY_STATIC(lightning_wincap_debug);
#define GST_CAT_DEFAULT lightning_wincap_debug

/// The window's CLIENT area, rounded to even.
///
/// Even because VP8 encodes in 16x16 macroblocks and chroma is subsampled by
/// two; an odd width reaches `videoconvert` and costs a copy per frame to fix
/// something we can simply never produce.
bool clientSize(HWND window, gint *width, gint *height)
{
    RECT rect{};
    if (!GetClientRect(window, &rect))
        return false;
    const LONG w = rect.right - rect.left;
    const LONG h = rect.bottom - rect.top;
    if (w < 2 || h < 2)
        return false;
    *width = static_cast<gint>(w & ~1L);
    *height = static_cast<gint>(h & ~1L);
    return true;
}

void releaseSurface(LightningWindowCaptureSrc *self)
{
    if (self->bitmap) {
        DeleteObject(self->bitmap);
        self->bitmap = nullptr;
    }
    if (self->memoryDc) {
        DeleteDC(self->memoryDc);
        self->memoryDc = nullptr;
    }
    self->pixels = nullptr;
}

/// A top-down 32-bit DIB we own, so a frame is a straight memcpy out.
bool createSurface(LightningWindowCaptureSrc *self)
{
    releaseSurface(self);
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    self->memoryDc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = self->width;
    // NEGATIVE height means top-down, which is the order video/x-raw wants.
    // A bottom-up DIB would need the rows reversed on every single frame.
    info.bmiHeader.biHeight = -self->height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    self->bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                    &self->pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!self->memoryDc || !self->bitmap || !self->pixels) {
        releaseSurface(self);
        return false;
    }
    SelectObject(self->memoryDc, self->bitmap);
    return true;
}

/// Draw the window into our DIB. Returns false only when the window is gone.
bool paintWindow(LightningWindowCaptureSrc *self)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!IsWindow(window))
        return false;

    // Clear first. PrintWindow may leave regions untouched for a window that
    // is partly unrendered, and stale pixels from the previous frame there
    // look like tearing rather than like the blank they are.
    RECT full{0, 0, self->width, self->height};
    FillRect(self->memoryDc, &full,
             static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    gint currentW = 0;
    gint currentH = 0;
    const bool sized = clientSize(window, &currentW, &currentH);

    if (sized && currentW == self->width && currentH == self->height) {
        // Same size as negotiated: print straight into the DIB.
        if (PrintWindow(window, self->memoryDc, PW_RENDERFULLCONTENT))
            return true;
        // Fall through to the DC copy below rather than emitting black: some
        // windows refuse PrintWindow and copy fine.
        HDC windowDc = GetDC(window);
        if (windowDc) {
            BitBlt(self->memoryDc, 0, 0, self->width, self->height, windowDc,
                   0, 0, SRCCOPY);
            ReleaseDC(window, windowDc);
        }
        return true;
    }

    // The window was RESIZED after the share started. Print at its real size
    // and scale into the negotiated one, so the stream's resolution — which
    // the encoder and the SFU already agreed — never changes.
    if (!sized)
        return true;   // minimised or degenerate: a black frame, not an EOS.

    HDC screen = GetDC(nullptr);
    if (!screen)
        return true;
    HDC scratchDc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = currentW;
    info.bmiHeader.biHeight = -currentH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *scratchPixels = nullptr;
    HBITMAP scratch = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                       &scratchPixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (scratchDc && scratch) {
        SelectObject(scratchDc, scratch);
        if (!PrintWindow(window, scratchDc, PW_RENDERFULLCONTENT)) {
            HDC windowDc = GetDC(window);
            if (windowDc) {
                BitBlt(scratchDc, 0, 0, currentW, currentH, windowDc, 0, 0,
                       SRCCOPY);
                ReleaseDC(window, windowDc);
            }
        }
        // Letterbox rather than stretch: a resized window that changes aspect
        // must not have faces or text squashed to fit the old rectangle.
        const double scale =
            (std::min)(static_cast<double>(self->width) / currentW,
                       static_cast<double>(self->height) / currentH);
        const int drawW = (std::max)(1, static_cast<int>(currentW * scale));
        const int drawH = (std::max)(1, static_cast<int>(currentH * scale));
        SetStretchBltMode(self->memoryDc, HALFTONE);
        SetBrushOrgEx(self->memoryDc, 0, 0, nullptr);
        StretchBlt(self->memoryDc, (self->width - drawW) / 2,
                   (self->height - drawH) / 2, drawW, drawH, scratchDc, 0, 0,
                   currentW, currentH, SRCCOPY);
    }
    if (scratch)
        DeleteObject(scratch);
    if (scratchDc)
        DeleteDC(scratchDc);
    return true;
}

gboolean startSrc(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!self->hwnd || !IsWindow(window)) {
        // NAMED, not silent. "Screen sharing couldn't start" with nothing
        // behind it is the failure mode this whole lane has been paying for.
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share is not available."),
                          ("no such window handle"));
        return FALSE;
    }
    if (!clientSize(window, &self->width, &self->height)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share has no visible area."),
                          ("client rect is empty or minimised"));
        return FALSE;
    }
    if (!createSurface(self)) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                          ("Could not prepare the window capture."),
                          ("CreateDIBSection failed"));
        return FALSE;
    }
    self->nextPts = 0;
    self->windowGone = FALSE;
    GST_INFO_OBJECT(self, "window capture started %dx%d", self->width,
                    self->height);
    return TRUE;
}

gboolean stopSrc(GstBaseSrc *base)
{
    releaseSurface(reinterpret_cast<LightningWindowCaptureSrc *>(base));
    return TRUE;
}

GstCaps *fixateSrc(GstBaseSrc *base, GstCaps *caps)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    caps = gst_caps_make_writable(caps);
    GstStructure *structure = gst_caps_get_structure(caps, 0);
    gst_structure_fixate_field_nearest_int(structure, "width", self->width);
    gst_structure_fixate_field_nearest_int(structure, "height", self->height);
    gst_structure_fixate_field_nearest_fraction(structure, "framerate",
                                                kFramerate, 1);
    return GST_BASE_SRC_CLASS(lightning_window_capture_src_parent_class)
        ->fixate(base, caps);
}

GstFlowReturn createFrame(GstPushSrc *push, GstBuffer **out)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(push);
    if (self->windowGone)
        return GST_FLOW_EOS;

    if (!paintWindow(self)) {
        // The window CLOSED. End the stream cleanly rather than erroring:
        // the user closing what they were sharing is a normal thing to do,
        // and an error here would tear down the whole call.
        GST_INFO_OBJECT(self, "shared window closed; ending capture");
        self->windowGone = TRUE;
        return GST_FLOW_EOS;
    }

    const gsize size = static_cast<gsize>(self->width) * self->height * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer)
        return GST_FLOW_ERROR;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    // GdiFlush before reading a DIB the GDI batch may not have written yet.
    // Without it the first frames of a share can be torn or empty.
    GdiFlush();
    memcpy(map.data, self->pixels, size);
    gst_buffer_unmap(buffer, &map);

    GST_BUFFER_PTS(buffer) = self->nextPts;
    GST_BUFFER_DTS(buffer) = self->nextPts;
    GST_BUFFER_DURATION(buffer) = kFrameDuration;
    self->nextPts += kFrameDuration;
    *out = buffer;
    return GST_FLOW_OK;
}

void setProperty(GObject *object, guint id, const GValue *value,
                 GParamSpec *spec)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(object);
    if (id == PROP_HWND)
        self->hwnd = g_value_get_uint64(value);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(object);
    if (id == PROP_HWND)
        g_value_set_uint64(value, self->hwnd);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

void lightning_window_capture_src_class_init(
    LightningWindowCaptureSrcClass *klass)
{
    auto *object = G_OBJECT_CLASS(klass);
    auto *element = GST_ELEMENT_CLASS(klass);
    auto *base = GST_BASE_SRC_CLASS(klass);
    auto *push = GST_PUSH_SRC_CLASS(klass);

    object->set_property = setProperty;
    object->get_property = getProperty;
    g_object_class_install_property(
        object, PROP_HWND,
        g_param_spec_uint64("hwnd", "Window handle",
                            "HWND of the window to capture", 0, G_MAXUINT64, 0,
                            static_cast<GParamFlags>(G_PARAM_READWRITE
                                                     | G_PARAM_STATIC_STRINGS)));

    gst_element_class_add_static_pad_template(element, &kSrcTemplate);
    gst_element_class_set_static_metadata(
        element, "Lightning window capture", "Source/Video",
        "Captures a single window by asking it to render itself, so nothing "
        "stacked on top of it is shared",
        "Lightning");

    base->start = startSrc;
    base->stop = stopSrc;
    base->fixate = fixateSrc;
    push->create = createFrame;
}

void lightning_window_capture_src_init(LightningWindowCaptureSrc *self)
{
    self->hwnd = 0;
    self->width = 0;
    self->height = 0;
    self->memoryDc = nullptr;
    self->bitmap = nullptr;
    self->pixels = nullptr;
    self->nextPts = 0;
    self->windowGone = FALSE;
    // LIVE, so the pipeline treats it as a capture: it must not be asked to
    // produce the backlog a non-live source would owe after a pause.
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}

struct EnumContext {
    QList<lightning::wincap::WindowInfo> *out;
    DWORD ownProcess;
};

BOOL CALLBACK enumProc(HWND window, LPARAM param)
{
    auto *ctx = reinterpret_cast<EnumContext *>(param);

    if (!IsWindowVisible(window) || IsIconic(window))
        return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;   // a dialog or tool window belonging to another

    // NEVER our own windows. Sharing the call into the call is a hall of
    // mirrors, and a picker that offers it invites exactly that mistake.
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == ctx->ownProcess)
        return TRUE;

    const LONG exStyle = GetWindowLong(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return TRUE;   // not on the taskbar, so not something a user picks

    // Cloaked windows are the ones that make a picker embarrassing: UWP
    // shells and background tabs report visible while showing nothing.
    //
    // Resolved at RUNTIME rather than linked. Linking dwmapi would add an
    // import to the Windows link line and therefore a new edge in a
    // packaging closure that is validated symbol by symbol; this asks for
    // one function and, if it is not there, simply keeps the window. A
    // slightly generous list is a far better failure than a new dependency
    // in the artifact.
    using DwmGetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, PVOID,
                                                      DWORD);
    static DwmGetWindowAttributeFn dwmGetWindowAttribute = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmGetWindowAttributeFn>(
                   reinterpret_cast<void *>(
                       GetProcAddress(dwm, "DwmGetWindowAttribute")))
                   : nullptr;
    }();
    constexpr DWORD kDwmwaCloaked = 14;   // DWMWA_CLOAKED
    if (dwmGetWindowAttribute) {
        BOOL cloaked = FALSE;
        if (SUCCEEDED(dwmGetWindowAttribute(window, kDwmwaCloaked, &cloaked,
                                            sizeof(cloaked)))
            && cloaked)
            return TRUE;
    }

    wchar_t title[512];
    const int length = GetWindowTextW(window, title, 512);
    if (length <= 0)
        return TRUE;   // untitled: nothing to show a person in a list

    RECT rect{};
    if (!GetClientRect(window, &rect))
        return TRUE;
    const LONG w = rect.right - rect.left;
    const LONG h = rect.bottom - rect.top;
    if (w < 2 || h < 2)
        return TRUE;

    lightning::wincap::WindowInfo info;
    info.handle = static_cast<quint64>(reinterpret_cast<uintptr_t>(window));
    info.title = QString::fromWCharArray(title, length);
    info.width = static_cast<int>(w);
    info.height = static_cast<int>(h);
    ctx->out->append(info);
    return TRUE;
}

} // namespace

namespace lightning::wincap {

bool available() { return true; }

namespace {

/// Copy a device context region into a QImage, scaled to fit `maxEdge`.
///
/// One helper for both preview kinds so a window and a screen cannot end up
/// with different colour handling or a different scaling rule.
QImage grabToImage(HDC source, int x, int y, int width, int height,
                   int maxEdge)
{
    if (width < 2 || height < 2 || !source)
        return {};
    HDC memory = CreateCompatibleDC(source);
    if (!memory)
        return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   // top-down, as QImage wants
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits,
                                      nullptr, 0);
    QImage out;
    if (bitmap && bits) {
        SelectObject(memory, bitmap);
        BitBlt(memory, 0, 0, width, height, source, x, y, SRCCOPY);
        GdiFlush();
        // COPIED, not wrapped: the DIB dies with this function and a QImage
        // sharing its memory would be a dangling read the moment it is drawn.
        out = QImage(reinterpret_cast<const uchar *>(bits), width, height,
                     width * 4, QImage::Format_RGB32)
                  .copy();
    }
    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(memory);
    if (out.isNull())
        return {};
    return out.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

struct MonitorHunt {
    int wanted;
    int seen;
    RECT rect;
    bool found;
};

BOOL CALLBACK monitorProc(HMONITOR, HDC, LPRECT rect, LPARAM param)
{
    auto *hunt = reinterpret_cast<MonitorHunt *>(param);
    if (hunt->seen++ == hunt->wanted) {
        hunt->rect = *rect;
        hunt->found = true;
        return FALSE;   // stop: we have the one we were asked for
    }
    return TRUE;
}

} // namespace

QImage captureThumbnail(quint64 handle, int maxEdge)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
    if (!handle || !IsWindow(window) || IsIconic(window))
        return {};
    RECT rect{};
    if (!GetClientRect(window, &rect))
        return {};
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    if (w < 2 || h < 2)
        return {};

    HDC screen = GetDC(nullptr);
    if (!screen)
        return {};
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits,
                                      nullptr, 0);
    ReleaseDC(nullptr, screen);
    QImage out;
    if (memory && bitmap && bits) {
        SelectObject(memory, bitmap);
        // THE SAME CALL THE CAPTURE MAKES. A preview drawn a different way
        // could show a picture the share cannot actually send.
        if (!PrintWindow(window, memory, PW_RENDERFULLCONTENT)) {
            HDC windowDc = GetDC(window);
            if (windowDc) {
                BitBlt(memory, 0, 0, w, h, windowDc, 0, 0, SRCCOPY);
                ReleaseDC(window, windowDc);
            }
        }
        GdiFlush();
        out = QImage(reinterpret_cast<const uchar *>(bits), w, h, w * 4,
                     QImage::Format_RGB32)
                  .copy();
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (memory)
        DeleteDC(memory);
    if (out.isNull())
        return {};
    return out.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

QImage captureScreenThumbnail(int displayIndex, int maxEdge)
{
    MonitorHunt hunt{displayIndex < 0 ? 0 : displayIndex, 0, {}, false};
    EnumDisplayMonitors(nullptr, nullptr, monitorProc,
                        reinterpret_cast<LPARAM>(&hunt));
    if (!hunt.found)
        return {};
    HDC screen = GetDC(nullptr);
    if (!screen)
        return {};
    // The virtual desktop's own coordinates: a second monitor starts at a
    // non-zero x, and grabbing from 0,0 would preview the wrong screen.
    const QImage out = grabToImage(screen, hunt.rect.left, hunt.rect.top,
                                   hunt.rect.right - hunt.rect.left,
                                   hunt.rect.bottom - hunt.rect.top, maxEdge);
    ReleaseDC(nullptr, screen);
    return out;
}

QList<WindowInfo> enumerateWindows()
{
    QList<WindowInfo> windows;
    EnumContext ctx{&windows, GetCurrentProcessId()};
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
    return windows;
}

void registerWindowCaptureSrc()
{
    static std::once_flag once;
    std::call_once(once, [] {
        GST_DEBUG_CATEGORY_INIT(lightning_wincap_debug,
                                "lightningwindowcapture", 0,
                                "Lightning window capture");
        gst_element_register(nullptr, windowCaptureSrcName(), GST_RANK_NONE,
                             LIGHTNING_TYPE_WINDOW_CAPTURE_SRC);
    });
}

const char *windowCaptureSrcName() { return "lightningwindowcapturesrc"; }

} // namespace lightning::wincap

#else // !Q_OS_WIN

namespace lightning::wincap {

// Off Windows this is not "unimplemented", it is "not the mechanism". Linux
// has the xdg portal, which owns the picker AND hands back a PipeWire node
// for exactly what was chosen; macOS captures a display through avfvideosrc.
// Reporting unavailable keeps the picker honest on both.
bool available() { return false; }
QList<WindowInfo> enumerateWindows() { return {}; }
QImage captureThumbnail(quint64, int) { return {}; }
QImage captureScreenThumbnail(int, int) { return {}; }
void registerWindowCaptureSrc() {}
const char *windowCaptureSrcName() { return "lightningwindowcapturesrc"; }

} // namespace lightning::wincap

#endif
