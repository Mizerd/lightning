#include "calls/RtcController.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>
#include <algorithm>

#include <QUrl>
#include <utility>

#include "matrix/MatrixClient.h"

namespace {
Q_LOGGING_CATEGORY(lcRtc, "lightning.calls.rtc")
constexpr int kMaxPresentedParticipants = 64;
constexpr int kMaxRefusedRooms = 32;
/// The expiry re-read waits this long past the deadline, so the Rust read
/// (which drops a membership once `expires_at_ms <= now`) is sure to see it
/// expired.
constexpr qint64 kExpiryRereadGraceMs = 1000;
constexpr qint64 kExpiryRereadFloorMs = 100;
constexpr qint64 kMaxExpiryWatchMs = 24LL * 60 * 60 * 1000;
}

RtcController::RtcController(QObject *parent) : QObject(parent)
{
    m_pokeTimer.setSingleShot(true);
    connect(&m_pokeTimer, &QTimer::timeout, this, &RtcController::flushPokes);
    m_permissionRereadTimer.setSingleShot(true);
    connect(&m_permissionRereadTimer, &QTimer::timeout, this,
            &RtcController::rereadCurrentRoomPermission);
    m_expiryTimer.setSingleShot(true);
    connect(&m_expiryTimer, &QTimer::timeout, this,
            &RtcController::rereadExpiredSessions);
}

void RtcController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client) {
        disconnect(m_client, nullptr, this, nullptr);
    }
    m_client = client;
    clearForNewSession();
    if (m_client) {
        connect(m_client, &MatrixClient::rtcSessionReceived, this,
                &RtcController::onSessionReceived);
        connect(m_client, &MatrixClient::rtcSessionChanged, this,
                &RtcController::onSessionPoked);
        connect(m_client, &MatrixClient::rtcTransportsReceived, this,
                &RtcController::onTransportsReceived);
        connect(m_client, &MatrixClient::roomMembersReceived, this,
                &RtcController::onRoomMembersReceived);
        connect(m_client, &MatrixClient::roomMemberEventSeen, this,
                &RtcController::onRoomMemberEventSeen);
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
    m_unresolvedIdentitiesLogged.clear();
    // Outstanding reads belonged to the previous account. An account switch
    // reuses the client and emits loggedOut, which lands here.
    m_pendingReads.clear();
    m_roomsBeingRead.clear();
    m_pokedRooms.clear();
    m_serverReadWanted.clear();
    m_lastServerReadMs.clear();
    m_serverReadStreak.clear();
    m_pokeTimer.stop();
    m_expiryTimer.stop();
    m_sessionReadAtMs.clear();
    m_discovered = false;
    m_serverAnswered = false;
    m_serviceUrls.clear();
    m_participantFocus.clear();
    m_availabilityCategory.clear();
    m_discoveryOp = 0;
    m_encryptedRooms.clear();
    m_canPublishMembership.clear();
    m_permissionStale.clear();
    m_membershipRefusedAtMs.clear();
    m_permissionReads.clear();
    m_permissionRoomsBeingRead.clear();
    m_currentRoom.clear();
    m_permissionRereadTimer.stop();
    Q_EMIT availabilityChanged();
}

void RtcController::setReadTimeoutMsForTest(int ms)
{
    m_readTimeoutMs = ms < 0 ? 0 : ms;
}

void RtcController::reapStaleReads()
{
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
    bool sessionNamesAFocus = false;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!sessionFocusFor(it.key()).isEmpty()) {
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
    if (m_discoveryOp != 0)
        return false;
    return !m_serverAnswered;
}

QString RtcController::sessionFocusFor(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend() || it->slotClosed)
        return {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool allExpired = !it->participants.isEmpty()
        && std::all_of(it->participants.cbegin(), it->participants.cend(),
                       [now](const RtcParticipant &member) {
                           return member.expiresAtMs > 0
                               && member.expiresAtMs <= now;
                       });
    return allExpired ? QString() : it->focusServiceUrl;
}

