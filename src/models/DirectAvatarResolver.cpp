#include "models/DirectAvatarResolver.h"

#include <QDateTime>
#include <QRandomGenerator>

#include <algorithm>
#include <limits>

DirectAvatarResolver::DirectAvatarResolver(QObject *parent)
    : QObject(parent)
{
}

void DirectAvatarResolver::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::userProfileFinished, this,
            &DirectAvatarResolver::onUserProfileFinished);
    connect(m_client, &MatrixClient::directPeerAvatarChanged, this,
            &DirectAvatarResolver::applyPeerAvatar);
    // The avatar cache is account-scoped: drop it on sign-out and account
    // switch (same signal). Owners clearing it too is harmless.
    connect(m_client, &MatrixClient::loggedOut, this,
            &DirectAvatarResolver::clear);
    // A destroyed client must leave null here, not a dangling pointer; nothing
    // enforces destruction order. The caches go too, being account-scoped.
    connect(m_client, &QObject::destroyed, this, [this] {
        m_client = nullptr;
        clear();
    });
}

void DirectAvatarResolver::clear()
{
    m_avatars.clear();
    m_ops.clear();
    m_pending.clear();
    m_noAvatar.clear();
    m_refreshDueAt.clear();
    m_peerFaces.clear();
}

bool DirectAvatarResolver::hasPeerFace(const QString &roomId,
                                       const QString &peer) const
{
    const auto face = m_peerFaces.constFind(roomId);
    return face != m_peerFaces.cend() && face->userId == peer;
}

QString DirectAvatarResolver::directPeer(const RoomInfo &room) const
{
    if (!room.avatarUrl.isEmpty())
        return {};
    if (!room.isDirect || room.directUserId.isEmpty())
        return {};

    // The Rust backend does not populate the member snapshot (fetched on demand
    // for Room Information only) but reports the m.direct targets, which is
    // exactly the 1:1 signal needed. Mock/HTTP derive it from `members`.
    if (!room.directUserIds.isEmpty()) {
        if (room.directUserIds.size() > 1)
            return {};
    } else if (m_client && !room.members.isEmpty()) {
        const QString self = m_client->currentUserId();
        QString other;
        for (auto it = room.members.cbegin(); it != room.members.cend(); ++it) {
            const QString userId = it.key().isEmpty() ? it->userId : it.key();
            if (userId.isEmpty() || userId == self)
                continue;
            if (!other.isEmpty() && other != userId)
                return {};
            other = userId;
        }
        if (other != room.directUserId)
            return {};
    }
    return room.directUserId;
}

QString DirectAvatarResolver::avatarFor(const RoomInfo &room) const
{
    if (!room.avatarUrl.isEmpty())
        return room.avatarUrl;
    const QString peer = directPeer(room);
    if (peer.isEmpty())
        return {};
    // The peer's own member event in this room, as of the last sync: newer
    // than a roster snapshot or a profile asked once, and it may say "none".
    if (hasPeerFace(room.id, peer))
        return m_peerFaces.value(room.id).avatarUrl;

    const auto member = room.members.constFind(peer);
    if (member != room.members.cend() && !member->avatarMxcUrl.isEmpty())
        return member->avatarMxcUrl;
    return m_avatars.value(peer);
}

qint64 DirectAvatarResolver::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void DirectAvatarResolver::resolveMissing(const QList<RoomInfo> &rooms)
{
    if (!m_client)
        return;
    const qint64 now = nowMs();
    // Peers never asked go out at once; peers whose answer went stale are
    // collected and only the oldest few are refreshed per pass.
    QList<QPair<qint64, QString>> stale;
    QSet<QString> seen;
    for (const RoomInfo &room : rooms) {
        const QString peer = directPeer(room);
        if (peer.isEmpty() || hasPeerFace(room.id, peer)
            || m_pending.contains(peer) || seen.contains(peer))
            continue;
        const auto due = m_refreshDueAt.constFind(peer);
        if (due != m_refreshDueAt.cend()) {
            if (now >= *due) {
                seen.insert(peer);
                stale.append({ *due, peer });
            }
            continue;
        }
        if (!avatarFor(room).isEmpty() || m_avatars.contains(peer)
            || m_noAvatar.contains(peer))
            continue;
        seen.insert(peer);
        stale.append({ std::numeric_limits<qint64>::min(), peer });
    }
    std::sort(stale.begin(), stale.end());
    int refreshes = 0;
    for (const auto &entry : stale) {
        const bool first = entry.first == std::numeric_limits<qint64>::min();
        if (!first && refreshes >= kMaxRefreshesPerPass)
            break;
        const quint64 opId = m_client->fetchUserProfile(entry.second);
        if (opId != 0) {
            m_pending.insert(entry.second);
            m_ops.insert(opId, entry.second);
            if (!first)
                ++refreshes;
        }
    }
}

