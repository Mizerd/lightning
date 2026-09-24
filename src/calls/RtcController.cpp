#include "calls/RtcController.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>

#include "matrix/MatrixClient.h"

namespace {
Q_LOGGING_CATEGORY(lcRtc, "lightning.calls.rtc")
/// Presentation bound; the Rust side already caps a session at 128 devices.
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
    // A new client is a new account (or none): forget what was observed.
    clearForNewSession();
    if (m_client) {
        connect(m_client, &MatrixClient::rtcSessionReceived, this,
                &RtcController::onSessionReceived);
        connect(m_client, &MatrixClient::rtcSessionChanged, this,
                &RtcController::onSessionPoked);
        connect(m_client, &MatrixClient::rtcTransportsReceived, this,
                &RtcController::onTransportsReceived);
    // Forget observed calls on sign-out: participant lists are other
    // people's presence in rooms this account may leave.
        connect(m_client, &MatrixClient::loggedOut, this,
                [this]() { clearForNewSession(); });
    }
    Q_EMIT availabilityChanged();
}

void RtcController::clearForNewSession()
{
    m_sessions.clear();
    // Diagnostics are per session, so a retry can report again.
    m_unresolvedIdentitiesLogged.clear();
    // Outstanding reads belonged to the previous account. An account switch
    // reuses the client and emits loggedOut, which lands here.
    m_pendingReads.clear();
    m_roomsBeingRead.clear();
    m_pokedRooms.clear();
    // Also forced reads and their cooldowns: the new account may ask at once.
    m_serverReadWanted.clear();
    m_lastServerReadMs.clear();
    m_serverReadStreak.clear();
    m_pokeTimer.stop();
    m_discovered = false;
    m_serverAnswered = false;
    m_serviceUrls.clear();
    m_participantFocus.clear();
    m_availabilityCategory.clear();
    m_discoveryOp = 0;
    // Room encryption belongs to the previous account. The resolver is kept:
    // it answers from whatever room list is current.
    m_encryptedRooms.clear();
    Q_EMIT availabilityChanged();
}

void RtcController::setReadTimeoutMsForTest(int ms)
{
    m_readTimeoutMs = ms < 0 ? 0 : ms;
}

void RtcController::reapStaleReads()
{
    // A reply may never arrive (the Rust event queue drops the oldest on
    // overflow). Without reaping, the room would stay unrefreshable and
    // flushPokes would re-arm forever.
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
    // Availability is a positive fact: a transport was actually named. A
    // session that names a focus counts even without MSC4143 discovery.
    bool sessionNamesAFocus = false;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->slotClosed && !it->focusServiceUrl.isEmpty()) {
            sessionNamesAFocus = true;
            break;
        }
    }
    return supported()
        && (sessionNamesAFocus
            || (m_discovered && (!m_serviceUrls.isEmpty()
                                 || !m_participantFocus.isEmpty())));
}

bool RtcController::discoveryWorthRetrying() const
{
    // Bounds the automatic (room-change) trigger only; an explicit discover()
    // is always honoured. One request in flight is enough.
    if (m_discoveryOp != 0)
        return false;
    // Once the server has answered either way, the account-scoped answer is
    // settled for the session.
    return !m_serverAnswered;
}

QString RtcController::sessionFocusFor(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return {};
    return it->focusServiceUrl;
}

bool RtcController::transportReachableFor(const QString &roomId) const
{
    // The room's own session focus first: it is where the participants are,
    // and without MSC4143 (most homeservers) it is the only focus there is.
    // The homeserver's answer applies everywhere; a participant-advertised
    // focus only to its own room.
    return !sessionFocusFor(roomId).isEmpty()
        || !m_serviceUrls.isEmpty()
        || !m_participantFocus.value(roomId).isEmpty();
}

void RtcController::setMediaEncryptionAvailable(bool available)
{
    if (m_mediaEncryption == available)
        return;
    m_mediaEncryption = available;
    Q_EMIT availabilityChanged();
}

