#include "CallController.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QUuid>

#include "CallMediaBackend.h"
#include "matrix/MatrixClient.h"

Q_LOGGING_CATEGORY(lcCalls, "matrix.calls")

namespace {
// Bounded memory of finished calls: a late hangup/answer/reject for one of
// these ids is absorbed silently.
constexpr int kRecentEndedCap = 32;
// MSC2746 clock-skew guard: prefer origin_server_ts over the sender's own
// clock when they disagree by more than this.
constexpr qint64 kSenderClockSkewToleranceMs = 10000;
constexpr qint64 kMinLifetimeMs = 5000;
constexpr qint64 kMaxLifetimeMs = 300000;
// At most this many busy auto-rejects per session; further unsolicited
// invites are dropped silently so a hostile sender cannot pump outbound sends.
constexpr int kMaxBusyRejectsPerSession = 8;
constexpr int kMaxPendingOps = 64;
// Bound on offer/answer production, so a hung engine cannot wedge the session.
constexpr qint64 kMediaProductionTimeoutMs = 15000;

QString glareWinner(const QString &a, const QString &b)
{
    // MSC2746 glare rule as in matrix-js-sdk: the lexicographically smaller
    // call id survives, so both peers agree on which call lives.
    return a.compare(b) <= 0 ? a : b;
}
} // namespace

CallController::CallController(QObject *parent)
    : QObject(parent)
{
    m_lifetimeTimer.setSingleShot(true);
    connect(&m_lifetimeTimer, &QTimer::timeout, this,
            &CallController::onLifetimeExpired);
    m_candidateFlushTimer.setSingleShot(true);
    m_candidateFlushTimer.setInterval(150);
    connect(&m_candidateFlushTimer, &QTimer::timeout, this,
            &CallController::flushLocalCandidates);
}

void CallController::onLifetimeExpired()
{
    if (!sessionLive())
        return;
    // Media production timed out before anything was sent: a local failure,
    // nothing to announce.
    if (m_session.offerPending || m_session.answerPending) {
        endSession(EndReason::MediaFailed);
        return;
    }
    if (m_state == State::Inviting && m_session.inviteDispatched
        && m_client) {
        // Outbound expiry announces itself on the wire.
        trackOp(m_client->callHangup(m_session.roomId, m_session.callId,
                                     m_session.ourPartyId,
                                     QStringLiteral("invite_timeout")),
                m_session.callId);
    }
    // Inbound expiry just stops ringing; the caller's timer is authoritative.
    endSession(EndReason::InviteTimeout);
}

void CallController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    // A new client is a new homeserver: drop the TURN cache and any in-flight
    // fetch, or a stale m_turnOp would block every future fetch.
    m_turnOp = 0;
    m_turnExpiryMs = 0;
    m_turnUris.clear();
    m_turnUsername.clear();
    m_turnPassword.clear();
    resetAudioIntent();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::callSignalReceived, this,
            &CallController::onCallSignal);
    connect(m_client, &MatrixClient::callSendFinished, this,
            &CallController::onCallSendFinished);
    connect(m_client, &MatrixClient::loggedOut, this,
            &CallController::onLoggedOut);
    connect(m_client, &MatrixClient::callCandidatesReceived, this,
            &CallController::onRemoteCandidates);
    connect(m_client, &MatrixClient::callTurnServersReceived, this,
            &CallController::onTurnServers);
    // Tell the bridge whether SDP transport is wanted; without a backend the
    // Rust side never queues SDP.
    m_client->setCallMediaCapable(m_mediaBackend != nullptr);
    // Registration order is not guaranteed, so whichever of setClient and
    // setMediaBackend completes the pair pre-fetches TURN; otherwise the first
    // call would have host candidates only.
    if (m_mediaBackend)
        requestTurnServersIfStale();
}

bool CallController::muteControlAvailable() const
{
    // Needs an engine that actually implements mute; the seam's default is a
    // no-op.
    return !m_mediaBackend.isNull()
        && m_mediaBackend->supportsMuteControl();
}

void CallController::setMicrophoneMuted(bool muted)
{
    if (m_microphoneMuted == muted)
        return;
    m_microphoneMuted = muted;
    // Unmuting while deafened lifts the deafen too.
    if (!muted && m_deafened) {
        m_deafened = false;
        applyAudioStateToBackend();
        Q_EMIT audioStateChanged();
        return;
    }
    applyAudioStateToBackend();
    Q_EMIT audioStateChanged();
}

void CallController::toggleMicrophoneMuted()
{
    setMicrophoneMuted(!m_microphoneMuted);
}

void CallController::setDeafened(bool deafened)
{
    if (m_deafened == deafened)
        return;
    if (deafened) {
        // Remember the prior mute so undeafening restores it.
        m_micMutedBeforeDeafen = m_microphoneMuted;
        m_deafened = true;
        m_microphoneMuted = true;
    } else {
        m_deafened = false;
        m_microphoneMuted = m_micMutedBeforeDeafen;
    }
    applyAudioStateToBackend();
    Q_EMIT audioStateChanged();
}

