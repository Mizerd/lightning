#include "profile/UserProfileResolver.h"

#include "matrix/MatrixClient.h"

UserProfileResolver::UserProfileResolver(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
}

void UserProfileResolver::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clear();
    if (m_client) {
        connect(m_client, &MatrixClient::userProfileFinished, this,
                &UserProfileResolver::onFinished);
    }
}

void UserProfileResolver::clear()
{
    m_profiles.clear();
    m_inFlight.clear();
    m_asking.clear();
    m_failedAt.clear();
}

UserProfileResolver::Profile
UserProfileResolver::profile(const QString &userId) const
{
    return m_profiles.value(userId);
}

QVariantMap UserProfileResolver::lookup(const QString &userId)
{
    request(userId);
    const Profile p = m_profiles.value(userId);
    QVariantMap out;
    out.insert(QStringLiteral("displayName"), p.displayName);
    out.insert(QStringLiteral("avatarUrl"), p.avatarUrl);
    out.insert(QStringLiteral("known"), p.known);
    return out;
}

void UserProfileResolver::request(const QString &userId)
{
    if (!m_client || userId.isEmpty() || !userId.startsWith(QLatin1Char('@'))
        || !userId.contains(QLatin1Char(':')))
        return;
    if (m_profiles.contains(userId) || m_asking.contains(userId))
        return;
    const auto failed = m_failedAt.constFind(userId);
    if (failed != m_failedAt.constEnd()
        && m_clock.elapsed() - failed.value() < m_failureRetryMs)
        return;
    if (m_profiles.size() >= kMaxProfiles)
        return;
    const quint64 op = m_client->fetchUserProfile(userId);
    if (op == 0)
        return;
    m_inFlight.insert(op, userId);
    m_asking.insert(userId);
}

void UserProfileResolver::onFinished(quint64 opId, bool ok,
                                     const QString &userId,
                                     const QString &displayName,
                                     const QString &avatarUrl,
                                     const QString &category)
{
    Q_UNUSED(category);
    const auto it = m_inFlight.find(opId);
    if (it == m_inFlight.end())
        return; // somebody else's fetch (the controller asks for its own)
    const QString asked = it.value();
    m_inFlight.erase(it);
    m_asking.remove(asked);
    // The answer names the user it is about; a mismatch is a backend fault
    // and is not stored under the user that was asked for.
    if (!ok || (!userId.isEmpty() && userId != asked)) {
        m_failedAt.insert(asked, m_clock.elapsed());
        return;
    }
    Profile p;
    p.displayName = displayName.trimmed();
    p.avatarUrl = avatarUrl.startsWith(QLatin1String("mxc://")) ? avatarUrl
                                                                 : QString();
    p.known = true;
    m_profiles.insert(asked, p);
    m_failedAt.remove(asked);
    Q_EMIT resolved(asked, p.displayName, p.avatarUrl);
}
