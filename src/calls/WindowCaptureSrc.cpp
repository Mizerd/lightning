#include "calls/WindowCaptureSrc.h"

#include <algorithm>

namespace lightning::wincap {

Size fitInto(int srcW, int srcH, int maxW, int maxH)
{
    if (srcW < 2 || srcH < 2 || maxW < 2 || maxH < 2)
        return {};
    // NEVER ABOVE 1.0: a window smaller than the ceiling keeps its own size.
    const double scale =
        (std::min)({1.0, static_cast<double>(maxW) / srcW,
                    static_cast<double>(maxH) / srcH});
    // Truncate, then clear the low bit. Both only ever shrink, so neither can
    // push a result back over the ceiling it was just fitted into.
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

/// A top-down 32-bit DIB we own, so a frame is a straight memcpy out.
///
/// The DC's ORIGINAL bitmap is kept, because `DeleteObject` on a bitmap that
/// is still selected into a device context does nothing and returns zero —
/// the DIB stays committed. On the resize path, which used to build one of
/// these per frame, that is 30 MiB a frame leaked for a 4K window.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previous = nullptr;
    void *pixels = nullptr;
};

struct LightningWindowCaptureSrc {
    GstPushSrc parent;

    /// The HWND, as a plain integer property so nothing Windows-shaped has to
    /// cross a GObject boundary.
    guint64 hwnd;

    /// THE NEGOTIATED FRAME SIZE — what the caps SAY, learned in `set_caps`.
    ///
    /// It used to be assumed instead of learned, and that was the whole of
    /// "Brave shared just the top right corner of the screen and it was all
    /// pixelated". The element fixated its src pad wherever downstream would
    /// let it — measured on a real Windows run, 1920x1080 for a 3840x2100
    /// window — and then went on allocating and copying a buffer of the
    /// WINDOW'S size. Downstream reads a buffer at the stride the CAPS imply,
    /// so every displayed row was half a source row and only the top quarter
    /// of the window was ever consumed. A 1556-wide window escaped it purely
    /// because 1556 is under the ceiling, so its stride happened to match and
    /// the damage was a silently cropped bottom edge.
    ///
    /// Held for the life of the element: a window the user then resizes must
    /// not renegotiate underneath the encoder and the SFU, so later frames
    /// are fitted into this size instead.
    gint outWidth;
    gint outHeight;
    Surface out;   // BGRA, top-down

    /// WHAT WE PRINT INTO: the window's own bounds, remade only when the
    /// window is resized. The previous revision built one of these PER FRAME
    /// on the resize path, which for a 4K window is a 32 MB allocation and
    /// free thirty times a second.
    gint printWidth;
    gint printHeight;
    Surface print;

    /// The window's VISIBLE size when the share started, used only to ask for
    /// a sensible frame size during fixation.
    gint startWidth;
    gint startHeight;

