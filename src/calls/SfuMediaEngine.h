// LiveKit SFU media engine: GStreamer webrtcbin against an SFU rather than a
// single peer.
//
// Separate from GstCallMediaBackend (the 1:1 lane) because LiveKit differs
// structurally:
//
//   * Two peer connections: the client offers on PUBLISHER (its own tracks),
//     the server offers on SUBSCRIBER (everyone else's). They negotiate
//     independently and must never be confused.
//   * N remote streams arriving and leaving at any time, each routed to a
//     participant the UI knows.
//   * Tracks are declared (AddTrack) before negotiation, so track ids are
//     client-chosen up front.
//
// Threading: GStreamer calls back on its own threads; every callback marshals
// to the GUI thread through a process-global alive registry and re-checks the
// session generation, so a stale callback never touches the next session.
//
// Privacy: SDP and ICE carry host IPs and are never logged. ICE servers come
// only from the SFU's JoinResponse; there is no third-party STUN fallback.
#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <QHash>
#include <QSet>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "calls/ShareAudioSources.h"

typedef struct _GstElement GstElement;
typedef struct _GstPromise GstPromise;
typedef struct _GstPad GstPad;
typedef struct _GstBuffer GstBuffer;

class CallFrameCryptor;
class SfuVideoRouter;

class SfuMediaEngine : public QObject
{
    Q_OBJECT

public:
    /// Which peer connection, in LiveKit's own vocabulary.
    enum class Target { Publisher, Subscriber };

    /// Default screen-share ceiling declared to the SFU in AddTrack; without a
    /// size it infers three-layer simulcast. The live values come from
    /// setShareQuality().
    static constexpr int kScreenWidth = 1920;
    static constexpr int kScreenHeight = 1080;

    /// Screen-share scanline ceiling (720/1080/1440/2160) and frame rate
    /// (15/30/60). Both are encoder cost, so the user chooses.
    void setShareQuality(int maxHeight, int fps);

    /// The device the user chose, as QMediaDevices names it. Applied when a
    /// capture is built, never to a live one: relinking a running send branch
    /// is riskier than waiting for the next publish. An empty id means
    /// "system default", which keeps following the platform default.
    struct DeviceChoice {
        QString id;
        QString description;
    };
    void setPreferredDevices(const DeviceChoice &camera,
                             const DeviceChoice &microphone,
                             const DeviceChoice &speaker);
    /// Thread-safe: read from the GUI thread and from a streaming thread.
    DeviceChoice cameraChoice() const;
    DeviceChoice microphoneChoice() const;
    DeviceChoice speakerChoice() const;

    /// The share's caps ceiling for a chosen height and rate. Static and pure
    /// so the caps strings are testable without a live peer.
    static QString shareLimitsCaps(int maxHeight, int fps);
    /// Convert-and-scale stage for a share, CPU or GPU.
    static QString shareScaleStage(int maxHeight, bool gpu);
    /// The capsfilter between the capture and the rest. Pins PAR either way;
    /// the GPU form also admits a linear DMA-BUF.
    static QString captureEntryFilter(bool gpu);
    /// Camera entry for a source offering MJPG, decoded explicitly. Raw YUY2
    /// at 720p30 exceeds USB 2.0 and negotiates down to ~10 fps, and
    /// captureEntryFilter()'s `video/x-raw` filter rules MJPG out.
    ///
    /// `jpegdec`, not `decodebin`: decodebin's pads appear only once data
    /// flows, so linking fails ("Delayed linking failed").
    static QString cameraJpegEntry();
    /// Whether the GPU share path should be attempted (default yes;
    /// `LIGHTNING_SHARE_GPU=0` forces the CPU). Intent only; availability is
    /// checked separately so the log can tell them apart.
    static bool shareGpuScalingRequested();
    /// The first missing GL element for the GPU share chain, or empty. Named
    /// so the fallback log identifies a missing plugin.
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
    /// `videorate`'s sink pad, where keep-alive frames are chained. Found by
    /// element name, so tests query it against the real description. Caller
    /// owns the returned pad; nullptr if absent.
    static GstPad *keepAliveInjectionPad(GstElement *bin);
    /// The first name in `names` with no registered factory, or empty. Takes
    /// the list as a parameter so the missing branch is testable.
    static QString firstMissingElement(const QList<QByteArray> &names);
    /// The CPU stage a publish falls back to: the threaded single pass for a
    /// share, the plain two-element stage for a camera. Separate so the
    /// choice is testable.
    static QString cpuFallbackScaleStage(bool screenShare, int shareMaxHeight);
    /// Whether the GPU chain reaches PAUSED here (GL context, negotiation,
    /// shaders). Built once per process against a test source.
    static bool gpuShareChainUsable();
    /// Whether this build can decode MJPG. Probed once with real image/jpeg.
    static bool jpegCameraChainAvailable();
    static QString shareEncoderStage(int maxHeight, int fps);
    int shareMaxHeight() const { return m_shareMaxHeight; }
    int shareFps() const { return m_shareFps; }
    static constexpr int kCameraWidth = 1280;
    static constexpr int kCameraHeight = 720;

