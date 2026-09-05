#include "calls/SfuMediaEngine.h"

#include <algorithm>

#include <unistd.h>

#include "calls/CallFrameCryptor.h"
#include "calls/GstBootstrap.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/WindowCaptureSrc.h"
#include "calls/SfuVideoRouter.h"

#include <mutex>

#include <QCoreApplication>
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
    // ONE init for the process, plugin path included — see GstBootstrap.h.
    // This used to do its own gst_init AND set GST_PLUGIN_PATH beside it,
    // which worked only when this engine happened to be probed first. It is
    // not: AppController probes the 1:1 backend before this one, that
    // backend called gst_init with no plugin path, and the second call was a
    // no-op. A packaged build then had the engine compiled in, 25 plugins
    // beside it, and an empty registry.
    const bool initOk = lightning::gst::ensureInitialised(whyNot);
    if (!initOk)
        return false;

    // Our own VP8 payloader, which the probe below then requires like any
    // other element. Registered here because this is the first thing that
    // runs after gst_init and before any pipeline is built.
    lightning::rtp::registerVp8Payloader();
    // Same reason and the same moment: an element we compile in has to be in
    // the registry before any pipeline description can name it.
    lightning::wincap::registerWindowCaptureSrc();
    // Everything the SFU pipelines need EXCEPT A CAPTURE SOURCE, and that
    // exception is deliberate but was previously described as its opposite:
    // this comment used to claim "video and screen capture are included",
    // which is false — no pipewiresrc, v4l2src or ximagesrc has ever been in
    // this list. Requiring one would refuse an AUDIO call on a machine that
    // merely has no camera plugin, which is worse than the honest refusal the
    // share route already makes at the point of use.
    //
    // The cost is real and is the reason this note exists: a bundle whose
    // capture source is missing, or present but unable to initialise, still
    // reports "group call (SFU) engine: available". Pipeline 142's AppImage
    // did exactly that while screen sharing was dead — libpipewire could not
    // load its own SPA plugins — so a green engine probe is NOT evidence that
    // a share can start. Whatever asks that question has to ask it elsewhere.
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
    // Keys can be installed BEFORE start(): a to-device key arrives while the
    // controller is still Preparing, and onSfuJoined is what calls start().
    // A join that is abandoned before then must still drop them, so the
    // clear sits before the never-started early return.
    clearKeys();
    if (!m_active && !m_publisher.pipeline && !m_subscriber.pipeline)
        return;
    m_generation.fetch_add(1);
    m_active = false;
    m_publishedBins.clear();
    m_publishWatch.clear();
    m_pendingTrackVolume.clear();
    m_volumeMissWarned.clear();
    // destroyPeer tears the pipelines down; the descriptors those bins were
    // using are ours to close and would otherwise leak one per screen share
    // per call.
    for (auto it = m_publishedFds.cbegin(); it != m_publishedFds.cend(); ++it) {
        if (it.value() > 0)
            ::close(it.value());
    }
    m_publishedFds.clear();
    m_statsTimer.stop();
    m_lastRtpBytes.clear();
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
    armStatsTrace();
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

/// The element that captures what the COMPUTER is playing, or empty.
///
/// A screen share has two halves and the desktop portal only carries one:
/// xdg-desktop-portal's ScreenCast interface has no audio at all, and WASAPI
/// does not put system audio on the video path either. So share audio is a
/// SECOND capture, chosen per platform.
///
/// CHOSEN AT RUNTIME, NOT AT COMPILE TIME, and the property is checked as
/// well as the element. `gst_parse_launch` fails outright on an unknown
/// property, so naming `loopback` on a build whose wasapi element does not
/// have it would take the whole bin down rather than degrade — and which
/// element a package ships is a packaging fact this code cannot see. The dev
/// shell here is GStreamer 1.26; Windows packages carry 1.28 and macOS 1.28.6,
/// and this lane has already been bitten twice by a property that moved
/// between the version developed against and the version shipped.
static QString shareAudioSourceDescription()
{
    struct Candidate {
        const char *element;
        const char *property;   // must exist, or the parse would fail
        const char *description;
    };
    // Order is preference. wasapi2 is the maintained Windows implementation;
    // wasapi is the older one and is still staged, so it stays as a fallback.
    static const Candidate kCandidates[] = {
#if defined(Q_OS_WIN)
        // MEASURED against the SHIPPED plugin, not assumed: the GStreamer
        // 1.28.5 MinGW SDK was installed under Wine and inspected, from an
        // installer whose SHA-256 matches the pin in the packaging repo's
        // Dockerfile byte for byte. Both elements resolve; `loopback` is a
        // Boolean on both, DEFAULT FALSE, so it has to be set explicitly;
        // `low-latency` is a Boolean on wasapi2src. Both are "changeable
        // only in NULL or READY", which is satisfied because they are set at
        // parse time, before the bin is ever brought up.
        //
        // What that does NOT establish is that the capture produces audio on
        // real Windows hardware — Wine answers questions about metadata, not
        // about WASAPI.
        { "wasapi2src", "loopback",
          "wasapi2src loopback=true low-latency=true" },
        { "wasapisrc", "loopback", "wasapisrc loopback=true" },
#elif defined(Q_OS_LINUX)
        // `@DEFAULT_MONITOR@` is resolved by the SERVER, so this follows the
        // user's default sink when they change it mid-call and needs no
        // device enumeration of our own. PipeWire answers it through its
        // PulseAudio compatibility, which every modern desktop runs.
        { "pulsesrc", "device", "pulsesrc device=@DEFAULT_MONITOR@" },
#endif
        { nullptr, nullptr, nullptr },
    };

    // INITIALISE FIRST. gst_element_factory_find() answers "no such element"
    // rather than failing when the registry has never been loaded, so asking
    // before GStreamer is up returns a confident FALSE — and the caller is a
    // UI that hides the switch on a false. In the running app the engine
    // bootstraps long before anyone can open the picker, so this was
    // invisible; a test that asked the question directly got "unsupported"
    // on a machine with pulsesrc installed, and both branches of its
    // assertion then held vacuously. Safe to call repeatedly and in any
    // order, which is exactly why it belongs here rather than at one call
    // site that happens to be first today.
    lightning::gst::ensureInitialised();

    for (const Candidate *c = kCandidates; c->element; ++c) {
        GstElementFactory *factory = gst_element_factory_find(c->element);
        if (!factory)
            continue;
        GstElement *probe = gst_element_factory_create(factory, nullptr);
        gst_object_unref(factory);
        if (!probe)
            continue;
        const bool hasProp =
            g_object_class_find_property(G_OBJECT_GET_CLASS(probe),
                                         c->property) != nullptr;
        gst_object_unref(probe);
        if (!hasProp) {
            qCInfo(lcSfuMedia) << "share audio: element" << c->element
                               << "has no property" << c->property
                               << "— skipping it rather than failing the bin";
            continue;
        }
        return QString::fromLatin1(c->description);
    }
    return QString();
}

bool SfuMediaEngine::shareAudioAvailable()
{
    return !shareAudioSourceDescription().isEmpty();
}

void SfuMediaEngine::setShareQuality(int maxHeight, int fps)
{
    // Snapped, never trusted: these cross from settings and a bad value
    // would reach the caps string. A ceiling of 0 negotiates nothing at all.
    m_shareMaxHeight = (maxHeight <= 900) ? 720
                     : (maxHeight <= 1260) ? 1080
                     : (maxHeight <= 1800) ? 1440
                                           : 2160;
    m_shareFps = (fps <= 22) ? 15 : (fps <= 45) ? 30 : 60;
}

