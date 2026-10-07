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
// Teardown: a stopped peer's pipeline stops at once, but its webrtcbin only
// once ICE gathering has ended (WebrtcRetirer.h, GitHub #3).
//
// Privacy: SDP and ICE carry host IPs and are never logged. ICE servers come
// only from the SFU's JoinResponse; there is no third-party STUN fallback.
#pragma once

#include <array>
#include <atomic>
#include <memory>
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

#include "calls/CaptureDeviceSelection.h"
#include "calls/ShareAudioSources.h"
#include "calls/noise/NoiseSuppressor.h"
#include "calls/WebrtcRetirer.h"

typedef struct _GstElement GstElement;
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
        /// Camera only: a camera was chosen and is not usable now. Opens no
        /// camera rather than the default (CaptureDeviceSelection.h).
        bool preferredMissing = false;
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
    /// Test-only: sets `property` on a fresh `factory` element the way a
    /// device binding is applied, and reads it back serialised; empty when
    /// the element refused it. avfvideosrc's `device-index` is an int, so the
    /// conversion is the thing under test.
    static QString applyDevicePropertyForTest(const QString &factory,
                                              const QString &property,
                                              const QString &value);
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
    /// Whether caps a camera reported admit MJPG. Unknown (null, ANY or
    /// empty) counts as yes, which keeps the MJPG attempt.
    static bool capsOfferJpeg(const GstCaps *caps);
    /// Opens `capsrc` in `bin` (READY) and asks whether the device can
    /// produce MJPG, then puts it back to NULL. Needed because v4l2src's
    /// template lists image/jpeg for every camera, so a raw-only camera
    /// parses behind the MJPG filter and only fails to negotiate.
    static bool captureOffersJpeg(GstElement *bin);
    static QString shareEncoderStage(int maxHeight, int fps);
    int shareMaxHeight() const { return m_shareMaxHeight; }
    int shareFps() const { return m_shareFps; }
    static constexpr int kCameraWidth = 1280;
    static constexpr int kCameraHeight = 720;

    /// One-time probe: GStreamer initialises and every element the SFU
    /// pipelines need resolves. `whyNot` receives a short, safe reason.
    static bool runtimeAvailable(QString *whyNot = nullptr);

    // ── The microphone chain, shared with AudioDeviceTester ──
    // The Settings microphone test must open the device exactly as a call
    // does (same element choice, same multi-input handling, same processing),
    // or it tests something else. These are the one implementation both use.

    /// The capture a call opens for `mic`.
    struct MicrophoneCapture {
        /// The source element, named `micsrc`, e.g. "pulsesrc name=micsrc".
        QString source;
        /// Set on `micsrc` after the parse (never interpolated: a quote in a
        /// device name would be parsed as syntax). Empty for the default.
        lightning::calls::DeviceBinding binding;
        /// Channels to pin for the multi-input matrix; 0 mixes normally.
        int channels = 0;
    };
    /// Resolved as publishAudio() resolves it: `autoaudiosrc` for "system
    /// default", otherwise the first concrete element that can bind the
    /// device. A chosen device asks GStreamer's device monitor, which may
    /// block for up to the enumeration budget (2.5 s); call it off the GUI
    /// thread where possible.
    static MicrophoneCapture resolveMicrophoneCapture(const DeviceChoice &mic);
    /// "<source> [channel caps] ! queue (bounded, leaky) ! audioconvert
    /// [mix-matrix] ! audioresample ! audio/x-raw,channels=1": mono at the
    /// device's rate, ready for processing. No trailing separator.
    static QString microphoneFrontDescription(const MicrophoneCapture &capture);
    /// Whether gst-plugins-good's `level` exists in this build.
    static bool levelElementAvailable();
    /// Sets `binding` on the element called `elementName` inside `bin`.
    /// Returns false when nothing was set.
    static bool applyCaptureBinding(GstElement *bin, const char *elementName,
                                    const lightning::calls::DeviceBinding &binding);
    /// The output a chosen speaker resolves to: a sink description named
    /// `outsink` and the binding to set on it. `autoaudiosink` for "system
    /// default" or when nothing can bind the device. May block like
    /// resolveMicrophoneCapture().
    struct OutputSink {
        QString sink;
        lightning::calls::DeviceBinding binding;
    };
    static OutputSink resolveSpeakerSink(const DeviceChoice &speaker);
    /// The loudest channel's peak from a `level` element message. False when
    /// the message carries no readable peak; -350 (digital silence) is a real
    /// reading and returns true.
    static bool readLevelPeak(const GstStructure *fields, double *peakDb);
    /// `level`'s reporting period on the microphone, in nanoseconds: the
    /// in-call meter's refresh rate.
    static constexpr quint64 kMicLevelIntervalNs = 50000000;
    /// The second `level` on the microphone, `miccapturelevel`: what the
    /// device captures, BEFORE noise suppression, for the dead-microphone
    /// judgement. Its windows are seconds long, so 200 ms is plenty.
    static constexpr quint64 kMicCaptureLevelIntervalNs = 200000000;

    explicit SfuMediaEngine(QObject *parent = nullptr);
    ~SfuMediaEngine() override;

    /// Headless mode: synthetic sources and fakesinks, so CI can drive a real
    /// handshake without devices.
    void setTestSourceMode(bool on) { m_testSources = on; }
    bool testSourceMode() const { return m_testSources; }

    /// Start a session. Tears down any previous one; the generation bump
    /// invalidates in-flight callbacks. Media keys are kept: they belong to
    /// the call, and a peer's key can arrive while we are still joining
    /// (join() has already cleared the previous call's).
    void start();
    /// Tear everything down, release every device and forget the call's keys.
    void stop();
    /// End this SFU session's media (peer connections, captures, receive
    /// bins) but keep the call's keys, as start() does: the call is
    /// reconnecting and the next start() continues it. A peer's key does not
    /// come again, so forgetting it here would leave that peer undecryptable
    /// after the rejoin. The generation bump stops every callback of the old
    /// peer connections.
    void suspend();
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

    /// Publish the microphone. `cid` must match the track id declared to the
    /// SFU with AddTrack.
    void publishAudio(const QString &cid);

    /// Publish what the computer is playing as a separate track, so it
    /// starts, mutes and stops independently of the microphone and the video.
    /// `selection` says which applications (or the whole system, minus us);
    /// Mode::Apps never falls back to the whole system: if the chosen
    /// applications cannot be captured it fails with
    /// `share_audio_apps_unavailable` rather than send more than was chosen.
    void publishShareAudio(const QString &cid,
                           const lightning::shareaudio::Selection &selection =
                               lightning::shareaudio::Selection{});
    /// Changes which applications a running share carries, in place: a
    /// deselected application's branch is muted (never unlinked) and a newly
    /// chosen one gets a branch. False when the running bin cannot express
    /// the change (a single loopback capture asked for applications, or the
    /// reverse); the caller then republishes the track.
    bool applyShareAudioSelection(
        const lightning::shareaudio::Selection &selection);

    /// Whether this build can capture what is playing (platform, element and
    /// property all checked at runtime). The UI asks before offering it.
    static bool shareAudioAvailable();
    /// Whether "everything except Lightning" really leaves Lightning out
    /// here: per-application capture, or Windows process-tree exclusion.
    /// False means the only whole-system capture is the output monitor, which
    /// carries this call back to the call.
    static bool shareAudioSystemExcludesUs();
    /// Every name an audio server may record this process under: PipeWire
    /// uses the binary name, which differs from applicationName().
    static QStringList ownAudioClientNames();
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
    /// The self-view branch a camera or share publish tees off (not in
    /// test-source mode). Its appsink never waits for a preroll frame.
    static QString selfViewBranch();
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
    /// One received track's chain, as a bin ready to add to the subscriber
    /// pipeline (floating ref). `testSink` ends audio in a fakesink. The bin
    /// handles its own preroll, so a track that never delivers a frame cannot
    /// hold the pipeline's state. Returns null with `*error` on failure.
    /// A non-empty `pulseClientName` makes the audio output a pulsesink on a
    /// Pulse connection of that name (outside test mode); see
    /// rebuildReceiveBin().
    /// `speaker`, when non-null and carrying a device binding (the user chose
    /// an output), replaces the default sink: its `outsink` element gets the
    /// binding after the parse, exactly as the 1:1 lane and the device test do.
    static GstElement *buildReceiveBin(bool video,
                                       const QString &volumeElementName,
                                       bool testSink, QString *error,
                                       const QString &pulseClientName = QString(),
                                       const OutputSink *speaker = nullptr);
    /// The sink part of an audio receive bin's description, for tests.
    /// Resolves the chosen speaker on a worker thread and caches it. Called
    /// when the choice changes and when a call starts; never from a
    /// streaming thread.
    void refreshSpeakerSink();
    /// The cached resolution (empty binding when none yet). Cheap; safe from
    /// any thread.
    OutputSink cachedSpeakerSink() const;
    static QString receiveSinkDescription(const QString &pulseClientName,
                                          const OutputSink *speaker);
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

    /// How long a direct camera publish may run without its capture producing
    /// a single buffer before it is reported as `camera_no_frames`. A camera
    /// that opens and never delivers posts no error (a PipeWire remote with
    /// no camera node routed to it waits forever), so without this the track
    /// is declared, the far end shows a tile, and nobody ever sees a picture.
    /// Generous: a UVC camera's first frame takes 0.3-2 s.
    static constexpr int kCameraFirstFrameTimeoutMs = 8000;
    /// The same for a camera granted by the xdg Camera portal: PipeWire's
    /// negotiation comes first, and IPU6/libcamera cameras behind it are slow
    /// to start. Nothing waits on it (the start runs off the GUI thread).
    static constexpr int kPortalCameraFirstFrameTimeoutMs = 15000;
    /// Test-only: shorten both first-frame budgets to `ms`.
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

    /// Test-only: the bin published under `cid` (borrowed), or null.
    GstElement *publishedBinForTest(const QString &cid) const
    {
        return m_publishedBins.value(cid, nullptr);
    }

    /// Called by the bus sync handler, any thread: a receive bin posted an error. Its pad
    /// is isolated at once (a sink's error flows back upstream and kills the
    /// shared ICE transport, silencing every track) and a rebuild scheduled.
    void isolateFailedReceiveBin(GstElement *pipelineChild);

    /// GUI thread: the microphone's source posted an error. A capture that
    /// failed (measured: pulsesrc "Failed to connect stream: Timeout" while
    /// the sound server stalled) never retries by itself, so the track stays
    /// declared and silent for the whole call. Restarted with backoff.
    void handleCaptureError(const QString &cid);
    /// Test-only: the receive bin of the first track from `streamId`
    /// (borrowed), or null.
    GstElement *receiveBinForTest(const QString &streamId) const
    {
        QMutexLocker lock(&m_receiveBinMutex);
        for (auto it = m_receiveBins.cbegin(); it != m_receiveBins.cend(); ++it) {
            if (it->streamId == streamId && it->bin)
                return it->bin;
        }
        return nullptr;
    }
    /// Whether the microphone can carry its audio level to the SFU in the
    /// RFC 6464 header extension (LiveKit's speaking indicator reads it):
    /// `rtphdrextclientaudiolevel` exists and `level` can attach
    /// GstAudioLevelMeta (1.20+). Without it the publish goes on as before.
    static bool audioLevelExtensionAvailable();
    /// The extmap id the microphone's audio level is sent under.
    static constexpr int kAudioLevelExtId = 1;
    /// Test-only: behave as if the audio-level extension were missing.
    static void disableAudioLevelExtensionForTest(bool disabled);
    /// Test-only: the microphone source description in test-source mode.
    void setMicrophoneSourceForTest(const QString &description)
    {
        m_testMicSource = description;
    }
    /// Test-only: the video source description in test-source mode.
    void setVideoSourceForTest(const QString &description)
    {
        m_testVideoSource = description;
    }
    /// Test-only: build the self-view branch in test-source mode too.
    void setSelfViewInTestModeForTest(bool on) { m_testSelfView = on; }
    /// Test-only: whether the subscriber's rtpbin drops timed-out sources.
    /// -1 when there is no subscriber yet.
    int subscriberAutoremoveForTest() const;
    /// Test-only: the first microphone restart waits `ms`, doubling after.
    void setCaptureRestartDelayForTest(int ms) { m_micRestartBaseDelayMs = ms; }
    /// Test-only: microphone restarts attempted in this run.
    int microphoneRestartsForTest() const { return m_micRestarts; }

    /// Test-only: a rebuild job sleeps `ms` before it builds anything.
    void setRebuildJobDelayForTest(int ms) { m_rebuildJobDelayMs.store(ms); }
    /// Test-only: retire every receive bin, as a renegotiation marking every
    /// section inactive would.
    int retireAllReceiveBinsForTest();
    /// Test-only: `outvol_*` elements in the subscriber pipeline.
    int receiveVolumeElementsForTest() const;
    /// Test-only: the first receive-bin rebuild waits `ms`, doubling after.
    void setReceiveRebuildDelayForTest(int ms) { m_rebuildBaseDelayMs = ms; }
    /// Test-only: every receive-bin rebuild fails, as with no sound server.
    void failReceiveRebuildsForTest(bool fail) { m_failReceiveRebuilds = fail; }

    /// GUI thread: the system's list of audio outputs changed (a headset
    /// plugged in or out, a Bluetooth sink back, an RDP audio endpoint
    /// returning). `anyOutput` is whether at least one output exists now.
    /// Debounced, because devices flap: once the changes have been quiet for
    /// a moment, every receive chain that gave up on its output gets ONE more
    /// rebuild, and one still retrying gets one more attempt when its own
    /// budget runs out. Without this a chain that gave up stayed silent until
    /// the user left and rejoined, although the device came back (measured on
    /// Windows 2026-10-07: AUDCLNT_E_DEVICE_INVALIDATED, back a minute later).
    void notifyAudioOutputsChanged(bool anyOutput = true);
    /// Test-only: how long the output changes must be quiet before the
    /// re-arm, and the most a burst may hold it off.
    void setOutputChangeDebounceForTest(int quietMs, int maxWaitMs)
    {
        m_outputChangeQuietMs = quietMs;
        m_outputChangeMaxWaitMs = maxWaitMs;
    }
    /// Test-only: receive-chain rebuilds started in this session, re-armed or
    /// not.
    int receiveRebuildAttemptsForTest() const { return m_receiveRebuildAttempts; }
    /// Test-only: receive chains a changed output list re-armed this session.
    int receiveRearmsForTest() const { return m_receiveRearms; }

    /// Test-only: a property of a named element inside a published bin,
    /// serialized by GStreamer; empty when absent.
    QString publishedBinPropertyForTest(const QString &cid,
                                        const QString &elementName,
                                        const QString &property) const;
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
    /// Per-application share audio from a scripted listing: `enumerate` is
    /// called synchronously on every scan with the records whose process is
    /// to be checked; branches are audiotestsrc. `options` sets the retire
    /// policy under test (followsProcess = Windows).
    void setShareAudioSourceForTest(
        std::function<lightning::shareaudio::Enumeration(
            const QList<lightning::shareaudio::Stream> &)> enumerate,
        const lightning::shareaudio::BranchOptions &options);
    void rescanShareAudioSourcesForTest() { rescanShareAudioSources(); }
    void setShareAudioBranchCapForTest(int cap) { m_shareAudioCap = cap; }
    /// A retired branch is NOT sent EOS: it goes on producing, as a source
    /// still mid-push would, so a test can prove nothing is taken out
    /// before its EOS has passed.
    void setShareAudioSuppressRetireEosForTest(bool on)
    {
        m_shareAudioSuppressRetireEosForTest = on;
    }
    bool shareAudioEosSeenForTest(const QString &id) const
    {
        const auto it = m_shareAudioEosSeen.constFind(id);
        return it != m_shareAudioEosSeen.cend() && (*it)->load();
    }
    /// Sends EOS to a retired branch's source now (after a suppressed one).
    void sendShareAudioEosForTest(const QString &id);
    /// Watches `fd` as a branch's PipeWire connection (test sources have
    /// none); the caller owns it.
    void setShareAudioConnectionForTest(const QString &id, int fd)
    {
        m_shareAudioConnections.insert(id, fd);
    }
    /// Branches built and not yet taken out (what the cap counts).
    int shareAudioBranchCountForTest() const { return m_shareAudioBranches; }
    /// `shareappbin*` bins actually in the share bin.
    int shareAudioBranchBinsForTest() const;
    bool shareAudioBranchMutedForTest(const QString &id) const;
    /// The branch index recorded for a stream id; -1 never built or taken
    /// out but remembered as failed, -2 no record at all.
    int shareAudioBranchIndexForTest(const QString &id) const;

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

    /// Test-only: stopped peers' webrtcbins not yet at NULL
    /// (WebrtcRetirer.h).
    int retiringWebrtcForTest() const { return m_retirer.retiringForTest(); }
    /// Test-only: webrtcbins set to NULL while ICE gathering was still
    /// running, which is the window GitHub #3's abort needs.
    int teardownsWhileGatheringForTest() const
    {
        return m_retirer.whileGatheringForTest();
    }
    /// Test-only: webrtcbins stopped because the bound ran out.
    int retireBoundExpiriesForTest() const
    {
        return m_retirer.boundExpiredForTest();
    }
    /// Test-only: teardowns that found gathering running and waited for it.
    int retiresThatWaitedOnGatheringForTest() const
    {
        return m_retirer.waitedOnGatheringForTest();
    }
    void setRetireBoundForTest(int ms) { m_retirer.setBoundForTest(ms); }

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
    /// Microphone noise suppression (GitHub #20): which ONE suppressor runs on
    /// the published microphone. Applied live to a running chain (see
    /// calls/noise/MicProcessing.h) and to every chain built afterwards. Not
    /// reset by stop(): it is a user setting, like the gain.
    void setNoiseSuppressionMode(calls::noise::Mode mode);
    /// The selected neural mode failed (it could not start, or stopped
    /// mid-call): build it again in the running microphone. webrtcdsp keeps
    /// suppressing until the new attempt reports. A no-op for Off and WebRTC.
    /// Explicit, never part of applying the setting: a backend that fails
    /// every time must not be rebuilt on every mute toggle.
    void retryNoiseSuppression();
    calls::noise::Mode noiseSuppressionMode() const
    {
        return calls::noise::Mode(m_noiseMode.load());
    }
    /// The selected neural backend could not start and webrtcdsp's own
    /// suppressor runs in its place. Valid when noiseSuppressionStatus()
    /// reports ok == false; cleared by a mode change.
    bool noiseSuppressionFellBackToWebrtc() const
    {
        return m_noiseFellBackToWebrtc;
    }
    /// Test-only: the mode the last denoiser report made webrtcdsp follow
    /// ("" when none yet). In test-source mode webrtcbin parks the stream
    /// (no answer to its offer), so the idle probe that replaces webrtcdsp
    /// never fires and its property cannot show the decision.
    QString lastDspFollowUpForTest() const { return m_lastDspFollowUp; }
    /// The voice-processing stage of the microphone chain for `mode`, for this
    /// runtime: the call's publish chain and the Settings microphone test
    /// both build from it, so the test runs the selected suppressor too.
    static QString voiceProcessingDescription(calls::noise::Mode mode);
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
    /// Microphone RTP packets whose timestamps were ahead of the pipeline
    /// clock and were held to it (see CaptureClock.h). Out of line: the type is
    /// incomplete here.
    quint64 micBuffersHeldToClock() const;
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
    /// One peer connection's ICE transport, for this session only (stale
    /// webrtcbins are filtered by generation). `target` is a Target;
    /// `state` is a closed set: "connected" (CONNECTED or COMPLETED),
    /// "disconnected" or "failed". Other ICE states are not reported.
    void transportStateChanged(int target, const QString &state);
    /// Terminal failure. `category` is safe to log; SDP never is.
    void failed(const QString &category);
    /// One published track cannot carry media; the call itself is fine.
    /// Deliberately not `failed()`, which ends the call. See
    /// handlePublishError() for when it is raised, and
    /// checkCameraFirstFrame() for `camera_no_frames` (a camera whose capture
    /// delivered nothing within kCameraFirstFrameTimeoutMs).
    void publishFailed(const QString &cid, const QString &category);
    /// A received track's output failed (the sound server went away) and
    /// could not be rebuilt: true once when recovery is given up, false once
    /// a later rebuild works again. The call carries on either way.
    void remotePlaybackFailed(bool failed);

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
    /// Every level reading of what is SENT (peak dBFS, loudest channel, after
    /// noise suppression and gain), for the in-call meter: every
    /// kMicLevelIntervalNs while audio flows, nothing while muted (`level`
    /// sits after the mute valve). A level, never audio.
    void localAudioLevel(double peakDb);
    /// The selected noise suppressor took effect on the microphone, or failed
    /// and the microphone passes through unsuppressed (`ok` false). `mode` is
    /// the setting's key ("off", "webrtc", "rnnoise", "deepfilternet").
    void noiseSuppressionStatus(const QString &mode, bool ok);
    /// What the share's sound track is sending, once a second (peak dBFS,
    /// loudest channel; -350 for digital silence). A level, never audio.
    void shareAudioLevel(double peakDb);
    /// What the share's sound track ACTUALLY is, after every change: whether
    /// one is live, whether it captures per application, whether it leaves
    /// Lightning out (false = the output monitor, the echo), the application
    /// keys it is carrying, the ones whose capture failed, and whether the
    /// per-share branch limit kept an application out.
    void shareAudioReport(bool live, bool perApplication, bool excludesUs,
                          const QStringList &carried,
                          const QStringList &failed, bool limitReached);

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
    /// Stops the pipeline now and hands the webrtcbin to m_retirer.
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
    /// An ICE connection state change, marshalled from onPeerStateNotify.
    void handleTransportState(quintptr token, quint64 generation,
                              const QString &state);
    /// A bus ERROR from inside the publishing bin `cid`, on the GUI thread.
    /// Reported only when the bin is still registered (so not a teardown),
    /// its capture delivered zero buffers (never prerolled), and it has not
    /// been reported yet.
    void handlePublishError(const QString &cid);
    /// The camera first-frame watchdog, armed by publishVideo(). Reports
    /// `camera_no_frames` once when the publish `cid` of this engine run is
    /// still registered, unreported, and its capture has delivered zero
    /// buffers, whatever state the bin reached.
    void checkCameraFirstFrame(const QString &cid, quint64 generation);

    /// A peak from `miccapturelevel`, what the device captures BEFORE noise
    /// suppression, on the GUI thread: the dead-microphone judgement and the
    /// periodic level log. Never the in-call meter. Pre-suppression on
    /// purpose: a suppressor that cleans a fan to digital silence has a LIVE
    /// microphone in front of it, and "nobody can hear you" would be false.
    void handleMicLevel(double peakDb);
    /// A peak from `miclevel`, what is SENT (after suppression and gain), on
    /// the GUI thread: the in-call meter (localAudioLevel) only.
    void handleMeterLevel(double peakDb);
    /// A peak from the share-audio track's `level`, on the GUI thread.
    void handleShareAudioLevel(double peakDb);
    /// The share's single loopback capture (`sharesrc`) reported that it
    /// cannot capture, on the GUI thread. Ends the share's sound, never the
    /// call, and says so (`share_audio_failed`).
    void handleShareAudioSourceError();
    /// An ERROR from application branch `index` (`shareapp<index>`), on the
    /// GUI thread: retired, never retried, reported in shareAudioReport.
    void handleShareAudioBranchError(int index);
    /// lightningdenoise reported a mode taking effect, on the GUI thread.
    void handleDenoiseStatus(const QString &requested, const QString &active,
                             bool ok, int latencySamples);
    /// As handleMicLevel(), with the clock supplied for tests.
    void handleMicLevelAt(double peakDb, qint64 nowMs);
    /// Forgets any previous silence judgement; called when the audio bin is
    /// built.
    void resetMicLevelState();
    bool microphoneSilentForTest() const { return m_micSilentAnnounced; }
    bool microphoneQuietForTest() const { return m_micQuietAnnounced; }
    double micPeakDbForTest() const { return m_micPeakDb; }
    /// Test-only: pretend the capture device has this many channels, so the
    /// real multi-input description is parsed.
    void setDeviceChannelsForTest(int channels) { m_testDeviceChannels = channels; }
    /// The audio description publishAudio() last built (element names, ssrc,
    /// gain; no user content), so tests check what production composed.
    QString lastAudioDescriptionForTest() const { return m_lastAudioDescription; }