    /// One-time probe: GStreamer initialises and every element the SFU
    /// pipelines need resolves. `whyNot` receives a short, safe reason.
    static bool runtimeAvailable(QString *whyNot = nullptr);

    explicit SfuMediaEngine(QObject *parent = nullptr);
    ~SfuMediaEngine() override;

    /// Headless mode: synthetic sources and fakesinks, so CI can drive a real
    /// handshake without devices.
    void setTestSourceMode(bool on) { m_testSources = on; }
    bool testSourceMode() const { return m_testSources; }

    /// Start a session. Tears down any previous one; the generation bump
    /// invalidates in-flight callbacks.
    void start();
    /// Tear everything down and release every device.
    void stop();
    bool active() const { return m_active; }

    /// Where received video frames go. The engine only asks it whether anyone
    /// is watching before copying a frame.
    ///
    /// Out of line: m_videoRouter is a QPointer to a forward-declared type,
    /// and an inline use would require the complete type in every includer.
    void setVideoRouter(SfuVideoRouter *router);
    /// Also read from streaming threads; the router locks internally.
    SfuVideoRouter *videoRouter() const;

    /// ICE servers from the SFU's JoinResponse. Applied to both peer
    /// connections; credentials are engine-only and never logged.
    void setIceServers(const QVariantList &servers);

    /// Publish the microphone. `cid` must match the track id declared to the
    /// SFU with AddTrack.
    void publishAudio(const QString &cid);

    /// Publish what the computer is playing as a separate track, so it
    /// starts, mutes and stops independently of the microphone and the video.
    void publishShareAudio(const QString &cid);

    /// Whether this build can capture what is playing (platform, element and
    /// property all checked at runtime). The UI asks before offering it.
    static bool shareAudioAvailable();
    /// Publish the camera, or a screen share when `screenShare` is true.
    ///
    /// Capture targets, mutually exclusive:
    ///   * `nodeId`: the portal's PipeWire node on Linux, a monitor index on
    ///     Windows/macOS; -1 for the camera.
    ///   * `windowHandle`: a Windows HWND when a single window was chosen.
    ///   * `captureRect`: an X11 root-window rectangle, only for the Linux
    ///     no-portal fallback.
    ///
    /// `pipewireFd` is the portal's OpenPipeWireRemote descriptor; ownership
    /// passes here (closed on failure or by unpublish()). Without it a portal
    /// node resolves against our default remote and produces no frames.
    void publishVideo(const QString &cid, bool screenShare, int nodeId,
                      int pipewireFd = -1, quint64 windowHandle = 0,
                      const QRect &captureRect = {});
    /// Headless voice-delay check with its own negative control.
    ///
    /// Takes the queue specs from the description videoPipelineDescription()
    /// actually produces, starves each queue's consumer (not the whole
    /// process, which forms no backlog), and runs a default `queue` beside
    /// them as a control. Reports `current-level-time` before, during and
    /// after. Needs no GUI, account or sound card, so it runs from a package.
    /// Returns 0 when every shipped queue stayed bounded and recovered.
    static int runQueueSelfTest(QString *report);

    /// When the "cannot be decrypted" badge goes up. A failure rate over a
    /// sliding window with hysteresis: a single bad frame must not badge, and
    /// one good frame in fifty must not reset it (both have shipped before).
    /// O(1) per frame on the streaming thread.
    struct BlockedRunPolicy {
        /// How many recent outcomes the verdict is taken over.
        static constexpr int kWindow = 100;
        /// Raise at this percentage of the window failing.
        static constexpr int kRaisePercent = 90;
        /// Clear only at this lower percentage.
        static constexpr int kClearPercent = 25;
        /// Never judge a track on fewer outcomes than this.
        static constexpr int kMinObserved = 50;

        std::array<bool, kWindow> recent{};
        int next = 0;
        int observed = 0;
        int failures = 0;
        bool announced = false;

        /// Records one frame outcome. Returns true only when the UI must be
        /// told something new, with `*raise` giving the direction.
        bool note(bool failed, bool *raise);
        /// The window's current failure percentage, or -1 while it holds
        /// fewer than `kMinObserved` outcomes.
        int failurePercent() const;
    };

