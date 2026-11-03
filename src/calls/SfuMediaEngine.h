#pragma once

#include "app/AllocatorPolicy.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <mutex>

#include <QHash>
#include <QSet>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>

#include "calls/StreamInfoStats.h"
#include "calls/CaptureDeviceSelection.h"
#include "calls/ShareAudioSources.h"
#include "calls/ShareCaptureRetry.h"
#include "calls/noise/NoiseSuppressor.h"
#include "calls/WebrtcRetirer.h"

typedef struct _GstElement GstElement;
typedef struct _GstObject GstObject;
typedef struct _GstStructure GstStructure;
typedef struct _GstPromise GstPromise;
typedef struct _GstPad GstPad;
typedef struct _GstBuffer GstBuffer;
typedef struct _GstCaps GstCaps;

class CallFrameCryptor;
class SfuVideoRouter;
namespace lightning::calls {
struct CaptureClockHold;
}

class SfuMediaEngine : public QObject
{
    Q_OBJECT

public:
    enum class Target { Publisher, Subscriber };

    static constexpr int kScreenWidth = 1920;
    static constexpr int kScreenHeight = 1080;

    void setShareQuality(int maxHeight, int fps, int bitrateCapKbps = 0);
    bool applyShareQualityLive(const QString &cid);
    void setCameraQuality(int maxHeight);
    int shareBitrateCapKbps() const { return m_shareBitrateCapKbps; }
    int cameraMaxHeight() const { return m_cameraMaxHeight; }
    static int snapCameraHeight(int height);
    static int snapShareBitrateCap(int kbps);
    static QString cameraLimitsCaps(bool portal, int maxHeight = kCameraHeight);
    static QString cameraEncoderStage(int maxHeight);

    struct DeviceChoice {
        QString id;
        QString description;
        /// Camera only: a camera was chosen and is not usable now. Opens no
        /// camera rather than the default (CaptureDeviceSelection.h).
        bool preferredMissing = false;
    };
    void setPreferredDevices(const DeviceChoice &camera,
                             const DeviceChoice &microphone,
                             const DeviceChoice &speaker);
    DeviceChoice cameraChoice() const;
    DeviceChoice microphoneChoice() const;
    DeviceChoice speakerChoice() const;

    static QString shareLimitsCaps(int maxHeight, int fps,
                                   bool glMemory = false);
    static QString applyDevicePropertyForTest(const QString &factory,
                                              const QString &property,
                                              const QString &value);
    static QString shareScaleStage(int maxHeight, bool gpu,
                                   bool keepOnGpu = false);
    struct VideoCodecChain {
        QString beforeEncoder;
        QString afterEncoder;
        QString payloader;
        QString rtpCaps;
    };
    struct ShareEncoderCandidate {
        const char *factory = nullptr;
        const char *stage = nullptr;
        const char *profile = nullptr;
        bool glInput = false;
    };
    struct ShareHardwareEncoder {
        ShareEncoderCandidate candidate;
        bool zeroCopy = false;
    };
    static QList<ShareEncoderCandidate> shareEncoderCandidates();
    /// Whether a share should try hardware. Off unless
    /// `LIGHTNING_SHARE_ENCODER=hardware`: a viewer without an H.264 receive
    /// chain, which is every release so far, would see nothing.
    static bool shareHardwareEncodeRequested();
    static std::optional<ShareHardwareEncoder>
    firstWorkingShareEncoder(const QList<ShareEncoderCandidate> &candidates,
                             bool glUsable);
    /// Starts the once-per-process probe on its own thread: it can take
    /// seconds, and a driver can hang it, so it must not run on the GUI thread.
    static void probeShareHardwareEncoder();
    static std::optional<ShareHardwareEncoder> shareHardwareEncoder();
    static QString shareEncoderStage(const ShareEncoderCandidate &candidate,
                                     int maxHeight, int fps, int bitrateCapKbps);
    static VideoCodecChain h264CodecChain(const ShareEncoderCandidate &candidate,
                                          bool zeroCopy);
    static QString h264DecoderElement();
    QString shareEncoderInUse() const { return m_shareEncoderInUse; }
    static QString captureEntryFilter(bool gpu);
    /// Caps the GPU entry falls back to when its DMA-BUF branch cannot
    /// negotiate with the compositor: system memory, any format.
    static QString systemMemoryEntryCaps();
    static QString gpuEntryCaps();
    /// The GPU path's named entry, starting in system memory.
    static QString gpuSystemMemoryFirstEntry();
    static bool entryOffersDmaBuf(const GstCaps *caps);
    /// pipewiresrc < 1.5.81 forgets its caps after one Format; a re-announce fails.
    static bool pipewireSrcKeepsFormatOnRenegotiation(const QString &version);
    static QString pipewirePluginVersion();
    /// A negotiation or format failure of the capture source. basesrc's
    /// generic STREAM/FAILED counts only when its debug names not-negotiated.
    static bool isCaptureFormatError(quint32 domain, int code,
                                     const QString &message,
                                     const QString &debug);
    /// Stops the share's capture, swaps its entry to the other memory and
    /// starts it again, under the bin's state lock. False, touching nothing,
    /// when the bin is no longer a live member of a playing pipeline.
    static bool restartShareCapture(GstElement *bin, ShareCaptureRetry &retry);
    /// Set once a share's DMA-BUF capture fails to negotiate; every later
    /// share then starts in system memory.
    static bool shareDmaBufCaptureFailedForTest();
    static void clearShareDmaBufCaptureFailedForTest();
    static void setPipewirePluginVersionForTest(const QString &version);
    /// A screen capture's DMA-BUF buffer whose single memory is SHORTER than
    /// the frame its video meta describes, re-wrapped at the file
    /// descriptor's real size; nullptr when the buffer needs nothing (not a
    /// DMA-BUF, already full size, or no way to size it). niri sets
    /// `chunk.size = 1` and pipewiresrc wraps exactly that, after which
    /// glupload copies a whole frame out of a 1-byte mapping and crashes the
    /// GL thread (issue #56). The returned buffer holds the original through
    /// a parent-buffer meta so the PipeWire buffer is not recycled under it.
    /// Linux with gstreamer-allocators only; elsewhere always nullptr.
    static GstBuffer *rewrapShortDmaBuf(GstBuffer *buffer);
    /// Camera entry for a source offering MJPG, decoded explicitly. Raw YUY2
    /// at 720p30 exceeds USB 2.0 and negotiates down to ~10 fps, and
    /// captureEntryFilter()'s `video/x-raw` filter rules MJPG out.
    ///
    /// `jpegdec`, not `decodebin`: decodebin's pads appear only once data
    /// flows, so linking fails ("Delayed linking failed").
    static QString cameraJpegEntry(const GstCaps *sourceCaps = nullptr);
    static constexpr int kCameraSourceMaxWidth = 1920;
    static constexpr int kCameraSourceMaxHeight = 1080;
    static QString directCameraRawEntry(const GstCaps *sourceCaps = nullptr);
    static GstCaps *preferredCameraCaps(const GstCaps *advertised,
                                         bool jpegAvailable);
    static QString cameraIoMode(quint32 capabilities);
    static bool isDirectCameraSource(GstObject *source);
    static QString cameraPublishErrorCategory(bool screenShare,
                                              bool busyError);
    static bool shareGpuScalingRequested();
    static QString missingGpuShareElement();
    /// PTS for the next keep-alive frame.
    ///
    /// Anchored to the sampled buffer's own PTS plus elapsed wall time, never
    /// the pipeline running time: WindowCaptureSrc and ximagesrc stamp
    /// zero-based PTS, and a running-time PTS would make videorate back-fill
    /// duplicates for the whole call age and then drop every real frame.
    /// Strictly increasing. Returns GST_CLOCK_TIME_NONE when the source gave
    /// no usable PTS.
    static quint64 keepAlivePts(quint64 sampledPts, bool sampledPtsValid,
                                qint64 elapsedMs, quint64 lastInjectedPts,
                                bool lastInjectedPtsValid);
    static GstPad *keepAliveInjectionPad(GstElement *bin);
    static QString firstMissingElement(const QList<QByteArray> &names);
    static QString cpuFallbackScaleStage(bool screenShare, int shareMaxHeight);
    static bool gpuShareChainUsable();
    static bool jpegCameraChainAvailable();
    static bool capsOfferJpeg(const GstCaps *caps,
                              const GstCaps *selectedEntryCaps = nullptr);
    static QString captureCameraEntry(GstElement *bin, bool jpegAvailable,
                                      QString *ioMode);
    static QString shareEncoderStage(int maxHeight, int fps,
                                     int bitrateCapKbps = 0);
    int shareMaxHeight() const { return m_shareMaxHeight; }
    int shareFps() const { return m_shareFps; }
    static constexpr int kCameraWidth = 1280;
    static constexpr int kCameraHeight = 720;

