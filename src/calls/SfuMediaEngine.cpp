#include "calls/SfuMediaEngine.h"

#include <unistd.h>

#include "calls/CallFrameCryptor.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/SfuVideoRouter.h"

#include <mutex>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
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

/// A monotonic millisecond clock shared by every publish probe.
///
/// Deliberately not the wall clock: these values are DIFFERENCES measured
/// across seconds of a live call, and a wall clock that steps (NTP, a
/// suspend/resume) would report a negative hold or an invented one.
qint64 monotonicMs()
{
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
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
    /// while a frame of theirs is still in the probe. SEND side only — the
    /// receive side resolves its ring per frame through `streamId`, because
    /// which ring a track belongs to is not known until the SFU has told us
    /// which participant is sending on that media section, and that can
    /// arrive after the pad does.
    std::shared_ptr<CallFrameCryptor> cryptor;
    /// Receive side: the sending participant's LiveKit sid, as unpacked from
    /// the subscriber offer's `msid`.
    QString streamId;
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
    /// Engine-wide totals this probe contributes to. Raw pointers into the
    /// engine, which outlives every probe (probes are removed with the
    /// pipelines in destroyPeer, on the Qt thread) — the same lifetime the
    /// `required` / `keyReady` pointers below already assume.
    std::atomic<quint64> *total = nullptr;
    std::atomic<quint64> *totalDropped = nullptr;
    /// Frames this probe let through, and frames it dropped.
    ///
    /// The ONE number that separates "our media never reaches the wire" from
    /// "it reaches the wire and the far end cannot use it" — two failures
    /// that look identical from the outside and have nothing in common. Both
    /// are counts of frames, never content. Owned by the probe, so no
    /// locking: a pad probe runs on one streaming thread.
    quint64 passed = 0;
    quint64 dropped = 0;
};

/// Log the first frame, then rarely. A per-frame log at 50 fps is not a log.
bool shouldReport(quint64 count)
{
    return count == 1 || count == 10 || count % 500 == 0;
}

