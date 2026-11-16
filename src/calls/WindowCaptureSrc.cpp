#include "calls/WindowCaptureSrc.h"

#include <algorithm>

namespace lightning::wincap {

Size fitInto(int srcW, int srcH, int maxW, int maxH)
{
    if (srcW < 2 || srcH < 2 || maxW < 2 || maxH < 2)
        return {};
    const double scale =
        (std::min)({1.0, static_cast<double>(maxW) / srcW,
                    static_cast<double>(maxH) / srcH});
    const int width = static_cast<int>(srcW * scale) & ~1;
    const int height = static_cast<int>(srcH * scale) & ~1;
    if (width < 2 || height < 2)
        return {};
    return {width, height};
}

bool x11WindowListed(const X11WindowFacts &facts)
{
    return facts.viewable && !facts.hidden && !facts.skipTaskbar
        && facts.normalType && !facts.ours && facts.width >= 2
        && facts.height >= 2 && !facts.title.trimmed().isEmpty();
}

#ifndef LIGHTNING_HAVE_X11_WINDOW_CAPTURE
QImage captureAreaThumbnail(const QRect &, int) { return {}; }
#endif

}

#ifdef Q_OS_WIN

#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include <windows.h>

namespace {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

constexpr int kFramerate = 30;
constexpr GstClockTime kFrameDuration = GST_SECOND / kFramerate;

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previous = nullptr;
    void *pixels = nullptr;
};

struct LightningWindowCaptureSrc {
    GstPushSrc parent;

    guint64 hwnd;

    /// The negotiated frame size, learned in `set_caps`. Buffers must match
    /// the caps: downstream reads them at the stride the caps imply, so a
    /// window-sized buffer under smaller caps is read as garbage. Fixed for
    /// the element's life; a resized window is fitted into it rather than
    /// renegotiated.
    gint outWidth;
    gint outHeight;
    Surface out;

    gint printWidth;
    gint printHeight;
    Surface print;

    gint startWidth;
    gint startHeight;

    guint64 frameIndex;
    GstClockTime pacingBase;
    gboolean pacingStarted;
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

// -------------------------------------------------------------- geometry --

using DwmGetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);

DwmGetWindowAttributeFn dwmGetWindowAttribute()
{
    static DwmGetWindowAttributeFn fn = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmGetWindowAttributeFn>(
                   reinterpret_cast<void *>(
                       GetProcAddress(dwm, "DwmGetWindowAttribute")))
                   : nullptr;
    }();
    return fn;
}

constexpr DWORD kDwmwaExtendedFrameBounds = 9;
constexpr DWORD kDwmwaCloaked = 14;

struct WindowGeometry {
    int printWidth = 0;
    int printHeight = 0;
    int cropX = 0;
    int cropY = 0;
    int cropWidth = 0;
    int cropHeight = 0;
};

bool windowGeometry(HWND window, WindowGeometry *out)
{
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect))
        return false;
    const int fullWidth = static_cast<int>(windowRect.right - windowRect.left);
    const int fullHeight =
        static_cast<int>(windowRect.bottom - windowRect.top);
    if (fullWidth < 2 || fullHeight < 2)
        return false;

    RECT visible = windowRect;
    RECT frame{};
    if (dwmGetWindowAttribute()
        && SUCCEEDED(dwmGetWindowAttribute()(window, kDwmwaExtendedFrameBounds,
                                             &frame, sizeof(frame)))
        && frame.right > frame.left && frame.bottom > frame.top) {
        visible.left = (std::max)(windowRect.left, frame.left);
        visible.top = (std::max)(windowRect.top, frame.top);
        visible.right = (std::min)(windowRect.right, frame.right);
        visible.bottom = (std::min)(windowRect.bottom, frame.bottom);
    }
    int cropWidth = static_cast<int>(visible.right - visible.left) & ~1;
    int cropHeight = static_cast<int>(visible.bottom - visible.top) & ~1;
    if (cropWidth < 2 || cropHeight < 2) {
        visible = windowRect;
        cropWidth = fullWidth & ~1;
        cropHeight = fullHeight & ~1;
    }
    if (cropWidth < 2 || cropHeight < 2)
        return false;

    out->printWidth = fullWidth;
    out->printHeight = fullHeight;
    out->cropX = static_cast<int>(visible.left - windowRect.left);
    out->cropY = static_cast<int>(visible.top - windowRect.top);
    out->cropWidth = cropWidth;
    out->cropHeight = cropHeight;
    return true;
}

bool createSurface(int width, int height, Surface *out)
{
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    out->dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    out->bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &out->pixels,
                                   nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!out->dc || !out->bitmap || !out->pixels) {
        if (out->bitmap)
            DeleteObject(out->bitmap);
        if (out->dc)
            DeleteDC(out->dc);
        *out = Surface{};
        return false;
    }
    out->previous =
        static_cast<HBITMAP>(SelectObject(out->dc, out->bitmap));
    return true;
}

void releaseSurface(Surface *surface)
{
    if (surface->dc && surface->previous)
        SelectObject(surface->dc, surface->previous);
    if (surface->bitmap)
        DeleteObject(surface->bitmap);
    if (surface->dc)
        DeleteDC(surface->dc);
    *surface = Surface{};
}