    static bool runtimeAvailable(QString *whyNot = nullptr);

    struct MicrophoneCapture {
        QString source;
        lightning::calls::DeviceBinding binding;
        int channels = 0;
    };
    static MicrophoneCapture resolveMicrophoneCapture(const DeviceChoice &mic);
    static QString microphoneFrontDescription(const MicrophoneCapture &capture);
    static bool levelElementAvailable();
    static bool echoProbeElementAvailable();
    static bool applyCaptureBinding(GstElement *bin, const char *elementName,
                                    const lightning::calls::DeviceBinding &binding);
    struct OutputSink {
        QString sink;
        lightning::calls::DeviceBinding binding;
    };
    static OutputSink resolveSpeakerSink(const DeviceChoice &speaker);
    static bool readLevelPeak(const GstStructure *fields, double *peakDb);
    static constexpr quint64 kMicLevelIntervalNs = 50000000;
    static constexpr quint64 kMicCaptureLevelIntervalNs = 200000000;

    explicit SfuMediaEngine(QObject *parent = nullptr);
    ~SfuMediaEngine() override;

    void setTestSourceMode(bool on) { m_testSources = on; }
    bool testSourceMode() const { return m_testSources; }

    /// Start a session. Tears down any previous one; the generation bump
    /// invalidates in-flight callbacks. Media keys are kept: they belong to
    /// the call, and a peer's key can arrive while we are still joining
    /// (join() has already cleared the previous call's).
    void start();
    void stop();
    void suspend();
    bool active() const { return m_active; }
    QVariantList streamInfoStats();
    void resetStreamInfoBaselines();

    void setVideoRouter(SfuVideoRouter *router);
    void setReceiveNamespace(const QString &prefix)
    { m_receiveNamespace = prefix; }
    SfuVideoRouter *videoRouter() const;

    void setIceServers(const QVariantList &servers);
    /// A STUN entry in the form webrtcbin's `stun-server` takes
    /// (`stun://host[:port]`), or empty when `iceUri` is not `stun:`.
    /// gstwebrtcnice parses the value as a URI and drops one with no host,
    /// which the ICE form `stun:host:port` is.
    static QString stunServerUri(const QString &iceUri);
    /// A TURN entry in the form `add-turn-server` takes, credentials
    /// percent-encoded and `?transport=` kept, or empty when `iceUri` is not
    /// `turn:`/`turns:`. Contains the credentials: never log it.
    static QString turnServerUri(const QString &iceUri, const QString &username,
                                 const QString &password);

    void publishAudio(const QString &cid);

    void publishShareAudio(const QString &cid,
                           const lightning::shareaudio::Selection &selection =
                               lightning::shareaudio::Selection{});
    bool applyShareAudioSelection(
        const lightning::shareaudio::Selection &selection);

    static bool shareAudioAvailable();
    static bool shareAudioSystemExcludesUs();
    static QStringList ownAudioClientNames();
    void publishVideo(const QString &cid, bool screenShare, int nodeId,
                      int pipewireFd = -1, quint64 windowHandle = 0,
                      const QRect &captureRect = {});
    static int runQueueSelfTest(QString *report);

    struct BlockedRunPolicy {
        static constexpr int kWindow = 100;
        static constexpr int kRaisePercent = 90;
        static constexpr int kClearPercent = 25;
        static constexpr int kMinObserved = 50;

        std::array<bool, kWindow> recent{};
        int next = 0;
        int observed = 0;
        int failures = 0;
        bool announced = false;

        bool note(bool failed, bool *raise);
        int failurePercent() const;
    };

    static QString videoPipelineDescription(const QString &source,
                                            const QString &rateStage,
                                            const QString &limits,
                                            const QString &encoder,
                                            const QString &selfView,
                                            quint32 ssrc,
                                            const QString &scaleStage,
                                            const QString &entryFilter,
                                            const VideoCodecChain &codec = {});
    static QString selfViewBranch(bool glMemory = false);
    /// The rate stage: `videorate` pinning the output to a fixed rate. A
    /// desktop capture delivers on damage and videorate needs a second input
    /// buffer to emit anything; see the definition.
    static QString videoRateStage(bool screenShare);
    /// Camera entry for the xdg Camera portal route (`pipewiresrc fd=`).
    ///
    /// A fixed downstream framerate propagates up to pipewiresrc and PipeWire
    /// refuses modes the camera lacks ("error set output format: -22"). A caps
    /// list does not help: the element does not move past an unsatisfiable
    /// first alternative. So: one raw structure with a size range, the rate
    /// capped by `videorate max-rate` rather than pinned. The direct
    /// `v4l2src` route is unchanged.
    static QString portalCameraEntry();
    static QString cameraRateStage(bool portal);
    static QString outputVolumeElementName(const QString &streamId);
    static GstElement *buildReceiveBin(bool video,
                                       const QString &volumeElementName,
                                       bool testSink, QString *error,
                                       const QString &pulseClientName = QString(),
                                       const OutputSink *speaker = nullptr,
                                       bool echoProbe = false,
                                       bool h264 = false);
    void refreshSpeakerSink(bool applyLive = false);
    void refreshMicrophoneCapture();
    OutputSink cachedSpeakerSink() const;
    static QString receiveSinkDescription(const QString &pulseClientName,
                                          const OutputSink *speaker);
    static QString volumeKeyFor(const QString &streamId,
                                const QString &trackKey);
    void setTrackVolume(const QString &streamId, const QString &trackKey,
                        int percent);