bool RtcController::transportReachableFor(const QString &roomId) const
{
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

bool RtcController::roomEncrypted(const QString &roomId) const
{
    const RoomEncryption stored =
        m_encryptedRooms.value(roomId, RoomEncryption::Unknown);
    if (m_encryptionResolver) {
        switch (m_encryptionResolver(roomId)) {
        case RoomEncryption::Yes:
            m_encryptedRooms.insert(roomId, RoomEncryption::Yes);
            return true;
        case RoomEncryption::No:
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
    m_permissionStale.remove(roomId);
    const auto it = m_canPublishMembership.constFind(roomId);
    if (it != m_canPublishMembership.cend() && it.value() == can)
        return;
    m_canPublishMembership.insert(roomId, can);
    Q_EMIT sessionChanged(roomId);
}

bool RtcController::publishRefused(const QString &roomId) const
{
    return m_membershipRefusedAtMs.contains(roomId)
        || !m_canPublishMembership.value(roomId, true);
}

void RtcController::setCurrentRoom(const QString &roomId,
                                   bool snapshotRequested)
{
    if (roomId != m_currentRoom) {
        m_currentRoom = roomId;
        m_permissionRereadTimer.stop();
        QStringList dropped;
        for (auto it = m_membershipRefusedAtMs.begin();
             it != m_membershipRefusedAtMs.end();) {
            if (it.key() != roomId) {
                dropped.append(it.key());
                it = m_membershipRefusedAtMs.erase(it);
            } else {
                ++it;
            }
        }
        for (const QString &room : std::as_const(dropped))
            Q_EMIT sessionChanged(room);
    }
    if (roomId.isEmpty() || !supported() || snapshotRequested)
        return;
    const bool fresh = m_canPublishMembership.contains(roomId)
        && !m_permissionStale.contains(roomId);
    if (!fresh)
        requestPermissionRead(roomId);
}

void RtcController::reapStalePermissionReads()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_permissionReads.begin(); it != m_permissionReads.end();) {
        if (m_readTimeoutMs > 0 && now - it->dispatchedAtMs > m_readTimeoutMs) {
            m_permissionRoomsBeingRead.remove(it->roomId);
            it = m_permissionReads.erase(it);
        } else {
            ++it;
        }
    }
}

void RtcController::requestPermissionRead(const QString &roomId)
{
    if (!m_client || roomId.isEmpty())
        return;
    reapStalePermissionReads();
    if (m_permissionRoomsBeingRead.contains(roomId))
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const quint64 opId = m_client->requestRoomMembers(roomId);
    if (opId == 0)
        return;
    m_permissionReads.insert(opId, PendingRead{roomId, now});
    m_permissionRoomsBeingRead.insert(roomId);
}

void RtcController::rereadCurrentRoomPermission()
{
    if (m_currentRoom.isEmpty() || !supported())
        return;
    reapStalePermissionReads();
    if (m_permissionRoomsBeingRead.contains(m_currentRoom)) {
        m_permissionRereadTimer.start(m_permissionRereadMs);
        return;
    }
    requestPermissionRead(m_currentRoom);
}

void RtcController::onRoomMembersReceived(quint64 opId, const QString &roomId,
                                          const QVariantMap &snapshot)
{
    const bool ok = snapshot.value(QStringLiteral("ok")).toBool();
    const bool partial = snapshot.value(QStringLiteral("partial")).toBool();
    const auto pending = m_permissionReads.constFind(opId);
    if (pending != m_permissionReads.cend() && (!ok || !partial)) {
        m_permissionRoomsBeingRead.remove(pending->roomId);
        m_permissionReads.erase(pending);
    }
    if (!ok || roomId.isEmpty())
        return;
    const QString key = QStringLiteral("canPublishCallMembership");
    if (!snapshot.contains(key))
        return;
    setCanPublishMembership(roomId, snapshot.value(key).toBool());
}

void RtcController::onRoomMemberEventSeen(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    const bool hadEvidence = m_membershipRefusedAtMs.remove(roomId);
    const bool known = m_canPublishMembership.contains(roomId);
    if (known)
        m_permissionStale.insert(roomId);
    const bool refusedBySnapshot = known && !m_canPublishMembership.value(roomId);
    if (roomId == m_currentRoom && (hadEvidence || refusedBySnapshot)
        && !m_permissionRereadTimer.isActive()) {
        m_permissionRereadTimer.start(m_permissionRereadMs);
    }
    if (hadEvidence)
        Q_EMIT sessionChanged(roomId);
}

void RtcController::noteMembershipRefused(const QString &roomId,
                                          const QString &category)
{
    if (roomId.isEmpty() || category != QLatin1String("forbidden"))
        return;
    if (m_membershipRefusedAtMs.size() >= kMaxRefusedRooms
        && !m_membershipRefusedAtMs.contains(roomId)) {
        m_membershipRefusedAtMs.clear();
    }
    m_membershipRefusedAtMs.insert(roomId, QDateTime::currentMSecsSinceEpoch());
    qCInfo(lcRtc) << "call membership refused as forbidden; the MatrixRTC "
                     "lane is marked not permitted in this room until a "
                     "membership or power-level change arrives, another room "
                     "is opened, or the account changes";
    Q_EMIT sessionChanged(roomId);
    Q_EMIT membershipRefused(roomId);
}

void RtcController::setPermissionRereadMsForTest(int ms)
{
    m_permissionRereadMs = ms < 0 ? 0 : ms;
}

void RtcController::setPokeCoalesceMsForTest(int ms)
{
    m_pokeCoalesceMs = ms < 0 ? 0 : ms;
}

void RtcController::refresh(const QString &roomId)
{
    if (roomId.isEmpty() || !supported())
        return;
    reapStaleReads();
    if (m_roomsBeingRead.contains(roomId))
        return;
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
    const int streak = m_serverReadStreak.value(roomId);
    qint64 cooldown = m_serverReadCooldownMs;
    for (int i = 0; i < streak && cooldown < m_serverReadCooldownMaxMs; ++i)
        cooldown *= 2;
    cooldown = qMin<qint64>(cooldown, m_serverReadCooldownMaxMs);
    const auto last = m_lastServerReadMs.constFind(roomId);
    if (last != m_lastServerReadMs.cend() && now - *last < cooldown)
        return;
    if (m_lastServerReadMs.size() >= 64 && !m_lastServerReadMs.contains(roomId)) {
        m_lastServerReadMs.clear();
        m_serverReadStreak.clear();
    }
    m_lastServerReadMs.insert(roomId, now);
    m_serverReadStreak.insert(roomId, streak + 1);
    m_serverReadWanted.insert(roomId);
    if (m_roomsBeingRead.contains(roomId)) {
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
    m_pokeTimer.start(m_pokeCoalesceMs);
}

void RtcController::flushPokes()
{
    const QSet<QString> rooms = std::move(m_pokedRooms);
    m_pokedRooms.clear();
    reapStaleReads();
    for (const QString &roomId : rooms) {
        if (m_roomsBeingRead.contains(roomId)) {
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
        return;
    const PendingRead read = *pending;
    m_pendingReads.erase(pending);
    m_roomsBeingRead.remove(read.roomId);
    if (session.roomId != read.roomId)
        return;

    const RtcSessionData previous = m_sessions.value(session.roomId);
    m_sessions.insert(session.roomId, session);
    m_sessionReadAtMs.insert(session.roomId, read.dispatchedAtMs);
    qCInfo(lcRtc) << "session read room participants=" << session.participants.size()
                  << "source=" << session.source
                  << "rawEvents=" << session.rawMembershipEvents
                  << "slotPresent=" << session.slotPresent
                  << "slotClosed=" << session.slotClosed;

    const bool changed = previous.participants.size()
            != session.participants.size()
        || previous.slotClosed != session.slotClosed
        || previous.slotPresent != session.slotPresent
        || previous.focusServiceUrl != session.focusServiceUrl
        || !std::equal(previous.participants.cbegin(),
                       previous.participants.cend(),
                       session.participants.cbegin(),
                       [](const RtcParticipant &a, const RtcParticipant &b) {
                           return a.userId == b.userId
                               && a.deviceId == b.deviceId
                               && a.rtcIdentity == b.rtcIdentity
                               && a.hashedRtcIdentity == b.hashedRtcIdentity
                               && a.focusServiceUrl == b.focusServiceUrl
                               && a.focusSelection == b.focusSelection
                               && a.intent == b.intent
                               && a.displayName == b.displayName
                               && a.avatarMxc == b.avatarMxc;
                       });
    if (changed) {
        m_serverReadStreak.remove(session.roomId);
        Q_EMIT sessionChanged(session.roomId);
    }
    scheduleExpiryReread();
}

bool RtcController::expiryReadDue(const QString &roomId, qint64 expiresAtMs,
                                  qint64 nowMs) const
{
    return expiresAtMs <= nowMs
        && expiresAtMs > m_sessionReadAtMs.value(roomId, 0);
}

void RtcController::scheduleExpiryReread()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 next = 0;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        const qint64 readAt = m_sessionReadAtMs.value(it.key(), 0);
        for (const RtcParticipant &p : it->participants) {
            if (p.expiresAtMs <= readAt
                || p.expiresAtMs - now > kMaxExpiryWatchMs)
                continue;
            if (next == 0 || p.expiresAtMs < next)
                next = p.expiresAtMs;
        }
    }
    if (next == 0) {
        m_expiryTimer.stop();
        return;
    }
    const qint64 delay =
        qMax(next + kExpiryRereadGraceMs - now, kExpiryRereadFloorMs);
    m_expiryTimer.start(static_cast<int>(delay));
}

void RtcController::rereadExpiredSessions()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList rooms;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        for (const RtcParticipant &p : it->participants) {
            if (expiryReadDue(it.key(), p.expiresAtMs, now)) {
                rooms.append(it.key());
                break;
            }
        }
    }
    for (const QString &roomId : std::as_const(rooms))
        m_sessionReadAtMs.insert(roomId, now);
    for (const QString &roomId : std::as_const(rooms))
        onSessionPoked(roomId);
    scheduleExpiryReread();
}