    /// WHICH FRAME SLOT the next buffer fills, counted from ZERO at the start
    /// of the share. Its timestamp is this times the frame duration.
    ///
    /// Counted from zero and NOT from the pipeline's running time, and the
    /// difference is not a detail: a publish bin is added to a publisher
    /// pipeline that has been PLAYING since the call was joined, and
    /// `videorate` downstream starts its output clock at SEGMENT START. Hand
    /// it a first buffer stamped with the age of the call and it owes thirty
    /// duplicate frames for every second of that age, which it emits as fast
    /// as the encoder will take them: a full-rate stream of ONE picture. That
    /// is the camera freeze, and it is why this element — which stamps from
    /// zero — was the one video source on Windows that worked.
    ///
    /// ADVANCED FROM THE CLOCK, not blindly incremented. The capture can be
    /// slower than 30 fps (PrintWindow on a 4K window was measured at 17-35),
    /// and a source that emits 17 pictures a second while stamping them 33 ms
    /// apart is telling the receiver that time is passing at half speed. The
    /// far end then renders in slow motion and drifts further behind for as
    /// long as the share runs.
    guint64 frameIndex;
    /// Where slot zero sits on the CLOCK, so the wait can pace without the
    /// timestamps having to carry running time.
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

/// DwmGetWindowAttribute, resolved at RUNTIME rather than linked.
///
/// Linking dwmapi would add an import to the Windows link line and therefore
/// a new edge in a packaging closure that is validated symbol by symbol. This
/// asks for one function and, if it is not there, degrades to the plain window
/// rect — a slightly generous rectangle is a far better failure than a new
/// dependency in the artifact.
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

/// Where a window is, and which part of what it draws a person actually sees.
///
/// TWO RECTANGLES, and conflating them is how the capture came to disagree
/// with itself. `PrintWindow` renders the WHOLE WINDOW from the window rect's
/// own origin; the previous revision sized its surface to `GetClientRect`, so
/// it caught the frame in the top of the picture and lost the same number of
/// pixels off the bottom of the content. And on Windows 10/11 the window rect
/// is bigger again than what DWM paints — an invisible resize border sits
/// outside the visible frame — so a maximised window printed with a margin of
/// nothing around it. `DWMWA_EXTENDED_FRAME_BOUNDS` is the visible rectangle,
/// and the difference between the two is exactly the offset to crop by.
///
/// ONE derivation, shared by the capture, the picker's list and the picker's
/// preview. Three private copies of it is three chances to offer a tile whose
/// shape is not what pressing Share sends.
struct WindowGeometry {
    int printWidth = 0;    ///< what PrintWindow will draw
    int printHeight = 0;
    int cropX = 0;         ///< where the visible frame starts inside that
    int cropY = 0;
    int cropWidth = 0;     ///< and how big it is, rounded down to even
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
        // Only ever a CROP. A reported bound reaching outside the window rect
        // would have us read pixels PrintWindow never drew.
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
    // NEGATIVE height means top-down, which is the order video/x-raw wants.
    // A bottom-up DIB would need the rows reversed on every single frame.
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

/// IsHungAppWindow, resolved at runtime like everything else here.
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

/// Ask the window to draw itself, at the size of its WINDOW rect.
///
/// Cleared first: PrintWindow may leave regions untouched for a window that
/// is partly unrendered, and stale pixels from the previous frame there look
/// like tearing rather than like the blank they are.
///
/// A HUNG WINDOW IS NEVER ASKED. PrintWindow sends the window a message and
/// waits for it, with no timeout, on whichever thread called — the streaming
/// thread here and Qt's image-reader thread in the picker. An application
/// that has stopped pumping its queue would stall the capture with no error
/// and no EOS, which is the frozen-picture failure this lane keeps producing
/// by other means.
void printInto(HWND window, const Surface &surface, int width, int height)
{
    fillBlack(surface.dc, width, height);
    if (isHungAppWindow() && isHungAppWindow()(window))
        return;
    if (PrintWindow(window, surface.dc, PW_RENDERFULLCONTENT))
        return;
    // ONLY WHEN PrintWindow SAYS IT FAILED, and deliberately not when it
    // succeeds while painting nothing.
    //
    // A window rendering through its own swapchain returns TRUE and leaves the
    // bitmap blank. It is tempting to detect that and read the window's DC
    // instead — an earlier revision of this round did exactly that — but the
    // header states the rule this file is built on: reading pixels from
    // anywhere but the window's own PrintWindow output was rejected outright,
    // and a window that will not print is REPORTED AS A BLACK FRAME rather
    // than worked around. Whether a DWM redirection surface can ever hand back
    // another window's pixels is an assumption about an implementation, and
    // this is the one file that must not make it. A useless black share is a
    // recoverable disappointment; a share that leaks a window the user did not
    // choose is not.
    //
    // GetWindowDC, not GetDC: the surface is sized to the WINDOW rect, and
    // GetDC hands back the CLIENT area's origin — copying from it would land
    // the content offset by the frame it is missing.
    HDC windowDc = GetWindowDC(window);
    if (windowDc) {
        BitBlt(surface.dc, 0, 0, width, height, windowDc, 0, 0, SRCCOPY);
        ReleaseDC(window, windowDc);
    }
}

// ---------------------------------------------------------- caps helpers --

/// Whether a caps field would accept `wanted`. An ABSENT field is
/// unconstrained, and anything exotic is given the benefit of the doubt:
/// refusing a structure we do not understand would send us to the fallback
/// for no reason.
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

/// The largest value a caps field permits, or `fallback` when it does not say.
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

/// Draw the window into the negotiated frame. Returns false only when the
/// window is GONE.
bool paintWindow(LightningWindowCaptureSrc *self)
{
    auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(self->hwnd));
    if (!IsWindow(window))
        return false;

    WindowGeometry geo;
    if (IsIconic(window) || !windowGeometry(window, &geo)) {
        // Minimised, or momentarily degenerate. A BLACK FRAME, not an EOS: a
        // window the user minimised is still the window they chose to share,
        // and ending the stream would make them start the share again.
        fillBlack(self->out.dc, self->outWidth, self->outHeight);
        return true;
    }

    if (geo.printWidth != self->printWidth
        || geo.printHeight != self->printHeight || !self->print.pixels) {
        // THE WINDOW WAS RESIZED. Rebuild the print surface once, here, and
        // never per frame.
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
        // The ordinary case, and a straight blit.
        BitBlt(self->out.dc, 0, 0, self->outWidth, self->outHeight,
               self->print.dc, geo.cropX, geo.cropY, SRCCOPY);
        return true;
    }