void fillBlack(HDC dc, int width, int height)
{
    RECT full{0, 0, width, height};
    FillRect(dc, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

using IsHungAppWindowFn = BOOL(WINAPI *)(HWND);

IsHungAppWindowFn isHungAppWindow()
{
    static IsHungAppWindowFn fn = [] {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        return user ? reinterpret_cast<IsHungAppWindowFn>(
                   reinterpret_cast<void *>(
                       GetProcAddress(user, "IsHungAppWindow")))
                    : nullptr;
    }();
    return fn;
}

/// Asks the window to draw itself at its window-rect size. Cleared first so
/// regions PrintWindow leaves untouched are black, not stale.
///
/// A hung window is never asked: PrintWindow waits on the window's message
/// queue with no timeout, which would stall the capture (or the picker)
/// without an error.
void printInto(HWND window, const Surface &surface, int width, int height)
{
    fillBlack(surface.dc, width, height);
    if (isHungAppWindow() && isHungAppWindow()(window))
        return;
    if (PrintWindow(window, surface.dc, PW_RENDERFULLCONTENT))
        return;
    // Only when PrintWindow reports failure. A window that prints nothing
    // (e.g. its own swapchain) is left black: reading pixels from anywhere but
    // the window's own PrintWindow output risks leaking another window, and a
    // black share is the safe failure.
    //
    // GetWindowDC, not GetDC: the surface is window-rect sized, and GetDC is
    // client-area relative.
    HDC windowDc = GetWindowDC(window);
    if (windowDc) {
        BitBlt(surface.dc, 0, 0, width, height, windowDc, 0, 0, SRCCOPY);
        ReleaseDC(window, windowDc);
    }
}

bool valueAdmits(const GValue *value, int wanted)
{
    if (!value)
        return true;
    if (G_VALUE_HOLDS_INT(value))
        return g_value_get_int(value) == wanted;
    if (GST_VALUE_HOLDS_INT_RANGE(value)) {
        return wanted >= gst_value_get_int_range_min(value)
            && wanted <= gst_value_get_int_range_max(value);
    }
    if (GST_VALUE_HOLDS_LIST(value)) {
        for (guint i = 0; i < gst_value_list_get_size(value); ++i) {
            if (valueAdmits(gst_value_list_get_value(value, i), wanted))
                return true;
        }
        return false;
    }
    return true;
}

int valueMax(const GValue *value, int fallback)
{
    if (!value)
        return fallback;
    if (G_VALUE_HOLDS_INT(value))
        return g_value_get_int(value);
    if (GST_VALUE_HOLDS_INT_RANGE(value))
        return gst_value_get_int_range_max(value);
    if (GST_VALUE_HOLDS_LIST(value)) {
        int best = 0;
        for (guint i = 0; i < gst_value_list_get_size(value); ++i)
            best = (std::max)(best,
                              valueMax(gst_value_list_get_value(value, i), 0));
        return best > 0 ? best : fallback;
    }
    return fallback;
}

bool paintWindow(LightningWindowCaptureSrc *self)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!IsWindow(window))
        return false;

    WindowGeometry geo;
    if (IsIconic(window) || !windowGeometry(window, &geo)) {
        fillBlack(self->out.dc, self->outWidth, self->outHeight);
        return true;
    }

    if (geo.printWidth != self->printWidth
        || geo.printHeight != self->printHeight || !self->print.pixels) {
        releaseSurface(&self->print);
        if (!createSurface(geo.printWidth, geo.printHeight, &self->print)) {
            self->printWidth = 0;
            self->printHeight = 0;
            fillBlack(self->out.dc, self->outWidth, self->outHeight);
            return true;
        }
        self->printWidth = geo.printWidth;
        self->printHeight = geo.printHeight;
        GST_INFO_OBJECT(self,
                        "shared window resized to %dx%d; publishing at %dx%d",
                        geo.cropWidth, geo.cropHeight, self->outWidth,
                        self->outHeight);
    }

    printInto(window, self->print, self->printWidth, self->printHeight);

    if (geo.cropWidth == self->outWidth && geo.cropHeight == self->outHeight) {
        BitBlt(self->out.dc, 0, 0, self->outWidth, self->outHeight,
               self->print.dc, geo.cropX, geo.cropY, SRCCOPY);
        return true;
    }

    const lightning::wincap::Size fit = lightning::wincap::fitInto(
        geo.cropWidth, geo.cropHeight, self->outWidth, self->outHeight);
    fillBlack(self->out.dc, self->outWidth, self->outHeight);
    if (fit.width < 2 || fit.height < 2)
        return true;
    SetStretchBltMode(self->out.dc, HALFTONE);
    SetBrushOrgEx(self->out.dc, 0, 0, nullptr);
    StretchBlt(self->out.dc, (self->outWidth - fit.width) / 2,
               (self->outHeight - fit.height) / 2, fit.width, fit.height,
               self->print.dc, geo.cropX, geo.cropY, geo.cropWidth,
               geo.cropHeight, SRCCOPY);
    return true;
}

void waitForFrameSlot(LightningWindowCaptureSrc *self)
{
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (!clock)
        return;
    const GstClockTime base = gst_element_get_base_time(GST_ELEMENT(self));
    const GstClockTime now = gst_clock_get_time(clock);
    if (!GST_CLOCK_TIME_IS_VALID(base) || !GST_CLOCK_TIME_IS_VALID(now)
        || now < base) {
        gst_object_unref(clock);
        return;
    }
    const GstClockTime running = now - base;
    if (!self->pacingStarted) {
        self->pacingStarted = TRUE;
        self->pacingBase = running;
    }
    if (self->pacingBase > running + GST_SECOND) {
        const GstClockTime span = self->frameIndex * kFrameDuration;
        self->pacingBase = running > span ? running - span : 0;
    }
    const guint64 elapsed =
        running > self->pacingBase ? (running - self->pacingBase) : 0;
    const guint64 dueIndex = elapsed / kFrameDuration;
    if (dueIndex > self->frameIndex)
        self->frameIndex = dueIndex;
    const GstClockTime due =
        self->pacingBase + self->frameIndex * kFrameDuration;
    if (due > running) {
        GstClockID id = gst_clock_new_single_shot_id(clock, base + due);
        gst_clock_id_wait(id, nullptr);
        gst_clock_id_unref(id);
    }
    gst_object_unref(clock);
}

gboolean startSrc(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!self->hwnd || !IsWindow(window)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share is not available."),
                          ("no such window handle"));
        return FALSE;
    }
    WindowGeometry geo;
    if (!windowGeometry(window, &geo)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share has no visible area."),
                          ("window rect is empty or minimised"));
        return FALSE;
    }
    self->startWidth = geo.cropWidth;
    self->startHeight = geo.cropHeight;
    self->frameIndex = 0;
    self->pacingBase = 0;
    self->pacingStarted = FALSE;
    self->windowGone = FALSE;
    GST_INFO_OBJECT(self, "window capture started, window is %dx%d",
                    self->startWidth, self->startHeight);
    return TRUE;
}

gboolean stopSrc(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    releaseSurface(&self->out);
    releaseSurface(&self->print);
    self->outWidth = 0;
    self->outHeight = 0;
    self->printWidth = 0;
    self->printHeight = 0;
    return TRUE;
}