void RtcController::onTransportsReceived(quint64 opId, bool serverAnswered,
                                          const QString &category,
                                          const QStringList &serverServiceUrls,
                                          const QString &participantFocusUrl)
{
    if (opId != m_discoveryOp)
        return;
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

int RtcController::authoritativeParticipantCount(const QString &roomId) const
{
    const auto it = m_sessions.constFind(roomId);
    if (it == m_sessions.cend())
        return -1;
    if (it->source != QLatin1String("store")
        && it->source != QLatin1String("server")
        && it->source != QLatin1String("server-none"))
        return -1;
    return it->slotClosed ? 0 : int(it->participants.size());
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
        noteUnresolvedIdentity(identity, QStringLiteral("no live session"));
        return out;
    }
    const RtcParticipant *matched = nullptr;
    int plainTried = 0;
    int sessionHashTried = 0;
    int stickyHashTried = 0;
    for (const RtcParticipant &participant : it->participants) {
        if (participant.wireFormat == QLatin1String("rtc"))
            ++stickyHashTried;
        else
            ++plainTried;
        if (!participant.hashedRtcIdentity.isEmpty())
            ++sessionHashTried;
        if (participant.rtcIdentity != identity
            && participant.hashedRtcIdentity != identity)
            continue;
        // A duplicate member id under two devices can produce the same hash.
        // Do not bind a key ring to whichever membership happened to sort first.
        if (matched && (matched->userId != participant.userId
                        || matched->deviceId != participant.deviceId)) {
            noteUnresolvedIdentity(identity, QStringLiteral("ambiguous membership match"));
            return {};
        }
        matched = &participant;
    }
    if (matched) {
        out.insert(QStringLiteral("userId"), matched->userId);
        out.insert(QStringLiteral("deviceId"), matched->deviceId);
        out.insert(QStringLiteral("displayName"), matched->displayName);
        out.insert(QStringLiteral("avatarMxc"), matched->avatarMxc);
        out.insert(QStringLiteral("ownUser"), matched->ownUser);
        out.insert(QStringLiteral("ownDevice"), matched->ownDevice);
        out.insert(QStringLiteral("focusServiceUrl"), matched->focusServiceUrl);
        return out;
    }
    noteUnresolvedIdentity(identity,
                           QStringLiteral("no membership matched it; tried plain=%1 sessionHash=%2 stickyHash=%3")
                               .arg(plainTried).arg(sessionHashTried).arg(stickyHashTried));
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
    const QString subject = identity + QChar(0x1f) + reason;
    if (m_unresolvedIdentitiesLogged.size() >= 256
        || m_unresolvedIdentitiesLogged.contains(subject)) {
        return;
    }
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
    const QString oldest = sessionFocusFor(roomId);
    if (!usesLegacyFocusFor(roomId) && !ownFocusUrlFor().isEmpty())
        return ownFocusUrlFor();
    if (!oldest.isEmpty())
        return oldest;
    if (!ownFocusUrlFor().isEmpty())
        return ownFocusUrlFor();
    return m_participantFocus.value(roomId);
}

