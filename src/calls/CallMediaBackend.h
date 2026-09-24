// The media-engine seam for 1:1 voice calls. GstCallMediaBackend implements
// it when built with WebRTC; tests drive CallController through a fake.
// Without a registered backend, placeCall()/answer() refuse (see
// CallController).
//
// SDP passed through this interface carries host IPs: never log, persist or
// expose it to QML; it lives in memory only during setup. Implementations own
// all negotiation state; CallController correlates by callId only.
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
    // implementation buffers candidates that precede the remote description.
    // An empty `candidate` is MSC2746's end-of-candidates marker.
    virtual void addRemoteCandidate(const QString &callId,
                                    const QString &candidate,
                                    const QString &sdpMid,
                                    int sdpMLineIndex) = 0;

    // ICE servers from the homeserver's /voip/turnServer only (no third-party
    // STUN that would leak the user's IP). The credentials are short-lived
    // TURN secrets: apply, never log or persist.
    virtual void setIceServers(const QStringList &uris,
                               const QString &username,
                               const QString &password) = 0;

    // Microphone mute must stop publishing, not lower a local volume: the peer
    // must receive nothing. The default no-op is safe because the UI gates on
    // supportsMuteControl().
    virtual void setMicrophoneMuted(const QString &callId, bool muted)
    { Q_UNUSED(callId); Q_UNUSED(muted); }

    // Local output mute ("deafen"): silence incoming call audio only; this one
    // is legitimately a local volume change.
    virtual void setOutputMuted(const QString &callId, bool muted)
    { Q_UNUSED(callId); Q_UNUSED(muted); }

    // Whether the two controls above work in this engine; the UI gates on it.
    virtual bool supportsMuteControl() const { return false; }

    // Tear down all session state for the call. Idempotent; no further signals
    // for this callId afterwards.
    virtual void close(const QString &callId) = 0;

Q_SIGNALS:
    void offerReady(const QString &callId, const QString &sdp);
    void answerReady(const QString &callId, const QString &sdp);
    // A locally gathered ICE candidate to trickle to the peer.
    void localCandidate(const QString &callId, const QString &candidate,
                        const QString &sdpMid, int sdpMLineIndex);
    // Local gathering finished; the controller sends MSC2746's empty
    // end-of-candidates marker.
    void gatheringComplete(const QString &callId);
    // Media is flowing; the call is Active.
    void connected(const QString &callId);
    // Terminal failure. `category` is a coarse label safe to log (never SDP or
    // device details).
    void failed(const QString &callId, const QString &category);
};