GstCaps *fixateSrc(GstBaseSrc *base, GstCaps *caps)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    caps = gst_caps_make_writable(caps);

    // Prefer a structure that can carry the window's own size. A caps query
    // through videoconvertscale lists the downstream-restricted structure
    // first, so fixating structure 0 blindly would clamp to the ceiling and
    // leave the scaling to GDI instead of videoscale.
    const guint count = gst_caps_get_size(caps);
    guint pick = 0;
    for (guint i = 0; i < count; ++i) {
        const GstStructure *candidate = gst_caps_get_structure(caps, i);
        if (valueAdmits(gst_structure_get_value(candidate, "width"),
                        self->startWidth)
            && valueAdmits(gst_structure_get_value(candidate, "height"),
                           self->startHeight)) {
            pick = i;
            break;
        }
    }
    if (pick != 0) {
        GstCaps *reordered = gst_caps_new_empty();
        for (guint i = 0; i < count; ++i) {
            const guint from = i == 0 ? pick : (i <= pick ? i - 1 : i);
            GstCapsFeatures *features = gst_caps_get_features(caps, from);
            gst_caps_append_structure_full(
                reordered,
                gst_structure_copy(gst_caps_get_structure(caps, from)),
                features ? gst_caps_features_copy(features) : nullptr);
        }
        gst_caps_unref(caps);
        caps = reordered;
    }

    GstStructure *structure = gst_caps_get_structure(caps, 0);
    const lightning::wincap::Size want = lightning::wincap::fitInto(
        self->startWidth, self->startHeight,
        valueMax(gst_structure_get_value(structure, "width"),
                 self->startWidth),
        valueMax(gst_structure_get_value(structure, "height"),
                 self->startHeight));
    gst_structure_fixate_field_nearest_int(
        structure, "width", want.width > 0 ? want.width : self->startWidth);
    gst_structure_fixate_field_nearest_int(
        structure, "height",
        want.height > 0 ? want.height : self->startHeight);
    gst_structure_fixate_field_nearest_fraction(structure, "framerate",
                                                kFramerate, 1);
    // Fixate the pixel aspect ratio too. The downstream PAR pin makes
    // videoconvertscale offer an open PAR range here, and an unfixated range
    // falls to its minimum (1/2147483647), which overflows videoscale.
    // videotestsrc-based probes cannot show this: they fixate PAR themselves.
    gst_structure_fixate_field_nearest_fraction(structure,
                                                "pixel-aspect-ratio", 1, 1);
    return GST_BASE_SRC_CLASS(lightning_window_capture_src_parent_class)
        ->fixate(base, caps);
}

gboolean setCapsSrc(GstBaseSrc *base, GstCaps *caps)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        GST_ERROR_OBJECT(self, "window capture caps are not video caps");
        return FALSE;
    }
    const gint width = GST_VIDEO_INFO_WIDTH(&info);
    const gint height = GST_VIDEO_INFO_HEIGHT(&info);
    if (width < 2 || height < 2)
        return FALSE;
    if (width != self->outWidth || height != self->outHeight
        || !self->out.pixels) {
        releaseSurface(&self->out);
        if (!createSurface(width, height, &self->out)) {
            self->outWidth = 0;
            self->outHeight = 0;
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                              ("Could not prepare the window capture."),
                              ("CreateDIBSection failed for %dx%d", width,
                               height));
            return FALSE;
        }
        self->outWidth = width;
        self->outHeight = height;
    }
    GST_INFO_OBJECT(self,
                    "window capture publishing %dx%d from a %dx%d window",
                    width, height, self->startWidth, self->startHeight);
    return TRUE;
}

GstFlowReturn createFrame(GstPushSrc *push, GstBuffer **out)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(push);
    if (self->windowGone)
        return GST_FLOW_EOS;
    if (!self->out.pixels || self->outWidth < 2 || self->outHeight < 2) {
        GST_ERROR_OBJECT(self, "window capture asked for a frame before its "
                               "caps were set");
        return GST_FLOW_NOT_NEGOTIATED;
    }

    waitForFrameSlot(self);

    if (!paintWindow(self)) {
        GST_INFO_OBJECT(self, "shared window closed; ending capture");
        self->windowGone = TRUE;
        return GST_FLOW_EOS;
    }

    const gsize size =
        static_cast<gsize>(self->outWidth) * self->outHeight * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer)
        return GST_FLOW_ERROR;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    GdiFlush();
    memcpy(map.data, self->out.pixels, size);
    gst_buffer_unmap(buffer, &map);
    // Attach video meta so the layout is stated, not assumed (stride is
    // width * 4 at 32 bpp).
    gst_buffer_add_video_meta(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                              GST_VIDEO_FORMAT_BGRA, self->outWidth,
                              self->outHeight);

    const GstClockTime pts = self->frameIndex * kFrameDuration;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = kFrameDuration;
    self->frameIndex++;
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
    base->set_caps = setCapsSrc;
    push->create = createFrame;
}

void lightning_window_capture_src_init(LightningWindowCaptureSrc *self)
{
    self->hwnd = 0;
    self->outWidth = 0;
    self->outHeight = 0;
    self->out = Surface{};
    self->printWidth = 0;
    self->printHeight = 0;
    self->print = Surface{};
    self->startWidth = 0;
    self->startHeight = 0;
    self->frameIndex = 0;
    self->pacingBase = 0;
    self->pacingStarted = FALSE;
    self->windowGone = FALSE;
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    // No do-timestamp: it would overwrite our zero-based timestamps with the
    // arrival time.
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

QString fileDescriptionUncached(const QString &executable);

using QueryFullProcessImageNameWFn = BOOL(WINAPI *)(HANDLE, DWORD, LPWSTR,
                                                    PDWORD);
using GetFileVersionInfoSizeWFn = DWORD(WINAPI *)(LPCWSTR, LPDWORD);
using GetFileVersionInfoWFn = BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
using VerQueryValueWFn = BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

QString fileDescription(const QString &executable)
{
    static QMutex cacheMutex;
    static QHash<QString, QString> cache;
    {
        QMutexLocker lock(&cacheMutex);
        const auto known = cache.constFind(executable);
        if (known != cache.cend())
            return *known;
    }
    const QString answer = fileDescriptionUncached(executable);
    QMutexLocker lock(&cacheMutex);
    cache.insert(executable, answer);
    return answer;
}

QString fileDescriptionUncached(const QString &executable)
{
    static GetFileVersionInfoSizeWFn sizeFn = nullptr;
    static GetFileVersionInfoWFn readFn = nullptr;
    static VerQueryValueWFn queryFn = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE version = LoadLibraryW(L"version.dll");
        if (!version)
            return;
        sizeFn = reinterpret_cast<GetFileVersionInfoSizeWFn>(
            reinterpret_cast<void *>(
                GetProcAddress(version, "GetFileVersionInfoSizeW")));
        readFn = reinterpret_cast<GetFileVersionInfoWFn>(
            reinterpret_cast<void *>(
                GetProcAddress(version, "GetFileVersionInfoW")));
        queryFn = reinterpret_cast<VerQueryValueWFn>(reinterpret_cast<void *>(
            GetProcAddress(version, "VerQueryValueW")));
    });
    if (!sizeFn || !readFn || !queryFn || executable.isEmpty())
        return {};

    const std::wstring path = executable.toStdWString();
    DWORD ignored = 0;
    const DWORD size = sizeFn(path.c_str(), &ignored);
    if (size == 0 || size > 1u << 20)
        return {};
    QByteArray block(static_cast<int>(size), Qt::Uninitialized);
    if (!readFn(path.c_str(), 0, size, block.data()))
        return {};

    struct Translation {
        WORD language;
        WORD codePage;
    };
    void *value = nullptr;
    UINT valueLength = 0;
    if (!queryFn(block.constData(), L"\\VarFileInfo\\Translation", &value,
                 &valueLength)
        || !value || valueLength < sizeof(Translation))
        return {};
    const auto *translations = static_cast<const Translation *>(value);
    const UINT count = valueLength / sizeof(Translation);
    for (UINT i = 0; i < count; ++i) {
        wchar_t key[64];
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                 translations[i].language, translations[i].codePage);
        void *text = nullptr;
        UINT textLength = 0;
        if (queryFn(block.constData(), key, &text, &textLength) && text
            && textLength > 0) {
            const QString described =
                QString::fromWCharArray(static_cast<const wchar_t *>(text))
                    .trimmed();
            if (!described.isEmpty())
                return described;
        }
    }
    return {};
}

