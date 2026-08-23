// The real voice-call media engine (2026-08-18 round 3): GStreamer's
// webrtcbin — full WebRTC (ICE via libnice, DTLS-SRTP, Opus), audio-only.
//
// Compiled only when the GStreamer WebRTC dev files are present
// (HAVE_LIGHTNING_WEBRTC); registered at runtime only when
// runtimeAvailable() confirms every required element factory resolves, so
// a packaged build without the plugins keeps CallController's honest
// refusal instead of a call that dies at the peer.
//
// Threading: GStreamer invokes callbacks on its own streaming/ICE threads.
// Every callback marshals into this object's (GUI) thread via queued
// invocation guarded by a process-global alive-registry, and Qt-side
// handlers re-check the active call id — a late callback for a closed
// call is a no-op, never a signal.
//
// Privacy: SDP and ICE candidates carry host IPs. Nothing here logs
// either; failures surface as coarse category strings only. ICE servers
// come exclusively from the homeserver's /voip/turnServer (applied via
// setIceServers) — no third-party STUN fallback.
#pragma once

#include <atomic>

#include "CallMediaBackend.h"

#include <QString>
#include <QStringList>

typedef struct _GstElement GstElement;
typedef struct _GstPromise GstPromise;

class GstCallMediaBackend : public CallMediaBackend
{
    Q_OBJECT

public:
    // One-time probe: GStreamer initializes and every element the pipeline
    // needs resolves. `whyNot` (optional) receives a short, safe reason.
    static bool runtimeAvailable(QString *whyNot = nullptr);

    explicit GstCallMediaBackend(QObject *parent = nullptr);
    ~GstCallMediaBackend() override;

    // Test mode: audiotestsrc (quiet sine) and fakesink instead of the
    // microphone and speakers, so headless CI can run a REAL loopback
    // WebRTC handshake with no audio devices. Set before the first call.
    void setTestToneMode(bool on) { m_testTone = on; }

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
    // A real valve/volume pair exists in the pipeline, so this engine can
    // honestly claim mute support.
    bool supportsMuteControl() const override { return true; }
    void close(const QString &callId) override;

private:
    struct Session {
        QString callId;
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        bool offerer = false;
        bool remoteDescriptionSet = false;
        QList<QPair<int, QString>> pendingRemoteCandidates;
        // Named send-side valve: drop=true stops buffers reaching the
        // encoder, so no RTP is produced at all. That is a real mute.
        GstElement *micValve = nullptr;
        // Desired states, kept on the session because a receive bin can be
        // created AFTER the user deafens — a late remote track must come up
        // already silenced rather than briefly audible.
        bool micMuted = false;
        bool outputMuted = false;
    };

    bool startSession(const QString &callId, bool offerer,
                      int opusPayloadType);
    void destroySessionLocked();
    void applyIceConfigLocked();
    void flushPendingCandidatesLocked();

    // Qt-thread handlers the GStreamer-thread callbacks marshal into
    // (slots so the file-local bus handler can queue by name too).
    //
    // SESSION IDENTITY (review round 3, HIGH): the engine is ONE object
    // reused call after call, and a GStreamer-thread event already queued
    // when a call is closed must never be attributed to the next call.
    // Every handler therefore carries a `token` — the pointer value of the
    // GstElement that emitted the event (webrtcbin, or the pipeline for
    // bus errors), which every session allocates fresh — and acts only
    // when it matches the live session. Promise-driven paths hold a REF on
    // that element, so its address cannot be recycled while their events
    // are in flight.
    Q_SLOT void handleLocalDescription(quintptr token, bool offer,
                                       const QString &sdp);
    Q_SLOT void handleRemoteDescriptionApplied(quintptr token);
    Q_SLOT void handleIceCandidate(quintptr token, int mlineIndex,
                                   const QString &candidate);
    Q_SLOT void handleGatheringComplete(quintptr token);
    Q_SLOT void handleConnectionState(quintptr token, int state);
    Q_SLOT void handleFailure(quintptr token, const QString &category);
    bool tokenMatchesLiveSession(quintptr token) const;

    // GStreamer C callbacks (static; user_data = backend).
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
    // (The bus sync handler is a file-local function in the .cpp — its
    // GStreamer signature types stay out of this header.)

    Session m_session; // exactly one live call, matching CallController
    bool m_sessionActive = false;
    // Read from the GStreamer streaming thread in onPadAdded (a remote
    // track can arrive while deafened and must come up already silenced),
    // written from the Qt thread. Atomic because those are different
    // threads and this engine has no lock — every other cross-thread hop
    // here goes through marshal(), which a pad-added handler cannot use
    // without letting the track go audible first.
    std::atomic<bool> m_outputMuted{false};
    bool m_testTone = false;
    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
