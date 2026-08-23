#include "calls/SfuMediaEngine.h"

#include "calls/CallFrameCryptor.h"
#include "calls/SfuVideoRouter.h"

#include <mutex>

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUrl>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

namespace {
Q_LOGGING_CATEGORY(lcSfuMedia, "lightning.calls.sfu")

// Same alive-registry discipline as the 1:1 engine: GStreamer calls back on
// its own threads and a marshalled lambda must never run against a destroyed
// engine. Registration is by pointer, and the queued invocation is tied to
// the engine as receiver, so anything already in flight dies with the QObject.
QMutex g_aliveMutex;
QSet<SfuMediaEngine *> g_aliveEngines;

template <typename Fn>
void marshal(SfuMediaEngine *engine, Fn &&fn)
{
    QMutexLocker lock(&g_aliveMutex);
    if (!g_aliveEngines.contains(engine))
        return;
    QMetaObject::invokeMethod(engine, std::forward<Fn>(fn),
                              Qt::QueuedConnection);
}

/// Context for a promise callback: pins the webrtcbin until the promise
/// settles and remembers which peer connection it belongs to.
struct PromiseCtx {
    SfuMediaEngine *engine = nullptr;
    GstElement *webrtc = nullptr; // owns one ref
    bool publisher = false;
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

/// LiveKit trickles candidates as the JSON form a browser's RTCIceCandidate
/// serialises to. Building it with QJsonDocument rather than string
/// concatenation means a candidate line can never inject JSON.
QString candidateInitJson(const QString &candidate, int mlineIndex)
{
    QJsonObject object;
    object.insert(QStringLiteral("candidate"), candidate);
    object.insert(QStringLiteral("sdpMLineIndex"), mlineIndex);
    // webrtcbin gives us the m-line index; LiveKit accepts that alone, and
    // an invented sdpMid would be worse than an absent one.
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

/// Parse LiveKit's candidate JSON back into what webrtcbin wants.
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
} // namespace

namespace {
/// What a pad probe needs to know. Owned by the probe, freed when the probe
/// is removed.
struct CryptoProbeCtx {
    SfuMediaEngine *engine = nullptr;
    /// Shared, not raw: a receive cryptor belongs to a sender who can leave
    /// while a frame of theirs is still in the probe.
    std::shared_ptr<CallFrameCryptor> cryptor;
    /// Direction: true encrypts (send side), false decrypts (receive side).
    bool encrypting = false;
    /// Video uses VP8's cleartext-header rule; audio uses Opus's.
    bool video = false;
    /// Read on the streaming thread; see SfuMediaEngine's atomics.
    const std::atomic<bool> *required = nullptr;
    const std::atomic<bool> *keyReady = nullptr;
    /// This track's IV stream id, unique among encrypting tracks. Send side
    /// only; a receiver reads the IV out of the frame.
    quint32 ivStream = 0;
};

/// Which sender an appsink belongs to. One per received video track.
struct VideoSinkCtx {
    SfuMediaEngine *engine = nullptr;
    QString streamId;
};

void videoSinkCtxFree(void *data, GClosure *)
{
    delete static_cast<VideoSinkCtx *>(data);
}

/// One decoded RGBA frame, from a GStreamer streaming thread to the router.
///
/// The frame is COPIED. A QVideoFrame cannot borrow a GstBuffer's memory:
/// the buffer is unreffed when this returns, while the frame lives until the
/// GUI thread has rendered it. Copying is also why `watching()` is checked
/// first — an unwatched participant costs a hash lookup instead of a
/// full-frame memcpy at frame rate.
GstFlowReturn onVideoSample(GstElement *sink, void *userData)
{
    auto *ctx = static_cast<VideoSinkCtx *>(userData);
    if (!ctx || !ctx->engine)
        return GST_FLOW_OK;

    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample)
        return GST_FLOW_OK;

    // Ask before copying: an unwatched participant costs a hash lookup
    // instead of a full-frame memcpy at frame rate. SfuVideoRouter takes its
    // own mutex, so this is safe to call from here.
    bool wanted = false;
    if (SfuVideoRouter *router = ctx->engine->videoRouter())
        wanted = router->watching(ctx->streamId);
    if (!wanted) {
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
    // A frame with no geometry is not a frame. Bounded because these numbers
    // come from a remote sender and are about to size an allocation.
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
        // Row by row: GStreamer and Qt need not agree on stride, and copying
        // the whole block when they disagree shears the image.
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

    // Delivered on the GUI thread: a QVideoSink belongs to its own thread.
    SfuMediaEngine *engine = ctx->engine;
    const QString streamId = ctx->streamId;
    marshal(engine, [engine, streamId, frame] {
        if (SfuVideoRouter *router = engine->videoRouter())
            router->deliverFrame(streamId, frame);
    });
    return GST_FLOW_OK;
}

void cryptoProbeCtxFree(void *data)
{
    delete static_cast<CryptoProbeCtx *>(data);
}

/// Encrypt or decrypt one ENCODED FRAME in the dataflow.
///
/// Placed between the encoder and the RTP payloader (send) or after the
/// depayloader (receive), so the buffer here is one complete encoded frame —
/// which is the unit LiveKit and Element Call encrypt. Doing this on RTP
/// packets instead would be a different scheme that interoperates with
/// nobody, and one frame can span many packets.
GstPadProbeReturn cryptoProbe(GstPad *pad, GstPadProbeInfo *info,
                              void *userData)
{
    Q_UNUSED(pad);
    auto *ctx = static_cast<CryptoProbeCtx *>(userData);
    auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !ctx || !ctx->cryptor)
        return GST_PAD_PROBE_OK;

