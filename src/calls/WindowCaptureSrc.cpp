#include "calls/WindowCaptureSrc.h"

#include <algorithm>

namespace lightning::wincap {

Size fitInto(int srcW, int srcH, int maxW, int maxH)
{
    if (srcW < 2 || srcH < 2 || maxW < 2 || maxH < 2)
        return {};
    // Never above 1.0: a window smaller than the ceiling keeps its size.
    const double scale =
        (std::min)({1.0, static_cast<double>(maxW) / srcW,
                    static_cast<double>(maxH) / srcH});
    // Truncate, then clear the low bit; both only shrink, so the result stays
    // inside the ceiling.
    const int width = static_cast<int>(srcW * scale) & ~1;
    const int height = static_cast<int>(srcH * scale) & ~1;
    if (width < 2 || height < 2)
        return {};
    return {width, height};
}

} // namespace lightning::wincap

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

// Asks the window to render its whole content even where covered. Defined
// here because mingw guards it behind a newer _WIN32_WINNT than the build
// uses.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Fixed capture rate. The source runs on its own clock and always produces a
// frame, so a still window keeps streaming (no videorate first-buffer hold).
constexpr int kFramerate = 30;
constexpr GstClockTime kFrameDuration = GST_SECOND / kFramerate;

/// A top-down 32-bit DIB we own, so a frame is a straight memcpy. The DC's
/// original bitmap is kept because DeleteObject on a bitmap still selected
/// into a DC silently fails and leaks it.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previous = nullptr;
    void *pixels = nullptr;
};

struct LightningWindowCaptureSrc {
    GstPushSrc parent;

    /// The HWND, as a plain integer property.
    guint64 hwnd;

    /// The negotiated frame size, learned in `set_caps`. Buffers must match
    /// the caps: downstream reads them at the stride the caps imply, so a
    /// window-sized buffer under smaller caps is read as garbage. Fixed for
    /// the element's life; a resized window is fitted into it rather than
    /// renegotiated.
    gint outWidth;
    gint outHeight;
    Surface out;   // BGRA, top-down

    /// The surface PrintWindow draws into, at the window's own bounds; rebuilt
    /// only when the window is resized.
    gint printWidth;
    gint printHeight;
    Surface print;

    /// The window's visible size at start, used during fixation.
    gint startWidth;
    gint startHeight;

    /// The frame slot the next buffer fills, counted from zero at share start;
    /// its timestamp is this times the frame duration.
    ///
    /// Zero-based, not pipeline running time: videorate starts at segment
    /// start, and a first buffer stamped with the call's age would make it
    /// emit duplicates for the whole age. Advanced from the clock rather than
    /// incremented, so a capture slower than 30 fps drops slots instead of
    /// making the receiver play in slow motion.
    guint64 frameIndex;
    /// Where slot zero sits on the clock, so pacing does not need running-time
    /// timestamps.
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

/// DwmGetWindowAttribute resolved at runtime rather than linked, so the
/// package gains no dwmapi import. Without it we fall back to the plain
/// window rect.
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

constexpr DWORD kDwmwaExtendedFrameBounds = 9;   // DWMWA_EXTENDED_FRAME_BOUNDS
constexpr DWORD kDwmwaCloaked = 14;              // DWMWA_CLOAKED

/// Where a window is and which part of it is visible. PrintWindow renders the
/// whole window from the window rect's origin, while DWM's
/// DWMWA_EXTENDED_FRAME_BOUNDS is the visible frame (Windows 10/11 add an
/// invisible resize border); the difference is the crop offset.
///
/// One derivation shared by the capture and the picker's list and preview,
/// so a preview always matches what is sent.
struct WindowGeometry {
    int printWidth = 0;    ///< what PrintWindow will draw
    int printHeight = 0;
    int cropX = 0;         ///< where the visible frame starts inside that
    int cropY = 0;
    int cropWidth = 0;     ///< its size, rounded down to even
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
        // Only ever a crop: never read outside what PrintWindow drew.
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

// -------------------------------------------------------------- surfaces --


bool createSurface(int width, int height, Surface *out)
{
    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;
    out->dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
        // Negative height: top-down, the row order video/x-raw expects.
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

/// IsHungAppWindow, resolved at runtime.
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

// ---------------------------------------------------------- caps helpers --

/// Whether a caps field accepts `wanted`. Absent or unfamiliar fields are
/// treated as accepting.
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

/// The largest value a caps field permits, or `fallback`.
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

// ------------------------------------------------------------ the element --

/// Draws the window into the negotiated frame. Returns false only when the
/// window is gone.
bool paintWindow(LightningWindowCaptureSrc *self)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!IsWindow(window))
        return false;

    WindowGeometry geo;
    if (IsIconic(window) || !windowGeometry(window, &geo)) {
        // Minimised or degenerate: a black frame, not EOS, so the share
        // survives the user minimising the window.
        fillBlack(self->out.dc, self->outWidth, self->outHeight);
        return true;
    }

    if (geo.printWidth != self->printWidth
        || geo.printHeight != self->printHeight || !self->print.pixels) {
        // The window was resized: rebuild the print surface once.
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
        // The ordinary case: a straight blit.
        BitBlt(self->out.dc, 0, 0, self->outWidth, self->outHeight,
               self->print.dc, geo.cropX, geo.cropY, SRCCOPY);
        return true;
    }