void DirectAvatarResolver::applyPeerAvatar(const QString &roomId,
                                           const QString &userId,
                                           const QString &avatarUrl)
{
    if (roomId.isEmpty() || userId.isEmpty())
        return;
    // A repeat of the face already recorded is silent. The first one for a
    // room is announced even when it matches the profile: the row may be
    // showing an older roster snapshot, which this now overrides.
    if (hasPeerFace(roomId, userId)
        && m_peerFaces.value(roomId).avatarUrl == avatarUrl)
        return;
    m_peerFaces.insert(roomId, PeerFace{ userId, avatarUrl });
    Q_EMIT avatarResolved(userId);
}

void DirectAvatarResolver::onUserProfileFinished(quint64 opId, bool ok,
                                                 const QString &userId,
                                                 const QString &displayName,
                                                 const QString &avatarUrl,
                                                 const QString &category)
{
    Q_UNUSED(displayName);
    Q_UNUSED(category);
    // Release the pending marker under both the requested and the reported id:
    // SDK normalisation can make them differ, and a stuck marker means the peer
    // is never fetched again.
    const QString requestedUser = m_ops.take(opId);
    if (!requestedUser.isEmpty())
        m_pending.remove(requestedUser);
    if (!userId.isEmpty())
        m_pending.remove(userId);

    // Cache under the SDK's authoritative id, and accept answers to ops we did
    // not start: every consumer shares this signal (e.g. a self-DM adopts the
    // account's own avatar fetched for the switcher). File under the SDK's id,
    // else the requested one; an answer naming neither is dropped, and our own
    // op must still be remembered so the next rebuild does not ask again.
    const QString subject = userId.isEmpty() ? requestedUser : userId;
    if (subject.isEmpty())
        return;
    // Only an answer to a lookup this resolver issued starts the refresh clock.
    if (!requestedUser.isEmpty()) {
        // Interval plus up to two thirds of it, per peer (15-25 min by default).
        const qint64 jitter = m_refreshMs > 0
            ? qint64(QRandomGenerator::global()->bounded(
                  quint32(std::max<qint64>(1, m_refreshMs * 2 / 3))))
            : 0;
        const qint64 due = nowMs() + m_refreshMs + jitter;
        m_refreshDueAt.insert(requestedUser, due);
        m_refreshDueAt.insert(subject, due);
    }
    if (ok && !avatarUrl.isEmpty()) {
        // A refresh that returns the face already held is silent.
        const bool changed = m_avatars.value(subject) != avatarUrl
            || (!requestedUser.isEmpty()
                && m_avatars.value(requestedUser) != avatarUrl);
        // Filed under both ids: owners look up by the room's `directUserId`,
        // which is the id we asked with.
        m_avatars.insert(subject, avatarUrl);
        m_noAvatar.remove(subject);
        if (!requestedUser.isEmpty() && requestedUser != subject) {
            m_avatars.insert(requestedUser, avatarUrl);
            m_noAvatar.remove(requestedUser);
            if (changed)
                Q_EMIT avatarResolved(requestedUser);
        }
        // Announce only a learned or CHANGED face. Announcing every answer let
        // owners rebuild, re-enter resolveMissing() and ask again forever.
        if (changed)
            Q_EMIT avatarResolved(subject);
        return;
    }

    // `ok` with no avatar URL is a fact about the user and is remembered
    // whoever asked. A failure is a fact about one request: record only our own
    // (so we do not retry every rebuild), never another consumer's, or an
    // unrelated bad round trip would pin a DM to initials for the session.
    // avatarFor() checks the member snapshot first, so a face from a member
    // event still wins.
    if (ok) {
        // The peer removed their picture: forget the old one and say so.
        const bool hadFace = m_avatars.contains(subject)
            || (!requestedUser.isEmpty() && m_avatars.contains(requestedUser));
        m_avatars.remove(subject);
        m_noAvatar.insert(subject);
        if (!requestedUser.isEmpty()) {
            m_avatars.remove(requestedUser);
            m_noAvatar.insert(requestedUser);
        }
        if (hadFace) {
            if (!requestedUser.isEmpty() && requestedUser != subject)
                Q_EMIT avatarResolved(requestedUser);
            Q_EMIT avatarResolved(subject);
        }
        return;
    }
    if (requestedUser.isEmpty())
        return;
    m_noAvatar.insert(requestedUser);
    if (subject != requestedUser)
        m_noAvatar.insert(subject);
}