    const bool haveKey = ctx->keyReady && ctx->keyReady->load();
    if (!haveKey) {
        // No key yet. In an encrypted room that means DROP: sending in the
        // clear would silently un-encrypt a call the user was told is
        // end-to-end encrypted, and rendering an undecryptable frame is
        // worse than dropping it.
        const bool required = ctx->required && ctx->required->load();
        return required ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
    }

    // VP8 keyframes leave 10 header bytes in the clear and delta frames 3
    // (so an SFU can still route and detect keyframes); Opus leaves the TOC
    // byte. The keyframe answer comes from GStreamer's own flag rather than
    // from parsing the bitstream — the encoder already told us.
    CallFrameCryptor::FrameKind kind = CallFrameCryptor::FrameKind::Audio;
    if (ctx->video) {
        kind = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)
            ? CallFrameCryptor::FrameKind::VideoDelta
            : CallFrameCryptor::FrameKind::VideoKey;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;
    const QByteArray input(reinterpret_cast<const char *>(map.data),
                           static_cast<int>(map.size));
    gst_buffer_unmap(buffer, &map);

    QByteArray output;
    if (ctx->encrypting) {
        // ssrc/timestamp feed the IV. The PTS is the frame's own timestamp,
        // and the per-SSRC counter inside the cryptor is what guarantees a
        // unique IV even when two frames share one — AES-GCM IV reuse is a
        // total break, so this must never be derived from content alone.
        const quint32 timestamp =
            GST_BUFFER_PTS_IS_VALID(buffer)
                ? static_cast<quint32>(GST_BUFFER_PTS(buffer) / 1000)
                : 0;
        output = ctx->cryptor->encryptFrame(input, kind, ctx->ivStream,
                                            timestamp);
    } else {
        output = ctx->cryptor->decryptFrame(input, kind);
    }

    if (output.isEmpty()) {
        // Encryption refused (no key), or decryption failed its
        // authentication tag. Either way the frame does not go on: there is
        // no cleartext fallback in this path by design.
        return GST_PAD_PROBE_DROP;
    }

    // The payload changed size, so this is a NEW buffer carrying the
    // original's timing and flags rather than an in-place edit.
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
    gst_buffer_copy_into(replacement, buffer,
                         static_cast<GstBufferCopyFlags>(
                             GST_BUFFER_COPY_TIMESTAMPS
                             | GST_BUFFER_COPY_FLAGS),
                         0, static_cast<gsize>(-1));

