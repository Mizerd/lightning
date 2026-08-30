// LiveKit SFU media engine (MatrixRTC phase 2): GStreamer webrtcbin against
// an SFU, rather than against one peer.
//
// This is a SEPARATE class from GstCallMediaBackend on purpose. The 1:1 lane
// is one peer connection carrying one bidirectional Opus stream; LiveKit is
// architecturally different in three ways that would have turned that class
// into a mess of conditionals:
//
//   * TWO peer connections. The client offers on PUBLISHER (its own tracks);
//     the server offers on SUBSCRIBER (everyone else's). They negotiate
//     independently and must never be confused — attaching a remote
//     description to the wrong one wires audio the wrong way round.
//   * N remote streams, arriving and leaving at any time, each of which has
//     to be routed to a participant the UI knows about.
//   * Publishing is DECLARED before it is negotiated (AddTrack, then the
//     offer), so track identity is client-chosen up front.
//
// Threading follows the existing engine exactly: GStreamer calls back on its
// own threads, every callback marshals to the GUI thread through a
// process-global alive registry, and every handler re-checks the live
// session generation. A queued callback from a closed session must never be
// attributed to the next one.
//
// Privacy: SDP and ICE carry host IPs; nothing here logs either. Failures
// surface as coarse categories. ICE servers come only from the SFU's own
// JoinResponse (they are that SFU's TURN servers, which the user's
// homeserver chose to advertise) — no third-party STUN fallback.
#pragma once

#include <atomic>
#include <memory>

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantList>

typedef struct _GstElement GstElement;
typedef struct _GstPromise GstPromise;
typedef struct _GstPad GstPad;

class CallFrameCryptor;
class SfuVideoRouter;

class SfuMediaEngine : public QObject
{
    Q_OBJECT

public:
    /// Which peer connection. LiveKit's own vocabulary, kept verbatim so the
    /// mapping to the wire is one-to-one and unmistakable.
    enum class Target { Publisher, Subscriber };

    /// The declared ceiling for each video source, matching the capsfilters
    /// the publish pipelines use. Declared to the SFU in AddTrack: a video
    /// track with no size and no layer leaves it to infer the shape of the
    /// track, and it infers three-layer simulcast.
    // The DEFAULT share ceiling. Still constants because they are what the
    // SFU is told at AddTrack time when nothing has chosen otherwise; the
    // live values are the two members below, set from settings before a
    // share publishes.
    static constexpr int kScreenWidth = 1920;
    static constexpr int kScreenHeight = 1080;

    /// What a screen share may send: a scanline ceiling (720/1080/1440) and
    /// a frame rate (15/30/60).
    ///
    /// BOTH are encoder cost. A share is a 4K capture downscaled on the CPU,
    /// converted, and encoded by four VP8 threads, all competing with
    /// whatever the user is actually sharing — a game, most usefully. These
    /// exist so that cost is the user's choice rather than a constant, which
    /// is what Discord offers and for the same reason.
    void setShareQuality(int maxHeight, int fps);

    /// The share's caps ceiling and encoder stage for a chosen height and
    /// rate. STATIC AND PURE so the arithmetic is testable without a live
    /// peer — every caps defect this lane has had was invisible to the tests
    /// that existed, because the string was only ever built inside a
    /// function that needed one.
    static QString shareLimitsCaps(int maxHeight, int fps);
    /// Convert-and-scale for a share: CPU, or GPU under LIGHTNING_SHARE_GPU.
    static QString shareScaleStage(int maxHeight, bool gpu);
    /// The capsfilter between the capture and the rest. Pins PAR either way;
    /// the GPU form also allows a non-system memory feature through.
    static QString captureEntryFilter(bool gpu);
    /// Whether the GPU share path should be ATTEMPTED. Default is now yes
    /// on platforms that can carry it; `LIGHTNING_SHARE_GPU=0` forces the
    /// CPU path and `=1` forces an attempt even where we would not try.
    /// This is intent only — availability is a separate question, because
    /// wanting the path and having the elements are different failures and
    /// the log has to be able to tell them apart.
    static bool shareGpuScalingRequested();
    /// The first required GL element that is MISSING, or empty if the GPU
    /// share chain can be built at all. Named rather than boolean so the
    /// fallback log says WHICH element was absent — a packaged build that
    /// forgot one plugin is the likeliest way this path dies, and "GPU
    /// unavailable" would send the next person hunting the driver.
    static QString missingGpuShareElement();
    /// The first name in `names` with no registered factory, or empty.
    ///
    /// SPLIT OUT SO IT CAN ACTUALLY BE TESTED. With the element list baked
    /// in, the dev shell has every GL element and the "something is
    /// missing" branch is unreachable from a test — an assertion over it
    /// passes on code that returns anything at all, which is precisely what
    /// a mutation check caught here. Taking the names as a parameter lets a
    /// test pass one that cannot exist and see the real answer.
    static QString firstMissingElement(const QList<QByteArray> &names);
    static QString shareEncoderStage(int maxHeight, int fps);
    int shareMaxHeight() const { return m_shareMaxHeight; }
    int shareFps() const { return m_shareFps; }
    static constexpr int kCameraWidth = 1280;
    static constexpr int kCameraHeight = 720;