    static int audioFactorPercent(int userPercent);
    static quint32 nextPublishSsrc();
    static void applyPublisherMsid(GstPad *sinkPad, const QString &cid);
    static QString trackSidFromMsid(const QString &msid);
    static QString participantIdFromMsid(const QString &msid);
    static QString localCameraStreamId();
    static QString localScreenStreamId();
    static QString screenShareSource(int nodeId, int pipewireFd,
                                     quint64 windowHandle = 0,
                                     const QRect &captureRect = {});
    /// The element the Linux no-portal fallback captures with, named once so
    /// the probe, the description and the tests agree.
    ///
    /// Inline on purpose: SfuCallController uses it in builds that do not
    /// compile SfuMediaEngine.cpp.
    static constexpr const char *x11ScreenCaptureElementName()
    {
        return "ximagesrc";
    }
    static bool elementAvailable(const char *name);
    static QString cameraSource(int pipewireFd = -1);
    struct CameraBinding {
        lightning::calls::DeviceBinding binding;
        QString refusal;
    };
    static CameraBinding resolveCameraBinding(const QString &source,
                                              const DeviceChoice &camera);
    void unpublish(const QString &cid);

    /// `captureEpoch` -1 is the current attempt.
    void handleCaptureEnded(const QString &cid, int captureEpoch = -1);

    static constexpr int kCameraFirstFrameTimeoutMs = 8000;
    static constexpr int kPortalCameraFirstFrameTimeoutMs = 15000;
    void setCameraFirstFrameTimeoutForTest(int ms)
    {
        m_cameraFirstFrameTimeoutMs = ms;
        m_portalCameraFirstFrameTimeoutMs = ms;
    }

    /// Lets a probe or sink callback outlive the engine safely. A portal
    /// camera's bin can still be starting when the engine tears down (its
    /// pipewiresrc may block for 30 s), so it is detached and finished on a
    /// pool thread, out of reach of the bounded teardown wait; its callbacks
    /// that touch the engine hold `mutex` while they do and do nothing once
    /// `open` is false. Closing takes the mutex, so it waits for at most one
    /// frame already inside.
    struct ProbeGate {
        std::mutex mutex;
        bool open = true;
    };

    int publisherTrackSlotsForTest() const;
    double receiveVolumeForTest(const QString &streamId) const;
    int receiveMutedForTest(const QString &streamId) const;

    bool hasPublishedBinForTest(const QString &cid) const
    {
        return m_publishedBins.contains(cid);
    }

    GstElement *publishedBinForTest(const QString &cid) const
    {
        return m_publishedBins.value(cid, nullptr);
    }

    /// Called by the bus sync handler, any thread: a receive bin posted an error. Its pad
    /// is isolated at once (a sink's error flows back upstream and kills the
    /// shared ICE transport, silencing every track) and a rebuild scheduled.
    void isolateFailedReceiveBin(GstElement *pipelineChild);

