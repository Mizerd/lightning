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
    QML_ELEMENT
    QML_UNCREATABLE("CallController is exposed via app.calls")
    Q_PROPERTY(int state READ stateInt NOTIFY stateChanged)
    Q_PROPERTY(bool ringing READ ringing NOTIFY stateChanged)
    Q_PROPERTY(bool rtcRing READ rtcRing NOTIFY stateChanged)
    Q_PROPERTY(QString activeRoomId READ activeRoomId NOTIFY stateChanged)
    Q_PROPERTY(QString callerUserId READ activeSenderId NOTIFY stateChanged)
    Q_PROPERTY(QString callerDisplayName READ callerDisplayName
                   NOTIFY callerDisplayNameChanged)
    Q_PROPERTY(QString activeCallId READ activeCallId NOTIFY stateChanged)
    Q_PROPERTY(bool mediaBackendAvailable READ mediaBackendAvailable
                   NOTIFY mediaBackendAvailableChanged)
    Q_PROPERTY(bool muteControlAvailable READ muteControlAvailable
                   NOTIFY mediaBackendAvailableChanged)
    Q_PROPERTY(bool microphoneMuted READ microphoneMuted
                   NOTIFY audioStateChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY audioStateChanged)
    Q_PROPERTY(bool videoSupported READ videoSupported
                   NOTIFY mediaBackendAvailableChanged)
    Q_PROPERTY(bool videoCall READ videoCall NOTIFY videoStateChanged)
    Q_PROPERTY(bool cameraOn READ cameraOn NOTIFY videoStateChanged)
    Q_PROPERTY(bool remoteVideoVisible READ remoteVideoVisible
                   NOTIFY videoStateChanged)
    Q_PROPERTY(bool remoteMicrophoneMuted READ remoteMicrophoneMuted
                   NOTIFY videoStateChanged)

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
    QString callerDisplayName() const;
    static QString presentableCallerName(const QString &userId,
                                         const QString &displayName,
                                         bool ambiguous = false);
    static QString sanitizedCallerName(const QString &displayName);
    static QString callerNameIn(const MatrixClient *client,
                                const QString &roomId, const QString &userId);
    bool sessionLive() const;

    void setMediaBackend(CallMediaBackend *backend);
    // Out of line: comparing a QPointer to a forward-declared type needs the
    // complete type on Qt 6.8.
    bool mediaBackendAvailable() const;

    Q_INVOKABLE void setMicrophoneMuted(bool muted);
    Q_INVOKABLE void toggleMicrophoneMuted();
    Q_INVOKABLE void setDeafened(bool deafened);
    Q_INVOKABLE void toggleDeafened();
    bool muteControlAvailable() const;
    bool microphoneMuted() const { return m_microphoneMuted; }
    bool deafened() const { return m_deafened; }

    bool videoSupported() const;
    bool videoCall() const { return sessionLive() && m_session.video; }
    bool cameraOn() const { return sessionLive() && m_cameraOn; }
    bool remoteVideoVisible() const
    {
        return sessionLive() && m_session.remoteVideoReceiving
            && !m_session.remoteVideoMuted;
    }
    bool remoteMicrophoneMuted() const
    {
        return sessionLive() && m_session.remoteAudioMuted;
    }
    Q_INVOKABLE bool setCameraOn(bool on);
    Q_INVOKABLE bool toggleCamera() { return setCameraOn(!m_cameraOn); }
    Q_INVOKABLE void attachVideoSink(bool self, QObject *sink);

    Q_INVOKABLE bool placeCall(const QString &roomId,
                               const QString &invitee = QString(),
                               bool video = false);
    Q_INVOKABLE bool answer(bool camera = false);
    Q_INVOKABLE QString lastRefusal() const { return m_lastRefusal; }

    bool placeCallWithOffer(const QString &roomId, const QString &offerSdp,
                            qint64 lifetimeMs = 60000,
                            const QString &invitee = QString());

    Q_INVOKABLE bool rejectIncoming();
    void noteAnsweredByOtherLane(const QString &roomId);
    Q_INVOKABLE bool hangup();

    void setTurnWaitTimeoutForTest(int ms) { m_turnWaitTimer.setInterval(qMax(1, ms)); }
    void setMediaConnectionTimeoutForTest(int ms) { m_mediaConnectionTimeoutMs = qMax(1, ms); }

    static bool isMissedCallReason(EndReason reason)
    {
        return reason == EndReason::InviteTimeout
            || reason == EndReason::RemoteHangup;
    }

    // Ring policy inputs. Backlog suppression defaults to true so a starting
    // app never rings for cold-start backlog; the sync owner lowers it once
    // live.
    // Lowering it re-announces a ring that arrived while it was raised and is
    // still live, with its real remaining time: a call that reached us during
    // the initial sync is not lost to it.
    void setBacklogSuppressed(bool suppressed);
    void setSenderIgnoredCheck(std::function<bool(const QString &)> check);
    void setRoomMutedCheck(std::function<bool(const QString &)> check);
    void setRingPolicyCheck(std::function<bool(const QString &roomId)> check);
    void noteRtcSession(const QString &roomId, qint64 ownMembershipJoinedAtMs,
                        int memberCount);
    static constexpr qint64 kOwnMembershipSkewMs = 5000;
    bool shouldRing() const;
    bool noticeAllowed(const QString &roomId, const QString &senderId) const;

    static QString mediaFailureNotice(const QString &category, bool wasActive);

