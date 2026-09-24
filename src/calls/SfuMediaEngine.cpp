#include "calls/SfuMediaEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

#include <unistd.h>

#include "calls/CallFrameCryptor.h"
#include "calls/CaptureDeviceSelection.h"
#include "calls/GstBootstrap.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/WindowCaptureSrc.h"
#include "calls/SfuVideoRouter.h"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
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
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

namespace {
Q_LOGGING_CATEGORY(lcSfuMedia, "lightning.calls.sfu")

// GStreamer calls back on its own threads, so a marshalled lambda must never
// run against a destroyed engine. The queued invocation uses the engine as
// receiver, so anything already in flight dies with the QObject.
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

/// Summarises what each SDP section negotiated: `m=` kinds and `a=rtpmap`
/// codec names only (gated on LIGHTNING_SDP_TRACE). Never the SDP text, which
/// carries host IPs, nor the ICE ufrag/pwd.
///
/// Answers whether RED rides the publisher transport: RED wraps the Opus
/// payload, so the far end's decryptor sees no Opus TOC and drops every audio
/// frame, which looks exactly like a key problem from our side.
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
            // Kind plus payload list; tells whether RED was offered.
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

/// Monotonic millisecond clock for publish probes. Not wall clock: these are
/// differences across a live call and must not jump on NTP or resume.
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

/// LiveKit trickles candidates as RTCIceCandidate JSON. Built with
/// QJsonDocument so a candidate line cannot inject JSON.
QString candidateInitJson(const QString &candidate, int mlineIndex)
{
    QJsonObject object;
    object.insert(QStringLiteral("candidate"), candidate);
    object.insert(QStringLiteral("sdpMLineIndex"), mlineIndex);
    // LiveKit accepts the m-line index alone; an invented sdpMid is worse.
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
/// Raises `remoteMediaBlocked` from a streaming thread by posting to the
/// engine's thread. Only the sid and a closed-set reason cross.
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

/// Why frames in the window are failing, for the badge's reason string.
enum class CryptoDropCause { None, NoKey, Undecryptable };

struct CryptoProbeCtx {
    SfuMediaEngine *engine = nullptr;
    /// Send side only. Shared because the sender can leave while one of their
    /// frames is still in the probe. The receive side resolves its ring per
    /// frame through `streamId`, since the sending participant may be
    /// announced after the pad appears.
    std::shared_ptr<CallFrameCryptor> cryptor;
    /// Receive side: the sender's LiveKit sid, from the subscriber offer's
    /// `msid`.
    QString streamId;
    /// Direction: true encrypts (send side), false decrypts (receive side).
    bool encrypting = false;
    /// Video uses VP8's cleartext-header rule; audio uses Opus's.
    bool video = false;
    /// Read on the streaming thread.
    const std::atomic<bool> *required = nullptr;
    const std::atomic<bool> *keyReady = nullptr;
    /// Send side: this track's IV stream id, unique among encrypting tracks.
    quint32 ivStream = 0;
    /// Engine-wide totals. Raw pointers into the engine: the pipeline going to
    /// NULL ends ordinary probes, and stop()/~SfuMediaEngine wait (bounded) for
    /// deferred publish teardowns, whose bins are unparented while still
    /// running. If that wait expires the guarantee is best-effort and logged.
    std::atomic<quint64> *total = nullptr;
    std::atomic<quint64> *totalDropped = nullptr;
    /// Receive side only; see framesArrivingEncryptedOnAClearCall().
    std::atomic<quint64> *totalCiphertextShaped = nullptr;
    /// Frames passed and dropped. Separates "our media never reaches the wire"
    /// from "the far end cannot use it". Owned by the single streaming thread,
    /// so no locking (as for the other per-probe fields below).
    quint64 passed = 0;
    quint64 dropped = 0;
    /// Once-per-track diagnosis lines (no key, failed, working).
    bool saidNoKey = false;
    /// One diagnosis line per failure reason, a bit per DecryptFailure value,
    /// so an early one-off failure cannot silence a later, different one.
    quint8 saidFailedMask = 0;
    bool saidWorking = false;
    /// Receive side: see SfuMediaEngine::framesServerInjected(). Null on the
    /// send side.
    std::atomic<quint64> *totalServerInjected = nullptr;
    /// Per-reason failure counts for the detail and histogram lines. Counts
    /// only, never keys, IVs or frame content.
    static constexpr int kFailureReasons = 7;
    std::array<quint64, kFailureReasons> failures{};
    /// Detail lines logged per reason (first five, then at shouldReport()).
    std::array<quint8, kFailureReasons> detailsLogged{};
    /// Frames dropped because the ring held no key at all.
    quint64 ringEmpty = 0;
    /// Consecutive failed frames, and consecutive `bad-iv-length` ones.
    /// Server-injected frames touch neither.
    quint64 run = 0;
    quint64 badIvRun = 0;
    quint64 longestBadIvRun = 0;
    /// Trailer bytes seen in `bad-iv-length` frames. One constant value
    /// suggests an unrecognised server trailer; many suggest the sender is
    /// not frame-encrypting at all.
    std::array<bool, 256> badIvKeyIndexSeen{};
    int distinctBadIvKeyIndices = 0;
    /// Server-injected frames: total, the burst in progress, bursts seen.
    quint64 sif = 0;
    quint64 sifRun = 0;
    quint64 sifBursts = 0;
    bool saidSif = false;
    /// Histogram line: written when something failed or was injected, at most
    /// every five seconds, and once more when the probe is freed.
    qint64 lastHistogramMs = -1;
    bool histogramDirty = false;
    /// Said once when a call we believe is clear carries ciphertext.
    bool saidUnexpectedCiphertext = false;
    /// Sliding window of frame outcomes the badge is decided on. A pure policy
    /// struct so its thresholds can be tested directly.
    SfuMediaEngine::BlockedRunPolicy blocked;
    /// Cause of the most recent failure, so the badge names the current fault:
    /// no key (key distribution) versus undecryptable (key agreement).
    CryptoDropCause lastCause = CryptoDropCause::None;
    /// Frames passed through on a clear call that look encrypted. Uses the
    /// badge's windowed policy because `looksEncrypted` is a structural test a
    /// clear frame can pass by chance; only the rate is meaningful.
    SfuMediaEngine::BlockedRunPolicy ciphertext;
    /// Clear frames that carried our trailer.
    quint64 ciphertextShaped = 0;
};

/// Records one frame outcome and tells the UI only when the verdict changes.
/// The send side never badges: failing to encrypt our own frames is a
/// different message to a different person.
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

/// Checks whether a frame on the `!required && !haveKey` receive path, which
/// is passed through unexamined, is actually ciphertext: a peer encrypting
/// while we hold no key produces silence downstream with no other signal.
///
/// Windowed via BlockedRunPolicy (raise at 90%, clear at 25%, min 50 frames)
/// because `looksEncrypted` is a two-byte heuristic. Reporting only: the frame
/// is still passed through, as dropping on a heuristic would break interop.
static void noteClearFrameShape(CryptoProbeCtx *ctx, GstBuffer *buffer,
                                CallFrameCryptor::FrameKind kind)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return;
    // No QByteArray: this runs per frame and QByteArray would deep copy.
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

/// Ring name for logs: the U+001F separator becomes '/', so the log has no
/// control characters and matches `ring=<user>/<device>`.
QString printableRing(const QString &ring)
{
    QString out = ring;
    out.replace(QChar(0x1f), QLatin1Char('/'));
    return out;
}

/// True for the first frame, then rarely.
bool shouldReport(quint64 count)
{
    return count == 1 || count == 10 || count % 500 == 0;
}

/// Counts RTP packets leaving the publishing bin's src pad, the last point we
/// own. The encrypt probe sits on the encoder's src pad and only counts what
/// was encoded; comparing the two localises a stalled payloader.
///
/// The probe is never removed and owns its context through its destroy
/// notify. The watchdog captures only the separately owned counter; sharing
/// the context with it was a use-after-free.
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

/// Which sender an appsink belongs to; one per received video track.
struct VideoSinkCtx {
    SfuMediaEngine *engine = nullptr;
    /// Primary routing key: the section's SDP `mid`, which names one track.
    /// Empty for the local self-view.
    QString mid;
    /// Fallback routing key: the sender's sid. Cannot tell camera from screen
    /// share, but tiles attach to it before tracks are announced and servers
    /// that omit `mid` still need to render.
    QString streamId;
    /// Decrypted frames with nowhere to go. Last, so brace initialisers stay
    /// valid.
    quint64 unrouted = 0;
};

void videoSinkCtxFree(void *data, GClosure *)
{
    delete static_cast<VideoSinkCtx *>(data);
}

/// Copies one decoded RGBA frame and hands it to the router. A QVideoFrame
/// cannot borrow the GstBuffer, which is unreffed on return.
GstFlowReturn onVideoSample(GstElement *sink, void *userData)
{
    auto *ctx = static_cast<VideoSinkCtx *>(userData);
    if (!ctx || !ctx->engine)
        return GST_FLOW_OK;

    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample)
        return GST_FLOW_OK;

    // Ask before copying: an unwatched sender costs a lookup, not a memcpy.
    // SfuVideoRouter is thread-safe.
    bool wanted = false;
    QString routeKey;
    if (SfuVideoRouter *router = ctx->engine->videoRouter()) {
        // Prefer the mid (the tile that asked for this track), then the sender.
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
        // Decrypted but unrouted. The crypto counters look healthy either
        // way, so log rarely with both keys so a routing mismatch shows up.
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
    // Dimensions come from a remote sender and size an allocation: bound them.
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
        // Row by row: GStreamer and Qt strides may differ.
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

    // Delivered on the GUI thread, which owns the QVideoSink.
    SfuMediaEngine *engine = ctx->engine;
    const QString streamId = routeKey;
    marshal(engine, [engine, streamId, frame] {
        if (SfuVideoRouter *router = engine->videoRouter())
            router->deliverFrame(streamId, frame);
    });
    return GST_FLOW_OK;
}

static_assert(static_cast<int>(CallFrameCryptor::DecryptFailure::AuthTag)
                  == CryptoProbeCtx::kFailureReasons - 1,
              "CryptoProbeCtx's per-reason arrays must cover every "
              "DecryptFailure value");

/// Monotonic ms for the histogram cadence; a clock step must not flood or
/// silence it.
qint64 probeMonotonicMs()
{
    return static_cast<qint64>(g_get_monotonic_time() / 1000);
}

/// Logs the length of a finished server-injected burst. livekit-server sends
/// 50 on a publisher mute (1 s at 50 fps) and 10 on track close. Bounded so a
/// hostile SFU cannot buy a log line per frame.
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

/// Per-stream receive summary. Numbers only.
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

/// Writes the histogram when something failed or was injected and five
/// seconds have passed; the first dirty event writes at once.
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
    // Final histogram: a sender who mutes sends nothing after the SFU's
    // injected frames, so no later frame would close the burst.
    if (ctx && !ctx->encrypting) {
        closeServerInjectedBurst(ctx);
        if (ctx->histogramDirty)
            logDecryptHistogram(ctx, "final");
    }
    delete ctx;
}

/// A frame the SFU wrote itself: dropped, counted separately, and kept out of
/// the badge window and the failure diagnosis.
///
/// livekit-client passes such frames on only if they match a known payload
/// (`identifySifPayload`). The SFU is outside the E2EE trust boundary, so we
/// drop instead; the frames are silence or a black 8x8 picture.
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

/// True if the last `n` bytes are base62, livekit-server's trailer alphabet.
/// A label for the detail line only.
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

/// Encrypts or decrypts one encoded frame, the unit LiveKit and Element Call
/// encrypt. Sits between encoder and payloader (send) or after the
/// depayloader (receive); per-RTP-packet crypto would interoperate with
/// nobody.
GstPadProbeReturn cryptoProbe(GstPad *pad, GstPadProbeInfo *info,
                              void *userData)
{
    Q_UNUSED(pad);
    auto *ctx = static_cast<CryptoProbeCtx *>(userData);
    auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !ctx)
        return GST_PAD_PROBE_OK;
    // The send ring never changes. The receive ring is looked up per frame:
    // the sid a keyed device publishes under arrives in a participant update
    // that is not ordered against the pad appearing.
    const std::shared_ptr<CallFrameCryptor> cryptor = ctx->encrypting
        ? ctx->cryptor
        : (ctx->engine ? ctx->engine->recvCryptorFor(ctx->streamId)
                       : nullptr);
    if (!cryptor)
        return GST_PAD_PROBE_OK;

