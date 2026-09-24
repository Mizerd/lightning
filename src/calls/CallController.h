// Voice-call state machine for the legacy MSC2746 `m.call.*` lane.
//
// Consumes SDP-free CallSignal observations (CallSignal.h) and drives one
// call session: glare resolution, party-id locking, invite lifetime,
// answered/declined-elsewhere, busy auto-reject, and a bounded LRU of
// finished call ids so a late event cannot resurrect an ended call.
//
// Media goes through the CallMediaBackend seam. With an engine registered
// (GstCallMediaBackend), placeCall()/answer() run real calls with trickled
// ICE and homeserver TURN; without one both refuse and inbound calls can only
// be observed or declined. SDP and candidates never reach QML.
//
// Ring policy is separate from ring state: a muted room still reaches
// Ringing, but shouldRing() is false.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

#include "matrix/CallSignal.h"

class CallMediaBackend;
class MatrixClient;

class CallController : public QObject
{
    Q_OBJECT
    // Registered so QML can compare states symbolically; app.calls is the
    // only instance.
    QML_ELEMENT
    QML_UNCREATABLE("CallController is exposed via app.calls")
    Q_PROPERTY(int state READ stateInt NOTIFY stateChanged)
    Q_PROPERTY(bool ringing READ ringing NOTIFY stateChanged)
    /// True while the live ring came over the MatrixRTC lane. The lanes are
    /// answered differently: a legacy ring by answer(), an RTC ring by joining
    /// the SFU session (`app.groupCall.join`), so the card needs to know which.
    Q_PROPERTY(bool rtcRing READ rtcRing NOTIFY stateChanged)
    Q_PROPERTY(QString activeRoomId READ activeRoomId NOTIFY stateChanged)
    Q_PROPERTY(QString callerUserId READ activeSenderId NOTIFY stateChanged)
    Q_PROPERTY(QString activeCallId READ activeCallId NOTIFY stateChanged)
    Q_PROPERTY(bool mediaBackendAvailable READ mediaBackendAvailable
                   NOTIFY mediaBackendAvailableChanged)
    // `muteControlAvailable` gates the UI: the seam's default mute is a no-op,
    // and a control that does nothing is worse than none.
    Q_PROPERTY(bool muteControlAvailable READ muteControlAvailable
                   NOTIFY mediaBackendAvailableChanged)
    Q_PROPERTY(bool microphoneMuted READ microphoneMuted
                   NOTIFY audioStateChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY audioStateChanged)

public:
    enum class State { Idle, Inviting, Ringing, Connecting, Active, Ended };
    Q_ENUM(State)

    enum class EndReason {
        None,
        LocalHangup,
        RemoteHangup,
        LocalReject,
        RemoteReject,
        InviteTimeout,
        AnsweredElsewhere,
        DeclinedElsewhere,
        GlareReplaced,
        Busy,
        SendFailed,
        MediaFailed,
        SessionLost,
    };
    Q_ENUM(EndReason)

    explicit CallController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Test-only override of the local MXID for targeted-invite filtering.
    // Production reads it live from the client, following account switches.
    void setOwnUserId(const QString &userId);

    State state() const { return m_state; }
    int stateInt() const { return static_cast<int>(m_state); }
    bool ringing() const { return m_state == State::Ringing; }
    /// False unless actually ringing: `m_session.rtc` outlives the ring.
    bool rtcRing() const
    {
        return m_state == State::Ringing && m_session.rtc;
    }
    EndReason endReason() const { return m_endReason; }
    QString activeRoomId() const;
    QString activeCallId() const;
    QString activeSenderId() const;
    bool sessionLive() const;

    // The media-engine seam (see CallMediaBackend.h). Not owned; nullptr keeps
    // placeCall()/answer() refusing. Registering also tells the bridge to
    // carry SDP for this client (memory only, never QML or logs).
    void setMediaBackend(CallMediaBackend *backend);
    // Out of line: comparing a QPointer to a forward-declared type needs the
    // complete type on Qt 6.8.
    bool mediaBackendAvailable() const;