    /// One-time probe: GStreamer initializes and every element the SFU
    /// pipelines need resolves. `whyNot` receives a short, safe reason.
    static bool runtimeAvailable(QString *whyNot = nullptr);

    explicit SfuMediaEngine(QObject *parent = nullptr);
    ~SfuMediaEngine() override;

    /// Headless mode: synthetic sources and fakesinks instead of real
    /// devices, so CI can drive a genuine handshake with no microphone,
    /// camera or display server.
    void setTestSourceMode(bool on) { m_testSources = on; }
    bool testSourceMode() const { return m_testSources; }

    /// Start a session. Tears down any previous one first: one SFU call at a
    /// time, and the generation bump invalidates every in-flight callback.
    void start();
    /// Tear everything down and release every device.
    void stop();
    bool active() const { return m_active; }

    /// Where received video frames go. Set once by the owner; the engine
    /// only ever reads it, and only to ask "is anyone watching this
    /// stream?" before paying for a frame copy.
    ///
    /// Defined in the .cpp, NOT inline: m_videoRouter is a
    /// QPointer<SfuVideoRouter> and that class is only forward-declared
    /// here. Assigning or dereferencing such a QPointer instantiates
    /// QPointer::data()'s static_cast, which needs the type COMPLETE — and
    /// an inline body is compiled in every translation unit that includes
    /// this header, none of which need to know about the router. Same trap
    /// that broke the Debian build in 0.7.4 (CLAUDE.md §16).
    void setVideoRouter(SfuVideoRouter *router);
    /// Read from a GStreamer streaming thread as well as the GUI thread. The
    /// router itself is internally locked; this only loads the pointer.
    SfuVideoRouter *videoRouter() const;

    /// ICE servers from the SFU's JoinResponse. Applied to both peer
    /// connections; credentials are engine-only and never logged.
    void setIceServers(const QVariantList &servers);

    /// Publish the microphone. `cid` is the client-chosen track id that was
    /// declared to the SFU with AddTrack; it must match or the SFU cannot
    /// map the negotiated media section to the track it authorized.
    void publishAudio(const QString &cid);

    /// The other half of a screen share: what the computer is playing.
    ///
    /// A separate track with its own cid, so it publishes, mutes and retires
    /// independently of both the microphone and the share's video — which is
    /// what a viewer expects, since stopping a share must silence it while
    /// the microphone keeps going.
    void publishShareAudio(const QString &cid);