Q_SIGNALS:
    void audioStateChanged();
    void videoStateChanged();
    void stateChanged();
    void cameraFailed(const QString &category);
    void mediaFailureEnded(const QString &category, bool wasActive);
    void callerDisplayNameChanged();
    void mediaBackendAvailableChanged();
    void incomingCallStarted(const QString &roomId, const QString &callId,
                             const QString &senderId, qint64 remainingMs);
    void incomingCallEnded(const QString &roomId, const QString &callId,
                           int reason, bool missed);
    void callStartedNotice(const QString &roomId, const QString &eventId,
                           const QString &senderId, qint64 remainingMs);
    void sendFailed(const QString &category);

private:
    enum class Direction { None, Outbound, Inbound };

    struct Session {
        QString roomId;
        QString callId;
        QString ourPartyId;
        QString remotePartyId;
        QString senderId;
        QString remoteUserId;
        QString inviteEventId;
        QString invitee;
        Direction direction = Direction::None;
        bool rtc = false;
        bool video = false;
        bool cameraOnAnswer = false;
        bool remoteAudioMuted = false;
        bool remoteVideoMuted = false;
        bool remoteVideoReceiving = false;
        qint64 lifetimeMs = 0;
        bool selectAnswerSent = false;
        bool offerPending = false;
        bool answerPending = false;
        // TURN must reach the backend before it creates its webrtcbin.
        bool mediaWaitingForTurn = false;
        QString pendingRemoteOffer;
        bool inviteDispatched = false;
        bool announcedLive = false;
        qint64 ringDeadlineMs = 0;
        qint64 ringStartedAtMs = 0;
        bool rtcSessionSeen = false;
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
    void handleNegotiate(const CallSignal &signal);
    void handleStreamMetadata(const CallSignal &signal);
    void noteRemoteMetadata(const CallSignal &signal);
    QString streamMetadataJson() const;
    void publishStreamMetadata();
    bool rtcNotificationRingsUs(const CallSignal &signal) const;
    void rememberNotice(const QString &eventId);
    Q_SLOT void onLifetimeExpired();
    void onMediaOfferReady(const QString &callId, const QString &sdp);
    void onMediaAnswerReady(const QString &callId, const QString &sdp);
    void onMediaConnected(const QString &callId);
    void onMediaFailed(const QString &callId, const QString &category);
    void onMediaNegotiationReady(const QString &callId, const QString &type,
                                 const QString &sdp);
    void onRemoteVideoChanged(const QString &callId, bool receiving);
    void onCameraFailed(const QString &callId, const QString &category);
    void onMediaLocalCandidate(const QString &callId,
                               const QString &candidate,
                               const QString &sdpMid, int sdpMLineIndex);
    void onMediaGatheringComplete(const QString &callId);
    void onRemoteCandidates(const QString &roomId, const QString &callId,
                            const QString &partyId, const QString &sender, bool own,
                            const QVariantList &candidates);
    void onTurnServers(quint64 opId, bool ok, const QString &username,
                       const QString &password, const QStringList &uris,
                       qint64 ttlSeconds, const QString &category);
    void flushLocalCandidates();
    void requestTurnServersIfStale();
    void startPendingMedia();

    bool matchesSession(const CallSignal &signal) const;
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
    QTimer m_turnWaitTimer;
    int m_mediaConnectionTimeoutMs = 30000;
    QStringList m_recentEnded;
    QHash<quint64, QString> m_pendingOps;
    QList<quint64> m_pendingOpOrder;
    // Bounds busy auto-rejects per session, or any room member could make us
    // send unbounded m.call.reject events.
    int m_busyRejectsThisSession = 0;
    QString m_ownUserId;
    void applyAudioStateToBackend();
    void resetAudioIntent();

    QString m_lastRefusal;
    bool m_microphoneMuted = false;
    bool m_cameraOn = false;
    bool m_deafened = false;
    bool m_micMutedBeforeDeafen = false;
    QVariantList m_pendingLocalCandidates;
    bool m_gatheringComplete = false;
    QTimer m_candidateFlushTimer;
    quint64 m_turnOp = 0;
    qint64 m_turnRequestedAtMs = 0;
    qint64 m_turnExpiryMs = 0;
    QStringList m_turnUris;
    QString m_turnUsername;
    QString m_turnPassword;
    bool m_backlogSuppressed = true;
    std::function<bool(const QString &)> m_senderIgnoredCheck;
    std::function<bool(const QString &)> m_roomMutedCheck;
    std::function<bool(const QString &)> m_ringPolicyCheck;
    QStringList m_recentNotices;
};