    // The window is no longer the shape the stream was negotiated at.
    // LETTERBOX rather than stretch: a resized window must not have faces or
    // text squashed to fit the old rectangle, and the stream's resolution —
    // which the encoder and the SFU already agreed — must not change.
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

/// Hold until this frame is due.
///
/// `gdiscreencapsrc` does exactly this, for exactly this reason: a capture
/// with no hardware to block on runs `create` as fast as the API returns.
/// Measured on a real Windows desktop, PrintWindow alone paced a 4K window at
/// 32-35 fps — a whole core spent producing frames the caps call 30 and
/// `videorate` then throws away, and worse again for a small window where
/// PrintWindow is cheap.
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
    // THE WAIT TARGET IS NOT THE TIMESTAMP. `frameIndex` counts slots from
    // zero; `pacingBase` is where slot zero was pinned to the clock. Pacing on
    // the running time directly would mean stamping buffers with it, which is
    // the videorate back-fill described on the field above.
    if (!self->pacingStarted) {
        self->pacingStarted = TRUE;
        self->pacingBase = running;
    }
    if (self->pacingBase > running + GST_SECOND) {
        // The clock jumped backwards. Re-pin — and REBASE THE INDEX with it,
        // or `due` lands `frameIndex / 30` seconds in the future: ten minutes
        // into a share that is a ten-minute wait, on a source that installs no
        // `unlock` vfunc and so could not be interrupted out of it.
        const GstClockTime span = self->frameIndex * kFrameDuration;
        self->pacingBase = running > span ? running - span : 0;
    }
    // SKIP THE SLOTS WE MISSED rather than stamping them late. A capture that
    // cannot keep up should drop frames, never slow the stream's own clock
    // down: `videorate` fills a gap by repeating, and a receiver renders on
    // the frame timeline it is given.
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
        // NAMED, not silent. "Screen sharing couldn't start" with nothing
        // behind it is the failure mode this whole lane has been paying for.
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
    // Only what fixation needs. The surfaces are built where their sizes are
    // KNOWN: the output one in set_caps, the print one on the first frame.
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

