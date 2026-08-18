// Voice-call signaling state machine (2026-08-18) — backend pipes only.
//
// Consumes SDP-free CallSignal observations from the backend (see
// CallSignal.h) and drives one MSC2746-shaped call session: glare
// resolution, party-id locking, invite lifetime, cross-device
// answered/declined-elsewhere, busy auto-reject, and a bounded LRU of
// finished call ids so a late event can never resurrect an ended call.
//
// THERE IS NO MEDIA STACK. placeCall() refuses (no media backend exists in
// the tree); placeCallWithOffer() is the complete outbound pipe a future
// media backend feeds with a real SDP — tests exercise it with a synthetic
// one. Inbound calls can be observed, ring-gated, and declined/hung up;
// they can never be answered, so Connecting/Active are reachable only
// through the outbound path. No QML surface consumes this yet.
//
// Ring POLICY and ring STATE are separate: a muted room still produces
// Ringing; only shouldRing() is false. NotificationManager subscribes in a
// later round.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
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
    Q_PROPERTY(int state READ stateInt NOTIFY stateChanged)
    Q_PROPERTY(bool ringing READ ringing NOTIFY stateChanged)
    Q_PROPERTY(QString activeRoomId READ activeRoomId NOTIFY stateChanged)
    Q_PROPERTY(QString callerUserId READ activeSenderId NOTIFY stateChanged)
    Q_PROPERTY(QString activeCallId READ activeCallId NOTIFY stateChanged)

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
    // Test-only override of the local user's MXID for targeted-invite
    // filtering. Production resolves it live from the client
    // (currentUserId()), so account switches are followed automatically;
    // the override exists because the mock is not logged in under test.
    void setOwnUserId(const QString &userId);

    State state() const { return m_state; }
    int stateInt() const { return static_cast<int>(m_state); }
    bool ringing() const { return m_state == State::Ringing; }
    EndReason endReason() const { return m_endReason; }
    QString activeRoomId() const;
    QString activeCallId() const;
    QString activeSenderId() const;
    bool sessionLive() const;

    // The media-engine seam (see CallMediaBackend.h). NOT owned; nullptr
    // (the production state — no engine exists in the tree) keeps
    // placeCall()/answer() refusing honestly. Registering also tells the
    // backend bridge to start carrying SDP for this client (C++ memory
    // only, never QML, never logs).
    void setMediaBackend(CallMediaBackend *backend);
    bool mediaBackendAvailable() const { return m_mediaBackend != nullptr; }

    // UI-facing entries. Both refuse without a media backend: a stubbed
    // offer/answer would place or accept a call that dies at the peer.
    Q_INVOKABLE bool placeCall(const QString &roomId);
    Q_INVOKABLE bool answer();
    QString lastRefusal() const { return m_lastRefusal; }

    // The complete outbound pipe, fed by a future media backend (tests feed
    // it a synthetic SDP). Sends a real m.call.invite.
    bool placeCallWithOffer(const QString &roomId, const QString &offerSdp,
                            qint64 lifetimeMs = 60000,
                            const QString &invitee = QString());

    // Decline the ringing inbound call (legacy reject or m.rtc.decline).
    Q_INVOKABLE bool rejectIncoming();
    // Hang up our own outbound call (Inviting/Connecting/Active).
    Q_INVOKABLE bool hangup();

    // Missed-REASON subset. NOT sufficient alone: a completed (answered)
    // call also ends RemoteHangup, so endSession additionally requires the
    // call to still have been RINGING — the `missed` flag on
    // incomingCallEnded is the authoritative answer (review round 2).
    static bool isMissedCallReason(EndReason reason)
    {
        return reason == EndReason::InviteTimeout
            || reason == EndReason::RemoteHangup;
    }

    // Ring policy inputs. Backlog suppression defaults TRUE — a freshly
    // started app must never ring for cold-start backlog; the sync
    // lifecycle owner lowers it once live (NotificationManager pattern).
    void setBacklogSuppressed(bool suppressed);
    void setSenderIgnoredCheck(std::function<bool(const QString &)> check);
    void setRoomMutedCheck(std::function<bool(const QString &)> check);
    bool shouldRing() const;

Q_SIGNALS:
    void stateChanged();
    // remainingMs is the invite's real remaining validity, so the ring's
    // duration can follow the call instead of a hardcoded window.
    void incomingCallStarted(const QString &roomId, const QString &callId,
                             const QString &senderId, qint64 remainingMs);
    // `missed` is decided where the pre-end state is known: inbound, still
    // Ringing, and ended by timeout or the caller giving up.
    void incomingCallEnded(const QString &roomId, const QString &callId,
                           int reason, bool missed);
    // A dispatched signaling send was rejected by the server. Category is
    // the coarse classify_room_error set; never raw error text.
    void sendFailed(const QString &category);

private:
    enum class Direction { None, Outbound, Inbound };

    struct Session {
        QString roomId;
        QString callId; // legacy call id, or the notification event id (rtc)
        QString ourPartyId;
        QString remotePartyId;
        QString senderId;
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
        // Outbound: the invite send was dispatched (used to decide whether
        // a media failure needs a wire hangup or only a local end).
        bool inviteDispatched = false;
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
    // Slot (not inline lambda) so tests can drive expiry via
    // QMetaObject::invokeMethod instead of waiting out real timers.
    Q_SLOT void onLifetimeExpired();
    void onMediaOfferReady(const QString &callId, const QString &sdp);
    void onMediaAnswerReady(const QString &callId, const QString &sdp);
    void onMediaConnected(const QString &callId);
    void onMediaFailed(const QString &callId, const QString &category);

    bool matchesSession(const CallSignal &signal) const;
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
    // QPointer: the backend is NOT owned, and a destroyed backend must
    // read as absent, never as a dangling pointer in endSession's close().
    QPointer<CallMediaBackend> m_mediaBackend;
    State m_state = State::Idle;
    EndReason m_endReason = EndReason::None;
    Session m_session;
    QTimer m_lifetimeTimer;
    QStringList m_recentEnded; // bounded LRU of finished call ids
    // Dispatched send ops, each scoped to the call it belongs to, so a
    // stale result from an ended call can never mutate or end the live
    // session (review 2026-08-18).
    QHash<quint64, QString> m_pendingOps;
    QList<quint64> m_pendingOpOrder;
    // Bound on unsolicited-invite auto-rejects per live session: without
    // it any room member could make this client emit an unbounded stream
    // of m.call.reject events with zero user interaction.
    int m_busyRejectsThisSession = 0;
    QString m_ownUserId;
    QString m_lastRefusal;
    bool m_backlogSuppressed = true;
    std::function<bool(const QString &)> m_senderIgnoredCheck;
    std::function<bool(const QString &)> m_roomMutedCheck;
};