    /// Whether this build, on this machine, can capture what is playing.
    ///
    /// Platform AND element AND property: the answer is not a compile-time
    /// constant, because which capture plugin a package ships is a packaging
    /// fact. The UI asks before offering the option, so a user is never
    /// offered a switch that cannot work.
    static bool shareAudioAvailable();
    /// Publish the camera, or a screen share when `screenShare` is true.
    /// `nodeId` is the PipeWire node a desktop portal handed us; -1 means
    /// the camera. `pipewireFd` is the descriptor from the portal's
    /// OpenPipeWireRemote and OWNERSHIP PASSES HERE — pipewiresrc dups it, so
    /// this closes the copy it was given once the element exists (including
    /// on every failure path). A node id without a remote fd names a node in
    /// the caller's own default PipeWire remote, where a portal node need not
    /// be visible at all: the pipeline then plays and produces no frames,
    /// which is a black screen share that reports success.
    /// `windowHandle` is a Windows HWND, widened, and non-zero ONLY when the
    /// user picked a single window rather than a display. It is the third
    /// mutually exclusive way of saying "capture this": a portal node on
    /// Linux, a display index on Windows/macOS, a window here.
    /// `captureRect` is the FOURTH, and it exists only for the Linux
    /// no-portal fallback: a rectangle of the X11 root window, in ROOT
    /// PIXELS, chosen in Lightning's own picker because there was no portal
    /// to draw one. Invalid everywhere else, which is how the branches below
    /// tell the fallback from the portal without a platform flag.
    void publishVideo(const QString &cid, bool screenShare, int nodeId,
                      int pipewireFd = -1, quint64 windowHandle = 0,
                      const QRect &captureRect = {});
    /// The VIDEO publish pipeline, as a gst_parse description.
    ///
    /// Exposed so the SCREEN-SHARE shape — which adds a `tee` and a self-view
    /// branch and is therefore not the shape test-source mode builds — can be
    /// parsed by a test. A typo there is a screen share that dies on its first
    /// frame, which is exactly the class of failure this file has already had.
    static QString videoPipelineDescription(const QString &source,
                                            const QString &rateStage,
                                            const QString &limits,
                                            const QString &encoder,
                                            const QString &selfView,
                                            quint32 ssrc,
                                            const QString &scaleStage,
                                            const QString &entryFilter);
    /// The element that turns the capture's own cadence into the pinned
    /// 30 fps the encoder and every WebRTC receiver expect.
    ///
    /// NOT the same element for both sources, and the difference is measured
    /// rather than stylistic — see the definition. A camera already produces
    /// on a clock; a desktop capture produces ON DAMAGE, and `videorate`
    /// cannot emit its first output frame until a SECOND input buffer has
    /// arrived — which, on a screen nobody is touching, is however long the
    /// user waits.
    static QString videoRateStage(bool screenShare);
    /// The name of the `volume` element in the receive bin carrying one
    /// stream. Public and static so the bin that CREATES it and the lookup
    /// that FINDS it share one derivation — they did not, and per-participant
    /// volume was a no-op for it.
    static QString outputVolumeElementName(const QString &streamId);
    /// The identity a receive bin's volume element is named for. Per TRACK,
    /// because one participant can publish both a microphone and a screen
    /// share's audio, and two bins sharing a name make a volume change land
    /// on whichever GStreamer finds first.
    static QString volumeKeyFor(const QString &streamId,
                                const QString &trackKey);
    /// Set the level on ONE track. `setParticipantVolume` is this with the
    /// participant's microphone track resolved for it.
    void setTrackVolume(const QString &streamId, const QString &trackKey,
                        int percent);

