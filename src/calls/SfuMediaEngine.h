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
    /// the camera.
    void publishVideo(const QString &cid, bool screenShare, int nodeId);
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
    /// Keyed by the sender's LiveKit stream id (the participant sid the SFU
    /// puts in the SDP's `msid`), because LiveKit's key index is
    /// per-participant: two senders may legitimately both use index 0 with
    /// different key material. One shared ring would decrypt at most one of
    /// them, so this is a ring PER sender, exactly as livekit-client keeps
    /// one decryptor per participant.
    void setInboundKey(const QString &streamId, int index,
                       const QByteArray &rawKey);
    /// Whether outgoing frames are actually being encrypted right now.
    bool encryptionActive() const;
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
    void remoteTrackAdded(const QString &identity, const QString &kind);
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

    // GStreamer-thread callbacks. Each carries the EMITTING element's
    // pointer as a session token, checked against the live session before
    // anything is acted on — the engine is one object reused call after
    // call, and a queued event from a closed session must never be
    // attributed to the next one.
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
    /// The cryptor for one sender, created if this is the first sight of it.
    std::shared_ptr<CallFrameCryptor> recvCryptorFor(const QString &streamId);
    /// Record media-section index -> stream id.
    ///
    /// Takes the already-extracted map rather than the SDP: GstSDPMessage is
    /// a typedef of an ANONYMOUS struct, so it cannot be forward-declared,
    /// and pulling gst/sdp into this header for one parameter would put
    /// GStreamer on every translation unit that mentions the engine.
    void noteStreamIds(const QHash<int, QString> &byMline);

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
    /// Read from GStreamer streaming threads inside the pad probes, written
    /// from the Qt thread. Atomic because those are different threads and a
    /// probe cannot marshal without letting a frame through first.
    std::atomic<bool> m_encryptionRequired{false};
    std::atomic<bool> m_sendKeyReady{false};
    std::atomic<bool> m_recvKeyReady{false};
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

    /// Not owned. QPointer so a router destroyed before the engine cannot
    /// be dereferenced from a late streaming-thread callback.
    QPointer<SfuVideoRouter> m_videoRouter;

    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