    void handleCaptureError(const QString &cid,
                            const QString &front = QString());
    int microphoneSwapsForTest() const { return m_micSwaps; }
    int microphoneSwapsAbandonedForTest() const { return m_micSwapsAbandoned; }
    int speakerSwapsForTest() const { return m_speakerSwaps->load(); }
    void setSpeakerSinkForTest(const QString &description)
    {
        m_testSpeakerSink = description;
    }
    void setMicrophoneSwapTimeoutForTest(int ms) { m_micSwapTimeoutMs = ms; }
    void completeMicrophoneSwap(quint64 serial);
    GstElement *receiveBinForTest(const QString &streamId) const
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (auto it = m_receiveBins.cbegin(); it != m_receiveBins.cend(); ++it) {
            if (it->streamId == streamId && it->bin)
                return it->bin;
        }
        return nullptr;
    }
    static bool audioLevelExtensionAvailable();
    static constexpr int kAudioLevelExtId = 1;
    static void disableAudioLevelExtensionForTest(bool disabled);
    void setMicrophoneSourceForTest(const QString &description)
    {
        m_testMicSource = description;
    }
    bool microphoneStartingForTest() const { return microphoneStarting(); }
    int microphoneStartsReportedForTest() const { return m_micStartsReported; }
    bool microphoneSwapDeferredForTest() const
    {
        return m_micSwapDeferred.has_value();
    }
    void setMicrophoneStartAbandonMsForTest(int ms) { m_micStartAbandonMs = ms; }
    void setMicrophoneRtpCheckMsForTest(int ms) { m_micRtpCheckMs = ms; }
    QString lastMicrophoneRtpVerdictForTest() const
    {
        return m_lastMicRtpVerdict;
    }
    int microphoneSourceRestartsDoneForTest() const
    {
        return m_micSourceRestartsDone->load();
    }
    quint64 microphoneCapturedForTest() const
    {
        return m_micCaptured ? m_micCaptured->load() : 0;
    }
    void restartMicrophoneForTest()
    {
        m_micRestartPending = true;
        restartMicrophone(m_micCid, m_generation.load());
    }
    bool microphoneRestartPendingForTest() const { return m_micRestartPending; }
    bool microphoneNoiseModeDeferredForTest() const
    {
        return m_micNoiseModeAfterStart;
    }
    void setVideoSourceForTest(const QString &description)
    {
        m_testVideoSource = description;
    }
    void setSelfViewInTestModeForTest(bool on) { m_testSelfView = on; }
    int subscriberAutoremoveForTest() const;
    void setCaptureRestartDelayForTest(int ms) { m_micRestartBaseDelayMs = ms; }
    void setMicSlowRetryForTest(int ms) { m_micSlowRetryMs = ms; }
    void addMicCapturedForTest(quint64 buffers)
    {
        if (m_micCaptured)
            m_micCaptured->fetch_add(buffers);
    }
    int microphoneRestartsForTest() const { return m_micRestarts; }
    static bool micRestartBudgetResets(int restarts, qint64 sinceRestartMs,
                                       bool deliveredSinceRestart);

    void setRebuildJobDelayForTest(int ms) { m_rebuildJobDelayMs.store(ms); }
    int retireAllReceiveBinsForTest();
    int receiveVolumeElementsForTest() const;
    void setReceiveRebuildDelayForTest(int ms) { m_rebuildBaseDelayMs = ms; }
    void failReceiveRebuildsForTest(bool fail) { m_failReceiveRebuilds = fail; }
    void setFreshRebuildRunAfterForTest(int ms) { m_freshRebuildRunAfterMs = ms; }
    void rebuiltReceiveChainsNeverRenderForTest(bool never)
    {
        m_rebuiltChainsNeverRenderForTest = never;
    }

    void notifyAudioOutputsChanged(bool anyOutput = true);
    void setOutputChangeDebounceForTest(int quietMs, int maxWaitMs)
    {
        m_outputChangeQuietMs = quietMs;
        m_outputChangeMaxWaitMs = maxWaitMs;
    }
    int receiveRebuildAttemptsForTest() const { return m_receiveRebuildAttempts; }
    int receiveRearmsForTest() const { return m_receiveRearms; }

    QString publishedBinPropertyForTest(const QString &cid,
                                        const QString &elementName,
                                        const QString &property) const;
    bool publishedBinHasElementForTest(const QString &cid,
                                       const QString &elementName) const;

    int busesWithPendingMessagesForTest() const;

    void failNextPublishLinkForTest() { m_failNextPublishLink = true; }
    void failNextShareBackupForTest() { m_failNextShareBackup = true; }
    int offersDroppedForTest() const { return m_offersDropped; }
    static void setShareHardwareEncoderForTest(
        const std::optional<ShareHardwareEncoder> &encoder);
    static bool shareHardwareEncoderFailedForTest();
    static void clearShareHardwareEncoderFailedForTest();
    void setShareAudioSourceForTest(
        std::function<lightning::shareaudio::Enumeration(
            const QList<lightning::shareaudio::Stream> &)> enumerate,
        const lightning::shareaudio::BranchOptions &options);
    void setShareAudioCaptureForTest(
        const lightning::shareaudio::SystemCapture &capture)
    {
        m_shareAudioCaptureForTest = capture;
    }
    void rescanShareAudioSourcesForTest() { rescanShareAudioSources(); }
    void setShareAudioRebuildGapMsForTest(qint64 ms)
    {
        m_shareAudioRebuildGapMs = ms;
    }
    void setShareAudioBranchCapForTest(int cap) { m_shareAudioCap = cap; }
    void setShareAudioStartDelayForTest(int ms)
    {
        m_shareAudioStartDelayMsForTest = ms;
    }
    void failShareAudioStartsForTest(int count)
    {
        m_shareAudioFailStartsForTest = count;
    }
    bool shareAudioBranchStartingForTest(const QString &id) const
    {
        const auto it = m_shareAudioRecords.constFind(id);
        return it != m_shareAudioRecords.cend() && it->starting;
    }
    QStringList shareAudioCarriedForTest() const { return m_shareAudioCarried; }
    QStringList shareAudioFailedKeysForTest() const
    {
        return m_shareAudioFailedKeys;
    }
    void setShareAudioSuppressRetireEosForTest(bool on)
    {
        m_shareAudioSuppressRetireEosForTest = on;
    }
    bool shareAudioEosSeenForTest(const QString &id) const
    {
        const auto it = m_shareAudioEosSeen.constFind(id);
        return it != m_shareAudioEosSeen.cend() && (*it)->load();
    }
    void sendShareAudioEosForTest(const QString &id);
    void setShareAudioConnectionForTest(const QString &id, int fd)
    {
        m_shareAudioConnections.insert(id, fd);
    }
    int shareAudioBranchCountForTest() const { return m_shareAudioBranches; }
    int shareAudioBranchBinsForTest() const;
    bool shareAudioBranchMutedForTest(const QString &id) const;
    int shareAudioBranchIndexForTest(const QString &id) const;

    int receiveBinsForTest() const
    {
        QMutexLocker lock(&m_receiveBinMutex);
        return static_cast<int>(m_receiveBins.size());
    }

    int pendingTeardownsForTest() const
    {
        return m_pendingTeardowns->load();
    }

    std::shared_ptr<const std::atomic<int>> teardownCounterForTest() const
    {
        return m_pendingTeardowns;
    }

    int retiringWebrtcForTest() const { return m_retirer.retiringForTest(); }
    int teardownsWhileGatheringForTest() const
    {
        return m_retirer.whileGatheringForTest();
    }
    int retireBoundExpiriesForTest() const
    {
        return m_retirer.boundExpiredForTest();
    }
    int retiresThatWaitedOnGatheringForTest() const
    {
        return m_retirer.waitedOnGatheringForTest();
    }
    void setRetireBoundForTest(int ms) { m_retirer.setBoundForTest(ms); }

    void noteTeardownComplete(const QString &cid, quint64 generation);

    void noteMicrophoneStarted(const QString &cid, quint64 generation,
                               bool ok, qint64 tookMs);
    void noteShareAudioBranchStarted(const QString &id, int index,
                                     quint64 generation, bool ok);

    void applyRemoteDescription(Target target, const QString &kind,
                                const QString &sdp);
    void applyRemoteCandidate(Target target, const QString &candidateInit);

    void setMicrophoneMuted(bool muted);
    void setShareAudioMuted(bool muted);
    bool shareAudioMuted() const { return m_shareAudioMuted; }
    void setMicrophoneGain(int percent);
    void setNoiseSuppressionMode(calls::noise::Mode mode);
    void retryNoiseSuppression();
    calls::noise::Mode noiseSuppressionMode() const
    {
        return calls::noise::Mode(m_noiseMode.load());
    }
    bool noiseSuppressionFellBackToWebrtc() const
    {
        return m_noiseFellBackToWebrtc;
    }
    QString lastDspFollowUpForTest() const { return m_lastDspFollowUp; }
    static QString voiceProcessingDescription(calls::noise::Mode mode,
                                              bool echoCancel = false,
                                              bool autoGain = true);
    void setVoiceProcessing(bool echoCancel, bool autoGain)
    {
        m_echoCancel = echoCancel;
        m_autoGain = autoGain;
    }
    bool echoCancellation() const { return m_echoCancel; }
    bool autoGainControl() const { return m_autoGain; }
    static QString echoProbeStage(bool echoCancel, bool probeAvailable);
    static constexpr const char *kEchoProbeName = "webrtcechoprobe0";
    bool echoProbeLive() const;
    /// GUI thread: the requested echo cancellation, once a playback probe
    /// exists. webrtcdsp cancels against ONE probe, so only the first
    /// participant it finds is cancelled.
    void engageEchoCancellation();
    void handleMicDspError(const QString &cid, const QString &reason);
    void handleMicDspErrorForTest(const QString &cid, const QString &reason)
    { handleMicDspError(cid, reason); }
    void handleShareEncoderFailed(const QString &cid);
    int echoEngageAttemptsForTest() const { return m_echoEngageAttempts; }
    void startNextMicrophoneWithEchoForTest() { m_micEchoAtStartForTest = true; }
    quint64 microphoneRtpOutForTest() const
    {
        return m_micRtpOut ? m_micRtpOut->load() : 0;
    }
    void setOutputMuted(bool muted);
    void setParticipantVolume(const QString &streamId, int percent);

    /// Install our own sending key (16 raw bytes, never logged) at `index`.
    /// With `adopt == false` the key goes into the ring without becoming
    /// current, so a key that reached nobody is not used to encrypt our
    /// frames (which would otherwise look perfectly healthy from our side).
    void setOutboundKey(int index, const QByteArray &rawKey, bool adopt = true);

    int adoptedOutboundKeyIndexForTest() const;
    /// Install a key received from one sender, for decrypting that sender's
    /// media. `senderName` is the stable per-device name (keys are addressed
    /// to Matrix devices and may arrive before the SFU names the sid);
    /// noteParticipantIdentity() joins the two. One ring per sender, since
    /// LiveKit key indices are per participant.
    void setInboundKey(const QString &senderName, int index,
                       const QByteArray &rawKey);
    void noteParticipantIdentity(const QString &streamId,
                                 const QString &senderName);
    std::shared_ptr<CallFrameCryptor> recvCryptorFor(const QString &name);
    bool encryptionActive() const;
    quint64 framesEncrypted() const { return m_framesEncrypted.load(); }
    quint64 framesDecrypted() const { return m_framesDecrypted.load(); }
    quint64 framesDropped() const { return m_framesDropped.load(); }
    quint64 micBuffersHeldToClock() const;
    quint64 framesArrivingEncryptedOnAClearCall() const
    { return m_framesClearButCiphertextShaped.load(); }
    quint64 framesServerInjected() const
    { return m_framesServerInjected.load(); }
    void setServerInjectedTrailer(const QByteArray &trailer);
    QByteArray serverInjectedTrailer() const;
    void installDecryptProbeForTest(GstPad *pad, bool video,
                                    const QString &streamId)
    { installDecryptProbe(pad, video, streamId); }
    void installEncryptProbeForTest(GstPad *pad, bool video)
    { installEncryptProbe(pad, video); }
    void setEncryptionRequired(bool required);
    /// Forget all media keys: they must not outlive the call that used them.
    void clearKeys();