    // VP8 keyframes leave 10 header bytes in the clear and delta frames 3;
    // Opus leaves the TOC byte. The keyframe flag comes from GStreamer rather
    // than from parsing the bitstream. Computed before the key check because
    // the clear-pass branch needs it too.
    CallFrameCryptor::FrameKind kind = CallFrameCryptor::FrameKind::Audio;
    if (ctx->video) {
        kind = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)
            ? CallFrameCryptor::FrameKind::VideoDelta
            : CallFrameCryptor::FrameKind::VideoKey;
    }

    // Server-injected frames first, before any key lookup, as livekit-client
    // does (`isFrameServerInjected`). They carry no crypto trailer and would
    // otherwise be misfiled as `bad-iv-length` or "no key".
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
        // Any other frame ends a burst in progress.
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
            if (ctx->totalDropped)
                ctx->totalDropped->fetch_add(1);
            ctx->lastCause = CryptoDropCause::NoKey;
            noteCryptoOutcome(ctx, /*failed=*/true);
            if (!ctx->encrypting) {
                ++ctx->ringEmpty;
                ++ctx->run;
                ctx->histogramDirty = true;
                maybeLogDecryptHistogram(ctx);
            }
            if (!ctx->encrypting && !ctx->saidNoKey) {
                ctx->saidNoKey = true;
                // Two possible causes; the log must not name only one.
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
            return GST_PAD_PROBE_DROP;
        }
        ++ctx->passed;
        if (ctx->total)
            ctx->total->fetch_add(1);
        // A clear pass is still an outcome, or the window goes stale.
        noteCryptoOutcome(ctx, /*failed=*/false);
        // Receive side only: check whether the "clear" frame is ciphertext.
        if (!ctx->encrypting)
            noteClearFrameShape(ctx, buffer, kind);
        if (shouldReport(ctx->passed)) {
            // Names the stream so a camera and a screen share can be told
            // apart. This counts decrypted frames handed downstream, not
            // frames drawn (Qt Quick's software backend renders no video).
            // `ciphertextShaped` shows whether "clear" frames carry our
            // trailer.
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
        if (ctx->totalDropped)
            ctx->totalDropped->fetch_add(1);
        ctx->lastCause = CryptoDropCause::Undecryptable;
        noteCryptoOutcome(ctx, /*failed=*/true);
        const int reasonBit = static_cast<int>(why.reason);
        const bool reasonKnown =
            reasonBit >= 0 && reasonBit < CryptoProbeCtx::kFailureReasons;
        if (!ctx->encrypting && reasonKnown) {
            // Accounting for the detail and histogram lines.
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
            // Detail line: first five failures per reason, then rate-limited.
            // Separates the causes behind `bad-iv-length`. Sizes, two trailer
            // bytes, the first cleartext header byte and booleans only; never
            // a key, IV, ciphertext or payload.
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
            // The cryptor names the cause; an empty return has six.
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
        return GST_PAD_PROBE_DROP;
    }
    ++ctx->passed;
    if (ctx->total)
        ctx->total->fetch_add(1);
    // Clear the badge once enough frames decrypt. The log-once flags stay
    // set so a flapping stream does not flood the log.
    noteCryptoOutcome(ctx, /*failed=*/false);
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

    // The payload changed size: a new buffer carrying the original's timing
    // and flags.
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
    // One process-wide init with the plugin path applied; see GstBootstrap.h.
    // Never gst_init here: whichever backend was probed first would decide
    // whether the bundled plugin path was applied.
    const bool initOk = lightning::gst::ensureInitialised(whyNot);
    if (!initOk)
        return false;

    // Our own elements must be registered before any pipeline names them.
    lightning::rtp::registerVp8Payloader();
    lightning::wincap::registerWindowCaptureSrc();
    // Everything the SFU pipelines need except a capture source: requiring
    // one would refuse audio calls on machines without a camera plugin.
    // Consequently an "available" engine is not evidence a share can start.
    static const char *const kRequired[] = {
        "webrtcbin",     "nicesrc",       "nicesink",     "dtlssrtpenc",
        "dtlssrtpdec",   "opusenc",       "opusdec",      "rtpopuspay",
        "rtpopusdepay",  "audioconvert",  "audioresample", "audiotestsrc",
        "fakesink",      "autoaudiosrc",  "autoaudiosink", "queue",
        "valve",         "volume",        "capsfilter",
        // Video publish/receive.
        "vp8enc",        "vp8dec",        "rtpvp8pay",    "rtpvp8depay",
        "videoconvert",  "videoscale",    "videotestsrc", "videorate",
        // Not `compositor`: nothing builds a pipeline from it.
        // Ours; without it encrypted video cannot be sent.
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
    // Armed by a screen-share publish; see tickShareKeepAlive(). The interval
    // is the poll, not the threshold.
    m_shareKeepAliveTimer.setInterval(200);
    m_shareKeepAliveTimer.setSingleShot(false);
    connect(&m_shareKeepAliveTimer, &QTimer::timeout, this,
            &SfuMediaEngine::tickShareKeepAlive);
    QMutexLocker lock(&g_aliveMutex);
    g_aliveEngines.insert(this);
}

SfuMediaEngine::~SfuMediaEngine()
{
    {
        // Unregister first so no new marshalled lambda targets us.
        QMutexLocker lock(&g_aliveMutex);
        g_aliveEngines.remove(this);
    }
    // stop() waits (bounded) for deferred publish teardowns even when the
    // session is already stopped; otherwise an unparented bin could push
    // buffers through probes pointing into members being destroyed.
    stop();
}

void SfuMediaEngine::start()
{
    stop();
    // Bump first so callbacks from the previous session are already stale.
    m_generation.fetch_add(1);
    m_active = true;
    m_framesEncrypted.store(0);
    m_framesDecrypted.store(0);
    m_framesDropped.store(0);
    m_framesClearButCiphertextShaped.store(0);
    m_framesServerInjected.store(0);
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    m_publisherEverPublished = false;
    Q_EMIT connectionStateChanged(QStringLiteral("connecting"));
}

void SfuMediaEngine::stop()
{
    // Keys can arrive before start() (the controller is still preparing), so
    // clear them before the never-started early return.
    clearKeys();
    // Wait (bounded) for deferred publish teardowns before destroying
    // anything: their unparented bins are unreachable from destroyPeer() and
    // their probes point at this object. A single atomic load when idle.
    awaitPublishTeardowns();
    if (!m_active && !m_publisher.pipeline && !m_subscriber.pipeline)
        return;
    m_generation.fetch_add(1);
    m_active = false;
    m_publishedBins.clear();
    m_shareKeepAliveTimer.stop();
    for (auto it = m_publishWatch.cbegin(); it != m_publishWatch.cend(); ++it)
        releaseKeepAlive(it->state);
    m_publishWatch.clear();
    m_pendingTrackVolume.clear();
    m_volumeMissWarned.clear();
    m_volumeAppliedLog.clear();
    // The descriptors those bins used are ours to close.
    for (auto it = m_publishedFds.cbegin(); it != m_publishedFds.cend(); ++it) {
        if (it.value() > 0)
            ::close(it.value());
    }
    m_publishedFds.clear();
    m_statsTimer.stop();
    m_lastRtpBytes.clear();
    m_shareAudioScanTimer.stop();
    m_shareAudioCid.clear();
    m_shareAudioSerials.clear();
    m_shareAudioBranches = 0;
    m_shareAudioNextIndex = 0;
    m_shareAudioScans = 0;
    m_shareAudioSources.stop();
    destroyPeer(m_publisher);
    destroyPeer(m_subscriber);
    // Engine state is per session; the controller re-applies user intent.
    m_microphoneMuted = false;
    m_outputMuted.store(false);
    m_publishedMedia.store(0);
    m_publisherEverPublished = false;
    // Media keys must not outlive the call that used them.
    clearKeys();
    {
        // Diagnoses are per call too; a second call must be able to repeat
        // them.
        QMutexLocker lock(&m_diagnosedMutex);
        m_diagnosedOnce.clear();
        m_keyArrivals.clear();
    }
    Q_EMIT connectionStateChanged(QStringLiteral("closed"));
}

namespace {
/// Bus sync handler for SFU pipelines. Runs on a streaming thread: it logs
/// and never touches a Qt object. Logs the element name and the static error
/// string only; `debug` carries file paths and is not logged.
///
/// Always drops after inspection: nothing pops these buses, so a passed
/// message would sit in the queue for the whole call and STATE_CHANGED
/// messages would keep torn-down elements alive.
GstBusSyncReply onBusMessage(GstBus *, GstMessage *message, void *userData)
{
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    const GstMessageType type = GST_MESSAGE_TYPE(message);
    // Capture level meter. An ELEMENT message; the peak is reduced to a double
    // here and marshalled to the GUI thread.
    if (type == GST_MESSAGE_ELEMENT && engine) {
        const GstStructure *fields = gst_message_get_structure(message);
        const gchar *srcName = GST_MESSAGE_SRC_NAME(message);
        if (fields && srcName && g_strcmp0(srcName, "miclevel") == 0
            && gst_structure_has_name(fields, "level")) {
            // -350 is a real reading: `level`'s floor for digital silence. So
            // "could not read" is tracked by a flag, not a sentinel value, or a
            // dead microphone could never raise the silence warning.
            double peak = -350.0;
            bool readable = false;
            if (const GValue *peaks = gst_structure_get_value(fields, "peak")) {
                // `level` posts one value per channel in a (deprecated)
                // GValueArray; take the loudest channel.
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                if (G_VALUE_HOLDS(peaks, G_TYPE_VALUE_ARRAY)) {
                    auto *array =
                        static_cast<GValueArray *>(g_value_get_boxed(peaks));
                    for (guint i = 0; array && i < array->n_values; ++i) {
                        const GValue *one = g_value_array_get_nth(array, i);
                        if (one && G_VALUE_HOLDS_DOUBLE(one)) {
                            peak = qMax(peak, g_value_get_double(one));
                            readable = true;
                        }
                    }
                } else if (GST_VALUE_HOLDS_ARRAY(peaks)) {
                    // GstValueArray is the modern equivalent. No runtime has
                    // been seen using it; accepted defensively.
                    const guint n = gst_value_array_get_size(peaks);
                    for (guint i = 0; i < n; ++i) {
                        const GValue *one = gst_value_array_get_value(peaks, i);
                        if (one && G_VALUE_HOLDS_DOUBLE(one)) {
                            peak = qMax(peak, g_value_get_double(one));
                            readable = true;
                        }
                    }
                } else {
                    // Unknown type: say so once instead of failing silently.
                    static std::atomic<bool> saidOnce{false};
                    if (!saidOnce.exchange(true)) {
                        qCWarning(lcSfuMedia)
                            << "the capture level meter posted a peak this "
                               "build cannot read: type="
                            << G_VALUE_TYPE_NAME(peaks)
                            << "- the microphone level and the silence "
                               "warning are BOTH unavailable in this build";
                    }
                }
                G_GNUC_END_IGNORE_DEPRECATIONS
            }
            // An unread message is not evidence of silence.
            if (readable) {
                marshal(engine,
                        [engine, peak] { engine->handleMicLevel(peak); });
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
    if (type == GST_MESSAGE_ERROR) {
        qCWarning(lcSfuMedia) << "pipeline error element=" << element
                              << "code=" << (error ? error->code : 0)
                              << "reason=" << reason;
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

        // The output device refused the stream (e.g. a surround or IEC958
        // profile). The failing element is the sink autoaudiosink chose, so
        // match by substring, and only on negotiation/open errors: ordinary
        // teardown posts errors too.
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
                   // Per-application share sources are `shareapp0`, `shareapp1`, ...
                   || element.startsWith(QLatin1String("shareapp")))) {
        // GStreamer posts a capture device that cannot open (e.g. a
        // microphone held exclusively on Windows) as a WARNING, so the call
        // proceeds with nothing behind the track. Logged loudly; deliberately
        // not surfaced in the UI yet.
        qCWarning(lcSfuMedia)
            << "capture source could not open its device: element=" << element
            << "code=" << error->code << "reason=" << reason
            << "— this track will carry nothing";
    } else {
        qCInfo(lcSfuMedia) << "pipeline warning element=" << element
                           << "code=" << (error ? error->code : 0)
                           << "reason=" << reason;
    }
    if (error)
        g_error_free(error);
    if (debug)
        g_free(debug);
    // Not failed(): pipelines post errors during ordinary teardown, and
    // failed() ends the call. Only publishFailed(), which ends nothing, is
    // raised, and handlePublishError() on the GUI thread decides.
    //
    // The failing element sits deep inside the bin, so walk up to the child
    // of the pipeline, which is named with the track's cid. m_publishedBins
    // belongs to the GUI thread and is not consulted here.
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
    // Drop after inspection; see above.
    gst_message_unref(message);
    return GST_BUS_DROP;
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

    // bundle-policy=max-bundle is what LiveKit negotiates.
    GstElement *pipeline = gst_pipeline_new(nullptr);
    if (pipeline) {
        // Set before anything is added: construction failures post at once.
        if (GstBus *bus = gst_element_get_bus(pipeline)) {
            gst_bus_set_sync_handler(bus, onBusMessage, this, nullptr);
            gst_object_unref(bus);
        }
    }
        // Named per peer connection so diagnostics can identify it without
        // touching the engine from a GStreamer thread.
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
    // The pipeline owns the element; we hold a ref for the session.
    peer.pipeline = pipeline;
    peer.webrtc = GST_ELEMENT(gst_object_ref(webrtc));

    if (target == Target::Publisher) {
        // Only the publisher offers; the subscriber answers the server's
        // offers, and a negotiation-needed there would compete.
        g_signal_connect(webrtc, "on-negotiation-needed",
                         G_CALLBACK(onNegotiationNeeded), this);
    }
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(onIceCandidate),
                     this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(onPadAdded), this);
    // Removes receive bins when tracks go away. Logged when it fires, since
    // whether webrtcbin raises it for a retired LiveKit track is unverified.
    g_signal_connect(webrtc, "pad-removed", G_CALLBACK(onPadRemoved), this);
    armStatsTrace();
    // Connection state: signalling keeps working over the WebSocket while
    // ICE/DTLS may never complete, so without this "no audio" and "no
    // connection" look the same. `this` is user data only so destroyPeer can
    // disconnect by data; the handler never touches the engine.
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
        // Never logged.
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
            // Sanity filter: these strings are assembled into a
            // credential-bearing URI.
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

/// Every name an audio server may record this process under: PipeWire uses
/// the binary name, which differs from applicationName().
static QStringList ourAudioClientNames()
{
    QStringList names{ QCoreApplication::applicationName() };
    const QString binary =
        QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    if (!binary.isEmpty() && !names.contains(binary, Qt::CaseInsensitive))
        names << binary;
    return names;
}

/// A loopback capture, and whether it excludes this process's own playback
/// (which decides what the log tells the user).
struct ShareAudioSource {
    QString description;
    bool excludesUs = false;
};

/// The element that captures what the computer is playing, or empty. The
/// screen-cast portal and WASAPI carry no audio, so share audio is a second
/// capture chosen per platform.
///
/// Chosen at runtime, and the property is checked as well as the element:
/// `gst_parse_launch` fails on an unknown property, and which version a
/// package ships is not visible at compile time.
static ShareAudioSource shareAudioSourceDescription()
{
    struct Candidate {
        const char *element;
        const char *property;   // must exist, or the parse would fail
        const char *description; // %1 is this process's pid, when used
        bool excludesUs;
    };
    // In order of preference. wasapi is the older fallback.
    static const Candidate kCandidates[] = {
#if defined(Q_OS_WIN)
        // Process loopback: exclude-process-tree with loopback-target-pid
        // captures the endpoint mix minus our own playback, so a shared
        // screen does not echo the call back. The mode is ignored unless the
        // pid is non-zero. Both properties exist only on Windows 10 build
        // 20348+, hence the property probe below. `loopback=true` is not
        // needed on this path.
        { "wasapi2src", "loopback-target-pid",
          "wasapi2src name=sharesrc loopback-mode=exclude-process-tree "
          "loopback-target-pid=%1 low-latency=true", true },
        // Older Windows: the full endpoint mix, echo included.
        { "wasapi2src", "loopback",
          "wasapi2src name=sharesrc loopback=true low-latency=true", false },
        { "wasapisrc", "loopback", "wasapisrc name=sharesrc loopback=true", false },
#elif defined(Q_OS_LINUX)
        // `@DEFAULT_MONITOR@` is resolved by the server, so it follows the
        // default sink. A sink monitor is post-mix and cannot exclude us; used
        // only when per-application PipeWire capture is unavailable.
        { "pulsesrc", "device",
          "pulsesrc name=sharesrc device=@DEFAULT_MONITOR@", false },
#endif
        { nullptr, nullptr, nullptr, false },
    };

    // Initialise first: before the registry loads, factory lookup reports
    // "no such element" instead of failing.
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
        // Only .arg() when there is a placeholder; otherwise Qt warns.
        QString description = QString::fromLatin1(c->description);
        if (description.contains(QLatin1String("%1")))
            description = description.arg(QCoreApplication::applicationPid());
        return ShareAudioSource{ description, c->excludesUs };
    }
    return ShareAudioSource{};
}

bool SfuMediaEngine::shareAudioAvailable()
{
    // Either capture counts; per-application capture does not go through
    // shareAudioSourceDescription().
    return lightning::shareaudio::perApplicationCaptureAvailable()
        || !shareAudioSourceDescription().description.isEmpty();
}

namespace {

// Devices as GStreamer sees them, asked per capture build. Qt device ids are
// not GStreamer handles (see CaptureDeviceSelection.h); asking each time also
// notices unplugged devices.
QList<lightning::calls::GstDeviceCandidate> enumerateDevices(const char *klass)
{
    QList<lightning::calls::GstDeviceCandidate> out;
    if (!lightning::gst::ensureInitialised())
        return out;
    GstDeviceMonitor *monitor = gst_device_monitor_new();
    if (!monitor)
        return out;
    gst_device_monitor_add_filter(monitor, klass, nullptr);
    // A monitor that will not start means "no answer": keep the default.
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
                // Serialised: identity keys may be int or uint, and a typed
                // string read of the wrong type returns null.
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
        out.append(candidate);
        gst_object_unref(device);
    }
    g_list_free(devices);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return out;
}

/// How long a device enumeration may block the caller before it is abandoned.
/// A healthy PipeWire answers in well under 250 ms.
constexpr int kDeviceEnumerationBudgetMs = 2500;

/// `enumerateDevices` with a time bound. gst_device_monitor_start() is
/// synchronous and unbounded (the PulseAudio provider waits for the initial
/// device list with no timeout), and the caller is the GUI thread in the join
/// path (GitHub issue #12).
///
/// A blocked enumeration cannot be cancelled, so the worker is abandoned and
/// the klass latched: a provider that hung once is not asked again this
/// session. The fallback is the platform default device.
QList<lightning::calls::GstDeviceCandidate> monitorCandidates(const char *klass)
{
    static QMutex latchMutex;
    static QSet<QString> hung;
    const QString key = QString::fromLatin1(klass);
    {
        QMutexLocker lock(&latchMutex);
        if (hung.contains(key))
            return {};
    }

    // Initialise on the caller's thread; first-time init must not happen in a
    // thread that may be abandoned.
    if (!lightning::gst::ensureInitialised())
        return {};

    auto slot = std::make_shared<
        std::promise<QList<lightning::calls::GstDeviceCandidate>>>();
    auto answer = slot->get_future();
    // Detached on purpose; the shared promise makes a late answer harmless.
    std::thread([slot, klass] {
        // Never let an exception break the promise: get() would rethrow it
        // on the GUI thread.
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

// Concrete elements that can carry a device choice, in preference order.
// `autoaudiosrc`/`autoaudiosink` have no device property.
QStringList microphoneElementPreference()
{
#if defined(Q_OS_WIN)
    return {QStringLiteral("wasapisrc")};
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
    return {QStringLiteral("wasapisink")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("osxaudiosink")};
#else
    return {QStringLiteral("pipewiresink"), QStringLiteral("pulsesink")};
#endif
}

// Elements this build can instantiate; naming a missing one fails the parse.
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

// Sets the resolved property on an already-parsed bin. Never interpolated into
// a description: a quote in a device name would be parsed as syntax.
void applyBindingTo(GstElement *bin, const char *elementName,
                    const lightning::calls::DeviceBinding &binding)
{
    if (binding.isEmpty())
        return;
    GstElement *element = gst_bin_get_by_name(GST_BIN(bin), elementName);
    if (!element)
        return;
    // Check the property exists; plugin versions differ and g_object_set on an
    // absent property only warns.
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element),
                                     binding.property.toUtf8().constData())) {
        g_object_set(element, binding.property.toUtf8().constData(),
                     binding.value.toUtf8().constData(), nullptr);
        qCInfo(lcSfuMedia) << "capture device bound element=" << elementName
                           << "property=" << binding.property
                           << "value=" << binding.value
                           << "matched-by=" << binding.reason;
    } else {
        qCWarning(lcSfuMedia)
            << "capture element has no" << binding.property
            << "property; using the platform default device instead";
    }
    gst_object_unref(element);
}

} // namespace

void SfuMediaEngine::setPreferredDevices(const DeviceChoice &camera,
                                         const DeviceChoice &microphone,
                                         const DeviceChoice &speaker)
{
    QMutexLocker lock(&m_deviceMutex);
    m_cameraChoice = camera;
    m_microphoneChoice = microphone;
    m_speakerChoice = speaker;
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

void SfuMediaEngine::setShareQuality(int maxHeight, int fps)
{
    // Snapped to known values: these come from settings and reach the caps
    // string.
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

    // Per-application capture (one pipewiresrc per playing app, summed, ours
    // excluded) avoids echoing the call back; the sink monitor fallback is
    // post-mix and cannot exclude us. The choice is logged.
    bool perApplication = false;
    QString source;
    // Enumerated once and reused for the bookkeeping below; a second
    // enumeration could record an app with no branch built for it.
    QList<lightning::shareaudio::Stream> streams;
    if (m_testSources) {
        source = QStringLiteral(
            "audiotestsrc name=sharesrc is-live=true wave=sine freq=220 "
            "volume=0.05");
    } else if (m_shareAudioSources.start()) {
        streams = m_shareAudioSources.streams(
            QCoreApplication::applicationPid(), ourAudioClientNames());
        source = lightning::shareaudio::mixedSourceDescription(streams);
        perApplication = true;
        // An empty list is not a reason to fall back: nothing playing yet is
        // normal, and the monitor would reintroduce the echo.
        qCInfo(lcSfuMedia) << "share audio: capturing" << streams.size()
                           << "application stream(s), this process excluded";
        for (const lightning::shareaudio::Stream &s : streams) {
            qCInfo(lcSfuMedia)
                << "share audio source app="
                << (s.appName.isEmpty() ? s.nodeName : s.appName)
                << "serial=" << s.serial;
        }
    } else {
        const ShareAudioSource loopback = shareAudioSourceDescription();
        source = loopback.description;
        // The two outcomes must read differently in the log: only one echoes.
        if (loopback.excludesUs) {
            qCInfo(lcSfuMedia)
                << "share audio: capturing this machine's output with our own "
                   "process tree excluded at the OS level, so the call's own "
                   "audio is not sent back";
        } else {
            qCWarning(lcSfuMedia)
                << "share audio: no way to exclude ourselves from the capture "
                   "here, falling back to the output monitor. Remote "
                   "participants WILL hear this call's own audio, including "
                   "their own voices.";
        }
    }
    if (source.isEmpty()) {
        // The SFU was already told a track is coming, so report the failure.
        qCWarning(lcSfuMedia)
            << "share audio: no loopback capture element on this platform";
        Q_EMIT failed(QStringLiteral("share_audio_unavailable"));
        return;
    }

    // Built by one function so tests parse the exact string.
    // mixedSourceDescription() ends in a pad reference, which accepts no
    // further assignments, so each capture element names itself `sharesrc`.
    const QString description =
        lightning::shareaudio::encodedTrackDescription(source,
                                                       nextPublishSsrc());

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
        // Stop the device monitor: m_shareAudioCid is not set yet, so no
        // other path would release it.
        m_shareAudioSources.stop();
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    gst_element_set_name(bin, cid.toUtf8().constData());
    if (!gst_bin_add(GST_BIN(m_publisher.pipeline), bin)) {
        gst_object_unref(bin);
        m_shareAudioSources.stop();   // see the parse failure above
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    m_publishedBins.insert(cid, bin);
    // Encrypted on the encoder's src pad like every other track.
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
    if (srcPad && sinkPad && !consumePublishLinkFailure())
        linked = gst_pad_link(srcPad, sinkPad);
    if (srcPad)
        gst_object_unref(srcPad);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia) << "share audio link failed code=" << linked;
        // Unregister and remove the bin and release the request pad, or the cid
        // can never be republished and the next offer carries a dead m=audio.
        releaseFailedPublishPad(sinkPad);
        m_publishedBins.remove(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        m_shareAudioSources.stop();   // see the parse failure above
        Q_EMIT failed(QStringLiteral("share_audio_failed"));
        return;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(bin);
    // Counted like every published track; unpublish() decrements for every
    // cid, and a zero count defers offers.
    ++m_publishedMedia;
    m_publisherEverPublished = true;
    if (perApplication) {
        m_shareAudioCid = cid;
        m_shareAudioSerials.clear();
        m_shareAudioBranches = 0;
        m_shareAudioNextIndex = 0;
        m_shareAudioScans = 0;
        for (const lightning::shareaudio::Stream &s : streams) {
            m_shareAudioSerials.insert(s.serial);
            ++m_shareAudioBranches;
            ++m_shareAudioNextIndex;
        }
        if (!m_shareAudioScanTimer.isActive()) {
            m_shareAudioScanTimer.setInterval(2000);
            m_shareAudioScanTimer.setSingleShot(false);
            connect(&m_shareAudioScanTimer, &QTimer::timeout, this,
                    &SfuMediaEngine::rescanShareAudioSources,
                    Qt::UniqueConnection);
            m_shareAudioScanTimer.start();
        }
    }
    qCInfo(lcSfuMedia) << "share audio published perApplication="
                       << perApplication;
}

SfuMediaEngine::PublishProbeState::~PublishProbeState()
{
    // No lock: this runs only after the last shared_ptr is gone.
    if (lastFrame)
        gst_buffer_unref(lastFrame);
    if (keepSink)
        gst_object_unref(keepSink);
}

quint64 SfuMediaEngine::keepAlivePts(quint64 sampledPts, bool sampledPtsValid,
                                     qint64 elapsedMs, quint64 lastInjectedPts,
                                     bool lastInjectedPtsValid)
{
    // No PTS, no anchor: refuse rather than guess. A still share staying still
    // is a smaller fault than a moving share dying.
    if (!sampledPtsValid)
        return GST_CLOCK_TIME_NONE;
    const quint64 elapsed =
        static_cast<quint64>(qMax<qint64>(0, elapsedMs)) * GST_MSECOND;
    quint64 at = sampledPts + elapsed;
    // Strictly increasing; videorate does not interpolate across a tie.
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

/// Drops the frame and pad a finished share was holding. Static: callers have
/// already removed the watch from the hash.
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
/// One keep-alive frame on its way to `videorate`, owned until it is chained.
struct KeepAliveInjection {
    GstPad *pad = nullptr;
    GstBuffer *buffer = nullptr;
};
}   // namespace

/// Run the keep-alive timer exactly while some watched share still holds a pad.
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

/// Gives `videorate` the second buffer a still screen never produces. A
/// PipeWire screencast delivers on damage and videorate emits nothing until a
/// second buffer arrives, so a window that does not repaint publishes no
/// video at all. See videoRateStage().
void SfuMediaEngine::tickShareKeepAlive()
{
    // Must exceed the probe's sampling throttle so a healthy share never looks
    // quiet.
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
        // Read the triple under one lock, or an old PTS could pair with a new
        // sample time.
            QMutexLocker lock(&state->keepMutex);
            if (state->lastFrame)
                sample = gst_buffer_ref(state->lastFrame);
            sampledPts = state->lastFramePts;
            sampledPtsValid = state->lastFramePtsValid;
            sampledAtMs = state->lastSampleAtMs;
        }
        if (!sample)
            continue;
        // Anchored to the sampled frame's PTS; see keepAlivePts().
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
        // gst_pad_chain() rather than gst_pad_push() from an IDLE probe, which
        // deadlocks (the IDLE probe blocks the pad the push waits on). Run via
        // gst_element_call_async so videorate and the capsfilter do not run on
        // the GUI thread.
        if (GstElement *rate = gst_pad_get_parent_element(state->keepSink)) {
            auto *carried = new KeepAliveInjection{
                GST_PAD(gst_object_ref(state->keepSink)), inject};
            gst_element_call_async(
                rate,
                [](GstElement *, gpointer data) {
                    auto *job = static_cast<KeepAliveInjection *>(data);
                    // FLUSHING means the share stopped meanwhile; ignore it.
                    gst_pad_chain(job->pad, job->buffer);
                    job->buffer = nullptr;   // chain consumed it
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
        // Rate-limited; the first one matters most.
        if (n == 1 || n % 100 == 0) {
            qCInfo(lcSfuMedia)
                << "screen share keep-alive: the capture has been quiet, "
                   "re-pushed the last picture count=" << n
                << "quietMs=" << (now - last);
        }
    }
}

/// Adds a branch for an application that started playing during a share.
/// Addition only: ended branches retire themselves via `on-disconnect=eos`,
/// so no pad is unlinked on a live pipeline.
void SfuMediaEngine::rescanShareAudioSources()
{
    if (m_shareAudioCid.isEmpty() || !m_shareAudioSources.running()) {
        m_shareAudioScanTimer.stop();
        return;
    }
    GstElement *bin = m_publishedBins.value(m_shareAudioCid, nullptr);
    if (!bin) {
        // The share audio track is gone; stop scanning.
        m_shareAudioScanTimer.stop();
        m_shareAudioCid.clear();
        m_shareAudioSerials.clear();
        m_shareAudioScans = 0;
        m_shareAudioSources.stop();
        return;
    }
    // Bounded: every app that plays leaves a parked branch behind. Past the
    // cap, stop polling altogether.
    constexpr int kMaxBranches = 24;
    if (m_shareAudioBranches >= kMaxBranches) {
        m_shareAudioScanTimer.stop();
        return;
    }
    // Warn once if a long share never found any application: the track is
    // carrying only the silence floor, which must not look like success.
    ++m_shareAudioScans;
    constexpr int kScansBeforeDoubt = 15;   // ~30 s at the 2 s interval
    if (m_shareAudioBranches == 0 && m_shareAudioScans == kScansBeforeDoubt) {
        qCWarning(lcSfuMedia)
            << "share audio: no application audio stream has been found in"
            << (kScansBeforeDoubt * 2)
            << "seconds — this share is carrying silence. Either nothing on "
               "this machine is playing, or per-application enumeration is "
               "not working here.";
    }

    GstElement *mixer = gst_bin_get_by_name(
        GST_BIN(bin),
        lightning::shareaudio::mixerElementName().toUtf8().constData());
    if (!mixer)
        return;

    const QList<lightning::shareaudio::Stream> streams =
        m_shareAudioSources.streams(QCoreApplication::applicationPid(),
                                    ourAudioClientNames());
    for (const lightning::shareaudio::Stream &s : streams) {
        if (m_shareAudioSerials.contains(s.serial))
            continue;
        if (m_shareAudioBranches >= kMaxBranches)
            break;
        // Record the serial first so a branch that cannot be built is not
        // retried every scan.
        m_shareAudioSerials.insert(s.serial);
        // A separate monotonic index, not the branch count, so element names
        // are never reused within the bin.
        const QString description = lightning::shareaudio::
            applicationBranchDescription(s, m_shareAudioNextIndex);
        if (description.isEmpty())
            continue;
        GError *error = nullptr;
        GstElement *branch = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &error);
        if (error) {
            qCWarning(lcSfuMedia)
                << "share audio: could not build a branch for a new "
                   "application:"
                << (error->message ? error->message : "?");
            g_error_free(error);
            if (branch)
                gst_object_unref(branch);
            continue;
        }
        if (!branch)
            continue;
        if (!gst_bin_add(GST_BIN(bin), branch)) {
            gst_object_unref(branch);
            continue;
        }
        GstPad *srcPad = gst_element_get_static_pad(branch, "src");
        GstPad *sinkPad = gst_element_request_pad_simple(mixer, "sink_%u");
        GstPadLinkReturn linked = GST_PAD_LINK_REFUSED;
        if (srcPad && sinkPad)
            linked = gst_pad_link(srcPad, sinkPad);
        if (srcPad)
            gst_object_unref(srcPad);
        if (linked != GST_PAD_LINK_OK) {
            // Release the request pad: an unfed audiomixer sink pad makes the
            // aggregator wait out its latency on every buffer. Safe only
            // because the link failed, so no streaming thread is inside the
            // pad; releasing a linked pad on a running pipeline can deadlock.
            if (sinkPad)
                gst_element_release_request_pad(mixer, sinkPad);
            if (sinkPad)
                gst_object_unref(sinkPad);
            qCWarning(lcSfuMedia)
                << "share audio: a new application's branch would not link,"
                << "code=" << linked;
            gst_bin_remove(GST_BIN(bin), branch);
            continue;
        }
        if (sinkPad)
            gst_object_unref(sinkPad);
        if (!gst_element_sync_state_with_parent(branch)) {
            // A branch stuck in NULL with a linked mixer pad neither produces
            // nor EOSes and stalls the aggregator; retire it.
            qCWarning(lcSfuMedia)
                << "share audio: a new application's branch would not start;"
                << "retiring it rather than leaving a silent pad";
            GstPad *stuck = gst_element_get_static_pad(branch, "src");
            GstPad *peer = stuck ? gst_pad_get_peer(stuck) : nullptr;
            if (stuck && peer)
                gst_pad_unlink(stuck, peer);
            if (peer) {
                gst_element_release_request_pad(mixer, peer);
                gst_object_unref(peer);
            }
            if (stuck)
                gst_object_unref(stuck);
            gst_element_set_state(branch, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(bin), branch);
            continue;
        }
        ++m_shareAudioBranches;
        ++m_shareAudioNextIndex;
        qCInfo(lcSfuMedia) << "share audio: added an application that started "
                              "playing mid-share app="
                           << (s.appName.isEmpty() ? s.nodeName : s.appName)
                           << "serial=" << s.serial;
    }
    gst_object_unref(mixer);
}

void SfuMediaEngine::publishAudio(const QString &cid)
{
    if (!ensurePeer(Target::Publisher) || cid.isEmpty())
        return;
    if (m_publishedBins.contains(cid))
        return;

    // `autoaudiosrc` is a bin with no device property, so honouring a
    // preference means choosing a concrete element. chooseCaptureElement()
    // returns nothing unless the element exists and can bind the device.
    // Skipped entirely when there is no preference: enumeration is synchronous
    // and possibly slow (see monitorCandidates).
    const DeviceChoice mic = m_testSources ? DeviceChoice{} : microphoneChoice();
    const lightning::calls::ElementChoice micChoice =
        mic.id.isEmpty()
            ? lightning::calls::ElementChoice{}
            : lightning::calls::chooseCaptureElement(
                  lightning::calls::CaptureKind::Microphone,
                  microphoneElementPreference(),
                  availableElements(microphoneElementPreference()),
                  mic.id, mic.description,
                  monitorCandidates("Audio/Source"));
    const QString source = m_testSources
        ? QStringLiteral(
              "audiotestsrc is-live=true wave=sine freq=440 volume=0.05 "
              "name=micsrc")
        : (micChoice.isEmpty() ? QStringLiteral("autoaudiosrc name=micsrc")
                               : QStringLiteral("%1 name=micsrc")
                                     .arg(micChoice.element));
    // The valve is the real mute: drop=true stops buffers before the encoder.
    // Opus PT 111 is what LiveKit expects.
    //
    // `webrtcdsp` provides the automatic gain control browsers apply by
    // default (about +10 dB on quiet input). Optional: it lives in
    // gst-plugins-bad and naming a missing element fails the parse, so it is
    // probed and the chain is unchanged without it. Echo cancellation is off
    // because it needs a `webrtcechoprobe` in the playback path. It processes
    // fixed 10 ms S16 chunks, hence the explicit caps on this branch only.
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

    // Level meter right before the encoder, so it measures what the far end
    // receives. Optional like `webrtcdsp`. 200 ms interval: short enough to
    // catch a single word.
    static const bool levelAvailable = [] {
        GstElementFactory *factory = gst_element_factory_find("level");
        if (!factory)
            return false;
        gst_object_unref(factory);
        return true;
    }();
    const QString levelStage =
        levelAvailable
            ? QStringLiteral("! level name=miclevel post-messages=true "
                             "interval=200000000 ")
            : QString();

    // Multi-input interfaces: averaging to mono mixes in the silent inputs
    // (-12 dB with four channels), so take input 1 instead; see
    // captureMixMatrix(). Empty for one- and two-channel devices. Limited to
    // pipewiresrc, the only element this was verified on: a wrong channel
    // pin fails negotiation and kills the microphone.
    const int captureChannels =
        m_testDeviceChannels > 0
            ? m_testDeviceChannels
            : (micChoice.element == QLatin1String("pipewiresrc")
                   ? micChoice.binding.channels
                   : 0);
    const QString channelCaps =
        lightning::calls::captureChannelCaps(captureChannels);
    const QString mixMatrix =
        lightning::calls::captureMixMatrix(captureChannels);
    if (!mixMatrix.isEmpty()) {
        qCInfo(lcSfuMedia)
            << "multi-input capture device: taking input 1 of"
            << captureChannels
            << "rather than their mean — averaging empty inputs would cost"
            << qRound(20.0 * std::log10(1.0 / captureChannels)) << "dB";
    }

    const QString description =
        QStringLiteral("%1 %7 "
                       // Bounded and leaky: a default queue holds up to one
                       // second and never drains, which becomes permanent
                       // latency. Dropping old audio beats delaying all of it.
                       "! queue max-size-buffers=0 max-size-bytes=0 "
                       "max-size-time=100000000 leaky=downstream "
                       "! audioconvert %8 ! audioresample "
                       // Mono pinned: Windows often exposes a mic as two
                       // channels with signal in only one, which the far end
                       // heard in one ear. audioconvert averages real stereo.
                       "! audio/x-raw,channels=1 "
                       "! valve name=micvalve drop=%2 "
                       "%5 "
                       // Microphone gain must be applied to raw audio before
                       // the encoder. The initial value is set here so a
                       // rebuilt bin comes up at the user's level.
                       "! volume name=micvol volume=%4 "
                       "%6 "
                       "! opusenc name=audioenc "
                       // Explicit ssrc: see nextPublishSsrc().
                       "! rtpopuspay pt=111 ssrc=%3 "
                       // A capsfilter, not a bare caps string: gst_parse reads
                       // trailing caps as an element name. webrtcbin builds the
                       // m= section from these caps: the ssrc produces
                       // `a=ssrc`/`a=msid`, and clock-rate plus encoding-params
                       // give the RFC 7587 rtpmap `opus/48000/2`.
                       "! capsfilter caps=\"application/x-rtp,media=audio,"
                       "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
                       "encoding-params=(string)2,ssrc=(uint)%3\"")
            .arg(source,
                 m_microphoneMuted ? QStringLiteral("true")
                                   : QStringLiteral("false"),
                 QString::number(nextPublishSsrc()),
                 // Locale-independent: a comma decimal would be parsed as 0.
                 QString::number(
                     audioFactorPercent(m_microphoneGain.load()) / 100.0,
                     'f', 3),
                 gainStage, levelStage, channelCaps, mixMatrix);

    qCInfo(lcSfuMedia) << "publishing microphone: valve drop="
                       << m_microphoneMuted << "device-channels="
                       << micChoice.binding.channels
                       << "dsp=" << dspAvailable << "level=" << levelAvailable;
    m_lastAudioDescription = description;

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
        // GStreamer's parse diagnostic: element and property names only.
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
    applyBindingTo(bin, "micsrc", micChoice.binding);
    resetMicLevelState();
    // Encrypt on the encoder's src pad: one whole encoded frame.
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "audioenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            installEncryptProbe(encoded, /*video=*/false);
            gst_object_unref(encoded);
        }
        gst_object_unref(encoder);
    }
    // The link result decides whether there is anything to offer.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    if (srcPad) {
        auto packets = std::make_shared<std::atomic<quint64>>(0);
        auto *rtpCtx = new RtpOutCtx{packets, false};
        gst_pad_add_probe(srcPad,
                          GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER
                                          | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                          countRtpOut, rtpCtx, rtpOutCtxFree);
        // A capture that never starts posts nothing to the bus while every
        // other indicator stays green, so warn if no RTP flows after 3 s.
        QTimer::singleShot(3000, this, [this, packets] {
            if (packets->load() > 0)
                return;
            // Read the mute state at fire time.
            if (m_microphoneMuted) {
                qCInfo(lcSfuMedia)
                    << "no RTP from the microphone after 3s — it is MUTED, "
                       "which is the valve doing its job";
                return;
            }
            qCWarning(lcSfuMedia)
                << "THE MICROPHONE PRODUCED NO RTP AT ALL after 3s while "
                   "UNMUTED: the capture chain never started, nothing was "
                   "packetised and nobody can hear this device. The call "
                   "will otherwise look completely healthy — the track is "
                   "published, the transport is connected and the SDP "
                   "carries audio.";
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
    gst_element_sync_state_with_parent(bin);
    // Now there is something to offer; the negotiation-needed fired at
    // PLAYING was ignored.
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
    // Release the request pad: a leftover webrtcbin sink pad is a transceiver
    // and puts an empty m= section in the next offer. Safe only because the
    // link failed, so no streaming thread is inside the pad; releasing a
    // linked pad on a running pipeline can deadlock.
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
    // Without `a=msid` the SFU cannot match a media section to a declared
    // track, and the track never becomes published. Browsers write the track
    // id (livekit-client's cid) themselves; we set it on the pad.
    g_object_set(sinkPad, "msid", cid.toUtf8().constData(), nullptr);
}

/// Whether to use the GPU convert-and-scale segment for a screen share.
///
/// The CPU path requests system memory, forcing a full-size readback of the
/// compositor's GPU buffer before scaling, which stalls the desktop. The GPU
/// path imports the DMA-BUF (`glupload`), converts and scales on the GPU, and
/// downloads the reduced frame.
///
/// A capture must be constrained to a LINEAR DMA-BUF: NVIDIA block-linear
/// buffers import as empty textures (a black share). See captureEntryFilter().
bool SfuMediaEngine::shareGpuScalingRequested()
{
    // On by default; `LIGHTNING_SHARE_GPU=0` forces the CPU. The ladder makes
    // this safe: elements are probed, the chain must reach PAUSED, and a
    // description that fails to build falls back once, with a log line.
    return qEnvironmentVariable("LIGHTNING_SHARE_GPU")
           != QLatin1String("0");
}

/// The first missing GL element, or empty. Probed because an unknown element
/// fails the whole parse; naming it helps find packaging bugs.
QString SfuMediaEngine::missingGpuShareElement()
{
    // Must match shareScaleStage()'s GPU branch exactly.
    return firstMissingElement({QByteArrayLiteral("glupload"),
                                QByteArrayLiteral("glcolorconvert"),
                                QByteArrayLiteral("glcolorscale"),
                                QByteArrayLiteral("gldownload")});
}

/// Whether the MJPG camera decode chain can be built. Cached; see the header.
bool SfuMediaEngine::jpegCameraChainAvailable()
{
    static const bool available = [] {
        // Only checks that the decoder exists and links; whether a camera
        // offers MJPG is answered by negotiation. `jpegenc` feeds real
        // image/jpeg so the probe actually exercises the capsfilter.
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
        // The real scale stage fed by videotestsrc: asks whether GL works,
        // not whether a desktop can be captured.
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
        // Bounded: a hanging driver counts as unusable.
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
    // Decode MJPG, then apply the same PAR pin as the raw entry (an
    // unfixated PAR overflows videoscale). videoconvert because jpegdec's
    // output layout varies.
    return QStringLiteral(
        "capsfilter caps=\"image/jpeg\" "
        "! jpegdec "
        "! videoconvert "
        "! capsfilter caps=\"video/x-raw,"
        "pixel-aspect-ratio=(fraction)1/1\"");
}

QString SfuMediaEngine::shareScaleStage(int maxHeight, bool gpu)
{
    // The flag is a parameter so both branches are testable.
    if (!gpu) {
        // One threaded pass: videoconvertscale saves ~10% CPU over two
        // elements, and n-threads=4 halves wall time so 4K keeps up with real
        // time. 8 threads cost more total CPU for little gain.
        return QStringLiteral("videoconvertscale n-threads=4");
    }

    // Constrain size inside the GL segment, or glcolorscale passes through.
    // Ranges, so nothing is upscaled.
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
                          "! video/x-raw(memory:GLMemory),"
                          "width=(int)[1,%1],height=(int)[1,%2] "
                          "! gldownload ! videoconvert")
        .arg(QString::number(w), QString::number(h));
}

QString SfuMediaEngine::shareLimitsCaps(int maxHeight, int fps)
{
    // 16:9 width, both as ranges so nothing is upscaled. PAR fixed at 1/1
    // (an unfixated PAR overflows videoscale). A fixed 60/1 behind videorate
    // negotiates fine against the portal's max-framerate=59/1.
    const int shareH = maxHeight;
    const int shareW = (shareH * 16) / 9;
    return QStringLiteral("video/x-raw,width=(int)[1,%1],"
                          "height=(int)[1,%2],"
                          "framerate=(fraction)%3/1,"
                          "pixel-aspect-ratio=(fraction)1/1")
        .arg(QString::number(shareW), QString::number(shareH),
             QString::number(fps));
}

/// The share's encoder stage: bitrate scales with the picture, keyframe
/// interval with the rate.
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

/// The CPU stage a publish falls back to; see the header.
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
    // One structure, never a list; see the header. PipeWire picks the
    // smallest mode in range, so the lower bound sets the quality floor and
    // the upper bound admits 1080p-only cameras.
    return QStringLiteral(
        "capsfilter caps=\"video/x-raw,width=(int)[640,1920],"
        "height=(int)[360,1080],pixel-aspect-ratio=(fraction)1/1\"");
}

QString SfuMediaEngine::cameraLimitsCaps(bool portal)
{
    return portal
        ? QStringLiteral("video/x-raw,width=(int)[1,1280],"
                         "height=(int)[1,720],"
                         "pixel-aspect-ratio=(fraction)1/1")
        : QStringLiteral("video/x-raw,width=(int)[1,1280],"
                         "height=(int)[1,720],"
                         "framerate=(fraction)30/1,"
                         "pixel-aspect-ratio=(fraction)1/1");
}

QString SfuMediaEngine::cameraRateStage(bool portal)
{
    // Same element and name (`vidrate`); the rate becomes a ceiling.
    return portal
        ? QStringLiteral("videorate name=vidrate skip-to-first=true "
                         "max-rate=30")
        : videoRateStage(false);
}

namespace {

/// One queue under test, and what the run measured.
struct QueueProbeResult {
    QString spec;
    bool isControl = false;
    qint64 settledMs = -1;   ///< level just before the brake goes on
    qint64 peakMs = -1;      ///< highest level while starved
    qint64 realtimeMs = -1;  ///< level with the consumer back at real time (permanent latency)
    qint64 recoveredMs = -1; ///< level after the consumer is let run free
    bool ran = false;
    QString error;
};

/// Read `current-level-time` off a queue, in milliseconds.
qint64 queueLevelMs(GstElement *queue)
{
    guint64 ns = 0;
    g_object_get(queue, "current-level-time", &ns, nullptr);
    return static_cast<qint64>(ns / GST_MSECOND);
}

/// Runs one queue through settle, starve and recover. `identity sleep-time`
/// starves the consumer while the live source keeps producing, which is how
/// a backlog forms; freezing the whole process (SIGSTOP) cannot show it.
QueueProbeResult runOneQueueProbe(const QString &spec, bool isControl)
{
    QueueProbeResult out;
    out.spec = spec;
    out.isControl = isControl;

    // 10 ms buffers from a live source: the shape a capture delivers.
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

    // Settle: 1.5 s of ordinary flow.
    out.settledMs = sampleFor(1500, nullptr);

    // Starve: 40 ms of sleep per 10 ms buffer.
    g_object_set(brake, "sleep-time", guint(40000), nullptr);
    out.peakMs = 0;
    sampleFor(2000, &out.peakMs);

    // Restore to exactly real time, all a live encoder ever gets. A consumer
    // that merely keeps up cannot drain a backlog, so this phase shows
    // whether the latency is permanent.
    g_object_set(brake, "sleep-time", guint(10000), nullptr);
    out.realtimeMs = sampleFor(3000, nullptr);

    // Then free, to tell a held backlog from one merely in flight.
    g_object_set(brake, "sleep-time", guint(0), nullptr);
    out.recoveredMs = sampleFor(3000, nullptr);

    out.ran = true;
    gst_object_unref(queue);
    gst_object_unref(brake);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return out;
}

} // namespace

int SfuMediaEngine::runQueueSelfTest(QString *report)
{
    QString text;
    QTextStream out(&text);

    // Queue specs are taken from the description production actually builds.
    const QString produced = videoPipelineDescription(
        QStringLiteral("videotestsrc is-live=true"),
        videoRateStage(/*screenShare=*/false),
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
    // State the scope in the transcript itself; release validators print it.
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
        results.append(runOneQueueProbe(spec, /*isControl=*/false));
    // Control: GStreamer's default queue under the same starvation.
    results.append(runOneQueueProbe(QStringLiteral("queue"),
                                    /*isControl=*/true));

    bool ok = true;
    bool measured = true;   ///< every probe, including the control, ran
    qint64 controlHeldMs = -1;
    for (const QueueProbeResult &r : results) {
        out << (r.isControl ? "CONTROL " : "SHIPPED ") << r.spec << "\n";
        if (!r.ran) {
            // A probe that did not run is not a failed queue.
            out << "    could not run: " << r.error << "\n\n";
            measured = false;
            continue;
        }
        if (r.isControl)
            controlHeldMs = r.realtimeMs;
        // This probe uses 10 ms buffers, so a buffer-count bound reads as far
        // less time than it holds on the video path.
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
        // Shipped bounds are 100-200 ms; the extra headroom is for scheduling.
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

    // The control is asserted: if the starvation did not happen, every row
    // reads low and a pass would demonstrate nothing.
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
               // `capsrc` is named so the capture can be counted separately
               // from the encoder: the rate stage duplicates frames, so a
               // stalled capture still yields full-rate send counters.
               // The capture queue is small and leaky: realtime video drops a
               // late frame instead of stalling the source.
               "%1 name=capsrc "
               // PAR pinned at the source too: the ceiling's PAR pin makes
               // videoconvertscale offer the source an open PAR range, and a
               // source that does not fixate PAR itself falls to the range
               // minimum (1/2147483647) and negotiation overflows. A fixed
               // value cannot be fixated to a minimum.
               "! %9 "
               "! queue max-size-buffers=4 leaky=downstream "
               "! %8 ! %7 "
               "! %2 "
               "! tee name=t %4"
               // Bounded and leaky: a default queue holds a second and never
               // drains, and this one feeds the encoder most likely to fall
               // behind. Leaking only costs frames; backpressure would stall
               // the tee, the capture and the self-view.
               "t. ! queue max-size-buffers=0 max-size-bytes=0 "
               "max-size-time=100000000 leaky=downstream "
               "! valve name=vidvalve drop=false ! %3 name=videoenc "
               // Our payloader, not rtpvp8pay; see RtpVp8Payloader.h.
               // Explicit ssrc: see nextPublishSsrc().
               "! %6 pt=96 ssrc=%5 "
               // The ssrc must be in the caps; see publishAudio().
               "! capsfilter caps=\"application/x-rtp,media=video,"
               "encoding-name=VP8,payload=96,clock-rate=(int)90000,"
               "ssrc=(uint)%5\"")
        .arg(source, limits, encoder, selfView, QString::number(ssrc),
             QLatin1String(lightning::rtp::vp8PayloaderName()), rateStage, scaleStage,
             entryFilter);
}

QString SfuMediaEngine::trackSidFromMsid(const QString &msid)
{
    // The track sid (`TR_...`), the only id that names one track on both
    // ends. LiveKit packs the msid stream id as `PA_<participant>|TR_<track>`;
    // this mirrors livekit-client's `extractTrackSid()`. The section `mid` is
    // assigned independently per connection and cannot be used for routing.
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
    // LiveKit packs "<PA_participant>|<TR_track>" into it for every protocol
    // version > 0; keep the participant part, like livekit-client's
    // `unpackStreamId()`. A leading separator yields empty, not "|TR_...".
    const int packed = streamId.indexOf(QLatin1Char('|'));
    if (packed >= 0)
        streamId.truncate(packed);
    return streamId;
}

QString SfuMediaEngine::localCameraStreamId()
{
    // A colon means it can never collide with a LiveKit sid.
    return QStringLiteral("local:camera");
}

QString SfuMediaEngine::localScreenStreamId()
{
    // LiveKit ids are "PA_..."/"TR_..."; the colon guarantees no collision.
    return QStringLiteral("local:screen");
}

bool SfuMediaEngine::elementAvailable(const char *name)
{
    if (!name || !*name)
        return false;
    // Asking before gst_init would report every element absent; the shared
    // bootstrap also applies the bundled plugin path (GstBootstrap.h).
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
    // gdiscreencapsrc: the d3d11 and mediafoundation plugins do not load with
    // this mingw-w64 toolchain, so d3d11screencapturesrc is not shipped. Its
    // property names differ (`monitor-index`/`show-cursor` vs `monitor`/
    // `cursor`), so do not mix them up.
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    // A single window uses our own element: gdiscreencapsrc cannot capture a
    // window, and cropping the screen would share whatever overlaps it. See
    // WindowCaptureSrc.h.
    if (windowHandle != 0) {
        return QStringLiteral("%1 hwnd=%2")
            .arg(QLatin1String(lightning::wincap::windowCaptureSrcName()))
            .arg(windowHandle);
    }
    return QStringLiteral("gdiscreencapsrc monitor=%1 cursor=true")
        .arg(nodeId < 0 ? 0 : nodeId);
#elif defined(Q_OS_MACOS)
    // `capture-screen` switches avfvideosrc from camera to display;
    // `device-index` selects the display.
    Q_UNUSED(pipewireFd);
    Q_UNUSED(captureRect);
    return QStringLiteral("avfvideosrc capture-screen=true "
                          "capture-screen-cursor=true device-index=%1")
        .arg(nodeId < 0 ? 0 : nodeId);
#else
    // No portal: the user picked a rectangle of the X11 root window. Reached
    // only on X11 sessions. Never use this on Wayland: XWayland's root window
    // is black, so it would send a black rectangle with healthy counters.
    //
    // `endx`/`endy` are inclusive, so QRect::right()/bottom() are correct.
    // `use-damage=false`: XDamage can miss redraws on a composited root, and
    // a full grab per frame costs nothing measurable here.
    // `do-timestamp=true` is inert on ximagesrc (it stamps its own zero-based
    // PTS, which is what an element added to a running pipeline needs).
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
    // Never inherit the plugin's min-buffers default; see above.
    return QStringLiteral("pipewiresrc path=%1 min-buffers=1 do-timestamp=true")
        .arg(nodeId);
#endif
}

QString SfuMediaEngine::cameraSource(int pipewireFd)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Sandboxed route: a camera granted by the xdg Camera portal, taken only
    // when a descriptor was actually granted.
    //
    // `fd` and no `path`: the Camera portal grants a whole remote containing
    // only camera nodes, and `autoconnect` selects one. `min-buffers=1` for
    // the reason given in screenShareSource(). `do-timestamp=true` mirrors the
    // screen share; running-time stamps are handled by skip-to-first in
    // videoRateStage().
    if (pipewireFd >= 0) {
        return QStringLiteral(
                   "pipewiresrc fd=%1 min-buffers=1 do-timestamp=true")
            .arg(pipewireFd);
    }
#else
    Q_UNUSED(pipewireFd);
#endif
#if defined(Q_OS_WIN)
    // ksvideosrc: mfvideosrc is newer, but the mediafoundation plugin does
    // not load with this toolchain. Kernel Streaming covers ordinary UVC
    // webcams.
    return QStringLiteral("ksvideosrc");
#elif defined(Q_OS_MACOS)
    // The same element the screen branch uses, without capture-screen.
    return QStringLiteral("avfvideosrc");
#else
    // `v4l2src`, not `autovideosrc`: on a PipeWire desktop autodetect picks
    // pipewiresrc by rank, with no target, and the camera never delivers.
    return QStringLiteral("v4l2src");
#endif
}

void SfuMediaEngine::publishVideo(const QString &cid, bool screenShare,
                                  int nodeId, int pipewireFd,
                                  quint64 windowHandle,
                                  const QRect &captureRect)
{
    // The fd belongs to this engine now and is held for the life of the
    // publishing bin (closed by unpublish()): whether pipewiresrc dups it
    // depends on the plugin version. RAII so every early return closes it;
    // ownership moves to m_publishedFds on success.
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
    if (m_testSources) {
        source = QStringLiteral("videotestsrc is-live=true pattern=smpte");
    } else if (screenShare) {
        // Screen capture goes through PipeWire with the node id from the
        // portal ScreenCast session; a negative id would publish whatever
        // PipeWire picks, so it is refused unless a window handle or an X11
        // rectangle was given instead. This guard has a twin in
        // SfuCallController::startScreenShare; keep them in sync.
        if (nodeId < 0 && windowHandle == 0 && !captureRect.isValid()) {
            Q_EMIT failed(QStringLiteral("screen_share_no_source"));
            return;
        }
        source = screenShareSource(nodeId, pipewireFd, windowHandle,
                                   captureRect);
    } else {
        // `v4l2src`, not `autovideosrc`: autodetect picks `pipewiresrc` by
        // rank on a PipeWire desktop, with no target, and with the pinned
        // 30/1 rate that fails to negotiate, so the camera never delivers.
        // v4l2src with the same caps works. Relaxing the rate range does not
        // help. Or a remote granted by the xdg Camera portal; see
        // cameraSource().
        source = cameraSource(pipewireFd);
        // Log which route the camera took, so a failed portal request is
        // distinguishable from a broken camera.
        qCInfo(lcSfuMedia) << "camera source="
                           << (pipewireFd >= 0 ? "xdg camera portal "
                                                 "(pipewiresrc)"
                                               : "direct device");
    }

    // Ceilings matching livekit-client's presets: screen share h1080fps30
    // (user-adjustable), camera h720. Sizes are ranges so videoscale picks the
    // largest fit and never upscales.
    //
    // The framerate is fixed, not a range: a desktop capture negotiates 0/1
    // (delivers on damage), and a range including it leaves videorate and
    // vp8enc with no target rate.
    //
    // PAR is pinned to 1/1 so videoscale preserves aspect by choosing a size
    // rather than signalling a non-square PAR, which VP8 and RTP drop (an
    // ultrawide would arrive squashed).
    //
    // A portal camera uses its own entry; see portalCameraEntry().
    const bool portalCamera = !screenShare && pipewireFd >= 0;
    const QString limits = screenShare
        ? shareLimitsCaps(m_shareMaxHeight, m_shareFps)
        : cameraLimitsCaps(portalCamera);
    const QString encoder = screenShare
        ? shareEncoderStage(m_shareMaxHeight, m_shareFps)
        : QStringLiteral("vp8enc deadline=1 lag-in-frames=0 threads=4 "
                         "cpu-used=2 static-threshold=0 "
                         "keyframe-max-dist=30 "
                         "end-usage=cbr target-bitrate=1700000");
    // Self-view branch, tee'd after the scaler so it costs one convert of the
    // downscaled frame. For a share it is the only way to see what is sent;
    // for a camera, our own track is never received, so there would be no
    // preview otherwise. `max-buffers=1 drop=true` keeps a slow preview from
    // adding latency to the published branch.
    const QString selfView = !m_testSources
        ? QStringLiteral("t. ! queue max-size-buffers=2 leaky=downstream "
                         "! videoconvert ! video/x-raw,format=RGBA "
                         "! appsink name=selfvidsink emit-signals=true "
                         "sync=false max-buffers=1 drop=true ")
        : QString();
    const QString scaleStage = screenShare
        ? shareScaleStage(m_shareMaxHeight, shareGpuScalingRequested())
        : QStringLiteral("videoconvert ! videoscale");
    // The CPU fallback stage is per path: the camera keeps
    // `videoconvert ! videoscale`; the threaded pass is share-sized policy.
    const QString cpuScaleStage =
        cpuFallbackScaleStage(screenShare, m_shareMaxHeight);
    // GPU first, CPU fallback. Missing elements (a packaging problem) and a
    // chain that will not build (a caps or driver problem) log differently.
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
    // Elements existing is not the same as working (no GL context, headless
    // session): gpuShareChainUsable() requires PAUSED once per process,
    // before a capture is opened.
    if (useGpu && !gpuShareChainUsable()) {
        qCInfo(lcSfuMedia)
            << "screen share falling back to the CPU: the GL chain is "
               "present but cannot run on this machine";
        useGpu = false;
    }

    QString scaleStageInUse = useGpu ? scaleStage : cpuScaleStage;

    // Cameras try MJPG first: raw YUY2 at 720p30 exceeds USB 2.0 and
    // negotiates down to ~10 fps. Falls back to raw when the description
    // fails to parse.
    //
    // Not for a portal camera: a raw-only device fails to negotiate (not to
    // parse) behind an image/jpeg filter, so the ladder cannot catch it, and
    // this pipewiresrc does not move past an unsatisfiable caps alternative.
    // The portal route uses a raw size range with a rate ceiling
    // (portalCameraEntry()); MJPG there needs the device modes enumerated.
    const bool tryJpeg =
        !screenShare && pipewireFd < 0 && jpegCameraChainAvailable();
    QString entryInUse = tryJpeg      ? cameraJpegEntry()
                         : portalCamera ? portalCameraEntry()
                                        : captureEntryFilter(useGpu);
    if (!screenShare) {
        // Log which chain the camera is on; the negotiated caps line alone
        // cannot say why a camera ended up at a low rate.
        qCInfo(lcSfuMedia) << "camera chain="
                           << (tryJpeg        ? "mjpg"
                               : portalCamera ? "portal-raw-range"
                                              : "raw")
                           << "(jpeg elements"
                           << (jpegCameraChainAvailable() ? "present"
                                                          : "absent")
                           << ")";
    }
    QString description = videoPipelineDescription(
        source, screenShare ? videoRateStage(true) : cameraRateStage(portalCamera),
            limits, encoder, selfView,
        nextPublishSsrc(), scaleStageInUse, entryInUse);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error && tryJpeg) {
        // The MJPG description did not build; fall back to the raw entry.
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
            source, screenShare ? videoRateStage(true) : cameraRateStage(portalCamera),
            limits, encoder, selfView,
            nextPublishSsrc(), scaleStageInUse, entryInUse);
        bin = gst_parse_bin_from_description(description.toUtf8().constData(),
                                             TRUE, &error);
    }
    if (error && useGpu) {
        // The GPU description did not build; log why and retry once on the
        // CPU instead of failing the share.
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
        // Logged after the ladder so it reports the outcome, not the intent.
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
    // Ownership moved: releasePublishedFd() closes it from here on.
    portalFd.release();
    // Shared by the probes below and handlePublishError(); created before the
    // bin can play.
    auto probeState = std::make_shared<PublishProbeState>();
    probeState->startedMs = monotonicMs();
    probeState->screenShare = screenShare;
    m_publishWatch.insert(cid, PublishWatch{probeState, false});
    // Count what the capture itself produces: everything else is downstream
    // of the rate stage, which manufactures frames. handlePublishError() also
    // keys on it (zero plus a bus error means the publish never prerolled).
    //
    // Apply the user's camera choice, resolved against GStreamer's device
    // monitor (see CaptureDeviceSelection.h), before the bin plays. Not for
    // shares (the portal dialog chooses) and not for portal cameras: the
    // stored choice names host devices, while the portal's pipewiresrc is on
    // a different remote where those ids do not exist.
    if (const DeviceChoice camera = cameraChoice();
        !screenShare && pipewireFd < 0 && !camera.id.isEmpty()) {
        applyBindingTo(bin, "capsrc",
                       lightning::calls::resolveDeviceBinding(
                           lightning::calls::CaptureKind::Camera,
                           cameraSource(), camera.id, camera.description,
                           monitorCandidates("Video/Source")));
    }
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
                        // Log the negotiated caps once. A `memory:DMABuf`
                        // feature here is a classic reason a capture stops
                        // after one buffer.
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
            // Retire the track when the capture ends itself (e.g. the shared
            // window closed, which posts EOS). Routes into the Stop path, which
            // sets the transceiver inactive and clears the far end's tile.
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
    // Sample what reaches `videorate` so the keep-alive timer has something
    // to re-push. Probed on its upstream peer (a src pad), after scaling and
    // after any DMABuf download, so a copy is small and holds no pool buffer.
    // Screen share only: a camera delivers on a clock.
    if (screenShare) {
        {
            if (GstPad *rateSink = keepAliveInjectionPad(bin)) {
                // Injection chains into the sink pad; sampling happens on its
                // peer so the probe never sees injected frames and mistakes a
                // dead capture for a live one.
                probeState->keepSink = rateSink;   // ref moves to the state
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
                            // Throttled to five samples a second: the sample
                            // is only read once the source goes quiet.
                            const qint64 sampled = state->lastSampleMs.load();
                            if (sampled >= 0 && now - sampled < 200)
                                return GST_PAD_PROBE_OK;
                            // Deep copy: a shallow copy would hold the
                            // source's pool memory, which stalls the capture.
                            GstBuffer *kept = gst_buffer_copy_deep(buffer);
                            if (!kept)
                                return GST_PAD_PROBE_OK;
                            state->lastSampleMs.store(now);
                            GstBuffer *old = nullptr;
                            {
                                QMutexLocker lock(&state->keepMutex);
                                old = state->lastFrame;
                                state->lastFrame = kept;
                                // Keep the source's PTS: the only timebase the
                                // injection may use; see keepAlivePts().
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
        g_signal_connect_data(selfSink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(selfSink);
    }
    if (GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "videoenc")) {
        if (GstPad *encoded = gst_element_get_static_pad(encoder, "src")) {
            // Time from first captured to first encoded frame: what the rate
            // stage held at share start. One-shot; the probe removes itself.
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
    // The link result decides whether there is anything to offer.
    GstPad *srcPad = gst_element_get_static_pad(bin, "src");
    // RTP leaving the bin, as for the microphone: `frames encrypted` counts
    // only what the encoder produced.
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
        // The keep-alive timer was armed before the link; disarm it too.
        updateShareKeepAliveTimer();
        releasePublishedFd(cid);
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        Q_EMIT failed(QStringLiteral("publish_link_failed"));
        return;
    }
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_element_sync_state_with_parent(bin);
    // Now there is something to offer; the negotiation-needed fired at
    // PLAYING was ignored.
    ++m_publishedMedia;
    m_publisherEverPublished = true;
    renegotiatePublisher();
}

void SfuMediaEngine::releasePublishedFd(const QString &cid)
{
    const int fd = m_publishedFds.take(cid);
    if (fd > 0)
        ::close(fd);
}

namespace {

/// One deferred publish-bin teardown. Owns a pipeline ref; the bin is kept
/// alive by gst_element_call_async.
struct PublishTeardown {
    SfuMediaEngine *engine = nullptr;
    GstElement *pipeline = nullptr;
    /// The webrtcbin and request pad this bin published through; refs owned
    /// here and released in the async step.
    GstElement *webrtc = nullptr;
    GstPad *peer = nullptr;
    QString cid;
    /// m_generation when armed, so a completion after stop()/start() cannot
    /// renegotiate the new session.
    quint64 generation = 0;
    /// Shared with the engine, which waits (bounded) on it; must stay
    /// decrementable from a GStreamer thread after the engine is gone.
    std::shared_ptr<std::atomic<int>> outstanding;
};

/// Receive-side counterpart of PublishTeardown. No request pad or transceiver
/// to retire: the far end ended the track.
struct ReceiveTeardown {
    GstElement *pipeline = nullptr;
    /// Shared with the engine; see PublishTeardown::outstanding.
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

void publishTeardownFree(gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    if (ctx->peer)
        gst_object_unref(ctx->peer);
    if (ctx->webrtc)
        gst_object_unref(ctx->webrtc);
    if (ctx->pipeline)
        gst_object_unref(ctx->pipeline);
    // Last, after every ref is released: the engine's wait ends at zero.
    if (ctx->outstanding)
        ctx->outstanding->fetch_sub(1);
    delete ctx;
}

/// Runs on a GStreamer thread-pool thread, not a streaming thread, so it may
/// change state.
void publishTeardownAsync(GstElement *bin, gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    // Unparent first so the bin quiesces with no running parent.
    gst_bin_remove(GST_BIN(ctx->pipeline), bin);
    gst_element_set_state(bin, GST_STATE_NULL);

    // The transceiver was already retired synchronously in unpublish().
    // Engine state is touched only on the GUI thread; marshal() drops the call
    // if the engine is gone.
    SfuMediaEngine *engine = ctx->engine;
    const QString cid = ctx->cid;
    const quint64 generation = ctx->generation;
    marshal(engine, [engine, cid, generation] {
        // Only once the element is at NULL, since the fd carries its PipeWire
        // connection. The generation guards against a closed call.
        engine->noteTeardownComplete(cid, generation);
    });
}

/// The pad is idle here, so nothing holds the stream lock the state change
/// needs.
GstPadProbeReturn publishTeardownProbe(GstPad *pad, GstPadProbeInfo *,
                                       gpointer data)
{
    auto *ctx = static_cast<PublishTeardown *>(data);
    GstElement *bin = gst_pad_get_parent_element(pad);
    if (!bin) {
        // The probe has no destroy notify, so this branch owns ctx.
        publishTeardownFree(ctx);
        return GST_PAD_PROBE_REMOVE;
    }
    // Ownership of ctx moves to gst_element_call_async. No destroy notify on
    // the probe: it would run on removal, below, and free ctx too early.
    gst_element_call_async(bin, publishTeardownAsync, ctx,
                           publishTeardownFree);
    gst_object_unref(bin);
    // Remove: the teardown must fire exactly once.
    return GST_PAD_PROBE_REMOVE;
}

} // namespace

void SfuMediaEngine::unpublish(const QString &cid)
{
    // Take first: errors the bin posts on its way to NULL must not be
    // reported as a publish failure.
    GstElement *bin = m_publishedBins.take(cid);
    if (const auto dead = m_publishWatch.take(cid); dead.state)
        releaseKeepAlive(dead.state);
    updateShareKeepAliveTimer();
    if (cid == m_shareAudioCid) {
        // Stop the device scan and release the monitor's PipeWire connection.
        m_shareAudioScanTimer.stop();
        m_shareAudioCid.clear();
        m_shareAudioSerials.clear();
        m_shareAudioBranches = 0;
        m_shareAudioNextIndex = 0;
        m_shareAudioScans = 0;
        m_shareAudioSources.stop();
    }
    if (!bin || !m_publisher.pipeline) {
        releasePublishedFd(cid);
        return;
    }
    // Accounting is synchronous; only the GStreamer wind-down is deferred.
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
        // No src pad, nothing in flight: the direct path is safe.
        gst_element_set_state(bin, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_publisher.pipeline), bin);
        releasePublishedFd(cid);
        renegotiatePublisher();
        return;
    }

    // Counted from here until publishTeardownFree(); stop() and the
    // destructor wait on it (bounded).
    m_pendingTeardowns->fetch_add(1);
    auto *ctx = new PublishTeardown{
        this, GST_ELEMENT(gst_object_ref(m_publisher.pipeline)),
        /*webrtc=*/nullptr, /*peer=*/nullptr, cid,
        m_generation.load(), m_pendingTeardowns};
    // Retire the transceiver now, on this thread. The far end only learns the
    // track ended through the renegotiated offer (otherwise it freezes on the
    // last frame and the next share adds another m-line). Unlinking also makes
    // the in-flight push return NOT_LINKED, which lets the IDLE probe below
    // fire at all.
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
            // Element and pad names only.
            qCInfo(lcSfuMedia) << "publish transceiver retired pad="
                               << (padName ? padName : "?");
            g_free(padName);
            gst_object_unref(webrtc);
        }
        gst_object_unref(peer);
    }

    // No destroy notify: publishTeardownProbe hands ctx on.
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
    // A completion from a closed session must not touch the next one: it
    // could close a re-registered fd and make the new publisher offer with no
    // media section, which LiveKit answers with Leave(STATE_MISMATCH).
    if (generation != m_generation.load()) {
        qCInfo(lcSfuMedia)
            << "publish teardown completed for a closed session; dropped";
        return;
    }
    releasePublishedFd(cid);
    // Only now; offering while the bin was attached would describe a track on
    // its way out.
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
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, promiseCtxNew(this, m_publisher.webrtc, true),
        promiseCtxFree);
    g_signal_emit_by_name(m_publisher.webrtc, "create-offer", nullptr,
                          promise);
}

void SfuMediaEngine::awaitPublishTeardowns()
{
    // Bounded; runs entirely on GStreamer threads, so no event loop is needed.
    // Required because publishTeardownAsync unparents a bin before stopping
    // it, leaving it running out of destroyPeer()'s reach while its crypto
    // probes point into this engine.
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
        // If this appears, the IDLE probe is not firing.
        qCWarning(lcSfuMedia)
            << "publish teardown did not finish within its budget; still"
            << left << "outstanding after" << kTeardownWaitMs << "ms";
    }
}

namespace {

/// Media-section index -> stream id, mid and track sid, read from each
/// section's `msid` and `mid`, as livekit-client does.
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
                // The participant sid; one per sender, so it cannot tell a
                // camera from a screen share.
                streamId = SfuMediaEngine::participantIdFromMsid(value);
                // The track sid names one track on both ends.
                trackSid = SfuMediaEngine::trackSidFromMsid(value);
            } else if (key == QLatin1String("mid")) {
                // The section's mid, which LiveKit also states on TrackInfo.
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
    // GStreamer's SDP parser accepts non-SDP text as an empty message, so the
    // section count is the real check.
    if (!parsed || gst_sdp_message_medias_len(message) == 0) {
        gst_sdp_message_free(message);
        // Never log the SDP text: it carries host IPs.
        Q_EMIT failed(QStringLiteral("bad_remote_sdp"));
        return;
    }
    // Log the section count only. Fewer sections than offered means the SFU
    // refused a track, which is otherwise invisible.
    qCInfo(lcSfuMedia) << "remote description applied kind=" << kind
                       << "target=" << static_cast<int>(target)
                       << "sections=" << gst_sdp_message_medias_len(message);
    if (!qEnvironmentVariableIsEmpty("LIGHTNING_SDP_TRACE")) {
        qCInfo(lcSfuMedia) << "  sdp codecs (remote" << kind << "target"
                           << static_cast<int>(target)
                           << "):" << sdpCodecSummary(sdp);
    }
    // Record the subscriber's stream ids before set-remote-description:
    // pad-added can fire from inside that call.
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

    // Apply candidates that arrived before the description; the SFU trickles
    // immediately, so dropping them would kill the call.
    for (const QString &pending : peer.pendingCandidates) {
        QString line;
        int mline = 0;
        if (parseCandidateInit(pending, &line, &mline)) {
            g_signal_emit_by_name(peer.webrtc, "add-ice-candidate", mline,
                                  line.toUtf8().constData());
        }
    }
    peer.pendingCandidates.clear();

    // The server offers on the subscriber and expects our answer.
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
        // Bounded, in case the peer never negotiates.
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
    // Log valve changes: a muted and a stalled capture are otherwise the same
    // silence, since the track, transport and SDP all stay healthy.
    if (m_microphoneMuted != muted) {
        qCInfo(lcSfuMedia) << "microphone valve drop=" << muted;
        // Reset the silence judgement on the transition: `level` sits after
        // the valve, so a muted capture posts no levels at all.
        m_micSilentSinceMs = -1;
        if (m_micSilentAnnounced) {
            m_micSilentAnnounced = false;
            Q_EMIT localAudioSilent(false, m_micPeakDb);
        }
    }
    m_microphoneMuted = muted;
    if (!m_publisher.pipeline)
        return;
    // drop=true on each bin's `micvalve` discards buffers before the encoder.
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
    // Stored on the user scale; see audioFactorPercent().
    const int clamped = percent < 0 ? 0 : (percent > 200 ? 200 : percent);
    m_microphoneGain.store(clamped);
    if (!m_publisher.pipeline)
        return;
    const gdouble factor = audioFactorPercent(clamped) / 100.0;
    // Matched by our element name, not the factory: `autoaudiosrc` may
    // contain a volume element of its own. Video bins simply have no match.
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
    // Log how many elements the gain reached, so a control that goes nowhere
    // is visible. qCInfo so a `*.debug=false` run still sees it; the GUI call
    // suite reads this line.
    if (elements > 0) {
        qCInfo(lcSfuMedia) << "microphone gain applied: percent=" << clamped
                           << "gst=" << factor << "elements=" << elements;
    } else {
        // A publisher without `micvol` means the gain is going nowhere.
        qCWarning(lcSfuMedia)
            << "microphone gain had nowhere to land: percent=" << clamped
            << "publishedBins=" << m_publishedBins.size();
    }
}

void SfuMediaEngine::setOutputMuted(bool muted)
{
    m_outputMuted.store(muted);
    if (!m_subscriber.pipeline)
        return;
    // Matched by our element name, never the volume factory: autoaudiosink
    // may contain a volume element of its own.
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
            // Resync happens exactly while a remote track is being added;
            // bounded restart.
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
    // 100..200 -> 100..1000 linearly, so 200 hits the element's ceiling.
    return 100 + (user - 100) * 9;
}

QString SfuMediaEngine::volumeKeyFor(const QString &streamId,
                                    const QString &trackKey)
{
    // Per track, not per participant: a sharer publishes microphone and
    // desktop audio, and a by-name lookup returns only the first match. Falls
    // back to the stream id when the track is unknown.
    return trackKey.isEmpty() ? streamId
                              : (streamId + QLatin1Char('_') + trackKey);
}

QString SfuMediaEngine::outputVolumeElementName(const QString &streamId)
{
    // Single source of the element name for creation and lookup. The
    // `outvol` prefix is load-bearing: deafen matches it across all receive
    // bins. Non-alphanumerics become '_' (the `mline:N` fallback id has a
    // ':').
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
    // Every audio track the participant publishes.
    setTrackVolume(streamId, QString(), percent);
}

void SfuMediaEngine::setTrackVolume(const QString &streamId,
                                    const QString &trackKey, int percent)
{
    if (!m_subscriber.pipeline || streamId.isEmpty())
        return;
    // 0..1000%: above unity is real amplification; the `volume` element's
    // own range ends at 10x.
    const double volume = audioFactorPercent(percent) / 100.0;
    // Receive volume elements are named per stream, so this is local only and
    // sends nothing. The target name is kept for the diagnostic below.
    const QString target =
        trackKey.isEmpty() ? outputVolumeElementName(streamId)
                           : outputVolumeElementName(
                                 volumeKeyFor(streamId, trackKey));

    // A named track moves that track alone; an empty key moves every audio
    // track of the participant.
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
    // Announce a landing positively; the absence of the warning below is not
    // proof. Names and counts only. Logged on change per key so dragging a
    // slider cannot flood.
    const auto announce = [&](const QString &key, int elements) {
        // Before the rate limit: a later miss must be reported again even if
        // the landed value equals the last one.
        m_volumeMissWarned.remove(key);
        if (m_volumeAppliedLog.value(key, -1) == percent)
            return;
        m_volumeAppliedLog.insert(key, percent);
        // qCInfo: the GUI call suite reads this line and may run with
        // `*.debug=false`.
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
        // Iterate: gst_bin_get_by_name returns only the first match and a
        // sharer has two elements.
        bool any = false;
        int applied = 0;
        const QString prefix =
            outputVolumeElementName(streamId);
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_subscriber.pipeline));
        GValue item = G_VALUE_INIT;
        // Handle RESYNC: the pipeline changes exactly when a track is being
        // added. Bounded restart, as in setOutputMuted().
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
                // Restart from scratch; setting the volume twice is harmless.
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

    // Nowhere to land: the receive bin may not exist yet. Remember the value
    // so applyPendingTrackVolume() applies it when the bin is built, and log
    // once per key with the wanted name and the elements that do exist (zero
    // means no audio bin yet; non-zero means a naming mismatch).
    const QString pendingKey =
        trackKey.isEmpty() ? streamId : volumeKeyFor(streamId, trackKey);
    m_pendingTrackVolume.insert(pendingKey, percent);
    if (m_volumeMissWarned.contains(pendingKey))
        return;
    m_volumeMissWarned.insert(pendingKey);
    // Names only, no content.
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
    // Apply the participant-level value first so a per-track choice wins.
    // The applied-log is cleared too: this is a newly built element, and the
    // landing line must be logged again.
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
    // A resync restarts the sweep rather than ending it; bounded, as in
    // setOutputMuted().
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
    // A resync restarts the sweep; see receiveMutedForTest().
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
    // GStreamer thread: walk the reply here, then marshal plain numbers to the
    // engine's thread.
    auto *ctx = static_cast<PromiseCtx *>(userData);
    // Read everything from ctx before the unref: on the last reference the
    // destroy notify frees it.
    SfuMediaEngine *engine = ctx->engine;
    StatsWalk walk;
    walk.peer = ctx->publisher ? QStringLiteral("pub") : QStringLiteral("sub");
    if (const GstStructure *reply = gst_promise_get_reply(promise))
        gst_structure_foreach(reply, collectStat, &walk);
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    if (walk.out.isEmpty())
        return;
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

int SfuMediaEngine::adoptedOutboundKeyIndexForTest() const
{
    return m_sendCryptor->currentKeyIndex();
}

void SfuMediaEngine::setOutboundKey(int index, const QByteArray &rawKey,
                                    bool adopt)
{
    if (!m_sendCryptor->setKey(index, rawKey))
        return;
    // The key is kept in the ring either way; `adopt` decides which index our
    // frames use.
    if (adopt)
        m_sendCryptor->setCurrentKeyIndex(index);
    // Set ready only once a key is installed. A non-adopting install leaves
    // the flag alone: the previously adopted key is still usable.
    if (adopt)
        m_sendKeyReady.store(true);
}

void SfuMediaEngine::setInboundKey(const QString &senderName, int index,
                                   const QByteArray &rawKey)
{
    // Log accepted and refused keys (never the key itself): "never arrived"
    // and "arrived and was discarded" otherwise look identical.
    if (senderName.isEmpty()) {
        qCWarning(lcSfuMedia) << "call diagnosis: a media key was DISCARDED "
                                 "because it named no ring — index=" << index;
        return;
    }
    // Created on first sight, so a key may precede its sender's track; the
    // reverse order is handled in pad-added.
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
    // A separate bounded record, so a rotating sender cannot use up the
    // other diagnoses.
    if (noteKeyArrival(senderName, index)) {
        qCInfo(lcSfuMedia) << "call diagnosis: a media key ARRIVED and was "
                              "installed for ring="
                           << printableRing(senderName)
                           << "index=" << index;
    }
}

/// Whether to log a key arrival: a new index for this ring, the first eight
/// per ring, then every sixteenth. A re-install of the same index is silent.
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

/// One diagnosis per subject per call, so repeated conclusions do not flood
/// the log. Cleared with the session.
bool SfuMediaEngine::noteDiagnosisOnce(const QString &subject)
{
    QMutexLocker lock(&m_diagnosedMutex);
    // Bounded: subjects are built from SFU-supplied names.
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
        // Warn once: the caller runs per frame.
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
        // Refused bindings leave a keyed ring and an attributed track that
        // never meet, so nothing decrypts. Warn once per pair, naming which
        // check failed.
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
    // Point both names at one shared ring rather than recording a
    // redirection: a ring may already exist under either name, in either
    // arrival order, and a redirect would strand one of them.
    auto byName = m_recvCryptors.constFind(senderName);
    auto byStream = m_recvCryptors.constFind(streamId);
    if (byName != m_recvCryptors.cend()
        && byStream != m_recvCryptors.cend()
        && byName.value() == byStream.value()) {
        return;
    }
    // Prefer the sender-named ring: that is where media keys were installed.
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
    // True only when a send key is installed; reported to the user.
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
        // Per-sender rings belong to the ending call. A probe still holding
        // one sees an empty ring and drops.
        for (const auto &cryptor : std::as_const(m_recvCryptors))
            cryptor->clearKeys();
        m_recvCryptors.clear();
        m_streamForMline.clear();
        m_midForMline.clear();
        // Cleared together with its siblings: section mids repeat across
        // calls, so a stale entry would route a track to the wrong surface.
        m_trackForMline.clear();
    }
    m_sendKeyReady.store(false);
    m_recvKeyReady.store(false);
    // The server-injected trailer belongs to the current SFU session.
    setServerInjectedTrailer(QByteArray());
}

