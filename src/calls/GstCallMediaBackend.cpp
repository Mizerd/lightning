#include "GstCallMediaBackend.h"

#include "calls/CaptureClock.h"
#include "calls/GstBootstrap.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

#include <mutex>

// Coarse lifecycle/category lines only. Never log SDP, candidates, TURN
// credentials, or GStreamer error details that could embed them.
Q_LOGGING_CATEGORY(lcCallMedia, "matrix.calls.media")

namespace {

// GStreamer calls back on its own threads; marshalled lambdas must never run
// against a destroyed backend. The queued invocation uses the backend as
// receiver, so anything in flight when it dies is dropped by Qt.
QMutex g_aliveMutex;
QSet<GstCallMediaBackend *> g_aliveBackends;

template <typename Fn>
void marshal(GstCallMediaBackend *backend, Fn &&fn)
{
    QMutexLocker lock(&g_aliveMutex);
    if (!g_aliveBackends.contains(backend))
        return;
    QMetaObject::invokeMethod(backend, std::forward<Fn>(fn),
                              Qt::QueuedConnection);
}

// Context for promise callbacks: keeps the webrtcbin alive until the promise
// settles and records which call the description belongs to.
struct PromiseCtx {
    GstCallMediaBackend *backend = nullptr;
    GstElement *webrtc = nullptr; // owns one ref
    QString callId;
};

PromiseCtx *promiseCtxNew(GstCallMediaBackend *backend, GstElement *webrtc,
                          const QString &callId)
{
    auto *ctx = new PromiseCtx;
    ctx->backend = backend;
    ctx->webrtc = GST_ELEMENT(gst_object_ref(webrtc));
    ctx->callId = callId;
    return ctx;
}

void promiseCtxFree(gpointer data)
{
    auto *ctx = static_cast<PromiseCtx *>(data);
    if (ctx->webrtc)
        gst_object_unref(ctx->webrtc);
    delete ctx;
}

// Applies the "offer"/"answer" description from a settled promise as the
// local description and returns its SDP, or empty on any failure.
QString applyCreatedDescription(GstPromise *promise, GstElement *webrtc,
                                const char *field)
{
    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED)
        return {};
    const GstStructure *reply = gst_promise_get_reply(promise);
    if (!reply)
        return {};
    GstWebRTCSessionDescription *description = nullptr;
    gst_structure_get(reply, field, GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &description, nullptr);
    if (!description)
        return {};
    g_signal_emit_by_name(webrtc, "set-local-description", description,
                          nullptr);
    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);
    return sdp;
}

constexpr int kDefaultOpusPayloadType = 111;