    // Real mute: the engine stops publishing, so the peer receives nothing.
    Q_INVOKABLE void setMicrophoneMuted(bool muted);
    Q_INVOKABLE void toggleMicrophoneMuted();
    // Deafen silences incoming audio and also mutes the microphone;
    // undeafening restores the previous mute state rather than unmuting.
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void toggleDeafened();
    bool muteControlAvailable() const;
    bool microphoneMuted() const { return m_microphoneMuted; }
    bool deafened() const { return m_deafened; }

    /// `invitee` is the one user this call is for (MSC2746): only they ring,
    /// and every later signal on the session must come from them, since
    /// call_id is readable by every room member. Empty means the first
    /// answerer is locked in as the peer.
    Q_INVOKABLE bool placeCall(const QString &roomId,
                               const QString &invitee = QString());
    Q_INVOKABLE bool answer();
    /// Why the last placeCall()/answer() refused, as a closed-set token. The
    /// card maps it to wording and never shows the token itself.
    Q_INVOKABLE QString lastRefusal() const { return m_lastRefusal; }

    // The outbound pipe given a ready offer (tests feed a synthetic SDP).
    // Sends a real m.call.invite.
    bool placeCallWithOffer(const QString &roomId, const QString &offerSdp,
                            qint64 lifetimeMs = 60000,
                            const QString &invitee = QString());

    // Decline the ringing inbound call (legacy reject or m.rtc.decline).
    Q_INVOKABLE bool rejectIncoming();
    /// The user joined this conversation's MatrixRTC call in this app instead
    /// of accepting the ring. Clears the local ring and sends nothing: a
    /// decline would tell the caller "no" about a call the user just joined.
    void noteAnsweredByOtherLane(const QString &roomId);
    // Hang up our own call (outbound from Inviting; inbound once answered).
    Q_INVOKABLE bool hangup();

    // Reasons that can mean "missed". Not sufficient alone: an answered call
    // also ends with RemoteHangup, so the `missed` flag on incomingCallEnded
    // (which also requires Ringing) is authoritative.
    static bool isMissedCallReason(EndReason reason)
    {
        return reason == EndReason::InviteTimeout
            || reason == EndReason::RemoteHangup;
    }

    // Ring policy inputs. Backlog suppression defaults to true so a starting
    // app never rings for cold-start backlog; the sync owner lowers it once
    // live.
    void setBacklogSuppressed(bool suppressed);
    void setSenderIgnoredCheck(std::function<bool(const QString &)> check);
    void setRoomMutedCheck(std::function<bool(const QString &)> check);
    bool shouldRing() const;

Q_SIGNALS:
    void audioStateChanged();
    void stateChanged();
    void mediaBackendAvailableChanged();
    // remainingMs is the invite's real remaining validity.
    void incomingCallStarted(const QString &roomId, const QString &callId,
                             const QString &senderId, qint64 remainingMs);
    // `missed`: inbound, still Ringing, and ended by timeout or the caller
    // giving up.
    void incomingCallEnded(const QString &roomId, const QString &callId,
                           int reason, bool missed);
    // A signaling send was rejected by the server; category is the coarse
    // classify_room_error set, never raw error text.
    void sendFailed(const QString &category);

private:
    enum class Direction { None, Outbound, Inbound };

    struct Session {
        QString roomId;
        QString callId; // legacy call id, or the notification event id (rtc)
        QString ourPartyId;
        QString remotePartyId;
        QString senderId;
        // The user every remote signal must come from: the invite's sender
        // (inbound), or the named invitee or first answerer (outbound).
        QString remoteUserId;
        QString inviteEventId;
        QString invitee;
        Direction direction = Direction::None;
        bool rtc = false;
        qint64 lifetimeMs = 0;
        bool selectAnswerSent = false;
        // Outbound: the media backend is producing the offer; the invite
        // has not been dispatched yet.
        bool offerPending = false;
        // Inbound: the media backend is producing the answer.
        bool answerPending = false;
        // Outbound: the invite was sent, so a media failure needs a wire
        // hangup rather than only a local end.
        bool inviteDispatched = false;
        // Inbound: candidates trickled while ringing, before the engine has a
        // session; dropping them loses most usable candidates. Bounded and
        // drained right after createAnswer().
        QVariantList earlyRemoteCandidates;
    };

