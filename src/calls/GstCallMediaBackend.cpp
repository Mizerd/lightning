#include "GstCallMediaBackend.h"

#include "calls/CaptureClock.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/SerialSinkSwap.h"
#include "calls/GstBootstrap.h"
#include "calls/SfuVideoRouter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <utility>

// Coarse lifecycle/category lines only. Never log SDP, candidates, TURN
// credentials, or GStreamer error details that could embed them.
Q_LOGGING_CATEGORY(lcCallMedia, "matrix.calls.media")

unsigned int GstCallMediaBackend::microphoneRtpMtuForTest() const
{
    if (!m_session.pipeline)
        return 0;
    GstElement *pay = gst_bin_get_by_name(GST_BIN(m_session.pipeline), "micpay");
    if (!pay)
        return 0;
    unsigned int mtu = 0;
    g_object_get(pay, "mtu", &mtu, nullptr);
    gst_object_unref(pay);
    return mtu;
}

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

GstPadProbeReturn dropAllProbe(GstPad *, GstPadProbeInfo *, gpointer)
{
    return GST_PAD_PROBE_DROP;
}

/// Removes `front` from `pipeline` now and stops it on a pool thread, under
/// its state lock, so a start of it still queued finds it parentless.
void stopRemovedFrontAsync(GstBin *pipeline, GstElement *front)
{
    gst_object_ref(front);
    const bool locked = g_rec_mutex_trylock(GST_STATE_GET_LOCK(front));
    gst_bin_remove(pipeline, front);
    if (locked)
        GST_STATE_UNLOCK(front);
    gst_element_call_async(
        front,
        [](GstElement *element, gpointer) {
            GST_STATE_LOCK(element);
            gst_element_set_state(element, GST_STATE_NULL);
            GST_STATE_UNLOCK(element);
        },
        nullptr, nullptr);
    gst_object_unref(front);
}

/// One capture start on a pool thread.
struct FrontStart {
    GstCallMediaBackend *backend = nullptr; // only handed to marshal()
    quint64 serial = 0;
    quint64 frontSerial = 0;
    bool chosen = false;
    std::shared_ptr<std::atomic<bool>> pending;
};

void frontStartAsync(GstElement *front, gpointer data)
{
    auto *ctx = static_cast<FrontStart *>(data);
    QElapsedTimer took;
    took.start();
    // Parent checked and state changed under the front's state lock, which
    // the detach (try) and the stop take: a front already removed is left.
    GST_STATE_LOCK(front);
    bool ok = false;
    if (GstObject *parent = gst_object_get_parent(GST_OBJECT(front))) {
        gst_element_set_locked_state(front, FALSE);
        ok = gst_element_sync_state_with_parent(front);
        gst_object_unref(parent);
    }
    GST_STATE_UNLOCK(front);
    const qint64 ms = took.elapsed();
    ctx->pending->store(false);
    GstCallMediaBackend *backend = ctx->backend;
    const quint64 serial = ctx->serial;
    const quint64 frontSerial = ctx->frontSerial;
    const bool chosen = ctx->chosen;
    marshal(backend, [backend, serial, frontSerial, ok, chosen, ms] {
        backend->noteMicrophoneFrontStarted(serial, frontSerial, ok, chosen,
                                            ms);
    });
}