public:
    /// Peak dBFS at or below which a capture is DEAD: digital silence or the
    /// converter's own floor. `level` reports exact zero as -350 and 16-bit
    /// audio floors near -96; a live microphone in a quiet room still peaks
    /// at -60..-78 (measured, 2026-10-04 call log), so a ceiling of -60 fired
    /// four times on a user who was merely pausing between sentences.
    static constexpr double kMicSilenceCeilingDb = -85.0;
    /// How long the dead ceiling must hold before it is reported; a
    /// conversational pause is not a diagnosis.
    static constexpr qint64 kMicSilenceWindowMs = 10000;
    /// A live but very quiet capture (gain far too low, wrong input): peaks
    /// at or below this for the much longer window below. Never reported on
    /// the short window, because speech pauses sit here.
    static constexpr double kMicQuietCeilingDb = -60.0;
    static constexpr qint64 kMicQuietWindowMs = 60000;

    /// Pure: carries the "silent since" mark across one report. -1 (not 0,
    /// which is a legal instant) means audible. Returns the new mark.
    static qint64 micSilenceSince(double peakDb, qint64 silentSinceMs,
                                  qint64 nowMs);
    /// Pure: has a mark aged past the window?
    static bool micSilenceReached(qint64 silentSinceMs, qint64 nowMs);
    /// As micSilenceSince()/micSilenceReached() for the quiet tier.
    static qint64 micQuietSince(double peakDb, qint64 quietSinceMs,
                                qint64 nowMs);
    static bool micQuietReached(qint64 quietSinceMs, qint64 nowMs);