void CallController::toggleDeafened() { setDeafened(!m_deafened); }

void CallController::resetAudioIntent()
{
    // Audio intent survives between calls but not an account change: a
    // deafened state carried across sign-out would leave the next account
    // unable to hear.
    if (!m_microphoneMuted && !m_deafened && !m_micMutedBeforeDeafen)
        return;
    m_microphoneMuted = false;
    m_deafened = false;
    m_micMutedBeforeDeafen = false;
    Q_EMIT audioStateChanged();
}

void CallController::applyAudioStateToBackend()
{
    // Without a live session the intent is kept and applied when one starts.
    if (m_mediaBackend.isNull() || !sessionLive()
        || m_session.callId.isEmpty())
        return;
    m_mediaBackend->setMicrophoneMuted(m_session.callId, m_microphoneMuted);
    m_mediaBackend->setOutputMuted(m_session.callId, m_deafened);
}

bool CallController::mediaBackendAvailable() const
{
    return !m_mediaBackend.isNull();
}

void CallController::setMediaBackend(CallMediaBackend *backend)
{
    if (m_mediaBackend == backend)
        return;
    if (m_mediaBackend) {
        // A live call's media job belongs to the old backend: close it there.
        if (sessionLive())
            m_mediaBackend->close(m_session.callId);
        disconnect(m_mediaBackend, nullptr, this, nullptr);
    }
    m_mediaBackend = backend;
    if (m_mediaBackend) {
        connect(m_mediaBackend, &CallMediaBackend::offerReady, this,
                &CallController::onMediaOfferReady);
        connect(m_mediaBackend, &CallMediaBackend::answerReady, this,
                &CallController::onMediaAnswerReady);
        connect(m_mediaBackend, &CallMediaBackend::connected, this,
                &CallController::onMediaConnected);
        connect(m_mediaBackend, &CallMediaBackend::failed, this,
                &CallController::onMediaFailed);
        connect(m_mediaBackend, &CallMediaBackend::localCandidate, this,
                &CallController::onMediaLocalCandidate);
        connect(m_mediaBackend, &CallMediaBackend::gatheringComplete, this,
                &CallController::onMediaGatheringComplete);
    }
    if (m_client)
        m_client->setCallMediaCapable(m_mediaBackend != nullptr);
    if (m_mediaBackend)
        requestTurnServersIfStale();
    Q_EMIT mediaBackendAvailableChanged();
}

void CallController::requestTurnServersIfStale()
{
    if (!m_client || !m_mediaBackend)
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_turnOp != 0) {
        // A lost result event must not strand the op and disable TURN.
        if (nowMs - m_turnRequestedAtMs < 30000)
            return; // one fetch in flight
        m_turnOp = 0;
    }
    if (m_turnExpiryMs > nowMs + 60000) {
        // Cache still valid: (re)apply it.
        m_mediaBackend->setIceServers(m_turnUris, m_turnUsername,
                                      m_turnPassword);
        return;
    }
    m_turnOp = m_client->requestCallTurnServers();
    m_turnRequestedAtMs = nowMs;
}

void CallController::onTurnServers(quint64 opId, bool ok,
                                   const QString &username,
                                   const QString &password,
                                   const QStringList &uris,
                                   qint64 ttlSeconds,
                                   const QString &category)
{
    if (opId != m_turnOp)
        return;
    m_turnOp = 0;
    if (!ok) {
        // Degrade to host candidates; never a third-party STUN fallback. Log
        // the category only, never credentials.
        qCInfo(lcCalls) << "TURN fetch failed category=" << category;
        return;
    }
    // Bound the homeserver's answer so a broken server cannot stall the GUI
    // thread with huge lists or credentials.
    m_turnUris = uris.mid(0, 16);
    m_turnUsername = username.left(1024);
    m_turnPassword = password.left(1024);
    m_turnExpiryMs = QDateTime::currentMSecsSinceEpoch()
        + qBound<qint64>(qint64(60), ttlSeconds, qint64(86400)) * 1000;
    if (m_mediaBackend)
        m_mediaBackend->setIceServers(m_turnUris, m_turnUsername,
                                      m_turnPassword);
}