QString executablePathFor(DWORD pid)
{
    static QueryFullProcessImageNameWFn imageNameFn = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel) {
            imageNameFn = reinterpret_cast<QueryFullProcessImageNameWFn>(
                reinterpret_cast<void *>(
                    GetProcAddress(kernel, "QueryFullProcessImageNameW")));
        }
    });
    if (!imageNameFn || !pid)
        return {};
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[MAX_PATH];
    DWORD length = MAX_PATH;
    const BOOL got = imageNameFn(process, 0, path, &length);
    CloseHandle(process);
    if (!got || length == 0)
        return {};
    return QString::fromWCharArray(path, static_cast<int>(length));
}

QString applicationNameForExecutable(const QString &executable)
{
    if (executable.isEmpty())
        return {};
    const QString described = fileDescription(executable);
    if (!described.isEmpty())
        return described;
    QString base = executable.section(QLatin1Char('\\'), -1);
    if (base.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
        base.chop(4);
    if (base.isEmpty())
        return {};
    base[0] = base.at(0).toUpper();
    return base;
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
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == ctx->ownProcess)
        return TRUE;

    const LONG exStyle = GetWindowLong(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return TRUE;

    if (dwmGetWindowAttribute()) {
        BOOL cloaked = FALSE;
        if (SUCCEEDED(dwmGetWindowAttribute()(window, kDwmwaCloaked, &cloaked,
                                              sizeof(cloaked)))
            && cloaked)
            return TRUE;
    }

    wchar_t title[512];
    const int length = GetWindowTextW(window, title, 512);
    if (length <= 0)
        return TRUE;

    WindowGeometry geo;
    if (!windowGeometry(window, &geo))
        return TRUE;

    lightning::wincap::WindowInfo info;
    info.handle = static_cast<quint64>(reinterpret_cast<uintptr_t>(window));
    info.title = QString::fromWCharArray(title, length);
    const QString executable = executablePathFor(pid);
    info.pid = pid;
    info.executable = executable.section(QLatin1Char('\\'), -1).toLower();
    info.application = applicationNameForExecutable(executable);
    info.width = geo.cropWidth;
    info.height = geo.cropHeight;
    ctx->out->append(info);
    return TRUE;
}

}

namespace lightning::wincap {

bool available() { return true; }

ProcessInfo processInfo(quint64 pid)
{
    static QueryFullProcessImageNameWFn imageNameFn = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel) {
            imageNameFn = reinterpret_cast<QueryFullProcessImageNameWFn>(
                reinterpret_cast<void *>(
                    GetProcAddress(kernel, "QueryFullProcessImageNameW")));
        }
    });
    ProcessInfo info;
    if (!pid)
        return info;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process)
        return info;
    FILETIME created{}, exited{}, kernelTime{}, userTime{};
    if (GetProcessTimes(process, &created, &exited, &kernelTime, &userTime)) {
        info.startTime = (static_cast<qint64>(created.dwHighDateTime) << 32)
            | static_cast<qint64>(created.dwLowDateTime);
    }
    DWORD exitCode = 0;
    info.running = GetExitCodeProcess(process, &exitCode)
        && exitCode == STILL_ACTIVE;
    if (imageNameFn) {
        wchar_t path[MAX_PATH];
        DWORD length = MAX_PATH;
        if (imageNameFn(process, 0, path, &length) && length > 0)
            info.executablePath =
                QString::fromWCharArray(path, static_cast<int>(length));
    }
    CloseHandle(process);
    return info;
}

QString applicationNameForExecutablePath(const QString &path)
{
    return applicationNameForExecutable(path);
}

namespace {

QImage grabToImage(HDC source, int x, int y, int width, int height,
                   int maxEdge)
{
    if (width < 2 || height < 2 || !source)
        return {};
    Surface surface;
    if (!createSurface(width, height, &surface))
        return {};
    BitBlt(surface.dc, 0, 0, width, height, source, x, y, SRCCOPY);
    GdiFlush();
    QImage out = QImage(reinterpret_cast<const uchar *>(surface.pixels), width,
                        height, width * 4, QImage::Format_RGB32)
                     .copy();
    releaseSurface(&surface);
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

struct NameHunt {
    const QString *wanted;
    int seen;
    int index;
    RECT rect;
    bool found;
};

BOOL CALLBACK nameProc(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    auto *hunt = reinterpret_cast<NameHunt *>(param);
    const int here = hunt->seen++;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<LPMONITORINFO>(&info)))
        return TRUE;
    if (QString::fromWCharArray(info.szDevice) != *hunt->wanted)
        return TRUE;
    hunt->index = here;
    hunt->rect = info.rcMonitor;
    hunt->found = true;
    return FALSE;
}

