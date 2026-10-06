#include "calls/noise/DenoiseElement.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

// Native-endian float: the frames are handed to the backend as float*.
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define LIGHTNING_DENOISE_FORMAT "F32LE"
#else
#define LIGHTNING_DENOISE_FORMAT "F32BE"
#endif

namespace calls::noise {
namespace {

// No spaces: the same text goes into a gst_parse description.
constexpr const char *kParseCaps = "audio/x-raw,format=" LIGHTNING_DENOISE_FORMAT
                                   ",rate=48000,channels=1,layout=interleaved";

// ── Backend construction (never on the streaming thread) ──────────────────

std::mutex g_factoryMutex;
SuppressorFactory g_factory; // empty: createSuppressor()

std::atomic<int> g_liveElements{0};

/// Hands the freed pages of a destroyed neural backend back to the system.
///
/// A neural backend allocates on the threads that touch it: its model is
/// unpacked on a pool thread, and tract allocates per frame on the streaming
/// thread. Every call builds a new pipeline with new streaming threads, and
/// glibc gives a new thread its own malloc arena; memory freed inside a
/// non-main arena stays mapped unless the top of that heap happens to be
/// free. Measured live (DeepFilterNet, 2026-10-06): RSS +11..19 MB per
/// leave/rejoin while Off stayed flat. malloc_trim(0) releases the free pages
/// of every arena. Runs on the calling thread, which is never the streaming
/// thread; it costs a few ms and only after a backend was destroyed.
void releaseFreedBackendMemory()
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

std::unique_ptr<NoiseSuppressor> buildSuppressor(Mode mode)
{
    SuppressorFactory factory;
    {
        std::lock_guard<std::mutex> lock(g_factoryMutex);
        factory = g_factory;
    }
    std::unique_ptr<NoiseSuppressor> made;
    try {
        made = factory ? factory(mode) : createSuppressor(mode);
    } catch (...) {
        made.reset();
    }
    // The element frames for exactly kFrameSamples; anything else, or a
    // backend that says it failed, is no backend at all.
    if (made
        && (!made->ok() || made->frameSize() != kFrameSamples
            || made->latencySamples() < 0 || made->mode() != mode)) {
        made.reset();
    }
    return made;
}

/// One requested change, built off the streaming thread and adopted by it
/// between frames. After adoption it carries the RETIRED backend back off the
/// streaming thread to be destroyed.
struct Handoff {
    std::unique_ptr<NoiseSuppressor> suppressor;
    Mode requested = Mode::Off;
    bool ok = true;
    unsigned long long generation = 0;
};

struct State {
    // Control side (GUI or pool threads). The mutex serialises requests and
    // publications so a slow, superseded build can never overwrite a newer
    // request; the streaming thread never takes it.
    std::mutex controlMutex;
    std::atomic<int> requested{int(Mode::Off)};
    std::atomic<unsigned long long> generation{0};
    std::atomic<Handoff *> next{nullptr};

    // Streaming thread only.
    std::unique_ptr<NoiseSuppressor> active;
    std::array<float, kFrameSamples> frameA{};
    std::array<float, kFrameSamples> frameB{};
    /// Input being collected into the next frame.
    float *filling = frameA.data();
    /// The last processed frame, being emitted.
    float *draining = frameB.data();
    int position = 0;

    // Status, read from any thread.
    std::atomic<int> activeMode{int(Mode::Off)};
    std::atomic<bool> ok{true};
    std::atomic<int> latency{0};
    std::atomic<unsigned long long> frames{0};