void CallController::onRemoteCandidates(const QString &roomId,
                                        const QString &callId,
                                        const QString &partyId, bool own,
                                        const QVariantList &candidates)
{
    if (own)
        return; // our other device's candidates are not for our engine
    if (!m_mediaBackend || !sessionLive() || m_session.rtc
        || roomId != m_session.roomId || callId != m_session.callId)
        return;
    // Once the remote party is known (from the invite or first answer), only
    // its candidates are used: each is a STUN check to an address of the
    // sender's choosing. Before that, any answering device may trickle
    // (MSC2746).
    if (!m_session.remotePartyId.isEmpty()
        && partyId != m_session.remotePartyId)
        return;
    // Inbound and still ringing: the engine has no session until answer(), so
    // hold the trickle. Bounded.
    if (m_session.direction == Direction::Inbound
        && m_state == State::Ringing && !m_session.answerPending) {
        for (const QVariant &value : candidates) {
            if (m_session.earlyRemoteCandidates.size() >= 64)
                break;
            m_session.earlyRemoteCandidates.append(value);
        }
        return;
    }
    for (const QVariant &value : candidates) {
        const QVariantMap entry = value.toMap();
        m_mediaBackend->addRemoteCandidate(
            callId, entry.value(QStringLiteral("candidate")).toString(),
            entry.value(QStringLiteral("sdpMid")).toString(),
            entry.contains(QStringLiteral("sdpMLineIndex"))
                ? entry.value(QStringLiteral("sdpMLineIndex")).toInt()
                : 0);
    }
}

void CallController::onMediaLocalCandidate(const QString &callId,
                                           const QString &candidate,
                                           const QString &sdpMid,
                                           int sdpMLineIndex)
{
    if (!sessionLive() || callId != m_session.callId)
        return;
    if (candidate.trimmed().isEmpty())
        return; // completion is signalled separately
    QVariantMap entry;
    entry.insert(QStringLiteral("candidate"), candidate);
    if (!sdpMid.isEmpty())
        entry.insert(QStringLiteral("sdpMid"), sdpMid);
    entry.insert(QStringLiteral("sdpMLineIndex"), sdpMLineIndex);
    m_pendingLocalCandidates.append(entry);
    // Bounded against a runaway engine.
    while (m_pendingLocalCandidates.size() > 64)
        m_pendingLocalCandidates.removeFirst();
    if (!m_candidateFlushTimer.isActive())
        m_candidateFlushTimer.start();
}

void CallController::onMediaGatheringComplete(const QString &callId)
{
    if (!sessionLive() || callId != m_session.callId)
        return;
    m_gatheringComplete = true;
    m_candidateFlushTimer.stop();
    flushLocalCandidates();
}

void CallController::flushLocalCandidates()
{
    if (!m_client || !sessionLive())
        return;
    if (m_gatheringComplete) {
        // MSC2746 v1: an empty candidate ends the trickle.
        QVariantMap end;
        end.insert(QStringLiteral("candidate"), QString());
        m_pendingLocalCandidates.append(end);
        m_gatheringComplete = false;
    }
    if (m_pendingLocalCandidates.isEmpty())
        return;
    // The Rust side rejects batches over 32 entries whole; chunk them.
    constexpr int kMaxPerEvent = 32;
    while (!m_pendingLocalCandidates.isEmpty()) {
        const QVariantList chunk = m_pendingLocalCandidates.mid(0, kMaxPerEvent);
        m_pendingLocalCandidates =
            m_pendingLocalCandidates.mid(chunk.size());
        trackOp(m_client->callCandidates(m_session.roomId, m_session.callId,
                                         m_session.ourPartyId, chunk),
                m_session.callId);
    }
}

void CallController::setOwnUserId(const QString &userId)
{
    m_ownUserId = userId;
}

QString CallController::activeRoomId() const
{
    return sessionLive() ? m_session.roomId : QString();
}

QString CallController::activeCallId() const
{
    return sessionLive() ? m_session.callId : QString();
}

QString CallController::activeSenderId() const
{
    return sessionLive() ? m_session.senderId : QString();
}

bool CallController::sessionLive() const
{
    switch (m_state) {
    case State::Inviting:
    case State::Ringing:
    case State::Connecting:
    case State::Active:
        return true;
    case State::Idle:
    case State::Ended:
        return false;
    }
    return false;
}

bool CallController::placeCall(const QString &roomId, const QString &invitee)
{
    m_lastRefusal.clear();
    if (!m_mediaBackend) {
        // No backend can produce an offer; a stubbed one would place a call
        // that dies at the peer.
        m_lastRefusal = QStringLiteral("no_media_backend");
        return false;
    }
    if (!m_client || !m_client->supportsCallSignaling()) {
        m_lastRefusal = QStringLiteral("unsupported_backend");
        return false;
    }
    if (roomId.isEmpty()) {
        m_lastRefusal = QStringLiteral("invalid_arguments");
        return false;
    }
    if (sessionLive()) {
        m_lastRefusal = QStringLiteral("call_in_progress");
        return false;
    }
    Session session;
    session.roomId = roomId;
    session.callId = freshPartyId() + freshPartyId();
    session.ourPartyId = freshPartyId();
    session.direction = Direction::Outbound;
    session.lifetimeMs = 60000;
    session.offerPending = true;
    session.invitee = invitee;
    session.remoteUserId = invitee;
    m_session = session;
    m_busyRejectsThisSession = 0;
    m_endReason = EndReason::None;
    setState(State::Inviting);
    requestTurnServersIfStale();
    // The invite is sent once the backend delivers the offer; until then the
    // timer bounds offer production, then re-arms for the wire lifetime.
    armLifetimeTimer(kMediaProductionTimeoutMs);
    m_mediaBackend->createOffer(m_session.callId);
    // Apply the user's mute/deafen intent now: the pipeline starts unmuted and
    // `connected` arrives through a queued signal after RTP is already
    // flowing.
    applyAudioStateToBackend();
    return true;
}