Q_SIGNALS:
    void shareTrackReady(const QString &cid, const QString &backupCid);
    void cameraPortalReleased(const QString &cid);
    void cameraCaptureReady(const QString &cid);
    void localDescription(int target, const QString &kind, const QString &sdp);
    void localCandidate(int target, const QString &candidateInit);
    void remoteTrackAdded(const QString &identity, const QString &mid,
                          const QString &kind);
    void remoteTrackRemoved(const QString &identity, const QString &kind);
    void connectionStateChanged(const QString &state);
    void transportStateChanged(int target, const QString &state);
    void failed(const QString &category);
    void publishFailed(const QString &cid, const QString &category);
    void microphoneRecovered(const QString &cid);
    void echoCancellationFailed();
    void remotePlaybackFailed(bool failed);

    void remoteMediaBlocked(const QString &streamId, const QString &reason);

    void localAudioSilent(bool silent, double peakDb);
    void localAudioLevel(double peakDb);
    void noiseSuppressionStatus(const QString &mode, bool ok);
    void shareAudioLevel(double peakDb);
    void shareAudioReport(bool live, bool perApplication, bool excludesUs,
                          const QStringList &carried,
                          const QStringList &failed, bool limitReached);
    void shareAudioRebuildWanted(const QString &cid);

private:
    friend class SfuMediaEngineTest;
    bool publishShareBackup(const QString &cid, GstElement *primary, bool glMemory);
    static QString offerForSfu(const QString &sdp, bool multiCodec);
    static QString offerIdentity(const QString &sdp);
    static QString receivableVideoCaps(bool h264);
    QHash<QString, QString> m_shareBackups;
    struct Peer {
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        QString offeredIdentity;
        QString answeredOffer;
        bool remoteDescriptionSet = false;
        bool offerPending = false;
        bool negotiationDirty = false;
        bool negotiationScheduled = false;
        quint64 remoteDescriptionRevision = 0;
        quint64 remoteCandidatesReceived = 0;
        QList<QString> pendingCandidates;
    };

    Peer &peerFor(Target target)
    {
        return target == Target::Publisher ? m_publisher : m_subscriber;
    }

    bool ensurePeer(Target target);
    void destroyPeer(Peer &peer);
    void applyIceTo(Peer &peer);
    void renegotiatePublisher();
    void releasePublishedFd(const QString &cid);
    void awaitPublishTeardowns();
    bool consumePublishLinkFailure();
    /// Releases (and unrefs) a webrtcbin request pad after a failed link. A
    /// leftover pad is a transceiver and adds an empty m= section to the next
    /// offer. Safe only because the pad has no peer.
    void releaseFailedPublishPad(GstPad *sinkPad);
    static constexpr int kTeardownWaitMs = 750;

    static void onPeerStateNotify(GstElement *webrtc, void *paramSpec,
                                  void *userData);
    static void onNegotiationNeeded(GstElement *webrtc, void *userData);
    static void onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                               char *candidate, void *userData);
    static void onPadAdded(GstElement *webrtc, void *pad, void *userData);
    static void onPadRemoved(GstElement *webrtc, void *pad, void *userData);
    static void onOfferCreated(GstPromise *promise, void *userData);
    static void onAnswerCreated(GstPromise *promise, void *userData);
    static void onRemoteDescriptionSet(GstPromise *promise, void *userData);
    static void onLocalDescriptionSet(GstPromise *promise, void *userData);

public Q_SLOTS:
    /// `token` is the emitting webrtcbin and `generation` is m_generation when
    /// the callback fired. Both must match: a new webrtcbin can reuse an old
    /// address.
    void handleLocalDescription(quintptr token, quint64 generation, bool offer,
                                const QString &sdp);
    void handleLocalCandidate(quintptr token, quint64 generation,
                              int mlineIndex, const QString &candidate);
    void handleFailure(quintptr token, quint64 generation,
                       const QString &category);
    void handleTransportState(quintptr token, quint64 generation,
                              const QString &state);
    void handlePublishError(const QString &cid, bool resourceError = false,
                            bool busyError = false,
                            bool captureFormatError = false,
                            int captureEpoch = -1);
    bool retryShareCaptureInOtherMemory(const QString &cid);
    int shareCaptureEpochForTest(const QString &cid) const
    {
        const auto retry = m_publishWatch.value(cid).retry;
        return retry ? retry->requested() : 0;
    }
    void checkCameraFirstFrame(const QString &cid, quint64 generation);

    void handleMicLevel(double peakDb);
    void handleMeterLevel(double peakDb);
    void handleShareAudioLevel(double peakDb);
    void handleShareAudioSourceError();
    void handleShareAudioBranchError(int index);
    void handleDenoiseStatus(const QString &requested, const QString &active,
                             bool ok, int latencySamples);
    void handleMicLevelAt(double peakDb, qint64 nowMs);
    void resetMicLevelState();
    bool microphoneSilentForTest() const { return m_micSilentAnnounced; }
    bool microphoneQuietForTest() const { return m_micQuietAnnounced; }
    double micPeakDbForTest() const { return m_micPeakDb; }
    void setDeviceChannelsForTest(int channels) { m_testDeviceChannels = channels; }
    QString lastAudioDescriptionForTest() const { return m_lastAudioDescription; }

