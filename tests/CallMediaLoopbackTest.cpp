// The real WebRTC handshake, no mocks: two GstCallMediaBackend instances in
// one process exchange SDP and trickled ICE as CallController wires them over
// Matrix, and must reach CONNECTED over loopback host candidates with
// DTLS-SRTP and Opus RTP flowing (test-tone mode, so no audio device is
// needed). Without the GStreamer plugins the suite skips: an absent engine is
// a supported configuration.
//
// Also the video offers Element and Nheko send to a legacy call: this engine
// is audio only, and it used to accept the video section and link the video
// track into the Opus chain, which stopped the whole transport.
#include <QtTest/QtTest>

#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTimer>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <atomic>

#include "calls/GstCallMediaBackend.h"

namespace {

// Element Desktop 1.12.29's legacy video offer, captured 2026-09-25 from an
// m.call.invite (candidates removed, ice-pwd replaced).
const char kElementVideoOffer[] = R"SDP(v=0
o=- 3875380875183614964 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1
a=extmap-allow-mixed
a=msid-semantic: WMS 0bef0d1b-a1ca-49bd-8192-f665f62d73e3
m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:TtgY
a=ice-pwd:0123456789abcdefghijklmn
a=ice-options:trickle
a=fingerprint:sha-256 7C:1C:49:0F:43:DE:EA:64:BB:80:AD:AB:0A:23:90:E7:9C:67:48:D6:A2:58:7F:AB:56:9A:56:65:27:CB:C6:43
a=setup:actpass
a=mid:0
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=sendrecv
a=msid:0bef0d1b-a1ca-49bd-8192-f665f62d73e3 17aaf9df-0588-40fc-907e-4885e0bfdb82
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:111 rrtr
a=rtcp-fb:63 rrtr
a=rtcp-fb:9 rrtr
a=rtcp-fb:0 rrtr
a=rtcp-fb:8 rrtr
a=rtcp-fb:13 rrtr
a=rtcp-fb:110 rrtr
a=rtcp-fb:126 rrtr
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;usedtx=1;useinbandfec=1
a=rtpmap:63 red/48000/2
a=fmtp:63 111/111
a=rtpmap:9 G722/8000
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:13 CN/8000
a=rtpmap:110 telephone-event/48000
a=rtpmap:126 telephone-event/8000
a=ssrc:1211291690 cname:4ZKh4Wl2LSfpuzHr
a=ssrc:1211291690 msid:0bef0d1b-a1ca-49bd-8192-f665f62d73e3 17aaf9df-0588-40fc-907e-4885e0bfdb82
m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 104 107 108 109 114 115 116 117 39 40 45 46 98 99 100 101 118 119 120
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:TtgY
a=ice-pwd:0123456789abcdefghijklmn
a=ice-options:trickle
a=fingerprint:sha-256 7C:1C:49:0F:43:DE:EA:64:BB:80:AD:AB:0A:23:90:E7:9C:67:48:D6:A2:58:7F:AB:56:9A:56:65:27:CB:C6:43
a=setup:actpass
a=mid:1
a=extmap:14 urn:ietf:params:rtp-hdrext:toffset
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:13 urn:3gpp:video-orientation
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type
a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing
a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id
a=sendrecv
a=msid:0bef0d1b-a1ca-49bd-8192-f665f62d73e3 f83065b8-c935-47a8-9e53-59e6c6143507
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:96 rrtr
a=rtcp-fb:97 rrtr
a=rtcp-fb:102 rrtr
a=rtcp-fb:103 rrtr
a=rtcp-fb:104 rrtr
a=rtcp-fb:107 rrtr
a=rtcp-fb:108 rrtr
a=rtcp-fb:109 rrtr
a=rtcp-fb:114 rrtr
a=rtcp-fb:115 rrtr
a=rtcp-fb:116 rrtr
a=rtcp-fb:117 rrtr
a=rtcp-fb:39 rrtr
a=rtcp-fb:40 rrtr
a=rtcp-fb:45 rrtr
a=rtcp-fb:46 rrtr
a=rtcp-fb:98 rrtr
a=rtcp-fb:99 rrtr
a=rtcp-fb:100 rrtr
a=rtcp-fb:101 rrtr
a=rtcp-fb:118 rrtr
a=rtcp-fb:119 rrtr
a=rtcp-fb:120 rrtr
a=rtpmap:96 VP8/90000
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
a=rtpmap:102 H264/90000
a=rtcp-fb:102 goog-remb
a=rtcp-fb:102 transport-cc
a=rtcp-fb:102 ccm fir
a=rtcp-fb:102 nack
a=rtcp-fb:102 nack pli
a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f
a=rtpmap:103 rtx/90000
a=fmtp:103 apt=102
a=rtpmap:104 H264/90000
a=rtcp-fb:104 goog-remb
a=rtcp-fb:104 transport-cc
a=rtcp-fb:104 ccm fir
a=rtcp-fb:104 nack
a=rtcp-fb:104 nack pli
a=fmtp:104 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f
a=rtpmap:107 rtx/90000
a=fmtp:107 apt=104
a=rtpmap:108 H264/90000
a=rtcp-fb:108 goog-remb
a=rtcp-fb:108 transport-cc
a=rtcp-fb:108 ccm fir
a=rtcp-fb:108 nack
a=rtcp-fb:108 nack pli
a=fmtp:108 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:109 rtx/90000
a=fmtp:109 apt=108
a=rtpmap:114 H264/90000
a=rtcp-fb:114 goog-remb
a=rtcp-fb:114 transport-cc
a=rtcp-fb:114 ccm fir
a=rtcp-fb:114 nack
a=rtcp-fb:114 nack pli
a=fmtp:114 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f
a=rtpmap:115 rtx/90000
a=fmtp:115 apt=114
a=rtpmap:116 H264/90000
a=rtcp-fb:116 goog-remb
a=rtcp-fb:116 transport-cc
a=rtcp-fb:116 ccm fir
a=rtcp-fb:116 nack
a=rtcp-fb:116 nack pli
a=fmtp:116 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f
a=rtpmap:117 rtx/90000
a=fmtp:117 apt=116
a=rtpmap:39 H264/90000
a=rtcp-fb:39 goog-remb
a=rtcp-fb:39 transport-cc
a=rtcp-fb:39 ccm fir
a=rtcp-fb:39 nack
a=rtcp-fb:39 nack pli
a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=4d001f
a=rtpmap:40 rtx/90000
a=fmtp:40 apt=39
a=rtpmap:45 AV1/90000
a=rtcp-fb:45 goog-remb
a=rtcp-fb:45 transport-cc
a=rtcp-fb:45 ccm fir
a=rtcp-fb:45 nack
a=rtcp-fb:45 nack pli
a=fmtp:45 level-idx=5;profile=0;tier=0
a=rtpmap:46 rtx/90000
a=fmtp:46 apt=45
a=rtpmap:98 VP9/90000
a=rtcp-fb:98 goog-remb
a=rtcp-fb:98 transport-cc
a=rtcp-fb:98 ccm fir
a=rtcp-fb:98 nack
a=rtcp-fb:98 nack pli
a=fmtp:98 profile-id=0
a=rtpmap:99 rtx/90000
a=fmtp:99 apt=98
a=rtpmap:100 VP9/90000
a=rtcp-fb:100 goog-remb
a=rtcp-fb:100 transport-cc
a=rtcp-fb:100 ccm fir
a=rtcp-fb:100 nack
a=rtcp-fb:100 nack pli
a=fmtp:100 profile-id=2
a=rtpmap:101 rtx/90000
a=fmtp:101 apt=100
a=rtpmap:118 red/90000
a=rtpmap:119 rtx/90000
a=fmtp:119 apt=118
a=rtpmap:120 ulpfec/90000
a=ssrc-group:FID 859673104 379345268
a=ssrc:859673104 cname:4ZKh4Wl2LSfpuzHr
a=ssrc:859673104 msid:0bef0d1b-a1ca-49bd-8192-f665f62d73e3 f83065b8-c935-47a8-9e53-59e6c6143507
a=ssrc:379345268 cname:4ZKh4Wl2LSfpuzHr
a=ssrc:379345268 msid:0bef0d1b-a1ca-49bd-8192-f665f62d73e3 f83065b8-c935-47a8-9e53-59e6c6143507)SDP";