/// Which sender an appsink belongs to. One per received video track.
struct VideoSinkCtx {
    SfuMediaEngine *engine = nullptr;
    /// Primary routing key: the media section's SDP `mid`, which LiveKit also
    /// states on the TrackInfo, so it names ONE track. Empty for the local
    /// self-view branch, which has no negotiated section.
    QString mid;
    /// Fallback routing key: the sending participant's sid. One per
    /// participant, so it cannot distinguish their camera from their screen
    /// share — kept because it is what a tile attaches to before any track
    /// has been announced, and because a server that omits `mid` must still
    /// render video.
    QString streamId;
    /// Frames that decrypted and had nowhere to go. LAST, so the brace
    /// initialisers that build this stay valid. See onVideoSample.
    quint64 unrouted = 0;
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
    QString routeKey;
    if (SfuVideoRouter *router = ctx->engine->videoRouter()) {
        // The mid wins when something is listening for it: that is the tile
        // that asked for THIS track. Otherwise fall back to the sender.
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
        // DECRYPTED BUT NOWHERE TO GO. Frames arriving and decrypting is not
        // the same as frames being SEEN, and the two were indistinguishable:
        // the crypto counters climb either way, so a tile that never attached
        // its sink looks exactly like working video. Logged rarely, with the
        // keys involved, so a routing mismatch names itself.
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
    const QString streamId = routeKey;
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
    if (!buffer || !ctx)
        return GST_PAD_PROBE_OK;
    // The SEND ring is the engine's own and never changes. A RECEIVE ring is
    // looked up every frame: a key arrives addressed to a Matrix DEVICE, and
    // the sid that device is publishing under is only learned from a
    // participant update, which is not ordered against the pad appearing. A
    // ring captured at install time froze that race in whichever order the
    // SFU happened to send them.
    const std::shared_ptr<CallFrameCryptor> cryptor = ctx->encrypting
        ? ctx->cryptor
        : (ctx->engine ? ctx->engine->recvCryptorFor(ctx->streamId)
                       : nullptr);
    if (!cryptor)
        return GST_PAD_PROBE_OK;

    const bool haveKey = ctx->keyReady && ctx->keyReady->load();
    if (!haveKey) {
        // No key yet. In an encrypted room that means DROP: sending in the
        // clear would silently un-encrypt a call the user was told is
        // end-to-end encrypted, and rendering an undecryptable frame is
        // worse than dropping it.
        const bool required = ctx->required && ctx->required->load();
        if (required) {
            ++ctx->dropped;
            if (ctx->totalDropped)
                ctx->totalDropped->fetch_add(1);
            if (shouldReport(ctx->dropped)) {
                qCWarning(lcSfuMedia)
                    << "frames dropped: no key" << (ctx->encrypting ? "out" : "in")
                    << "video=" << ctx->video << "count=" << ctx->dropped;
            }
            return GST_PAD_PROBE_DROP;
        }
        ++ctx->passed;
        if (ctx->total)
            ctx->total->fetch_add(1);
        if (shouldReport(ctx->passed)) {
            qCInfo(lcSfuMedia) << "frames in the clear"
                               << (ctx->encrypting ? "out" : "in")
                               << "video=" << ctx->video
                               << "count=" << ctx->passed;
        }
        return GST_PAD_PROBE_OK;
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
        output = cryptor->encryptFrame(input, kind, ctx->ivStream,
                                       timestamp);
    } else {
        output = cryptor->decryptFrame(input, kind);
    }

    if (output.isEmpty()) {
        // Encryption refused (no key), or decryption failed its
        // authentication tag. Either way the frame does not go on: there is
        // no cleartext fallback in this path by design.
        //
        // On the RECEIVE side a run of these is the signature of a key
        // mismatch — media IS arriving and none of it is usable, which is
        // indistinguishable from silence without this line.
        ++ctx->dropped;
        if (ctx->totalDropped)
            ctx->totalDropped->fetch_add(1);
        if (shouldReport(ctx->dropped)) {
            qCWarning(lcSfuMedia)
                << (ctx->encrypting ? "encrypt failed" : "decrypt failed")
                << "video=" << ctx->video << "count=" << ctx->dropped
                << "passed=" << ctx->passed;
        }
        return GST_PAD_PROBE_DROP;
    }
    ++ctx->passed;
    if (ctx->total)
        ctx->total->fetch_add(1);
    if (shouldReport(ctx->passed)) {
        qCInfo(lcSfuMedia) << (ctx->encrypting ? "frames encrypted"
                                               : "frames decrypted")
                           << "video=" << ctx->video
                           << "count=" << ctx->passed
                           << "dropped=" << ctx->dropped;
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
    // Our own VP8 payloader, which the probe below then requires like any
    // other element. Registered here because this is the first thing that
    // runs after gst_init and before any pipeline is built.
    lightning::rtp::registerVp8Payloader();
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
        // NOT `compositor`: it was the screen share's rate stage for one
        // commit and is gone again (see videoRateStage()). Requiring an
        // element nothing builds a pipeline from would refuse a call on a
        // capability the engine never exercises.
        // Ours. Absent means encrypted video could not be sent at all, which
        // is a refusal to make honestly rather than at the first frame.
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
    m_framesEncrypted.store(0);
    m_framesDecrypted.store(0);
    m_framesDropped.store(0);
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    Q_EMIT connectionStateChanged(QStringLiteral("connecting"));
}

void SfuMediaEngine::stop()
{
    if (!m_active && !m_publisher.pipeline && !m_subscriber.pipeline)
        return;
    m_generation.fetch_add(1);
    m_active = false;
    m_publishedBins.clear();
    m_publishWatch.clear();
    // destroyPeer tears the pipelines down; the descriptors those bins were
    // using are ours to close and would otherwise leak one per screen share
    // per call.
    for (auto it = m_publishedFds.cbegin(); it != m_publishedFds.cend(); ++it) {
        if (it.value() > 0)
            ::close(it.value());
    }
    m_publishedFds.clear();
    destroyPeer(m_publisher);
    destroyPeer(m_subscriber);
    // Engine state is per session; the user's intent lives in the
    // controller and is re-applied on the next start.
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    // Media keys must not outlive the call that used them.
    clearKeys();
    Q_EMIT connectionStateChanged(QStringLiteral("closed"));
}

namespace {
/// GStreamer bus messages from an SFU pipeline.
///
/// This exists because the engine used to CLEAR a sync handler it never set:
/// every ERROR any element posted was discarded, so a capture that could not
/// start — a screen share whose PipeWire source cannot connect, for one —
/// looked exactly like a capture that produced no interesting frames. There
/// was nothing in a log to read.
///
/// Runs on a STREAMING THREAD: it logs and hands the pipeline back, and never
/// touches a Qt object. What is logged is the element's own name and the
/// plugin's static error string; the `debug` field is deliberately NOT logged
/// because it carries file paths and source locations.
GstBusSyncReply onBusMessage(GstBus *, GstMessage *message, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const GstMessageType type = GST_MESSAGE_TYPE(message);
    if (type != GST_MESSAGE_ERROR && type != GST_MESSAGE_WARNING)
        return GST_BUS_PASS;

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
    if (type == GST_MESSAGE_ERROR) {
        qCWarning(lcSfuMedia) << "pipeline error element=" << element
                              << "code=" << (error ? error->code : 0)
                              << "reason=" << reason;
        // ONE error is worth naming outright, because it is structural and
        // not a transient.
        //
        // `rtpvp8pay` PARSES the VP8 bitstream to build its payload
        // descriptor: it reads partition0's size from the frame tag, checks a
        // keyframe's `0x9d 0x01 0x2a` start code, then bool-decodes
        // segmentation and filter fields out of the compressed partition
        // (gstrtpvp8pay.c, gst_rtp_vp8_pay_parse_frame). We encrypt the whole
        // encoded frame first — as LiveKit and Element Call do — so the
        // payloader is handed ciphertext, fails to parse it, and posts
        // STREAM/ENCODE "Failed to parse VP8 frame". The share publishes
        // exactly one frame and stops.
        //
        // libwebrtc does not hit this: its VP8 packetizer takes the keyframe
        // flag and picture id from the encoder as METADATA and never reads the
        // payload. Interoperable encrypted video therefore needs a payloader
        // that does not parse, which rtpvp8pay has no option to become.
        if (element.startsWith(QLatin1String("rtpvp8pay"))) {
            qCWarning(lcSfuMedia)
                << "encrypted VP8 cannot be payloaded by rtpvp8pay: it parses"
                << "the bitstream. Video send is not carried in an encrypted"
                << "room until a non-parsing payloader lands.";
        }
    } else {
        qCInfo(lcSfuMedia) << "pipeline warning element=" << element
                           << "code=" << (error ? error->code : 0)
                           << "reason=" << reason;
    }
    if (error)
        g_error_free(error);
    if (debug)
        g_free(debug);
    // STILL NOT failed(), and that is deliberate and unchanged. Turning a bus
    // ERROR into failed() was tried and reverted the same hour: a pipeline
    // posts errors during ordinary teardown ("Internal data stream error"
    // from a source whose downstream has already gone), and under load the
    // engine's own suite saw three of them on a healthy key install.
    // failed() ENDS THE CALL (SfuCallController::onEngineFailed tears down),
    // so reporting those would tear working calls down.
    //
    // What IS raised, on one narrow shape, is publishFailed() — which ends
    // nothing. All of the discrimination happens on the GUI thread in
    // handlePublishError(); this side only answers "which publishing bin, if
    // any, does this error come from?", which needs no engine state at all.
    //
    // WHY THE PARENT WALK. GST_MESSAGE_SRC is the element that actually
    // failed — a `pipewiresrc` or a `v4l2src` deep inside the bin, whose own
    // name says nothing about the track. The bin we NAMED with the track's
    // cid is the ancestor sitting directly under the pipeline, so walking up
    // to it is the only way to attribute the error. m_publishedBins is NOT
    // consulted here: it is a plain QHash owned by the GUI thread.
    if (type == GST_MESSAGE_ERROR && engine) {
        QString publishCid;
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
                gst_object_unref(parent);
                gst_object_unref(walk);
                break;
            }
            gst_object_unref(walk);
            walk = parent;
        }
        if (!publishCid.isEmpty()) {
            marshal(engine, [engine, publishCid] {
                engine->handlePublishError(publishCid);
            });
        }
    }
    Q_UNUSED(engine);
    return GST_BUS_PASS;
}
} // namespace

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
    if (pipeline) {
        // Set BEFORE anything is added: an element that fails during its own
        // construction posts on the bus immediately.
        if (GstBus *bus = gst_element_get_bus(pipeline)) {
            gst_bus_set_sync_handler(bus, onBusMessage, this, nullptr);
            gst_object_unref(bus);
        }
    }
    // NAMED per peer connection, because every diagnostic below identifies
    // itself by the element name rather than by dereferencing the engine
    // from a GStreamer thread.
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
    // WHETHER THE CONNECTION EVER COMES UP.
    //
    // Nothing observed this, and its absence is why "no audio" could not be
    // told apart from "no connection": signalling runs over the WebSocket
    // and keeps working — participants, speakers and mute all update — while
    // ICE or DTLS never completes and not one RTP packet moves in either
    // direction. Element then draws a tile and a mute badge for a track it
    // will never receive, which is exactly what a stalled peer connection
    // looks like from the far side.
    //
    // `this` is the user data so destroyPeer's
    // g_signal_handlers_disconnect_by_data still removes these; the handler
    // reads only the element's own name and property, never the engine.
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
    // WHY LIGHTNING WAS QUIETER THAN ELEMENT ON THE SAME MICROPHONE.
    //
    // element-call captures through the browser's WebRTC audio path, which
    // runs automatic gain control by default. Lightning ran none, so the raw
    // capture went out at whatever level the device produced: same PC, same
    // mic, audibly quieter. Reported as "in element app my microphone volume
    // good, but in ligthing qiuet".
    //
    // `webrtcdsp` IS that processor — the same libwebrtc audio processing
    // module, in GStreamer form. Measured through this chain shape, on a
    // deliberately quiet -29 dBFS sine, sampled after the AGC converges:
    //
    //   without dsp   rms 1158.0   -29.0 dBFS
    //   with dsp      rms 3935.2   -18.4 dBFS      (+10.6 dB, ~3.4x)
    //
    // OPTIONAL, and absence must be free. It lives in gst-plugins-bad, which
    // a packaged build need not carry, and a description naming an element
    // that does not exist fails to PARSE — which would take the microphone
    // out entirely rather than making it quiet. So it is probed here and the
    // chain is byte-identical to the pre-existing one when it is missing;
    // it is deliberately NOT in kRequired for the same reason.
    //
    // `echo-cancel=false`: real echo cancellation needs a `webrtcechoprobe`
    // in the PLAYBACK path to tell the canceller what we are playing, and
    // wiring that is a change to the receive side. Claiming echo-cancel
    // without the probe cancels against nothing. Left off deliberately, and
    // the probe remains a follow-up.
    //
    // Explicit caps because webrtcdsp processes fixed 10 ms chunks of S16 —
    // and they are added ONLY on this branch, so the no-dsp chain keeps the
    // negotiation it has always had.
    static const bool dspAvailable = [] {
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        if (!factory)
            return false;
        gst_object_unref(factory);
        return true;
    }();
    const QString gainStage =
        dspAvailable
            ? QStringLiteral(
                  "! audio/x-raw,format=S16LE,rate=48000 "
                  "! webrtcdsp echo-cancel=false gain-control=true "
                  "noise-suppression=true ! audioconvert ")
            : QString();

    const QString description =
        QStringLiteral("%1 ! queue ! audioconvert ! audioresample "
                       "! valve name=micvalve drop=%2 "
                       "%5 "
                       // OWN MICROPHONE GAIN, in the RAW AUDIO DOMAIN and
                       // BEFORE the encoder — which is the only place it can
                       // go. After opusenc the samples are an encoded frame,
                       // and after the payloader they are an ENCRYPTED one;
                       // a volume element there would be scaling ciphertext.
                       //
                       // Placed after the valve rather than before it purely
                       // for readability: the valve is a hard stop, so the
                       // order between the two cannot matter. `volume` above
                       // 1.0 amplifies, which is what "200% like in discord"
                       // asks for; the initial value is stated here so a bin
                       // rebuilt on renegotiation comes up at the user's
                       // level rather than at unity for a moment.
                       "! volume name=micvol volume=%4 "
                       "! opusenc name=audioenc "
                       // ssrc=%3: see nextPublishSsrc(). Without an EXPLICIT
                       // ssrc the payloader has not chosen one when the offer
                       // is generated, so webrtcbin emits no `a=ssrc` lines
                       // and the SFU cannot associate arriving RTP with any
                       // transceiver — the track never publishes at all.
                       "! rtpopuspay pt=111 ssrc=%3 "
                       // capsfilter, NOT a bare caps string. gst_parse only
                       // accepts caps as a filter BETWEEN two elements; as
                       // the last item in a bin description the parser reads
                       // it as an element name and fails with
                       // `no element "application"`. That is exactly what
                       // happened here — every SFU audio and video publish
                       // failed on every machine, so the MatrixRTC lane
                       // never put a single track on the wire.
                       // The caps webrtcbin READS to build the m= section, so
                       // everything the SFU needs has to be stated here:
                       //   * ssrc — or the offer has no `a=ssrc` and hence no
                       //     `a=msid`, and the SFU cannot attribute arriving
                       //     RTP to a transceiver (gstwebrtcbin.c warns
                       //     "Caps are missing ssrc" and drops both lines).
                       //   * clock-rate and encoding-params — so the rtpmap
                       //     reads `opus/48000/2`, the form RFC 7587 defines
                       //     and every other client publishes.
                       "! capsfilter caps=\"application/x-rtp,media=audio,"
                       "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
                       "encoding-params=(string)2,ssrc=(uint)%3\"")
            .arg(source,
                 m_microphoneMuted ? QStringLiteral("true")
                                   : QStringLiteral("false"),
                 QString::number(nextPublishSsrc()),
                 // Locale-INDEPENDENT. gst_parse_bin_from_description reads
                 // "0,7" as a truncated 0 in a comma-decimal locale, which
                 // would silently mute a user whose desktop is Lithuanian —
                 // and this repo's maintainer's is.
                 QString::number(m_microphoneGain.load() / 100.0, 'f', 3),
                 gainStage);

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
    // The link RESULT decides whether there is anything to offer, so it is
    // checked rather than discarded. It used to be thrown away, and a failed
    // link produced an offer with no media section instead of an error.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    applyPublisherMsid(sinkPad, cid);
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad)
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "publisher link failed code=" << linked;
        m_publishedBins.remove(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("publish_link_failed"));
        return;
    }
    gst_element_sync_state_with_parent(bin);
    // Now there IS something to offer, so ask for the offer explicitly. The
    // on-negotiation-needed that fired at PLAYING was deliberately ignored.
    ++m_publishedMedia;
    renegotiatePublisher();
}