void frontStartFree(gpointer data)
{
    delete static_cast<FrontStart *>(data);
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

constexpr int kDefaultVp8PayloadType = 96;

// The live video section's dynamic VP8 payload type, or -1 when the offer has
// no such section or no VP8 in it (the section is then declined). Scoped to
// the section: an audio section's numbers mean nothing here.
int vp8PayloadTypeFromSdp(const QString &sdp)
{
    const qsizetype section = sdp.indexOf(QLatin1String("m=video "));
    if (section < 0)
        return -1;
    const qsizetype next = sdp.indexOf(QLatin1String("\nm="), section);
    const QString video =
        next < 0 ? sdp.mid(section) : sdp.mid(section, next - section);
    // Port 0 rejects a section, except under a=bundle-only: that is how a
    // max-bundle offerer (webrtcbin, so Lightning and Nheko) offers one.
    if (video.startsWith(QLatin1String("m=video 0 "))
        && !video.contains(QLatin1String("a=bundle-only")))
        return -1;
    static const QRegularExpression rtpmap(
        QStringLiteral("a=rtpmap:(\\d{1,3})\\s+VP8/90000"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rtpmap.match(video);
    if (!match.hasMatch())
        return -1;
    bool ok = false;
    const int payload = match.captured(1).toInt(&ok);
    return ok && payload >= 96 && payload <= 127 ? payload : -1;
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

// Sets every transceiver whose offered section this call does not carry to
// inactive, before the answer is created, so the answer declines it instead of
// accepting a track this engine cannot play: everything that is not audio,
// unless `video` says the session has a video chain for the video section.
// Keyed on the offer's section: webrtcbin reports our own audio transceiver's
// kind as unknown at this point. Returns how many were declined.
int declineUnhandledSections(GstElement *webrtc, bool video)
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
        const QString kind = offeredKind(remote->sdp, mline);
        if (kind != QLatin1String("audio")
            && !(video && kind == QLatin1String("video"))) {
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

// Whether a receive pad carries VP8 video, the only video this engine decodes.
// Unlike the Opus check an absent field is not good enough: a video chain
// linked to the wrong track stops the transport, and draining costs nothing.
bool padCarriesVp8Video(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);
    if (!caps)
        return false;
    bool vp8 = false;
    if (!gst_caps_is_empty(caps) && !gst_caps_is_any(caps)
        && gst_caps_get_size(caps) > 0) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *media = gst_structure_get_string(structure, "media");
        const gchar *encoding =
            gst_structure_get_string(structure, "encoding-name");
        vp8 = media && encoding && g_ascii_strcasecmp(media, "video") == 0
            && g_ascii_strcasecmp(encoding, "VP8") == 0;
    }
    gst_caps_unref(caps);
    return vp8;
}

// Which picture an appsink feeds; one per sink, freed with its closure.
struct VideoSinkCtx {
    GstCallMediaBackend *backend = nullptr; // handed to marshal(); see below
    std::shared_ptr<SfuVideoRouter> router;
    QString key;
    quintptr token = 0;
    bool remote = false;
    int padSerial = 0;
};

void videoSinkCtxFree(void *data, GClosure *)
{
    delete static_cast<VideoSinkCtx *>(data);
}

constexpr auto kRemoteVideoKey = "remote";
constexpr auto kSelfVideoKey = "self";

// One decoded RGBA frame to the router, as the MatrixRTC lane delivers them:
// copied, because a QVideoFrame cannot borrow the GstBuffer, and through the
// router's one-frame mailbox, so a busy GUI thread collects at most one.
GstFlowReturn onVideoSample(GstElement *sink, void *userData)
{
    auto *ctx = static_cast<VideoSinkCtx *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample)
        return GST_FLOW_OK;
    // Only a remote sink may touch the backend: its pipeline stops before the
    // backend goes. A self-view can outlive it (the front stops on a pool
    // thread), so it has the router it shares and marshal(), which checks.
    if (ctx->remote) {
        ctx->backend->noteRemoteVideoFrame(ctx->token);
        // One surface and possibly two tracks: a peer's screen share beside
        // its camera (see remoteFrameWanted()).
        if (!ctx->backend->remoteFrameWanted(ctx->padSerial)) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }
    }
    SfuVideoRouter *router = ctx->router.get();
    if (!router || !router->watching(ctx->key)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    int width = 0;
    int height = 0;
    if (caps && gst_caps_get_size(caps) > 0) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
    }
    // Dimensions come from the peer and size an allocation: bound them.
    GstMapInfo map;
    if (!buffer || width <= 0 || height <= 0 || width > 8192 || height > 8192
        || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    QVideoFrame frame(QVideoFrameFormat(QSize(width, height),
                                        QVideoFrameFormat::Format_RGBA8888));
    if (frame.map(QVideoFrame::WriteOnly)) {
        const int sourceStride = width * 4;
        const int targetStride = frame.bytesPerLine(0);
        const qsizetype available = static_cast<qsizetype>(map.size);
        uchar *target = frame.bits(0);
        for (int row = 0; row < height; ++row) {
            const qsizetype offset = qsizetype(row) * sourceStride;
            if (offset + sourceStride > available)
                break;
            std::memcpy(target + qsizetype(row) * targetStride,
                        map.data + offset, size_t(sourceStride));
        }
        frame.unmap();
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    if (router->offerFrame(ctx->key, frame)) {
        GstCallMediaBackend *backend = ctx->backend;
        const QString key = ctx->key;
        marshal(backend, [backend, key] {
            backend->videoRouter()->deliverPending(key);
        });
    }
    return GST_FLOW_OK;
}

// Whether `element` sits in a bin whose name starts with one of `prefixes`.
// The camera front, the send chain and the receive chain are not the call: an
// error in one costs a picture.
bool insideBinNamed(GstObject *element, std::initializer_list<const char *> prefixes)
{
    GstObject *walk = element ? GST_OBJECT(gst_object_ref(element)) : nullptr;
    bool found = false;
    while (walk && !found) {
        gchar *name = gst_object_get_name(walk);
        for (const char *prefix : prefixes)
            found = found || (name && g_str_has_prefix(name, prefix));
        g_free(name);
        GstObject *parent = gst_object_get_parent(walk);
        gst_object_unref(walk);
        walk = parent;
    }
    if (walk)
        gst_object_unref(walk);
    return found;
}

/// One camera start on a pool thread; see frontStartAsync().
struct CameraStart {
    GstCallMediaBackend *backend = nullptr;
    quintptr token = 0;
};

void cameraStartAsync(GstElement *front, gpointer data)
{
    auto *ctx = static_cast<CameraStart *>(data);
    GST_STATE_LOCK(front);
    bool ok = true;
    if (GstObject *parent = gst_object_get_parent(GST_OBJECT(front))) {
        gst_element_set_locked_state(front, FALSE);
        ok = gst_element_sync_state_with_parent(front);
        gst_object_unref(parent);
    }
    GST_STATE_UNLOCK(front);
    if (!ok) {
        GstCallMediaBackend *backend = ctx->backend;
        const quintptr token = ctx->token;
        marshal(backend, [backend, token] {
            QMetaObject::invokeMethod(
                backend, "handleCameraLost", Qt::DirectConnection,
                Q_ARG(quintptr, token),
                Q_ARG(QString, QStringLiteral("camera_failed")));
        });
    }
}

void cameraStartFree(gpointer data)
{
    delete static_cast<CameraStart *>(data);
}

GstPadProbeReturn dropWhenDead(GstPad *, GstPadProbeInfo *, gpointer dead)
{
    return static_cast<std::atomic<bool> *>(dead)->load() ? GST_PAD_PROBE_DROP
                                                          : GST_PAD_PROBE_OK;
}

GstPadProbeReturn countSharedPacket(GstPad *, GstPadProbeInfo *, gpointer data)
{
    (*static_cast<std::shared_ptr<std::atomic<int>> *>(data))->fetch_add(1);
    return GST_PAD_PROBE_OK;
}

void sharedCounterFree(gpointer data)
{
    delete static_cast<std::shared_ptr<std::atomic<int>> *>(data);
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

/// The new output for a received track (a speaker switch), built on the
/// switch's pool thread (lightning::calls::sinkswap): parsed, its latency
/// settled like the SFU lane's, its device bound.
GstElement *buildReplacementSink(const QByteArray &description,
                                 const lightning::calls::DeviceBinding &binding)
{
    GError *error = nullptr;
    GstElement *next = gst_parse_bin_from_description(description.constData(),
                                                      TRUE, &error);
    if (error || !next) {
        if (error)
            g_error_free(error);
        if (next)
            gst_object_unref(gst_object_ref_sink(next));
        return nullptr;
    }
    next = GST_ELEMENT(gst_object_ref_sink(next));
    // Without it the new sink gets no latency and drops buffers as late:
    // choppy audio on the new output (measured on the SFU lane).
    g_object_set(next, "async-handling", TRUE, nullptr);
    applyBinding(next, "outsink", binding);
    return next;
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
        // receiver. A camera that fails costs the picture, never the call.
        GstObject *source = GST_MESSAGE_SRC(message);
        const bool received = insideBinNamed(source, {"camrecv"});
        const bool camera =
            received || insideBinNamed(source, {"camfront", "camchain"});
        QMutexLocker lock(&g_aliveMutex);
        if (g_aliveBackends.contains(ctx->backend)) {
            QMetaObject::invokeMethod(
                ctx->backend, camera ? "handleCameraLost" : "handleFailure",
                Qt::QueuedConnection, Q_ARG(quintptr, ctx->pipelineToken),
                Q_ARG(QString, received ? QStringLiteral("video_receive")
                               : camera ? QStringLiteral("camera_failed")
                                        : QStringLiteral("media_pipeline")));
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

bool GstCallMediaBackend::videoRuntimeAvailable()
{
    if (!lightning::gst::ensureInitialised(nullptr))
        return false;
    static const char *const kRequired[] = {
        "vp8enc",       "vp8dec",     "rtpvp8pay", "rtpvp8depay",
        "videoconvert", "appsink",    "tee",
    };
    for (const char *name : kRequired) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (!factory)
            return false;
        gst_object_unref(factory);
    }
    return true;
}

bool GstCallMediaBackend::supportsVideo() const
{
    static const bool available = videoRuntimeAvailable();
    return available;
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
    m_videoRouter = std::make_shared<SfuVideoRouter>();
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

void GstCallMediaBackend::prepareCall(const QString &callId, bool video,
                                      bool cameraOn)
{
    m_preparedCallId = callId;
    m_preparedVideo = video;
    m_preparedCamera = cameraOn;
}

bool GstCallMediaBackend::startSession(const QString &callId, bool offerer,
                                       int opusPayloadType, int vp8PayloadType)
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
        "! rtpopuspay name=micpay pt=%1 ssrc=%2 mtu=%3 "
        "! application/x-rtp,media=audio,encoding-name=OPUS,payload=%1,"
        "ssrc=(uint)%2 "
        "! wb. ").arg(payload).arg(ssrc).arg(lightning::rtp::kRtpPayloadMtu);
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
    m_session.streamId = QUuid::createUuid().toString(QUuid::Id128);
    m_receivedVideoPackets.store(0);
    m_decodedVideoFrames.store(0);
    m_sentVideoFrames->store(0);
    m_videoReceiveDead.store(false);
    m_remoteVideoReceiving = false;
    m_lastRemoteFrameMs.store(0);
    m_remoteVideoPads.store(0);
    m_lastNewestPadFrameMs.store(0);
    // Both tracks name one stream, as a browser's do: a peer reading MSC3077
    // metadata looks the stream up by this id.
    setPadStreamId(webrtc, "sink_0", m_session.streamId);
    if (GstPad *audioPad = gst_element_get_static_pad(webrtc, "sink_0")) {
        statePadCaps(audioPad,
                     QStringLiteral("application/x-rtp,media=audio,"
                                    "encoding-name=OPUS,clock-rate=(int)48000,"
                                    "encoding-params=(string)2,payload=(int)%1")
                         .arg(payload));
        gst_object_unref(audioPad);
    }
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

    // The answerer is driven by createAnswer(); this asks for an offer only
    // when m_offerWanted says so (the first one, and an upgrade to video).
    m_offerWanted.store(offerer);
    g_signal_connect(webrtc, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
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

    // A video call: the send chain is on webrtcbin before the first
    // description exists, with or without a camera behind it.
    const bool prepared = m_preparedCallId == callId;
    if (vp8PayloadType > 0 && supportsVideo()
        && !addVideoChainLocked(vp8PayloadType)) {
        qCWarning(lcCallMedia) << "the video chain did not build; this call "
                                  "carries audio only";
    }
    m_session.cameraWanted = prepared && m_preparedCamera;
    m_acceptVideo.store(m_session.camInput != nullptr);
    // Told, or the call says the camera is on while nothing opened it. Queued:
    // the controller is still inside createOffer()/createAnswer().
    if (m_session.cameraWanted && !m_session.camInput) {
        m_session.cameraWanted = false;
        QMetaObject::invokeMethod(
            this,
            [this, callId] {
                Q_EMIT cameraFailed(callId, QStringLiteral("camera_unavailable"));
            },
            Qt::QueuedConnection);
    }
    m_preparedCallId.clear();

    // The capture is started apart, off this thread (startMicrophoneFrontAsync):
    // its source opening a sound server that does not answer blocked the GUI
    // thread for as long as the server took (issue #12 family). Locked out of
    // the pipeline's own state change until then.
    m_session.serial = ++m_sessionSerial;
    gst_element_set_locked_state(front, TRUE);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        qCWarning(lcCallMedia) << "pipeline refused to start";
        destroySessionLocked();
        return false;
    }
    startMicrophoneFrontAsync(front, !plan.microphoneFront.isEmpty());
    if (m_session.camInput && m_session.cameraWanted)
        startCameraLocked();
    // webrtcbin asks for negotiation only once a buffer has told it the caps,
    // and a capture muted before its first buffer never does: the offer would
    // not come at all. They are stated above, so nothing needs to arrive.
    if (offerer)
        onNegotiationNeeded(webrtc, this);
    if (!m_negotiationTimer) {
        m_negotiationTimer = new QTimer(this);
        m_negotiationTimer->setSingleShot(true);
        connect(m_negotiationTimer, &QTimer::timeout, this,
                &GstCallMediaBackend::onNegotiationTimeout);
        m_offerDelayTimer = new QTimer(this);
        m_offerDelayTimer->setSingleShot(true);
        connect(m_offerDelayTimer, &QTimer::timeout, this, [this] {
            if (m_sessionActive && m_session.offerDelayed)
                sendOfferLocked();
        });
    }
    qCInfo(lcCallMedia) << "media session started offerer=" << offerer
                        << "video=" << (m_session.camInput != nullptr)
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
    if (m_negotiationTimer) {
        m_negotiationTimer->stop();
        m_offerDelayTimer->stop();
    }
    // The capture leaves first, started or not, and is stopped off this
    // thread: the pipeline's NULL below would otherwise wait on its state
    // lock (a start in flight) or on its own stop (a capture whose sound
    // server stopped answering mid-call), on this thread. Its output drops
    // everything first, so nothing it still pushes meets an unlinked pad.
    detachStartingFront();
    stopCameraLocked();
    if (GstElement *front = m_session.micFront; front && m_session.pipeline) {
        if (GstPad *out = gst_element_get_static_pad(front, "src")) {
            gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
                              dropAllProbe, nullptr, nullptr);
            gst_object_unref(out);
        }
        stopRemovedFrontAsync(GST_BIN(m_session.pipeline), front);
        m_session.micFront = nullptr;
    }
    // Never a plain set_state(NULL) (GitHub #3): the pipeline stops here, its
    // webrtcbin once ICE gathering has ended. Takes both references.
    m_retirer.retire(m_session.pipeline, m_session.webrtc);
    m_session = Session();
    m_sessionActive = false;
    // Engine state is per session. The controller owns the deafen intent and
    // re-applies it; a latched value would silence a later call.
    m_outputMuted.store(false);
    m_acceptVideo.store(false);
    m_offerWanted.store(false);
    m_remoteVideoReceiving = false;
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
    const bool video = m_preparedCallId == callId && m_preparedVideo;
    if (!startSession(callId, /*offerer=*/true, kDefaultOpusPayloadType,
                      video ? kDefaultVp8PayloadType : -1))
        Q_EMIT failed(callId, QStringLiteral("media_init"));
    // The offer arrives via on-negotiation-needed -> create-offer.
}

void GstCallMediaBackend::createAnswer(const QString &callId,
                                       const QString &remoteOfferSdp)
{
    // Taken before set-remote-description: webrtcbin pairs an offered section
    // with a sink pad it already has, so the chain must exist by then.
    if (!startSession(callId, /*offerer=*/false,
                      opusPayloadTypeFromSdp(remoteOfferSdp),
                      vp8PayloadTypeFromSdp(remoteOfferSdp))) {
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

void GstCallMediaBackend::statePadCaps(GstPad *sink, const QString &caps)
{
    GstWebRTCRTPTransceiver *transceiver = nullptr;
    g_object_get(sink, "transceiver", &transceiver, nullptr);
    GstCaps *rtp = gst_caps_from_string(caps.toUtf8().constData());
    if (transceiver && rtp) {
        g_object_set(transceiver, "codec-preferences", rtp, "direction",
                     GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV, nullptr);
    }
    if (rtp)
        gst_caps_unref(rtp);
    if (transceiver)
        gst_object_unref(transceiver);
}

void GstCallMediaBackend::setPadStreamId(GstElement *webrtc, const char *pad,
                                         const QString &streamId)
{
    GstPad *sink = gst_element_get_static_pad(webrtc, pad);
    if (!sink)
        return;
    // Absent on an old webrtcbin: the id then stays what webrtcbin chose, and
    // metadata must not name a stream the SDP does not.
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "msid"))
        g_object_set(sink, "msid", streamId.toUtf8().constData(), nullptr);
    else
        m_session.streamId.clear();
    gst_object_unref(sink);
}

bool GstCallMediaBackend::addVideoChainLocked(int payloadType)
{
    if (m_session.camInput)
        return true;
    if (!m_session.pipeline || !m_session.webrtc)
        return false;
    const quint32 ssrc = QRandomGenerator::global()->bounded(1u, 0xFFFFFFFFu);
    // Same values as the MatrixRTC camera at 720p. The queue in front is the
    // live-path rule: bounded and leaky, a late frame is worth nothing. The
    // caps name the ssrc for the reason startSession() gives, and the feedback
    // a browser needs to ask for a keyframe; without it a lost one freezes the
    // picture for good.
    const QString description = QStringLiteral(
        "queue name=caminput max-size-buffers=0 max-size-bytes=0 "
        "max-size-time=100000000 leaky=downstream "
        "! videoconvert ! video/x-raw,format=I420 "
        "! vp8enc deadline=1 lag-in-frames=0 threads=4 cpu-used=2 "
        "static-threshold=0 keyframe-max-dist=30 end-usage=cbr "
        "target-bitrate=1700000 "
        "! rtpvp8pay name=vidpay pt=%1 ssrc=%2 mtu=%3 picture-id-mode=15-bit "
        "! capsfilter caps=\"application/x-rtp,media=video,encoding-name=VP8,"
        "clock-rate=(int)90000,payload=(int)%1,ssrc=(uint)%2,"
        "rtcp-fb-nack-pli=(boolean)true,rtcp-fb-ccm-fir=(boolean)true,"
        "rtcp-fb-nack=(boolean)true\"")
                                    .arg(payloadType)
                                    .arg(ssrc)
                                    .arg(lightning::rtp::kRtpPayloadMtu);
    GError *error = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error) {
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        return false;
    }
    gst_element_set_name(bin, "camchain");
    if (!gst_bin_add(GST_BIN(m_session.pipeline), bin))
        return false; // sunk and dropped by gst_bin_add
    GstPad *sink = gst_element_request_pad_simple(m_session.webrtc, "sink_%u");
    GstPad *out = gst_element_get_static_pad(bin, "src");
    bool linked = sink && out && gst_pad_link(out, sink) == GST_PAD_LINK_OK;
    if (sink && linked && !m_session.streamId.isEmpty()
        && g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "msid")) {
        g_object_set(sink, "msid", m_session.streamId.toUtf8().constData(),
                     nullptr);
    }
    // No buffer may ever pass with the camera off: see statePadCaps().
    if (sink && linked) {
        statePadCaps(sink,
                     QStringLiteral("application/x-rtp,media=video,"
                                    "encoding-name=VP8,clock-rate=(int)90000,"
                                    "payload=(int)%1,"
                                    "rtcp-fb-nack-pli=(boolean)true,"
                                    "rtcp-fb-ccm-fir=(boolean)true,"
                                    "rtcp-fb-nack=(boolean)true")
                         .arg(payloadType));
    }
    if (out)
        gst_object_unref(out);
    if (sink)
        gst_object_unref(sink);
    if (!linked) {
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_session.pipeline), bin);
        return false;
    }
    gst_element_sync_state_with_parent(bin);
    m_session.camInput = bin; // borrowed: the pipeline owns it
    return true;
}