    /// The video publish pipeline as a gst_parse description, exposed so the
    /// screen-share shape (tee plus self-view) can be parsed in tests.
    static QString videoPipelineDescription(const QString &source,
                                            const QString &rateStage,
                                            const QString &limits,
                                            const QString &encoder,
                                            const QString &selfView,
                                            quint32 ssrc,
                                            const QString &scaleStage,
                                            const QString &entryFilter);
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
    /// Camera size ceiling. `portal` omits the pinned frame rate.
    static QString cameraLimitsCaps(bool portal);
    /// Camera rate stage. `portal` caps the rate instead of pinning it.
    static QString cameraRateStage(bool portal);
    /// Name of the receive bin's `volume` element for one stream; shared by
    /// the bin that creates it and the lookup that finds it.
    static QString outputVolumeElementName(const QString &streamId);
    /// The identity a receive volume element is named for: per track, since a
    /// participant can publish microphone and share audio.
    static QString volumeKeyFor(const QString &streamId,
                                const QString &trackKey);
    /// Set the level of one track.
    void setTrackVolume(const QString &streamId, const QString &trackKey,
                        int percent);

    /// Audio factor in percent for a user volume on the 0-200 slider. 0-100
    /// is 1:1 attenuation; 100-200 maps linearly to 100-1000 so boost is
    /// audible (1000 is the `volume` element's ceiling). Stored and displayed
    /// values stay on the user scale.
    static int audioFactorPercent(int userPercent);
    /// A distinct non-zero SSRC for one published track; see the definition.
    static quint32 nextPublishSsrc();
    /// Sets the msid the SFU uses to recognise a declared track.
    static void applyPublisherMsid(GstPad *sinkPad, const QString &cid);
    /// The track sid (`TR_...`) from an SDP `a=msid:` value, which names one
    /// track on both ends. A section `mid` is per-connection and cannot.
    static QString trackSidFromMsid(const QString &msid);
    /// The sending participant's id from an SDP `a=msid:` value. Media keys
    /// and video sinks are both keyed on it.
    static QString participantIdFromMsid(const QString &msid);
    /// Router key for the local camera's self-view frames.
    static QString localCameraStreamId();
    /// Router key for the local screen share's self-view frames.
    static QString localScreenStreamId();
    /// The capture-source fragment for a screen share.
    ///
    /// Linux: `nodeId` is the portal's PipeWire node and `pipewireFd` its
    /// remote (`path=` alone would resolve against our default remote and
    /// never produce a buffer). Windows/macOS: `nodeId` is a monitor index.
    /// `windowHandle` (Windows, single window) routes to WindowCaptureSrc;
    /// `captureRect` (Linux, no portal) routes to ximagesrc.
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
    /// Whether `name` is in the running GStreamer registry. Used for optional
    /// capabilities, not the required-element list.
    static bool elementAvailable(const char *name);
    /// The camera capture-source fragment per platform. On Linux `v4l2src`,
    /// not `autovideosrc`; see the definition.
    ///
    /// `pipewireFd` is from the xdg Camera portal, the only camera a Flathub
    /// build can have (no `/dev/video*` in the sandbox); -1 for direct
    /// capture. The route is chosen by SfuCallController::linuxCameraRoute().
    static QString cameraSource(int pipewireFd = -1);
    /// Stop publishing one track and renegotiate.
    void unpublish(const QString &cid);

    /// A capture that ended itself (window closed, camera unplugged): retire
    /// it, or the far end keeps the last picture. Distinct from
    /// handlePublishError(), which covers a publish that never started.
    void handleCaptureEnded(const QString &cid);

    /// Test-only: sink pads on the publisher webrtcbin (one per offered m=
    /// section), or -1 with no publisher.
    int publisherTrackSlotsForTest() const;
    /// The "volume" property of the first receive volume element for
    /// `streamId`, or -1 when that stream has no receive bin yet.
    double receiveVolumeForTest(const QString &streamId) const;
    /// Test-only: the "mute" property of the first receive volume element for
    /// `streamId`: 1 muted, 0 audible, -1 when there is no receive bin yet.
    int receiveMutedForTest(const QString &streamId) const;

    /// Test-only: is a bin registered under this cid?
    bool hasPublishedBinForTest(const QString &cid) const
    {
        return m_publishedBins.contains(cid);
    }

    /// Test-only: does the bin published under `cid` contain `elementName`?
    /// Out of line so includers do not link against GStreamer.
    bool publishedBinHasElementForTest(const QString &cid,
                                       const QString &elementName) const;

    /// Test-only: buses holding unread messages. Nothing pops them, so the
    /// correct answer is always 0.
    int busesWithPendingMessagesForTest() const;