bool CallController::answer()
{
    m_lastRefusal.clear();
    if (m_state != State::Ringing) {
        m_lastRefusal = QStringLiteral("not_ringing");
        return false;
    }
    if (!m_mediaBackend) {
        m_lastRefusal = QStringLiteral("no_media_backend");
        return false;
    }
    if (m_session.rtc) {
        // A MatrixRTC ring is answered by joining the session
        // (SfuCallController::join), not through this legacy `m.call.*` path:
        // there is no offer to answer, and an m.call.invite in reply would
        // ring every member of the room. The surface routes by `rtcRing`.
        // CallControllerTest pins this token.
        m_lastRefusal = QStringLiteral("rtc_unsupported");
        return false;
    }
    if (!m_client) {
        m_lastRefusal = QStringLiteral("unsupported_backend");
        return false;
    }
    // Single-shot take: the remote offer lives in memory only while the
    // answer is produced.
    const QString remoteOffer =
        m_client->takeCallSessionDescription(m_session.inviteEventId);
    if (remoteOffer.trimmed().isEmpty()) {
        m_lastRefusal = QStringLiteral("no_remote_offer");
        return false;
    }
    m_session.answerPending = true;
    requestTurnServersIfStale();
    // The user acted, so the timer now bounds answer production instead.
    armLifetimeTimer(kMediaProductionTimeoutMs);
    m_mediaBackend->createAnswer(m_session.callId, remoteOffer);
    // Seed the audio intent before media can flow, as for outbound.
    applyAudioStateToBackend();
    // Drain candidates the caller trickled while we were ringing.
    const QVariantList early = m_session.earlyRemoteCandidates;
    m_session.earlyRemoteCandidates.clear();
    for (const QVariant &value : early) {
        const QVariantMap entry = value.toMap();
        m_mediaBackend->addRemoteCandidate(
            m_session.callId,
            entry.value(QStringLiteral("candidate")).toString(),
            entry.value(QStringLiteral("sdpMid")).toString(),
            entry.contains(QStringLiteral("sdpMLineIndex"))
                ? entry.value(QStringLiteral("sdpMLineIndex")).toInt()
                : 0);
    }
    return true;
}

bool CallController::placeCallWithOffer(const QString &roomId,
                                        const QString &offerSdp,
                                        qint64 lifetimeMs,
                                        const QString &invitee)
{
    m_lastRefusal.clear();
    if (!m_client || !m_client->supportsCallSignaling()) {
        m_lastRefusal = QStringLiteral("unsupported_backend");
        return false;
    }
    if (roomId.isEmpty() || offerSdp.trimmed().isEmpty()) {
        m_lastRefusal = QStringLiteral("invalid_arguments");
        return false;
    }
    if (sessionLive()) {
        m_lastRefusal = QStringLiteral("call_in_progress");
        return false;
    }
    const qint64 lifetime =
        qBound(kMinLifetimeMs, lifetimeMs, kMaxLifetimeMs);
    Session session;
    session.roomId = roomId;
    session.callId = freshPartyId() + freshPartyId();
    session.ourPartyId = freshPartyId();
    session.direction = Direction::Outbound;
    session.lifetimeMs = lifetime;
    session.invitee = invitee;
    session.remoteUserId = invitee;
    const quint64 opId = m_client->callInvite(
        roomId, session.callId, session.ourPartyId, QStringLiteral("offer"),
        offerSdp, static_cast<quint64>(lifetime), invitee);
    if (opId == 0) {
        m_lastRefusal = QStringLiteral("dispatch_failed");
        return false;
    }
    trackOp(opId, session.callId);
    session.inviteDispatched = true; // the invite went out synchronously
    m_session = session;
    m_busyRejectsThisSession = 0;
    m_endReason = EndReason::None;
    setState(State::Inviting);
    armLifetimeTimer(lifetime);
    qCInfo(lcCalls) << "outbound call invite dispatched call_id="
                    << m_session.callId;
    return true;
}

bool CallController::rejectIncoming()
{
    if (m_state != State::Ringing || !m_client)
        return false;
    if (m_session.rtc) {
        trackOp(m_client->callRtcDecline(m_session.roomId,
                                         m_session.inviteEventId),
                m_session.callId);
    } else {
        trackOp(m_client->callReject(m_session.roomId, m_session.callId,
                                     m_session.ourPartyId),
                m_session.callId);
    }
    endSession(EndReason::LocalReject);
    return true;
}

