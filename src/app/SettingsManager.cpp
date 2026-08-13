#include "app/SettingsManager.h"

#include "storage/SecretStore.h"
#include "storage/AppDataPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QLoggingCategory>

#include <algorithm>

Q_LOGGING_CATEGORY(lcSettings, "matrix.settings")

namespace {
constexpr auto kHomeserver          = "homeserver/url";
constexpr auto kTheme               = "ui/theme";
constexpr auto kMessageLayout       = "ui/messageLayout";
constexpr auto kTextScale           = "ui/textScale";
constexpr auto kUiFont              = "ui/uiFont";
constexpr auto kLanguage            = "ui/language";
constexpr auto kStartMinimized      = "ui/startMinimized";
constexpr auto kCustomAppIcon       = "ui/customAppIconEnabled";
constexpr auto kNotifications       = "notifications/enabled";
constexpr auto kRecentEmoji         = "emoji/recent";
constexpr auto kPreferredEmojiTone  = "emoji/preferredTone";
// v0.5.11: link previews. The encrypted-room key MUST default to false —
// requesting a preview reveals the URL to the homeserver, which encrypted
// rooms never do without an explicit user decision.
constexpr auto kPreviewsUnencrypted = "previews/autoLoadUnencrypted";
constexpr auto kPreviewsEncrypted   = "previews/loadInEncryptedRooms";
constexpr auto kPreviewsAnimateGifs = "previews/animateGifs";
// v0.6.1: GIF browser policy.
constexpr auto kGifAutoplay         = "gif/autoplay";       // 0/1/2
constexpr auto kGifSafeSearch       = "gif/safeSearch";     // gif::Rating id
constexpr auto kGifStoreRecent      = "gif/storeRecent";    // bool
constexpr auto kGifProvider         = "gif/provider";       // "giphy"/"klipy"
// Presentation-only timeline preference. The underlying SDK/model retains
// every state event so changing this never requires a resync.
constexpr auto kShowRoomActivity    = "timeline/showRoomActivity";
// v0.5.19: 0=Standard, 1=Fast, 2=Very fast (see TimelineScrollController).
constexpr auto kTimelineWheelSpeed  = "timeline/wheelSpeed";
constexpr int kRecentEmojiLimit     = 32;
// v0.2/v0.3 stored the access token here in plaintext. v0.4 migrates it out
// on first read; the key stays defined only so the migration code can find
// and delete the legacy value.
constexpr auto kAccessTokenLegacy   = "session/accessToken";
// Pre-0.7 single-session metadata. Migrated into accounts/<slug>/ on first
// start; the keys stay defined only for that migration.
constexpr auto kUserId              = "session/userId";
constexpr auto kDeviceId            = "session/deviceId";
constexpr auto kSyncToken           = "session/syncToken";

// v0.7 multi-account registry.
constexpr auto kAccountsGroup       = "accounts";
constexpr auto kActiveAccount       = "accounts/active";
constexpr auto kAccountUserId       = "userId";
constexpr auto kAccountHomeserver   = "homeserver";
constexpr auto kAccountDeviceId     = "deviceId";
constexpr auto kAccountDisplayName  = "displayName";
constexpr auto kAccountAvatarUrl    = "avatarUrl";
constexpr auto kAccountAddedAt      = "addedAt";
constexpr auto kAccountSyncToken    = "syncToken";
// The account's real on-disk SDK store directory name, recorded at login
// instead of re-derived. See SettingsManager::storeSlugFor.
constexpr auto kAccountStoreSlug    = "storeSlug";

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
    migrateLegacySessionRecord();
}

QString SettingsManager::accountKey(const QString &slug, const char *subKey) const
{
    return QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1Char('/') + QLatin1String(subKey);
}

QString SettingsManager::slugForSavedAccount(const QString &userId) const
{
    const QString slug = matrix::app_data::safeUserSlug(userId.trimmed());
    if (slug.isEmpty())
        return {};
    // The slug substitution is not injective (distinct identities can
    // flatten to the same slug), so a record only belongs to the queried
    // account when its stored canonical user id matches exactly.
    const QString stored =
        m_store->value(accountKey(slug, kAccountUserId)).toString();
    return stored == userId.trimmed() ? slug : QString{};
}

bool SettingsManager::accountSlugConflicts(const QString &userId) const
{
    const QString uid = userId.trimmed();
    const QString slug = matrix::app_data::safeUserSlug(uid);
    if (slug.isEmpty())
        return false;
    const QString stored =
        m_store->value(accountKey(slug, kAccountUserId)).toString();
    return !stored.isEmpty() && stored != uid;
}