// The offerer's dynamic Opus payload type from its rtpmap; an answer must
// reuse it (RFC 3264).
int opusPayloadTypeFromSdp(const QString &sdp)
{
    static const QRegularExpression rtpmap(
        QStringLiteral("a=rtpmap:(\\d{1,3})\\s+opus/48000"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rtpmap.match(sdp);
    if (!match.hasMatch())
        return kDefaultOpusPayloadType;
    bool ok = false;
    const int payload = match.captured(1).toInt(&ok);
    return ok && payload >= 96 && payload <= 127 ? payload
                                                 : kDefaultOpusPayloadType;
}

// One media section's shape: kind, direction and whether it is rejected.
QString sectionShape(const GstSDPMedia *media)
{
    const char *kind = gst_sdp_media_get_media(media);
    QString out = QString::fromUtf8(kind ? kind : "?").left(16);
    static const char *const kDirections[] = {"sendrecv", "sendonly",
                                              "recvonly", "inactive"};
    const char *direction = "sendrecv"; // the SDP default
    for (const char *candidate : kDirections) {
        if (gst_sdp_media_get_attribute_val(media, candidate)) {
            direction = candidate;
            break;
        }
    }
    out += QLatin1Char(':') + QString::fromLatin1(direction);
    if (gst_sdp_media_get_port(media) == 0)
        out += QStringLiteral(":port0");
    return out;
}

// The offer's media kind at `mline`, or empty when there is no such section.
QString offeredKind(const GstSDPMessage *sdp, guint mline)
{
    if (!sdp || mline >= gst_sdp_message_medias_len(sdp))
        return {};
    const char *kind =
        gst_sdp_media_get_media(gst_sdp_message_get_media(sdp, mline));
    return QString::fromUtf8(kind ? kind : "");
}

// Sets every transceiver whose offered section is not audio to inactive,
// before the answer is created, so the answer declines it instead of
// accepting a track this engine cannot play. Keyed on the offer's section:
// webrtcbin reports our own audio transceiver's kind as unknown at this
// point. Returns how many were declined.
int declineUnhandledSections(GstElement *webrtc)
{
    GstWebRTCSessionDescription *remote = nullptr;
    g_object_get(webrtc, "remote-description", &remote, nullptr);
    if (!remote)
        return 0;
    int declined = 0;
    // Bounded: an offer names a handful of sections.
    for (guint i = 0; i < 32; ++i) {
        GstWebRTCRTPTransceiver *transceiver = nullptr;
        g_signal_emit_by_name(webrtc, "get-transceiver", static_cast<gint>(i),
                              &transceiver);
        if (!transceiver)
            break;
        guint mline = G_MAXUINT;
        g_object_get(transceiver, "mlineindex", &mline, nullptr);
        if (offeredKind(remote->sdp, mline) != QLatin1String("audio")) {
            g_object_set(transceiver, "direction",
                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE,
                         nullptr);
            ++declined;
        }
        gst_object_unref(transceiver);
    }
    gst_webrtc_session_description_free(remote);
    return declined;
}

// Whether a receive pad carries Opus audio, the only thing the receive chain
// decodes. No caps at all fails closed: such a pad is drained, since linking
// an unknown track into the Opus chain is what stopped the transport. Absent
// FIELDS are tolerated (a pad's caps may name only application/x-rtp), a
// field naming something else is not.
bool padCarriesOpusAudio(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);
    if (!caps)
        return false;
    bool opus = false;
    if (!gst_caps_is_empty(caps) && !gst_caps_is_any(caps)
        && gst_caps_get_size(caps) > 0) {
        opus = true;
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *media = gst_structure_get_string(structure, "media");
        const gchar *encoding =
            gst_structure_get_string(structure, "encoding-name");
        if (media && g_ascii_strcasecmp(media, "audio") != 0)
            opus = false;
        if (encoding && g_ascii_strcasecmp(encoding, "OPUS") != 0)
            opus = false;
    }
    gst_caps_unref(caps);
    return opus;
}

GstPadProbeReturn countPacket(GstPad *, GstPadProbeInfo *, gpointer counter)
{
    static_cast<std::atomic<int> *>(counter)->fetch_add(1);
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn countDecoded(GstPad *, GstPadProbeInfo *, gpointer counter)
{
    static_cast<std::atomic<quint64> *>(counter)->fetch_add(1);
    return GST_PAD_PROBE_OK;
}

// Test-tone mode, the receive chain's output: the loudest sample, in
// thousandths of full scale. Reads only the two formats the chain produces
// (opusdec's S16LE, or F32LE where a sink asks for it).
GstPadProbeReturn recordPeak(GstPad *pad, GstPadProbeInfo *info,
                             gpointer peakMilli)
{
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    const gchar *format = nullptr;
    if (caps && gst_caps_get_size(caps) > 0)
        format = gst_structure_get_string(gst_caps_get_structure(caps, 0),
                                          "format");
    const bool s16 = g_strcmp0(format, "S16LE") == 0;
    const bool f32 = g_strcmp0(format, "F32LE") == 0;
    if (caps)
        gst_caps_unref(caps);
    if (!s16 && !f32)
        return GST_PAD_PROBE_OK;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;
    double peak = 0;
    if (s16) {
        const auto *samples = reinterpret_cast<const qint16 *>(map.data);
        for (gsize i = 0; i < map.size / 2; ++i)
            peak = qMax(peak, qAbs(double(samples[i])) / 32768.0);
    } else {
        const auto *samples = reinterpret_cast<const float *>(map.data);
        for (gsize i = 0; i < map.size / 4; ++i)
            peak = qMax(peak, qAbs(double(samples[i])));
    }
    gst_buffer_unmap(buffer, &map);
    auto *target = static_cast<std::atomic<int> *>(peakMilli);
    const int milli = int(qMin(peak, 1.0) * 1000.0);
    int seen = target->load();
    while (milli > seen && !target->compare_exchange_weak(seen, milli)) {
    }
    return GST_PAD_PROBE_OK;
}

// Sets a resolved device binding on the element called `name` inside `bin`.
// Never interpolated into a description: a quote in a device name would be
// parsed as syntax. Converted to the property's own type (osxaudiosrc's
// `device` is an int). False when nothing was set; the caller then keeps the
// element's own default device. Same rule as SfuMediaEngine's.
bool applyBinding(GstElement *bin, const char *name,
                  const lightning::calls::DeviceBinding &binding)
{
    if (binding.isEmpty())
        return true;
    GstElement *element = gst_bin_get_by_name(GST_BIN(bin), name);
    if (!element)
        return false;
    const QByteArray property = binding.property.toUtf8();
    const QByteArray value = binding.value.toUtf8();
    GParamSpec *spec = g_object_class_find_property(
        G_OBJECT_GET_CLASS(element), property.constData());
    bool set = false;
    if (spec && (spec->flags & G_PARAM_WRITABLE)
        && !(spec->flags & G_PARAM_CONSTRUCT_ONLY)) {
        if (spec->value_type == G_TYPE_STRING) {
            g_object_set(element, property.constData(), value.constData(),
                         nullptr);
            set = true;
        } else {
            GValue typed = G_VALUE_INIT;
            g_value_init(&typed, spec->value_type);
            // g_param_value_validate() is true when it had to change the
            // value, i.e. it was out of range.
            set = gst_value_deserialize(&typed, value.constData())
                && !g_param_value_validate(spec, &typed);
            if (set)
                g_object_set_property(G_OBJECT(element), property.constData(),
                                      &typed);
            g_value_unset(&typed);
        }
    }
    // The property and how it matched, never the value (a device name).
    if (set) {
        qCInfo(lcCallMedia) << "audio device bound element=" << name
                            << "property=" << binding.property
                            << "matched-by=" << binding.reason;
    } else {
        qCWarning(lcCallMedia) << "audio element" << name << "did not accept"
                               << binding.property
                               << "- using its default device";
    }
    gst_object_unref(element);
    return set;
}

// A receive track's sink being replaced (a speaker switch), handed to an IDLE
// probe on the track's `outvol` src pad: the swap runs when no buffer is in
// flight there, so the decode chain never sees an unlinked pad (a not-linked
// return posts an error, and an error ends the call).
struct SinkSwap {
    GstElement *bin = nullptr; // the receive bin; owns one ref
    QByteArray description;
    lightning::calls::DeviceBinding binding;
    // The backend's test counter, shared: the probe can outlive a call.
    std::shared_ptr<std::atomic<int>> swaps;
};

void sinkSwapFree(gpointer data)
{
    auto *swap = static_cast<SinkSwap *>(data);
    if (swap->bin)
        gst_object_unref(swap->bin);
    delete swap;
}

GstPadProbeReturn swapSinkWhenIdle(GstPad *pad, GstPadProbeInfo *,
                                   gpointer data)
{
    auto *swap = static_cast<SinkSwap *>(data);
    GError *error = nullptr;
    GstElement *next = gst_parse_bin_from_description(
        swap->description.constData(), TRUE, &error);
    if (error) {
        g_error_free(error);
        if (next)
            gst_object_unref(next);
        qCWarning(lcCallMedia) << "speaker switch: the new output could not "
                                  "be built; keeping the current one";
        return GST_PAD_PROBE_REMOVE;
    }
    if (!next)
        return GST_PAD_PROBE_REMOVE;
    applyBinding(next, "outsink", swap->binding);
    if (GstPad *peer = gst_pad_get_peer(pad)) {
        GstElement *old = gst_pad_get_parent_element(peer);
        gst_pad_unlink(pad, peer);
        gst_object_unref(peer);
        if (old) {
            gst_element_set_state(old, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(swap->bin), old);
            gst_object_unref(old);
        }
    }
    if (!gst_bin_add(GST_BIN(swap->bin), next))
        return GST_PAD_PROBE_REMOVE; // sunk and dropped by gst_bin_add
    GstPad *sinkPad = gst_element_get_static_pad(next, "sink");
    const bool linked =
        sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(next);
    if (linked && swap->swaps)
        swap->swaps->fetch_add(1);
    if (!linked)
        qCWarning(lcCallMedia) << "speaker switch: the new output did not link";
    return GST_PAD_PROBE_REMOVE;
}

// One get-stats walk, reduced to the numbers the 1:1 stats line prints.
struct StatsTotals {
    quint64 inboundAudio = 0;
    quint64 inboundAudioBytes = 0;
    qint64 inboundLost = 0;
    quint64 outboundAudio = 0;
    quint64 inboundOther = 0;
};

gboolean addStat(GQuark, const GValue *value, gpointer data)
{
    auto *totals = static_cast<StatsTotals *>(data);
    if (!GST_VALUE_HOLDS_STRUCTURE(value))
        return TRUE;
    const GstStructure *s = gst_value_get_structure(value);
    GstWebRTCStatsType type = GST_WEBRTC_STATS_CODEC;
    if (!s
        || !gst_structure_get(s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type,
                              nullptr))
        return TRUE;
    const gchar *kind = gst_structure_has_field(s, "kind")
        ? gst_structure_get_string(s, "kind")
        : nullptr;
    const bool video = g_strcmp0(kind, "video") == 0;
    guint64 packets = 0;
    if (type == GST_WEBRTC_STATS_INBOUND_RTP) {
        if (gst_structure_has_field(s, "packets-received"))
            gst_structure_get_uint64(s, "packets-received", &packets);
        if (video) {
            totals->inboundOther += packets;
            return TRUE;
        }
        totals->inboundAudio += packets;
        guint64 bytes = 0;
        if (gst_structure_has_field(s, "bytes-received")
            && gst_structure_get_uint64(s, "bytes-received", &bytes))
            totals->inboundAudioBytes += bytes;
        gint64 lost = 0;
        if (gst_structure_has_field(s, "packets-lost")
            && gst_structure_get_int64(s, "packets-lost", &lost))
            totals->inboundLost += lost;
    } else if (type == GST_WEBRTC_STATS_OUTBOUND_RTP && !video) {
        if (gst_structure_has_field(s, "packets-sent")
            && gst_structure_get_uint64(s, "packets-sent", &packets))
            totals->outboundAudio += packets;
    }
    return TRUE;
}

struct BusCtx {
    GstCallMediaBackend *backend = nullptr;
    quintptr pipelineToken = 0;
};

void busCtxFree(gpointer data)
{
    delete static_cast<BusCtx *>(data);
}

GstBusSyncReply busSyncHandler(GstBus *bus, GstMessage *message,
                               gpointer userData)
{
    Q_UNUSED(bus);
    auto *ctx = static_cast<BusCtx *>(userData);
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        // Error details can embed device or address strings: category only.
        // Invoked by name under the alive lock; the queued call dies with the
        // receiver.
        QMutexLocker lock(&g_aliveMutex);
        if (g_aliveBackends.contains(ctx->backend)) {
            QMetaObject::invokeMethod(
                ctx->backend, "handleFailure", Qt::QueuedConnection,
                Q_ARG(quintptr, ctx->pipelineToken),
                Q_ARG(QString, QStringLiteral("media_pipeline")));
        }
    }
    // Drop after inspection: nothing drains this bus, so passing messages
    // would grow the queue for the whole call.
    gst_message_unref(message);
    return GST_BUS_DROP;
}

} // namespace