void CallController::noteAnsweredByOtherLane(const QString &roomId)
{
    // Only an unanswered ring is affected; a connected legacy call is not torn
    // down because a group call started in the same room.
    if (m_state != State::Ringing || roomId.isEmpty())
        return;
    if (m_session.roomId != roomId)
        return;
    qCInfo(lcCalls) << "ring cleared: answered through the MatrixRTC lane";
    // AnsweredElsewhere is exactly this fact and is not a missed call.
    endSession(EndReason::AnsweredElsewhere);
}

bool CallController::hangup()
{
    if (!m_client || !sessionLive())
        return false;
    // Outbound calls can hang up from Inviting on; an inbound call only once
    // answered (before that, use rejectIncoming()).
    const bool answerable = m_session.direction == Direction::Outbound
        || m_state == State::Connecting || m_state == State::Active;
    if (!answerable)
        return false;
    // An offer still in production was never sent; there is nothing to hang
    // up on the wire.
    const bool announced = m_session.inviteDispatched
        || m_session.direction == Direction::Inbound;
    if (announced) {
        trackOp(m_client->callHangup(m_session.roomId, m_session.callId,
                                     m_session.ourPartyId,
                                     QStringLiteral("user_hangup")),
                m_session.callId);
    }
    endSession(EndReason::LocalHangup);
    return true;
}

void CallController::setBacklogSuppressed(bool suppressed)
{
    m_backlogSuppressed = suppressed;
}

void CallController::setSenderIgnoredCheck(
    std::function<bool(const QString &)> check)
{
    m_senderIgnoredCheck = std::move(check);
}

void CallController::setRoomMutedCheck(
    std::function<bool(const QString &)> check)
{
    m_roomMutedCheck = std::move(check);
}

bool CallController::shouldRing() const
{
    if (m_state != State::Ringing)
        return false;
    if (m_backlogSuppressed)
        return false;
    if (m_senderIgnoredCheck && m_senderIgnoredCheck(m_session.senderId))
        return false;
    if (m_roomMutedCheck && m_roomMutedCheck(m_session.roomId))
        return false;
    return true;
}

void CallController::onCallSignal(const CallSignal &signal)
{
    switch (signal.kind) {
    case CallSignal::Kind::Invite:          handleInvite(signal); break;
    case CallSignal::Kind::Answer:          handleAnswer(signal); break;
    case CallSignal::Kind::Hangup:          handleHangup(signal); break;
    case CallSignal::Kind::Reject:          handleReject(signal); break;
    case CallSignal::Kind::SelectAnswer:    handleSelectAnswer(signal); break;
    case CallSignal::Kind::RtcNotification: handleRtcNotification(signal); break;
    case CallSignal::Kind::RtcDecline:      handleRtcDecline(signal); break;
    }
}

void CallController::onCallSendFinished(quint64 opId, bool ok,
                                        const QString &category,
                                        const QString &callId,
                                        const QString &eventId)
{
    Q_UNUSED(callId);
    const auto it = m_pendingOps.constFind(opId);
    if (it == m_pendingOps.constEnd())
        return; // not ours / stale
    // Use the call this op was dispatched for, never the echo: a stale result
    // from an ended call must not touch the current one.
    const QString opCallId = it.value();
    m_pendingOps.erase(it);
    m_pendingOpOrder.removeOne(opId);
    const bool forLiveSession =
        sessionLive() && opCallId == m_session.callId;
    if (ok) {
        if (forLiveSession && m_state == State::Inviting
            && m_session.inviteEventId.isEmpty())
            m_session.inviteEventId = eventId;
        return;
    }
    qCWarning(lcCalls) << "call signaling send failed category=" << category;
    if (!forLiveSession)
        return; // an already-ended call's failure is not actionable
    Q_EMIT sendFailed(category);
    // An outbound invite that never reached the server is a dead call.
    if (m_state == State::Inviting)
        endSession(EndReason::SendFailed);
}

void CallController::onLoggedOut()
{
    m_pendingOps.clear();
    m_pendingOpOrder.clear();
    m_busyRejectsThisSession = 0;
    m_lifetimeTimer.stop();
    m_candidateFlushTimer.stop();
    m_pendingLocalCandidates.clear();
    m_gatheringComplete = false;
    m_turnOp = 0;
    m_turnExpiryMs = 0;
    m_turnUris.clear();
    m_turnUsername.clear();
    m_turnPassword.clear();
    // Clear the backend's TURN credentials too, or the next account's first
    // call would relay through the previous account's server.
    if (m_mediaBackend)
        m_mediaBackend->setIceServers({}, {}, {});
    resetAudioIntent();
    if (sessionLive())
        endSession(EndReason::SessionLost);
    else {
        m_session = Session();
        m_endReason = EndReason::None;
        setState(State::Idle);
    }
    // After ending: endSession records the id, and that memory must not
    // survive into the next account.
    m_recentEnded.clear();
}