    /// The audio factor, in percent, for a volume the USER set on a 0-200
    /// slider.
    ///
    /// Two ranges, because one linear scale could not serve both jobs. 0-100
    /// is ordinary attenuation and must stay 1:1 — that half is how someone
    /// turns a loud person down, and a curve there would make every setting
    /// mean something other than it says. 100-200 is BOOST, and it expands to
    /// 100-1000: a straight 0-200 slider tops out at +6 dB, which against a
    /// sender already running AGC reads as "above 100% barely any
    /// difference", while 1000% is where the desired loudness actually lives.
    ///
    /// 1000 is not arbitrary — it is the GStreamer `volume` element's own
    /// factor ceiling (its range is 0-10). Past it the element clamps
    /// silently.
    ///
    /// Stored and displayed values are always the USER scale; this is applied
    /// only where the number meets the audio, so nothing on disk or on screen
    /// has to know about the curve.
    static int audioFactorPercent(int userPercent);
    /// A distinct, non-zero SSRC for one published track. See the
    /// definition: without an explicit one the offer carries no `a=ssrc`,
    /// and then no `a=msid` either.
    static quint32 nextPublishSsrc();
    /// Give one publisher pad the msid the SFU will use to recognise the
    /// track we declared. See the definition for why an offer without it is
    /// a call that connects and carries nothing.
    static void applyPublisherMsid(GstPad *sinkPad, const QString &cid);
    /// The TRACK SID (`TR_…`) out of one SDP `a=msid:` value — the id that
    /// names one track identically on both ends. See the definition: a
    /// media-section `mid` cannot serve here, because the SFU's TrackInfo mid
    /// belongs to the PUBLISHER's connection, not ours.
    static QString trackSidFromMsid(const QString &msid);
    /// The sending participant's id out of one SDP `a=msid:` value.
    ///
    /// Exposed as a static because it is the single thing that decides
    /// whether a received track can be attributed to anyone at all — the
    /// media key and the video sink are BOTH keyed on what this returns — and
    /// testing it otherwise would need a live SFU. Same reason
    /// screenShareSource() is exposed.
    static QString participantIdFromMsid(const QString &msid);
    /// The router key under which the LOCAL CAMERA's own frames are
    /// delivered. Our camera is published rather than received, so this
    /// self-view branch is the only local video that exists.
    static QString localCameraStreamId();
    /// The router key under which the LOCAL screen share's own frames are
    /// delivered. Discord shows the sharer their own share; without a
    /// self-view the only way to find out whether a share is actually
    /// carrying pixels is to ask the person on the other end.
    static QString localScreenStreamId();
    /// The capture-source fragment for a screen share, exposed so the one
    /// thing that decides whether a share carries pixels at all is testable
    /// without a live PipeWire node: a portal node id is only reachable
    /// through the portal's OWN remote, and `pipewiresrc path=` alone
    /// resolves it against the caller's default remote, where it need not
    /// exist. That pipeline plays, reports no error, and emits no buffer.
    /// On Linux `nodeId` is the PipeWire node the xdg portal handed us and
    /// `pipewireFd` its remote. On Windows and macOS there is no portal:
    /// `nodeId` is a MONITOR INDEX and `pipewireFd` is unused (-1). One
    /// signature for all three so the call controller's plumbing is
    /// platform-blind — the platform difference belongs in the pipeline
    /// description, which is the only thing that actually differs.
    /// ...and `windowHandle`, non-zero only on Windows and only when a
    /// single window was chosen, which routes to Lightning's own capture
    /// element (WindowCaptureSrc.h) because no shippable GStreamer element
    /// can take a window.
    /// ...and `captureRect`, valid ONLY on the Linux no-portal fallback,
    /// where it is a rectangle of the X11 root window and routes to
    /// `ximagesrc`. A portal node and a root rectangle are mutually
    /// exclusive by construction: the fallback only ever runs because there
    /// was no portal to grant a node.
    static QString screenShareSource(int nodeId, int pipewireFd,
                                     quint64 windowHandle = 0,
                                     const QRect &captureRect = {});
    /// The element the Linux no-portal fallback captures a display with.
    ///
    /// Named in ONE place, like `wincap::windowCaptureSrcName()`, so the
    /// capability probe, the pipeline description and the tests cannot
    /// disagree about the spelling — a probe for an element the pipeline
    /// does not name proves nothing, which is the shape of the sctp defect
    /// (§16).
    ///
    /// INLINE on purpose: `SfuCallController::linuxShareRefusal()` names this
    /// element in a user-facing sentence and is compiled in builds where
    /// SfuMediaEngine.cpp is NOT, so an out-of-line definition here would be
    /// an undefined symbol in every no-WebRTC target. Same hazard as the
    /// `wincap::available()` accessor in SfuCallController.h.
    static constexpr const char *x11ScreenCaptureElementName()
    {
        return "ximagesrc";
    }
    /// Whether `name` is in the RUNNING GStreamer registry.
    ///
    /// Deliberately NOT part of the engine's required-element list: a machine
    /// with no X11 capture plugin must still be able to make ordinary calls,
    /// so this is an optional capability probed at the moment it is offered.
    /// Answering it honestly is what stops the fallback picker appearing on a
    /// system where the pipeline could only fail at PLAYING.
    static bool elementAvailable(const char *name);
    /// The capture-source fragment for the CAMERA, per platform.
    ///
    /// Exposed for the same reason screenShareSource() is: it decides whether
    /// a camera carries pixels at all, and on Linux the choice of `v4l2src`
    /// over `autovideosrc` is a MEASURED one (see the definition) that a
    /// future edit must not undo by accident.
    static QString cameraSource();
    /// Stop publishing one track and renegotiate.
    void unpublish(const QString &cid);

    /// A capture that ENDED ITSELF — the shared window was closed, a camera
    /// was unplugged. Distinct from handlePublishError(), which is about a
    /// publish that never started: this one has been delivering frames and
    /// then stops, and if nobody retires it the far end keeps the last
    /// picture forever while the control stays lit. That is the frozen-tile
    /// failure the unpublish round fixed for the STOP button, reachable again
    /// through a path the button never touches.
    void handleCaptureEnded(const QString &cid);

    /// How many sink pads the PUBLISHER webrtcbin currently holds — one per
    /// live outgoing track, and therefore one per m= section it will offer.
    /// Test-only observation point: unpublish() retires a transceiver
    /// asynchronously, and without a way to see the count come back down a
    /// leak here is invisible until it reaches another client's screen.
    /// Returns -1 when there is no publisher at all.
    int publisherTrackSlotsForTest() const;

