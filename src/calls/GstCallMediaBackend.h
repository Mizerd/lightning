#pragma once

#include "app/AllocatorPolicy.h"
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
    static bool runtimeAvailable(QString *whyNot = nullptr);

    static int offerPromiseErrorReplyContextRefsForTest();

    explicit GstCallMediaBackend(QObject *parent = nullptr);
    ~GstCallMediaBackend() override;

    void setTestToneMode(bool on) { m_testTone = on; }
    static bool videoRuntimeAvailable();
    int receivedVideoPacketsForTest() const
    { return m_receivedVideoPackets.load(); }
    int decodedVideoFramesForTest() const { return m_decodedVideoFrames.load(); }
    bool cameraFrontLiveForTest() const { return m_session.camFront != nullptr; }
    int sentVideoFramesForTest() const { return m_sentVideoFrames->load(); }
    void setNegotiationTimeoutForTest(int ms) { m_negotiationTimeoutMs = ms; }
    void failVideoReceiveForTest();
    void failCameraForTest();
    unsigned int microphoneRtpMtuForTest() const;
    int receivedAudioPacketsForTest() const
    { return m_receivedAudioPackets.load(); }
    int drainedReceivePadsForTest() const { return m_drainedPads.load(); }
    int retiringWebrtcForTest() const { return m_retirer.retiringForTest(); }
    int teardownsWhileGatheringForTest() const
    { return m_retirer.whileGatheringForTest(); }
    int retireBoundExpiriesForTest() const
    { return m_retirer.boundExpiredForTest(); }
    int retiresThatWaitedOnGatheringForTest() const
    { return m_retirer.waitedOnGatheringForTest(); }
    void setRetireBoundForTest(int ms) { m_retirer.setBoundForTest(ms); }
    void setRetireSoftBoundForTest(int ms)
    { m_retirer.setSoftBoundForTest(ms); }
    int retiresPastTheSoftBoundForTest() const
    { return m_retirer.outlivedSoftBoundForTest(); }
    void drainRetiredForTest(int budgetMs) { m_retirer.drain(budgetMs); }
    int retiresLeftRunningAtQuitForTest() const
    { return m_retirer.leftRunningAtQuitForTest(); }

    static QString sdpSectionShape(const QString &sdp);

    struct AudioDevicePlan {
        QString microphoneFront;
        /// Set on `micsrc` after the parse, never interpolated.
        lightning::calls::DeviceBinding microphoneBinding;
        QString speakerSink;
        lightning::calls::DeviceBinding speakerBinding;
    };
    /// Asked at the start of every call and again on audioDevicesChanged().
    /// It may block (GStreamer's device monitor, bounded); it runs on this
    /// object's thread. Without a resolver the system defaults are used. In
    /// test-tone mode a resolver's non-empty entries replace the tone and the
    /// fakesink, so a test can switch between fake devices.
    using AudioDeviceResolver = std::function<AudioDevicePlan()>;
    void setAudioDeviceResolver(AudioDeviceResolver resolver);
    struct CameraPlan {
        QString front;
        lightning::calls::DeviceBinding binding;
        QString refusal;
        bool needsPortal = false;
        QString probe;
        std::function<QString(GstElement *)> chooseEntry;
    };
    using CameraResolver = std::function<CameraPlan(int portalFd)>;
    void setCameraResolver(CameraResolver resolver);
    void providePortalCamera(const QString &callId, int fd);
    /// The chosen microphone or speaker changed. A live call moves to the new
    /// devices now (the microphone is swapped in front of the mute valve, so
    /// mute, encoder, payloader and the negotiated track are untouched; each
    /// received track's sink is replaced, keeping its deafen state). Without
    /// a call nothing happens: the next call resolves at its start.
    void audioDevicesChanged();

    int microphoneSwapsForTest() const { return m_micSwaps.load(); }
    int speakerSwapsForTest() const { return m_speakerSwaps->load(); }
    QStringList receiveOutputsForTest() const;
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
    void cameraAccessNeeded(const QString &callId);