    ~State() { delete next.exchange(nullptr); }
};

struct LightningDenoise {
    GstBaseTransform parent;
    State *state;
};

struct LightningDenoiseClass {
    GstBaseTransformClass parent;
};

GType lightning_denoise_get_type();
#define LIGHTNING_TYPE_DENOISE (lightning_denoise_get_type())
#define LIGHTNING_DENOISE(obj)                                                 \
    (reinterpret_cast<LightningDenoise *>(obj))

G_DEFINE_TYPE(LightningDenoise, lightning_denoise, GST_TYPE_BASE_TRANSFORM)

const GEnumValue kModeValues[] = {
    {int(Mode::Off), "No noise suppression", "off"},
    {int(Mode::WebRtc),
     "WebRTC (runs in webrtcdsp; pass-through here)", "webrtc"},
    {int(Mode::RNNoise), "RNNoise", "rnnoise"},
    {int(Mode::DeepFilterNet), "DeepFilterNet", "deepfilternet"},
    {0, nullptr, nullptr},
};

GType modeEnumType()
{
    static const GType type =
        g_enum_register_static("LightningDenoiseMode", kModeValues);
    return type;
}

enum {
    PROP_0,
    PROP_MODE,
    PROP_ACTIVE_MODE,
    PROP_BACKEND_OK,
    PROP_FRAMES,
    PROP_LATENCY,
};

GstStaticPadTemplate kSinkTemplate = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format=(string)" LIGHTNING_DENOISE_FORMAT
                    ", rate=(int)48000, channels=(int)1, "
                    "layout=(string)interleaved"));

GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format=(string)" LIGHTNING_DENOISE_FORMAT
                    ", rate=(int)48000, channels=(int)1, "
                    "layout=(string)interleaved"));

// ── Retiring: backends are destroyed on GStreamer's thread pool ───────────

void destroyHandoff(GstElement *, gpointer data)
{
    auto *h = static_cast<Handoff *>(data);
    const bool hadBackend = h->suppressor != nullptr;
    delete h;
    // A pool thread: trimming here never stalls the stream.
    if (hadBackend)
        releaseFreedBackendMemory();
}

/// Hands `h` (and the backend it carries) to a pool thread to be destroyed.
/// Never called from finalize: call_async takes a reference on the element.
void retire(GstElement *element, Handoff *h)
{
    if (!h)
        return;
    gst_element_call_async(element, destroyHandoff, h, nullptr);
}

/// Control side, under State::controlMutex.
void publishLocked(GstElement *element, State *s, Handoff *h)
{
    if (h->generation != s->generation.load(std::memory_order_acquire)) {
        // Superseded while it was being built.
        retire(element, h);
        return;
    }
    Handoff *stale = s->next.exchange(h, std::memory_order_acq_rel);
    retire(element, stale);
}

struct Request {
    Mode mode;
    unsigned long long generation;
};

void freeRequest(gpointer data) { delete static_cast<Request *>(data); }

/// Pool thread: build the backend, then publish it for the streaming thread.
void buildTask(GstElement *element, gpointer data)
{
    const auto *request = static_cast<const Request *>(data);
    State *s = LIGHTNING_DENOISE(element)->state;
    if (!s || request->generation != s->generation.load())
        return; // superseded before it started
    auto *h = new Handoff;
    h->requested = request->mode;
    h->generation = request->generation;
    h->suppressor = buildSuppressor(request->mode);
    h->ok = h->suppressor != nullptr;
    std::lock_guard<std::mutex> lock(s->controlMutex);
    publishLocked(element, s, h);
}

void requestMode(GstElement *element, Mode mode)
{
    State *s = LIGHTNING_DENOISE(element)->state;
    std::lock_guard<std::mutex> lock(s->controlMutex);
    // The same mode again is nothing to do, unless it FAILED: then a
    // re-select is how the user retries it (a backend that failed to build,
    // or one that failed mid-call and was retired).
    if (s->requested.exchange(int(mode)) == int(mode) && s->ok.load())
        return;
    const unsigned long long generation = ++s->generation;
    if (mode == Mode::Off || mode == Mode::WebRtc) {
        // Nothing to build: pass-through. WebRTC suppression is webrtcdsp's.
        auto *h = new Handoff;
        h->requested = mode;
        h->generation = generation;
        h->ok = true;
        publishLocked(element, s, h);
        return;
    }
    // Model setup may take a while (DeepFilterNet unpacks its model): never
    // on the caller's thread, which is the GUI thread in a call.
    gst_element_call_async(element, buildTask,
                           new Request{mode, generation}, freeRequest);
}

