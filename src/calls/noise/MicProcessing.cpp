#include "calls/noise/MicProcessing.h"

#include "calls/noise/DenoiseElement.h"

#include <QStringList>

#include <gst/gst.h>

namespace calls::noise {
namespace {

bool factoryExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

/// webrtcdsp exactly as voiceProcessingDescription() configures it.
GstElement *makeDsp(bool suppress)
{
    GstElement *dsp = gst_element_factory_make("webrtcdsp", nullptr);
    if (!dsp)
        return nullptr;
    g_object_set(dsp, "echo-cancel", FALSE, "gain-control", TRUE,
                 "noise-suppression", suppress ? TRUE : FALSE, nullptr);
    return dsp;
}

bool dspSuppresses(GstElement *dsp)
{
    gboolean on = FALSE;
    g_object_get(dsp, "noise-suppression", &on, nullptr);
    return on;
}

struct SwapContext {
    GstBin *parent;
    bool suppress;
    /// Replace even when the element already reports the wanted value: it
    /// may have negotiated (and so read its configuration) before it was set.
    bool force;
};

void freeSwapContext(gpointer data)
{
    auto *context = static_cast<SwapContext *>(data);
    gst_object_unref(context->parent);
    delete context;
}

bool linkThrough(GstPad *upstream, GstElement *element, GstPad *downstream)
{
    GstPad *sink = gst_element_get_static_pad(element, "sink");
    GstPad *src = gst_element_get_static_pad(element, "src");
    const bool linked = sink && src
        && gst_pad_link(upstream, sink) == GST_PAD_LINK_OK
        && gst_pad_link(src, downstream) == GST_PAD_LINK_OK;
    if (!linked) {
        if (sink && gst_pad_is_linked(sink))
            gst_pad_unlink(upstream, sink);
        if (src && gst_pad_is_linked(src))
            gst_pad_unlink(src, downstream);
    }
    if (sink)
        gst_object_unref(sink);
    if (src)
        gst_object_unref(src);
    return linked;
}

/// Runs with the pad feeding webrtcdsp IDLE: no buffer is inside the element
/// and none can enter until this returns. Replaces it with one configured
/// for `suppress`. The new element receives the pad's sticky events (caps,
/// segment) again with the next buffer, because linking reschedules them.
GstPadProbeReturn swapDsp(GstPad *upstream, GstPadProbeInfo *, gpointer data)
{
    auto *context = static_cast<SwapContext *>(data);
    // Looked up now, not when the probe was added: two quick switches queue
    // two probes, and each must act on whatever is current.
    GstElement *old = gst_bin_get_by_name(context->parent, kDspName);
    if (!old)
        return GST_PAD_PROBE_REMOVE;
    if (!context->force && dspSuppresses(old) == context->suppress) {
        gst_object_unref(old);
        return GST_PAD_PROBE_REMOVE;
    }
    GstPad *oldSink = gst_element_get_static_pad(old, "sink");
    GstPad *oldSrc = gst_element_get_static_pad(old, "src");
    GstPad *feeding = oldSink ? gst_pad_get_peer(oldSink) : nullptr;
    GstPad *downstream = oldSrc ? gst_pad_get_peer(oldSrc) : nullptr;
    // Only the chain this file builds: anything else is left alone.
    GstElement *fresh = (feeding == upstream && downstream)
        ? makeDsp(context->suppress)
        : nullptr;
    if (fresh) {
        gst_object_ref_sink(fresh);
        gst_pad_unlink(upstream, oldSink);
        gst_pad_unlink(oldSrc, downstream);
        gst_element_set_state(old, GST_STATE_NULL);
        gst_bin_remove(context->parent, old); // `old` keeps our reference
        gst_object_set_name(GST_OBJECT(fresh), kDspName);
        bool replaced = gst_bin_add(context->parent, fresh);
        if (replaced && !linkThrough(upstream, fresh, downstream)) {
            gst_element_set_state(fresh, GST_STATE_NULL);
            gst_bin_remove(context->parent, fresh);
            replaced = false;
        }
        if (replaced) {
            gst_element_sync_state_with_parent(fresh);
        } else {
            // Put the old one back rather than leave the microphone cut.
            gst_bin_add(context->parent, old);
            linkThrough(upstream, old, downstream);
            gst_element_sync_state_with_parent(old);
        }
        gst_object_unref(fresh);
    }
    if (feeding)
        gst_object_unref(feeding);
    if (downstream)
        gst_object_unref(downstream);
    if (oldSink)
        gst_object_unref(oldSink);
    if (oldSrc)
        gst_object_unref(oldSrc);
    gst_object_unref(old);
    return GST_PAD_PROBE_REMOVE;
}

/// Returns false when there was nothing to schedule on.
bool scheduleDspSwap(GstElement *dsp, bool suppress, bool force)
{
    GstPad *sink = gst_element_get_static_pad(dsp, "sink");
    GstPad *upstream = sink ? gst_pad_get_peer(sink) : nullptr;
    GstObject *parent = gst_object_get_parent(GST_OBJECT(dsp));
    bool scheduled = false;
    if (upstream && parent && GST_IS_BIN(parent)) {
        auto *context = new SwapContext{GST_BIN(parent), suppress, force};
        parent = nullptr; // owned by the context now
        // IDLE runs at once when nothing is flowing, otherwise right after
        // the buffer in flight; a muted (valve-closed) chain is idle.
        gst_pad_add_probe(upstream, GST_PAD_PROBE_TYPE_IDLE, swapDsp, context,
                          freeSwapContext);
        scheduled = true;
    }
    if (parent)
        gst_object_unref(parent);
    if (upstream)
        gst_object_unref(upstream);
    if (sink)
        gst_object_unref(sink);
    return scheduled;
}

} // namespace

bool webrtcDspAvailable()
{
    // Not cached before init: the answer would stay "no" for the process.
    if (!gst_is_initialized())
        return false;
    static const bool available = factoryExists("webrtcdsp");
    return available;
}

bool denoiseElementAvailable()
{
    if (!gst_is_initialized())
        return false;
    registerDenoiseElement();
    static const bool available = factoryExists(denoiseElementName());
    return available;
}

QString voiceProcessingDescription(Mode mode, bool dspAvailable,
                                   bool denoiseAvailable)
{
    QString out;
    if (denoiseAvailable) {
        // Any capture format reaches it as F32 mono 48 kHz; the front of the
        // chain already resampled, so audioresample is a pass-through there.
        out += QStringLiteral("! audioconvert ! audioresample ! %1 "
                              "! %2 name=%3 mode=%4 ")
                   .arg(QLatin1String(denoiseCaps()),
                        QLatin1String(denoiseElementName()),
                        QLatin1String(kDenoiseName),
                        QLatin1String(modeKey(mode)));
    }
    // dithering=none: through a pass-through denoiser, 16-bit capture goes to
    // F32 and back bit-exact; audioconvert's default TPDF dither would add
    // noise to the "Off" stream.
    // Without the denoiser it is still an ELEMENT, never bare caps: gst_parse
    // reads a caps string straight after another caps string (the front's
    // `audio/x-raw,channels=1`) as an element name.
    const QString backFromFloat = denoiseAvailable
        ? QStringLiteral("! audioconvert dithering=none ")
        : QStringLiteral("! audioconvert ");
    if (dspAvailable) {
        // webrtcdsp processes fixed 10 ms S16 chunks, hence the caps. It
        // keeps its high-pass filter and gain control in every mode; only its
        // suppressor follows the selection. Echo cancellation stays off: it
        // needs a webrtcechoprobe in the playback path, which does not exist.
        out += QStringLiteral("%1! audio/x-raw,format=S16LE,rate=48000 "
                              "! webrtcdsp name=%2 echo-cancel=false "
                              "gain-control=true noise-suppression=%3 "
                              "! audioconvert ")
                   .arg(backFromFloat, QLatin1String(kDspName),
                        dspSuppressesNoise(mode) ? QLatin1String("true")
                                                 : QLatin1String("false"));
    } else {
        out += backFromFloat;
    }
    return out;
}

QString voiceProcessingDescription(Mode mode)
{
    return voiceProcessingDescription(mode, webrtcDspAvailable(),
                                      denoiseElementAvailable());
}

Unavailable availability(Mode mode, bool engineAvailable)
{
    if (mode == Mode::Off)
        return Unavailable::None;
    if (!engineAvailable)
        return Unavailable::NoCallEngine;
    if (mode == Mode::WebRtc)
        return webrtcDspAvailable() ? Unavailable::None
                                    : Unavailable::NoWebrtcDsp;
    if (!backendCompiledIn(mode) || !denoiseElementAvailable())
        return Unavailable::NotInThisBuild;
    return Unavailable::None;
}

const char *unavailableKey(Unavailable reason)
{
    switch (reason) {
    case Unavailable::None:
        return "";
    case Unavailable::NoCallEngine:
        return "no-call-engine";
    case Unavailable::NoWebrtcDsp:
        return "no-webrtcdsp";
    case Unavailable::NotInThisBuild:
        return "not-in-build";
    }
    return "";
}

QString applyMode(GstElement *root, Mode mode, DspSwitch when)
{
    if (!root || !GST_IS_BIN(root))
        return QStringLiteral("no chain");
    QStringList did;
    bool denoiser = false;
    if (GstElement *denoise =
            gst_bin_get_by_name(GST_BIN(root), kDenoiseName)) {
        setDenoiseMode(denoise, mode);
        gst_object_unref(denoise);
        denoiser = true;
        did << QStringLiteral("denoise=%1").arg(QLatin1String(modeKey(mode)));
    }
    if (denoiser && when == DspSwitch::WaitForDenoiser
        && dspWaitsForDenoiser(mode)) {
        // webrtcdsp keeps suppressing until the new backend runs.
        did << QStringLiteral("dsp waits for the denoiser");
    } else {
        did << applyDspMode(root, mode);
    }
    return did.join(QLatin1Char(' '));
}

QString applyDspMode(GstElement *root, Mode mode)
{
    if (!root || !GST_IS_BIN(root))
        return QStringLiteral("no chain");
    QStringList did;
    if (GstElement *dsp = gst_bin_get_by_name(GST_BIN(root), kDspName)) {
        const bool want = dspSuppressesNoise(mode);
        GstPad *sink = gst_element_get_static_pad(dsp, "sink");
        GstCaps *caps = sink ? gst_pad_get_current_caps(sink) : nullptr;
        if (!caps) {
            // Not negotiated yet: its setup reads this value when it does.
            g_object_set(dsp, "noise-suppression", want ? TRUE : FALSE,
                         nullptr);
            // Unless it negotiated in between: then replace it regardless.
            caps = sink ? gst_pad_get_current_caps(sink) : nullptr;
            if (caps && scheduleDspSwap(dsp, want, /*force=*/true))
                did << QStringLiteral("dsp replaced (late)");
            else
                did << QStringLiteral("dsp configured");
        } else if (dspSuppresses(dsp) != want) {
            did << (scheduleDspSwap(dsp, want, /*force=*/false)
                        ? QStringLiteral("dsp replaced")
                        : QStringLiteral("dsp unlinked, left alone"));
        } else {
            did << QStringLiteral("dsp unchanged");
        }
        if (caps)
            gst_caps_unref(caps);
        if (sink)
            gst_object_unref(sink);
        gst_object_unref(dsp);
    }
    return did.isEmpty() ? QStringLiteral("no dsp") : did.join(QLatin1Char(' '));
}

} // namespace calls::noise