bool GstCallMediaBackend::runtimeAvailable(QString *whyNot)
{
    // One process-wide init with the plugin path applied (GstBootstrap.h).
    // This backend is probed before the SFU engine, so a bare gst_init here
    // left packaged builds with an empty registry.
    const bool initOk = lightning::gst::ensureInitialised(whyNot);
    if (!initOk) {
        if (whyNot)
            *whyNot = QStringLiteral("gstreamer_init_failed");
        return false;
    }
    // Everything the pipeline needs, including webrtcbin's own runtime
    // elements (nice transport, DTLS-SRTP).
    static const char *const kRequired[] = {
        "webrtcbin",    "nicesrc",      "nicesink",     "dtlssrtpenc",
        "dtlssrtpdec",  "opusenc",      "opusdec",      "rtpopuspay",
        "rtpopusdepay", "audioconvert", "audioresample", "audiotestsrc",
        "fakesink",     "autoaudiosrc", "autoaudiosink", "queue",
        // Mute uses a real valve (send) and volume (receive), so both must
        // resolve for mute support.
        "valve",        "volume",
        "capsfilter",
        // Created by webrtcbin and dtlssrtpenc themselves, so nothing above
        // names them: the RTP session and bundling (rtpmanager) and SRTP
        // (needs libsrtp2). A snap without libsrtp2 passed every other entry
        // and carried no media (2026-09-29).
        "rtpbin",       "rtpfunnel",    "srtpenc",      "srtpdec",
    };
    for (const char *name : kRequired) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (!factory) {
            if (whyNot)
                *whyNot = QStringLiteral("missing_element_")
                    + QString::fromLatin1(name);
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

QString GstCallMediaBackend::sdpSectionShape(const QString &sdp)
{
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(sdp.toUtf8().constData(), &message)
        != GST_SDP_OK) {
        if (message)
            gst_sdp_message_free(message);
        return QStringLiteral("unparsable");
    }
    QStringList sections;
    const guint count = qMin<guint>(gst_sdp_message_medias_len(message), 16);
    for (guint i = 0; i < count; ++i)
        sections.append(sectionShape(gst_sdp_message_get_media(message, i)));
    gst_sdp_message_free(message);
    return sections.isEmpty() ? QStringLiteral("none")
                              : sections.join(QLatin1Char(' '));
}

int GstCallMediaBackend::offerPromiseErrorReplyContextRefsForTest()
{
    if (!lightning::gst::ensureInitialised())
        return -1;
    GstElement *webrtc = gst_element_factory_make("webrtcbin", nullptr);
    if (!webrtc)
        return -1;
    // Floating from the factory (one ref); promiseCtxNew takes a second. A
    // null backend makes marshal() drop the hand-off, so only the promise
    // lifetime is exercised.
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated,
        promiseCtxNew(nullptr, webrtc,
                      QStringLiteral("promise-ctx-lifetime-probe")),
        promiseCtxFree);
    // webrtcbin's error reply: an "error" field and no "offer".
    // gst_promise_reply calls the change function inline.
    GError *error = g_error_new(g_quark_from_static_string("lightning-test"),
                                0, "no description");
    gst_promise_reply(promise,
                      gst_structure_new("application/x-gst-promise-error",
                                        "error", G_TYPE_ERROR, error,
                                        nullptr));
    g_clear_error(&error);
    // Not unreffed here: the change function consumed the last reference, as
    // in production.
    const int refs = static_cast<int>(GST_OBJECT_REFCOUNT_VALUE(webrtc));
    gst_object_unref(webrtc);
    return refs;
}

GstCallMediaBackend::GstCallMediaBackend(QObject *parent)
    : CallMediaBackend(parent)
{
    QMutexLocker lock(&g_aliveMutex);
    g_aliveBackends.insert(this);
}

GstCallMediaBackend::~GstCallMediaBackend()
{
    {
        // Unregister first so no new marshalled lambda targets us.
        QMutexLocker lock(&g_aliveMutex);
        g_aliveBackends.remove(this);
    }
    if (m_sessionActive)
        destroySessionLocked();
    // Bounded: at quit a webrtcbin still gathering is stopped anyway.
    m_retirer.drain(lightning::webrtc::Retirer::kQuitBudgetMs);
}