void SfuMediaEngine::setServerInjectedTrailer(const QByteArray &trailer)
{
    // SFU input: only a base62 trailer of LiveKit's shape arms detection;
    // anything else disarms. A short trailer could match real frames.
    const bool usable = CallFrameCryptor::isUsableServerTrailer(trailer);
    {
        QMutexLocker lock(&m_sifMutex);
        const QByteArray next = usable ? trailer : QByteArray();
        if (next == m_sifTrailer)
            return;
        m_sifTrailer = next;
    }
    // Only the length and last byte are logged; the trailer is a per-room
    // constant, and the last byte ties it to `bad-iv-length keyIndex= N`.
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
    for (auto it = byMline.cbegin(); it != byMline.cend(); ++it) {
        // Absent or absurd values leave the section unattributed, which drops
        // rather than mis-decrypts.
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
    // The engine owns the send cryptor for its whole life; a non-owning alias
    // suffices.
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
    // The stream id, not a ring (see cryptoProbe()). The ring is created now
    // so a key arriving later has somewhere to land.
    ctx->streamId = streamId;
    recvCryptorFor(streamId);
    ctx->encrypting = false;
    ctx->video = video;
    ctx->required = &m_encryptionRequired;
    // Not consulted on receive (cryptoProbe() asks the ring, since any
    // sender's key sets this flag); assigned only to keep both contexts the
    // same shape.
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
    // Generation first, so a reused element address from an old session
    // cannot match.
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
        // Log the drop: a dropped offer and one never created look the same,
        // and LiveKit fails the join after 60 s either way.
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
    // Summarise the subscriber answer per section (mid, kind, direction, or
    // REJECTED for port 0), to see whether we actually accepted the offered
    // media. No user content.
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
                // Port 0 in field 1 rejects the section.
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

void SfuMediaEngine::resetMicLevelState()
{
    const bool wasAnnounced = m_micSilentAnnounced;
    m_micSilentSinceMs = -1;
    m_micSilentAnnounced = false;
    m_micPeakDb = 0;
    m_micLastLogMs = 0;
    // A new capture starts unjudged; clear any shown notice.
    if (wasAnnounced)
        Q_EMIT localAudioSilent(false, 0);
}

void SfuMediaEngine::handleMicLevel(double peakDb)
{
    // Monotonic: these are durations, and a clock step would fire or suppress
    // the silence warning.
    static QElapsedTimer monotonic;
    if (!monotonic.isValid())
        monotonic.start();
    handleMicLevelAt(peakDb, monotonic.elapsed());
}

void SfuMediaEngine::handleMicLevelAt(double peakDb, qint64 nowMs)
{
    // -350 dBFS is `level`'s floor for true digital silence, not a parse
    // failure; it must reach the silence detector.
    m_micPeakDb = peakDb;

    // Muted is silent by request: clear any pending silence judgement.
    if (m_microphoneMuted) {
        m_micSilentSinceMs = -1;
        if (m_micSilentAnnounced) {
            m_micSilentAnnounced = false;
            Q_EMIT localAudioSilent(false, peakDb);
        }
        return;
    }

    // Log the level every 5 s whatever it says. A dBFS peak of the user's own
    // device is not content.
    if (m_micLastLogMs == 0 || nowMs - m_micLastLogMs >= 5000) {
        m_micLastLogMs = nowMs;
        qCInfo(lcSfuMedia)
            << "microphone level peak=" << qRound(peakDb) << "dBFS";
    }

    m_micSilentSinceMs = micSilenceSince(peakDb, m_micSilentSinceMs, nowMs);
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

void SfuMediaEngine::handlePublishError(const QString &cid)
{
    // Runs on the GUI thread, where the state is safe to read.
    if (!m_active)
        return;
    // Teardown errors cannot reach here: unpublish() removes the cid before
    // setting the bin to NULL, and stop() clears the bus handler before
    // destroying pipelines. Keep that order.
    if (!m_publishedBins.contains(cid))
        return;
    const auto watch = m_publishWatch.constFind(cid);
    if (watch == m_publishWatch.cend() || !watch->state || watch->reported)
        return;
    // Only a publish that never captured a buffer is reported; errors from a
    // working bin are transients and must not end the track.
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
    // Property names and enum values only.
    gint value = -1;
    g_object_get(webrtc, spec->name, &value, nullptr);
    const gchar *rawName = GST_ELEMENT_NAME(webrtc);
    qCInfo(lcSfuMedia) << "peer" << (rawName ? rawName : "?")
                       << spec->name << "=" << value;
}

void SfuMediaEngine::onNegotiationNeeded(GstElement *webrtc, void *userData)
{
    // Starts the offer chain; if it never fires, nothing is published.
    auto *engine = static_cast<SfuMediaEngine *>(userData);
    // webrtcbin raises this at PLAYING before any track exists, and an offer
    // with no media section is a state mismatch to the SFU. The real offer
    // comes from renegotiatePublisher() once a track is linked.
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

// promiseCtxFree runs when the promise is finalized, which on the error-reply
// path is the gst_promise_unref() in these functions. So every `ctx->` read
// happens above it, and the element gets its own reference.
void SfuMediaEngine::onOfferCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    auto *engine = ctx->engine;
    GstElement *webrtc = GST_ELEMENT(gst_object_ref(ctx->webrtc));
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "offer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description,
                          nullptr);
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    if (!description) {
        gst_object_unref(webrtc);
        return;
    }

    GstPromise *local = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", description, local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);

    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);

    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    gst_object_unref(webrtc);
    const quint64 generation = engine->m_generation.load();
    marshal(engine, [engine, token, generation, sdp] {
        engine->handleLocalDescription(token, generation, /*offer=*/true, sdp);
    });
}

void SfuMediaEngine::onAnswerCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    // Read ctx above the unref; see onOfferCreated.
    auto *engine = ctx->engine;
    const bool publisher = ctx->publisher;
    GstElement *webrtc = GST_ELEMENT(gst_object_ref(ctx->webrtc));
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *description = nullptr;
    if (reply)
        gst_structure_get(reply, "answer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description,
                          nullptr);
    gst_promise_unref(promise); // may free ctx: read nothing from it below
    if (!description) {
        gst_object_unref(webrtc);
        return;
    }

    GstPromise *local = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", description, local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);

    // Log what webrtcbin built for the subscriber, which can differ from the
    // SDP we answered: no transceivers means nothing can arrive; RECVONLY or
    // SENDRECV means our side is fine and the fault is upstream; INACTIVE or
    // NONE means it negotiated to nothing. Counts and enums only.
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
        // Direction enum: 0 NONE, 1 INACTIVE, 2 SENDONLY, 3 RECVONLY,
        // 4 SENDRECV.
        qCInfo(lcSfuMedia) << "subscriber transceivers n=" << summary.size()
                           << summary.join(QLatin1Char(' '))
                           << "(dir: 1=inactive 3=recvonly 4=sendrecv)";
    }

    gchar *text = gst_sdp_message_as_text(description->sdp);
    const QString sdp = QString::fromUtf8(text ? text : "");
    g_free(text);
    gst_webrtc_session_description_free(description);

    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    gst_object_unref(webrtc);
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

