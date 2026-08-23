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

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

typedef struct _GstElement GstElement;
typedef struct _GstPromise GstPromise;

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
    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