void CallController::handleInvite(const CallSignal &signal)
{
    if (signal.own)
        return; // our own invite echoing back, or another device's
    if (recentlyEnded(signal.callId))
        return;
    // An ignored sender gets nothing from us, not even a reject: any outbound
    // event would confirm we are online (see
    // NotificationManager::senderIsIgnored).
    if (m_senderIgnoredCheck && m_senderIgnoredCheck(signal.sender))
        return;
    // Sync can re-deliver the invite we are already handling; without this
    // the busy branch would reject our own live call.
    if (sessionLive() && signal.roomId == m_session.roomId
        && signal.callId == m_session.callId)
        return;
    // A dual-stack caller may announce one call on both lanes (an
    // m.rtc.notification and an m.call.invite, with different ids). Treat it
    // as the same conversation: no second ring and no busy reject telling the
    // caller "declined". Adopting the legacy leg would change session
    // identity mid-ring and is not done.
    if (m_state == State::Ringing && m_session.rtc
        && signal.roomId == m_session.roomId
        && !signal.sender.isEmpty() && signal.sender == m_session.senderId)
        return;
    // Targeted invite for a different (known) user: not ours to ring.
    const QString ownUser = ownUserId();
    if (!signal.invitee.isEmpty() && !ownUser.isEmpty()
        && signal.invitee != ownUser)
        return;
    const qint64 remaining = remainingInviteMs(
        signal.originServerTs, 0, signal.lifetimeMs);
    if (remaining <= 0) {
        // Cold-start backlog: an expired invite is dropped without ringing or
        // sending anything.
        return;
    }
    if (sessionLive()) {
        if (m_state == State::Inviting && m_session.roomId == signal.roomId) {
            // Glare: both sides invited each other. Smaller call id wins.
            if (glareWinner(m_session.callId, signal.callId)
                == signal.callId) {
                // Theirs survives: retire ours on the wire, but only if our
                // invite actually went out.
                if (m_client && m_session.inviteDispatched) {
                    trackOp(m_client->callHangup(
                                m_session.roomId, m_session.callId,
                                m_session.ourPartyId,
                                QStringLiteral("replaced")),
                            m_session.callId);
                }
                endSession(EndReason::GlareReplaced);
                // fall through to adopt the surviving invite below
            } else {
                // Ours survives: reject theirs.
                if (m_client) {
                    trackOp(m_client->callReject(signal.roomId, signal.callId,
                                                 freshPartyId()),
                            signal.callId);
                }
                return;
            }
        } else {
            // Busy: reject without touching the live session. Bounded per
            // session because this send is remotely triggered; beyond the cap
            // the invite is dropped silently.
            if (m_client
                && m_busyRejectsThisSession < kMaxBusyRejectsPerSession) {
                ++m_busyRejectsThisSession;
                trackOp(m_client->callReject(signal.roomId, signal.callId,
                                             freshPartyId()),
                        signal.callId);
            }
            return;
        }
    }
    Session session;
    session.roomId = signal.roomId;
    session.callId = signal.callId;
    session.ourPartyId = freshPartyId();
    session.remotePartyId = signal.partyId;
    session.senderId = signal.sender;
    session.remoteUserId = signal.sender;
    session.inviteEventId = signal.eventId;
    session.invitee = signal.invitee;
    session.direction = Direction::Inbound;
    session.lifetimeMs = signal.lifetimeMs;
    m_session = session;
    m_busyRejectsThisSession = 0;
    m_endReason = EndReason::None;
    setState(State::Ringing);
    armLifetimeTimer(remaining);
    Q_EMIT incomingCallStarted(signal.roomId, signal.callId, signal.sender,
                               remaining);
}

void CallController::handleAnswer(const CallSignal &signal)
{
    if (!matchesSession(signal))
        return;
    if (signal.own) {
        // Another of OUR devices answered the call we are ringing for.
        if (m_state == State::Ringing
            && signal.partyId != m_session.ourPartyId)
            endSession(EndReason::AnsweredElsewhere);
        return;
    }
    if (m_state != State::Inviting)
        return;
    if (!fromExpectedPeer(signal))
        return;
    if (m_session.remotePartyId.isEmpty()) {
        // The first answer locks the party (MSC2746) and the user, and is
        // selected on the wire exactly once.
        m_session.remotePartyId = signal.partyId;
        m_session.remoteUserId = signal.sender;
        if (m_client && !m_session.selectAnswerSent) {
            m_session.selectAnswerSent = true;
            trackOp(m_client->callSelectAnswer(
                        m_session.roomId, m_session.callId,
                        m_session.ourPartyId, signal.partyId),
                    m_session.callId);
        }
        m_lifetimeTimer.stop();
        setState(State::Connecting);
        // Hand the peer's answer to the media backend (single-shot take from
        // the bridge; empty without a backend).
        if (m_mediaBackend && m_client) {
            const QString remoteAnswer =
                m_client->takeCallSessionDescription(signal.eventId);
            if (!remoteAnswer.trimmed().isEmpty())
                m_mediaBackend->setRemoteAnswer(m_session.callId,
                                                remoteAnswer);
        }
        return;
    }
    // A later answer from a different party is ignored; the lock stands.
}