GstElement *GstCallMediaBackend::buildCameraFront(CameraPlan plan)
{
    QString front = plan.front;
    if (front.isEmpty() && m_testTone) {
        front = QStringLiteral("videotestsrc is-live=true pattern=ball "
                               "name=capsrc "
                               "! video/x-raw,width=320,height=180,"
                               "framerate=(fraction)15/1");
    }
    if (front.isEmpty())
        return nullptr;
    if (plan.chooseEntry && !plan.probe.isEmpty()) {
        GError *error = nullptr;
        GstElement *probe = gst_parse_bin_from_description(
            plan.probe.toUtf8().constData(), FALSE, &error);
        if (error) {
            g_error_free(error);
            if (probe)
                gst_object_unref(probe);
            return nullptr;
        }
        probe = GST_ELEMENT(gst_object_ref_sink(probe));
        applyBinding(probe, "capsrc", plan.binding);
        front.replace(QStringLiteral("@ENTRY@"), plan.chooseEntry(probe));
        gst_element_set_state(probe, GST_STATE_NULL);
        gst_object_unref(probe);
    }
    // The self-view lives in the front, so it stops with the camera. Both
    // branches are bounded: a slow picture must not stall the other one, and
    // `async=false` keeps a capture with no first frame from holding the
    // running pipeline back (selfViewBranch() in the MatrixRTC lane).
    front += QStringLiteral(
        " ! tee name=camt "
        "camt. ! queue max-size-buffers=2 leaky=downstream ! videoconvert "
        "! video/x-raw,format=RGBA ! appsink name=selfvidsink "
        "emit-signals=true sync=false async=false max-buffers=1 drop=true "
        "camt. ! queue max-size-buffers=2 leaky=downstream "
        "! identity name=camfrontout");
    GError *error = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        front.toUtf8().constData(), TRUE, &error);
    if (error) {
        qCWarning(lcCallMedia) << "camera description did not parse code="
                               << error->code;
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        return nullptr;
    }
    if (!bin)
        return nullptr;
    static std::atomic<int> serial{0};
    const QByteArray name =
        QByteArray("camfront") + QByteArray::number(serial.fetch_add(1));
    gst_element_set_name(bin, name.constData());
    applyBinding(bin, "capsrc", plan.binding);
    if (GstElement *self = gst_bin_get_by_name(GST_BIN(bin), "selfvidsink")) {
        auto *ctx = new VideoSinkCtx{
            this, m_videoRouter, QString::fromLatin1(kSelfVideoKey), 0, false};
        g_signal_connect_data(self, "new-sample", G_CALLBACK(onVideoSample),
                              ctx, videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(self);
    }
    if (GstElement *out = gst_bin_get_by_name(GST_BIN(bin), "camfrontout")) {
        if (GstPad *pad = gst_element_get_static_pad(out, "src")) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, countSharedPacket,
                              new std::shared_ptr<std::atomic<int>>(
                                  m_sentVideoFrames),
                              sharedCounterFree);
            gst_object_unref(pad);
        }
        gst_object_unref(out);
    }
    return bin;
}