QString SettingsManager::canonicalUserIdForTypedIdentity(
    const QString &typedUserId, bool *ambiguous) const
{
    if (ambiguous)
        *ambiguous = false;
    const QString typed = typedUserId.trimmed();
    if (typed.isEmpty())
        return {};

    // Exact match wins outright — no scan, and no chance of a
    // case-insensitive sibling stealing a genuine uppercase-localpart
    // account (those are legal on older homeservers).
    if (!slugForSavedAccount(typed).isEmpty())
        return typed;

    const qsizetype colon = typed.indexOf(QLatin1Char(':'));
    if (!typed.startsWith(QLatin1Char('@')) || colon <= 1)
        return {};
    const QString localpart = typed.mid(1, colon - 1);
    const QString serverName = typed.mid(colon + 1);

    QString match;
    for (const QString &saved : savedAccountUserIds()) {
        const qsizetype savedColon = saved.indexOf(QLatin1Char(':'));
        if (!saved.startsWith(QLatin1Char('@')) || savedColon <= 1)
            continue;
        if (saved.mid(savedColon + 1) != serverName)
            continue;
        if (saved.mid(1, savedColon - 1)
                .compare(localpart, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!match.isEmpty()) {
            // Two saved accounts differ only by localpart case. Both are
            // legitimate identities; picking one would hand this login the
            // wrong store. Refuse.
            if (ambiguous)
                *ambiguous = true;
            return {};
        }
        match = saved;
    }
    return match;
}

QString SettingsManager::storeSlugFor(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return {};
    return m_store->value(accountKey(slug, kAccountStoreSlug)).toString();
}

void SettingsManager::setStoreSlugFor(const QString &userId,
                                      const QString &storeSlug)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return;
    const QString key = accountKey(slug, kAccountStoreSlug);
    if (storeSlug.trimmed().isEmpty()) {
        m_store->remove(key);
    } else {
        if (m_store->value(key).toString() == storeSlug)
            return;
        m_store->setValue(key, storeSlug);
    }
    // The store directory already exists on disk by the time this is called;
    // the mapping to it must not be the thing that is lost in a crash.
    m_store->sync();
}

bool SettingsManager::secretBackendUnavailable() const
{
    if (!m_secretStore)
        return true;
    // isAvailable() alone is not enough: it is a construction-time probe, and
    // createDefault() only ever returns a backend that probed available, so it
    // can never report a keyring that locks AFTER startup — which is the
    // common case. lastReadFailed() reports the outcome of the actual read,
    // which is what callers are really asking about when they are deciding
    // whether an empty token means "no account" or "cannot tell".
    return !m_secretStore->isAvailable() || m_secretStore->lastReadFailed();
}

QString SettingsManager::accountOwningStoreSlug(const QString &storeSlug) const
{
    const QString slug = storeSlug.trimmed();
    if (slug.isEmpty())
        return {};
    for (const QString &userId : savedAccountUserIds()) {
        if (matrix::app_data::safeUserSlug(userId) == slug)
            return userId;
        if (storeSlugFor(userId) == slug)
            return userId;
        // The delegated reconstruction: a bare-localpart login against a
        // .well-known-delegated homeserver produced a slug built from the URL
        // host, which matches neither of the above and involves no casing at
        // all. Without this an account's real store looks unowned.
        matrix::app_data::AccountIdentity identity;
        if (resolveSavedIdentity(userId, &identity)
            && matrix::app_data::delegatedHomeserverStoreSlug(identity) == slug) {
            return userId;
        }
    }
    return {};
}

bool SettingsManager::resolveSavedIdentity(
    const QString &userId, matrix::app_data::AccountIdentity *out) const
{
    if (!out)
        return false;
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return false;
    const QString hs =
        m_store->value(accountKey(slug, kAccountHomeserver)).toString();
    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(hs, userId, &identity))
        return false;
    // A recorded store location always wins over the derived one. An
    // unusable recording (unsafe or wrongly scoped) is ignored rather than
    // applied — bindStoreSlug refuses instead of half-applying.
    const QString recorded =
        m_store->value(accountKey(slug, kAccountStoreSlug)).toString();
    if (!recorded.isEmpty() && recorded != identity.slug)
        matrix::app_data::bindStoreSlug(&identity, recorded);
    *out = identity;
    return true;
}

void SettingsManager::migrateLegacySessionRecord()
{
    // Pre-0.7 builds kept exactly one session under session/*. Convert it
    // into the first accounts/<slug>/ record so the account survives the
    // multi-account upgrade, then drop the legacy keys.
    QString legacyUser = m_store->value(kUserId).toString().trimmed();
    if (legacyUser.isEmpty())
        return;
    matrix::app_data::AccountIdentity identity;
    if (matrix::app_data::resolveAccountIdentity(
            m_store->value(kHomeserver).toString(), legacyUser, &identity)) {
        legacyUser = identity.userId;
    }
    const QString slug = matrix::app_data::safeUserSlug(legacyUser);
    if (slug.isEmpty()) {
        qCWarning(lcSettings)
            << "legacy session user id is not a safe account id; leaving as-is";
        return;
    }
    if (!m_store->contains(accountKey(slug, kAccountUserId))) {
        m_store->setValue(accountKey(slug, kAccountUserId), legacyUser);
        m_store->setValue(accountKey(slug, kAccountHomeserver),
                          identity.isValid()
                              ? identity.homeserver
                              : m_store->value(kHomeserver).toString());
        m_store->setValue(accountKey(slug, kAccountDeviceId),
                          m_store->value(kDeviceId).toString());
        m_store->setValue(accountKey(slug, kAccountSyncToken),
                          m_store->value(kSyncToken).toString());
        m_store->setValue(accountKey(slug, kAccountAddedAt),
                          QDateTime::currentDateTimeUtc()
                              .toString(Qt::ISODate));
        qCInfo(lcSettings) << "migrated legacy session into account record";
    }
    if (!m_store->contains(kActiveAccount))
        m_store->setValue(kActiveAccount, legacyUser);
    m_store->remove(kUserId);
    m_store->remove(kDeviceId);
    m_store->remove(kSyncToken);
}