    /// Test-only: make the next publish's link to webrtcbin fail once, to
    /// reach the link-failure cleanup path.
    void failNextPublishLinkForTest() { m_failNextPublishLink = true; }

    /// Test-only: receive bins held by the subscriber pipeline.
    int receiveBinsForTest() const
    {
        QMutexLocker lock(&m_receiveBinMutex);
        return static_cast<int>(m_receiveBins.size());
    }

    /// Test-only: outstanding deferred publish teardowns (zero after stop()).
    int pendingTeardownsForTest() const
    {
        return m_pendingTeardowns->load();
    }

    /// Test-only: the same counter, shared, readable after destruction.
    std::shared_ptr<const std::atomic<int>> teardownCounterForTest() const
    {
        return m_pendingTeardowns;
    }

    /// Tail of unpublish(), run on the GUI thread once the deferred teardown
    /// has put the bin at NULL. Public only for the GStreamer callbacks; not
    /// a control surface. A `generation` mismatch is dropped so a completion
    /// cannot touch the next session.
    void noteTeardownComplete(const QString &cid, quint64 generation);

    /// A remote description from the SFU for one peer connection.
    void applyRemoteDescription(Target target, const QString &kind,
                                const QString &sdp);
    /// One remote ICE candidate for one peer connection.
    void applyRemoteCandidate(Target target, const QString &candidateInit);

    /// Real mute: stops publishing, never attenuates.
    void setMicrophoneMuted(bool muted);
    /// Own microphone gain, 0..200 (see audioFactorPercent()), applied by a
    /// `volume` element in the send chain before the encoder. Not a mute:
    /// zero gain still publishes RTP.
    void setMicrophoneGain(int percent);
    /// Local playback mute for every remote track.
    void setOutputMuted(bool muted);
    /// Local volume for one participant, 0..200, keyed by their LiveKit
    /// stream id (`PA_...` sid). Local only; sends nothing.
    void setParticipantVolume(const QString &streamId, int percent);

    // Media encryption is per encoded frame (between encoder and payloader,
    // and after the depayloader), as LiveKit and Element Call do it.
    // encryptionActive() is what the controller reports as `mediaEncrypted`.

    /// Install our own sending key (32 raw bytes, never logged) at `index`.
    /// With `adopt == false` the key goes into the ring without becoming
    /// current, so a key that reached nobody is not used to encrypt our
    /// frames (which would otherwise look perfectly healthy from our side).
    void setOutboundKey(int index, const QByteArray &rawKey, bool adopt = true);

    /// The key index our frames currently use. Out of line: CallFrameCryptor
    /// is only forward-declared here.
    int adoptedOutboundKeyIndexForTest() const;
    /// Install a key received from one sender, for decrypting that sender's
    /// media. `senderName` is the stable per-device name (keys are addressed
    /// to Matrix devices and may arrive before the SFU names the sid);
    /// noteParticipantIdentity() joins the two. One ring per sender, since
    /// LiveKit key indices are per participant.
    void setInboundKey(const QString &senderName, int index,
                       const QByteArray &rawKey);
    /// Binds a sender's LiveKit stream id to the name its key ring is stored
    /// under, making them one ring. Keys and participant updates arrive in
    /// no fixed order; without this binding every frame from that sender is
    /// undecryptable. Idempotent; re-run on every participant update, which
    /// is how a binding that could not be made yet is retried.
    void noteParticipantIdentity(const QString &streamId,
                                 const QString &senderName);
    /// The key ring for one sender (participant identity or stream id),
    /// created on first sight. Public because the decrypt probe resolves it
    /// per frame on a streaming thread. Internally locked.
    std::shared_ptr<CallFrameCryptor> recvCryptorFor(const QString &name);
    /// Whether outgoing frames are actually being encrypted right now.
    bool encryptionActive() const;
    /// Frames encrypted, decrypted and dropped: separate "media never flowed"
    /// from "media flowed and was unusable". Written from streaming threads.
    quint64 framesEncrypted() const { return m_framesEncrypted.load(); }
    quint64 framesDecrypted() const { return m_framesDecrypted.load(); }
    quint64 framesDropped() const { return m_framesDropped.load(); }
    /// Receive-side frames passed through in the clear (`!required &&
    /// !haveKey`) that carry a crypto trailer: a peer encrypting on a call we
    /// believe is clear. A rate, not a count: `looksEncrypted` is a two-byte
    /// heuristic that cleartext can pass by chance.
    quint64 framesArrivingEncryptedOnAClearCall() const
    { return m_framesClearButCiphertextShaped.load(); }
    /// Frames livekit-server injected itself (recognised by the room's
    /// `sif_trailer`: blank frames on mute or track close), dropped and not
    /// counted in framesDropped().
    quint64 framesServerInjected() const
    { return m_framesServerInjected.load(); }
    /// Arms recognition of server-injected frames with the SFU's trailer, or
    /// disarms it with an empty array. Cleared with the keys, so it never
    /// outlives its SFU session. Only a trailer passing
    /// CallFrameCryptor::isUsableServerTrailer() arms. Thread-safe.
    void setServerInjectedTrailer(const QByteArray &trailer);
    QByteArray serverInjectedTrailer() const;
    /// Test-only: install the real decrypt probe on an arbitrary pad.
    void installDecryptProbeForTest(GstPad *pad, bool video,
                                    const QString &streamId)
    { installDecryptProbe(pad, video, streamId); }
    /// Require encryption: with no key installed, frames are dropped rather
    /// than sent in the clear.
    void setEncryptionRequired(bool required);
    /// Forget all media keys: they must not outlive the call that used them.
    void clearKeys();

Q_SIGNALS:
    /// A local description to hand the SFU.
    void localDescription(int target, const QString &kind, const QString &sdp);
    /// A local ICE candidate to trickle, already in LiveKit's JSON form.
    void localCandidate(int target, const QString &candidateInit);
    /// A remote track came up or went away. `identity` is the sender's LiveKit
    /// participant sid; `mid` names the exact track (camera vs screen share).
    void remoteTrackAdded(const QString &identity, const QString &mid,
                          const QString &kind);
    void remoteTrackRemoved(const QString &identity, const QString &kind);
    /// Aggregate connection state for the session, as a closed-set string.
    void connectionStateChanged(const QString &state);
    /// Terminal failure. `category` is safe to log; SDP never is.
    void failed(const QString &category);
    /// One published track cannot carry media; the call itself is fine.
    /// Deliberately not `failed()`, which ends the call. See
    /// handlePublishError() for when it is raised.
    void publishFailed(const QString &cid, const QString &category);

