#include "profile/ProfileBioManager.h"

ProfileBioManager::ProfileBioManager(QObject *parent)
    : QObject(parent)
{
}

void ProfileBioManager::setClient(MatrixClient *client)
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
    connect(m_client, &MatrixClient::profileBioReceived, this,
            &ProfileBioManager::handleReceived);
    connect(m_client, &MatrixClient::profileBioSet, this,
            &ProfileBioManager::handleSet);
    // Answers belong to the account that fetched them.
    connect(m_client, &MatrixClient::loggedOut, this,
            &ProfileBioManager::clearSession);
    Q_EMIT availableChanged();
}

bool ProfileBioManager::available() const
{
    return m_client && m_client->supportsProfileBios();
}

QString ProfileBioManager::bioFor(const QString &userId) const
{
    return m_cache.value(userId);
}

QString ProfileBioManager::ownBio() const
{
    if (!m_client)
        return {};
    return bioFor(m_client->currentUserId());
}

void ProfileBioManager::request(const QString &userId)
{
    if (!available() || userId.isEmpty() || !m_supported)
        return;
    // Once per user per session.
    if (m_asked.contains(userId))
        return;
    if (m_cache.size() >= kMaxCached)
        return;
    m_asked.insert(userId);
    const quint64 opId = m_nextOpId++;
    m_inFlight.insert(opId, userId);
    m_client->fetchProfileBio(userId, opId);
}

void ProfileBioManager::refresh(const QString &userId)
{
    if (userId.isEmpty())
        return;
    m_asked.remove(userId);
    request(userId);
}

void ProfileBioManager::setOwnBio(const QString &text)
{
    if (!available() || m_pendingWrite != 0)
        return;
    setLastError({});
    m_pendingWrite = m_nextOpId++;
    Q_EMIT busyChanged();
    // Rust decides that whitespace-only clears, so both paths agree.
    m_client->setProfileBio(text, m_pendingWrite);
}

void ProfileBioManager::clearOwnBio()
{
    setOwnBio(QString());
}

void ProfileBioManager::cache(const QString &userId, const QString &bio)
{
    if (userId.isEmpty())
        return;
    const QString previous = m_cache.value(userId);
    const bool existed = m_cache.contains(userId);
    m_cache.insert(userId, bio);
    if (existed && previous == bio)
        return;
    ++m_revision;
    Q_EMIT revisionChanged();
}

void ProfileBioManager::handleReceived(quint64 opId, const QString &userId,
                                       const QString &bio, bool supported)
{
    const auto it = m_inFlight.constFind(opId);
    if (it == m_inFlight.constEnd())
        return;   // a stale answer from a previous session
    m_inFlight.erase(it);

    if (!supported && m_supported) {
        // Latched for the session: the server does not know the endpoint.
        m_supported = false;
        Q_EMIT supportedChanged();
    }
    cache(userId, bio);
}

void ProfileBioManager::handleSet(quint64 opId, bool ok, const QString &bio,
                                  const QString &category)
{
    if (opId != m_pendingWrite)
        return;
    m_pendingWrite = 0;
    Q_EMIT busyChanged();
    if (!ok) {
        setLastError(category.isEmpty() ? QStringLiteral("failed") : category);
        // An unrecognised write settles it too; latch it, or the account could
        // retry a failing write forever.
        if (category == QLatin1String("unsupported") && m_supported) {
            m_supported = false;
            Q_EMIT supportedChanged();
        }
        return;
    }
    setLastError({});
    // Not optimistic: the server accepted the write, and the cached value is
    // the bounded, sanitized text actually stored.
    if (m_client) {
        const QString own = m_client->currentUserId();
        m_asked.insert(own);
        cache(own, bio);
    }
}

void ProfileBioManager::clearSession()
{
    m_cache.clear();
    m_asked.clear();
    m_inFlight.clear();
    m_pendingWrite = 0;
    m_supported = true;
    m_lastError.clear();
    ++m_revision;
    Q_EMIT revisionChanged();
    Q_EMIT supportedChanged();
    Q_EMIT busyChanged();
    Q_EMIT lastErrorChanged();
}

void ProfileBioManager::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}