quint32 SfuMediaEngine::nextPublishSsrc()
{
    // A DISTINCT, NON-ZERO synchronisation source per published track,
    // chosen by us rather than by the payloader.
    //
    // webrtcbin writes `a=ssrc:<n> msid:<msid> <trans>` only for an ssrc it
    // can read out of the pad's CAPS (gstwebrtcbin.c, _media_add_ssrcs).
    // A payloader that has not been asked for one has not picked it yet when
    // create-offer runs, so the offer goes out with no `a=ssrc` and no
    // `a=msid` — and an SFU receiving RTP it cannot attribute to a
    // transceiver reports an empty mid and times the publication out. Not a
    // credential and not key material: an SSRC is a public RTP field, so the
    // ordinary generator is right here.
    quint32 ssrc = 0;
    while (ssrc == 0)
        ssrc = QRandomGenerator::global()->generate();
    return ssrc;
}

void SfuMediaEngine::applyPublisherMsid(GstPad *sinkPad, const QString &cid)
{
    if (!sinkPad)
        return;
    // NAME THE TRACK IN THE SDP, or the SFU cannot tell which declared track
    // a media section carries.
    //
    // webrtcbin's offer is minimal by default: no `a=msid`, no `a=ssrc`. The
    // SFU then receives RTP it cannot associate with any transceiver and logs
    // `could not get mid for track {trackID: ""}`, followed by
    // `publish time out` — the track stays DECLARED but never becomes
    // PUBLISHED. Every symptom of a dead call follows from that one fact: no
    // audio, no video, no screen share, nothing to forward to anyone, and a
    // remote client that renders the participant as muted because their
    // track never started.
    //
    // livekit-client never has to do this: its cid IS
    // `track.mediaStreamTrack.id`, which the browser writes into the SDP for
    // it. Setting the pad's msid to the same cid reproduces that by hand.
    g_object_set(sinkPad, "msid", cid.toUtf8().constData(), nullptr);
}

QString SfuMediaEngine::videoRateStage(bool screenShare)
{
    Q_UNUSED(screenShare);
    // ONE STAGE FOR BOTH, AND `compositor` MUST NOT COME BACK HERE.
    //
    // 2026-08-25 shipped `compositor background=black` for the screen share
    // to kill videorate's first-buffer hold (measured: first encoded buffer
    // 5.515 s -> 0.015 s). It worked, and it broke the share, because
    // COMPOSITOR IS NOT A SCALER. It paints each input at its native size at
    // xpos/ypos on an output canvas — so a 3840x2160 desktop on the 1920x1080
    // canvas the size ceiling negotiates showed the TOP-LEFT QUARTER of the
    // screen and nothing else. Reported as "shares only 1/4 of my screen",
    // and measured directly afterwards:
    //
    //   videotestsrc 3840x2160 ! videoconvert ! videoscale
    //     ! compositor background=black
    //     ! video/x-raw,width=[1,1920],height=[1,1080],framerate=30/1
    //   -> compositor SINK negotiated 3840x2160, SRC 1920x1080
    //
    // videoscale upstream does NOT rescue it: compositor's sink pad accepts
    // the full 4K, so nothing ever asks videoscale to scale. Fixing it would
    // mean driving the sink pad's `sizing-policy`/width/height per capture,
    // which hardcodes an aspect this code does not know.
    //
    // So the hold is back, and it is the lesser fault: a share that takes
    // ~1 s (busy desktop) to 10 s (still desktop) to appear is annoying; a
    // share that shows a quarter of the screen is broken. The hold is real
    // and still open — videorate emits nothing until a SECOND input buffer
    // arrives, and PipeWire delivers on damage, so the wait IS "how long
    // until something on screen changes". Whatever fixes it must be measured
    // on a REAL 4K capture and must assert the negotiated output size.
    //
    // Refuted and not to be retried: `queue min-threshold-buffers=0` and
    // `identity` in front of videorate (neither can manufacture the second
    // buffer); `capssetter` (gst_util_fraction_multiply CRITICAL, zero frames
    // out); `videorate skip-to-first` and `max-duplication-time`; and the two
    // SOURCE-level properties `min-buffers=8` and `keepalive-time=100`, each
    // of which killed the capture outright.
    return QStringLiteral("videorate");
}