bool SettingsManager::upsertAccountRecord(const QString &userId,
                                          const QString &homeserver,
                                          const QString &deviceId)
{
    const QString uid = userId.trimmed();
    const QString slug = matrix::app_data::safeUserSlug(uid);
    if (slug.isEmpty()) {
        qCWarning(lcSettings)
            << "refusing to save account record for unsafe user id";
        return false;
    }
    // Never clobber a different account whose identity flattens to the same
    // slug — that would also alias both accounts onto one on-disk SDK store.
    const QString existing =
        m_store->value(accountKey(slug, kAccountUserId)).toString();
    if (!existing.isEmpty() && existing != uid) {
        qCWarning(lcSettings)
            << "refusing account record: slug collision with a different "
               "saved account";
        return false;
    }
    const bool isNew = existing.isEmpty();
    m_store->setValue(accountKey(slug, kAccountUserId), uid);
    m_store->setValue(accountKey(slug, kAccountHomeserver), homeserver);
    m_store->setValue(accountKey(slug, kAccountDeviceId), deviceId);
    if (isNew) {
        m_store->setValue(accountKey(slug, kAccountAddedAt),
                          QDateTime::currentDateTimeUtc()
                              .toString(Qt::ISODate));
    }
    Q_EMIT accountsChanged();
    return true;
}

QStringList SettingsManager::savedAccountUserIds() const
{
    struct Entry {
        QString addedAt;
        QString userId;
    };
    QList<Entry> entries;
    m_store->beginGroup(QLatin1String(kAccountsGroup));
    const QStringList groups = m_store->childGroups();
    for (const QString &slug : groups) {
        const QString uid =
            m_store->value(slug + QLatin1String("/") + QLatin1String(kAccountUserId))
                .toString();
        if (uid.isEmpty())
            continue;
        entries.append({m_store
                            ->value(slug + QLatin1String("/")
                                    + QLatin1String(kAccountAddedAt))
                            .toString(),
                        uid});
    }
    m_store->endGroup();
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.addedAt != b.addedAt)
                      return a.addedAt < b.addedAt;
                  return a.userId < b.userId;
              });
    QStringList ids;
    ids.reserve(entries.size());
    for (const Entry &e : entries)
        ids.append(e.userId);
    return ids;
}

bool SettingsManager::hasSavedAccount(const QString &userId) const
{
    return !slugForSavedAccount(userId).isEmpty();
}

QVariantMap SettingsManager::accountRecord(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return {};
    QVariantMap record;
    record.insert(QStringLiteral("userId"),
                  m_store->value(accountKey(slug, kAccountUserId)).toString());
    record.insert(QStringLiteral("homeserver"),
                  m_store->value(accountKey(slug, kAccountHomeserver)).toString());
    record.insert(QStringLiteral("deviceId"),
                  m_store->value(accountKey(slug, kAccountDeviceId)).toString());
    record.insert(QStringLiteral("displayName"),
                  m_store->value(accountKey(slug, kAccountDisplayName)).toString());
    record.insert(QStringLiteral("avatarUrl"),
                  m_store->value(accountKey(slug, kAccountAvatarUrl)).toString());
    record.insert(QStringLiteral("addedAt"),
                  m_store->value(accountKey(slug, kAccountAddedAt)).toString());
    return record;
}

QString SettingsManager::accessTokenFor(const QString &userId) const
{
    const QString uid = userId.trimmed();
    if (uid.isEmpty() || !m_secretStore)
        return {};
    return m_secretStore->readSecret(uid, QLatin1String(kSecretAccessToken));
}

QString SettingsManager::activeAccountUserId() const
{
    const QString uid = m_store->value(kActiveAccount).toString();
    if (uid.isEmpty())
        return {};
    // Self-heal: an active pointer whose record is gone means no session.
    return hasSavedAccount(uid) ? uid : QString{};
}

void SettingsManager::setActiveAccountUserId(const QString &userId)
{
    const QString uid = userId.trimmed();
    const QString next = (!uid.isEmpty() && hasSavedAccount(uid)) ? uid : QString{};
    if (activeAccountUserId() == next)
        return;
    if (next.isEmpty())
        m_store->remove(kActiveAccount);
    else
        m_store->setValue(kActiveAccount, next);
    m_activeSlugCacheUserId.clear();
    m_activeSlugCache.clear();
    Q_EMIT sessionChanged();
    Q_EMIT homeserverUrlChanged();
    // Appearance is per-account: the switched-to account may resolve
    // different values, so consumers must re-read them.
    Q_EMIT themeChanged();
    Q_EMIT messageLayoutChanged();
    Q_EMIT textScaleChanged();
    Q_EMIT uiFontChanged();
}

void SettingsManager::updateAccountProfile(const QString &userId,
                                           const QString &displayName,
                                           const QString &avatarUrl)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return;
    const bool changed =
        m_store->value(accountKey(slug, kAccountDisplayName)).toString()
            != displayName
        || m_store->value(accountKey(slug, kAccountAvatarUrl)).toString()
            != avatarUrl;
    if (!changed)
        return;
    m_store->setValue(accountKey(slug, kAccountDisplayName), displayName);
    m_store->setValue(accountKey(slug, kAccountAvatarUrl), avatarUrl);
    Q_EMIT accountsChanged();
}

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
void SettingsManager::registerDemoAccount(const QString &homeserverUrl,
                                          const QString &userId,
                                          const QString &displayName,
                                          const QString &avatarUrl,
                                          int order)
{
    const QString uid = userId.trimmed();
    const QString slug = matrix::app_data::safeUserSlug(uid);
    if (slug.isEmpty())
        return;
    m_store->setValue(accountKey(slug, kAccountUserId), uid);
    m_store->setValue(accountKey(slug, kAccountHomeserver), homeserverUrl);
    // A stable fictional device id — this is metadata for the Sessions UI, not
    // a credential. Deterministic so screenshots reproduce.
    m_store->setValue(accountKey(slug, kAccountDeviceId),
                      QStringLiteral("DEMODEVICE%1").arg(order));
    m_store->setValue(accountKey(slug, kAccountDisplayName), displayName);
    m_store->setValue(accountKey(slug, kAccountAvatarUrl), avatarUrl);
    // Deterministic addedAt (NOT wall-clock) so savedAccountUserIds() orders the
    // switcher rows identically on every launch, regardless of registration
    // timing. Second-resolution ISO strings sort lexicographically = by order.
    m_store->setValue(accountKey(slug, kAccountAddedAt),
                      QStringLiteral("2026-07-23T09:%1:00")
                          .arg(order, 2, 10, QLatin1Char('0')));
    // Deliberately NO SecretStore write: demo accounts carry no token. The mock
    // account-switch path is exempt from the token check, so none is needed.
    Q_EMIT accountsChanged();
}

