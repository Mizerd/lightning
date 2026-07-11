#include "app/SettingsManager.h"

#include "storage/SecretStore.h"
#include "storage/AppDataPaths.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSettings, "matrix.settings")

namespace {
constexpr auto kHomeserver          = "homeserver/url";
constexpr auto kTheme               = "ui/theme";
constexpr auto kLanguage            = "ui/language";
constexpr auto kStartMinimized      = "ui/startMinimized";
constexpr auto kNotifications       = "notifications/enabled";
constexpr auto kRecentEmoji         = "emoji/recent";
constexpr auto kPreferredEmojiTone  = "emoji/preferredTone";
// v0.5.11: link previews. The encrypted-room key MUST default to false —
// requesting a preview reveals the URL to the homeserver, which encrypted
// rooms never do without an explicit user decision.
constexpr auto kPreviewsUnencrypted = "previews/autoLoadUnencrypted";
constexpr auto kPreviewsEncrypted   = "previews/loadInEncryptedRooms";
constexpr auto kPreviewsAnimateGifs = "previews/animateGifs";
constexpr int kRecentEmojiLimit     = 32;
// v0.2/v0.3 stored the access token here in plaintext. v0.4 migrates it out
// on first read; the key stays defined only so the migration code can find
// and delete the legacy value.
constexpr auto kAccessTokenLegacy   = "session/accessToken";
constexpr auto kUserId              = "session/userId";
constexpr auto kDeviceId            = "session/deviceId";
constexpr auto kSyncToken           = "session/syncToken";

// SecretStore keys.
constexpr auto kSecretAccessToken   = "accessToken";
}

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
    , m_store(std::make_unique<QSettings>())
{
    if (!m_store->contains(kHomeserver)) {
        m_store->setValue(kHomeserver, QStringLiteral("https://matrix.org"));
    }
}

void SettingsManager::setSecretStore(SecretStore *store)
{
    if (m_secretStore == store)
        return;
    m_secretStore = store;
    migratePlaintextTokenIfPresent();
    Q_EMIT secretBackendChanged();
    Q_EMIT sessionChanged();
}