QString RtcController::ownFocusUrlFor() const
{
    return m_serviceUrls.isEmpty() ? QString() : m_serviceUrls.first();
}

bool RtcController::usesLegacyFocusFor(const QString &roomId) const
{
    const auto session = m_sessions.constFind(roomId);
    if (session == m_sessions.cend() || session->slotClosed)
        return false;
    for (const RtcParticipant &member : session->participants) {
        const qint64 expiry = member.expiresAtMs;
        if ((expiry == 0 || expiry > QDateTime::currentMSecsSinceEpoch())
            && !member.ownDevice
            && member.focusSelection != QLatin1String("multi_sfu"))
            return true;
    }
    return false;
}

QStringList RtcController::subscriberFocusUrlsFor(const QString &roomId) const
{
    QStringList urls;
    const auto session = m_sessions.constFind(roomId);
    if (session == m_sessions.cend() || session->slotClosed)
        return urls;
    const QString oldest = sessionFocusFor(roomId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const RtcParticipant &member : session->participants) {
        if (member.ownDevice
            || (member.expiresAtMs > 0 && member.expiresAtMs <= now))
            continue;
        const QString url = member.focusSelection == QLatin1String("multi_sfu")
            ? member.focusServiceUrl
            : member.focusSelection == QLatin1String("oldest_membership")
                ? oldest : QString();
        const auto sameFocus = [&url](const QString &existing) {
            return sameFocusUrl(existing, url);
        };
        if (!url.isEmpty() && std::none_of(urls.cbegin(), urls.cend(), sameFocus)
            && urls.size() < 8)
            urls.append(url);
    }
    return urls;
}

