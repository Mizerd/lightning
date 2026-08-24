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
    static constexpr int kScreenWidth = 1920;
    static constexpr int kScreenHeight = 1080;
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
    /// Publish the camera, or a screen share when `screenShare` is true.
    /// `nodeId` is the PipeWire node a desktop portal handed us; -1 means
    /// the camera. `pipewireFd` is the descriptor from the portal's
    /// OpenPipeWireRemote and OWNERSHIP PASSES HERE — pipewiresrc dups it, so
    /// this closes the copy it was given once the element exists (including
    /// on every failure path). A node id without a remote fd names a node in
    /// the caller's own default PipeWire remote, where a portal node need not
    /// be visible at all: the pipeline then plays and produces no frames,
    /// which is a black screen share that reports success.
    void publishVideo(const QString &cid, bool screenShare, int nodeId,
                      int pipewireFd = -1);
    /// The VIDEO publish pipeline, as a gst_parse description.
    ///
    /// Exposed so the SCREEN-SHARE shape — which adds a `tee` and a self-view
    /// branch and is therefore not the shape test-source mode builds — can be
    /// parsed by a test. A typo there is a screen share that dies on its first
    /// frame, which is exactly the class of failure this file has already had.
    static QString videoPipelineDescription(const QString &source,
                                            const QString &limits,
                                            const QString &encoder,
                                            const QString &selfView,
                                            quint32 ssrc);
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
    static QString screenShareSource(int nodeId, int pipewireFd);
    /// Stop publishing one track and renegotiate.
    void unpublish(const QString &cid);

    /// A remote description from the SFU for one peer connection.
    void applyRemoteDescription(Target target, const QString &kind,
                                const QString &sdp);
    /// One remote ICE candidate for one peer connection.
    void applyRemoteCandidate(Target target, const QString &candidateInit);

    /// Real mute: stops publishing, never attenuates (see the 1:1 engine).
    void setMicrophoneMuted(bool muted);
    /// Local playback mute for every remote track.
    void setOutputMuted(bool muted);
    /// Local volume for ONE remote participant, 0..100. Local-only: it
    /// changes nothing for anyone else and sends no event.
    void setParticipantVolume(const QString &identity, int percent);

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
                       const QHash<int, QString> &midsByMline);

    Peer m_publisher;
    Peer m_subscriber;
    bool m_active = false;
    bool m_testSources = false;
    /// Bumped on every start/stop so a late callback is discarded.
    std::atomic<quint64> m_generation{0};
    /// Read from a GStreamer streaming thread in pad-added: a remote track
    /// arriving after the user deafened must come up already silenced,
    /// and marshalling first would let it be briefly audible.
    std::atomic<bool> m_outputMuted{false};
    bool m_microphoneMuted = false;
    /// Published tracks by client-chosen id, so unpublish can find them.
    QHash<QString, GstElement *> m_publishedBins;
    /// PipeWire remote descriptors owned by a publishing bin (screen shares
    /// only), closed when that bin is torn down. See publishVideo.
    QHash<QString, int> m_publishedFds;
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