void SfuMediaEngine::publishShareAudio(const QString &cid)
{
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    const QString source = m_testSources
        ? QStringLiteral(
              "audiotestsrc is-live=true wave=sine freq=220 volume=0.05")
        : shareAudioSourceDescription();
    if (source.isEmpty()) {
        // An honest refusal, not a broken bin. The caller has already told
        // the SFU a track is coming, so it must hear about this.
        qCWarning(lcSfuMedia)
            << "share audio: no loopback capture element on this platform";
        Q_EMIT failed(QStringLiteral("share_audio_unavailable"));
        return;
    }

    // DELIBERATELY NOT THE MICROPHONE CHAIN, in three ways.
    //
    //  * NO `webrtcdsp`. Its gain control and noise suppression exist to make
    //    a voice intelligible; run over music or game audio they pump the
    //    level and chew the quiet parts. The mic wants them and this does not.
    //  * STEREO. The mic path pins channels=1 on purpose — voice is mono and
    //    a Windows mic commonly reports two channels with signal in one. A
    //    desktop mix is genuinely stereo and downmixing it would be a defect,
    //    so this pins 2 rather than leaving the device to decide.
    //  * MUSIC-GRADE OPUS. `audio-type=generic` and 128 kbit/s: opusenc's
    //    default is voice-tuned at 64 kbit/s mono, which is audibly wrong on
    //    a music bed.
    const QString description =
        QStringLiteral("%1 ! queue ! audioconvert ! audioresample "
                       "! audio/x-raw,channels=2,rate=48000 "
                       // Its own valve. Muting the share's audio must not
                       // touch the microphone, and vice versa — they are two
                       // tracks and the user thinks of them as two things.
                       "! valve name=sharevalve drop=false "
                       "! opusenc name=shareaudioenc audio-type=generic "
                       "bitrate=128000 "
                       "! rtpopuspay pt=111 ssrc=%2 "
                       // Same reasoning as the microphone bin: the caps
                       // webrtcbin READS to build the m= section, so the ssrc
                       // has to be stated or the offer carries no a=ssrc and
                       // the SFU cannot attribute the RTP to a transceiver.
                       "! capsfilter caps=\"application/x-rtp,media=audio,"
                       "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
                       "encoding-params=(string)2,ssrc=(uint)%2\"")
            .arg(source, QString::number(nextPublishSsrc()));

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
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    // Encrypted exactly like every other track: on the ENCODER's src pad,
    // one whole encoded frame, which is the unit LiveKit and Element Call
    // encrypt. A share audio track that skipped this would be the one
    // cleartext stream in an encrypted call.
    if (GstElement *encoder =
            gst_bin_get_by_name(GST_BIN(bin), "shareaudioenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded, /*video=*/false);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
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
        qCWarning(lcSfuMedia) << "share audio link failed code=" << linked;
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    gst_element_sync_state_with_parent(bin);
    qCInfo(lcSfuMedia) << "share audio published";
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
                       // MONO, PINNED. Voice is mono — Opus's rtpmap says
                       // `/2` because RFC 7587 fixes that field, not because
                       // the stream is stereo — and leaving the channel
                       // count unpinned means the capture device decides it.
                       //
                       // That is a real platform difference, not a
                       // theoretical one: a Windows microphone is commonly
                       // exposed by WASAPI as TWO channels with signal only
                       // in the first, so the far end received a stereo
                       // stream whose right channel was silent. Reported as
                       // "I hear you but only in my left ear" — and it could
                       // not reproduce on Linux, where the source hands over
                       // one channel to begin with.
                       //
                       // `audioconvert` above does the downmix, so a genuine
                       // stereo mic is averaged rather than half-discarded.
                       // A device with signal in one channel only loses 6 dB
                       // to that average, which the DSP's gain control makes
                       // back; being quiet in both ears is the right failure
                       // next to being absent from one.
                       "! audio/x-raw,channels=1 "
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
                 QString::number(
                     audioFactorPercent(m_microphoneGain.load()) / 100.0,
                     'f', 3),
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

/// The share's caps ceiling, as a string, for a chosen height and rate.
///
/// STATIC AND PURE so it can be tested. Its neighbours videoPipelineDescription()
/// and videoRateStage() are static for the same reason, and the record on this
/// lane is that all three caps defects it has had were invisible to every test
/// that existed at the time — because the string was only ever built inside a
/// function that needs a live peer.
/// The convert-and-scale segment for a screen share: CPU by default, GPU
/// when opted into.
///
/// WHY THERE IS A GPU VARIANT AT ALL. The default segment is
/// `videoconvert ! videoscale`, and the capsfilter above it asks for plain
/// `video/x-raw` — system memory. A compositor's capture buffer lives on the
/// GPU, so that request forces a READBACK of every frame at CAPTURE size
/// (3840x2160 here, ~33 MB) before anything is scaled. A readback is cheap in
/// CPU time and expensive in stalls: the GPU must finish and hand the buffer
/// back before the compositor continues, which is how a desktop stutters
/// while the CPU looks idle. It also explains why choosing a lower resolution
/// changes nothing — the readback is at capture size whatever the ceiling is.
///
/// The GPU variant imports the buffer instead (`glupload` takes a DMA-BUF
/// zero-copy), scales it on the GPU, and downloads the SMALLER frame.
///
/// STATE, 2026-08-30: BLOCKED BELOW US. Off by default. Do not re-litigate
/// any of the four points below — each one is measured, not argued.
///
///  1. THE READBACK IS REAL AND THIS REMOVES IT. With the `(ANY)` entry
///     filter the capture negotiates `video/x-raw(memory:DMABuf)` instead of
///     system-memory BGRA. That was the whole hypothesis for a desktop that
///     stutters at low CPU load, and it is demonstrated.
///  2. THE GL CHAIN IS CORRECT. `glcolorconvert ! glcolorscale ! gldownload`
///     fed GL memory directly writes a full 235 KB PNG. It carries pixels.
///  3. THE IMPORT IS WHAT COMES BACK EMPTY. Fed the compositor's buffer the
///     same chain produces black while frames flow and the encoder is fed.
///  4. AND THE COMPOSITOR WILL NOT HAND OVER ANYTHING ELSE. Asking for
///     `drm-format=AR24:0x0` (DRM_FORMAT_MOD_LINEAR) FIRST changes nothing:
///     KWin on this NVIDIA stack still answers
///     `AR24:0x0300000000606014` — block-linear — and importing that as an
///     EGLImage yields an empty texture on driver 595.71.05.
///
/// So the blocker is the NVIDIA/KWin/glupload combination, not a pipeline
/// string, and no rearrangement of these elements fixes it. What might: a
/// CUDA import path (`cudaupload` is present on this machine) or NVENC
/// (`nvh264enc`), which would sidestep EGLImage entirely — at the cost of
/// H.264, and this lane's payloader is VP8-specific for a recorded reason.
/// Retry the GL route when the driver or GStreamer moves; the flag and its
/// tests are left in place so that is a one-line experiment rather than a
/// re-investigation.
///
/// SETTLED: the import works. With the `(ANY)` entry filter the capture
/// negotiates `video/x-raw(memory:DMABuf), drm-format=AR24:0x03000000006060
/// 14, format=DMA_DRM` instead of plain system-memory BGRA. That is the
/// readback gone, which was the entire hypothesis, and it is measured rather
/// than argued.
///
/// THE BLACK SHARE IS FIXED, AND THE CAUSE WAS IN OUR OWN CAPSFILTER — not
/// where the paragraph that used to sit here said it was. That paragraph
/// blamed `GST_MESSAGE_NEED_CONTEXT`: no answer on the bus, so `glupload`
/// invents a GstGLDisplay that cannot sample a modifier'd buffer. Plausible,
/// and WRONG, and it is kept named here because it would have cost somebody
/// a week of bus plumbing for a one-line caps bug.
///
/// What was actually happening: `captureEntryFilter()` offered a LINEAR
/// DMA-BUF first and `video/x-raw(ANY)` as a fallback. `(ANY)` matches every
/// caps feature INCLUDING memory:DMABuf at any modifier, so the linear entry
/// was only ever a PREFERENCE and the compositor was free to decline it and
/// take the second — which it did, answering
/// `drm-format=AR24:0x0300000000606014`, NVIDIA block-linear, which imports
/// as an empty texture. Frames flowed, the encoder ran, every pixel was
/// black. The fallback is now plain `video/x-raw`, so the choice is a linear
/// DMA-BUF the chain can import or system memory, never a tiled buffer.
///
/// GENERALISE, because this lane keeps relearning it: a field you PREFER is
/// not a field you CONSTRAIN, and two of this file's own tests were pinning
/// the preference — one requiring linear to come first, the other requiring
/// the fallback that let the peer skip it.
///
/// EARLIER, AND NOW FIXED — kept because it explains the shape of the code:
///
///     screen share scaling on the GPU (opt-in)
///     pipeline error element="capsrc" reason="stream error: no more input
///     formats"
///
/// and the share never starts. The reason is upstream of this function and
/// is the thing to fix before trying again: `videoPipelineDescription()`
/// puts `capsfilter caps="video/x-raw,pixel-aspect-ratio=1/1"` BETWEEN the
/// source and this segment. Plain `video/x-raw` is SYSTEM MEMORY, so
/// pipewiresrc is asked to hand over a downloaded buffer before `glupload`
/// ever sees it — the import can never happen, and the intersection then
/// fails outright.
///
/// So the next attempt is not a different GL element: it is moving that PAR
/// pin. And that is delicate, because the pin exists for a recorded reason —
/// a PAR left unfixated is taken to its MINIMUM by gst_caps_fixate,
/// 1/2147483647, which kills videoscale with an integer overflow. Any rework
/// has to keep a fixed PAR in front of the source while letting the memory
/// feature through.
///
/// LIVE-VALIDATED ON LINUX and now the default there; see
/// shareGpuScalingRequested() for the platform gate and the ladder. It is
/// not the default because it cannot be measured here: a probe built on
/// `videotestsrc` produces system memory, so `glupload` has to upload 4K
/// frames it would otherwise import for free — measured 2.39 s against the
/// CPU path's 1.50 s, which is the opposite of what a real capture would
/// show and exactly the "a probe must share the property under test" trap.
/// Three GStreamer properties have shipped on this lane on reasoning alone
/// and every one made things worse; this one waits for a number from a real
/// share.
bool SfuMediaEngine::shareGpuScalingRequested()
{
    // ON EVERYWHERE, with `LIGHTNING_SHARE_GPU=0` to force the CPU.
    //
    // It was Linux-only while unproven; it is now confirmed working on a
    // packaged Windows build (`screen share scaling on the GPU`, a game
    // holding 225 of 240 fps, and the capture feeding the encoder 1:1
    // instead of videorate tripling every frame). What makes the default
    // safe is not that measurement but the LADDER underneath it: the
    // elements are probed, the chain is proved to reach PAUSED, and a
    // description that still will not build falls back once. A machine that
    // cannot do any of that gets the CPU path and a log line saying which
    // step declined.
    return qEnvironmentVariable("LIGHTNING_SHARE_GPU")
           != QLatin1String("0");
}

/// The first missing GL element, or empty when the chain can be built.
///
/// A FACTORY PROBE, NOT A GUESS. `gst_parse_bin_from_description` fails
/// outright on an unknown element, so on a build that did not stage
/// libgstopengl the GPU description would take the whole share down rather
/// than degrade. Asking the registry first turns that into a fallback, and
/// naming the element turns "the GPU path is unavailable" into something a
/// packaging bug can actually be found from.
QString SfuMediaEngine::missingGpuShareElement()
{
    // Exactly the elements shareScaleStage()'s GPU branch names. Kept
    // beside it deliberately: a chain that gains an element and not an
    // entry here would go back to failing the whole share.
    return firstMissingElement({QByteArrayLiteral("glupload"),
                                QByteArrayLiteral("glcolorconvert"),
                                QByteArrayLiteral("glcolorscale"),
                                QByteArrayLiteral("gldownload")});
}

/// Whether the GPU chain can reach PAUSED here. Cached; see the header.
bool SfuMediaEngine::jpegCameraChainAvailable()
{
    static const bool available = [] {
        // ONLY asks whether the DECODER exists and links. It deliberately
        // does NOT ask whether any camera offers MJPG — that is per-device
        // and per-driver, and the pipeline itself answers it by failing to
        // negotiate, at which point the raw fallback takes over.
        //
        // `jpegenc ! ` in front so the chain under test actually carries
        // image/jpeg, rather than a raw source that would link past the
        // capsfilter and prove nothing — the probe-must-share-the-property
        // rule this lane has learned repeatedly.
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
        // The REAL scale stage, not an approximation of it — a probe that
        // does not share the property under test proves nothing, which this
        // lane has now learned four separate times. videotestsrc feeds it so
        // no capture, portal or permission is involved: what is being asked
        // is whether GL works, not whether a desktop can be grabbed.
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
        // BOUNDED, and on the GUI thread. A GL context that cannot be created
        // fails fast; a driver that hangs must not take the call with it, so
        // an inconclusive answer counts as unusable rather than as a wait.
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

/// The first of `names` with no registered factory, or empty.
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

/// The capsfilter that sits between the capture and everything after it.
///
/// THE PAR PIN IS NOT OPTIONAL. A pixel-aspect-ratio left unfixated is taken
/// to its MINIMUM by gst_caps_fixate — 1/2147483647 — and videoscale then
/// dies of integer overflow. That is recorded, and it is why a fixed value
/// sits in front of the source rather than only downstream.
///
/// But plain `video/x-raw` also pins the MEMORY to system, which is what
/// stopped the GPU path from ever importing the compositor's buffer and made
/// pipewiresrc report "no more input formats". `(ANY)` is the caps-features
/// wildcard: same PAR pin, any memory. Verified to parse and run end to end
/// before being wired in.
QString SfuMediaEngine::captureEntryFilter(bool gpu)
{
    // LINEAR FIRST, THEN ANYTHING. Caps are an ordered preference list and
    // negotiation takes the first structure both ends accept, so this asks
    // the compositor for an untiled DMA-BUF and falls back to whatever it
    // has if it cannot.
    //
    // WHY: the import, not the chain, is what produced a black share.
    // Measured — the GL chain (glcolorconvert ! glcolorscale ! gldownload)
    // fed GL memory directly writes a full 235 KB PNG, so it carries pixels
    // fine; fed the compositor's buffer it carries none. That buffer arrives
    // as `drm-format=AR24:0x0300000000606014`, an NVIDIA BLOCK-LINEAR
    // modifier, and importing one of those as an EGLImage is what comes back
    // empty on this driver. `0x0` is DRM_FORMAT_MOD_LINEAR, which every
    // importer can sample.
    //
    // The compositor detiles to satisfy it, which is a GPU blit — still far
    // cheaper than the full-frame readback this whole path exists to remove.
    // THE FALLBACK MUST NOT BE `video/x-raw(ANY)`, AND THIS WAS THE BUG.
    // `(ANY)` matches every caps feature INCLUDING memory:DMABuf with any
    // modifier, so listing linear first expressed a PREFERENCE the fallback
    // then silently defeated: the compositor answered
    // `drm-format=AR24:0x0300000000606014` — NVIDIA block-linear — and the
    // share went out black while every counter stayed healthy. A field you
    // prefer is not a field you constrain, which is the same trap as the
    // fixate rules above.
    //
    // So the choice is now genuinely binary: a LINEAR DMA-BUF, which the GL
    // chain can import, or plain SYSTEM MEMORY, which is the CPU path we
    // already know works. A compositor that will only produce block-linear
    // now falls back instead of producing a black picture, which is the
    // honest failure and costs nothing that was working before.
    return gpu
        ? QStringLiteral("capsfilter caps=\"video/x-raw(memory:DMABuf),"
                         "drm-format=(string)AR24:0x0000000000000000,"
                         "pixel-aspect-ratio=(fraction)1/1;"
                         "video/x-raw,"
                         "pixel-aspect-ratio=(fraction)1/1\"")
        : QStringLiteral("capsfilter caps=\"video/x-raw,"
                         "pixel-aspect-ratio=(fraction)1/1\"");
}

QString SfuMediaEngine::cameraJpegEntry()
{
    // image/jpeg -> jpegdec -> raw, then the SAME pixel-aspect-ratio pin the
    // raw entry applies. The PAR filter has to stay: a source that fixates no
    // PAR of its own hands videoscale a range whose minimum is 1/2147483647,
    // and that is an integer overflow rather than a squashed picture (§16).
    //
    // `videoconvert` after the decoder because jpegdec emits I420 or one of
    // a few YUV layouts depending on the file, and the scale stage downstream
    // is entitled to want something else.
    return QStringLiteral(
        "capsfilter caps=\"image/jpeg\" "
        "! jpegdec "
        "! videoconvert "
        "! capsfilter caps=\"video/x-raw,"
        "pixel-aspect-ratio=(fraction)1/1\"");
}

QString SfuMediaEngine::shareScaleStage(int maxHeight, bool gpu)
{
    // The FLAG IS A PARAMETER so both branches are testable. Reading the
    // environment inside would make the GPU string unreachable from a test,
    // and a malformed GL caps string then fails for the first person to opt
    // in rather than in CI.
    if (!gpu) {
        // ONE PASS AND THREADED, and both halves are measured rather than
        // assumed. 4K -> 1080p, 5 s of video, this machine:
        //
        //   videoconvert ! videoscale        3.66 s wall   3.64 s CPU
        //   videoconvertscale                3.29 s wall   3.26 s CPU
        //   videoconvertscale n-threads=4    1.65 s wall   3.62 s CPU
        //   videoconvertscale n-threads=8    1.42 s wall   4.10 s CPU
        //
        // The single pass is a free ~10% of TOTAL CPU: one walk over the
        // pixels instead of two. The threads buy something different and
        // more important — the same total CPU spread over cores, so WALL
        // time halves. That is what decides whether the stage keeps up:
        // 5 s of video has to be converted in under 5 s, and at 3.66 s the
        // convert alone was taking 73% of realtime before the encoder's
        // ~2.35 s had even started. Serially that is ~6 s of work per 5 s
        // of video, so frames back up and the share stutters.
        //
        // 4 rather than 8: 8 shaves another 0.23 s of wall and costs 13%
        // MORE total CPU, which is the wrong trade on a machine that is
        // also running whatever the user is sharing.
        return QStringLiteral("videoconvertscale n-threads=4");
    }

    // The size has to be constrained INSIDE the GL segment, or glcolorscale
    // has no target and passes through — leaving the scale to happen after
    // the download, which is the cost this exists to avoid. Ranges, so the
    // "never upscale" property is unchanged.
    const int h = maxHeight;
    const int w = (h * 16) / 9;
    // `glcolorconvert` IS NOT OPTIONAL, and leaving it out is why the first
    // working negotiation produced a BLACK picture. The portal hands over
    // `format=DMA_DRM` (measured: `drm-format=AR24`), which is an opaque
    // DRM-modifier buffer rather than a sampleable colour format —
    // glcolorscale has nothing it can filter and the result is empty. The
    // convert turns it into RGBA in GL first, which is where the whole
    // point of this path lives: it happens on the GPU, on the imported
    // buffer, with no download.
    // `texture-target=2D` PINNED ON THE CONVERT'S OUTPUT, and this is the
    // part that is easy to leave out. A DMA-BUF imported as an EGLImage
    // arrives as GL_TEXTURE_EXTERNAL_OES, not a normal 2D texture — the
    // caps field is `texture-target`, values 2D / rectangle / external-oes.
    // Without pinning it, glcolorconvert is free to pass the external-oes
    // texture straight through, and glcolorscale then samples it as if it
    // were 2D and reads nothing. Frames flow, the encoder runs, the picture
    // is black — exactly the symptom.
    //
    // Pinning it makes the convert do the external-oes -> 2D step, which is
    // the one thing that has to happen after an EGLImage import and before
    // anything tries to filter the texture.
    return QStringLiteral("glupload ! glcolorconvert "
                          "! video/x-raw(memory:GLMemory),format=RGBA,"
                          "texture-target=2D "
                          "! glcolorscale "
                          "! video/x-raw(memory:GLMemory),"
                          "width=(int)[1,%1],height=(int)[1,%2] "
                          "! gldownload ! videoconvert")
        .arg(QString::number(w), QString::number(h));
}

QString SfuMediaEngine::shareLimitsCaps(int maxHeight, int fps)
{
    // Width follows the height at 16:9, and BOTH STAY RANGES so videoscale
    // still refuses to upscale: a 1280x800 window publishes at its own size
    // under a 1440 ceiling. `pixel-aspect-ratio` stays FIXED at 1/1, because
    // a field left un-fixated is taken to its minimum by gst_caps_fixate —
    // for PAR that is 1/2147483647, which kills videoscale with an integer
    // overflow.
    //
    // 60 fps ABOVE THE SOURCE'S DECLARED max-framerate IS FINE, measured
    // rather than reasoned: the portal's real caps are
    // `framerate=0/1, max-framerate=59/1`, and a fixed 60/1 behind
    // `videorate` negotiates cleanly — videorate's src pad comes out at
    // `framerate=(fraction)60/1` with `max-framerate=59/1` still alongside
    // it and nothing reports not-negotiated. Checked because picking 60
    // would otherwise have broken sharing outright, and three caps
    // properties have shipped on this lane on reasoning alone and each made
    // things worse.
    const int shareH = maxHeight;
    const int shareW = (shareH * 16) / 9;
    return QStringLiteral("video/x-raw,width=(int)[1,%1],"
                          "height=(int)[1,%2],"
                          "framerate=(fraction)%3/1,"
                          "pixel-aspect-ratio=(fraction)1/1")
        .arg(QString::number(shareW), QString::number(shareH),
             QString::number(fps));
}

/// The share's encoder stage, whose bitrate follows the picture and whose
/// keyframe interval follows the rate. Static and pure, for the same reason.
QString SfuMediaEngine::shareEncoderStage(int maxHeight, int fps)
{
    const int shareH = maxHeight;
    const int shareW = (shareH * 16) / 9;
    const double shareScale = (double(shareW) * shareH * fps)
                            / (1920.0 * 1080.0 * 30.0);
    const int shareBitrate =
        std::clamp(int(3000000.0 * shareScale), 800000, 6000000);
    return QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                          "cpu-used=4 static-threshold=100 "
                          "keyframe-max-dist=%2 "
                          "end-usage=cbr target-bitrate=%1")
        .arg(QString::number(shareBitrate), QString::number(2 * fps));
}

/// The CPU stage a publish falls back to. See the header.
QString SfuMediaEngine::cpuFallbackScaleStage(bool screenShare,
                                              int shareMaxHeight)
{
    return screenShare ? shareScaleStage(shareMaxHeight, false)
                       : QStringLiteral("videoconvert ! videoscale");
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
    // Refuted and not to be retried AGAINST THE HOLD: `queue
    // min-threshold-buffers=0` and `identity` in front of videorate (neither
    // can manufacture the second buffer); `capssetter`
    // (gst_util_fraction_multiply CRITICAL, zero frames out);
    // `max-duplication-time`; and the two SOURCE-level properties
    // `min-buffers=8` and `keepalive-time=100`, each of which killed the
    // capture outright.
    //
    // `skip-to-first` IS ON THAT LIST AND IS NOW SET ANYWAY, because it was
    // tried against the wrong problem. It does nothing for the hold — videorate
    // still needs a second buffer to know the interval — and it is the whole
    // of a DIFFERENT defect: the one that froze the camera on a single frame.
    //
    // videorate starts its output clock at SEGMENT START, not at the first
    // buffer's timestamp (gst_video_rate_compute_next_ts). A publish bin is
    // added to a publisher pipeline that has been PLAYING since the call was
    // joined, and `ksvideosrc` and `gdiscreencapsrc` both stamp buffers with
    // the pipeline's RUNNING TIME — so the first buffer of a camera switched
    // on three minutes into a call carries a PTS of three minutes, and
    // videorate owes thirty duplicates for every second of it. It emits them
    // as fast as the encoder will take them: a full-rate stream of ONE
    // picture, with every counter healthy. Reported as "camera didnt work, it
    // sent one frame out and froze".
    //
    // MEASURED, not reasoned — every earlier property on this lane was
    // shipped on reasoning and three of them made things worse. appsrc
    // pushing ten buffers at 10 fps into videorate ! 30/1 ! fakesink, on
    // GStreamer 1.26.11:
    //
    //   first PTS      0 s, skip-to-first=false ->    27 buffers out
    //   first PTS     10 s, skip-to-first=false ->   327 buffers out
    //   first PTS     10 s, skip-to-first=true  ->    27 buffers out
    //   first PTS    174 s, skip-to-first=false ->  5247 buffers out
    //   first PTS    174 s, skip-to-first=true  ->    27 buffers out
    //
    // 174 s is the real call age at which the camera was switched on in the
    // Windows log. The corroboration in that log is exact: the monitor share
    // was published 8.73 s into the call and encoded 262 frames more than a
    // 30 fps steady state accounts for, against 30 x 8.73 = 262 predicted.
    //
    // The negative control is Lightning's own WindowCaptureSrc, which stamps
    // from ZERO and therefore never back-filled — which is why a WINDOW share
    // worked while the camera did not, and why that element must keep its
    // zero-based timeline (see the `frameIndex` field there).
    return QStringLiteral("videorate skip-to-first=true");
}

QString SfuMediaEngine::videoPipelineDescription(const QString &source,
                                                const QString &rateStage,
                                                const QString &limits,
                                                const QString &encoder,
                                                const QString &selfView,
                                                quint32 ssrc,
                                                const QString &scaleStage,
                                                const QString &entryFilter)
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
               "%1 name=capsrc "
               // SQUARE PIXELS AT THE SOURCE, and this one is belt and
               // braces for elements we do not own.
               //
               // Pinning the pixel aspect ratio at the CEILING (see `limits`)
               // is what makes videoscale answer a size ceiling by choosing a
               // SIZE rather than by emitting a non-square PAR that VP8 and
               // RTP silently drop. But that pin also makes videoconvertscale
               // offer the SOURCE an open PAR range, and a source that does
               // not fixate PAR itself then falls through to
               // `gst_caps_fixate`, which takes a range's MINIMUM —
               // 1/2147483647 — and negotiation dies of integer overflow.
               //
               // Lightning's own capture element fixates PAR now, and the
               // Windows log proves `gdiscreencapsrc` and `ksvideosrc` both
               // DECLARE `pixel-aspect-ratio=(fraction)1/1` already. But
               // `avfvideosrc` does not appear to, `pipewiresrc` is untested,
               // and none of the three is testable from here. A FIXED value
               // in front of the source is not a range, so it cannot be
               // fixated to a minimum: measured, it rescues a source that
               // fixates no PAR at all, at every size that fails without it.
               "! %9 "
               "! queue max-size-buffers=4 leaky=downstream "
               "! %8 ! %7 "
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
             QLatin1String(lightning::rtp::vp8PayloaderName()), rateStage, scaleStage,
             entryFilter);
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

bool SfuMediaEngine::elementAvailable(const char *name)
{
    if (!name || !*name)
        return false;
    // The registry does not exist until gst_init has run, and asking before
    // it has answers "absent" for EVERY element — which here would silently
    // demote a perfectly capable X11 desktop to "no screen sharing". One
    // entry point, because GST_PLUGIN_PATH is read during gst_init exactly
    // once and whoever gets there first decides what a packaged build can
    // see (GstBootstrap.h).
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
    //
    // WINDOWS AND macOS HAVE NO PORTAL, so `nodeId` means something else
    // there: a MONITOR INDEX rather than a PipeWire node. The signature is
    // shared deliberately — the call controller's plumbing stays
    // platform-blind and the difference lives here, in the one place that
    // actually differs.
#if defined(Q_OS_WIN)
    // gdiscreencapsrc, and NOT by preference. d3d11screencapturesrc is the
    // better element — Desktop Duplication the compositor already has, where
    // GDI is a per-frame BitBlt of the whole desktop and costs real CPU at
    // 4K — but the d3d11 and mediafoundation plugins do not load in this
    // toolchain at all (a mingw-w64 mbstate_t ABI break), so they are not
    // shipped. See lightning-deploy docs/windows-packaging.md; revisit when
    // GStreamer or mingw-w64 is next bumped.
    //
    // Property names verified against the shipped plugin binary rather than
    // assumed: `monitor` ("Which monitor to use (0 = 1st monitor and
    // default)") and `cursor` ("Whether to show mouse cursor"). The d3d11
    // element's `monitor-index`/`show-cursor` are DIFFERENT names, and using
    // them here would be a pipeline that never builds.
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    // A SINGLE WINDOW goes to our own element, because nothing shippable
    // takes one: `gdiscreencapsrc` has `monitor` and a crop rectangle and no
    // window property at all, and `d3d11screencapturesrc`, which does, is in
    // the plugin this toolchain cannot load. Cropping the screen to the
    // window's rectangle was the cheap alternative and is refused on purpose
    // — it would share whatever is stacked on top of that window. See
    // WindowCaptureSrc.h.
    if (windowHandle != 0) {
        return QStringLiteral("%1 hwnd=%2")
            .arg(QLatin1String(lightning::wincap::windowCaptureSrcName()))
            .arg(windowHandle);
    }
    return QStringLiteral("gdiscreencapsrc monitor=%1 cursor=true")
        .arg(nodeId < 0 ? 0 : nodeId);
#elif defined(Q_OS_MACOS)
    // One element does both capture kinds on macOS; `capture-screen` is what
    // switches avfvideosrc from a camera to a display, and `device-index`
    // selects which display.
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    return QStringLiteral("avfvideosrc capture-screen=true "
                          "capture-screen-cursor=true device-index=%1")
        .arg(nodeId < 0 ? 0 : nodeId);
#else
    // NO PORTAL ON THIS SESSION, so Lightning drew the picker itself and what
    // the user chose is a RECTANGLE OF THE X11 ROOT WINDOW, not a node id.
    //
    // Reached only through
    // `SfuCallController::LinuxShareRoute::FallbackDisplays`, which requires
    // an X11 session AND this element in the running registry. It is NOT a
    // Wayland path and must never become one: measured on this repo's own KDE
    // Wayland session, XWayland's root window reports the full desktop extent
    // (7680x2160) and is 99.999% BLACK — 16,588,607 of 16,588,800 pixels are
    // zero, because native Wayland windows never touch it. An ximagesrc
    // pipeline there negotiates perfect 4K caps, runs at a clean 30 fps and
    // sends a black rectangle: healthy counters and nothing shared, which is
    // the exact failure class this file has already been burned by twice.
    //
    // `endx`/`endy` are INCLUSIVE — read off the shipped plugin rather than
    // assumed (§16), then measured: `startx=100 endx=1379` negotiated
    // `width=(int)1280`. Qt's `QRect::right()` is `x + width - 1`, which is
    // that edge exactly, so the famous off-by-one is the CORRECT value here
    // and not a bug for a later reader to "fix".
    //
    // `use-damage=false` is a MEASURED choice, not another blind property.
    // Both modes deliver 30 buffers in 0.967 s in this repo's dev shell, so
    // the default costs nothing to leave — but XDamage on a composited root
    // can report no damage for a window that is in fact being redrawn, and a
    // stale image pushed at full rate is indistinguishable downstream from a
    // live one. A fresh XGetImage per frame removes that class outright at no
    // measured cost in this shape.
    //
    // `do-timestamp=true` mirrors the pipewiresrc line below and is INERT on
    // this element, which is worth recording so nobody chases it: ximagesrc
    // stamps its own PTS, and `GstBaseSrc` only applies do-timestamp to a
    // buffer whose PTS is still NONE. Measured — buffers 2 and 3 come out at
    // EXACTLY 0:00:00.033333333 and 0:00:00.066666666 with the property both
    // on and off, and a wall-clock stamp would jitter instead. That
    // zero-based, frame-counter PTS is the property a source in a bin added
    // to an already-running pipeline must have: a source that stamps pipeline
    // RUNNING time makes videorate back-fill duplicates across the whole call
    // age, which is what froze the Windows camera on one frame.
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
        // `min-buffers=1`, AND IT IS THE DIFFERENCE BETWEEN SHARING AND NOT.
        //
        // gst-plugin-pipewire's DEFAULT_MIN_BUFFERS was 8 up to 1.4.x and is 1
        // from 1.6. The element asks for SPA_PARAM_Buffers as
        // RANGE(default, min-buffers, max-buffers), and a compositor's
        // screencast source caps it low -- KWin 6.6 offers RANGE(3, 2, 4).
        // PipeWire 1.6 added an explicit "reject impossible range" -EINVAL to
        // spa_pod_filter_prop() when the two ranges cannot intersect; 1.4.x had
        // no such check and let it through. So a bundled 1.4.2 element against
        // a 1.6 daemon asks for at least 8 where at most 4 exist, negotiation
        // returns -EINVAL, and the daemon reports it verbatim as
        // "error alloc buffers: Invalid argument" -- which is exactly what the
        // 0.8.1 AppImage did on a KDE desktop while a from-source build on the
        // SAME machine worked, because that one loads the host's 1.6 plugin.
        //
        // Measured on a live 1.6.6 daemon with a real link: 8 -> 0 buffers and
        // that error; 5 -> the same error; 4 -> 4 buffers; 1 -> 3 buffers and
        // STREAMING. Setting it explicitly makes the request version- and
        // compositor-independent instead of inheriting whichever default the
        // bundled plugin happens to carry.
        return QStringLiteral(
                   "pipewiresrc fd=%1 path=%2 min-buffers=1 do-timestamp=true")
            .arg(pipewireFd).arg(nodeId);
    }
    // Same reasoning as the fd branch above: never inherit the plugin's default.
    return QStringLiteral("pipewiresrc path=%1 min-buffers=1 do-timestamp=true")
        .arg(nodeId);
#endif
}

QString SfuMediaEngine::cameraSource()
{
#if defined(Q_OS_WIN)
    // ksvideosrc (Kernel Streaming), and NOT by preference either: mfvideosrc
    // is the more modern path and sees devices KS does not, but the
    // mediafoundation plugin does not load in this toolchain (see
    // screenShareSource above). Kernel Streaming reaches every ordinary UVC
    // webcam, which is what this needs to cover.
    return QStringLiteral("ksvideosrc");
#elif defined(Q_OS_MACOS)
    // The same element the screen branch uses, without capture-screen.
    return QStringLiteral("avfvideosrc");
#else
    // `v4l2src`, NOT `autovideosrc`, and the reason is MEASURED — see the
    // note at the call site. autovideosrc picks by rank, and on a PipeWire
    // desktop pipewiresrc outranks v4l2src, so autodetect instantiated a
    // pipewiresrc with no target and the camera never produced a frame.
    return QStringLiteral("v4l2src");
#endif
}

void SfuMediaEngine::publishVideo(const QString &cid, bool screenShare,
                                  int nodeId, int pipewireFd,
                                  quint64 windowHandle,
                                  const QRect &captureRect)
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
        // A WINDOW handle is a source in its own right, so the node-id
        // refusal below must not reject it. Without this a window share fails
        // as "no source" while holding a perfectly good HWND.
        //
        // ...AND SO IS AN X11 ROOT RECTANGLE, for the same reason. This guard
        // has a TWIN in SfuCallController::startScreenShare and the pair must
        // learn every new source kind together: last time only one of them
        // was taught about windows, and picking a window returned false
        // before a single line was logged ("selecting a window and sharing
        // does nothing"). Three ways of saying "capture this" now, and a
        // refusal is correct only when NONE of them was given.
        if (nodeId < 0 && windowHandle == 0 && !captureRect.isValid()) {
            closeFd();
            Q_EMIT failed(QStringLiteral("screen_share_no_source"));
            return;
        }
        source = screenShareSource(nodeId, pipewireFd, windowHandle,
                                   captureRect);
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
        //
        // The element itself is per-platform (mfvideosrc / avfvideosrc /
        // v4l2src); everything above is the Linux reasoning for why it is not
        // `autovideosrc`, and it is kept because that is the branch it
        // describes.
        source = cameraSource();
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
    // largest fitting size inside them.
    //
    // THE PIXEL ASPECT RATIO IS PINNED TO 1/1, AND WITHOUT IT THE COMMENT
    // THAT USED TO SIT HERE WAS FALSE.
    //
    // It claimed videoscale "picks the largest size inside them that keeps
    // the display aspect ratio, so an ultrawide stays ultrawide instead of
    // being stretched into 16:9". videoscale does keep the display aspect
    // ratio — but it keeps it by clamping BOTH axes to the ceiling and
    // signalling the difference as a non-square PAR, and VP8 carries no PAR
    // and neither does the RTP payload. A libwebrtc receiver draws the frame
    // at its literal width by height, so the ratio videoscale carefully
    // preserved is thrown away at the far end and the picture arrives
    // distorted. Measured, videoscale ! this ceiling ! fakesink:
    //
    //   3840x2160 -> 1920x1080 PAR  1/1    (16:9, so no distortion, which is
    //                                       why nobody has seen this)
    //   3840x2100 -> 1920x1080 PAR 36/35   ->  2.9% stretch
    //   1920x1200 -> 1920x1080 PAR  9/10   -> 11%   stretch
    //   3440x1440 -> 1920x1080 PAR 43/32   -> 34%   squash
    //
    // With the PAR pinned, videoscale has to satisfy the ratio by choosing a
    // SIZE, which is the thing that actually survives to the far end:
    //
    //   3840x2160 -> 1920x1080     3840x2100 -> 1920x1050
    //   3440x1440 -> 1920x 804     1920x1200 -> 1728x1080
    //   1556x1212 -> 1387x1080      800x 600 ->  800x 600 (never upscaled)
    //
    // Every one of those negotiated cleanly from a source declaring no PAR of
    // its own, which is what Lightning's own window capture element does.
    // The share ceiling and rate are the USER'S, not constants. Width
    // follows the height at 16:9 and both remain RANGES, so the "never
    // upscaled" property above is unchanged: a 1280x800 window still
    // publishes at its own size under a 1440 ceiling.
    const QString limits = screenShare
        ? shareLimitsCaps(m_shareMaxHeight, m_shareFps)
        : QStringLiteral("video/x-raw,width=(int)[1,1280],"
                         "height=(int)[1,720],"
                         "framerate=(fraction)30/1,"
                         "pixel-aspect-ratio=(fraction)1/1");
    const QString encoder = screenShare
        ? shareEncoderStage(m_shareMaxHeight, m_shareFps)
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
    const QString scaleStage = screenShare
        ? shareScaleStage(m_shareMaxHeight, shareGpuScalingRequested())
        : QStringLiteral("videoconvert ! videoscale");
    // THE CPU STAGE THIS PUBLISH WOULD FALL BACK TO, and it is per-path on
    // purpose. The camera keeps its own `videoconvert ! videoscale`: the
    // threaded single pass was measured against a 4K DESKTOP, where the
    // convert was 73% of realtime, and a 1280x720 camera is a ninth of
    // those pixels with no such problem. Reaching for the share's stage
    // here would silently re-point an unrelated path at share-sized
    // policy — which is exactly what the first version of this ladder did,
    // because `useGpu` is false for every camera publish.
    const QString cpuScaleStage =
        cpuFallbackScaleStage(screenShare, m_shareMaxHeight);
    // ── GPU FIRST, CPU FALLBACK ──────────────────────────────────────
    //
    // The GPU chain is tried by default and the CPU chain catches it. Two
    // things can go wrong and they need DIFFERENT log lines, because they
    // send whoever reads the capture to different places:
    //
    //   * the elements are not there  -> a PACKAGING problem
    //   * they are there and the description will not build -> a caps or
    //     driver problem on that machine
    //
    // "GPU unavailable" for both would have sent the last three rounds of
    // this lane hunting the driver for what is a missing plugin.
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
    // AND THE ELEMENTS EXISTING IS NOT THE SAME AS THEM WORKING. A driver
    // that cannot give us a GL context, or a headless session with no
    // display to make one on, passes the factory probe above and then fails
    // when the real chain runs. gpuShareChainUsable() builds that chain
    // against a test source once per process and requires PAUSED, so the
    // decision happens BEFORE a capture is opened and a share is committed.
    if (useGpu && !gpuShareChainUsable()) {
        qCInfo(lcSfuMedia)
            << "screen share falling back to the CPU: the GL chain is "
               "present but cannot run on this machine";
        useGpu = false;
    }

    QString scaleStageInUse = useGpu ? scaleStage : cpuScaleStage;

    // A CAMERA GETS THE MJPG CHAIN FIRST. A USB camera advertises image/jpeg
    // beside raw, and MJPG is the only mode that fits 720p30 through USB 2.0
    // — raw YUY2 at 1280x720 is 18.4 MB/s and negotiates down to 10 fps,
    // which is the reported Windows camera defect. The raw entry filter sits
    // directly after the source, so image/jpeg cannot satisfy the first
    // element downstream and no MJPG mode can ever be chosen.
    //
    // Tried FIRST and fallen back from, exactly as the GPU share chain is:
    // a camera with no MJPG mode, or a build with no jpegdec, simply fails to
    // parse or to negotiate and gets today's raw chain. Nothing that works
    // now can stop working.
    const bool tryJpeg = !screenShare && jpegCameraChainAvailable();
    QString entryInUse =
        tryJpeg ? cameraJpegEntry() : captureEntryFilter(useGpu);
    if (!screenShare) {
        // WHICH CHAIN THE CAMERA IS ON, said once, before anything can fail.
        //
        // "capture negotiated caps=" below reports what came out, which is
        // the answer to "what resolution and rate am I sending" — but not to
        // "why". A camera pinned at 10 fps because MJPG could not be
        // negotiated and a camera pinned at 10 fps because the device has no
        // better mode produce the same caps line, and telling them apart
        // cost a round trip with a tester. This says which chain was built.
        qCInfo(lcSfuMedia) << "camera chain="
                           << (tryJpeg ? "mjpg" : "raw")
                           << "(jpeg elements"
                           << (jpegCameraChainAvailable() ? "present"
                                                          : "absent")
                           << ")";
    }
    QString description = videoPipelineDescription(
        source, videoRateStage(screenShare), limits, encoder, selfView,
        nextPublishSsrc(), scaleStageInUse, entryInUse);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error && tryJpeg) {
        // The MJPG description did not build. Say what GStreamer said and
        // rebuild on the raw entry rather than failing the camera.
        qCWarning(lcSfuMedia)
            << "camera MJPG pipeline failed to build, falling back to raw:"
            << (error->message ? error->message : "?");
        g_clear_error(&error);
        if (bin) {
            gst_object_unref(bin);
            bin = nullptr;
        }
        entryInUse = captureEntryFilter(useGpu);
        description = videoPipelineDescription(
            source, videoRateStage(screenShare), limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, entryInUse);
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
    }
    if (error && useGpu) {
        // SECOND CHANCE, ONCE. The GPU description did not build, so say
        // exactly what GStreamer said and rebuild on the CPU rather than
        // failing the share. A tester who reports "sharing broke" then has
        // the reason in the same capture.
        qCWarning(lcSfuMedia)
            << "screen share GPU pipeline failed to build, falling back to "
               "the CPU:" << (error->message ? error->message : "?");
        g_clear_error(&error);
        if (bin) {
            gst_object_unref(bin);
            bin = nullptr;
        }
        useGpu = false;
        scaleStageInUse = cpuScaleStage;
        description = videoPipelineDescription(
            source, videoRateStage(screenShare), limits, encoder, selfView,
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
        // Names the path that actually built, so a capture from a tester
        // says which one produced it without anyone having to ask. Says it
        // AFTER the ladder, or it would report an intention rather than an
        // outcome — which is how the row window once shipped as a no-op.
        qCInfo(lcSfuMedia) << "screen share scaling on"
                           << (useGpu ? "the GPU" : "the CPU");
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
            // AND WHEN THE CAPTURE ENDS ITSELF.
            //
            // Closing the window you are sharing is an ordinary thing to do,
            // and the capture element answers it with EOS rather than an
            // error — correctly, because ending the whole call over it would
            // be worse. But nothing was listening: the encoder stopped, the
            // track stayed declared, and the far end kept the last frame
            // indefinitely while this end still showed "Your screen" with the
            // Stop control armed. Retiring it here routes into exactly the
            // path the Stop button uses, which is the one that sets the
            // transceiver INACTIVE and actually clears the far end's tile.
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
    /// The webrtcbin whose request pad this bin was publishing through, and
    /// that pad. Both refs are owned here and released in the async step —
    /// the pad has to OUTLIVE the unlink so the transceiver can be retired,
    /// which is what tells the far end the track is over.
    GstElement *webrtc = nullptr;
    GstPad *peer = nullptr;
    QString cid;
};

void publishTeardownFree(gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    if (ctx->peer)
        gst_object_unref(ctx->peer);
    if (ctx->webrtc)
        gst_object_unref(ctx->webrtc);
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

    // RETIRE THE TRANSCEIVER, or the far end never learns the track ended.
    //
    // Quiescing our own pipeline is invisible to everyone else: the m= section
    // stays in the SDP, so Element keeps rendering the last frame it got —
    // a frozen picture that only leaving the call clears — and the NEXT share
    // is offered as an ADDITIONAL m-line rather than reusing this one. A live
    // capture of three shares in one session shows exactly that, the answer
    // growing 2 -> 3 -> 4 sections while nothing is ever removed.
    //
    // Releasing the request pad is what marks the transceiver inactive, so
    // the renegotiated offer says the track is gone and webrtcbin can reuse
    // the section for the next publish instead of appending another.
    //
    // The previous commit deliberately did NOT do this, on the reasoning that
    // it would "change the m-line layout" of an offer that took a day to make
    // interoperate. That reasoning was backwards: leaving the section behind
    // is itself the layout change, one that accumulates.
    // The transceiver was already retired synchronously in unpublish() — see
    // the comment there for why it cannot wait for this callback.
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
        this, GST_ELEMENT(gst_object_ref(m_publisher.pipeline)),
        /*webrtc=*/nullptr, /*peer=*/nullptr, cid};
    // RETIRE THE TRANSCEIVER NOW, ON THIS THREAD. Two reasons, and the
    // second is why it cannot ride the deferred teardown below.
    //
    // 1. It is what the far end actually sees. Quiescing our own pipeline is
    //    invisible to everyone else: the m= section stays in the SDP, so the
    //    remote keeps rendering the last frame it got — a frozen picture that
    //    only leaving the call clears — and the NEXT share is offered as an
    //    ADDITIONAL section rather than reusing this one. A live capture of
    //    three shares in one session shows the answer growing 2 -> 3 -> 4
    //    with nothing ever removed.
    //
    // 2. It UNBLOCKS the deferred teardown. The IDLE probe below fires only
    //    when no push is in flight, and a pad pushing into a webrtcbin that
    //    is not draining never becomes idle — measured: the probe did not
    //    fire for the whole three seconds a test waited, then fired during
    //    engine teardown. Unlinking here makes the in-flight push return
    //    NOT_LINKED, the streaming thread unwinds, and the pad goes idle.
    //    Without this the async teardown does not deadlock — it simply never
    //    runs, which is a leak wearing a fix's clothes.
    if (GstPad *peer = gst_pad_get_peer(srcPad)) {
        // DIRECTION FIRST, and this is the line the far end actually obeys.
        //
        // Releasing the request pad drops our track and its msid from the
        // offer, but it does NOT change the transceiver's direction — the
        // section stays `a=sendrecv`. Measured, before and after, on the
        // renegotiated offer:
        //
        //   before   m=video ... | a=sendrecv
        //   after    m=video ... | a=sendrecv        <- msid gone, still sending
        //
        // So the remote is told there is still a video section we send on,
        // with nothing behind it: a live m-line producing no RTP, which is
        // rendered as an empty tile that never goes away ("element keep
        // showing a grey box", "its like the first one doesnt stop"). An m=
        // section may never be REMOVED from an SDP either — the count has to
        // stay stable across renegotiation — so marking it inactive is not
        // merely the tidy option, it is the only correct one.
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
            // Element and pad names only — never a participant, a track or
            // anything captured. This line is how a future run says whether
            // the far end was told.
            qCInfo(lcSfuMedia) << "publish transceiver retired pad="
                               << (padName ? padName : "?");
            g_free(padName);
            gst_object_unref(webrtc);
        }
        gst_object_unref(peer);
    }

    // No destroy notify — publishTeardownProbe hands ctx to
    // gst_element_call_async, which owns it from then on.
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_IDLE, publishTeardownProbe,
                      ctx, nullptr);
    gst_object_unref(srcPad);
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
                // ...and the TRACK sid from the same line. It is what names
                // ONE track on both ends — the frame-crypto key is installed
                // against it, and it is the only thing that tells a
                // participant's camera from their screen share.
                trackSid = SfuMediaEngine::trackSidFromMsid(value);
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
        if (tracks)
            tracks->insert(static_cast<int>(index), trackSid);
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
        QHash<int, QString> tracks;
        streamIdsFromSdp(message, &streams, &mids, &tracks);
        noteStreamIds(streams, mids, tracks);
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
    // Stored on the USER scale; expanded to the audio factor at the two
    // places it meets a `volume` element, here and in the publish
    // description. See audioFactorPercent().
    const int clamped = percent < 0 ? 0 : (percent > 200 ? 200 : percent);
    m_microphoneGain.store(clamped);
    if (!m_publisher.pipeline)
        return;
    const gdouble factor = audioFactorPercent(clamped) / 100.0;
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

