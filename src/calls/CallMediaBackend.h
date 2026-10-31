// The media-engine seam for 1:1 voice and video calls. GstCallMediaBackend implements
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

    // Whether this engine carries video in this build. A false engine keeps
    // answering a video offer with the video section declined.
    virtual bool supportsVideo() const { return false; }

    // Asked before createOffer()/createAnswer(): `video` adds a video section
    // to an offer; `cameraOn` opens the camera. An answer carries a video
    // section exactly when the offer does and the engine can.
    virtual void prepareCall(const QString &callId, bool video, bool cameraOn)
    { Q_UNUSED(callId); Q_UNUSED(video); Q_UNUSED(cameraOn); }

    // Camera on or off in a live call. Off closes the device, so the light
    // goes out; the section stays and the peer learns from stream metadata.
    // On when the call has no video section yet adds one: the engine answers
    // with negotiationReady("offer", ...).
    virtual void setCameraEnabled(const QString &callId, bool on)
    { Q_UNUSED(callId); Q_UNUSED(on); }

    // m.call.negotiate received. "offer" is answered with
    // negotiationReady("answer", ...); "answer" completes our offer.
    virtual void applyNegotiation(const QString &callId, const QString &type,
                                  const QString &sdp)
    { Q_UNUSED(callId); Q_UNUSED(type); Q_UNUSED(sdp); }

    // The msid stream id our SDP names for the camera and microphone, which
    // keys MSC3077 stream metadata. Empty: name no stream.
    virtual QString localStreamId() const { return {}; }

    // Hand a QVideoSink (as QObject, keeping Qt Multimedia out of this seam)
    // to the remote picture or to our own. `self` picks which. A destroyed
    // sink releases itself.
    virtual void attachVideoSink(bool self, QObject *sink)
    { Q_UNUSED(self); Q_UNUSED(sink); }

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
    // A description for m.call.negotiate: `type` is "offer" or "answer".
    void negotiationReady(const QString &callId, const QString &type,
                          const QString &sdp);
    // The peer's picture is arriving (first frame after a gap) or stopped.
    void remoteVideoChanged(const QString &callId, bool receiving);
    // Our camera could not open. The call goes on without it. `category` is
    // the coarse set the MatrixRTC lane reports.
    void cameraFailed(const QString &callId, const QString &category);
    // Terminal failure. `category` is a coarse label safe to log (never SDP or
    // device details).
    void failed(const QString &callId, const QString &category);
};
