#pragma once

#include <QObject>
#include <QString>

class CallMediaBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void createOffer(const QString &callId) = 0;

    virtual void createAnswer(const QString &callId,
                              const QString &remoteOfferSdp) = 0;

    virtual void setRemoteAnswer(const QString &callId,
                                 const QString &remoteAnswerSdp) = 0;

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

    virtual void setOutputMuted(const QString &callId, bool muted)
    { Q_UNUSED(callId); Q_UNUSED(muted); }

    virtual bool supportsMuteControl() const { return false; }

    virtual bool supportsVideo() const { return false; }

    virtual void prepareCall(const QString &callId, bool video, bool cameraOn)
    { Q_UNUSED(callId); Q_UNUSED(video); Q_UNUSED(cameraOn); }

    virtual void setCameraEnabled(const QString &callId, bool on)
    { Q_UNUSED(callId); Q_UNUSED(on); }

    virtual void applyNegotiation(const QString &callId, const QString &type,
                                  const QString &sdp)
    { Q_UNUSED(callId); Q_UNUSED(type); Q_UNUSED(sdp); }

    virtual QString localStreamId() const { return {}; }

    virtual void attachVideoSink(bool self, QObject *sink)
    { Q_UNUSED(self); Q_UNUSED(sink); }

    virtual void close(const QString &callId) = 0;

Q_SIGNALS:
    void offerReady(const QString &callId, const QString &sdp);
    void answerReady(const QString &callId, const QString &sdp);
    void localCandidate(const QString &callId, const QString &candidate,
                        const QString &sdpMid, int sdpMLineIndex);
    void gatheringComplete(const QString &callId);
    void connected(const QString &callId);
    void negotiationReady(const QString &callId, const QString &type,
                          const QString &sdp);
    void remoteVideoChanged(const QString &callId, bool receiving);
    void cameraFailed(const QString &callId, const QString &category);
    void failed(const QString &callId, const QString &category);
};
