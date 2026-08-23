// The media-engine seam for voice calls (2026-08-18 round 2).
//
// NO implementation exists in the tree: Lightning has no WebRTC stack, and
// per the locked-dependency rule none can be added incidentally. This
// interface is the typed contract a future engine implements; tests drive
// the full CallController state machine through a fake. Registering a
// backend is what makes placeCall()/answer() reachable — without one they
// refuse honestly (see CallController).
//
// SDP handling contract: SDP strings passed through this interface carry
// host IPs and must never be logged, persisted, or exposed to QML. They
// exist in memory for the duration of call setup only. Implementations own
// all RTC negotiation state; CallController only correlates by callId.
#pragma once

#include <QObject>
#include <QString>

class CallMediaBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Begin producing an SDP offer for a new outbound call. Asynchronous:
    // answer with offerReady(callId, sdp) or failed(callId, category).
    virtual void createOffer(const QString &callId) = 0;

    // Produce an SDP answer for an inbound call's remote offer. Announce
    // with answerReady(callId, sdp) or failed(callId, category).
    virtual void createAnswer(const QString &callId,
                              const QString &remoteOfferSdp) = 0;

    // Complete the outbound handshake with the peer's answer. Success is
    // announced as connected(callId) once media actually flows.
    virtual void setRemoteAnswer(const QString &callId,
                                 const QString &remoteAnswerSdp) = 0;

    // Feed one remote ICE candidate (trickled via m.call.candidates). The
    // implementation buffers candidates that arrive before the remote
    // description is applied. An empty `candidate` is MSC2746's
    // end-of-candidates marker.
    virtual void addRemoteCandidate(const QString &callId,
                                    const QString &candidate,
                                    const QString &sdpMid,
                                    int sdpMLineIndex) = 0;

    // ICE server configuration from the HOMESERVER's /voip/turnServer —
    // policy: Lightning's engine contacts only servers the homeserver
    // names (no third-party STUN that would leak the user's IP). The
    // credentials are short-lived TURN secrets: apply, never log/persist.
    virtual void setIceServers(const QStringList &uris,
                               const QString &username,
                               const QString &password) = 0;

    // Microphone mute. This must STOP PUBLISHING, not lower a local
    // volume: the peer must receive nothing while muted. (Lowering local
    // gain still sends audio and is explicitly not mute.)
    //
    // Default no-op so an engine that genuinely cannot mute stays
    // buildable; `supportsMuteControl()` is what the UI consults, so a
    // no-op default can never present a working-looking mute button.
    virtual void setMicrophoneMuted(const QString &callId, bool muted)
    { Q_UNUSED(callId); Q_UNUSED(muted); }

    // Local output mute ("deafen"): silence incoming call audio only. This
    // legitimately IS a local volume operation — there is no upstream to
    // stop — and must not touch media playback outside the call.
    virtual void setOutputMuted(const QString &callId, bool muted)
    { Q_UNUSED(callId); Q_UNUSED(muted); }

    // Whether the two controls above actually do something in this engine.
    // The UI must gate on this rather than assume, so a control is never
    // offered that silently does nothing.
    virtual bool supportsMuteControl() const { return false; }

    // Tear down all session state for the call. Must be idempotent and
    // must not emit further signals for this callId afterwards.
    virtual void close(const QString &callId) = 0;

Q_SIGNALS:
    void offerReady(const QString &callId, const QString &sdp);
    void answerReady(const QString &callId, const QString &sdp);
    // A locally gathered ICE candidate to trickle to the peer.
    void localCandidate(const QString &callId, const QString &candidate,
                        const QString &sdpMid, int sdpMLineIndex);
    // Local gathering finished: the controller sends MSC2746's empty
    // end-of-candidates marker.
    void gatheringComplete(const QString &callId);
    // Media is flowing — the call is Active.
    void connected(const QString &callId);
    // Terminal failure for this call. `category` is a coarse label safe to
    // log (never SDP or device detail).
    void failed(const QString &callId, const QString &category);
};