    // The window no longer matches the negotiated shape: letterbox rather
    // than stretch, keeping the agreed stream resolution.
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

/// Waits until this frame is due. Without pacing the capture would run as
/// fast as PrintWindow returns, burning a core on frames videorate drops
/// (gdiscreencapsrc paces the same way).
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
    // Pace against `pacingBase` (slot zero on the clock) so timestamps can
    // stay zero-based; see `frameIndex`.
    if (!self->pacingStarted) {
        self->pacingStarted = TRUE;
        self->pacingBase = running;
    }
    if (self->pacingBase > running + GST_SECOND) {
        // The clock jumped backwards: re-pin and rebase the index, or the
        // next wait could be minutes long on a source with no `unlock`.
        const GstClockTime span = self->frameIndex * kFrameDuration;
        self->pacingBase = running > span ? running - span : 0;
    }
    // Skip missed slots rather than stamping late: a slow capture drops
    // frames and never slows the stream clock.
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
        // Report a named error rather than failing silently.
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
    // Only what fixation needs. Surfaces are built when their sizes are known:
    // output in set_caps, print on the first frame.
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
            // The chosen one first, then the rest in order.
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
    // Fit, never two independent clamps, which would distort the aspect.
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

/// Learns the negotiated size, so every buffer matches the caps.
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
        // The window closed: end the stream cleanly (EOS, not an error, which
        // would end the whole call).
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
    // GdiFlush before reading a DIB the GDI batch may not have written yet.
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
    // Live: the pipeline must not ask for a backlog after a pause.
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    // No do-timestamp: it would overwrite our zero-based timestamps with the
    // arrival time.
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

// ------------------------------------------------- which application ------

// Resolved at runtime rather than linked, so the package gains no psapi or
// version.lib import; this also avoids declarations hidden by the
// _WIN32_WINNT floor.
QString fileDescriptionUncached(const QString &executable);

using QueryFullProcessImageNameWFn = BOOL(WINAPI *)(HANDLE, DWORD, LPWSTR,
                                                    PDWORD);
using GetFileVersionInfoSizeWFn = DWORD(WINAPI *)(LPCWSTR, LPDWORD);
using GetFileVersionInfoWFn = BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
using VerQueryValueWFn = BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/// The executable's description ("Brave Browser", "Windows Explorer"), as
/// Task Manager shows it. Cached by path: the picker asks for every window,
/// and parsing the same version resource repeatedly on the GUI thread is
/// wasteful. Bounded by the number of distinct programs with windows.
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

    // The description lives under the file's own language and codepage; read
    // them from the translation table.
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
            // NUL-terminated overload: the reported length includes the
            // terminator.
            const QString described =
                QString::fromWCharArray(static_cast<const wchar_t *>(text))
                    .trimmed();
            if (!described.isEmpty())
                return described;
        }
    }
    return {};
}

/// Which application a window belongs to, best effort. Empty is a valid
/// answer; a wrong name would be worse than none.
QString applicationNameFor(HWND window)
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
    if (!imageNameFn)
        return {};

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
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

    const QString executable =
        QString::fromWCharArray(path, static_cast<int>(length));
    const QString described = fileDescription(executable);
    if (!described.isEmpty())
        return described;
    // Fallback: the capitalised file name without extension.
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
        return TRUE;   // an owned dialog or tool window

    // Never our own windows: sharing the call into itself.
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == ctx->ownProcess)
        return TRUE;

    const LONG exStyle = GetWindowLong(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return TRUE;   // not on the taskbar

    // Cloaked windows (UWP shells, background tabs) report visible while
    // showing nothing.
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
        return TRUE;   // untitled: nothing to show in a list

    // The same geometry the capture uses, so the row matches what is sent.
    WindowGeometry geo;
    if (!windowGeometry(window, &geo))
        return TRUE;

    lightning::wincap::WindowInfo info;
    info.handle = static_cast<quint64>(reinterpret_cast<uintptr_t>(window));
    info.title = QString::fromWCharArray(title, length);
    info.application = applicationNameFor(window);
    info.width = geo.cropWidth;
    info.height = geo.cropHeight;
    ctx->out->append(info);
    return TRUE;
}

} // namespace

namespace lightning::wincap {

bool available() { return true; }

namespace {

/// Copy a device context region into a QImage, scaled to fit `maxEdge`.
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
    // Copied: the DIB is freed when this function returns.
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

/// The same walk, looking for a monitor by the platform's own device name.
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
    // Cast explicitly: whether MONITORINFOEXW* converts to LPMONITORINFO
    // implicitly differs between the Windows SDK and mingw-w64 headers.
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
        return FALSE;   // stop: found the requested monitor
    }
    return TRUE;
}

} // namespace

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
    // The same call and crop as the capture, so the preview matches.
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
    // Virtual-desktop coordinates: other monitors do not start at 0,0.
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

} // namespace lightning::wincap

#else // !Q_OS_WIN

namespace lightning::wincap {

// Off Windows this is not the mechanism: Linux uses the xdg portal and macOS
// captures displays through avfvideosrc.
bool available() { return false; }
QList<WindowInfo> enumerateWindows() { return {}; }
bool displayForDeviceName(const QString &, int *, int *, int *)
{
    return false;
}
QImage captureThumbnail(quint64, int) { return {}; }
QImage captureScreenThumbnail(int, int) { return {}; }
void registerWindowCaptureSrc() {}
const char *windowCaptureSrcName() { return "lightningwindowcapturesrc"; }

} // namespace lightning::wincap

#endif