// ── Streaming thread ──────────────────────────────────────────────────────

void clearDelayLine(State *s)
{
    s->position = 0;
    // What the next frame's worth of output reads until a real processed
    // frame replaces it: silence, never stale audio from before a gap.
    std::fill(s->draining, s->draining + kFrameSamples, 0.0f);
}

/// Every backend receives finite samples in a sane range, whatever the
/// capture produced.
void sanitise(float *frame)
{
    for (int i = 0; i < kFrameSamples; ++i) {
        const float v = frame[i];
        frame[i] = std::isfinite(v) ? std::clamp(v, -4.0f, 4.0f) : 0.0f;
    }
}

void postStatus(GstElement *element, Mode requested, Mode active, bool ok,
                int latency)
{
    // Once per mode change, never per frame.
    GstStructure *fields = gst_structure_new(
        kStatusMessageName, "requested", G_TYPE_STRING, modeKey(requested),
        "active", G_TYPE_STRING, modeKey(active), "ok", G_TYPE_BOOLEAN,
        ok ? TRUE : FALSE, "latency-samples", G_TYPE_INT, latency, nullptr);
    gst_element_post_message(
        element, gst_message_new_element(GST_OBJECT(element), fields));
}

void adopt(GstElement *element, State *s, Handoff *h)
{
    const bool wasActive = s->active != nullptr;
    // h now carries the previous backend, to be destroyed elsewhere.
    std::swap(s->active, h->suppressor);
    const bool isActive = s->active != nullptr;
    // Backend to backend keeps the framing (both run 480-sample frames), so
    // the swap lands exactly between two frames. Into or out of
    // pass-through, the delay line starts or ends: reset it.
    if (wasActive != isActive)
        clearDelayLine(s);
    const Mode active = isActive ? s->active->mode() : Mode::Off;
    const int latency =
        isActive ? kFrameSamples + s->active->latencySamples() : 0;
    const int previousLatency = s->latency.exchange(latency);
    s->activeMode.store(int(active));
    s->ok.store(h->ok);
    postStatus(element, h->requested, active, h->ok, latency);
    if (previousLatency != latency) {
        gst_element_post_message(element,
                                 gst_message_new_latency(GST_OBJECT(element)));
    }
    retire(element, h);
}

GstFlowReturn transformIp(GstBaseTransform *trans, GstBuffer *buffer)
{
    State *s = LIGHTNING_DENOISE(trans)->state;
    // A relaxed peek first: the exchange is only paid on a real change.
    if (s->next.load(std::memory_order_relaxed)) {
        if (Handoff *h = s->next.exchange(nullptr, std::memory_order_acq_rel))
            adopt(GST_ELEMENT(trans), s, h);
    }
    if (!s->active)
        return GST_FLOW_OK; // pass-through: not one byte touched

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE))
        return GST_FLOW_OK;
    // A gap (the valve reopening after mute): what is in the delay line
    // belongs before it, and so does the backend's own history, which would
    // otherwise replay the moment of muting after unmute (measured live,
    // 2026-10-06: 60-80 ms of pre-mute speech with RNNoise and
    // DeepFilterNet). Only a backend whose reset is safe here gets one.
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
        clearDelayLine(s);
        if (s->active->resetIsRealtimeSafe())
            s->active->reset();
    }
    // A trailing partial sample (a malformed buffer) is left as it is.
    const gsize samples = map.size / sizeof(float);
    guint8 *bytes = map.data;
    gsize done = 0;
    while (done < samples) {
        const gsize room = gsize(kFrameSamples - s->position);
        const gsize take = std::min(room, samples - done);
        // memcpy, not float access: nothing guarantees the buffer's
        // alignment. Input in first, then the delayed output over it.
        std::memcpy(s->filling + s->position, bytes + done * sizeof(float),
                    take * sizeof(float));
        std::memcpy(bytes + done * sizeof(float), s->draining + s->position,
                    take * sizeof(float));
        s->position += int(take);
        done += take;
        if (s->position == kFrameSamples) {
            sanitise(s->filling);
            s->active->process(s->filling);
            std::swap(s->filling, s->draining);
            s->position = 0;
            s->frames.fetch_add(1, std::memory_order_relaxed);
            if (s->active->failed())
                break;
        }
    }
    // The backend stopped processing for good (a caught panic): it would
    // pass audio through while this element still reported it running.
    // Retire it and report the mode as failed, so the engine falls back to
    // WebRTC suppression and says so. Once per failure: the backend is gone
    // after this, and the rest of this buffer passes through.
    if (s->active->failed()) {
        if (auto *h = new (std::nothrow) Handoff) {
            h->requested = Mode(s->requested.load());
            h->ok = false;
            h->generation = s->generation.load();
            adopt(GST_ELEMENT(trans), s, h);
        }
    }
    gst_buffer_unmap(buffer, &map);
    // The output now carries delayed audio, whatever this buffer held.
    GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_GAP);
    return GST_FLOW_OK;
}