BOOL CALLBACK monitorProc(HMONITOR, HDC, LPRECT rect, LPARAM param)
{
    auto *hunt = reinterpret_cast<MonitorHunt *>(param);
    if (hunt->seen++ == hunt->wanted) {
        hunt->rect = *rect;
        hunt->found = true;
        return FALSE;
    }
    return TRUE;
}

}

QImage captureThumbnail(quint64 handle, int maxEdge)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
    if (!handle || !IsWindow(window) || IsIconic(window))
        return {};
    WindowGeometry geo;
    if (!windowGeometry(window, &geo))
        return {};

    Surface surface;
    if (!createSurface(geo.printWidth, geo.printHeight, &surface))
        return {};
    printInto(window, surface, geo.printWidth, geo.printHeight);
    GdiFlush();
    const QImage full(reinterpret_cast<const uchar *>(surface.pixels),
                      geo.printWidth, geo.printHeight, geo.printWidth * 4,
                      QImage::Format_RGB32);
    QImage out =
        full.copy(geo.cropX, geo.cropY, geo.cropWidth, geo.cropHeight);
    releaseSurface(&surface);
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
    const QImage out = grabToImage(screen, hunt.rect.left, hunt.rect.top,
                                   hunt.rect.right - hunt.rect.left,
                                   hunt.rect.bottom - hunt.rect.top, maxEdge);
    ReleaseDC(nullptr, screen);
    return out;
}

bool displayForDeviceName(const QString &deviceName, int *index, int *width,
                          int *height)
{
    if (deviceName.isEmpty())
        return false;
    NameHunt hunt{&deviceName, 0, -1, {}, false};
    EnumDisplayMonitors(nullptr, nullptr, nameProc,
                        reinterpret_cast<LPARAM>(&hunt));
    if (!hunt.found)
        return false;
    if (index)
        *index = hunt.index;
    if (width)
        *width = static_cast<int>(hunt.rect.right - hunt.rect.left);
    if (height)
        *height = static_cast<int>(hunt.rect.bottom - hunt.rect.top);
    return true;
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

}

#elif defined(LIGHTNING_HAVE_X11_WINDOW_CAPTURE)

#include <cstring>
#include <memory>
#include <mutex>

#include <QByteArray>
#include <QGuiApplication>
#include <QPainter>
#include <QSet>
#include <QWindow>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include <xcb/composite.h>
#include <xcb/xcb.h>

namespace {

constexpr int kFramerate = 30;
constexpr GstClockTime kFrameDuration = GST_SECOND / kFramerate;

struct XcbDeleter {
    void operator()(xcb_connection_t *c) const { xcb_disconnect(c); }
};
using Connection = std::unique_ptr<xcb_connection_t, XcbDeleter>;

template <typename T> using Reply = std::unique_ptr<T, decltype(&free)>;
template <typename T> Reply<T> reply(T *raw) { return Reply<T>(raw, &free); }

Connection connectX(xcb_window_t *root)
{
    int screen = 0;
    Connection c(xcb_connect(nullptr, &screen));
    if (!c || xcb_connection_has_error(c.get()))
        return {};
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c.get()));
    for (int i = 0; i < screen && it.rem; ++i)
        xcb_screen_next(&it);
    if (!it.rem)
        return {};
    if (root)
        *root = it.data->root;
    return c;
}

xcb_atom_t atom(xcb_connection_t *c, const char *name)
{
    auto r = reply(xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name),
        nullptr));
    return r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
}

QByteArray property(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop,
                    xcb_atom_t type, uint32_t maxWords = 4096)
{
    auto r = reply(xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, prop, type, 0, maxWords), nullptr));
    if (!r || r->type == XCB_ATOM_NONE)
        return {};
    return QByteArray(static_cast<const char *>(xcb_get_property_value(r.get())),
                      xcb_get_property_value_length(r.get()));
}

QList<quint32> words(const QByteArray &bytes)
{
    QList<quint32> out;
    for (qsizetype i = 0; i + 4 <= bytes.size(); i += 4) {
        quint32 v;
        std::memcpy(&v, bytes.constData() + i, 4);
        out.append(v);
    }
    return out;
}

/// The window's own contents as RGB32. Free of what overlaps it only while
/// the window or its frame is redirected (by a compositor or by us); without
/// that, GetImage returns whatever covers it.
QImage windowPixels(xcb_connection_t *c, xcb_window_t w, int *width,
                    int *height)
{
    auto attrs = reply(xcb_get_window_attributes_reply(
        c, xcb_get_window_attributes(c, w), nullptr));
    if (!attrs || attrs->map_state != XCB_MAP_STATE_VIEWABLE)
        return {};
    auto geo = reply(xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr));
    if (!geo)
        return {};
    if (width)
        *width = geo->width;
    if (height)
        *height = geo->height;
    if (geo->depth != 24 && geo->depth != 32)
        return {};
    // Only a redirected window has a pixmap of its own. A child of a
    // compositor-redirected frame does not; GetImage on the window then reads
    // the frame's pixmap, which is equally free of what overlaps it.
    xcb_drawable_t drawable = xcb_generate_id(c);
    int16_t origin = static_cast<int16_t>(geo->border_width);
    if (xcb_generic_error_t *e = xcb_request_check(
            c, xcb_composite_name_window_pixmap_checked(c, w, drawable))) {
        free(e);
        drawable = w;
        origin = 0;
    }
    auto image = reply(xcb_get_image_reply(
        c,
        xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable, origin, origin,
                      geo->width, geo->height, ~0u),
        nullptr));
    if (drawable != w)
        xcb_free_pixmap(c, drawable);
    if (!image)
        return {};
    const int stride = geo->width * 4;
    if (xcb_get_image_data_length(image.get()) < stride * geo->height)
        return {};
    QImage out(xcb_get_image_data(image.get()), geo->width, geo->height, stride,
               QImage::Format_RGB32);
    return out.copy();
}

bool redirect(xcb_connection_t *c, xcb_window_t w)
{
    auto version = reply(xcb_composite_query_version_reply(
        c, xcb_composite_query_version(c, 0, 4), nullptr));
    if (!version)
        return false;
    xcb_generic_error_t *e = xcb_request_check(
        c, xcb_composite_redirect_window_checked(
               c, w, XCB_COMPOSITE_REDIRECT_AUTOMATIC));
    free(e);
    return e == nullptr;
}