public:
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
        bool micMuted = false;
        bool outputMuted = false;
        GstElement *micFront = nullptr;
        std::shared_ptr<lightning::calls::CaptureClockHold> clockHold;
        /// The capture's start runs on a pool thread (a sound server that
        /// does not answer blocks it, ~8.7 s measured on the GUI thread):
        /// true until that start has finished. Shared with the job.
        std::shared_ptr<std::atomic<bool>> frontStarting;
        quint64 serial = 0;
        quint64 frontSerial = 0;
        GstElement *camInput = nullptr;
        GstElement *camFront = nullptr;
        bool cameraWanted = false;
        int cameraPortalFd = -1;
        QString streamId;
        bool negotiating = false;
        bool localOfferSet = false;
        bool offerDelayed = false;
        bool videoUnconfirmed = false;
        int statsReports = 0;
        quint64 lastInboundAudioPackets = 0;
        quint64 lastOutboundAudioPackets = 0;
    };

    bool startSession(const QString &callId, bool offerer,
                      int opusPayloadType, int vp8PayloadType);
    bool addVideoChainLocked(int payloadType);
    GstElement *buildCameraFront(CameraPlan plan);
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
    void setPadStreamId(GstElement *webrtc, const char *pad,
                        const QString &streamId);
    AudioDevicePlan resolveDevices() const;
    GstElement *buildMicrophoneFront(const AudioDevicePlan &plan) const;
    bool swapMicrophoneLocked(const AudioDevicePlan &plan);
    void startMicrophoneFrontAsync(GstElement *front, bool chosenDevice);
    /// A front whose start is still in flight leaves the pipeline at once and
    /// is stopped on a pool thread, so nothing on this thread ever waits on
    /// its state lock (teardown, a switch).
    void detachStartingFront();
public:
    void noteMicrophoneFrontStarted(quint64 serial, quint64 frontSerial,
                                    bool ok, bool chosenDevice, qint64 tookMs);
    void noteRemoteVideoFrame(quintptr token);
    bool remoteFrameWanted(int padSerial);
    bool microphoneFrontStartingForTest() const
    {
        return m_session.frontStarting && m_session.frontStarting->load();
    }
    int microphoneFrontStartsReportedForTest() const
    {
        return m_frontStartsReported;
    }
    void setMicrophoneFrontForTest(const QString &description)
    {
        m_testFront = description;
    }
private:
    void swapSpeakersLocked(const AudioDevicePlan &plan);
    QString receiveSinkDescription(const AudioDevicePlan &plan) const;
    void requestStats();
    void handleStats(quintptr token, quint64 inboundAudio,
                            quint64 inboundAudioBytes, qint64 inboundLost,
                            quint64 outboundAudio, quint64 inboundOther,
                            int iceState);
    void destroySessionLocked();
    void applyIceConfigLocked();
    void flushPendingCandidatesLocked();

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

    Session m_session;
    bool m_sessionActive = false;
    lightning::CallAllocatorHold m_callAllocator;
    // Read on a GStreamer thread in onPadAdded, written on the Qt thread;
    // atomic because pad-added cannot marshal without the track going audible
    // first.
    std::atomic<bool> m_outputMuted{false};
    std::atomic<int> m_receivedAudioPackets{0};
    std::atomic<int> m_drainedPads{0};
    std::atomic<int> m_receivedPeakMilli{0};
    std::atomic<int> m_micSwaps{0};
    std::shared_ptr<std::atomic<int>> m_speakerSwaps =
        std::make_shared<std::atomic<int>>(0);
    std::atomic<quint64> m_decodedBuffers{0};
    std::atomic<int> m_receivedVideoPackets{0};
    std::atomic<int> m_decodedVideoFrames{0};
    std::shared_ptr<std::atomic<int>> m_sentVideoFrames =
        std::make_shared<std::atomic<int>>(0);
    std::atomic<bool> m_videoReceiveDead{false};
    QTimer *m_negotiationTimer = nullptr;
    QTimer *m_offerDelayTimer = nullptr;
    int m_calleeOfferDelayMs = 1500;
    int m_negotiationTimeoutMs = 60000;
    std::atomic<bool> m_offerWanted{false};
    std::atomic<bool> m_acceptVideo{false};
    std::atomic<qint64> m_lastRemoteFrameMs{0};
    std::atomic<int> m_remoteVideoPads{0};
    std::atomic<qint64> m_lastNewestPadFrameMs{0};
    static constexpr qint64 kNewestPadQuietMs = 1500;
    bool m_remoteVideoReceiving = false;
    static constexpr qint64 kRemoteVideoStallMs = 4000;
    std::shared_ptr<SfuVideoRouter> m_videoRouter;
    CameraResolver m_resolveCamera;
    bool m_preparedVideo = false;
    bool m_preparedCamera = false;
    QString m_preparedCallId;
    bool m_testTone = false;
    QString m_testFront;
    quint64 m_sessionSerial = 0;
    quint64 m_frontSerial = 0;
    int m_frontStartsReported = 0;
    AudioDeviceResolver m_resolveDevices;
    mutable QMutex m_planMutex;
    AudioDevicePlan m_plan;
    QTimer *m_statsTimer = nullptr;
    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
    lightning::webrtc::Retirer m_retirer;
};