bool GstCallMediaBackend::startCameraLocked()
{
    if (m_session.camFront || !m_session.camInput || !m_session.pipeline)
        return m_session.camFront != nullptr;
    const CameraPlan plan = m_resolveCamera
        ? m_resolveCamera(m_session.cameraPortalFd)
        : CameraPlan{};
    if (plan.needsPortal) {
        // Answered by providePortalCamera(), possibly after a dialog.
        Q_EMIT cameraAccessNeeded(m_session.callId);
        return true;
    }
    if (!plan.refusal.isEmpty()) {
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(m_session.callId, plan.refusal);
        return false;
    }
    GstElement *front = buildCameraFront(plan);
    if (!front) {
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(m_session.callId,
                            QStringLiteral("camera_failed"));
        return false;
    }
    if (!gst_bin_add(GST_BIN(m_session.pipeline), front)
        || !gst_element_link(front, m_session.camInput)) {
        if (GST_OBJECT_PARENT(front))
            gst_bin_remove(GST_BIN(m_session.pipeline), front);
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(m_session.callId,
                            QStringLiteral("camera_failed"));
        return false;
    }
    m_session.camFront = front; // borrowed: the pipeline owns it
    // Off this thread, as the microphone: opening a device can block.
    gst_element_set_locked_state(front, TRUE);
    gst_element_call_async(
        front, cameraStartAsync,
        new CameraStart{this, reinterpret_cast<quintptr>(m_session.pipeline)},
        cameraStartFree);
    qCInfo(lcCallMedia) << "camera started";
    return true;
}