void SfuMediaEngine::onPadRemoved(GstElement *webrtc, void *pad,
                                  void *userData)
{
    Q_UNUSED(webrtc);
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
        // No bin was built for this pad; that failure was already logged.
        return;
    }
        // Whether webrtcbin raises pad-removed for a track LiveKit retires is
        // unverified, so log it.
    qCInfo(lcSfuMedia) << "a remote track's pad was removed; retiring its "
                          "receive bin";
    GstElement *pipeline = engine->m_subscriber.pipeline;
    if (!pipeline) {
        // stop() already set everything to NULL.
        return;
    }
    auto *ctx = new ReceiveTeardown{
        GST_ELEMENT(gst_object_ref(pipeline)), engine->m_pendingTeardowns};
    // Count before queueing, so stop()'s wait cannot see zero too early.
    engine->m_pendingTeardowns->fetch_add(1);
    gst_element_call_async(bin, receiveTeardownAsync, ctx, receiveTeardownFree);
    // Marshalled: this runs on a streaming thread.
    const QString streamId = entry.streamId;
    const QString kind = entry.kind;
    marshal(engine, [engine, streamId, kind] {
        Q_EMIT engine->remoteTrackRemoved(streamId, kind);
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

    // The decrypt ring is keyed by sender (`streamId`); the track sid picks
    // the surface. Getting either wrong is silent. Sources in order: the
    // pad's `msid`, our own SDP parse matched on the section mid, then the
    // mid itself (routes, never decrypts).
    QString streamId;
    QString trackMid;
    {
        // The pad's own msid, from the remote description. Never derive a
        // section index from the pad name: pads are numbered over produced
        // media, while LiveKit's offer has a data channel in section 0.
        gchar *padMsid = nullptr;
        g_object_get(srcPad, "msid", &padMsid, nullptr);
        if (padMsid) {
            const QString msid = QString::fromUtf8(padMsid);
            streamId = participantIdFromMsid(msid);
            // The track key is the track sid; see trackSidFromMsid().
            trackMid = trackSidFromMsid(msid);
            g_free(padMsid);
        }
        // If the pad property gives nothing (how much webrtcbin populates it
        // varies by GStreamer version), fall back to our own parse of the same
        // SDP, not to another pad property. A transceiver mid is never a
        // decryption key: keys arrive addressed by `TR_...`.
        const bool padCarriedMsid = !streamId.isEmpty() || !trackMid.isEmpty();
        // Fall back when either id is missing, and also when the pad's stream
        // id names no sender in this description (a differently packed msid
        // can yield a wrong-kind token, which is worse than none).
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
                // Match on the section's own mid, never a positional index.
                QMutexLocker lock(&engine->m_recvMutex);
                sdpSectionsKnown = engine->m_midForMline.size();
                for (auto it = engine->m_midForMline.cbegin();
                     it != engine->m_midForMline.cend(); ++it) {
                    if (it.value() != sectionMid)
                        continue;
                    // Fill only what is missing; the pad's own answer stays.
                    if (streamId.isEmpty())
                        streamId = engine->m_streamForMline.value(it.key());
                    if (trackMid.isEmpty())
                        trackMid = engine->m_trackForMline.value(it.key());
                    break;
                }
            }
            // Last resort: a mid routes something but can never decrypt.
            if (trackMid.isEmpty())
                trackMid = sectionMid;
        }
        // `sdpSections` distinguishes "no sections parsed" from "sections
        // present but the mid matched none".
        qCInfo(lcSfuMedia) << "received track attributed="
                           << !streamId.isEmpty() << "trackKey=" << trackMid
                           << "fromPadMsid=" << padCarriedMsid
                           << "sdpSections=" << sdpSectionsKnown
                           << "padSenderUnknown=" << padStreamIdUnknown;
        if (streamId.isEmpty()) {
            // Unattributed tracks get their own unkeyed ring, which drops;
            // decrypting with another participant's key would corrupt.
            streamId = trackMid.isEmpty()
                ? QStringLiteral("unattributed")
                : QStringLiteral("mid:%1").arg(trackMid);
        }
    }

    // Media kind from the caps.
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
    if (mediaKind.isEmpty()) {
        // A guess; log it, since a wrong one leaves the track silent.
        qCWarning(lcSfuMedia)
            << "call diagnosis: an incoming track's caps named no media type;"
            << "assuming audio for stream=" << streamId << "mid=" << trackMid;
        mediaKind = QStringLiteral("audio");
    }

    const quintptr token = reinterpret_cast<quintptr>(webrtc);
    const quint64 generation = engine->m_generation.load();
    // Audio volume elements keep the `outvol` prefix (deafen matches it) and a
    // per-track suffix (per-participant volume).
    //
    // Video goes to an RGBA appsink (a plain row copy into a QVideoFrame).
    // max-buffers=1 drop=true: a late video frame is worthless. The video
    // chain is the same in test-source mode, since an appsink needs no
    // display; only audio ends in a fakesink there.
    const QString description = mediaKind == QLatin1String("video")
        // Bounded but not leaky: this queue carries RTP into the depayloader,
        // and dropping a packet corrupts the VP8 bitstream downstream of
        // webrtcbin, which then sends no PLI. Latency is bounded by the
        // appsink's drop=true instead.
        ? QStringLiteral("queue max-size-buffers=0 max-size-bytes=0 "
                         "max-size-time=200000000 "
                         "! rtpvp8depay name=recvdepay ! vp8dec "
                         "! videoconvert ! video/x-raw,format=RGBA "
                         "! appsink name=vidsink emit-signals=true "
                         "sync=false max-buffers=1 drop=true")
        : (engine->testSourceMode()
               // Leaky is safe for audio: Opus frames are independent and the
               // decoder conceals a loss.
               ? QStringLiteral("queue max-size-buffers=0 max-size-bytes=0 "
                                "max-size-time=200000000 leaky=downstream "
                                "! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! fakesink sync=false")
                     .arg(outputVolumeElementName(
                         volumeKeyFor(streamId, trackMid)))
               : QStringLiteral("queue max-size-buffers=0 max-size-bytes=0 "
                                "max-size-time=200000000 leaky=downstream "
                                "! rtpopusdepay name=recvdepay "
                                "! opusdec ! audioconvert "
                                "! audioresample ! volume name=%1 "
                                "! autoaudiosink")
                     .arg(outputVolumeElementName(
                         volumeKeyFor(streamId, trackMid))));

    GError *error = nullptr;
    GstElement *bin = gst_parse_bin_from_description(
        description.toUtf8().constData(), TRUE, &error);
    if (error) {
        // Log GStreamer's message: it names the element that failed.
        qCWarning(lcSfuMedia)
            << "call diagnosis: the receive bin for stream=" << streamId
            << "kind=" << mediaKind << "could not be BUILT:"
            << (error->message ? error->message : "?")
            << "— this participant will never be heard or seen";
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
        qCWarning(lcSfuMedia)
            << "call diagnosis: the receive bin for stream=" << streamId
            << "kind=" << mediaKind
            << "could not be ADDED to the subscriber pipeline";
        gst_object_unref(pipeline);
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
        });
        return;
    }
    // Decrypt on the depayloader's src pad, where the encoded frame is whole.
    // Installed before the bin plays so no frame reaches the decoder
    // unexamined.
    bool decryptProbeInstalled = false;
    if (GstElement *depay = gst_bin_get_by_name(GST_BIN(bin), "recvdepay")) {
        if (GstPad *framePad = gst_element_get_static_pad(depay, "src")) {
            engine->installDecryptProbe(framePad,
                                        mediaKind == QLatin1String("video"),
                                        streamId);
            decryptProbeInstalled = true;
            gst_object_unref(framePad);
        }
        gst_object_unref(depay);
    }
    if (!decryptProbeInstalled) {
        // Without the probe, ciphertext goes straight into the decoder and
        // every counter stays at zero.
        qCWarning(lcSfuMedia)
            << "call diagnosis: no decrypt probe could be installed for "
               "stream=" << streamId << "kind=" << mediaKind
            << "— its frames will not be decrypted or counted";
    }
    // Route decoded video; installed before the bin plays.
    if (GstElement *appsink = gst_bin_get_by_name(GST_BIN(bin), "vidsink")) {
        auto *ctx = new VideoSinkCtx{engine, trackMid, streamId};
        g_signal_connect_data(appsink, "new-sample",
                              G_CALLBACK(onVideoSample), ctx,
                              videoSinkCtxFree, GConnectFlags(0));
        gst_object_unref(appsink);
    }
    // Apply any volume chosen before this bin existed, on the GUI thread.
    marshal(engine, [engine, streamId, trackMid, generation] {
        engine->applyPendingTrackVolume(streamId, trackMid, generation);
    });
    // Apply the current deafen state before the bin plays, so a new track is
    // never briefly audible. Uses the same name derivation as the bin
    // (per-track), since gst_bin_get_by_name matches exactly.
    if (GstElement *volume = gst_bin_get_by_name(
            GST_BIN(bin),
            outputVolumeElementName(volumeKeyFor(streamId, trackMid))
                .toUtf8().constData())) {
        g_object_set(volume, "mute",
                     engine->m_outputMuted.load() ? TRUE : FALSE, nullptr);
        // Per-person levels are re-applied by SfuCallController on the
        // participant change that brings this track; this thread has no
        // settings access. Deafen must be applied here.
        gst_object_unref(volume);
    }
    gst_element_sync_state_with_parent(bin);
    GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
    const GstPadLinkReturn linked = gst_pad_link(srcPad, sinkPad);
    if (sinkPad)
        gst_object_unref(sinkPad);
    gst_object_unref(pipeline);
    if (linked != GST_PAD_LINK_OK) {
        qCWarning(lcSfuMedia)
            << "call diagnosis: the receive bin for stream=" << streamId
            << "kind=" << mediaKind << "would not LINK to its pad, code="
            << linked;
        marshal(engine, [engine, token, generation] {
            engine->handleFailure(token, generation,
                                  QStringLiteral("media_receive"));
        });
        return;
    }
        // Remember which bin this pad feeds so pad-removed can retire it;
        // otherwise every toggle leaves a playing bin behind, and a stale
        // `outvol_*` can shadow the live one.
    {
        QMutexLocker lock(&engine->m_receiveBinMutex);
        engine->m_receiveBins.insert(static_cast<GstPad *>(pad),
                                     ReceiveBin{bin, streamId, mediaKind});
    }
    // A synthetic stream id here plus `attributed=false` above marks a track
    // nobody can key.
    qCInfo(lcSfuMedia)
        << "call diagnosis: a receive chain is RUNNING for stream=" << streamId
        << "mid=" << trackMid << "kind=" << mediaKind;
    marshal(engine, [engine, mediaKind, streamId, trackMid] {
        // The UI keys tiles off MatrixRTC membership (who is present); this
        // says who is actually sending, and on which track.
        Q_EMIT engine->remoteTrackAdded(streamId, trackMid, mediaKind);
    });
}