    /// Test-only: is a bin registered under this cid? Lets a test assert that
    /// a refusal REFUSED, rather than inferring it from the absence of a
    /// crash. "Target absent" and "work done" are different outcomes.
    bool hasPublishedBinForTest(const QString &cid) const
    {
        return m_publishedBins.contains(cid);
    }

    /// The tail of unpublish(), run once the deferred teardown has actually
    /// put the bin at NULL. Public only because that teardown is driven by
    /// GStreamer callbacks that are not members of this class; it is not a
    /// control surface and nothing outside SfuMediaEngine.cpp should call it.
    /// Always arrives on the GUI thread, via marshal().
    void noteTeardownComplete(const QString &cid);

    /// A remote description from the SFU for one peer connection.
    void applyRemoteDescription(Target target, const QString &kind,
                                const QString &sdp);
    /// One remote ICE candidate for one peer connection.
    void applyRemoteCandidate(Target target, const QString &candidateInit);

    /// Real mute: stops publishing, never attenuates (see the 1:1 engine).
    void setMicrophoneMuted(bool muted);
    /// Own microphone gain, 0..200, applied to WHAT OTHERS HEAR.
    ///
    /// A `volume` element in the SEND chain, before the encoder, in the raw
    /// audio domain — never after the frame cryptor, which sees only
    /// ciphertext. 200 is a linear factor of 2.0, which the element accepts;
    /// clipping past unity is the user's own choice, exactly as it is in
    /// Discord, and it is theirs to hear.
    ///
    /// This is NOT mute and must never be used as one. Attenuating to zero
    /// still publishes RTP; `setMicrophoneMuted` stops buffers at the valve
    /// so nothing is produced at all. The two are independent and both apply.
    void setMicrophoneGain(int percent);
    /// Local playback mute for every remote track.
    void setOutputMuted(bool muted);
    /// Local volume for ONE remote participant, 0..200, keyed by that
    /// participant's LiveKit STREAM ID (their `PA_…` sid), which is the name
    /// the receive bin's volume element carries.
    ///
    /// It used to take the SFU `identity` and look up `outvol_<identity>`,
    /// while every receive bin named its element plainly `outvol` — so the
    /// lookup matched nothing and the whole function was a PERMANENT NO-OP.
    /// Nothing noticed because no surface called it.
    ///
    /// Local-only: it changes nothing for anyone else and sends no event.
    void setParticipantVolume(const QString &streamId, int percent);

    // ── Media encryption ──
    //
    // Encryption happens per ENCODED FRAME, between the encoder and the RTP
    // payloader on the way out and after the depayloader on the way in.
    // That is where LiveKit and Element Call do it, so the bytes on the wire
    // have the same shape theirs do; encrypting per RTP PACKET instead would
    // be a different scheme that interoperates with nobody.
    //
    // `encryptionActive()` is what the controller reports as
    // `mediaEncrypted`, so it can never claim encryption it is not doing.