    // PREFER A STRUCTURE THAT CAN CARRY THE WINDOW'S OWN SIZE, and this is
    // the half of the garbling fix that is about QUALITY rather than
    // correctness.
    //
    // The publish pipeline ends in a 1920x1080 ceiling with `videoscale` in
    // front of it, so the source is free to hand over the window at its
    // native size and let the scaler do the work. But a caps query answered
    // through videoconvertscale puts the DOWNSTREAM-restricted structure
    // FIRST — passthrough is cheaper, so it is offered first — and appends
    // the size-opened one after it. Fixating structure 0 blind therefore
    // lands inside the ceiling and leaves this element to scale in GDI, one
    // HALFTONE StretchBlt per frame. `gdiscreencapsrc` never met this because
    // it reports FIXED caps: the restricted structure intersects to nothing
    // and drops out, which is exactly why a MONITOR share negotiated the full
    // 3840x2160 and a WINDOW share did not.
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
            // The chosen one first, then the rest in their original order.
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
    // FIT, never two independent clamps. A 3840x2100 window clamped to
    // width<=1920 and height<=1080 separately is a 16:9 rectangle holding a
    // 1.83:1 picture, and everything in it is stretched.
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
    // AND THE PIXEL ASPECT RATIO, WHICH IS NOT OPTIONAL.
    //
    // The publish ceiling pins `pixel-aspect-ratio=1/1` so videoscale answers
    // a size ceiling by choosing a SIZE rather than by signalling the shape as
    // a PAR that VP8 and RTP silently drop. Pinning it downstream is what
    // makes videoconvertscale offer this element an OPEN PAR RANGE upstream —
    // and a field this fixate leaves alone falls through to
    // `gst_caps_fixate`, which takes a range's MINIMUM. That is
    // 1/2147483647, and videoscale then overflows converting it back to 1/1:
    //
    //   3840x2100 -> ERROR negotiation problem (integer overflow)
    //   3840x2160 -> ERROR negotiation problem
    //   1557x1213 -> "succeeds" with pixel-aspect-ratio=1/2147483647
    //
    // Measured on a faithful clone of this element, including the reorder
    // above — which is what makes the size-OPENED structure the one being
    // fixated, and therefore the one carrying the open PAR. Every case
    // negotiates at 1/1 with this line.
    //
    // A probe built on `videotestsrc` CANNOT SEE THIS: videotestsrc fixates
    // PAR in its own fixate vfunc, so it never meets the open range. The
    // measurement that missed this defect was taken exactly that way.
    gst_structure_fixate_field_nearest_fraction(structure,
                                                "pixel-aspect-ratio", 1, 1);
    return GST_BASE_SRC_CLASS(lightning_window_capture_src_parent_class)
        ->fixate(base, caps);
}

/// LEARN THE SIZE WE AGREED TO. Without this vfunc the element fixated one
/// size and produced another, which is the defect this round exists to fix.
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
        // The window CLOSED. End the stream cleanly rather than erroring:
        // the user closing what they were sharing is a normal thing to do,
        // and an error here would tear down the whole call.
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
    // Without it the first frames of a share can be torn or empty.
    GdiFlush();
    memcpy(map.data, self->out.pixels, size);
    gst_buffer_unmap(buffer, &map);
    // SELF-DESCRIBING. CreateDIBSection at 32 bpp gives a stride of exactly
    // width * 4, which is also what BGRA means at that width — but a buffer
    // that SAYS so is a buffer no consumer has to take it on trust from, and
    // taking a size on trust is precisely what this round is here to stop.
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
    // LIVE, so the pipeline treats it as a capture: it must not be asked to
    // produce the backlog a non-live source would owe after a pause.
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    // NOT do-timestamp. It would overwrite the timestamps `createFrame`
    // assigns with the arrival time — the same instant `create` returned — so
    // nothing downstream would ever wait, and this element's own pacing is
    // what makes it a 30 fps source rather than a busy loop.
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

// ------------------------------------------------- which application ------

// Everything below is RESOLVED AT RUNTIME rather than linked, for the reason
// dwmGetWindowAttribute() already gives: this file's whole point is that a
// window capture costs the artifact no new dependency, and a psapi or
// version.lib import would be a new edge in a packaging closure that is
// validated symbol by symbol. It also sidesteps the `_WIN32_WINNT` floor this
// build compiles at, which hides some of these declarations outright.
QString fileDescriptionUncached(const QString &executable);

using QueryFullProcessImageNameWFn = BOOL(WINAPI *)(HANDLE, DWORD, LPWSTR,
                                                    PDWORD);
using GetFileVersionInfoSizeWFn = DWORD(WINAPI *)(LPCWSTR, LPDWORD);
using GetFileVersionInfoWFn = BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
using VerQueryValueWFn = BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/// The executable's own description — "Brave Browser", "Windows Explorer" —
/// which is the string Task Manager shows, so it is the name the user already
/// associates with the window.
///
/// CACHED BY PATH, because the picker asks this for EVERY window it lists and
/// the answer is a property of the file: without the cache, opening the picker
/// on a desktop with a dozen Chrome windows reads and parses the same
/// multi-hundred-KB version resource a dozen times, on the thread that is
/// trying to show a dialog. Session-lifetime is correct — an executable does
/// not change description while it is running — and the map is bounded by how
/// many distinct programs have a window open.
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

    // The description lives under the file's OWN language and codepage, and
    // there is no fixed one to guess at: ask the translation table.
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
            // The NUL-terminated overload deliberately: the length VerQuery
            // reports counts the terminator, so passing it would carry a NUL
            // into the string and out into a label.
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
/// answer and the picker says only the title in that case — a WRONG
/// application name would be worse than none, since the whole point of it is
/// to tell the user what they are about to broadcast.
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
    // Fallback: the file name without its extension. "brave" reads better
    // capitalised, and it is still an honest answer.
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
        return TRUE;   // untitled: nothing to show a person in a list

    // THE SAME GEOMETRY THE CAPTURE USES. A row that advertised the client
    // rect while the capture delivered the visible frame is a row promising a
    // shape the share does not send.
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
    // COPIED, not wrapped: the DIB dies with this function and a QImage
    // sharing its memory would be a dangling read the moment it is drawn.
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
    // CAST EXPLICITLY. Whether `MONITORINFOEXW *` converts to
    // `LPMONITORINFO` on its own depends on which spelling of the struct the
    // Windows headers use — C++ inheritance in the SDK, an anonymous member
    // in mingw-w64 — and that is a difference this build only meets on the
    // platform it cannot compile on locally.
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
    WindowGeometry geo;
    if (!windowGeometry(window, &geo))
        return {};

    Surface surface;
    if (!createSurface(geo.printWidth, geo.printHeight, &surface))
        return {};
    // THE SAME CALL THE CAPTURE MAKES, cropped the same way. A preview drawn
    // differently could show a picture the share cannot actually send.
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
    // The virtual desktop's own coordinates: a second monitor starts at a
    // non-zero x, and grabbing from 0,0 would preview the wrong screen.
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

// Off Windows this is not "unimplemented", it is "not the mechanism". Linux
// has the xdg portal, which owns the picker AND hands back a PipeWire node
// for exactly what was chosen; macOS captures a display through avfvideosrc.
// Reporting unavailable keeps the picker honest on both.
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