void CallController::handleHangup(const CallSignal &signal)
{
    if (!matchesSession(signal))
        return;
    if (signal.own) {
        if (m_state == State::Ringing
            && signal.partyId != m_session.ourPartyId)
            endSession(EndReason::DeclinedElsewhere);
        return;
    }
    // Only the peer can hang up; otherwise a stranger could end the call or,
    // while ringing, fake a missed call.
    if (!fromExpectedPeer(signal))
        return;
    endSession(EndReason::RemoteHangup);
}

void CallController::handleReject(const CallSignal &signal)
{
    if (!matchesSession(signal))
        return;
    if (signal.own) {
        if (m_state == State::Ringing
            && signal.partyId != m_session.ourPartyId)
            endSession(EndReason::DeclinedElsewhere);
        return;
    }
    if (!fromExpectedPeer(signal))
        return;
    if (m_state == State::Inviting)
        endSession(EndReason::RemoteReject);
}

void CallController::handleSelectAnswer(const CallSignal &signal)
{
    if (!matchesSession(signal))
        return;
    // The caller locked onto some answer. If we are still ringing it was not
    // ours (answering moves us to Connecting when the answer is sent), so the
    // call settled elsewhere. Only the caller may select an answer.
    if (!signal.own && !fromExpectedPeer(signal))
        return;
    if (m_state == State::Ringing
        && signal.selectedPartyId != m_session.ourPartyId)
        endSession(EndReason::AnsweredElsewhere);
}

void CallController::handleRtcNotification(const CallSignal &signal)
{
    if (signal.own)
        return;
    if (recentlyEnded(signal.eventId))
        return;
    if (m_senderIgnoredCheck && m_senderIgnoredCheck(signal.sender))
        return;
    const qint64 remaining = remainingInviteMs(
        signal.originServerTs, signal.senderTs, signal.lifetimeMs);
    if (remaining <= 0)
        return;
    if (sessionLive()) {
        // No auto-decline: an m.rtc.decline stops the ring on every one of our
        // devices, and being busy here must not silence the others.
        return;
    }
    Session session;
    session.roomId = signal.roomId;
    // MSC4075 has no call id; the notification event id is the key.
    session.callId = signal.eventId;
    session.senderId = signal.sender;
    session.inviteEventId = signal.eventId;
    session.direction = Direction::Inbound;
    session.rtc = true;
    session.lifetimeMs = signal.lifetimeMs;
    m_session = session;
    m_busyRejectsThisSession = 0;
    m_endReason = EndReason::None;
    setState(State::Ringing);
    armLifetimeTimer(remaining);
    Q_EMIT incomingCallStarted(signal.roomId, session.callId, signal.sender,
                               remaining);
}

void CallController::handleRtcDecline(const CallSignal &signal)
{
    if (!signal.own)
        return; // a peer declining someone's ring is not our concern
    if (m_state == State::Ringing && m_session.rtc
        && signal.targetEventId == m_session.inviteEventId)
        endSession(EndReason::DeclinedElsewhere);
}

void CallController::onMediaOfferReady(const QString &callId,
                                       const QString &sdp)
{
    if (!sessionLive() || m_state != State::Inviting
        || callId != m_session.callId || !m_session.offerPending)
        return;
    m_session.offerPending = false;
    if (!m_client || sdp.trimmed().isEmpty()) {
        endSession(EndReason::MediaFailed);
        return;
    }
    const quint64 opId = m_client->callInvite(
        m_session.roomId, m_session.callId, m_session.ourPartyId,
        QStringLiteral("offer"), sdp,
        static_cast<quint64>(m_session.lifetimeMs), m_session.invitee);
    if (opId == 0) {
        endSession(EndReason::SendFailed);
        return;
    }
    trackOp(opId, m_session.callId);
    m_session.inviteDispatched = true;
    armLifetimeTimer(m_session.lifetimeMs);
}

void CallController::onMediaAnswerReady(const QString &callId,
                                        const QString &sdp)
{
    if (m_state != State::Ringing || callId != m_session.callId
        || !m_session.answerPending)
        return;
    m_session.answerPending = false;
    if (!m_client || sdp.trimmed().isEmpty()) {
        endSession(EndReason::MediaFailed);
        return;
    }
    const quint64 opId = m_client->callAnswer(
        m_session.roomId, m_session.callId, m_session.ourPartyId,
        QStringLiteral("answer"), sdp);
    if (opId == 0) {
        endSession(EndReason::SendFailed);
        return;
    }
    trackOp(opId, m_session.callId);
    m_lifetimeTimer.stop();
    setState(State::Connecting);
}

void CallController::onMediaConnected(const QString &callId)
{
    if (m_state != State::Connecting || callId != m_session.callId)
        return;
    setState(State::Active);
    // The engine's mute state starts clean per session, so push the user's
    // intent down, or a muted user is live on connect.
    applyAudioStateToBackend();
}