    gst_buffer_unref(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = replacement;
    return GST_PAD_PROBE_OK;
}
} // namespace

bool SfuMediaEngine::runtimeAvailable(QString *whyNot)
{
    static std::once_flag once;
    static bool initOk = false;
    std::call_once(once, [] {
        GError *error = nullptr;
        initOk = gst_init_check(nullptr, nullptr, &error) == TRUE;
        if (error)
            g_error_free(error);
    });
    if (!initOk) {
        if (whyNot)
            *whyNot = QStringLiteral("gstreamer_init_failed");
        return false;
    }
    // Everything the SFU pipelines need. Video and screen capture are
    // included because an SFU call that cannot carry them is not what this
    // engine claims to be — a partial probe would let it register and then
    // fail at the moment the user turns a camera on.
    static const char *const kRequired[] = {
        "webrtcbin",     "nicesrc",       "nicesink",     "dtlssrtpenc",
        "dtlssrtpdec",   "opusenc",       "opusdec",      "rtpopuspay",
        "rtpopusdepay",  "audioconvert",  "audioresample", "audiotestsrc",
        "fakesink",      "autoaudiosrc",  "autoaudiosink", "queue",
        "valve",         "volume",        "capsfilter",
        // Video publish/receive.
        "vp8enc",        "vp8dec",        "rtpvp8pay",    "rtpvp8depay",
        "videoconvert",  "videoscale",    "videotestsrc", "videorate",
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
    QMutexLocker lock(&g_aliveMutex);
    g_aliveEngines.insert(this);
}

SfuMediaEngine::~SfuMediaEngine()
{
    {
        // Unregister FIRST: from here no new marshalled lambda targets us,
        // and anything already queued dies with the QObject.
        QMutexLocker lock(&g_aliveMutex);
        g_aliveEngines.remove(this);
    }
    stop();
}

void SfuMediaEngine::start()
{
    stop();
    // Bump BEFORE anything can call back, so a callback still in flight from
    // the previous session is already stale by the time it lands.
    m_generation.fetch_add(1);
    m_active = true;
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    Q_EMIT connectionStateChanged(QStringLiteral("connecting"));
}

void SfuMediaEngine::stop()
{
    if (!m_active && !m_publisher.pipeline && !m_subscriber.pipeline)
        return;
    m_generation.fetch_add(1);
    m_active = false;
    m_publishedBins.clear();
    destroyPeer(m_publisher);
    destroyPeer(m_subscriber);
    // Engine state is per session; the user's intent lives in the
    // controller and is re-applied on the next start.
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    // Media keys must not outlive the call that used them.
    clearKeys();
    Q_EMIT connectionStateChanged(QStringLiteral("closed"));
}

void SfuMediaEngine::destroyPeer(Peer &peer)
{
    if (peer.webrtc) {
        g_signal_handlers_disconnect_by_data(peer.webrtc, this);
        gst_object_unref(peer.webrtc);
    }
    if (peer.pipeline) {
        GstBus *bus = gst_element_get_bus(peer.pipeline);
        if (bus) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }
        gst_element_set_state(peer.pipeline, GST_STATE_NULL);
        gst_object_unref(peer.pipeline);
    }
    peer = Peer();
}

bool SfuMediaEngine::ensurePeer(Target target)
{
    Peer &peer = peerFor(target);
    if (peer.webrtc)
        return true;
    if (!m_active)
        return false;

    // bundle-policy=max-bundle is what LiveKit negotiates; anything else
    // produces an SDP the SFU will not accept.
    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *webrtc = gst_element_factory_make("webrtcbin", "wb");
    if (!pipeline || !webrtc) {
        if (pipeline)
            gst_object_unref(pipeline);
        if (webrtc)
            gst_object_unref(webrtc);
        Q_EMIT failed(QStringLiteral("pipeline_failed"));
        return false;
    }
    g_object_set(webrtc, "bundle-policy", 3 /* max-bundle */, "latency", 100,
                 nullptr);
    if (!gst_bin_add(GST_BIN(pipeline), webrtc)) {
        // gst_bin_add sinks-and-drops on failure: webrtc may be FINALIZED.
        gst_object_unref(pipeline);
        Q_EMIT failed(QStringLiteral("pipeline_failed"));
        return false;
    }
    // Borrowed for the session; the pipeline owns it.
    peer.pipeline = pipeline;
    peer.webrtc = GST_ELEMENT(gst_object_ref(webrtc));

    if (target == Target::Publisher) {
        // Only the publisher offers. The subscriber's negotiation is driven
        // entirely by the server's offers, and answering our own
        // negotiation-needed there would produce a competing offer.
        g_signal_connect(webrtc, "on-negotiation-needed",
                         G_CALLBACK(onNegotiationNeeded), this);
    }
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate),
                     this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(onPadAdded), this);

    applyIceTo(peer);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return true;
}