QString crlf(const char *text)
{
    QString sdp = QString::fromUtf8(text);
    sdp.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return sdp + QStringLiteral("\r\n");
}

GstPadProbeReturn countBuffer(GstPad *, GstPadProbeInfo *, gpointer counter)
{
    static_cast<std::atomic<int> *>(counter)->fetch_add(1);
    return GST_PAD_PROBE_OK;
}

// A peer shaped like Element's video call: one webrtcbin sending Opus audio
// and VP8 video, sendrecv, bundled. It plays what it receives into fakesinks
// and counts the audio packets. webrtcbin keeps sending video after an
// inactive answer, which is what exercises the receiver's drain.
class AvOfferer : public QObject
{
    Q_OBJECT
public:
    AvOfferer()
    {
        GError *error = nullptr;
        m_pipeline = gst_parse_launch(
            "webrtcbin name=w bundle-policy=max-compat "
            "audiotestsrc is-live=true freq=600 ! opusenc "
            "! rtpopuspay pt=111 "
            "! application/x-rtp,media=audio,encoding-name=OPUS,payload=111 "
            "! w. "
            "videotestsrc is-live=true "
            "! video/x-raw,width=320,height=240,framerate=15/1 "
            "! vp8enc deadline=1 ! rtpvp8pay pt=96 "
            "! application/x-rtp,media=video,encoding-name=VP8,payload=96 "
            "! w. ",
            &error);
        if (error) {
            g_error_free(error);
            if (m_pipeline)
                gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
            return;
        }
        m_webrtc = gst_bin_get_by_name(GST_BIN(m_pipeline), "w");
        g_signal_connect(m_webrtc, "pad-added", G_CALLBACK(onPadAdded), this);
        g_signal_connect(m_webrtc, "on-ice-candidate",
                         G_CALLBACK(onIceCandidate), this);
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }
    ~AvOfferer() override
    {
        if (!m_pipeline)
            return;
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        g_signal_handlers_disconnect_by_data(m_webrtc, this);
        gst_object_unref(m_webrtc);
        gst_object_unref(m_pipeline);
    }
    bool valid() const { return m_pipeline != nullptr; }
    int audioPacketsReceived() const { return m_audioPackets.load(); }