void RtcController::setMediaAvailable(bool available)
{
    if (m_mediaAvailable == available)
        return;
    m_mediaAvailable = available;
    Q_EMIT availabilityChanged();
}

void RtcController::setEncryptionResolver(EncryptionResolver resolver)
{
    m_encryptionResolver = std::move(resolver);
}

/// Asks the resolver first, then falls back to the stored record. The record
/// was only filled for rooms that were opened or called from, so a join
/// from the incoming-call card took the fail-closed default in an
/// unencrypted room and dropped every frame. Pulling means no surface can
/// forget to push.
bool RtcController::roomEncrypted(const QString &roomId) const
{
    const RoomEncryption stored =
        m_encryptedRooms.value(roomId, RoomEncryption::Unknown);
    if (m_encryptionResolver) {
        switch (m_encryptionResolver(roomId)) {
        case RoomEncryption::Yes:
            // Remember it, so the irreversibility guard covers every room
            // ever seen encrypted. One record, not two caches.
            m_encryptedRooms.insert(roomId, RoomEncryption::Yes);
            return true;
        case RoomEncryption::No:
            // A known Yes still wins: encryption cannot be removed in Matrix,
            // so a "no" here is a stale view (see setRoomEncrypted).
            return stored == RoomEncryption::Yes;
        case RoomEncryption::Unknown:
            break;
        }
    }
    // Unknown fails closed.
    return stored != RoomEncryption::No;
}

void RtcController::setRoomEncrypted(const QString &roomId, bool encrypted)
{
    if (roomId.isEmpty())
        return;
    const auto it = m_encryptedRooms.constFind(roomId);
    const RoomEncryption wanted =
        encrypted ? RoomEncryption::Yes : RoomEncryption::No;
    if (it != m_encryptedRooms.cend() && it.value() == wanted)
        return;
    // Encryption is irreversible in Matrix, so this record only moves one
    // way. A read claiming a known-encrypted room is now plaintext is stale
    // or partial, and obeying it would make the next call join in the clear.
    // The worst case of refusing is a call that insists on encryption and
    // fails loudly. Only a known Yes blocks a downgrade; callers record only
    // known answers.
    if (!encrypted && it != m_encryptedRooms.cend()
        && it.value() == RoomEncryption::Yes) {
        qCWarning(lcRtc)
            << "refusing to downgrade a known-encrypted room to plaintext"
            << "room=" << roomId
            << "— encryption cannot be removed in Matrix, so this read is"
            << "stale or incomplete; the call stays encrypted";
        return;
    }
    m_encryptedRooms.insert(roomId, wanted);
    Q_EMIT sessionChanged(roomId);
}

void RtcController::setCanPublishMembership(const QString &roomId, bool can)
{
    if (roomId.isEmpty())
        return;
    const auto it = m_canPublishMembership.constFind(roomId);
    if (it != m_canPublishMembership.cend() && it.value() == can)
        return;
    m_canPublishMembership.insert(roomId, can);
    // Same signal as the encryption fact; the banner and call row re-read
    // their block reason from it.
    Q_EMIT sessionChanged(roomId);
}

void RtcController::setPokeCoalesceMsForTest(int ms)
{
    m_pokeCoalesceMs = ms < 0 ? 0 : ms;
}

void RtcController::refresh(const QString &roomId)
{
    if (roomId.isEmpty() || !supported())
        return;
    // One read per room at a time; flushPokes() re-pokes a room whose read
    // was in flight, so a burst is not lost.
    reapStaleReads();
    if (m_roomsBeingRead.contains(roomId))
        return;
    // A forced-read request is consumed only when a read is dispatched, so a
    // request made during another read still reaches the wire.
    const bool preferServer = m_serverReadWanted.contains(roomId);
    const quint64 opId = m_client->rtcSession(roomId, preferServer);
    if (opId == 0)
        return;
    m_serverReadWanted.remove(roomId);
    m_pendingReads.insert(
        opId, PendingRead{roomId, QDateTime::currentMSecsSinceEpoch()});
    m_roomsBeingRead.insert(roomId);
}

