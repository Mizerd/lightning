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

#if defined(Q_OS_UNIX)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "calls/GstCallMediaBackend.h"
#include "calls/RtpVp8Payloader.h"

namespace {

// A UDP port on 127.0.0.1 that never answers. Named as the STUN server it
// keeps ICE gathering running until libnice gives up on it (about 2 s,
// measured with libnice 0.1.23), which is the window GitHub #3 needs.
// Nothing reaches it: libnice pins each host candidate's socket to its
// interface (IP_UNICAST_IF), so loopback is unreachable, which only makes
// the silence certain.
class SilentUdpPort
{
public:
    SilentUdpPort()
    {
#if defined(Q_OS_UNIX)
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof address;
        if (::bind(m_fd, reinterpret_cast<sockaddr *>(&address), length) == 0
            && ::getsockname(m_fd, reinterpret_cast<sockaddr *>(&address),
                             &length)
                == 0) {
            m_port = ntohs(address.sin_port);
        }
#endif
    }
    ~SilentUdpPort()
    {
#if defined(Q_OS_UNIX)
        if (m_fd >= 0)
            ::close(m_fd);
#endif
    }
    int port() const { return m_port; }
    QStringList asStunServer() const
    {
        return { QStringLiteral("stun:127.0.0.1:%1").arg(m_port) };
    }

private:
    int m_fd = -1;
    int m_port = 0;
};

// How long a retirement in these tests takes: their silent STUN/TURN servers
// hold gathering only for libnice's own timers (about 2 s over UDP, 7.9 s over
// TCP), which the SOFT bound covers, and the webrtcbin stops as soon as that
// gathering ends. Not the hard bound (kGatheringHardBoundMs, 60 s): a test
// that needed it would be measuring a gathering that never ends, and should
// shorten the bound with setRetireBoundForTest() instead of waiting a minute.
constexpr int kRetiredOnceGatheringEndsWithinMs =
    lightning::webrtc::Retirer::kGatheringBoundMs + 3000;
static_assert(kRetiredOnceGatheringEndsWithinMs
                  < lightning::webrtc::Retirer::kGatheringHardBoundMs,
              "the wait is meant to be shorter than the hard bound");

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

namespace {
/// A capture whose start blocks the thread that starts it, as one opening a
/// sound server that does not answer does (~8.7 s measured, 2026-09-29).
struct SlowStartMic11 {
    GstBin parent;
};
struct SlowStartMic11Class {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStartMic11, slow_start_mic11, GST_TYPE_BIN)

constexpr int kSlowMicStartMs = 2500;

GstStateChangeReturn slowStartMic11ChangeState(GstElement *element,
                                               GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        g_usleep(gulong(kSlowMicStartMs) * 1000);
    return GST_ELEMENT_CLASS(slow_start_mic11_parent_class)
        ->change_state(element, transition);
}

void slow_start_mic11_init(SlowStartMic11 *self)
{
    GstElement *device = gst_element_factory_make("audiotestsrc", nullptr);
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", inner));
    gst_object_unref(inner);
}

void slow_start_mic11_class_init(SlowStartMic11Class *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-start microphone (test)",
        "Source/Audio", "A capture whose sound server answers late",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStartMic11ChangeState;
}

bool registerSlowStartMic11()
{
    static const bool registered = gst_element_register(
        nullptr, "lightningslowstartmic11", GST_RANK_NONE,
        slow_start_mic11_get_type());
    return registered;
}

const QString kSlowFront = QStringLiteral(
    "lightningslowstartmic11 name=micsrc ! audioconvert ! audioresample");

/// A capture that starts at once and then blocks the thread that STOPS it,
/// as one whose sound server stopped answering mid-call does.
struct SlowStopMic11 {
    GstBin parent;
};
struct SlowStopMic11Class {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStopMic11, slow_stop_mic11, GST_TYPE_BIN)

GstStateChangeReturn slowStopMic11ChangeState(GstElement *element,
                                              GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
        g_usleep(gulong(kSlowMicStartMs) * 1000);
    return GST_ELEMENT_CLASS(slow_stop_mic11_parent_class)
        ->change_state(element, transition);
}

void slow_stop_mic11_init(SlowStopMic11 *self)
{
    GstElement *device = gst_element_factory_make("audiotestsrc", nullptr);
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", inner));
    gst_object_unref(inner);
}

void slow_stop_mic11_class_init(SlowStopMic11Class *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-stop microphone (test)",
        "Source/Audio", "A capture whose sound server stopped answering",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStopMic11ChangeState;
}

/// An output whose device opening blocks the thread that opens it.
struct SlowOpenSink11 {
    GstBin parent;
};
struct SlowOpenSink11Class {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowOpenSink11, slow_open_sink11, GST_TYPE_BIN)

GstStateChangeReturn slowOpenSink11ChangeState(GstElement *element,
                                               GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_NULL_TO_READY)
        g_usleep(gulong(kSlowMicStartMs) * 1000);
    return GST_ELEMENT_CLASS(slow_open_sink11_parent_class)
        ->change_state(element, transition);
}

void slow_open_sink11_init(SlowOpenSink11 *self)
{
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    gst_bin_add(GST_BIN(self), sink);
    GstPad *inner = gst_element_get_static_pad(sink, "sink");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("sink", inner));
    gst_object_unref(inner);
}

void slow_open_sink11_class_init(SlowOpenSink11Class *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-open output (test)", "Sink/Audio",
        "An output whose sound server answers late", "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowOpenSink11ChangeState;
}

bool registerSlowOpenSink11()
{
    static const bool registered = gst_element_register(
        nullptr, "lightningslowopensink11", GST_RANK_NONE,
        slow_open_sink11_get_type());
    return registered;
}

bool registerSlowStopMic11()
{
    static const bool registered = gst_element_register(
        nullptr, "lightningslowstopmic11", GST_RANK_NONE,
        slow_stop_mic11_get_type());
    return registered;
}
} // namespace