    /// Asks for an offer; it arrives on offerReady once both sections exist
    /// (webrtcbin leaves a section out until its caps are known).
    void createOffer()
    {
        g_signal_emit_by_name(
            m_webrtc, "create-offer", nullptr,
            gst_promise_new_with_change_func(onOffer, this, nullptr));
    }
    void setRemoteAnswer(const QString &sdp)
    {
        GstSDPMessage *message = nullptr;
        if (gst_sdp_message_new_from_text(sdp.toUtf8().constData(), &message)
            != GST_SDP_OK) {
            if (message)
                gst_sdp_message_free(message);
            return;
        }
        GstWebRTCSessionDescription *answer =
            gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                                               message);
        g_signal_emit_by_name(m_webrtc, "set-remote-description", answer,
                              nullptr);
        gst_webrtc_session_description_free(answer);
    }
    void addRemoteCandidate(int mline, const QString &candidate)
    {
        if (candidate.trimmed().isEmpty())
            return;
        g_signal_emit_by_name(m_webrtc, "add-ice-candidate",
                              static_cast<guint>(qMax(0, mline)),
                              candidate.toUtf8().constData());
    }

Q_SIGNALS:
    void offerReady(const QString &sdp, int sections);
    void localCandidate(int mline, const QString &candidate);