    void onCallSignal(const CallSignal &signal);
    void onCallSendFinished(quint64 opId, bool ok, const QString &category,
                            const QString &callId, const QString &eventId);
    void onLoggedOut();

    void handleInvite(const CallSignal &signal);
    void handleAnswer(const CallSignal &signal);
    void handleHangup(const CallSignal &signal);
    void handleReject(const CallSignal &signal);
    void handleSelectAnswer(const CallSignal &signal);
    void handleRtcNotification(const CallSignal &signal);
    void handleRtcDecline(const CallSignal &signal);
    // A slot so tests can drive expiry via QMetaObject::invokeMethod.
    Q_SLOT void onLifetimeExpired();
    void onMediaOfferReady(const QString &callId, const QString &sdp);
    void onMediaAnswerReady(const QString &callId, const QString &sdp);
    void onMediaConnected(const QString &callId);
    void onMediaFailed(const QString &callId, const QString &category);
    void onMediaLocalCandidate(const QString &callId,
                               const QString &candidate,
                               const QString &sdpMid, int sdpMLineIndex);
    void onMediaGatheringComplete(const QString &callId);
    void onRemoteCandidates(const QString &roomId, const QString &callId,
                            const QString &partyId, bool own,
                            const QVariantList &candidates);
    void onTurnServers(quint64 opId, bool ok, const QString &username,
                       const QString &password, const QStringList &uris,
                       qint64 ttlSeconds, const QString &category);
    void flushLocalCandidates();
    void requestTurnServersIfStale();

    bool matchesSession(const CallSignal &signal) const;
    // True when `signal.sender` is the bound remote user, or none is bound
    // yet. See placeCall().
    bool fromExpectedPeer(const CallSignal &signal) const;
    bool recentlyEnded(const QString &callId) const;
    void rememberEnded(const QString &callId);
    void endSession(EndReason reason);
    void setState(State state);
    void armLifetimeTimer(qint64 remainingMs);
    qint64 remainingInviteMs(qint64 originServerTs, qint64 senderTs,
                             qint64 lifetimeMs) const;
    void trackOp(quint64 opId, const QString &callId);
    QString ownUserId() const;
    static QString freshPartyId();

    MatrixClient *m_client = nullptr;
    // QPointer: not owned, and a destroyed backend must read as absent.
    QPointer<CallMediaBackend> m_mediaBackend;
    State m_state = State::Idle;
    EndReason m_endReason = EndReason::None;
    Session m_session;
    QTimer m_lifetimeTimer;
    QStringList m_recentEnded; // bounded LRU of finished call ids
    // Send ops, each tied to its call, so a stale result cannot affect the
    // live session.
    QHash<quint64, QString> m_pendingOps;
    QList<quint64> m_pendingOpOrder;
    // Bounds busy auto-rejects per session, or any room member could make us
    // send unbounded m.call.reject events.
    int m_busyRejectsThisSession = 0;
    QString m_ownUserId;
    void applyAudioStateToBackend();
    void resetAudioIntent();

    QString m_lastRefusal;
    // Local audio intent, owned here because the engine resets per session;
    // re-applied when the next call connects.
    bool m_microphoneMuted = false;
    bool m_deafened = false;
    // The mic state to return to when undeafening.
    bool m_micMutedBeforeDeafen = false;
    // Local candidates, batched (150 ms) into m.call.candidates events;
    // MSC2746's empty end marker rides the final batch.
    QVariantList m_pendingLocalCandidates;
    bool m_gatheringComplete = false;
    QTimer m_candidateFlushTimer;
    // Homeserver TURN credential cache: memory only, refreshed before expiry,
    // never logged.
    quint64 m_turnOp = 0;
    qint64 m_turnRequestedAtMs = 0;
    qint64 m_turnExpiryMs = 0;
    QStringList m_turnUris;
    QString m_turnUsername;
    QString m_turnPassword;
    bool m_backlogSuppressed = true;
    std::function<bool(const QString &)> m_senderIgnoredCheck;
    std::function<bool(const QString &)> m_roomMutedCheck;
};