private:
    /// stop() with `endOfCall`; start() without, keeping the media keys and
    /// resetting only the per-session routing.
    void teardown(bool endOfCall);
    /// Section routing and the injected-frame trailer: per SFU session.
    void clearSessionRouting();
    bool tokenIsLive(quintptr token, quint64 generation,
                     Target *target = nullptr) const;
    /// Install the ENCRYPT probe on one outgoing pad. With `gate`, the probe
    /// does nothing once the gate is closed; see ProbeGate.
    void installEncryptProbe(GstPad *pad, bool video,
                             std::shared_ptr<ProbeGate> gate = {});
    /// A portal camera started off the GUI thread (see publishVideo()): closes
    /// its gate, and when its start is still in progress, or `force`, detaches
    /// the bin from the publisher now and finishes it (NULL, then its
    /// descriptor closed) on a pool thread, so this thread never changes the
    /// state of its pipewiresrc: a pipewiresrc whose PipeWire link never
    /// negotiated blocks EVERY state change in pw_thread_loop_timed_wait (30 s,
    /// measured on Debian 12 at hang-up, after the start had returned).
    /// Returns true when it took the bin over. `bin` may be null (gate only).
    bool detachPendingStart(const QString &cid, GstElement *bin,
                            bool force = false);
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
    /// The output the chosen speaker resolved to, cached so the streaming
    /// thread never enumerates devices. Guarded by m_deviceMutex. Empty (no
    /// binding) until the worker resolve lands: a track arriving in that
    /// window plays on the default sink.
    OutputSink m_resolvedSpeaker;
    quint64 m_speakerSeq = 0;
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
    /// Same for kMicQuietCeilingDb (the long window).
    qint64 m_micQuietSinceMs = -1;
    bool m_micSilentAnnounced = false;
    /// The log-only very-quiet tier; never raises the badge.
    bool m_micQuietAnnounced = false;
    /// Last captured (pre-suppression) peak, and when the level was last
    /// logged.
    double m_micPeakDb = 0;
    /// Last SENT (post-suppression) peak, for the same log line; -350 until
    /// a reading arrives.
    double m_micSentPeakDb = -350.0;
    qint64 m_micLastLogMs = 0;
    QString m_lastAudioDescription;
    int m_testDeviceChannels = 0;
    /// Own microphone gain in percent, 0..200. Atomic because the send chain
    /// may be rebuilt on a streaming thread and must start at the user's
    /// level.
    std::atomic<int> m_microphoneGain{100};
    /// calls::noise::Mode; read when a chain is (re)built.
    std::atomic<int> m_noiseMode{int(calls::noise::kDefaultMode)};
    bool m_noiseFellBackToWebrtc = false;
    QString m_lastDspFollowUp;
    /// Published tracks by client-chosen id, so unpublish can find them.
    QHash<QString, GstElement *> m_publishedBins;
    /// PipeWire remote descriptors owned by publishing bins, closed on
    /// teardown.
    QHash<QString, int> m_publishedFds;
    /// Portal cameras whose start was handed to a pool thread: the gate of
    /// their engine-touching callbacks, and whether the start is still in
    /// progress (cleared by the pool thread). See detachPendingStart().
    struct AsyncStart {
        std::shared_ptr<ProbeGate> gate;
        std::shared_ptr<std::atomic<bool>> pending;
    };
    QHash<QString, AsyncStart> m_asyncStarts;
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
    /// The current microphone's clock hold; its probe keeps its own
    /// reference. See micBuffersHeldToClock().
    std::shared_ptr<lightning::calls::CaptureClockHold> m_micClockHold;
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
        /// The SDP mid of the pad's transceiver; see retireReceiveBins().
        QString transceiverMid;
        /// The track key the bin's volume and video route are named for.
        QString trackKey;
        /// Set while the bin's output has failed: a probe dropping what the
        /// pad carries, so the failure cannot travel back into webrtcbin.
        unsigned long isolateProbe = 0; // a gulong probe id
        /// Rebuilds in the current run of failures, and when the last began.
        int rebuilds = 0;
        /// The Pulse client name a rebuild uses, when the output that failed
        /// was a pulsesink; see rebuildReceiveBin().
        QString rebuildClientName = QString();
        /// A rebuild job is out for this entry. The entry stays in the map
        /// meanwhile, so a retire in that window is seen; see
        /// finishReceiveRebuild().
        bool rebuilding = false;
        /// The level the user had set on this track's volume element, kept
        /// across rebuild attempts; -1 when unknown.
        double level = -1.0;
        qint64 lastRebuildMs = 0;
        /// Every rebuild of the run failed and the engine stopped trying.
        /// Only a changed output list starts it again; see
        /// notifyAudioOutputsChanged().
        bool givenUp = false;
        /// The output list changed while this chain was still retrying: when
        /// its own budget runs out it gets one more attempt instead of giving
        /// up (the change may have landed during an attempt already failing).
        bool rearmOnGiveUp = false;
        /// The current attempt was granted by a changed output list.
        bool rearmed = false;
    };
    QHash<GstPad *, ReceiveBin> m_receiveBins;
    /// Retire the receive bins fed by these transceivers, except the one on
    /// `keep`. Any thread. webrtcbin never removes a src pad, so this, not
    /// pad-removed, is how a track LiveKit retires stops playing. Returns how
    /// many bins went.
    int retireReceiveBins(const QSet<QString> &transceiverMids, GstPad *keep,
                          const char *reason);
    /// Unparent and stop one receive bin off the streaming thread.
    /// `announce` emits remoteTrackRemoved (not for a rebuild).
    void teardownReceiveBin(const ReceiveBin &entry, bool announce = true);
    /// Build a receive chain for `srcPad` and add, probe, sync and link it.
    /// `*binOut` is the bin once it is in the pipeline, even if linking then
    /// failed. Any thread that is not the pad's streaming thread mid-push.
    bool attachReceiveChain(GstElement *pipeline, GstPad *srcPad,
                            const QString &streamId, const QString &trackKey,
                            const QString &mediaKind, GstElement **binOut,
                            QString *why,
                            const QString &pulseClientName = QString());
    /// The engine-side half of attachReceiveChain: build, add, probes, volume.
    /// Quick; never waits on a device.
    bool prepareReceiveChain(GstElement *pipeline, const QString &streamId,
                             const QString &trackKey, const QString &mediaKind,
                             GstElement **binOut, QString *why,
                             const QString &pulseClientName = QString());
    /// The other half: start the bin and link it. May block on the device;
    /// touches no engine state.
    static bool startReceiveChain(GstPad *srcPad, GstElement *bin,
                                  QString *why);
    /// GUI thread: a rebuild job's verdict. See rebuildReceiveBin().
    void finishReceiveRebuild(GstPad *pad, quint64 generation,
                              GstElement *bin, bool ok, const QString &why,
                              double level, const QString &outputName);
    /// The client name of the fresh Pulse connection after a drop, or empty.
    /// Guarded by m_receiveBinMutex.
    QString m_freshPulseClient;
    std::atomic<int> m_rebuildJobDelayMs{0};
    /// True for the current session; a rebuild job holds a copy and stops
    /// its own bin once this goes false (teardown sets it and replaces it).
    std::shared_ptr<std::atomic<bool>> m_sessionLive =
        std::make_shared<std::atomic<bool>>(true);
    /// Bumped when a run of output failures begins; names the fresh Pulse
    /// connection the rebuilds of that run share.
    std::atomic<int> m_receiveSinkEpoch{0};
    /// GUI thread: rebuild an isolated bin after a backoff, bounded.
    void scheduleReceiveRebuild(GstPad *pad, quint64 generation);
    void rebuildReceiveBin(GstPad *pad, quint64 generation);
    /// A rebuild job on a GStreamer pool thread; see rebuildReceiveBin().
    static void runRebuildJob(GstElement *pipeline, void *data);
    static constexpr int kMaxReceiveRebuilds = 5;
    /// GUI thread: the debounced half of notifyAudioOutputsChanged().
    void rearmReceiveChains();
    /// Trailing-edge debounce for output-list changes; single shot.
    QTimer m_outputChangeTimer;
    /// When the current burst of output changes began; -1 when none is open.
    qint64 m_outputChangeBurstStartMs = -1;
    /// What the last change in the burst said about outputs existing.
    bool m_outputChangeAnyOutput = true;
    int m_outputChangeQuietMs = 1000;
    int m_outputChangeMaxWaitMs = 5000;
    int m_receiveRebuildAttempts = 0;
    int m_receiveRearms = 0;
    void restartMicrophone(const QString &cid, quint64 generation);
    static constexpr int kMaxMicRestarts = 5;
    QString m_micCid;
    QString m_testMicSource;
    QString m_testVideoSource;
    bool m_testSelfView = false;
    int m_micRestarts = 0;
    qint64 m_micLastRestartMs = 0;
    bool m_micRestartPending = false;
    bool m_micFailureReported = false;
    int m_micRestartBaseDelayMs = 500;
    int m_cameraFirstFrameTimeoutMs = kCameraFirstFrameTimeoutMs;
    int m_portalCameraFirstFrameTimeoutMs = kPortalCameraFirstFrameTimeoutMs;
    int m_rebuildBaseDelayMs = 500;
    bool m_failReceiveRebuilds = false;
    /// remotePlaybackFailed(true) was emitted and not yet withdrawn.
    bool m_playbackFailureAnnounced = false;

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
        /// " name=value" for each loss-shaped counter this GStreamer reports.
        QString extra;
    };