void SfuMediaEngine::applyIceTo(Peer &peer)
{
    if (!peer.webrtc || m_iceUris.isEmpty())
        return;
    for (const QString &uri : m_iceUris) {
        if (uri.startsWith(QLatin1String("stun:"))) {
            g_object_set(peer.webrtc, "stun-server", uri.toUtf8().constData(),
                         nullptr);
            continue;
        }
        if (!uri.startsWith(QLatin1String("turn:"))
            && !uri.startsWith(QLatin1String("turns:")))
            continue;
        // Credentials are percent-encoded into the URI form webrtcbin wants.
        // Never logged: these are live relay credentials.
        const QString scheme =
            uri.startsWith(QLatin1String("turns:")) ? QStringLiteral("turns")
                                                    : QStringLiteral("turn");
        const QString host = uri.section(QLatin1Char(':'), 1);
        const QString full =
            QStringLiteral("%1://%2:%3@%4")
                .arg(scheme,
                     QString::fromUtf8(QUrl::toPercentEncoding(m_iceUsername)),
                     QString::fromUtf8(QUrl::toPercentEncoding(m_icePassword)),
                     host);
        gboolean added = FALSE;
        g_signal_emit_by_name(peer.webrtc, "add-turn-server",
                              full.toUtf8().constData(), &added);
    }
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
            // Sanity filter, same as the 1:1 engine: these strings are
            // assembled into a credential-bearing URI.
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

void SfuMediaEngine::publishAudio(const QString &cid)
{
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    const QString source = m_testSources
        ? QStringLiteral(
              "audiotestsrc is-live=true wave=sine freq=440 volume=0.05")
        : QStringLiteral("autoaudiosrc");
    // The valve is the real mute (drop=true stops buffers before the
    // encoder, so no RTP is produced at all). Opus 111 is the ecosystem
    // convention and what LiveKit expects for audio.
    const QString description =
        QStringLiteral("%1 ! queue ! audioconvert ! audioresample "
                       "! valve name=micvalve drop=%2 "
                       "! opusenc name=audioenc "
                       "! rtpopuspay pt=111 "
                       // capsfilter, NOT a bare caps string. gst_parse only
                       // accepts caps as a filter BETWEEN two elements; as
                       // the last item in a bin description the parser reads
                       // it as an element name and fails with
                       // `no element "application"`. That is exactly what
                       // happened here — every SFU audio and video publish
                       // failed on every machine, so the MatrixRTC lane
                       // never put a single track on the wire.
                       "! capsfilter caps=\"application/x-rtp,media=audio,"
                       "encoding-name=OPUS,payload=111\"")
            .arg(source, m_microphoneMuted ? QStringLiteral("true")
                                           : QStringLiteral("false"));

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
        // The message is GStreamer's own parse diagnostic — element and
        // property names, never user content. It used to be discarded, which
        // left "the call publishes nothing" with no way to find out why.
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
    // Encrypt on the ENCODER's src pad — after encoding, before RTP
    // payloading. That is one whole encoded frame, which is the unit
    // LiveKit and Element Call encrypt.
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "audioenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded, /*video=*/false);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    if (srcPad && sinkPad)
        gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(bin);
}

void SfuMediaEngine::publishVideo(const QString &cid, bool screenShare,
                                  int nodeId)
{
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    QString source;
    if (m_testSources) {
        source = QStringLiteral("videotestsrc is-live=true pattern=smpte");
    } else if (screenShare) {
        // Wayland and modern X11 desktops both capture through PipeWire; the
        // node id comes from the xdg-desktop-portal ScreenCast session, so
        // this never touches the framebuffer directly and the user's picker
        // decides what is shared. A negative node id would mean "whatever
        // PipeWire feels like", which is exactly how you publish the wrong
        // monitor, so it is refused.
        if (nodeId < 0) {
            Q_EMIT failed(QStringLiteral("screen_share_no_source"));
            return;
        }
        source = QStringLiteral("pipewiresrc path=%1 do-timestamp=true")
                     .arg(nodeId);
    } else {
        source = QStringLiteral("autovideosrc");
    }

    // Resolution and rate CEILINGS, matching livekit-client's own presets:
    // screen share is ScreenSharePresets.h1080fps30 (1920x1080, 30 fps,
    // 3 Mbit/s) and camera is the h720 default (1280x720, 30 fps,
    // 1.7 Mbit/s). Element Call publishes at those numbers, so a Lightning
    // share lands in an Element grid looking like every other one.
    //
    // The ceiling is the point, not the exact size. A screencast source is
    // whatever the monitor is: on a 4K display an uncapped pipeline asks
    // VP8 to encode 3840x2160 in real time, which costs far more CPU than
    // the frame is worth and overruns the bitrate anyway. The caps are
    // RANGES, so videoscale picks the largest size inside them that keeps
    // the display aspect ratio — an ultrawide stays ultrawide instead of
    // being stretched into 16:9.
    const QString limits = screenShare
        ? QStringLiteral("video/x-raw,width=(int)[1,1920],"
                         "height=(int)[1,1080],"
                         "framerate=(fraction)[0/1,30/1]")
        : QStringLiteral("video/x-raw,width=(int)[1,1280],"
                         "height=(int)[1,720],"
                         "framerate=(fraction)[0/1,30/1]");

    // Screen content is text-heavy and mostly static, so the trades differ
    // from a camera's on every axis:
    //   * static-threshold skips macroblocks that did not change, which on a
    //     desktop is most of the frame most of the time. On a camera it
    //     would smear real motion, so it stays 0 there.
    //   * a longer keyframe distance spends the budget on legible text
    //     rather than on periodic full frames.
    //   * cpu-used trades encoder effort for headroom, which 1080p needs and
    //     720p does not.
    const QString encoder = screenShare
        ? QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                         "cpu-used=4 static-threshold=100 "
                         "keyframe-max-dist=60 "
                         "end-usage=cbr target-bitrate=3000000")
        : QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                         "cpu-used=2 static-threshold=0 "
                         "keyframe-max-dist=30 "
                         "end-usage=cbr target-bitrate=1700000");
    const QString description =
        QStringLiteral("%1 ! queue ! videoconvert ! videoscale ! videorate "
                       "! %2 "
                       "! valve name=vidvalve drop=false ! %3 name=videoenc "
                       "! rtpvp8pay pt=96 "
                       // capsfilter for the same reason as the audio path.
                       "! capsfilter caps=\"application/x-rtp,media=video,"
                       "encoding-name=VP8,payload=96\"")
            .arg(source, limits, encoder);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
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
    gst_element_set_name(bin, cid.toUtf8().constData());
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), bin)) {
        Q_EMIT failed(QStringLiteral("camera_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "videoenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded, /*video=*/true);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    if (srcPad && sinkPad)
        gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(bin);
}

void SfuMediaEngine::unpublish(const QString &cid)
{
    GstElement *bin = m_publishedBins.take(cid);
    if (!bin || !m_publisher.pipeline)
        return;
    gst_element_set_state(bin, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
    renegotiatePublisher();
}

void SfuMediaEngine::renegotiatePublisher()
{
    if (!m_publisher.webrtc)
        return;
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, promiseCtxNew(this, m_publisher.webrtc, true),
        promiseCtxFree);
    g_signal_emit_by_name(m_publisher.webrtc, "create-offer", nullptr,
                          promise);
}

namespace {

/// Media-section index -> LiveKit stream id, from each section's `msid`.
///
/// Unified plan puts one `a=msid:<stream-id> <track-id>` per media section,
/// and LiveKit's stream id IS the sending participant's sid. That is the
/// same attribute livekit-client reads to decide which participant a
/// received track belongs to.
QHash<int, QString> streamIdsFromSdp(GstSDPMessage *message)
{
    QHash<int, QString> out;
    if (!message)
        return out;
    const guint sections = gst_sdp_message_medias_len(message);
    for (guint index = 0; index < sections; ++index) {
        const GstSDPMedia *media = gst_sdp_message_get_media(message, index);
        if (!media)
            continue;
        QString streamId;
        const guint attributes = gst_sdp_media_attributes_len(media);
        for (guint a = 0; a < attributes; ++a) {
            const GstSDPAttribute *attribute =
                gst_sdp_media_get_attribute(media, a);
            if (!attribute || !attribute->key)
                continue;
            const QLatin1String key(attribute->key);
            if (key != QLatin1String("msid"))
                continue;
            // "<stream-id> <track-id>"; the stream id is what attributes the
            // track to a sender.
            const QString value = QString::fromUtf8(attribute->value
                                                        ? attribute->value
                                                        : "");
            streamId = value.section(QLatin1Char(' '), 0, 0).trimmed();
            break;
        }
        out.insert(static_cast<int>(index), streamId);
    }
    return out;
}

} // namespace

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
    // GStreamer's SDP parser is PERMISSIVE: it returns OK for text that is
    // not SDP at all, producing an empty message. A description with no
    // media sections describes nothing, and handing that to webrtcbin as a
    // remote description is how a call ends up negotiated against garbage.
    // So the section count is the real check, not the parser's verdict.
    if (!parsed || gst_sdp_message_medias_len(message) == 0) {
        gst_sdp_message_free(message);
        // The SDP text itself is never logged: it carries host IPs.
        Q_EMIT failed(QStringLiteral("bad_remote_sdp"));
        return;
    }
    // Only the SUBSCRIBER connection carries other people's tracks, so only
    // its description tells us who a received pad belongs to. Recorded
    // BEFORE set-remote-description, because pad-added can fire from inside
    // that call and would otherwise find the map empty.
    if (target == Target::Subscriber)
        noteStreamIds(streamIdsFromSdp(message));

    const GstWebRTCSDPType type = (kind == QLatin1String("offer"))
        ? GST_WEBRTC_SDP_TYPE_OFFER
        : GST_WEBRTC_SDP_TYPE_ANSWER;
    GstWebRTCSessionDescription *description =
        gst_webrtc_session_description_new(type, message);
    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(peer.webrtc, "set-remote-description", description,
                          promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(description);
    peer.remoteDescriptionSet = true;

    // Candidates that arrived before the description could not be applied;
    // drain them now. Dropping them instead is the realistic call-killer,
    // because the SFU trickles immediately.
    for (const QString &pending : peer.pendingCandidates) {
        QString line;
        int mline = 0;
        if (parseCandidateInit(pending, &line, &mline)) {
            g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", mline,
                                  line.toUtf8().constData());
        }
    }
    peer.pendingCandidates.clear();

    // The server offers on SUBSCRIBER and expects our answer.
    if (type == GST_WEBRTC_SDP_TYPE_OFFER) {
        GstPromise *answerPromise = gst_promise_new_with_change_func(
            onAnswerCreated,
            promiseCtxNew(this, peer.webrtc, target == Target::Publisher),
            promiseCtxFree);
        g_signal_emit_by_name(peer.webrtc, "create-answer", nullptr,
                              answerPromise);
    }
}