public:
    static constexpr double kMicSilenceCeilingDb = -85.0;
    static constexpr qint64 kMicSilenceWindowMs = 10000;
    static constexpr double kMicQuietCeilingDb = -60.0;
    static constexpr qint64 kMicQuietWindowMs = 60000;

    static qint64 micSilenceSince(double peakDb, qint64 silentSinceMs,
                                  qint64 nowMs);
    static bool micSilenceReached(qint64 silentSinceMs, qint64 nowMs);
    static qint64 micQuietSince(double peakDb, qint64 quietSinceMs,
                                qint64 nowMs);
    static bool micQuietReached(qint64 quietSinceMs, qint64 nowMs);

private:
    void teardown(bool endOfCall);
    void clearSessionRouting();
    bool tokenIsLive(quintptr token, quint64 generation,
                     Target *target = nullptr) const;
    void installEncryptProbe(GstPad *pad, bool video,
                             std::shared_ptr<ProbeGate> gate = {},
                             bool h264 = false);
    bool detachPendingStart(const QString &cid, GstElement *bin,
                            bool force = false);
    void installDecryptProbe(GstPad *pad, bool video, const QString &streamId,
                             bool h264 = false,
                             std::shared_ptr<lightning::calls::StreamInfoStats> stats = {});
    void noteStreamIds(const QHash<int, QString> &byMline,
                       const QHash<int, QString> &midsByMline,
                       const QHash<int, QString> &tracksByMline);

    Peer m_publisher;
    Peer m_subscriber;
    bool m_active = false;
    lightning::CallAllocatorHold m_callAllocator;
    bool m_testSources = false;
    mutable QMutex m_deviceMutex;
    DeviceChoice m_cameraChoice;
    DeviceChoice m_microphoneChoice;
    DeviceChoice m_speakerChoice;
    OutputSink m_resolvedSpeaker;
    quint64 m_speakerSeq = 0;
    int m_shareMaxHeight = kScreenHeight;
    int m_shareFps = 30;
    int m_shareBitrateCapKbps = 0;
    int m_cameraMaxHeight = kCameraHeight;
    bool m_shareGlEncode = false;
    QString m_shareEncoderInUse;
    QString m_shareEncoderCid;
    std::atomic<bool> m_echoCancel{false};
    bool m_micEchoAfterStart = false;
    bool m_echoGivenUp = false;
    bool m_micDspFailureReported = false;
    bool m_micDspRescueAfterStart = false;
    bool m_micEchoAtStartForTest = false;
    void giveUpEchoCancellation(const QString &why);
    void rescueMicrophoneFromDsp();
    bool echoAllowedOnMic() const;
    int m_echoEngageAttempts = 0;
    std::atomic<bool> m_autoGain{true};
    std::atomic<quint64> m_generation{0};
    std::atomic<bool> m_outputMuted{false};
    bool m_microphoneMuted = false;
    bool m_shareAudioMuted = false;
    qint64 m_micSilentSinceMs = -1;
    qint64 m_micQuietSinceMs = -1;
    bool m_micSilentAnnounced = false;
    bool m_micQuietAnnounced = false;
    double m_micPeakDb = 0;
    double m_micSentPeakDb = -350.0;
    qint64 m_micLastLogMs = 0;
    QString m_lastAudioDescription;
    int m_testDeviceChannels = 0;
    std::atomic<int> m_microphoneGain{100};
    std::atomic<int> m_noiseMode{int(calls::noise::kDefaultMode)};
    bool m_noiseFellBackToWebrtc = false;
    QString m_lastDspFollowUp;
    QHash<QString, GstElement *> m_publishedBins;
    QHash<QString, int> m_publishedFds;
    struct AsyncStart {
        std::shared_ptr<ProbeGate> gate;
        std::shared_ptr<std::atomic<bool>> pending;
    };
    QHash<QString, AsyncStart> m_asyncStarts;
    QHash<QString, int> m_pendingTrackVolume;
    QSet<QString> m_volumeMissWarned;
    QHash<QString, int> m_volumeAppliedLog;
    void applyPendingTrackVolume(const QString &streamId,
                                 const QString &trackKey, quint64 generation);

    struct PublishProbeState {
        std::atomic<quint64> captured{0};
        /// Frames stay in GL memory: the keep-alive holds a reference, because
        /// a deep copy is a GL readback on every sample.
        bool keepByReference = false;
        std::atomic<qint64> firstCaptureMs{-1};
        std::atomic<qint64> firstEncodedMs{-1};
        qint64 startedMs = 0;
        bool screenShare = false;

        QMutex keepMutex;
        GstBuffer *lastFrame = nullptr;
        quint64 lastFramePts = 0;
        bool lastFramePtsValid = false;
        /// videorate's sink pad, ref'd while set; keep-alive frames are
        /// chained into it. Pushing from an IDLE probe on the upstream peer
        /// would deadlock.
        GstPad *keepSink = nullptr;
        std::atomic<qint64> lastFrameMs{-1};
        std::atomic<qint64> lastSampleMs{-1};
        std::atomic<quint64> keepAliveInjected{0};
        quint64 lastInjectedPts = 0;
        bool lastInjectedPtsValid = false;
        qint64 lastSampleAtMs = -1;

        ~PublishProbeState();
    };
    struct PublishWatch {
        std::shared_ptr<PublishProbeState> state;
        bool reported = false;
        std::shared_ptr<ShareCaptureRetry> retry;
    };
    QHash<QString, PublishWatch> m_publishWatch;
    std::unique_ptr<CallFrameCryptor> m_sendCryptor;
    QHash<QString, std::shared_ptr<CallFrameCryptor>> m_recvCryptors;
    mutable QMutex m_recvMutex;
    QSet<QString> m_diagnosedOnce;
    mutable QMutex m_diagnosedMutex;
    bool noteDiagnosisOnce(const QString &subject);
    struct KeyArrivalLog {
        int lastIndex = -1;
        quint32 arrivals = 0;
    };
    QHash<QString, KeyArrivalLog> m_keyArrivals;
    bool noteKeyArrival(const QString &ring, int index);
    QHash<int, QString> m_streamForMline;
    QHash<int, QString> m_midForMline;
    QHash<int, QString> m_trackForMline;
    std::atomic<bool> m_encryptionRequired{false};
    std::atomic<bool> m_sendKeyReady{false};
    std::atomic<bool> m_recvKeyReady{false};
    std::atomic<quint64> m_framesEncrypted{0};
    std::atomic<quint64> m_framesDecrypted{0};
    std::atomic<quint64> m_framesDropped{0};
    std::atomic<quint64> m_framesClearButCiphertextShaped{0};
    std::atomic<quint64> m_framesServerInjected{0};
    std::shared_ptr<lightning::calls::CaptureClockHold> m_micClockHold;
    QByteArray m_sifTrailer;
    mutable QMutex m_sifMutex;
    std::atomic<bool> m_sifRefusedWarned{false};
    /// A distinct IV stream id per encrypting track. The cryptor counts per
    /// stream id, and two tracks sharing one could produce the same IV under
    /// the same key, a full AES-GCM break. Only local uniqueness matters: the
    /// IV travels in the frame.
    std::atomic<quint32> m_nextIvStream{1};
    /// Publisher tracks linked to the publisher webrtcbin. webrtcbin raises
    /// on-negotiation-needed at PLAYING before any track exists, and an offer
    /// with no media section gets Leave(STATE_MISMATCH) from LiveKit. Atomic:
    /// written on the Qt thread, read on a GStreamer thread.
    std::atomic<int> m_publishedMedia{0};
    bool m_publisherEverPublished = false;
    std::shared_ptr<std::atomic<int>> m_pendingTeardowns
        = std::make_shared<std::atomic<int>>(0);
    bool m_failNextPublishLink = false;
    bool m_failNextShareBackup = false;
    int m_offersDropped = 0;
    mutable QMutex m_receiveBinMutex;
    struct ReceiveBin {
        GstElement *bin = nullptr;
        QString streamId;
        QString kind;
        QString transceiverMid;
        QString trackKey;
        unsigned long isolateProbe = 0;
        int rebuilds = 0;
        QString rebuildClientName = QString();
        bool rebuilding = false;
        double level = -1.0;
        qint64 lastRebuildMs = 0;
        qint64 recoveredMs = 0;
        bool givenUp = false;
        bool rearmOnGiveUp = false;
        bool rearmed = false;
    };
    QHash<GstPad *, ReceiveBin> m_receiveBins;
    /// Retire the receive bins fed by these transceivers, except the one on
    /// `keep`. Any thread. webrtcbin never removes a src pad, so this, not
    /// pad-removed, is how a track LiveKit retires stops playing. Returns how
    /// many bins went.
    /// `subscriberWebrtc` is the subscriber webrtcbin the CALLER holds (a
    /// streaming-thread caller passes its callback's own element; the GUI
    /// thread passes m_subscriber.webrtc), only for the diagnostic count.
    int retireReceiveBins(const QSet<QString> &transceiverMids, GstPad *keep,
                          const char *reason,
                          GstElement *subscriberWebrtc = nullptr);
    static int subscriberJitterbufferCount(GstElement *webrtc);
    int receiveBinCount() const;
    void teardownReceiveBin(const ReceiveBin &entry, bool announce = true);
    bool attachReceiveChain(GstElement *pipeline, GstPad *srcPad,
                            const QString &streamId, const QString &trackKey,
                            const QString &mediaKind, GstElement **binOut,
                            QString *why,
                            const QString &pulseClientName = QString(),
                            bool h264 = false);
    bool prepareReceiveChain(GstElement *pipeline, const QString &streamId,
                             const QString &trackKey, const QString &mediaKind,
                             GstElement **binOut, QString *why,
                             const QString &pulseClientName = QString(),
                             bool h264 = false);
    /// The other half: start the bin and link it. May block on the device;
    /// touches no engine state.
    static bool startReceiveChain(GstPad *srcPad, GstElement *bin,
                                  QString *why);
    void finishReceiveRebuild(GstPad *pad, quint64 generation,
                              GstElement *bin, bool ok, const QString &why,
                              double level, const QString &outputName);
    void noteReceiveChainRendering(GstPad *pad, quint64 generation);
    QString m_freshPulseClient;
    std::atomic<int> m_rebuildJobDelayMs{0};
    std::shared_ptr<std::atomic<bool>> m_sessionLive =
        std::make_shared<std::atomic<bool>>(true);
    std::atomic<int> m_receiveSinkEpoch{0};
    void scheduleReceiveRebuild(GstPad *pad, quint64 generation);
    void rebuildReceiveBin(GstPad *pad, quint64 generation);
    static void runRebuildJob(GstElement *pipeline, void *data);
    static constexpr int kMaxReceiveRebuilds = 5;
    void rearmReceiveChains();
    QTimer m_outputChangeTimer;
    qint64 m_outputChangeBurstStartMs = -1;
    bool m_outputChangeAnyOutput = true;
    int m_outputChangeQuietMs = 1000;
    int m_outputChangeMaxWaitMs = 5000;
    int m_receiveRebuildAttempts = 0;
    int m_receiveRearms = 0;
    void restartMicrophone(const QString &cid, quint64 generation);
    MicrophoneCapture testMicrophoneCapture() const;
    void swapMicrophoneFront(const MicrophoneCapture &capture);
    bool microphoneStarting() const
    {
        return m_micStartPending && m_micStartPending->load()
            && !m_micStartTakenOver;
    }
    void runMicrophoneStartDeferrals(const QString &cid, quint64 generation);
    void abandonMicrophoneSwap(const char *why);
    void forgetMicrophoneSwap();
    void swapReceiveSinks();
    struct PendingMicSwap {
        GstElement *bin = nullptr;
        GstElement *front = nullptr;
        QString name;
        unsigned long blockProbe = 0;
        quint64 serial = 0;
        bool currentFailed = false;
    };
    PendingMicSwap m_micSwap;
    std::shared_ptr<std::atomic<bool>> m_micStartPending;
    std::optional<MicrophoneCapture> m_micSwapDeferred;
    bool m_micRestartAfterStart = false;
    bool m_micNoiseModeAfterStart = false;
    bool m_micNoiseDspForceAfterStart = false;
    qint64 m_micStartDispatchedMs = 0;
    bool m_micStartTakenOver = false;
    bool m_micSwapReplacesStart = false;
    QString m_micLiveFront;
    std::shared_ptr<std::atomic<int>> m_micSourceRestartsDone =
        std::make_shared<std::atomic<int>>(0);
    int m_micStartAbandonMs = 5000;
    std::shared_ptr<std::atomic<quint64>> m_micRtpOut;
    int m_micRtpCheckMs = 3000;
    bool m_micRtpVerdictOwed = false;
    QString m_lastMicRtpVerdict;
    void checkMicrophoneRtp(const QString &cid, quint64 generation);
    int m_micStartsReported = 0;
    quint64 m_micSwapSerial = 0;
    quint64 m_micResolveSeq = 0;
    int m_micSwaps = 0;
    int m_micSwapsAbandoned = 0;
    int m_micSwapTimeoutMs = 5000;
    std::shared_ptr<std::atomic<int>> m_speakerSwaps =
        std::make_shared<std::atomic<int>>(0);
    QString m_testSpeakerSink;
    static constexpr int kMaxMicRestarts = 5;
    QString m_micCid;
    QString m_testMicSource;
    QString m_testVideoSource;
    bool m_testSelfView = false;
    int m_micRestarts = 0;
    qint64 m_micLastRestartMs = 0;
    std::shared_ptr<std::atomic<quint64>> m_micCaptured;
    quint64 m_micCapturedAtRestart = 0;
    bool m_micRestartPending = false;
    bool m_micFailureReported = false;
    int m_micSlowRetryMs = 30000;
    void scheduleMicSlowRetry(const QString &cid);
    int m_micRestartBaseDelayMs = 500;
    int m_cameraFirstFrameTimeoutMs = kCameraFirstFrameTimeoutMs;
    int m_portalCameraFirstFrameTimeoutMs = kPortalCameraFirstFrameTimeoutMs;
    int m_rebuildBaseDelayMs = 500;
    int m_freshRebuildRunAfterMs = 10000;
    bool m_rebuiltChainsNeverRenderForTest = false;
    bool m_failReceiveRebuilds = false;
    bool m_playbackFailureAnnounced = false;

    /// Not owned. QPointer so a destroyed router is never dereferenced from a
    /// late streaming-thread callback.
    QPointer<SfuVideoRouter> m_videoRouter;
    QString m_receiveNamespace;

