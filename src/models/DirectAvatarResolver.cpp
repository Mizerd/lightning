#include "models/DirectAvatarResolver.h"

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

    const auto member = room.members.constFind(peer);
    if (member != room.members.cend() && !member->avatarMxcUrl.isEmpty())
        return member->avatarMxcUrl;
    return m_avatars.value(peer);
}

void DirectAvatarResolver::resolveMissing(const QList<RoomInfo> &rooms)
{
    if (!m_client)
        return;
    for (const RoomInfo &room : rooms) {
        const QString peer = directPeer(room);
        if (peer.isEmpty() || !avatarFor(room).isEmpty()
            || m_avatars.contains(peer) || m_pending.contains(peer)
            || m_noAvatar.contains(peer))
            continue;
        const quint64 opId = m_client->fetchUserProfile(peer);
        if (opId != 0) {
            m_pending.insert(peer);
            m_ops.insert(opId, peer);
        }
    }
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
    if (ok && !avatarUrl.isEmpty()) {
        // Filed under both ids: owners look up by the room's `directUserId`,
        // which is the id we asked with.
        m_avatars.insert(subject, avatarUrl);
        m_noAvatar.remove(subject);
        if (!requestedUser.isEmpty() && requestedUser != subject) {
            m_avatars.insert(requestedUser, avatarUrl);
            m_noAvatar.remove(requestedUser);
            Q_EMIT avatarResolved(requestedUser);
        }
        // Announce only a learned face. Announcing every answer let owners
        // rebuild, re-enter resolveMissing() and ask again forever.
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
        m_noAvatar.insert(subject);
        if (!requestedUser.isEmpty())
            m_noAvatar.insert(requestedUser);
        return;
    }
    if (requestedUser.isEmpty())
        return;
    m_noAvatar.insert(requestedUser);
    if (subject != requestedUser)
        m_noAvatar.insert(subject);
}
