// The 1:1 voice-call media engine: GStreamer webrtcbin (ICE via libnice,
// DTLS-SRTP, Opus), audio only.
//
// Audio only means a video offer (Element's and Nheko's video calls) is
// answered with its video section inactive, and any receive pad that is not
// Opus audio is drained into a fakesink. Linking it into the Opus chain
// failed caps negotiation, and the not-negotiated return stopped the shared
// bundled transport: no audio either way.
//
// Devices: the microphone and speaker are resolved per call through the same
// path as the MatrixRTC lane (setAudioDeviceResolver()), and a pick made
// during a call reaches it (audioDevicesChanged()). Until 2026-10-08 a pick
// was stored for the next call only, which a user reported as "it always uses
// my default microphone".
//
// Compiled only with the GStreamer WebRTC dev files (HAVE_LIGHTNING_WEBRTC)
// and registered only when runtimeAvailable() finds every required element,
// so a build without the plugins keeps CallController's honest refusal.
//
// Threading: callbacks arrive on GStreamer threads and marshal to this
// object's thread through a process-global alive registry; Qt-side handlers
// re-check the session, so a late callback for a closed call does nothing.
//
// Teardown: a closed call's pipeline stops at once, but its webrtcbin only
// once ICE gathering has ended (WebrtcRetirer.h, GitHub #3).
//
// Privacy: SDP and candidates carry host IPs and are never logged. ICE
// servers come only from the homeserver's /voip/turnServer; no third-party
// STUN fallback.
#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "CallMediaBackend.h"
#include "calls/CaptureDeviceSelection.h"
#include "calls/WebrtcRetirer.h"

#include <QMutex>
#include <QString>
#include <QStringList>

class QTimer;

typedef struct _GstElement GstElement;
typedef struct _GstPromise GstPromise;

namespace lightning::calls {
struct CaptureClockHold;
}

class GstCallMediaBackend : public CallMediaBackend
{
    Q_OBJECT

public:
    // One-time probe: GStreamer initialises and every required element
    // resolves. `whyNot` receives a short, safe reason.
    static bool runtimeAvailable(QString *whyNot = nullptr);

    /// Test-only: drives one create-offer change function through webrtcbin's
    /// error reply with no other reference on the promise, so the
    /// gst_promise_unref() inside it is the last one and frees the callback
    /// context mid-function. Returns the references left on the pinned
    /// element right after the reply: 1 means the context was destroyed
    /// synchronously; -1 when webrtcbin is unavailable.
    static int offerPromiseErrorReplyContextRefsForTest();

    explicit GstCallMediaBackend(QObject *parent = nullptr);
    ~GstCallMediaBackend() override;

    // Test mode: a quiet sine and a fakesink instead of real devices, so
    // headless CI can run a real loopback handshake. Set before the first
    // call.
    void setTestToneMode(bool on) { m_testTone = on; }
    /// Test-only, test-tone mode: RTP packets that reached the Opus receive
    /// chain in the current session.
    int receivedAudioPacketsForTest() const
    { return m_receivedAudioPackets.load(); }
    /// Test-only: receive pads drained because they were not Opus audio.
    int drainedReceivePadsForTest() const { return m_drainedPads.load(); }
    /// Test-only: closed calls' webrtcbins not yet at NULL (WebrtcRetirer.h).
    int retiringWebrtcForTest() const { return m_retirer.retiringForTest(); }
    /// Test-only: webrtcbins set to NULL while ICE gathering was still
    /// running, which is the window GitHub #3's abort needs.
    int teardownsWhileGatheringForTest() const
    { return m_retirer.whileGatheringForTest(); }
    /// Test-only: webrtcbins stopped because the bound ran out.
    int retireBoundExpiriesForTest() const
    { return m_retirer.boundExpiredForTest(); }
    /// Test-only: closes that found gathering running and waited for it.
    int retiresThatWaitedOnGatheringForTest() const
    { return m_retirer.waitedOnGatheringForTest(); }
    void setRetireBoundForTest(int ms) { m_retirer.setBoundForTest(ms); }

    /// "audio:sendrecv video:inactive", one entry per media section, with
    /// ":port0" for a rejected one. No addresses, ids or codecs: loggable.
    static QString sdpSectionShape(const QString &sdp);