int SfuMediaEngine::audioFactorPercent(int userPercent)
{
    const int user = userPercent < 0 ? 0 : (userPercent > 200 ? 200
                                                              : userPercent);
    if (user <= 100)
        return user;
    // 100..200 -> 100..1000, straight line: 9 points of factor per point of
    // slider above unity, so 200 lands exactly on the element's ceiling.
    return 100 + (user - 100) * 9;
}

QString SfuMediaEngine::volumeKeyFor(const QString &streamId,
                                    const QString &trackKey)
{
    // PER TRACK, not per participant — and this became load-bearing the day
    // a screen share started carrying audio. One participant can now publish
    // TWO audio tracks (their microphone and their desktop), and both receive
    // bins used to name their volume element after the same stream id. A
    // lookup is `gst_bin_get_by_name` from the pipeline root, which returns
    // the FIRST match, so setting that person's volume moved whichever bin
    // GStreamer happened to find and left the other one alone.
    //
    // The track key falls back to the stream id when a track cannot be
    // identified, which keeps the old single-track behaviour exactly as it
    // was rather than inventing a name nothing will look up.
    return trackKey.isEmpty() ? streamId
                              : (streamId + QLatin1Char('_') + trackKey);
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
    // The whole participant, which today means every audio track they
    // publish. Kept because the participant-level control is what the tile
    // offers and what has been live-validated; setTrackVolume is the finer
    // grain the share tile needs.
    setTrackVolume(streamId, QString(), percent);
}