    /// A remote participant's frames are arriving and being dropped.
    /// `streamId` is the LiveKit participant sid. `reason` is a closed set:
    ///   "no_key"        no media key installed for that stream
    ///   "undecryptable" a key is installed and the frames will not open
    ///   ""              cleared: frames decrypt again
    void remoteMediaBlocked(const QString &streamId, const QString &reason);

    /// Our own microphone is delivering silence. Needed because every other
    /// counter treats silence like speech. `peakDb` is the capture peak in
    /// dBFS, never audio.
    void localAudioSilent(bool silent, double peakDb);

private:
    struct Peer {
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        bool remoteDescriptionSet = false;
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
    /// Close and forget the PipeWire descriptor a published bin owned.
    void releasePublishedFd(const QString &cid);
    /// Waits (bounded) for deferred publish teardowns. Their bins are briefly
    /// unparented and still running, out of destroyPeer()'s reach, with
    /// probes pointing into this engine.
    void awaitPublishTeardowns();
    /// Test-only fault injection; one-shot, always false in production.
    bool consumePublishLinkFailure();
    /// Releases (and unrefs) a webrtcbin request pad after a failed link. A
    /// leftover pad is a transceiver and adds an empty m= section to the next
    /// offer. Safe only because the pad has no peer.
    void releaseFailedPublishPad(GstPad *sinkPad);
    /// Bound for awaitPublishTeardowns(): an IDLE probe plus one thread-pool
    /// hop, short enough that a stuck pad costs a pause, not a hang.
    static constexpr int kTeardownWaitMs = 750;

    // GStreamer-thread callbacks. Each carries the emitting element's pointer
    // as a session token, checked against the live session before use.
    /// Logs ICE/DTLS state transitions on one peer connection.
    static void onPeerStateNotify(GstElement *webrtc, void *paramSpec,
                                  void *userData);
    static void onNegotiationNeeded(GstElement *webrtc, void *userData);
    static void onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                               char *candidate, void *userData);
    static void onPadAdded(GstElement *webrtc, void *pad, void *userData);
    /// Retires the receive bin this pad fed, asynchronously (a synchronous
    /// state change inside a PLAYING pipeline deadlocks).
    static void onPadRemoved(GstElement *webrtc, void *pad, void *userData);
    static void onOfferCreated(GstPromise *promise, void *userData);
    static void onAnswerCreated(GstPromise *promise, void *userData);

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
    /// A bus ERROR from inside the publishing bin `cid`, on the GUI thread.
    /// Reported only when the bin is still registered (so not a teardown),
    /// its capture delivered zero buffers (never prerolled), and it has not
    /// been reported yet.
    void handlePublishError(const QString &cid);