void SfuMediaEngine::applyRemoteCandidate(Target target,
                                          const QString &candidateInit)
{
    if (!m_active)
        return;
    Peer &peer = peerFor(target);
    if (!peer.webrtc || !peer.remoteDescriptionSet) {
        // Bounded: a peer that never negotiates must not grow this list
        // without limit.
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
    m_microphoneMuted = muted;
    if (!m_publisher.pipeline)
        return;
    // Every published audio bin carries a valve named micvalve. drop=true
    // discards buffers BEFORE the encoder, so nothing is published at all.
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        if (GstElement *valve =
                gst_bin_get_by_name(GST_BIN(it.value()), "micvalve")) {
            g_object_set(valve, "drop", muted ? TRUE : FALSE, nullptr);
            gst_object_unref(valve);
        }
    }
}

void SfuMediaEngine::setOutputMuted(bool muted)
{
    m_outputMuted.store(muted);
    if (!m_subscriber.pipeline)
        return;
    // Matched by the NAME we gave them, never by the volume FACTORY:
    // autoaudiosink is a bin that may contain a volume element of its own,
    // and a recursive factory match would reach into it.
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
            // Fires exactly when a remote track is being added, which is
            // exactly when a missed element would stay audible. Bounded.
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

void SfuMediaEngine::setParticipantVolume(const QString &identity,
                                          int percent)
{
    if (!m_subscriber.pipeline || identity.isEmpty())
        return;
    const double volume = qBound(0, percent, 100) / 100.0;
    // Each remote audio bin's volume element is named outvol_<identity>, so
    // one participant can be attenuated without touching anyone else. This
    // is LOCAL ONLY: it changes nothing for other participants and sends no
    // event of any kind.
    const QString target = QStringLiteral("outvol_%1").arg(identity);
    if (GstElement *element = gst_bin_get_by_name(
            GST_BIN(m_subscriber.pipeline), target.toUtf8().constData())) {
        g_object_set(element, "volume", volume, nullptr);
        gst_object_unref(element);
    }
}

void SfuMediaEngine::setVideoRouter(SfuVideoRouter *router)
{
    m_videoRouter = router;
}

SfuVideoRouter *SfuMediaEngine::videoRouter() const
{
    return m_videoRouter.data();
}

void SfuMediaEngine::setOutboundKey(int index, const QByteArray &rawKey)
{
    if (!m_sendCryptor->setKey(index, rawKey))
        return;
    m_sendCryptor->setCurrentKeyIndex(index);
    // Published only after the key is really installed, so a probe can never
    // see "ready" without a usable key behind it.
    m_sendKeyReady.store(true);
}

void SfuMediaEngine::setInboundKey(const QString &streamId, int index,
                                   const QByteArray &rawKey)
{
    if (streamId.isEmpty())
        return;
    // Created on first sight, so a key can arrive before that sender's
    // track does. The reverse order is handled in pad-added.
    if (!recvCryptorFor(streamId)->setKey(index, rawKey))
        return;
    m_recvKeyReady.store(true);
}

std::shared_ptr<CallFrameCryptor>
SfuMediaEngine::recvCryptorFor(const QString &streamId)
{
    QMutexLocker lock(&m_recvMutex);
    auto it = m_recvCryptors.constFind(streamId);
    if (it != m_recvCryptors.cend())
        return it.value();
    auto cryptor = std::make_shared<CallFrameCryptor>();
    m_recvCryptors.insert(streamId, cryptor);
    return cryptor;
}

bool SfuMediaEngine::encryptionActive() const
{
    // The honest answer: frames are encrypted only when a key is installed.
    // Never optimistic — this is what the controller reports to the user.
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
        // Cleared AND dropped: the per-sender rings belong to the call that
        // is ending. A probe still holding a shared_ptr sees an empty ring
        // and drops, which is the correct answer for a torn-down call.
        for (const auto &cryptor : std::as_const(m_recvCryptors))
            cryptor->clearKeys();
        m_recvCryptors.clear();
        m_streamForMline.clear();
    }
    m_sendKeyReady.store(false);
    m_recvKeyReady.store(false);
}