    /// The microphone and speaker a call uses, resolved the way the MatrixRTC
    /// lane resolves them (SfuMediaEngine::resolveMicrophoneCapture() and
    /// resolveSpeakerSink(), so a device choice and the multi-input matrix
    /// behave the same in both lanes).
    struct AudioDevicePlan {
        /// "<source named micsrc> ... ! audio/x-raw,channels=1", no trailing
        /// separator (SfuMediaEngine::microphoneFrontDescription()). Empty
        /// means `autoaudiosrc`, which follows the system default.
        QString microphoneFront;
        /// Set on `micsrc` after the parse, never interpolated.
        lightning::calls::DeviceBinding microphoneBinding;
        /// One sink element named `outsink`, e.g. "pulsesink name=outsink".
        /// Empty means `autoaudiosink`.
        QString speakerSink;
        /// Set on `outsink` after the parse.
        lightning::calls::DeviceBinding speakerBinding;
    };
    /// Asked at the start of every call and again on audioDevicesChanged().
    /// It may block (GStreamer's device monitor, bounded); it runs on this
    /// object's thread. Without a resolver the system defaults are used. In
    /// test-tone mode a resolver's non-empty entries replace the tone and the
    /// fakesink, so a test can switch between fake devices.
    using AudioDeviceResolver = std::function<AudioDevicePlan()>;
    void setAudioDeviceResolver(AudioDeviceResolver resolver);
    /// The chosen microphone or speaker changed. A live call moves to the new
    /// devices now (the microphone is swapped in front of the mute valve, so
    /// mute, encoder, payloader and the negotiated track are untouched; each
    /// received track's sink is replaced, keeping its deafen state). Without
    /// a call nothing happens: the next call resolves at its start.
    void audioDevicesChanged();

    /// Test-only: microphone fronts and receive sinks replaced mid-call.
    int microphoneSwapsForTest() const { return m_micSwaps.load(); }
    int speakerSwapsForTest() const { return m_speakerSwaps->load(); }
    /// Test-only, test-tone mode: the loudest decoded sample (0..1) the
    /// receive chain produced since the last reset.
    double receivedPeakForTest() const
    { return m_receivedPeakMilli.load() / 1000.0; }
    void resetReceivedPeakForTest() { m_receivedPeakMilli.store(0); }

    void createOffer(const QString &callId) override;
    void createAnswer(const QString &callId,
                      const QString &remoteOfferSdp) override;
    void setRemoteAnswer(const QString &callId,
                         const QString &remoteAnswerSdp) override;
    void addRemoteCandidate(const QString &callId, const QString &candidate,
                            const QString &sdpMid, int sdpMLineIndex) override;
    void setIceServers(const QStringList &uris, const QString &username,
                       const QString &password) override;
    void setMicrophoneMuted(const QString &callId, bool muted) override;
    void setOutputMuted(const QString &callId, bool muted) override;
    // A real valve/volume pair exists, so mute is genuinely supported.
    bool supportsMuteControl() const override { return true; }
    void close(const QString &callId) override;

private:
    struct Session {
        QString callId;
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        bool offerer = false;
        bool offerSent = false;
        bool remoteDescriptionSet = false;
        QList<QPair<int, QString>> pendingRemoteCandidates;
        // Send-side valve: drop=true stops buffers before the encoder, so no
        // RTP is produced (a real mute).
        GstElement *micValve = nullptr;
        // Desired states, kept so a receive bin created after deafening comes
        // up already silenced.
        bool micMuted = false;
        bool outputMuted = false;
        // The capture in front of the valve (a bin named "micfront"), and the
        // clock hold its replacement re-attaches to (CaptureClock.h).
        GstElement *micFront = nullptr;
        std::shared_ptr<lightning::calls::CaptureClockHold> clockHold;
        // Periodic RTP statistics; see requestStats().
        int statsReports = 0;
        quint64 lastInboundAudioPackets = 0;
        quint64 lastOutboundAudioPackets = 0;
    };

