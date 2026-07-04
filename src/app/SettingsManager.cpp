#include "app/SettingsManager.h"

namespace {
constexpr auto kHomeserver          = "homeserver/url";
constexpr auto kTheme               = "ui/theme";
constexpr auto kLanguage            = "ui/language";
constexpr auto kStartMinimized      = "ui/startMinimized";
constexpr auto kNotifications       = "notifications/enabled";
constexpr auto kAccessToken         = "session/accessToken";
constexpr auto kUserId              = "session/userId";
constexpr auto kDeviceId            = "session/deviceId";
constexpr auto kSyncToken           = "session/syncToken";
}

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
    , m_store(std::make_unique<QSettings>())
{
    if (!m_store->contains(kHomeserver)) {
        m_store->setValue(kHomeserver, QStringLiteral("https://matrix.org"));
    }
}

QString SettingsManager::homeserverUrl() const
{
    return m_store->value(kHomeserver, QStringLiteral("https://matrix.org")).toString();
}

void SettingsManager::setHomeserverUrl(const QString &url)
{
    if (homeserverUrl() == url)
        return;
    m_store->setValue(kHomeserver, url);
    Q_EMIT homeserverUrlChanged();
}

SettingsManager::Theme SettingsManager::theme() const
{
    return static_cast<Theme>(m_store->value(kTheme, SystemTheme).toInt());
}

void SettingsManager::setTheme(Theme t)
{
    if (theme() == t)
        return;
    m_store->setValue(kTheme, static_cast<int>(t));
    Q_EMIT themeChanged();
}

QString SettingsManager::language() const
{
    return m_store->value(kLanguage, QStringLiteral("en")).toString();
}

void SettingsManager::setLanguage(const QString &lang)
{
    if (language() == lang)
        return;
    m_store->setValue(kLanguage, lang);
    Q_EMIT languageChanged();
}

bool SettingsManager::startMinimized() const
{
    return m_store->value(kStartMinimized, false).toBool();
}

void SettingsManager::setStartMinimized(bool v)
{
    if (startMinimized() == v)
        return;
    m_store->setValue(kStartMinimized, v);
    Q_EMIT startMinimizedChanged();
}

bool SettingsManager::notificationsEnabled() const
{
    return m_store->value(kNotifications, true).toBool();
}

void SettingsManager::setNotificationsEnabled(bool v)
{
    if (notificationsEnabled() == v)
        return;
    m_store->setValue(kNotifications, v);
    Q_EMIT notificationsEnabledChanged();
}

bool SettingsManager::hasSession() const
{
    return !accessToken().isEmpty() && !userId().isEmpty() && !homeserverUrl().isEmpty();
}

QString SettingsManager::accessToken() const
{
    return m_store->value(kAccessToken).toString();
}

QString SettingsManager::userId() const
{
    return m_store->value(kUserId).toString();
}

QString SettingsManager::deviceId() const
{
    return m_store->value(kDeviceId).toString();
}

QString SettingsManager::syncToken() const
{
    return m_store->value(kSyncToken).toString();
}

void SettingsManager::saveSession(const QString &homeserverUrl_,
                                  const QString &userId_,
                                  const QString &deviceId_,
                                  const QString &accessToken_)
{
    const QString prevUser = userId();
    const bool hsChanged = homeserverUrl() != homeserverUrl_;

    m_store->setValue(kHomeserver, homeserverUrl_);
    m_store->setValue(kUserId, userId_);
    m_store->setValue(kDeviceId, deviceId_);
    m_store->setValue(kAccessToken, accessToken_);

    // If the account changed, the previous sync token belongs to a different
    // access token and must not be reused.
    if (prevUser != userId_) {
        m_store->remove(kSyncToken);
    }

    if (hsChanged)
        Q_EMIT homeserverUrlChanged();
    Q_EMIT sessionChanged();
}

void SettingsManager::setSyncToken(const QString &token)
{
    if (syncToken() == token)
        return;
    m_store->setValue(kSyncToken, token);
}

void SettingsManager::clearSession()
{
    if (!hasSession())
        return;
    m_store->remove(kAccessToken);
    m_store->remove(kUserId);
    m_store->remove(kDeviceId);
    m_store->remove(kSyncToken);
    Q_EMIT sessionChanged();
}
