// The 1:1 call media engine: GStreamer webrtcbin (ICE via libnice, DTLS-SRTP,
// Opus, VP8).
//
// Video is VP8, and a build or an offer without it falls back to audio: the
// section is answered inactive and any receive pad this engine cannot decode
// is drained into a fakesink. Linking one into the Opus chain failed caps
// negotiation, and the not-negotiated return stopped the shared bundled
// transport: no audio either way.
//
// The send chain ends at webrtcbin from the start of a video call; the camera
// is a separate front (buildCameraFront()) linked to it only while it is on,
// so turning the camera off closes the device without renegotiating. The
// front also carries the self-view. A voice call that gains a camera adds the
// chain and renegotiates (m.call.negotiate).
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
class SfuVideoRouter;

typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
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
    /// Whether this GStreamer can carry 1:1 video: VP8 both ways. Audio calls
    /// do not depend on it, so it is not part of runtimeAvailable().
    static bool videoRuntimeAvailable();
    /// Test-only, test-tone mode: RTP that reached the VP8 receive chain, and
    /// the frames it decoded, in the current session.
    int receivedVideoPacketsForTest() const
    { return m_receivedVideoPackets.load(); }
    int decodedVideoFramesForTest() const { return m_decodedVideoFrames.load(); }
    /// Test-only: the camera front is in the pipeline.
    bool cameraFrontLiveForTest() const { return m_session.camFront != nullptr; }
    /// Test-only: frames the camera front produced for the encoder.
    int sentVideoFramesForTest() const { return m_sentVideoFrames->load(); }
    /// Test-only: the bound on an unanswered renegotiation.
    void setNegotiationTimeoutForTest(int ms) { m_negotiationTimeoutMs = ms; }
    /// Test-only: an unknown peer's receive chain fails, as a corrupt stream
    /// would make it.
    void failVideoReceiveForTest();
    /// Test-only: the camera fails, as an unplugged device would make it.
    void failCameraForTest();
    unsigned int microphoneRtpMtuForTest() const;
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
    /// Test-only: the soft bound (reported, not stopped) alone.
    void setRetireSoftBoundForTest(int ms)
    { m_retirer.setSoftBoundForTest(ms); }
    /// Test-only: retirements that outlived the soft bound and kept waiting.
    int retiresPastTheSoftBoundForTest() const
    { return m_retirer.outlivedSoftBoundForTest(); }
    /// Test-only: what the destructor does at quit, with this budget.
    void drainRetiredForTest(int budgetMs) { m_retirer.drain(budgetMs); }
    /// Test-only: webrtcbins left running at quit because still gathering.
    int retiresLeftRunningAtQuitForTest() const
    { return m_retirer.leftRunningAtQuitForTest(); }

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
    /// The camera a call opens, composed the way the MatrixRTC lane composes
    /// its capture. `front` is "<source named capsrc> ! ... ! video/x-raw...",
    /// no trailing separator; `refusal` non-empty opens nothing (a chosen
    /// camera that is gone is never replaced by the default).
    struct CameraPlan {
        QString front;
        lightning::calls::DeviceBinding binding;
        QString refusal;
        /// A sandbox: nothing can open until the Camera portal grants a
        /// PipeWire remote. The engine emits cameraAccessNeeded().
        bool needsPortal = false;
        /// When set, "@ENTRY@" in `front` is replaced by what this answers for
        /// the bound `probe` bin, which holds the source in READY: the camera's
        /// advertised formats pick the entry (MJPG or raw).
        QString probe;
        std::function<QString(GstElement *)> chooseEntry;
    };
    /// `portalFd`: the granted remote, or -1. Runs on this object's thread.
    using CameraResolver = std::function<CameraPlan(int portalFd)>;
    void setCameraResolver(CameraResolver resolver);
    /// The Camera portal's answer to cameraAccessNeeded(). Takes ownership of
    /// `fd`; -1 means no camera (cancelled or failed).
    void providePortalCamera(const QString &callId, int fd);
    /// The chosen microphone or speaker changed. A live call moves to the new
    /// devices now (the microphone is swapped in front of the mute valve, so
    /// mute, encoder, payloader and the negotiated track are untouched; each
    /// received track's sink is replaced, keeping its deafen state). Without
    /// a call nothing happens: the next call resolves at its start.
    void audioDevicesChanged();

    /// Test-only: microphone fronts and receive sinks replaced mid-call.
    int microphoneSwapsForTest() const { return m_micSwaps.load(); }
    int speakerSwapsForTest() const { return m_speakerSwaps->load(); }
    /// Test-only: the factory of every element named "outsink" in the live
    /// call's pipeline (one per received track when nothing is orphaned).
    QStringList receiveOutputsForTest() const;
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
    bool supportsVideo() const override;
    void prepareCall(const QString &callId, bool video, bool cameraOn) override;
    void setCameraEnabled(const QString &callId, bool on) override;
    void applyNegotiation(const QString &callId, const QString &type,
                          const QString &sdp) override;
    QString localStreamId() const override { return m_session.streamId; }
    void attachVideoSink(bool self, QObject *sink) override;
    void close(const QString &callId) override;