void SfuMediaEngine::noteStreamIds(const QHash<int, QString> &byMline)
{
    QMutexLocker lock(&m_recvMutex);
    for (auto it = byMline.cbegin(); it != byMline.cend(); ++it) {
        // An absent or absurd value leaves the section UNATTRIBUTED, which
        // drops rather than mis-decrypts.
        if (it.value().isEmpty() || it.value().size() > 256)
            m_streamForMline.remove(it.key());
        else
            m_streamForMline.insert(it.key(), it.value());
    }
}

void SfuMediaEngine::installEncryptProbe(GstPad *pad, bool video)
{
    if (!pad)
        return;
    auto *ctx = new CryptoProbeCtx;
    ctx->engine = this;
    // The send cryptor outlives every send probe (it is owned by the engine
    // for the engine's whole life), so a non-owning alias is enough here.
    ctx->cryptor = std::shared_ptr<CallFrameCryptor>(m_sendCryptor.get(),
                                                     [](CallFrameCryptor *) {});
    ctx->encrypting = true;
    ctx->video = video;
    ctx->required = &m_encryptionRequired;
    ctx->keyReady = &m_sendKeyReady;
    // One per encrypting track, never reused within a session.
    ctx->ivStream = m_nextIvStream.fetch_add(1);
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cryptoProbe, ctx,
                      cryptoProbeCtxFree);
}

