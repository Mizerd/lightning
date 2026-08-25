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
    // A user-id-keyed avatar cache is ACCOUNT-SCOPED memory: sign-out, and the
    // account switch that emits the same signal, must drop it rather than let
    // one account's faces describe the next account's peers. Owners that clear
    // it themselves make this idempotent, not duplicated.
    connect(m_client, &MatrixClient::loggedOut, this,
            &DirectAvatarResolver::clear);
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

    // The Rust backend never populates the per-room member snapshot below (it
    // is fetched separately, on demand, only for the Room Information "People"
    // tab) — it instead reports the authoritative m.direct target list
    // directly, which is exactly the "unambiguous 1:1" signal this needs and
    // requires no member fetch at all. Backends that only ever populate
    // `members` (Mock/HTTP) derive the same signal from there.
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
    // Always release the pending marker for the op that completed, keyed by
    // BOTH what we requested and what the SDK reports. An early return
    // whenever the requested and returned ids differ (SDK id normalization)
    // leaves the target stuck pending forever, so nothing ever re-fetches it
    // and the DM avatar is wedged on initials. That regression is why this
    // takes both keys.
    const QString requestedUser = m_ops.take(opId);
    if (!requestedUser.isEmpty())
        m_pending.remove(requestedUser);
    if (!userId.isEmpty())
        m_pending.remove(userId);

    // Cache under the SDK's authoritative user id, and accept results even for
    // ops we did not start — every consumer shares this one client signal.
    // That is what lets a self-DM row, whose direct target is our OWN user id,
    // adopt the signed-in account's own avatar (fetched for the account
    // switcher) instead of resolving to an initial forever.
    if (userId.isEmpty())
        return;
    if (ok && !avatarUrl.isEmpty()) {
        m_avatars.insert(userId, avatarUrl);
        m_noAvatar.remove(userId);
        // ONLY a learned face is announced. Announcing every answer is what
        // closed the loop: an owner that rebuilds on this signal re-entered
        // resolveMissing(), which found the peer neither cached nor pending
        // and asked again, forever. "Nothing was learned" changes no row, so
        // there is nothing for a consumer to repaint either.
        Q_EMIT avatarResolved(userId);
        return;
    }
    // A profile that answered with no avatar, or a lookup that failed. Both
    // are recorded so the next rebuild does not ask again; neither is
    // announced, because neither changed what any row renders.
    m_noAvatar.insert(userId);
    if (!requestedUser.isEmpty())
        m_noAvatar.insert(requestedUser);
}