void resetStreamingState(State *s)
{
    clearDelayLine(s);
    if (s->active)
        s->active->reset();
}

gboolean stop(GstBaseTransform *trans)
{
    // Streaming has stopped: a restarted capture starts clean.
    resetStreamingState(LIGHTNING_DENOISE(trans)->state);
    return TRUE;
}

gboolean sinkEvent(GstBaseTransform *trans, GstEvent *event)
{
    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP)
        resetStreamingState(LIGHTNING_DENOISE(trans)->state);
    return GST_BASE_TRANSFORM_CLASS(lightning_denoise_parent_class)
        ->sink_event(trans, event);
}

gboolean query(GstBaseTransform *trans, GstPadDirection direction,
               GstQuery *q)
{
    const gboolean answered =
        GST_BASE_TRANSFORM_CLASS(lightning_denoise_parent_class)
            ->query(trans, direction, q);
    if (answered && direction == GST_PAD_SRC
        && GST_QUERY_TYPE(q) == GST_QUERY_LATENCY) {
        const int samples = LIGHTNING_DENOISE(trans)->state->latency.load();
        if (samples > 0) {
            gboolean live = FALSE;
            GstClockTime minimum = 0;
            GstClockTime maximum = GST_CLOCK_TIME_NONE;
            gst_query_parse_latency(q, &live, &minimum, &maximum);
            const GstClockTime ours = gst_util_uint64_scale_int(
                guint64(samples), GST_SECOND, kSampleRate);
            minimum += ours;
            if (GST_CLOCK_TIME_IS_VALID(maximum))
                maximum += ours;
            gst_query_set_latency(q, live, minimum, maximum);
        }
    }
    return answered;
}

void setProperty(GObject *object, guint id, const GValue *value,
                 GParamSpec *spec)
{
    switch (id) {
    case PROP_MODE:
        requestMode(GST_ELEMENT(object), Mode(g_value_get_enum(value)));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        break;
    }
}

void getProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    const State *s = LIGHTNING_DENOISE(object)->state;
    switch (id) {
    case PROP_MODE:
        g_value_set_enum(value, s->requested.load());
        break;
    case PROP_ACTIVE_MODE:
        g_value_set_enum(value, s->activeMode.load());
        break;
    case PROP_BACKEND_OK:
        g_value_set_boolean(value, s->ok.load() ? TRUE : FALSE);
        break;
    case PROP_FRAMES:
        g_value_set_uint64(value, s->frames.load());
        break;
    case PROP_LATENCY:
        g_value_set_int(value, s->latency.load());
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        break;
    }
}