void CallController::onMediaFailed(const QString &callId,
                                   const QString &category)
{
    Q_UNUSED(category); // coarse label; the end reason is the record
    if (!sessionLive() || callId != m_session.callId)
        return;
    // Announce when the peer may still be waiting on us (an invite sent, or
    // an inbound call we began answering). user_media_failed is MSC2746's
    // reason for this.
    const bool announced = m_session.inviteDispatched
        || m_state == State::Connecting || m_state == State::Active
        || (m_state == State::Ringing && m_session.answerPending);
    if (announced && m_client && !m_session.rtc) {
        trackOp(m_client->callHangup(m_session.roomId, m_session.callId,
                                     m_session.ourPartyId,
                                     QStringLiteral("user_media_failed")),
                m_session.callId);
    }
    endSession(EndReason::MediaFailed);
}

bool CallController::matchesSession(const CallSignal &signal) const
{
    return sessionLive() && !m_session.rtc
        && signal.roomId == m_session.roomId
        && signal.callId == m_session.callId;
}

bool CallController::fromExpectedPeer(const CallSignal &signal) const
{
    // (room, call_id) identifies the call, not who may speak for the other
    // side (call_id is readable by every room member). Once known, only the
    // peer counts, as in matrix-js-sdk (`getSender() !== getOpponentMember()`).
    if (m_session.remoteUserId.isEmpty())
        return true; // outbound, no invitee, no answer yet: nobody to bind to
    return signal.sender == m_session.remoteUserId;
}

bool CallController::recentlyEnded(const QString &callId) const
{
    return m_recentEnded.contains(callId);
}

void CallController::rememberEnded(const QString &callId)
{
    if (callId.isEmpty())
        return;
    m_recentEnded.removeOne(callId);
    m_recentEnded.append(callId);
    while (m_recentEnded.size() > kRecentEndedCap)
        m_recentEnded.removeFirst();
}

void CallController::endSession(EndReason reason)
{
    if (!sessionLive())
        return;
    m_lifetimeTimer.stop();
    rememberEnded(m_session.callId);
    const QString callId = m_session.callId;
    const QString roomId = m_session.roomId;
    const bool wasInbound = m_session.direction == Direction::Inbound;
    // Decided here while the pre-end state is known: an answered call is
    // never missed.
    const bool missed = wasInbound && m_state == State::Ringing
        && isMissedCallReason(reason);
    // Discard any unconsumed remote offer; it must not outlive its call.
    if (m_client && !m_session.inviteEventId.isEmpty())
        m_client->takeCallSessionDescription(m_session.inviteEventId);
    m_candidateFlushTimer.stop();
    m_pendingLocalCandidates.clear();
    m_gatheringComplete = false;
    if (m_mediaBackend)
        m_mediaBackend->close(callId);
    m_endReason = reason;
    setState(State::Ended);
    if (wasInbound)
        Q_EMIT incomingCallEnded(roomId, callId, static_cast<int>(reason),
                                 missed);
    qCInfo(lcCalls) << "call ended reason=" << static_cast<int>(reason);
}

void CallController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void CallController::armLifetimeTimer(qint64 remainingMs)
{
    // Clamped so clock skew cannot arm a wild timer.
    const qint64 bounded =
        qBound<qint64>(0, remainingMs, kMaxLifetimeMs);
    m_lifetimeTimer.start(static_cast<int>(bounded));
}

qint64 CallController::remainingInviteMs(qint64 originServerTs,
                                         qint64 senderTs,
                                         qint64 lifetimeMs) const
{
    const qint64 lifetime = qBound(kMinLifetimeMs,
                                   lifetimeMs > 0 ? lifetimeMs : kMinLifetimeMs,
                                   kMaxLifetimeMs);
    // MSC4075: prefer the sender's clock unless it disagrees with the
    // server's by too much; MSC2746 invites carry only the server ts.
    qint64 base = originServerTs;
    if (senderTs > 0
        && (originServerTs <= 0
            || qAbs(senderTs - originServerTs) <= kSenderClockSkewToleranceMs))
        base = senderTs;
    if (base <= 0)
        return lifetime; // no usable timestamp: assume fresh
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - base;
    return qBound<qint64>(0, lifetime - elapsed, lifetime);
}

void CallController::trackOp(quint64 opId, const QString &callId)
{
    if (opId == 0)
        return;
    m_pendingOps.insert(opId, callId);
    m_pendingOpOrder.append(opId);
    while (m_pendingOpOrder.size() > kMaxPendingOps)
        m_pendingOps.remove(m_pendingOpOrder.takeFirst());
}

QString CallController::ownUserId() const
{
    if (!m_ownUserId.isEmpty())
        return m_ownUserId;
    return m_client ? m_client->currentUserId() : QString();
}

QString CallController::freshPartyId()
{
    // Opaque random VoIP id, never the (identifying) device id.
    return QUuid::createUuid().toString(QUuid::Id128).left(16);
}
