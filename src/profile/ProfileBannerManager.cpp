#include "profile/ProfileBannerManager.h"

ProfileBannerManager::ProfileBannerManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileBannerManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clearSession();
    if (!m_client) {
        Q_EMIT availableChanged();
        return;
    }
    connect(m_client, &MatrixClient::profileBannerReceived, this,
            &ProfileBannerManager::handleReceived);
    connect(m_client, &MatrixClient::profileBannerSet, this,
            &ProfileBannerManager::handleSet);
    // A banner belongs to the account that fetched it. The next account's
    // profile cards must not inherit the previous one's answers.
    connect(m_client, &MatrixClient::loggedOut, this,
            &ProfileBannerManager::clearSession);
    Q_EMIT availableChanged();
}

bool ProfileBannerManager::available() const
{
    return m_client && m_client->supportsProfileBanners();
}

QString ProfileBannerManager::bannerFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileBannerManager::ownBanner() const
{
    if (!m_client)
        return {};
    return bannerFor(m_client->currentUserId());
}

void ProfileBannerManager::request(const QString &userId)
{
    if (!available() || userId.isEmpty() || !m_supported)
        return;
    // Once per user per session, and once per user in flight: a profile
    // popover that opens, closes and opens again must not cost two requests.
    if (m_asked.contains(userId))
        return;
    if (m_cache.size() >= kMaxCached)
        return;
    m_asked.insert(userId);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileBanner(userId, opId);
}

void ProfileBannerManager::setOwnBanner(const QString &localPath)
{
    if (!available() || localPath.isEmpty() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    m_client->setProfileBanner(localPath, m_pendingWrite);
}

void ProfileBannerManager::clearOwnBanner()
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    // An empty path IS the clear; the Rust side deletes both field names.
    m_client->setProfileBanner(QString(), m_pendingWrite);
}

void ProfileBannerManager::handleReceived(quint64 opId, const QString &userId,
                                          const QString &mxc, bool supported)
{
    const auto it = m_inFlight.constFind(opId);
    if (it == m_inFlight.constEnd())
        return;   // a stale answer from a previous session
    m_inFlight.erase(it);

    if (!supported && m_supported) {
        // Latched for the session: the server has said it does not know the
        // endpoint, so every further request would ask the same question and
        // get the same answer. It clears with the session.
        m_supported = false;
        Q_EMIT supportedChanged();
    }
    const QString previous = m_cache.value(userId);
    m_cache.insert(userId, mxc);
    if (previous == mxc)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::handleSet(quint64 opId, bool ok, const QString &mxc,
                                     const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        return;
    }
    setLastError({});
    if (!m_client)
        return;
    // Nothing is applied optimistically anywhere else in this application and
    // nothing is here either — but the server has now ACKNOWLEDGED the write,
    // so the local answer for our own user is authoritative and does not need
    // a round trip to re-read.
    const QString self = m_client->currentUserId();
    if (self.isEmpty())
        return;
    m_cache.insert(self, mxc);
    m_asked.insert(self);
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBannerManager::clearSession()
{
    m_cache.clear();
    m_asked.clear();
    m_inFlight.clear();
    m_pendingWrite = 0;
    setLastError({});
    // A different account may be on a server that DOES implement extended
    // profiles; the latch is per session.
    if (!m_supported) {
        m_supported = true;
        Q_EMIT supportedChanged();
    }
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT busyChanged();
}

void ProfileBannerManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