void SettingsManager::clearDemoAccounts()
{
    const QStringList ids = savedAccountUserIds();
    m_store->beginGroup(QLatin1String(kAccountsGroup));
    const QStringList groups = m_store->childGroups();
    m_store->endGroup();
    for (const QString &slug : groups)
        m_store->remove(QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug);
    m_store->remove(kActiveAccount);
    if (m_secretStore) {
        for (const QString &uid : ids)
            m_secretStore->clearAccountSecrets(uid);
    }
    Q_EMIT accountsChanged();
    Q_EMIT sessionChanged();
}
#endif // LIGHTNING_ENABLE_SCREENSHOT_DEMO

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

    // Legacy single-session token (pre-multi-account): session/accessToken.
    if (m_store->contains(kAccessTokenLegacy)) {
        const QString legacyToken = m_store->value(kAccessTokenLegacy).toString();
        const QString uid = userId();
        if (legacyToken.isEmpty() || uid.isEmpty()) {
            // Nothing useful to migrate; just clean up.
            m_store->remove(kAccessTokenLegacy);
        } else if (m_secretStore->storeSecret(uid, QLatin1String(kSecretAccessToken), legacyToken)) {
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

    // Multi-account plaintext tokens left by an earlier InsecureFallback run
    // (secrets/<safeUser>/accessToken). Move them into the now-secure store —
    // otherwise a Windows user who once ran the insecure fallback keeps tokens
    // in the registry after upgrading to the Credential Manager backend.
    migrateInsecureSecretsGroup();
}

void SettingsManager::migrateInsecureSecretsGroup()
{
    // Only migrate INTO a genuinely secure backend; never plaintext->plaintext.
    if (!m_secretStore || !m_secretStore->isSecure())
        return;

    m_store->beginGroup(QStringLiteral("secrets"));
    const QStringList accounts = m_store->childGroups();
    m_store->endGroup();
    if (accounts.isEmpty())
        return;

    int migrated = 0;
    int failed = 0;
    for (const QString &safeUser : accounts) {
        const QString plainKey = QStringLiteral("secrets/%1/%2")
            .arg(safeUser, QLatin1String(kSecretAccessToken));
        if (!m_store->contains(plainKey))
            continue;
        const QString token = m_store->value(plainKey).toString();
        const QString groupKey = QStringLiteral("secrets/%1").arg(safeUser);
        if (token.isEmpty()) {
            m_store->remove(groupKey);
            continue;
        }
        // safeUser equals the MXID for every valid id: InsecureFallback only
        // substitutes '/' and '\\', which a Matrix user id never contains.
        // Verify the secure write read-backs before deleting the plaintext, so
        // a failed write never locks the user out of their session.
        if (m_secretStore->storeSecret(safeUser, QLatin1String(kSecretAccessToken), token)
            && m_secretStore->readSecret(safeUser, QLatin1String(kSecretAccessToken)) == token) {
            m_store->remove(groupKey);
            ++migrated;
        } else {
            ++failed;   // keep plaintext; do not claim success
        }
    }

    if (migrated > 0 || failed > 0)
        m_store->sync();
    if (migrated > 0)
        qCInfo(lcSettings) << "Secure credential migration: completed for"
                           << migrated << "account(s) into"
                           << m_secretStore->backendName();
    if (failed > 0)
        qCWarning(lcSettings)
            << "Secure credential migration: failed for" << failed
            << "account(s) — plaintext left in place; SecretStore error:"
            << m_secretStore->lastError();
}

QString SettingsManager::homeserverUrl() const
{
    // The active account's homeserver when one is selected; otherwise the
    // login-screen prefill value.
    const QString active = activeAccountUserId();
    if (!active.isEmpty()) {
        const QString slug = slugForSavedAccount(active);
        const QString hs =
            m_store->value(accountKey(slug, kAccountHomeserver)).toString();
        if (!hs.isEmpty())
            return hs;
    }
    return m_store->value(kHomeserver, QStringLiteral("https://matrix.org")).toString();
}

void SettingsManager::setHomeserverUrl(const QString &url)
{
    if (homeserverUrl() == url)
        return;
    m_store->setValue(kHomeserver, url);
    Q_EMIT homeserverUrlChanged();
}

QString SettingsManager::loginHomeserverPrefill() const
{
    // Account-independent prefill: the raw global value, never the active
    // account's server. This is what the login field must read so the
    // add-account flow can target a different homeserver.
    return m_store->value(kHomeserver, QStringLiteral("https://matrix.org"))
        .toString();
}

void SettingsManager::setLoginHomeserverPrefill(const QString &url)
{
    if (loginHomeserverPrefill() == url)
        return;
    m_store->setValue(kHomeserver, url);
    Q_EMIT loginHomeserverPrefillChanged();
    // The active-account view may also observe the global key when no
    // per-account server is stored yet, so keep that binding coherent.
    Q_EMIT homeserverUrlChanged();
}

QVariant SettingsManager::appearanceValue(const char *globalKey,
                                          const QVariant &fallback) const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty()) {
        const QString key = accountKey(slug, globalKey);
        if (m_store->contains(key))
            return m_store->value(key);
    }
    return m_store->value(QLatin1String(globalKey), fallback);
}

