#include "GstCallMediaBackend.h"

#include "calls/GstBootstrap.h"

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
    const QString source = m_testTone
        ? QStringLiteral(
              "audiotestsrc is-live=true wave=sine freq=440 volume=0.05")
        // A chosen device, or autoaudiosrc for "system default", which keeps
        // following the default.
        : (m_audioSourceElement.isEmpty()
               ? QStringLiteral("autoaudiosrc")
               : m_audioSourceElement);
    // As offerer we use 111 (the common convention); as answerer RFC 3264
    // requires the offerer's number, which createAnswer() extracts.
    const int payload = qBound(96, opusPayloadType, 127);
    const QString description = QStringLiteral(
        "webrtcbin name=wb bundle-policy=max-bundle latency=100 "
        // Bounded and leaky: a default queue holds a second and never drains
        // it, which becomes permanent latency.
        "%1 ! queue max-size-buffers=0 max-size-bytes=0 "
        "max-size-time=100000000 leaky=downstream "
        "! audioconvert ! audioresample "
        // valve name=micvalve: drop=true stops buffers before the encoder, so
        // nothing is published while muted (lowering volume would still send).
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
    // Borrowed: the pipeline owns the valve and outlives the session struct.
    if (GstElement *valve = gst_bin_get_by_name(GST_BIN(pipeline),
                                                "micvalve")) {
        m_session.micValve = valve;
        gst_object_unref(valve);
    }
    m_sessionActive = true;

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

void GstCallMediaBackend::setAudioDevices(const QString &sourceElement,
                                          const QString &sinkElement)
{
    // Stored for the next session: relinking a live send branch is riskier
    // than waiting.
    m_audioSourceElement = sourceElement;
    m_audioSinkElement = sinkElement;
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
    auto *backend = static_cast<GstCallMediaBackend *>(userData);
    GstPad *srcPad = GST_PAD(pad);
    if (GST_PAD_DIRECTION(srcPad) != GST_PAD_SRC)
        return;
    GstElement *pipeline =
        GST_ELEMENT(gst_element_get_parent(webrtc)); // owns one ref
    if (!pipeline)
        return;
    // m_testTone and m_audioSinkElement are set before any session and never
    // changed during one, so reading them here is safe.
    const QString sink = backend->m_audioSinkElement.isEmpty()
        ? QStringLiteral("autoaudiosink")
        : backend->m_audioSinkElement;
    // Bounded and leaky: the receive side is where a listener hears delay,
    // and a default queue would keep a stall's second of audio forever. 200 ms
    // absorbs local scheduling jitter. Leaking is safe for Opus (the decoder
    // conceals a lost frame), unlike RTP in front of a video depayloader.
    const QString recvQueue = QStringLiteral(
        "queue max-size-buffers=0 max-size-bytes=0 "
        "max-size-time=200000000 leaky=downstream ");
    const QString descriptionString = backend->m_testTone
        ? recvQueue
            + QStringLiteral("! rtpopusdepay ! opusdec ! audioconvert "
                             "! audioresample ! volume name=outvol "
                             "! fakesink sync=false")
        : recvQueue
            + QStringLiteral("! rtpopusdepay ! opusdec ! audioconvert "
                             "! audioresample ! volume name=outvol ! %1")
                  .arg(sink);
    const QByteArray descriptionUtf8 = descriptionString.toUtf8();
    const char *description = descriptionUtf8.constData();
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
        // gst_bin_add sinks and drops the element on failure: never touch bin
        // again.
        gst_object_unref(pipeline);
        marshal(backend, [backend, token] {
            backend->handleFailure(token, QStringLiteral("media_receive"));
        });
        return;
    }
    // Apply the current deafen state before the bin plays, so a new track is
    // never briefly audible.
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