bool littleEndianServer(xcb_connection_t *c)
{
    return xcb_get_setup(c)->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
}

struct LightningWindowCaptureSrc {
    GstPushSrc parent;
    guint64 xid;
    xcb_connection_t *conn;
    xcb_atom_t netState;
    xcb_atom_t stateHidden;
    gint outWidth;
    gint outHeight;
    gint startWidth;
    gint startHeight;
    gint lastWidth;
    gint lastHeight;
    guint64 frameIndex;
    GstClockTime pacingBase;
    gboolean pacingStarted;
    gboolean windowGone;
};

struct LightningWindowCaptureSrcClass {
    GstPushSrcClass parent;
};

enum { PROP_0, PROP_XID };

#define LIGHTNING_TYPE_WINDOW_CAPTURE_SRC \
    (lightning_window_capture_src_get_type())

G_DEFINE_TYPE(LightningWindowCaptureSrc, lightning_window_capture_src,
              GST_TYPE_PUSH_SRC)

// BGRx: a depth-24/32 ZPixmap is 32 bpp, little-endian BGRX; alpha is ignored.
GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)BGRx, "
                    "width=(int)[1,16384], height=(int)[1,16384], "
                    "framerate=(fraction)30/1"));

GST_DEBUG_CATEGORY_STATIC(lightning_wincap_debug);
#define GST_CAT_DEFAULT lightning_wincap_debug

bool valueAdmits(const GValue *value, int wanted)
{
    if (!value)
        return true;
    if (G_VALUE_HOLDS_INT(value))
        return g_value_get_int(value) == wanted;
    if (GST_VALUE_HOLDS_INT_RANGE(value)) {
        return wanted >= gst_value_get_int_range_min(value)
            && wanted <= gst_value_get_int_range_max(value);
    }
    if (GST_VALUE_HOLDS_LIST(value)) {
        for (guint i = 0; i < gst_value_list_get_size(value); ++i) {
            if (valueAdmits(gst_value_list_get_value(value, i), wanted))
                return true;
        }
        return false;
    }
    return true;
}

int valueMax(const GValue *value, int fallback)
{
    if (!value)
        return fallback;
    if (G_VALUE_HOLDS_INT(value))
        return g_value_get_int(value);
    if (GST_VALUE_HOLDS_INT_RANGE(value))
        return gst_value_get_int_range_max(value);
    if (GST_VALUE_HOLDS_LIST(value)) {
        int best = 0;
        for (guint i = 0; i < gst_value_list_get_size(value); ++i)
            best = std::max(best,
                            valueMax(gst_value_list_get_value(value, i), 0));
        return best > 0 ? best : fallback;
    }
    return fallback;
}

/// False only when the window no longer exists. Unmapped (minimised, on
/// another workspace) or unreadable frames are black, never another window.
bool paintWindow(LightningWindowCaptureSrc *self, uchar *dst)
{
    xcb_connection_t *c = self->conn;
    const auto window = static_cast<xcb_window_t>(self->xid);
    QImage frame(dst, self->outWidth, self->outHeight, self->outWidth * 4,
                 QImage::Format_RGB32);
    frame.fill(Qt::black);

    xcb_generic_error_t *error = nullptr;
    auto attrs = reply(xcb_get_window_attributes_reply(
        c, xcb_get_window_attributes(c, window), &error));
    if (!attrs) {
        const bool gone = xcb_connection_has_error(c)
            || (error && error->error_code == XCB_WINDOW);
        free(error);
        return !gone;
    }
    // Mutter-based window managers keep a minimised window mapped.
    if (attrs->map_state != XCB_MAP_STATE_VIEWABLE
        || words(property(c, window, self->netState, XCB_ATOM_ATOM))
               .contains(self->stateHidden))
        return true;

    int width = 0;
    int height = 0;
    const QImage pixels = windowPixels(c, window, &width, &height);
    if (pixels.isNull())
        return true;
    if (width != self->lastWidth || height != self->lastHeight) {
        self->lastWidth = width;
        self->lastHeight = height;
        GST_INFO_OBJECT(self,
                        "shared window resized to %dx%d; publishing at %dx%d",
                        width, height, self->outWidth, self->outHeight);
    }
    if (pixels.width() == self->outWidth && pixels.height() == self->outHeight) {
        std::memcpy(dst, pixels.constBits(),
                    static_cast<size_t>(self->outWidth) * self->outHeight * 4);
        return true;
    }
    const lightning::wincap::Size fit = lightning::wincap::fitInto(
        pixels.width(), pixels.height(), self->outWidth, self->outHeight);
    if (fit.width < 2 || fit.height < 2)
        return true;
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect((self->outWidth - fit.width) / 2,
                            (self->outHeight - fit.height) / 2, fit.width,
                            fit.height),
                      pixels);
    return true;
}

void waitForFrameSlot(LightningWindowCaptureSrc *self)
{
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (!clock)
        return;
    const GstClockTime base = gst_element_get_base_time(GST_ELEMENT(self));
    const GstClockTime now = gst_clock_get_time(clock);
    if (!GST_CLOCK_TIME_IS_VALID(base) || !GST_CLOCK_TIME_IS_VALID(now)
        || now < base) {
        gst_object_unref(clock);
        return;
    }
    const GstClockTime running = now - base;
    if (!self->pacingStarted) {
        self->pacingStarted = TRUE;
        self->pacingBase = running;
    }
    if (self->pacingBase > running + GST_SECOND) {
        const GstClockTime span = self->frameIndex * kFrameDuration;
        self->pacingBase = running > span ? running - span : 0;
    }
    const guint64 elapsed =
        running > self->pacingBase ? (running - self->pacingBase) : 0;
    const guint64 dueIndex = elapsed / kFrameDuration;
    if (dueIndex > self->frameIndex)
        self->frameIndex = dueIndex;
    const GstClockTime due =
        self->pacingBase + self->frameIndex * kFrameDuration;
    if (due > running) {
        GstClockID id = gst_clock_new_single_shot_id(clock, base + due);
        gst_clock_id_wait(id, nullptr);
        gst_clock_id_unref(id);
    }
    gst_object_unref(clock);
}