private:
    void armStatsTrace();
    void requestStats();
    static void onStatsReady(GstPromise *promise, void *userData);
    void logStats(const QList<RtpStat> &stats);
    QTimer m_statsTimer;
    int m_statsIntervalMs = -1;   // -1: environment not read yet
    QHash<quint32, QPair<qint64, quint64>> m_lastRtpBytes; // ssrc -> (ms, bytes)
    QHash<quint32, qint64> m_lastRtpLost;                   // ssrc|dir -> lost

    // Always-on audio health (2026-10-04 "call audio pops" TODO): every 5 s,
    // one compact line per inbound audio track plus the receive-side
    // jitterbuffer counters, so a pop can be told apart as network loss
    // (packetsLost / jitterbuffer lost), a late packet (jitterbuffer late),
    // our own receive queue leaking, or the decoder concealing. Numbers only.
    // LIGHTNING_CALL_STATS_TRACE=off silences it; any other value keeps the
    // full per-SSRC trace on top of it.
public:
    struct RecvHealth {
        QString streamId;
        /// Opus decoder input: GAP events and RTP "packet lost" events, each
        /// of which makes opusdec conceal a frame.
        std::atomic<quint64> gapEvents{0};
        std::atomic<quint64> lostEvents{0};
        /// The leaky receive queue was full: a packet was dropped.
        std::atomic<quint64> queueOverruns{0};
        // GUI-thread only: totals at the previous report.
        quint64 lastGap = 0;
        quint64 lastLost = 0;
        quint64 lastOverruns = 0;
    };