    /// Install our own sending key at `index` and make it current. `rawKey`
    /// is 32 raw bytes; never logged, never copied elsewhere.
    void setOutboundKey(int index, const QByteArray &rawKey);
    /// Install a key received from one sender, for decrypting THAT sender's
    /// media.
    ///
    /// `senderName` is the caller's stable per-DEVICE name, not a LiveKit
    /// sid: a key is addressed to a Matrix device and arrives whenever its
    /// to-device message does, which may be long before the SFU has named
    /// that device's sid. noteParticipantIdentity() joins the two names
    /// afterwards. One ring PER sender, because LiveKit's key index is
    /// per-participant — two senders may legitimately both use index 0 with
    /// different material, and one shared ring would decrypt at most one of
    /// them. livekit-client keeps one decryptor per participant for the same
    /// reason.
    void setInboundKey(const QString &senderName, int index,
                       const QByteArray &rawKey);
    /// Bind the LiveKit stream id a sender publishes under to the stable
    /// name their key ring is stored under, making the two ONE ring.
    ///
    /// A media key is addressed to a Matrix DEVICE and arrives whenever its
    /// to-device message lands; the sid that device publishes under is only
    /// learned from the SFU's participant list. Nothing orders those two, so
    /// the ring is stored under the device name (which is always knowable
    /// from the key itself) and this is what lets an arriving FRAME find it.
    /// Without the binding, every frame from that sender is dropped as
    /// undecryptable for the whole call. Idempotent, and safe to re-run on
    /// every participant update — which is exactly how a binding that could
    /// not be made yet is retried.
    void noteParticipantIdentity(const QString &streamId,
                                 const QString &senderName);
    /// The key ring for one sender, created on first sight. `name` is either
    /// a MatrixRTC participant identity or a LiveKit stream id;
    /// noteParticipantIdentity() makes those two names one ring.
    ///
    /// PUBLIC because the decrypt pad probe resolves it per frame on a
    /// GStreamer streaming thread: which participant owns a media section can
    /// be learned after the pad exists, so a ring captured at install time
    /// froze whichever order the SFU happened to send things in. Internally
    /// locked; safe from any thread.
    std::shared_ptr<CallFrameCryptor> recvCryptorFor(const QString &name);
    /// Whether outgoing frames are actually being encrypted right now.
    bool encryptionActive() const;
    /// Frames that reached the wire, and frames that arrived and decrypted.
    ///
    /// The two numbers that separate "media never flowed" from "media flowed
    /// and was unusable" — indistinguishable from the outside (silence, a
    /// black tile, a mute badge at the far end) and completely different
    /// faults. Counts only; never content. Written from GStreamer streaming
    /// threads, so atomic.
    quint64 framesEncrypted() const { return m_framesEncrypted.load(); }
    quint64 framesDecrypted() const { return m_framesDecrypted.load(); }
    quint64 framesDropped() const { return m_framesDropped.load(); }
    /// Require encryption. With this set and no key installed, frames are
    /// DROPPED rather than sent in the clear — the whole point of the gate.
    void setEncryptionRequired(bool required);
    /// Forget all media keys: they must not outlive the call that used them.
    void clearKeys();

Q_SIGNALS:
    /// A local description to hand the SFU.
    void localDescription(int target, const QString &kind, const QString &sdp);
    /// A local ICE candidate to trickle, already in LiveKit's JSON form.
    void localCandidate(int target, const QString &candidateInit);
    /// A remote track started or stopped rendering.
    /// A remote track came up. `streamId` attributes it to a SENDER (the
    /// LiveKit participant sid) and `mid` to the exact TRACK, which is what
    /// tells a camera from a screen share when one person sends both.
    void remoteTrackAdded(const QString &identity, const QString &mid,
                          const QString &kind);
    void remoteTrackRemoved(const QString &identity, const QString &kind);
    /// Aggregate connection state for the session, as a closed-set string.
    void connectionStateChanged(const QString &state);
    /// Terminal failure. `category` is safe to log; SDP never is.
    void failed(const QString &category);
    /// ONE published track could not carry media, and the CALL IS FINE.
    ///
    /// Deliberately not `failed()`: that ends the call (see
    /// SfuCallController::onEngineFailed), and a camera that cannot negotiate
    /// must not take an otherwise healthy conversation down with it.
    ///
    /// This exists because the failure was previously invisible. `onBusMessage`
    /// logs bus errors and never raises them — for a good reason, since a
    /// pipeline posts errors during ordinary teardown and turning those into
    /// call failures tore down working calls — so a camera whose source could
    /// not negotiate left `cameraOn` true and the button lit for the rest of
    /// the session, with the reason readable only in a log. The narrow rule
    /// that makes this safe is in handlePublishError().
    void publishFailed(const QString &cid, const QString &category);

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