void SfuMediaEngine::setTrackVolume(const QString &streamId,
                                    const QString &trackKey, int percent)
{
    if (!m_subscriber.pipeline || streamId.isEmpty())
        return;
    // 0..1000, not 0..100. Above unity is real amplification — the whole
    // point of the request — and the `volume` element takes a linear factor
    // whose own range is 0-10, so 1000% is its ceiling rather than ours.
    // Clamping at 100 here threw away every boost; clamping at 200 left only
    // +6 dB, which against a sender already running AGC reads as "barely any
    // difference".
    const double volume = audioFactorPercent(percent) / 100.0;
    // Each remote audio bin's volume element is named for the STREAM it
    // carries, so one participant can be attenuated without touching anyone
    // else. LOCAL ONLY: it changes nothing for other participants and sends
    // no event of any kind.
    //
    // Recursive, because the element lives inside the per-track bin rather
    // than directly in the pipeline; gst_bin_get_by_name already recurses.
    // What we were looking for, kept for the diagnostic below: a volume that
    // lands nowhere used to be silent, and naming the element it wanted is
    // what made the participant-volume no-op findable.
    const QString target =
        trackKey.isEmpty() ? outputVolumeElementName(streamId)
                           : outputVolumeElementName(
                                 volumeKeyFor(streamId, trackKey));

    // A NAMED TRACK moves that track alone; an EMPTY key moves every audio
    // track this participant publishes, which is what the participant-level
    // control means now that a sharer can have two.
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
    if (!trackKey.isEmpty()) {
        if (apply(volumeKeyFor(streamId, trackKey)))
            return;
    } else {
        // Every bin whose name starts with this participant's prefix. An
        // iteration rather than one lookup, because gst_bin_get_by_name
        // returns only the FIRST match and a sharer has two.
        bool any = false;
        const QString prefix =
            outputVolumeElementName(streamId);
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            gchar *raw = element ? gst_element_get_name(element) : nullptr;
            const QString name = QString::fromUtf8(raw ? raw : "");
            g_free(raw);
            if (name.startsWith(prefix)) {
                g_object_set(element, "volume", volume, nullptr);
                any = true;
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        if (any)
            return;
    }

    // A MISS HERE IS A CONTROL THAT SILENTLY DOES NOTHING, so it says so.
    //
    // Per-participant volume was reported twice as "does nothing, but does
    // remember the set %" — the value reaching settings while never reaching
    // the audio. Every link in that chain reads correctly on paper, so this
    // reports which one actually broke rather than inviting a fourth guess:
    // the name we looked for, and how many receive volume elements exist at
    // all. Zero means no remote audio bin has been built (nothing to turn
    // down); a non-zero count with no match means the two ends derived the
    // stream's name differently, which is the failure this code has had
    // before.
    //
    // NOTHING TO LAND ON YET. Reported from a live log: the same line
    // hundreds of times for one participant whose audio track had not
    // arrived, and a Windows tester whose "volume 0" never muted — the value
    // was applied before the receive bin existed and then never again. So
    // the wanted volume is REMEMBERED here and applied by
    // applyPendingTrackVolume() the moment that stream's bin is built, and
    // the diagnostic below is logged once per key rather than per attempt.
    const QString pendingKey =
        trackKey.isEmpty() ? streamId : volumeKeyFor(streamId, trackKey);
    m_pendingTrackVolume.insert(pendingKey, percent);
    if (m_volumeMissWarned.contains(pendingKey))
        return;
    m_volumeMissWarned.insert(pendingKey);
    // Names only. A LiveKit stream sid is the same class of identifier this
    // file already logs as `trackKey=`, and no participant, track content or
    // capture is named.
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
    // Both shapes a caller can have asked for: the whole participant, and
    // this one track. The participant-level value is applied first so a
    // per-track choice, when there is one, wins.
    if (m_pendingTrackVolume.contains(streamId)) {
        m_volumeMissWarned.remove(streamId);
        setTrackVolume(streamId, QString(), m_pendingTrackVolume.value(streamId));
    }
    const QString key = volumeKeyFor(streamId, trackKey);
    if (m_pendingTrackVolume.contains(key)) {
        m_volumeMissWarned.remove(key);
        setTrackVolume(streamId, trackKey, m_pendingTrackVolume.value(key));
    }
}

double SfuMediaEngine::receiveVolumeForTest(const QString &streamId) const
{
    if (!m_subscriber.pipeline)
        return -1.0;
    const QString prefix = outputVolumeElementName(streamId);
    double found = -1.0;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
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
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    return found;
}

// ── RTP statistics trace ─────────────────────────────────────────────────

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
        m_statsIntervalMs = qEnvironmentVariableIsSet("LIGHTNING_CALL_STATS_TRACE")
            ? statsTraceIntervalMs(qEnvironmentVariable("LIGHTNING_CALL_STATS_TRACE"))
            : 0;
        if (m_statsIntervalMs > 0) {
            m_statsTimer.setInterval(m_statsIntervalMs);
            m_statsTimer.setSingleShot(false);
            connect(&m_statsTimer, &QTimer::timeout, this,
                    &SfuMediaEngine::requestStats);
            qCInfo(lcSfuMedia) << "rtp stats trace armed intervalMs="
                               << m_statsIntervalMs;
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

} // namespace

void SfuMediaEngine::requestStats()
{
    const auto ask = [this](Peer &peer, bool publisher) {
        if (!peer.webrtc)
            return;
        GstPromise *promise = gst_promise_new_with_change_func(
            onStatsReady, promiseCtxNew(this, peer.webrtc, publisher),
            promiseCtxFree);
        g_signal_emit_by_name(peer.webrtc, "get-stats", nullptr, promise);
    };
    ask(m_publisher, true);
    ask(m_subscriber, false);
}

void SfuMediaEngine::onStatsReady(GstPromise *promise, void *userData)
{
    // GStreamer thread. Read the reply here (a plain structure walk), then
    // hand numbers to the engine's thread; the alive registry in marshal()
    // drops the hand-off if the engine is gone.
    auto *ctx = static_cast<PromiseCtx *>(userData);
    StatsWalk walk;
    walk.peer = ctx->publisher ? QStringLiteral("pub") : QStringLiteral("sub");
    if (const GstStructure *reply = gst_promise_get_reply(promise))
        gst_structure_foreach(reply, collectStat, &walk);
    gst_promise_unref(promise);
    if (walk.out.isEmpty())
        return;
    SfuMediaEngine *engine = ctx->engine;
    const QList<RtpStat> stats = walk.out;
    marshal(engine, [engine, stats] { engine->logStats(stats); });
}

void SfuMediaEngine::logStats(const QList<RtpStat> &stats)
{
    const qint64 now = monotonicMs();
    for (const RtpStat &st : stats) {
        if (st.dir == QLatin1String("remote-inbound")) {
            qCInfo(lcSfuMedia).nospace().noquote()
                << "rtp stats peer=" << st.peer << " " << st.dir
                << " ssrc=" << st.ssrc << " lost=" << st.lost
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
            << " pli=" << st.pli << " nack=" << st.nack << " fir=" << st.fir;
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
    //
    // BOUNDED. Names arrive from remote input -- a media key's Olm-vouched
    // sender, the SFU's participant list -- and each ring holds sixteen key
    // slots, so an unbounded map is a per-call memory amplifier for anyone
    // who can address this device. 128 participants times a handful of
    // aliases is far inside the cap; past it a new name gets a DETACHED ring
    // that is never stored, which decrypts nothing and remembers nothing.
    constexpr int kMaxReceiveRings = 1024;
    auto cryptor = std::make_shared<CallFrameCryptor>();
    if (m_recvCryptors.size() >= kMaxReceiveRings)
        return cryptor;
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
        // WITH ITS SIBLINGS, always. These three are one record of one
        // subscriber description, and a track sid left behind from the last
        // call would be matched against the next call's section mids — which
        // are small integers that repeat, so a stale entry is not merely
        // useless, it is a plausible-looking wrong answer that routes a
        // track to the previous call's surface.
        m_trackForMline.clear();
    }
    m_sendKeyReady.store(false);
    m_recvKeyReady.store(false);
}

void SfuMediaEngine::noteStreamIds(const QHash<int, QString> &byMline,
                                   const QHash<int, QString> &midsByMline,
                                   const QHash<int, QString> &tracksByMline)
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
    for (auto it = tracksByMline.cbegin(); it != tracksByMline.cend(); ++it) {
        if (it.value().isEmpty() || it.value().size() > 128)
            m_trackForMline.remove(it.key());
        else
            m_trackForMline.insert(it.key(), it.value());
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

bool SfuMediaEngine::tokenIsLive(quintptr token, quint64 generation,
                                 Target *target) const
{
    if (!m_active)
        return false;
    // Generation first: start() and stop() each bump it, so a callback that
    // fired under a previous session cannot match even if the allocator
    // reused the element's address.
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
    // WHAT WE ACTUALLY ANSWERED, on the SUBSCRIBER only.
    //
    // A byte count says an answer exists; it does not say whether that
    // answer accepts the media being offered. Windows produced answers of a
    // plausible size for 1-, 2- and 3-section offers and then received not
    // one track — no `pad-added`, no receive bin, nothing — while the same
    // build sent audio the far end could hear. Every theory about the
    // RECEIVE side is unfalsifiable until this line exists, and two of them
    // have already been wrong.
    //
    // One line per section: its mid, its media kind, whether the port is
    // non-zero (zero REJECTS the section), and the direction we claimed.
    // `recvonly`/`sendrecv` means we asked to be sent to; `inactive` or a
    // zero port means we declined and the SFU is right not to send.
    //
    // Publisher side deliberately excluded: it offers, we already log the
    // offer, and doubling the volume on a working path helps nobody. Nothing
    // here is user content — SDP media kinds, mids, ports and directions.
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
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("m="))) {
                flush();
                // "m=<media> <port> <proto> ..." — the port is field 1, and
                // 0 there is how SDP says "I refuse this section".
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
        flush();
        qCInfo(lcSfuMedia) << "subscriber answer sections="
                           << sections.join(QLatin1Char(' '));
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

void SfuMediaEngine::handleCaptureEnded(const QString &cid)
{
    if (!m_active || !m_publishedBins.contains(cid))
        return;
    auto watch = m_publishWatch.find(cid);
    if (watch == m_publishWatch.end() || !watch->state || watch->reported)
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
    const quint64 generation = engine->m_generation.load();
    marshal(engine, [engine, token, generation, sdp] {
        engine->handleLocalDescription(token, generation, /*offer=*/true, sdp);
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

    // WHAT WEBRTCBIN ACTUALLY BUILT, as opposed to what we answered.
    //
    // The SDP summary logged in handleLocalDescription says what we CLAIMED;
    // this says what the element did about it. They can disagree, and when a
    // subscriber receives nothing at all that disagreement is the diagnosis:
    //
    //   * no transceivers  -> webrtcbin created none from the remote offer,
    //                         so nothing can ever arrive no matter how good
    //                         the answer looks;
    //   * transceivers present with current-direction RECVONLY/SENDRECV
    //                      -> our side is set up correctly and the fault is
    //                         upstream (the SFU is not sending, or the media
    //                         is not reaching this host);
    //   * current-direction INACTIVE/NONE
    //                      -> negotiated to nothing despite the SDP text.
    //
    // Subscriber only, and only counts and enum values — no ids, no content.
    // Two prior theories about this receive path were wrong; this is the
    // measurement that replaces a third guess.
    if (!ctx->publisher) {
        GArray *transceivers = nullptr;
        g_signal_emit_by_name(ctx->webrtc, "get-transceivers", &transceivers);
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
        // Direction enum, so the line reads without the header to hand:
        // 0 NONE, 1 INACTIVE, 2 SENDONLY, 3 RECVONLY, 4 SENDRECV.
        qCInfo(lcSfuMedia) << "subscriber transceivers n=" << summary.size()
                           << summary.join(QLatin1Char(' '))
                           << "(dir: 1=inactive 3=recvonly 4=sendrecv)";
    }

    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);

    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    auto *engine = ctx->engine;
    const quint64 generation = engine->m_generation.load();
    marshal(engine, [engine, token, generation, sdp] {
        engine->handleLocalDescription(token, generation, /*offer=*/false, sdp);
    });
}

void SfuMediaEngine::onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                                    char *candidate, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    const QString line = QString::fromUtf8(candidate ? candidate : "");
    const int mline = static_cast<int>(mlineIndex);
    marshal(engine, [engine, token, generation, mline, line] {
        engine->handleLocalCandidate(token, generation, mline, line);
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

    // Which sender is this, and which of their tracks? The decrypt ring is
    // keyed by the SENDER (`streamId`); the TRACK sid decides which surface
    // the media lands on. Two different questions, two different ids, and
    // getting either wrong is silent — a healthy connection carrying nothing
    // anyone can open or see.
    //
    // Three sources, in this order: the pad's own `msid`, then our own parse
    // of the same description matched on the section mid, then the mid
    // itself as a last resort that routes rather than decrypts. Everything
    // below is about the ways the first source has been observed to be
    // absent or wrong.
    QString streamId;
    QString trackMid;
    {
        // THE PAD'S OWN msid, which webrtcbin extracts from the remote
        // description. Read FIRST because it belongs to this exact pad — but
        // never trusted blindly, for the version reasons below.
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
        // WHEN THE PAD PROPERTY GIVES NOTHING, resolve from OUR OWN parse of
        // the same SDP rather than from a different property of the pad.
        //
        // The old fallback took the transceiver's `mid` and USED IT AS THE
        // TRACK KEY. That recovers the participant (a mid does identify the
        // section) but never the track sid, so the key ring was named "1" or
        // "2" — a ring nobody had keyed, because keys arrive addressed by
        // `TR_…`. Every frame then failed to decrypt with `passed=0`: audio
        // silent, video absent, both ends reporting a healthy connection.
        // The remote end was fine; only this side could not open anything.
        //
        // How much of the pad's `msid` webrtcbin populates has moved between
        // GStreamer releases, and the packaged Windows runtime (1.28.5) is
        // NOT the version this is developed against (1.26.11) — so this path
        // is reachable on a user's machine and unreachable on mine. The SDP
        // text is identical on both, and we already parse it for the
        // participant and the mid; the track sid comes from the same line.
        const bool padCarriedMsid = !streamId.isEmpty() || !trackMid.isEmpty();
        // EITHER id missing, not both. A pad can carry a stream id and no
        // track sid — an unpacked msid whose track token is not a `TR_…` —
        // and the earlier shape skipped the whole fallback in that case,
        // leaving the track key EMPTY. An empty key routes video to no
        // surface at all, which is the same invisible share by a different
        // road.
        //
        // ...AND WHEN AN ID IS PRESENT BUT NAMES NOTHING IN THE DESCRIPTION.
        // The version hazard above is not only "the property is empty": a
        // release that packs the msid differently hands back a token of the
        // WRONG KIND — the track id where the stream id belongs — and that
        // is worse than empty, because the decrypt ring is keyed by the
        // participant and a confidently wrong participant is a ring nobody
        // keyed while every id looks well-formed. Deliberately NOT a prefix
        // test (`PA_`/`TR_` are LiveKit's spelling, not a guarantee we should
        // hard-code): the test is whether the value appears among the stream
        // ids our own parse of THIS description recorded. A participant that
        // is in no section of the offer we were sent cannot be the sender of
        // a track in it.
        int sdpSectionsKnown = 0;
        bool padStreamIdUnknown = false;
        if (!streamId.isEmpty()) {
            QMutexLocker lock(&engine->m_recvMutex);
            padStreamIdUnknown = !engine->m_streamForMline.isEmpty()
                && !std::any_of(engine->m_streamForMline.cbegin(),
                                engine->m_streamForMline.cend(),
                                [&streamId](const QString &known) {
                                    return known == streamId;
                                });
        }
        if (padStreamIdUnknown) {
            qCWarning(lcSfuMedia)
                << "pad msid named a sender absent from the subscriber "
                   "description; re-deriving from the SDP";
            streamId.clear();
        }
        if (streamId.isEmpty() || trackMid.isEmpty()) {
            QString sectionMid;
            GstWebRTCRTPTransceiver *transceiver = nullptr;
            g_object_get(srcPad, "transceiver", &transceiver, nullptr);
            if (transceiver) {
                gchar *mid = nullptr;
                g_object_get(transceiver, "mid", &mid, nullptr);
                if (mid) {
                    sectionMid = QString::fromUtf8(mid);
                    g_free(mid);
                }
                gst_object_unref(transceiver);
            }
            if (!sectionMid.isEmpty()) {
                // Matched on the section's OWN mid, never on a positional
                // index: webrtcbin numbers src pads over the media it
                // produces while LiveKit's offer carries a data channel in
                // section 0, so the two counts do not agree.
                QMutexLocker lock(&engine->m_recvMutex);
                sdpSectionsKnown = engine->m_midForMline.size();
                for (auto it = engine->m_midForMline.cbegin();
                     it != engine->m_midForMline.cend(); ++it) {
                    if (it.value() != sectionMid)
                        continue;
                    // Fill only what is MISSING. What the pad itself said is
                    // first-hand and stays.
                    if (streamId.isEmpty())
                        streamId = engine->m_streamForMline.value(it.key());
                    if (trackMid.isEmpty())
                        trackMid = engine->m_trackForMline.value(it.key());
                    break;
                }
            }
            // Only now, and only if the SDP had no track sid either. A mid is
            // a LAST RESORT that routes something rather than nothing; it is
            // never a key that can decrypt.
            if (trackMid.isEmpty())
                trackMid = sectionMid;
        }
        // `sdpSections` is what makes ONE capture decisive when this still
        // goes wrong: zero says our own SDP scan found no media sections to
        // match against, non-zero with an empty streamId says the sections
        // are there and the mid did not match one. Those are different
        // causes and they were previously indistinguishable from the log.
        qCInfo(lcSfuMedia) << "received track attributed="
                           << !streamId.isEmpty() << "trackKey=" << trackMid
                           << "fromPadMsid=" << padCarriedMsid
                           << "sdpSections=" << sdpSectionsKnown
                           << "padSenderUnknown=" << padStreamIdUnknown;
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
    const quint64 generation = engine->m_generation.load();
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
                     .arg(outputVolumeElementName(
                         volumeKeyFor(streamId, trackMid)))
               : QStringLiteral("queue ! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! autoaudiosink")
                     .arg(outputVolumeElementName(
                         volumeKeyFor(streamId, trackMid))));

    GError *error = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error) {
        g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        gst_object_unref(pipeline);
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
        });
        return;
    }
    if (!gst_bin_add(GST_BIN(pipeline), bin)) {
        gst_object_unref(pipeline);
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
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
    // A volume the user chose before this bin existed lands now. On the GUI
    // thread, like every other write to the volume elements.
    marshal(engine, [engine, streamId, trackMid, generation] {
        engine->applyPendingTrackVolume(streamId, trackMid, generation);
    });
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
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
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