void SettingsManager::migratePlaintextTokenIfPresent()
{
    if (!m_secretStore)
        return;
    if (!m_store->contains(kAccessTokenLegacy))
        return;
    const QString legacyToken = m_store->value(kAccessTokenLegacy).toString();
    const QString uid = userId();
    if (legacyToken.isEmpty() || uid.isEmpty()) {
        // Nothing useful to migrate; just clean up.
        m_store->remove(kAccessTokenLegacy);
        return;
    }
    if (m_secretStore->storeSecret(uid, QLatin1String(kSecretAccessToken), legacyToken)) {
        qCInfo(lcSettings)
            << "migrated legacy plaintext access token for" << uid
            << "into" << m_secretStore->backendName();
        m_store->remove(kAccessTokenLegacy);
    } else {
        qCWarning(lcSettings)
            << "failed to migrate plaintext access token — leaving in place;"
            << "SecretStore error:" << m_secretStore->lastError();
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

bool SettingsManager::autoLoadLinkPreviews() const
{
    return m_store->value(kPreviewsUnencrypted, true).toBool();
}

void SettingsManager::setAutoLoadLinkPreviews(bool v)
{
    if (autoLoadLinkPreviews() == v)
        return;
    m_store->setValue(kPreviewsUnencrypted, v);
    Q_EMIT autoLoadLinkPreviewsChanged();
}

bool SettingsManager::loadPreviewsInEncryptedRooms() const
{
    // Privacy default: never reveal encrypted-room URLs to the homeserver
    // automatically.
    return m_store->value(kPreviewsEncrypted, false).toBool();
}

void SettingsManager::setLoadPreviewsInEncryptedRooms(bool v)
{
    if (loadPreviewsInEncryptedRooms() == v)
        return;
    m_store->setValue(kPreviewsEncrypted, v);
    Q_EMIT loadPreviewsInEncryptedRoomsChanged();
}

bool SettingsManager::animateGifPreviews() const
{
    return m_store->value(kPreviewsAnimateGifs, true).toBool();
}

void SettingsManager::setAnimateGifPreviews(bool v)
{
    if (animateGifPreviews() == v)
        return;
    m_store->setValue(kPreviewsAnimateGifs, v);
    Q_EMIT animateGifPreviewsChanged();
}

QStringList SettingsManager::recentEmoji() const
{
    return m_store->value(kRecentEmoji).toStringList();
}

void SettingsManager::recordRecentEmoji(const QString &emoji)
{
    if (emoji.isEmpty()) return;
    QStringList recent = recentEmoji();
    recent.removeAll(emoji);
    recent.prepend(emoji);
    while (recent.size() > kRecentEmojiLimit) recent.removeLast();
    m_store->setValue(kRecentEmoji, recent);
}

void SettingsManager::clearRecentEmoji() { m_store->remove(kRecentEmoji); }

QString SettingsManager::preferredEmojiTone() const
{
    return m_store->value(kPreferredEmojiTone).toString();
}

void SettingsManager::setPreferredEmojiTone(const QString &tone)
{
    m_store->setValue(kPreferredEmojiTone, tone);
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
    const QString uid = userId();
    if (uid.isEmpty())
        return {};
    if (m_secretStore) {
        return m_secretStore->readSecret(uid, QLatin1String(kSecretAccessToken));
    }
    // No SecretStore wired yet — fall back to the legacy plaintext key so we
    // don't lose a running session between refactor steps. This branch is
    // unreachable in normal execution because AppController always wires a
    // SecretStore before touching accessToken().
    return m_store->value(kAccessTokenLegacy).toString();
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

bool SettingsManager::secretsAreSecure() const
{
    return m_secretStore && m_secretStore->isSecure() && m_secretStore->isAvailable();
}

QString SettingsManager::secretBackendName() const
{
    return m_secretStore ? m_secretStore->backendName()
                         : QStringLiteral("no secret store");
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

    if (m_secretStore) {
        if (!m_secretStore->storeSecret(userId_, QLatin1String(kSecretAccessToken), accessToken_)) {
            qCWarning(lcSettings)
                << "failed to persist access token to SecretStore:"
                << m_secretStore->lastError();
        }
        // Make sure a stale legacy plaintext token is not left behind.
        m_store->remove(kAccessTokenLegacy);
    } else {
        // Unwired store — same reasoning as accessToken(): keep the process
        // working, but this branch should not fire in normal execution.
        m_store->setValue(kAccessTokenLegacy, accessToken_);
    }

    if (prevUser != userId_) {
        m_store->remove(kSyncToken);
        if (m_secretStore && !prevUser.isEmpty()) {
            m_secretStore->clearAccountSecrets(prevUser);
        }
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

bool SettingsManager::clearSession()
{
    const QString uid = userId();
    if (uid.isEmpty()) {
        m_store->remove(kDeviceId);
        m_store->remove(kSyncToken);
        m_store->remove(kAccessTokenLegacy);
        return true;
    }
    return clearSessionForAccount(uid);
}

bool SettingsManager::clearSessionForAccount(const QString &uid)
{
    const QString target = uid.trimmed();
    if (target.isEmpty())
        return false;

    const QString activeUser = userId();
    bool activeAccount = activeUser == target;
    if (!activeAccount && !activeUser.isEmpty()) {
        matrix::app_data::AccountIdentity activeIdentity;
        matrix::app_data::AccountIdentity targetIdentity;
        activeAccount = matrix::app_data::resolveAccountIdentity(
                            homeserverUrl(), activeUser, &activeIdentity)
            && matrix::app_data::resolveAccountIdentity(
                homeserverUrl(), target, &targetIdentity)
            && activeIdentity.userId == targetIdentity.userId;
    }
    if (activeAccount) {
        m_store->remove(kUserId);
        m_store->remove(kDeviceId);
        m_store->remove(kSyncToken);
        m_store->remove(kAccessTokenLegacy);
    }

    bool secretsCleared = true;
    if (m_secretStore) {
        // Use the exact key originally persisted for a normalized match so a
        // legacy mixed-case homeserver cannot orphan its SecretStore entry.
        const QString secretUser = activeAccount ? activeUser : target;
        secretsCleared = m_secretStore->clearAccountSecrets(secretUser);
        if (!secretsCleared) {
            qCWarning(lcSettings)
                << "failed to clear account secrets from SecretStore:"
                << m_secretStore->lastError();
        }
    }

    if (activeAccount)
        Q_EMIT sessionChanged();
    return secretsCleared;
}