private:
    /// Wires the counters of one audio receive bin; no-op for video.
    void instrumentReceiveBin(GstElement *bin, const QString &trackKey,
                              const QString &streamId);
    void logReceiveHealth();
    mutable QMutex m_recvHealthMutex;
    QHash<QString, std::weak_ptr<RecvHealth>> m_recvHealth;
    /// Jitterbuffer element name -> pushed, lost, late, duplicates at the
    /// previous report.
    QHash<QString, std::array<quint64, 4>> m_lastJitterbuffer;
    /// True when LIGHTNING_CALL_STATS_TRACE asked for the full trace.
    bool m_statsFull = false;
    /// Report counters for the once-a-minute heartbeat of the compact lines.
    int m_healthReports = 0;
    int m_statsReports = 0;

    // Per-application share audio; see ShareAudioSources.h. A plain poll, not
    // a bus watch (which would need a GLib main loop). Only additions change
    // the topology: departing apps retire their branch via
    // `pipewiresrc on-disconnect=eos` where the plugin has it (else the live
    // mixer stops waiting on the silent pad), and a deselected app's branch
    // is muted, so no pad is unlinked on a live pipeline.
    void rescanShareAudioSources();
    /// Adds a branch for `stream` to the running share bin; false when it
    /// could not be built or linked.
    bool addShareAudioBranch(GstElement *bin, GstElement *mixer,
                             const lightning::shareaudio::Stream &stream);
    /// Applies one enumeration pass: plan (shareaudio::planScan), retire,
    /// mute, add, report.
    void applyShareAudioScan(const lightning::shareaudio::Enumeration &e);
    /// Takes a finished branch out of the share: an IDLE probe unlinks it and
    /// releases its mixer pad, and gst_element_call_async sets it to NULL and
    /// removes it. Never blocks the calling (GUI) thread.
    void takeOutShareAudioBranch(GstElement *bin, GstElement *mixer, int index);
    void emitShareAudioReport();
    /// Clears the per-application bookkeeping; the bin is the caller's.
    void resetShareAudioState();

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

    QTimer m_shareAudioScanTimer;
    QString m_shareAudioCid;
    /// The cid of the share-audio bin whatever its kind, so a `sharesrc`
    /// error can be told apart from a stale one.
    QString m_shareAudioAnyCid;
    /// Stream::id() -> what was built for it (see planScan()).
    QHash<QString, lightning::shareaudio::BranchRecord> m_shareAudioRecords;
    /// Stream::id() -> the scan that retired it; taken out once its EOS has
    /// passed (m_shareAudioEosSeen) or it errored.
    QHash<QString, int> m_shareAudioRetiredAt;
    /// Stream::id() -> set by an event probe when EOS passes the branch's src.
    QHash<QString, std::shared_ptr<std::atomic<bool>>> m_shareAudioEosSeen;
    /// Stream::id() -> the branch's own PipeWire connection (fd), watched each
    /// scan for the daemon hanging up. Not owned: closed with the element.
    QHash<QString, int> m_shareAudioConnections;
    /// Stream::id() -> the scan a failed branch was taken out on (cool-down).
    QHash<QString, int> m_shareAudioFailedAt;
    QElapsedTimer m_shareAudioScanStarted;
    bool m_shareAudioHangLogged = false;
    bool m_shareAudioSuppressRetireEosForTest = false;
    lightning::shareaudio::Selection m_shareAudioSelection;
    lightning::shareaudio::BranchOptions m_shareAudioOptions;
    /// What shareAudioReport last said.
    QStringList m_shareAudioCarried;
    QStringList m_shareAudioFailedKeys;
    bool m_shareAudioLimitReached = false;
    bool m_shareAudioPerApplication = false;
    bool m_shareAudioExcludesUs = false;
    int m_shareAudioBranches = 0;        // built and not yet taken out
    int m_shareAudioCap = 24;            // bounds a share that outlives many apps
    int m_shareAudioNextIndex = 0;       // element names; never reused
    int m_shareAudioScans = 0;           // scans since the share started
    int m_shareAudioLevelReports = 0;    // throttles the level log line
    bool m_shareAudioSourceErrorReported = false;
    /// An enumeration runs on a worker; one at a time, and its answer is
    /// dropped if the share it was for has gone (generation).
    bool m_shareAudioScanInFlight = false;
    quint64 m_shareAudioGeneration = 0;
    /// Test seam: a scripted listing instead of PipeWire/WASAPI, enumerated
    /// synchronously, with audiotestsrc branches.
    std::function<lightning::shareaudio::Enumeration(
        const QList<lightning::shareaudio::Stream> &)>
        m_shareAudioEnumerateForTest;
    lightning::shareaudio::BranchOptions m_shareAudioOptionsForTest;

    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;

    /// Stopped peers' webrtcbins, stopped once their ICE gathering has ended.
    lightning::webrtc::Retirer m_retirer;
};
