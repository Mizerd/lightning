#include "GstCallMediaBackend.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

#include <mutex>

// Coarse lifecycle/category lines only. NEVER log SDP, candidates, TURN
// credentials, or GStreamer error detail strings that could embed them.
Q_LOGGING_CATEGORY(lcCallMedia, "matrix.calls.media")

namespace {

// GStreamer invokes callbacks on its own threads; marshalled lambdas must
// never run against a destroyed backend. The registry makes "is this
// backend still alive" answerable from any thread; the queued invocation
// itself is tied to the backend as receiver, so anything still in flight
// when the backend dies is dropped by Qt.
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

// Context for promise callbacks: keeps the webrtcbin alive until the
// promise settles and remembers which call the description belongs to.
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

// Takes the "offer"/"answer" description out of a settled promise, applies
// it as the local description, and hands the serialized SDP back. Shared
// by the offer and answer creation callbacks. Returns an empty string on
// any failure (interrupted promise, missing field).
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

// The offerer's dynamic payload number for Opus, from its SDP's rtpmap
// ("a=rtpmap:<pt> opus/48000[/2]"). RFC 3264: our answer must reuse it.
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
        // The error detail can embed device/address strings: category
        // only. By-name queued invoke under the alive lock (the slot is
        // private; the metaobject does not mind, and the queued call dies
        // with the receiver).
        QMutexLocker lock(&g_aliveMutex);
        if (g_aliveBackends.contains(ctx->backend)) {
            QMetaObject::invokeMethod(
                ctx->backend, "handleFailure", Qt::QueuedConnection,
                Q_ARG(quintptr, ctx->pipelineToken),
                Q_ARG(QString, QStringLiteral("media_pipeline")));
        }
    }
    // DROP after inspection: nothing drains this bus's async queue, so
    // passing messages through would grow it for the call's whole
    // duration (review round 3).
    gst_message_unref(message);
    return GST_BUS_DROP;
}

} // namespace