void GstCallMediaBackend::stopCameraLocked()
{
    GstElement *front = std::exchange(m_session.camFront, nullptr);
    if (!front || !m_session.pipeline) {
        closePortalFdLocked(); // granted, and nothing was built on it
        return;
    }
    if (GstPad *out = gst_element_get_static_pad(front, "src")) {
        gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
                          dropAllProbe, nullptr, nullptr);
        gst_object_unref(out);
    }
    stopRemovedFrontAsync(GST_BIN(m_session.pipeline), front);
    closePortalFdLocked();
    qCInfo(lcCallMedia) << "camera stopped";
}

void GstCallMediaBackend::closePortalFdLocked()
{
#ifdef Q_OS_LINUX
    // The stop above is asynchronous, but pipewiresrc dups or keeps its own:
    // the MatrixRTC lane holds it for the bin only because that varies by
    // plugin version, and a late close there is the same close.
    if (m_session.cameraPortalFd >= 0)
        ::close(m_session.cameraPortalFd);
#endif
    m_session.cameraPortalFd = -1;
}

void GstCallMediaBackend::providePortalCamera(const QString &callId, int fd)
{
    const bool live = m_sessionActive && m_session.callId == callId
        && m_session.cameraWanted;
    if (!live) {
#ifdef Q_OS_LINUX
        if (fd >= 0)
            ::close(fd);
#endif
        return;
    }
    if (fd < 0) {
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(callId, QStringLiteral("camera_unavailable"));
        return;
    }
    closePortalFdLocked();
    m_session.cameraPortalFd = fd;
    startCameraLocked();
}

void GstCallMediaBackend::setCameraEnabled(const QString &callId, bool on)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    m_session.cameraWanted = on;
    if (!on) {
        stopCameraLocked();
        return;
    }
    if (m_session.camInput) {
        startCameraLocked();
        // An earlier offer for this section was never answered.
        if (m_session.videoUnconfirmed && !m_session.negotiating)
            requestOfferLocked();
        return;
    }
    // A voice call gaining a camera: the chain goes on webrtcbin, which then
    // asks for negotiation. Only once the first exchange is finished: a
    // section added under an open offer would be silently left out of it.
    if (!supportsVideo() || !m_session.remoteDescriptionSet
        || m_session.negotiating) {
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(callId, QStringLiteral("camera_unavailable"));
        return;
    }
    if (!addVideoChainLocked(kDefaultVp8PayloadType)) {
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(callId, QStringLiteral("camera_failed"));
        return;
    }
    m_acceptVideo.store(true);
    m_session.videoUnconfirmed = true;
    startCameraLocked();
    requestOfferLocked();
}