bool GstCallMediaBackend::startSession(const QString &callId, bool offerer,
                                       int opusPayloadType)
{
    if (m_sessionActive) {
        // One call at a time; an existing session here is a controller bug, so
        // refuse rather than leak the old pipeline.
        qCWarning(lcCallMedia) << "session already active; refusing new call";
        return false;
    }
    // The devices this call opens: asked now, so a choice made since the
    // last call applies (and a live call follows later ones; see
    // audioDevicesChanged()).
    const AudioDevicePlan plan = resolveDevices();
    {
        QMutexLocker lock(&m_planMutex);
        m_plan = plan;
    }
    // As offerer we use 111 (the common convention); as answerer RFC 3264
    // requires the offerer's number, which createAnswer() extracts.
    const int payload = qBound(96, opusPayloadType, 127);
    // Our SSRC, fixed before any description exists and named in the caps
    // webrtcbin reads. webrtcbin writes a=ssrc into an ANSWER only from caps
    // already on its sink pad, and a real capture delivers its first buffer
    // ~0.7 s after the session starts, so every answer went out without one.
    // A browser answering a BUNDLE of audio plus a declined video section
    // then cannot map our unsignalled SSRC to its audio receiver and drops
    // every packet: Element's legacy VIDEO call heard nothing from Lightning
    // (2026-10-08; Chromium's webrtc-internals: 2620 packets in on the
    // transport, no inbound-rtp at all). With a single m-line the browser
    // falls back to its only receiver, which is why voice calls worked.
    const quint32 ssrc = QRandomGenerator::global()->bounded(1u, 0xFFFFFFFFu);
    // The capture is a separate bin (buildMicrophoneFront()) linked in front
    // of the valve, so a device switch replaces it without touching the rest.
    const QString description = QStringLiteral(
        "webrtcbin name=wb bundle-policy=max-bundle latency=100 "
        // valve name=micvalve: drop=true stops buffers before the encoder, so
        // nothing is published while muted (lowering volume would still send).
        "valve name=micvalve drop=false "
        // Every capture is converted to exactly this before the encoder, so a
        // device switch mid-call never changes the encoder's or the track's
        // caps (a caps change on webrtcbin's pad can ask for renegotiation).
        "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=1 "
        "! opusenc "
        "! rtpopuspay name=micpay pt=%1 ssrc=%2 "
        "! application/x-rtp,media=audio,encoding-name=OPUS,payload=%1,"
        "ssrc=(uint)%2 "
        "! wb. ").arg(payload).arg(ssrc);
    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &error);
    if (error) {
        // The GError message can embed the parse text: category only.
        qCWarning(lcCallMedia) << "pipeline construction failed";
        g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }
    GstElement *webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "wb");
    if (!webrtc) {
        gst_object_unref(pipeline);
        return false;
    }
    // The pipeline runs on the system clock, never on the capture's. An audio
    // source provides a clock and the pipeline prefers it; a device switch
    // then removes the clock with the source, and a pipeline whose clock is
    // gone waits on it for ever: webrtcbin's clocksync held every packet and
    // the swap's own state change deadlocked behind it (measured live,
    // 2026-10-08). Nothing here could recover: this bus drops CLOCK_LOST.
    if (GstClock *clock = gst_system_clock_obtain()) {
        gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
        gst_object_unref(clock);
    }
    GstElement *front = buildMicrophoneFront(plan);
    if (!front && !plan.microphoneFront.isEmpty()) {
        // The chosen device's capture would not even build: the default
        // beats no call.
        qCWarning(lcCallMedia)
            << "the chosen microphone's capture could not be built; using "
               "the system default";
        front = buildMicrophoneFront(AudioDevicePlan{});
    }
    GstElement *valveForFront =
        gst_bin_get_by_name(GST_BIN(pipeline), "micvalve");
    bool frontLinked = false;
    if (front && valveForFront && gst_bin_add(GST_BIN(pipeline), front)) {
        frontLinked = gst_element_link(front, valveForFront);
    } else if (front) {
        gst_object_unref(front); // never added
        front = nullptr;
    }
    if (valveForFront)
        gst_object_unref(valveForFront);
    if (!frontLinked) {
        qCWarning(lcCallMedia) << "pipeline construction failed (capture)";
        gst_object_unref(webrtc);
        gst_object_unref(pipeline);
        return false;
    }

    m_session = Session();
    m_receivedAudioPackets.store(0);
    m_drainedPads.store(0);
    m_session.callId = callId;
    m_session.pipeline = pipeline;
    m_session.webrtc = webrtc;
    m_session.offerer = offerer;
    m_session.micFront = front; // borrowed: the pipeline owns it
    // Borrowed: the pipeline owns the valve and outlives the session struct.
    if (GstElement *valve = gst_bin_get_by_name(GST_BIN(pipeline),
                                                "micvalve")) {
        m_session.micValve = valve;
        gst_object_unref(valve);
    }
    // webrtcbin syncs its inputs to the clock, so a microphone whose
    // timestamps run ahead would starve the leaky queue; see CaptureClock.h.
    // Seen at the capture, corrected on the RTP, so the RTP timestamps stay
    // sample-accurate.
    {
        GstElement *micsrc = gst_bin_get_by_name(GST_BIN(pipeline), "micsrc");
        GstElement *micpay = gst_bin_get_by_name(GST_BIN(pipeline), "micpay");
        GstPad *captured =
            micsrc ? gst_element_get_static_pad(micsrc, "src") : nullptr;
        GstPad *rtp = micpay ? gst_element_get_static_pad(micpay, "src") : nullptr;
        if (captured && rtp) {
            auto hold = std::make_shared<lightning::calls::CaptureClockHold>();
            hold->report = [](quint64 count, qint64 leadMs) {
                qCWarning(lcCallMedia)
                    << "microphone timestamps ran" << leadMs
                    << "ms ahead of the pipeline clock; its packets are "
                       "released on time (packets held so far:"
                    << count << ")";
            };
            lightning::calls::holdCaptureToClock(captured, rtp, hold);
            // A replacement capture re-attaches to the same hold.
            m_session.clockHold = hold;
        }
        if (captured)
            gst_object_unref(captured);
        if (rtp)
            gst_object_unref(rtp);
        if (micsrc)
            gst_object_unref(micsrc);
        if (micpay)
            gst_object_unref(micpay);
    }
    m_sessionActive = true;
    // Before any callback can run; see destroySessionLocked().
    lightning::webrtc::installGate(webrtc);

    applyIceConfigLocked();

    if (offerer) {
        // Only the offerer handles negotiation-needed; the answerer is driven
        // by createAnswer().
        g_signal_connect(webrtc, "on-negotiation-needed",
                         G_CALLBACK(onNegotiationNeeded), this);
    }
    g_signal_connect(webrtc, "on-ice-candidate",
                     G_CALLBACK(onIceCandidateGst), this);
    g_signal_connect(webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringNotify), this);
    g_signal_connect(webrtc, "notify::connection-state",
                     G_CALLBACK(onConnectionNotify), this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(onPadAdded), this);

    GstBus *bus = gst_element_get_bus(pipeline);
    auto *busCtx = new BusCtx;
    busCtx->backend = this;
    busCtx->pipelineToken = reinterpret_cast<quintptr>(pipeline);
    gst_bus_set_sync_handler(bus, busSyncHandler, busCtx, busCtxFree);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        qCWarning(lcCallMedia) << "pipeline refused to start";
        destroySessionLocked();
        return false;
    }
    qCInfo(lcCallMedia) << "media session started offerer=" << offerer
                        << "testTone=" << m_testTone
                        << "chosenMicrophone=" << !plan.microphoneFront.isEmpty()
                        << "chosenSpeaker=" << !plan.speakerSink.isEmpty();
    m_decodedBuffers.store(0);
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        m_statsTimer->setInterval(5000);
        connect(m_statsTimer, &QTimer::timeout, this,
                &GstCallMediaBackend::requestStats);
    }
    m_statsTimer->start();
    return true;
}

void GstCallMediaBackend::destroySessionLocked()
{
    if (!m_sessionActive)
        return;
    if (m_session.webrtc)
        g_signal_handlers_disconnect_by_data(m_session.webrtc, this);
    if (m_session.pipeline) {
        GstBus *bus = gst_element_get_bus(m_session.pipeline);
        if (bus) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }
    }
    if (m_statsTimer)
        m_statsTimer->stop();
    // Never a plain set_state(NULL) (GitHub #3): the pipeline stops here, its
    // webrtcbin once ICE gathering has ended. Takes both references.
    m_retirer.retire(m_session.pipeline, m_session.webrtc);
    m_session = Session();
    m_sessionActive = false;
    // Engine state is per session. The controller owns the deafen intent and
    // re-applies it; a latched value would silence a later call.
    m_outputMuted.store(false);
    qCInfo(lcCallMedia) << "media session destroyed";
}

void GstCallMediaBackend::applyIceConfigLocked()
{
    if (!m_session.webrtc)
        return;
    // Only servers the homeserver named (see CallMediaBackend.h). The first
    // stun: URI sets stun-server; each turn(s): URI is added with the
    // credentials percent-encoded.
    bool stunApplied = false;
    const QByteArray user =
        QUrl::toPercentEncoding(m_iceUsername);
    const QByteArray password =
        QUrl::toPercentEncoding(m_icePassword);
    // Homeserver-supplied but still remote input assembled into a
    // credential-bearing URI: refuse '@', '/', '\\', whitespace and control
    // characters.
    const auto saneServerUri = [](const QString &uri) {
        if (uri.size() > 512)
            return false;
        for (const QChar ch : uri) {
            if (ch.unicode() < 0x21 || ch == QLatin1Char('@')
                || ch == QLatin1Char('/') || ch == QLatin1Char('\\'))
                return false;
        }
        return true;
    };
    for (const QString &uri : m_iceUris) {
        if (!saneServerUri(uri))
            continue;
        if (uri.startsWith(QLatin1String("stun:"))) {
            if (stunApplied)
                continue;
            const QString value =
                QStringLiteral("stun://") + uri.mid(5);
            g_object_set(m_session.webrtc, "stun-server",
                         value.toUtf8().constData(), nullptr);
            stunApplied = true;
        } else if (uri.startsWith(QLatin1String("turn:"))
                   || uri.startsWith(QLatin1String("turns:"))) {
            const bool secure = uri.startsWith(QLatin1String("turns:"));
            const QString rest = uri.mid(secure ? 6 : 5);
            const QString value = (secure ? QStringLiteral("turns://")
                                          : QStringLiteral("turn://"))
                + QString::fromUtf8(user) + QLatin1Char(':')
                + QString::fromUtf8(password) + QLatin1Char('@') + rest;
            gboolean added = FALSE;
            g_signal_emit_by_name(m_session.webrtc, "add-turn-server",
                                  value.toUtf8().constData(), &added);
            // Never log the URI: it carries credentials.
        }
    }
}