    /// A `level` peak from the capture chain, on the GUI thread.
    void handleMicLevel(double peakDb);
    /// As handleMicLevel(), with the clock supplied for tests.
    void handleMicLevelAt(double peakDb, qint64 nowMs);
    /// Forgets any previous silence judgement; called when the audio bin is
    /// built.
    void resetMicLevelState();
    bool microphoneSilentForTest() const { return m_micSilentAnnounced; }
    double micPeakDbForTest() const { return m_micPeakDb; }
    /// Test-only: pretend the capture device has this many channels, so the
    /// real multi-input description is parsed.
    void setDeviceChannelsForTest(int channels) { m_testDeviceChannels = channels; }
    /// The audio description publishAudio() last built (element names, ssrc,
    /// gain; no user content), so tests check what production composed.
    QString lastAudioDescriptionForTest() const { return m_lastAudioDescription; }

public:
    /// Peak dBFS at or below which a capture carries nothing audible. Speech
    /// peaks around -20; `level` reports digital silence as -350.
    static constexpr double kMicSilenceCeilingDb = -60.0;
    /// How long the ceiling must hold before it is reported; a conversational
    /// pause is not a diagnosis.
    static constexpr qint64 kMicSilenceWindowMs = 10000;

    /// Pure: carries the "silent since" mark across one report. -1 (not 0,
    /// which is a legal instant) means audible. Returns the new mark.
    static qint64 micSilenceSince(double peakDb, qint64 silentSinceMs,
                                  qint64 nowMs);
    /// Pure: has a mark aged past the window?
    static bool micSilenceReached(qint64 silentSinceMs, qint64 nowMs);

private:
    bool tokenIsLive(quintptr token, quint64 generation,
                     Target *target = nullptr) const;
    /// Install the ENCRYPT probe on one outgoing pad.
    void installEncryptProbe(GstPad *pad, bool video);
    /// Install the DECRYPT probe on one incoming pad for `streamId`. An
    /// unknown stream id still gets a ring, so a later key lands correctly.
    void installDecryptProbe(GstPad *pad, bool video, const QString &streamId);
    /// Records media-section index -> stream id, mid and track sid. Takes maps
    /// rather than the SDP: GstSDPMessage cannot be forward-declared.
    void noteStreamIds(const QHash<int, QString> &byMline,
                       const QHash<int, QString> &midsByMline,
                       const QHash<int, QString> &tracksByMline);

    Peer m_publisher;
    Peer m_subscriber;
    bool m_active = false;
    bool m_testSources = false;
    mutable QMutex m_deviceMutex;
    DeviceChoice m_cameraChoice;
    DeviceChoice m_microphoneChoice;
    DeviceChoice m_speakerChoice;
    int m_shareMaxHeight = kScreenHeight;
    int m_shareFps = 30;
    /// Bumped on every start/stop so a late callback is discarded.
    std::atomic<quint64> m_generation{0};
    /// Read on a streaming thread in pad-added, so a track arriving while
    /// deafened comes up silenced.
    std::atomic<bool> m_outputMuted{false};
    bool m_microphoneMuted = false;
    /// When the capture first fell to or below kMicSilenceCeilingDb, or -1
    /// while audible. Not judged while muted.
    qint64 m_micSilentSinceMs = -1;
    bool m_micSilentAnnounced = false;
    /// Last peak, and when the level was last logged.
    double m_micPeakDb = 0;
    qint64 m_micLastLogMs = 0;
    QString m_lastAudioDescription;
    int m_testDeviceChannels = 0;
    /// Own microphone gain in percent, 0..200. Atomic because the send chain
    /// may be rebuilt on a streaming thread and must start at the user's
    /// level.
    std::atomic<int> m_microphoneGain{100};
    /// Published tracks by client-chosen id, so unpublish can find them.
    QHash<QString, GstElement *> m_publishedBins;
    /// PipeWire remote descriptors owned by publishing bins, closed on
    /// teardown.
    QHash<QString, int> m_publishedFds;
    /// Volumes requested before their receive bin existed, keyed as
    /// setTrackVolume() addresses them; applied when the bin is built.
    QHash<QString, int> m_pendingTrackVolume;
    /// Keys whose "nowhere to land" diagnostic has been logged once.
    QSet<QString> m_volumeMissWarned;
    /// Last applied percentage per key, so the "applied" line logs only on a
    /// real change.
    QHash<QString, int> m_volumeAppliedLog;
    void applyPendingTrackVolume(const QString &streamId,
                                 const QString &trackKey, quint64 generation);