void finalize(GObject *object)
{
    auto *self = LIGHTNING_DENOISE(object);
    // No streaming and no pending pool task (each holds a reference), so the
    // backends are destroyed right here.
    const bool hadBackend = self->state
        && (self->state->active
            || (self->state->next.load() && self->state->next.load()->suppressor));
    delete self->state;
    self->state = nullptr;
    --g_liveElements;
    // The end of a call that ran a neural backend. Finalize may run on the
    // GUI thread, so the trim gets a short-lived thread of its own.
    if (hadBackend)
        std::thread(releaseFreedBackendMemory).detach();
    G_OBJECT_CLASS(lightning_denoise_parent_class)->finalize(object);
}

void lightning_denoise_class_init(LightningDenoiseClass *klass)
{
    auto *gobject = G_OBJECT_CLASS(klass);
    auto *element = GST_ELEMENT_CLASS(klass);
    auto *base = GST_BASE_TRANSFORM_CLASS(klass);

    gobject->set_property = setProperty;
    gobject->get_property = getProperty;
    gobject->finalize = finalize;

    const auto readable =
        GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(
        gobject, PROP_MODE,
        g_param_spec_enum(
            "mode", "Mode",
            "Noise suppressor to run; off and webrtc pass through",
            modeEnumType(), int(Mode::Off),
            GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                        | GST_PARAM_MUTABLE_PLAYING)));
    g_object_class_install_property(
        gobject, PROP_ACTIVE_MODE,
        g_param_spec_enum("active-mode", "Active mode",
                          "The backend actually running (off: none)",
                          modeEnumType(), int(Mode::Off), readable));
    g_object_class_install_property(
        gobject, PROP_BACKEND_OK,
        g_param_spec_boolean("backend-ok", "Backend OK",
                             "The requested mode is in effect", TRUE,
                             readable));
    g_object_class_install_property(
        gobject, PROP_FRAMES,
        g_param_spec_uint64("frames", "Frames",
                            "Frames the backend has processed", 0,
                            G_MAXUINT64, 0, readable));
    g_object_class_install_property(
        gobject, PROP_LATENCY,
        g_param_spec_int("latency-samples", "Latency",
                         "Delay this element adds, in samples at 48 kHz", 0,
                         G_MAXINT, 0, readable));

    gst_element_class_add_static_pad_template(element, &kSinkTemplate);
    gst_element_class_add_static_pad_template(element, &kSrcTemplate);
    gst_element_class_set_static_metadata(
        element, "Lightning microphone noise suppressor",
        "Filter/Effect/Audio",
        "Runs RNNoise or DeepFilterNet on the outgoing microphone, in place",
        "Lightning");

    base->transform_ip = transformIp;
    base->stop = stop;
    base->sink_event = sinkEvent;
    base->query = query;
}

void lightning_denoise_init(LightningDenoise *self)
{
    self->state = new State;
    ++g_liveElements;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
}

} // namespace

void registerDenoiseElement()
{
    static std::once_flag once;
    std::call_once(once, [] {
        gst_element_register(nullptr, denoiseElementName(), GST_RANK_NONE,
                             LIGHTNING_TYPE_DENOISE);
    });
}

const char *denoiseElementName() { return "lightningdenoise"; }

const char *denoiseCaps() { return kParseCaps; }

int liveDenoiseElements() { return g_liveElements.load(); }

void setDenoiseMode(GstElement *element, Mode mode)
{
    if (!element)
        return;
    g_object_set(element, "mode", int(mode), nullptr);
}

DenoiseStatus denoiseStatus(GstElement *element)
{
    DenoiseStatus status;
    if (!element)
        return status;
    gint requested = 0;
    gint active = 0;
    gboolean ok = TRUE;
    guint64 frames = 0;
    gint latency = 0;
    g_object_get(element, "mode", &requested, "active-mode", &active,
                 "backend-ok", &ok, "frames", &frames, "latency-samples",
                 &latency, nullptr);
    status.requested = Mode(requested);
    status.active = Mode(active);
    status.ok = ok;
    status.frames = frames;
    status.latencySamples = latency;
    return status;
}

void setSuppressorFactoryForTest(SuppressorFactory factory)
{
    std::lock_guard<std::mutex> lock(g_factoryMutex);
    g_factory = std::move(factory);
}

} // namespace calls::noise