void GstCallMediaBackend::setIceServers(const QStringList &uris,
                                        const QString &username,
                                        const QString &password)
{
    m_iceUris = uris;
    m_iceUsername = username;
    m_icePassword = password;
    // Applied to the next session; a live call keeps its negotiated config.
}

void GstCallMediaBackend::createOffer(const QString &callId)
{
    if (!startSession(callId, /*offerer=*/true, kDefaultOpusPayloadType))
        Q_EMIT failed(callId, QStringLiteral("media_init"));
    // The offer arrives via on-negotiation-needed -> create-offer.
}

void GstCallMediaBackend::createAnswer(const QString &callId,
                                       const QString &remoteOfferSdp)
{
    if (!startSession(callId, /*offerer=*/false,
                      opusPayloadTypeFromSdp(remoteOfferSdp))) {
        Q_EMIT failed(callId, QStringLiteral("media_init"));
        return;
    }
    GstSDPMessage *message = nullptr;
    // GStreamer's SDP parser accepts garbage, so also require a media section.
    if (gst_sdp_message_new_from_text(remoteOfferSdp.toUtf8().constData(),
                                      &message)
            != GST_SDP_OK
        || gst_sdp_message_medias_len(message) == 0) {
        if (message)
            gst_sdp_message_free(message);
        destroySessionLocked();
        Q_EMIT failed(callId, QStringLiteral("bad_remote_offer"));
        return;
    }
    // Shape only (kinds and directions), never the SDP itself.
    qCInfo(lcCallMedia) << "remote offer sections="
                        << sdpSectionShape(remoteOfferSdp);
    GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_OFFER, message); // takes ownership of message
    GstPromise *promise = gst_promise_new_with_change_func(
        onRemoteOfferSet, promiseCtxNew(this, m_session.webrtc, callId),
        promiseCtxFree);
    g_signal_emit_by_name(m_session.webrtc, "set-remote-description", offer,
                          promise);
    gst_webrtc_session_description_free(offer);
}

void GstCallMediaBackend::setRemoteAnswer(const QString &callId,
                                          const QString &remoteAnswerSdp)
{
    if (!m_sessionActive || m_session.callId != callId || !m_session.webrtc)
        return;
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(remoteAnswerSdp.toUtf8().constData(),
                                      &message)
            != GST_SDP_OK
        || gst_sdp_message_medias_len(message) == 0) {
        if (message)
            gst_sdp_message_free(message);
        Q_EMIT failed(callId, QStringLiteral("bad_remote_answer"));
        return;
    }
    GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_ANSWER, message);
    GstPromise *promise = gst_promise_new_with_change_func(
        onRemoteAnswerSet, promiseCtxNew(this, m_session.webrtc, callId),
        promiseCtxFree);
    g_signal_emit_by_name(m_session.webrtc, "set-remote-description", answer,
                          promise);
    gst_webrtc_session_description_free(answer);
}

void GstCallMediaBackend::addRemoteCandidate(const QString &callId,
                                             const QString &candidate,
                                             const QString &sdpMid,
                                             int sdpMLineIndex)
{
    Q_UNUSED(sdpMid); // webrtcbin keys on the m-line index
    if (!m_sessionActive || m_session.callId != callId || !m_session.webrtc)
        return;
    if (candidate.trimmed().isEmpty())
        return; // MSC2746 end-of-candidates
    if (!m_session.remoteDescriptionSet) {
        // Trickled candidates can outrun the description; apply them once it
        // is set. Bounded.
        if (m_session.pendingRemoteCandidates.size() < 64)
            m_session.pendingRemoteCandidates.append(
                qMakePair(sdpMLineIndex, candidate));
        return;
    }
    g_signal_emit_by_name(m_session.webrtc, "add-ice-candidate",
                          static_cast<guint>(qMax(0, sdpMLineIndex)),
                          candidate.toUtf8().constData());
}

void GstCallMediaBackend::setAudioDeviceResolver(AudioDeviceResolver resolver)
{
    m_resolveDevices = std::move(resolver);
}

GstCallMediaBackend::AudioDevicePlan GstCallMediaBackend::resolveDevices() const
{
    return m_resolveDevices ? m_resolveDevices() : AudioDevicePlan{};
}

QString GstCallMediaBackend::receiveSinkDescription(
    const AudioDevicePlan &plan) const
{
    if (!plan.speakerSink.isEmpty())
        return plan.speakerSink;
    // The test tone plays nowhere; a real call follows the system default.
    return m_testTone ? QStringLiteral("fakesink sync=false name=outsink")
                      : QStringLiteral("autoaudiosink name=outsink");
}

GstElement *GstCallMediaBackend::buildMicrophoneFront(
    const AudioDevicePlan &plan) const
{
    QString front = plan.microphoneFront;
    if (front.isEmpty()) {
        front = m_testTone
            ? QStringLiteral("audiotestsrc is-live=true wave=sine freq=440 "
                             "volume=0.05 name=micsrc "
                             "! audioconvert ! audioresample")
            // autoaudiosrc for "system default", which keeps following the
            // default. Bounded and leaky: a default queue holds a second and
            // never drains it, which becomes permanent latency.
            : QStringLiteral("autoaudiosrc name=micsrc "
                             "! queue max-size-buffers=0 max-size-bytes=0 "
                             "max-size-time=100000000 leaky=downstream "
                             "! audioconvert ! audioresample");
    }
    // The shared front ends in a caps shorthand ("! audio/x-raw,..."), which
    // the parser accepts only BETWEEN two elements: as the last item it is
    // read as an element called "audio" (GST_PARSE_ERROR_NO_SUCH_ELEMENT).
    // A pass-through element after it makes the caps a real filter.
    front += QStringLiteral(" ! identity name=micfrontout");
    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(front.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
        // The description names elements only (the device is a binding set
        // after the parse), so it is safe to log; the message is not.
        qCWarning(lcCallMedia) << "capture description did not parse code="
                               << error->code << "description=" << front;
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        return nullptr;
    }
    if (!bin)
        return nullptr;
    // Unique per build, so an outgoing capture and its replacement can share
    // the pipeline for the moment of the swap.
    static std::atomic<int> serial{0};
    const QByteArray name =
        QByteArray("micfront") + QByteArray::number(serial.fetch_add(1));
    gst_element_set_name(bin, name.constData());
    applyBinding(bin, "micsrc", plan.microphoneBinding);
    return bin;
}

void GstCallMediaBackend::audioDevicesChanged()
{
    if (!m_sessionActive || !m_session.pipeline)
        return; // the next call resolves at its start
    const AudioDevicePlan plan = resolveDevices();
    AudioDevicePlan current;
    {
        QMutexLocker lock(&m_planMutex);
        current = m_plan;
    }
    // The signal also fires for a camera change: move only what changed, so a
    // camera pick never interrupts the microphone.
    const auto sameBinding = [](const lightning::calls::DeviceBinding &a,
                                const lightning::calls::DeviceBinding &b) {
        return a.property == b.property && a.value == b.value;
    };
    const bool micChanged = plan.microphoneFront != current.microphoneFront
        || !sameBinding(plan.microphoneBinding, current.microphoneBinding);
    const bool speakerChanged = plan.speakerSink != current.speakerSink
        || !sameBinding(plan.speakerBinding, current.speakerBinding);
    if (!micChanged && !speakerChanged)
        return;
    {
        QMutexLocker lock(&m_planMutex);
        if (micChanged) {
            m_plan.microphoneFront = plan.microphoneFront;
            m_plan.microphoneBinding = plan.microphoneBinding;
        }
        if (speakerChanged) {
            m_plan.speakerSink = plan.speakerSink;
            m_plan.speakerBinding = plan.speakerBinding;
        }
    }
    if (micChanged) {
        const bool ok = swapMicrophoneLocked(plan);
        qCInfo(lcCallMedia) << "microphone switched mid-call ok=" << ok
                            << "chosen=" << !plan.microphoneFront.isEmpty();
    }
    if (speakerChanged) {
        swapSpeakersLocked(plan);
        qCInfo(lcCallMedia) << "speaker switch requested mid-call chosen="
                            << !plan.speakerSink.isEmpty();
    }
}