/// Two engines wired as CallController wires them, renegotiation
/// included. Everything the video cases need.
struct WiredPair {
    GstCallMediaBackend caller;
    GstCallMediaBackend callee;
    QString callId;
    QString failure;
    QString offerShape;
    QString answerShape;
    QString renegotiationShape;
    bool connected = false;
    int negotiations = 0;
    bool dropNegotiations = false;
    QObject guard;

    explicit WiredPair(const QString &id) : callId(id)
    {
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);
        QObject::connect(&caller, &CallMediaBackend::offerReady, &guard,
                         [this](const QString &, const QString &sdp) {
                             offerShape =
                                 GstCallMediaBackend::sdpSectionShape(sdp);
                             callee.createAnswer(callId, sdp);
                         });
        QObject::connect(&callee, &CallMediaBackend::answerReady, &guard,
                         [this](const QString &, const QString &sdp) {
                             answerShape =
                                 GstCallMediaBackend::sdpSectionShape(sdp);
                             caller.setRemoteAnswer(callId, sdp);
                         });
        const auto cross = [this](GstCallMediaBackend *from,
                                  GstCallMediaBackend *to) {
            QObject::connect(
                from, &CallMediaBackend::localCandidate, &guard,
                [to](const QString &id, const QString &candidate,
                     const QString &mid, int mline) {
                    to->addRemoteCandidate(id, candidate, mid, mline);
                });
            QObject::connect(
                from, &CallMediaBackend::negotiationReady, &guard,
                [this, to](const QString &, const QString &type,
                           const QString &sdp) {
                    if (dropNegotiations)
                        return;
                    ++negotiations;
                    renegotiationShape =
                        GstCallMediaBackend::sdpSectionShape(sdp);
                    to->applyNegotiation(callId, type, sdp);
                });
            QObject::connect(from, &CallMediaBackend::failed, &guard,
                             [this](const QString &, const QString &why) {
                                 failure = why;
                             });
        };
        cross(&caller, &callee);
        cross(&callee, &caller);
        QObject::connect(&caller, &CallMediaBackend::connected, &guard,
                         [this](const QString &) { connected = true; });
    }
    ~WiredPair()
    {
        caller.close(callId);
        callee.close(callId);
    }
};


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

    /// webrtcbin and dtlssrtpenc create rtpbin, rtpfunnel and srtpenc/dec
    /// themselves, so nothing we build names them, and a package missing one
    /// (a snap without libsrtp2) passed the probe and carried no media. Each
    /// is taken out of the registry in turn: the probe must name it.
    void theProbeNamesElementsWebrtcbinCreatesItself()
    {
        GstRegistry *registry = gst_registry_get();
        for (const char *name : { "srtpenc", "srtpdec", "rtpbin", "rtpfunnel" }) {
            GstPluginFeature *feature =
                gst_registry_lookup_feature(registry, name);
            if (!feature)
                QSKIP(qPrintable(QStringLiteral("%1 is not installed here")
                                     .arg(QString::fromLatin1(name))));
            gst_registry_remove_feature(registry, feature);
            QString why;
            const bool available = GstCallMediaBackend::runtimeAvailable(&why);
            // Back before asserting, so a failure cannot strip the registry
            // for the cases after this one.
            gst_registry_add_feature(registry, feature); // refs it again
            gst_object_unref(feature);
            QVERIFY2(!available,
                     qPrintable(QStringLiteral("the probe passed without %1")
                                    .arg(QString::fromLatin1(name))));
            QCOMPARE(why, QStringLiteral("missing_element_")
                              + QString::fromLatin1(name));
        }
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

    void theMicrophonePayloaderUsesTheSharedMtu()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        const QString callId = QStringLiteral("mtu-test");
        engine.createOffer(callId);
        const unsigned int mtu = engine.microphoneRtpMtuForTest();
        engine.close(callId);
        QCOMPARE(mtu, lightning::rtp::kRtpPayloadMtu);
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
        // The other close GitHub #3 aborted in: the second session is still
        // gathering.
        caller.close(second);
        QTRY_COMPARE_WITH_TIMEOUT(caller.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(callee.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(caller.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(callee.teardownsWhileGatheringForTest(), 0);
    }

    /// Rokas, 2026-10-08: "Lightning always used my default microphone, no
    /// matter which mic I picked in the call's device menu." A pick mid-call
    /// was stored for the NEXT call only. Now the live call moves: the caller
    /// starts on a silent fake microphone and switches to a loud one, and the
    /// far end must hear the loud one; then the callee switches its speaker
    /// and must keep playing. Neither switch may renegotiate (a second offer
    /// would reach the peer as a new invite) or end the call.
    void aDeviceSwitchReachesTheLiveCall()
    {
        const QString callId = QStringLiteral("device-switch-1");
        GstCallMediaBackend caller;
        GstCallMediaBackend callee;
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);
        bool loudMicrophone = false;
        bool secondSpeaker = false;
        caller.setAudioDeviceResolver([&loudMicrophone] {
            GstCallMediaBackend::AudioDevicePlan plan;
            plan.microphoneFront = loudMicrophone
                // Shaped like SfuMediaEngine::microphoneFrontDescription():
                // it ends in a caps shorthand, which the parser rejects as
                // the last item of a bin (the first live run failed on it).
                ? QStringLiteral("audiotestsrc is-live=true wave=sine "
                                 "freq=1000 volume=0.5 name=micsrc "
                                 "! audioconvert ! audioresample "
                                 "! audio/x-raw,channels=1")
                : QStringLiteral("audiotestsrc is-live=true wave=silence "
                                 "name=micsrc ! audioconvert ! audioresample");
            return plan;
        });
        callee.setAudioDeviceResolver([&secondSpeaker] {
            GstCallMediaBackend::AudioDevicePlan plan;
            plan.speakerSink = secondSpeaker
                ? QStringLiteral("fakesink sync=false async=false "
                                 "name=outsink")
                : QStringLiteral("fakesink sync=false name=outsink");
            return plan;
        });

        int offers = 0;
        bool callerConnected = false;
        bool calleeConnected = false;
        QString failure;
        connect(&caller, &CallMediaBackend::offerReady, &callee,
                [&](const QString &id, const QString &sdp) {
                    ++offers;
                    callee.createAnswer(id, sdp);
                });
        connect(&callee, &CallMediaBackend::answerReady, &caller,
                [&](const QString &id, const QString &sdp) {
                    caller.setRemoteAnswer(id, sdp);
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
                [&](const QString &) { callerConnected = true; });
        connect(&callee, &CallMediaBackend::connected, this,
                [&](const QString &) { calleeConnected = true; });
        const auto noteFailure = [&](const QString &, const QString &why) {
            failure = why;
        };
        connect(&caller, &CallMediaBackend::failed, this, noteFailure);
        connect(&callee, &CallMediaBackend::failed, this, noteFailure);

        caller.createOffer(callId);
        QTRY_VERIFY2_WITH_TIMEOUT(
            (callerConnected && calleeConnected) || !failure.isEmpty(),
            qPrintable(QStringLiteral("no connection; failure=%1")
                           .arg(failure)),
            45000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_VERIFY_WITH_TIMEOUT(callee.receivedAudioPacketsForTest() > 50,
                                 15000);

        // The silent microphone: what arrives is silence.
        callee.resetReceivedPeakForTest();
        QTest::qWait(1000);
        QVERIFY2(callee.receivedPeakForTest() < 0.02,
                 qPrintable(QString::number(callee.receivedPeakForTest())));

        // Mid-call pick of the loud microphone.
        loudMicrophone = true;
        caller.audioDevicesChanged();
        QCOMPARE(caller.microphoneSwapsForTest(), 1);
        callee.resetReceivedPeakForTest();
        QTRY_VERIFY2_WITH_TIMEOUT(
            callee.receivedPeakForTest() > 0.2 || !failure.isEmpty(),
            qPrintable(QStringLiteral("far end peak %1 after the switch; "
                                      "failure=%2")
                           .arg(callee.receivedPeakForTest())
                           .arg(failure)),
            10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        // A repeated notice with nothing changed (a camera pick fires the same
        // signal) must not touch the microphone.
        caller.audioDevicesChanged();
        QCOMPARE(caller.microphoneSwapsForTest(), 1);

        // Mid-call pick of another speaker: the track keeps playing through
        // the new sink (a sink that did not take over would stall the chain
        // and the peak would stop moving).
        secondSpeaker = true;
        callee.audioDevicesChanged();
        QTRY_COMPARE_WITH_TIMEOUT(callee.speakerSwapsForTest(), 1, 5000);
        callee.resetReceivedPeakForTest();
        QTRY_VERIFY2_WITH_TIMEOUT(
            callee.receivedPeakForTest() > 0.2 || !failure.isEmpty(),
            qPrintable(QStringLiteral("playback stalled after the speaker "
                                      "switch; failure=%1")
                           .arg(failure)),
            10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Neither switch renegotiated.
        QCOMPARE(offers, 1);

        caller.close(callId);
        callee.close(callId);
    }

    /// Element's video call reaches the legacy lane as an audio+video offer.
    /// The answer must accept the audio and decline the video: before the
    /// fix it answered `video recvonly`, promising to play a track this
    /// engine has no branch for.
    /// The claim the matrix.org feature row makes: a picture and sound, both
    /// ways, in one call, over the same bundled transport.
    void aVideoCallCarriesPictureAndSoundBothWays()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("video-both-ways"));
        pair.caller.prepareCall(pair.callId, true, true);
        pair.callee.prepareCall(pair.callId, true, true);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(pair.connected || !pair.failure.isEmpty(),
                                  "no connection", 45000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
        // Port 0 and a=bundle-only: webrtcbin's max-bundle offer.
        QCOMPARE(pair.offerShape,
                 QStringLiteral("audio:sendrecv video:sendrecv:port0"));
        QCOMPARE(pair.answerShape,
                 QStringLiteral("audio:sendrecv video:sendrecv"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            pair.caller.decodedVideoFramesForTest() > 20
                && pair.callee.decodedVideoFramesForTest() > 20,
            qPrintable(QStringLiteral("frames caller=%1 callee=%2 sent=%3/%4")
                           .arg(pair.caller.decodedVideoFramesForTest())
                           .arg(pair.callee.decodedVideoFramesForTest())
                           .arg(pair.caller.sentVideoFramesForTest())
                           .arg(pair.callee.sentVideoFramesForTest())),
            30000);
        QTRY_VERIFY_WITH_TIMEOUT(pair.caller.receivedAudioPacketsForTest() > 100
                                     && pair.callee.receivedAudioPacketsForTest()
                                         > 100,
                                 15000);
        QCOMPARE(pair.negotiations, 0);
        QVERIFY(pair.failure.isEmpty());
    }

    /// Answering a video call without the camera: the section stays, the
    /// caller's picture arrives, and turning the camera on later needs no
    /// renegotiation. Off closes the capture and the picture stops.
    void theCameraTogglesWithoutRenegotiating()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("camera-toggle"));
        pair.caller.prepareCall(pair.callId, true, true);
        pair.callee.prepareCall(pair.callId, true, false);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(
            pair.callee.decodedVideoFramesForTest() > 10
                || !pair.failure.isEmpty(),
            qPrintable(QStringLiteral("frames=%1 answer=%2")
                           .arg(pair.callee.decodedVideoFramesForTest())
                           .arg(pair.answerShape)),
            45000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
        QCOMPARE(pair.answerShape,
                 QStringLiteral("audio:sendrecv video:sendrecv"));
        QVERIFY(!pair.callee.cameraFrontLiveForTest());
        QCOMPARE(pair.caller.decodedVideoFramesForTest(), 0);

        pair.callee.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY_WITH_TIMEOUT(pair.caller.decodedVideoFramesForTest() > 10,
                                 20000);
        pair.callee.setCameraEnabled(pair.callId, false);
        QVERIFY(!pair.callee.cameraFrontLiveForTest());
        QTest::qWait(600);
        const int settled = pair.caller.decodedVideoFramesForTest();
        QTest::qWait(1000);
        QCOMPARE(pair.caller.decodedVideoFramesForTest(), settled);
        pair.callee.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY_WITH_TIMEOUT(
            pair.caller.decodedVideoFramesForTest() > settled + 10, 20000);
        QCOMPARE(pair.negotiations, 0);
        QVERIFY(pair.failure.isEmpty());
    }

    /// A voice call gains a picture through m.call.negotiate, from either
    /// end, and the audio survives it.
    void aVoiceCallUpgradesToVideoFromEitherEnd_data()
    {
        QTest::addColumn<bool>("callerUpgrades");
        QTest::newRow("caller") << true;
        QTest::newRow("callee") << false;
    }
    void aVoiceCallUpgradesToVideoFromEitherEnd()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        QFETCH(bool, callerUpgrades);
        WiredPair pair(QStringLiteral("upgrade"));
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(pair.connected || !pair.failure.isEmpty(),
                                  "no connection", 45000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
        QCOMPARE(pair.offerShape, QStringLiteral("audio:sendrecv"));
        QTRY_VERIFY_WITH_TIMEOUT(pair.callee.receivedAudioPacketsForTest() > 50,
                                 15000);

        GstCallMediaBackend &up = callerUpgrades ? pair.caller : pair.callee;
        GstCallMediaBackend &far = callerUpgrades ? pair.callee : pair.caller;
        up.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY2_WITH_TIMEOUT(
            pair.negotiations >= 2 || !pair.failure.isEmpty(),
            qPrintable(QStringLiteral("negotiations=%1")
                           .arg(pair.negotiations)),
            15000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
        QCOMPARE(pair.negotiations, 2);
        QVERIFY2(pair.renegotiationShape.contains(QLatin1String("video:")),
                 qPrintable(pair.renegotiationShape));
        QTRY_VERIFY2_WITH_TIMEOUT(
            far.decodedVideoFramesForTest() > 15,
            qPrintable(QStringLiteral("frames=%1 sent=%2 packets=%3")
                           .arg(far.decodedVideoFramesForTest())
                           .arg(up.sentVideoFramesForTest())
                           .arg(far.receivedVideoPacketsForTest())),
            30000);
        const int before = pair.callee.receivedAudioPacketsForTest();
        QTRY_VERIFY_WITH_TIMEOUT(
            pair.callee.receivedAudioPacketsForTest() > before + 25, 10000);

        // The other side's camera now needs no second negotiation.
        far.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY_WITH_TIMEOUT(up.decodedVideoFramesForTest() > 15, 30000);
        QCOMPARE(pair.negotiations, 2);
        QVERIFY(pair.failure.isEmpty());
    }

    /// Two people turn their cameras on at once. The callee waits a moment
    /// before offering, so the caller's offer serves both and each sees the
    /// other. Offers that really cross cannot be repaired: webrtcbin aborts
    /// the process answering after a rollback (requestOfferLocked()).
    void offersThatCrossSettleOnTheCallersAndBothCamerasStillWork()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("crossed"));
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(pair.connected || !pair.failure.isEmpty(),
                                  "no connection", 45000);
        QTRY_VERIFY_WITH_TIMEOUT(pair.callee.receivedAudioPacketsForTest() > 20,
                                 15000);
        pair.caller.setCameraEnabled(pair.callId, true);
        pair.callee.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY2_WITH_TIMEOUT(
            (pair.caller.decodedVideoFramesForTest() > 15
             && pair.callee.decodedVideoFramesForTest() > 15)
                || !pair.failure.isEmpty(),
            qPrintable(QStringLiteral("frames caller=%1 callee=%2 negotiations=%3")
                           .arg(pair.caller.decodedVideoFramesForTest())
                           .arg(pair.callee.decodedVideoFramesForTest())
                           .arg(pair.negotiations)),
            30000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
    }

    /// An offer nobody answers (a lost send, a silent peer) ends in a camera
    /// failure, not a stuck camera, the call goes on, and the camera can try
    /// again.
    void anUnansweredOfferFailsTheCameraAndCanBeRetried()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("unanswered"));
        pair.caller.setNegotiationTimeoutForTest(500);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(pair.connected || !pair.failure.isEmpty(),
                                  "no connection", 45000);
        QString cameraFailure;
        connect(&pair.caller, &CallMediaBackend::cameraFailed, this,
                [&](const QString &, const QString &why) {
                    cameraFailure = why;
                });
        pair.dropNegotiations = true;
        pair.caller.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY_WITH_TIMEOUT(!cameraFailure.isEmpty(), 5000);
        QVERIFY(!pair.caller.cameraFrontLiveForTest());
        pair.dropNegotiations = false;
        pair.caller.setCameraEnabled(pair.callId, true);
        QTRY_VERIFY2_WITH_TIMEOUT(
            pair.callee.decodedVideoFramesForTest() > 10,
            qPrintable(QStringLiteral("negotiations=%1 failure=%2")
                           .arg(pair.negotiations)
                           .arg(pair.failure)),
            20000);
        QVERIFY(pair.failure.isEmpty());
    }

    /// A picture that cannot be decoded costs the picture. Its chain's errors
    /// are not the call's, and audio keeps flowing past it.
    void aFailedVideoReceiveLeavesTheAudioRunning()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("receive-failure"));
        pair.caller.prepareCall(pair.callId, true, true);
        pair.callee.prepareCall(pair.callId, true, false);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY_WITH_TIMEOUT(pair.callee.decodedVideoFramesForTest() > 5
                                     || !pair.failure.isEmpty(),
                                 45000);
        pair.callee.failVideoReceiveForTest();
        QTest::qWait(300);
        const int frames = pair.callee.decodedVideoFramesForTest();
        const int audio = pair.callee.receivedAudioPacketsForTest();
        QTest::qWait(1000);
        QCOMPARE(pair.callee.decodedVideoFramesForTest(), frames);
        QVERIFY(pair.callee.receivedAudioPacketsForTest() > audio + 20);
        QVERIFY(pair.failure.isEmpty());
    }

    /// Our camera failing says nothing about the peer's picture.
    void aFailedCameraLeavesThePeersPictureArriving()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("camera-only"));
        pair.caller.prepareCall(pair.callId, true, true);
        pair.callee.prepareCall(pair.callId, true, true);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY_WITH_TIMEOUT(pair.callee.decodedVideoFramesForTest() > 5
                                     || !pair.failure.isEmpty(),
                                 45000);
        pair.callee.failCameraForTest();
        QVERIFY(!pair.callee.cameraFrontLiveForTest());
        QTest::qWait(300);
        const int frames = pair.callee.decodedVideoFramesForTest();
        QTRY_VERIFY_WITH_TIMEOUT(
            pair.callee.decodedVideoFramesForTest() > frames + 5, 5000);
    }

    /// A camera that dies mid-call costs the picture and never the call.
    void aFailedCameraLeavesTheCallRunning()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        WiredPair pair(QStringLiteral("camera-failure"));
        pair.caller.setCameraResolver([](int) {
            GstCallMediaBackend::CameraPlan plan;
            // Fails at runtime, in the front, after the call is up.
            plan.front = QStringLiteral(
                "videotestsrc is-live=true num-buffers=5 name=camsrc "
                "! video/x-raw,width=64,height=36,framerate=(fraction)30/1 "
                "! fakesrc-does-not-exist");
            return plan;
        });
        QString cameraFailure;
        connect(&pair.caller, &CallMediaBackend::cameraFailed, this,
                [&](const QString &, const QString &why) {
                    cameraFailure = why;
                });
        pair.caller.prepareCall(pair.callId, true, true);
        pair.callee.prepareCall(pair.callId, true, false);
        pair.caller.createOffer(pair.callId);
        QTRY_VERIFY2_WITH_TIMEOUT(pair.connected || !pair.failure.isEmpty(),
                                  "no connection", 45000);
        QCOMPARE(cameraFailure, QStringLiteral("camera_failed"));
        QTRY_VERIFY_WITH_TIMEOUT(pair.callee.receivedAudioPacketsForTest() > 50,
                                 15000);
        QVERIFY2(pair.failure.isEmpty(), qPrintable(pair.failure));
    }

    /// Placing a call with the microphone already muted (the intent survives
    /// between calls): the valve shuts before the first buffer, so webrtcbin
    /// never saw caps on its pad, no offer came, and the call died after 15 s
    /// as a media failure with nothing sent. Found in the GUI, 2026-10-31.
    void aCallPlacedWhileMutedStillMakesItsOffer()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        // A capture that has not delivered anything, as a shut valve leaves it.
        engine.setMicrophoneFrontForTest(QStringLiteral(
            "audiotestsrc is-live=true name=micsrc ! valve drop=true "
            "! audioconvert ! audioresample"));
        QString offer;
        connect(&engine, &CallMediaBackend::offerReady, this,
                [&](const QString &, const QString &sdp) { offer = sdp; });
        engine.createOffer(QStringLiteral("muted-offer"));
        QTRY_VERIFY2_WITH_TIMEOUT(!offer.isEmpty(), "no offer", 6000);
        QCOMPARE(GstCallMediaBackend::sdpSectionShape(offer),
                 QStringLiteral("audio:sendrecv"));
        QVERIFY(offer.contains(QStringLiteral("OPUS/48000/2"),
                               Qt::CaseInsensitive));
        QVERIFY(offer.contains(QRegularExpression(
            QStringLiteral("a=ssrc:[1-9][0-9]* cname:"))));
        engine.close(QStringLiteral("muted-offer"));
    }

    /// Element's video offer, against an engine that has video: both sections
    /// accepted. The chain must exist before the description is applied, or
    /// the section is answered with nothing to send.
    void aVp8OfferIsAnsweredWithItsVideo()
    {
        if (!GstCallMediaBackend::videoRuntimeAvailable())
            QSKIP("VP8 elements unavailable here");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        QString answer;
        QString failure;
        connect(&engine, &CallMediaBackend::answerReady, this,
                [&](const QString &, const QString &sdp) { answer = sdp; });
        connect(&engine, &CallMediaBackend::failed, this,
                [&](const QString &, const QString &why) { failure = why; });
        engine.prepareCall(QStringLiteral("vp8-offer"), true, false);
        engine.createAnswer(QStringLiteral("vp8-offer"),
                            crlf(kElementVideoOffer));
        QTRY_VERIFY2_WITH_TIMEOUT(!answer.isEmpty() || !failure.isEmpty(),
                                  "no answer", 15000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(GstCallMediaBackend::sdpSectionShape(answer),
                 QStringLiteral("audio:sendrecv video:sendrecv"));
        // The offerer's payload type, and our one stream named in metadata.
        const QString stream = engine.localStreamId();
        QVERIFY(!stream.isEmpty());
        QVERIFY2(answer.count(QStringLiteral(" msid:") + stream) == 2,
                 qPrintable(answer));
        QVERIFY(answer.contains(QStringLiteral("VP8/90000"),
                                Qt::CaseInsensitive));
        engine.close(QStringLiteral("vp8-offer"));
    }

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
        // A video codec this engine has no chain for: an offer that names
        // no VP8 at all.
        const QString offer = crlf(kElementVideoOffer)
                                  .replace(QStringLiteral("VP8/90000"),
                                           QStringLiteral("H265/90000"));
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

    /// Element's legacy video call heard nothing from Lightning (2026-10-08):
    /// the answer named no SSRC for our audio, because webrtcbin takes it from
    /// caps on its sink pad and a real microphone had not delivered a buffer
    /// yet. Chromium then cannot demux our RTP in a two-section BUNDLE and
    /// drops all of it. A capture that delivers nothing before the answer is
    /// exactly that case: the answer must still carry a=ssrc.
    void anAnswerNamesItsSsrcBeforeTheMicrophoneDelivers()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setAudioDeviceResolver([] {
            GstCallMediaBackend::AudioDevicePlan plan;
            // A microphone that has not produced its first buffer.
            plan.microphoneFront = QStringLiteral(
                "audiotestsrc is-live=true name=micsrc ! valve drop=true "
                "! audioconvert ! audioresample ! audio/x-raw,channels=1");
            return plan;
        });
        QString answer;
        QString failure;
        connect(&engine, &CallMediaBackend::answerReady, this,
                [&](const QString &, const QString &sdp) { answer = sdp; });
        connect(&engine, &CallMediaBackend::failed, this,
                [&](const QString &, const QString &why) { failure = why; });
        engine.createAnswer(QStringLiteral("ssrc-1"), crlf(kElementVideoOffer));
        QTRY_VERIFY2_WITH_TIMEOUT(!answer.isEmpty() || !failure.isEmpty(),
                                  "no answer", 15000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // The audio section (the first) names its SSRC.
        const int video = answer.indexOf(QStringLiteral("m=video"));
        const QString audioSection =
            video > 0 ? answer.left(video) : answer;
        QVERIFY2(audioSection.contains(QRegularExpression(
                     QStringLiteral("a=ssrc:[1-9][0-9]* cname:"))),
                 qPrintable(audioSection));
        engine.close(QStringLiteral("ssrc-1"));
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
        // GitHub #3's abort came from this close: it lands while the fresh
        // session is still gathering.
        engine.close(QStringLiteral("good-after-bad"));
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
    }

    /// GitHub #3: a webrtcbin set to NULL while its ICE gathering can still
    /// emit a candidate aborts the process on every GStreamer we ship
    /// (gstreamer#5138). A STUN server that never answers holds gathering
    /// open; the call closes on its first candidate, when gathering has
    /// certainly started. The webrtcbin must reach NULL only after the
    /// gathering has ended, and within the bound.
    void closingWhileIceGathersStopsWebrtcbinOnlyAfterGathering()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setIceServers(stun.asStunServer(), QString(), QString());
        const QString callId = QStringLiteral("close-on-first-candidate");
        bool closed = false;
        connect(&engine, &CallMediaBackend::localCandidate, this,
                [&](const QString &id, const QString &, const QString &, int) {
                    if (id != callId || closed)
                        return;
                    closed = true;
                    engine.close(callId);
                });
        engine.createOffer(callId);
        QTRY_VERIFY_WITH_TIMEOUT(closed, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
        QVERIFY2(engine.retiresThatWaitedOnGatheringForTest() == 1,
                 "gathering had already ended when the call closed, so this "
                 "run proved nothing: the silent STUN server did not hold it "
                 "open (no IPv4 host candidate to discover from?)");
        QCOMPARE(engine.retireBoundExpiriesForTest(), 0);
    }

    /// The same, closed the moment the offer is ready, which is the shape of
    /// the reporter's crash: set-local-description may not have run yet, so
    /// the close has to wait it out before it can see that gathering began.
    void closingTheMomentTheOfferIsReadyWaitsForGatheringToo()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setIceServers(stun.asStunServer(), QString(), QString());
        const QString callId = QStringLiteral("close-on-offer");
        bool closed = false;
        connect(&engine, &CallMediaBackend::offerReady, this,
                [&](const QString &id, const QString &) {
                    if (id != callId || closed)
                        return;
                    closed = true;
                    engine.close(callId);
                });
        engine.createOffer(callId);
        QTRY_VERIFY_WITH_TIMEOUT(closed, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
        QVERIFY2(engine.retiresThatWaitedOnGatheringForTest() == 1,
                 "the close did not see the gathering the offer started");
        QCOMPARE(engine.retireBoundExpiriesForTest(), 0);
    }

    /// A call closed before its answer exists must not go on to create one:
    /// a set-local-description queued after the close would start gathering
    /// behind the teardown's back. Nothing is gathered, so the webrtcbin
    /// stops at once, well before the silent server would let it.
    void closingBeforeTheAnswerExistsStartsNoGathering()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        // close() races webrtcbin's own thread creating the answer; under
        // heavy load the answer can win, which is correct code taking the
        // other path. One clean attempt of three proves the gate; without it
        // every attempt gathers.
        int clean = 0;
        for (int attempt = 0; attempt < 3 && clean == 0; ++attempt) {
            GstCallMediaBackend engine;
            engine.setTestToneMode(true);
            engine.setIceServers(stun.asStunServer(), QString(), QString());
            const QString callId =
                QStringLiteral("close-before-answer-%1").arg(attempt);
            engine.createAnswer(callId, crlf(kElementVideoOffer));
            engine.close(callId);
            // Well inside the ~2 s the silent server would hold a gathering.
            const bool settled = QTest::qWaitFor(
                [&engine] { return engine.retiringWebrtcForTest() == 0; },
                1500);
            if (settled && engine.retiresThatWaitedOnGatheringForTest() == 0
                && engine.teardownsWhileGatheringForTest() == 0)
                ++clean;
        }
        QVERIFY2(clean > 0,
                 "every close before the answer still started a gathering");
    }

    /// The bound is real: a webrtcbin whose gathering outlasts it is stopped
    /// anyway, and counted. Safe here because the silent server can never
    /// produce the late candidate the crash needs.
    void aWebrtcbinStillGatheringAtTheBoundIsStoppedAnyway()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setRetireBoundForTest(300);
        engine.setIceServers(stun.asStunServer(), QString(), QString());
        const QString callId = QStringLiteral("close-past-the-bound");
        bool closed = false;
        connect(&engine, &CallMediaBackend::localCandidate, this,
                [&](const QString &id, const QString &, const QString &, int) {
                    if (id != callId || closed)
                        return;
                    closed = true;
                    engine.close(callId);
                });
        engine.createOffer(callId);
        QTRY_VERIFY_WITH_TIMEOUT(closed, 15000);
        // libnice gives up on the silent server only after about 2 s.
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0, 1500);
        QCOMPARE(engine.retireBoundExpiriesForTest(), 1);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 1);
    }

    /// Past the soft bound a retirement is REPORTED and keeps waiting: a
    /// gathering that outlives libnice's own timers is a server name still
    /// resolving, whose candidates would land during a stop (gstreamer#5138).
    /// Before 2026-10-15 the 10 s bound was the stop itself; with it at 300
    /// ms, as here, the silent server's ~2 s gathering was stopped midway
    /// (aWebrtcbinStillGatheringAtTheBoundIsStoppedAnyway).
    void aWebrtcbinStillGatheringPastTheSoftBoundKeepsWaiting()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setRetireSoftBoundForTest(300);
        engine.setIceServers(stun.asStunServer(), QString(), QString());
        const QString callId = QStringLiteral("close-past-the-soft-bound");
        bool closed = false;
        connect(&engine, &CallMediaBackend::localCandidate, this,
                [&](const QString &id, const QString &, const QString &, int) {
                    if (id != callId || closed)
                        return;
                    closed = true;
                    engine.close(callId);
                });
        engine.createOffer(callId);
        QTRY_VERIFY_WITH_TIMEOUT(closed, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QVERIFY2(engine.retiresThatWaitedOnGatheringForTest() == 1,
                 "gathering had already ended at the close, so this run "
                 "proved nothing");
        QCOMPARE(engine.retiresPastTheSoftBoundForTest(), 1);
        QCOMPARE(engine.retireBoundExpiriesForTest(), 0);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
    }

    /// At quit a webrtcbin still gathering when the budget runs out is left
    /// for the process to end, not stopped mid-gathering (the crash).
    void aWebrtcbinStillGatheringAtQuitIsLeftRatherThanStopped()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setIceServers(stun.asStunServer(), QString(), QString());
        const QString callId = QStringLiteral("quit-while-gathering");
        bool closed = false;
        connect(&engine, &CallMediaBackend::localCandidate, this,
                [&](const QString &id, const QString &, const QString &, int) {
                    if (id != callId || closed)
                        return;
                    closed = true;
                    engine.close(callId);
                });
        engine.createOffer(callId);
        QTRY_VERIFY_WITH_TIMEOUT(closed, 15000);
        // libnice gives up on the silent server only after about 2 s.
        engine.drainRetiredForTest(100);
        if (engine.retiresLeftRunningAtQuitForTest() == 0
            && engine.teardownsWhileGatheringForTest() == 0)
            QSKIP("gathering had already ended before the budget ran out");
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(engine.retiresLeftRunningAtQuitForTest(), 1);
        QCOMPARE(engine.retiringWebrtcForTest(), 0);
    }

    /// 3c in the 1:1 lane: the capture's start blocked the GUI thread for as
    /// long as the sound server took. It is started on a pool thread now, and
    /// reports back to the call it was started for. Fails on the old backend,
    /// whose createOffer() returned only after the capture had started.
    void theOneToOneMicrophoneIsStartedOffTheGuiThread()
    {
        QVERIFY(registerSlowStartMic11());
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setMicrophoneFrontForTest(kSlowFront);
        const QString callId = QStringLiteral("slow-mic-call");
        QElapsedTimer took;
        took.start();
        engine.createOffer(callId);
        QVERIFY2(took.elapsed() < kSlowMicStartMs / 2,
                 qPrintable(QStringLiteral("createOffer() blocked %1 ms on "
                                           "the microphone's start")
                                .arg(took.elapsed())));
        QVERIFY(engine.microphoneFrontStartingForTest());
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneFrontStartsReportedForTest(),
                                  1, 3 * kSlowMicStartMs);
        QVERIFY(!engine.microphoneFrontStartingForTest());
        engine.close(callId);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
    }

    /// Hanging up on a call whose capture is RUNNING and whose sound server
    /// then stops answering: close() stopped the pipeline, capture included,
    /// on the GUI thread. The capture leaves first and stops off it now.
    /// Fails on the old backend: close() took the capture's ~2.5 s stop.
    void closingAOneToOneCallNeverWaitsOnTheCapturesStop()
    {
        QVERIFY(registerSlowStopMic11());
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setMicrophoneFrontForTest(QStringLiteral(
            "lightningslowstopmic11 name=micsrc ! audioconvert "
            "! audioresample"));
        const QString callId = QStringLiteral("slow-stop-hangup");
        engine.createOffer(callId);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneFrontStartsReportedForTest(),
                                  1, 5000);
        QElapsedTimer took;
        took.start();
        engine.close(callId);
        QVERIFY2(took.elapsed() < kSlowMicStartMs / 2,
                 qPrintable(QStringLiteral("close() waited %1 ms on the "
                                           "capture's stop")
                                .arg(took.elapsed())));
        QTest::qWait(kSlowMicStartMs + 500);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
    }

    /// Recheck L-h + M2 in the 1:1 lane: a speaker switch ran inside an IDLE
    /// probe, on the GUI thread whenever the pad was idle, so opening the
    /// new output on a silent sound server froze the window; and two picks
    /// in a row must not race. The first pick here is slow to open; the
    /// switch returns at once, the second pick wins, and one output is left.
    void twoOneToOneSpeakerPicksNeverBlockAndTheLastWins()
    {
        QVERIFY(registerSlowOpenSink11());
        const QString callId = QStringLiteral("speaker-race-1");
        GstCallMediaBackend caller;
        GstCallMediaBackend callee;
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);
        QString speaker = QStringLiteral("fakesink sync=false name=outsink");
        callee.setAudioDeviceResolver([&speaker] {
            GstCallMediaBackend::AudioDevicePlan plan;
            plan.speakerSink = speaker;
            return plan;
        });
        bool callerConnected = false;
        bool calleeConnected = false;
        QString failure;
        connect(&caller, &CallMediaBackend::offerReady, &callee,
                [&](const QString &id, const QString &sdp) {
                    callee.createAnswer(id, sdp);
                });
        connect(&callee, &CallMediaBackend::answerReady, &caller,
                [&](const QString &id, const QString &sdp) {
                    caller.setRemoteAnswer(id, sdp);
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
                [&](const QString &) { callerConnected = true; });
        connect(&callee, &CallMediaBackend::connected, this,
                [&](const QString &) { calleeConnected = true; });
        const auto noteFailure = [&](const QString &, const QString &why) {
            failure = why;
        };
        connect(&caller, &CallMediaBackend::failed, this, noteFailure);
        connect(&callee, &CallMediaBackend::failed, this, noteFailure);
        caller.createOffer(callId);
        QTRY_VERIFY2_WITH_TIMEOUT(
            (callerConnected && calleeConnected) || !failure.isEmpty(),
            qPrintable(QStringLiteral("no connection; failure=%1")
                           .arg(failure)),
            45000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_VERIFY_WITH_TIMEOUT(callee.receivedAudioPacketsForTest() > 50,
                                 15000);
        QCOMPARE(callee.receiveOutputsForTest(),
                 QStringList{ QStringLiteral("fakesink") });

        QElapsedTimer took;
        took.start();
        speaker = QStringLiteral("lightningslowopensink11 name=outsink");
        callee.audioDevicesChanged();
        speaker = QStringLiteral("fakesink sync=false async=false "
                                 "name=outsink");
        callee.audioDevicesChanged();
        QVERIFY2(took.elapsed() < kSlowMicStartMs / 2,
                 qPrintable(QStringLiteral("two speaker picks held the GUI "
                                           "thread %1 ms")
                                .arg(took.elapsed())));
        QTRY_COMPARE_WITH_TIMEOUT(callee.speakerSwapsForTest(), 1,
                                  3 * kSlowMicStartMs);
        QTest::qWait(500);
        QCOMPARE(callee.speakerSwapsForTest(), 1);
        QCOMPARE(callee.receiveOutputsForTest(),
                 QStringList{ QStringLiteral("fakesink") });
        // And it plays: decoded audio keeps reaching the (new) output.
        callee.resetReceivedPeakForTest();
        QTRY_VERIFY_WITH_TIMEOUT(callee.receivedPeakForTest() > 0.0, 10000);
        caller.close(callId);
        callee.close(callId);
    }

    /// Hanging up while that start is in flight: close() does not wait for
    /// it, and its late report is dropped (session serial and front).
    void closingAOneToOneCallWhileItsMicrophoneStartsDoesNotWait()
    {
        QVERIFY(registerSlowStartMic11());
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        engine.setMicrophoneFrontForTest(kSlowFront);
        const QString callId = QStringLiteral("slow-mic-hangup");
        engine.createOffer(callId);
        QVERIFY(engine.microphoneFrontStartingForTest());
        QElapsedTimer took;
        took.start();
        engine.close(callId);
        QVERIFY2(took.elapsed() < kSlowMicStartMs / 2,
                 qPrintable(QStringLiteral("close() waited %1 ms for the "
                                           "microphone's start")
                                .arg(took.elapsed())));
        // A new call while the old start is still running: not its report.
        engine.setMicrophoneFrontForTest(QString());
        engine.createOffer(QStringLiteral("next-call"));
        QTest::qWait(kSlowMicStartMs + 1000);
        QCOMPARE(engine.microphoneFrontStartsReportedForTest(), 1);
        engine.close(QStringLiteral("next-call"));
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
    }

    /// Only webrtcbin waits. Everything else of the call stops inside
    /// close(), so nothing is heard (or sent) after hanging up while the
    /// webrtcbin finishes gathering.
    void aClosedCallsMediaStopsAtOnceWhileWebrtcbinWaits()
    {
        SilentUdpPort stun;
        if (stun.port() == 0)
            QSKIP("no loopback UDP socket for the silent STUN server");
        const QString callId = QStringLiteral("media-stops-at-once");
        GstCallMediaBackend caller;
        GstCallMediaBackend callee;
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);
        // Only the callee gathers against the silent server, so it is still
        // gathering when it hangs up.
        callee.setIceServers(stun.asStunServer(), QString(), QString());
        QString failure;
        connect(&caller, &CallMediaBackend::offerReady, &callee,
                [&](const QString &id, const QString &sdp) {
                    callee.createAnswer(id, sdp);
                });
        connect(&callee, &CallMediaBackend::answerReady, &caller,
                [&](const QString &id, const QString &sdp) {
                    caller.setRemoteAnswer(id, sdp);
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
        const auto noteFailure = [&](const QString &, const QString &why) {
            failure = why;
        };
        connect(&caller, &CallMediaBackend::failed, this, noteFailure);
        connect(&callee, &CallMediaBackend::failed, this, noteFailure);

        caller.createOffer(callId);
        QTRY_VERIFY2_WITH_TIMEOUT(
            callee.receivedAudioPacketsForTest() > 10 || !failure.isEmpty(),
            qPrintable(QStringLiteral("no audio reached the callee; "
                                      "failure=%1").arg(failure)),
            30000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        callee.close(callId);
        const int atClose = callee.receivedAudioPacketsForTest();
        QTest::qWait(400);
        const bool webrtcbinStillWaiting = callee.retiringWebrtcForTest() == 1;
        QCOMPARE(callee.receivedAudioPacketsForTest(), atClose);
        if (!webrtcbinStillWaiting) {
            qWarning("the callee's gathering had ended before the check, so "
                     "the stop was not observed alongside a waiting webrtcbin "
                     "in this run");
        }
        caller.close(callId);
        QTRY_COMPARE_WITH_TIMEOUT(callee.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(caller.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(callee.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(caller.teardownsWhileGatheringForTest(), 0);
    }
};

QTEST_GUILESS_MAIN(CallMediaLoopbackTest)
#include "CallMediaLoopbackTest.moc"