void SettingsManager::setAppearanceValue(const char *globalKey,
                                         const QVariant &value)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty())
        m_store->setValue(accountKey(slug, globalKey), value);
    m_store->setValue(QLatin1String(globalKey), value);
}

SettingsManager::Theme SettingsManager::theme() const
{
    const int stored = appearanceValue(kTheme, SystemTheme).toInt();
    // An unknown / out-of-range stored theme (e.g. written by a newer build,
    // or corrupted) falls back to the safe default rather than rendering an
    // undefined palette.
    if (stored < 0 || stored > kMaxThemeId)
        return SystemTheme;
    return static_cast<Theme>(stored);
}

void SettingsManager::setTheme(Theme t)
{
    const int value = static_cast<int>(t);
    if (value < 0 || value > kMaxThemeId)
        t = SystemTheme;
    if (theme() == t)
        return;
    setAppearanceValue(kTheme, static_cast<int>(t));
    Q_EMIT themeChanged();
}

QStringList SettingsManager::uiFontChoices()
{
    // The curated bundled UI families (all OFL, all shipped as variable
    // fonts in data/fonts). Manrope stays the default; JetBrains Mono,
    // Material Symbols, and emoji fallback are never selectable here.
    return { QStringLiteral("Manrope"), QStringLiteral("Inter"),
             QStringLiteral("IBM Plex Sans"), QStringLiteral("Source Sans 3"),
             QStringLiteral("Plus Jakarta Sans") };
}

QString SettingsManager::uiFont() const
{
    const QString stored =
        appearanceValue(kUiFont, QStringLiteral("Manrope")).toString();
    // An unknown stored family (newer build, corruption) falls back to the
    // default instead of asking the platform for an arbitrary font.
    return uiFontChoices().contains(stored) ? stored
                                            : QStringLiteral("Manrope");
}

void SettingsManager::setUiFont(const QString &family)
{
    const QString next = uiFontChoices().contains(family)
        ? family : QStringLiteral("Manrope");
    if (uiFont() == next)
        return;
    setAppearanceValue(kUiFont, next);
    Q_EMIT uiFontChanged();
}

int SettingsManager::messageLayout() const
{
    const int stored = appearanceValue(kMessageLayout, 0).toInt();
    return (stored < 0 || stored > kMaxMessageLayout) ? 0 : stored;
}

void SettingsManager::setMessageLayout(int layout)
{
    if (layout < 0 || layout > kMaxMessageLayout)
        layout = 0;
    if (messageLayout() == layout)
        return;
    setAppearanceValue(kMessageLayout, layout);
    Q_EMIT messageLayoutChanged();
}

int SettingsManager::textScale() const
{
    const int stored = appearanceValue(kTextScale, 100).toInt();
    return (stored < kMinTextScale || stored > kMaxTextScale) ? 100 : stored;
}