bool GstCallMediaBackend::swapMicrophoneLocked(const AudioDevicePlan &plan)
{
    GstElement *pipeline = m_session.pipeline;
    GstElement *valve = m_session.micValve;
    if (!pipeline || !valve)
        return false;
    GstElement *next = buildMicrophoneFront(plan);
    if (!next) {
        qCWarning(lcCallMedia) << "microphone switch: the new capture could "
                                  "not be built; keeping the current one";
        return false;
    }
    // Stop the outgoing capture BEFORE unlinking it. Stopped first, its
    // threads see FLUSHING and end quietly; unlinked first, a live source
    // gets not-linked, which posts an error, and an error ends the call.
    if (GstElement *old = m_session.micFront) {
        gst_element_set_state(old, GST_STATE_NULL);
        gst_element_unlink(old, valve);
        gst_bin_remove(GST_BIN(pipeline), old); // drops the pipeline's ref
        m_session.micFront = nullptr;
    }
    const auto install = [&](GstElement *front) {
        if (!gst_bin_add(GST_BIN(pipeline), front))
            return false; // sunk and dropped by gst_bin_add
        if (!gst_element_link(front, valve)
            || !gst_element_sync_state_with_parent(front)) {
            gst_element_set_state(front, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(pipeline), front);
            return false;
        }
        m_session.micFront = front;
        return true;
    };
    bool ok = install(next);
    if (!ok && !plan.microphoneFront.isEmpty()) {
        qCWarning(lcCallMedia) << "microphone switch: the chosen device would "
                                  "not start; using the system default";
        if (GstElement *fallback = buildMicrophoneFront(AudioDevicePlan{}))
            install(fallback);
    }
    if (!m_session.micFront)
        return false;
    // The clock hold watches the capture's own pad (CaptureClock.h); the RTP
    // half stays on the payloader.
    if (GstElement *source =
            gst_bin_get_by_name(GST_BIN(m_session.micFront), "micsrc")) {
        if (GstPad *pad = gst_element_get_static_pad(source, "src")) {
            lightning::calls::watchReplacementCapture(pad,
                                                      m_session.clockHold);
            gst_object_unref(pad);
        }
        gst_object_unref(source);
    }
    if (ok)
        m_micSwaps.fetch_add(1);
    return ok;
}

void GstCallMediaBackend::swapSpeakersLocked(const AudioDevicePlan &plan)
{
    if (!m_session.pipeline)
        return;
    const QByteArray description = receiveSinkDescription(plan).toUtf8();
    // Every received track's volume element, matched by our name as in
    // setOutputMuted(); the sink after it is what moves, the volume (and so
    // the deafen state) stays.
    QList<GstElement *> volumes;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_session.pipeline));
    if (!it)
        return;
    GValue item = G_VALUE_INIT;
    bool done = false;
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            gchar *name = element ? gst_element_get_name(element) : nullptr;
            if (g_strcmp0(name, "outvol") == 0)
                volumes.append(GST_ELEMENT(gst_object_ref(element)));
            g_free(name);
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            for (GstElement *volume : volumes)
                gst_object_unref(volume);
            volumes.clear();
            if (resyncsLeft-- <= 0) {
                done = true;
                break;
            }
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    for (GstElement *volume : volumes) {
        GstObject *bin = gst_object_get_parent(GST_OBJECT(volume));
        GstPad *pad = gst_element_get_static_pad(volume, "src");
        if (bin && pad) {
            auto *swap = new SinkSwap;
            swap->bin = GST_ELEMENT(bin); // takes the parent's ref
            bin = nullptr;
            swap->description = description;
            swap->binding = plan.speakerBinding;
            swap->swaps = m_speakerSwaps;
            // Runs at once if the pad is idle, otherwise right after the
            // buffer in flight.
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_IDLE, swapSinkWhenIdle,
                              swap, sinkSwapFree);
        }
        if (bin)
            gst_object_unref(bin);
        if (pad)
            gst_object_unref(pad);
        gst_object_unref(volume);
    }
}

void GstCallMediaBackend::setMicrophoneMuted(const QString &callId,
                                             bool muted)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    m_session.micMuted = muted;
    if (!m_session.micValve)
        return;
    // drop=true discards buffers before the encoder: a real mute, no RTP.
    g_object_set(m_session.micValve, "drop", muted ? TRUE : FALSE, nullptr);
}

void GstCallMediaBackend::setOutputMuted(const QString &callId, bool muted)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    m_session.outputMuted = muted;
    // Read by onPadAdded on a GStreamer thread, so a track arriving while
    // deafened comes up silenced.
    m_outputMuted.store(muted);
    if (!m_session.pipeline)
        return;
    // Mute every receive volume element, matched by our name ("outvol"), not
    // the "volume" factory: auto elements may contain their own volume, and a
    // factory match could reach the send chain. Deafen and mic mute must stay
    // independent controls.
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_session.pipeline));
    if (!it)
        return;
    GValue item = G_VALUE_INIT;
    bool done = false;
    // Bounded resyncs, so a constantly changing pipeline cannot spin forever.
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            if (element) {
                gchar *name = gst_element_get_name(element);
                if (g_strcmp0(name, "outvol") == 0)
                    g_object_set(element, "mute", muted ? TRUE : FALSE,
                                 nullptr);
                g_free(name);
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            // The pipeline changes exactly while a track is added, which is
            // when a missed element would stay audible. Restart, bounded.
            if (resyncsLeft-- <= 0) {
                done = true;
                break;
            }
            gst_iterator_resync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
}

void GstCallMediaBackend::close(const QString &callId)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    destroySessionLocked();
}

// ── Qt-thread handlers ──────────────────────────────────────────────────

bool GstCallMediaBackend::tokenMatchesLiveSession(quintptr token) const
{
    if (!m_sessionActive)
        return false;
    return token == reinterpret_cast<quintptr>(m_session.webrtc)
        || token == reinterpret_cast<quintptr>(m_session.pipeline);
}

void GstCallMediaBackend::handleLocalDescription(quintptr token, bool offer,
                                                 const QString &sdp)
{
    if (!tokenMatchesLiveSession(token))
        return;
    if (sdp.isEmpty()) {
        handleFailure(token, QStringLiteral("description_failed"));
        return;
    }
    if (offer && m_session.offerSent) {
        // This lane never renegotiates; a second offer would reach the peer
        // as a new invite for the same call.
        qCWarning(lcCallMedia) << "a renegotiation offer was created and "
                                  "not sent";
        return;
    }
    if (offer)
        m_session.offerSent = true;
    qCInfo(lcCallMedia) << (offer ? "local offer sections="
                                  : "local answer sections=")
                        << sdpSectionShape(sdp);
    if (offer)
        Q_EMIT offerReady(m_session.callId, sdp);
    else
        Q_EMIT answerReady(m_session.callId, sdp);
}