QString SfuMediaEngine::videoPipelineDescription(const QString &source,
                                                const QString &rateStage,
                                                const QString &limits,
                                                const QString &encoder,
                                                const QString &selfView,
                                                quint32 ssrc)
{
    return QStringLiteral(
               // `capsrc` is named so a probe can count what the CAPTURE
               // actually delivers, separately from what the encoder emits.
               // The RATE STAGE (%7 — videoRateStage()) sits between them and
               // DUPLICATES the last frame to hold the rate, so a capture
               // that stalls still produces a full-rate stream of identical
               // pictures — which looks like healthy send counters and a
               // frozen image at both ends. That is true of BOTH rate
               // elements, which is why the capture is counted on its own.
               // `leaky=downstream` on the CAPTURE queue, bounded small.
               //
               // Realtime video must drop a late frame, never stall the thing
               // producing it. A plain queue applies backpressure, so a
               // software VP8 encoder that falls behind at 1080p reaches back
               // and stalls the PipeWire source — and a stalled screen capture
               // is indistinguishable from a working one downstream, because
               // the rate stage then repeats the last picture at full rate.
               "%1 name=capsrc ! queue max-size-buffers=4 leaky=downstream "
               "! videoconvert ! videoscale ! %7 "
               "! %2 "
               "! tee name=t %4"
               "t. ! queue "
               "! valve name=vidvalve drop=false ! %3 name=videoenc "
               // OUR payloader, not rtpvp8pay: see RtpVp8Payloader.h.
               // rtpvp8pay parses the VP8 bitstream and cannot payload an
               // encrypted frame.
               // ssrc=%5: same reason as the audio path.
               "! %6 pt=96 ssrc=%5 "
               // Same as the audio path: the ssrc must be in the caps or the
               // offer names no source and no track.
               "! capsfilter caps=\"application/x-rtp,media=video,"
               "encoding-name=VP8,payload=96,clock-rate=(int)90000,"
               "ssrc=(uint)%5\"")
        .arg(source, limits, encoder, selfView, QString::number(ssrc),
             QLatin1String(lightning::rtp::vp8PayloaderName()), rateStage);
}

QString SfuMediaEngine::trackSidFromMsid(const QString &msid)
{
    // THE TRACK SID (`TR_…`) — the only id that names one track on both ends.
    //
    // `a=msid:<stream-id> <track-id>`, and LiveKit packs the stream id as
    // `PA_<participant>|TR_<track>`. So the track sid is the half after the
    // separator, or failing that the track-id token. That is exactly
    // livekit-client's `extractTrackSid()`.
    //
    // This replaced routing by the media-section `mid`, which CANNOT work: the
    // `mid` LiveKit states on a TrackInfo is the PUBLISHER's media-section id,
    // and the mid our subscriber transceiver is given is assigned
    // independently on our own connection. The two agree only by coincidence.
    // Measured against a real SFU: Element's screen share arrived and
    // decrypted — 500+ frames — under our subscriber mid "3", while the tile
    // waited on the publisher's mid, so nothing was ever watching and the
    // share was never drawn.
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
    // `a=msid:<stream-id> <track-id>` — the stream id is the first token.
    QString streamId = msid.section(QLatin1Char(' '), 0, 0).trimmed();
    // ...and LiveKit PACKS TWO IDS INTO THAT STREAM ID.
    //
    // The server writes `PackStreamID(publisherID, trackID)` —
    // "<PA_participantSid>|<TR_trackId>" — for every client whose protocol
    // version supports it, and `SupportsPackedStreamId()` is literally
    // `v > 0`, so that is every client that is not speaking protocol 0.
    // Taking the whole token produced a stream id matching NOTHING we hold:
    // a media key is installed against a participant and a video sink is
    // attached against one, so a remote participant's frames were dropped
    // for want of a key AND their video was routed to no surface at all.
    // Splitting on '|' and keeping the participant sid is exactly
    // livekit-client's own `unpackStreamId()`.
    //
    // `>= 0`, not `> 0`: a LEADING separator names no participant, and the
    // honest answer there is empty (which routes and decrypts nothing)
    // rather than the confident wrong "|TR_...". The reference splits
    // unconditionally and yields "" for that input too.
    const int packed = streamId.indexOf(QLatin1Char('|'));
    if (packed >= 0)
        streamId.truncate(packed);
    return streamId;
}

QString SfuMediaEngine::localCameraStreamId()
{
    // Same shape and the same reasoning as localScreenStreamId(): a colon
    // means it can never collide with a LiveKit sid.
    return QStringLiteral("local:camera");
}

QString SfuMediaEngine::localScreenStreamId()
{
    // Not a LiveKit sid and deliberately shaped so it can never collide with
    // one: LiveKit ids are "PA_..."/"TR_..." and this has a colon in it.
    return QStringLiteral("local:screen");
}

QString SfuMediaEngine::screenShareSource(int nodeId, int pipewireFd)
{
    // `fd` FIRST: the portal's remote is where this node lives, and
    // pipewiresrc resolves `path` against whichever remote it was given.
    // Without the fd it looks in the caller's own default remote, finds
    // nothing, and plays happily forever without emitting a buffer.
    // NOTE ON `min-buffers`, so it is not tried again blind.
    //
    // The capture was observed delivering exactly ONE frame and stopping
    // (`capture delivered frames count= 1`, never a second) while `videorate`
    // repeated it into thousands of encoded frames. pipewiresrc's pool size
    // looked like the cause — its `min-buffers` defaults to 1, and this
    // pipeline holds several buffers downstream. Raising it to 8 was tried and
    // made things WORSE: not one frame arrived. So the pool size is NOT the
    // fault, and the default is restored here deliberately.
    // NOTE ON `keepalive-time`, so it is not tried again blind.
    //
    // The ~1s frozen first picture at the receiver is `videorate`: measured in
    // this repo's dev shell against exactly this caps shape (input
    // framerate=(fraction)0/1, output pinned 30/1) it emits NOTHING for the
    // first buffer — it holds it until a SECOND arrives — then back-fills the
    // gap in one sub-millisecond burst of duplicates timestamped across it. A
    // PipeWire desktop capture delivers ON DAMAGE, so that gap is "how long
    // until the screen moves", and a libwebrtc receiver renders on the frame
    // timeline.
    //
    // `pipewiresrc keepalive-time=100` looked like the answer: re-push the
    // buffer the element already holds, touching no caps, no pool and no
    // negotiation. It was shipped on that reasoning and it made things
    // STRICTLY WORSE — the share froze on its FIRST frame and never
    // recovered, and the local self-view (tee'd off the capture, so it proves
    // the capture and not the network) sat on "Waiting for the picture".
    // Reverted. This is the SECOND property tried here on reasoning alone and
    // the second to kill the capture; the first was `min-buffers` below.
    //
    // The hold remains OPEN. A downstream `compositor` was tried as the rate
    // stage and had to be reverted for cropping the capture to a quarter of
    // the screen (videoRateStage()), so nothing has yet fixed it without
    // breaking something else. What stands: a fix must NOT reach into the
    // source — both properties above failed exactly because they did — and it
    // needs a live measurement, which `publish first encoded frame ...
    // rateStageHoldMs=` (publishVideo) provides.
    if (pipewireFd >= 0) {
        return QStringLiteral("pipewiresrc fd=%1 path=%2 do-timestamp=true")
            .arg(pipewireFd).arg(nodeId);
    }
    return QStringLiteral("pipewiresrc path=%1 do-timestamp=true").arg(nodeId);
}