Q_SIGNALS:
    /// The camera needs the xdg Camera portal; answer with
    /// providePortalCamera().
    void cameraAccessNeeded(const QString &callId);

public:
    /// The two pictures; the appsink callbacks read it.
    SfuVideoRouter *videoRouter() const { return m_videoRouter.get(); }

private:
    struct Session {
        QString callId;
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        bool offerer = false;
        bool offerSent = false;
        bool answerSent = false;
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
        /// The capture's start runs on a pool thread (a sound server that
        /// does not answer blocks it, ~8.7 s measured on the GUI thread):
        /// true until that start has finished. Shared with the job.
        std::shared_ptr<std::atomic<bool>> frontStarting;
        /// Which session a start's report belongs to (m_sessionSerial).
        quint64 serial = 0;
        /// Which capture: a serial per front started (m_frontSerial), never
        /// its address, which a later front can reuse.
        quint64 frontSerial = 0;
        // Video. `camInput` is the send chain's first element, borrowed from
        // the pipeline, or null while the call has no video section.
        GstElement *camInput = nullptr;
        GstElement *camFront = nullptr;
        bool cameraWanted = false;
        // The portal's PipeWire remote, held while the camera is on.
        int cameraPortalFd = -1;
        // The msid both our tracks carry, the key of our stream metadata.
        QString streamId;
        // An offer of ours is on its way or out and unanswered.
        bool negotiating = false;
        // That offer's description exists (webrtcbin has it as local).
        bool localOfferSet = false;
        // The callee's offer is waiting out its delay.
        bool offerDelayed = false;
        // The video section was offered and the peer has not answered it.
        bool videoUnconfirmed = false;
        // Periodic RTP statistics; see requestStats().
        int statsReports = 0;
        quint64 lastInboundAudioPackets = 0;
        quint64 lastOutboundAudioPackets = 0;
    };

    bool startSession(const QString &callId, bool offerer,
                      int opusPayloadType, int vp8PayloadType);
    // The VP8 send chain, on a running or starting pipeline. False when it
    // would not build; the call carries on as audio.
    bool addVideoChainLocked(int payloadType);
    GstElement *buildCameraFront(CameraPlan plan);
    // Opens the camera now, or says why not.
    bool startCameraLocked();
    void stopCameraLocked();
    void closePortalFdLocked();
    void requestOfferLocked();
    void sendOfferLocked();
    void applyRemoteOfferLocked(const QString &sdp);
    void onNegotiationTimeout();
    // Tells webrtcbin a sink pad's caps. It otherwise learns them from the
    // first buffer: without one a section is left out of a description (port
    // 0), and a call muted, or with its camera off, has no first buffer.
    static void statePadCaps(GstPad *sink, const QString &caps);
    // Names the stream both tracks belong to on one of webrtcbin's sink pads.
    void setPadStreamId(GstElement *webrtc, const char *pad,
                        const QString &streamId);
    AudioDevicePlan resolveDevices() const;
    // Builds the capture bin for `plan` (ghost src pad, `micsrc` bound), or
    // null. Not added to anything.
    GstElement *buildMicrophoneFront(const AudioDevicePlan &plan) const;
    // Replaces the live capture with `plan`'s; false when the new one would
    // not start (the default is put back).
    bool swapMicrophoneLocked(const AudioDevicePlan &plan);
    /// Starts `front` (already in the pipeline and linked) on a pool thread;
    /// the answer comes back through noteMicrophoneFrontStarted().
    void startMicrophoneFrontAsync(GstElement *front, bool chosenDevice);
    /// A front whose start is still in flight leaves the pipeline at once and
    /// is stopped on a pool thread, so nothing on this thread ever waits on
    /// its state lock (teardown, a switch).
    void detachStartingFront();