    bool startSession(const QString &callId, bool offerer,
                      int opusPayloadType);
    AudioDevicePlan resolveDevices() const;
    // Builds the capture bin for `plan` (ghost src pad, `micsrc` bound), or
    // null. Not added to anything.
    GstElement *buildMicrophoneFront(const AudioDevicePlan &plan) const;
    // Replaces the live capture with `plan`'s; false when the new one would
    // not start (the default is put back).
    bool swapMicrophoneLocked(const AudioDevicePlan &plan);
    void swapSpeakersLocked(const AudioDevicePlan &plan);
    // The receive sink description for `plan`, or the test fakesink.
    QString receiveSinkDescription(const AudioDevicePlan &plan) const;
    void requestStats();
    void handleStats(quintptr token, quint64 inboundAudio,
                            quint64 inboundAudioBytes, qint64 inboundLost,
                            quint64 outboundAudio, quint64 inboundOther,
                            int iceState);
    void destroySessionLocked();
    void applyIceConfigLocked();
    void flushPendingCandidatesLocked();

    // Qt-thread handlers the GStreamer callbacks marshal into (slots, so the
    // bus handler can queue by name).
    //
    // The engine is reused across calls, so each handler carries a `token`:
    // the pointer of the element that emitted the event (webrtcbin, or the
    // pipeline for bus errors), allocated fresh per session. Handlers act only
    // when it matches the live session; promise paths hold a ref so the
    // address cannot be recycled meanwhile.
    Q_SLOT void handleLocalDescription(quintptr token, bool offer,
                                       const QString &sdp);
    Q_SLOT void handleRemoteDescriptionApplied(quintptr token);
    Q_SLOT void handleIceCandidate(quintptr token, int mlineIndex,
                                   const QString &candidate);
    Q_SLOT void handleGatheringComplete(quintptr token);
    Q_SLOT void handleConnectionState(quintptr token, int state);
    Q_SLOT void handleFailure(quintptr token, const QString &category);
    bool tokenMatchesLiveSession(quintptr token) const;

    // GStreamer C callbacks (user_data = backend).
    static void onNegotiationNeeded(GstElement *webrtc, void *userData);
    static void onOfferCreated(GstPromise *promise, void *userData);
    static void onAnswerCreated(GstPromise *promise, void *userData);
    static void onRemoteOfferSet(GstPromise *promise, void *userData);
    static void onRemoteAnswerSet(GstPromise *promise, void *userData);
    static void onIceCandidateGst(GstElement *webrtc, unsigned mlineIndex,
                                  char *candidate, void *userData);
    static void onIceGatheringNotify(GstElement *webrtc, void *pspec,
                                     void *userData);
    static void onConnectionNotify(GstElement *webrtc, void *pspec,
                                   void *userData);
    static void onPadAdded(GstElement *webrtc, void *pad, void *userData);
    // The bus sync handler is file-local in the .cpp, keeping GStreamer types
    // out of this header.

    Session m_session; // exactly one live call, matching CallController
    bool m_sessionActive = false;
    // Read on a GStreamer thread in onPadAdded, written on the Qt thread;
    // atomic because pad-added cannot marshal without the track going audible
    // first.
    std::atomic<bool> m_outputMuted{false};
    // Test counters, written on GStreamer threads.
    std::atomic<int> m_receivedAudioPackets{0};
    std::atomic<int> m_drainedPads{0};
    std::atomic<int> m_receivedPeakMilli{0};
    std::atomic<int> m_micSwaps{0};
    // Shared with pending swap probes, which can outlive a call.
    std::shared_ptr<std::atomic<int>> m_speakerSwaps =
        std::make_shared<std::atomic<int>>(0);
    // Decoded receive buffers, for the stats line: shows whether RTP that
    // arrived also reached the decoder (written on a streaming thread).
    std::atomic<quint64> m_decodedBuffers{0};
    bool m_testTone = false;
    AudioDeviceResolver m_resolveDevices;
    // The plan the live call uses. Read by onPadAdded on a GStreamer thread,
    // so guarded; written on this object's thread.
    mutable QMutex m_planMutex;
    AudioDevicePlan m_plan;
    QTimer *m_statsTimer = nullptr;
    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
    /// Closed calls' webrtcbins, stopped once their ICE gathering has ended.
    lightning::webrtc::Retirer m_retirer;
};
