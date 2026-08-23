#include "calls/RtcController.h"

#include <QVariantMap>
#include <algorithm>

#include "matrix/MatrixClient.h"

namespace {
/// Facepiles and banners never need more than a handful, and the Rust side
/// already caps a session at 128 devices. This is the presentation bound.
constexpr int kMaxPresentedParticipants = 64;
} // namespace

RtcController::RtcController(QObject *parent) : QObject(parent)
{
    m_pokeTimer.setSingleShot(true);
    connect(&m_pokeTimer, &QTimer::timeout, this, &RtcController::flushPokes);
}

void RtcController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client) {
        disconnect(m_client, nullptr, this, nullptr);
    }
    m_client = client;
    // A new client is a new account (or none). Everything observed belonged
    // to the previous one.
    clearForNewSession();
    if (m_client) {
        connect(m_client, &MatrixClient::rtcSessionReceived, this,
                &RtcController::onSessionReceived);
        connect(m_client, &MatrixClient::rtcSessionChanged, this,
                &RtcController::onSessionPoked);
        connect(m_client, &MatrixClient::rtcTransportsReceived, this,
                &RtcController::onTransportsReceived);
        // Sign-out must forget observed calls immediately: a participant
        // list is other people's presence in a room this account may no
        // longer be in.
        connect(m_client, &MatrixClient::loggedOut, this,
                [this]() { clearForNewSession(); });
    }
    Q_EMIT availabilityChanged();
}

void RtcController::clearForNewSession()
{
    m_sessions.clear();
    // Every outstanding read belonged to the account that is going away.
    // This IS the isolation (see the header): an account switch reuses the
    // same client and emits loggedOut, which lands here.
    m_pendingReads.clear();
    m_roomsBeingRead.clear();
    m_pokedRooms.clear();
    m_pokeTimer.stop();
    m_discovered = false;
    m_serverAnswered = false;
    m_serviceUrls.clear();
    m_participantFocus.clear();
    m_availabilityCategory.clear();
    m_discoveryOp = 0;
    Q_EMIT availabilityChanged();
}

void RtcController::setReadTimeoutMsForTest(int ms)
{
    m_readTimeoutMs = ms < 0 ? 0 : ms;
}

void RtcController::reapStaleReads()
{
    // A reply can legitimately never arrive: the Rust event queue drops the
    // oldest event on overflow. Without this, the room stays in
    // m_roomsBeingRead forever (never refreshable again) and flushPokes
    // re-arms the timer every tick for the rest of the session.
    if (m_readTimeoutMs <= 0)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        if (now - it->dispatchedAtMs > m_readTimeoutMs) {
            m_roomsBeingRead.remove(it->roomId);
            it = m_pendingReads.erase(it);
        } else {
            ++it;
        }
    }
}

bool RtcController::supported() const
{
    return m_client && m_client->supportsMatrixRtc();
}

bool RtcController::callingAvailable() const
{
    // Availability is a POSITIVE fact: a transport was actually named. An
    // unanswered or failed discovery is not availability, and neither is a
    // server that answered with an empty list.
    return supported() && m_discovered
        && (!m_serviceUrls.isEmpty() || !m_participantFocus.isEmpty());
}

bool RtcController::transportReachableFor(const QString &roomId) const
{
    // The homeserver's answer applies everywhere; a participant-advertised
    // focus applies ONLY to the room whose participants advertised it.
    return !m_serviceUrls.isEmpty()
        || !m_participantFocus.value(roomId).isEmpty();
}

void RtcController::setPokeCoalesceMsForTest(int ms)
{
    m_pokeCoalesceMs = ms < 0 ? 0 : ms;
}

void RtcController::refresh(const QString &roomId)
{
    if (roomId.isEmpty() || !supported())
        return;
    // One read per room at a time: a poke burst must not fan out into N
    // identical reads. The burst is still not lost — flushPokes() re-pokes
    // a room whose read was in flight.
    reapStaleReads();
    if (m_roomsBeingRead.contains(roomId))
        return;
    const quint64 opId = m_client->rtcSession(roomId);
    if (opId == 0)
        return;
    m_pendingReads.insert(
        opId, PendingRead{roomId, QDateTime::currentMSecsSinceEpoch()});
    m_roomsBeingRead.insert(roomId);
}

void RtcController::discover(const QString &roomId)
{
    if (!supported())
        return;
    const quint64 opId = m_client->rtcTransports(roomId);
    if (opId == 0)
        return;
    // Only the newest discovery counts; an older reply must not overwrite
    // it. The epoch is carried too, for the same reason reads carry one: a
    // replacement client restarts its op counter, so an id match alone
    // cannot prove the reply belongs to this account.
    m_discoveryOp = opId;
    m_discoveryRoomId = roomId;
}