bool RtcController::sameFocusUrl(const QString &left, const QString &right)
{
    if (left == right)
        return true;
    QUrl a(left);
    QUrl b(right);
    if (!a.isValid() || !b.isValid() || a.isEmpty() || b.isEmpty())
        return false;
    a = a.adjusted(QUrl::NormalizePathSegments);
    b = b.adjusted(QUrl::NormalizePathSegments);
    if (a.scheme() == QLatin1String("https") && a.port() == 443)
        a.setPort(-1);
    if (b.scheme() == QLatin1String("https") && b.port() == 443)
        b.setPort(-1);
    if (a.path().isEmpty())
        a.setPath(QStringLiteral("/"));
    if (b.path().isEmpty())
        b.setPath(QStringLiteral("/"));
    return a == b;
}

RtcController::JoinBlock RtcController::joinBlock(const QString &roomId) const
{
    if (!supported())
        return JoinBlock::Unsupported;
    if (m_client->networkRouteActive())
        return JoinBlock::NetworkRoute;
    const auto it = m_sessions.constFind(roomId);
    if (it != m_sessions.cend() && it->slotClosed)
        return JoinBlock::SessionClosed;
    if (!transportReachableFor(roomId)) {
        if (!m_discovered)
            return JoinBlock::Undiscovered;
        return m_serverAnswered ? JoinBlock::NoTransport
                                : JoinBlock::DiscoveryFailed;
    }
    if (roomEncrypted(roomId) && !m_mediaEncryption)
        return JoinBlock::MediaEncryptionUnavailable;
    if (!m_mediaAvailable)
        return JoinBlock::NoMediaTransport;
    if (publishRefused(roomId))
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
    case JoinBlock::NetworkRoute:     return QStringLiteral("network_route");
    case JoinBlock::NoPermission:      return QStringLiteral("no_permission");
    case JoinBlock::MediaEncryptionUnavailable:
        return QStringLiteral("media_encryption_unavailable");
    }
    return QStringLiteral("unsupported");
}