gboolean startSrc(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    Connection c = connectX(nullptr);
    if (!c || !littleEndianServer(c.get())) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                          ("Could not connect to the X server."),
                          ("no usable X connection"));
        return FALSE;
    }
    const auto window = static_cast<xcb_window_t>(self->xid);
    // Without a redirect, an occluded window would share what covers it.
    if (!self->xid || !redirect(c.get(), window)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share is not available."),
                          ("no such window or no Composite extension"));
        return FALSE;
    }
    int width = 0;
    int height = 0;
    windowPixels(c.get(), window, &width, &height);
    if (width < 2 || height < 2) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
                          ("The window to share has no visible area."),
                          ("window is unmapped or empty"));
        return FALSE;
    }
    self->netState = atom(c.get(), "_NET_WM_STATE");
    self->stateHidden = atom(c.get(), "_NET_WM_STATE_HIDDEN");
    self->conn = c.release();
    self->startWidth = width & ~1;
    self->startHeight = height & ~1;
    self->lastWidth = width;
    self->lastHeight = height;
    self->frameIndex = 0;
    self->pacingBase = 0;
    self->pacingStarted = FALSE;
    self->windowGone = FALSE;
    GST_INFO_OBJECT(self, "window capture started, window is %dx%d", width,
                    height);
    return TRUE;
}

gboolean stopSrc(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    // Disconnecting also drops our redirect.
    if (self->conn)
        xcb_disconnect(self->conn);
    self->conn = nullptr;
    self->outWidth = 0;
    self->outHeight = 0;
    return TRUE;
}

GstCaps *fixateSrc(GstBaseSrc *base, GstCaps *caps)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    caps = gst_caps_make_writable(caps);

    // Prefer a structure that can carry the window's own size: through
    // videoconvertscale the downstream-restricted structure comes first.
    const guint count = gst_caps_get_size(caps);
    guint pick = 0;
    for (guint i = 0; i < count; ++i) {
        const GstStructure *candidate = gst_caps_get_structure(caps, i);
        if (valueAdmits(gst_structure_get_value(candidate, "width"),
                        self->startWidth)
            && valueAdmits(gst_structure_get_value(candidate, "height"),
                           self->startHeight)) {
            pick = i;
            break;
        }
    }
    if (pick != 0) {
        GstCaps *reordered = gst_caps_new_empty();
        for (guint i = 0; i < count; ++i) {
            const guint from = i == 0 ? pick : (i <= pick ? i - 1 : i);
            GstCapsFeatures *features = gst_caps_get_features(caps, from);
            gst_caps_append_structure_full(
                reordered,
                gst_structure_copy(gst_caps_get_structure(caps, from)),
                features ? gst_caps_features_copy(features) : nullptr);
        }
        gst_caps_unref(caps);
        caps = reordered;
    }

    GstStructure *structure = gst_caps_get_structure(caps, 0);
    const lightning::wincap::Size want = lightning::wincap::fitInto(
        self->startWidth, self->startHeight,
        valueMax(gst_structure_get_value(structure, "width"),
                 self->startWidth),
        valueMax(gst_structure_get_value(structure, "height"),
                 self->startHeight));
    gst_structure_fixate_field_nearest_int(
        structure, "width", want.width > 0 ? want.width : self->startWidth);
    gst_structure_fixate_field_nearest_int(
        structure, "height",
        want.height > 0 ? want.height : self->startHeight);
    gst_structure_fixate_field_nearest_fraction(structure, "framerate",
                                                kFramerate, 1);
    // An unfixated PAR range falls to 1/2147483647 and overflows videoscale.
    gst_structure_fixate_field_nearest_fraction(structure,
                                                "pixel-aspect-ratio", 1, 1);
    return GST_BASE_SRC_CLASS(lightning_window_capture_src_parent_class)
        ->fixate(base, caps);
}

gboolean setCapsSrc(GstBaseSrc *base, GstCaps *caps)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(base);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps))
        return FALSE;
    const gint width = GST_VIDEO_INFO_WIDTH(&info);
    const gint height = GST_VIDEO_INFO_HEIGHT(&info);
    if (width < 2 || height < 2)
        return FALSE;
    self->outWidth = width;
    self->outHeight = height;
    GST_INFO_OBJECT(self, "window capture publishing %dx%d from a %dx%d window",
                    width, height, self->startWidth, self->startHeight);
    return TRUE;
}

GstFlowReturn createFrame(GstPushSrc *push, GstBuffer **out)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(push);
    if (self->windowGone)
        return GST_FLOW_EOS;
    if (!self->conn || self->outWidth < 2 || self->outHeight < 2)
        return GST_FLOW_NOT_NEGOTIATED;

    waitForFrameSlot(self);

    const gsize size =
        static_cast<gsize>(self->outWidth) * self->outHeight * 4;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer)
        return GST_FLOW_ERROR;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    const bool alive = paintWindow(self, map.data);
    gst_buffer_unmap(buffer, &map);
    if (!alive) {
        gst_buffer_unref(buffer);
        GST_INFO_OBJECT(self, "shared window closed; ending capture");
        self->windowGone = TRUE;
        return GST_FLOW_EOS;
    }
    gst_buffer_add_video_meta(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                              GST_VIDEO_FORMAT_BGRx, self->outWidth,
                              self->outHeight);
    const GstClockTime pts = self->frameIndex * kFrameDuration;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = kFrameDuration;
    self->frameIndex++;
    *out = buffer;
    return GST_FLOW_OK;
}

void setProperty(GObject *object, guint id, const GValue *value,
                 GParamSpec *spec)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(object);
    if (id == PROP_XID)
        self->xid = g_value_get_uint64(value);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    auto *self = reinterpret_cast<LightningWindowCaptureSrc *>(object);
    if (id == PROP_XID)
        g_value_set_uint64(value, self->xid);
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
        object, PROP_XID,
        g_param_spec_uint64("xid", "Window id", "X11 window to capture", 0,
                            G_MAXUINT32, 0,
                            static_cast<GParamFlags>(G_PARAM_READWRITE
                                                     | G_PARAM_STATIC_STRINGS)));

    gst_element_class_add_static_pad_template(element, &kSrcTemplate);
    gst_element_class_set_static_metadata(
        element, "Lightning window capture", "Source/Video",
        "Captures a single X11 window from its composited pixmap, so nothing "
        "stacked on top of it is shared",
        "Lightning");

    base->start = startSrc;
    base->stop = stopSrc;
    base->fixate = fixateSrc;
    base->set_caps = setCapsSrc;
    push->create = createFrame;
}