void GstCallMediaBackend::requestOfferLocked()
{
    m_session.negotiating = true;
    m_session.localOfferSet = false;
    m_negotiationTimer->start(m_negotiationTimeoutMs);
    // Two offers that cross cannot be undone: webrtcbin takes a rollback and
    // then aborts the process answering the next offer (measured, 1.28.6,
    // `_create_answer_task: (g_strcmp0 (mid, last_mid) == 0)`, and a peer
    // could trigger that at will). So the callee waits a moment before it
    // offers, and a caller's offer arriving meanwhile serves both.
    if (!m_session.offerer) {
        m_session.offerDelayed = true;
        m_offerDelayTimer->start(m_calleeOfferDelayMs);
        return;
    }
    sendOfferLocked();
}

void GstCallMediaBackend::sendOfferLocked()
{
    m_session.offerDelayed = false;
    m_offerWanted.store(true);
    // webrtcbin may not ask by itself: nothing new reaches it while the camera
    // is still opening.
    onNegotiationNeeded(m_session.webrtc, this);
}

void GstCallMediaBackend::onNegotiationTimeout()
{
    if (!m_sessionActive || !m_session.negotiating)
        return;
    // A lost send, a silent peer, or crossed offers. The offer stays with
    // webrtcbin (see requestOfferLocked()), so this end takes no further peer
    // offer; the call goes on, and turning the camera on again offers anew.
    qCWarning(lcCallMedia) << "a renegotiation went unanswered";
    m_session.negotiating = false;
    m_session.offerDelayed = false;
    m_offerDelayTimer->stop();
    if (m_session.cameraWanted) {
        stopCameraLocked();
        m_session.cameraWanted = false;
        Q_EMIT cameraFailed(m_session.callId,
                            QStringLiteral("camera_unavailable"));
    }
}

void GstCallMediaBackend::applyNegotiation(const QString &callId,
                                           const QString &type,
                                           const QString &sdp)
{
    if (!m_sessionActive || m_session.callId != callId || !m_session.webrtc)
        return;
    const bool offer = type == QLatin1String("offer");
    if (!offer && type != QLatin1String("answer"))
        return;
    if (offer && m_session.offerDelayed && vp8PayloadTypeFromSdp(sdp) > 0) {
        // Our offer has not left: the caller's carries what it asked for.
        m_offerDelayTimer->stop();
        m_negotiationTimer->stop();
        m_session.offerDelayed = false;
        m_session.negotiating = false;
        m_session.videoUnconfirmed = false;
    }
    if (offer && m_session.offerDelayed) {
        // A caller's offer without video (a hold): ours waits until it is done.
        m_offerDelayTimer->start(m_calleeOfferDelayMs);
        applyRemoteOfferLocked(sdp);
        return;
    }
    if (offer && (m_session.negotiating || m_session.localOfferSet)) {
        // Crossed, or one we gave up on: webrtcbin cannot take an offer now
        // (see requestOfferLocked()). The timeout reports it.
        qCInfo(lcCallMedia) << "an offer into our own open one was ignored";
        return;
    }
    if (!offer && !m_session.negotiating) {
        qCInfo(lcCallMedia) << "an unasked-for renegotiation answer was ignored";
        return;
    }
    if (offer) {
        applyRemoteOfferLocked(sdp);
        return;
    }
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(sdp.toUtf8().constData(), &message)
            != GST_SDP_OK
        || gst_sdp_message_medias_len(message) == 0) {
        if (message)
            gst_sdp_message_free(message);
        qCWarning(lcCallMedia) << "an unusable renegotiation was ignored";
        return;
    }
    qCInfo(lcCallMedia) << "renegotiation answered sections="
                        << sdpSectionShape(sdp);
    m_session.negotiating = false;
    m_session.localOfferSet = false;
    m_session.videoUnconfirmed = false;
    m_negotiationTimer->stop();
    GstWebRTCSessionDescription *description =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                                           message); // takes ownership
    GstPromise *promise = gst_promise_new_with_change_func(
        onRemoteAnswerSet, promiseCtxNew(this, m_session.webrtc, callId),
        promiseCtxFree);
    g_signal_emit_by_name(m_session.webrtc, "set-remote-description",
                          description, promise);
    gst_webrtc_session_description_free(description);
}

void GstCallMediaBackend::applyRemoteOfferLocked(const QString &sdp)
{
    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new_from_text(sdp.toUtf8().constData(), &message)
            != GST_SDP_OK
        || gst_sdp_message_medias_len(message) == 0) {
        if (message)
            gst_sdp_message_free(message);
        qCWarning(lcCallMedia) << "an unusable renegotiation was ignored";
        return;
    }
    qCInfo(lcCallMedia) << "renegotiation offered sections="
                        << sdpSectionShape(sdp);
    // The peer turned its camera on in a voice call. Our chain has to be on
    // webrtcbin before the description is, or the section is answered with
    // nothing to send; no camera opens until the user asks for it.
    const int payload = vp8PayloadTypeFromSdp(sdp);
    if (payload > 0 && supportsVideo() && addVideoChainLocked(payload))
        m_acceptVideo.store(true);
    GstWebRTCSessionDescription *description =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER,
                                           message); // takes ownership
    GstPromise *promise = gst_promise_new_with_change_func(
        onRemoteOfferSet,
        promiseCtxNew(this, m_session.webrtc, m_session.callId),
        promiseCtxFree);
    g_signal_emit_by_name(m_session.webrtc, "set-remote-description",
                          description, promise);
    gst_webrtc_session_description_free(description);
}

void GstCallMediaBackend::failCameraForTest()
{
    handleCameraLost(reinterpret_cast<quintptr>(m_session.pipeline),
                     QStringLiteral("camera_failed"));
}

void GstCallMediaBackend::failVideoReceiveForTest()
{
    handleCameraLost(reinterpret_cast<quintptr>(m_session.pipeline),
                     QStringLiteral("video_receive"));
}

void GstCallMediaBackend::attachVideoSink(bool self, QObject *sink)
{
    auto *videoSink = qobject_cast<QVideoSink *>(sink);
    const QString key = QString::fromLatin1(self ? kSelfVideoKey
                                                 : kRemoteVideoKey);
    if (videoSink)
        m_videoRouter->attachSink(key, videoSink);
}

void GstCallMediaBackend::noteRemoteVideoFrame(quintptr token)
{
    m_decodedVideoFrames.fetch_add(1);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastRemoteFrameMs.exchange(now) != 0)
        return;
    GstCallMediaBackend *backend = this;
    marshal(backend, [backend, token] {
        if (!backend->tokenMatchesLiveSession(token)
            || backend->m_remoteVideoReceiving)
            return;
        backend->m_remoteVideoReceiving = true;
        Q_EMIT backend->remoteVideoChanged(backend->m_session.callId, true);
    });
}