void RtcController::refreshFromServer(const QString &roomId)
{
    if (roomId.isEmpty() || !supported())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // The gap doubles for each forced read in a row that changed nothing: a
    // participant we cannot name is a permanent condition, and polling the
    // server about it would be a request storm.
    const int streak = m_serverReadStreak.value(roomId);
    qint64 cooldown = m_serverReadCooldownMs;
    for (int i = 0; i < streak && cooldown < m_serverReadCooldownMaxMs; ++i)
        cooldown *= 2;
    cooldown = qMin<qint64>(cooldown, m_serverReadCooldownMaxMs);
    const auto last = m_lastServerReadMs.constFind(roomId);
    if (last != m_lastServerReadMs.cend() && now - *last < cooldown)
        return;
    // Bounded: this map outlives calls.
    if (m_lastServerReadMs.size() >= 64 && !m_lastServerReadMs.contains(roomId)) {
        m_lastServerReadMs.clear();
        m_serverReadStreak.clear();
    }
    m_lastServerReadMs.insert(roomId, now);
    m_serverReadStreak.insert(roomId, streak + 1);
    m_serverReadWanted.insert(roomId);
    if (m_roomsBeingRead.contains(roomId)) {
        // A store-backed read is in flight; poke so the forced read follows
        // it rather than being lost.
        m_pokedRooms.insert(roomId);
        if (m_pokeCoalesceMs > 0)
            m_pokeTimer.start(m_pokeCoalesceMs);
        else
            flushPokes();
        return;
    }
    refresh(roomId);
}

void RtcController::discover(const QString &roomId)
{
    if (!supported())
        return;
    const quint64 opId = m_client->rtcTransports(roomId);
    if (opId == 0)
        return;
    // Only the newest discovery counts; an older reply must not overwrite it.
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
    // Restarting the timer coalesces a burst into one read.
    m_pokeTimer.start(m_pokeCoalesceMs);
}