public:
    /// The capture start's report, on this object's thread; acts only for
    /// the session and front it was started for. Public only for that
    /// callback.
    void noteMicrophoneFrontStarted(quint64 serial, quint64 frontSerial,
                                    bool ok, bool chosenDevice, qint64 tookMs);
    /// A remote picture reached the appsink. Any thread; public only for that
    /// callback.
    void noteRemoteVideoFrame(quintptr token);
    bool remoteFrameWanted(int padSerial);
    /// Test-only: the capture's start is still in flight.
    bool microphoneFrontStartingForTest() const
    {
        return m_session.frontStarting && m_session.frontStarting->load();
    }
    /// Test-only: capture starts reported to the session they belong to.
    int microphoneFrontStartsReportedForTest() const
    {
        return m_frontStartsReported;
    }
    /// Test-only, test-tone mode: the capture description used instead of
    /// the test sine (e.g. a source whose start blocks).
    void setMicrophoneFrontForTest(const QString &description)
    {
        m_testFront = description;
    }
private:
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
    Q_SLOT void handleCameraLost(quintptr token, const QString &category);
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
    std::atomic<int> m_receivedVideoPackets{0};
    std::atomic<int> m_decodedVideoFrames{0};
    // Shared with what can outlive this object by a moment: a stopped camera
    // front finishes on a pool thread, and its self-view keeps delivering.
    std::shared_ptr<std::atomic<int>> m_sentVideoFrames =
        std::make_shared<std::atomic<int>>(0);
    // Set when the receive chain failed: its packets go nowhere from then on,
    // so its error return cannot stop the transport the audio shares.
    std::atomic<bool> m_videoReceiveDead{false};
    QTimer *m_negotiationTimer = nullptr;
    QTimer *m_offerDelayTimer = nullptr;
    int m_calleeOfferDelayMs = 1500;
    int m_negotiationTimeoutMs = 60000; // a negotiate's lifetime
    // Set for the session's first offer and for each deliberate upgrade:
    // webrtcbin also asks for negotiation after our own answer, and those
    // must not become offers. Read on a GStreamer thread.
    std::atomic<bool> m_offerWanted{false};
    // The session has a video chain, so a video section and a VP8 track are
    // ours to take. Read on GStreamer threads.
    std::atomic<bool> m_acceptVideo{false};
    std::atomic<qint64> m_lastRemoteFrameMs{0};
    std::atomic<int> m_remoteVideoPads{0};
    std::atomic<qint64> m_lastNewestPadFrameMs{0};
    static constexpr qint64 kNewestPadQuietMs = 1500;
    bool m_remoteVideoReceiving = false;
    static constexpr qint64 kRemoteVideoStallMs = 4000;
    std::shared_ptr<SfuVideoRouter> m_videoRouter;
    CameraResolver m_resolveCamera;
    // prepareCall() until the session exists.
    bool m_preparedVideo = false;
    bool m_preparedCamera = false;
    QString m_preparedCallId;
    bool m_testTone = false;
    QString m_testFront;
    quint64 m_sessionSerial = 0;
    quint64 m_frontSerial = 0;
    int m_frontStartsReported = 0;
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