    // GStreamer-thread callbacks. Each carries the EMITTING element's
    // pointer as a session token, checked against the live session before
    // anything is acted on — the engine is one object reused call after
    // call, and a queued event from a closed session must never be
    // attributed to the next one.
    /// ICE / DTLS state transitions on one peer connection, logged.
    ///
    /// The single missing signal that separates "media never negotiated"
    /// from "media negotiated and is unusable": those two produce identical
    /// symptoms (silence, a black tile, a mute badge at the far end) and
    /// have completely different causes.
    static void onPeerStateNotify(GstElement *webrtc, void *paramSpec,
                                  void *userData);
    static void onNegotiationNeeded(GstElement *webrtc, void *userData);
    static void onIceCandidate(GstElement *webrtc, unsigned mlineIndex,
                               char *candidate, void *userData);
    static void onPadAdded(GstElement *webrtc, void *pad, void *userData);
    static void onOfferCreated(GstPromise *promise, void *userData);
    static void onAnswerCreated(GstPromise *promise, void *userData);

public Q_SLOTS:
    void handleLocalDescription(quintptr token, bool offer,
                                const QString &sdp);
    void handleLocalCandidate(quintptr token, int mlineIndex,
                              const QString &candidate);
    void handleFailure(quintptr token, const QString &category);
    /// A GStreamer bus ERROR was posted from inside the publishing bin named
    /// `cid`. Runs on the GUI thread; every discriminator lives here.
    ///
    /// A bus error is NOT by itself a failure — see onBusMessage — so this
    /// reports only the one shape that cannot be anything else:
    ///
    ///   * the bin is STILL REGISTERED as published. unpublish() takes the cid
    ///     out of m_publishedBins before it sets the bin to NULL, and stop()
    ///     clears the bus sync handler before tearing the pipelines down, so
    ///     an ordinary teardown error can never satisfy this;
    ///   * the CAPTURE ITSELF has delivered zero buffers. Everything
    ///     downstream of the rate stage manufactures frames, so this is the
    ///     only counter that separates "never prerolled" from "produced media
    ///     and then hit a transient";
    ///   * and it has not been reported yet, because a failed pipeline posts
    ///     many errors and the user needs one message.
    void handlePublishError(const QString &cid);

private:
    bool tokenIsLive(quintptr token, Target *target = nullptr) const;
    /// Install the ENCRYPT probe on one outgoing pad.
    void installEncryptProbe(GstPad *pad, bool video);
    /// Install the DECRYPT probe on one incoming pad, using the cryptor for
    /// `streamId`. An unknown stream id still gets a cryptor, so a key
    /// arriving later is applied to the right sender.
    void installDecryptProbe(GstPad *pad, bool video, const QString &streamId);
    /// Record media-section index -> stream id.
    ///
    /// Takes the already-extracted map rather than the SDP: GstSDPMessage is
    /// a typedef of an ANONYMOUS struct, so it cannot be forward-declared,
    /// and pulling gst/sdp into this header for one parameter would put
    /// GStreamer on every translation unit that mentions the engine.
    void noteStreamIds(const QHash<int, QString> &byMline,
                       const QHash<int, QString> &midsByMline,
                       const QHash<int, QString> &tracksByMline);

    Peer m_publisher;
    Peer m_subscriber;
    bool m_active = false;
    bool m_testSources = false;
    int m_shareMaxHeight = kScreenHeight;
    int m_shareFps = 30;
    /// Bumped on every start/stop so a late callback is discarded.
    std::atomic<quint64> m_generation{0};
    /// Read from a GStreamer streaming thread in pad-added: a remote track
    /// arriving after the user deafened must come up already silenced,
    /// and marshalling first would let it be briefly audible.
    std::atomic<bool> m_outputMuted{false};
    bool m_microphoneMuted = false;
    /// Own microphone gain as a PERCENTAGE, 0..200. Atomic because the send
    /// chain is built on the GStreamer streaming thread on renegotiation and
    /// must come up already at the user's level — the same reason
    /// m_outputMuted is atomic, and the same hazard: a track that is briefly
    /// at the wrong volume is a track the user hears wrong.
    std::atomic<int> m_microphoneGain{100};
    /// Published tracks by client-chosen id, so unpublish can find them.
    QHash<QString, GstElement *> m_publishedBins;
    /// PipeWire remote descriptors owned by a publishing bin (screen shares
    /// only), closed when that bin is torn down. See publishVideo.
    QHash<QString, int> m_publishedFds;