void SfuMediaEngine::installDecryptProbe(GstPad *pad, bool video,
                                         const QString &streamId)
{
    if (!pad)
        return;
    auto *ctx = new CryptoProbeCtx;
    ctx->engine = this;
    // A sender we cannot attribute gets its own ring under the empty key
    // rather than sharing anyone else's: decrypting one participant's frames
    // with another's key is a silent corruption, and no key at all is an
    // honest drop.
    ctx->cryptor = recvCryptorFor(streamId);
    ctx->encrypting = false;
    ctx->video = video;
    ctx->required = &m_encryptionRequired;
    ctx->keyReady = &m_recvKeyReady;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cryptoProbe, ctx,
                      cryptoProbeCtxFree);
}

bool SfuMediaEngine::tokenIsLive(quintptr token, Target *target) const
{
    if (!m_active)
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

void SfuMediaEngine::handleLocalDescription(quintptr token, bool offer,
                                            const QString &sdp)
{
    Target target = Target::Publisher;
    if (!tokenIsLive(token, &target))
        return; // a queued callback from a closed session
    Q_EMIT localDescription(static_cast<int>(target),
                            offer ? QStringLiteral("offer")
                                  : QStringLiteral("answer"),
                            sdp);
}

void SfuMediaEngine::handleLocalCandidate(quintptr token, int mlineIndex,
                                          const QString &candidate)
{
    Target target = Target::Publisher;
    if (!tokenIsLive(token, &target))
        return;
    Q_EMIT localCandidate(static_cast<int>(target),
                          candidateInitJson(candidate, mlineIndex));
}

void SfuMediaEngine::handleFailure(quintptr token, const QString &category)
{
    if (!tokenIsLive(token))
        return;
    Q_EMIT failed(category);
}

// ── GStreamer-thread callbacks ──────────────────────────────────────────

void SfuMediaEngine::onNegotiationNeeded(GstElement *webrtc, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, promiseCtxNew(engine, webrtc, true), promiseCtxFree);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void SfuMediaEngine::onOfferCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "offer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description,
                          nullptr);
    gst_promise_unref(promise);
    if (!description)
        return;

    GstPromise *local = gst_promise_new();
    g_signal_emit_by_name(ctx->webrtc, "set-local-description", description,
                          local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);

    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);

    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    auto *engine = ctx->engine;
    marshal(engine, [engine, token, sdp] {
        engine->handleLocalDescription(token, /*offer=*/true, sdp);
    });
}

void SfuMediaEngine::onAnswerCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "answer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description,
                          nullptr);
    gst_promise_unref(promise);
    if (!description)
        return;

    GstPromise *local = gst_promise_new();
    g_signal_emit_by_name(ctx->webrtc, "set-local-description", description,
                          local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);

    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);

    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    auto *engine = ctx->engine;
    marshal(engine, [engine, token, sdp] {
        engine->handleLocalDescription(token, /*offer=*/false, sdp);
    });
}

void SfuMediaEngine::onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                                    char *candidate, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const QString line = QString::fromUtf8(candidate ? candidate : "");
    const int mline = static_cast<int>(mlineIndex);
    marshal(engine, [engine, token, mline, line] {
        engine->handleLocalCandidate(token, mline, line);
    });
}