void RtcController::onSessionPoked(const QString &roomId)
{
    if (roomId.isEmpty() || !supported())
        return;
    m_pokedRooms.insert(roomId);
    if (m_pokeCoalesceMs <= 0) {
        flushPokes();
        return;
    }
    // Restarting the timer on every poke coalesces a burst into ONE read
    // once the burst stops, instead of reading once per membership event.
    m_pokeTimer.start(m_pokeCoalesceMs);
}

void RtcController::flushPokes()
{
    const QSet<QString> rooms = std::move(m_pokedRooms);
    m_pokedRooms.clear();
    reapStaleReads();
    for (const QString &roomId : rooms) {
        if (m_roomsBeingRead.contains(roomId)) {
            // A read is already in flight and will return state from BEFORE
            // this poke. Re-poke so the change is not lost. Bounded by the
            // reap above: a read whose reply never arrives releases its
            // room instead of re-arming this timer forever.
            m_pokedRooms.insert(roomId);
            continue;
        }
        refresh(roomId);
    }
    if (!m_pokedRooms.isEmpty() && m_pokeCoalesceMs > 0)
        m_pokeTimer.start(m_pokeCoalesceMs);
}

void RtcController::onSessionReceived(quint64 opId,
                                      const RtcSessionData &session)
{
    const auto pending = m_pendingReads.constFind(opId);
    if (pending == m_pendingReads.cend())
        return; // not ours, or already superseded
    const PendingRead read = *pending;
    m_pendingReads.erase(pending);
    m_roomsBeingRead.remove(read.roomId);
    // The room the reply names must be the room we asked about.
    if (session.roomId != read.roomId)
        return;

    const RtcSessionData previous = m_sessions.value(session.roomId);
    m_sessions.insert(session.roomId, session);

    // Only announce a real change: a poke storm on an unchanged call would
    // otherwise re-render every banner and facepile for nothing.
    const bool changed = previous.participants.size()
            != session.participants.size()
        || previous.slotClosed != session.slotClosed
        || previous.slotPresent != session.slotPresent
        || previous.focusServiceUrl != session.focusServiceUrl
        || !std::equal(previous.participants.cbegin(),
                       previous.participants.cend(),
                       session.participants.cbegin(),
                       [](const RtcParticipant &a, const RtcParticipant &b) {
                           // Profile fields are compared too: they are what
                           // the facepile DRAWS, and a name or avatar that
                           // resolved after the first read would otherwise
                           // stay stale until some unrelated change fired.
                           return a.userId == b.userId
                               && a.deviceId == b.deviceId
                               && a.intent == b.intent
                               && a.displayName == b.displayName
                               && a.avatarMxc == b.avatarMxc;
                       });
    if (changed)
        Q_EMIT sessionChanged(session.roomId);
}

void RtcController::onTransportsReceived(quint64 opId, bool serverAnswered,
                                          const QString &category,
                                          const QStringList &serverServiceUrls,
                                          const QString &participantFocusUrl)
{
    if (opId != m_discoveryOp)
        return; // a superseded discovery
    const QString discoveredRoom = m_discoveryRoomId;
    m_discoveryOp = 0;
    m_discovered = true;
    m_serverAnswered = serverAnswered;
    m_serviceUrls = serverServiceUrls;
    if (!discoveredRoom.isEmpty()) {
        if (participantFocusUrl.isEmpty())
            m_participantFocus.remove(discoveredRoom);
        else
            m_participantFocus.insert(discoveredRoom, participantFocusUrl);
    }
    // The category explains an ABSENCE. When a transport was found there is
    // nothing to explain, so it is cleared rather than left to leak a stale
    // failure into a working state.
    m_availabilityCategory =
        (!serverServiceUrls.isEmpty() || !participantFocusUrl.isEmpty())
        ? QString()
        : category;
    Q_EMIT availabilityChanged();
}

int RtcController::participantCount(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return 0;
    return it->participants.size();
}

bool RtcController::hasLiveSession(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    return it != m_sessions.cend() && it->live();
}

bool RtcController::ownUserInSession(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return false;
    return std::any_of(it->participants.cbegin(), it->participants.cend(),
                       [](const RtcParticipant &p) { return p.ownUser; });
}

