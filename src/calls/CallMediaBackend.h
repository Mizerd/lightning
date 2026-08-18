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

    // Tear down all session state for the call. Must be idempotent and
    // must not emit further signals for this callId afterwards.
    virtual void close(const QString &callId) = 0;

Q_SIGNALS:
    void offerReady(const QString &callId, const QString &sdp);
    void answerReady(const QString &callId, const QString &sdp);
    // Media is flowing — the call is Active.
    void connected(const QString &callId);
    // Terminal failure for this call. `category` is a coarse label safe to
    // log (never SDP or device detail).
    void failed(const QString &callId, const QString &category);
};