void SfuMediaEngine::publishVideo(const QString &cid, bool screenShare,
                                  int nodeId, int pipewireFd)
{
    // The fd belongs to this engine now. It is held for the LIFETIME of the
    // publishing bin and closed by unpublish(), NOT once the element exists:
    // whether pipewiresrc dups the descriptor it is handed is a detail of the
    // plugin version, and closing a descriptor it did not dup stops the
    // capture dead. Holding it costs one fd; guessing costs the share.
    const auto closeFd = [pipewireFd] {
        if (pipewireFd >= 0)
            ::close(pipewireFd);
    };
    if (!ensurePeer(Target::Publisher) || cid.isEmpty()) {
        closeFd();
        return;
    }
    if (m_publishedBins.contains(cid)) {
        closeFd();
        return;
    }

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
            closeFd();
            Q_EMIT failed(QStringLiteral("screen_share_no_source"));
            return;
        }
        source = screenShareSource(nodeId, pipewireFd);
    } else {
        // `v4l2src`, NOT `autovideosrc`, and the reason is measured.
        //
        // autovideosrc picks by RANK, and on a PipeWire desktop `pipewiresrc`
        // outranks `v4l2src` (primary+1 vs primary), so autodetect always
        // instantiated pipewiresrc — with NO `path` and no `target-object`,
        // because only the screen-share branch has a portal node to give it.
        // On its own that already fails: `autovideosrc ! fakesink` with no
        // caps at all reports "stream error: target not found".
        //
        // It became invisible when the publish caps pinned
        // `framerate=(fraction)30/1`. That pin is what finally made Element
        // render a SCREEN SHARE (interop round, e189b8a) and its whole
        // justification is about a desktop capture negotiating 0/1 — but it
        // was applied to BOTH branches, and the camera never had that
        // problem. Together they produce
        //   stream error: error set output format: -22 (Invalid argument)
        //   streaming stopped, reason not-negotiated (-4)
        // so the bin never prerolls, the capture emits zero buffers, and
        // nothing is ever encoded, encrypted or sent. Element sees a declared
        // camera track carrying nothing. That is why the 2026-08-27 video
        // router fix could not restore the camera: there was never a frame to
        // route.
        //
        // Bisected against the real production pipeline, 3/3 deterministic
        // each way: 30/1 fails; [0/1,30/1] (the pre-e189b8a caps) plays;
        // **[1/1,30/1] ALSO FAILS** — so relaxing the range is NOT the cure,
        // and "put 0/1 back" would restore exactly the variable-rate state
        // the pin exists to prevent. The fragile element is
        // pipewiresrc-without-a-target. The identical description with
        // v4l2src and the 30/1 pin LEFT IN PLACE runs clean, negotiating a
        // genuine 640x480 YUY2 @ 30/1 camera mode.
        //
        // Long-term this should be a node id resolved from a GstDeviceMonitor
        // on Video/Source — that is the only shape that can offer a choice
        // between two cameras, and it is docs/matrixrtc.md's open item 1.
        source = QStringLiteral("v4l2src");
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
    // THE FRAMERATE IS FIXED, NOT A RANGE — and the range was the bug.
    //
    // A desktop capture negotiates `framerate=(fraction)0/1`: PipeWire
    // delivers a buffer when the screen CHANGES, not on a clock. Measured
    // exactly that, straight off the portal:
    //   video/x-raw, format=BGRA, width=3840, height=2160,
    //   framerate=(fraction)0/1, max-framerate=(fraction)59/1
    // A range that INCLUDES 0/1 lets that variable rate negotiate all the way
    // through, so `videorate` has no target to convert to and `vp8enc` is
    // given no rate to plan its bitrate against. Every WebRTC sender encodes
    // at a steady cadence instead; a receiver's jitter buffer and decoder are
    // built for one.
    //
    // Naming a fixed rate here is what `videorate` is FOR: it turns the
    // on-damage source into an even stream, repeating the last picture when
    // the screen is still — which is correct for a screen share and is what
    // the far end expects.
    //
    // The SIZE stays a range: those are ceilings, and videoscale picks the
    // largest fitting size that keeps the display aspect ratio, so an
    // ultrawide (or this 4K source) stays its own shape instead of being
    // stretched into 16:9.
    const QString limits = screenShare
        ? QStringLiteral("video/x-raw,width=(int)[1,1920],"
                         "height=(int)[1,1080],"
                         "framerate=(fraction)30/1")
        : QStringLiteral("video/x-raw,width=(int)[1,1280],"
                         "height=(int)[1,720],"
                         "framerate=(fraction)30/1");

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
    // A screen share carries a SELF-VIEW branch. The tee is placed after the
    // scaler, so the preview costs one extra RGBA convert of an
    // already-downscaled frame rather than a second capture: sharing is the
    // one case where the user genuinely cannot tell from their own screen
    // whether anything is being sent. `max-buffers=1 drop=true` keeps a slow
    // preview from becoming latency on the branch that is actually published.
    // The CAMERA gets one too. Our own camera is published, never received,
    // so without a self-view branch there is no local video anywhere and the
    // tile can only ever show an avatar — reported as "the camera doesn't
    // work, the PC shows it as in use but I see no preview". Every other call
    // client shows you your own camera, and it is also the only way to tell a
    // dead capture from a working one without asking the far end.
    const QString selfView = !m_testSources
        ? QStringLiteral("t. ! queue max-size-buffers=2 leaky=downstream "
                         "! videoconvert ! video/x-raw,format=RGBA "
                         "! appsink name=selfvidsink emit-signals=true "
                         "sync=false max-buffers=1 drop=true ")
        : QString();
    const QString description = videoPipelineDescription(
        source, videoRateStage(screenShare), limits, encoder, selfView,
        nextPublishSsrc());

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
    if (pipewireFd >= 0)
        m_publishedFds.insert(cid, pipewireFd);
    // The shared record the two probes below write and handlePublishError()
    // reads. Created BEFORE the bin can play, so nothing can be missed.
    auto probeState = std::make_shared<PublishProbeState>();
    probeState->startedMs = monotonicMs();
    probeState->screenShare = screenShare;
    m_publishWatch.insert(cid, PublishWatch{probeState, false});
    // COUNT WHAT THE CAPTURE ITSELF PRODUCES.
    //
    // Every counter we had was downstream of the rate stage, which
    // manufactures frames, so "the encoder is busy" could never distinguish a
    // live capture from a dead one repeating a single picture. It is also the
    // fact handlePublishError() keys on: zero here plus a bus error is a
    // publish that never prerolled, which is a real failure the user is
    // entitled to be told about.
    if (GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capsrc")) {
        if (GstPad *srcPad = gst_element_get_static_pad(capture, "src")) {
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
                        // WHAT THE CAPTURE ACTUALLY NEGOTIATED, once.
                        //
                        // A `memory:DMABuf` feature here is the other classic
                        // reason a PipeWire capture delivers one buffer and
                        // stops: the pipeline downstream cannot map it, and
                        // nothing reports an error. The format and size are
                        // the compositor's, never content.
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
            gst_object_unref(srcPad);
        }
        gst_object_unref(capture);
    }
    if (GstElement *selfSink = gst_bin_get_by_name(GST_BIN(bin),
                                                   "selfvidsink")) {
        auto *ctx = new VideoSinkCtx{this, QString(),
                                     screenShare ? localScreenStreamId()
                                                 : localCameraStreamId()};
        g_signal_connect_data(selfSink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(selfSink);
    }
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "videoenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            // THE ONE MEASUREMENT THAT SETTLES THE OPENING FREEZE.
            //
            // docs/matrixrtc.md asks for exactly this pair before anything on
            // this publish path is changed again: the moment the CAPTURE
            // delivered its first buffer against the moment the first frame
            // was actually ENCODED. The difference is what the rate stage
            // held, and it is the whole of the reported "start a share and it
            // freezes for 5-10 seconds". It could previously only be
            // recovered by eyeballing two log timestamps from different
            // lines, which is why nobody ever did.
            //
            // One shot: it removes itself, so a long share costs nothing.
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
            installEncryptProbe(encoded, /*video=*/true);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    // The link RESULT decides whether there is anything to offer, so it is
    // checked rather than discarded. It used to be thrown away, and a failed
    // link produced an offer with no media section instead of an error.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_publisher.webrtc,
                                                     "sink_%u");
    applyPublisherMsid(sinkPad, cid);
    GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
    if (srcPad && sinkPad)
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "publisher link failed code=" << linked;
        m_publishedBins.remove(cid);
        m_publishWatch.remove(cid);
        releasePublishedFd(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("publish_link_failed"));
        return;
    }
    gst_element_sync_state_with_parent(bin);
    // Now there IS something to offer, so ask for the offer explicitly. The
    // on-negotiation-needed that fired at PLAYING was deliberately ignored.
    ++m_publishedMedia;
    renegotiatePublisher();
}

