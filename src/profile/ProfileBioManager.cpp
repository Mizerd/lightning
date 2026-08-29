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
    // A bio belongs to the account that fetched it. The next account's profile
    // cards must not inherit the previous one's answers.
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
    // Once per user per session: a profile popover that opens, closes and
    // opens again must not cost two requests.
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
    // Whitespace-only IS a clear; Rust decides that, so the two entry points
    // stay one code path and cannot disagree about what "empty" means.
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
        // Latched for the session: the server has said it does not know the
        // endpoint, so every further request would ask the same question and
        // get the same answer. It clears with the session.
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
        // A write that came back unrecognised settles the same question a read
        // settles, and settles it more definitively — the endpoint is not
        // there. Without this the account could be invited to fail at the same
        // write indefinitely. It clears with the session, like the read latch.
        if (category == QLatin1String("unsupported") && m_supported) {
            m_supported = false;
            Q_EMIT supportedChanged();
        }
        return;
    }
    setLastError({});
    // NOT optimistic: the server has accepted this write, and the value cached
    // is the one the write path reports actually stored — the bounded,
    // sanitized text, not what was typed into the field.
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