    /// What one publishing bin's capture has produced, shared with the pad
    /// probes. `captured == 0` identifies a dead publish (downstream counters
    /// see manufactured frames), and firstEncodedMs - firstCaptureMs is how
    /// long the rate stage held the opening picture.
    struct PublishProbeState {
        std::atomic<quint64> captured{0};
        /// Monotonic ms since the publish was dispatched; -1 means "not yet".
        std::atomic<qint64> firstCaptureMs{-1};
        std::atomic<qint64> firstEncodedMs{-1};
        /// Written once before the bin plays; read only afterwards.
        qint64 startedMs = 0;
        bool screenShare = false;

        // Keep-alive for an on-damage capture that has gone quiet: videorate
        // emits nothing until a second buffer arrives, so a window that never
        // repaints would publish nothing. tickShareKeepAlive() re-pushes a
        // deep copy sampled after the scale stage (small, and it holds no
        // PipeWire pool buffer).
        //
        // `keepMutex` guards `lastFrame`, `lastFramePts`, `lastFramePtsValid`
        // and `lastSampleAtMs`, which are only meaningful together.
        QMutex keepMutex;
        GstBuffer *lastFrame = nullptr;
        /// The source's PTS for that frame; injections are stamped from it
        /// (see keepAlivePts()).
        quint64 lastFramePts = 0;
        bool lastFramePtsValid = false;
        /// videorate's sink pad, ref'd while set; keep-alive frames are
        /// chained into it. Pushing from an IDLE probe on the upstream peer
        /// would deadlock.
        GstPad *keepSink = nullptr;
        /// Monotonic ms of the last REAL buffer; -1 means none yet.
        std::atomic<qint64> lastFrameMs{-1};
        /// Monotonic ms the last sampled copy was taken, to throttle copying.
        std::atomic<qint64> lastSampleMs{-1};
        std::atomic<quint64> keepAliveInjected{0};
        /// Last injected PTS, so injections strictly increase. GUI thread only.
        quint64 lastInjectedPts = 0;
        bool lastInjectedPtsValid = false;
        /// When `lastFrame` was sampled; guarded by `keepMutex`, unlike the
        /// atomic throttle above.
        qint64 lastSampleAtMs = -1;

        // Backstop cleanup: releaseKeepAlive() runs while the bin may still be
        // playing, so the sampling probe can store one more frame afterwards.
        // The state dies only with the probe's own shared_ptr, so this cannot
        // race.
        ~PublishProbeState();
    };
    struct PublishWatch {
        std::shared_ptr<PublishProbeState> state;
        /// One report per publish.
        bool reported = false;
    };
    /// GUI thread only; probes hold their own shared_ptr to the state.
    QHash<QString, PublishWatch> m_publishWatch;
    /// One cryptor for what we send.
    std::unique_ptr<CallFrameCryptor> m_sendCryptor;
    /// One cryptor per sender for receiving, keyed by name (see
    /// recvCryptorFor()). Created on demand by a key or a track, in either
    /// order. Guarded by m_recvMutex.
    QHash<QString, std::shared_ptr<CallFrameCryptor>> m_recvCryptors;
    mutable QMutex m_recvMutex;
    /// Subjects already diagnosed this session; see noteDiagnosisOnce(). Own
    /// mutex: callers may already hold m_recvMutex.
    QSet<QString> m_diagnosedOnce;
    mutable QMutex m_diagnosedMutex;
    bool noteDiagnosisOnce(const QString &subject);
    /// Per-ring key-arrival record for the "key ARRIVED" line. Guarded by
    /// m_diagnosedMutex, at most 256 rings, cleared with the session.
    struct KeyArrivalLog {
        int lastIndex = -1;
        quint32 arrivals = 0;
    };
    QHash<QString, KeyArrivalLog> m_keyArrivals;
    bool noteKeyArrival(const QString &ring, int index);
    /// Media-section index -> sender's LiveKit stream id, from the subscriber
    /// offer's `msid`.
    QHash<int, QString> m_streamForMline;
    /// Media-section index -> the section's SDP `mid`.
    QHash<int, QString> m_midForMline;
    /// Media-section index -> track sid (`TR_...`), parsed from the same
    /// `a=msid:` line. Parsed ourselves because how much of the pad's `msid`
    /// property webrtcbin fills varies across GStreamer versions.
    QHash<int, QString> m_trackForMline;
    /// Read by the pad probes on streaming threads, written from the Qt thread.
    std::atomic<bool> m_encryptionRequired{false};
    std::atomic<bool> m_sendKeyReady{false};
    std::atomic<bool> m_recvKeyReady{false};
    /// See framesEncrypted(). Reset per session in start().
    std::atomic<quint64> m_framesEncrypted{0};
    std::atomic<quint64> m_framesDecrypted{0};
    std::atomic<quint64> m_framesDropped{0};
    /// See framesArrivingEncryptedOnAClearCall(). Reset per session.
    std::atomic<quint64> m_framesClearButCiphertextShaped{0};
    /// See framesServerInjected(). Reset per session in start().
    std::atomic<quint64> m_framesServerInjected{0};
    /// See setServerInjectedTrailer(). Own mutex; nests inside nothing.
    QByteArray m_sifTrailer;
    mutable QMutex m_sifMutex;
    /// A refused trailer is warned about once per engine.
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
    /// Whether this publisher has ever carried a track. Unlike
    /// m_publishedMedia it stays true after the last track is withdrawn,
    /// which must still renegotiate (the section goes a=inactive). Reset with
    /// the session.
    bool m_publisherEverPublished = false;
    /// Deferred publish teardowns in flight. Shared, so a decrement after a
    /// timed-out wait stays safe once the engine is gone.
    std::shared_ptr<std::atomic<int>> m_pendingTeardowns
        = std::make_shared<std::atomic<int>>(0);
    /// Test-only fault injection; see failNextPublishLinkForTest().
    bool m_failNextPublishLink = false;
    /// Receive bins keyed by the webrtcbin src pad feeding each, so a
    /// departing track's bin can be retired. Written from streaming threads,
    /// read by stop(), hence the mutex.
    mutable QMutex m_receiveBinMutex;
    struct ReceiveBin {
        GstElement *bin = nullptr;
        /// Who stopped sending, for remoteTrackRemoved.
        QString streamId;
        QString kind;
    };
    QHash<GstPad *, ReceiveBin> m_receiveBins;