void SfuMediaEngine::releasePublishedFd(const QString &cid)
{
    const int fd = m_publishedFds.take(cid);
    if (fd > 0)
        ::close(fd);
}

namespace {

/// One deferred publish-bin teardown. Owns a ref on the pipeline it must
/// remove the bin from; the bin itself is kept alive by
/// gst_element_call_async, which refs its element for the call.
struct PublishTeardown {
    SfuMediaEngine *engine = nullptr;
    GstElement *pipeline = nullptr;
    QString cid;
};

void publishTeardownFree(gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    if (ctx->pipeline)
        gst_object_unref(ctx->pipeline);
    delete ctx;
}

/// Runs on a GStreamer thread pool thread, NOT on a streaming thread — which
/// is the entire reason it exists. Safe to change state here.
void publishTeardownAsync(GstElement *bin, gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    // Unparent first so the state change is a standalone element quiescing,
    // with no parent still running to coordinate against.
    gst_bin_remove(GST_BIN(ctx->pipeline), bin);
    gst_element_set_state(bin, GST_STATE_NULL);
    // Back to the GUI thread for anything that touches engine state. marshal()
    // drops it if the engine has since been destroyed.
    // Back to the GUI thread for anything that touches engine state.
    // marshal() drops it if the engine has since been destroyed, and the
    // invocation is tied to the engine as receiver so it dies with the
    // QObject either way.
    SfuMediaEngine *engine = ctx->engine;
    const QString cid = ctx->cid;
    marshal(engine, [engine, cid] {
        // Only once the element is at NULL: the descriptor is what its
        // PipeWire connection rides on.
        engine->noteTeardownComplete(cid);
    });
}

/// The pad is IDLE here: no buffer is in flight through it, so nothing holds
/// the stream lock the state change needs.
GstPadProbeReturn publishTeardownProbe(GstPad *pad, GstPadProbeInfo *,
                                       gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    GstElement *bin = gst_pad_get_parent_element(pad);
    if (!bin) {
        // Nothing to hand ownership to, so free it here. The probe carries NO
        // destroy notify (see below), which makes this branch the only owner.
        publishTeardownFree(ctx);
        return GST_PAD_PROBE_REMOVE;
    }
    if (GstPad *peer = gst_pad_get_peer(pad)) {
        gst_pad_unlink(pad, peer);
        gst_object_unref(peer);
    }
    // OWNERSHIP OF ctx MOVES HERE. gst_pad_add_probe was given no destroy
    // notify on purpose: a probe's notify runs the moment the probe is
    // REMOVED, which is immediately below — it would free the context out
    // from under publishTeardownAsync, which runs later on another thread.
    gst_element_call_async(bin, publishTeardownAsync, ctx,
                           publishTeardownFree);
    gst_object_unref(bin);
    // REMOVE, not OK: a teardown must fire exactly once.
    return GST_PAD_PROBE_REMOVE;
}

} // namespace

void SfuMediaEngine::unpublish(const QString &cid)
{
    // The TAKE comes first, and handlePublishError() depends on that ordering:
    // the errors this bin is about to post as it goes to NULL must not be
    // reportable as a publish failure. Same reason stop() clears the bus sync
    // handler before it destroys the pipelines.
    GstElement *bin = m_publishedBins.take(cid);
    m_publishWatch.remove(cid);
    if (!bin || !m_publisher.pipeline) {
        releasePublishedFd(cid);
        return;
    }
    // The accounting is SYNCHRONOUS even though the teardown below is not.
    // The caller has been told this cid is unpublished and must be able to
    // publish a new one immediately; only the GStreamer wind-down is deferred.
    if (m_publishedMedia.load() > 0)
        --m_publishedMedia;

    // TEARING THIS DOWN SYNCHRONOUSLY DEADLOCKS, and did.
    //
    // `gst_element_set_state(bin, GST_STATE_NULL)` on a bin still inside the
    // PLAYING publisher pipeline, with its streaming thread mid-push, is a
    // lock cycle. Captured from a core dump of exactly this call:
    //
    //   this thread   set_state -> gst_bin_change_state_func
    //                 -> gst_bin_src_pads_activate -> gst_pad_set_active
    //                 -> activate_mode_internal
    //                 -> __pthread_mutex_lock            [wants stream lock]
    //   queue1:src    gst_queue_loop -> vp8enc -> our payloader
    //                 -> gst_pad_chain_data_unchecked
    //                 -> do_probe_callbacks -> g_cond_wait  [holds it]
    //
    // On the GUI thread. That is "stop screen share and my feed stays frozen
    // ... the only way to clear it is rejoin the call", and the proof from
    // the other side was Element playing its share-start jingle the FIRST
    // time and never again: the track was never withdrawn, so re-sharing was
    // not a new share.
    //
    // Two reorderings were tried and BOTH stall at the identical point —
    // unlinking from webrtcbin first, and unparenting first — because
    // neither stops a push already in flight. Do not re-propose them.
    //
    // So: block the pad, then get off this thread.
    //   * GST_PAD_PROBE_TYPE_IDLE fires only when no push is in flight, by
    //     construction. That is what breaks the cycle. It runs inline on THIS
    //     thread if the pad is already idle, otherwise on the streaming
    //     thread once it finishes its buffer.
    //   * The state change then goes through gst_element_call_async, so it
    //     never runs on a streaming thread either — changing an element's
    //     state from within its own streaming thread is its own deadlock.
    // Whole-pipeline teardown (stop()) is untouched and was never affected:
    // setting the PIPELINE to NULL flushes properly.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    if (!srcPad) {
        // No src pad means nothing can be mid-push through it, so the direct
        // path is safe — and is the only one available.
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        releasePublishedFd(cid);
        renegotiatePublisher();
        return;
    }

    auto *ctx = new PublishTeardown{
        this, GST_ELEMENT(gst_object_ref(m_publisher.pipeline)), cid};
    // No destroy notify — publishTeardownProbe hands ctx to
    // gst_element_call_async, which owns it from then on.
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_IDLE, publishTeardownProbe,
                      ctx, nullptr);
    gst_object_unref(srcPad);
}

void SfuMediaEngine::noteTeardownComplete(const QString &cid)
{
    releasePublishedFd(cid);
    // Renegotiated only now. Offering while the bin was still attached would
    // describe a track that is on its way out.
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
void streamIdsFromSdp(GstSDPMessage *message, QHash<int, QString> *streams,
                      QHash<int, QString> *mids)
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
                // "<stream-id> <track-id>"; the stream id is what attributes
                // the track to a SENDER — one per participant, so it cannot
                // tell that participant's camera from their screen share.
                streamId = SfuMediaEngine::participantIdFromMsid(value);
            } else if (key == QLatin1String("mid")) {
                // The section's own id, which LiveKit also states on every
                // TrackInfo it publishes (`mid`, tag 12). That pairing is
                // what lets a receiver route a SECOND video track from the
                // same person to a different surface.
                mid = value.trimmed();
            }
        }
        if (streams)
            streams->insert(static_cast<int>(index), streamId);
        if (mids)
            mids->insert(static_cast<int>(index), mid);
    }
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
    // The SECTION COUNT, never the SDP. An answer with fewer sections than
    // we offered is the SFU refusing a track, which is otherwise invisible:
    // the track stays declared, the far end draws a tile for it, and no
    // media ever flows.
    qCInfo(lcSfuMedia) << "remote description applied kind=" << kind
                       << "target=" << static_cast<int>(target)
                       << "sections=" << gst_sdp_message_medias_len(message);
    // Only the SUBSCRIBER connection carries other people's tracks, so only
    // its description tells us who a received pad belongs to. Recorded
    // BEFORE set-remote-description, because pad-added can fire from inside
    // that call and would otherwise find the map empty.
    if (target == Target::Subscriber) {
        QHash<int, QString> streams;
        QHash<int, QString> mids;
        streamIdsFromSdp(message, &streams, &mids);
        noteStreamIds(streams, mids);
    }

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