bool RtcController::ownDeviceInSession(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return false;
    return std::any_of(it->participants.cbegin(), it->participants.cend(),
                       [](const RtcParticipant &p) { return p.ownDevice; });
}

bool RtcController::hasVideoIntent(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return false;
    return std::any_of(
        it->participants.cbegin(), it->participants.cend(),
        [](const RtcParticipant &p) {
            return p.intent == QLatin1String("video");
        });
}

QVariantList RtcController::participants(const QString &roomId, int max) const
{
    QVariantList out;
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return out;
    const int limit = max > 0 ? std::min(max, kMaxPresentedParticipants)
                              : kMaxPresentedParticipants;
    for (const RtcParticipant &participant : it->participants) {
        if (out.size() >= limit)
            break;
        QVariantMap row;
        row.insert(QStringLiteral("userId"), participant.userId);
        // deviceId deliberately NOT exposed: RtcSession.h states it is
        // compared, never rendered, and nothing in QML needs it. A second
        // device shows as a second tile by position, not by its id.
        row.insert(QStringLiteral("intent"), participant.intent);
        row.insert(QStringLiteral("displayName"), participant.displayName);
        row.insert(QStringLiteral("avatarMxc"), participant.avatarMxc);
        row.insert(QStringLiteral("ownUser"), participant.ownUser);
        row.insert(QStringLiteral("ownDevice"), participant.ownDevice);
        row.insert(QStringLiteral("joinedAtMs"), participant.joinedAtMs);
        out.append(row);
    }
    return out;
}

QStringList RtcController::participantUserIds(const QString &roomId,
                                               int max) const
{
    QStringList out;
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return out;
    const int limit = max > 0 ? std::min(max, kMaxPresentedParticipants)
                              : kMaxPresentedParticipants;
    for (const RtcParticipant &participant : it->participants) {
        if (out.size() >= limit)
            break;
        // One entry per PERSON here: the same account on a laptop and a
        // phone is two participants but one face, and a facepile showing
        // the same avatar twice with no explanation reads as a bug.
        if (!out.contains(participant.userId))
            out.append(participant.userId);
    }
    return out;
}

QVariantList RtcController::participantFaces(const QString &roomId,
                                              int max) const
{
    QVariantList out;
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return out;
    const int limit = max > 0 ? std::min(max, kMaxPresentedParticipants)
                              : kMaxPresentedParticipants;
    QStringList seen;
    for (const RtcParticipant &participant : it->participants) {
        if (out.size() >= limit)
            break;
        // One face per PERSON: the same account on a laptop and a phone is
        // two participants but one face, and a repeated avatar with no
        // explanation reads as a bug.
        if (seen.contains(participant.userId))
            continue;
        seen.append(participant.userId);
        QVariantMap row;
        row.insert(QStringLiteral("userId"), participant.userId);
        row.insert(QStringLiteral("displayName"), participant.displayName);
        row.insert(QStringLiteral("avatarMxc"), participant.avatarMxc);
        out.append(row);
    }
    return out;
}

RtcController::JoinBlock RtcController::joinBlock(const QString &roomId) const
{
    if (!supported())
        return JoinBlock::Unsupported;
    const auto it = m_sessions.constFind(roomId);
    if (it != m_sessions.cend() && it->slotClosed)
        return JoinBlock::SessionClosed;
    if (!m_discovered)
        return JoinBlock::Undiscovered;
    if (!transportReachableFor(roomId)) {
        // A server that answered and named nothing is a different fact from
        // a discovery that never completed, and the user-facing wording
        // differs: "this homeserver has no calling" versus "couldn't check".
        return m_serverAnswered ? JoinBlock::NoTransport
                                : JoinBlock::DiscoveryFailed;
    }
    // Everything on the Matrix side checks out. What is missing is the SFU
    // media transport, which this build does not have — so joining would
    // publish a membership no peer could connect to.
    return JoinBlock::NoMediaTransport;
}

QString RtcController::joinBlockReason(const QString &roomId) const
{
    switch (joinBlock(roomId)) {
    case JoinBlock::None:             return QString();
    case JoinBlock::Unsupported:      return QStringLiteral("unsupported");
    case JoinBlock::Undiscovered:     return QStringLiteral("undiscovered");
    case JoinBlock::NoTransport:      return QStringLiteral("no_transport");
    case JoinBlock::DiscoveryFailed:  return QStringLiteral("discovery_failed");
    case JoinBlock::SessionClosed:    return QStringLiteral("session_closed");
    case JoinBlock::NoMediaTransport: return QStringLiteral("no_media_transport");
    }
    return QStringLiteral("unsupported");
}