    /// Not owned. QPointer so a destroyed router is never dereferenced from a
    /// late streaming-thread callback.
    QPointer<SfuVideoRouter> m_videoRouter;

    // Opt-in RTP statistics trace (LIGHTNING_CALL_STATS_TRACE). The frame
    // counters sit above RTP and miss loss, keyframe requests and bitrate
    // dips. Logs numbers only, per SSRC: packets, loss, jitter, bitrate,
    // PLI/NACK/FIR, and remote RTT and fraction lost. The value is the
    // interval in seconds (1/true/yes = 5 s).
public:
    /// Test-only: the capture element order this platform will try.
    static QStringList microphoneElementsForTest();
    static int statsTraceIntervalMs(const QString &raw);
    struct RtpStat {
        QString peer;      // "pub" / "sub"
        QString dir;       // inbound / outbound / remote-inbound
        QString kind;      // audio / video / "" when the build does not say
        quint32 ssrc = 0;
        quint64 packets = 0;
        qint64 lost = 0;
        double jitter = 0;     // seconds
        quint64 bytes = 0;
        quint32 pli = 0;
        quint32 nack = 0;
        quint32 fir = 0;
        double rtt = 0;        // seconds, remote-inbound only
        double fractionLost = 0;
    };
private:
    void armStatsTrace();
    void requestStats();
    static void onStatsReady(GstPromise *promise, void *userData);
    void logStats(const QList<RtpStat> &stats);
    QTimer m_statsTimer;
    int m_statsIntervalMs = -1;   // -1: environment not read yet
    QHash<quint32, QPair<qint64, quint64>> m_lastRtpBytes; // ssrc -> (ms, bytes)

    // Per-application share audio; see ShareAudioSources.h. A plain poll, not
    // a bus watch (which would need a GLib main loop). Only additions are
    // handled: departing apps retire their branch via
    // `pipewiresrc on-disconnect=eos`, so no pad is unlinked on a live
    // pipeline.
    void rescanShareAudioSources();

    // Supplies the second buffer `videorate` needs while the screen is still.
    // One timer for all shares; never armed for cameras, which deliver on a
    // clock.
    void tickShareKeepAlive();
    /// Releases the injection pad and sampled frame a publish is holding.
    static void releaseKeepAlive(const std::shared_ptr<PublishProbeState> &s);
    /// Runs the timer exactly while some watched share still holds a pad.
    /// Called from every site that can change the answer.
    void updateShareKeepAliveTimer();
    QTimer m_shareKeepAliveTimer;

    lightning::shareaudio::SourceMonitor m_shareAudioSources;
    QTimer m_shareAudioScanTimer;
    QString m_shareAudioCid;
    QSet<QString> m_shareAudioSerials;   // already given a branch
    int m_shareAudioBranches = 0;        // bounds a share that outlives many apps
    int m_shareAudioNextIndex = 0;       // element names; never reused
    int m_shareAudioScans = 0;           // polls since the share started

    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