void SfuMediaEngine::setMicrophoneGain(int percent)
{
    const int clamped = percent < 0 ? 0 : (percent > 200 ? 200 : percent);
    m_microphoneGain.store(clamped);
    if (!m_publisher.pipeline)
        return;
    const gdouble factor = clamped / 100.0;
    // Matched by the NAME we gave it, exactly as the deafen path is, and for
    // the same reason: `autoaudiosrc` is a bin that may contain a volume
    // element of its own, so a recursive FACTORY match could reach into the
    // capture device instead of our own stage.
    //
    // Every published bin is walked rather than only the audio one, because
    // the engine does not index bins by kind — a video bin simply has no
    // element with this name, so the loop is a no-op there.
    for (auto it = m_publishedBins.cbegin(); it != m_publishedBins.cend();
         ++it) {
        if (GstElement *gain =
                gst_bin_get_by_name(GST_BIN(it.value()), "micvol")) {
            g_object_set(gain, "volume", factor, nullptr);
            gst_object_unref(gain);
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

QString SfuMediaEngine::outputVolumeElementName(const QString &streamId)
{
    // ONE derivation, used by the bin that creates the element and by the
    // lookup that finds it again. They used to disagree — the bin named its
    // element `outvol` and the lookup asked for `outvol_<identity>` — which
    // made per-participant volume a permanent no-op, so the two names now
    // come from the same function and cannot drift.
    //
    // The prefix is load-bearing: deafen matches `outvol` by PREFIX across
    // every receive bin, so a per-stream suffix must not break it.
    //
    // Non-alphanumerics are folded to '_'. A stream id is normally `PA_…`,
    // but the receive path falls back to a `mline:N` synthetic name, and a
    // ':' in an element name inside a gst_parse description is a parse
    // hazard for no benefit.
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
    if (!m_subscriber.pipeline || streamId.isEmpty())
        return;
    // 0..200, not 0..100. Above unity is real amplification — the whole point
    // of the request — and the `volume` element takes a linear factor for
    // which 2.0 is legal. Clamping at 100 here threw away the upper half of
    // every slider.
    const double volume = qBound(0, percent, 200) / 100.0;
    // Each remote audio bin's volume element is named for the STREAM it
    // carries, so one participant can be attenuated without touching anyone
    // else. LOCAL ONLY: it changes nothing for other participants and sends
    // no event of any kind.
    //
    // Recursive, because the element lives inside the per-track bin rather
    // than directly in the pipeline; gst_bin_get_by_name already recurses.
    const QString target = outputVolumeElementName(streamId);
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

void SfuMediaEngine::setInboundKey(const QString &senderName, int index,
                                   const QByteArray &rawKey)
{
    if (senderName.isEmpty())
        return;
    // Created on first sight, so a key can arrive before that sender's
    // track does. The reverse order is handled in pad-added.
    if (!recvCryptorFor(senderName)->setKey(index, rawKey))
        return;
    m_recvKeyReady.store(true);
}

std::shared_ptr<CallFrameCryptor>
SfuMediaEngine::recvCryptorFor(const QString &name)
{
    QMutexLocker lock(&m_recvMutex);
    auto it = m_recvCryptors.constFind(name);
    if (it != m_recvCryptors.cend())
        return it.value();
    // A name we have never seen gets a ring OF ITS OWN rather than sharing
    // anyone else's: decrypting one participant's frames with another's key
    // is silent corruption, and no key at all is an honest drop.
    auto cryptor = std::make_shared<CallFrameCryptor>();
    m_recvCryptors.insert(name, cryptor);
    return cryptor;
}

void SfuMediaEngine::noteParticipantIdentity(const QString &streamId,
                                             const QString &senderName)
{
    if (streamId.isEmpty() || senderName.isEmpty()
        || streamId.size() > 256 || senderName.size() > 512
        || streamId == senderName) {
        return;
    }
    QMutexLocker lock(&m_recvMutex);
    // Make the two names the SAME ring rather than recording a redirection.
    //
    // A ring can already exist under either name — a pad arrives keyed by
    // sid, a media key arrives keyed by identity, and nothing orders those
    // two. A redirection map would leave whichever one was created first
    // stranded: the keys would be in one object and the frames would consult
    // the other. Pointing both names at one shared ring cannot strand
    // anything, in either arrival order.
    auto byName = m_recvCryptors.constFind(senderName);
    auto byStream = m_recvCryptors.constFind(streamId);
    if (byName != m_recvCryptors.cend()
        && byStream != m_recvCryptors.cend()
        && byName.value() == byStream.value()) {
        return;
    }
    // The SENDER-NAMED ring wins where both exist: it is the one a media key
    // was addressed to, so it is the one holding real material.
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
        m_midForMline.clear();
    }
    m_sendKeyReady.store(false);
    m_recvKeyReady.store(false);
}

void SfuMediaEngine::noteStreamIds(const QHash<int, QString> &byMline,
                                   const QHash<int, QString> &midsByMline)
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
    for (auto it = midsByMline.cbegin(); it != midsByMline.cend(); ++it) {
        if (it.value().isEmpty() || it.value().size() > 128)
            m_midForMline.remove(it.key());
        else
            m_midForMline.insert(it.key(), it.value());
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
    ctx->total = &m_framesEncrypted;
    ctx->totalDropped = &m_framesDropped;
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
    // The stream id, NOT a ring: see cryptoProbe(). recvCryptorFor() is still
    // called once here so the ring exists from the moment the track does,
    // which is what lets a key that arrives later land somewhere real.
    ctx->streamId = streamId;
    recvCryptorFor(streamId);
    ctx->encrypting = false;
    ctx->video = video;
    ctx->required = &m_encryptionRequired;
    ctx->keyReady = &m_recvKeyReady;
    ctx->total = &m_framesDecrypted;
    ctx->totalDropped = &m_framesDropped;
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
    if (!tokenIsLive(token, &target)) {
        // A queued callback from a closed session — correct to drop, but it
        // used to be SILENT, and a dropped offer is indistinguishable from
        // an offer that was never created. LiveKit answers a session with no
        // peer connection with JOIN_FAILURE after 60 s, so knowing which of
        // the two happened is the whole diagnosis.
        qCWarning(lcSfuMedia) << "local description dropped: stale session"
                              << "offer=" << offer;
        return;
    }
    qCInfo(lcSfuMedia) << "local description ready offer=" << offer
                       << "target=" << static_cast<int>(target)
                       << "bytes=" << sdp.size();
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

void SfuMediaEngine::handlePublishError(const QString &cid)
{
    // Every discriminator, on the GUI thread, where the state is safe to read.
    if (!m_active)
        return;
    // STILL PUBLISHED? unpublish() takes the cid out of m_publishedBins
    // BEFORE it sets the bin to NULL, and stop() clears the bus sync handler
    // before destroyPeer() tears the pipelines down — so an error posted by
    // an ordinary teardown cannot get this far. That is what makes this safe
    // without a timer, and it is why the two cleanup paths must keep that
    // order.
    if (!m_publishedBins.contains(cid))
        return;
    const auto watch = m_publishWatch.constFind(cid);
    if (watch == m_publishWatch.cend() || !watch->state || watch->reported)
        return;
    // DID THE CAPTURE EVER PRODUCE ANYTHING? A bus error from a bin that has
    // been delivering frames is a transient in something else — a decoder
    // hiccup, a renegotiation — and this engine has no business ending a
    // working track over it. Zero capture buffers is the one shape that
    // cannot be anything but "this publish never started": the bin did not
    // preroll, nothing was encoded, nothing was encrypted, and the far end
    // has a declared track carrying nothing.
    if (watch->state->captured.load() > 0)
        return;
    const bool screenShare = watch->state->screenShare;
    m_publishWatch[cid].reported = true;
    qCWarning(lcSfuMedia)
        << "publish produced no capture buffers and errored; reporting it"
        << "screenShare=" << screenShare;
    Q_EMIT publishFailed(cid, screenShare
                                  ? QStringLiteral("screen_share_failed")
                                  : QStringLiteral("camera_failed"));
}

// ── GStreamer-thread callbacks ──────────────────────────────────────────

void SfuMediaEngine::onPeerStateNotify(GstElement *webrtc, void *paramSpec,
                                       void *userData)
{
    Q_UNUSED(userData);
    auto *spec = static_cast<GParamSpec *>(paramSpec);
    if (!webrtc || !spec || !spec->name)
        return;
    // Enum value only — the property names are GStreamer's own and the
    // values are small integers. Nothing here can carry an address or a
    // credential, unlike the SDP and candidates these states describe.
    gint value = -1;
    g_object_get(webrtc, spec->name, &value, nullptr);
    const gchar *rawName = GST_ELEMENT_NAME(webrtc);
    qCInfo(lcSfuMedia) << "peer" << (rawName ? rawName : "?")
                       << spec->name << "=" << value;
}

void SfuMediaEngine::onNegotiationNeeded(GstElement *webrtc, void *userData)
{
    // The trigger for the entire offer chain. If this never fires, nothing
    // was published and the SFU will time the participant out.
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    // Nothing to offer yet. webrtcbin raises this as soon as it hits PLAYING,
    // before any track is attached, and an offer built then carries no media
    // section — which the SFU reads as a state mismatch against the track we
    // already declared. The offer that matters is the one
    // renegotiatePublisher() makes once a track is really linked.
    const int media = engine->m_publishedMedia.load();
    if (media == 0) {
        qCInfo(lcSfuMedia) << "negotiation needed: deferred, no media yet";
        return;
    }
    qCInfo(lcSfuMedia) << "negotiation needed: offering" << media << "track(s)";
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
    QString trackMid;
    {
        // THE PAD'S OWN msid AND mid, which webrtcbin fills in from the
        // remote description for exactly this purpose.
        //
        // This used to derive a media-section index from the pad NAME
        // (`src_<n>`) and look that up in the SDP. That index is NOT the
        // m-line index: webrtcbin numbers its src pads over the media it
        // actually produces, while LiveKit's subscriber offer carries a DATA
        // CHANNEL in section 0. So `src_0` was read against the data channel,
        // which has no `msid`, and every received track came out
        // UNATTRIBUTED — and an unattributed track gets its own empty key
        // ring, so every frame failed to decrypt no matter how correct the
        // keys were. The remote participant was permanently silent and
        // invisible; measured as `first recv frame streamId= "mline:0"`
        // against a real SFU.
        gchar *padMsid = nullptr;
        g_object_get(srcPad, "msid", &padMsid, nullptr);
        if (padMsid) {
            const QString msid = QString::fromUtf8(padMsid);
            streamId = participantIdFromMsid(msid);
            // The TRACK key is the track SID, not a media-section id. See
            // trackSidFromMsid() for why a mid cannot work here.
            trackMid = trackSidFromMsid(msid);
            g_free(padMsid);
        }
        if (trackMid.isEmpty()) {
            // No track sid in the msid: fall back to the section's own mid so
            // a server that packs its ids differently still routes SOMETHING.
            GstWebRTCRTPTransceiver *transceiver = nullptr;
            g_object_get(srcPad, "transceiver", &transceiver, nullptr);
            if (transceiver) {
                gchar *mid = nullptr;
                g_object_get(transceiver, "mid", &mid, nullptr);
                if (mid) {
                    trackMid = QString::fromUtf8(mid);
                    g_free(mid);
                }
                gst_object_unref(transceiver);
            }
        }
        // Fallback for a pad that carries neither property: match on the
        // section's OWN mid rather than on a positional index, so it can
        // never be indexed by the wrong number again.
        if (streamId.isEmpty() && !trackMid.isEmpty()) {
            QMutexLocker lock(&engine->m_recvMutex);
            for (auto it = engine->m_midForMline.cbegin();
                 it != engine->m_midForMline.cend(); ++it) {
                if (it.value() == trackMid) {
                    streamId = engine->m_streamForMline.value(it.key());
                    break;
                }
            }
        }
        qCInfo(lcSfuMedia) << "received track attributed="
                           << !streamId.isEmpty() << "trackKey=" << trackMid;
        if (streamId.isEmpty()) {
            // Unattributed. Deliberately given its OWN ring rather than
            // folded into a shared one: a frame decrypted with the wrong
            // participant's key is silent corruption, and a ring nobody has
            // keyed simply drops.
            streamId = trackMid.isEmpty()
                ? QStringLiteral("unattributed")
                : QStringLiteral("mid:%1").arg(trackMid);
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
    // The volume element keeps the "outvol" PREFIX so deafen reaches every
    // received track regardless of who sent it, and carries the stream id as
    // a suffix so ONE participant can be turned down without touching anyone
    // else. Before this it was plainly "outvol" in every bin, and
    // setParticipantVolume looked for a name nothing had.
    // Received video goes to an appsink whose samples become QVideoFrames on
    // a QML VideoOutput. RGBA because that maps 1:1 onto
    // QVideoFrameFormat::Format_RGBA8888 with a plain row copy — a planar
    // format would need per-plane strides for no benefit at these sizes.
    //
    // max-buffers=1 drop=true is deliberate: a late video frame is worthless
    // and queueing them turns a slow consumer into growing latency. Audio is
    // never dropped this way.
    // VIDEO RECEIVE IS THE SAME CHAIN IN EVERY MODE.
    //
    // Test-source mode used to end in a fakesink here, which meant the appsink
    // — and therefore the whole route-to-a-surface path — was never exercised
    // by anything except a user's desktop. That is precisely where a remote
    // screen share was being lost. An appsink needs no display, so there is
    // nothing to gain by substituting for it; only the SOURCES need to be
    // synthetic in a headless run, and audio still ends in a fakesink because
    // there is no audio device.
    const QString description = mediaKind == QLatin1String("video")
        ? QStringLiteral("queue ! rtpvp8depay name=recvdepay ! vp8dec "
                         "! videoconvert ! video/x-raw,format=RGBA "
                         "! appsink name=vidsink emit-signals=true "
                         "sync=false max-buffers=1 drop=true")
        : (engine->testSourceMode()
               ? QStringLiteral("queue ! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! fakesink sync=false")
                     .arg(outputVolumeElementName(streamId))
               : QStringLiteral("queue ! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! autoaudiosink")
                     .arg(outputVolumeElementName(streamId)));

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
        auto *ctx = new VideoSinkCtx{engine, trackMid, streamId};
        g_signal_connect_data(appsink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(appsink);
    }
    // Apply the CURRENT deafen state before the bin plays: a track arriving
    // after the user deafened must not be audible even briefly.
    if (GstElement *volume = gst_bin_get_by_name(
            GST_BIN(bin),
            outputVolumeElementName(streamId).toUtf8().constData())) {
        g_object_set(volume, "mute",
                     engine->m_outputMuted.load() ? TRUE : FALSE, nullptr);
        // The per-person LEVEL is deliberately NOT applied here. This code
        // runs on the GStreamer streaming thread and has no access to the
        // settings store; SfuCallController re-applies every stored volume
        // whenever the participant set changes, which is the same event that
        // brings this track into existence. Deafen is different and must be
        // here: it is a hard silence the user has already asked for, and a
        // track that is briefly audible after deafening is a real leak.
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
    marshal(engine, [engine, mediaKind, streamId, trackMid] {
        // The stream id is the sending participant's LiveKit sid, from the
        // subscriber offer's msid; the mid names the individual track. The UI
        // still keys tiles off MatrixRTC membership, which is authoritative
        // for who is PRESENT; this says which of them is actually sending,
        // and on which section.
        Q_EMIT engine->remoteTrackAdded(streamId, trackMid, mediaKind);
    });
}