void RtcController::flushPokes()
{
    const QSet<QString> rooms = std::move(m_pokedRooms);
    m_pokedRooms.clear();
    reapStaleReads();
    for (const QString &roomId : rooms) {
        if (m_roomsBeingRead.contains(roomId)) {
            // A read in flight returns pre-poke state; re-poke. Bounded by the
            // reap above.
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
        return; // not ours, or superseded
    const PendingRead read = *pending;
    m_pendingReads.erase(pending);
    m_roomsBeingRead.remove(read.roomId);
    // The reply must be for the room we asked about.
    if (session.roomId != read.roomId)
        return;

    const RtcSessionData previous = m_sessions.value(session.roomId);
    m_sessions.insert(session.roomId, session);
    // Counts only, never ids: distinguishes "nobody else here" from "nobody
    // addressable".
    qCInfo(lcRtc) << "session read room participants=" << session.participants.size()
                  << "source=" << session.source
                  << "rawEvents=" << session.rawMembershipEvents
                  << "slotPresent=" << session.slotPresent
                  << "slotClosed=" << session.slotClosed;

    // Announce only a real change, so a poke storm does not re-render every
    // banner and facepile.
    const bool changed = previous.participants.size()
            != session.participants.size()
        || previous.slotClosed != session.slotClosed
        || previous.slotPresent != session.slotPresent
        || previous.focusServiceUrl != session.focusServiceUrl
        || !std::equal(previous.participants.cbegin(),
                       previous.participants.cend(),
                       session.participants.cbegin(),
                       [](const RtcParticipant &a, const RtcParticipant &b) {
                           // Profile fields too: the facepile draws them, and
                           // a late-resolved name would otherwise stay stale.
                           return a.userId == b.userId
                               && a.deviceId == b.deviceId
                               && a.intent == b.intent
                               && a.displayName == b.displayName
                               && a.avatarMxc == b.avatarMxc;
                       });
    if (changed) {
        // A read that changed the answer resets the escalating gap.
        m_serverReadStreak.remove(session.roomId);
        Q_EMIT sessionChanged(session.roomId);
    }
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
    // The category explains an absence; clear it when a transport was found.
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

QString RtcController::identityForMembership(
    const QString &roomId, const QString &membershipEventId,
    const QString &sender) const
{
    if (membershipEventId.isEmpty() || sender.isEmpty())
        return {};
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend())
        return {};
    for (const RtcParticipant &participant : it->participants) {
        if (participant.membershipEventId != membershipEventId)
            continue;
        // The sender must own the membership: anyone may annotate anyone's
        // state event.
        if (participant.userId != sender)
            return {};
        return participant.rtcIdentity;
    }
    return {};
}

bool RtcController::knowsMembership(const QString &roomId,
                                    const QString &membershipEventId) const
{
    if (membershipEventId.isEmpty())
        return false;
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend())
        return false;
    for (const RtcParticipant &participant : it->participants) {
        if (participant.membershipEventId == membershipEventId)
            return true;
    }
    return false;
}

QString RtcController::ownMembershipEventId(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend())
        return {};
    for (const RtcParticipant &participant : it->participants) {
        if (participant.ownDevice)
            return participant.membershipEventId;
    }
    return {};
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
        // deviceId is not exposed (RtcSession.h: compared, never rendered).
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

QString RtcController::rtcIdentityFor(const QString &roomId,
                                      const QString &userId,
                                      const QString &deviceId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return {};
    if (userId.isEmpty() || deviceId.isEmpty())
        return {};
    for (const RtcParticipant &participant : it->participants) {
        if (participant.userId == userId
            && participant.deviceId == deviceId) {
            return participant.rtcIdentity;
        }
    }
    return {};
}

QVariantMap RtcController::participantForIdentity(
    const QString &roomId, const QString &identity) const
{
    QVariantMap out;
    if (identity.isEmpty())
        return out;
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed) {
        // An empty answer makes a participant unkeyable; record it.
        noteUnresolvedIdentity(identity, QStringLiteral("no live session"));
        return out;
    }
    for (const RtcParticipant &participant : it->participants) {
        if (participant.rtcIdentity != identity)
            continue;
        out.insert(QStringLiteral("userId"), participant.userId);
        // The device, not just the person: keys are per device, and one
        // person's laptop and phone are separate senders. A public Matrix
        // device id, as in mediaKeyTargetsJson().
        out.insert(QStringLiteral("deviceId"), participant.deviceId);
        // Room-resolved profile; empty degrades to initials.
        out.insert(QStringLiteral("displayName"), participant.displayName);
        out.insert(QStringLiteral("avatarMxc"), participant.avatarMxc);
        out.insert(QStringLiteral("ownUser"), participant.ownUser);
        out.insert(QStringLiteral("ownDevice"), participant.ownDevice);
        return out;
    }
    // An empty lookup means the participant gets no key and their ring is
    // never bound (they hear us, we cannot hear them). Logged once per
    // identity per session; the caller runs on a tick.
    noteUnresolvedIdentity(identity,
                           QStringLiteral("no membership matched it"));
    return out;
}

/// Logs once that an SFU identity could not be resolved to a Matrix device.
///
/// The identity is not opaque (by default `<user id>:<device id>`), so this
/// local log line carries a third-party Matrix id: a deliberate widening
/// recorded in docs/privacy.md, needed to correlate diagnostics. No key
/// material, tokens or room content; the support-diagnostics export is held
/// to a stricter bar.
void RtcController::noteUnresolvedIdentity(const QString &identity,
                                           const QString &reason) const
{
    // Keyed on identity and reason: "no session yet" is transient at call
    // start and must not silence the real "no membership matched it".
    const QString subject = identity + QChar(0x1f) + reason;
    // Bounded: identities come from the SFU.
    if (m_unresolvedIdentitiesLogged.size() >= 256
        || m_unresolvedIdentitiesLogged.contains(subject)) {
        return;
    }
    // A miss at join is normal: the SFU is reached before our own membership
    // syncs back, so early lookups (including our own device) fail briefly.
    // Only a fault that persists past the grace period is logged;
    // reconcileKeyLane() re-tests on its tick.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto firstSeen = m_unresolvedIdentityFirstSeenMs.constFind(subject);
    if (firstSeen == m_unresolvedIdentityFirstSeenMs.cend()) {
        if (m_unresolvedIdentityFirstSeenMs.size() < 256)
            m_unresolvedIdentityFirstSeenMs.insert(subject, now);
        return;
    }
    if (now - *firstSeen < m_unresolvedIdentityGraceMs)
        return;
    m_unresolvedIdentitiesLogged.insert(subject);
    qCWarning(lcRtc) << "call diagnosis: the SFU participant" << identity
                     << "could NOT be resolved to a Matrix user and device ("
                     << reason
                     << ") — no media key can be addressed to them and their "
                        "media cannot be decrypted";
}