    /// What one publishing bin's own capture has actually produced, shared
    /// with the GStreamer pad probes that write it.
    ///
    /// Two questions need this and nothing else can answer either. Whether a
    /// publish is DEAD (`captured == 0` — everything past the rate stage
    /// manufactures frames, so no downstream counter can tell), and how long
    /// the rate stage HELD the opening picture (`firstEncodedMs -
    /// firstCaptureMs`, which is the measurement docs/matrixrtc.md asks for
    /// before anything on this publish path is changed again).
    struct PublishProbeState {
        std::atomic<quint64> captured{0};
        /// Monotonic ms since the publish was dispatched; -1 means "not yet".
        std::atomic<qint64> firstCaptureMs{-1};
        std::atomic<qint64> firstEncodedMs{-1};
        /// Written once, before the bin is set playing, and only read after.
        qint64 startedMs = 0;
        bool screenShare = false;
    };
    struct PublishWatch {
        std::shared_ptr<PublishProbeState> state;
        /// One report per publish: a pipeline that has failed posts errors
        /// repeatedly and the user needs one message, not a storm.
        bool reported = false;
    };
    /// GUI thread only. The probes hold their own shared_ptr to the state, so
    /// a probe outliving this map cannot dereference anything freed.
    QHash<QString, PublishWatch> m_publishWatch;
    /// One cryptor for what we send.
    std::unique_ptr<CallFrameCryptor> m_sendCryptor;
    /// One cryptor PER SENDER for what we receive, keyed by LiveKit stream
    /// id. Created on demand — by a key arriving for a sender we have not
    /// seen a track from yet, or by a track arriving from a sender whose key
    /// has not landed yet, in either order.
    ///
    /// Guarded by m_recvMutex: a GStreamer streaming thread reads this from
    /// inside pad-added while the Qt thread may be installing a key.
    QHash<QString, std::shared_ptr<CallFrameCryptor>> m_recvCryptors;
    mutable QMutex m_recvMutex;
    /// Media-section index -> LiveKit stream id, from the subscriber offer's
    /// `msid`. webrtcbin names a received pad `src_<index>`, so the index is
    /// how a pad is attributed to the sender that produced it.
    QHash<int, QString> m_streamForMline;
    /// Media-section index -> the section's SDP `mid`. LiveKit states the
    /// same value on every TrackInfo, so this is what distinguishes two video
    /// tracks from ONE participant — a camera and a screen share.
    QHash<int, QString> m_midForMline;
    /// Media-section index -> the section's LiveKit TRACK SID (`TR_…`), from
    /// the same `a=msid:` line the participant comes from.
    ///
    /// WHY WE PARSE IT OURSELVES rather than reading the pad's `msid`
    /// property. That property is webrtcbin's own extraction, and how much of
    /// it is populated has moved between GStreamer releases — the dev shell
    /// is 1.26.11 while the packaged Windows runtime is 1.28.5. When the
    /// property comes back empty the code fell back to the transceiver `mid`,
    /// which recovers the participant but NEVER the track sid, so the track
    /// key was the string "1" or "2" and the frames it named were decrypted
    /// against a ring nobody had keyed.
    ///
    /// The SDP text is the same on every platform, and we already parse it
    /// for the participant and the mid. Taking the track sid from the same
    /// pass removes the version-sensitive dependency entirely.
    QHash<int, QString> m_trackForMline;
    /// Read from GStreamer streaming threads inside the pad probes, written
    /// from the Qt thread. Atomic because those are different threads and a
    /// probe cannot marshal without letting a frame through first.
    std::atomic<bool> m_encryptionRequired{false};
    std::atomic<bool> m_sendKeyReady{false};
    std::atomic<bool> m_recvKeyReady{false};
    /// See framesEncrypted(). Reset per session in start().
    std::atomic<quint64> m_framesEncrypted{0};
    std::atomic<quint64> m_framesDecrypted{0};
    std::atomic<quint64> m_framesDropped{0};
    /// A distinct IV stream id per encrypting track.
    ///
    /// The cryptor keeps its send counter PER SSRC, so two tracks sharing
    /// one would share a counter — and two frames with the same timestamp
    /// and counter under the same key produce the SAME IV. For AES-GCM that
    /// is not a weakening, it is a full break of both frames. Audio and
    /// video are separate tracks on separate threads, so they must never
    /// share this.
    ///
    /// The value need not be the real RTP SSRC: the IV travels inside the
    /// frame and a receiver uses it verbatim (livekit-client never checks
    /// this field against the RTP header), so local uniqueness is the
    /// entire requirement.
    std::atomic<quint32> m_nextIvStream{1};
    /// How many publisher tracks are actually LINKED to the publisher
    /// webrtcbin.
    ///
    /// webrtcbin emits `on-negotiation-needed` the moment it reaches PLAYING,
    /// which ensurePeer() does before any track exists — so an offer created
    /// from that signal has NO media section at all. We sent exactly that: a
    /// 98-byte SDP with no `m=` line, right after telling the SFU about a
    /// track via AddTrack. LiveKit answered `Leave(reason=6 STATE_MISMATCH)`
    /// every time, in every room.
    ///
    /// Atomic because on-negotiation-needed arrives on a GStreamer thread
    /// while this is written from the Qt thread.
    std::atomic<int> m_publishedMedia{0};

    /// Not owned. QPointer so a router destroyed before the engine cannot
    /// be dereferenced from a late streaming-thread callback.
    QPointer<SfuVideoRouter> m_videoRouter;

    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