void lightning_window_capture_src_init(LightningWindowCaptureSrc *self)
{
    self->xid = 0;
    self->conn = nullptr;
    self->netState = XCB_ATOM_NONE;
    self->stateHidden = XCB_ATOM_NONE;
    self->outWidth = 0;
    self->outHeight = 0;
    self->startWidth = 0;
    self->startHeight = 0;
    self->lastWidth = 0;
    self->lastHeight = 0;
    self->frameIndex = 0;
    self->pacingBase = 0;
    self->pacingStarted = FALSE;
    self->windowGone = FALSE;
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

QSet<xcb_window_t> ownWindows()
{
    QSet<xcb_window_t> out;
    const auto windows = QGuiApplication::allWindows();
    for (const QWindow *w : windows) {
        if (w && w->handle())
            out.insert(static_cast<xcb_window_t>(w->winId()));
    }
    return out;
}

QImage scaledThumbnail(const QImage &image, int maxEdge)
{
    if (image.isNull())
        return {};
    return image.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

}

namespace lightning::wincap {

bool available()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("xcb"))
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

ProcessInfo processInfo(quint64) { return {}; }
QString applicationNameForExecutablePath(const QString &) { return {}; }

QList<WindowInfo> enumerateWindows()
{
    QList<WindowInfo> out;
    if (!available())
        return out;
    xcb_window_t root = 0;
    Connection conn = connectX(&root);
    if (!conn)
        return out;
    xcb_connection_t *c = conn.get();
    const xcb_atom_t utf8 = atom(c, "UTF8_STRING");
    const xcb_atom_t netName = atom(c, "_NET_WM_NAME");
    const xcb_atom_t netPid = atom(c, "_NET_WM_PID");
    const xcb_atom_t netType = atom(c, "_NET_WM_WINDOW_TYPE");
    const xcb_atom_t typeNormal = atom(c, "_NET_WM_WINDOW_TYPE_NORMAL");
    const xcb_atom_t netState = atom(c, "_NET_WM_STATE");
    const xcb_atom_t stateHidden = atom(c, "_NET_WM_STATE_HIDDEN");
    const xcb_atom_t stateSkip = atom(c, "_NET_WM_STATE_SKIP_TASKBAR");

    QList<quint32> clients = words(property(
        c, root, atom(c, "_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW));
    if (clients.isEmpty())
        clients = words(property(c, root, atom(c, "_NET_CLIENT_LIST"),
                                 XCB_ATOM_WINDOW));
    const QSet<xcb_window_t> ours = ownWindows();

    // Stacking order is bottom to top; list the topmost first.
    for (auto it = clients.crbegin(); it != clients.crend(); ++it) {
        const xcb_window_t w = *it;
        X11WindowFacts facts;
        auto attrs = reply(xcb_get_window_attributes_reply(
            c, xcb_get_window_attributes(c, w), nullptr));
        auto geo =
            reply(xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr));
        if (!attrs || !geo)
            continue;
        facts.viewable = attrs->map_state == XCB_MAP_STATE_VIEWABLE;
        facts.width = geo->width;
        facts.height = geo->height;
        facts.ours = ours.contains(w);
        const QList<quint32> types =
            words(property(c, w, netType, XCB_ATOM_ATOM));
        facts.normalType = types.isEmpty() || types.contains(typeNormal);
        const QList<quint32> states =
            words(property(c, w, netState, XCB_ATOM_ATOM));
        facts.hidden = states.contains(stateHidden);
        facts.skipTaskbar = states.contains(stateSkip);
        facts.title = QString::fromUtf8(property(c, w, netName, utf8));
        if (facts.title.isEmpty())
            facts.title = QString::fromLatin1(
                property(c, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING));
        if (!x11WindowListed(facts))
            continue;

        WindowInfo info;
        info.handle = w;
        info.title = facts.title;
        // WM_CLASS is "instance\0Class\0"; the class is the application name.
        const QList<QByteArray> wmClass =
            property(c, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING).split('\0');
        info.application = QString::fromLocal8Bit(
            wmClass.size() > 1 ? wmClass.at(1) : wmClass.value(0));
        const QList<quint32> pid = words(property(c, w, netPid,
                                                  XCB_ATOM_CARDINAL, 1));
        info.pid = pid.isEmpty() ? 0 : pid.first();
        info.width = facts.width;
        info.height = facts.height;
        out.append(info);
    }
    return out;
}

QImage captureThumbnail(quint64 handle, int maxEdge)
{
    Connection c = connectX(nullptr);
    if (!c || !handle || !littleEndianServer(c.get()))
        return {};
    // No redirect here: only the local user sees this, and redirecting and
    // dropping it per thumbnail would make an uncomposited desktop repaint.
    return scaledThumbnail(
        windowPixels(c.get(), static_cast<xcb_window_t>(handle), nullptr,
                     nullptr),
        maxEdge);
}

QImage captureScreenThumbnail(int, int) { return {}; }

QImage captureAreaThumbnail(const QRect &area, int maxEdge)
{
    xcb_window_t root = 0;
    Connection conn = connectX(&root);
    if (!conn || !area.isValid() || area.x() < 0 || area.y() < 0
        || !littleEndianServer(conn.get()))
        return {};
    xcb_connection_t *c = conn.get();
    auto image = reply(xcb_get_image_reply(
        c,
        xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, root,
                      static_cast<int16_t>(area.x()),
                      static_cast<int16_t>(area.y()),
                      static_cast<uint16_t>(area.width()),
                      static_cast<uint16_t>(area.height()), ~0u),
        nullptr));
    const int stride = area.width() * 4;
    if (!image || (image->depth != 24 && image->depth != 32)
        || xcb_get_image_data_length(image.get()) < stride * area.height())
        return {};
    return scaledThumbnail(QImage(xcb_get_image_data(image.get()), area.width(),
                                  area.height(), stride, QImage::Format_RGB32),
                           maxEdge);
}

bool displayForDeviceName(const QString &, int *, int *, int *)
{
    return false;
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

}

#else

namespace lightning::wincap {

bool available() { return false; }
ProcessInfo processInfo(quint64) { return {}; }
QString applicationNameForExecutablePath(const QString &) { return {}; }
QList<WindowInfo> enumerateWindows() { return {}; }
bool displayForDeviceName(const QString &, int *, int *, int *)
{
    return false;
}
QImage captureThumbnail(quint64, int) { return {}; }
QImage captureScreenThumbnail(int, int) { return {}; }
void registerWindowCaptureSrc() {}
const char *windowCaptureSrcName() { return "lightningwindowcapturesrc"; }

}

#endif