void SfuMediaEngine::onPadAdded(GstElement *webrtc, void *pad, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    GstPad *srcPad = GST_PAD(pad);
    if (GST_PAD_DIRECTION(srcPad) != GST_PAD_SRC)
        return;
    GstElement *pipeline = GST_ELEMENT(gst_element_get_parent(webrtc));
    if (!pipeline)
        return;

    // Which sender is this? webrtcbin names a received pad `src_<index>`
    // where the index is the SDP media-section index, which is exactly the
    // key noteStreamIds() recorded. That is how a decrypt probe gets the
    // right sender's key ring instead of a shared one.
    QString streamId;
    {
        const gchar *rawName = GST_PAD_NAME(srcPad);
        const QString padName = QString::fromUtf8(rawName ? rawName : "");
        bool ok = false;
        const int mline =
            padName.section(QLatin1Char('_'), -1).toInt(&ok);
        if (ok) {
            QMutexLocker lock(&engine->m_recvMutex);
            streamId = engine->m_streamForMline.value(mline);
        }
        if (streamId.isEmpty()) {
            // Unattributed. Deliberately given its OWN ring keyed by the
            // media-section index rather than folded into a shared one: a
            // frame decrypted with the wrong participant's key is silent
            // corruption, and a ring nobody has keyed simply drops.
            streamId = QStringLiteral("mline:%1").arg(mline);
        }
    }

    // What kind of media is this? The caps tell us; without them we cannot
    // build a chain, and guessing would produce a pipeline that fails later
    // and further from the cause.
    GstCaps *caps = gst_pad_get_current_caps(srcPad);
    if (!caps)
        caps = gst_pad_query_caps(srcPad, nullptr);
    QString mediaKind;
    if (caps) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const gchar *media =
            structure ? gst_structure_get_string(structure, "media") : nullptr;
        mediaKind = QString::fromUtf8(media ? media : "");
        gst_caps_unref(caps);
    }
    if (mediaKind.isEmpty())
        mediaKind = QStringLiteral("audio");

    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    // The volume element keeps the "outvol" name so deafen reaches every
    // received track regardless of who sent it.
    // Received video goes to an appsink whose samples become QVideoFrames on
    // a QML VideoOutput. RGBA because that maps 1:1 onto
    // QVideoFrameFormat::Format_RGBA8888 with a plain row copy — a planar
    // format would need per-plane strides for no benefit at these sizes.
    //
    // max-buffers=1 drop=true is deliberate: a late video frame is worthless
    // and queueing them turns a slow consumer into growing latency. Audio is
    // never dropped this way.
    const QString description = mediaKind == QLatin1String("video")
        ? (engine->testSourceMode()
               ? QStringLiteral("queue ! rtpvp8depay name=recvdepay ! vp8dec "
                                "! videoconvert ! fakesink sync=false")
               : QStringLiteral("queue ! rtpvp8depay name=recvdepay ! vp8dec "
                                "! videoconvert ! video/x-raw,format=RGBA "
                                "! appsink name=vidsink emit-signals=true "
                                "sync=false max-buffers=1 drop=true"))
        : (engine->testSourceMode()
               ? QStringLiteral("queue ! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=outvol "
                                "! fakesink sync=false")
               : QStringLiteral("queue ! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=outvol "
                                "! autoaudiosink"));

    GError *error = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error) {
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        gst_object_unref(pipeline);
        marshal(engine, [engine, token] {
            engine->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    if (!gst_bin_add(GST_BIN(pipeline), bin)) {
        gst_object_unref(pipeline);
        marshal(engine, [engine, token] {
            engine->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    // Decrypt on the DEPAYLOADER's src pad: the encoded frame is whole again
    // there, which is the unit LiveKit encrypted. Installed before the bin
    // plays, so no frame can reach the decoder unexamined.
    if (GstElement *depay = gst_bin_get_by_name(GST_BIN(bin), "recvdepay")) {
        if (GstPad *framePad = gst_element_get_static_pad(depay, "src")) {
            engine->installDecryptProbe(framePad,
                                        mediaKind == QLatin1String("video"),
                                        streamId);
            gst_object_unref(framePad);
        }
        gst_object_unref(depay);
    }
    // Route decoded video to whoever is showing this participant. Installed
    // before the bin plays so no frame is produced without a destination
    // decided.
    if (GstElement *appsink = gst_bin_get_by_name(GST_BIN(bin), "vidsink")) {
        auto *ctx = new VideoSinkCtx{engine, streamId};
        g_signal_connect_data(appsink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(appsink);
    }
    // Apply the CURRENT deafen state before the bin plays: a track arriving
    // after the user deafened must not be audible even briefly.
    if (GstElement *volume = gst_bin_get_by_name(GST_BIN(bin), "outvol")) {
        g_object_set(volume, "mute",
                     engine->m_outputMuted.load() ? TRUE : FALSE, nullptr);
        gst_object_unref(volume);
    }
    gst_element_sync_state_with_parent(bin);
    GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
    const GstPadLinkReturn linked = gst_pad_link(srcPad, sinkPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_object_unref(pipeline);
    if (linked != GST_PAD_LINK_OK) {
        marshal(engine, [engine, token] {
            engine->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    marshal(engine, [engine, mediaKind, streamId] {
        // The stream id is the sending participant's LiveKit sid, from the
        // subscriber offer's msid. The UI still keys tiles off MatrixRTC
        // membership, which is authoritative for who is PRESENT; this says
        // which of them is actually sending.
        Q_EMIT engine->remoteTrackAdded(streamId, mediaKind);
    });
}
