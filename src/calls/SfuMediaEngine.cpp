#include "calls/SfuMediaEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "calls/CallFrameCryptor.h"
#include "calls/CaptureClock.h"
#include "calls/CaptureDeviceSelection.h"
#include "calls/GstBootstrap.h"
#include "calls/RtpH264Payloader.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/SerialSinkSwap.h"
#include "calls/WindowCaptureSrc.h"
#include "calls/noise/DenoiseElement.h"
#include "calls/noise/MicProcessing.h"
#include "calls/SfuVideoRouter.h"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <QVersionNumber>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/sdp/sdp.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-event.h>
#if defined(LIGHTNING_HAVE_GST_ALLOCATORS)
#include <gst/allocators/gstdmabuf.h>
#endif
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

namespace {
std::atomic<bool> g_shareHardwareEncoderFailed{false};
std::atomic<bool> g_shareDmaBufCaptureFailed{false};
std::atomic<bool> g_shareSystemMemoryCaptureFailed{false};
QString g_pipewirePluginVersionForTest;
constexpr const char kCaptureRetryKey[] = "lightning-capture-retry";

int captureRetryStamp(GObject *bin)
{
    const auto *retry = static_cast<std::shared_ptr<ShareCaptureRetry> *>(
        g_object_get_data(bin, kCaptureRetryKey));
    return retry && *retry ? (*retry)->stamp() : 0;
}
const QLatin1String kShareBackupSuffix("-vp8");

Q_LOGGING_CATEGORY(lcSfuMedia, "lightning.calls.sfu")

// GStreamer calls back on its own threads, so a marshalled lambda must never
// run against a destroyed engine. The queued invocation uses the engine as
// receiver, so anything already in flight dies with the QObject.
QMutex g_aliveMutex;
QSet<SfuMediaEngine *> g_aliveEngines;
/// g_aliveMutex is not recursive. Held only inside marshal() and the
/// engine's constructor and destructor, never across a call that could
/// marshal: a rebuild job once did, and every marshal() in the process then
/// waited forever (the engine suite timed out).
thread_local bool t_holdingAliveMutex = false;

template <typename Fn>
void marshal(SfuMediaEngine *engine, Fn &&fn)
{
    Q_ASSERT_X(!t_holdingAliveMutex, "marshal",
               "g_aliveMutex is already held on this thread; it is not "
               "recursive");
    QMutexLocker lock(&g_aliveMutex);
    t_holdingAliveMutex = true;
    struct Release {
        ~Release() { t_holdingAliveMutex = false; }
    } release;
    if (!g_aliveEngines.contains(engine))
        return;
    QMetaObject::invokeMethod(engine, std::forward<Fn>(fn),
                              Qt::QueuedConnection);
}

QString sdpCodecSummary(const QString &sdp)
{
    QString out;
    QString section;
    const QList<QStringView> lines = QStringView(sdp).split(u'\n');
    auto flush = [&out, &section]() {
        if (!section.isEmpty()) {
            out += (out.isEmpty() ? QStringLiteral("") : QStringLiteral(" "))
                 + QStringLiteral("[") + section.trimmed()
                 + QStringLiteral("]");
            section.clear();
        }
    };
    for (QStringView raw : lines) {
        const QStringView line = raw.trimmed();
        if (line.startsWith(u"m=")) {
            flush();
            const QList<QStringView> f = line.mid(2).split(u' ',
                                                          Qt::SkipEmptyParts);
            if (!f.isEmpty())
                section = f.first().toString();
        } else if (line.startsWith(u"a=rtpmap:") && !section.isEmpty()) {
            const QStringView body = line.mid(9);
            const qsizetype sp = body.indexOf(u' ');
            if (sp > 0) {
                section += QStringLiteral(" pt") + body.left(sp).toString()
                         + QStringLiteral("=") + body.mid(sp + 1).toString();
            }
        }
    }
    flush();
    return out.isEmpty() ? QStringLiteral("<none>") : out;
}

qint64 monotonicMs()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

struct PromiseCtx {
    SfuMediaEngine *engine = nullptr;
    GstElement *webrtc = nullptr;
    bool publisher = false;
    quint64 generation = 0;
    quint64 revision = 0;
    bool offer = false;
    bool quiet = false;
    bool iceRestart = false;
    QByteArray remoteIceUfrag;
    QByteArray remoteIcePwd;
    QString sdp;
};

PromiseCtx *promiseCtxNew(SfuMediaEngine *engine, GstElement *webrtc,
                          bool publisher)
{
    auto *ctx = new PromiseCtx;
    ctx->engine = engine;
    ctx->webrtc = GST_ELEMENT(gst_object_ref(webrtc));
    ctx->publisher = publisher;
    return ctx;
}

void promiseCtxFree(void *data)
{
    auto *ctx = static_cast<PromiseCtx *>(data);
    if (ctx->webrtc)
        gst_object_unref(ctx->webrtc);
    delete ctx;
}

using NiceRestartStream = gboolean (*)(gpointer agent, guint streamId);

// GstWebRTCICE has no restart; webrtcbin's own libnice agent does. Resolved
// from the module that defines NiceAgent, since that module is loaded locally.
NiceRestartStream niceRestartStreamFor(GObject *agent)
{
    auto *anchor = reinterpret_cast<void *>(G_OBJECT_GET_CLASS(agent)->get_property);
    if (!anchor)
        return nullptr;
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(anchor), &module)) {
        return nullptr;
    }
    return reinterpret_cast<NiceRestartStream>(
        reinterpret_cast<void *>(GetProcAddress(module, "nice_agent_restart_stream")));
#else
    Dl_info info{};
    if (!dladdr(anchor, &info) || !info.dli_fname)
        return nullptr;
    void *handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!handle)
        return nullptr;
    void *symbol = dlsym(handle, "nice_agent_restart_stream");
    dlclose(handle);
    return reinterpret_cast<NiceRestartStream>(symbol);
#endif
}

GObject *iceStreamOfTransport(GObject *dtls)
{
    if (!dtls)
        return nullptr;
    GObject *stream = nullptr;
    GObject *ice = nullptr;
    g_object_get(dtls, "transport", &ice, nullptr);
    if (ice) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "stream"))
            g_object_get(ice, "stream", &stream, nullptr);
        g_object_unref(ice);
    }
    return stream;
}

GObject *iceStreamOf(GstElement *webrtc)
{
    GObject *stream = nullptr;
    GArray *transceivers = nullptr;
    g_signal_emit_by_name(webrtc, "get-transceivers", &transceivers);
    if (transceivers) {
        for (guint i = 0; i < transceivers->len && !stream; ++i) {
            auto *transceiver = g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);
            GstWebRTCRTPReceiver *receiver = nullptr;
            if (transceiver)
                g_object_get(transceiver, "receiver", &receiver, nullptr);
            if (!receiver)
                continue;
            GObject *dtls = nullptr;
            g_object_get(receiver, "transport", &dtls, nullptr);
            stream = iceStreamOfTransport(dtls);
            if (dtls)
                g_object_unref(dtls);
            gst_object_unref(receiver);
        }
        g_array_unref(transceivers);
    }
    if (!stream) {
        GObject *sctp = nullptr;
        g_object_get(webrtc, "sctp-transport", &sctp, nullptr);
        if (sctp) {
            GObject *dtls = nullptr;
            g_object_get(sctp, "transport", &dtls, nullptr);
            stream = iceStreamOfTransport(dtls);
            if (dtls)
                g_object_unref(dtls);
            g_object_unref(sctp);
        }
    }
    return stream;
}

// webrtcbin never restarts ICE: libnice keeps checking the pair the remote side
// has just forgotten. libnice's restart also clears the remote credentials, so
// the offer's are installed again.
bool restartIceStream(GstElement *webrtc, const QByteArray &remoteUfrag,
                      const QByteArray &remotePwd)
{
    if (remoteUfrag.isEmpty() || remotePwd.isEmpty())
        return false;
    GObject *ice = nullptr;
    g_object_get(webrtc, "ice-agent", &ice, nullptr);
    if (!ice)
        return false;
    GObject *agent = nullptr;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent"))
        g_object_get(ice, "agent", &agent, nullptr);
    GObject *stream = iceStreamOf(webrtc);
    guint streamId = 0;
    if (stream)
        g_object_get(stream, "stream-id", &streamId, nullptr);
    const NiceRestartStream restart = agent ? niceRestartStreamFor(agent) : nullptr;
    const bool ok = restart && streamId != 0 && restart(agent, streamId)
        && gst_webrtc_ice_set_remote_credentials(
               GST_WEBRTC_ICE(ice), GST_WEBRTC_ICE_STREAM(stream),
               remoteUfrag.constData(), remotePwd.constData());
    if (stream)
        g_object_unref(stream);
    if (agent)
        g_object_unref(agent);
    g_object_unref(ice);
    return ok;
}

QString candidateInitJson(const QString &candidate, int mlineIndex)
{
    QJsonObject object;
    object.insert(QStringLiteral("candidate"), candidate);
    object.insert(QStringLiteral("sdpMLineIndex"), mlineIndex);
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool parseCandidateInit(const QString &json, QString *candidate,
                        int *mlineIndex)
{
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject object = document.object();
    const QString line = object.value(QStringLiteral("candidate")).toString();
    if (line.isEmpty())
        return false;
    *candidate = line;
    *mlineIndex = object.value(QStringLiteral("sdpMLineIndex")).toInt(0);
    return true;
}
}

namespace {
void announceBlocked(SfuMediaEngine *engine, const QString &streamId,
                     const QString &reason)
{
    if (!engine)
        return;
    QMetaObject::invokeMethod(
        engine,
        [engine, streamId, reason] {
            Q_EMIT engine->remoteMediaBlocked(streamId, reason);
        },
        Qt::QueuedConnection);
}

using StreamInfo = lightning::calls::StreamInfoStats;
using StreamInfoPtr = std::shared_ptr<StreamInfo>;
constexpr const char *streamInfoKey = "lightning-stream-info";

StreamInfoPtr attachStreamInfo(GstElement *bin)
{
    auto stats = std::make_shared<StreamInfo>();
    g_object_set_data_full(G_OBJECT(bin), streamInfoKey, new StreamInfoPtr(stats),
        [](gpointer data) { delete static_cast<StreamInfoPtr *>(data); });
    return stats;
}

// Fixed closed-set factory name only: never serialise source properties (a
// portal source description can carry a file descriptor or device path).
QString streamInfoFactory(GstElement *bin, const char *name)
{
    GstElement *element = gst_bin_get_by_name(GST_BIN(bin), name);
    if (!element)
        return {};
    GstElementFactory *factory = gst_element_get_factory(element);
    const QString result = factory
        ? QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)))
        : QString();
    gst_object_unref(element);
    return result;
}

void instrumentStreamInfo(GstPad *pad, const StreamInfoPtr &stats, bool rtp)
{
    if (!pad)
        return;
    struct Context { StreamInfoPtr stats; bool rtp; };
    gst_pad_add_probe(pad,
        GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST
                        | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto *ctx = static_cast<Context *>(data);
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
                GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
                if (ctx->rtp && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
                    GstCaps *caps = nullptr;
                    gst_event_parse_caps(event, &caps);
                    guint ssrc = 0;
                    if (caps && gst_caps_get_size(caps)
                        && gst_structure_get_uint(gst_caps_get_structure(caps, 0),
                                                  "ssrc", &ssrc))
                        ctx->stats->ssrc = ssrc;
                }
                if (!ctx->rtp && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
                    GstCaps *caps = nullptr;
                    gst_event_parse_caps(event, &caps);
                    if (caps && gst_caps_get_size(caps)) {
                        const GstStructure *s = gst_caps_get_structure(caps, 0);
                        int w = 0, h = 0;
                        gst_structure_get_int(s, "width", &w);
                        gst_structure_get_int(s, "height", &h);
                        ctx->stats->width = w;
                        ctx->stats->height = h;
                    }
                }
                return GST_PAD_PROBE_OK;
            }
            auto count = [ctx](GstBuffer *buffer) {
                if (!buffer) return;
                if (ctx->rtp) {
                    ctx->stats->bytes.fetch_add(gst_buffer_get_size(buffer));
                    ctx->stats->packets.fetch_add(1);
                } else {
                    ctx->stats->frames.fetch_add(1);
                    if (ctx->stats->outgoing
                        && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
                        ctx->stats->keyframes.fetch_add(1);
                }
            };
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
                count(GST_PAD_PROBE_INFO_BUFFER(info));
            else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
                for (guint i = 0; i < gst_buffer_list_length(list); ++i)
                    count(gst_buffer_list_get(list, i));
            }
            return GST_PAD_PROBE_OK;
        }, new Context{stats, rtp},
        [](gpointer data) { delete static_cast<Context *>(data); });
}

// A depayloader waiting for a keyframe asks again on every delta frame it
// discards, about thirty a second; a receiver passes on one per interval.
void instrumentKeyframeRequests(GstPad *pad, const StreamInfoPtr &stats,
                                qint64 minIntervalMs = 0)
{
    if (!pad)
        return;
    struct Context {
        StreamInfoPtr stats;
        qint64 minIntervalMs;
        std::atomic<qint64> lastMs{-1};
    };
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto *ctx = static_cast<Context *>(data);
            GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
            if (!event || !gst_video_event_is_force_key_unit(event))
                return GST_PAD_PROBE_OK;
            const qint64 now = monotonicMs();
            const qint64 last = ctx->lastMs.load();
            if (ctx->minIntervalMs > 0 && last >= 0
                && now - last < ctx->minIntervalMs)
                return GST_PAD_PROBE_DROP;
            ctx->lastMs.store(now);
            if (ctx->stats)
                ctx->stats->keyframeRequests.fetch_add(1);
            gboolean allHeaders = FALSE;
            gst_video_event_parse_upstream_force_key_unit(event, nullptr,
                                                          &allHeaders, nullptr);
            if (ctx->minIntervalMs > 0 && allHeaders) {
                // rtpsession turns all-headers into a FIR, which LiveKit was
                // not seen to pass on to the publisher; ask with a PLI.
                GST_PAD_PROBE_INFO_DATA(info) =
                    gst_video_event_new_upstream_force_key_unit(
                        GST_CLOCK_TIME_NONE, FALSE, 0);
                gst_event_unref(event);
            }
            return GST_PAD_PROBE_OK;
        }, new Context{stats, minIntervalMs, {}},
        [](gpointer data) { delete static_cast<Context *>(data); });
}

// rtpvp9depay joins a picture's spatial layers into one buffer, but a sender
// encrypts each layer's frame on its own; ending the picture at every layer
// hands the decrypt probe one frame at a time.
void splitVp9SpatialLayers(GstPad *depaySink)
{
    if (!depaySink)
        return;
    gst_pad_add_probe(depaySink, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer) {
            GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            if (!buffer || !gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp))
                return GST_PAD_PROBE_OK;
            const bool split = !gst_rtp_buffer_get_marker(&rtp)
                && lightning::calls::vp9PayloadEndsFrame(
                       static_cast<const unsigned char *>(
                           gst_rtp_buffer_get_payload(&rtp)),
                       gst_rtp_buffer_get_payload_len(&rtp));
            gst_rtp_buffer_unmap(&rtp);
            if (!split)
                return GST_PAD_PROBE_OK;
            buffer = gst_buffer_make_writable(buffer);
            GST_PAD_PROBE_INFO_DATA(info) = buffer;
            if (gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &rtp)) {
                gst_rtp_buffer_set_marker(&rtp, TRUE);
                gst_rtp_buffer_unmap(&rtp);
            }
            return GST_PAD_PROBE_OK;
        }, nullptr, nullptr);
}

enum class CryptoDropCause { None, NoKey, Undecryptable };

struct CryptoProbeCtx {
    SfuMediaEngine *engine = nullptr;
    std::shared_ptr<SfuMediaEngine::ProbeGate> gate;
    std::shared_ptr<CallFrameCryptor> cryptor;
    QString streamId;
    bool encrypting = false;
    bool video = false;
    bool h264 = false;
    bool encryptedWhole = false;
    const std::atomic<bool> *required = nullptr;
    const std::atomic<bool> *keyReady = nullptr;
    quint32 ivStream = 0;
    std::atomic<quint64> *total = nullptr;
    std::atomic<quint64> *totalDropped = nullptr;
    std::atomic<quint64> *totalCiphertextShaped = nullptr;
    StreamInfoPtr streamInfo;
    quint64 passed = 0;
    quint64 dropped = 0;
    bool saidNoKey = false;
    quint8 saidFailedMask = 0;
    bool saidWorking = false;
    std::atomic<quint64> *totalServerInjected = nullptr;
    static constexpr int kFailureReasons = 7;
    std::array<quint64, kFailureReasons> failures{};
    std::array<quint8, kFailureReasons> detailsLogged{};
    quint64 ringEmpty = 0;
    quint64 run = 0;
    quint64 badIvRun = 0;
    quint64 longestBadIvRun = 0;
    std::array<bool, 256> badIvKeyIndexSeen{};
    int distinctBadIvKeyIndices = 0;
    quint64 sif = 0;
    bool awaitingKeyframe = false;
    qint64 lastKeyframeRequestMs = -1;
    quint64 sifRun = 0;
    quint64 sifBursts = 0;
    bool saidSif = false;
    qint64 lastHistogramMs = -1;
    bool histogramDirty = false;
    bool saidUnexpectedCiphertext = false;
    SfuMediaEngine::BlockedRunPolicy blocked;
    CryptoDropCause lastCause = CryptoDropCause::None;
    SfuMediaEngine::BlockedRunPolicy ciphertext;
    quint64 ciphertextShaped = 0;
};

static void noteCryptoOutcome(CryptoProbeCtx *ctx, bool failed)
{
    bool raise = false;
    if (!ctx->blocked.note(failed, &raise))
        return;
    if (ctx->encrypting || !ctx->engine)
        return;
    announceBlocked(ctx->engine, ctx->streamId,
                    !raise ? QString()
                           : ctx->lastCause == CryptoDropCause::NoKey
                               ? QStringLiteral("no_key")
                               : QStringLiteral("undecryptable"));
}

static void noteClearFrameShape(CryptoProbeCtx *ctx, GstBuffer *buffer,
                                CallFrameCryptor::FrameKind kind)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return;
    const bool shaped = CallFrameCryptor::looksEncrypted(
        reinterpret_cast<const char *>(map.data),
        static_cast<qsizetype>(map.size), kind);
    gst_buffer_unmap(buffer, &map);
    if (shaped) {
        ++ctx->ciphertextShaped;
        if (ctx->totalCiphertextShaped)
            ctx->totalCiphertextShaped->fetch_add(1);
    }

    bool raise = false;
    if (!ctx->ciphertext.note(shaped, &raise))
        return;
    if (!raise || ctx->saidUnexpectedCiphertext)
        return;
    ctx->saidUnexpectedCiphertext = true;
    qCWarning(lcSfuMedia)
        << "call diagnosis: this call is NOT encrypted for us and no media "
           "key is installed for stream="
        << ctx->streamId
        << "— but its frames carry the frame-crypto trailer, so the sender "
           "IS encrypting and what reaches the decoder is ciphertext. You "
           "will not hear or see them, and the two ends disagree about "
           "whether this call is encrypted (video="
        << ctx->video << "shaped=" << ctx->ciphertextShaped
        << "of" << ctx->passed << ")";
}

QString printableRing(const QString &ring)
{
    QString out = ring;
    out.replace(QChar(0x1f), QLatin1Char('/'));
    return out;
}

bool shouldReport(quint64 count)
{
    return count == 1 || count == 10 || count % 500 == 0;
}

struct RtpOutCtx {
    std::shared_ptr<std::atomic<quint64>> packets;
    bool video = false;
};

void rtpOutCtxFree(gpointer data)
{
    delete static_cast<RtpOutCtx *>(data);
}

GstPadProbeReturn countRtpOut(GstPad *, GstPadProbeInfo *info, gpointer user)
{
    auto *ctx = static_cast<RtpOutCtx *>(user);
    if (!ctx)
        return GST_PAD_PROBE_OK;
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;
    if (!ctx->packets)
        return GST_PAD_PROBE_OK;
    const quint64 n = ++(*ctx->packets);
    if (shouldReport(n)) {
        qCInfo(lcSfuMedia) << "rtp packets handed to webrtcbin video="
                           << ctx->video << "count=" << n;
    }
    return GST_PAD_PROBE_OK;
}

struct VideoSinkCtx {
    SfuMediaEngine *engine = nullptr;
    QString mid;
    QString streamId;
    quint64 unrouted = 0;
    std::shared_ptr<SfuMediaEngine::ProbeGate> gate;
};

void videoSinkCtxFree(void *data, GClosure *)
{
    delete static_cast<VideoSinkCtx *>(data);
}

GstFlowReturn onVideoSample(GstElement *sink, void *userData)
{
    auto *ctx = static_cast<VideoSinkCtx *>(userData);
    if (!ctx || !ctx->engine)
        return GST_FLOW_OK;
    std::unique_lock<std::mutex> gateLock;
    if (ctx->gate) {
        gateLock = std::unique_lock<std::mutex>(ctx->gate->mutex);
        if (!ctx->gate->open) {
            if (GstSample *dropped =
                    gst_app_sink_pull_sample(GST_APP_SINK(sink)))
                gst_sample_unref(dropped);
            return GST_FLOW_OK;
        }
    }

    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample)
        return GST_FLOW_OK;

    bool wanted = false;
    QString routeKey;
    if (SfuVideoRouter *router = ctx->engine->videoRouter()) {
        if (!ctx->mid.isEmpty() && router->watching(ctx->mid)) {
            routeKey = ctx->mid;
            wanted = true;
        } else if (!ctx->streamId.isEmpty()
                   && router->watching(ctx->streamId)) {
            routeKey = ctx->streamId;
            wanted = true;
        }
    }
    if (!wanted) {
        const quint64 unrouted = ++ctx->unrouted;
        if (shouldReport(unrouted)) {
            qCWarning(lcSfuMedia)
                << "video frames decrypted but NOT rendered: nothing is"
                << "watching trackKey=" << ctx->mid
                << "or stream=" << ctx->streamId << "count=" << unrouted;
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    int width = 0;
    int height = 0;
    if (caps) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        if (structure) {
            gst_structure_get_int(structure, "width", &width);
            gst_structure_get_int(structure, "height", &height);
        }
    }
    if (!buffer || width <= 0 || height <= 0 || width > 8192
        || height > 8192) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
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
            const qsizetype offset =
                static_cast<qsizetype>(row) * sourceStride;
            if (offset + sourceStride > available)
                break;
            memcpy(target + static_cast<qsizetype>(row) * targetStride,
                   map.data + offset, static_cast<size_t>(sourceStride));
        }
        frame.unmap();
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    SfuMediaEngine *engine = ctx->engine;
    const QString streamId = routeKey;
    SfuVideoRouter *router = engine->videoRouter();
    if (router && router->offerFrame(streamId, frame)) {
        marshal(engine, [engine, streamId] {
            if (SfuVideoRouter *r = engine->videoRouter())
                r->deliverPending(streamId);
        });
    }
    return GST_FLOW_OK;
}

static_assert(static_cast<int>(CallFrameCryptor::DecryptFailure::AuthTag)
                  == CryptoProbeCtx::kFailureReasons - 1,
              "CryptoProbeCtx's per-reason arrays must cover every "
              "DecryptFailure value");

qint64 probeMonotonicMs()
{
    return static_cast<qint64>(g_get_monotonic_time() / 1000);
}

void closeServerInjectedBurst(CryptoProbeCtx *ctx)
{
    if (ctx->sifRun == 0)
        return;
    ++ctx->sifBursts;
    if (ctx->sifBursts <= 20 || ctx->sifBursts % 100 == 0) {
        qCInfo(lcSfuMedia)
            << "server-injected burst stream=" << ctx->streamId
            << "video=" << ctx->video << "frames=" << ctx->sifRun
            << "burst=" << ctx->sifBursts
            << "(50 = LiveKit's publisher-mute burst, 10 = its track-close "
               "burst)";
    }
    ctx->sifRun = 0;
}

void logDecryptHistogram(CryptoProbeCtx *ctx, const char *when)
{
    using F = CallFrameCryptor::DecryptFailure;
    const auto n = [ctx](F reason) {
        return ctx->failures[static_cast<size_t>(reason)];
    };
    qCInfo(lcSfuMedia)
        << "decrypt histogram" << when << "stream=" << ctx->streamId
        << "video=" << ctx->video << "ok=" << ctx->passed
        << "badIv=" << n(F::BadIvLength) << "noKey=" << n(F::NoKeyForIndex)
        << "authTag=" << n(F::AuthTag)
        << "short=" << (n(F::ShortWire) + n(F::ShortBody))
        << "cipherInit=" << n(F::CipherInit) << "ringEmpty=" << ctx->ringEmpty
        << "sif=" << ctx->sif << "sifBursts=" << ctx->sifBursts
        << "distinctBadIvKeyIdx=" << std::min(ctx->distinctBadIvKeyIndices, 8)
        << "longestBadIvRun=" << ctx->longestBadIvRun;
}

void maybeLogDecryptHistogram(CryptoProbeCtx *ctx)
{
    if (ctx->encrypting || !ctx->histogramDirty)
        return;
    const qint64 now = probeMonotonicMs();
    if (ctx->lastHistogramMs >= 0 && now - ctx->lastHistogramMs < 5000)
        return;
    ctx->lastHistogramMs = now;
    ctx->histogramDirty = false;
    logDecryptHistogram(ctx, "periodic");
}

void cryptoProbeCtxFree(void *data)
{
    auto *ctx = static_cast<CryptoProbeCtx *>(data);
    if (ctx && !ctx->encrypting) {
        closeServerInjectedBurst(ctx);
        if (ctx->histogramDirty)
            logDecryptHistogram(ctx, "final");
    }
    delete ctx;
}

void noteServerInjectedFrame(CryptoProbeCtx *ctx, qsizetype size,
                             int trailerBytes)
{
    ++ctx->sif;
    ++ctx->sifRun;
    if (ctx->totalServerInjected)
        ctx->totalServerInjected->fetch_add(1);
    ctx->histogramDirty = true;
    if (!ctx->saidSif) {
        ctx->saidSif = true;
        qCInfo(lcSfuMedia)
            << "call diagnosis: stream=" << ctx->streamId
            << "is receiving SERVER-INJECTED frames — the SFU's own "
               "unencrypted blank frames, sent when that sender mutes, "
               "unpublishes or leaves. Dropped, and NOT a decryption "
               "failure (video="
            << ctx->video << "size=" << size << "trailerBytes="
            << trailerBytes << ")";
    }
    if (shouldReport(ctx->sif)) {
        qCInfo(lcSfuMedia) << "server-injected frames dropped stream="
                           << ctx->streamId << "video=" << ctx->video
                           << "count=" << ctx->sif;
    }
    maybeLogDecryptHistogram(ctx);
}

bool tailIsBase62(const QByteArray &wire, int n)
{
    if (wire.size() < n)
        return false;
    for (qsizetype i = wire.size() - n; i < wire.size(); ++i) {
        const char c = wire.at(i);
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z');
        if (!ok)
            return false;
    }
    return true;
}

// A received video frame we could not hand on breaks the decoder's reference
// chain, so the deltas after it are held until a keyframe. A keyframe is only
// asked for once a held frame decrypts: without the key it cannot help.
void loseVideoReference(CryptoProbeCtx *ctx)
{
    if (!ctx->encrypting && ctx->video)
        ctx->awaitingKeyframe = true;
}

// For a frame that is about to pass: false when it is a delta to hold.
bool releaseHeldVideo(CryptoProbeCtx *ctx, GstBuffer *buffer, GstPad *depayOut)
{
    if (ctx->encrypting || !ctx->video || !ctx->awaitingKeyframe)
        return true;
    if (!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
        ctx->awaitingKeyframe = false;
        return true;
    }
    const qint64 now = monotonicMs();
    if (ctx->lastKeyframeRequestMs < 0 || now - ctx->lastKeyframeRequestMs >= 500) {
        ctx->lastKeyframeRequestMs = now;
        if (depayOut)
            gst_pad_send_event(depayOut, gst_video_event_new_upstream_force_key_unit(
                                             GST_CLOCK_TIME_NONE, FALSE, 0));
    }
    return false;
}

void noteReceivedKeyframe(CryptoProbeCtx *ctx, GstBuffer *buffer)
{
    if (!ctx->encrypting && ctx->video && ctx->streamInfo
        && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
        ctx->streamInfo->keyframes.fetch_add(1);
}

GstPadProbeReturn cryptoProbe(GstPad *pad, GstPadProbeInfo *info,
                              void *userData)
{
    auto *ctx = static_cast<CryptoProbeCtx *>(userData);
    auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !ctx)
        return GST_PAD_PROBE_OK;
    std::unique_lock<std::mutex> gateLock;
    if (ctx->gate) {
        gateLock = std::unique_lock<std::mutex>(ctx->gate->mutex);
        if (!ctx->gate->open)
            return GST_PAD_PROBE_DROP;
    }
    const std::shared_ptr<CallFrameCryptor> cryptor = ctx->encrypting
        ? ctx->cryptor
        : (ctx->engine ? ctx->engine->recvCryptorFor(ctx->streamId)
                       : nullptr);
    if (!cryptor)
        return GST_PAD_PROBE_OK;

    CallFrameCryptor::FrameKind kind = CallFrameCryptor::FrameKind::Audio;
    if (ctx->video) {
        kind = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)
            ? CallFrameCryptor::FrameKind::VideoDelta
            : CallFrameCryptor::FrameKind::VideoKey;
        if (ctx->h264)
            kind = CallFrameCryptor::FrameKind::VideoH264;
        else if (ctx->encryptedWhole)
            kind = CallFrameCryptor::FrameKind::VideoWhole;
    }

    bool sifArmed = false;
    if (!ctx->encrypting && ctx->engine) {
        const QByteArray trailer = ctx->engine->serverInjectedTrailer();
        sifArmed = !trailer.isEmpty();
        if (sifArmed) {
            GstMapInfo sifMap;
            bool injected = false;
            qsizetype size = 0;
            if (gst_buffer_map(buffer, &sifMap, GST_MAP_READ)) {
                size = static_cast<qsizetype>(sifMap.size);
                injected = CallFrameCryptor::endsWithServerTrailer(
                    reinterpret_cast<const char *>(sifMap.data), size,
                    trailer);
                gst_buffer_unmap(buffer, &sifMap);
            }
            if (injected) {
                noteServerInjectedFrame(ctx, size, trailer.size());
                return GST_PAD_PROBE_DROP;
            }
        }
        closeServerInjectedBurst(ctx);
    }

    const bool haveKey = ctx->encrypting
        ? (ctx->keyReady && ctx->keyReady->load())
        : cryptor->hasAnyKey();
    if (!haveKey) {
        // No key yet. If encryption is required, drop: sending in the clear
        // would silently un-encrypt the call, and an undecryptable frame is
        // worse than none.
        const bool required = ctx->required && ctx->required->load();
        if (required) {
            ++ctx->dropped;
            if (ctx->streamInfo) ctx->streamInfo->dropped.fetch_add(1);
            if (ctx->totalDropped)
                ctx->totalDropped->fetch_add(1);
            ctx->lastCause = CryptoDropCause::NoKey;
            noteCryptoOutcome(ctx,true);
            if (!ctx->encrypting) {
                ++ctx->ringEmpty;
                ++ctx->run;
                ctx->histogramDirty = true;
                maybeLogDecryptHistogram(ctx);
            }
            if (!ctx->encrypting && !ctx->saidNoKey) {
                ctx->saidNoKey = true;
                qCWarning(lcSfuMedia)
                    << "call diagnosis: frames are arriving from stream="
                    << ctx->streamId
                    << "and being DROPPED because no media key has been "
                       "installed for it — either the sender's key never "
                       "reached this device, or the sender is not "
                       "encrypting and we require it (video="
                    << ctx->video << ")";
            }
            if (shouldReport(ctx->dropped)) {
                qCWarning(lcSfuMedia)
                    << "frames dropped: no key" << (ctx->encrypting ? "out" : "in")
                    << "stream=" << ctx->streamId
                    << "video=" << ctx->video << "count=" << ctx->dropped;
            }
            loseVideoReference(ctx);
            return GST_PAD_PROBE_DROP;
        }
        if (!releaseHeldVideo(ctx, buffer, pad))
            return GST_PAD_PROBE_DROP;
        ++ctx->passed;
        if (ctx->streamInfo) ctx->streamInfo->decrypted.fetch_add(1);
        noteReceivedKeyframe(ctx, buffer);
        if (ctx->total)
            ctx->total->fetch_add(1);
        noteCryptoOutcome(ctx,false);
        if (!ctx->encrypting)
            noteClearFrameShape(ctx, buffer, kind);
        if (shouldReport(ctx->passed)) {
            qCInfo(lcSfuMedia) << "frames in the clear"
                               << (ctx->encrypting ? "out" : "in")
                               << "stream=" << ctx->streamId
                               << "video=" << ctx->video
                               << "count=" << ctx->passed
                               << "ciphertextShaped=" << ctx->ciphertextShaped;
        }
        return GST_PAD_PROBE_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;
    const QByteArray input(reinterpret_cast<const char *>(map.data),
                           static_cast<int>(map.size));
    gst_buffer_unmap(buffer, &map);

    QByteArray output;
    CallFrameCryptor::DecryptDiagnosis why;
    if (ctx->encrypting) {
        // The per-SSRC counter inside the cryptor keeps the IV unique even
        // when two frames share a PTS; AES-GCM IV reuse is a total break.
        const quint32 timestamp =
            GST_BUFFER_PTS_IS_VALID(buffer)
                ? static_cast<quint32>(GST_BUFFER_PTS(buffer) / 1000)
                : 0;
        output = cryptor->encryptFrame(input, kind, ctx->ivStream,
                                       timestamp);
    } else {
        output = cryptor->decryptFrame(input, kind, &why);
    }

    if (output.isEmpty()) {
        // Encryption refused or authentication failed. No cleartext
        // fallback by design. On receive, a run of these means a key
        // mismatch and is otherwise indistinguishable from silence.
        ++ctx->dropped;
        if (ctx->streamInfo) ctx->streamInfo->dropped.fetch_add(1);
        if (ctx->totalDropped)
            ctx->totalDropped->fetch_add(1);
        ctx->lastCause = CryptoDropCause::Undecryptable;
        noteCryptoOutcome(ctx,true);
        const int reasonBit = static_cast<int>(why.reason);
        const bool reasonKnown =
            reasonBit >= 0 && reasonBit < CryptoProbeCtx::kFailureReasons;
        if (!ctx->encrypting && reasonKnown) {
            ++ctx->failures[static_cast<size_t>(reasonBit)];
            ++ctx->run;
            if (why.reason == CallFrameCryptor::DecryptFailure::BadIvLength) {
                ++ctx->badIvRun;
                ctx->longestBadIvRun =
                    std::max(ctx->longestBadIvRun, ctx->badIvRun);
                if (why.keyIndex >= 0 && why.keyIndex < 256
                    && !ctx->badIvKeyIndexSeen[static_cast<size_t>(
                        why.keyIndex)]) {
                    ctx->badIvKeyIndexSeen[static_cast<size_t>(why.keyIndex)] =
                        true;
                    ++ctx->distinctBadIvKeyIndices;
                }
            } else {
                ctx->badIvRun = 0;
            }
            ctx->histogramDirty = true;
            quint8 &logged = ctx->detailsLogged[static_cast<size_t>(reasonBit)];
            if (logged < 5
                || shouldReport(ctx->failures[static_cast<size_t>(reasonBit)])) {
                if (logged < 5)
                    ++logged;
                const qsizetype size = input.size();
                const int ivLenByte = size >= 2
                    ? static_cast<unsigned char>(input.at(size - 2)) : -1;
                const int keyIndexByte = size >= 1
                    ? static_cast<unsigned char>(input.at(size - 1)) : -1;
                const int hdr0 = size >= 1
                    ? static_cast<unsigned char>(input.at(0)) : -1;
                const qint64 ptsMs = GST_BUFFER_PTS_IS_VALID(buffer)
                    ? static_cast<qint64>(GST_BUFFER_PTS(buffer)
                                          / GST_MSECOND)
                    : -1;
                qCWarning(lcSfuMedia)
                    << "decrypt failed detail stream=" << ctx->streamId
                    << "video=" << ctx->video << "reason="
                    << CallFrameCryptor::decryptFailureName(why.reason)
                    << "size=" << size << "ivLenByte=" << ivLenByte
                    << "keyIndexByte=" << keyIndexByte << "hdr0="
                    << (hdr0 < 0 ? QStringLiteral("-")
                                 : QStringLiteral("0x%1").arg(hdr0, 2, 16,
                                                              QLatin1Char('0')))
                    << "silenceShape="
                    << CallFrameCryptor::startsWithOpusSilenceFrame(
                           input.constData(), size)
                    << "tailBase62=" << tailIsBase62(input, 16)
                    << "sifArmed=" << sifArmed << "run=" << ctx->run
                    << "ptsMs=" << ptsMs;
            }
            maybeLogDecryptHistogram(ctx);
        }
        if (!ctx->encrypting && reasonKnown
            && !(ctx->saidFailedMask & (1u << reasonBit))) {
            ctx->saidFailedMask =
                static_cast<quint8>(ctx->saidFailedMask | (1u << reasonBit));
            qCWarning(lcSfuMedia)
                << "call diagnosis: frames from stream=" << ctx->streamId
                << "will not DECRYPT: reason="
                << CallFrameCryptor::decryptFailureName(why.reason)
                << "keyIndex=" << why.keyIndex
                << "(video=" << ctx->video << ")";
        }
        if (shouldReport(ctx->dropped)) {
            qCWarning(lcSfuMedia)
                << (ctx->encrypting ? "encrypt failed" : "decrypt failed")
                << "stream=" << ctx->streamId
                << "video=" << ctx->video << "count=" << ctx->dropped
                << "passed=" << ctx->passed << "reason="
                << (ctx->encrypting
                        ? "n/a"
                        : CallFrameCryptor::decryptFailureName(why.reason));
        }
        loseVideoReference(ctx);
        return GST_PAD_PROBE_DROP;
    }
    if (!releaseHeldVideo(ctx, buffer, pad))
        return GST_PAD_PROBE_DROP;
    ++ctx->passed;
    if (ctx->streamInfo) ctx->streamInfo->decrypted.fetch_add(1);
    noteReceivedKeyframe(ctx, buffer);
    if (ctx->total)
        ctx->total->fetch_add(1);
    noteCryptoOutcome(ctx,false);
    if (!ctx->encrypting) {
        ctx->run = 0;
        ctx->badIvRun = 0;
        maybeLogDecryptHistogram(ctx);
    }
    if (!ctx->encrypting && !ctx->saidWorking) {
        ctx->saidWorking = true;
        qCInfo(lcSfuMedia)
            << "call diagnosis: frames from stream=" << ctx->streamId
            << "DECRYPT correctly (video=" << ctx->video << ")";
    }
    if (shouldReport(ctx->passed)) {
        qCInfo(lcSfuMedia) << (ctx->encrypting ? "frames encrypted"
                                               : "frames decrypted")
                           << "stream=" << ctx->streamId
                           << "video=" << ctx->video
                           << "count=" << ctx->passed
                           << "dropped=" << ctx->dropped;
    }

    GstBuffer *replacement = gst_buffer_new_allocate(
        nullptr, static_cast<gsize>(output.size()), nullptr);
    if (!replacement)
        return GST_PAD_PROBE_DROP;
    GstMapInfo out;
    if (!gst_buffer_map(replacement, &out, GST_MAP_WRITE)) {
        gst_buffer_unref(replacement);
        return GST_PAD_PROBE_DROP;
    }
    memcpy(out.data, output.constData(), static_cast<size_t>(output.size()));
    gst_buffer_unmap(replacement, &out);
    // Metas too: the microphone's GstAudioLevelMeta rides on this buffer
    // to the payloader, which writes it into the audio-level extension.
    gst_buffer_copy_into(replacement, buffer,
                         static_cast<GstBufferCopyFlags>(
                             GST_BUFFER_COPY_TIMESTAMPS
                             | GST_BUFFER_COPY_FLAGS
                             | GST_BUFFER_COPY_META),
                         0, static_cast<gsize>(-1));

    gst_buffer_unref(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = replacement;
    return GST_PAD_PROBE_OK;
}
}

bool SfuMediaEngine::runtimeAvailable(QString *whyNot)
{
    const bool initOk = lightning::gst::ensureInitialised(whyNot);
    if (!initOk)
        return false;

    lightning::rtp::registerVp8Payloader();
    lightning::rtp::registerH264Payloader();
    lightning::wincap::registerWindowCaptureSrc();
    calls::noise::registerDenoiseElement();
    static const char *const kRequired[] = {
        "webrtcbin",     "nicesrc",       "nicesink",     "dtlssrtpenc",
        "dtlssrtpdec",   "opusenc",       "opusdec",      "rtpopuspay",
        "rtpopusdepay",  "audioconvert",  "audioresample", "audiotestsrc",
        "fakesink",      "autoaudiosrc",  "autoaudiosink", "queue",
        "valve",         "volume",        "capsfilter",
        "vp8enc",        "vp8dec",        "rtpvp8pay",    "rtpvp8depay",
        "videoconvert",  "videoscale",    "videotestsrc", "videorate",
        "srtpenc",       "srtpdec",       "sctpenc",      "sctpdec",
        "rtpbin",        "rtpfunnel",
        lightning::rtp::vp8PayloaderName(),
    };
    for (const char *name : kRequired) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (!factory) {
            if (whyNot)
                *whyNot = QStringLiteral("missing_element:%1")
                              .arg(QLatin1String(name));
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

SfuMediaEngine::SfuMediaEngine(QObject *parent)
    : QObject(parent)
    , m_sendCryptor(std::make_unique<CallFrameCryptor>())
{
    m_shareKeepAliveTimer.setInterval(200);
    m_shareKeepAliveTimer.setSingleShot(false);
    connect(&m_shareKeepAliveTimer, &QTimer::timeout, this,
            &SfuMediaEngine::tickShareKeepAlive);
    m_outputChangeTimer.setSingleShot(true);
    connect(&m_outputChangeTimer, &QTimer::timeout, this,
            &SfuMediaEngine::rearmReceiveChains);
    QMutexLocker lock(&g_aliveMutex);
    g_aliveEngines.insert(this);
}

SfuMediaEngine::~SfuMediaEngine()
{
    {
        QMutexLocker lock(&g_aliveMutex);
        g_aliveEngines.remove(this);
    }
    // stop() waits (bounded) for deferred publish teardowns even when the
    // session is already stopped; otherwise an unparented bin could push
    // buffers through probes pointing into members being destroyed.
    stop();
    m_retirer.drain(lightning::webrtc::Retirer::kQuitBudgetMs);
}

void SfuMediaEngine::start()
{
    teardown(false);
    m_callAllocator.engage();
    refreshSpeakerSink();
    m_generation.fetch_add(1);
    m_active = true;
    if (shareHardwareEncodeRequested())
        probeShareHardwareEncoder();
    m_framesEncrypted.store(0);
    m_framesDecrypted.store(0);
    m_framesDropped.store(0);
    m_framesClearButCiphertextShaped.store(0);
    m_framesServerInjected.store(0);
    m_micClockHold.reset();
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    m_publisherEverPublished = false;
    Q_EMIT connectionStateChanged(QStringLiteral("connecting"));
}

void SfuMediaEngine::stop()
{
    teardown(true);
    m_callAllocator.release();
}

void SfuMediaEngine::suspend()
{
    teardown(false);
}

void SfuMediaEngine::teardown(bool endOfCall)
{
    if (endOfCall)
        clearKeys();
    else
        clearSessionRouting();
    // Wait (bounded) for deferred publish teardowns before destroying
    // anything: their unparented bins are unreachable from destroyPeer() and
    // their probes point at this object. A single atomic load when idle.
    awaitPublishTeardowns();
    if (!m_active && !m_publisher.pipeline && !m_subscriber.pipeline)
        return;
    m_generation.fetch_add(1);
    m_sessionLive->store(false);
    m_sessionLive = std::make_shared<std::atomic<bool>>(true);
    m_active = false;
    for (const QString &cid : m_asyncStarts.keys())
        detachPendingStart(cid, m_publishedBins.value(cid, nullptr),
true);
    m_asyncStarts.clear();
    m_publishedBins.clear();
    m_shareBackups.clear();
    m_shareKeepAliveTimer.stop();
    for (auto it = m_publishWatch.cbegin(); it != m_publishWatch.cend(); ++it)
        releaseKeepAlive(it->state);
    m_publishWatch.clear();
    m_shareEncoderCid.clear();
    m_shareEncoderInUse.clear();
    m_shareGlEncode = false;
    m_pendingTrackVolume.clear();
    m_volumeMissWarned.clear();
    m_volumeAppliedLog.clear();
    for (auto it = m_publishedFds.cbegin(); it != m_publishedFds.cend(); ++it) {
        if (it.value() > 0)
            ::close(it.value());
    }
    m_publishedFds.clear();
    m_statsTimer.stop();
    m_lastRtpBytes.clear();
    m_lastRtpLost.clear();
    m_streamInfoRtp.clear();
    m_lastJitterbuffer.clear();
    {
        QMutexLocker lock(&m_recvHealthMutex);
        m_recvHealth.clear();
    }
    resetShareAudioState();
    destroyPeer(m_publisher);
    destroyPeer(m_subscriber);
    {
        QMutexLocker lock(&m_receiveBinMutex);
        m_receiveBins.clear();
    }
    m_playbackFailureAnnounced = false;
    m_outputChangeTimer.stop();
    m_outputChangeBurstStartMs = -1;
    m_receiveRebuildAttempts = 0;
    m_receiveRearms = 0;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        m_freshPulseClient.clear();
    }
    forgetMicrophoneSwap();
    m_micStartPending.reset();
    m_micSwapDeferred.reset();
    m_micRestartAfterStart = false;
    m_micNoiseModeAfterStart = false;
    m_micNoiseDspForceAfterStart = false;
    m_micStartTakenOver = false;
    m_micEchoAfterStart = false;
    m_micDspRescueAfterStart = false;
    m_echoGivenUp = false;
    m_micDspFailureReported = false;
    m_micSwapReplacesStart = false;
    m_micRtpVerdictOwed = false;
    m_micLiveFront.clear();
    m_micCid.clear();
    m_micRestarts = 0;
    m_micRestartPending = false;
    m_micFailureReported = false;
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    m_publisherEverPublished = false;
    if (endOfCall)
        clearKeys();
    else
        clearSessionRouting();
    {
        QMutexLocker lock(&m_diagnosedMutex);
        m_diagnosedOnce.clear();
        m_keyArrivals.clear();
    }
    Q_EMIT connectionStateChanged(QStringLiteral("closed"));
}

namespace {
GstBusSyncReply onBusMessage(GstBus *, GstMessage *message, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const GstMessageType type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_ELEMENT && engine) {
        const GstStructure *fields = gst_message_get_structure(message);
        const gchar *srcName = GST_MESSAGE_SRC_NAME(message);
        if (fields && srcName
            && g_strcmp0(srcName, calls::noise::kDenoiseName) == 0
            && gst_structure_has_name(fields,
                                      calls::noise::kStatusMessageName)) {
            const QString requested = QString::fromUtf8(
                gst_structure_get_string(fields, "requested"));
            const QString active =
                QString::fromUtf8(gst_structure_get_string(fields, "active"));
            gboolean ok = FALSE;
            gint latency = 0;
            gst_structure_get_boolean(fields, "ok", &ok);
            gst_structure_get_int(fields, "latency-samples", &latency);
            marshal(engine, [engine, requested, active, ok, latency] {
                engine->handleDenoiseStatus(requested, active, ok, latency);
            });
        } else if (fields && srcName
                   && (g_strcmp0(srcName, "miccapturelevel") == 0
                       || g_strcmp0(srcName, "miclevel") == 0)
                   && gst_structure_has_name(fields, "level")) {
            const bool captured = g_strcmp0(srcName, "miccapturelevel") == 0;
            double peak = -350.0;
            if (SfuMediaEngine::readLevelPeak(fields, &peak)) {
                if (captured) {
                    marshal(engine,
                            [engine, peak] { engine->handleMicLevel(peak); });
                } else {
                    marshal(engine,
                            [engine, peak] { engine->handleMeterLevel(peak); });
                }
            }
        } else if (fields && srcName
                   && g_strcmp0(srcName, "sharelevel") == 0
                   && gst_structure_has_name(fields, "level")) {
            double peak = -350.0;
            if (SfuMediaEngine::readLevelPeak(fields, &peak)) {
                marshal(engine,
                        [engine, peak] { engine->handleShareAudioLevel(peak); });
            }
        }
        gst_message_unref(message);
        return GST_BUS_DROP;
    }
    if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_WARNING) {
        gst_message_unref(message);
        return GST_BUS_DROP;
    }

    GError *error = nullptr;
    gchar *debug = nullptr;
    if (type == GST_MESSAGE_ERROR)
        gst_message_parse_error(message, &error, &debug);
    else
        gst_message_parse_warning(message, &error, &debug);
    const gchar *rawName = GST_MESSAGE_SRC_NAME(message);
    const QString element = QString::fromUtf8(rawName ? rawName : "?");
    const QString reason =
        QString::fromUtf8(error && error->message ? error->message : "?");
    if (engine && element.startsWith(QLatin1String("sharesrc"))
        && (type == GST_MESSAGE_ERROR
            || (error && error->domain == GST_RESOURCE_ERROR))) {
        marshal(engine, [engine] { engine->handleShareAudioSourceError(); });
    }
    if (engine && type == GST_MESSAGE_ERROR) {
        const int branch =
            lightning::shareaudio::branchIndexFromElementName(element);
        if (branch >= 0) {
            marshal(engine, [engine, branch] {
                engine->handleShareAudioBranchError(branch);
            });
        }
    }
    bool hardwareEncoderFailed = false;
    if (type == GST_MESSAGE_ERROR) {
        qCWarning(lcSfuMedia) << "pipeline error element=" << element
                              << "code=" << (error ? error->code : 0)
                              << "reason=" << reason;
        GstElementFactory *failedFactory =
            element == QLatin1String("videoenc") && GST_IS_ELEMENT(GST_MESSAGE_SRC(message))
            ? gst_element_get_factory(GST_ELEMENT(GST_MESSAGE_SRC(message)))
            : nullptr;
        hardwareEncoderFailed = failedFactory
            && g_strcmp0(gst_plugin_feature_get_name(
                             GST_PLUGIN_FEATURE(failedFactory)),
                         "vp8enc") != 0;
        // rtpvp8pay parses the VP8 bitstream to build its payload descriptor,
        // so fed an encrypted frame it posts "Failed to parse VP8 frame" and
        // the share stops after one frame. libwebrtc's packetizer takes that
        // data from the encoder as metadata; see RtpVp8Payloader.
        if (element.startsWith(QLatin1String("rtpvp8pay"))) {
            qCWarning(lcSfuMedia)
                << "encrypted VP8 cannot be payloaded by rtpvp8pay: it parses"
                << "the bitstream. Video send is not carried in an encrypted"
                << "room until a non-parsing payloader lands.";
        }

        const bool audioSinkError =
            element.contains(QLatin1String("audiosink")) && error
            && ((error->domain == GST_STREAM_ERROR
                 && error->code == GST_STREAM_ERROR_FORMAT)
                || (error->domain == GST_CORE_ERROR
                    && error->code == GST_CORE_ERROR_NEGOTIATION)
                || (error->domain == GST_RESOURCE_ERROR
                    && (error->code == GST_RESOURCE_ERROR_OPEN_WRITE
                        || error->code
                               == GST_RESOURCE_ERROR_OPEN_READ_WRITE)));
        if (audioSinkError) {
            qCWarning(lcSfuMedia)
                << "THE PLAYBACK DEVICE REFUSED THE CALL'S AUDIO: nothing "
                   "from this call can be heard until a working output is "
                   "selected. A format error here usually means the chosen "
                   "output sits on a profile — surround, or an IEC958 "
                   "passthrough — that cannot take a plain stereo stream.";
        }
    } else if (error && error->domain == GST_RESOURCE_ERROR
               && (element.startsWith(QLatin1String("micsrc"))
                   || element.startsWith(QLatin1String("capsrc"))
                   || element.startsWith(QLatin1String("sharesrc"))
                   || element.startsWith(QLatin1String("shareapp")))) {
        qCWarning(lcSfuMedia)
            << "capture source could not open its device: element=" << element
            << "code=" << error->code << "reason=" << reason
            << "— this track will carry nothing";
    } else {
        qCInfo(lcSfuMedia) << "pipeline warning element=" << element
                           << "code=" << (error ? error->code : 0)
                           << "reason=" << reason;
    }
    const bool captureResourceError =
        type == GST_MESSAGE_ERROR && error
        && error->domain == GST_RESOURCE_ERROR
        && element.startsWith(QLatin1String("capsrc"))
        && SfuMediaEngine::isDirectCameraSource(GST_MESSAGE_SRC(message));
    const bool captureBusyError = captureResourceError
        && (error->code == GST_RESOURCE_ERROR_BUSY
            || reason.contains(QLatin1String("busy"), Qt::CaseInsensitive));
    const bool captureFormatError =
        type == GST_MESSAGE_ERROR && error
        && element.startsWith(QLatin1String("capsrc"))
        && SfuMediaEngine::isCaptureFormatError(
            error->domain, error->code, reason,
            QString::fromUtf8(debug ? debug : ""));
    if (error)
        g_error_free(error);
    if (debug)
        g_free(debug);
    if (type == GST_MESSAGE_ERROR && engine) {
        QString publishCid;
        int captureEpoch = 0;
        GstObject *walk = GST_MESSAGE_SRC(message);
        if (walk)
            gst_object_ref(walk);
        while (walk) {
            GstObject *parent = gst_object_get_parent(walk);
            if (!parent) {
                gst_object_unref(walk);
                break;
            }
            if (GST_IS_PIPELINE(parent)) {
                gchar *name = gst_object_get_name(walk);
                publishCid = QString::fromUtf8(name ? name : "");
                g_free(name);
                captureEpoch = captureRetryStamp(G_OBJECT(walk));
                if (GST_IS_ELEMENT(walk))
                    engine->isolateFailedReceiveBin(GST_ELEMENT(walk));
                gst_object_unref(parent);
                gst_object_unref(walk);
                break;
            }
            gst_object_unref(walk);
            walk = parent;
        }
        if (hardwareEncoderFailed && !publishCid.isEmpty()) {
            marshal(engine, [engine, publishCid] {
                engine->handleShareEncoderFailed(publishCid);
            });
        }
        if (!publishCid.isEmpty()) {
            const bool captureSource =
                element.startsWith(QLatin1String("micsrc"));
            QString front;
            if (captureSource) {
                GstObject *up = GST_MESSAGE_SRC(message)
                    ? gst_object_get_parent(GST_MESSAGE_SRC(message))
                    : nullptr;
                while (up && front.isEmpty()) {
                    gchar *name = gst_object_get_name(up);
                    if (name && g_str_has_prefix(name, "micfront"))
                        front = QString::fromUtf8(name);
                    g_free(name);
                    GstObject *next = gst_object_get_parent(up);
                    gst_object_unref(up);
                    up = next;
                }
                if (up)
                    gst_object_unref(up);
            }
            if (element == QLatin1String(calls::noise::kDspName)) {
                marshal(engine, [engine, publishCid, reason] {
                    engine->handleMicDspError(publishCid, reason);
                });
            } else {
                marshal(engine, [engine, publishCid, captureSource, front,
                                 captureResourceError, captureBusyError,
                                 captureFormatError, captureEpoch] {
                    if (captureSource)
                        engine->handleCaptureError(publishCid, front);
                    engine->handlePublishError(publishCid,
                                               captureResourceError,
                                               captureBusyError,
                                               captureFormatError,
                                               captureEpoch);
                });
            }
        }
    }
    Q_UNUSED(engine);
    gst_message_unref(message);
    return GST_BUS_DROP;
}
}

void SfuMediaEngine::destroyPeer(Peer &peer)
{
    if (peer.webrtc)
        g_signal_handlers_disconnect_by_data(peer.webrtc, this);
    if (peer.pipeline) {
        GstBus *bus = gst_element_get_bus(peer.pipeline);
        if (bus) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }
    }
    // Never a plain set_state(NULL) (GitHub #3): the pipeline, with every bin
    // and probe of ours, stops here; its webrtcbin once ICE gathering has
    // ended. Takes both references.
    m_retirer.retire(peer.pipeline, peer.webrtc);
    peer = Peer();
}

bool SfuMediaEngine::ensurePeer(Target target)
{
    Peer &peer = peerFor(target);
    if (peer.webrtc)
        return true;
    if (!m_active)
        return false;

    GstElement *pipeline = gst_pipeline_new(nullptr);
    if (pipeline) {
        if (GstBus *bus = gst_element_get_bus(pipeline)) {
            gst_bus_set_sync_handler(bus, onBusMessage, this, nullptr);
            gst_object_unref(bus);
        }
        if (GstClock *clock = gst_system_clock_obtain()) {
            gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
            gst_object_unref(clock);
        }
    }
    GstElement *webrtc = gst_element_factory_make(
        "webrtcbin",
        target == Target::Publisher ? "wb-pub" : "wb-sub");
    if (!pipeline || !webrtc) {
        if (pipeline)
            gst_object_unref(pipeline);
        if (webrtc)
            gst_object_unref(webrtc);
        Q_EMIT failed(QStringLiteral("pipeline_failed"));
        return false;
    }
    g_object_set(webrtc, "bundle-policy", 3, "latency", 100,
                 nullptr);
    if (target == Target::Subscriber) {
        g_signal_connect(webrtc, "on-new-transceiver", G_CALLBACK(+[](
            GstElement *, GstWebRTCRTPTransceiver *transceiver, gpointer) {
            GstWebRTCKind kind = GST_WEBRTC_KIND_UNKNOWN;
            g_object_get(transceiver, "kind", &kind, nullptr);
            if (kind != GST_WEBRTC_KIND_VIDEO)
                return;
            enableVideoRetransmission(transceiver);
            GstCaps *caps = gst_caps_from_string(
                receivableVideoCaps().toUtf8().constData());
            g_object_set(transceiver, "codec-preferences", caps, nullptr);
            gst_caps_unref(caps);
        }), nullptr);
        // Pinned OFF: a source must never be removed on timeout. A muted
        // LiveKit track sends nothing, so rtpbin times it out ~40 s into the
        // mute; with autoremove the source goes, webrtcbin keeps its src pad
        // (it removes pads only on release), and on unmute the returning
        // SSRC's new payload pad is linked to nothing. Its not-linked flow
        // return stops the bundled nicesrc, and every received track in the
        // call goes silent for good (measured live against Element, 1.26).
        // The cost is two threads per remote rejoin that are never reclaimed;
        // reclaiming them needs a removal tied to a signalled leave, not to a
        // timeout.
        if (GstElement *rtpbin =
                gst_bin_get_by_name(GST_BIN(webrtc), "rtpbin")) {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(rtpbin),
                                             "autoremove"))
                g_object_set(rtpbin, "autoremove", FALSE, nullptr);
            gst_object_unref(rtpbin);
        }
    }
    if (!gst_bin_add(GST_BIN(pipeline), webrtc)) {
        gst_object_unref(pipeline);
        Q_EMIT failed(QStringLiteral("pipeline_failed"));
        return false;
    }
    peer.pipeline = pipeline;
    peer.webrtc = GST_ELEMENT(gst_object_ref(webrtc));
    // Before any callback can run; see destroyPeer().
    lightning::webrtc::installGate(webrtc);

    if (target == Target::Publisher) {
        g_signal_connect(webrtc, "on-negotiation-needed",
                         G_CALLBACK(onNegotiationNeeded), this);
    }
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate),
                     this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(onPadAdded), this);
    g_signal_connect(webrtc, "pad-removed", G_CALLBACK(onPadRemoved), this);
    armStatsTrace();
    g_signal_connect(webrtc, "notify::ice-connection-state",
                     G_CALLBACK(onPeerStateNotify), this);
    g_signal_connect(webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(onPeerStateNotify), this);
    g_signal_connect(webrtc, "notify::connection-state",
                     G_CALLBACK(onPeerStateNotify), this);

    applyIceTo(peer);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return true;
}

QString SfuMediaEngine::stunServerUri(const QString &iceUri)
{
    if (!iceUri.startsWith(QLatin1String("stun:")) || iceUri.size() <= 5)
        return {};
    return QStringLiteral("stun://") + iceUri.mid(5);
}

QString SfuMediaEngine::turnServerUri(const QString &iceUri,
                                      const QString &username,
                                      const QString &password)
{
    const bool secure = iceUri.startsWith(QLatin1String("turns:"));
    if (!secure && !iceUri.startsWith(QLatin1String("turn:")))
        return {};
    const QString rest = iceUri.mid(secure ? 6 : 5);
    if (rest.isEmpty())
        return {};
    return QStringLiteral("%1://%2:%3@%4")
        .arg(secure ? QStringLiteral("turns") : QStringLiteral("turn"),
             QString::fromUtf8(QUrl::toPercentEncoding(username)),
             QString::fromUtf8(QUrl::toPercentEncoding(password)), rest);
}

void SfuMediaEngine::applyIceTo(Peer &peer)
{
    if (!peer.webrtc || m_iceUris.isEmpty())
        return;
    bool stunApplied = false;
    int turnServers = 0;
    for (const QString &uri : m_iceUris) {
        const QString stun = stunServerUri(uri);
        if (!stun.isEmpty()) {
            if (!stunApplied) {
                g_object_set(peer.webrtc, "stun-server",
                             stun.toUtf8().constData(), nullptr);
                stunApplied = true;
            }
            continue;
        }
        const QString turn = turnServerUri(uri, m_iceUsername, m_icePassword);
        if (turn.isEmpty())
            continue;
        gboolean added = FALSE;
        g_signal_emit_by_name(peer.webrtc, "add-turn-server",
                              turn.toUtf8().constData(), &added);
        if (added)
            ++turnServers;
    }
    qCInfo(lcSfuMedia) << "ice servers applied stun=" << stunApplied
                       << "turn=" << turnServers;
}

void SfuMediaEngine::setIceServers(const QVariantList &servers)
{
    m_iceUris.clear();
    m_iceUsername.clear();
    m_icePassword.clear();
    for (const QVariant &value : servers) {
        const QVariantMap entry = value.toMap();
        const QStringList uris =
            entry.value(QStringLiteral("urls")).toStringList();
        for (const QString &uri : uris) {
            if (uri.length() > 512 || uri.contains(QLatin1Char('@'))
                || uri.contains(QLatin1Char('/')))
                continue;
            m_iceUris.append(uri);
        }
        if (m_iceUsername.isEmpty()) {
            m_iceUsername =
                entry.value(QStringLiteral("username")).toString();
            m_icePassword =
                entry.value(QStringLiteral("credential")).toString();
        }
    }
    applyIceTo(m_publisher);
    applyIceTo(m_subscriber);
}

static QStringList ourAudioClientNames()
{
    return SfuMediaEngine::ownAudioClientNames();
}

QStringList SfuMediaEngine::ownAudioClientNames()
{
    QStringList names{ QCoreApplication::applicationName() };
    const QString binary =
        QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    if (!binary.isEmpty() && !names.contains(binary, Qt::CaseInsensitive))
        names << binary;
    return names;
}

bool SfuMediaEngine::shareAudioAvailable()
{
    return lightning::shareaudio::systemCapture(
               QCoreApplication::applicationPid()).excludesUs;
}

bool SfuMediaEngine::shareAudioSystemExcludesUs()
{
    return lightning::shareaudio::systemCapture(
               QCoreApplication::applicationPid()).excludesUs;
}

namespace {

QList<lightning::calls::GstDeviceCandidate> enumerateDevices(const char *klass)
{
    QList<lightning::calls::GstDeviceCandidate> out;
    if (!lightning::gst::ensureInitialised())
        return out;
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    if (!monitor)
        return out;
    gst_device_monitor_add_filter(monitor, klass, nullptr);
    if (!gst_device_monitor_start(monitor)) {
        gst_object_unref(monitor);
        return out;
    }
    GList *devices = gst_device_monitor_get_devices(monitor);
    for (GList *item = devices; item; item = item->next) {
        auto *device = static_cast<GstDevice *>(item->data);
        lightning::calls::GstDeviceCandidate candidate;
        if (gchar *name = gst_device_get_display_name(device)) {
            candidate.displayName = QString::fromUtf8(name);
            g_free(name);
        }
        if (GstStructure *props = gst_device_get_properties(device)) {
            const int fields = gst_structure_n_fields(props);
            for (int i = 0; i < fields; ++i) {
                const gchar *key = gst_structure_nth_field_name(props, i);
                if (!key)
                    continue;
                const GValue *value = gst_structure_get_value(props, key);
                if (!value)
                    continue;
                gchar *text = gst_value_serialize(value);
                if (text) {
                    candidate.properties.insert(QString::fromUtf8(key),
                                                QString::fromUtf8(text));
                    g_free(text);
                }
            }
            gst_structure_free(props);
        }
        GObjectClass *deviceClass = G_OBJECT_GET_CLASS(device);
        if (!candidate.properties.contains(QStringLiteral("device.path"))) {
            if (GParamSpec *spec =
                    g_object_class_find_property(deviceClass, "path");
                spec && spec->value_type == G_TYPE_STRING) {
                gchar *path = nullptr;
                g_object_get(device, "path", &path, nullptr);
                if (path && *path) {
                    candidate.properties.insert(QStringLiteral("device.path"),
                                                QString::fromUtf8(path));
                }
                g_free(path);
            }
        }
        if (!candidate.properties.contains(QStringLiteral("device.index"))) {
            if (GParamSpec *spec =
                    g_object_class_find_property(deviceClass, "device-index");
                spec && spec->value_type == G_TYPE_INT) {
                gint index = -1;
                g_object_get(device, "device-index", &index, nullptr);
                if (index >= 0) {
                    candidate.properties.insert(QStringLiteral("device.index"),
                                                QString::number(index));
                }
            }
        }
        out.append(candidate);
        gst_object_unref(device);
    }
    g_list_free(devices);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return out;
}

constexpr int kDeviceEnumerationBudgetMs = 2500;
constexpr qint64 kSlowCaptureStartWarnMs = 500;

QList<lightning::calls::GstDeviceCandidate> monitorCandidates(
    const char *klass, bool *answered = nullptr)
{
    if (answered)
        *answered = false;
    static QMutex latchMutex;
    static QSet<QString> hung;
    const QString key = QString::fromLatin1(klass);
    {
        QMutexLocker lock(&latchMutex);
        if (hung.contains(key))
            return {};
    }

    if (!lightning::gst::ensureInitialised())
        return {};

    auto slot = std::make_shared<
        std::promise<QList<lightning::calls::GstDeviceCandidate>>>();
    auto answer = slot->get_future();
    std::thread([slot, klass] {
        try {
            slot->set_value(enumerateDevices(klass));
        } catch (...) {
            try {
                slot->set_value({});
            } catch (...) {
            }
        }
    }).detach();

    if (answer.wait_for(std::chrono::milliseconds(kDeviceEnumerationBudgetMs))
        == std::future_status::ready) {
        if (answered)
            *answered = true;
        return answer.get();
    }

    {
        QMutexLocker lock(&latchMutex);
        hung.insert(key);
    }
    qCWarning(lcSfuMedia)
        << "device enumeration did not answer within"
        << kDeviceEnumerationBudgetMs << "ms for" << klass
        << "- continuing on the platform default device and not asking again "
           "this session";
    return {};
}

QStringList microphoneElementPreference()
{
#if defined(Q_OS_WIN)
    return {QStringLiteral("wasapi2src"), QStringLiteral("wasapisrc")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("osxaudiosrc")};
#else
    // `pulsesrc` first: the gst-plugin-pipewire 1.4 bundled in the AppImage
    // stalls in `connecting` when `target-object` is set against a 1.6 daemon,
    // so choosing a microphone silenced it. pipewire-pulse also exposes the
    // same node names QMediaDevices reports, so no id translation is needed.
    // `pipewiresrc` remains for hosts without pipewire-pulse.
    return {QStringLiteral("pulsesrc"), QStringLiteral("pipewiresrc")};
#endif
}

QStringList speakerElementPreference()
{
#if defined(Q_OS_WIN)
    return {QStringLiteral("wasapi2sink"), QStringLiteral("wasapisink")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("osxaudiosink")};
#else
    return {QStringLiteral("pulsesink"), QStringLiteral("pipewiresink")};
#endif
}

QStringList availableElements(const QStringList &names)
{
    QStringList out;
    if (!lightning::gst::ensureInitialised())
        return out;
    for (const QString &name : names) {
        GstElementFactory *factory =
            gst_element_factory_find(name.toUtf8().constData());
        if (!factory)
            continue;
        gst_object_unref(factory);
        out << name;
    }
    return out;
}

bool applyBindingTo(GstElement *bin, const char *elementName,
                    const lightning::calls::DeviceBinding &binding)
{
    if (binding.isEmpty())
        return true;
    GstElement *element = gst_bin_get_by_name(GST_BIN(bin), elementName);
    if (!element)
        return false;
    const QByteArray property = binding.property.toUtf8();
    const QByteArray value = binding.value.toUtf8();
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element),
                                                    property.constData());
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
            set = gst_value_deserialize(&typed, value.constData())
                && !g_param_value_validate(spec, &typed);
            if (set)
                g_object_set_property(G_OBJECT(element), property.constData(),
                                      &typed);
            g_value_unset(&typed);
        }
    }
    if (set) {
        qCInfo(lcSfuMedia) << "capture device bound element=" << elementName
                           << "property=" << binding.property
                           << "value=" << binding.value
                           << "matched-by=" << binding.reason;
    } else {
        qCWarning(lcSfuMedia)
            << "capture element" << elementName << "did not accept"
            << binding.property << "=" << binding.value;
    }
    gst_object_unref(element);
    return set;
}

bool factoryExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

}

SfuMediaEngine::MicrophoneCapture
SfuMediaEngine::resolveMicrophoneCapture(const DeviceChoice &mic)
{
    const lightning::calls::ElementChoice choice =
        mic.id.isEmpty()
            ? lightning::calls::ElementChoice{}
            : lightning::calls::chooseCaptureElement(
                  lightning::calls::CaptureKind::Microphone,
                  microphoneElementPreference(),
                  availableElements(microphoneElementPreference()),
                  mic.id, mic.description,
                  monitorCandidates("Audio/Source"));
    MicrophoneCapture capture;
    capture.source = choice.isEmpty()
        ? QStringLiteral("autoaudiosrc name=micsrc")
        : QStringLiteral("%1 name=micsrc").arg(choice.element);
    capture.binding = choice.binding;
    capture.channels = choice.element == QLatin1String("pipewiresrc")
        ? choice.binding.channels
        : 0;
    return capture;
}

QString SfuMediaEngine::microphoneFrontDescription(
    const MicrophoneCapture &capture)
{
    const QString channelCaps =
        lightning::calls::captureChannelCaps(capture.channels);
    const QString mixMatrix = lightning::calls::captureMixMatrix(capture.channels);
    return QStringLiteral("%1 %2 "
                          // Bounded and leaky: a default queue holds up to
                          // one second and never drains, which becomes
                          // permanent latency. Dropping old audio beats
                          // delaying all of it.
                          "! queue max-size-buffers=0 max-size-bytes=0 "
                          "max-size-time=100000000 leaky=downstream "
                          "! audioconvert %3 ! audioresample "
                          "! audio/x-raw,channels=1")
        .arg(capture.source, channelCaps, mixMatrix);
}

bool SfuMediaEngine::levelElementAvailable()
{
    static const bool available = factoryExists("level");
    return available;
}

bool SfuMediaEngine::echoProbeElementAvailable()
{
    static const bool available = factoryExists("webrtcechoprobe");
    return available;
}

bool SfuMediaEngine::applyCaptureBinding(
    GstElement *bin, const char *elementName,
    const lightning::calls::DeviceBinding &binding)
{
    return applyBindingTo(bin, elementName, binding);
}

SfuMediaEngine::OutputSink
SfuMediaEngine::resolveSpeakerSink(const DeviceChoice &speaker)
{
    const lightning::calls::ElementChoice choice =
        speaker.id.isEmpty()
            ? lightning::calls::ElementChoice{}
            : lightning::calls::chooseCaptureElement(
                  lightning::calls::CaptureKind::Speaker,
                  speakerElementPreference(),
                  availableElements(speakerElementPreference()),
                  speaker.id, speaker.description,
                  monitorCandidates("Audio/Sink"));
    OutputSink out;
    out.sink = choice.isEmpty()
        ? QStringLiteral("autoaudiosink name=outsink")
        : QStringLiteral("%1 name=outsink").arg(choice.element);
    out.binding = choice.binding;
    return out;
}

bool SfuMediaEngine::readLevelPeak(const GstStructure *fields, double *peakDb)
{
    // -350 is a real reading: `level`'s floor for digital silence. So "could
    // not read" is the return value, not a sentinel, or a dead microphone
    // could never raise the silence warning.
    double peak = -350.0;
    bool readable = false;
    const GValue *peaks = fields ? gst_structure_get_value(fields, "peak")
                                 : nullptr;
    if (peaks) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (G_VALUE_HOLDS(peaks, G_TYPE_VALUE_ARRAY)) {
            auto *array = static_cast<GValueArray *>(g_value_get_boxed(peaks));
            for (guint i = 0; array && i < array->n_values; ++i) {
                const GValue *one = g_value_array_get_nth(array, i);
                if (one && G_VALUE_HOLDS_DOUBLE(one)) {
                    peak = qMax(peak, g_value_get_double(one));
                    readable = true;
                }
            }
        } else if (GST_VALUE_HOLDS_ARRAY(peaks)) {
            const guint n = gst_value_array_get_size(peaks);
            for (guint i = 0; i < n; ++i) {
                const GValue *one = gst_value_array_get_value(peaks, i);
                if (one && G_VALUE_HOLDS_DOUBLE(one)) {
                    peak = qMax(peak, g_value_get_double(one));
                    readable = true;
                }
            }
        } else {
            static std::atomic<bool> saidOnce{false};
            if (!saidOnce.exchange(true)) {
                qCWarning(lcSfuMedia)
                    << "the capture level meter posted a peak this build "
                       "cannot read: type="
                    << G_VALUE_TYPE_NAME(peaks)
                    << "- the microphone level and the silence warning are "
                       "BOTH unavailable in this build";
            }
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
    if (readable && peakDb)
        *peakDb = peak;
    return readable;
}

QString SfuMediaEngine::applyDevicePropertyForTest(const QString &factory,
                                                  const QString &property,
                                                  const QString &value)
{
    if (!lightning::gst::ensureInitialised())
        return {};
    GstElement *element =
        gst_element_factory_make(factory.toUtf8().constData(), "capsrc");
    if (!element)
        return {};
    GstElement *bin = gst_bin_new(nullptr);
    gst_bin_add(GST_BIN(bin), element);
    lightning::calls::DeviceBinding binding;
    binding.property = property;
    binding.value = value;
    binding.reason = QStringLiteral("test");
    QString out;
    if (applyBindingTo(bin, "capsrc", binding)) {
        const QByteArray name = property.toUtf8();
        GParamSpec *spec = g_object_class_find_property(
            G_OBJECT_GET_CLASS(element), name.constData());
        GValue read = G_VALUE_INIT;
        g_value_init(&read, spec->value_type);
        g_object_get_property(G_OBJECT(element), name.constData(), &read);
        gchar *text = gst_value_serialize(&read);
        out = QString::fromUtf8(text ? text : "");
        g_free(text);
        g_value_unset(&read);
    }
    gst_object_unref(bin);
    return out;
}

void SfuMediaEngine::setPreferredDevices(const DeviceChoice &camera,
                                         const DeviceChoice &microphone,
                                         const DeviceChoice &speaker)
{
    bool speakerChanged = false;
    bool microphoneChanged = false;
    {
        QMutexLocker lock(&m_deviceMutex);
        speakerChanged = speaker.id != m_speakerChoice.id
            || speaker.description != m_speakerChoice.description;
        microphoneChanged = microphone.id != m_microphoneChoice.id
            || microphone.description != m_microphoneChoice.description;
        m_cameraChoice = camera;
        m_microphoneChoice = microphone;
        m_speakerChoice = speaker;
    }
    if (speakerChanged)
        refreshSpeakerSink(true);
    if (microphoneChanged)
        refreshMicrophoneCapture();
}

void SfuMediaEngine::refreshSpeakerSink(bool applyLive)
{
    DeviceChoice choice;
    quint64 seq = 0;
    {
        QMutexLocker lock(&m_deviceMutex);
        choice = m_speakerChoice;
        seq = ++m_speakerSeq;
        m_resolvedSpeaker = OutputSink{};
    }
    if (choice.id.isEmpty() || testSourceMode()) {
        if (applyLive)
            swapReceiveSinks();
        return;
    }
    QThreadPool::globalInstance()->start([this, choice, seq, applyLive] {
        const OutputSink resolved = resolveSpeakerSink(choice);
        marshal(this, [this, resolved, seq, applyLive] {
            {
                QMutexLocker lock(&m_deviceMutex);
                if (seq != m_speakerSeq)
                    return;
                m_resolvedSpeaker = resolved;
            }
            if (applyLive)
                swapReceiveSinks();
        });
    });
}

SfuMediaEngine::OutputSink SfuMediaEngine::cachedSpeakerSink() const
{
    QMutexLocker lock(&m_deviceMutex);
    return m_resolvedSpeaker;
}

SfuMediaEngine::DeviceChoice SfuMediaEngine::cameraChoice() const
{
    QMutexLocker lock(&m_deviceMutex);
    return m_cameraChoice;
}

SfuMediaEngine::DeviceChoice SfuMediaEngine::microphoneChoice() const
{
    QMutexLocker lock(&m_deviceMutex);
    return m_microphoneChoice;
}

SfuMediaEngine::DeviceChoice SfuMediaEngine::speakerChoice() const
{
    QMutexLocker lock(&m_deviceMutex);
    return m_speakerChoice;
}

int SfuMediaEngine::snapCameraHeight(int height)
{
    if (height == 360 || height == 480 || height == 1080)
        return height;
    return kCameraHeight;
}

int SfuMediaEngine::snapShareBitrateCap(int kbps)
{
    if (kbps <= 0)
        return 0;
    static const int offered[] = { 500, 1000, 2000, 3000, 4500 };
    int best = offered[0];
    for (int v : offered) {
        if (qAbs(v - kbps) < qAbs(best - kbps))
            best = v;
    }
    return best;
}

void SfuMediaEngine::setCameraQuality(int maxHeight)
{
    m_cameraMaxHeight = snapCameraHeight(maxHeight);
}

QString SfuMediaEngine::echoProbeStage(bool echoCancel, bool probeAvailable)
{
    if (!echoCancel || !probeAvailable)
        return {};
    return QStringLiteral("! audioconvert "
                          "! audio/x-raw,format=S16LE,rate=48000 "
                          "! webrtcechoprobe name=%1 ")
        .arg(QLatin1String(kEchoProbeName));
}

bool SfuMediaEngine::echoProbeLive() const
{
    QMutexLocker lock(&m_receiveBinMutex);
    for (auto it = m_receiveBins.cbegin(); it != m_receiveBins.cend(); ++it) {
        if (!it->bin)
            continue;
        if (GstElement *probe = gst_bin_get_by_name(GST_BIN(it->bin),
                                                    kEchoProbeName)) {
            gst_object_unref(probe);
            return true;
        }
    }
    return false;
}

namespace {
bool syncStateIfStillParented(GstElement *bin);
}

bool SfuMediaEngine::echoAllowedOnMic() const
{
    return !m_echoGivenUp && echoProbeLive();
}

void SfuMediaEngine::handleShareEncoderFailed(const QString &cid)
{
    if (!m_active || cid != m_shareEncoderCid || !m_publishedBins.contains(cid))
        return;
    g_shareHardwareEncoderFailed.store(true);
    const auto watch = m_publishWatch.constFind(cid);
    if (watch == m_publishWatch.cend() || !watch->state || watch->reported)
        return;
    m_publishWatch[cid].reported = true;
    qCWarning(lcSfuMedia) << "screen share hardware encoder failed; the share "
                             "ends and the next one uses software";
    Q_EMIT publishFailed(cid, QStringLiteral("screen_share_failed"));
}

void SfuMediaEngine::engageEchoCancellation()
{
    if (!m_active || !m_echoCancel.load() || m_echoGivenUp
        || m_micCid.isEmpty())
        return;
    GstElement *bin = m_publishedBins.value(m_micCid, nullptr);
    if (!bin)
        return;
    if (microphoneStarting()) {
        m_micEchoAfterStart = true;
        return;
    }
    if (!echoProbeLive())
        return;
    ++m_echoEngageAttempts;
    const quint64 generation = m_generation.load();
    qCInfo(lcSfuMedia)
        << "echo cancellation at first remote audio:"
        << calls::noise::engageEchoCancel(bin, [this, generation] {
               marshal(this, [this, generation] {
                   if (m_active && generation == m_generation.load()
                       && echoProbeLive())
                       giveUpEchoCancellation(QStringLiteral(
                           "no free playback probe"));
               });
           });
}

void SfuMediaEngine::giveUpEchoCancellation(const QString &why)
{
    if (m_echoGivenUp)
        return;
    m_echoGivenUp = true;
    qCWarning(lcSfuMedia)
        << "ECHO CANCELLATION FAILED and is off for the rest of this call; "
           "the microphone keeps sending without it. reason="
        << why;
    Q_EMIT echoCancellationFailed();
}

void SfuMediaEngine::handleMicDspError(const QString &cid,
                                       const QString &reason)
{
    if (!m_active || cid != m_micCid || !m_publishedBins.contains(cid))
        return;
    if (m_echoGivenUp)
        return;
    bool echoWasOn = false;
    if (GstElement *bin = m_publishedBins.value(cid, nullptr)) {
        if (GST_IS_BIN(bin)) {
            if (GstElement *dsp = gst_bin_get_by_name(GST_BIN(bin),
                                                      calls::noise::kDspName)) {
                gboolean on = FALSE;
                g_object_get(dsp, "echo-cancel", &on, nullptr);
                echoWasOn = on;
                gst_object_unref(dsp);
            }
        }
    }
    if (!echoWasOn) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: webrtcdsp errored on the microphone with echo "
               "cancellation off; the microphone may be silent. reason="
            << reason;
        if (!m_micDspFailureReported) {
            m_micDspFailureReported = true;
            Q_EMIT publishFailed(cid, QStringLiteral("audio_source_failed"));
        }
        return;
    }
    giveUpEchoCancellation(reason);
    if (microphoneStarting()) {
        m_micDspRescueAfterStart = true;
        return;
    }
    rescueMicrophoneFromDsp();
}

void SfuMediaEngine::rescueMicrophoneFromDsp()
{
    GstElement *bin = m_publishedBins.value(m_micCid, nullptr);
    if (!bin)
        return;
    const QString did = calls::noise::dropEchoCancel(bin);
    qCInfo(lcSfuMedia) << "microphone after a webrtcdsp error:" << did;
    if (did != QLatin1String("echo dropped"))
        return;
    gst_element_call_async(
        bin,
        [](GstElement *micBin, gpointer) {
            syncStateIfStillParented(micBin);
        },
        nullptr, nullptr);
}

void SfuMediaEngine::setShareQuality(int maxHeight, int fps,
                                     int bitrateCapKbps)
{
    m_shareBitrateCapKbps = snapShareBitrateCap(bitrateCapKbps);
    m_shareMaxHeight = (maxHeight <= 900) ? 720
                     : (maxHeight <= 1260) ? 1080
                     : (maxHeight <= 1800) ? 1440
                                           : 2160;
    m_shareFps = (fps <= 22) ? 15 : (fps <= 45) ? 30 : 60;
}

bool SfuMediaEngine::applyShareQualityLive(const QString &cid)
{
    GstElement *bin = m_publishedBins.value(cid, nullptr);
    if (!bin)
        return false;
    bool applied = false;
    auto retarget = [bin](const char *name, const QString &caps) {
        GstElement *filter = gst_bin_get_by_name(GST_BIN(bin), name);
        if (!filter)
            return false;
        GstCaps *parsed = gst_caps_from_string(caps.toUtf8().constData());
        if (parsed)
            g_object_set(filter, "caps", parsed, nullptr);
        const bool ok = parsed != nullptr;
        if (parsed)
            gst_caps_unref(parsed);
        gst_object_unref(filter);
        return ok;
    };
    applied = retarget("publimits",
                       shareLimitsCaps(m_shareMaxHeight, m_shareFps,
                                       m_shareGlEncode));
    if (!applied)
        return false;
    {
        const int h = m_shareMaxHeight;
        retarget("sharegl",
                 QStringLiteral("video/x-raw(memory:GLMemory),"
                                "width=(int)[1,%1],height=(int)[1,%2]")
                     .arg(QString::number((h * 16) / 9),
                          QString::number(h)));
    }
    if (GstElement *enc = gst_bin_get_by_name(GST_BIN(bin), "videoenc")) {
        const QString stage = shareEncoderStage(m_shareMaxHeight, m_shareFps,
                                                m_shareBitrateCapKbps);
        static const QRegularExpression rate(
            QStringLiteral("target-bitrate=(\\d+)"));
        static const QRegularExpression keyframe(
            QStringLiteral("keyframe-max-dist=(\\d+)"));
        const auto bitrate = rate.match(stage);
        const auto dist = keyframe.match(stage);
        if (bitrate.hasMatch() && g_object_class_find_property(
                G_OBJECT_GET_CLASS(enc), "target-bitrate"))
            g_object_set(enc, "target-bitrate", bitrate.captured(1).toInt(),
                         nullptr);
        if (dist.hasMatch() && g_object_class_find_property(
                G_OBJECT_GET_CLASS(enc), "keyframe-max-dist"))
            g_object_set(enc, "keyframe-max-dist", dist.captured(1).toInt(),
                         nullptr);
        if (bitrate.hasMatch()
            && !g_object_class_find_property(G_OBJECT_GET_CLASS(enc),
                                             "target-bitrate")
            && g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "bitrate"))
            g_object_set(enc, "bitrate", bitrate.captured(1).toInt() / 1000,
                         nullptr);
        gst_object_unref(enc);
    }
    qCInfo(lcSfuMedia) << "screen share quality applied live height="
                       << m_shareMaxHeight << "fps=" << m_shareFps;
    return true;
}

void SfuMediaEngine::publishShareAudio(
    const QString &cid, const lightning::shareaudio::Selection &selection)
{
    using lightning::shareaudio::Mode;
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;
    if (!selection.capturesAnything()) {
        qCWarning(lcSfuMedia)
            << "share audio: asked to publish with nothing chosen to capture";
        Q_EMIT failed(QStringLiteral("share_audio_apps_unavailable"));
        return;
    }

    const bool testSeam = static_cast<bool>(m_shareAudioEnumerateForTest);
    const bool captureSeam = m_shareAudioCaptureForTest.has_value();
    const auto capture = captureSeam
        ? *m_shareAudioCaptureForTest
        : (testSeam || m_testSources)
            ? lightning::shareaudio::SystemCapture{}
            : lightning::shareaudio::systemCapture(
                  QCoreApplication::applicationPid());
    const bool wantPerApplication = selection.mode == Mode::Apps
        || capture.kind == lightning::shareaudio::SystemCapture::Kind::PerApplication;
    bool perApplication = false;
    bool excludesUs = false;
    QString source;
    if (testSeam) {
        perApplication = true;
        excludesUs = true;
        m_shareAudioOptions = m_shareAudioOptionsForTest;
    } else if (m_testSources) {
        source = QStringLiteral(
            "audiotestsrc name=sharesrc is-live=true wave=sine freq=220 "
            "volume=0.05");
        excludesUs = true;
    } else if (wantPerApplication
               && (captureSeam
                       ? capture.kind == lightning::shareaudio::SystemCapture::
                             Kind::PerApplication
                       : lightning::shareaudio::perApplicationCaptureAvailable())) {
        perApplication = true;
        excludesUs = true;
        m_shareAudioOptions = lightning::shareaudio::branchOptions();
    } else if (selection.mode == Mode::Apps) {
        qCWarning(lcSfuMedia)
            << "share audio: per-application capture unavailable";
        Q_EMIT failed(QStringLiteral("share_audio_apps_unavailable"));
        return;
    } else {
        source = capture.description;
        excludesUs = capture.excludesUs;
        if (!excludesUs) {
            qCWarning(lcSfuMedia)
                << "share audio disabled: no capture can exclude this call's "
                   "playback; an output monitor would echo participants";
        }
    }
    if (perApplication) {
        source = lightning::shareaudio::mixedSourceDescription(
            {}, m_shareAudioOptions);
    }
    if (source.isEmpty()) {
        qCWarning(lcSfuMedia)
            << "share audio: no loopback capture element on this platform";
        Q_EMIT failed(QStringLiteral("share_audio_unavailable"));
        return;
    }

    const QString description = lightning::shareaudio::encodedTrackDescription(
        source, nextPublishSsrc(), elementAvailable("level"),
        m_shareAudioMuted);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
        qCWarning(lcSfuMedia) << "share audio pipeline parse failed:"
                              << (error->message ? error->message : "?");
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    gst_element_set_name(bin, cid.toUtf8().constData());
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), bin)) {
        gst_object_unref(bin);
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    if (GstElement *encoder =
            gst_bin_get_by_name(GST_BIN(bin), "shareaudioenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded,false);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    applyPublisherMsid(sinkPad, cid);
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad && !consumePublishLinkFailure())
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "share audio link failed code=" << linked;
        releaseFailedPublishPad(sinkPad);
        m_publishedBins.remove(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(bin);
    ++m_publishedMedia;
    m_publisherEverPublished = true;
    m_shareAudioAnyCid = cid;
    m_shareAudioSelection = selection;
    m_shareAudioSourceErrorReported = false;
    m_shareAudioLevelReports = 0;
    m_shareAudioPerApplication = perApplication;
    m_shareAudioExcludesUs = excludesUs;
    m_shareAudioCarried.clear();
    m_shareAudioFailedKeys.clear();
    m_shareAudioLimitReached = false;
    if (perApplication) {
        m_shareAudioCid = cid;
        m_shareAudioRecords.clear();
        m_shareAudioRetiredAt.clear();
        m_shareAudioEosSeen.clear();
        m_shareAudioConnections.clear();
        m_shareAudioFailedAt.clear();
        m_shareAudioFailures.clear();
        m_shareAudioAlive->store(false);
        m_shareAudioAlive = std::make_shared<std::atomic<bool>>(true);
        m_shareAudioBranches = 0;
        m_shareAudioNextIndex = 0;
        m_shareAudioScans = 0;
        ++m_shareAudioGeneration;
        m_shareAudioScanInFlight = false;
        if (!m_shareAudioScanTimer.isActive()) {
            m_shareAudioScanTimer.setInterval(2000);
            m_shareAudioScanTimer.setSingleShot(false);
            connect(&m_shareAudioScanTimer, &QTimer::timeout, this,
                    &SfuMediaEngine::rescanShareAudioSources,
                    Qt::UniqueConnection);
            m_shareAudioScanTimer.start();
        }
        qCInfo(lcSfuMedia)
            << "share audio: per-application capture, mode="
            << (selection.mode == Mode::Apps ? "apps" : "system")
            << "chosen=" << selection.keys.size()
            << "retire-on-disconnect=" << m_shareAudioOptions.retireOnDisconnect
            << "follows-process=" << m_shareAudioOptions.followsProcess;
        rescanShareAudioSources();
    }
    emitShareAudioReport();
    qCInfo(lcSfuMedia) << "share audio published perApplication="
                       << perApplication << "excludesUs=" << excludesUs;
}

SfuMediaEngine::PublishProbeState::~PublishProbeState()
{
    if (lastFrame)
        gst_buffer_unref(lastFrame);
    if (keepSink)
        gst_object_unref(keepSink);
}

quint64 SfuMediaEngine::keepAlivePts(quint64 sampledPts, bool sampledPtsValid,
                                     qint64 elapsedMs, quint64 lastInjectedPts,
                                     bool lastInjectedPtsValid)
{
    if (!sampledPtsValid)
        return GST_CLOCK_TIME_NONE;
    const quint64 elapsed =
        static_cast<quint64>(qMax<qint64>(0, elapsedMs)) * GST_MSECOND;
    quint64 at = sampledPts + elapsed;
    if (lastInjectedPtsValid && at <= lastInjectedPts)
        at = lastInjectedPts + 1;
    return at;
}

GstPad *SfuMediaEngine::keepAliveInjectionPad(GstElement *bin)
{
    if (!bin)
        return nullptr;
    GstElement *rate = gst_bin_get_by_name(GST_BIN(bin), "vidrate");
    if (!rate)
        return nullptr;
    GstPad *sink = gst_element_get_static_pad(rate, "sink");
    gst_object_unref(rate);
    return sink;
}

void SfuMediaEngine::releaseKeepAlive(
    const std::shared_ptr<PublishProbeState> &state)
{
    if (!state)
        return;
    GstBuffer *frame = nullptr;
    {
        QMutexLocker lock(&state->keepMutex);
        frame = state->lastFrame;
        state->lastFrame = nullptr;
    }
    if (frame)
        gst_buffer_unref(frame);
    if (GstPad *pad = state->keepSink) {
        state->keepSink = nullptr;
        gst_object_unref(pad);
    }
}

namespace {
struct KeepAliveInjection {
    GstPad *pad = nullptr;
    GstBuffer *buffer = nullptr;
};
}

void SfuMediaEngine::updateShareKeepAliveTimer()
{
    bool wanted = false;
    for (auto it = m_publishWatch.cbegin(); it != m_publishWatch.cend(); ++it) {
        if (it->state && it->state->screenShare && it->state->keepSink)
            wanted = true;
    }
    if (wanted && !m_shareKeepAliveTimer.isActive())
        m_shareKeepAliveTimer.start();
    else if (!wanted && m_shareKeepAliveTimer.isActive())
        m_shareKeepAliveTimer.stop();
}

void SfuMediaEngine::tickShareKeepAlive()
{
    constexpr qint64 kQuietMs = 500;
    const qint64 now = monotonicMs();
    for (auto it = m_publishWatch.cbegin(); it != m_publishWatch.cend(); ++it) {
        const auto &state = it->state;
        if (!state || !state->screenShare || !state->keepSink)
            continue;
        const qint64 last = state->lastFrameMs.load();
        if (last < 0 || now - last < kQuietMs)
            continue;
        GstBuffer *sample = nullptr;
        quint64 sampledPts = 0;
        bool sampledPtsValid = false;
        qint64 sampledAtMs = -1;
        {
            QMutexLocker lock(&state->keepMutex);
            if (state->lastFrame)
                sample = gst_buffer_ref(state->lastFrame);
            sampledPts = state->lastFramePts;
            sampledPtsValid = state->lastFramePtsValid;
            sampledAtMs = state->lastSampleAtMs;
        }
        if (!sample)
            continue;
        const quint64 at = keepAlivePts(
            sampledPts, sampledPtsValid,
            sampledAtMs >= 0 ? now - sampledAtMs : 0,
            state->lastInjectedPts, state->lastInjectedPtsValid);
        if (!GST_CLOCK_TIME_IS_VALID(at)) {
            gst_buffer_unref(sample);
            continue;
        }
        GstBuffer *inject = gst_buffer_copy(sample);
        gst_buffer_unref(sample);
        if (!inject)
            continue;
        state->lastInjectedPts = at;
        state->lastInjectedPtsValid = true;
        GST_BUFFER_PTS(inject) = at;
        GST_BUFFER_DTS(inject) = at;
        GST_BUFFER_DURATION(inject) = GST_CLOCK_TIME_NONE;
        if (GstElement *rate = gst_pad_get_parent_element(state->keepSink)) {
            auto *carried = new KeepAliveInjection{
                GST_PAD(gst_object_ref(state->keepSink)), inject};
            gst_element_call_async(
                rate,
                [](GstElement *, gpointer data) {
                    auto *job = static_cast<KeepAliveInjection *>(data);
                    gst_pad_chain(job->pad, job->buffer);
                    job->buffer = nullptr;
                },
                carried,
                [](gpointer data) {
                    auto *job = static_cast<KeepAliveInjection *>(data);
                    if (job->buffer)
                        gst_buffer_unref(job->buffer);
                    gst_object_unref(job->pad);
                    delete job;
                });
            gst_object_unref(rate);
        } else {
            gst_buffer_unref(inject);
            continue;
        }
        const quint64 n = state->keepAliveInjected.fetch_add(1) + 1;
        if (n == 1 || n % 100 == 0) {
            qCInfo(lcSfuMedia)
                << "screen share keep-alive: the capture has been quiet, "
                   "re-pushed the last picture count=" << n
                << "quietMs=" << (now - last);
        }
    }
}

void SfuMediaEngine::resetShareAudioState()
{
    const bool wasLive = !m_shareAudioAnyCid.isEmpty();
    m_shareAudioScanTimer.stop();
    m_shareAudioCid.clear();
    m_shareAudioAnyCid.clear();
    m_shareAudioRecords.clear();
    m_shareAudioRetiredAt.clear();
    m_shareAudioEosSeen.clear();
    m_shareAudioConnections.clear();
    m_shareAudioFailedAt.clear();
    m_shareAudioFailures.clear();
    m_shareAudioAlive->store(false);
    m_shareAudioHangLogged = false;
    m_shareAudioCarried.clear();
    m_shareAudioFailedKeys.clear();
    m_shareAudioLimitReached = false;
    m_shareAudioPerApplication = false;
    m_shareAudioExcludesUs = false;
    m_shareAudioBranches = 0;
    m_shareAudioNextIndex = 0;
    m_shareAudioScans = 0;
    m_shareAudioLevelReports = 0;
    m_shareAudioSourceErrorReported = false;
    ++m_shareAudioGeneration;
    m_shareAudioScanInFlight = false;
    if (wasLive)
        emitShareAudioReport();
}

void SfuMediaEngine::emitShareAudioReport()
{
    Q_EMIT shareAudioReport(!m_shareAudioAnyCid.isEmpty(),
                            m_shareAudioPerApplication, m_shareAudioExcludesUs,
                            m_shareAudioCarried, m_shareAudioFailedKeys,
                            m_shareAudioLimitReached);
}

bool SfuMediaEngine::applyShareAudioSelection(
    const lightning::shareaudio::Selection &selection)
{
    using lightning::shareaudio::Mode;
    const bool running = !m_shareAudioAnyCid.isEmpty()
        && m_publishedBins.contains(m_shareAudioAnyCid);
    if (!running) {
        m_shareAudioSelection = selection;
        return true;
    }
    if (!selection.capturesAnything())
        return false;
    if (m_shareAudioCid.isEmpty()) {
        if (selection.mode != Mode::System)
            return false;
        m_shareAudioSelection = selection;
        return true;
    }
#if defined(Q_OS_WIN)
    if (selection.mode == Mode::System && !m_shareAudioEnumerateForTest)
        return false;
#endif
    m_shareAudioSelection = selection;
    qCInfo(lcSfuMedia) << "share audio: selection changed in place mode="
                       << (selection.mode == Mode::Apps ? "apps" : "system")
                       << "chosen=" << selection.keys.size();
    rescanShareAudioSources();
    return true;
}

namespace {
GstPadProbeReturn noteBranchEos(GstPad *, GstPadProbeInfo *info, gpointer data);
void freeBranchEosFlag(gpointer data);

struct BranchStart {
    SfuMediaEngine *engine = nullptr;
    GstElement *shareBin = nullptr;
    GstElement *mixer = nullptr;
    QString id;
    int index = -1;
    quint64 generation = 0;
    int delayMsForTest = 0;
    bool failForTest = false;
    std::shared_ptr<std::atomic<bool>> shareAlive;
    std::shared_ptr<std::atomic<int>> outstanding;
};

void branchStartFree(gpointer data)
{
    auto *ctx = static_cast<BranchStart *>(data);
    if (ctx->mixer)
        gst_object_unref(ctx->mixer);
    if (ctx->shareBin)
        gst_object_unref(ctx->shareBin);
    if (ctx->outstanding)
        ctx->outstanding->fetch_sub(1);
    delete ctx;
}

/// On a GStreamer pool thread. pipewiresrc's PAUSED->PLAYING waits up to
/// 30 s for its stream to reach STREAMING (GST_PIPEWIRE_DEFAULT_TIMEOUT), and
/// right after a daemon restart it sometimes does: on the GUI thread that was
/// a 30 s freeze of the whole application (measured in the Flatpak, with the
/// MatrixRTC delayed leave firing meanwhile and every video frame of the
/// share piling up behind it).
void branchStartAsync(GstElement *branch, gpointer data)
{
    auto *ctx = static_cast<BranchStart *>(data);
    if (ctx->delayMsForTest > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ctx->delayMsForTest));
    bool ok = false;
    if (ctx->shareAlive && ctx->shareAlive->load())
        ok = !ctx->failForTest && gst_element_sync_state_with_parent(branch);
    if (ok && ctx->shareAlive && !ctx->shareAlive->load()) {
        gst_element_set_state(branch, GST_STATE_NULL);
        ok = false;
    }
    if (!ok) {
        GstPad *src = gst_element_get_static_pad(branch, "src");
        GstPad *peer = src ? gst_pad_get_peer(src) : nullptr;
        if (src && peer)
            gst_pad_unlink(src, peer);
        if (peer) {
            gst_element_release_request_pad(ctx->mixer, peer);
            gst_object_unref(peer);
        }
        if (src)
            gst_object_unref(src);
        gst_element_set_state(branch, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(ctx->shareBin), branch);
    }
    SfuMediaEngine *engine = ctx->engine;
    const QString id = ctx->id;
    const int index = ctx->index;
    const quint64 generation = ctx->generation;
    marshal(engine, [engine, id, index, generation, ok] {
        engine->noteShareAudioBranchStarted(id, index, generation, ok);
    });
}
}

bool SfuMediaEngine::addShareAudioBranch(
    GstElement *bin, GstElement *mixer,
    const lightning::shareaudio::Stream &s)
{
    lightning::shareaudio::BranchRecord record;
    record.appKey = s.appKey;
    record.pid = s.pid;
    record.startTime = s.startTime;
    record.failed = true;
    m_shareAudioRecords.insert(s.id(), record);
    m_shareAudioFailedAt.insert(s.id(), m_shareAudioScans);
    const int index = m_shareAudioNextIndex++;
    const QString description = lightning::shareaudio::
        applicationBranchDescription(s, index, m_shareAudioOptions);
    if (description.isEmpty())
        return false;
    GError *error = nullptr;
    GstElement *branch = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error) {
        qCWarning(lcSfuMedia)
            << "share audio: could not build a branch for an application:"
            << (error->message ? error->message : "?");
        g_error_free(error);
        if (branch)
            gst_object_unref(branch);
        return false;
    }
    if (!branch)
        return false;
    int connection = -1;
    if (m_shareAudioOptions.capture == lightning::shareaudio::Capture::PipeWire) {
        if (GstElement *source = gst_bin_get_by_name(
                GST_BIN(branch), lightning::shareaudio::branchSourceName(index)
                                     .toUtf8()
                                     .constData())) {
            connection = lightning::shareaudio::givePrivatePipeWireConnection(
                G_OBJECT(source));
            gst_object_unref(source);
        }
    }
    gst_element_set_name(branch, lightning::shareaudio::branchBinName(index)
                                     .toUtf8()
                                     .constData());
    if (!gst_bin_add(GST_BIN(bin), branch)) {
        gst_object_unref(branch);
        return false;
    }
    GstPad *srcPad = gst_element_get_static_pad(branch, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(mixer, "sink_%u");
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad)
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (linked != GST_PAD_LINK_OK) {
        if (sinkPad) {
            gst_element_release_request_pad(mixer, sinkPad);
            gst_object_unref(sinkPad);
        }
        qCWarning(lcSfuMedia)
            << "share audio: an application's branch would not link,"
            << "code=" << linked;
        gst_bin_remove(GST_BIN(bin), branch);
        return false;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    auto eosSeen = std::make_shared<std::atomic<bool>>(false);
    if (GstPad *ghost = gst_element_get_static_pad(branch, "src")) {
        gst_pad_add_probe(ghost, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                          noteBranchEos,
                          new std::shared_ptr<std::atomic<bool>>(eosSeen),
                          freeBranchEosFlag);
        gst_object_unref(ghost);
    }
    m_shareAudioEosSeen.insert(s.id(), eosSeen);
    lightning::shareaudio::BranchRecord &built = m_shareAudioRecords[s.id()];
    built.failed = false;
    built.index = index;
    built.starting = true;
    m_shareAudioFailedAt.remove(s.id());
    if (connection >= 0)
        m_shareAudioConnections.insert(s.id(), connection);
    ++m_shareAudioBranches;

    m_pendingTeardowns->fetch_add(1);
    auto *ctx = new BranchStart;
    ctx->engine = this;
    ctx->shareBin = GST_ELEMENT(gst_object_ref(bin));
    ctx->mixer = GST_ELEMENT(gst_object_ref(mixer));
    ctx->id = s.id();
    ctx->index = index;
    ctx->generation = m_shareAudioGeneration;
    ctx->delayMsForTest = m_shareAudioStartDelayMsForTest;
    if (m_shareAudioFailStartsForTest > 0) {
        --m_shareAudioFailStartsForTest;
        ctx->failForTest = true;
    }
    ctx->shareAlive = m_shareAudioAlive;
    ctx->outstanding = m_pendingTeardowns;
    gst_element_call_async(branch, branchStartAsync, ctx, branchStartFree);
    return true;
}

void SfuMediaEngine::noteShareAudioBranchStarted(const QString &id, int index,
                                                 quint64 generation, bool ok)
{
    if (generation != m_shareAudioGeneration)
        return;
    auto record = m_shareAudioRecords.find(id);
    if (record == m_shareAudioRecords.end() || record->index != index)
        return;
    record->starting = false;
    if (ok && record->failed) {
        return;
    }
    if (ok) {
        m_shareAudioFailures.remove(id);
        qCInfo(lcSfuMedia) << "share audio: capturing an application app="
                           << record->appKey << "branch=" << index;
        refreshShareAudioKeys();
        return;
    }
    const int failures = ++m_shareAudioFailures[id];
    record->failed = true;
    record->index = -1;
    record->retired = false;
    m_shareAudioRetiredAt.remove(id);
    m_shareAudioEosSeen.remove(id);
    m_shareAudioConnections.remove(id);
    m_shareAudioFailedAt.insert(id, m_shareAudioScans);
    if (m_shareAudioBranches > 0)
        --m_shareAudioBranches;
    qCWarning(lcSfuMedia) << "share audio: an application's branch would not "
                             "start; trying again in about"
                          << shareAudioCoolDownScans(failures) * 2 << "s app="
                          << record->appKey << "branch=" << index
                          << "attempt=" << failures;
    refreshShareAudioKeys();
}

int SfuMediaEngine::shareAudioCoolDownScans(int failures)
{
    if (failures <= 1)
        return 2;
    if (failures == 2)
        return 5;
    return 15;
}

void SfuMediaEngine::refreshShareAudioKeys()
{
    QStringList carried;
    QStringList failed;
    for (auto it = m_shareAudioRecords.cbegin();
         it != m_shareAudioRecords.cend(); ++it) {
        if (it->appKey.isEmpty())
            continue;
        if (it->failed) {
            if (!failed.contains(it->appKey))
                failed << it->appKey;
        } else if (it->index >= 0 && !it->retired && !it->muted
                   && !carried.contains(it->appKey)) {
            carried << it->appKey;
        }
    }
    for (const QString &key : carried)
        failed.removeAll(key);
    carried.sort();
    failed.sort();
    if (carried == m_shareAudioCarried && failed == m_shareAudioFailedKeys)
        return;
    m_shareAudioCarried = carried;
    m_shareAudioFailedKeys = failed;
    emitShareAudioReport();
}

namespace {

GstPadProbeReturn noteBranchEos(GstPad *, GstPadProbeInfo *info, gpointer data)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event && GST_EVENT_TYPE(event) == GST_EVENT_EOS)
        static_cast<std::shared_ptr<std::atomic<bool>> *>(data)->get()->store(
            true);
    return GST_PAD_PROBE_OK;
}

void freeBranchEosFlag(gpointer data)
{
    delete static_cast<std::shared_ptr<std::atomic<bool>> *>(data);
}

struct BranchTakeOut {
    GstElement *shareBin = nullptr;
    GstElement *mixer = nullptr;
    std::shared_ptr<std::atomic<int>> outstanding;
};

void branchTakeOutFree(gpointer data)
{
    auto *ctx = static_cast<BranchTakeOut *>(data);
    if (ctx->mixer)
        gst_object_unref(ctx->mixer);
    if (ctx->shareBin)
        gst_object_unref(ctx->shareBin);
    if (ctx->outstanding)
        ctx->outstanding->fetch_sub(1);
    delete ctx;
}

/// On a GStreamer thread-pool thread, never a streaming thread and never the
/// GUI thread: a pipewiresrc going to NULL can wait on its daemon with no
/// timeout (1.4.x).
void branchTakeOutAsync(GstElement *branch, gpointer data)
{
    auto *ctx = static_cast<BranchTakeOut *>(data);
    gst_bin_remove(GST_BIN(ctx->shareBin), branch);
    gst_element_set_state(branch, GST_STATE_NULL);
}

GstPadProbeReturn branchTakeOutProbe(GstPad *pad, GstPadProbeInfo *,
                                     gpointer data)
{
    auto *ctx = static_cast<BranchTakeOut *>(data);
    if (GstPad *peer = gst_pad_get_peer(pad)) {
        gst_pad_unlink(pad, peer);
        gst_element_release_request_pad(ctx->mixer, peer);
        gst_object_unref(peer);
    }
    GstElement *branch = gst_pad_get_parent_element(pad);
    if (!branch) {
        branchTakeOutFree(ctx);
        return GST_PAD_PROBE_REMOVE;
    }
    gst_element_call_async(branch, branchTakeOutAsync, ctx, branchTakeOutFree);
    gst_object_unref(branch);
    return GST_PAD_PROBE_REMOVE;
}

}

void SfuMediaEngine::takeOutShareAudioBranch(GstElement *bin, GstElement *mixer,
                                             int index)
{
    GstElement *branch = gst_bin_get_by_name(
        GST_BIN(bin),
        lightning::shareaudio::branchBinName(index).toUtf8().constData());
    if (!branch)
        return;
    GstPad *src = gst_element_get_static_pad(branch, "src");
    gst_object_unref(branch);
    if (!src)
        return;
    m_pendingTeardowns->fetch_add(1);
    auto *ctx = new BranchTakeOut{
        GST_ELEMENT(gst_object_ref(bin)), GST_ELEMENT(gst_object_ref(mixer)),
        m_pendingTeardowns};
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_IDLE, branchTakeOutProbe, ctx,
                      nullptr);
    gst_object_unref(src);
}

void SfuMediaEngine::noteShareAudioDaemonRestart(int closed)
{
    const qint64 now = monotonicMs();
    if (m_shareAudioRebuildAskedMs >= 0
        && now - m_shareAudioRebuildAskedMs < m_shareAudioRebuildGapMs) {
        qCInfo(lcSfuMedia) << "share audio: the audio daemon closed every "
                              "capture again; recapturing in place (a fresh "
                              "track was asked for"
                           << (now - m_shareAudioRebuildAskedMs) << "ms ago)";
        return;
    }
    m_shareAudioRebuildAskedMs = now;
    qCWarning(lcSfuMedia)
        << "share audio: the audio daemon closed all" << closed
        << "captures of the share at once (it restarted); asking for a fresh "
           "share-audio track, so nothing of the old one (encoder, "
           "timestamps, the receivers' state for its SSRC) outlives it";
    const QString cid = m_shareAudioCid;
    const quint64 generation = m_shareAudioGeneration;
    QMetaObject::invokeMethod(
        this,
        [this, cid, generation] {
            if (generation != m_shareAudioGeneration || cid != m_shareAudioCid)
                return;
            Q_EMIT shareAudioRebuildWanted(cid);
        },
        Qt::QueuedConnection);
}

void SfuMediaEngine::rescanShareAudioSources()
{
    if (m_shareAudioCid.isEmpty()) {
        m_shareAudioScanTimer.stop();
        return;
    }
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (!bin) {
        resetShareAudioState();
        return;
    }
    ++m_shareAudioScans;

    int watchedLive = 0;
    int closedNow = 0;
    for (auto it = m_shareAudioConnections.cbegin();
         it != m_shareAudioConnections.cend(); ++it) {
        auto record = m_shareAudioRecords.find(it.key());
        if (record == m_shareAudioRecords.end() || record->index < 0
            || record->retired)
            continue;
        const bool counted = !record->failed && !record->starting;
        if (counted)
            ++watchedLive;
        if (!lightning::shareaudio::connectionClosed(it.value()))
            continue;
        if (counted)
            ++closedNow;
        record->retired = true;
        m_shareAudioRetiredAt.insert(it.key(), m_shareAudioScans);
        GstElement *source = m_shareAudioSuppressRetireEosForTest
            ? nullptr
            : gst_bin_get_by_name(
                  GST_BIN(bin),
                  lightning::shareaudio::branchSourceName(record->index)
                      .toUtf8()
                      .constData());
        if (source) {
            gst_element_send_event(source, gst_event_new_eos());
            gst_object_unref(source);
        }
        qCInfo(lcSfuMedia) << "share audio: the audio daemon closed an "
                              "application's capture (restarted?) app="
                           << record->appKey << "branch=" << record->index;
    }
    if (closedNow > 0 && closedNow == watchedLive)
        noteShareAudioDaemonRestart(closedNow);

    for (auto it = m_shareAudioEosSeen.cbegin(); it != m_shareAudioEosSeen.cend();
         ++it) {
        if (!it.value()->load())
            continue;
        auto record = m_shareAudioRecords.find(it.key());
        if (record == m_shareAudioRecords.end() || record->index < 0
            || record->retired)
            continue;
        record->retired = true;
        m_shareAudioRetiredAt.insert(it.key(), m_shareAudioScans);
        qCInfo(lcSfuMedia) << "share audio: an application's capture ended by "
                              "itself app="
                           << record->appKey << "branch=" << record->index;
    }

    if (GstElement *mixer = gst_bin_get_by_name(
            GST_BIN(bin),
            lightning::shareaudio::mixerElementName().toUtf8().constData())) {
        for (auto it = m_shareAudioRetiredAt.begin();
             it != m_shareAudioRetiredAt.end();) {
            auto record = m_shareAudioRecords.find(it.key());
            if (record == m_shareAudioRecords.end() || record->index < 0) {
                it = m_shareAudioRetiredAt.erase(it);
                continue;
            }
            if (record->starting) {
                ++it;
                continue;
            }
            const auto eos = m_shareAudioEosSeen.constFind(it.key());
            const auto connection = m_shareAudioConnections.constFind(it.key());
            const bool finished = (eos != m_shareAudioEosSeen.cend()
                                   && (*eos)->load())
                || record->failed
                || (connection != m_shareAudioConnections.cend()
                    && lightning::shareaudio::connectionClosed(*connection));
            if (!finished) {
                ++it;
                continue;
            }
            takeOutShareAudioBranch(bin, mixer, record->index);
            --m_shareAudioBranches;
            m_shareAudioEosSeen.remove(it.key());
            m_shareAudioConnections.remove(it.key());
            if (record->failed) {
                record->index = -1;
                m_shareAudioFailedAt.insert(it.key(), m_shareAudioScans);
            } else {
                m_shareAudioRecords.erase(record);
            }
            it = m_shareAudioRetiredAt.erase(it);
        }
        gst_object_unref(mixer);
    }
    for (auto it = m_shareAudioFailedAt.begin();
         it != m_shareAudioFailedAt.end();) {
        const int coolDown =
            shareAudioCoolDownScans(m_shareAudioFailures.value(it.key(), 1));
        if (m_shareAudioScans - it.value() < coolDown) {
            ++it;
            continue;
        }
        auto record = m_shareAudioRecords.find(it.key());
        if (record != m_shareAudioRecords.end() && record->index < 0)
            m_shareAudioRecords.erase(record);
        it = m_shareAudioFailedAt.erase(it);
    }
    refreshShareAudioKeys();

    QList<lightning::shareaudio::Stream> check;
    for (auto it = m_shareAudioRecords.cbegin();
         it != m_shareAudioRecords.cend(); ++it) {
        if (it->index < 0 || it->retired || it->failed)
            continue;
        lightning::shareaudio::Stream s;
        s.serial = QString::number(it->pid);
        s.pid = it->pid;
        s.startTime = it->startTime;
        check << s;
    }

    if (m_shareAudioEnumerateForTest) {
        applyShareAudioScan(m_shareAudioEnumerateForTest(check));
        return;
    }
    if (m_shareAudioScanInFlight) {
        if (!m_shareAudioHangLogged && m_shareAudioScanStarted.isValid()
            && m_shareAudioScanStarted.elapsed() > 10000) {
            m_shareAudioHangLogged = true;
            qCWarning(lcSfuMedia)
                << "share audio: an application enumeration has not returned"
                << "for" << m_shareAudioScanStarted.elapsed() / 1000
                << "s; the share keeps what it has, and nothing new is added "
                   "until it does (a stuck audio daemon?)";
        }
        return;
    }
    m_shareAudioScanInFlight = true;
    m_shareAudioScanStarted.start();
    const quint64 generation = m_shareAudioGeneration;
    lightning::shareaudio::enumerateAsync(
        this, QCoreApplication::applicationPid(), ourAudioClientNames(), check,
        [this, generation](const lightning::shareaudio::Enumeration &e) {
            if (generation != m_shareAudioGeneration)
                return;
            m_shareAudioScanInFlight = false;
            if (m_shareAudioHangLogged) {
                m_shareAudioHangLogged = false;
                qCInfo(lcSfuMedia) << "share audio: the enumeration returned";
            }
            applyShareAudioScan(e);
        });
}

void SfuMediaEngine::applyShareAudioScan(
    const lightning::shareaudio::Enumeration &e)
{
    if (m_shareAudioCid.isEmpty())
        return;
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (!bin)
        return;
    GstElement *mixer = gst_bin_get_by_name(
        GST_BIN(bin),
        lightning::shareaudio::mixerElementName().toUtf8().constData());
    if (!mixer)
        return;
    if (e.elapsedMs > 20 && m_shareAudioScans % 30 == 1) {
        qCInfo(lcSfuMedia) << "share audio: one enumeration pass took"
                           << e.elapsedMs << "ms streams=" << e.streams.size();
    }

    lightning::shareaudio::ScanInput in;
    in.live = e.streams;
    in.records = m_shareAudioRecords;
    in.selection = m_shareAudioSelection;
    in.options = m_shareAudioOptions;
    in.running = e.running;
    in.parents = e.parents;
    in.liveBranches = m_shareAudioBranches;
    in.cap = m_shareAudioCap;
    const lightning::shareaudio::ScanPlan plan =
        lightning::shareaudio::planScan(in);

    for (const QString &id : plan.retire) {
        auto record = m_shareAudioRecords.find(id);
        if (record == m_shareAudioRecords.end())
            continue;
        record->retired = true;
        m_shareAudioRetiredAt.insert(id, m_shareAudioScans);
        if (m_shareAudioSuppressRetireEosForTest)
            continue;
        if (GstElement *source = gst_bin_get_by_name(
                GST_BIN(bin), lightning::shareaudio::branchSourceName(
                                  record->index)
                                  .toUtf8()
                                  .constData())) {
            gst_element_send_event(source, gst_event_new_eos());
            gst_object_unref(source);
        }
        qCInfo(lcSfuMedia) << "share audio: an application stopped, its "
                              "capture retired app="
                           << record->appKey << "branch=" << record->index;
    }
    for (const auto &change : plan.mute) {
        auto record = m_shareAudioRecords.find(change.first);
        if (record == m_shareAudioRecords.end() || record->index < 0)
            continue;
        if (GstElement *volume = gst_bin_get_by_name(
                GST_BIN(bin), lightning::shareaudio::branchVolumeName(
                                  record->index)
                                  .toUtf8()
                                  .constData())) {
            g_object_set(volume, "mute", change.second ? TRUE : FALSE, nullptr);
            gst_object_unref(volume);
            record->muted = change.second;
            qCInfo(lcSfuMedia) << "share audio:"
                               << (change.second ? "muted" : "unmuted")
                               << "app=" << record->appKey
                               << "branch=" << record->index;
        }
    }
    for (const lightning::shareaudio::Stream &s : plan.add) {
        if (!addShareAudioBranch(bin, mixer, s))
            ++m_shareAudioFailures[s.id()];
    }
    gst_object_unref(mixer);

    if (plan.limitReached && !m_shareAudioLimitReached) {
        qCWarning(lcSfuMedia) << "share audio: the per-share limit of"
                              << m_shareAudioCap
                              << "application captures is reached; a newly "
                                 "chosen or newly playing application is left "
                                 "out";
    }
    if (plan.limitReached != m_shareAudioLimitReached) {
        m_shareAudioLimitReached = plan.limitReached;
        emitShareAudioReport();
    }
    refreshShareAudioKeys();

    constexpr int kScansBeforeDoubt = 15;
    if (m_shareAudioCarried.isEmpty() && m_shareAudioScans == kScansBeforeDoubt) {
        qCWarning(lcSfuMedia)
            << "share audio: no chosen application audio stream has been "
               "found in"
            << (kScansBeforeDoubt * 2)
            << "seconds — this share is carrying silence. streams-seen="
            << e.streams.size() << "chosen=" << m_shareAudioSelection.keys.size();
    }
}

void SfuMediaEngine::handleShareAudioBranchError(int index)
{
    for (auto it = m_shareAudioRecords.begin(); it != m_shareAudioRecords.end();
         ++it) {
        if (it->index != index || it->failed)
            continue;
        it->failed = true;
        if (!it->retired) {
            it->retired = true;
            m_shareAudioRetiredAt.insert(it.key(), m_shareAudioScans);
        }
        const int failures = ++m_shareAudioFailures[it.key()];
        qCWarning(lcSfuMedia) << "share audio: an application's capture failed"
                              << "app=" << it->appKey << "branch=" << index
                              << "attempt=" << failures;
        refreshShareAudioKeys();
        return;
    }
}

void SfuMediaEngine::handleShareAudioLevel(double peakDb)
{
    if (m_shareAudioAnyCid.isEmpty())
        return;
    Q_EMIT shareAudioLevel(peakDb);
    if (m_shareAudioLevelReports++ % 5 == 0) {
        qCInfo(lcSfuMedia) << "share audio level peak=" << qRound(peakDb)
                           << "dBFS carrying=" << m_shareAudioCarried.size()
                           << "app(s) perApplication="
                           << m_shareAudioPerApplication;
    }
}

void SfuMediaEngine::handleShareAudioSourceError()
{
    if (!m_active || m_shareAudioAnyCid.isEmpty()
        || !m_publishedBins.contains(m_shareAudioAnyCid)
        || m_shareAudioSourceErrorReported)
        return;
    m_shareAudioSourceErrorReported = true;
    qCWarning(lcSfuMedia)
        << "share audio: the loopback capture reported it cannot capture; "
           "the share continues without sound";
    Q_EMIT failed(QStringLiteral("share_audio_failed"));
}

void SfuMediaEngine::setShareAudioSourceForTest(
    std::function<lightning::shareaudio::Enumeration(
        const QList<lightning::shareaudio::Stream> &)> enumerate,
    const lightning::shareaudio::BranchOptions &options)
{
    m_shareAudioEnumerateForTest = std::move(enumerate);
    m_shareAudioOptionsForTest = options;
    m_shareAudioOptionsForTest.capture = lightning::shareaudio::Capture::Test;
}

bool SfuMediaEngine::shareAudioBranchMutedForTest(const QString &id) const
{
    const auto it = m_shareAudioRecords.constFind(id);
    if (it == m_shareAudioRecords.cend() || it->index < 0)
        return false;
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (!bin)
        return false;
    gboolean mute = FALSE;
    if (GstElement *volume = gst_bin_get_by_name(
            GST_BIN(bin),
            lightning::shareaudio::branchVolumeName(it->index).toUtf8().constData())) {
        g_object_get(volume, "mute", &mute, nullptr);
        gst_object_unref(volume);
    }
    return mute;
}

int SfuMediaEngine::shareAudioBranchBinsForTest() const
{
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (!bin)
        return 0;
    int found = 0;
    GstIterator *it = gst_bin_iterate_elements(GST_BIN(bin));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        GstElement *element = GST_ELEMENT(g_value_get_object(&item));
        gchar *name = gst_element_get_name(element);
        if (name && g_str_has_prefix(name, "shareappbin"))
            ++found;
        g_free(name);
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return found;
}

void SfuMediaEngine::sendShareAudioEosForTest(const QString &id)
{
    const auto it = m_shareAudioRecords.constFind(id);
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (it == m_shareAudioRecords.cend() || it->index < 0 || !bin)
        return;
    if (GstElement *source = gst_bin_get_by_name(
            GST_BIN(bin),
            lightning::shareaudio::branchSourceName(it->index).toUtf8().constData())) {
        gst_element_send_event(source, gst_event_new_eos());
        gst_object_unref(source);
    }
}

int SfuMediaEngine::shareAudioBranchIndexForTest(const QString &id) const
{
    const auto it = m_shareAudioRecords.constFind(id);
    return it == m_shareAudioRecords.cend() ? -2 : it->index;
}

namespace {
void instrumentMicrophoneSource(
    GstElement *bin, const std::shared_ptr<std::atomic<quint64>> &captured)
{
    GstElement *micsrc = gst_bin_get_by_name(GST_BIN(bin), "micsrc");
    if (!micsrc)
        return;
    if (GstPad *out = gst_element_get_static_pad(micsrc, "src")) {
        gst_pad_add_probe(
            out, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
            [](GstPad *, GstPadProbeInfo *info, gpointer) {
                GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
                return event && GST_EVENT_TYPE(event) == GST_EVENT_EOS
                    ? GST_PAD_PROBE_DROP
                    : GST_PAD_PROBE_OK;
            },
            nullptr, nullptr);
        if (captured) {
            gst_pad_add_probe(
                out, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *, gpointer data) {
                    static_cast<std::shared_ptr<std::atomic<quint64>> *>(data)
                        ->get()
                        ->fetch_add(1, std::memory_order_relaxed);
                    return GST_PAD_PROBE_OK;
                },
                new std::shared_ptr<std::atomic<quint64>>(captured),
                [](gpointer data) {
                    delete static_cast<std::shared_ptr<std::atomic<quint64>> *>(
                        data);
                });
        }
        gst_object_unref(out);
    }
    gst_object_unref(micsrc);
}
}

SfuMediaEngine::MicrophoneCapture SfuMediaEngine::testMicrophoneCapture() const
{
    MicrophoneCapture capture;
    capture.source = !m_testMicSource.isEmpty()
        ? QStringLiteral("%1 name=micsrc").arg(m_testMicSource)
        : QStringLiteral("audiotestsrc is-live=true wave=sine freq=440 "
                         "volume=0.05 name=micsrc");
    return capture;
}

namespace {
bool syncStateIfStillParented(GstElement *bin)
{
    GST_STATE_LOCK(bin);
    GstObject *parent = gst_object_get_parent(GST_OBJECT(bin));
    const bool ok = parent && gst_element_sync_state_with_parent(bin);
    if (parent)
        gst_object_unref(parent);
    GST_STATE_UNLOCK(bin);
    return ok;
}

struct MicStart {
    SfuMediaEngine *engine = nullptr;
    QString cid;
    quint64 generation = 0;
    std::shared_ptr<std::atomic<bool>> pending;
};

void micStartFree(gpointer data)
{
    delete static_cast<MicStart *>(data);
}

void micStartAsync(GstElement *bin, gpointer data)
{
    auto *ctx = static_cast<MicStart *>(data);
    QElapsedTimer took;
    took.start();
    const bool ok = syncStateIfStillParented(bin);
    const qint64 ms = took.elapsed();
    ctx->pending->store(false);
    SfuMediaEngine *engine = ctx->engine;
    const QString cid = ctx->cid;
    const quint64 generation = ctx->generation;
    marshal(engine, [engine, cid, generation, ok, ms] {
        engine->noteMicrophoneStarted(cid, generation, ok, ms);
    });
}
}

void SfuMediaEngine::checkMicrophoneRtp(const QString &cid,
                                        quint64 generation)
{
    if (!m_active || generation != m_generation.load() || cid != m_micCid
        || !m_micRtpOut)
        return;
    if (m_micRtpOut->load() > 0) {
        m_lastMicRtpVerdict = QStringLiteral("flowing");
        return;
    }
    if (microphoneStarting()) {
        m_lastMicRtpVerdict = QStringLiteral("starting");
        m_micRtpVerdictOwed = true;
        qCWarning(lcSfuMedia)
            << "the microphone is STILL STARTING after"
            << m_micRtpCheckMs / 1000.0
            << "s: the sound server has not answered its capture; nothing "
               "can be sent until it does";
        return;
    }
    if (m_microphoneMuted) {
        m_lastMicRtpVerdict = QStringLiteral("muted");
        qCInfo(lcSfuMedia)
            << "no RTP from the microphone after 3s — it is MUTED, "
               "which is the valve doing its job";
        return;
    }
    m_lastMicRtpVerdict = QStringLiteral("none");
    qCWarning(lcSfuMedia)
        << "THE MICROPHONE PRODUCED NO RTP AT ALL after 3s while "
           "UNMUTED: the capture chain never started, nothing was "
           "packetised and nobody can hear this device. The call "
           "will otherwise look completely healthy — the track is "
           "published, the transport is connected and the SDP "
           "carries audio.";
}

void SfuMediaEngine::noteMicrophoneStarted(const QString &cid,
                                           quint64 generation, bool ok,
                                           qint64 tookMs)
{
    if (!m_active || generation != m_generation.load() || cid != m_micCid)
        return;
    ++m_micStartsReported;
    if (!ok && m_echoGivenUp) {
    } else if (!ok) {
        qCWarning(lcSfuMedia) << "the microphone did not start; its capture "
                                 "reports why";
    } else if (tookMs >= kSlowCaptureStartWarnMs) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: starting the microphone took" << tookMs
            << "ms (the sound server was slow to answer); the window stayed "
               "responsive meanwhile";
    }
    if (m_micStartTakenOver) {
        m_micStartTakenOver = false;
        return;
    }
    if (m_micSwapDeferred) {
        const MicrophoneCapture capture = *m_micSwapDeferred;
        m_micSwapDeferred.reset();
        qCInfo(lcSfuMedia) << "microphone switch deferred during its start; "
                              "applying it now";
        swapMicrophoneFront(capture);
    }
    runMicrophoneStartDeferrals(cid, generation);
}

void SfuMediaEngine::runMicrophoneStartDeferrals(const QString &cid,
                                                 quint64 generation)
{
    if (m_micDspRescueAfterStart) {
        m_micDspRescueAfterStart = false;
        rescueMicrophoneFromDsp();
    }
    if (m_micRestartAfterStart) {
        m_micRestartAfterStart = false;
        restartMicrophone(cid, generation);
    }
    if (m_micNoiseModeAfterStart) {
        m_micNoiseModeAfterStart = false;
        const bool force = m_micNoiseDspForceAfterStart;
        m_micNoiseDspForceAfterStart = false;
        if (GstElement *bin = m_publishedBins.value(cid, nullptr)) {
            const calls::noise::Mode mode = noiseSuppressionMode();
            const QString did = force
                ? calls::noise::applyDspMode(bin, mode,true,
                                             echoAllowedOnMic())
                : calls::noise::applyMode(
                      bin, mode, calls::noise::DspSwitch::WaitForDenoiser,
                      nullptr, echoAllowedOnMic());
            qCInfo(lcSfuMedia)
                << "microphone noise suppression applied after the start:"
                << did;
        }
    }
    if (m_micEchoAfterStart) {
        m_micEchoAfterStart = false;
        engageEchoCancellation();
    }
    if (m_micRtpVerdictOwed) {
        m_micRtpVerdictOwed = false;
        QTimer::singleShot(m_micRtpCheckMs, this, [this, cid, generation] {
            checkMicrophoneRtp(cid, generation);
        });
    }
}

void SfuMediaEngine::publishAudio(const QString &cid)
{
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    MicrophoneCapture capture = m_testSources
        ? testMicrophoneCapture()
        : resolveMicrophoneCapture(microphoneChoice());
    if (m_testDeviceChannels > 0)
        capture.channels = m_testDeviceChannels;
    const calls::noise::Mode noiseMode = noiseSuppressionMode();
    const bool dspAvailable = calls::noise::webrtcDspAvailable();
    const bool echoCancel = m_micEchoAtStartForTest;
    m_micEchoAtStartForTest = false;
    if (m_echoCancel.load() && !echoProbeLive()) {
        qCInfo(lcSfuMedia)
            << "echo cancellation requested but no playback probe exists yet: "
               "the microphone starts without it and takes it up at the first "
               "remote audio track";
    }
    const QString gainStage = voiceProcessingDescription(
        noiseMode, echoCancel, m_autoGain.load());

    const bool levelAvailable = levelElementAvailable();
    const QString captureLevelStage =
        levelAvailable
            ? QStringLiteral("! level name=miccapturelevel post-messages=true "
                             "interval=%1 ")
                  .arg(kMicCaptureLevelIntervalNs)
            : QString();
    const QString levelStage =
        levelAvailable
            ? QStringLiteral("! level name=miclevel post-messages=true "
                             "interval=%1 ")
                  .arg(kMicLevelIntervalNs)
            : QString();

    if (!lightning::calls::captureMixMatrix(capture.channels).isEmpty()) {
        qCInfo(lcSfuMedia)
            << "multi-input capture device: taking input 1 of"
            << capture.channels
            << "rather than their mean — averaging empty inputs would cost"
            << qRound(20.0 * std::log10(1.0 / capture.channels)) << "dB";
    }

    const QString description =
        microphoneFrontDescription(capture)
        + QStringLiteral(" ! valve name=micvalve drop=%1 "
                         "%4 "
                         "! volume name=micvol volume=%3 "
                         "%5 "
                         "! opusenc name=audioenc "
                         "! rtpopuspay pt=111 ssrc=%2 mtu=%6 "
                         // A capsfilter, not a bare caps string: gst_parse
                         // reads trailing caps as an element name. webrtcbin
                         // builds the m= section from these caps: the ssrc
                         // produces `a=ssrc`/`a=msid`, and clock-rate plus
                         // encoding-params give the RFC 7587 rtpmap
                         // `opus/48000/2`.
                         "! capsfilter name=micrtpcaps "
                         "caps=\"application/x-rtp,media=audio,"
                         "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
                         "encoding-params=(string)2,ssrc=(uint)%2\"")
              .arg(m_microphoneMuted ? QStringLiteral("true")
                                     : QStringLiteral("false"),
                   QString::number(nextPublishSsrc()),
                   QString::number(
                       audioFactorPercent(m_microphoneGain.load()) / 100.0,
                       'f', 3),
                   captureLevelStage + gainStage, levelStage)
              .arg(lightning::rtp::kRtpPayloadMtu);

    qCInfo(lcSfuMedia) << "publishing microphone: valve drop="
                       << m_microphoneMuted << "device-channels="
                       << capture.binding.channels
                       << "dsp=" << dspAvailable << "level=" << levelAvailable;
    qCInfo(lcSfuMedia)
        << "microphone noise suppression mode="
        << calls::noise::modeKey(noiseMode) << "webrtcdsp="
        << (dspAvailable ? (calls::noise::dspSuppressesNoise(noiseMode)
                                ? "suppressing"
                                : "gain-only")
                         : "absent")
        << "denoise-element=" << calls::noise::denoiseElementAvailable();
    if (noiseMode == calls::noise::Mode::Off
        || noiseMode == calls::noise::Mode::WebRtc) {
        Q_EMIT noiseSuppressionStatus(
            QLatin1String(calls::noise::modeKey(noiseMode)),
            noiseMode == calls::noise::Mode::Off || dspAvailable);
    }
    m_lastAudioDescription = description;

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
        qCWarning(lcSfuMedia) << "audio pipeline parse failed:"
                              << (error->message ? error->message : "?");
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        Q_EMIT failed(QStringLiteral("audio_source_failed"));
        return;
    }
    gst_element_set_name(bin, cid.toUtf8().constData());
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), bin)) {
        Q_EMIT failed(QStringLiteral("audio_source_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    applyBindingTo(bin, "micsrc", capture.binding);
    resetMicLevelState();
    m_micCid = cid;
    m_micRestarts = 0;
    m_micRestartPending = false;
    m_micFailureReported = false;
    // A failed source pushes EOS; reaching webrtcbin it would end the track
    // (RTCP BYE) for good. The source is restarted instead; see
    // handleCaptureError().
    // What the device delivers, upstream of the mute valve: a restart that
    // delivered is a restart that worked, muted or not (see
    // micRestartBudgetResets()).
    m_micCaptured = std::make_shared<std::atomic<quint64>>(0);
    m_micCapturedAtRestart = 0;
    instrumentMicrophoneSource(bin, m_micCaptured);
    GstElement *micsrc = gst_bin_get_by_name(GST_BIN(bin), "micsrc");
    GstElement *rtpOut = gst_bin_get_by_name(GST_BIN(bin), "micrtpcaps");
    GstPad *captured = micsrc ? gst_element_get_static_pad(micsrc, "src") : nullptr;
    GstPad *rtp = rtpOut ? gst_element_get_static_pad(rtpOut, "src") : nullptr;
    if (captured && rtp) {
        auto hold = std::make_shared<lightning::calls::CaptureClockHold>();
        hold->report = [](quint64 count, qint64 leadMs) {
            qCWarning(lcSfuMedia)
                << "microphone timestamps ran" << leadMs
                << "ms ahead of the pipeline clock; its packets are released "
                   "on time (packets held so far:"
                << count << ")";
        };
        m_micClockHold = hold;
        lightning::calls::holdCaptureToClock(captured, rtp, hold);
    }
    if (captured)
        gst_object_unref(captured);
    if (rtp)
        gst_object_unref(rtp);
    if (micsrc)
        gst_object_unref(micsrc);
    if (rtpOut)
        gst_object_unref(rtpOut);
    bool levelExtension = false;
    if (levelAvailable && audioLevelExtensionAvailable()) {
        GstElement *meter = gst_bin_get_by_name(GST_BIN(bin), "miclevel");
        GstElement *rtpCaps = gst_bin_get_by_name(GST_BIN(bin), "micrtpcaps");
        if (meter && rtpCaps) {
            g_object_set(meter, "audio-level-meta", TRUE, nullptr);
            GstCaps *caps = nullptr;
            g_object_get(rtpCaps, "caps", &caps, nullptr);
            if (caps) {
                caps = gst_caps_make_writable(caps);
                GValue entry = G_VALUE_INIT;
                GValue text = G_VALUE_INIT;
                gst_value_array_init(&entry, 3);
                g_value_init(&text, G_TYPE_STRING);
                for (const char *part :
                     {"", "urn:ietf:params:rtp-hdrext:ssrc-audio-level",
                      "vad=on"}) {
                    g_value_set_string(&text, part);
                    gst_value_array_append_value(&entry, &text);
                }
                g_value_unset(&text);
                gst_structure_take_value(
                    gst_caps_get_structure(caps, 0),
                    QByteArray("extmap-")
                        .append(QByteArray::number(kAudioLevelExtId))
                        .constData(),
                    &entry);
                g_object_set(rtpCaps, "caps", caps, nullptr);
                gst_caps_unref(caps);
                levelExtension = true;
            }
        }
        if (meter)
            gst_object_unref(meter);
        if (rtpCaps)
            gst_object_unref(rtpCaps);
    }
    qCInfo(lcSfuMedia) << "microphone audio level extension=" << levelExtension
                       << "(the SFU's speaking indicator)";
    const auto gate = std::make_shared<ProbeGate>();
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "audioenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded,false, gate);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    if (srcPad) {
        auto packets = std::make_shared<std::atomic<quint64>>(0);
        auto *rtpCtx = new RtpOutCtx{packets, false};
        gst_pad_add_probe(srcPad,
                          GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER
                                          | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                          countRtpOut, rtpCtx, rtpOutCtxFree);
        m_micRtpOut = packets;
        m_micRtpVerdictOwed = false;
        const quint64 generation = m_generation.load();
        QTimer::singleShot(m_micRtpCheckMs, this, [this, cid, generation] {
            checkMicrophoneRtp(cid, generation);
        });
    }
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    applyPublisherMsid(sinkPad, cid);
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad && !consumePublishLinkFailure())
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "publisher link failed code=" << linked;
        releaseFailedPublishPad(sinkPad);
        m_publishedBins.remove(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("publish_link_failed"));
        return;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    {
        auto pending = std::make_shared<std::atomic<bool>>(true);
        m_asyncStarts.insert(cid, AsyncStart{gate, pending});
        m_micStartPending = pending;
        m_micLiveFront.clear();
        m_micStartDispatchedMs = monotonicMs();
        m_micStartTakenOver = false;
        m_micSwapReplacesStart = false;
        m_micNoiseModeAfterStart = false;
        m_micNoiseDspForceAfterStart = false;
        m_micSwapDeferred.reset();
        m_micRestartAfterStart = false;
        m_micEchoAfterStart = m_echoCancel.load();
        m_micDspRescueAfterStart = false;
        auto *ctx = new MicStart{this, cid, m_generation.load(), pending};
        gst_element_call_async(bin, micStartAsync, ctx, micStartFree);
    }
    ++m_publishedMedia;
    m_publisherEverPublished = true;
    renegotiatePublisher();
}

bool SfuMediaEngine::consumePublishLinkFailure()
{
    if (!m_failNextPublishLink)
        return false;
    m_failNextPublishLink = false;
    qCWarning(lcSfuMedia) << "publisher link failure injected for a test";
    return true;
}

void SfuMediaEngine::releaseFailedPublishPad(GstPad *sinkPad)
{
    if (!sinkPad)
        return;
    if (m_publisher.webrtc)
        gst_element_release_request_pad(m_publisher.webrtc, sinkPad);
    gst_object_unref(sinkPad);
}

quint32 SfuMediaEngine::nextPublishSsrc()
{
    // A distinct non-zero SSRC per track, chosen by us. webrtcbin writes
    // `a=ssrc`/`a=msid` only for an ssrc present in the pad caps, and without
    // them the SFU cannot attribute RTP and times the publication out. SSRCs
    // are public, so the ordinary generator is fine.
    quint32 ssrc = 0;
    while (ssrc == 0)
        ssrc = QRandomGenerator::global()->generate();
    return ssrc;
}

void SfuMediaEngine::applyPublisherMsid(GstPad *sinkPad, const QString &cid)
{
    if (!sinkPad)
        return;
    g_object_set(sinkPad, "msid", cid.toUtf8().constData(), nullptr);
}

bool SfuMediaEngine::shareGpuScalingRequested()
{
    return qEnvironmentVariable("LIGHTNING_SHARE_GPU")
           != QLatin1String("0");
}

QString SfuMediaEngine::missingGpuShareElement()
{
    return firstMissingElement({QByteArrayLiteral("glupload"),
                                QByteArrayLiteral("glcolorconvert"),
                                QByteArrayLiteral("glcolorscale"),
                                QByteArrayLiteral("gldownload")});
}

bool SfuMediaEngine::jpegCameraChainAvailable()
{
    static const bool available = [] {
        const QString desc =
            QStringLiteral("videotestsrc num-buffers=1 ! jpegenc ! ")
            + cameraJpegEntry() + QStringLiteral(" ! fakesink");
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        const bool built = !error && pipeline;
        if (!built) {
            qCInfo(lcSfuMedia)
                << "camera MJPG chain unavailable, cameras will use the raw "
                   "entry:" << (error && error->message ? error->message : "?");
        }
        if (error)
            g_error_free(error);
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        return built;
    }();
    return available;
}

bool SfuMediaEngine::gpuShareChainUsable()
{
    static const bool usable = [] {
        const QString desc =
            QStringLiteral("videotestsrc num-buffers=1 ! ")
            + shareScaleStage(1080, true) + QStringLiteral(" ! fakesink");
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error || !pipeline) {
            qCInfo(lcSfuMedia)
                << "GPU share chain unusable, will use the CPU: build failed:"
                << (error && error->message ? error->message : "?");
            if (error)
                g_error_free(error);
            if (pipeline)
                gst_object_unref(pipeline);
            return false;
        }
        bool ok = gst_element_set_state(pipeline, GST_STATE_PAUSED)
                  != GST_STATE_CHANGE_FAILURE;
        if (ok) {
            GstState state = GST_STATE_NULL;
            const GstStateChangeReturn ret = gst_element_get_state(
                pipeline, &state, nullptr, 3 * GST_SECOND);
            ok = ret == GST_STATE_CHANGE_SUCCESS && state == GST_STATE_PAUSED;
            if (!ok) {
                qCInfo(lcSfuMedia)
                    << "GPU share chain unusable, will use the CPU: the GL "
                       "pipeline did not reach PAUSED (ret=" << int(ret)
                    << "state=" << int(state) << ")";
            }
        } else {
            qCInfo(lcSfuMedia)
                << "GPU share chain unusable, will use the CPU: the GL "
                   "pipeline refused to change state";
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return ok;
    }();
    return usable;
}

QString SfuMediaEngine::firstMissingElement(const QList<QByteArray> &names)
{
    for (const QByteArray &name : names) {
        GstElementFactory *factory = gst_element_factory_find(name.constData());
        if (!factory)
            return QString::fromLatin1(name);
        gst_object_unref(factory);
    }
    return {};
}

/// The capsfilter between the capture and everything after it.
///
/// The PAR pin is required: an unfixated pixel-aspect-ratio fixates to
/// 1/2147483647 and videoscale overflows.
QString SfuMediaEngine::captureEntryFilter(bool gpu)
{
    // GPU: a linear DMA-BUF (the compositor detiles with a cheap blit), else
    // plain system memory. Never `video/x-raw(ANY)` as the fallback: it also
    // matches block-linear DMA-BUFs, which import as empty textures on NVIDIA
    // and give a black share. A preferred field is not a constrained one.
    //
    // The linear modifier is spelled by LEAVING THE SUFFIX OFF on GStreamer
    // 1.28 (`gst_video_dma_drm_fourcc_to_string(fourcc, 0)` is "AR24"), and
    // glupload advertises that form; the older `AR24:0x0000000000000000`
    // intersects to nothing there, so the DMA-BUF branch never matched and a
    // DMA-BUF-only compositor (niri offers BGRx/XR24 only) failed with "no
    // more input formats" (issue #56). Both spellings and both alpha
    // variants are listed. A share whose entry cannot negotiate is retried
    // once in the other memory, see retryShareCaptureInOtherMemory().
    return gpu
        ? QStringLiteral("capsfilter name=capentry caps=\"%1\"")
              .arg(gpuEntryCaps())
        : QStringLiteral("capsfilter caps=\"%1\"")
              .arg(systemMemoryEntryCaps());
}

QString SfuMediaEngine::gpuEntryCaps()
{
    return QStringLiteral("video/x-raw(memory:DMABuf),"
                          "drm-format=(string){ AR24, XR24,"
                          " AR24:0x0000000000000000,"
                          " XR24:0x0000000000000000 },"
                          "pixel-aspect-ratio=(fraction)1/1;"
                          "video/x-raw,"
                          "pixel-aspect-ratio=(fraction)1/1");
}

bool SfuMediaEngine::entryOffersDmaBuf(const GstCaps *caps)
{
    for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
        GstCapsFeatures *features = gst_caps_get_features(caps, i);
        if (features
            && gst_caps_features_contains(features,
                                          "memory:DMABuf"))
            return true;
    }
    return false;
}

QString SfuMediaEngine::gpuSystemMemoryFirstEntry()
{
    return QStringLiteral("capsfilter name=capentry caps=\"%1\"")
        .arg(systemMemoryEntryCaps());
}

QString SfuMediaEngine::systemMemoryEntryCaps()
{
    return QStringLiteral("video/x-raw,pixel-aspect-ratio=(fraction)1/1");
}

bool SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
    const QString &version)
{
    const QVersionNumber parsed = QVersionNumber::fromString(version);
    return !parsed.isNull() && parsed >= QVersionNumber(1, 5, 81);
}

QString SfuMediaEngine::pipewirePluginVersion()
{
    if (!g_pipewirePluginVersionForTest.isEmpty())
        return g_pipewirePluginVersionForTest;
    GstPlugin *plugin =
        gst_registry_find_plugin(gst_registry_get(), "pipewire");
    if (!plugin)
        return {};
    const QString version = QString::fromUtf8(gst_plugin_get_version(plugin));
    gst_object_unref(plugin);
    return version;
}

bool SfuMediaEngine::isCaptureFormatError(quint32 domain, int code,
                                          const QString &message,
                                          const QString &debug)
{
    if (domain == GST_CORE_ERROR)
        return code == GST_CORE_ERROR_NEGOTIATION;
    if (domain == GST_STREAM_ERROR) {
        if (code == GST_STREAM_ERROR_FAILED)
            return debug.contains(QLatin1String("not-negotiated"));
        return code == GST_STREAM_ERROR_FORMAT
               || code == GST_STREAM_ERROR_WRONG_TYPE;
    }
    if (domain == GST_RESOURCE_ERROR) {
        return message.contains(QLatin1String("unhandled format"),
                                Qt::CaseInsensitive)
               || message.contains(QLatin1String("no more input formats"),
                                   Qt::CaseInsensitive);
    }
    return false;
}

bool SfuMediaEngine::shareDmaBufCaptureFailedForTest()
{
    return g_shareDmaBufCaptureFailed.load();
}

void SfuMediaEngine::clearShareDmaBufCaptureFailedForTest()
{
    g_shareDmaBufCaptureFailed.store(false);
    g_shareSystemMemoryCaptureFailed.store(false);
}

void SfuMediaEngine::setPipewirePluginVersionForTest(const QString &version)
{
    g_pipewirePluginVersionForTest = version;
}

GstBuffer *SfuMediaEngine::rewrapShortDmaBuf(GstBuffer *buffer)
{
#if defined(LIGHTNING_HAVE_GST_ALLOCATORS)
    if (!buffer || gst_buffer_n_memory(buffer) != 1)
        return nullptr;
    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    if (!mem || !gst_is_dmabuf_memory(mem))
        return nullptr;
    const GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (!meta || meta->n_planes < 1)
        return nullptr;
    gsize need = meta->offset[0] + gsize(qMax(meta->stride[0], 0))
                                       * (meta->n_planes == 1 ? meta->height
                                                              : 1);
    if (meta->n_planes > 1) {
        const guint last = meta->n_planes - 1;
        need = qMax(need, meta->offset[last] + gsize(qMax(meta->stride[last], 0)));
    }
    gsize memOffset = 0;
    if (gst_memory_get_sizes(mem, &memOffset, nullptr) >= need)
        return nullptr;
    const int fd = gst_dmabuf_memory_get_fd(mem);
    if (fd < 0)
        return nullptr;
    const off_t total = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (total <= 0 || memOffset + need > gsize(total))
        return nullptr;
    const int dupFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0)
        return nullptr;
    GstMemory *full = gst_dmabuf_allocator_alloc(mem->allocator, dupFd,
                                                 gsize(total));
    if (!full) {
        close(dupFd);
        return nullptr;
    }
    if (memOffset > 0)
        gst_memory_resize(full, gssize(memOffset),
                          gsize(total) - memOffset);
    GstBuffer *out = gst_buffer_new();
    gst_buffer_copy_into(out, buffer,
                         GstBufferCopyFlags(GST_BUFFER_COPY_FLAGS
                                            | GST_BUFFER_COPY_TIMESTAMPS
                                            | GST_BUFFER_COPY_META),
                         0, -1);
    gst_buffer_append_memory(out, full);
    if (!gst_buffer_get_video_meta(out)) {
        gst_buffer_add_video_meta_full(out, meta->flags, meta->format,
                                       meta->width, meta->height,
                                       meta->n_planes, meta->offset,
                                       meta->stride);
    }
    gst_buffer_add_parent_buffer_meta(out, buffer);
    return out;
#else
    Q_UNUSED(buffer);
    return nullptr;
#endif
}

bool SfuMediaEngine::isDirectCameraSource(GstObject *source)
{
    if (!source || !GST_IS_ELEMENT(source))
        return false;
    GstElementFactory *factory = gst_element_get_factory(GST_ELEMENT(source));
    return factory && QByteArray(gst_plugin_feature_get_name(
                          GST_PLUGIN_FEATURE(factory))) == "v4l2src";
}

QString SfuMediaEngine::cameraIoMode(quint32 capabilities)
{
#ifdef Q_OS_LINUX
    return capabilities & V4L2_CAP_STREAMING ? QStringLiteral("mmap")
                                            : QStringLiteral("auto");
#else
    Q_UNUSED(capabilities);
    return QStringLiteral("auto");
#endif
}

bool SfuMediaEngine::capsOfferJpeg(const GstCaps *caps,
                                  const GstCaps *selectedEntryCaps)
{
    if (!caps || gst_caps_is_any(caps) || gst_caps_is_empty(caps))
        return true;
    GstCaps *jpeg = gst_caps_new_empty_simple("image/jpeg");
    GstCaps *compatible = selectedEntryCaps
        ? gst_caps_intersect(jpeg, const_cast<GstCaps *>(selectedEntryCaps))
        : gst_caps_ref(jpeg);
    const bool offers = gst_caps_can_intersect(caps, compatible);
    gst_caps_unref(compatible);
    gst_caps_unref(jpeg);
    return offers;
}

GstCaps *SfuMediaEngine::preferredCameraCaps(const GstCaps *advertised,
                                           bool jpegAvailable)
{
    if (!advertised || gst_caps_is_any(advertised)
        || gst_caps_is_empty(advertised))
        return nullptr;
    for (const bool reachesPublishRate : {true, false}) {
        for (const bool bounded : {true, false}) {
            for (const char *type : {"image/jpeg", "video/x-raw"}) {
                if (!jpegAvailable && QByteArray(type) == "image/jpeg")
                    continue;
                GstCaps *filter = bounded
                    ? gst_caps_new_simple(type, "width", GST_TYPE_INT_RANGE, 1,
                                          kCameraSourceMaxWidth, "height",
                                          GST_TYPE_INT_RANGE, 1,
                                          kCameraSourceMaxHeight, nullptr)
                    : gst_caps_new_empty_simple(type);
                if (reachesPublishRate)
                    gst_caps_set_simple(filter, "framerate", GST_TYPE_FRACTION_RANGE,
                                        30, 1, G_MAXINT, 1, nullptr);
                GstCaps *modes = gst_caps_intersect_full(
                    const_cast<GstCaps *>(advertised), filter,
                    GST_CAPS_INTERSECT_FIRST);
                gst_caps_unref(filter);
                struct Mode {
                    GstCaps *caps;
                    qint64 area;
                    int rateNumerator = 0;
                    int rateDenominator = 1;
                };
                QList<Mode> ordered;
                for (guint i = 0; i < gst_caps_get_size(modes); ++i) {
                    GstCaps *mode = gst_caps_copy_nth(modes, i);
                    GstStructure *st = gst_caps_get_structure(mode, 0);
                    gst_structure_fixate_field_nearest_int(
                        st, "width", kCameraSourceMaxWidth);
                    gst_structure_fixate_field_nearest_int(
                        st, "height", kCameraSourceMaxHeight);
                    int width = 0, height = 0;
                    gst_structure_get_int(st, "width", &width);
                    gst_structure_get_int(st, "height", &height);
                    Mode candidate{mode, qint64(width) * height};
                    GstStructure *rate = gst_structure_copy(st);
                    gst_structure_fixate_field_nearest_fraction(rate, "framerate", 30, 1);
                    gst_structure_get_fraction(rate, "framerate",
                                               &candidate.rateNumerator,
                                               &candidate.rateDenominator);
                    gst_structure_free(rate);
                    if (!reachesPublishRate)
                        gst_structure_fixate_field_nearest_fraction(
                            st, "framerate", 30, 1);
                    gst_structure_fixate_field_nearest_fraction(
                        st, "pixel-aspect-ratio", 1, 1);
                    candidate.caps = gst_caps_fixate(candidate.caps);
                    ordered.append(candidate);
                }
                gst_caps_unref(modes);
                std::stable_sort(ordered.begin(), ordered.end(),
                    [bounded, reachesPublishRate](const Mode &a, const Mode &b) {
                        if (!reachesPublishRate) {
                            const int rateOrder = gst_util_fraction_compare(
                                a.rateNumerator, a.rateDenominator,
                                b.rateNumerator, b.rateDenominator);
                            if (rateOrder != 0)
                                return rateOrder > 0;
                        }
                        return bounded ? a.area > b.area : a.area < b.area;
                    });
                if (!ordered.isEmpty()) {
                    GstCaps *preferred = gst_caps_new_empty();
                    for (const Mode &mode : ordered)
                        gst_caps_append(preferred, mode.caps);
                    return preferred;
                }
            }
        }
    }
    return nullptr;
}

QString SfuMediaEngine::cameraJpegEntry(const GstCaps *sourceCaps)
{
    gchar *text = sourceCaps ? gst_caps_to_string(sourceCaps) : nullptr;
    QString caps = text ? QString::fromUtf8(text) : QStringLiteral("image/jpeg");
    g_free(text);
    caps.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    caps.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral(
        "capsfilter caps=\"%1\" ! jpegdec ! videoconvert "
        "! capsfilter caps=\"video/x-raw,pixel-aspect-ratio=(fraction)1/1\"")
        .arg(caps);
}

QString SfuMediaEngine::directCameraRawEntry(const GstCaps *sourceCaps)
{
    if (!sourceCaps)
        return captureEntryFilter(false);
    gchar *text = gst_caps_to_string(sourceCaps);
    QString caps = QString::fromUtf8(text);
    g_free(text);
    caps.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    caps.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("capsfilter caps=\"%1\" ! %2")
        .arg(caps, captureEntryFilter(false));
}

QString SfuMediaEngine::captureCameraEntry(GstElement *bin, bool jpegAvailable,
                                          QString *ioMode)
{
    GstElement *source = gst_bin_get_by_name(GST_BIN(bin), "capsrc");
    GstCaps *selected = nullptr;
    if (source && gst_element_set_state(source, GST_STATE_READY)
                      != GST_STATE_CHANGE_FAILURE) {
        if (GstPad *pad = gst_element_get_static_pad(source, "src")) {
            GstCaps *caps = gst_pad_query_caps(pad, nullptr);
            selected = preferredCameraCaps(caps, jpegAvailable);
            if (selected && capsOfferJpeg(selected)
                && !capsOfferJpeg(caps, selected)) {
                gst_caps_unref(selected);
                selected = preferredCameraCaps(caps, false);
            }
            if (caps)
                gst_caps_unref(caps);
            gst_object_unref(pad);
        }
#ifdef Q_OS_LINUX
        if (isDirectCameraSource(GST_OBJECT(source))
            && g_object_class_find_property(G_OBJECT_GET_CLASS(source),
                                             "device-fd")) {
            int fd = -1;
            g_object_get(source, "device-fd", &fd, nullptr);
            v4l2_capability capability{};
            if (fd >= 0 && ::ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
                const quint32 caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS
                    ? capability.device_caps : capability.capabilities;
                *ioMode = cameraIoMode(caps);
            }
        }
#endif
    }
    if (source) {
        gst_element_set_state(source, GST_STATE_NULL);
        gst_object_unref(source);
    }
    const QString entry = selected
        ? (capsOfferJpeg(selected) ? cameraJpegEntry(selected)
                                   : directCameraRawEntry(selected))
        : directCameraRawEntry();
    if (selected)
        gst_caps_unref(selected);
    return entry;
}

QString SfuMediaEngine::cameraPublishErrorCategory(bool screenShare,
                                                   bool busyError)
{
    if (screenShare)
        return QStringLiteral("screen_share_failed");
    return busyError ? QStringLiteral("camera_busy")
                         : QStringLiteral("camera_failed");
}

QString SfuMediaEngine::shareScaleStage(int maxHeight, bool gpu, bool keepOnGpu)
{
    if (!gpu) {
        return QStringLiteral("videoconvertscale n-threads=4");
    }

    const int h = maxHeight;
    const int w = (h * 16) / 9;
    // glcolorconvert turns the imported DMA_DRM buffer into sampleable RGBA
    // on the GPU. `texture-target=2D` forces the external-OES -> 2D step an
    // EGLImage import needs; without it glcolorscale samples nothing and the
    // picture is black.
    return QStringLiteral("glupload ! glcolorconvert "
                          "! video/x-raw(memory:GLMemory),format=RGBA,"
                          "texture-target=2D "
                          "! glcolorscale "
                          "! capsfilter name=sharegl caps=\""
                          "video/x-raw(memory:GLMemory),"
                          "width=(int)[1,%1],height=(int)[1,%2]\"%3")
        .arg(QString::number(w), QString::number(h),
             keepOnGpu ? QString()
                       : QStringLiteral(" ! gldownload ! videoconvert"));
}

QString SfuMediaEngine::shareLimitsCaps(int maxHeight, int fps, bool glMemory)
{
    // 16:9 width, both as ranges so nothing is upscaled. PAR fixed at 1/1
    // (an unfixated PAR overflows videoscale). A fixed 60/1 behind videorate
    // negotiates fine against the portal's max-framerate=59/1.
    const int shareH = maxHeight;
    const int shareW = (shareH * 16) / 9;
    return QStringLiteral("video/x-raw%4,width=(int)[1,%1],"
                          "height=(int)[1,%2],"
                          "framerate=(fraction)%3/1,"
                          "pixel-aspect-ratio=(fraction)1/1")
        .arg(QString::number(shareW), QString::number(shareH),
             QString::number(fps),
             glMemory ? QStringLiteral("(memory:GLMemory)") : QString());
}

namespace {
int shareBitrateFor(int maxHeight, int fps, int bitrateCapKbps)
{
    const int shareH = maxHeight;
    const int shareW = (shareH * 16) / 9;
    const double shareScale = (double(shareW) * shareH * fps)
                            / (1920.0 * 1080.0 * 30.0);
    const int shareBitrate =
        std::clamp(int(3000000.0 * shareScale), 800000, 6000000);
    return bitrateCapKbps > 0 ? std::min(shareBitrate, bitrateCapKbps * 1000)
                              : shareBitrate;
}
}

const QList<QByteArray> &SfuMediaEngine::extraVaH264Encoders()
{
    static const QList<QByteArray> names = [] {
        QList<QByteArray> found;
        if (!lightning::gst::ensureInitialised())
            return found;
        static const QRegularExpression perDevice(
            QStringLiteral("^varenderD\\d+h264(lp)?enc$"));
        GList *features = gst_registry_get_feature_list(
            gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
        for (GList *l = features; l; l = l->next) {
            const QString name = QString::fromUtf8(
                gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(l->data)));
            if (perDevice.match(name).hasMatch())
                found << name.toUtf8();
        }
        gst_plugin_feature_list_free(features);
        return sortedVaEncoders(found);
    }();
    return names;
}

QList<QByteArray> SfuMediaEngine::sortedVaEncoders(QList<QByteArray> names)
{
    // Low-power entry points first, as for the first device; then by node.
    std::sort(names.begin(), names.end(), [](const QByteArray &a, const QByteArray &b) {
        const bool lpA = a.endsWith("lpenc"), lpB = b.endsWith("lpenc");
        const QByteArray nodeA = a.left(a.indexOf("h264"));
        const QByteArray nodeB = b.left(b.indexOf("h264"));
        if (nodeA != nodeB)
            return nodeA.size() != nodeB.size() ? nodeA.size() < nodeB.size()
                                                : nodeA < nodeB;
        return lpA && !lpB;
    });
    return names;
}

QList<SfuMediaEngine::ShareEncoderCandidate>
SfuMediaEngine::shareEncoderCandidates()
{
    QList<ShareEncoderCandidate> list;
#if defined(Q_OS_MACOS)
    list << ShareEncoderCandidate{
        "vtenc_h264_hw",
        "realtime=true allow-frame-reordering=false bitrate=%1 "
        "max-keyframe-interval=%2",
        "baseline", false};
#else
    list << ShareEncoderCandidate{
        "nvh264enc",
        "preset=low-latency-hq rc-mode=cbr bitrate=%1 gop-size=%2 bframes=0 "
        "zerolatency=true",
        "constrained-baseline", true};
#if defined(Q_OS_WIN)
    list << ShareEncoderCandidate{"nvd3d11h264enc",
                                  "preset=p1 tune=ultra-low-latency rc-mode=cbr "
                                  "bitrate=%1 gop-size=%2 bframes=0",
                                  "constrained-baseline", false}
         << ShareEncoderCandidate{"amfh264enc",
                                  "usage=ultra-low-latency rate-control=cbr "
                                  "bitrate=%1 gop-size=%2",
                                  "constrained-baseline", false}
         << ShareEncoderCandidate{"qsvh264enc",
                                  "rate-control=cbr bitrate=%1 gop-size=%2 "
                                  "b-frames=0",
                                  "constrained-baseline", false}
         << ShareEncoderCandidate{"mfh264enc",
                                  "low-latency=true rc-mode=cbr bitrate=%1 "
                                  "gop-size=%2 bframes=0",
                                  "constrained-baseline", false};
#else
    list << ShareEncoderCandidate{"vah264lpenc",
                                  "rate-control=cbr bitrate=%1 key-int-max=%2 "
                                  "b-frames=0 target-usage=7",
                                  "constrained-baseline", false}
         << ShareEncoderCandidate{"vah264enc",
                                  "rate-control=cbr bitrate=%1 key-int-max=%2 "
                                  "b-frames=0 target-usage=7",
                                  "constrained-baseline", false};
    // The va plugin names only the FIRST render node's elements vah264enc; on a
    // hybrid laptop that node is often a GPU that cannot encode (NVIDIA through
    // nvidia-vaapi-driver), and the integrated GPU's encoder is varenderD129….
    for (const QByteArray &factory : extraVaH264Encoders()) {
        list << ShareEncoderCandidate{factory.constData(),
                                      "rate-control=cbr bitrate=%1 key-int-max=%2 "
                                      "b-frames=0 target-usage=7",
                                      "constrained-baseline", false};
    }
#endif
#endif
    return list;
}

bool SfuMediaEngine::shareHardwareEncodeRequested()
{
    return qEnvironmentVariable("LIGHTNING_SHARE_ENCODER")
        == QLatin1String("hardware");
}

QString SfuMediaEngine::shareEncoderStage(const ShareEncoderCandidate &candidate,
                                          int maxHeight, int fps,
                                          int bitrateCapKbps)
{
    return QStringLiteral("%1 %2")
        .arg(QLatin1String(candidate.factory),
             QString::fromLatin1(candidate.stage)
                 .arg(shareBitrateFor(maxHeight, fps, bitrateCapKbps) / 1000)
                 .arg(2 * fps));
}

SfuMediaEngine::VideoCodecChain
SfuMediaEngine::h264CodecChain(const ShareEncoderCandidate &candidate,
                               bool zeroCopy)
{
    VideoCodecChain chain;
    if (!zeroCopy) {
        chain.beforeEncoder = QStringLiteral("videoconvert ! ");
    }
    chain.afterEncoder = QStringLiteral(
        "! video/x-h264,profile=%1,stream-format=byte-stream,alignment=au "
        "! h264parse name=videoframes config-interval=-1 ")
                             .arg(QLatin1String(candidate.profile));
    chain.payloader = QLatin1String(lightning::rtp::h264PayloaderName());
    chain.rtpCaps = QStringLiteral(
        "encoding-name=H264,packetization-mode=(string)1,"
        "profile-level-id=(string)%1,level-asymmetry-allowed=(string)1,")
                        .arg(QLatin1String(lightning::rtp::kH264ProfileLevelId));
    return chain;
}

namespace {
bool encoderRunsHere(const QString &description, QString *why)
{
    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &error);
    if (error || !pipeline) {
        *why = QString::fromUtf8(error && error->message ? error->message : "?");
        if (error)
            g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }
    bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE;
    if (ok) {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 3 * GST_SECOND,
            GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        if (message && !ok) {
            GError *failure = nullptr;
            gst_message_parse_error(message, &failure, nullptr);
            *why = QString::fromUtf8(failure ? failure->message : "?");
            if (failure)
                g_error_free(failure);
        } else if (!message) {
            *why = QStringLiteral("no answer in 3 s");
        }
        if (message)
            gst_message_unref(message);
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}
}

std::optional<SfuMediaEngine::ShareHardwareEncoder>
SfuMediaEngine::firstWorkingShareEncoder(
    const QList<ShareEncoderCandidate> &candidates, bool glUsable)
{
    for (const ShareEncoderCandidate &candidate : candidates) {
        if (!firstMissingElement({QByteArray(candidate.factory)}).isEmpty())
            continue;
        const QString encode = shareEncoderStage(candidate, 720, 30, 0);
        const QString tail = QStringLiteral(" ! video/x-h264,profile=%1 "
                                            "! fakesink")
                                 .arg(QLatin1String(candidate.profile));
        QString why;
        if (glUsable && candidate.glInput
            && encoderRunsHere(
                QStringLiteral("videotestsrc num-buffers=3 ! ")
                    + shareScaleStage(720, true, true)
                    + QStringLiteral(" ! ") + encode + tail,
                &why))
            return ShareHardwareEncoder{candidate, true};
        if (encoderRunsHere(
                QStringLiteral("videotestsrc num-buffers=3 "
                               "! video/x-raw,format=NV12,width=1280,height=720 "
                               "! videoconvert ! ") + encode + tail,
                &why))
            return ShareHardwareEncoder{candidate, false};
        qCInfo(lcSfuMedia) << "hardware encoder" << candidate.factory
                           << "cannot run here:" << why;
    }
    return std::nullopt;
}

namespace {
std::atomic<int> g_shareEncoderProbe{0};
std::optional<SfuMediaEngine::ShareHardwareEncoder> g_shareEncoder;
}

void SfuMediaEngine::probeShareHardwareEncoder()
{
    int unasked = 0;
    if (!g_shareEncoderProbe.compare_exchange_strong(unasked, 1))
        return;
    std::thread([] {
        g_shareEncoder = firstWorkingShareEncoder(
            shareEncoderCandidates(),
            shareGpuScalingRequested()
                && missingGpuShareElement().isEmpty() && gpuShareChainUsable());
        g_shareEncoderProbe.store(2);
    }).detach();
}

std::optional<SfuMediaEngine::ShareHardwareEncoder>
SfuMediaEngine::shareHardwareEncoder()
{
    return g_shareEncoderProbe.load() == 2 ? g_shareEncoder : std::nullopt;
}

void SfuMediaEngine::setShareHardwareEncoderForTest(
    const std::optional<ShareHardwareEncoder> &encoder)
{
    g_shareEncoder = encoder;
    g_shareEncoderProbe.store(2);
}

bool SfuMediaEngine::shareHardwareEncoderFailedForTest()
{
    return g_shareHardwareEncoderFailed.load();
}

void SfuMediaEngine::clearShareHardwareEncoderFailedForTest()
{
    g_shareHardwareEncoderFailed.store(false);
}

QString SfuMediaEngine::h264DecoderElement()
{
    for (const char *name : {"avdec_h264", "openh264dec"}) {
        if (firstMissingElement({QByteArray(name)}).isEmpty())
            return QLatin1String(name);
    }
    return {};
}

QString SfuMediaEngine::shareEncoderStage(int maxHeight, int fps,
                                          int bitrateCapKbps)
{
    const int shareBitrate = shareBitrateFor(maxHeight, fps, bitrateCapKbps);
    return QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                          "cpu-used=4 static-threshold=100 "
                          "keyframe-max-dist=%2 "
                          "end-usage=cbr target-bitrate=%1")
        .arg(QString::number(shareBitrate), QString::number(2 * fps));
}

QString SfuMediaEngine::cpuFallbackScaleStage(bool screenShare,
                                              int shareMaxHeight)
{
    return screenShare ? shareScaleStage(shareMaxHeight, false)
                       : QStringLiteral("videoconvert ! videoscale");
}

QString SfuMediaEngine::videoRateStage(bool screenShare)
{
    Q_UNUSED(screenShare);
    // One stage for camera and share. Not `compositor`: it is not a scaler
    // and crops a 4K capture to the top-left quarter of a 1080p canvas.
    //
    // videorate emits nothing until a second buffer arrives, so a still
    // window would never publish; tickShareKeepAlive() re-pushes the last
    // picture into this element (hence the name). GAP events, queues,
    // identity, capssetter and source keep-alive properties were all tried
    // and do not help.
    //
    // skip-to-first: videorate starts its clock at segment start, and sources
    // that stamp pipeline running time (ksvideosrc, gdiscreencapsrc) would
    // otherwise get one duplicate per frame interval of call age, freezing a
    // late-started camera on a single picture.
    return QStringLiteral("videorate name=vidrate skip-to-first=true");
}

QString SfuMediaEngine::portalCameraEntry()
{
    return QStringLiteral(
        "capsfilter caps=\"video/x-raw,width=(int)[640,1920],"
        "height=(int)[360,1080],pixel-aspect-ratio=(fraction)1/1\"");
}

QString SfuMediaEngine::cameraLimitsCaps(bool portal, int maxHeight)
{
    const int h = snapCameraHeight(maxHeight);
    const int w = (h * 16) / 9;
    const QString size = QStringLiteral("video/x-raw,width=(int)[1,%1],"
                                        "height=(int)[1,%2],")
                             .arg(QString::number(w), QString::number(h));
    return portal
        ? size + QStringLiteral("pixel-aspect-ratio=(fraction)1/1")
        : size + QStringLiteral("framerate=(fraction)30/1,"
                                "pixel-aspect-ratio=(fraction)1/1");
}

QString SfuMediaEngine::cameraEncoderStage(int maxHeight)
{
    int bitrate = 1700000;
    switch (snapCameraHeight(maxHeight)) {
    case 360:
        bitrate = 700000;
        break;
    case 480:
        bitrate = 1100000;
        break;
    case 1080:
        bitrate = 3000000;
        break;
    default:
        break;
    }
    return QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                          "cpu-used=2 static-threshold=0 "
                          "keyframe-max-dist=30 "
                          "end-usage=cbr target-bitrate=%1")
        .arg(bitrate);
}

QString SfuMediaEngine::cameraRateStage(bool portal)
{
    return portal
        ? QStringLiteral("videorate name=vidrate skip-to-first=true "
                         "max-rate=30")
        : videoRateStage(false);
}

namespace {

struct QueueProbeResult {
    QString spec;
    bool isControl = false;
    qint64 settledMs = -1;
    qint64 peakMs = -1;
    qint64 realtimeMs = -1;
    qint64 recoveredMs = -1;
    bool ran = false;
    QString error;
};

qint64 queueLevelMs(GstElement *queue)
{
    guint64 ns = 0;
    g_object_get(queue, "current-level-time", &ns, nullptr);
    return static_cast<qint64>(ns / GST_MSECOND);
}

QueueProbeResult runOneQueueProbe(const QString &spec, bool isControl)
{
    QueueProbeResult out;
    out.spec = spec;
    out.isControl = isControl;

    const QString desc =
        QStringLiteral("audiotestsrc is-live=true samplesperbuffer=480 ! "
                       "audio/x-raw,rate=48000,channels=1 ! %1 name=q ! "
                       "identity name=brake sync=false ! "
                       "fakesink sync=false async=false")
            .arg(spec);

    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(desc.toUtf8().constData(), &error);
    if (error || !pipeline) {
        out.error = error && error->message
            ? QString::fromUtf8(error->message)
            : QStringLiteral("could not build the probe pipeline");
        if (error)
            g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return out;
    }

    GstElement *queue = gst_bin_get_by_name(GST_BIN(pipeline), "q");
    GstElement *brake = gst_bin_get_by_name(GST_BIN(pipeline), "brake");
    if (!queue || !brake) {
        out.error = QStringLiteral("the probe pipeline lost its own elements");
        if (queue)
            gst_object_unref(queue);
        if (brake)
            gst_object_unref(brake);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return out;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND)
        == GST_STATE_CHANGE_FAILURE) {
        out.error = QStringLiteral("the probe pipeline would not reach "
                                   "PLAYING");
        gst_object_unref(queue);
        gst_object_unref(brake);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return out;
    }

    const auto sampleFor = [&](int ms, qint64 *peak) {
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + ms;
        qint64 last = -1;
        while (QDateTime::currentMSecsSinceEpoch() < until) {
            const qint64 level = queueLevelMs(queue);
            last = level;
            if (peak && level > *peak)
                *peak = level;
            QThread::msleep(20);
        }
        return last;
    };

    out.settledMs = sampleFor(1500, nullptr);

    g_object_set(brake, "sleep-time", guint(40000), nullptr);
    out.peakMs = 0;
    sampleFor(2000, &out.peakMs);

    g_object_set(brake, "sleep-time", guint(10000), nullptr);
    out.realtimeMs = sampleFor(3000, nullptr);

    g_object_set(brake, "sleep-time", guint(0), nullptr);
    out.recoveredMs = sampleFor(3000, nullptr);

    out.ran = true;
    gst_object_unref(queue);
    gst_object_unref(brake);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return out;
}

}

int SfuMediaEngine::runQueueSelfTest(QString *report)
{
    QString text;
    QTextStream out(&text);

    const QString produced = videoPipelineDescription(
        QStringLiteral("videotestsrc is-live=true"),
        videoRateStage(false),
        QStringLiteral("video/x-raw,width=(int)[1,1280],height=(int)[1,720],"
                       "framerate=(fraction)30/1,"
                       "pixel-aspect-ratio=(fraction)1/1"),
        QStringLiteral("vp8enc"), QString(), 1u,
        QStringLiteral("videoconvert ! videoscale"),
        captureEntryFilter(false));

    static const QRegularExpression queueElement(
        QStringLiteral("queue(?:\\s+[a-z-]+=[^\\s!]+)*"));
    QStringList specs;
    QRegularExpressionMatchIterator it = queueElement.globalMatch(produced);
    while (it.hasNext()) {
        const QString spec = it.next().captured(0).trimmed();
        if (!specs.contains(spec))
            specs.append(spec);
    }

    out << "queue self-test: the voice-delay property, measured directly\n\n";
    out << "GStreamer " << gst_version_string() << "\n";
    out << "queues measured here: " << specs.size()
        << ", taken from the description videoPipelineDescription()\n"
           "actually returns. NOT measured: the receive-side queues and the\n"
           "self-view branch (built inside member functions, not reachable\n"
           "from a static function), the share-audio queues, and the whole\n"
           "1:1 lane in GstCallMediaBackend. Those are covered STATICALLY by\n"
           "`everyLiveQueueIsBoundedAndLeaky`, which asserts a per-file count\n"
           "across all three media sources (11 queues) — that is a different\n"
           "claim from the behavioural one below, and neither substitutes for\n"
           "the other.\n\n";
    if (specs.isEmpty()) {
        out << "RESULT: nothing was measured — no queue could be read out of "
               "the pipeline this build\nproduces.\n"
            << "VERDICT: unmeasurable\n";
        out.flush();
        if (report)
            *report = text;
        return 2;
    }

    QList<QueueProbeResult> results;
    for (const QString &spec : specs)
        results.append(runOneQueueProbe(spec,false));
    results.append(runOneQueueProbe(QStringLiteral("queue"),
true));

    bool ok = true;
    bool measured = true;
    qint64 controlHeldMs = -1;
    for (const QueueProbeResult &r : results) {
        out << (r.isControl ? "CONTROL " : "SHIPPED ") << r.spec << "\n";
        if (!r.ran) {
            out << "    could not run: " << r.error << "\n\n";
            measured = false;
            continue;
        }
        if (r.isControl)
            controlHeldMs = r.realtimeMs;
        if (r.spec.contains(QStringLiteral("max-size-buffers="))
            && !r.spec.contains(QStringLiteral("max-size-time="))) {
            out << "    NOTE: this bound is in BUFFERS, and this probe uses "
                   "10 ms buffers.\n"
                   "          On the video path the same bound is 4 frames = "
                   "133 ms at 30 fps,\n"
                   "          800 ms at 5 fps. The milliseconds below are the "
                   "PROBE'S, not\n          the shipped path's.\n";
        }
        out << "    settled                       " << r.settledMs
            << " ms\n"
            << "    peak while starved            " << r.peakMs << " ms\n"
            << "    consumer restored to realtime " << r.realtimeMs
            << " ms\n"
            << "    consumer let run free         " << r.recoveredMs
            << " ms\n";
        if (!r.isControl) {
            const bool bounded = r.peakMs <= 450;
            const bool notHeld = r.realtimeMs <= 350;
            out << "    bounded while starved:        "
                << (bounded ? "yes" : "NO") << "\n"
                << "    gave it back at realtime:     "
                << (notHeld ? "yes" : "NO") << "\n";
            if (!bounded || !notHeld)
                ok = false;
        }
        out << "\n";
    }

    static constexpr qint64 kControlMustHoldMs = 500;
    if (measured && controlHeldMs < kControlMustHoldMs) {
        out << "the CONTROL queue held only " << controlHeldMs
            << " ms with its consumer starved and then restored.\n"
               "A GStreamer default queue holds a full second, so the "
               "starvation did not\nhappen and nothing here was "
               "demonstrated.\n\n";
        measured = false;
    }

    if (!measured) {
        out << "RESULT: nothing was measured — see the rows above. This is "
               "NOT a statement\nabout any queue in this build.\n"
            << "VERDICT: unmeasurable\n";
        out.flush();
        if (report)
            *report = text;
        return 2;
    }

    out << (ok
                ? "RESULT: every queue this build ships stayed within its "
                  "bound while its\nconsumer was starved, and was not still "
                  "holding a backlog once the consumer\nwas merely keeping "
                  "up. Read the CONTROL line beside them: that is what the\n"
                  "GStreamer default does on this same machine, in this same "
                  "run.\n"
                : "RESULT: a live queue was still holding a backlog after its "
                  "consumer had\ncaught up to real time. That is permanent "
                  "added delay for the rest of a call.\nCompare it against "
                  "the CONTROL line above.\n")
        << (ok ? "VERDICT: pass\n" : "VERDICT: fail\n");
    out.flush();
    if (report)
        *report = text;
    return ok ? 0 : 1;
}

int SfuMediaEngine::BlockedRunPolicy::failurePercent() const
{
    if (observed < kMinObserved)
        return -1;
    return failures * 100 / observed;
}

bool SfuMediaEngine::BlockedRunPolicy::note(bool failed, bool *raise)
{
    if (observed == kWindow) {
        if (recent[static_cast<size_t>(next)])
            --failures;
    } else {
        ++observed;
    }
    recent[static_cast<size_t>(next)] = failed;
    if (failed)
        ++failures;
    next = (next + 1) % kWindow;

    const int percent = failurePercent();
    if (percent < 0)
        return false;
    if (!announced && percent >= kRaisePercent) {
        announced = true;
        if (raise)
            *raise = true;
        return true;
    }
    if (announced && percent <= kClearPercent) {
        announced = false;
        if (raise)
            *raise = false;
        return true;
    }
    return false;
}

QString SfuMediaEngine::selfViewBranch(bool glMemory)
{
    return QStringLiteral("t. ! queue max-size-buffers=2 leaky=downstream %1"
                          "! videoconvert ! video/x-raw,format=RGBA "
                          "! appsink name=selfvidsink emit-signals=true "
                          "sync=false async=false max-buffers=1 drop=true ")
        .arg(glMemory ? QStringLiteral("! gldownload ") : QString());
}

QString SfuMediaEngine::videoPipelineDescription(const QString &source,
                                                const QString &rateStage,
                                                const QString &limits,
                                                const QString &encoder,
                                                const QString &selfView,
                                                quint32 ssrc,
                                                const QString &scaleStage,
                                                const QString &entryFilter,
                                                const VideoCodecChain &codec)
{
    return QStringLiteral(
               "%1 name=capsrc "
               "! %9 "
               "! queue max-size-buffers=4 leaky=downstream "
               "! %8 ! %7 "
               "! capsfilter name=publimits caps=\"%2\" "
               "! tee name=t %4"
               "t. ! queue max-size-buffers=0 max-size-bytes=0 "
               "max-size-time=100000000 leaky=downstream "
               "! valve name=vidvalve drop=false ! %11%3 name=videoenc %12"
               "! %6 pt=%14 ssrc=%5 mtu=%10 "
               "! capsfilter caps=\"application/x-rtp,media=video,"
               "%13payload=%14,clock-rate=(int)90000,"
               "ssrc=(uint)%5\"")
        .arg(source, limits, encoder, selfView, QString::number(ssrc),
             codec.payloader.isEmpty()
                 ? QString::fromLatin1(lightning::rtp::vp8PayloaderName())
                 : codec.payloader,
             rateStage, scaleStage, entryFilter)
        .arg(lightning::rtp::kRtpPayloadMtu)
        .arg(codec.beforeEncoder, codec.afterEncoder,
             codec.rtpCaps.isEmpty() ? QStringLiteral("encoding-name=VP8,")
                                     : codec.rtpCaps)
        .arg(codec.payloader.isEmpty() ? 96 : 102);
}

QString SfuMediaEngine::trackSidFromMsid(const QString &msid)
{
    const QString streamId = msid.section(QLatin1Char(' '), 0, 0).trimmed();
    const int packed = streamId.indexOf(QLatin1Char('|'));
    if (packed >= 0) {
        const QString tail = streamId.mid(packed + 1);
        if (tail.startsWith(QLatin1String("TR")))
            return tail;
    }
    const QString trackId = msid.section(QLatin1Char(' '), 1, 1).trimmed();
    if (trackId.startsWith(QLatin1String("TR")))
        return trackId;
    return {};
}

QString SfuMediaEngine::participantIdFromMsid(const QString &msid)
{
    QString streamId = msid.section(QLatin1Char(' '), 0, 0).trimmed();
    const int packed = streamId.indexOf(QLatin1Char('|'));
    if (packed >= 0)
        streamId.truncate(packed);
    return streamId;
}

QString SfuMediaEngine::localCameraStreamId()
{
    return QStringLiteral("local:camera");
}

QString SfuMediaEngine::localScreenStreamId()
{
    return QStringLiteral("local:screen");
}

bool SfuMediaEngine::elementAvailable(const char *name)
{
    if (!name || !*name)
        return false;
    if (!lightning::gst::ensureInitialised())
        return false;
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

QString SfuMediaEngine::screenShareSource(int nodeId, int pipewireFd,
                                         quint64 windowHandle,
                                         const QRect &captureRect)
{
    // pipewiresrc needs the portal's `fd`: without it `path` resolves against
    // our own default remote and never produces a buffer.
    //
    // Do not add `keepalive-time` or raise `min-buffers` here: both were
    // tried and stopped the capture entirely. The still-screen stall is
    // handled at the rate stage (see videoRateStage()).
    //
    // Windows and macOS have no portal; there `nodeId` is a monitor index.
#if defined(Q_OS_WIN)
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    if (windowHandle != 0) {
        return QStringLiteral("%1 hwnd=%2")
            .arg(QLatin1String(lightning::wincap::windowCaptureSrcName()))
            .arg(windowHandle);
    }
    return QStringLiteral("gdiscreencapsrc monitor=%1 cursor=true")
        .arg(nodeId < 0 ? 0 : nodeId);
#elif defined(Q_OS_MACOS)
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    return QStringLiteral("avfvideosrc capture-screen=true "
                          "capture-screen-cursor=true device-index=%1")
        .arg(nodeId < 0 ? 0 : nodeId);
#else
    if (captureRect.isValid()) {
        return QStringLiteral("%1 startx=%2 starty=%3 endx=%4 endy=%5 "
                              "use-damage=false show-pointer=true "
                              "do-timestamp=true")
            .arg(QLatin1String(x11ScreenCaptureElementName()))
            .arg(captureRect.x())
            .arg(captureRect.y())
            .arg(captureRect.right())
            .arg(captureRect.bottom());
    }
    if (pipewireFd >= 0) {
        // Explicit `min-buffers=1`: the default was 8 up to gst-plugin-pipewire
        // 1.4 and compositors offer at most ~4 buffers, which PipeWire 1.6
        // rejects with "error alloc buffers: Invalid argument". 1 intersects
        // any range, whatever plugin version is bundled.
        return QStringLiteral(
                   "pipewiresrc fd=%1 path=%2 min-buffers=1 do-timestamp=true")
            .arg(pipewireFd).arg(nodeId);
    }
    return QStringLiteral("pipewiresrc path=%1 min-buffers=1 do-timestamp=true")
        .arg(nodeId);
#endif
}

SfuMediaEngine::CameraBinding
SfuMediaEngine::resolveCameraBinding(const QString &source,
                                     const DeviceChoice &camera)
{
    CameraBinding out;
    bool listed = false;
    const auto candidates = monitorCandidates("Video/Source", &listed);
    out.binding = lightning::calls::resolveDeviceBinding(
        lightning::calls::CaptureKind::Camera,
        source.section(QLatin1Char(' '), 0, 0), camera.id, camera.description,
        candidates);
    if (out.binding.refused) {
        qCWarning(lcSfuMedia)
            << (listed ? "the chosen camera is not among the devices "
                         "GStreamer lists"
                       : "the camera list could not be read")
            << "- opening no camera rather than the default";
        out.refusal = listed ? QStringLiteral("camera_unavailable")
                             : QStringLiteral("camera_list_unavailable");
    }
    return out;
}

QString SfuMediaEngine::cameraSource(int pipewireFd)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    if (pipewireFd >= 0) {
        return QStringLiteral(
                   "pipewiresrc fd=%1 min-buffers=1 always-copy=true "
                   "do-timestamp=true")
            .arg(pipewireFd);
    }
#else
    Q_UNUSED(pipewireFd);
#endif
#if defined(Q_OS_WIN)
    return QStringLiteral("ksvideosrc");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("avfvideosrc");
#else
    // `v4l2src`, not `autovideosrc`: on a PipeWire desktop autodetect picks
    // pipewiresrc by rank, with no target, and the camera never delivers.
    //
    // Probe the bound device in READY before preferring MMAP. Auto retains
    // read/write-only devices and drivers whose capabilities cannot be read.
    return QStringLiteral("v4l2src");
#endif
}

void SfuMediaEngine::publishVideo(const QString &cid, bool screenShare,
                                  int nodeId, int pipewireFd,
                                  quint64 windowHandle,
                                  const QRect &captureRect)
{
    struct PortalFd {
        int fd;
        ~PortalFd()
        {
            if (fd >= 0)
                ::close(fd);
        }
        void release() { fd = -1; }
    } portalFd{pipewireFd};
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    QString source;
    lightning::calls::DeviceBinding cameraBinding;
    if (m_testSources) {
        source = m_testVideoSource.isEmpty()
            ? QStringLiteral("videotestsrc is-live=true pattern=smpte")
            : m_testVideoSource;
    } else if (screenShare) {
        if (nodeId < 0 && windowHandle == 0 && !captureRect.isValid()) {
            Q_EMIT failed(QStringLiteral("screen_share_no_source"));
            return;
        }
        source = screenShareSource(nodeId, pipewireFd, windowHandle,
                                   captureRect);
    } else {
        source = cameraSource(pipewireFd);
        qCInfo(lcSfuMedia) << "camera source="
                           << (pipewireFd >= 0 ? "xdg camera portal "
                                                 "(pipewiresrc)"
                                               : "direct device");
        const DeviceChoice camera = cameraChoice();
        const auto refuseCamera = [this, cid](const QString &category) {
            QMetaObject::invokeMethod(
                this,
                [this, cid, category] { Q_EMIT publishFailed(cid, category); },
                Qt::QueuedConnection);
        };
        if (pipewireFd < 0 && camera.id.isEmpty() && camera.preferredMissing) {
            qCWarning(lcSfuMedia)
                << "the chosen camera is not connected; opening no camera "
                   "rather than the default";
            refuseCamera(QStringLiteral("camera_unavailable"));
            return;
        }
        if (pipewireFd < 0 && !camera.id.isEmpty()) {
            const CameraBinding resolved = resolveCameraBinding(source, camera);
            cameraBinding = resolved.binding;
            if (cameraBinding.refused) {
                refuseCamera(resolved.refusal);
                return;
            }
        }
    }

    const bool portalCamera = !screenShare && pipewireFd >= 0;
    QString limits = screenShare
        ? shareLimitsCaps(m_shareMaxHeight, m_shareFps)
        : cameraLimitsCaps(portalCamera, m_cameraMaxHeight);
    QString encoder = screenShare
        ? shareEncoderStage(m_shareMaxHeight, m_shareFps,
                            m_shareBitrateCapKbps)
        : cameraEncoderStage(m_cameraMaxHeight);
    const bool wantSelfView = !m_testSources || m_testSelfView;
    QString selfView = wantSelfView ? selfViewBranch() : QString();
    const QString scaleStage = screenShare
        ? shareScaleStage(m_shareMaxHeight, shareGpuScalingRequested())
        : QStringLiteral("videoconvert ! videoscale");
    const QString cpuScaleStage =
        cpuFallbackScaleStage(screenShare, m_shareMaxHeight);
    bool useGpu = screenShare && shareGpuScalingRequested();
    if (useGpu) {
        const QString missing = missingGpuShareElement();
        if (!missing.isEmpty()) {
            qCInfo(lcSfuMedia)
                << "screen share falling back to the CPU: GStreamer element"
                << missing << "is not available in this build";
            useGpu = false;
        }
    }
    if (useGpu && !gpuShareChainUsable()) {
        qCInfo(lcSfuMedia)
            << "screen share falling back to the CPU: the GL chain is "
               "present but cannot run on this machine";
        useGpu = false;
    }

    QString scaleStageInUse = useGpu ? scaleStage : cpuScaleStage;

    VideoCodecChain codec;
    QString hardwareFactory;
    bool glEncode = false;
    if (screenShare && shareHardwareEncodeRequested()) {
        const auto hardware = g_shareHardwareEncoderFailed.load()
            ? std::nullopt
            : shareHardwareEncoder();
        if (hardware) {
            hardwareFactory = QLatin1String(hardware->candidate.factory);
            glEncode = useGpu && hardware->zeroCopy;
            codec = h264CodecChain(hardware->candidate, glEncode);
            encoder = shareEncoderStage(hardware->candidate, m_shareMaxHeight,
                                        m_shareFps, m_shareBitrateCapKbps);
            if (glEncode) {
                scaleStageInUse =
                    shareScaleStage(m_shareMaxHeight, true, true);
                limits = shareLimitsCaps(m_shareMaxHeight, m_shareFps, true);
                if (wantSelfView)
                    selfView = selfViewBranch(true);
            }
        } else {
            qCInfo(lcSfuMedia)
                << "screen share encoding in software: no hardware encoder "
                   "works here, or the probe has not finished";
        }
    }
    const auto dropHardwareEncoder = [&] {
        hardwareFactory.clear();
        glEncode = false;
        codec = {};
        encoder = shareEncoderStage(m_shareMaxHeight, m_shareFps,
                                    m_shareBitrateCapKbps);
        limits = shareLimitsCaps(m_shareMaxHeight, m_shareFps);
        if (wantSelfView)
            selfView = selfViewBranch();
    };

    const bool tryJpeg =
        !screenShare && pipewireFd < 0 && jpegCameraChainAvailable();
    bool dmaBufEntry = false;
    if (useGpu && screenShare) {
        const QString pipewireVersion = pipewirePluginVersion();
        dmaBufEntry = pipewireSrcKeepsFormatOnRenegotiation(pipewireVersion)
            ? !g_shareDmaBufCaptureFailed.load()
            : g_shareSystemMemoryCaptureFailed.load();
        qCInfo(lcSfuMedia) << "screen share capture starts in"
                           << (dmaBufEntry ? "DMA-BUF" : "system memory")
                           << "pipewiresrc="
                           << (pipewireVersion.isEmpty() ? QStringLiteral("none")
                                                         : pipewireVersion);
    }
    const QString shareEntry = !useGpu ? captureEntryFilter(false)
        : dmaBufEntry                  ? captureEntryFilter(true)
                                       : gpuSystemMemoryFirstEntry();
    QString entryInUse = tryJpeg      ? cameraJpegEntry()
                         : portalCamera ? portalCameraEntry()
                         : !screenShare ? directCameraRawEntry()
                                        : shareEntry;
    QString description = videoPipelineDescription(
        source, screenShare ? videoRateStage(true) : cameraRateStage(portalCamera),
            limits, encoder, selfView,
        nextPublishSsrc(), scaleStageInUse, entryInUse, codec);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error && tryJpeg) {
        qCWarning(lcSfuMedia)
            << "camera MJPG pipeline failed to build, falling back to raw:"
            << (error->message ? error->message : "?");
        g_clear_error(&error);
        if (bin) {
            gst_object_unref(bin);
            bin = nullptr;
        }
        entryInUse = screenShare ? shareEntry
                                 : directCameraRawEntry();
        description = videoPipelineDescription(
            source, screenShare ? videoRateStage(true) : cameraRateStage(portalCamera),
            limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, entryInUse);
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
    }
    if (error && !hardwareFactory.isEmpty()) {
        qCWarning(lcSfuMedia)
            << "screen share hardware encoder pipeline failed to build, "
               "falling back to software:"
            << (error->message ? error->message : "?");
        g_clear_error(&error);
        if (bin) {
            gst_object_unref(bin);
            bin = nullptr;
        }
        dropHardwareEncoder();
        scaleStageInUse = useGpu ? scaleStage : cpuScaleStage;
        description = videoPipelineDescription(
            source, videoRateStage(true), limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, entryInUse);
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
    }
    if (error && useGpu) {
        qCWarning(lcSfuMedia)
            << "screen share GPU pipeline failed to build, falling back to "
               "the CPU:" << (error->message ? error->message : "?");
        g_clear_error(&error);
        if (bin) {
            gst_object_unref(bin);
            bin = nullptr;
        }
        useGpu = false;
        dropHardwareEncoder();
        scaleStageInUse = cpuScaleStage;
        description = videoPipelineDescription(
            source, screenShare ? videoRateStage(true) : cameraRateStage(portalCamera),
            limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, captureEntryFilter(false));
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
    }
    if (error) {
        qCWarning(lcSfuMedia) << "video pipeline parse failed:"
                              << (error->message ? error->message : "?");
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        Q_EMIT failed(screenShare ? QStringLiteral("screen_share_failed")
                                  : QStringLiteral("camera_failed"));
        return;
    }
    if (screenShare) {
        qCInfo(lcSfuMedia) << "screen share scaling on"
                           << (useGpu ? "the GPU" : "the CPU");
        m_shareGlEncode = glEncode;
        m_shareEncoderCid = cid;
        const QString path = glEncode ? QStringLiteral("gpu-zero-copy")
            : useGpu ? QStringLiteral("gpu-readback")
                     : QStringLiteral("cpu");
        m_shareEncoderInUse =
            (hardwareFactory.isEmpty() ? QStringLiteral("vp8enc")
                                       : hardwareFactory)
            + QLatin1Char(' ') + path;
        qCInfo(lcSfuMedia) << "screen share encoder=" << m_shareEncoderInUse;
    }
    if (!screenShare && !cameraBinding.isEmpty()
        && !applyBindingTo(bin, "capsrc", cameraBinding)) {
        gst_object_unref(bin);
        QMetaObject::invokeMethod(
            this,
            [this, cid] {
                Q_EMIT publishFailed(cid,
                                     QStringLiteral("camera_unavailable"));
            },
            Qt::QueuedConnection);
        return;
    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr bool kProbeCameraFormats = false;
#else
    constexpr bool kProbeCameraFormats = true;
#endif
    QString ioMode = QStringLiteral("auto");
    const QString selectedEntry = kProbeCameraFormats && !screenShare && !portalCamera
        ? captureCameraEntry(bin, tryJpeg && entryInUse == cameraJpegEntry(), &ioMode)
        : entryInUse;
    if (!screenShare)
        qCInfo(lcSfuMedia) << "camera chain="
                           << (portalCamera ? "portal-raw-range"
                               : selectedEntry.contains(QLatin1String("jpegdec"))
                                   ? "mjpg" : "raw")
                           << "(jpeg elements"
                           << (jpegCameraChainAvailable() ? "present" : "absent")
                           << ")";
    if (selectedEntry != entryInUse || ioMode != QLatin1String("auto")) {
        gst_object_unref(bin);
        entryInUse = selectedEntry;
        if (source.section(QLatin1Char(' '), 0, 0) == QLatin1String("v4l2src"))
            source += QStringLiteral(" io-mode=%1").arg(ioMode);
        description = videoPipelineDescription(
            source, cameraRateStage(portalCamera), limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, entryInUse);
        error = nullptr;
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
        if (error) {
            qCWarning(lcSfuMedia) << "video pipeline parse failed:"
                                  << (error->message ? error->message : "?");
            g_error_free(error);
            if (bin)
                gst_object_unref(bin);
            Q_EMIT failed(QStringLiteral("camera_failed"));
            return;
        }
        if (!cameraBinding.isEmpty()
            && !applyBindingTo(bin, "capsrc", cameraBinding)) {
            gst_object_unref(bin);
            QMetaObject::invokeMethod(
                this,
                [this, cid] {
                    Q_EMIT publishFailed(cid,
                                         QStringLiteral("camera_unavailable"));
                },
                Qt::QueuedConnection);
            return;
        }
    }
    gst_element_set_name(bin, cid.toUtf8().constData());
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), bin)) {
        Q_EMIT failed(QStringLiteral("camera_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    if (pipewireFd >= 0)
        m_publishedFds.insert(cid, pipewireFd);
    portalFd.release();
    const auto streamInfo = attachStreamInfo(bin);
    streamInfo->outgoing = true;
    streamInfo->screen = screenShare;
    streamInfo->source = streamInfoFactory(bin, "capsrc");
    streamInfo->encoder = screenShare && !shareEncoderInUse().isEmpty()
        ? shareEncoderInUse()
        : streamInfoFactory(bin, "videoenc");
    if (screenShare && !hardwareFactory.isEmpty())
        streamInfo->codec = QStringLiteral("H.264");
    streamInfo->scaling = useGpu ? QStringLiteral("GPU") : QStringLiteral("CPU");
    auto probeState = std::make_shared<PublishProbeState>();
    const bool asyncStart = !screenShare && pipewireFd >= 0;
    const std::shared_ptr<ProbeGate> gate =
        asyncStart ? std::make_shared<ProbeGate>() : nullptr;
    probeState->startedMs = monotonicMs();
    probeState->screenShare = screenShare;
    probeState->keepByReference = m_shareGlEncode && screenShare;
    m_publishWatch.insert(cid, PublishWatch{probeState, false});
    if (GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capsrc")) {
        if (GstPad *srcPad = gst_element_get_static_pad(capture, "src")) {
            if (screenShare) {
                gst_pad_add_probe(
                    srcPad, GST_PAD_PROBE_TYPE_BUFFER,
                    [](GstPad *, GstPadProbeInfo *info, gpointer) {
                        GstBuffer *in = GST_PAD_PROBE_INFO_BUFFER(info);
                        if (GstBuffer *fixed =
                                SfuMediaEngine::rewrapShortDmaBuf(in)) {
                            GST_PAD_PROBE_INFO_DATA(info) = fixed;
                            gst_buffer_unref(in);
                        }
                        return GST_PAD_PROBE_OK;
                    },
                    nullptr, nullptr);
            }
            if (GstElement *entry = screenShare
                    ? gst_bin_get_by_name(GST_BIN(bin), "capentry")
                    : nullptr) {
                gst_object_unref(entry);
                auto retry = std::make_shared<ShareCaptureRetry>();
                m_publishWatch[cid].retry = retry;
                g_object_set_data_full(
                    G_OBJECT(bin), kCaptureRetryKey,
                    new std::shared_ptr<ShareCaptureRetry>(retry),
                    [](gpointer data) {
                        delete static_cast<std::shared_ptr<ShareCaptureRetry> *>(
                            data);
                    });
                // An EOS before the first buffer must not reach webrtcbin: the
                // capture may still be restarted in place.
                struct EosGuard {
                    std::shared_ptr<PublishProbeState> state;
                    std::shared_ptr<ShareCaptureRetry> retry;
                    SfuMediaEngine *engine;
                    QString cid;
                };
                gst_pad_add_probe(
                    srcPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                    [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
                        const auto *guard = static_cast<EosGuard *>(data);
                        if (!event || GST_EVENT_TYPE(event) != GST_EVENT_EOS
                            || guard->state->captured.load() > 0)
                            return GST_PAD_PROBE_OK;
                        const int epoch = guard->retry->stamp();
                        SfuMediaEngine *engine = guard->engine;
                        const QString cid = guard->cid;
                        marshal(engine, [engine, cid, epoch] {
                            engine->handleCaptureEnded(cid, epoch);
                        });
                        return GST_PAD_PROBE_DROP;
                    },
                    new EosGuard{probeState, retry, this, cid},
                    [](gpointer data) { delete static_cast<EosGuard *>(data); });
            }
            auto *held = new std::shared_ptr<PublishProbeState>(probeState);
            gst_pad_add_probe(
                srcPad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *pad, GstPadProbeInfo *, gpointer data) {
                    auto &state =
                        *static_cast<std::shared_ptr<PublishProbeState> *>(
                            data);
                    const quint64 n = state->captured.fetch_add(1) + 1;
                    if (n == 1) {
                        state->firstCaptureMs.store(monotonicMs()
                                                    - state->startedMs);
                        if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
                            gchar *text = gst_caps_to_string(caps);
                            qCInfo(lcSfuMedia)
                                << "capture negotiated caps="
                                << (text ? text : "?");
                            g_free(text);
                            gst_caps_unref(caps);
                        }
                    }
                    if (shouldReport(n)) {
                        qCInfo(lcSfuMedia)
                            << "capture delivered frames count=" << n;
                    }
                    return GST_PAD_PROBE_OK;
                },
                held,
                [](gpointer data) {
                    delete static_cast<std::shared_ptr<PublishProbeState> *>(
                        data);
                });
            struct EndedCtx {
                SfuMediaEngine *engine;
                QString cid;
            };
            auto *endedCtx = new EndedCtx{this, cid};
            gst_pad_add_probe(
                srcPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
                    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_EOS)
                        return GST_PAD_PROBE_OK;
                    auto *ctx = static_cast<EndedCtx *>(data);
                    SfuMediaEngine *engine = ctx->engine;
                    const QString cid = ctx->cid;
                    marshal(engine, [engine, cid] {
                        engine->handleCaptureEnded(cid);
                    });
                    return GST_PAD_PROBE_OK;
                },
                endedCtx,
                [](gpointer data) { delete static_cast<EndedCtx *>(data); });
            gst_object_unref(srcPad);
        }
        gst_object_unref(capture);
    }
    if (screenShare) {
        {
            if (GstPad *rateSink = keepAliveInjectionPad(bin)) {
                probeState->keepSink = rateSink;
                if (GstPad *feed = gst_pad_get_peer(rateSink)) {
                    auto *held =
                        new std::shared_ptr<PublishProbeState>(probeState);
                    gst_pad_add_probe(
                        feed, GST_PAD_PROBE_TYPE_BUFFER,
                        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                            auto &state = *static_cast<
                                std::shared_ptr<PublishProbeState> *>(data);
                            GstBuffer *buffer =
                                GST_PAD_PROBE_INFO_BUFFER(info);
                            if (!buffer)
                                return GST_PAD_PROBE_OK;
                            const qint64 now = monotonicMs();
                            state->lastFrameMs.store(now);
                            const qint64 sampled = state->lastSampleMs.load();
                            if (sampled >= 0 && now - sampled < 200)
                                return GST_PAD_PROBE_OK;
                            GstBuffer *kept = state->keepByReference
                                ? gst_buffer_ref(buffer)
                                : gst_buffer_copy_deep(buffer);
                            if (!kept)
                                return GST_PAD_PROBE_OK;
                            state->lastSampleMs.store(now);
                            GstBuffer *old = nullptr;
                            {
                                QMutexLocker lock(&state->keepMutex);
                                old = state->lastFrame;
                                state->lastFrame = kept;
                                state->lastFramePtsValid =
                                    GST_BUFFER_PTS_IS_VALID(kept);
                                state->lastFramePts =
                                    state->lastFramePtsValid
                                        ? GST_BUFFER_PTS(kept)
                                        : 0;
                                state->lastSampleAtMs = now;
                            }
                            if (old)
                                gst_buffer_unref(old);
                            return GST_PAD_PROBE_OK;
                        },
                        held,
                        [](gpointer data) {
                            delete static_cast<
                                std::shared_ptr<PublishProbeState> *>(data);
                        });
                    gst_object_unref(feed);
                }
            }
        }
        updateShareKeepAliveTimer();
    }
    if (GstElement *selfSink = gst_bin_get_by_name(GST_BIN(bin),
                                                   "selfvidsink")) {
        auto *ctx = new VideoSinkCtx{this, QString(),
                                     screenShare ? localScreenStreamId()
                                                 : localCameraStreamId()};
        ctx->gate = gate;
        g_signal_connect_data(selfSink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(selfSink);
    }
    GstElement *frames = gst_bin_get_by_name(GST_BIN(bin), "videoframes");
    const bool h264Frames = frames != nullptr;
    if (GstElement *encoder = frames ? frames
                                     : gst_bin_get_by_name(GST_BIN(bin),
                                                           "videoenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            auto *held = new std::shared_ptr<PublishProbeState>(probeState);
            gst_pad_add_probe(
                encoded, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *, gpointer data) {
                    auto &state =
                        *static_cast<std::shared_ptr<PublishProbeState> *>(
                            data);
                    const qint64 at = monotonicMs() - state->startedMs;
                    state->firstEncodedMs.store(at);
                    const qint64 captured = state->firstCaptureMs.load();
                    qCInfo(lcSfuMedia)
                        << "publish first encoded frame screenShare="
                        << state->screenShare << "afterPublishMs=" << at
                        << "firstCaptureMs=" << captured << "rateStageHoldMs="
                        << (captured >= 0 ? at - captured : qint64(-1));
                    return GST_PAD_PROBE_REMOVE;
                },
                held,
                [](gpointer data) {
                    delete static_cast<std::shared_ptr<PublishProbeState> *>(
                        data);
                });
            installEncryptProbe(encoded,true, gate, h264Frames);
            instrumentStreamInfo(encoded, streamInfo, false);
            instrumentKeyframeRequests(encoded, streamInfo);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    instrumentStreamInfo(srcPad, streamInfo, true);
    if (srcPad) {
        auto packets = std::make_shared<std::atomic<quint64>>(0);
        auto *rtpCtx = new RtpOutCtx{packets, true};
        gst_pad_add_probe(srcPad,
                          GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER
                                          | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                          countRtpOut, rtpCtx, rtpOutCtxFree);
    }
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    applyPublisherMsid(sinkPad, cid);
    enablePublishRetransmission(sinkPad);
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad && !consumePublishLinkFailure())
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "publisher link failed code=" << linked;
        releaseFailedPublishPad(sinkPad);
        m_publishedBins.remove(cid);
        if (const auto dead = m_publishWatch.take(cid); dead.state)
            releaseKeepAlive(dead.state);
        updateShareKeepAliveTimer();
        releasePublishedFd(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("publish_link_failed"));
        return;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    ++m_publishedMedia;
    if (screenShare) {
        if (!hardwareFactory.isEmpty() && !publishShareBackup(cid, bin, glEncode)) {
            g_shareHardwareEncoderFailed.store(true);
            unpublish(cid);
            QMetaObject::invokeMethod(
                this,
                [this, cid] {
                    Q_EMIT publishFailed(cid,
                                         QStringLiteral("screen_share_failed"));
                },
                Qt::QueuedConnection);
            return;
        }
        Q_EMIT shareTrackReady(cid, m_shareBackups.value(cid));
    }
    if (asyncStart) {
        auto pending = std::make_shared<std::atomic<bool>>(true);
        m_asyncStarts.insert(cid, AsyncStart{gate, pending});
        gst_element_call_async(
            bin,
            [](GstElement *element, gpointer data) {
                syncStateIfStillParented(element);
                (*static_cast<std::shared_ptr<std::atomic<bool>> *>(data))
                    ->store(false);
            },
            new std::shared_ptr<std::atomic<bool>>(pending),
            [](gpointer data) {
                delete static_cast<std::shared_ptr<std::atomic<bool>> *>(
                    data);
            });
    } else {
        gst_element_sync_state_with_parent(bin);
    }
    m_publisherEverPublished = true;
    renegotiatePublisher();
    if (!screenShare) {
        const quint64 generation = m_generation.load();
        QTimer::singleShot(asyncStart ? m_portalCameraFirstFrameTimeoutMs
                                      : m_cameraFirstFrameTimeoutMs,
                           this, [this, cid, generation] {
                               checkCameraFirstFrame(cid, generation);
                           });
    }
}

void SfuMediaEngine::installKeyframeRequestGate(
    GstPad *depaySink, std::shared_ptr<lightning::calls::StreamInfoStats> stats)
{
    instrumentKeyframeRequests(depaySink, stats, 300);
}

void SfuMediaEngine::enableVideoRetransmission(GstWebRTCRTPTransceiver *transceiver)
{
    if (!transceiver)
        return;
    GstWebRTCKind kind = GST_WEBRTC_KIND_UNKNOWN;
    g_object_get(transceiver, "kind", &kind, nullptr);
    if (kind == GST_WEBRTC_KIND_VIDEO)
        g_object_set(transceiver, "do-nack", TRUE, nullptr);
}

void SfuMediaEngine::enablePublishRetransmission(GstPad *webrtcSinkPad)
{
    // offerForSfu() renumbers video codecs across sections when hardware
    // encoding is requested; an RTX payload type webrtcbin picked could
    // collide with that, so retransmission stays off on that path.
    if (!webrtcSinkPad || shareHardwareEncodeRequested())
        return;
    GstWebRTCRTPTransceiver *transceiver = nullptr;
    g_object_get(webrtcSinkPad, "transceiver", &transceiver, nullptr);
    if (!transceiver)
        return;
    g_object_set(transceiver, "do-nack", TRUE, nullptr);
    gst_object_unref(transceiver);
}

QList<lightning::calls::ReceiveVideoCodec> SfuMediaEngine::receivableVideoCodecs()
{
    auto codecs = lightning::calls::receivableVideoCodecs(
        [](const QByteArray &name) {
            return firstMissingElement({name}).isEmpty();
        });
    if (qEnvironmentVariable("LIGHTNING_RECEIVE_VIDEO_CODEC")
        == QLatin1String("vp8")) {
        codecs.removeIf([](const lightning::calls::ReceiveVideoCodec &codec) {
            return codec.encoding != QLatin1String("VP8");
        });
    }
    return codecs;
}

lightning::calls::ReceiveVideoCodec SfuMediaEngine::receivableVideoCodec(
    const QString &encoding)
{
    const auto codecs = receivableVideoCodecs();
    for (const auto &codec : codecs) {
        if (codec.encoding.compare(encoding, Qt::CaseInsensitive) == 0)
            return codec;
    }
    // vp8dec is in kRequired, so this is only reached before that check.
    return codecs.value(0, {QStringLiteral("VP8"),
                            QStringLiteral("rtpvp8depay name=recvdepay "
                                           "! vp8dec name=videodec "),
                            false});
}

QString SfuMediaEngine::receivableVideoCaps()
{
    QStringList caps;
    for (const auto &codec : receivableVideoCodecs())
        if (codec.encoding == QLatin1String("VP8")
            || codec.encoding == QLatin1String("H264"))
        caps << QStringLiteral("application/x-rtp,media=video,encoding-name=%1,"
                               "clock-rate=90000").arg(codec.encoding);
    return caps.join(QLatin1Char(';'));
}

QString SfuMediaEngine::offerForSfu(const QString &sdp, bool multiCodec)
{
    if (!multiCodec)
        return sdp;
    // LiveKit matches a media section to a declared cid by the msid's SECOND
    // token, and webrtcbin writes its transceiver's name there. A lone track
    // survives on the SFU's "first pending of this kind" fallback; with two
    // video sections that fallback hands a section the wrong track, so each
    // names its cid, as a browser's do.
    static const QRegularExpression msid(
        QStringLiteral("^(a=(?:ssrc:\\d+ )?msid:)(\\S+) \\S+(\\r?)$"));
    const QStringList lines = sdp.split(QLatin1Char('\n'));
    // pion fixes a session's video codecs from the FIRST video section it ever
    // sees, and a browser lists every codec in each one. webrtcbin lists one,
    // so anything else, here the backup's VP8 or a share's H.264 after a VP8
    // camera, is answered with the wrong codec. Every section offers both.
    struct Codec { QLatin1String payload; QStringList lines; };
    const QList<Codec> codecs = {
        {QLatin1String("96"), {QStringLiteral("a=rtpmap:96 VP8/90000"),
            QStringLiteral("a=rtcp-fb:96 nack pli"),
            QStringLiteral("a=rtcp-fb:96 ccm fir"),
            QStringLiteral("a=rtcp-fb:96 transport-cc")}},
        {QLatin1String("102"), {QStringLiteral("a=rtpmap:102 H264/90000"),
            QStringLiteral("a=fmtp:102 packetization-mode=1;profile-level-id=%1;"
                           "level-asymmetry-allowed=1")
                .arg(QLatin1String(lightning::rtp::kH264ProfileLevelId)),
            QStringLiteral("a=rtcp-fb:102 nack pli"),
            QStringLiteral("a=rtcp-fb:102 ccm fir"),
            QStringLiteral("a=rtcp-fb:102 transport-cc")}},
    };
    QStringList out;
    QStringList section;
    bool video = false;
    const auto flushSection = [&] {
        out.append(section);
        section.clear();
    };
    const bool crlf = sdp.contains(QLatin1String("\r\n"));
    const QString eol = crlf ? QStringLiteral("\r") : QString();
    for (QString line : lines) {
        if (line.startsWith(QLatin1String("m=")) || line.isEmpty())
            flushSection();
        if (line.startsWith(QLatin1String("m=")))
            video = line.startsWith(QLatin1String("m=video "));
        if (line.startsWith(QLatin1String("m=video "))) {
            const QStringList words = QString(line).remove(QLatin1Char('\r'))
                                          .split(QLatin1Char(' '));
            for (const Codec &codec : codecs) {
                if (words.mid(3).contains(codec.payload))
                    continue;
                line.insert(line.size() - eol.size(),
                            QLatin1Char(' ') + codec.payload);
                for (const QString &attribute : codec.lines)
                    section << attribute + eol;
            }
        }
        if (const auto match = msid.match(line); video && match.hasMatch()) {
            line = match.captured(1) + match.captured(2) + QLatin1Char(' ')
                + match.captured(2) + match.captured(3);
        }
        out << line;
    }
    flushSection();
    return out.join(QLatin1Char('\n'));
}

bool SfuMediaEngine::publishShareBackup(const QString &cid, GstElement *primary,
                                        bool glMemory)
{
    const QString backupCid = cid + kShareBackupSuffix;
    const QString scale = glMemory
        ? QStringLiteral("glcolorconvert ! glcolorscale ! "
            "video/x-raw(memory:GLMemory),width=(int)[1,640],height=(int)[1,360],"
            "pixel-aspect-ratio=(fraction)1/1 ! gldownload ! videoconvert")
        : QStringLiteral("videoscale ! videoconvert");
    const quint32 ssrc = nextPublishSsrc();
    const QString description = QStringLiteral(
        "queue max-size-buffers=2 leaky=downstream "
        "! valve name=backupvalve drop=false "
        "! videorate drop-only=true skip-to-first=true "
        "! %1 ! video/x-raw,format=I420,width=(int)[1,640],height=(int)[1,360],"
        "pixel-aspect-ratio=(fraction)1/1,framerate=(fraction)10/1 "
        "! vp8enc name=videoenc deadline=1 lag-in-frames=0 cpu-used=8 threads=2 "
        "end-usage=cbr target-bitrate=350000 keyframe-max-dist=20 "
        "! %2 pt=96 ssrc=%3 mtu=%4 "
        "! capsfilter caps=\"application/x-rtp,media=video,encoding-name=VP8,payload=96,"
        "clock-rate=(int)90000,ssrc=(uint)%3\"")
        .arg(scale, QString::fromLatin1(lightning::rtp::vp8PayloaderName()))
        .arg(ssrc).arg(lightning::rtp::kRtpPayloadMtu);
    GError *error = nullptr;
    GstElement *backup = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error || !backup) {
        g_clear_error(&error);
        if (backup) gst_object_unref(backup);
        return false;
    }
    gst_element_set_name(backup, backupCid.toUtf8().constData());
    if (GstPad *entry = gst_element_get_static_pad(backup, "sink")) {
        gst_pad_add_probe(entry, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
            [](GstPad *, GstPadProbeInfo *info, gpointer) {
                GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
                if (GST_QUERY_TYPE(query) != GST_QUERY_CAPS)
                    return GST_PAD_PROBE_OK;
                GstCaps *filter = nullptr;
                gst_query_parse_caps(query, &filter);
                GstCaps *any = gst_caps_new_any();
                gst_query_set_caps_result(query, filter ? filter : any);
                gst_caps_unref(any);
                return GST_PAD_PROBE_HANDLED;
            }, nullptr, nullptr);
        gst_object_unref(entry);
    }
    GstElement *tee = gst_bin_get_by_name(GST_BIN(primary), "t");
    GstPad *raw = tee ? gst_element_request_pad_simple(tee, "src_%u") : nullptr;
    GstPad *ghost = raw ? gst_ghost_pad_new("backup_raw", raw) : nullptr;
    if (raw) gst_object_unref(raw);
    if (tee) gst_object_unref(tee);
    if (!ghost || !gst_element_add_pad(primary, ghost)) {
        if (ghost) gst_object_unref(ghost);
        gst_object_unref(backup);
        return false;
    }
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), backup))
        return false;
    GstPad *rtp = gst_element_get_static_pad(backup, "src");
    GstPad *sink = gst_element_request_pad_simple(m_publisher.webrtc, "sink_%u");
    applyPublisherMsid(sink, backupCid);
    const bool linked = !std::exchange(m_failNextShareBackup, false)
        && gst_element_link_pads(primary, "backup_raw", backup, "sink")
        && rtp && sink && gst_pad_link(rtp, sink) == GST_PAD_LINK_OK;
    if (!linked) {
        if (rtp) gst_object_unref(rtp);
        releaseFailedPublishPad(sink);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), backup);
        return false;
    }
    gst_object_unref(sink);
    const auto stats = attachStreamInfo(backup);
    stats->outgoing = true;
    stats->screen = true;
    stats->codec = QStringLiteral("VP8");
    stats->encoder = QStringLiteral("vp8enc backup");
    stats->source = QStringLiteral("shared capture");
    stats->scaling = glMemory ? QStringLiteral("GPU") : QStringLiteral("CPU");
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(backup), "videoenc");
    GstPad *frames = gst_element_get_static_pad(encoder, "src");
    installEncryptProbe(frames, true);
    instrumentStreamInfo(frames, stats, false);
    instrumentStreamInfo(rtp, stats, true);
    gst_object_unref(frames);
    gst_object_unref(encoder);
    gst_object_unref(rtp);
    m_publishedBins.insert(backupCid, backup);
    m_shareBackups.insert(cid, backupCid);
    ++m_publishedMedia;
    gst_element_sync_state_with_parent(backup);
    qCInfo(lcSfuMedia) << "screen share VP8 backup enabled ceiling=640x360 fps=10";
    return true;
}

namespace {
struct AbandonedStart {
    int fd = -1;
    SfuMediaEngine *engine = nullptr;
    QString cid;
};

void finishAbandonedStart(GstElement *bin, gpointer data)
{
    GST_STATE_LOCK(bin);
    gst_element_set_state(bin, GST_STATE_NULL);
    GST_STATE_UNLOCK(bin);
    auto *ctx = static_cast<AbandonedStart *>(data);
    if (ctx && ctx->fd > 0) {
        ::close(ctx->fd);
        ctx->fd = -1;
    }
    if (ctx && !ctx->cid.isEmpty()) {
        SfuMediaEngine *engine = ctx->engine;
        const QString cid = ctx->cid;
        marshal(engine, [engine, cid] {
            Q_EMIT engine->cameraPortalReleased(cid);
        });
    }
}

void abandonedStartFree(gpointer data)
{
    delete static_cast<AbandonedStart *>(data);
}
}

bool SfuMediaEngine::detachPendingStart(const QString &cid, GstElement *bin,
                                        bool force)
{
    const AsyncStart start = m_asyncStarts.take(cid);
    if (!start.gate)
        return false;
    {
        std::lock_guard<std::mutex> lock(start.gate->mutex);
        start.gate->open = false;
    }
    if (!bin || !m_publisher.pipeline
        || (!force && !start.pending->load()))
        return false;
    gst_object_ref(bin);
    const bool stateLocked = g_rec_mutex_trylock(GST_STATE_GET_LOCK(bin));
    gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
    if (stateLocked)
        GST_STATE_UNLOCK(bin);
    auto *ctx = new AbandonedStart{m_publishedFds.take(cid), this, cid};
    if (ctx->fd == 0)
        ctx->fd = -1;
    gst_element_call_async(bin, finishAbandonedStart, ctx,
                           abandonedStartFree);
    gst_object_unref(bin);
    qCInfo(lcSfuMedia) << "a capture started off the GUI thread (portal camera "
                          "or microphone) detached and finished off the GUI "
                          "thread starting="
                       << start.pending->load();
    return true;
}

void SfuMediaEngine::releasePublishedFd(const QString &cid)
{
    const int fd = m_publishedFds.take(cid);
    if (fd > 0)
        ::close(fd);
}

namespace {

struct PublishTeardown {
    SfuMediaEngine *engine = nullptr;
    GstElement *pipeline = nullptr;
    GstElement *webrtc = nullptr;
    GstPad *peer = nullptr;
    QString cid;
    quint64 generation = 0;
    std::shared_ptr<std::atomic<int>> outstanding;
};

struct ReceiveTeardown {
    GstElement *pipeline = nullptr;
    std::shared_ptr<std::atomic<int>> outstanding;
};

void receiveTeardownFree(gpointer data)
{
    auto *ctx = static_cast<ReceiveTeardown *>(data);
    if (ctx->pipeline)
        gst_object_unref(ctx->pipeline);
    if (ctx->outstanding)
        ctx->outstanding->fetch_sub(1);
    delete ctx;
}

/// Runs on a GStreamer thread-pool thread, never a streaming thread, so it
/// may change state. A synchronous set_state(NULL) on a bin inside a PLAYING
/// pipeline deadlocks.
void receiveTeardownAsync(GstElement *bin, gpointer data)
{
    auto *ctx = static_cast<ReceiveTeardown *>(data);
    gst_bin_remove(GST_BIN(ctx->pipeline), bin);
    gst_element_set_state(bin, GST_STATE_NULL);
}

GstPadProbeReturn dropRetiredPadProbe(GstPad *, GstPadProbeInfo *, gpointer)
{
    return GST_PAD_PROBE_DROP;
}

QString transceiverMidOf(GstPad *pad)
{
    QString sectionMid;
    GstWebRTCRTPTransceiver *transceiver = nullptr;
    g_object_get(pad, "transceiver", &transceiver, nullptr);
    if (transceiver) {
        gchar *mid = nullptr;
        g_object_get(transceiver, "mid", &mid, nullptr);
        if (mid) {
            sectionMid = QString::fromUtf8(mid);
            g_free(mid);
        }
        gst_object_unref(transceiver);
    }
    return sectionMid;
}

void publishTeardownFree(gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    if (ctx->peer)
        gst_object_unref(ctx->peer);
    if (ctx->webrtc)
        gst_object_unref(ctx->webrtc);
    if (ctx->pipeline)
        gst_object_unref(ctx->pipeline);
    if (ctx->outstanding)
        ctx->outstanding->fetch_sub(1);
    delete ctx;
}

void publishTeardownAsync(GstElement *bin, gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    gst_bin_remove(GST_BIN(ctx->pipeline), bin);
    gst_element_set_state(bin, GST_STATE_NULL);

    SfuMediaEngine *engine = ctx->engine;
    const QString cid = ctx->cid;
    const quint64 generation = ctx->generation;
    marshal(engine, [engine, cid, generation] {
        engine->noteTeardownComplete(cid, generation);
    });
}

GstPadProbeReturn publishTeardownProbe(GstPad *pad, GstPadProbeInfo *,
                                       gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    GstElement *bin = gst_pad_get_parent_element(pad);
    if (!bin) {
        publishTeardownFree(ctx);
        return GST_PAD_PROBE_REMOVE;
    }
    gst_element_call_async(bin, publishTeardownAsync, ctx,
                           publishTeardownFree);
    gst_object_unref(bin);
    return GST_PAD_PROBE_REMOVE;
}

}

void SfuMediaEngine::unpublish(const QString &cid)
{
    if (const QString backup = m_shareBackups.take(cid); !backup.isEmpty())
        unpublish(backup);
    GstElement *bin = m_publishedBins.take(cid);
    bool deliveredNothing = false;
    if (const auto dead = m_publishWatch.take(cid); dead.state) {
        deliveredNothing = dead.state->captured.load() == 0;
        releaseKeepAlive(dead.state);
    }
    updateShareKeepAliveTimer();
    if (cid == m_shareEncoderCid) {
        m_shareEncoderCid.clear();
        m_shareEncoderInUse.clear();
        m_shareGlEncode = false;
    }
    if (cid == m_micCid) {
        forgetMicrophoneSwap();
        m_micStartPending.reset();
        m_micSwapDeferred.reset();
        m_micRestartAfterStart = false;
        m_micNoiseModeAfterStart = false;
        m_micNoiseDspForceAfterStart = false;
        m_micStartTakenOver = false;
        m_micEchoAfterStart = false;
        m_micDspRescueAfterStart = false;
        m_micSwapReplacesStart = false;
        m_micRtpVerdictOwed = false;
        m_micLiveFront.clear();
        m_micCid.clear();
    }
    if (cid == m_shareAudioCid || cid == m_shareAudioAnyCid) {
        resetShareAudioState();
    }
    if (!bin || !m_publisher.pipeline) {
        detachPendingStart(cid, nullptr);
        const bool portal = m_publishedFds.contains(cid);
        releasePublishedFd(cid);
        if (portal)
            Q_EMIT cameraPortalReleased(cid);
        return;
    }
    if (m_publishedMedia.load() > 0)
        --m_publishedMedia;

    // Never set_state(NULL) synchronously here: with the streaming thread
    // mid-push, set_state waits for the stream lock that the push holds, a
    // deadlock on the GUI thread. Reordering the unlink or unparent does not
    // help, since neither stops a push already in flight.
    //
    // Instead an IDLE probe waits until no push is in flight, and the state
    // change runs via gst_element_call_async, off any streaming thread.
    // stop() tears down the whole pipeline and is unaffected.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    if (!srcPad) {
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        const bool portal = m_publishedFds.contains(cid);
        releasePublishedFd(cid);
        if (portal)
            Q_EMIT cameraPortalReleased(cid);
        renegotiatePublisher();
        return;
    }

    m_pendingTeardowns->fetch_add(1);
    auto *ctx = new PublishTeardown{
        this, GST_ELEMENT(gst_object_ref(m_publisher.pipeline)),
nullptr,nullptr, cid,
        m_generation.load(), m_pendingTeardowns};
    if (GstPad *peer = gst_pad_get_peer(srcPad)) {
        // Set the direction to inactive: releasing the request pad drops the
        // msid but leaves the section `a=sendrecv`, which the far end renders
        // as an empty tile forever. Sections cannot be removed from an SDP.
        if (GstWebRTCRTPTransceiver *transceiver = nullptr;
            (g_object_get(peer, "transceiver", &transceiver, nullptr),
             transceiver != nullptr)) {
            g_object_set(transceiver, "direction",
                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE,
                         nullptr);
            gst_object_unref(transceiver);
        }
        gst_pad_unlink(srcPad, peer);
        if (GstElement *webrtc = gst_pad_get_parent_element(peer)) {
            gchar *padName = gst_pad_get_name(peer);
            gst_element_release_request_pad(webrtc, peer);
            qCInfo(lcSfuMedia) << "publish transceiver retired pad="
                               << (padName ? padName : "?");
            g_free(padName);
            gst_object_unref(webrtc);
        }
        gst_object_unref(peer);
    }

    if (detachPendingStart(cid, bin,deliveredNothing)) {
        publishTeardownFree(ctx);
        gst_object_unref(srcPad);
        renegotiatePublisher();
        return;
    }
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_IDLE, publishTeardownProbe,
                      ctx, nullptr);
    gst_object_unref(srcPad);
}

int SfuMediaEngine::busesWithPendingMessagesForTest() const
{
    int pending = 0;
    for (const Peer *peer : {&m_publisher, &m_subscriber}) {
        if (!peer->pipeline)
            continue;
        if (GstBus *bus = gst_element_get_bus(peer->pipeline)) {
            if (gst_bus_have_pending(bus))
                ++pending;
            gst_object_unref(bus);
        }
    }
    return pending;
}

int SfuMediaEngine::publisherTrackSlotsForTest() const
{
    if (!m_publisher.webrtc)
        return -1;
    int slotCount = 0;
    GstIterator *it = gst_element_iterate_sink_pads(m_publisher.webrtc);
    if (!it)
        return 0;
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK:
            ++slotCount;
            g_value_reset(&item);
            break;
        case GST_ITERATOR_RESYNC:
            slotCount = 0;
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
    return slotCount;
}

void SfuMediaEngine::noteTeardownComplete(const QString &cid,
                                          quint64 generation)
{
    if (generation != m_generation.load()) {
        qCInfo(lcSfuMedia)
            << "publish teardown completed for a closed session; dropped";
        return;
    }
    const bool portal = m_publishedFds.contains(cid);
    releasePublishedFd(cid);
    if (portal)
        Q_EMIT cameraPortalReleased(cid);
    renegotiatePublisher();
}

void SfuMediaEngine::renegotiateAfterResume()
{
    // An offer whose answer died with the old socket leaves offerPending set,
    // and nothing else ever clears it.
    m_publisher.offerPending = false;
    renegotiatePublisher();
}

void SfuMediaEngine::renegotiatePublisher()
{
    if (!m_publisher.webrtc)
        return;
    // Never offer from a publisher that has no media section at all (LiveKit
    // answers Leave(STATE_MISMATCH)); deferred teardowns can reach this. The
    // gate is "has ever carried a track", not the live count: withdrawing the
    // last track must still renegotiate so its section goes inactive.
    if (!m_publisherEverPublished) {
        qCInfo(lcSfuMedia)
            << "renegotiation skipped: this publisher has no media section";
        return;
    }
    Peer &peer = m_publisher;
    peer.negotiationDirty = true;
    if (peer.offerPending || peer.negotiationScheduled)
        return;
    peer.negotiationScheduled = true;
    const quintptr token = reinterpret_cast<quintptr>(peer.webrtc);
    const quint64 generation = m_generation.load();
    marshal(this, [this, token, generation] {
        Target target = Target::Publisher;
        if (!tokenIsLive(token, generation, &target) || target != Target::Publisher)
            return;
        Peer &peer = m_publisher;
        peer.negotiationScheduled = false;
        if (peer.offerPending || !peer.negotiationDirty)
            return;
        peer.negotiationDirty = false;
        peer.offerPending = true;
        auto *ctx = promiseCtxNew(this, peer.webrtc, true);
        ctx->generation = generation;
        GstPromise *promise = gst_promise_new_with_change_func(
            onOfferCreated, ctx, promiseCtxFree);
        g_signal_emit_by_name(peer.webrtc, "create-offer", nullptr, promise);
    });
}

void SfuMediaEngine::awaitPublishTeardowns()
{
    if (m_pendingTeardowns->load() <= 0)
        return;
    QElapsedTimer timer;
    timer.start();
    while (m_pendingTeardowns->load() > 0
           && timer.elapsed() < kTeardownWaitMs) {
        QThread::msleep(2);
    }
    const int left = m_pendingTeardowns->load();
    if (left > 0) {
        qCWarning(lcSfuMedia)
            << "publish teardown did not finish within its budget; still"
            << left << "outstanding after" << kTeardownWaitMs << "ms";
    }
}

namespace {

void streamIdsFromSdp(GstSDPMessage *message, QHash<int, QString> *streams,
                      QHash<int, QString> *mids, QHash<int, QString> *tracks)
{
    if (!message)
        return;
    const guint sections = gst_sdp_message_medias_len(message);
    for (guint index = 0; index < sections; ++index) {
        const GstSDPMedia *media = gst_sdp_message_get_media(message, index);
        if (!media)
            continue;
        QString streamId;
        QString mid;
        QString trackSid;
        QString ssrcMsid;
        const guint attributes = gst_sdp_media_attributes_len(media);
        for (guint a = 0; a < attributes; ++a) {
            const GstSDPAttribute *attribute =
                gst_sdp_media_get_attribute(media, a);
            if (!attribute || !attribute->key)
                continue;
            const QLatin1String key(attribute->key);
            const QString value = QString::fromUtf8(attribute->value
                                                        ? attribute->value
                                                        : "");
            if (key == QLatin1String("msid")) {
                streamId = SfuMediaEngine::participantIdFromMsid(value);
                trackSid = SfuMediaEngine::trackSidFromMsid(value);
            } else if (key == QLatin1String("mid")) {
                mid = value.trimmed();
            } else if (key == QLatin1String("ssrc") && ssrcMsid.isEmpty()) {
                const qsizetype at = value.indexOf(QLatin1String(" msid:"));
                if (at >= 0)
                    ssrcMsid = value.mid(at + 6).trimmed();
            }
        }
        if (streamId.isEmpty() && trackSid.isEmpty() && !ssrcMsid.isEmpty()) {
            streamId = SfuMediaEngine::participantIdFromMsid(ssrcMsid);
            trackSid = SfuMediaEngine::trackSidFromMsid(ssrcMsid);
        }
        if (streams)
            streams->insert(static_cast<int>(index), streamId);
        if (mids)
            mids->insert(static_cast<int>(index), mid);
        if (tracks)
            tracks->insert(static_cast<int>(index), trackSid);
    }
}

}

QString SfuMediaEngine::sdpIceUfrag(const QString &sdp)
{
    return sdpAttribute(sdp, QStringLiteral("ice-ufrag"));
}

QString SfuMediaEngine::sdpAttribute(const QString &sdp, const QString &key)
{
    const QString prefix = QStringLiteral("a=") + key + QLatin1Char(':');
    for (QStringView line : QStringView(sdp).split(u'\n')) {
        line = line.trimmed();
        if (line.startsWith(prefix))
            return line.mid(prefix.size()).trimmed().toString();
    }
    return {};
}

void SfuMediaEngine::applyRemoteDescription(Target target, const QString &kind,
                                            const QString &sdp)
{
    if (!m_active)
        return;
    if (!ensurePeer(target))
        return;
    Peer &peer = peerFor(target);

    GstSDPMessage *message = nullptr;
    if (gst_sdp_message_new(&message) != GST_SDP_OK)
        return;
    const QByteArray sdpBytes = sdp.toUtf8();
    const bool parsed = gst_sdp_message_parse_buffer(
                            reinterpret_cast<const guint8 *>(
                                sdpBytes.constData()),
                            static_cast<guint>(sdpBytes.size()), message)
        == GST_SDP_OK;
    if (!parsed || gst_sdp_message_medias_len(message) == 0) {
        gst_sdp_message_free(message);
        Q_EMIT failed(QStringLiteral("bad_remote_sdp"));
        return;
    }
    qCInfo(lcSfuMedia) << "remote description submitted kind=" << kind
                       << "target=" << static_cast<int>(target)
                       << "sections=" << gst_sdp_message_medias_len(message);
    if (!qEnvironmentVariableIsEmpty("LIGHTNING_SDP_TRACE")) {
        qCInfo(lcSfuMedia) << "  sdp codecs (remote" << kind << "target"
                           << static_cast<int>(target)
                           << "):" << sdpCodecSummary(sdp);
    }
    if (target == Target::Subscriber) {
        QHash<int, QString> streams;
        QHash<int, QString> mids;
        QHash<int, QString> tracks;
        streamIdsFromSdp(message, &streams, &mids, &tracks);
        noteStreamIds(streams, mids, tracks);
    }

    const GstWebRTCSDPType type = (kind == QLatin1String("offer"))
        ? GST_WEBRTC_SDP_TYPE_OFFER
        : GST_WEBRTC_SDP_TYPE_ANSWER;
    bool iceRestart = false;
    if (target == Target::Subscriber && type == GST_WEBRTC_SDP_TYPE_OFFER) {
        const QString ufrag = sdpIceUfrag(sdp);
        iceRestart = !peer.remoteIceUfrag.isEmpty() && !ufrag.isEmpty()
            && ufrag != peer.remoteIceUfrag;
        if (!ufrag.isEmpty())
            peer.remoteIceUfrag = ufrag;
        if (iceRestart) {
            peer.iceRestarting = true;
            peer.restartCandidates.clear();
            qCInfo(lcSfuMedia) << "subscriber ICE restart offered";
        }
    }
    const QByteArray remoteIcePwd = iceRestart
        ? sdpAttribute(sdp, QStringLiteral("ice-pwd")).toUtf8() : QByteArray();
    GstWebRTCSessionDescription *description =
        gst_webrtc_session_description_new(type, message);
    peer.remoteDescriptionSet = false;
    auto *ctx = promiseCtxNew(this, peer.webrtc, target == Target::Publisher);
    ctx->generation = m_generation.load();
    ctx->revision = ++peer.remoteDescriptionRevision;
    ctx->offer = type == GST_WEBRTC_SDP_TYPE_OFFER;
    ctx->iceRestart = iceRestart;
    if (iceRestart) {
        ctx->remoteIceUfrag = peer.remoteIceUfrag.toUtf8();
        ctx->remoteIcePwd = remoteIcePwd;
    }
    GstPromise *promise = gst_promise_new_with_change_func(
        onRemoteDescriptionSet, ctx, promiseCtxFree);
    g_signal_emit_by_name(peer.webrtc, "set-remote-description", description,
                          promise);
    gst_webrtc_session_description_free(description);
}

void SfuMediaEngine::onRemoteDescriptionSet(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise);
        return;
    }
    auto *engine = ctx->engine;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    const quint64 generation = ctx->generation;
    const quint64 revision = ctx->revision;
    const bool offer = ctx->offer;
    const bool iceRestart = ctx->iceRestart;
    const QByteArray remoteIceUfrag = ctx->remoteIceUfrag;
    const QByteArray remoteIcePwd = ctx->remoteIcePwd;
    bool ok = gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    int errorCode = -1;
    if (ok) {
        const GstStructure *reply = gst_promise_get_reply(promise);
        ok = !reply || !gst_structure_has_field(reply, "error");
        GError *error = nullptr;
        if (!ok && gst_structure_get(reply, "error", G_TYPE_ERROR, &error, nullptr)
            && error) {
            errorCode = error->code;
            g_error_free(error);
        }
    }
    gst_promise_unref(promise);
    marshal(engine, [engine, token, generation, revision, offer, iceRestart,
                     remoteIceUfrag, remoteIcePwd, ok, errorCode] {
        Target target = Target::Publisher;
        if (!engine->tokenIsLive(token, generation, &target))
            return;
        Peer &peer = engine->peerFor(target);
        if (revision != peer.remoteDescriptionRevision)
            return;
        if (!ok) {
            peer.offerPending = false;
            peer.negotiationDirty = false;
            qCWarning(lcSfuMedia) << "remote description rejected target="
                                 << static_cast<int>(target)
                                 << "errorCode=" << errorCode;
            Q_EMIT engine->failed(QStringLiteral("remote_sdp_rejected"));
            return;
        }
        peer.remoteDescriptionSet = true;
        qCInfo(lcSfuMedia) << "remote description accepted target="
                          << static_cast<int>(target)
                          << "queuedCandidates=" << peer.pendingCandidates.size();
        const auto pending = std::exchange(peer.pendingCandidates, {});
        for (const QString &candidate : pending) {
            QString line;
            int mline = 0;
            if (parseCandidateInit(candidate, &line, &mline))
                g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", mline,
                                      line.toUtf8().constData());
        }
        if (offer) {
            auto *ctx = promiseCtxNew(engine, peer.webrtc, target == Target::Publisher);
            ctx->generation = generation;
            ctx->iceRestart = iceRestart;
            ctx->remoteIceUfrag = remoteIceUfrag;
            ctx->remoteIcePwd = remoteIcePwd;
            GstPromise *answer = gst_promise_new_with_change_func(
                onAnswerCreated, ctx, promiseCtxFree);
            g_signal_emit_by_name(peer.webrtc, "create-answer", nullptr, answer);
        } else if (target == Target::Publisher) {
            peer.offerPending = false;
            peer.answeredOffer = peer.offeredIdentity;
            if (peer.negotiationDirty)
                engine->renegotiatePublisher();
        }
    });
}

void SfuMediaEngine::applyRemoteCandidate(Target target,
                                          const QString &candidateInit)
{
    if (!m_active)
        return;
    Peer &peer = peerFor(target);
    ++peer.remoteCandidatesReceived;
    if (peer.remoteCandidatesReceived <= 3 || peer.remoteCandidatesReceived % 20 == 0)
        qCInfo(lcSfuMedia) << "remote ICE candidates target=" << static_cast<int>(target)
                          << "received=" << peer.remoteCandidatesReceived
                          << "descriptionReady=" << peer.remoteDescriptionSet;

    if (peer.iceRestarting && peer.restartCandidates.size() < 64)
        peer.restartCandidates.append(candidateInit);
    if (!peer.webrtc || !peer.remoteDescriptionSet) {
        if (peer.pendingCandidates.size() < 256)
            peer.pendingCandidates.append(candidateInit);
        return;
    }
    QString line;
    int mline = 0;
    if (!parseCandidateInit(candidateInit, &line, &mline))
        return;
    g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", mline,
                          line.toUtf8().constData());
}

void SfuMediaEngine::setMicrophoneMuted(bool muted)
{
    if (m_microphoneMuted != muted) {
        qCInfo(lcSfuMedia) << "microphone valve drop=" << muted;
        m_micSilentSinceMs = -1;
        m_micQuietSinceMs = -1;
        m_micQuietAnnounced = false;
        if (m_micSilentAnnounced) {
            m_micSilentAnnounced = false;
            Q_EMIT localAudioSilent(false, m_micPeakDb);
        }
    }
    m_microphoneMuted = muted;
    if (!m_publisher.pipeline)
        return;
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        if (GstElement *valve =
                gst_bin_get_by_name(GST_BIN(it.value()), "micvalve")) {
            g_object_set(valve, "drop", muted ? TRUE : FALSE, nullptr);
            gst_object_unref(valve);
        }
    }
}

void SfuMediaEngine::setShareAudioMuted(bool muted)
{
    if (m_shareAudioMuted != muted)
        qCInfo(lcSfuMedia) << "share audio valve drop=" << muted;
    m_shareAudioMuted = muted;
    if (!m_publisher.pipeline)
        return;
    const QByteArray name = lightning::shareaudio::shareValveElementName()
                                .toUtf8();
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        if (GstElement *valve = gst_bin_get_by_name(GST_BIN(it.value()),
                                                    name.constData())) {
            g_object_set(valve, "drop", muted ? TRUE : FALSE, nullptr);
            gst_object_unref(valve);
        }
    }
}

void SfuMediaEngine::setMicrophoneGain(int percent)
{
    const int clamped = percent < 0 ? 0 : (percent > 200 ? 200 : percent);
    m_microphoneGain.store(clamped);
    if (!m_publisher.pipeline)
        return;
    const gdouble factor = audioFactorPercent(clamped) / 100.0;
    int elements = 0;
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        if (GstElement *gain =
                gst_bin_get_by_name(GST_BIN(it.value()), "micvol")) {
            g_object_set(gain, "volume", factor, nullptr);
            gst_object_unref(gain);
            ++elements;
        }
    }
    if (elements > 0) {
        qCInfo(lcSfuMedia) << "microphone gain applied: percent=" << clamped
                           << "gst=" << factor << "elements=" << elements;
    } else {
        qCWarning(lcSfuMedia)
            << "microphone gain had nowhere to land: percent=" << clamped
            << "publishedBins=" << m_publishedBins.size();
    }
}

QString SfuMediaEngine::voiceProcessingDescription(calls::noise::Mode mode,
                                                   bool echoCancel,
                                                   bool autoGain)
{
    return calls::noise::voiceProcessingDescriptionWith(mode, echoCancel,
                                                        autoGain);
}

void SfuMediaEngine::setNoiseSuppressionMode(calls::noise::Mode mode)
{
    const int previous = m_noiseMode.exchange(int(mode));
    if (previous == int(mode))
        return;
    m_noiseFellBackToWebrtc = false;
    qCInfo(lcSfuMedia) << "microphone noise suppression mode="
                       << calls::noise::modeKey(mode) << "(was"
                       << calls::noise::modeKey(calls::noise::Mode(previous))
                       << ")";
    if (!m_publisher.pipeline)
        return;
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        GstElement *denoise =
            gst_bin_get_by_name(GST_BIN(it.value()), calls::noise::kDenoiseName);
        GstElement *dsp =
            gst_bin_get_by_name(GST_BIN(it.value()), calls::noise::kDspName);
        const bool microphone = denoise || dsp;
        const bool hasDsp = dsp != nullptr;
        if (denoise)
            gst_object_unref(denoise);
        if (dsp)
            gst_object_unref(dsp);
        if (!microphone)
            continue;
        const bool starting = it.key() == m_micCid && microphoneStarting();
        bool dspDeferred = false;
        const QString did = calls::noise::applyMode(
            it.value(), mode,
            starting ? calls::noise::DspSwitch::Later
                     : calls::noise::DspSwitch::WaitForDenoiser,
            &dspDeferred, echoAllowedOnMic());
        if (starting) {
            m_micNoiseModeAfterStart = true;
            m_micNoiseDspForceAfterStart =
                m_micNoiseDspForceAfterStart || dspDeferred;
        }
        qCInfo(lcSfuMedia) << "microphone noise suppression applied live:"
                           << did;
        const QString key = QLatin1String(calls::noise::modeKey(mode));
        if (mode == calls::noise::Mode::Off) {
            Q_EMIT noiseSuppressionStatus(key, true);
        } else if (mode == calls::noise::Mode::WebRtc) {
            if (!hasDsp) {
                qCWarning(lcSfuMedia)
                    << "microphone noise suppression mode= webrtc backend ok= "
                       "false: this GStreamer has no webrtcdsp, so the "
                       "microphone is sent WITHOUT noise suppression";
            }
            Q_EMIT noiseSuppressionStatus(key, hasDsp);
        }
    }
}

void SfuMediaEngine::retryNoiseSuppression()
{
    const calls::noise::Mode mode = noiseSuppressionMode();
    if (!calls::noise::dspWaitsForDenoiser(mode) || !m_publisher.pipeline)
        return;
    qCInfo(lcSfuMedia) << "microphone noise suppression mode="
                       << calls::noise::modeKey(mode) << "retried";
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        GstElement *denoise =
            gst_bin_get_by_name(GST_BIN(it.value()), calls::noise::kDenoiseName);
        if (!denoise)
            continue;
        gst_object_unref(denoise);
        const bool starting = it.key() == m_micCid && microphoneStarting();
        bool dspDeferred = false;
        calls::noise::applyMode(it.value(), mode,
                                starting
                                    ? calls::noise::DspSwitch::Later
                                    : calls::noise::DspSwitch::WaitForDenoiser,
                                &dspDeferred, echoAllowedOnMic());
        if (starting) {
            m_micNoiseModeAfterStart = true;
            m_micNoiseDspForceAfterStart =
                m_micNoiseDspForceAfterStart || dspDeferred;
        }
    }
}

void SfuMediaEngine::handleDenoiseStatus(const QString &requested,
                                         const QString &active, bool ok,
                                         int latencySamples)
{
    if (requested
        != QLatin1String(calls::noise::modeKey(noiseSuppressionMode())))
        return;
    const auto mode = calls::noise::modeFromKey(requested.toStdString(),
                                                calls::noise::Mode::Off);
    if (mode == calls::noise::Mode::Off || mode == calls::noise::Mode::WebRtc)
        return;
    const calls::noise::Mode follow = ok ? mode : calls::noise::Mode::WebRtc;
    m_lastDspFollowUp = QLatin1String(calls::noise::modeKey(follow));
    bool anyDsp = false;
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        GstElement *dsp =
            gst_bin_get_by_name(GST_BIN(it.value()), calls::noise::kDspName);
        if (!dsp)
            continue;
        gst_object_unref(dsp);
        anyDsp = true;
        qCInfo(lcSfuMedia) << "microphone noise suppression: the denoiser "
                              "reported, webrtcdsp follows"
                           << calls::noise::modeKey(follow) << ":"
                           << calls::noise::applyDspMode(it.value(), follow, false,
                                                         echoAllowedOnMic());
    }
    m_noiseFellBackToWebrtc = !ok && anyDsp;
    if (ok) {
        qCInfo(lcSfuMedia)
            << "microphone noise suppression mode=" << requested
            << "backend ok= true active=" << active << "latency-ms="
            << (latencySamples * 1000.0 / calls::noise::kSampleRate);
    } else if (anyDsp) {
        qCWarning(lcSfuMedia)
            << "microphone noise suppression mode=" << requested
            << "backend ok= false: falling back to WebRTC noise suppression";
    } else {
        qCWarning(lcSfuMedia)
            << "microphone noise suppression mode=" << requested
            << "backend ok= false: the backend could not start and this "
               "GStreamer has no webrtcdsp, so the microphone is sent "
               "WITHOUT noise suppression";
    }
    Q_EMIT noiseSuppressionStatus(requested, ok);
}

void SfuMediaEngine::setOutputMuted(bool muted)
{
    m_outputMuted.store(muted);
    if (!m_subscriber.pipeline)
        return;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
    if (!it)
        return;
    GValue item = G_VALUE_INIT;
    bool done = false;
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            if (element) {
                gchar *name = gst_element_get_name(element);
                if (name && g_str_has_prefix(name, "outvol"))
                    g_object_set(element, "mute", muted ? TRUE : FALSE,
                                 nullptr);
                g_free(name);
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
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

int SfuMediaEngine::audioFactorPercent(int userPercent)
{
    const int user = userPercent < 0 ? 0 : (userPercent > 200 ? 200
                                                              : userPercent);
    if (user <= 100)
        return user;
    return 100 + (user - 100) * 9;
}

QString SfuMediaEngine::volumeKeyFor(const QString &streamId,
                                    const QString &trackKey)
{
    return trackKey.isEmpty() ? streamId
                              : (streamId + QLatin1Char('_') + trackKey);
}

QString SfuMediaEngine::outputVolumeElementName(const QString &streamId)
{
    QString safe;
    safe.reserve(streamId.size());
    for (const QChar c : streamId) {
        safe.append((c.isLetterOrNumber() || c == QLatin1Char('_'))
                        ? c
                        : QChar(QLatin1Char('_')));
    }
    return QStringLiteral("outvol_%1").arg(safe);
}

void SfuMediaEngine::setParticipantVolume(const QString &streamId,
                                          int percent)
{
    setTrackVolume(streamId, QString(), percent);
}

void SfuMediaEngine::setTrackVolume(const QString &streamId,
                                    const QString &trackKey, int percent)
{
    if (!m_subscriber.pipeline || streamId.isEmpty())
        return;
    const double volume = audioFactorPercent(percent) / 100.0;
    const QString target =
        trackKey.isEmpty() ? outputVolumeElementName(streamId)
                           : outputVolumeElementName(
                                 volumeKeyFor(streamId, trackKey));

    const auto apply = [&](const QString &key) {
        const QString target = outputVolumeElementName(key);
        if (GstElement *element = gst_bin_get_by_name(
                GST_BIN(m_subscriber.pipeline), target.toUtf8().constData())) {
            g_object_set(element, "volume", volume, nullptr);
            gst_object_unref(element);
            return true;
        }
        return false;
    };
    const auto announce = [&](const QString &key, int elements) {
        m_volumeMissWarned.remove(key);
        if (m_volumeAppliedLog.value(key, -1) == percent)
            return;
        m_volumeAppliedLog.insert(key, percent);
        qCInfo(lcSfuMedia)
            << "participant volume applied: wanted=" << outputVolumeElementName(key)
            << "percent=" << percent << "gst=" << volume
            << "elements=" << elements;
    };
    if (!trackKey.isEmpty()) {
        const QString key = volumeKeyFor(streamId, trackKey);
        if (apply(key)) {
            announce(key, 1);
            return;
        }
    } else {
        bool any = false;
        int applied = 0;
        const QString prefix =
            outputVolumeElementName(streamId);
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
        GValue item = G_VALUE_INIT;
        bool done = false;
        int resyncsLeft = 8;
        while (!done) {
            switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK: {
                auto *element = GST_ELEMENT(g_value_get_object(&item));
                gchar *raw = element ? gst_element_get_name(element) : nullptr;
                const QString name = QString::fromUtf8(raw ? raw : "");
                g_free(raw);
                if (name.startsWith(prefix)) {
                    g_object_set(element, "volume", volume, nullptr);
                    any = true;
                    ++applied;
                }
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                any = false;
                applied = 0;
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
        if (any) {
            announce(streamId, applied);
            return;
        }
    }

    const QString pendingKey =
        trackKey.isEmpty() ? streamId : volumeKeyFor(streamId, trackKey);
    m_pendingTrackVolume.insert(pendingKey, percent);
    if (m_volumeMissWarned.contains(pendingKey))
        return;
    m_volumeMissWarned.insert(pendingKey);
    int volumeElements = 0;
    QStringList known;
    if (GstIterator *it =
            gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline))) {
        GValue item = G_VALUE_INIT;
        bool done = false;
        int resyncsLeft = 8;
        while (!done) {
            switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK: {
                if (auto *element = GST_ELEMENT(g_value_get_object(&item))) {
                    gchar *name = gst_element_get_name(element);
                    if (name && g_str_has_prefix(name, "outvol")) {
                        ++volumeElements;
                        if (known.size() < 8)
                            known << QString::fromUtf8(name);
                    }
                    g_free(name);
                }
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                volumeElements = 0;
                known.clear();
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
    qCWarning(lcSfuMedia)
        << "participant volume had nowhere to land: wanted=" << target
        << "receive volume elements=" << volumeElements
        << "named=" << known.join(QLatin1Char(','));
}

void SfuMediaEngine::applyPendingTrackVolume(const QString &streamId,
                                             const QString &trackKey,
                                             quint64 generation)
{
    if (generation != m_generation.load() || !m_subscriber.pipeline)
        return;
    if (m_pendingTrackVolume.contains(streamId)) {
        m_volumeMissWarned.remove(streamId);
        m_volumeAppliedLog.remove(streamId);
        setTrackVolume(streamId, QString(), m_pendingTrackVolume.value(streamId));
    }
    const QString key = volumeKeyFor(streamId, trackKey);
    if (m_pendingTrackVolume.contains(key)) {
        m_volumeMissWarned.remove(key);
        m_volumeAppliedLog.remove(key);
        setTrackVolume(streamId, trackKey, m_pendingTrackVolume.value(key));
    }
}

int SfuMediaEngine::receiveMutedForTest(const QString &streamId) const
{
    if (!m_subscriber.pipeline)
        return -1;
    const QString prefix = outputVolumeElementName(streamId);
    int found = -1;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
    GValue item = G_VALUE_INIT;
    bool done = false;
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            gchar *raw = element ? gst_element_get_name(element) : nullptr;
            const QString name = QString::fromUtf8(raw ? raw : "");
            g_free(raw);
            if (found < 0 && name.startsWith(prefix)) {
                gboolean muted = FALSE;
                g_object_get(element, "mute", &muted, nullptr);
                found = muted ? 1 : 0;
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            found = -1;
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
    return found;
}

double SfuMediaEngine::receiveVolumeForTest(const QString &streamId) const
{
    if (!m_subscriber.pipeline)
        return -1.0;
    const QString prefix = outputVolumeElementName(streamId);
    double found = -1.0;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
    GValue item = G_VALUE_INIT;
    bool done = false;
    int resyncsLeft = 8;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            gchar *raw = element ? gst_element_get_name(element) : nullptr;
            const QString name = QString::fromUtf8(raw ? raw : "");
            g_free(raw);
            if (found < 0 && name.startsWith(prefix)) {
                gdouble v = -1.0;
                g_object_get(element, "volume", &v, nullptr);
                found = v;
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            found = -1.0;
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
    return found;
}

int SfuMediaEngine::statsTraceIntervalMs(const QString &raw)
{
    const QString v = raw.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("0") || v == QLatin1String("off")
        || v == QLatin1String("false") || v == QLatin1String("no"))
        return 0;
    if (v == QLatin1String("1") || v == QLatin1String("true")
        || v == QLatin1String("yes") || v == QLatin1String("on"))
        return 5000;
    bool ok = false;
    const int seconds = v.toInt(&ok);
    if (!ok || seconds <= 0)
        return 5000;
    return qBound(1, seconds, 600) * 1000;
}

void SfuMediaEngine::armStatsTrace()
{
    if (m_statsIntervalMs < 0) {
        const bool requested = qEnvironmentVariableIsSet("LIGHTNING_CALL_STATS_TRACE");
        m_statsIntervalMs = requested
            ? statsTraceIntervalMs(qEnvironmentVariable("LIGHTNING_CALL_STATS_TRACE"))
            : 5000;
        m_statsFull = requested && m_statsIntervalMs > 0;
        if (m_statsIntervalMs > 0) {
            m_statsTimer.setInterval(m_statsIntervalMs);
            m_statsTimer.setSingleShot(false);
            connect(&m_statsTimer, &QTimer::timeout, this,
                    &SfuMediaEngine::requestStats);
            qCInfo(lcSfuMedia) << "rtp stats trace armed intervalMs="
                               << m_statsIntervalMs << "full=" << m_statsFull;
        }
    }
    if (m_statsIntervalMs > 0 && !m_statsTimer.isActive())
        m_statsTimer.start();
}

namespace {

struct StatsWalk {
    QList<SfuMediaEngine::RtpStat> out;
    QString peer;
};

gboolean collectStat(GQuark, const GValue *value, gpointer data)
{
    auto *walk = static_cast<StatsWalk *>(data);
    if (!GST_VALUE_HOLDS_STRUCTURE(value))
        return TRUE;
    const GstStructure *s = gst_value_get_structure(value);
    if (!s)
        return TRUE;
    GstWebRTCStatsType type = GST_WEBRTC_STATS_CODEC;
    if (!gst_structure_get(s, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr))
        return TRUE;
    if (type != GST_WEBRTC_STATS_INBOUND_RTP
        && type != GST_WEBRTC_STATS_OUTBOUND_RTP
        && type != GST_WEBRTC_STATS_REMOTE_INBOUND_RTP)
        return TRUE;

    SfuMediaEngine::RtpStat st;
    st.peer = walk->peer;
    st.dir = type == GST_WEBRTC_STATS_INBOUND_RTP ? QStringLiteral("inbound")
           : type == GST_WEBRTC_STATS_OUTBOUND_RTP ? QStringLiteral("outbound")
           : QStringLiteral("remote-inbound");
    guint ssrc = 0;
    if (gst_structure_has_field(s, "ssrc"))
        gst_structure_get_uint(s, "ssrc", &ssrc);
    st.ssrc = ssrc;
    if (gst_structure_has_field(s, "kind")) {
        if (const gchar *kind = gst_structure_get_string(s, "kind"))
            st.kind = QString::fromUtf8(kind);
    }
    auto u64 = [&](const char *field, quint64 *dst) {
        if (!gst_structure_has_field(s, field))
            return;
        guint64 v = 0;
        if (gst_structure_get_uint64(s, field, &v))
            *dst = v;
    };
    auto u32 = [&](const char *field, quint32 *dst) {
        if (!gst_structure_has_field(s, field))
            return;
        guint v = 0;
        if (gst_structure_get_uint(s, field, &v))
            *dst = v;
    };
    auto i64 = [&](const char *field, qint64 *dst) {
        if (!gst_structure_has_field(s, field))
            return;
        gint64 v = 0;
        if (gst_structure_get_int64(s, field, &v))
            *dst = v;
    };
    auto dbl = [&](const char *field, double *dst) {
        if (!gst_structure_has_field(s, field))
            return;
        gdouble v = 0;
        if (gst_structure_get_double(s, field, &v))
            *dst = v;
    };
    if (type == GST_WEBRTC_STATS_INBOUND_RTP
        || type == GST_WEBRTC_STATS_REMOTE_INBOUND_RTP) {
        const int n = gst_structure_n_fields(s);
        for (int i = 0; i < n && st.extra.size() < 400; ++i) {
            const char *name = gst_structure_nth_field_name(s, i);
            if (!name)
                continue;
            const QByteArray key(name);
            if (!(key.contains("discard") || key.contains("duplicate")
                  || key.contains("repair") || key.contains("burst")
                  || key.contains("gap") || key.contains("conceal")
                  || key.contains("fec") || key.contains("retransmit")
                  || key.contains("jitter-buffer") || key.contains("late")))
                continue;
            const GValue *v = gst_structure_get_value(s, name);
            GValue d = G_VALUE_INIT;
            g_value_init(&d, G_TYPE_DOUBLE);
            if (v && g_value_transform(v, &d)) {
                st.extra += QStringLiteral(" %1=%2")
                                .arg(QString::fromLatin1(key),
                                     QString::number(g_value_get_double(&d),
                                                     'g', 6));
            }
            g_value_unset(&d);
        }
    }
    if (type == GST_WEBRTC_STATS_INBOUND_RTP) {
        u64("packets-received", &st.packets);
        i64("packets-lost", &st.lost);
        dbl("jitter", &st.jitter);
        u64("bytes-received", &st.bytes);
        u32("pli-count", &st.pli);
        u32("nack-count", &st.nack);
        u32("fir-count", &st.fir);
    } else if (type == GST_WEBRTC_STATS_OUTBOUND_RTP) {
        u64("packets-sent", &st.packets);
        u64("bytes-sent", &st.bytes);
        u32("pli-count", &st.pli);
        u32("nack-count", &st.nack);
        u32("fir-count", &st.fir);
    } else {
        i64("packets-lost", &st.lost);
        dbl("jitter", &st.jitter);
        dbl("round-trip-time", &st.rtt);
        dbl("fraction-lost", &st.fractionLost);
    }
    walk->out.append(st);
    return TRUE;
}

}

void SfuMediaEngine::requestStats()
{
    const auto ask = [this](Peer &peer, bool publisher) {
        if (!peer.webrtc)
            return;
        auto *ctx = promiseCtxNew(this, peer.webrtc, publisher);
        ctx->generation = m_generation.load();
        GstPromise *promise = gst_promise_new_with_change_func(
            onStatsReady, ctx, promiseCtxFree);
        g_signal_emit_by_name(peer.webrtc, "get-stats", nullptr, promise);
    };
    ask(m_publisher, true);
    ask(m_subscriber, false);
    logReceiveHealth();
}

void SfuMediaEngine::onStatsReady(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    SfuMediaEngine *engine = ctx->engine;
    StatsWalk walk;
    walk.peer = ctx->publisher ? QStringLiteral("pub") : QStringLiteral("sub");
    if (const GstStructure *reply = gst_promise_get_reply(promise))
        gst_structure_foreach(reply, collectStat, &walk);
    gst_promise_unref(promise);
    if (walk.out.isEmpty())
        return;
    const QList<RtpStat> stats = walk.out;
    const bool quiet = ctx->quiet;
    const quint64 generation = ctx->generation;
    marshal(engine, [engine, stats, quiet, generation] {
        if (!engine->m_active || engine->m_generation.load() != generation)
            return;
        engine->noteStreamInfoRtp(stats);
        if (!quiet)
            engine->logStats(stats);
    });
}

void SfuMediaEngine::requestStreamInfoRtpStats()
{
    for (Peer *peer : {&m_publisher, &m_subscriber}) {
        if (!peer->webrtc)
            continue;
        auto *ctx = promiseCtxNew(this, peer->webrtc, peer == &m_publisher);
        ctx->quiet = true;
        ctx->generation = m_generation.load();
        GstPromise *promise = gst_promise_new_with_change_func(
            onStatsReady, ctx, promiseCtxFree);
        g_signal_emit_by_name(peer->webrtc, "get-stats", nullptr, promise);
    }
}

void SfuMediaEngine::noteStreamInfoRtp(const QList<RtpStat> &stats)
{
    for (const RtpStat &st : stats) {
        if (st.kind == QLatin1String("audio"))
            continue;
        const quint64 key = (quint64(st.dir == QLatin1String("remote-inbound")) << 32)
            | st.ssrc;
        m_streamInfoRtp.insert(key, st);
    }
}

void SfuMediaEngine::logStats(const QList<RtpStat> &stats)
{
    const qint64 now = monotonicMs();
    QSet<quint32> videoSsrcs;
    for (const RtpStat &st : stats) {
        if (st.kind == QLatin1String("video"))
            videoSsrcs.insert(st.ssrc);
    }
    const bool beat = (m_statsReports++ % 12) == 0;
    for (const RtpStat &st : stats) {
        if (!m_statsFull && videoSsrcs.contains(st.ssrc))
            continue;
        qint64 lostDelta = 0;
        {
            const quint32 key = st.ssrc ^ (st.dir == QLatin1String("inbound")
                                               ? 0u : 0x80000000u);
            const auto prev = m_lastRtpLost.constFind(key);
            if (prev != m_lastRtpLost.constEnd())
                lostDelta = st.lost - *prev;
            m_lastRtpLost.insert(key, st.lost);
        }
        if (!m_statsFull && lostDelta == 0 && !beat)
            continue;
        if (st.dir == QLatin1String("remote-inbound")) {
            qCInfo(lcSfuMedia).nospace().noquote()
                << "rtp stats peer=" << st.peer << " " << st.dir
                << " ssrc=" << st.ssrc << " lost=" << st.lost
                << " lostDelta=" << lostDelta << st.extra
                << " jitterMs=" << QString::number(st.jitter * 1000.0, 'f', 1)
                << " rttMs=" << QString::number(st.rtt * 1000.0, 'f', 1)
                << " fractionLost="
                << QString::number(st.fractionLost, 'f', 3);
            continue;
        }
        double kbps = -1;
        const auto last = m_lastRtpBytes.constFind(st.ssrc);
        if (last != m_lastRtpBytes.constEnd() && now > last->first
            && st.bytes >= last->second) {
            kbps = double(st.bytes - last->second) * 8.0
                   / double(now - last->first);
        }
        m_lastRtpBytes.insert(st.ssrc, qMakePair(now, st.bytes));
        qCInfo(lcSfuMedia).nospace().noquote()
            << "rtp stats peer=" << st.peer << " " << st.dir
            << " ssrc=" << st.ssrc
            << (st.kind.isEmpty() ? QString() : QStringLiteral(" kind=") + st.kind)
            << " packets=" << st.packets << " lost=" << st.lost
            << " jitterMs=" << QString::number(st.jitter * 1000.0, 'f', 1)
            << " kbps=" << (kbps < 0 ? QStringLiteral("?")
                                     : QString::number(kbps, 'f', 0))
            << " lostDelta=" << lostDelta
            << " pli=" << st.pli << " nack=" << st.nack << " fir=" << st.fir
            << st.extra;
    }
}

namespace {

using RecvHealthPtr = std::shared_ptr<SfuMediaEngine::RecvHealth>;

void freeRecvHealthPtr(gpointer data, GClosure *)
{
    delete static_cast<RecvHealthPtr *>(data);
}

void freeRecvHealthPtrPlain(gpointer data)
{
    delete static_cast<RecvHealthPtr *>(data);
}

GstPadProbeReturn onConcealProbe(GstPad *, GstPadProbeInfo *info,
                                 gpointer userData)
{
    auto *health = static_cast<RecvHealthPtr *>(userData);
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (!health || !event)
        return GST_PAD_PROBE_OK;
    if (GST_EVENT_TYPE(event) == GST_EVENT_GAP) {
        (*health)->gapEvents.fetch_add(1);
    } else if (GST_EVENT_TYPE(event) == GST_EVENT_CUSTOM_DOWNSTREAM) {
        const GstStructure *st = gst_event_get_structure(event);
        if (st && gst_structure_has_name(st, "GstRTPPacketLost"))
            (*health)->lostEvents.fetch_add(1);
    }
    return GST_PAD_PROBE_OK;
}

void onRecvQueueOverrun(GstElement *, gpointer userData)
{
    auto *health = static_cast<RecvHealthPtr *>(userData);
    if (health)
        (*health)->queueOverruns.fetch_add(1);
}

}

void SfuMediaEngine::instrumentReceiveBin(GstElement *bin,
                                          const QString &trackKey,
                                          const QString &streamId)
{
    GstElement *dec = gst_bin_get_by_name(GST_BIN(bin), "recvdec");
    GstElement *queue = gst_bin_get_by_name(GST_BIN(bin), "recvqueue");
    if (!dec && !queue) {
        return;
    }
    auto health = std::make_shared<RecvHealth>();
    health->streamId = streamId;
    if (dec) {
        if (GstPad *sink = gst_element_get_static_pad(dec, "sink")) {
            gst_pad_add_probe(
                sink,
                GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                onConcealProbe, new RecvHealthPtr(health),
                freeRecvHealthPtrPlain);
            gst_object_unref(sink);
        }
        gst_object_unref(dec);
    }
    if (queue) {
        g_signal_connect_data(queue, "overrun", G_CALLBACK(onRecvQueueOverrun),
                              new RecvHealthPtr(health), freeRecvHealthPtr,
                              GConnectFlags(0));
        gst_object_unref(queue);
    }
    QMutexLocker lock(&m_recvHealthMutex);
    m_recvHealth.insert(trackKey, health);
}

void SfuMediaEngine::logReceiveHealth()
{
    const bool beat = (m_healthReports++ % 12) == 0;
    {
        QMutexLocker lock(&m_recvHealthMutex);
        for (auto it = m_recvHealth.begin(); it != m_recvHealth.end();) {
            const RecvHealthPtr h = it.value().lock();
            if (!h) {
                it = m_recvHealth.erase(it);
                continue;
            }
            const quint64 gap = h->gapEvents.load();
            const quint64 lost = h->lostEvents.load();
            const quint64 over = h->queueOverruns.load();
            const bool news = gap != h->lastGap || lost != h->lastLost
                || over != h->lastOverruns;
            if (!news && !beat) {
                ++it;
                continue;
            }
            qCInfo(lcSfuMedia).nospace().noquote()
                << "audio receive health stream=\"" << h->streamId
                << "\" concealEvents(gap)=+" << (gap - h->lastGap)
                << " packetLostEvents=+" << (lost - h->lastLost)
                << " queueOverruns=+" << (over - h->lastOverruns)
                << " totals gap=" << gap << " lost=" << lost
                << " overruns=" << over;
            h->lastGap = gap;
            h->lastLost = lost;
            h->lastOverruns = over;
            ++it;
        }
    }
    if (!m_subscriber.webrtc || !GST_IS_BIN(m_subscriber.webrtc))
        return;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.webrtc));
    GValue item = G_VALUE_INIT;
    bool done = false;
    int logged = 0;
    while (!done && logged < 16) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *el = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(el);
            const gchar *fname =
                factory ? gst_plugin_feature_get_name(
                              GST_PLUGIN_FEATURE(factory))
                        : nullptr;
            if (fname && g_strcmp0(fname, "rtpjitterbuffer") == 0
                && g_object_class_find_property(G_OBJECT_GET_CLASS(el),
                                                "stats")) {
                GstStructure *st = nullptr;
                g_object_get(el, "stats", &st, nullptr);
                if (st) {
                    guint64 pushed = 0, lost = 0, late = 0, dup = 0,
                            avgJitter = 0, rtx = 0, rtxOk = 0;
                    gst_structure_get_uint64(st, "num-pushed", &pushed);
                    gst_structure_get_uint64(st, "num-lost", &lost);
                    gst_structure_get_uint64(st, "num-late", &late);
                    gst_structure_get_uint64(st, "num-duplicates", &dup);
                    gst_structure_get_uint64(st, "avg-jitter", &avgJitter);
                    gst_structure_get_uint64(st, "rtx-count", &rtx);
                    gst_structure_get_uint64(st, "rtx-success-count", &rtxOk);
                    gst_structure_free(st);
                    const QString name = QString::fromUtf8(GST_OBJECT_NAME(el));
                    std::array<quint64, 4> &last = m_lastJitterbuffer[name];
                    if (lost != last[1] || late != last[2] || dup != last[3]
                        || (beat && pushed != last[0])) {
                        qCInfo(lcSfuMedia).nospace().noquote()
                            << "jitterbuffer " << name << " pushed=+"
                            << (pushed - last[0]) << " lost=+"
                            << (lost - last[1]) << " late=+"
                            << (late - last[2]) << " duplicates=+"
                            << (dup - last[3]) << " avgJitterMs="
                            << QString::number(double(avgJitter) / 1.0e6, 'f',
                                               1)
                            << " rtx=" << rtx << " rtxOk=" << rtxOk
                            << " totals lost=" << lost << " late=" << late;
                        ++logged;
                    }
                    last = {pushed, lost, late, dup};
                }
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
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

void SfuMediaEngine::setVideoRouter(SfuVideoRouter *router)
{
    m_videoRouter = router;
}

SfuVideoRouter *SfuMediaEngine::videoRouter() const
{
    return m_videoRouter.data();
}

int SfuMediaEngine::adoptedOutboundKeyIndexForTest() const
{
    return m_sendCryptor->currentKeyIndex();
}

void SfuMediaEngine::setOutboundKey(int index, const QByteArray &rawKey,
                                    bool adopt)
{
    if (!m_sendCryptor->setKey(index, rawKey))
        return;
    if (adopt)
        m_sendCryptor->setCurrentKeyIndex(index);
    if (adopt)
        m_sendKeyReady.store(true);
}

void SfuMediaEngine::setInboundKey(const QString &senderName, int index,
                                   const QByteArray &rawKey)
{
    if (senderName.isEmpty()) {
        qCWarning(lcSfuMedia) << "call diagnosis: a media key was DISCARDED "
                                 "because it named no ring — index=" << index;
        return;
    }
    if (!recvCryptorFor(senderName)->setKey(index, rawKey)) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: the media key for ring="
            << printableRing(senderName)
            << "index=" << index
            << "was REFUSED by the cryptor (unusable index or key length="
            << rawKey.size() << ")";
        return;
    }
    m_recvKeyReady.store(true);
    if (noteKeyArrival(senderName, index)) {
        qCInfo(lcSfuMedia) << "call diagnosis: a media key ARRIVED and was "
                              "installed for ring="
                           << printableRing(senderName)
                           << "index=" << index;
    }
}

bool SfuMediaEngine::noteKeyArrival(const QString &ring, int index)
{
    QMutexLocker lock(&m_diagnosedMutex);
    auto it = m_keyArrivals.find(ring);
    if (it == m_keyArrivals.end()) {
        if (m_keyArrivals.size() >= 256)
            return false;
        it = m_keyArrivals.insert(ring, KeyArrivalLog{});
    }
    KeyArrivalLog &log = it.value();
    if (log.lastIndex == index)
        return false;
    log.lastIndex = index;
    ++log.arrivals;
    if (log.arrivals <= 8 || log.arrivals % 16 == 0)
        return true;
    return false;
}

bool SfuMediaEngine::noteDiagnosisOnce(const QString &subject)
{
    QMutexLocker lock(&m_diagnosedMutex);
    if (m_diagnosedOnce.size() >= 512)
        return false;
    if (m_diagnosedOnce.contains(subject))
        return false;
    m_diagnosedOnce.insert(subject);
    return true;
}

std::shared_ptr<CallFrameCryptor>
SfuMediaEngine::recvCryptorFor(const QString &name)
{
    QMutexLocker lock(&m_recvMutex);
    auto it = m_recvCryptors.constFind(name);
    if (it != m_recvCryptors.cend())
        return it.value();
    // An unknown name gets its own ring; decrypting with another sender's key
    // would be silent corruption.
    //
    // Bounded, since names come from remote input and each ring holds up to
    // kMaxKeysPerRing keys. Past the cap a new name gets a detached ring that
    // is never stored and decrypts nothing.
    constexpr int kMaxReceiveRings = 1024;
    auto cryptor = std::make_shared<CallFrameCryptor>();
    if (m_recvCryptors.size() >= kMaxReceiveRings) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qCWarning(lcSfuMedia)
                << "call diagnosis: the receive key ring cap of"
                << kMaxReceiveRings
                << "is full — further senders get a ring that cannot hold a "
                   "key and their media will never decrypt";
        }
        return cryptor;
    }
    m_recvCryptors.insert(name, cryptor);
    return cryptor;
}

void SfuMediaEngine::noteParticipantIdentity(const QString &streamId,
                                             const QString &senderName)
{
    if (streamId.isEmpty() || senderName.isEmpty()
        || streamId.size() > 256 || senderName.size() > 512
        || streamId == senderName) {
        const char *why =
            streamId.isEmpty()      ? "no stream id"
            : senderName.isEmpty()  ? "no sender name"
            : streamId == senderName ? "the stream id and the sender are the "
                                       "same string"
                                     : "an identifier is over its size cap";
        if (noteDiagnosisOnce(QStringLiteral("bind:%1:%2")
                                  .arg(streamId, senderName))) {
            qCWarning(lcSfuMedia)
                << "call diagnosis: a media key ring could NOT be bound to a "
                   "sending stream, so that participant's frames will not "
                   "decrypt —"
                << why;
        }
        return;
    }
    QMutexLocker lock(&m_recvMutex);
    auto byName = m_recvCryptors.constFind(senderName);
    auto byStream = m_recvCryptors.constFind(streamId);
    if (byName != m_recvCryptors.cend()
        && byStream != m_recvCryptors.cend()
        && byName.value() == byStream.value()) {
        return;
    }
    std::shared_ptr<CallFrameCryptor> shared =
        byName != m_recvCryptors.cend()
            ? byName.value()
            : (byStream != m_recvCryptors.cend()
                   ? byStream.value()
                   : std::make_shared<CallFrameCryptor>());
    m_recvCryptors.insert(senderName, shared);
    m_recvCryptors.insert(streamId, shared);
}

bool SfuMediaEngine::encryptionActive() const
{
    return m_sendKeyReady.load();
}

void SfuMediaEngine::setEncryptionRequired(bool required)
{
    m_encryptionRequired.store(required);
}

void SfuMediaEngine::clearKeys()
{
    m_sendCryptor->clearKeys();
    {
        QMutexLocker lock(&m_recvMutex);
        for (const auto &cryptor : std::as_const(m_recvCryptors))
            cryptor->clearKeys();
        m_recvCryptors.clear();
        m_streamForMline.clear();
        m_midForMline.clear();
        m_trackForMline.clear();
    }
    m_sendKeyReady.store(false);
    m_recvKeyReady.store(false);
    setServerInjectedTrailer(QByteArray());
}

void SfuMediaEngine::clearSessionRouting()
{
    {
        QMutexLocker lock(&m_recvMutex);
        m_streamForMline.clear();
        m_midForMline.clear();
        m_trackForMline.clear();
    }
    setServerInjectedTrailer(QByteArray());
}

void SfuMediaEngine::setServerInjectedTrailer(const QByteArray &trailer)
{
    const bool usable = CallFrameCryptor::isUsableServerTrailer(trailer);
    {
        QMutexLocker lock(&m_sifMutex);
        const QByteArray next = usable ? trailer : QByteArray();
        if (next == m_sifTrailer)
            return;
        m_sifTrailer = next;
    }
    if (usable) {
        qCInfo(lcSfuMedia)
            << "sfu join sifTrailer len=" << trailer.size() << "last="
            << static_cast<int>(
                   static_cast<unsigned char>(trailer.at(trailer.size() - 1)));
    } else if (!trailer.isEmpty() && !m_sifRefusedWarned) {
        m_sifRefusedWarned = true;
        qCWarning(lcSfuMedia)
            << "sfu join sifTrailer REFUSED len=" << trailer.size()
            << "(need" << CallFrameCryptor::kMinServerTrailerBytes << "to"
            << CallFrameCryptor::kMaxServerTrailerBytes
            << "base62 bytes); server-injected frames will read as decrypt "
               "failures";
    }
}

QByteArray SfuMediaEngine::serverInjectedTrailer() const
{
    QMutexLocker lock(&m_sifMutex);
    return m_sifTrailer;
}

void SfuMediaEngine::noteStreamIds(const QHash<int, QString> &byMline,
                                   const QHash<int, QString> &midsByMline,
                                   const QHash<int, QString> &tracksByMline)
{
    QMutexLocker lock(&m_recvMutex);
    m_streamForMline.clear();
    m_midForMline.clear();
    m_trackForMline.clear();
    for (auto it = byMline.cbegin(); it != byMline.cend(); ++it) {
        if (it.value().isEmpty() || it.value().size() > 256)
            m_streamForMline.remove(it.key());
        else
            m_streamForMline.insert(it.key(), it.value());
    }
    for (auto it = midsByMline.cbegin(); it != midsByMline.cend(); ++it) {
        if (it.value().isEmpty() || it.value().size() > 128)
            m_midForMline.remove(it.key());
        else
            m_midForMline.insert(it.key(), it.value());
    }
    for (auto it = tracksByMline.cbegin(); it != tracksByMline.cend(); ++it) {
        if (it.value().isEmpty() || it.value().size() > 128)
            m_trackForMline.remove(it.key());
        else
            m_trackForMline.insert(it.key(), it.value());
    }
}

void SfuMediaEngine::installEncryptProbe(GstPad *pad, bool video,
                                         std::shared_ptr<ProbeGate> gate,
                                         bool h264)
{
    if (!pad)
        return;
    auto *ctx = new CryptoProbeCtx;
    ctx->engine = this;
    ctx->gate = std::move(gate);
    ctx->cryptor = std::shared_ptr<CallFrameCryptor>(m_sendCryptor.get(),
                                                     [](CallFrameCryptor *) {});
    ctx->encrypting = true;
    ctx->video = video;
    ctx->h264 = h264;
    ctx->required = &m_encryptionRequired;
    ctx->keyReady = &m_sendKeyReady;
    ctx->total = &m_framesEncrypted;
    ctx->totalDropped = &m_framesDropped;
    ctx->ivStream = m_nextIvStream.fetch_add(1);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cryptoProbe, ctx,
                      cryptoProbeCtxFree);
}

void SfuMediaEngine::installDecryptProbe(GstPad *pad, bool video,
                                         const QString &streamId,
                                         const QString &videoCodec,
                                         StreamInfoPtr stats)
{
    if (!pad)
        return;
    auto *ctx = new CryptoProbeCtx;
    ctx->streamInfo = std::move(stats);
    ctx->engine = this;
    ctx->streamId = streamId;
    recvCryptorFor(streamId);
    ctx->encrypting = false;
    ctx->video = video;
    ctx->h264 = videoCodec == QLatin1String("H264");
    ctx->encryptedWhole = false;
    ctx->required = &m_encryptionRequired;
    ctx->keyReady = &m_recvKeyReady;
    ctx->total = &m_framesDecrypted;
    ctx->totalDropped = &m_framesDropped;
    ctx->totalCiphertextShaped = &m_framesClearButCiphertextShaped;
    ctx->totalServerInjected = &m_framesServerInjected;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cryptoProbe, ctx,
                      cryptoProbeCtxFree);
}

bool SfuMediaEngine::tokenIsLive(quintptr token, quint64 generation,
                                 Target *target) const
{
    if (!m_active)
        return false;
    if (generation != m_generation.load())
        return false;
    if (m_publisher.webrtc
        && token == reinterpret_cast<quintptr>(m_publisher.webrtc)) {
        if (target)
            *target = Target::Publisher;
        return true;
    }
    if (m_subscriber.webrtc
        && token == reinterpret_cast<quintptr>(m_subscriber.webrtc)) {
        if (target)
            *target = Target::Subscriber;
        return true;
    }
    return false;
}

void SfuMediaEngine::handleLocalDescription(quintptr token, quint64 generation,
                                            bool offer, const QString &sdp)
{
    Target target = Target::Publisher;
    if (!tokenIsLive(token, generation, &target)) {
        qCWarning(lcSfuMedia) << "local description dropped: stale session"
                              << "offer=" << offer;
        return;
    }
    qCInfo(lcSfuMedia) << "local description ready offer=" << offer
                       << "target=" << static_cast<int>(target)
                       << "bytes=" << sdp.size();
    if (!qEnvironmentVariableIsEmpty("LIGHTNING_SDP_TRACE")) {
        qCInfo(lcSfuMedia) << "  sdp codecs (local" << (offer ? "offer" : "answer")
                           << "target" << static_cast<int>(target)
                           << "):" << sdpCodecSummary(sdp);
    }
    if (target == Target::Subscriber) {
        QStringList sections;
        int index = 0;
        const QStringList lines = sdp.split(QLatin1Char('\n'));
        QString mid, kind, direction;
        bool portOpen = false;
        auto flush = [&] {
            if (kind.isEmpty())
                return;
            sections << QStringLiteral("[%1 mid=%2 %3 %4]")
                            .arg(QString::number(index++), mid.isEmpty()
                                     ? QStringLiteral("?") : mid, kind,
                                 portOpen
                                     ? (direction.isEmpty()
                                            ? QStringLiteral("dir=?")
                                            : QStringLiteral("dir=%1")
                                                  .arg(direction))
                                     : QStringLiteral("REJECTED-port0"));
            mid.clear(); kind.clear(); direction.clear(); portOpen = false;
        };
        QSet<QString> ended;
        auto noteEnded = [&] {
            if ((kind == QLatin1String("audio") || kind == QLatin1String("video"))
                && !mid.isEmpty()
                && (!portOpen || direction == QLatin1String("inactive"))) {
                ended.insert(mid);
            }
        };
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("m="))) {
                noteEnded();
                flush();
                const QStringList f = line.mid(2).split(QLatin1Char(' '));
                kind = f.value(0);
                portOpen = f.value(1) != QLatin1String("0");
            } else if (line.startsWith(QLatin1String("a=mid:"))) {
                mid = line.mid(6);
            } else if (line == QLatin1String("a=recvonly")
                       || line == QLatin1String("a=sendonly")
                       || line == QLatin1String("a=sendrecv")
                       || line == QLatin1String("a=inactive")) {
                direction = line.mid(2);
            }
        }
        noteEnded();
        flush();
        qCInfo(lcSfuMedia) << "subscriber answer sections="
                           << sections.join(QLatin1Char(' '));
        retireReceiveBins(ended, nullptr, "its section went inactive",
                          m_subscriber.webrtc);
    }
    Q_EMIT localDescription(static_cast<int>(target),
                            offer ? QStringLiteral("offer")
                                  : QStringLiteral("answer"),
                            sdp);
}

void SfuMediaEngine::handleLocalCandidate(quintptr token, quint64 generation,
                                          int mlineIndex,
                                          const QString &candidate)
{
    Target target = Target::Publisher;
    if (!tokenIsLive(token, generation, &target))
        return;
    Q_EMIT localCandidate(static_cast<int>(target),
                          candidateInitJson(candidate, mlineIndex));
}

void SfuMediaEngine::handleFailure(quintptr token, quint64 generation,
                                   const QString &category)
{
    if (!tokenIsLive(token, generation))
        return;
    Q_EMIT failed(category);
}

void SfuMediaEngine::handleTransportState(quintptr token, quint64 generation,
                                          const QString &state)
{
    Target target = Target::Publisher;
    if (!tokenIsLive(token, generation, &target))
        return;
    Q_EMIT transportStateChanged(static_cast<int>(target), state);
}

void SfuMediaEngine::handleCaptureEnded(const QString &cid, int captureEpoch)
{
    if (!m_active || !m_publishedBins.contains(cid))
        return;
    auto watch = m_publishWatch.find(cid);
    if (watch == m_publishWatch.end() || !watch->state || watch->reported
        || (watch->retry && watch->retry->isStale(captureEpoch)))
        return;
    const bool screenShare = watch->state->screenShare;
    watch->reported = true;
    qCInfo(lcSfuMedia) << "capture ended itself; retiring the publish"
                       << "screenShare=" << screenShare;
    Q_EMIT publishFailed(cid,
                         screenShare
                             ? QStringLiteral("screen_share_source_closed")
                             : QStringLiteral("camera_source_closed"));
}

QString SfuMediaEngine::publishedBinPropertyForTest(
    const QString &cid, const QString &elementName,
    const QString &property) const
{
    GstElement *bin = m_publishedBins.value(cid, nullptr);
    if (!bin)
        return {};
    GstElement *found =
        gst_bin_get_by_name(GST_BIN(bin), elementName.toUtf8().constData());
    if (!found)
        return {};
    const QByteArray name = property.toUtf8();
    QString out;
    if (GParamSpec *spec = g_object_class_find_property(
            G_OBJECT_GET_CLASS(found), name.constData())) {
        GValue read = G_VALUE_INIT;
        g_value_init(&read, spec->value_type);
        g_object_get_property(G_OBJECT(found), name.constData(), &read);
        if (G_VALUE_HOLDS_ENUM(&read)) {
            auto *klass =
                static_cast<GEnumClass *>(g_type_class_ref(G_VALUE_TYPE(&read)));
            const GEnumValue *value =
                g_enum_get_value(klass, g_value_get_enum(&read));
            out = QString::fromUtf8(value ? value->value_nick : "");
            g_type_class_unref(klass);
        } else {
            gchar *text = gst_value_serialize(&read);
            out = QString::fromUtf8(text ? text : "");
            g_free(text);
        }
        g_value_unset(&read);
    }
    gst_object_unref(found);
    return out;
}

bool SfuMediaEngine::publishedBinHasElementForTest(
    const QString &cid, const QString &elementName) const
{
    GstElement *bin = m_publishedBins.value(cid, nullptr);
    if (!bin)
        return false;
    GstElement *found =
        gst_bin_get_by_name(GST_BIN(bin), elementName.toUtf8().constData());
    if (!found)
        return false;
    gst_object_unref(found);
    return true;
}

QStringList SfuMediaEngine::microphoneElementsForTest()
{
    return microphoneElementPreference();
}

qint64 SfuMediaEngine::micSilenceSince(double peakDb, qint64 silentSinceMs,
                                       qint64 nowMs)
{
    if (peakDb > kMicSilenceCeilingDb)
        return -1;
    return silentSinceMs < 0 ? nowMs : silentSinceMs;
}

bool SfuMediaEngine::micSilenceReached(qint64 silentSinceMs, qint64 nowMs)
{
    if (silentSinceMs < 0)
        return false;
    return nowMs - silentSinceMs >= kMicSilenceWindowMs;
}

qint64 SfuMediaEngine::micQuietSince(double peakDb, qint64 quietSinceMs,
                                     qint64 nowMs)
{
    if (peakDb > kMicQuietCeilingDb)
        return -1;
    return quietSinceMs < 0 ? nowMs : quietSinceMs;
}

bool SfuMediaEngine::micQuietReached(qint64 quietSinceMs, qint64 nowMs)
{
    if (quietSinceMs < 0)
        return false;
    return nowMs - quietSinceMs >= kMicQuietWindowMs;
}

quint64 SfuMediaEngine::micBuffersHeldToClock() const
{
    return m_micClockHold ? m_micClockHold->held.load() : 0;
}

void SfuMediaEngine::resetMicLevelState()
{
    const bool wasAnnounced = m_micSilentAnnounced;
    m_micSilentSinceMs = -1;
    m_micQuietSinceMs = -1;
    m_micQuietAnnounced = false;
    m_micSilentAnnounced = false;
    m_micPeakDb = 0;
    m_micSentPeakDb = -350.0;
    m_micLastLogMs = 0;
    if (wasAnnounced)
        Q_EMIT localAudioSilent(false, 0);
}

void SfuMediaEngine::handleMeterLevel(double peakDb)
{
    if (m_microphoneMuted)
        return;
    m_micSentPeakDb = peakDb;
    Q_EMIT localAudioLevel(peakDb);
}

void SfuMediaEngine::handleMicLevel(double peakDb)
{
    static QElapsedTimer monotonic;
    if (!monotonic.isValid())
        monotonic.start();
    handleMicLevelAt(peakDb, monotonic.elapsed());
}

bool SfuMediaEngine::micRestartBudgetResets(int restarts, qint64 sinceRestartMs,
                                            bool deliveredSinceRestart)
{
    if (restarts <= 0)
        return false;
    if (sinceRestartMs > 60000)
        return true;
    return deliveredSinceRestart && sinceRestartMs >= 10000;
}

void SfuMediaEngine::handleMicLevelAt(double peakDb, qint64 nowMs)
{
    m_micPeakDb = peakDb;

    if (m_microphoneMuted) {
        m_micSilentSinceMs = -1;
        m_micQuietSinceMs = -1;
        m_micQuietAnnounced = false;
        if (m_micSilentAnnounced) {
            m_micSilentAnnounced = false;
            Q_EMIT localAudioSilent(false, peakDb);
        }
        return;
    }

    if (m_micLastLogMs == 0 || nowMs - m_micLastLogMs >= 5000) {
        m_micLastLogMs = nowMs;
        qCInfo(lcSfuMedia)
            << "microphone level peak=" << qRound(peakDb) << "dBFS sent="
            << qRound(m_micSentPeakDb) << "dBFS";
    }

    m_micSilentSinceMs = micSilenceSince(peakDb, m_micSilentSinceMs, nowMs);
    m_micQuietSinceMs = micQuietSince(peakDb, m_micQuietSinceMs, nowMs);
    const bool quiet = micQuietReached(m_micQuietSinceMs, nowMs);
    if (quiet != m_micQuietAnnounced) {
        m_micQuietAnnounced = quiet;
        if (quiet) {
            qCWarning(lcSfuMedia)
                << "the microphone is very quiet: peak has stayed at or "
                   "below"
                << kMicQuietCeilingDb << "dBFS for"
                << (kMicQuietWindowMs / 1000)
                << "s while unmuted. The device is live; check the input "
                   "level and the selected microphone in call settings.";
        } else {
            qCInfo(lcSfuMedia) << "microphone level recovered peak="
                               << qRound(peakDb) << "dBFS";
        }
    }
    const bool silent = micSilenceReached(m_micSilentSinceMs, nowMs);
    if (silent == m_micSilentAnnounced)
        return;
    m_micSilentAnnounced = silent;
    if (silent) {
        qCWarning(lcSfuMedia)
            << "THE MICROPHONE IS CAPTURING NOTHING: peak has stayed at or "
               "below"
            << kMicSilenceCeilingDb << "dBFS for"
            << (kMicSilenceWindowMs / 1000)
            << "s while unmuted — every frame published in that time carried "
               "silence, and the far end cannot hear this device. Check the "
               "microphone selected in call settings.";
    } else {
        qCInfo(lcSfuMedia) << "microphone is capturing again peak="
                           << qRound(peakDb) << "dBFS";
    }
    Q_EMIT localAudioSilent(silent, peakDb);
}

namespace {
std::atomic<bool> g_audioLevelExtensionDisabled{false};

struct SourceRestart {
    QByteArray front;
    std::shared_ptr<std::atomic<int>> done;
};

void sourceRestartFree(gpointer data)
{
    delete static_cast<SourceRestart *>(data);
}

void restartSourceAsync(GstElement *bin, gpointer data)
{
    const auto *ctx = static_cast<const SourceRestart *>(data);
    struct Done {
        const SourceRestart *ctx;
        ~Done()
        {
            if (ctx && ctx->done)
                ctx->done->fetch_add(1);
        }
    } done{ctx};
    if (ctx && !ctx->front.isEmpty()) {
        GstElement *front =
            gst_bin_get_by_name(GST_BIN(bin), ctx->front.constData());
        GstElement *source =
            front ? gst_bin_get_by_name(GST_BIN(front), "micsrc") : nullptr;
        if (source) {
            gst_element_set_state(source, GST_STATE_NULL);
            gst_element_sync_state_with_parent(source);
            gst_object_unref(source);
        }
        if (front)
            gst_object_unref(front);
        return;
    }
    GstElement *source = gst_bin_get_by_name(GST_BIN(bin), "micsrc");
    if (source)
        gst_element_set_state(source, GST_STATE_NULL);
    GstState state = GST_STATE_VOID_PENDING;
    gst_element_get_state(bin, &state, nullptr, 0);
    if (state != GST_STATE_PLAYING)
        gst_element_sync_state_with_parent(bin);
    else if (source)
        gst_element_sync_state_with_parent(source);
    if (source)
        gst_object_unref(source);
}
}

int SfuMediaEngine::subscriberAutoremoveForTest() const
{
    if (!m_subscriber.webrtc)
        return -1;
    GstElement *rtpbin =
        gst_bin_get_by_name(GST_BIN(m_subscriber.webrtc), "rtpbin");
    if (!rtpbin)
        return -1;
    gboolean on = FALSE;
    g_object_get(rtpbin, "autoremove", &on, nullptr);
    gst_object_unref(rtpbin);
    return on ? 1 : 0;
}

void SfuMediaEngine::disableAudioLevelExtensionForTest(bool disabled)
{
    g_audioLevelExtensionDisabled.store(disabled);
}

bool SfuMediaEngine::audioLevelExtensionAvailable()
{
    if (g_audioLevelExtensionDisabled.load())
        return false;
    static const bool available = [] {
        GstElementFactory *ext =
            gst_element_factory_find("rtphdrextclientaudiolevel");
        if (!ext)
            return false;
        gst_object_unref(ext);
        GstElement *meter = gst_element_factory_make("level", nullptr);
        if (!meter)
            return false;
        const bool meta = g_object_class_find_property(
                              G_OBJECT_GET_CLASS(meter), "audio-level-meta")
            != nullptr;
        gst_object_unref(meter);
        return meta;
    }();
    return available;
}

void SfuMediaEngine::handleCaptureError(const QString &cid,
                                        const QString &front)
{
    if (!m_active || cid.isEmpty() || cid != m_micCid)
        return;
    const bool fromPendingSwap = m_micSwap.front && !front.isEmpty()
        && front == m_micSwap.name;
    if (front != m_micLiveFront && !fromPendingSwap) {
        qCInfo(lcSfuMedia) << "microphone: an error from a capture already "
                              "replaced is ignored";
        return;
    }
    if (m_micSwap.front) {
        if (!front.isEmpty() && front == m_micSwap.name) {
            abandonMicrophoneSwap("the chosen microphone failed to start");
            return;
        }
        m_micSwap.currentFailed = true;
        return;
    }
    if (m_micRestartPending || m_micFailureReported)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool delivered = m_micCaptured
        && m_micCaptured->load() > m_micCapturedAtRestart;
    if (micRestartBudgetResets(m_micRestarts, now - m_micLastRestartMs,
                               delivered))
        m_micRestarts = 0;
    if (m_micRestarts >= kMaxMicRestarts) {
        m_micFailureReported = true;
        qCWarning(lcSfuMedia)
            << "call diagnosis: gave up restarting the microphone after"
            << m_micRestarts << "attempts; this device cannot be heard. "
               "Trying it again every"
            << m_micSlowRetryMs / 1000 << "s";
        Q_EMIT publishFailed(cid, QStringLiteral("audio_source_failed"));
        scheduleMicSlowRetry(cid);
        return;
    }
    m_micRestartPending = true;
    const int delay = m_micRestartBaseDelayMs * (1 << m_micRestarts);
    qCWarning(lcSfuMedia)
        << "call diagnosis: the microphone capture FAILED; restarting it in"
        << delay << "ms (attempt" << (m_micRestarts + 1) << "of"
        << kMaxMicRestarts << ")";
    const quint64 generation = m_generation.load();
    QTimer::singleShot(delay, this, [this, cid, generation] {
        restartMicrophone(cid, generation);
    });
}

void SfuMediaEngine::scheduleMicSlowRetry(const QString &cid)
{
    const quint64 generation = m_generation.load();
    QTimer::singleShot(m_micSlowRetryMs, this, [this, cid, generation] {
        if (!m_active || generation != m_generation.load() || cid != m_micCid
            || !m_micFailureReported)
            return;
        const quint64 before = m_micCaptured ? m_micCaptured->load() : 0;
        QTimer::singleShot(1000, this, [this, cid, generation, before] {
            if (!m_active || generation != m_generation.load()
                || cid != m_micCid || !m_micFailureReported)
                return;
            const quint64 now = m_micCaptured ? m_micCaptured->load() : 0;
            if (now > before) {
                m_micFailureReported = false;
                m_micRestarts = 0;
                qCInfo(lcSfuMedia) << "call diagnosis: the microphone is "
                                      "capturing again after a restart";
                Q_EMIT microphoneRecovered(cid);
                return;
            }
            qCInfo(lcSfuMedia) << "call diagnosis: retrying the microphone; "
                                  "it is delivering nothing";
            m_micLastRestartMs = QDateTime::currentMSecsSinceEpoch();
            m_micCapturedAtRestart = now;
            if (GstElement *bin = m_publishedBins.value(cid);
                bin && !m_micSwap.front && !microphoneStarting())
                gst_element_call_async(
                    bin, restartSourceAsync,
                    new SourceRestart{m_micLiveFront.toUtf8(), m_micSourceRestartsDone},
                    sourceRestartFree);
            scheduleMicSlowRetry(cid);
        });
    });
}

void SfuMediaEngine::restartMicrophone(const QString &cid, quint64 generation)
{
    if (!m_active || generation != m_generation.load() || cid != m_micCid) {
        m_micRestartPending = false;
        return;
    }
    GstElement *bin = m_publishedBins.value(cid);
    if (!bin) {
        m_micRestartPending = false;
        return;
    }
    if (microphoneStarting()) {
        m_micRestartAfterStart = true;
        qCInfo(lcSfuMedia) << "microphone restart waits for its start";
        return;
    }
    m_micRestartPending = false;
    if (m_micSwap.front) {
        m_micSwap.currentFailed = true;
        return;
    }
    ++m_micRestarts;
    m_micLastRestartMs = QDateTime::currentMSecsSinceEpoch();
    m_micCapturedAtRestart = m_micCaptured ? m_micCaptured->load() : 0;
    gst_element_call_async(bin, restartSourceAsync,
                           new SourceRestart{m_micLiveFront.toUtf8(), m_micSourceRestartsDone},
                           sourceRestartFree);
}

void SfuMediaEngine::handlePublishError(const QString &cid,
                                        bool resourceError, bool busyError,
                                        bool captureFormatError,
                                        int captureEpoch)
{
    if (!m_active)
        return;
    if (!m_publishedBins.contains(cid))
        return;
    if (const QString primary = m_shareBackups.key(cid); !primary.isEmpty()) {
        handleShareEncoderFailed(primary);
        return;
    }
    const auto watch = m_publishWatch.constFind(cid);
    if (watch == m_publishWatch.cend() || !watch->state || watch->reported)
        return;
    if (watch->retry && watch->retry->isStale(captureEpoch))
        return;
    if (watch->state->captured.load() > 0)
        return;
    const bool screenShare = watch->state->screenShare;
    if (screenShare && captureFormatError
        && retryShareCaptureInOtherMemory(cid))
        return;
    m_publishWatch[cid].reported = true;
    qCWarning(lcSfuMedia)
        << "publish produced no capture buffers and errored; reporting it"
        << "screenShare=" << screenShare;
    Q_EMIT publishFailed(cid,
        resourceError && !busyError && !screenShare
            ? QStringLiteral("camera_resource")
            : cameraPublishErrorCategory(screenShare, busyError));
}

bool SfuMediaEngine::restartShareCapture(GstElement *bin,
                                         ShareCaptureRetry &retry)
{
    // Held across the restart so a teardown's NULL transition of this bin
    // cannot interleave and leave the source PLAYING in a disposed bin.
    GST_STATE_LOCK(bin);
    GstObject *parent = gst_object_get_parent(GST_OBJECT(bin));
    const bool live = ShareCaptureRetry::mayRestart(
        GST_STATE_TARGET(bin) == GST_STATE_PLAYING, parent != nullptr);
    if (parent)
        gst_object_unref(parent);
    GstElement *source =
        live ? gst_bin_get_by_name(GST_BIN(bin), "capsrc") : nullptr;
    GstElement *entry =
        source ? gst_bin_get_by_name(GST_BIN(bin), "capentry") : nullptr;
    if (!entry) {
        if (source)
            gst_object_unref(source);
        GST_STATE_UNLOCK(bin);
        return false;
    }
    gst_element_set_state(source, GST_STATE_NULL);
    GstCaps *current = nullptr;
    g_object_get(entry, "caps", &current, nullptr);
    const bool wasDmaBuf = current && entryOffersDmaBuf(current);
    if (current)
        gst_caps_unref(current);
    GstCaps *caps = gst_caps_from_string(
        (wasDmaBuf ? systemMemoryEntryCaps() : gpuEntryCaps())
            .toUtf8()
            .constData());
    g_object_set(entry, "caps", caps, nullptr);
    gst_caps_unref(caps);
    retry.publish();
    gst_element_sync_state_with_parent(source);
    gst_object_unref(entry);
    gst_object_unref(source);
    GST_STATE_UNLOCK(bin);
    return true;
}

bool SfuMediaEngine::retryShareCaptureInOtherMemory(const QString &cid)
{
    const auto watch = m_publishWatch.constFind(cid);
    GstElement *bin = m_publishedBins.value(cid, nullptr);
    if (watch == m_publishWatch.cend() || !watch->retry || !bin
        || !GST_IS_BIN(bin))
        return false;
    GstElement *entry = gst_bin_get_by_name(GST_BIN(bin), "capentry");
    if (!entry)
        return false;
    GstCaps *current = nullptr;
    g_object_get(entry, "caps", &current, nullptr);
    gst_object_unref(entry);
    const bool wasDmaBuf = current && entryOffersDmaBuf(current);
    if (current)
        gst_caps_unref(current);
    if (!watch->retry->begin())
        return false;
    (wasDmaBuf ? g_shareDmaBufCaptureFailed : g_shareSystemMemoryCaptureFailed)
        .store(true);
    qCWarning(lcSfuMedia)
        << "screen share capture could not negotiate in"
        << (wasDmaBuf ? "DMA-BUF" : "system memory") << "; retrying it in"
        << (wasDmaBuf ? "system memory" : "DMA-BUF");
    gst_element_call_async(
        bin,
        [](GstElement *element, gpointer data) {
            auto &retry =
                *static_cast<std::shared_ptr<ShareCaptureRetry> *>(data);
            if (!restartShareCapture(element, *retry))
                qCInfo(lcSfuMedia) << "screen share capture restart "
                                      "abandoned: the share was torn down";
        },
        new std::shared_ptr<ShareCaptureRetry>(watch->retry),
        [](gpointer data) {
            delete static_cast<std::shared_ptr<ShareCaptureRetry> *>(data);
        });
    return true;
}

void SfuMediaEngine::checkCameraFirstFrame(const QString &cid,
                                           quint64 generation)
{
    if (!m_active || generation != m_generation.load()
        || !m_publishedBins.contains(cid))
        return;
    const auto watch = m_publishWatch.constFind(cid);
    if (watch == m_publishWatch.cend() || !watch->state || watch->reported
        || watch->state->screenShare)
        return;
    if (watch->state->captured.load() > 0) {
        Q_EMIT cameraCaptureReady(cid);
        return;
    }
    GstState current = GST_STATE_VOID_PENDING;
    if (GstElement *bin = m_publishedBins.value(cid))
        gst_element_get_state(bin, &current, nullptr, 0);
    m_publishWatch[cid].reported = true;
    qCWarning(lcSfuMedia) << "camera delivered no frames; reporting it"
                          << "waitedMs="
                          << (m_publishedFds.contains(cid)
                                  ? m_portalCameraFirstFrameTimeoutMs
                                  : m_cameraFirstFrameTimeoutMs)
                          << "portal=" << m_publishedFds.contains(cid)
                          << "state=" << gst_element_state_get_name(current);
    if (GstElement *bin = m_publishedBins.value(cid)) {
        if (GstElement *source = gst_bin_get_by_name(GST_BIN(bin), "capsrc")) {
            if (GstPad *pad = gst_element_get_static_pad(source, "src")) {
                GstCaps *offered = gst_pad_query_caps(pad, nullptr);
                GstCaps *requested = gst_pad_peer_query_caps(pad, nullptr);
                GstCaps *negotiated = gst_pad_get_current_caps(pad);
                gchar *offeredText = offered ? gst_caps_to_string(offered) : nullptr;
                gchar *requestedText = requested
                    ? gst_caps_to_string(requested) : nullptr;
                gchar *negotiatedText = negotiated
                    ? gst_caps_to_string(negotiated) : nullptr;
                qCWarning(lcSfuMedia)
                    << "camera no-frames source caps offered="
                    << (offeredText ? offeredText : "<unavailable>")
                    << "negotiated="
                    << (negotiatedText ? negotiatedText : "<none>")
                    << "requestedSource="
                    << (requestedText ? requestedText : "<unavailable>")
                    << "requestedOutput="
                    << cameraLimitsCaps(m_publishedFds.contains(cid),
                                        m_cameraMaxHeight);
                g_free(offeredText);
                g_free(requestedText);
                g_free(negotiatedText);
                if (offered)
                    gst_caps_unref(offered);
                if (requested)
                    gst_caps_unref(requested);
                if (negotiated)
                    gst_caps_unref(negotiated);
                gst_object_unref(pad);
            }
            gst_object_unref(source);
        }
    }
    Q_EMIT publishFailed(cid, QStringLiteral("camera_no_frames"));
}

void SfuMediaEngine::onPeerStateNotify(GstElement *webrtc, void *paramSpec,
                                       void *userData)
{
    auto *spec = static_cast<GParamSpec *>(paramSpec);
    if (!webrtc || !spec || !spec->name)
        return;
    gint value = -1;
    g_object_get(webrtc, spec->name, &value, nullptr);
    const gchar *rawName = GST_ELEMENT_NAME(webrtc);
    qCInfo(lcSfuMedia) << "peer" << (rawName ? rawName : "?")
                       << spec->name << "=" << value;

    if (!userData || g_strcmp0(spec->name, "ice-connection-state") != 0)
        return;
    const char *word = nullptr;
    switch (value) {
    case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
    case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
        word = "connected";
        break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
        word = "disconnected";
        break;
    case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
        word = "failed";
        break;
    default:
        return;
    }
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    const QString state = QString::fromLatin1(word);
    marshal(engine, [engine, token, generation, state] {
        engine->handleTransportState(token, generation, state);
    });
}

void SfuMediaEngine::onNegotiationNeeded(GstElement *webrtc, void *userData)
{
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    marshal(engine, [engine, token, generation] {
        Target target = Target::Publisher;
        if (engine->tokenIsLive(token, generation, &target)
            && target == Target::Publisher)
            engine->renegotiatePublisher();
    });
}

void SfuMediaEngine::onOfferCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise);
        return;
    }
    auto *engine = ctx->engine;
    const quint64 generation = ctx->generation;
    GstElement *webrtc = GST_ELEMENT(gst_object_ref(ctx->webrtc));
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &description, nullptr);
    gst_promise_unref(promise);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    if (!description) {
        gst_object_unref(webrtc);
        marshal(engine, [engine, token, generation] {
            if (!engine->tokenIsLive(token, generation))
                return;
            engine->m_publisher.offerPending = false;
            engine->m_publisher.negotiationDirty = false;
            Q_EMIT engine->failed(QStringLiteral("local_offer_failed"));
        });
        return;
    }
    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    std::shared_ptr<GstWebRTCSessionDescription> held(
        description, gst_webrtc_session_description_free);
    std::shared_ptr<GstElement> element(
        webrtc, [](GstElement *e) { gst_object_unref(e); });
    marshal(engine, [engine, token, generation, held, element, sdp] {
        if (!engine->tokenIsLive(token, generation))
            return;
        Peer &peer = engine->m_publisher;
        const QString identity = offerIdentity(sdp);
        if (identity == peer.answeredOffer) {
            ++engine->m_offersDropped;
            peer.offerPending = false;
            if (peer.negotiationDirty)
                engine->renegotiatePublisher();
            return;
        }
        peer.offeredIdentity = identity;
        auto *localCtx = promiseCtxNew(engine, element.get(), true);
        localCtx->generation = generation;
        localCtx->offer = true;
        localCtx->sdp = offerForSfu(sdp, shareHardwareEncodeRequested());
        GstPromise *local = gst_promise_new_with_change_func(
            onLocalDescriptionSet, localCtx, promiseCtxFree);
        g_signal_emit_by_name(element.get(), "set-local-description",
                              held.get(), local);
    });
}

QString SfuMediaEngine::offerIdentity(const QString &sdp)
{
    QStringList kept = sdp.split(QLatin1Char('\n'));
    kept.removeIf([](const QString &line) {
        return line.startsWith(QLatin1String("o="))
            || line.startsWith(QLatin1String("a=candidate"))
            || line.startsWith(QLatin1String("a=end-of-candidates"));
    });
    return kept.join(QLatin1Char('\n'));
}

void SfuMediaEngine::onAnswerCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise);
        return;
    }
    auto *engine = ctx->engine;
    const bool publisher = ctx->publisher;
    const quint64 generation = ctx->generation;
    const bool iceRestart = ctx->iceRestart && !publisher;
    const QByteArray remoteIceUfrag = ctx->remoteIceUfrag;
    const QByteArray remoteIcePwd = ctx->remoteIcePwd;
    GstElement *webrtc = GST_ELEMENT(gst_object_ref(ctx->webrtc));
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "answer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description,
                          nullptr);
    gst_promise_unref(promise);
    if (!description) {
        const quintptr token = reinterpret_cast<quintptr>(webrtc);
        gst_object_unref(webrtc);
        marshal(engine, [engine, token, generation] {
            if (engine->tokenIsLive(token, generation))
                Q_EMIT engine->failed(QStringLiteral("local_answer_failed"));
        });
        return;
    }

    if (!publisher) {
        GArray *transceivers = nullptr;
        g_signal_emit_by_name(webrtc, "get-transceivers", &transceivers);
        QStringList summary;
        if (transceivers) {
            for (guint i = 0; i < transceivers->len; ++i) {
                auto *t = g_array_index(transceivers,
                                        GstWebRTCRTPTransceiver *, i);
                if (!t)
                    continue;
                GstWebRTCRTPTransceiverDirection dir =
                    GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_NONE;
                GstWebRTCRTPTransceiverDirection current =
                    GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_NONE;
                gchar *mid = nullptr;
                g_object_get(t, "direction", &dir, "current-direction",
                             &current, "mid", &mid, nullptr);
                summary << QStringLiteral("[mid=%1 dir=%2 current=%3]")
                               .arg(mid ? QString::fromUtf8(mid)
                                        : QStringLiteral("?"))
                               .arg(static_cast<int>(dir))
                               .arg(static_cast<int>(current));
                g_free(mid);
            }
            g_array_unref(transceivers);
        }
        qCInfo(lcSfuMedia) << "subscriber transceivers n=" << summary.size()
                           << summary.join(QLatin1Char(' '))
                           << "(dir: 1=inactive 3=recvonly 4=sendrecv)";
    }

    if (iceRestart) {
        // The answer keeps the old local credentials on purpose: libnice's
        // peer-reflexive candidates carry them and survive the restart, and the
        // set-local-description below puts them back on the stream.
        const bool restarted = restartIceStream(webrtc, remoteIceUfrag, remoteIcePwd);
        qCInfo(lcSfuMedia) << "subscriber ICE restart answered restarted=" << restarted;
    }

    gchar *text = gst_sdp_message_as_text(description->sdp);
    auto *localCtx = promiseCtxNew(engine, webrtc, publisher);
    localCtx->generation = generation;
    localCtx->iceRestart = iceRestart;
    localCtx->sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    GstPromise *local = gst_promise_new_with_change_func(
        onLocalDescriptionSet, localCtx, promiseCtxFree);
    g_signal_emit_by_name(webrtc, "set-local-description", description, local);
    gst_webrtc_session_description_free(description);
    gst_object_unref(webrtc);
}

void SfuMediaEngine::onLocalDescriptionSet(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const lightning::webrtc::CallbackScope scope(ctx->webrtc);
    if (!scope.live()) {
        gst_promise_unref(promise);
        return;
    }
    auto *engine = ctx->engine;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    const quint64 generation = ctx->generation;
    const bool offer = ctx->offer;
    const bool iceRestart = ctx->iceRestart;
    const QString sdp = ctx->sdp;
    const bool replied = gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    const GstStructure *reply = replied ? gst_promise_get_reply(promise) : nullptr;
    const bool ok = replied && (!reply || !gst_structure_has_field(reply, "error"));
    gst_promise_unref(promise);
    marshal(engine, [engine, token, generation, offer, iceRestart, sdp, ok] {
        Target target = Target::Publisher;
        if (!engine->tokenIsLive(token, generation, &target))
            return;
        if (!ok) {
            Peer &peer = engine->peerFor(target);
            peer.offerPending = false;
            peer.negotiationDirty = false;
            Q_EMIT engine->failed(QStringLiteral("local_sdp_rejected"));
            return;
        }
        if (iceRestart) {
            // The restart dropped every remote candidate the offer brought.
            Peer &peer = engine->peerFor(target);
            peer.iceRestarting = false;
            const auto candidates = std::exchange(peer.restartCandidates, {});
            for (const QString &candidate : candidates) {
                QString line;
                int mline = 0;
                if (parseCandidateInit(candidate, &line, &mline))
                    g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", mline,
                                          line.toUtf8().constData());
            }
            qCInfo(lcSfuMedia) << "subscriber ICE restart re-added remote candidates="
                               << candidates.size();
        }
        engine->handleLocalDescription(token, generation, offer, sdp);
    });
}

void SfuMediaEngine::onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                                    char *candidate, void *userData)
{
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    const QString line = QString::fromUtf8(candidate ? candidate : "");
    const int mline = static_cast<int>(mlineIndex);
    marshal(engine, [engine, token, generation, mline, line] {
        engine->handleLocalCandidate(token, generation, mline, line);
    });
}

void SfuMediaEngine::onPadRemoved(GstElement *webrtc, void *pad,
                                  void *userData)
{
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    if (!engine || !pad)
        return;
    auto *removed = static_cast<GstPad *>(pad);
    ReceiveBin entry;
    {
        QMutexLocker lock(&engine->m_receiveBinMutex);
        entry = engine->m_receiveBins.take(removed);
    }
    GstElement *bin = entry.bin;
    if (!bin) {
        return;
    }
    qCInfo(lcSfuMedia) << "a remote track's pad was removed; retiring its "
                          "receive bin";
    engine->teardownReceiveBin(entry);
}

void SfuMediaEngine::teardownReceiveBin(const ReceiveBin &entry,
                                        bool announce)
{
    GstElement *pipeline = m_subscriber.pipeline;
    if (!pipeline || !entry.bin) {
        return;
    }
    auto *ctx = new ReceiveTeardown{
        GST_ELEMENT(gst_object_ref(pipeline)), m_pendingTeardowns};
    m_pendingTeardowns->fetch_add(1);
    gst_element_call_async(entry.bin, receiveTeardownAsync, ctx,
                           receiveTeardownFree);
    if (!announce)
        return;
    const QString streamId = entry.streamId;
    const QString kind = entry.kind;
    marshal(this, [engine = this, streamId, kind] {
        Q_EMIT engine->remoteTrackRemoved(streamId, kind);
    });
}

int SfuMediaEngine::retireReceiveBins(const QSet<QString> &transceiverMids,
                                      GstPad *keep, const char *reason,
                                      GstElement *subscriberWebrtc)
{
    if (transceiverMids.isEmpty())
        return 0;
    struct Retired {
        GstPad *pad;
        ReceiveBin entry;
    };
    QList<Retired> retired;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (auto it = m_receiveBins.begin(); it != m_receiveBins.end();) {
            if (it.key() != keep && !it.value().transceiverMid.isEmpty()
                && transceiverMids.contains(it.value().transceiverMid)) {
                retired.append({it.key(), it.value()});
                it = m_receiveBins.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Retired &r : std::as_const(retired)) {
        // Before the bin goes: the pad stays on webrtcbin for good.
        gst_pad_add_probe(r.pad,
                          static_cast<GstPadProbeType>(
                              GST_PAD_PROBE_TYPE_BUFFER
                              | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                          dropRetiredPadProbe, nullptr, nullptr);
        qCInfo(lcSfuMedia) << "retiring a receive bin: stream="
                           << r.entry.streamId << "kind=" << r.entry.kind
                           << "transceiver mid=" << r.entry.transceiverMid
                           << "reason=" << reason;
        teardownReceiveBin(r.entry);
    }
    if (!retired.isEmpty()) {
        qCInfo(lcSfuMedia) << "subscriber rtp jitterbuffers="
                           << subscriberJitterbufferCount(subscriberWebrtc)
                           << "receive bins=" << receiveBinCount()
                           << "after retiring=" << retired.size();
    }
    return static_cast<int>(retired.size());
}

int SfuMediaEngine::subscriberJitterbufferCount(GstElement *webrtc)
{
    if (!webrtc)
        return -1;
    GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(webrtc), "rtpbin");
    if (!rtpbin)
        return -1;
    int count = 0;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(rtpbin));
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory
                && g_strcmp0(GST_OBJECT_NAME(factory), "rtpjitterbuffer") == 0)
                ++count;
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            count = 0;
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
    gst_object_unref(rtpbin);
    return count;
}

int SfuMediaEngine::receiveBinCount() const
{
    QMutexLocker lock(&m_receiveBinMutex);
    return static_cast<int>(m_receiveBins.size());
}

void SfuMediaEngine::isolateFailedReceiveBin(GstElement *pipelineChild)
{
    GstPad *pad = nullptr;
    QString streamId;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (auto it = m_receiveBins.begin(); it != m_receiveBins.end(); ++it) {
            if (it->bin != pipelineChild)
                continue;
            if (it->isolateProbe != 0)
                return;
            bool runStarted = true;
            for (const ReceiveBin &other : std::as_const(m_receiveBins))
                runStarted = runStarted && other.isolateProbe == 0;
            if (runStarted)
                m_receiveSinkEpoch.fetch_add(1);
            it->isolateProbe = gst_pad_add_probe(
                it.key(),
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER
                                             | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                dropRetiredPadProbe, nullptr, nullptr);
            pad = it.key();
            streamId = it->streamId;
            break;
        }
    }
    if (!pad)
        return;
    qCWarning(lcSfuMedia)
        << "call diagnosis: the output of the receive chain for stream="
        << streamId << "FAILED; isolated so the other tracks keep playing, "
                       "and it will be rebuilt";
    const quint64 generation = m_generation.load();
    marshal(this, [engine = this, pad, generation] {
        engine->scheduleReceiveRebuild(pad, generation);
    });
}

void SfuMediaEngine::scheduleReceiveRebuild(GstPad *pad, quint64 generation)
{
    if (!m_active || generation != m_generation.load())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int attempt = 0;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        auto it = m_receiveBins.find(pad);
        if (it == m_receiveBins.end() || it->isolateProbe == 0
            || it->rebuilding)
            return;
        if (it->rebuilds > 0
            && (now - it->lastRebuildMs > 60000
                || (it->recoveredMs > 0
                    && now - it->recoveredMs >= m_freshRebuildRunAfterMs)))
            it->rebuilds = 0;
        attempt = it->rebuilds;
    }
    if (attempt >= kMaxReceiveRebuilds) {
        bool rearmNow = false;
        bool wasRearmed = false;
        {
            QMutexLocker lock(&m_receiveBinMutex);
            auto it = m_receiveBins.find(pad);
            if (it == m_receiveBins.end())
                return;
            wasRearmed = it->rearmed;
            it->rearmed = false;
            if (it->rearmOnGiveUp) {
                it->rearmOnGiveUp = false;
                it->rearmed = true;
                it->rebuilds = kMaxReceiveRebuilds - 1;
                rearmNow = true;
            } else {
                it->givenUp = true;
            }
        }
        if (rearmNow) {
            ++m_receiveRearms;
            qCInfo(lcSfuMedia)
                << "call diagnosis: an audio output was added or removed "
                   "while a receive chain was failing; one more rebuild "
                   "before giving up";
            rebuildReceiveBin(pad, generation);
            return;
        }
        if (wasRearmed) {
            qCWarning(lcSfuMedia)
                << "call diagnosis: the rebuild after the audio outputs "
                   "changed failed too; that track stays silent until an "
                   "output is added again";
        } else {
            qCWarning(lcSfuMedia)
                << "call diagnosis: gave up rebuilding a receive chain after"
                << attempt
                << "attempts; that track stays silent until an audio output "
                   "is added";
        }
        if (!m_playbackFailureAnnounced) {
            m_playbackFailureAnnounced = true;
            Q_EMIT remotePlaybackFailed(true);
        }
        return;
    }
    const int delay = m_rebuildBaseDelayMs * (1 << attempt);
    QTimer::singleShot(delay, this, [this, pad, generation] {
        rebuildReceiveBin(pad, generation);
    });
}

void SfuMediaEngine::notifyAudioOutputsChanged(bool anyOutput)
{
    if (!m_active)
        return;
    m_outputChangeAnyOutput = anyOutput;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_outputChangeBurstStartMs < 0)
        m_outputChangeBurstStartMs = now;
    const qint64 waited = now - m_outputChangeBurstStartMs;
    const qint64 left = qMax<qint64>(0, m_outputChangeMaxWaitMs - waited);
    m_outputChangeTimer.start(
        static_cast<int>(qMin<qint64>(m_outputChangeQuietMs, left)));
}

void SfuMediaEngine::rearmReceiveChains()
{
    m_outputChangeBurstStartMs = -1;
    if (!m_active || !m_subscriber.pipeline)
        return;
    const bool anyOutput = m_outputChangeAnyOutput;
    const quint64 generation = m_generation.load();
    QList<GstPad *> rearm;
    int stillRetrying = 0;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (auto it = m_receiveBins.begin(); it != m_receiveBins.end(); ++it) {
            if (it->isolateProbe == 0 || it->kind == QLatin1String("video"))
                continue;
            if (!anyOutput)
                continue;
            if (!it->givenUp) {
                it->rearmOnGiveUp = true;
                ++stillRetrying;
                continue;
            }
            if (it->rebuilding)
                continue;
            it->givenUp = false;
            it->rearmed = true;
            it->rebuilds = kMaxReceiveRebuilds - 1;
            rearm.append(it.key());
        }
    }
    if (rearm.isEmpty() && stillRetrying == 0)
        return;
    qCInfo(lcSfuMedia) << "call diagnosis: the audio outputs changed (any="
                       << anyOutput << "); rebuilding" << rearm.size()
                       << "silent receive chain(s)," << stillRetrying
                       << "still retrying";
    m_receiveRearms += static_cast<int>(rearm.size());
    for (GstPad *pad : std::as_const(rearm))
        rebuildReceiveBin(pad, generation);
}

namespace {
struct RebuildJob {
    SfuMediaEngine *engine = nullptr;
    GstElement *pipeline = nullptr;
    GstElement *bin = nullptr;
    GstPad *pad = nullptr;
    quint64 generation = 0;
    double level = -1.0;
    bool testMode = false;
    int delayMs = 0;
    std::shared_ptr<std::atomic<bool>> sessionLive;
};

void rebuildJobFree(gpointer data)
{
    auto *job = static_cast<RebuildJob *>(data);
    if (job->bin)
        gst_object_unref(job->bin);
    if (job->pad)
        gst_object_unref(job->pad);
    if (job->pipeline)
        gst_object_unref(job->pipeline);
    delete job;
}

QString sinkFactoryOf(GstElement *bin)
{
    QString name = QStringLiteral("?");
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        auto *element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory *factory = gst_element_get_factory(element);
        if (factory && !GST_IS_BIN(element)
            && GST_OBJECT_FLAG_IS_SET(element, GST_ELEMENT_FLAG_SINK))
            name = QString::fromUtf8(GST_OBJECT_NAME(factory));
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return name;
}

void stopDetachedBin(GstElement *bin, GstPad *pad)
{
    if (pad) {
        if (GstPad *peer = gst_pad_get_peer(pad)) {
            gst_pad_unlink(pad, peer);
            gst_object_unref(peer);
        }
    }
    gst_object_ref(bin);
    if (GstObject *parent = gst_object_get_parent(GST_OBJECT(bin))) {
        GST_STATE_LOCK(GST_ELEMENT(parent));
        GST_STATE_LOCK(bin);
        if (GST_OBJECT_PARENT(bin) == parent)
            gst_bin_remove(GST_BIN(parent), bin);
        GST_STATE_UNLOCK(bin);
        GST_STATE_UNLOCK(GST_ELEMENT(parent));
        gst_object_unref(parent);
    }
    GST_STATE_LOCK(bin);
    gst_element_set_state(bin, GST_STATE_NULL);
    GST_STATE_UNLOCK(bin);
    gst_object_unref(bin);
}

void stopDetachedBinAsync(GstElement *bin)
{
    gst_element_call_async(
        bin, [](GstElement *element, gpointer) {
            stopDetachedBin(element, nullptr);
        },
        nullptr, nullptr);
}
}

void SfuMediaEngine::runRebuildJob(GstElement *, void *data)
{
    auto *job = static_cast<RebuildJob *>(data);
    if (job->delayMs > 0)
        QThread::msleep(static_cast<unsigned long>(job->delayMs));
    if (!job->sessionLive->load()) {
        stopDetachedBin(job->bin, job->pad);
        return;
    }
    QString why;
    bool ok = startReceiveChain(job->pad, job->bin, &why);
    if (ok && gst_element_get_state(job->bin, nullptr, nullptr, 0)
                  == GST_STATE_CHANGE_FAILURE) {
        ok = false;
        why = QStringLiteral("would not start");
    }
    const QString outputName = sinkFactoryOf(job->bin);
    if (ok && !job->testMode && outputName == QLatin1String("fakesink")) {
        ok = false;
        why = QStringLiteral("no audio output would open");
    }
    if (!ok || !job->sessionLive->load()) {
        stopDetachedBin(job->bin, job->pad);
        if (!job->sessionLive->load())
            return;
    }
    const std::shared_ptr<GstElement> bin(
        ok ? GST_ELEMENT(gst_object_ref(job->bin)) : nullptr,
        [](GstElement *element) {
            if (element)
                gst_object_unref(element);
        });
    const std::shared_ptr<GstPad> pad(GST_PAD(gst_object_ref(job->pad)),
                                      [](GstPad *p) { gst_object_unref(p); });
    const quint64 generation = job->generation;
    const double level = job->level;
    SfuMediaEngine *engine = job->engine;
    marshal(engine, [engine, pad, generation, bin, ok, why, level,
                     outputName] {
        engine->finishReceiveRebuild(pad.get(), generation, bin.get(), ok, why,
                                     level, outputName);
    });
}

void SfuMediaEngine::rebuildReceiveBin(GstPad *pad, quint64 generation)
{
    if (!m_active || generation != m_generation.load()
        || !m_subscriber.pipeline)
        return;
    ReceiveBin entry;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        auto it = m_receiveBins.find(pad);
        if (it == m_receiveBins.end() || it->isolateProbe == 0
            || it->rebuilding)
            return;
        it->rebuilding = true;
        it->givenUp = false;
        it->rebuilds += 1;
        it->lastRebuildMs = QDateTime::currentMSecsSinceEpoch();
        it->recoveredMs = 0;
        entry = it.value();
        it->bin = nullptr;
    }
    ++m_receiveRebuildAttempts;
    const QString volumeName =
        outputVolumeElementName(volumeKeyFor(entry.streamId, entry.trackKey));
    QString clientName;
    if (entry.bin && !testSourceMode()) {
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(entry.bin));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory
                && g_strcmp0(GST_OBJECT_NAME(factory), "pulsesink") == 0) {
                gchar *client = nullptr;
                g_object_get(element, "client-name", &client, nullptr);
                const QString base =
                    QString::fromUtf8(client && *client ? client : "Lightning")
                        .section(QStringLiteral(" (reconnected"), 0, 0);
                g_free(client);
                clientName = QStringLiteral("%1 (reconnected %2)")
                                 .arg(base)
                                 .arg(m_receiveSinkEpoch.load());
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
    }
    if (!clientName.isEmpty()) {
        QMutexLocker lock(&m_receiveBinMutex);
        m_freshPulseClient = clientName;
        auto it = m_receiveBins.find(pad);
        if (it != m_receiveBins.end())
            it->rebuildClientName = clientName;
    } else {
        clientName = entry.rebuildClientName;
    }
    double level = entry.level;
    if (entry.bin) {
        if (GstElement *volume = gst_bin_get_by_name(
                GST_BIN(entry.bin), volumeName.toUtf8().constData())) {
            g_object_get(volume, "volume", &level, nullptr);
            gst_object_unref(volume);
        }
        {
            QMutexLocker lock(&m_receiveBinMutex);
            auto it = m_receiveBins.find(pad);
            if (it != m_receiveBins.end())
                it->level = level;
        }
        if (GstPad *peer = gst_pad_get_peer(pad)) {
            gst_pad_unlink(pad, peer);
            gst_object_unref(peer);
        }
        teardownReceiveBin(entry,false);
    }
    if (m_failReceiveRebuilds) {
        finishReceiveRebuild(pad, generation, nullptr, false,
                             QStringLiteral("rebuilds disabled for the test"),
                             level, QString());
        return;
    }
    GstElement *bin = nullptr;
    QString why;
    if (!prepareReceiveChain(m_subscriber.pipeline, entry.streamId,
                             entry.trackKey, entry.kind, &bin, &why,
                             clientName)) {
        if (bin)
            stopDetachedBinAsync(bin);
        finishReceiveRebuild(pad, generation, nullptr, false, why, level,
                             QString());
        return;
    }
    auto *job = new RebuildJob;
    job->engine = this;
    job->pipeline = GST_ELEMENT(gst_object_ref(m_subscriber.pipeline));
    job->bin = GST_ELEMENT(gst_object_ref(bin));
    job->pad = GST_PAD(gst_object_ref(pad));
    job->generation = generation;
    job->level = level;
    job->testMode = testSourceMode();
    job->delayMs = m_rebuildJobDelayMs.load();
    job->sessionLive = m_sessionLive;
    gst_element_call_async(m_subscriber.pipeline, runRebuildJob, job,
                           rebuildJobFree);
}

void SfuMediaEngine::noteReceiveChainRendering(GstPad *pad, quint64 generation)
{
    if (!m_active || generation != m_generation.load())
        return;
    QMutexLocker lock(&m_receiveBinMutex);
    auto it = m_receiveBins.find(pad);
    if (it == m_receiveBins.end() || it->isolateProbe != 0 || it->rebuilding)
        return;
    it->recoveredMs = QDateTime::currentMSecsSinceEpoch();
}

void SfuMediaEngine::finishReceiveRebuild(GstPad *pad, quint64 generation,
                                          GstElement *bin, bool ok,
                                          const QString &why, double level,
                                          const QString &outputName)
{
    const auto discard = [](GstElement *orphan) {
        if (orphan)
            stopDetachedBinAsync(orphan);
    };
    if (!m_active || generation != m_generation.load()) {
        discard(bin);
        return;
    }
    ReceiveBin entry;
    gulong probe = 0;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        auto it = m_receiveBins.find(pad);
        if (it == m_receiveBins.end() || !it->rebuilding) {
            lock.unlock();
            if (bin) {
                if (GstPad *peer = gst_pad_get_peer(pad)) {
                    gst_pad_unlink(pad, peer);
                    gst_object_unref(peer);
                }
            }
            qCInfo(lcSfuMedia) << "a receive chain rebuild finished for a "
                                  "track retired meanwhile; discarded";
            discard(bin);
            return;
        }
        it->rebuilding = false;
        entry = it.value();
        if (ok) {
            it->bin = bin;
            probe = it->isolateProbe;
            it->isolateProbe = 0;
            it->rearmed = false;
            it->rearmOnGiveUp = false;
            it->givenUp = false;
        }
    }
    if (!ok) {
        qCWarning(lcSfuMedia) << "receive chain rebuild" << entry.rebuilds
                              << "for stream=" << entry.streamId
                              << "failed:" << why;
        scheduleReceiveRebuild(pad, generation);
        return;
    }
    const QString volumeName =
        outputVolumeElementName(volumeKeyFor(entry.streamId, entry.trackKey));
    if (level >= 0.0) {
        if (GstElement *volume = gst_bin_get_by_name(
                GST_BIN(bin), volumeName.toUtf8().constData())) {
            g_object_set(volume, "volume", level, nullptr);
            gst_object_unref(volume);
        }
    }
    if (m_rebuiltChainsNeverRenderForTest) {
        if (GstElement *volume = gst_bin_get_by_name(
                GST_BIN(bin), volumeName.toUtf8().constData())) {
            if (GstPad *out = gst_element_get_static_pad(volume, "src")) {
                gst_pad_add_probe(
                    out, GST_PAD_PROBE_TYPE_BUFFER,
                    [](GstPad *, GstPadProbeInfo *, gpointer) {
                        return GST_PAD_PROBE_DROP;
                    },
                    nullptr, nullptr);
                gst_object_unref(out);
            }
            gst_object_unref(volume);
        }
    }
    gst_pad_remove_probe(pad, probe);
    struct Rendering {
        SfuMediaEngine *engine;
        GstPad *pad;
        quint64 generation;
        QString streamId;
    };
    if (GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin))) {
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory && !GST_IS_BIN(element)
                && GST_OBJECT_FLAG_IS_SET(element, GST_ELEMENT_FLAG_SINK)) {
                if (GstPad *in = gst_element_get_static_pad(element, "sink")) {
                    auto *rendering =
                        new Rendering{this, pad, generation, entry.streamId};
                    gst_pad_add_probe(
                        in, GST_PAD_PROBE_TYPE_BUFFER,
                        [](GstPad *, GstPadProbeInfo *, gpointer data) {
                            const auto *r = static_cast<Rendering *>(data);
                            qCInfo(lcSfuMedia)
                                << "call diagnosis: the rebuilt receive chain "
                                   "for stream="
                                << r->streamId << "is RENDERING again";
                            SfuMediaEngine *engine = r->engine;
                            GstPad *key = r->pad;
                            const quint64 generation = r->generation;
                            marshal(engine, [engine, key, generation] {
                                engine->noteReceiveChainRendering(key,
                                                                  generation);
                            });
                            return GST_PAD_PROBE_REMOVE;
                        },
                        rendering,
                        [](gpointer data) {
                            delete static_cast<Rendering *>(data);
                        });
                    gst_object_unref(in);
                }
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
    }
    qCInfo(lcSfuMedia) << "call diagnosis: the receive chain for stream="
                       << entry.streamId << "was REBUILT (attempt"
                       << entry.rebuilds << "output=" << outputName
                       << (entry.rebuildClientName.isEmpty()
                               ? QStringLiteral("")
                               : QStringLiteral("on a new Pulse connection"))
                       << "); waiting for it to render";
    if (entry.rearmed) {
        qCInfo(lcSfuMedia)
            << "call diagnosis: receive chain restored after device returned "
               "stream="
            << entry.streamId << "output=" << outputName;
    }
    if (m_playbackFailureAnnounced) {
        bool anyStillFailed = false;
        {
            QMutexLocker lock(&m_receiveBinMutex);
            for (const ReceiveBin &other : std::as_const(m_receiveBins))
                anyStillFailed = anyStillFailed || other.isolateProbe != 0;
        }
        if (!anyStillFailed) {
            m_playbackFailureAnnounced = false;
            Q_EMIT remotePlaybackFailed(false);
        }
    }
}

int SfuMediaEngine::retireAllReceiveBinsForTest()
{
    QSet<QString> mids;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (const ReceiveBin &entry : std::as_const(m_receiveBins))
            mids.insert(entry.transceiverMid);
    }
    return retireReceiveBins(mids, nullptr, "retired by the test",
                             m_subscriber.webrtc);
}

int SfuMediaEngine::receiveVolumeElementsForTest() const
{
    if (!m_subscriber.pipeline)
        return 0;
    int count = 0;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        auto *element = GST_ELEMENT(g_value_get_object(&item));
        if (g_str_has_prefix(GST_OBJECT_NAME(element), "outvol_"))
            ++count;
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return count;
}

QString SfuMediaEngine::receiveSinkDescription(const QString &pulseClientName,
                                               const OutputSink *speaker)
{
    if (speaker && !speaker->binding.isEmpty())
        return speaker->sink;
    return pulseClientName.isEmpty() ? QStringLiteral("autoaudiosink")
                                     : QStringLiteral("pulsesink name=recvsink");
}

GstElement *SfuMediaEngine::buildReceiveBin(bool video,
                                            const QString &volumeElementName,
                                            bool testSink, QString *error,
                                            const QString &pulseClientName,
                                            const OutputSink *speaker,
                                            bool echoProbe,
                                            const QString &videoCodec)
{
    const QString description = video
        // Bounded but not leaky: a packet dropped here is a gap the
        // depayloader then waits out for a keyframe. Latency is bounded by
        // the appsink's drop=true instead.
        ? QStringLiteral("queue max-size-buffers=0 max-size-bytes=0 "
                         "max-size-time=200000000 "
                         "! %1"
                         "! videoconvert ! video/x-raw,format=RGBA "
                         "! appsink name=vidsink emit-signals=true "
                         "sync=false max-buffers=1 drop=true")
              .arg(receivableVideoCodec(QString()).chain)
        : (testSink
               ? QStringLiteral("queue name=recvqueue max-size-buffers=0 "
                                "max-size-bytes=0 "
                                "max-size-time=200000000 leaky=downstream "
                                "! rtpopusdepay name=recvdepay "
                                "! opusdec name=recvdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! fakesink sync=false")
                     .arg(volumeElementName)
               : QStringLiteral("queue name=recvqueue max-size-buffers=0 "
                                "max-size-bytes=0 "
                                "max-size-time=200000000 leaky=downstream "
                                "! rtpopusdepay name=recvdepay "
                                "! opusdec name=recvdec ! audioconvert "
                                "! audioresample ! volume name=%1 %3! %2")
                     .arg(volumeElementName,
                          receiveSinkDescription(pulseClientName, speaker),
                          echoProbeStage(echoProbe,
                                         calls::noise::webrtcDspAvailable()
                                             && echoProbeElementAvailable())));

    GError *parseError = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &parseError);
    if (parseError) {
        if (error)
            *error = QString::fromUtf8(parseError->message ? parseError->message
                                                           : "?");
        g_error_free(parseError);
        if (bin)
            gst_object_unref(bin);
        return nullptr;
    }
    if (!bin) {
        if (error)
            *error = QStringLiteral("no bin");
        return nullptr;
    }
    const bool chosenOutput =
        speaker && !speaker->binding.isEmpty() && !video && !testSink;
    if (chosenOutput)
        applyBindingTo(bin, "outsink", speaker->binding);
    if (!pulseClientName.isEmpty() && !video && !testSink) {
        GstElement *sink = gst_bin_get_by_name(
            GST_BIN(bin), chosenOutput ? "outsink" : "recvsink");
        if (sink && !g_object_class_find_property(G_OBJECT_GET_CLASS(sink),
                                                  "client-name")) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        if (sink) {
            const QByteArray client = pulseClientName.toUtf8();
            const QByteArray app =
                pulseClientName.section(QStringLiteral(" (reconnected"), 0, 0)
                    .toUtf8();
            GstStructure *props = gst_structure_new(
                "props", "application.name", G_TYPE_STRING, app.constData(),
                nullptr);
            g_object_set(sink, "client-name", client.constData(),
                         "stream-properties", props, nullptr);
            gst_structure_free(props);
            gst_object_unref(sink);
        }
    }
    // The bin settles its own sink's preroll. Otherwise a sink that never
    // gets a buffer (a sender with no key, whose frames the decrypt probe
    // drops) holds the whole subscriber pipeline in PAUSED, and every bin
    // added after it syncs to PAUSED too: one prerolled frame, then silence
    // for everyone until that key arrives.
    //
    // Not `async=false` on the sink instead: this bin going to PLAYING is what
    // gives its sink the jitterbuffer's latency, and without it a syncing
    // sink was measured at 0 ms rather than 161 ms.
    g_object_set(bin, "async-handling", TRUE, nullptr);
    return bin;
}

bool SfuMediaEngine::attachReceiveChain(GstElement *pipeline, GstPad *srcPad,
                                        const QString &streamId,
                                        const QString &trackKey,
                                        const QString &mediaKind,
                                        GstElement **binOut, QString *why,
                                        const QString &pulseClientName,
                                        const QString &videoCodec)
{
    return prepareReceiveChain(pipeline, streamId, trackKey, mediaKind,
                               binOut, why, pulseClientName, videoCodec)
        && startReceiveChain(srcPad, *binOut, why);
}

bool SfuMediaEngine::prepareReceiveChain(GstElement *pipeline,
                                         const QString &streamId,
                                         const QString &trackKey,
                                         const QString &mediaKind,
                                         GstElement **binOut, QString *why,
                                         const QString &pulseClientName,
                                         const QString &videoCodec)
{
    *binOut = nullptr;
    const bool video = mediaKind == QLatin1String("video");
    const QString volumeName =
        outputVolumeElementName(volumeKeyFor(streamId, trackKey));
    QString client = pulseClientName;
    if (client.isEmpty() && !video && !testSourceMode()) {
        QMutexLocker lock(&m_receiveBinMutex);
        client = m_freshPulseClient;
    }
    OutputSink speaker;
    const OutputSink *speakerPtr = nullptr;
    if (!video && !testSourceMode()) {
        speaker = cachedSpeakerSink();
        if (!speaker.binding.isEmpty())
            speakerPtr = &speaker;
    }
    QString buildError;
    GstElement *bin =
        buildReceiveBin(video, volumeName, testSourceMode(), &buildError,
                        client, speakerPtr, m_echoCancel.load(), videoCodec);
    if (!bin) {
        *why = QStringLiteral("could not be BUILT: %1").arg(buildError);
        return false;
    }
    if (!gst_bin_add(GST_BIN(pipeline), bin)) {
        *why = QStringLiteral("could not be ADDED to the subscriber pipeline");
        return false;
    }
    *binOut = bin;
    const auto streamInfo = video ? attachStreamInfo(bin) : StreamInfoPtr();
    if (streamInfo) {
        if (!videoCodec.isEmpty() && videoCodec != QLatin1String("VP8"))
            streamInfo->codec = videoCodec == QLatin1String("H264")
                ? QStringLiteral("H.264") : videoCodec;
        GstPad *rtp = gst_element_get_static_pad(bin, "sink");
        instrumentStreamInfo(rtp, streamInfo, true);
        if (rtp) gst_object_unref(rtp);
        if (GstElement *sink = gst_bin_get_by_name(GST_BIN(bin), "vidsink")) {
            GstPad *decoded = gst_element_get_static_pad(sink, "sink");
            instrumentStreamInfo(decoded, streamInfo, false);
            if (decoded) gst_object_unref(decoded);
            gst_object_unref(sink);
        }
    }
    bool decryptProbeInstalled = false;
    if (GstElement *depay = gst_bin_get_by_name(GST_BIN(bin), "recvdepay")) {
        if (video) {
            for (const char *property : {"request-keyframe", "wait-for-keyframe"}) {
                if (g_object_class_find_property(G_OBJECT_GET_CLASS(depay), property))
                    g_object_set(depay, property, TRUE, nullptr);
            }
            GstPad *rtpIn = gst_element_get_static_pad(depay, "sink");
            installKeyframeRequestGate(rtpIn, streamInfo);
            if (videoCodec == QLatin1String("VP9"))
                splitVp9SpatialLayers(rtpIn);
            if (rtpIn) gst_object_unref(rtpIn);
        }
        if (streamInfo)
            streamInfo->encoder = streamInfoFactory(bin, "videodec");
        if (GstPad *framePad = gst_element_get_static_pad(depay, "src")) {
            installDecryptProbe(framePad, video, streamId, videoCodec, streamInfo);
            decryptProbeInstalled = true;
            gst_object_unref(framePad);
        }
        gst_object_unref(depay);
    }
    if (!decryptProbeInstalled) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: no decrypt probe could be installed for "
               "stream=" << streamId << "kind=" << mediaKind
            << "— its frames will not be decrypted or counted";
    }
    instrumentReceiveBin(bin, trackKey, streamId);
    if (GstElement *appsink = gst_bin_get_by_name(GST_BIN(bin), "vidsink")) {
        auto *ctx = new VideoSinkCtx{
            this, m_receiveNamespace + trackKey,
            m_receiveNamespace + streamId};
        g_signal_connect_data(appsink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(appsink);
    }
    const quint64 generation = m_generation.load();
    marshal(this, [engine = this, streamId, trackKey, generation] {
        engine->applyPendingTrackVolume(streamId, trackKey, generation);
    });
    if (GstElement *volume = gst_bin_get_by_name(
            GST_BIN(bin), volumeName.toUtf8().constData())) {
        g_object_set(volume, "mute", m_outputMuted.load() ? TRUE : FALSE,
                     nullptr);
        gst_object_unref(volume);
    }
    return true;
}

bool SfuMediaEngine::startReceiveChain(GstPad *srcPad, GstElement *bin,
                                       QString *why)
{
    gst_element_sync_state_with_parent(bin);
    GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
    const GstPadLinkReturn linked = gst_pad_link(srcPad, sinkPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    if (linked != GST_PAD_LINK_OK) {
        *why = QStringLiteral("would not LINK to its pad, code=%1")
                   .arg(static_cast<int>(linked));
        return false;
    }
    return true;
}

void SfuMediaEngine::onPadAdded(GstElement *webrtc, void *pad, void *userData)
{
    const lightning::webrtc::CallbackScope scope(webrtc);
    if (!scope.live())
        return;
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    GstPad *srcPad = GST_PAD(pad);
    if (GST_PAD_DIRECTION(srcPad) != GST_PAD_SRC)
        return;
    GstElement *pipeline = GST_ELEMENT(gst_element_get_parent(webrtc));
    if (!pipeline)
        return;

    const QString padTransceiverMid = transceiverMidOf(srcPad);

    QString streamId;
    QString trackMid;
    int sdpSectionsKnown = 0;
    {
        QMutexLocker lock(&engine->m_recvMutex);
        sdpSectionsKnown = engine->m_midForMline.size();
        if (!padTransceiverMid.isEmpty()) {
            for (auto it = engine->m_midForMline.cbegin();
                 it != engine->m_midForMline.cend(); ++it) {
                if (it.value() != padTransceiverMid)
                    continue;
                streamId = engine->m_streamForMline.value(it.key());
                trackMid = engine->m_trackForMline.value(it.key());
                break;
            }
        }
    }
    qCInfo(lcSfuMedia) << "received track attributed=" << !streamId.isEmpty()
                       << "trackKey=" << trackMid
                       << "sdpSections=" << sdpSectionsKnown
                       << "midMatched=" << !trackMid.isEmpty();
    if (trackMid.isEmpty())
        trackMid = padTransceiverMid;
    if (streamId.isEmpty()) {
        streamId = trackMid.isEmpty()
            ? QStringLiteral("unattributed")
            : QStringLiteral("mid:%1").arg(trackMid);
    }

    GstCaps *caps = gst_pad_get_current_caps(srcPad);
    if (!caps)
        caps = gst_pad_query_caps(srcPad, nullptr);
    QString mediaKind;
    QString videoCodec;
    if (caps) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *media =
            structure ? gst_structure_get_string(structure, "media") : nullptr;
        mediaKind = QString::fromUtf8(media ? media : "");
        // Only a codec every structure agrees on; anything else is VP8.
        for (guint i = 0; i < gst_caps_get_size(caps); ++i) {
            const gchar *encoding = gst_structure_get_string(
                gst_caps_get_structure(caps, i), "encoding-name");
            const QString name = QString::fromLatin1(encoding ? encoding : "")
                                     .toUpper();
            if (i > 0 && name != videoCodec)
                videoCodec.clear();
            else
                videoCodec = name;
            if (videoCodec.isEmpty())
                break;
        }
        gst_caps_unref(caps);
    }
    if (mediaKind.isEmpty()) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: an incoming track's caps named no media type;"
            << "assuming audio for stream=" << streamId << "mid=" << trackMid;
        mediaKind = QStringLiteral("audio");
    }

    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    GstElement *bin = nullptr;
    QString why;
    const bool attached = engine->attachReceiveChain(
        pipeline, srcPad, streamId, trackMid, mediaKind, &bin, &why, QString(),
        videoCodec);
    gst_object_unref(pipeline);
    if (!attached) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: the receive bin for stream=" << streamId
            << "kind=" << mediaKind << "failed:" << why
            << "— this participant will never be heard or seen";
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
        });
        return;
    }
    {
        QMutexLocker lock(&engine->m_receiveBinMutex);
        engine->m_receiveBins.insert(
            static_cast<GstPad *>(pad),
            ReceiveBin{bin, streamId, mediaKind, padTransceiverMid, trackMid});
    }
    if (!padTransceiverMid.isEmpty()) {
        engine->retireReceiveBins({padTransceiverMid},
                                  static_cast<GstPad *>(pad),
                                  "its transceiver has a newer stream", webrtc);
    }
    qCInfo(lcSfuMedia)
        << "call diagnosis: a receive chain is RUNNING for stream=" << streamId
        << "mid=" << trackMid << "kind=" << mediaKind;
    marshal(engine, [engine, generation, mediaKind, streamId, trackMid] {
        if (engine->m_generation.load() != generation)
            return;
        if (mediaKind == QLatin1String("audio"))
            engine->engageEchoCancellation();
        Q_EMIT engine->remoteTrackAdded(streamId, trackMid, mediaKind);
    });
}

namespace {
struct MicSwapBlock {
    SfuMediaEngine *engine = nullptr;
    quint64 serial = 0;
    std::atomic<bool> reported{false};
};

GstPadProbeReturn onMicSwapReady(GstPad *, GstPadProbeInfo *, gpointer data)
{
    auto *block = static_cast<MicSwapBlock *>(data);
    if (!block->reported.exchange(true)) {
        SfuMediaEngine *engine = block->engine;
        const quint64 serial = block->serial;
        marshal(engine,
                [engine, serial] { engine->completeMicrophoneSwap(serial); });
    }
    return GST_PAD_PROBE_OK;
}

QList<GstElement *> captureFrontOf(GstElement *bin, GstPad *pad)
{
    QList<GstElement *> chain;
    GstPad *peer = gst_pad_get_peer(pad);
    while (peer) {
        GstElement *element = gst_pad_get_parent_element(peer);
        gst_object_unref(peer);
        peer = nullptr;
        if (!element)
            break;
        GstObject *parent = gst_object_get_parent(GST_OBJECT(element));
        const bool child = parent == GST_OBJECT(bin);
        if (parent)
            gst_object_unref(parent);
        if (!child || chain.contains(element) || chain.size() >= 16) {
            gst_object_unref(element);
            break;
        }
        chain.append(element);
        GstIterator *it = gst_element_iterate_sink_pads(element);
        GValue item = G_VALUE_INIT;
        if (gst_iterator_next(it, &item) == GST_ITERATOR_OK)
            peer = gst_pad_get_peer(GST_PAD(g_value_get_object(&item)));
        if (G_IS_VALUE(&item))
            g_value_unset(&item);
        gst_iterator_free(it);
    }
    return chain;
}

struct FrontRetire {
    GstElement *bin = nullptr;
    QList<GstElement *> elements;
};

void retireFront(GstElement *, gpointer data)
{
    auto *job = static_cast<FrontRetire *>(data);
    GST_STATE_LOCK(job->bin);
    for (auto it = job->elements.crbegin(); it != job->elements.crend(); ++it) {
        GstElement *element = *it;
        GstObject *parent = gst_object_get_parent(GST_OBJECT(element));
        if (parent == GST_OBJECT(job->bin))
            gst_bin_remove(GST_BIN(job->bin), element);
        if (parent)
            gst_object_unref(parent);
    }
    GST_STATE_UNLOCK(job->bin);
    for (GstElement *element : std::as_const(job->elements))
        gst_element_set_state(element, GST_STATE_NULL);
}

void frontRetireFree(gpointer data)
{
    auto *job = static_cast<FrontRetire *>(data);
    for (GstElement *element : std::as_const(job->elements))
        gst_object_unref(element);
    if (job->bin)
        gst_object_unref(job->bin);
    delete job;
}

GstPadProbeReturn dropEverything(GstPad *, GstPadProbeInfo *, gpointer)
{
    return GST_PAD_PROBE_DROP;
}
}

void SfuMediaEngine::refreshMicrophoneCapture()
{
    if (!m_active || m_micCid.isEmpty() || !m_publishedBins.contains(m_micCid))
        return;
    const quint64 seq = ++m_micResolveSeq;
    const int channels = m_testDeviceChannels;
    if (m_testSources) {
        MicrophoneCapture capture = testMicrophoneCapture();
        if (channels > 0)
            capture.channels = channels;
        swapMicrophoneFront(capture);
        return;
    }
    const DeviceChoice choice = microphoneChoice();
    const quint64 generation = m_generation.load();
    const QString cid = m_micCid;
    QThreadPool::globalInstance()->start(
        [this, choice, seq, generation, cid, channels] {
            MicrophoneCapture capture = resolveMicrophoneCapture(choice);
            if (channels > 0)
                capture.channels = channels;
            marshal(this, [this, capture, seq, generation, cid] {
                if (seq != m_micResolveSeq || generation != m_generation.load()
                    || cid != m_micCid)
                    return;
                swapMicrophoneFront(capture);
            });
        });
}

void SfuMediaEngine::swapMicrophoneFront(const MicrophoneCapture &capture)
{
    GstElement *bin =
        m_micCid.isEmpty() ? nullptr : m_publishedBins.value(m_micCid);
    if (!m_active || !bin)
        return;
    if (microphoneStarting()) {
        const qint64 waited = monotonicMs() - m_micStartDispatchedMs;
        if (waited < m_micStartAbandonMs) {
            m_micSwapDeferred = capture;
            qCInfo(lcSfuMedia) << "microphone switch waits for the "
                                  "microphone's start";
            const QString cid = m_micCid;
            const quint64 generation = m_generation.load();
            const quint64 seq = m_micResolveSeq;
            QTimer::singleShot(int(m_micStartAbandonMs - waited) + 1, this,
                               [this, cid, generation, seq] {
                if (!m_active || generation != m_generation.load()
                    || cid != m_micCid || seq != m_micResolveSeq
                    || !m_micSwapDeferred || !microphoneStarting())
                    return;
                const MicrophoneCapture deferred = *m_micSwapDeferred;
                swapMicrophoneFront(deferred);
            });
            return;
        }
        qCWarning(lcSfuMedia)
            << "the microphone's start has not finished after" << waited
            << "ms; the chosen device replaces its capture now";
        m_micSwapDeferred.reset();
        m_micSwapReplacesStart = true;
    }
    if (m_micSwap.front)
        abandonMicrophoneSwap("a newer pick replaces it");
    GstElement *valve = gst_bin_get_by_name(GST_BIN(bin), "micvalve");
    GstPad *valveIn =
        valve ? gst_element_get_static_pad(valve, "sink") : nullptr;
    if (valve)
        gst_object_unref(valve);
    if (!valveIn)
        return;
    GstCaps *carried = gst_pad_get_current_caps(valveIn);
    gst_object_unref(valveIn);

    const QString description = microphoneFrontDescription(capture)
        + QStringLiteral(" ! capsfilter name=micfrontcaps");
    GError *error = nullptr;
    GstElement *front = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error || !front) {
        qCWarning(lcSfuMedia)
            << "microphone switch: the new capture did not parse code="
            << (error ? error->code : -1) << "description=" << description
            << "- keeping the current microphone";
        if (error)
            g_error_free(error);
        if (front)
            gst_object_unref(gst_object_ref_sink(front));
        if (carried)
            gst_caps_unref(carried);
        return;
    }
    front = GST_ELEMENT(gst_object_ref_sink(front));
    const quint64 serial = ++m_micSwapSerial;
    const QByteArray name = QByteArray("micfront") + QByteArray::number(serial);
    gst_element_set_name(front, name.constData());
    const bool pinned = carried != nullptr;
    if (carried) {
        if (GstElement *pin =
                gst_bin_get_by_name(GST_BIN(front), "micfrontcaps")) {
            g_object_set(pin, "caps", carried, nullptr);
            gst_object_unref(pin);
        }
        gst_caps_unref(carried);
    }
    applyBindingTo(front, "micsrc", capture.binding);
    instrumentMicrophoneSource(front, m_micCaptured);
    GstPad *out = gst_element_get_static_pad(front, "src");
    if (!out) {
        qCWarning(lcSfuMedia) << "microphone switch: the new capture has no "
                                 "output; keeping the current microphone";
        gst_object_unref(front);
        return;
    }
    auto *block = new MicSwapBlock;
    block->engine = this;
    block->serial = serial;
    const gulong probe = gst_pad_add_probe(
        out,
        GstPadProbeType(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER
                        | GST_PAD_PROBE_TYPE_BUFFER_LIST),
        onMicSwapReady, block,
        [](gpointer data) { delete static_cast<MicSwapBlock *>(data); });
    gst_object_unref(out);
    if (!gst_bin_add(GST_BIN(bin), front)) {
        qCWarning(lcSfuMedia) << "microphone switch: the new capture could "
                                 "not be added; keeping the current one";
        gst_object_unref(front);
        return;
    }
    m_micSwap.bin = GST_ELEMENT(gst_object_ref(bin));
    m_micSwap.front = front;
    m_micSwap.name = QString::fromLatin1(name);
    m_micSwap.blockProbe = probe;
    m_micSwap.serial = serial;
    m_micSwap.currentFailed = false;
    qCInfo(lcSfuMedia) << "microphone switch: starting the new capture beside "
                          "the live one; chosen="
                       << !capture.binding.isEmpty()
                       << "device-channels=" << capture.channels
                       << "caps-pinned=" << pinned;
    gst_element_call_async(
        front,
        [](GstElement *element, gpointer) {
            syncStateIfStillParented(element);
        },
        nullptr, nullptr);
    QTimer::singleShot(m_micSwapTimeoutMs, this, [this, serial] {
        if (m_micSwap.front && m_micSwap.serial == serial)
            abandonMicrophoneSwap("the chosen microphone delivered nothing");
    });
}

void SfuMediaEngine::completeMicrophoneSwap(quint64 serial)
{
    if (!m_micSwap.front || m_micSwap.serial != serial)
        return;
    GstElement *bin = m_micSwap.bin;
    if (!m_active || m_micCid.isEmpty()
        || m_publishedBins.value(m_micCid) != bin) {
        forgetMicrophoneSwap();
        return;
    }
    GstElement *front = m_micSwap.front;
    GstElement *valve = gst_bin_get_by_name(GST_BIN(bin), "micvalve");
    GstPad *valveIn =
        valve ? gst_element_get_static_pad(valve, "sink") : nullptr;
    if (valve)
        gst_object_unref(valve);
    GstPad *next = gst_element_get_static_pad(front, "src");
    if (!valveIn || !next) {
        if (valveIn)
            gst_object_unref(valveIn);
        if (next)
            gst_object_unref(next);
        abandonMicrophoneSwap("the send chain has no valve to link to");
        return;
    }
    const QList<GstElement *> old = captureFrontOf(bin, valveIn);
    GstPad *oldOut = gst_pad_get_peer(valveIn);
    gulong dropProbe = 0;
    if (oldOut) {
        dropProbe = gst_pad_add_probe(oldOut, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
                                      dropEverything, nullptr, nullptr);
        gst_pad_unlink(oldOut, valveIn);
    }
    const GstPadLinkReturn linked = gst_pad_link(next, valveIn);
    if (linked != GST_PAD_LINK_OK) {
        if (oldOut) {
            gst_pad_link(oldOut, valveIn);
            gst_pad_remove_probe(oldOut, dropProbe);
            gst_object_unref(oldOut);
        }
        gst_object_unref(valveIn);
        gst_object_unref(next);
        for (GstElement *element : old)
            gst_object_unref(element);
        qCWarning(lcSfuMedia) << "microphone switch: link code=" << linked;
        abandonMicrophoneSwap("the new capture would not link");
        return;
    }
    gst_pad_remove_probe(next, m_micSwap.blockProbe);
    gst_object_unref(next);
    gst_object_unref(valveIn);
    if (oldOut)
        gst_object_unref(oldOut);
    if (!old.isEmpty()) {
        auto *job = new FrontRetire;
        job->bin = GST_ELEMENT(gst_object_ref(bin));
        job->elements = old;
        gst_element_call_async(bin, retireFront, job, frontRetireFree);
    }
    if (GstElement *source = gst_bin_get_by_name(GST_BIN(front), "micsrc")) {
        if (GstPad *pad = gst_element_get_static_pad(source, "src")) {
            lightning::calls::watchReplacementCapture(pad, m_micClockHold);
            gst_object_unref(pad);
        }
        gst_object_unref(source);
    }
    const bool wasFailed = m_micFailureReported;
    m_micRestarts = 0;
    m_micFailureReported = false;
    m_micCapturedAtRestart = m_micCaptured ? m_micCaptured->load() : 0;
    m_micLiveFront = m_micSwap.name;
    gst_object_unref(m_micSwap.front);
    gst_object_unref(m_micSwap.bin);
    m_micSwap = PendingMicSwap{};
    ++m_micSwaps;
    resetMicLevelState();
    if (m_micSwapReplacesStart) {
        m_micSwapReplacesStart = false;
        if (m_micStartPending && m_micStartPending->load()) {
            m_micStartTakenOver = true;
            runMicrophoneStartDeferrals(m_micCid, m_generation.load());
        }
    }
    qCInfo(lcSfuMedia) << "microphone switched mid-call: the new capture is "
                          "live; track, encoder and encryption unchanged "
                          "(outgoing elements:"
                       << old.size() << ")";
    if (wasFailed)
        Q_EMIT microphoneRecovered(m_micCid);
}

void SfuMediaEngine::abandonMicrophoneSwap(const char *why)
{
    if (!m_micSwap.front)
        return;
    qCWarning(lcSfuMedia) << "microphone switch abandoned:" << why
                          << "- the call keeps the microphone it has";
    GstElement *front = m_micSwap.front;
    GstElement *bin = m_micSwap.bin;
    const bool currentFailed = m_micSwap.currentFailed;
    m_micSwap = PendingMicSwap{};
    m_micSwapReplacesStart = false;
    ++m_micSwapsAbandoned;
    stopDetachedBinAsync(front);
    gst_object_unref(front);
    if (bin)
        gst_object_unref(bin);
    if (currentFailed)
        handleCaptureError(m_micCid, m_micLiveFront);
}

void SfuMediaEngine::forgetMicrophoneSwap()
{
    if (m_micSwap.front)
        gst_object_unref(m_micSwap.front);
    if (m_micSwap.bin)
        gst_object_unref(m_micSwap.bin);
    m_micSwap = PendingMicSwap{};
}

namespace {
GstElement *buildReplacementOutput(const QByteArray &description,
                                   const lightning::calls::DeviceBinding &binding,
                                   const QByteArray &clientName)
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
    g_object_set(next, "async-handling", TRUE, nullptr);
    applyBindingTo(next, "outsink", binding);
    if (!clientName.isEmpty()) {
        const QByteArray app = QString::fromUtf8(clientName)
                                   .section(QStringLiteral(" (reconnected"), 0, 0)
                                   .toUtf8();
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(next));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(element),
                                             "client-name")) {
                GstStructure *props = gst_structure_new(
                    "props", "application.name", G_TYPE_STRING,
                    app.constData(), nullptr);
                g_object_set(element, "client-name", clientName.constData(),
                             "stream-properties", props, nullptr);
                gst_structure_free(props);
            }
            g_value_reset(&item);
        }
        if (G_IS_VALUE(&item))
            g_value_unset(&item);
        gst_iterator_free(it);
    }
    return next;
}
}

void SfuMediaEngine::swapReceiveSinks()
{
    if (!m_active || !m_subscriber.pipeline)
        return;
    QByteArray description;
    lightning::calls::DeviceBinding binding;
    QByteArray client;
    if (testSourceMode()) {
        if (m_testSpeakerSink.isEmpty())
            return;
        description =
            (m_testSpeakerSink + QStringLiteral(" name=outsink")).toUtf8();
    } else {
        QString fresh;
        {
            QMutexLocker lock(&m_receiveBinMutex);
            fresh = m_freshPulseClient;
        }
        const OutputSink speaker = cachedSpeakerSink();
        const bool chosen = !speaker.binding.isEmpty();
        description =
            receiveSinkDescription(fresh, chosen ? &speaker : nullptr).toUtf8();
        if (chosen)
            binding = speaker.binding;
        client = fresh.toUtf8();
    }
    QList<QPair<GstElement *, GstPad *>> pads;
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (const ReceiveBin &entry : std::as_const(m_receiveBins)) {
            if (!entry.bin || entry.kind == QLatin1String("video")
                || entry.rebuilding || entry.isolateProbe != 0)
                continue;
            const QByteArray volumeName =
                outputVolumeElementName(
                    volumeKeyFor(entry.streamId, entry.trackKey))
                    .toUtf8();
            if (GstElement *volume = gst_bin_get_by_name(
                    GST_BIN(entry.bin), volumeName.constData())) {
                if (GstPad *pad = gst_element_get_static_pad(volume, "src"))
                    pads.append({GST_ELEMENT(gst_object_ref(entry.bin)), pad});
                gst_object_unref(volume);
            }
        }
    }
    for (const auto &[bin, pad] : std::as_const(pads)) {
        lightning::calls::sinkswap::Request request;
        request.build = [description, binding, client] {
            return buildReplacementOutput(description, binding, client);
        };
        request.swaps = m_speakerSwaps;
        request.live = m_sessionLive;
        request.warn = [](const char *why) {
            qCWarning(lcSfuMedia) << "speaker switch:" << why;
        };
        lightning::calls::sinkswap::request(bin, pad, std::move(request));
        gst_object_unref(pad);
        gst_object_unref(bin);
    }
    qCInfo(lcSfuMedia) << "speaker switched mid-call: moving" << pads.size()
                       << "received track(s) to the"
                       << (binding.isEmpty() ? "default" : "chosen")
                       << "output";
}

QVariantList SfuMediaEngine::streamInfoStats()
{
    QVariantList rows;
    const qint64 now = monotonicMs();
    auto append = [&](GstElement *bin, const QString &stream, const QString &track) {
        if (!bin) return;
        auto *held = static_cast<StreamInfoPtr *>(
            g_object_get_data(G_OBJECT(bin), streamInfoKey));
        if (!held) return;
        QVariantMap row = (*held)->sample(now);
        const quint32 ssrc = (*held)->ssrc.load();
        if (const auto rtp = m_streamInfoRtp.constFind(ssrc);
            ssrc && rtp != m_streamInfoRtp.constEnd()) {
            row.insert(QStringLiteral("lost"), rtp->lost);
            row.insert(QStringLiteral("nack"), rtp->nack);
            row.insert(QStringLiteral("pli"), rtp->pli + rtp->fir);
            row.insert(QStringLiteral("jitterMs"), rtp->jitter * 1000.0);
            if (rtp->dir == QLatin1String("inbound"))
                row.insert(QStringLiteral("packets"), rtp->packets);
            else
                row.insert(QStringLiteral("packetsSent"), rtp->packets);
        }
        if (const auto remote = m_streamInfoRtp.constFind((quint64(1) << 32) | ssrc);
            ssrc && remote != m_streamInfoRtp.constEnd())
            row.insert(QStringLiteral("lost"), remote->lost);
        row.insert(QStringLiteral("streamId"), stream);
        row.insert(QStringLiteral("trackKey"), track);
        row.insert(QStringLiteral("encrypted"), m_encryptionRequired.load());
        rows.append(row);
    };
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend(); ++it)
        append(it.value(), QString(), it.key());
    QMutexLocker lock(&m_receiveBinMutex);
    for (const auto &receive : m_receiveBins)
        append(receive.bin, m_receiveNamespace + receive.streamId,
               m_receiveNamespace + receive.trackKey);
    return rows;
}

void SfuMediaEngine::resetStreamInfoBaselines()
{
    auto reset = [](GstElement *bin) {
        if (!bin) return;
        auto *held = static_cast<StreamInfoPtr *>(
            g_object_get_data(G_OBJECT(bin), streamInfoKey));
        if (held) (*held)->lastMs = -1;
    };
    for (auto *bin : m_publishedBins) reset(bin);
    QMutexLocker lock(&m_receiveBinMutex);
    for (const auto &receive : m_receiveBins) reset(receive.bin);
}