void GstCallMediaBackend::handleRemoteDescriptionApplied(quintptr token)
{
    if (!tokenMatchesLiveSession(token))
        return;
    m_session.remoteDescriptionSet = true;
    flushPendingCandidatesLocked();
}

void GstCallMediaBackend::flushPendingCandidatesLocked()
{
    if (!m_session.webrtc)
        return;
    const auto pending = m_session.pendingRemoteCandidates;
    m_session.pendingRemoteCandidates.clear();
    for (const auto &entry : pending) {
        g_signal_emit_by_name(m_session.webrtc, "add-ice-candidate",
                              static_cast<guint>(qMax(0, entry.first)),
                              entry.second.toUtf8().constData());
    }
}

void GstCallMediaBackend::handleIceCandidate(quintptr token, int mlineIndex,
                                             const QString &candidate)
{
    if (!tokenMatchesLiveSession(token))
        return;
    Q_EMIT localCandidate(m_session.callId, candidate, QString(),
                          mlineIndex);
}

void GstCallMediaBackend::handleGatheringComplete(quintptr token)
{
    if (!tokenMatchesLiveSession(token))
        return;
    Q_EMIT gatheringComplete(m_session.callId);
}

void GstCallMediaBackend::requestStats()
{
    if (!m_sessionActive || !m_session.webrtc)
        return;
    struct StatsCtx {
        GstCallMediaBackend *backend = nullptr;
        quintptr token = 0;
    };
    auto *ctx = new StatsCtx{this, reinterpret_cast<quintptr>(m_session.webrtc)};
    GstPromise *promise = gst_promise_new_with_change_func(
        [](GstPromise *promise, gpointer data) {
            auto *ctx = static_cast<StatsCtx *>(data);
            GstCallMediaBackend *backend = ctx->backend;
            const quintptr token = ctx->token;
            StatsTotals totals;
            if (gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED) {
                if (const GstStructure *reply = gst_promise_get_reply(promise))
                    gst_structure_foreach(reply, addStat, &totals);
            }
            gst_promise_unref(promise); // may free ctx
            marshal(backend, [backend, token, totals] {
                // The ICE state is read here, on the Qt thread.
                int ice = -1;
                if (backend->tokenMatchesLiveSession(token)
                    && backend->m_session.webrtc) {
                    GstWebRTCICEConnectionState state =
                        GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
                    g_object_get(backend->m_session.webrtc,
                                 "ice-connection-state", &state, nullptr);
                    ice = int(state);
                }
                backend->handleStats(token, totals.inboundAudio,
                                     totals.inboundAudioBytes,
                                     totals.inboundLost, totals.outboundAudio,
                                     totals.inboundOther, ice);
            });
        },
        ctx, [](gpointer data) { delete static_cast<StatsCtx *>(data); });
    g_signal_emit_by_name(m_session.webrtc, "get-stats", nullptr, promise);
}

void GstCallMediaBackend::handleStats(quintptr token, quint64 inboundAudio,
                                      quint64 inboundAudioBytes,
                                      qint64 inboundLost,
                                      quint64 outboundAudio,
                                      quint64 inboundOther, int iceState)
{
    if (!tokenMatchesLiveSession(token))
        return;
    const int report = m_session.statsReports++;
    // A stall is news only once the transport is up (ICE connected or
    // completed), and an unmoving send count is the point of a mute.
    const bool transportUp = iceState == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED
        || iceState == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED;
    const bool inStalled = report > 0 && transportUp
        && inboundAudio == m_session.lastInboundAudioPackets;
    const bool outStalled = report > 0 && transportUp && !m_session.micMuted
        && outboundAudio == m_session.lastOutboundAudioPackets;
    m_session.lastInboundAudioPackets = inboundAudio;
    m_session.lastOutboundAudioPackets = outboundAudio;
    // The first half minute every 5 s, then once a minute, and whenever audio
    // stopped arriving or leaving: the line that tells "nothing arrived" from
    // "arrived and was not decoded" from "never sent". Counts only.
    if (report >= 6 && report % 12 != 0 && !inStalled && !outStalled)
        return;
    const QString counts =
        QStringLiteral("inbound audio packets=%1 bytes=%2 lost=%3 decoded=%4 "
                       "outbound audio packets=%5 inbound other packets=%6 "
                       "ice=%7")
            .arg(inboundAudio)
            .arg(inboundAudioBytes)
            .arg(inboundLost)
            .arg(m_decodedBuffers.load())
            .arg(outboundAudio)
            .arg(inboundOther)
            .arg(iceState);
    if (inStalled || outStalled) {
        qCWarning(lcCallMedia).noquote()
            << "rtp stats:" << (inStalled ? "no audio arriving;" : "")
            << (outStalled ? "no audio leaving;" : "") << counts;
    } else {
        qCInfo(lcCallMedia).noquote() << "rtp stats" << counts;
    }
}

void GstCallMediaBackend::handleConnectionState(quintptr token, int state)
{
    if (!tokenMatchesLiveSession(token))
        return;
    qCInfo(lcCallMedia) << "call media connection state=" << state;
    switch (static_cast<GstWebRTCPeerConnectionState>(state)) {
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
        qCInfo(lcCallMedia) << "call media connected";
        Q_EMIT connected(m_session.callId);
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
        handleFailure(token, QStringLiteral("media_connection"));
        break;
    default:
        break;
    }
}

void GstCallMediaBackend::handleFailure(quintptr token,
                                        const QString &category)
{
    if (!tokenMatchesLiveSession(token))
        return;
    const QString callId = m_session.callId;
    qCWarning(lcCallMedia) << "call media failed category=" << category;
    destroySessionLocked();
    Q_EMIT failed(callId, category);
}

// ── GStreamer-thread callbacks ─────────────────────────────────────────

void GstCallMediaBackend::onNegotiationNeeded(GstElement *webrtc,
                                              void *userData)
{
    // A closed call queues nothing more on its webrtcbin.
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    // Session identity travels as the emitting element's pointer (kept alive
    // by the promise ctx), so a stale offer cannot be attributed to a newer
    // session without reading backend state on this thread.
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, promiseCtxNew(backend, webrtc, QString()),
        promiseCtxFree);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

// promiseCtxFree is the promise's destroy notify, so the gst_promise_unref()
// in these change functions can free ctx (webrtcbin's error-reply path holds
// no other reference). Read every ctx field before the unref; a function
// that keeps using the element takes its own reference.
void GstCallMediaBackend::onOfferCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    // Held to the end: set-local-description must not start gathering on a
    // webrtcbin that is being retired.
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise); // frees ctx
        return;
    }
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    const QString sdp =
        applyCreatedDescription(promise, ctx->webrtc, "offer");
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    marshal(backend, [backend, token, sdp] {
        backend->handleLocalDescription(token, /*offer=*/true, sdp);
    });
}

void GstCallMediaBackend::onRemoteOfferSet(GstPromise *promise,
                                           void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise); // frees ctx
        return;
    }
    GstCallMediaBackend *backend = ctx->backend;
    // Our own reference: the ctx's is released by promiseCtxFree.
    GstElement *webrtc = GST_ELEMENT(gst_object_ref(ctx->webrtc));
    const QString callId = ctx->callId;
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const bool replied =
        gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    if (!replied) {
        gst_object_unref(webrtc);
        return;
    }
    marshal(backend, [backend, token] {
        backend->handleRemoteDescriptionApplied(token);
    });
    // Decline what this engine cannot carry before answering; see the header.
    const int declined = declineUnhandledSections(webrtc);
    if (declined > 0) {
        qCInfo(lcCallMedia) << "declining" << declined
                            << "offered section(s) that are not audio";
    }
    // Answer creation continues on this thread; the element is ref-held, and
    // an answer for a closed session is dropped by the Qt-side token check.
    GstPromise *answerPromise = gst_promise_new_with_change_func(
        onAnswerCreated, promiseCtxNew(backend, webrtc, callId),
        promiseCtxFree);
    g_signal_emit_by_name(webrtc, "create-answer", nullptr, answerPromise);
    gst_object_unref(webrtc);
}