public:
    static QStringList microphoneElementsForTest();
    static int statsTraceIntervalMs(const QString &raw);
    struct RtpStat {
        QString peer;
        QString dir;
        QString kind;
        quint32 ssrc = 0;
        quint64 packets = 0;
        qint64 lost = 0;
        double jitter = 0;
        quint64 bytes = 0;
        quint32 pli = 0;
        quint32 nack = 0;
        quint32 fir = 0;
        double rtt = 0;
        double fractionLost = 0;
        QString extra;
    };
private:
    void armStatsTrace();
    void requestStats();
    static void onStatsReady(GstPromise *promise, void *userData);
    void logStats(const QList<RtpStat> &stats);
    QTimer m_statsTimer;
    int m_statsIntervalMs = -1;
    QHash<quint32, QPair<qint64, quint64>> m_lastRtpBytes;
    QHash<quint32, qint64> m_lastRtpLost;

public:
    struct RecvHealth {
        QString streamId;
        std::atomic<quint64> gapEvents{0};
        std::atomic<quint64> lostEvents{0};
        std::atomic<quint64> queueOverruns{0};
        quint64 lastGap = 0;
        quint64 lastLost = 0;
        quint64 lastOverruns = 0;
    };
private:
    void instrumentReceiveBin(GstElement *bin, const QString &trackKey,
                              const QString &streamId);
    void logReceiveHealth();
    mutable QMutex m_recvHealthMutex;
    QHash<QString, std::weak_ptr<RecvHealth>> m_recvHealth;
    QHash<QString, std::array<quint64, 4>> m_lastJitterbuffer;
    bool m_statsFull = false;
    int m_healthReports = 0;
    int m_statsReports = 0;

    // Per-application share audio; see ShareAudioSources.h. A plain poll, not
    // a bus watch (which would need a GLib main loop). Only additions change
    // the topology: departing apps retire their branch via
    // `pipewiresrc on-disconnect=eos` where the plugin has it (else the live
    // mixer stops waiting on the silent pad), and a deselected app's branch
    // is muted, so no pad is unlinked on a live pipeline.
    void rescanShareAudioSources();
    bool addShareAudioBranch(GstElement *bin, GstElement *mixer,
                             const lightning::shareaudio::Stream &stream);
    void applyShareAudioScan(const lightning::shareaudio::Enumeration &e);
    void takeOutShareAudioBranch(GstElement *bin, GstElement *mixer, int index);
    void refreshShareAudioKeys();
    static int shareAudioCoolDownScans(int failures);
    void emitShareAudioReport();
    void resetShareAudioState();

    void tickShareKeepAlive();
    static void releaseKeepAlive(const std::shared_ptr<PublishProbeState> &s);
    void updateShareKeepAliveTimer();
    QTimer m_shareKeepAliveTimer;

    QTimer m_shareAudioScanTimer;
    QString m_shareAudioCid;
    QString m_shareAudioAnyCid;
    QHash<QString, lightning::shareaudio::BranchRecord> m_shareAudioRecords;
    QHash<QString, int> m_shareAudioRetiredAt;
    QHash<QString, std::shared_ptr<std::atomic<bool>>> m_shareAudioEosSeen;
    QHash<QString, int> m_shareAudioConnections;
    QHash<QString, int> m_shareAudioFailedAt;
    QHash<QString, int> m_shareAudioFailures;
    std::shared_ptr<std::atomic<bool>> m_shareAudioAlive =
        std::make_shared<std::atomic<bool>>(false);
    int m_shareAudioStartDelayMsForTest = 0;
    int m_shareAudioFailStartsForTest = 0;
    QElapsedTimer m_shareAudioScanStarted;
    bool m_shareAudioHangLogged = false;
    bool m_shareAudioSuppressRetireEosForTest = false;
    lightning::shareaudio::Selection m_shareAudioSelection;
    lightning::shareaudio::BranchOptions m_shareAudioOptions;
    QStringList m_shareAudioCarried;
    QStringList m_shareAudioFailedKeys;
    bool m_shareAudioLimitReached = false;
    bool m_shareAudioPerApplication = false;
    bool m_shareAudioExcludesUs = false;
    int m_shareAudioBranches = 0;
    int m_shareAudioCap = 24;
    int m_shareAudioNextIndex = 0;
    int m_shareAudioScans = 0;
    int m_shareAudioLevelReports = 0;
    bool m_shareAudioSourceErrorReported = false;
    bool m_shareAudioScanInFlight = false;
    quint64 m_shareAudioGeneration = 0;
    qint64 m_shareAudioRebuildAskedMs = -1;
    qint64 m_shareAudioRebuildGapMs = 10000;
    void noteShareAudioDaemonRestart(int closed);
    std::function<lightning::shareaudio::Enumeration(
        const QList<lightning::shareaudio::Stream> &)>
        m_shareAudioEnumerateForTest;
    lightning::shareaudio::BranchOptions m_shareAudioOptionsForTest;
    std::optional<lightning::shareaudio::SystemCapture>
        m_shareAudioCaptureForTest;

    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;

    lightning::webrtc::Retirer m_retirer;
};