bool GstCallMediaBackend::remoteFrameWanted(int padSerial)
{
    // The newest track wins, which is a peer's screen share over its camera,
    // and the older one comes back within a moment of the newest going quiet.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (padSerial == m_remoteVideoPads.load()) {
        m_lastNewestPadFrameMs.store(now);
        return true;
    }
    return now - m_lastNewestPadFrameMs.load() > kNewestPadQuietMs;
}

void GstCallMediaBackend::handleCameraLost(quintptr token,
                                           const QString &category)
{
    if (!tokenMatchesLiveSession(token))
        return;
    qCWarning(lcCallMedia) << "the camera failed category=" << category
                           << "; the call goes on without it";
    if (category == QLatin1String("video_receive")) {
        // Nothing more enters that chain, so it cannot stop the shared
        // transport. Our camera is not involved.
        m_videoReceiveDead.store(true);
        if (m_remoteVideoReceiving) {
            m_remoteVideoReceiving = false;
            m_lastRemoteFrameMs.store(0);
            Q_EMIT remoteVideoChanged(m_session.callId, false);
        }
        return;
    }
    // A send chain error with the camera off: nothing the user asked for.
    if (!m_session.camFront && !m_session.cameraWanted)
        return;
    stopCameraLocked();
    m_session.cameraWanted = false;
    Q_EMIT cameraFailed(m_session.callId, category);
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

void GstCallMediaBackend::setCameraResolver(CameraResolver resolver)
{
    m_resolveCamera = std::move(resolver);
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
            ? (m_testFront.isEmpty()
                   ? QStringLiteral("audiotestsrc is-live=true wave=sine "
                                    "freq=440 volume=0.05 name=micsrc "
                                    "! audioconvert ! audioresample")
                   : m_testFront)
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
    // The outgoing capture leaves the pipeline at once and is stopped off
    // this thread (a capture whose server stalled blocks its stop too). Its
    // output drops everything first, so a push in flight never meets an
    // unlinked pad (not-linked posts an error, and an error ends the call),
    // and once removed it has no bus to post on. One still starting is
    // detached the same way.
    if (m_session.frontStarting && m_session.frontStarting->load()) {
        detachStartingFront();
    } else if (GstElement *old = m_session.micFront) {
        if (GstPad *out = gst_element_get_static_pad(old, "src")) {
            gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
                              dropAllProbe, nullptr, nullptr);
            gst_object_unref(out);
        }
        stopRemovedFrontAsync(GST_BIN(pipeline), old);
        m_session.micFront = nullptr;
    }
    const auto install = [&](GstElement *front) {
        if (!gst_bin_add(GST_BIN(pipeline), front))
            return false; // sunk and dropped by gst_bin_add
        if (!gst_element_link(front, valve)) {
            gst_bin_remove(GST_BIN(pipeline), front);
            return false;
        }
        m_session.micFront = front;
        return true;
    };
    const bool chosen = !plan.microphoneFront.isEmpty();
    bool ok = install(next);
    if (!ok && chosen) {
        qCWarning(lcCallMedia) << "microphone switch: the chosen device would "
                                  "not link; using the system default";
        if (GstElement *fallback = buildMicrophoneFront(AudioDevicePlan{}))
            install(fallback);
    }
    if (!m_session.micFront)
        return false;
    // Started off this thread; a chosen device that will not start falls
    // back to the default there (noteMicrophoneFrontStarted()).
    startMicrophoneFrontAsync(m_session.micFront, ok && chosen);
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

QStringList GstCallMediaBackend::receiveOutputsForTest() const
{
    QStringList out;
    if (!m_session.pipeline)
        return out;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_session.pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        auto *element = GST_ELEMENT(g_value_get_object(&item));
        gchar *name = gst_element_get_name(element);
        if (g_strcmp0(name, "outsink") == 0) {
            GstElementFactory *factory = gst_element_get_factory(element);
            out << QString::fromUtf8(factory ? GST_OBJECT_NAME(factory)
                                             : "?");
        }
        g_free(name);
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return out;
}

void GstCallMediaBackend::startMicrophoneFrontAsync(GstElement *front,
                                                    bool chosenDevice)
{
    auto pending = std::make_shared<std::atomic<bool>>(true);
    m_session.frontStarting = pending;
    m_session.frontSerial = ++m_frontSerial;
    auto *ctx = new FrontStart{this, m_session.serial, m_session.frontSerial,
                               chosenDevice, pending};
    gst_element_call_async(front, frontStartAsync, ctx, frontStartFree);
}

void GstCallMediaBackend::detachStartingFront()
{
    if (!m_session.frontStarting || !m_session.frontStarting->load()
        || !m_session.micFront || !m_session.pipeline)
        return;
    qCInfo(lcCallMedia) << "the microphone is still starting; it is detached "
                           "and stopped off the GUI thread";
    stopRemovedFrontAsync(GST_BIN(m_session.pipeline), m_session.micFront);
    m_session.micFront = nullptr;
    m_session.frontStarting.reset();
    m_session.frontSerial = 0;
}

void GstCallMediaBackend::noteMicrophoneFrontStarted(quint64 serial,
                                                     quint64 frontSerial,
                                                     bool ok,
                                                     bool chosenDevice,
                                                     qint64 tookMs)
{
    // Another call, or a capture since replaced: nothing to say.
    if (!m_sessionActive || serial != m_session.serial
        || frontSerial != m_session.frontSerial || !m_session.micFront)
        return;
    ++m_frontStartsReported;
    if (tookMs >= 500) {
        qCWarning(lcCallMedia)
            << "call diagnosis: starting the microphone took" << tookMs
            << "ms (the sound server was slow to answer); the window stayed "
               "responsive meanwhile";
    }
    if (ok)
        return;
    if (chosenDevice) {
        qCWarning(lcCallMedia) << "the chosen microphone would not start; "
                                  "using the system default";
        AudioDevicePlan fallback;
        {
            QMutexLocker lock(&m_planMutex);
            fallback = m_plan;
        }
        fallback.microphoneFront.clear();
        fallback.microphoneBinding = {};
        swapMicrophoneLocked(fallback);
        return;
    }
    qCWarning(lcCallMedia) << "the microphone did not start; its capture "
                              "reports why";
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
            // Off this thread and one at a time per chain, the newest pick
            // winning (SerialSinkSwap.h): closing the old output and opening
            // the new one wait on the sound server, and this is the GUI
            // thread.
            lightning::calls::sinkswap::Request request;
            const lightning::calls::DeviceBinding binding = plan.speakerBinding;
            request.build = [description, binding] {
                return buildReplacementSink(description, binding);
            };
            request.swaps = m_speakerSwaps;
            request.warn = [](const char *why) {
                qCWarning(lcCallMedia) << "speaker switch:" << why;
            };
            lightning::calls::sinkswap::request(GST_ELEMENT(bin), pad,
                                                std::move(request));
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
    const bool renegotiation = m_session.answerSent || m_session.offerSent;
    if (sdp.isEmpty()) {
        if (renegotiation) {
            // The call is up and did not need this: it stays as it was.
            qCWarning(lcCallMedia) << "a renegotiation produced no description";
            m_session.negotiating = false;
            m_negotiationTimer->stop();
            if (m_session.cameraWanted && offer) {
                stopCameraLocked();
                m_session.cameraWanted = false;
                Q_EMIT cameraFailed(m_session.callId,
                                    QStringLiteral("camera_failed"));
            }
            return;
        }
        handleFailure(token, QStringLiteral("description_failed"));
        return;
    }
    qCInfo(lcCallMedia) << (offer ? "local offer sections="
                                  : "local answer sections=")
                        << sdpSectionShape(sdp);
    // The first description of each kind sets the call up; every later one is
    // a renegotiation, which reaches the peer as m.call.negotiate and never as
    // a new invite for the same call. An answerer's first offer is such a one
    // too: its first description was the answer.
    if (renegotiation) {
        if (offer)
            m_session.localOfferSet = true;
        Q_EMIT negotiationReady(m_session.callId,
                                offer ? QStringLiteral("offer")
                                      : QStringLiteral("answer"),
                                sdp);
        return;
    }
    if (offer) {
        m_session.offerSent = true;
        Q_EMIT offerReady(m_session.callId, sdp);
    } else {
        m_session.answerSent = true;
        Q_EMIT answerReady(m_session.callId, sdp);
    }
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
    // A picture that stopped: the peer's camera went away without metadata,
    // or its packets did. Nothing else would ever say so.
    if (m_remoteVideoReceiving
        && QDateTime::currentMSecsSinceEpoch() - m_lastRemoteFrameMs.load()
            > kRemoteVideoStallMs) {
        m_remoteVideoReceiving = false;
        m_lastRemoteFrameMs.store(0);
        Q_EMIT remoteVideoChanged(m_session.callId, false);
    }
    // The first half minute every 5 s, then once a minute, and whenever audio
    // stopped arriving or leaving: the line that tells "nothing arrived" from
    // "arrived and was not decoded" from "never sent". Counts only.
    if (report >= 6 && report % 12 != 0 && !inStalled && !outStalled)
        return;
    const QString counts =
        QStringLiteral("inbound audio packets=%1 bytes=%2 lost=%3 decoded=%4 "
                       "outbound audio packets=%5 inbound other packets=%6 "
                       "ice=%7 video frames decoded=%8 sent=%9")
            .arg(inboundAudio)
            .arg(inboundAudioBytes)
            .arg(inboundLost)
            .arg(m_decodedBuffers.load())
            .arg(outboundAudio)
            .arg(inboundOther)
            .arg(iceState)
            .arg(m_decodedVideoFrames.load())
            .arg(m_sentVideoFrames->load());
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
    if (!backend->m_offerWanted.exchange(false))
        return;
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
    const int declined =
        declineUnhandledSections(webrtc, backend->m_acceptVideo.load());
    if (declined > 0) {
        qCInfo(lcCallMedia) << "declining" << declined
                            << "offered section(s) this call does not carry";
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
    if (backend->m_acceptVideo.load() && padCarriesVp8Video(srcPad)) {
        // Bounded, not leaky: this is RTP into a depayloader, and a dropped
        // packet corrupts the VP8 bitstream downstream of webrtcbin. Latency
        // is bounded by the appsink's drop=true instead. The depayloader asks
        // for a keyframe on a gap, which webrtcbin sends as a PLI.
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            "queue max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 "
            "! rtpvp8depay request-keyframe=true wait-for-keyframe=true "
            "! vp8dec ! videoconvert ! video/x-raw,format=RGBA "
            "! appsink name=vidsink emit-signals=true sync=false async=false "
            "max-buffers=1 drop=true",
            TRUE, &error);
        bool ok = !error && bin;
        if (error)
            g_error_free(error);
        if (ok) {
            static std::atomic<int> receiveSerial{0};
            const QByteArray name = QByteArray("camrecv")
                + QByteArray::number(receiveSerial.fetch_add(1));
            gst_element_set_name(bin, name.constData());
            GstElement *sink = gst_bin_get_by_name(GST_BIN(bin), "vidsink");
            auto *ctx = new VideoSinkCtx{
                backend, backend->m_videoRouter,
                QString::fromLatin1(kRemoteVideoKey), token, true,
                backend->m_remoteVideoPads.fetch_add(1) + 1};
            g_signal_connect_data(sink, "new-sample",
                                  G_CALLBACK(onVideoSample), ctx,
                                  videoSinkCtxFree, GConnectFlags(0));
            gst_object_unref(sink);
            ok = gst_bin_add(GST_BIN(pipeline), bin);
            if (ok) {
                gst_element_sync_state_with_parent(bin);
                GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
                gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
                                  countPacket,
                                  &backend->m_receivedVideoPackets, nullptr);
                gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
                                  dropWhenDead, &backend->m_videoReceiveDead,
                                  nullptr);
                ok = gst_pad_link(srcPad, sinkPad) == GST_PAD_LINK_OK;
                gst_object_unref(sinkPad);
                if (!ok) {
                    gst_element_set_state(bin, GST_STATE_NULL);
                    gst_bin_remove(GST_BIN(pipeline), bin);
                }
            }
        } else if (bin) {
            gst_object_unref(bin);
        }
        if (ok) {
            gst_object_unref(pipeline);
            qCInfo(lcCallMedia) << "a VP8 receive track is linked";
            return;
        }
        // Falls through to the drain: the picture is lost, the call is not.
        qCWarning(lcCallMedia) << "the VP8 receive chain did not build";
    }
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