void SettingsManager::setTextScale(int percent)
{
    percent = std::clamp(percent, kMinTextScale, kMaxTextScale);
    if (textScale() == percent)
        return;
    setAppearanceValue(kTextScale, percent);
    Q_EMIT textScaleChanged();
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

bool SettingsManager::customAppIconEnabled() const
{
    return m_store->value(kCustomAppIcon, false).toBool();
}

void SettingsManager::setCustomAppIconEnabled(bool enabled)
{
    if (customAppIconEnabled() == enabled)
        return;
    m_store->setValue(kCustomAppIcon, enabled);
    Q_EMIT customAppIconEnabledChanged();
}

int SettingsManager::notificationPreview() const
{
    // 1 = sender only: the privacy-preserving default.
    const int mode =
        m_store->value(QStringLiteral("notifications/preview"), 1).toInt();
    return (mode < 0 || mode > 2) ? 1 : mode;
}

void SettingsManager::setNotificationPreview(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 1;
    if (notificationPreview() == mode)
        return;
    m_store->setValue(QStringLiteral("notifications/preview"), mode);
    Q_EMIT notificationPreviewChanged();
}

int SettingsManager::notificationSound() const
{
    // 1 = mentions and direct messages: the conservative default.
    const int mode =
        m_store->value(QStringLiteral("notifications/sound"), 1).toInt();
    return (mode < 0 || mode > 2) ? 1 : mode;
}

void SettingsManager::setNotificationSound(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 1;
    if (notificationSound() == mode)
        return;
    m_store->setValue(QStringLiteral("notifications/sound"), mode);
    Q_EMIT notificationSoundChanged();
}

// The per-room mode became account-derived state when the Rust backend's
// server push-rule sync landed, so it is stored per account
// (accounts/<slug>/notifications/room-mode/<roomId>) like the appearance
// values. Legacy (pre-scoping) modes live under the bare global key; reads
// fall back to it so an upgrading user keeps every mode until an account's
// first write shadows it. The legacy key is never deleted by an account
// write — it remains the shared fallback for the OTHER accounts.
QString SettingsManager::roomNotificationModeGlobalKey(const QString &roomId)
{
    return QStringLiteral("notifications/room-mode/") + roomId;
}

QString SettingsManager::activeAccountSlugCached() const
{
    const QString active = activeAccountUserId();
    if (active.isEmpty())
        return {};
    if (active != m_activeSlugCacheUserId) {
        m_activeSlugCacheUserId = active;
        m_activeSlugCache = slugForSavedAccount(active);
    }
    return m_activeSlugCache;
}

QString SettingsManager::roomNotificationModeScopedKey(const QString &roomId) const
{
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return {};
    return QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1Char('/') + roomNotificationModeGlobalKey(roomId);
}

int SettingsManager::roomNotificationMode(const QString &roomId) const
{
    if (roomId.isEmpty())
        return 0;
    const QString scopedKey = roomNotificationModeScopedKey(roomId);
    const QString readKey = (!scopedKey.isEmpty() && m_store->contains(scopedKey))
        ? scopedKey
        : roomNotificationModeGlobalKey(roomId);
    const int mode = m_store->value(readKey, 0).toInt();
    return (mode < 0 || mode > 3) ? 0 : mode;
}

namespace {
// Learned video dimensions: hashed key (a raw event id never becomes a
// settings key) under the active account, with a bounded LRU index so the
// store cannot grow with the timeline.
QString videoDimsHash(const QString &mediaKey)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(mediaKey.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}
constexpr int kVideoDimsCap = 512;
} // namespace

QSize SettingsManager::knownVideoDimensions(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return {};
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/')
        + slug + QLatin1String("/media/video-dims/")
        + videoDimsHash(mediaKey);
    const QSize size = m_store->value(key).toSize();
    return size.isValid() && size.width() > 0 && size.height() > 0 ? size
                                                                   : QSize{};
}

QString SettingsManager::mediaInfoIndexKeyForSlug(const QString &slug)
{
    return QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/media/video-dims-index");
}

// Shared LRU touch for the learned-media keys: dims and size share one
// index, so an evicted entry drops BOTH of its keys and the store stays
// bounded regardless of which fact was learned first.
void SettingsManager::touchMediaInfoIndex(const QString &slug,
                                          const QString &hash)
{
    const QString base = QLatin1String(kAccountsGroup) + QLatin1Char('/')
        + slug + QLatin1String("/media/");
    const QString indexKey = mediaInfoIndexKeyForSlug(slug);
    QStringList index = m_store->value(indexKey).toStringList();
    index.removeOne(hash);
    index.append(hash);
    while (index.size() > kVideoDimsCap) {
        const QString victim = index.takeFirst();
        m_store->remove(base + QLatin1String("video-dims/") + victim);
        m_store->remove(base + QLatin1String("payload-size/") + victim);
    }
    m_store->setValue(indexKey, index);
}

double SettingsManager::knownMediaSizeBytes(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return 0;
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return 0;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/')
        + slug + QLatin1String("/media/payload-size/")
        + videoDimsHash(mediaKey);
    const qint64 bytes = m_store->value(key, 0).toLongLong();
    return bytes > 0 ? static_cast<double>(bytes) : 0;
}

void SettingsManager::setKnownMediaSizeBytes(const QString &mediaKey,
                                             qint64 bytes)
{
    if (!mediaKey.startsWith(QLatin1Char('$')) || bytes <= 0)
        return;
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return;
    const QString hash = videoDimsHash(mediaKey);
    m_store->setValue(QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
                          + QLatin1String("/media/payload-size/") + hash,
                      bytes);
    touchMediaInfoIndex(slug, hash);
}

void SettingsManager::setKnownVideoDimensions(const QString &mediaKey,
                                              int width, int height)
{
    // Only remote events: a local-echo key is transient and its remote id
    // records the same payload again once reconciled.
    if (!mediaKey.startsWith(QLatin1Char('$')) || width <= 0 || height <= 0)
        return;
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return;
    const QString hash = videoDimsHash(mediaKey);
    m_store->setValue(QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
                          + QLatin1String("/media/video-dims/") + hash,
                      QSize(width, height));
    touchMediaInfoIndex(slug, hash);
}

namespace {
// v0.6.7: the only picker ids that may reach the settings store. A whitelist,
// not a sanitizer — a rejected id writes and reads nothing at all, so no QML
// caller can compose a settings key out of user-controlled text.
//
// "picker" is the shared id both overlay pickers pass, which is what makes
// resizing one resize the other; the per-picker ids remain accepted so a
// future surface can opt out of the shared value without touching this gate.
bool isKnownPickerId(const QString &id)
{
    return id == QLatin1String("picker") || id == QLatin1String("gif")
        || id == QLatin1String("emoji");
}

QString pickerShareKey(const QString &id, const char *dimension)
{
    return QStringLiteral("pickers/%1/%2Share").arg(id, QLatin1String(dimension));
}

// Per mille of the space available to the picker. A share outside this range
// is treated as absent: below the floor the picker would be unusable, and
// above 1000 it would exceed the room it has. Guards a hand-edited or
// corrupted store; the QML side clamps to the live window on top of this.
constexpr int kMinPickerShare = 50;
constexpr int kMaxPickerShare = 1000;

int readPickerShare(QSettings *store, const QString &id, const char *dimension)
{
    if (!isKnownPickerId(id))
        return 0;
    const int v = store->value(pickerShareKey(id, dimension), 0).toInt();
    return (v >= kMinPickerShare && v <= kMaxPickerShare) ? v : 0;
}
} // namespace

int SettingsManager::pickerWidthShare(const QString &id) const
{
    return readPickerShare(m_store.get(), id, "width");
}

int SettingsManager::pickerHeightShare(const QString &id) const
{
    return readPickerShare(m_store.get(), id, "height");
}

void SettingsManager::setPickerShare(const QString &id, int widthPerMille,
                                     int heightPerMille)
{
    if (!isKnownPickerId(id))
        return;
    // Out-of-range means "forget it" rather than "store something wrong":
    // removing the keys restores the component's own default share next time.
    const bool sane = widthPerMille >= kMinPickerShare
                      && widthPerMille <= kMaxPickerShare
                      && heightPerMille >= kMinPickerShare
                      && heightPerMille <= kMaxPickerShare;
    if (!sane) {
        m_store->remove(pickerShareKey(id, "width"));
        m_store->remove(pickerShareKey(id, "height"));
        return;
    }
    m_store->setValue(pickerShareKey(id, "width"), widthPerMille);
    m_store->setValue(pickerShareKey(id, "height"), heightPerMille);
}

void SettingsManager::setRoomNotificationMode(const QString &roomId, int mode)
{
    if (roomId.isEmpty())
        return;
    // 3 = follow the account default. It is stored EXPLICITLY and is not the
    // same as an absent key: absence reads back as 0 (all messages), so
    // "never configured" and "deliberately following the account default"
    // would otherwise be indistinguishable in the UI. The server-side truth
    // is the absence of a user-defined push rule; this is the device-local
    // record of that choice.
    if (mode < 0 || mode > 3)
        mode = 0;
    if (roomNotificationMode(roomId) == mode)
        return;
    const QString globalKey = roomNotificationModeGlobalKey(roomId);
    const QString scopedKey = roomNotificationModeScopedKey(roomId);
    if (scopedKey.isEmpty()) {
        // No active account (logged out / pre-login): the global key keeps
        // its original device-local semantics.
        if (mode == 0)
            m_store->remove(globalKey);  // default: keep the file compact
        else
            m_store->setValue(globalKey, mode);
    } else if (mode == 0 && !m_store->contains(globalKey)) {
        m_store->remove(scopedKey);      // default: keep the file compact
    } else {
        // Write-through per account. While a legacy global value exists
        // for this room, even mode 0 is stored EXPLICITLY: removing the
        // scoped key would resurrect the legacy mode on the next read, and
        // deleting the legacy key would steal the other accounts' fallback.
        m_store->setValue(scopedKey, mode);
    }
    Q_EMIT roomNotificationModeChanged(roomId);
}

bool SettingsManager::notificationsEnabled() const
{
    return m_store->value(kNotifications, true).toBool();
}

bool SettingsManager::autoLoadLinkPreviews() const
{
    // Privacy default (2026-08-12): OFF. A preview is fetched by this client,
    // directly from the linked site — not through the homeserver's preview
    // proxy — so automatic loading would hand the user's IP address and read
    // timing to any host a sender chooses to link. Previews stay a deliberate
    // action: either the user turns this on, or they use a message's
    // "Load link preview". An account that already stored a value keeps it.
    return m_store->value(kPreviewsUnencrypted, false).toBool();
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
    // Privacy default: never contact a site linked from an encrypted room
    // automatically. (The fetch is client-side, so the exposure is to the
    // linked site, not to the homeserver.)
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

int SettingsManager::gifAutoplay() const
{
    // Default follows the legacy animateGifPreviews boolean: Always when it was
    // on (the pre-0.6.1 default), Never when the user had turned it off.
    const int fallback = animateGifPreviews() ? 0 : 2;
    const int v = m_store->value(kGifAutoplay, fallback).toInt();
    return (v >= 0 && v <= 2) ? v : 0;
}

void SettingsManager::setGifAutoplay(int mode)
{
    const int clamped = (mode >= 0 && mode <= 2) ? mode : 0;
    if (m_store->value(kGifAutoplay).isValid() && gifAutoplay() == clamped)
        return;
    m_store->setValue(kGifAutoplay, clamped);
    Q_EMIT gifAutoplayChanged();
}

int SettingsManager::gifSafeSearch() const
{
    // Default PG-13 (id 2) — a general-client default.
    const int v = m_store->value(kGifSafeSearch, 2).toInt();
    return (v >= 0 && v <= 3) ? v : 2;
}

void SettingsManager::setGifSafeSearch(int rating)
{
    const int clamped = (rating >= 0 && rating <= 3) ? rating : 2;
    if (gifSafeSearch() == clamped)
        return;
    m_store->setValue(kGifSafeSearch, clamped);
    Q_EMIT gifSafeSearchChanged();
}

bool SettingsManager::storeRecentGifs() const
{
    return m_store->value(kGifStoreRecent, true).toBool();
}

void SettingsManager::setStoreRecentGifs(bool v)
{
    if (storeRecentGifs() == v)
        return;
    m_store->setValue(kGifStoreRecent, v);
    Q_EMIT storeRecentGifsChanged();
}

QString SettingsManager::gifPreferredProvider() const
{
    const QString v = m_store->value(kGifProvider, QStringLiteral("giphy"))
                          .toString();
    return (v == QLatin1String("giphy") || v == QLatin1String("klipy"))
        ? v : QStringLiteral("giphy");
}

void SettingsManager::setGifPreferredProvider(const QString &id)
{
    if (id != QLatin1String("giphy") && id != QLatin1String("klipy"))
        return;
    if (gifPreferredProvider() == id)
        return;
    m_store->setValue(kGifProvider, id);
    Q_EMIT gifPreferredProviderChanged();
}

bool SettingsManager::showRoomActivity() const
{
    return m_store->value(kShowRoomActivity, true).toBool();
}

void SettingsManager::setShowRoomActivity(bool v)
{
    if (showRoomActivity() == v)
        return;
    m_store->setValue(kShowRoomActivity, v);
    Q_EMIT showRoomActivityChanged();
}

int SettingsManager::timelineWheelSpeed() const
{
    const int stored = m_store->value(kTimelineWheelSpeed,
                                      kDefaultTimelineWheelSpeed).toInt();
    // An unknown / legacy / corrupted value falls back to Fast rather than an
    // undefined speed.
    if (stored < 0 || stored > 2)
        return kDefaultTimelineWheelSpeed;
    return stored;
}

void SettingsManager::setTimelineWheelSpeed(int v)
{
    if (v < 0 || v > 2)
        v = kDefaultTimelineWheelSpeed;
    if (timelineWheelSpeed() == v)
        return;
    m_store->setValue(kTimelineWheelSpeed, v);
    Q_EMIT timelineWheelSpeedChanged();
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
    return activeAccountUserId();
}

QString SettingsManager::deviceId() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    return m_store->value(accountKey(slug, kAccountDeviceId)).toString();
}

QString SettingsManager::syncToken() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    return m_store->value(accountKey(slug, kAccountSyncToken)).toString();
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
    const bool hsChanged = homeserverUrl() != homeserverUrl_;

    // Canonicalize the identity so records, secrets, and store paths agree
    // regardless of input casing (matches the Rust store-path resolution).
    QString hsCanonical = homeserverUrl_;
    QString uidCanonical = userId_.trimmed();
    matrix::app_data::AccountIdentity identity;
    if (matrix::app_data::resolveAccountIdentity(homeserverUrl_, userId_,
                                                 &identity)) {
        hsCanonical = identity.homeserver;
        uidCanonical = identity.userId;
    }

    if (!upsertAccountRecord(uidCanonical, hsCanonical, deviceId_)) {
        qCWarning(lcSettings) << "saveSession rejected: unsafe user id";
        return;
    }
    // A fresh login is a new device — any previous sync position for this
    // account belongs to the old session.
    const QString slug = matrix::app_data::safeUserSlug(uidCanonical);
    m_store->remove(accountKey(slug, kAccountSyncToken));
    m_store->setValue(kActiveAccount, uidCanonical);
    // Keep the login prefill on the most recently used homeserver.
    m_store->setValue(kHomeserver, hsCanonical);

    if (m_secretStore) {
        if (!m_secretStore->storeSecret(uidCanonical, QLatin1String(kSecretAccessToken), accessToken_)) {
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

    // Multi-account: other signed-in accounts keep their records, sync
    // positions, and SecretStore tokens. (Pre-0.7 builds cleared the
    // previous user here, which made every login destroy the last session.)

    // Flush now. The SDK store directory is created eagerly on disk before
    // the server is even contacted, while QSettings otherwise only writes on
    // destruction — so a crash here used to leave a store with no record,
    // which the next login treats as an orphan and deletes. The store-path
    // reconciliation that runs right after this also has to see a durable
    // record.
    m_store->sync();

    if (hsChanged)
        Q_EMIT homeserverUrlChanged();
    Q_EMIT sessionChanged();
}

void SettingsManager::setSyncToken(const QString &token)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    if (syncToken() == token)
        return;
    m_store->setValue(accountKey(slug, kAccountSyncToken), token);
}

bool SettingsManager::clearSession()
{
    const QString uid = userId();
    if (uid.isEmpty()) {
        // Residual pre-0.7 metadata without an account record.
        m_store->remove(kUserId);
        m_store->remove(kDeviceId);
        m_store->remove(kSyncToken);
        m_store->remove(kAccessTokenLegacy);
        return true;
    }
    return clearSessionForAccount(uid);
}

bool SettingsManager::clearSessionForAccount(const QString &uid)
{
    return clearSessionForAccount(uid, nullptr);
}

bool SettingsManager::clearSessionForAccount(const QString &uid,
                                             bool *matchedRecord)
{
    if (matchedRecord)
        *matchedRecord = false;
    const QString target = uid.trimmed();
    if (target.isEmpty())
        return false;

    // Normalize: accept the exact saved id, or resolve a localpart/mixed
    // form against the saved records.
    QString slug = slugForSavedAccount(target);
    QString recordUserId = target;
    if (!slug.isEmpty()) {
        recordUserId =
            m_store->value(accountKey(slug, kAccountUserId)).toString();
    } else {
        matrix::app_data::AccountIdentity identity;
        if (matrix::app_data::resolveAccountIdentity(homeserverUrl(), target,
                                                     &identity)) {
            slug = slugForSavedAccount(identity.userId);
            // resolveAccountIdentity preserves the localpart case, so this
            // still misses an account saved under the server's canonical
            // casing. Fall back to the case-insensitive lookup — otherwise a
            // reset typed as "Mizerd" silently matches nothing while the real
            // "@mizerd:…" record, its token and the active pointer survive.
            if (slug.isEmpty()) {
                const QString canonical =
                    canonicalUserIdForTypedIdentity(identity.userId);
                if (!canonical.isEmpty())
                    slug = slugForSavedAccount(canonical);
            }
            if (!slug.isEmpty()) {
                recordUserId =
                    m_store->value(accountKey(slug, kAccountUserId)).toString();
            }
        }
    }
    if (matchedRecord)
        *matchedRecord = !slug.isEmpty();

    const bool activeAccount =
        !activeAccountUserId().isEmpty() && activeAccountUserId() == recordUserId;

    if (!slug.isEmpty()) {
        m_store->beginGroup(QLatin1String(kAccountsGroup));
        m_store->remove(slug);
        m_store->endGroup();
        Q_EMIT accountsChanged();
    }
    if (activeAccount) {
        m_store->remove(kActiveAccount);
        m_store->remove(kAccessTokenLegacy);
    }

    bool secretsCleared = true;
    if (m_secretStore) {
        // Use the exact key originally persisted so a legacy mixed-case
        // homeserver cannot orphan its SecretStore entry.
        secretsCleared = m_secretStore->clearAccountSecrets(recordUserId);
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