private:
    static void onOffer(GstPromise *promise, gpointer data)
    {
        auto *self = static_cast<AvOfferer *>(data);
        GstWebRTCSessionDescription *offer = nullptr;
        if (gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED) {
            gst_structure_get(gst_promise_get_reply(promise), "offer",
                              GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer,
                              nullptr);
        }
        gst_promise_unref(promise);
        if (!offer)
            return;
        const int sections =
            static_cast<int>(gst_sdp_message_medias_len(offer->sdp));
        gchar *text = gst_sdp_message_as_text(offer->sdp);
        const QString sdp = QString::fromUtf8(text);
        g_free(text);
        if (sections >= 2) {
            g_signal_emit_by_name(self->m_webrtc, "set-local-description",
                                  offer, nullptr);
        }
        gst_webrtc_session_description_free(offer);
        QPointer<AvOfferer> guard(self);
        QMetaObject::invokeMethod(
            self,
            [guard, sdp, sections] {
                if (guard)
                    Q_EMIT guard->offerReady(sdp, sections);
            },
            Qt::QueuedConnection);
    }
    static void onIceCandidate(GstElement *, guint mline, gchar *candidate,
                               gpointer data)
    {
        auto *self = static_cast<AvOfferer *>(data);
        QPointer<AvOfferer> guard(self);
        const QString line = QString::fromUtf8(candidate);
        const int index = static_cast<int>(mline);
        QMetaObject::invokeMethod(
            self,
            [guard, index, line] {
                if (guard)
                    Q_EMIT guard->localCandidate(index, line);
            },
            Qt::QueuedConnection);
    }
    static void onPadAdded(GstElement *, GstPad *pad, gpointer data)
    {
        auto *self = static_cast<AvOfferer *>(data);
        if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
            return;
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            caps = gst_pad_query_caps(pad, nullptr);
        bool audio = false;
        if (caps && gst_caps_get_size(caps) > 0) {
            const gchar *media = gst_structure_get_string(
                gst_caps_get_structure(caps, 0), "media");
            audio = g_strcmp0(media, "audio") == 0;
        }
        if (caps)
            gst_caps_unref(caps);
        GstElement *bin = gst_parse_bin_from_description(
            audio ? "rtpopusdepay ! opusdec ! fakesink sync=false"
                  : "fakesink sync=false async=false",
            TRUE, nullptr);
        gst_bin_add(GST_BIN(self->m_pipeline), bin);
        gst_element_sync_state_with_parent(bin);
        GstPad *sink = gst_element_get_static_pad(bin, "sink");
        if (audio) {
            gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, countBuffer,
                              &self->m_audioPackets, nullptr);
        }
        gst_pad_link(pad, sink);
        gst_object_unref(sink);
    }

    GstElement *m_pipeline = nullptr;
    GstElement *m_webrtc = nullptr;
    std::atomic<int> m_audioPackets{0};
};

} // namespace

class CallMediaLoopbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QString whyNot;
        if (!GstCallMediaBackend::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(
                QStringLiteral("webrtc runtime unavailable: ") + whyNot));
    }

    void runtimeProbeIsStable()
    {
        // A second call answers from the cached init and must agree.
        QVERIFY(GstCallMediaBackend::runtimeAvailable());
    }

    /// A promise change function must not touch its context after the
    /// `gst_promise_unref()` that can finalize the promise (whose destroy
    /// notify, `promiseCtxFree`, deletes the context), as when webrtcbin
    /// replies with an error. This pins the precondition: after the reply the
    /// test holds the only reference on the element, so the context was
    /// destroyed synchronously. The use-after-free itself is only visible
    /// under ASan; this case guards the ordering the fix relies on.
    void aPromiseChangeFunctionOutlivesItsOwnContext()
    {
        const int refs =
            GstCallMediaBackend::offerPromiseErrorReplyContextRefsForTest();
        if (refs < 0)
            QSKIP("webrtcbin could not be instantiated here");
        QVERIFY2(refs == 1,
                 qPrintable(QStringLiteral(
                     "the promise context survived its change function "
                     "(%1 references left, expected 1): if that ever becomes "
                     "true the reads below the unref would be safe, and this "
                     "case is the record of why they are hoisted")
                                .arg(refs)));
    }

    void loopbackCallReachesConnectedBothWays()
    {
        const QString callId = QStringLiteral("loopback-1");
        GstCallMediaBackend caller;
        GstCallMediaBackend callee;
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);

        bool callerConnected = false;
        bool calleeConnected = false;
        QString failureCategory;

        // Cross the signalling as CallController does over Matrix.
        bool sawAudioOffer = false;
        bool sawAudioAnswer = false;
        connect(&caller, &CallMediaBackend::offerReady, &callee,
                [&](const QString &id, const QString &sdp) {
                    if (id != callId)
                        return; // the second-call phase reuses the engines
                    sawAudioOffer = sdp.contains(QStringLiteral("m=audio"));
                    callee.createAnswer(callId, sdp);
                });
        connect(&callee, &CallMediaBackend::answerReady, &caller,
                [&](const QString &id, const QString &sdp) {
                    if (id != callId)
                        return;
                    sawAudioAnswer = sdp.contains(QStringLiteral("m=audio"));
                    caller.setRemoteAnswer(callId, sdp);
                });
        connect(&caller, &CallMediaBackend::localCandidate, &callee,
                [&](const QString &id, const QString &candidate,
                    const QString &mid, int mline) {
                    callee.addRemoteCandidate(id, candidate, mid, mline);
                });
        connect(&callee, &CallMediaBackend::localCandidate, &caller,
                [&](const QString &id, const QString &candidate,
                    const QString &mid, int mline) {
                    caller.addRemoteCandidate(id, candidate, mid, mline);
                });
        connect(&caller, &CallMediaBackend::connected, this,
                [&](const QString &id) {
                    if (id == callId)
                        callerConnected = true;
                });
        connect(&callee, &CallMediaBackend::connected, this,
                [&](const QString &id) {
                    if (id == callId)
                        calleeConnected = true;
                });
        const auto noteFailure = [&](const QString &, const QString &why) {
            failureCategory = why;
        };
        connect(&caller, &CallMediaBackend::failed, this, noteFailure);
        connect(&callee, &CallMediaBackend::failed, this, noteFailure);

        caller.createOffer(callId);

        QTRY_VERIFY2_WITH_TIMEOUT(
            callerConnected && calleeConnected,
            qPrintable(QStringLiteral("no connection; failure=%1")
                           .arg(failureCategory)),
            45000);
        QVERIFY(failureCategory.isEmpty());
        QVERIFY(sawAudioOffer);
        QVERIFY(sawAudioAnswer);

        // Tear down cleanly; a second call on the same engines must work.
        caller.close(callId);
        callee.close(callId);

        const QString second = QStringLiteral("loopback-2");
        callerConnected = false;
        calleeConnected = false;
        bool secondOffered = false;
        connect(&caller, &CallMediaBackend::offerReady, this,
                [&](const QString &id, const QString &) {
                    if (id == second)
                        secondOffered = true;
                });
        caller.createOffer(second);
        QTRY_VERIFY_WITH_TIMEOUT(secondOffered, 15000);
        caller.close(second);
    }

    /// Element's video call reaches the legacy lane as an audio+video offer.
    /// The answer must accept the audio and decline the video: before the
    /// fix it answered `video recvonly`, promising to play a track this
    /// engine has no branch for.
    void aVideoOfferIsAnsweredWithTheVideoDeclined()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        QString answer;
        QString failure;
        connect(&engine, &CallMediaBackend::answerReady, this,
                [&](const QString &, const QString &sdp) { answer = sdp; });
        connect(&engine, &CallMediaBackend::failed, this,
                [&](const QString &, const QString &why) { failure = why; });
        const QString offer = crlf(kElementVideoOffer);
        QCOMPARE(GstCallMediaBackend::sdpSectionShape(offer),
                 QStringLiteral("audio:sendrecv video:sendrecv"));

        engine.createAnswer(QStringLiteral("video-offer-1"), offer);
        QTRY_VERIFY2_WITH_TIMEOUT(!answer.isEmpty() || !failure.isEmpty(),
                                  "no answer", 15000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        const QString shape = GstCallMediaBackend::sdpSectionShape(answer);
        // One answer section per offer section (RFC 3264), audio accepted
        // both ways, video declined either way JSEP allows.
        const bool declined =
            shape == QLatin1String("audio:sendrecv video:inactive")
            || (shape.startsWith(QLatin1String("audio:sendrecv video:"))
                && shape.endsWith(QLatin1String(":port0")));
        QVERIFY2(declined, qPrintable(shape));
        // The Opus payload type is the offerer's.
        QVERIFY(answer.contains(QStringLiteral("a=rtpmap:111 OPUS/48000"),
                                Qt::CaseInsensitive));
        engine.close(QStringLiteral("video-offer-1"));
    }

    /// The whole call against an Element-shaped peer: audio must flow both
    /// ways. Before the fix the video track was linked into the Opus chain,
    /// caps negotiation failed, and the not-negotiated return stopped the
    /// bundled transport (`media_pipeline`, or silence).
    void aVideoCallerHearsAndIsHeardOverTheAudioLane()
    {
        AvOfferer peer;
        if (!peer.valid())
            QSKIP("vp8enc or videotestsrc unavailable here");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        const QString callId = QStringLiteral("video-call-1");
        QString failure;
        QString answerShape;
        bool connectedOk = false;
        bool answered = false;

        connect(&peer, &AvOfferer::offerReady, this,
                [&](const QString &sdp, int sections) {
                    if (answered)
                        return;
                    if (sections < 2) {
                        // Caps not negotiated yet; ask again shortly.
                        QTimer::singleShot(250, &peer,
                                           [&peer] { peer.createOffer(); });
                        return;
                    }
                    answered = true;
                    engine.createAnswer(callId, sdp);
                });
        connect(&engine, &CallMediaBackend::answerReady, this,
                [&](const QString &, const QString &sdp) {
                    answerShape = GstCallMediaBackend::sdpSectionShape(sdp);
                    peer.setRemoteAnswer(sdp);
                });
        connect(&peer, &AvOfferer::localCandidate, this,
                [&](int mline, const QString &candidate) {
                    engine.addRemoteCandidate(callId, candidate, QString(),
                                              mline);
                });
        connect(&engine, &CallMediaBackend::localCandidate, this,
                [&](const QString &, const QString &candidate,
                    const QString &, int mline) {
                    peer.addRemoteCandidate(mline, candidate);
                });
        connect(&engine, &CallMediaBackend::connected, this,
                [&](const QString &) { connectedOk = true; });
        connect(&engine, &CallMediaBackend::failed, this,
                [&](const QString &, const QString &why) { failure = why; });

        peer.createOffer();
        QTRY_VERIFY2_WITH_TIMEOUT(
            connectedOk || !failure.isEmpty(),
            qPrintable(QStringLiteral("no connection; answer=%1")
                           .arg(answerShape)),
            45000);
        // Audio in both directions, sustained well past the moment the first
        // video packet arrives (the old failure came within a second).
        QTRY_VERIFY2_WITH_TIMEOUT(
            (engine.receivedAudioPacketsForTest() > 150
             && peer.audioPacketsReceived() > 150) || !failure.isEmpty(),
            qPrintable(QStringLiteral("audio in=%1 out=%2 failure=%3")
                           .arg(engine.receivedAudioPacketsForTest())
                           .arg(peer.audioPacketsReceived())
                           .arg(failure)),
            30000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(answerShape.startsWith(QLatin1String("audio:sendrecv video:"))
                     && answerShape != QLatin1String(
                         "audio:sendrecv video:recvonly"),
                 qPrintable(answerShape));
        if (engine.drainedReceivePadsForTest() == 0) {
            qWarning("the peer sent no video after the inactive answer, so "
                     "the receive drain was not exercised in this run");
        }
        engine.close(callId);
    }

    void badRemoteOfferFailsCleanly()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        QSignalSpy failed(&engine, &CallMediaBackend::failed);
        engine.createAnswer(QStringLiteral("bad-1"),
                            QStringLiteral("this is not sdp"));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= 1, 10000);
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("bad_remote_offer"));
        // The engine recovered: a fresh offer session starts.
        bool offered = false;
        connect(&engine, &CallMediaBackend::offerReady, this,
                [&](const QString &, const QString &) { offered = true; });
        engine.createOffer(QStringLiteral("good-after-bad"));
        QTRY_VERIFY_WITH_TIMEOUT(offered, 15000);
        engine.close(QStringLiteral("good-after-bad"));
    }
};

QTEST_GUILESS_MAIN(CallMediaLoopbackTest)
#include "CallMediaLoopbackTest.moc"
