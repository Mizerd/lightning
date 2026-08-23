#include "calls/SfuMediaEngine.h"

#include <mutex>

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

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

SfuMediaEngine::SfuMediaEngine(QObject *parent) : QObject(parent)
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
                       "! valve name=micvalve drop=%2 ! opusenc "
                       "! rtpopuspay pt=111 "
                       "! application/x-rtp,media=audio,encoding-name=OPUS,"
                       "payload=111")
            .arg(source, m_microphoneMuted ? QStringLiteral("true")
                                           : QStringLiteral("false"));

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
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

    // Screen content is text-heavy: deadline=1 keeps latency sane while
    // keyframe-max-dist favours readability over motion smoothness, and the
    // camera path prefers the opposite trade.
    const QString encoder = screenShare
        ? QStringLiteral("vp8enc deadline=1 keyframe-max-dist=60 "
                         "end-usage=cbr target-bitrate=2500000")
        : QStringLiteral("vp8enc deadline=1 keyframe-max-dist=30 "
                         "end-usage=cbr target-bitrate=1200000");
    const QString description =
        QStringLiteral("%1 ! queue ! videoconvert ! videoscale ! videorate "
                       "! valve name=vidvalve drop=false ! %2 "
                       "! rtpvp8pay pt=96 "
                       "! application/x-rtp,media=video,encoding-name=VP8,"
                       "payload=96")
            .arg(source, encoder);

    GError *error = nullptr;
    GstElement *bin =
        gst_parse_bin_from_description(description.toUtf8().constData(), TRUE,
                                       &error);
    if (error) {
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
    if (gst_sdp_message_parse_buffer(
            reinterpret_cast<const guint8 *>(sdp.toUtf8().constData()),
            static_cast<guint>(sdp.toUtf8().size()), message)
        != GST_SDP_OK) {
        gst_sdp_message_free(message);
        // The SDP text itself is never logged: it carries host IPs.
        Q_EMIT failed(QStringLiteral("bad_remote_sdp"));
        return;
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
    // The remote track's identity is not known from the pad alone — LiveKit
    // maps it through the SDP's mid. Until that mapping is wired the volume
    // element still gets the "outvol" prefix, so deafen reaches it.
    const QString description = mediaKind == QLatin1String("video")
        ? (engine->testSourceMode()
               ? QStringLiteral("queue ! rtpvp8depay ! vp8dec ! videoconvert "
                                "! fakesink sync=false")
               : QStringLiteral("queue ! rtpvp8depay ! vp8dec ! videoconvert "
                                "! fakesink sync=false"))
        : (engine->testSourceMode()
               ? QStringLiteral("queue ! rtpopusdepay ! opusdec ! audioconvert "
                                "! audioresample ! volume name=outvol "
                                "! fakesink sync=false")
               : QStringLiteral("queue ! rtpopusdepay ! opusdec ! audioconvert "
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
    marshal(engine, [engine, mediaKind] {
        // Identity mapping through the SDP mid is not wired yet, so this
        // reports the KIND only. The UI keys tiles off MatrixRTC membership,
        // which is authoritative for who is present.
        Q_EMIT engine->remoteTrackAdded(QString(), mediaKind);
    });
}