bool GstCallMediaBackend::runtimeAvailable(QString *whyNot)
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
    // Everything the send/receive pipeline needs, including webrtcbin's
    // own runtime requirements (nice transport, DTLS-SRTP).
    static const char *const kRequired[] = {
        "webrtcbin",    "nicesrc",      "nicesink",     "dtlssrtpenc",
        "dtlssrtpdec",  "opusenc",      "opusdec",      "rtpopuspay",
        "rtpopusdepay", "audioconvert", "audioresample", "audiotestsrc",
        "fakesink",     "autoaudiosrc", "autoaudiosink", "queue",
        // Mute is implemented with a real valve (send) and volume (receive)
        // rather than by lowering gain, so both must resolve or the engine
        // must not claim mute support.
        "valve",        "volume",
        "capsfilter",
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

GstCallMediaBackend::GstCallMediaBackend(QObject *parent)
    : CallMediaBackend(parent)
{
    QMutexLocker lock(&g_aliveMutex);
    g_aliveBackends.insert(this);
}

GstCallMediaBackend::~GstCallMediaBackend()
{
    {
        // Unregister FIRST: from here no new marshalled lambda targets us,
        // and anything already queued dies with the QObject.
        QMutexLocker lock(&g_aliveMutex);
        g_aliveBackends.remove(this);
    }
    if (m_sessionActive)
        destroySessionLocked();
}

bool GstCallMediaBackend::startSession(const QString &callId, bool offerer,
                                       int opusPayloadType)
{
    if (m_sessionActive) {
        // One call at a time (matches CallController); a stale session here
        // is a controller bug — refuse rather than leak the old pipeline.
        qCWarning(lcCallMedia) << "session already active; refusing new call";
        return false;
    }
    const QString source = m_testTone
        ? QStringLiteral(
              "audiotestsrc is-live=true wave=sine freq=440 volume=0.05")
        : QStringLiteral("autoaudiosrc");
    // As the OFFERER we pick 111 (the ecosystem convention). As the
    // ANSWERER RFC 3264 requires reusing the OFFERER's number for the
    // matched codec, so createAnswer() extracts it from the remote offer's
    // rtpmap and passes it here (review round 3 — always re-asserting our
    // own 111 in an answer is not spec-compliant reconciliation).
    const int payload = qBound(96, opusPayloadType, 127);
    const QString description = QStringLiteral(
        "webrtcbin name=wb bundle-policy=max-bundle latency=100 "
        "%1 ! queue ! audioconvert ! audioresample "
        // valve name=micvalve: drop=true stops buffers reaching the encoder,
        // so NOTHING is published while muted. Lowering volume here would
        // still send audio and is not mute.
        "! valve name=micvalve drop=false ! opusenc "
        "! rtpopuspay pt=%2 "
        "! application/x-rtp,media=audio,encoding-name=OPUS,payload=%2 "
        "! wb. ").arg(source).arg(payload);
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

    m_session = Session();
    m_session.callId = callId;
    m_session.pipeline = pipeline;
    m_session.webrtc = webrtc;
    m_session.offerer = offerer;
    // Owned by the pipeline; borrowed here (gst_bin_get_by_name returns a
    // ref, released immediately — the pipeline outlives the session struct
    // and destroySessionLocked drops the whole pipeline).
    if (GstElement *valve = gst_bin_get_by_name(GST_BIN(pipeline),
                                                "micvalve")) {
        m_session.micValve = valve;
        gst_object_unref(valve);
    }
    m_sessionActive = true;

    applyIceConfigLocked();

    if (offerer) {
        // Only the offerer answers negotiation-needed; the answerer's
        // negotiation is driven explicitly by createAnswer().
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
                        << "testTone=" << m_testTone;
    return true;
}

void GstCallMediaBackend::destroySessionLocked()
{
    if (!m_sessionActive)
        return;
    if (m_session.webrtc) {
        g_signal_handlers_disconnect_by_data(m_session.webrtc, this);
        gst_object_unref(m_session.webrtc);
    }
    if (m_session.pipeline) {
        GstBus *bus = gst_element_get_bus(m_session.pipeline);
        if (bus) {
            gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
            gst_object_unref(bus);
        }
        gst_element_set_state(m_session.pipeline, GST_STATE_NULL);
        gst_object_unref(m_session.pipeline);
    }
    m_session = Session();
    m_sessionActive = false;
    // Engine state is PER SESSION. The user's deafen intent belongs to the
    // controller, which re-applies it when the next call connects; leaving
    // it latched here would silence a later call with no visible cause.
    m_outputMuted.store(false);
    qCInfo(lcCallMedia) << "media session destroyed";
}

void GstCallMediaBackend::applyIceConfigLocked()
{
    if (!m_session.webrtc)
        return;
    // Policy (see CallMediaBackend.h): only servers the homeserver named.
    // First stun: URI becomes the stun-server property; every turn(s): URI
    // is added with the short-lived credentials percent-encoded in.
    bool stunApplied = false;
    const QByteArray user =
        QUrl::toPercentEncoding(m_iceUsername);
    const QByteArray password =
        QUrl::toPercentEncoding(m_icePassword);
    // The uris come from OUR homeserver, but they are still remote input
    // assembled into a credential-bearing URI: refuse anything that could
    // smuggle structure past the userinfo we insert (an embedded '@' or
    // '/'), or that carries whitespace/control characters.
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
            // `added` deliberately not logged with the URI — credentials.
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
    // Applied to the NEXT session; a live call keeps the config it
    // negotiated with.
}

void GstCallMediaBackend::createOffer(const QString &callId)
{
    if (!startSession(callId, /*offerer=*/true, kDefaultOpusPayloadType))
        Q_EMIT failed(callId, QStringLiteral("media_init"));
    // The offer itself arrives via on-negotiation-needed → create-offer.
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
    // gst's SDP parser is deliberately permissive (garbage "parses"), so
    // additionally require at least one media section before trusting it.
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
        return; // MSC2746 end-of-candidates: nothing to feed
    if (!m_session.remoteDescriptionSet) {
        // Trickled candidates can outrun the description exchange; feed
        // them once the remote description is applied. Bounded.
        if (m_session.pendingRemoteCandidates.size() < 64)
            m_session.pendingRemoteCandidates.append(
                qMakePair(sdpMLineIndex, candidate));
        return;
    }
    g_signal_emit_by_name(m_session.webrtc, "add-ice-candidate",
                          static_cast<guint>(qMax(0, sdpMLineIndex)),
                          candidate.toUtf8().constData());
}

void GstCallMediaBackend::setMicrophoneMuted(const QString &callId,
                                             bool muted)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    m_session.micMuted = muted;
    if (!m_session.micValve)
        return;
    // drop=true discards buffers BEFORE the encoder, so no RTP is produced
    // and the peer receives nothing. This is a real mute, not attenuation.
    g_object_set(m_session.micValve, "drop", muted ? TRUE : FALSE, nullptr);
}

void GstCallMediaBackend::setOutputMuted(const QString &callId, bool muted)
{
    if (!m_sessionActive || m_session.callId != callId)
        return;
    m_session.outputMuted = muted;
    // Published for onPadAdded, which runs on a GStreamer thread and must
    // silence a track that arrives AFTER the user deafened.
    m_outputMuted.store(muted);
    if (!m_session.pipeline)
        return;
    // Every receive bin has its own volume element, and a group call has one
    // per remote track, so mute them ALL rather than the first found.
    //
    // Matched on the NAME we gave them ("outvol"), not on the "volume"
    // factory: `autoaudiosrc`/`autoaudiosink` are bins that may contain a
    // volume element of their own, and a recursive factory match would reach
    // into the SEND chain. Muting our own capture here would be wrong even
    // though deafen also mutes the mic, because the two controls must stay
    // independent — undeafening would then fight the valve.
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(m_session.pipeline));
    if (!it)
        return;
    GValue item = G_VALUE_INIT;
    bool done = false;
    // A pipeline that keeps changing must not spin this loop forever; a
    // handful of resyncs is far more than a track add needs.
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
            // The pipeline changed under us — a remote track being added is
            // exactly when that happens, and it is exactly when a missed
            // element would stay audible while deafened. Restart rather than
            // stop, but bounded.
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

void GstCallMediaBackend::handleConnectionState(quintptr token, int state)
{
    if (!tokenMatchesLiveSession(token))
        return;
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
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    // Session identity travels as the EMITTING ELEMENT's pointer, held
    // alive by the promise ctx's ref, so a stale offer for a closed call
    // can never be attributed to a newer session (reading backend state
    // from this thread would not be safe; the pointer needs no read).
    GstPromise *promise = gst_promise_new_with_change_func(
        onOfferCreated, promiseCtxNew(backend, webrtc, QString()),
        promiseCtxFree);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void GstCallMediaBackend::onOfferCreated(GstPromise *promise, void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const QString sdp =
        applyCreatedDescription(promise, ctx->webrtc, "offer");
    gst_promise_unref(promise);
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    marshal(backend, [backend, token, sdp] {
        backend->handleLocalDescription(token, /*offer=*/true, sdp);
    });
}

void GstCallMediaBackend::onRemoteOfferSet(GstPromise *promise,
                                           void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const bool replied =
        gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    gst_promise_unref(promise);
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    if (!replied)
        return;
    marshal(backend, [backend, token] {
        backend->handleRemoteDescriptionApplied(token);
    });
    // Answer creation continues on this thread against the ctx-held
    // element — safe even if the session closed meanwhile (the element is
    // ref-held; the eventual answer is dropped by the Qt-side token
    // check).
    GstPromise *answerPromise = gst_promise_new_with_change_func(
        onAnswerCreated, promiseCtxNew(backend, ctx->webrtc, ctx->callId),
        promiseCtxFree);
    g_signal_emit_by_name(ctx->webrtc, "create-answer", nullptr,
                          answerPromise);
}

void GstCallMediaBackend::onAnswerCreated(GstPromise *promise,
                                          void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const QString sdp =
        applyCreatedDescription(promise, ctx->webrtc, "answer");
    gst_promise_unref(promise);
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
    marshal(backend, [backend, token, sdp] {
        backend->handleLocalDescription(token, /*offer=*/false, sdp);
    });
}

void GstCallMediaBackend::onRemoteAnswerSet(GstPromise *promise,
                                            void *userData)
{
    auto *ctx = static_cast<PromiseCtx *>(userData);
    const bool replied =
        gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED;
    gst_promise_unref(promise);
    GstCallMediaBackend *backend = ctx->backend;
    const quintptr token = reinterpret_cast<quintptr>(ctx->webrtc);
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
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    GstPad *srcPad = GST_PAD(pad);
    if (GST_PAD_DIRECTION(srcPad) != GST_PAD_SRC)
        return;
    GstElement *pipeline =
        GST_ELEMENT(gst_element_get_parent(webrtc)); // owns one ref
    if (!pipeline)
        return;
    // The receive chain. m_testTone is set before any session and never
    // mutated during one, so this cross-thread read is benign.
    const char *description = backend->m_testTone
        ? "queue ! rtpopusdepay ! opusdec ! audioconvert ! audioresample "
          "! volume name=outvol ! fakesink sync=false"
        : "queue ! rtpopusdepay ! opusdec ! audioconvert ! audioresample "
          "! volume name=outvol ! autoaudiosink";
    const quintptr token = reinterpret_cast<quintptr>(webrtc);
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
        // gst_bin_add sinks-and-drops the element on failure: bin may be
        // FINALIZED here — report, never touch it again.
        gst_object_unref(pipeline);
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    // Apply the CURRENT deafen state before the bin goes playing: a remote
    // track can arrive after the user deafened, and coming up audible for
    // even a moment defeats the control.
    if (GstElement *vol = gst_bin_get_by_name(GST_BIN(bin), "outvol")) {
        g_object_set(vol, "mute",
                     backend->m_outputMuted.load() ? TRUE : FALSE, nullptr);
        gst_object_unref(vol);
    }
    gst_element_sync_state_with_parent(bin);
    GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
    const GstPadLinkReturn linked = gst_pad_link(srcPad, sinkPad);
    gst_object_unref(sinkPad);
    gst_object_unref(pipeline);
    if (linked != GST_PAD_LINK_OK) {
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
    }
}
