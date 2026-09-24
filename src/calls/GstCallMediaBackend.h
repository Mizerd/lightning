// The 1:1 voice-call media engine: GStreamer webrtcbin (ICE via libnice,
// DTLS-SRTP, Opus), audio only.
//
// Compiled only with the GStreamer WebRTC dev files (HAVE_LIGHTNING_WEBRTC)
// and registered only when runtimeAvailable() finds every required element,
// so a build without the plugins keeps CallController's honest refusal.
//
// Threading: callbacks arrive on GStreamer threads and marshal to this
// object's thread through a process-global alive registry; Qt-side handlers
// re-check the session, so a late callback for a closed call does nothing.
//
// Privacy: SDP and candidates carry host IPs and are never logged. ICE
// servers come only from the homeserver's /voip/turnServer; no third-party
// STUN fallback.
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

    /// Capture/playback element descriptions from CallDeviceController, e.g.
    /// `pulsesrc device="alsa_input...."`. Empty means the automatic element,
    /// which follows the system default. Applied to the next call.
    void setAudioDevices(const QString &sourceElement,
                         const QString &sinkElement);

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
    void close(const QString &callId) override;

private:
    struct Session {
        QString callId;
        GstElement *pipeline = nullptr;
        GstElement *webrtc = nullptr;
        bool offerer = false;
        bool remoteDescriptionSet = false;
        QList<QPair<int, QString>> pendingRemoteCandidates;
        // Send-side valve: drop=true stops buffers before the encoder, so no
        // RTP is produced (a real mute).
        GstElement *micValve = nullptr;
        // Desired states, kept so a receive bin created after deafening comes
        // up already silenced.
        bool micMuted = false;
        bool outputMuted = false;
    };

    bool startSession(const QString &callId, bool offerer,
                      int opusPayloadType);
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
    bool m_testTone = false;
    QString m_audioSourceElement;
    QString m_audioSinkElement;
    QStringList m_iceUris;
    QString m_iceUsername;
    QString m_icePassword;
};