void RtcController::forgetUnresolvedIdentityDiagnostics()
{
    // Per call, not per account: identities repeat across calls.
    m_unresolvedIdentitiesLogged.clear();
    m_unresolvedIdentityFirstSeenMs.clear();
}

QString RtcController::mediaKeyTargetsJson(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return QStringLiteral("[]");
    QJsonArray targets;
    for (const RtcParticipant &participant : it->participants) {
        if (participant.ownDevice)
            continue;
        if (participant.userId.isEmpty() || participant.deviceId.isEmpty())
            continue;
        if (targets.size() >= kMaxPresentedParticipants)
            break;
        QJsonObject target;
        target.insert(QStringLiteral("user_id"), participant.userId);
        target.insert(QStringLiteral("device_id"), participant.deviceId);
        targets.append(target);
    }
    return QString::fromUtf8(
        QJsonDocument(targets).toJson(QJsonDocument::Compact));
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
        // One entry per person: a user's laptop and phone are two participants
        // but one face.
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
        // One face per person, as above.
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

QString RtcController::focusUrlFor(const QString &roomId) const
{
    // An existing session's focus wins: its participants are on the focus the
    // oldest membership named, and choosing our own SFU would put us alone on
    // another server. Matches element-call. The homeserver's answer is for
    // starting a call.
    const QString session = sessionFocusFor(roomId);
    if (!session.isEmpty())
        return session;
    if (!m_serviceUrls.isEmpty())
        return m_serviceUrls.first();
    return m_participantFocus.value(roomId);
}

RtcController::JoinBlock RtcController::joinBlock(const QString &roomId) const
{
    if (!supported())
        return JoinBlock::Unsupported;
    const auto it = m_sessions.constFind(roomId);
    if (it != m_sessions.cend() && it->slotClosed)
        return JoinBlock::SessionClosed;
    // Reachability before discovery state: a room whose session names a
    // focus is joinable even if discovery never answered.
    if (!transportReachableFor(roomId)) {
        if (!m_discovered)
            return JoinBlock::Undiscovered;
        // "No calling on this homeserver" (answered, nothing named) differs
        // from "couldn't check".
        return m_serverAnswered ? JoinBlock::NoTransport
                                : JoinBlock::DiscoveryFailed;
    }
    // An encrypted room whose call media cannot be encrypted is refused;
    // checked before the media-transport block so the more relevant reason is
    // shown. Unknown counts as encrypted; the owner's resolver supplies the
    // real answer.
    if (roomEncrypted(roomId) && !m_mediaEncryption)
        return JoinBlock::MediaEncryptionUnavailable;
    // No SFU media engine: never publish a membership nobody can connect to.
    if (!m_mediaAvailable)
        return JoinBlock::NoMediaTransport;
    // Whether the server would accept our membership, checked last since it
    // is room-specific. Defaults to true: an unknown capability must not
    // disable Join; only a known refusal does.
    if (!m_canPublishMembership.value(roomId, true))
        return JoinBlock::NoPermission;
    return JoinBlock::None;
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
    case JoinBlock::NoPermission:      return QStringLiteral("no_permission");
    case JoinBlock::MediaEncryptionUnavailable:
        return QStringLiteral("media_encryption_unavailable");
    }
    return QStringLiteral("unsupported");
}
