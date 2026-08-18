#include "CallController.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QUuid>

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
// At most this many busy auto-rejects per live session; beyond it,
// unsolicited invites are dropped silently (still no ring, no state
// change) so a hostile sender cannot pump outbound sends.
constexpr int kMaxBusyRejectsPerSession = 8;
constexpr int kMaxPendingOps = 64;

QString glareWinner(const QString &a, const QString &b)
{
    // MSC2746 glare rule as implemented by matrix-js-sdk: the
    // lexicographically smaller call id survives, so both peers reach
    // opposite, consistent conclusions and exactly one call lives.
    return a.compare(b) <= 0 ? a : b;
}
} // namespace

CallController::CallController(QObject *parent)
    : QObject(parent)
{
    m_lifetimeTimer.setSingleShot(true);
    connect(&m_lifetimeTimer, &QTimer::timeout, this, [this] {
        if (!sessionLive())
            return;
        if (m_state == State::Inviting && m_client) {
            // Outbound expiry announces itself on the wire.
            trackOp(m_client->callHangup(m_session.roomId, m_session.callId,
                                         m_session.ourPartyId,
                                         QStringLiteral("invite_timeout")),
                    m_session.callId);
        }
        // Inbound expiry just stops ringing — the caller's own timer is
        // authoritative for the wire.
        endSession(EndReason::InviteTimeout);
    });
}

void CallController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::callSignalReceived, this,
            &CallController::onCallSignal);
    connect(m_client, &MatrixClient::callSendFinished, this,
            &CallController::onCallSendFinished);
    connect(m_client, &MatrixClient::loggedOut, this,
            &CallController::onLoggedOut);
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

bool CallController::placeCall(const QString &roomId)
{
    Q_UNUSED(roomId);
    // Honest refusal: no media backend exists in the tree, so there is
    // nothing that can produce an offer SDP. A stubbed offer would place a
    // call that dies at the peer — worse than refusing.
    m_lastRefusal = QStringLiteral("no_media_backend");
    return false;
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
    const quint64 opId = m_client->callInvite(
        roomId, session.callId, session.ourPartyId, QStringLiteral("offer"),
        offerSdp, static_cast<quint64>(lifetime), invitee);
    if (opId == 0) {
        m_lastRefusal = QStringLiteral("dispatch_failed");
        return false;
    }
    trackOp(opId, session.callId);
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

bool CallController::hangup()
{
    if (!m_client || !sessionLive() || m_session.direction != Direction::Outbound)
        return false;
    trackOp(m_client->callHangup(m_session.roomId, m_session.callId,
                                 m_session.ourPartyId,
                                 QStringLiteral("user_hangup")),
            m_session.callId);
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
    // The call this op was dispatched FOR — never trust the echo to name
    // the live session (a stale result from an ended call must not mutate
    // or end the current one; review 2026-08-18).
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
    if (sessionLive())
        endSession(EndReason::SessionLost);
    else {
        m_session = Session();
        m_endReason = EndReason::None;
        setState(State::Idle);
    }
    // AFTER ending: endSession records the id in the retired-call LRU, and
    // that memory must not survive into the next account's session.
    m_recentEnded.clear();
}

void CallController::handleInvite(const CallSignal &signal)
{
    if (signal.own)
        return; // our own outbound invite echoing back, or another device's
    if (recentlyEnded(signal.callId))
        return;
    // An ignored sender must elicit NOTHING from us — not a ring, and not
    // a reject either: any outbound event would confirm we are online
    // during the window before the server stops delivering their events
    // (the same race NotificationManager::senderIsIgnored closes).
    if (m_senderIgnoredCheck && m_senderIgnoredCheck(signal.sender))
        return;
    // Re-delivery of the invite we are already handling (sync can deliver
    // an event more than once, e.g. via the active-room subscription) is
    // idempotent — without this, the busy branch below would REJECT our
    // own live call while we kept ringing it.
    if (sessionLive() && signal.roomId == m_session.roomId
        && signal.callId == m_session.callId)
        return;
    // Targeted invite for a different (known) user: not ours to ring.
    const QString ownUser = ownUserId();
    if (!signal.invitee.isEmpty() && !ownUser.isEmpty()
        && signal.invitee != ownUser)
        return;
    const qint64 remaining = remainingInviteMs(
        signal.originServerTs, 0, signal.lifetimeMs);
    if (remaining <= 0) {
        // Cold-start backlog: an expired invite is dropped without ringing
        // and without any event sent (NotificationManager's backlog rule).
        return;
    }
    if (sessionLive()) {
        if (m_state == State::Inviting && m_session.roomId == signal.roomId) {
            // Glare: both sides invited each other. Smaller call id wins.
            if (glareWinner(m_session.callId, signal.callId)
                == signal.callId) {
                // Theirs survives: retire ours on the wire, adopt theirs.
                if (m_client) {
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
            // Busy: exactly one live session. Reject without touching it —
            // BOUNDED per session, because this send is remotely triggered
            // with zero user interaction; beyond the cap the invite is
            // dropped silently (still no ring, no state change).
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
    session.inviteEventId = signal.eventId;
    session.invitee = signal.invitee;
    session.direction = Direction::Inbound;
    session.lifetimeMs = signal.lifetimeMs;
    m_session = session;
    m_busyRejectsThisSession = 0;
    m_endReason = EndReason::None;
    setState(State::Ringing);
    armLifetimeTimer(remaining);
    Q_EMIT incomingCallStarted(signal.roomId, signal.callId, signal.sender);
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
    if (m_session.remotePartyId.isEmpty()) {
        // First answer locks the party (MSC2746 multi-device rule) and is
        // named on the wire exactly once.
        m_session.remotePartyId = signal.partyId;
        if (m_client && !m_session.selectAnswerSent) {
            m_session.selectAnswerSent = true;
            trackOp(m_client->callSelectAnswer(
                        m_session.roomId, m_session.callId,
                        m_session.ourPartyId, signal.partyId),
                    m_session.callId);
        }
        m_lifetimeTimer.stop();
        setState(State::Connecting);
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
    if (m_state == State::Inviting)
        endSession(EndReason::RemoteReject);
}

void CallController::handleSelectAnswer(const CallSignal &signal)
{
    if (!matchesSession(signal))
        return;
    // The caller locked onto some party's answer. If we are ringing and it
    // is not us (it never is — we cannot answer), the call is settled
    // elsewhere.
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
        // Deliberately NO auto-decline: an m.rtc.decline is a user action
        // that stops the ring on EVERY device of ours. Being busy on this
        // device must not silence the others.
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
    Q_EMIT incomingCallStarted(signal.roomId, session.callId, signal.sender);
}

void CallController::handleRtcDecline(const CallSignal &signal)
{
    if (!signal.own)
        return; // a peer declining someone's ring is not our concern
    if (m_state == State::Ringing && m_session.rtc
        && signal.targetEventId == m_session.inviteEventId)
        endSession(EndReason::DeclinedElsewhere);
}

bool CallController::matchesSession(const CallSignal &signal) const
{
    return sessionLive() && !m_session.rtc
        && signal.roomId == m_session.roomId
        && signal.callId == m_session.callId;
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
    const bool wasInbound = m_session.direction == Direction::Inbound;
    m_endReason = reason;
    setState(State::Ended);
    if (wasInbound)
        Q_EMIT incomingCallEnded(callId, static_cast<int>(reason));
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
    // Clamped so clock skew can never arm a wild timer.
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
        return lifetime; // no usable timestamp: assume fresh, full window
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
    // Opaque random VoIP id — never the device id (which is identifying).
    return QUuid::createUuid().toString(QUuid::Id128).left(16);
}