void GstCallMediaBackend::onAnswerCreated(GstPromise *promise,
                                          void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise); // frees ctx
        return;
    }
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    const QString sdp =
        applyCreatedDescription(promise, ctx->webrtc, "answer");
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    marshal(backend, [backend, token, sdp] {
        backend->handleLocalDescription(token, /*offer=*/false, sdp);
    });
}

void GstCallMediaBackend::onRemoteAnswerSet(GstPromise *promise,
                                            void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    const bool replied =
        gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    if (!replied)
        return;
    marshal(backend, [backend, token] {
        backend->handleRemoteDescriptionApplied(token);
    });
}

void GstCallMediaBackend::onIceCandidateGst(GstElement *webrtc,
                                            unsigned mlineIndex,
                                            char *candidate, void *userData)
{
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const QString line = QString::fromUtf8(candidate ? candidate : "");
    const int index = static_cast<int>(mlineIndex);
    marshal(backend, [backend, token, index, line] {
        backend->handleIceCandidate(token, index, line);
    });
}

void GstCallMediaBackend::onIceGatheringNotify(GstElement *webrtc,
                                               void *pspec, void *userData)
{
    Q_UNUSED(pspec);
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
        return;
    marshal(backend, [backend, token] {
        backend->handleGatheringComplete(token);
    });
}

void GstCallMediaBackend::onConnectionNotify(GstElement *webrtc, void *pspec,
                                             void *userData)
{
    Q_UNUSED(pspec);
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc, "connection-state", &state, nullptr);
    const int value = static_cast<int>(state);
    marshal(backend, [backend, token, value] {
        backend->handleConnectionState(token, value);
    });
}

void GstCallMediaBackend::onPadAdded(GstElement *webrtc, void *pad,
                                     void *userData)
{
    // Closing waits for this handler, so it never adds to a pipeline that is
    // already stopping; a later pad of a closed call stays unlinked.
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    GstPad *srcPad = GST_PAD(pad);
    if (GST_PAD_DIRECTION(srcPad) != GST_PAD_SRC)
        return;
    GstElement *pipeline =
        GST_ELEMENT(gst_element_get_parent(webrtc)); // owns one ref
    if (!pipeline)
        return;
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    if (!padCarriesOpusAudio(srcPad)) {
        // A track the Opus chain cannot decode (a video section the peer
        // sends anyway, as webrtcbin does after an inactive answer). Drained,
        // never linked into the chain: not-negotiated there stops the shared
        // transport, and with it the call's audio.
        backend->m_drainedPads.fetch_add(1);
        qCInfo(lcCallMedia) << "a receive track that is not Opus audio is "
                               "discarded";
        GError *error = nullptr;
        GstElement *drain = gst_parse_bin_from_description(
            "fakesink sync=false async=false", TRUE, &error);
        bool ok = !error && drain;
        if (error)
            g_error_free(error);
        if (ok && !gst_bin_add(GST_BIN(pipeline), drain)) {
            drain = nullptr; // sunk and dropped by gst_bin_add
            ok = false;
        }
        if (ok) {
            gst_element_sync_state_with_parent(drain);
            GstPad *sinkPad = gst_element_get_static_pad(drain, "sink");
            ok = sinkPad && gst_pad_link(srcPad, sinkPad) == GST_PAD_LINK_OK;
            if (sinkPad)
                gst_object_unref(sinkPad);
        } else if (drain) {
            gst_object_unref(drain);
        }
        gst_object_unref(pipeline);
        if (!ok) {
            // An unlinked pad returns not-linked, which stops the transport
            // just the same, so this is still a failure.
            marshal(backend, [backend, token] {
                backend->handleFailure(token, QStringLiteral("media_receive"));
            });
        }
        return;
    }
    // The plan can change mid-call (a speaker switch), hence the lock; a
    // switch racing this pad is caught by the switch's own walk, which
    // starts after the plan is updated.
    AudioDevicePlan plan;
    {
        QMutexLocker lock(&backend->m_planMutex);
        plan = backend->m_plan;
    }
    const QString sink = backend->receiveSinkDescription(plan);
    // Bounded and leaky: the receive side is where a listener hears delay,
    // and a default queue would keep a stall's second of audio forever. 200 ms
    // absorbs local scheduling jitter. Leaking is safe for Opus (the decoder
    // conceals a lost frame), unlike RTP in front of a video depayloader.
    const QString recvQueue = QStringLiteral(
        "queue max-size-buffers=0 max-size-bytes=0 "
        "max-size-time=200000000 leaky=downstream ");
    // `outsink` is what a speaker switch replaces (swapSpeakersLocked()).
    const QString descriptionString = recvQueue
        + QStringLiteral("! rtpopusdepay ! opusdec name=recvdec ! audioconvert "
                         "! audioresample ! volume name=outvol ! %1")
              .arg(sink);
    const QByteArray descriptionUtf8 = descriptionString.toUtf8();
    const char *description = descriptionUtf8.constData();
    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description, TRUE, &error);
    if (error) {
        g_error_free(error);
        if (bin)
            gst_object_unref(bin); // non-NULL result beside a set error
        gst_object_unref(pipeline);
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    if (!gst_bin_add(GST_BIN(pipeline), bin)) {
        // gst_bin_add sinks and drops the element on failure: never touch bin
        // again.
        gst_object_unref(pipeline);
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    applyBinding(bin, "outsink", plan.speakerBinding);
    // Apply the current deafen state before the bin plays, so a new track is
    // never briefly audible.
    if (GstElement *vol = gst_bin_get_by_name(GST_BIN(bin), "outvol")) {
        g_object_set(vol, "mute",
                     backend->m_outputMuted.load() ? TRUE : FALSE, nullptr);
        if (backend->m_testTone) {
            if (GstPad *out = gst_element_get_static_pad(vol, "src")) {
                gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_BUFFER, recordPeak,
                                  &backend->m_receivedPeakMilli, nullptr);
                gst_object_unref(out);
            }
        }
        gst_object_unref(vol);
    }
    // Decoded buffers, for the stats line: RTP that arrived but never reached
    // a decoder is a different fault from RTP that never arrived.
    if (GstElement *dec = gst_bin_get_by_name(GST_BIN(bin), "recvdec")) {
        if (GstPad *out = gst_element_get_static_pad(dec, "src")) {
            gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_BUFFER, countDecoded,
                              &backend->m_decodedBuffers, nullptr);
            gst_object_unref(out);
        }
        gst_object_unref(dec);
    }
    gst_element_sync_state_with_parent(bin);
    GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
    if (backend->m_testTone) {
        gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, countPacket,
                          &backend->m_receivedAudioPackets, nullptr);
    }
    const GstPadLinkReturn linked = gst_pad_link(srcPad, sinkPad);
    gst_object_unref(sinkPad);
    gst_object_unref(pipeline);
    if (linked != GST_PAD_LINK_OK) {
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    // The pad name ("src_0") only: which received track became audible.
    gchar *padName = gst_pad_get_name(srcPad);
    qCInfo(lcCallMedia) << "an Opus receive track is linked pad="
                        << (padName ? padName : "?")
                        << "chosenSpeaker=" << !plan.speakerSink.isEmpty();
    g_free(padName);
}
