#include "app/SettingsManager.h"

#include <QFile>
#include <QFileInfo>

#include "storage/SecretStore.h"
#include "storage/AppDataPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QLoggingCategory>
#include <QSet>

#include <algorithm>

Q_LOGGING_CATEGORY(lcSettings, "matrix.settings")

namespace {
constexpr auto kHomeserver          = "homeserver/url";
constexpr auto kTheme               = "ui/theme";
constexpr auto kMessageLayout       = "ui/messageLayout";
constexpr auto kRoomNavLayout       = "ui/roomNavigationLayout";
constexpr auto kRoomFilterMode      = "ui/roomFilterMode";
constexpr auto kTextScale           = "ui/textScale";
constexpr auto kUiFont              = "ui/uiFont";
constexpr auto kMonoFont            = "ui/monoFont";
// Device-global: imported fonts are registered process-wide before any
// account is restored.
constexpr auto kImportedFonts       = "ui/importedFonts";
constexpr auto kLanguage            = "ui/language";
constexpr auto kStartMinimized      = "ui/startMinimized";
constexpr auto kCustomAppIcon       = "ui/customAppIconEnabled";
constexpr auto kNotifications       = "notifications/enabled";
constexpr auto kRecentEmoji         = "emoji/recent";
constexpr auto kPreferredEmojiTone  = "emoji/preferredTone";
// The encrypted-room key must default to false: fetching a preview reveals
// the URL to a third party.
constexpr auto kPreviewsUnencrypted = "previews/autoLoadUnencrypted";
constexpr auto kPreviewsEncrypted   = "previews/loadInEncryptedRooms";
constexpr auto kPreviewsAnimateGifs = "previews/animateGifs";
// Screen-share quality. Device-global. Clamped on read and write because
// the value reaches a GStreamer caps string.
constexpr auto kShareMaxHeight      = "calls/shareMaxHeight";
constexpr auto kShareFps            = "calls/shareFps";
constexpr auto kSharePresence = "presence/shareOwn";
constexpr auto kSpacesRailVisible = "shell/spacesRailVisible";
constexpr auto kSpaceBannersVisible = "shell/spaceBannersVisible";
constexpr auto kSpaceBannerExpanded = "shell/spaceBannerExpanded";
constexpr auto kRoomListVisible   = "shell/roomListVisible";
constexpr auto kRoomListWidth     = "shell/roomListWidth";
constexpr auto kSpacesRailWidth   = "shell/spacesRailWidth";
constexpr auto kSpacesRailDepthStyle = "shell/spacesRailDepthStyle";
constexpr auto kSidePanelWidth    = "shell/sidePanelWidth";
constexpr auto kCloseToTray       = "shell/closeToTray";
constexpr auto kWindowGeometry    = "shell/windowGeometry";
constexpr auto kWindowMaximized   = "shell/windowMaximized";
constexpr auto kStartInTray       = "shell/startInTray";
// Account-scoped only; no global fallback key.
constexpr auto kVerifyWarningDismissed = "security/verifyWarningDismissed";
constexpr auto kMediaVolume         = "media/volume";       // 0..1
constexpr auto kMediaPlaybackRate   = "media/playbackRate";  // 0.25..4.0
// GIF browser policy.
constexpr auto kGifAutoplay         = "gif/autoplay";       // 0/1/2
constexpr auto kGifSafeSearch       = "gif/safeSearch";     // gif::Rating id
constexpr auto kGifStoreRecent      = "gif/storeRecent";    // bool
constexpr auto kGifProvider         = "gif/provider";       // "giphy"/"klipy"
// Presentation-only timeline preferences; the model keeps every event, so
// changing them never requires a resync.
constexpr auto kShowRoomActivity    = "timeline/showRoomActivity";
constexpr auto kShowMembership      = "timeline/showMembershipEvents";
constexpr auto kShowProfileChanges  = "timeline/showProfileChangeEvents";
constexpr auto kCollapseEmbeds      = "timeline/collapseEmbeds";
constexpr auto kReducedMotion       = "ui/reducedMotion";
constexpr auto kSmoothScrolling     = "ui/smoothScrolling";
constexpr auto kHiddenComposerButtons = "ui/hiddenComposerButtons";
constexpr auto kClockFormat         = "ui/clockFormat";
constexpr auto kEnterNewline        = "composer/enterInsertsNewline";
constexpr auto kComposerMode        = "composer/mode";
constexpr auto kSpellCheckEnabled   = "composer/spellCheck";
constexpr auto kSpellCheckLanguage  = "composer/spellLanguage";
constexpr auto kTextAsCaption       = "composer/textAsCaption";
// Shortcut overrides live in their own group, one key per action id.
constexpr auto kShortcutsGroup      = "shortcuts";
// 0 = Standard, 1 = Fast, 2 = Very fast (see TimelineScrollController).
constexpr auto kTimelineWheelSpeed  = "timeline/wheelSpeed";
// Device-global: main.cpp reads this key before QGuiApplication exists and
// turns it into QT_SCALE_FACTOR. Keep the key name and the 75..150 clamp in
// sync with that read.
constexpr auto kInterfaceZoom       = "ui/interfaceZoom";
constexpr int kRecentEmojiLimit     = 32;
// Legacy plaintext access token location; kept only so the migration can
// find and delete it.
constexpr auto kAccessTokenLegacy   = "session/accessToken";
// Pre-0.7 single-session metadata, kept only for migration.
constexpr auto kUserId              = "session/userId";
constexpr auto kDeviceId            = "session/deviceId";
constexpr auto kSyncToken           = "session/syncToken";

// Multi-account registry.
constexpr auto kAccountsGroup       = "accounts";
constexpr auto kActiveAccount       = "accounts/active";
constexpr auto kAccountUserId       = "userId";
constexpr auto kAccountHomeserver   = "homeserver";
constexpr auto kAccountDeviceId     = "deviceId";
constexpr auto kAccountDisplayName  = "displayName";
constexpr auto kAccountAvatarUrl    = "avatarUrl";
constexpr auto kAccountAddedAt      = "addedAt";
// Absent means "password", so accounts saved before OAuth need no migration.
constexpr auto kAccountAuthType     = "authType";
constexpr auto kAccountSyncToken    = "syncToken";
// The on-disk SDK store directory name recorded at login. See storeSlugFor.
constexpr auto kAccountStoreSlug    = "storeSlug";

// SecretStore keys.
constexpr auto kSecretAccessToken   = "accessToken";
// OAuth credentials, stored in the SecretStore beside the access token and
// never exposed to QML.
constexpr auto kSecretRefreshToken  = "refreshToken";
constexpr auto kSecretOAuthClientId = "oauthClientId";

// Mirrors InsecureFallbackSecretStore::settingsKey()'s group-name folding.
// Duplicated because including that Q_OBJECT header would pull its vtable
// into targets that do not link its .cpp.
// insecureSecretsGroupFoldingIsStillTwoCharacters (SettingsSessionTest)
// fails if the folding there changes.
QString insecureSecretsGroupName(const QString &userId)
{
    QString safeUser = userId;
    safeUser.replace(QLatin1Char('/'), QLatin1Char('_'));
    safeUser.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return safeUser;
}
}

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
    , m_store(std::make_unique<QSettings>())
{
    if (!m_store->contains(kHomeserver)) {
        m_store->setValue(kHomeserver, QStringLiteral("https://matrix.org"));
    }
    // Restrict the settings file to the owner: it holds account ids, drafts
    // and, under the insecure fallback, tokens. QSaveFile keeps the mode on
    // later writes, so doing it once is enough.
    m_store->sync();
    if (QFileInfo::exists(m_store->fileName())) {
        QFile::setPermissions(m_store->fileName(),
                              QFile::ReadOwner | QFile::WriteOwner);
    }
    migrateLegacySessionRecord();
    loadWindowGeometry();
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
    // The slug mapping is not injective, so a record matches only when its
    // stored user id is identical.
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

    // An exact match wins, so a legitimate uppercase-localpart account is never
    // shadowed by a case-insensitive sibling.
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
            // Two saved accounts differ only by case; refuse rather than guess.
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
    // The store directory already exists; flush so the mapping survives a
    // crash.
    m_store->sync();
}

bool SettingsManager::secretBackendUnavailable() const
{
    if (!m_secretStore)
        return true;
    // isAvailable() is a construction-time probe and cannot see a keyring that
    // locks later; lastReadFailed() reports the outcome of the actual read.
    return !m_secretStore->isAvailable() || m_secretStore->lastReadFailed();
}

bool SettingsManager::secretMissesAreInconclusive() const
{
    return !m_secretStore || m_secretStore->missesAreInconclusive();
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
        // Legacy delegated-homeserver slug, built from the URL host.
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
    // A recorded store location wins over the derived one; an unusable
    // recording is ignored (bindStoreSlug refuses rather than half-applies).
    const QString recorded =
        m_store->value(accountKey(slug, kAccountStoreSlug)).toString();
    if (!recorded.isEmpty() && recorded != identity.slug)
        matrix::app_data::bindStoreSlug(&identity, recorded);
    *out = identity;
    return true;
}

void SettingsManager::migrateLegacySessionRecord()
{
    // Convert the pre-0.7 single session into the first account record, then
    // drop the legacy keys.
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
    // Never clobber a different account whose id flattens to the same slug.
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
    // Log when an unknown id clears the active account instead of setting it.
    if (!uid.isEmpty() && next.isEmpty()) {
        qCWarning(lcSettings)
            << "asked to activate an account with no saved record; the active "
               "account is being CLEARED instead"
            << "slug=" << matrix::app_data::safeUserSlug(uid);
    }
    if (activeAccountUserId() == next)
        return;
    // Decides which account the next launch opens. Slug only in logs.
    qCInfo(lcSettings) << "active account moves"
                       << "from="
                       << matrix::app_data::safeUserSlug(activeAccountUserId())
                       << "to=" << matrix::app_data::safeUserSlug(next);
    if (next.isEmpty())
        m_store->remove(kActiveAccount);
    else
        m_store->setValue(kActiveAccount, next);
    // Flush: QSettings writes lazily, and the synchronous account switch that
    // follows can take long enough for a crash or kill to lose it, reopening
    // the previous account on next launch.
    m_store->sync();
    m_activeSlugCacheUserId.clear();
    m_activeSlugCache.clear();
    Q_EMIT sessionChanged();
    Q_EMIT homeserverUrlChanged();
    // Per-account values: the switched-to account may resolve differently, so
    // every getter that reads appearanceValue() must be announced here.
    // everyAccountScopedGetterHasItsSignalInTheAccountSwitch derives that list
    // from this file.
    Q_EMIT themeChanged();
    Q_EMIT messageLayoutChanged();
    Q_EMIT roomNavigationLayoutChanged();
    Q_EMIT roomFilterModeChanged();
    Q_EMIT textScaleChanged();
    Q_EMIT uiFontChanged();
    Q_EMIT monoFontChanged();
    Q_EMIT reducedMotionChanged();
    Q_EMIT smoothScrollingChanged();
    Q_EMIT collapseEmbedsChanged();
    Q_EMIT hiddenComposerButtonsChanged();
    Q_EMIT clockFormatChanged();
    Q_EMIT microphoneGainChanged();
    Q_EMIT verificationWarningDismissedChanged();
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
    // Deterministic fictional device id; metadata only.
    m_store->setValue(accountKey(slug, kAccountDeviceId),
                      QStringLiteral("DEMODEVICE%1").arg(order));
    m_store->setValue(accountKey(slug, kAccountDisplayName), displayName);
    m_store->setValue(accountKey(slug, kAccountAvatarUrl), avatarUrl);
    // Deterministic addedAt so the switcher order is stable across launches.
    m_store->setValue(accountKey(slug, kAccountAddedAt),
                      QStringLiteral("2026-07-23T09:%1:00")
                          .arg(order, 2, 10, QLatin1Char('0')));
    // No SecretStore write: demo accounts carry no token.
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

    // Legacy single-session token.
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

    // Multi-account plaintext tokens left by an earlier insecure-fallback run.
    migrateInsecureSecretsGroup();
}

void SettingsManager::migrateInsecureSecretsGroup()
{
    // Only migrate into a secure backend.
    if (!m_secretStore || !m_secretStore->isSecure())
        return;

    m_store->beginGroup(QStringLiteral("secrets"));
    const QStringList accounts = m_store->childGroups();
    m_store->endGroup();
    if (accounts.isEmpty())
        return;

    // The group name is not the user id: the fallback store folds '/' and '\\'
    // to '_', and localparts may contain '/'. Resolve each group back to
    // exactly one saved account, and leave the plaintext in place when there is
    // none or more than one; the folding is not injective.
    QHash<QString, QString> byGroupName;
    QSet<QString> ambiguousGroups;
    const QStringList saved = savedAccountUserIds();
    for (const QString &uid : saved) {
        const QString group = insecureSecretsGroupName(uid);
        const auto existing = byGroupName.constFind(group);
        if (existing != byGroupName.constEnd()) {
            if (*existing != uid)
                ambiguousGroups.insert(group);
            continue;
        }
        byGroupName.insert(group, uid);
    }

    // Every secret an account can own; migrating only the access token would
    // strand an OAuth account without its refresh token and client id.
    const QLatin1String secretKeys[] = {
        QLatin1String(kSecretAccessToken),
        QLatin1String(kSecretRefreshToken),
        QLatin1String(kSecretOAuthClientId),
    };

    int migrated = 0;
    int failed = 0;
    int unresolved = 0;
    for (const QString &safeUser : accounts) {
        const QString groupKey = QStringLiteral("secrets/%1").arg(safeUser);
        if (ambiguousGroups.contains(safeUser)) {
            ++unresolved;
            continue;
        }
        const QString uid = byGroupName.value(safeUser);
        if (uid.isEmpty()) {
            // No saved record owns this group; leave it for a later start.
            ++unresolved;
            continue;
        }

        m_store->beginGroup(groupKey);
        const QStringList presentKeys = m_store->childKeys();
        m_store->endGroup();
        if (presentKeys.isEmpty())
            continue;

        bool allMoved = true;
        int movedHere = 0;
        for (const QLatin1String &key : secretKeys) {
            const QString plainKey = groupKey + QLatin1Char('/') + key;
            if (!m_store->contains(plainKey))
                continue;
            const QString value = m_store->value(plainKey).toString();
            if (value.isEmpty())
                continue;   // nothing to move; removing it loses nothing
            // Verify the read-back under the real id before deleting the
            // plaintext.
            if (m_secretStore->storeSecret(uid, key, value)
                && m_secretStore->readSecret(uid, key) == value) {
                ++movedHere;
            } else {
                allMoved = false;
            }
        }

        // Unknown keys (e.g. from a newer build) are not ours to delete.
        for (const QString &present : presentKeys) {
            const bool known =
                std::any_of(std::begin(secretKeys), std::end(secretKeys),
                            [&present](const QLatin1String &key) {
                                return present == key;
                            });
            if (!known)
                allMoved = false;
        }

        if (allMoved) {
            m_store->remove(groupKey);
            if (movedHere > 0)
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
    if (unresolved > 0)
        qCWarning(lcSettings)
            << "Secure credential migration: left" << unresolved
            << "plaintext group(s) in place — no single saved account "
               "resolves to them";
}

QString SettingsManager::homeserverUrl() const
{
    // The active account's homeserver, else the login prefill.
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
    // Always the global value, never the active account's server.
    return m_store->value(kHomeserver, QStringLiteral("https://matrix.org"))
        .toString();
}

void SettingsManager::setLoginHomeserverPrefill(const QString &url)
{
    if (loginHomeserverPrefill() == url)
        return;
    m_store->setValue(kHomeserver, url);
    Q_EMIT loginHomeserverPrefillChanged();
    // homeserverUrl() falls back to the same global key.
    Q_EMIT homeserverUrlChanged();
}

namespace {
/// A QSettings-safe key for one Matrix user id. Hashed rather than escaped
/// because QSettings treats '/' as a group separator; a 16-hex collision
/// would at worst share one volume.
QString volumeKeyFor(const QString &userId)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(userId.trimmed().toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}
constexpr int kVolumeDefault = 100;
// User scale. SfuMediaEngine::audioFactorPercent() maps 100-200 onto
// 100-1000% gain; 0-100 is 1:1 attenuation.
constexpr int kVolumeMax = 200;
} // namespace

QStringList SettingsManager::hiddenMediaKeys() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/hiddenMedia");
    return m_store->value(key).toStringList();
}

void SettingsManager::setHiddenMediaKeys(const QStringList &keys)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/hiddenMedia");
    // An empty list removes the key.
    if (keys.isEmpty())
        m_store->remove(key);
    else
        m_store->setValue(key, keys);
}

QVariantMap SettingsManager::ownPresenceStatus() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/presenceStatus");
    return m_store->value(key).toMap();
}

void SettingsManager::setOwnPresenceStatus(const QVariantMap &status)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/presenceStatus");
    if (status.isEmpty())
        m_store->remove(key);
    else
        m_store->setValue(key, status);
}

QVariantList SettingsManager::scheduledSends() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/scheduledSends");
    return m_store->value(key).toList();
}

void SettingsManager::setScheduledSends(const QVariantList &rows)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/scheduledSends");
    if (rows.isEmpty())
        m_store->remove(key);
    else
        m_store->setValue(key, rows);
}

QVariantMap SettingsManager::activityState() const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/activity");
    return m_store->value(key).toMap();
}

void SettingsManager::setActivityState(const QVariantMap &state)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/activity");
    if (state.isEmpty())
        m_store->remove(key);
    else
        m_store->setValue(key, state);
}

int SettingsManager::callParticipantVolume(const QString &userId) const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty() || userId.trimmed().isEmpty())
        return kVolumeDefault;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/callVolumes/") + volumeKeyFor(userId);
    if (!m_store->contains(key))
        return kVolumeDefault;
    // Clamped on read: the store is hand-editable.
    return qBound(0, m_store->value(key, kVolumeDefault).toInt(), kVolumeMax);
}

void SettingsManager::setCallParticipantVolume(const QString &userId,
                                               int percent)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty() || userId.trimmed().isEmpty())
        return;
    const int clamped = qBound(0, percent, kVolumeMax);
    if (callParticipantVolume(userId) == clamped)
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/callVolumes/") + volumeKeyFor(userId);
    if (clamped == kVolumeDefault) {
        // The default is not stored, so reset really forgets.
        m_store->remove(key);
    } else {
        m_store->setValue(key, clamped);
    }
    // Flush: calls are when the client is least likely to exit cleanly.
    m_store->sync();
    Q_EMIT callParticipantVolumeChanged(userId, clamped);
}

int SettingsManager::callShareVolume(const QString &userId) const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty() || userId.trimmed().isEmpty())
        return kVolumeDefault;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/shareVolumes/") + volumeKeyFor(userId);
    if (!m_store->contains(key))
        return kVolumeDefault;
    return qBound(0, m_store->value(key, kVolumeDefault).toInt(), kVolumeMax);
}

void SettingsManager::setCallShareVolume(const QString &userId, int percent)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty() || userId.trimmed().isEmpty())
        return;
    const int clamped = qBound(0, percent, kVolumeMax);
    if (callShareVolume(userId) == clamped)
        return;
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/shareVolumes/") + volumeKeyFor(userId);
    if (clamped == kVolumeDefault)
        m_store->remove(key);
    else
        m_store->setValue(key, clamped);
    // Flush: calls are when the client is least likely to exit cleanly.
    m_store->sync();
    Q_EMIT callShareVolumeChanged(userId, clamped);
}

int SettingsManager::microphoneGain() const
{
    return qBound(0, appearanceValue("call/microphoneGain",
                                     kVolumeDefault).toInt(), kVolumeMax);
}

void SettingsManager::setMicrophoneGain(int percent)
{
    const int clamped = qBound(0, percent, kVolumeMax);
    if (microphoneGain() == clamped)
        return;
    setAppearanceValue("call/microphoneGain", clamped);
    Q_EMIT microphoneGainChanged();
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

QVariant SettingsManager::accountScopedValue(const char *globalKey,
                                             const QVariant &fallback) const
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty()) {
        const QString key = accountKey(slug, globalKey);
        if (m_store->contains(key))
            return m_store->value(key);
    }
    // Migration source only; nothing signed in writes here.
    return m_store->value(QLatin1String(globalKey), fallback);
}

void SettingsManager::setAccountScopedValue(const char *globalKey,
                                            const QVariant &value)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty()) {
        // No account to scope to: the signed-out shell writes the global key.
        m_store->setValue(QLatin1String(globalKey), value);
        return;
    }
    m_store->setValue(accountKey(slug, globalKey), value);
}

void SettingsManager::forgetDeviceGlobalAccountResidue()
{
    // Room ids and user labels; the bare keys are migration sources only.
    m_store->remove(QLatin1String(kRailLayoutKey));
    m_store->remove(QLatin1String(kChannelCollapsedKey));
    // Raw room ids; with no accounts left there is nothing to fall back for.
    m_store->remove(QStringLiteral("notifications/room-mode"));
    m_store->sync();
}

SettingsManager::Theme SettingsManager::theme() const
{
    const int stored = appearanceValue(kTheme, SystemTheme).toInt();
    // Unknown values (newer build, corruption) fall back to the default.
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
    // Curated bundled UI families (OFL), shown first in the picker. Excludes
    // the mono face, the icon font and the emoji fallback.
    return { QStringLiteral("Manrope"), QStringLiteral("Inter"),
             QStringLiteral("IBM Plex Sans"), QStringLiteral("Source Sans 3"),
             QStringLiteral("Plus Jakarta Sans") };
}

QString SettingsManager::acceptableFontFamily(const QString &family)
{
    const QString trimmed = family.trimmed();
    // Far above any real family name.
    if (trimmed.isEmpty() || trimmed.size() > 96)
        return {};
    for (const QChar c : trimmed) {
        if (c.category() == QChar::Other_Control
            || c.category() == QChar::Other_Surrogate
            || c.isNonCharacter())
            return {};
        // The name ends up in QML `font.family` and generated markup; refuse
        // characters no real font name contains.
        static const QString banned = QStringLiteral("<>\"'&;{}\\/");
        if (banned.contains(c))
            return {};
    }
    return trimmed;
}

QString SettingsManager::uiFont() const
{
    // Verbatim when syntactically sound; FontManager resolves it against the
    // host.
    const QString stored =
        appearanceValue(kUiFont, QStringLiteral("Manrope")).toString();
    const QString accepted = acceptableFontFamily(stored);
    return accepted.isEmpty() ? QStringLiteral("Manrope") : accepted;
}

void SettingsManager::setUiFont(const QString &family)
{
    const QString accepted = acceptableFontFamily(family);
    const QString next =
        accepted.isEmpty() ? QStringLiteral("Manrope") : accepted;
    if (uiFont() == next)
        return;
    setAppearanceValue(kUiFont, next);
    Q_EMIT uiFontChanged();
}

QString SettingsManager::monoFont() const
{
    const QString stored =
        appearanceValue(kMonoFont, QStringLiteral("JetBrains Mono")).toString();
    const QString accepted = acceptableFontFamily(stored);
    return accepted.isEmpty() ? QStringLiteral("JetBrains Mono") : accepted;
}

void SettingsManager::setMonoFont(const QString &family)
{
    const QString accepted = acceptableFontFamily(family);
    const QString next =
        accepted.isEmpty() ? QStringLiteral("JetBrains Mono") : accepted;
    if (monoFont() == next)
        return;
    setAppearanceValue(kMonoFont, next);
    Q_EMIT monoFontChanged();
}

QStringList SettingsManager::importedFontFiles() const
{
    // Bounded and de-duplicated; FontManager validates the names.
    QStringList out;
    const QStringList stored = m_store->value(QLatin1String(kImportedFonts))
                                   .toStringList();
    for (const QString &name : stored) {
        if (name.isEmpty() || name.size() > 128 || out.contains(name))
            continue;
        out.append(name);
        if (out.size() >= 64)
            break;
    }
    return out;
}

void SettingsManager::setImportedFontFiles(const QStringList &fileNames)
{
    QStringList next;
    for (const QString &name : fileNames) {
        if (name.isEmpty() || name.size() > 128 || next.contains(name))
            continue;
        next.append(name);
        if (next.size() >= 64)
            break;
    }
    if (next == importedFontFiles())
        return;
    m_store->setValue(QLatin1String(kImportedFonts), next);
    Q_EMIT importedFontFilesChanged();
}

int SettingsManager::messageLayout() const
{
    const int stored = appearanceValue(kMessageLayout, 0).toInt();
    return (stored < 0 || stored > kMaxMessageLayout) ? 0 : stored;
}

int SettingsManager::roomNavigationLayout() const
{
    const int stored = appearanceValue(kRoomNavLayout, 0).toInt();
    // Out-of-range values fall back to Classic, which works for every account.
    return (stored < 0 || stored > kMaxRoomNavigationLayout) ? 0 : stored;
}

void SettingsManager::setRoomNavigationLayout(int layout)
{
    if (layout < 0 || layout > kMaxRoomNavigationLayout)
        layout = 0;
    if (roomNavigationLayout() == layout)
        return;
    setAppearanceValue(kRoomNavLayout, layout);
    Q_EMIT roomNavigationLayoutChanged();
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

int SettingsManager::roomFilterMode() const
{
    // 0 All, 1 People, 2 Rooms, 3 Unreads (RoomListModel::filterMode).
    const int stored = appearanceValue(kRoomFilterMode, 0).toInt();
    return (stored < 0 || stored > 3) ? 0 : stored;
}

void SettingsManager::setRoomFilterMode(int mode)
{
    if (mode < 0 || mode > 3)
        mode = 0;
    if (roomFilterMode() == mode)
        return;
    setAppearanceValue(kRoomFilterMode, mode);
    Q_EMIT roomFilterModeChanged();
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
    // "system" means resolve against the desktop at each start; an explicit
    // code is stored verbatim. LocalizationManager validates it.
    return m_store->value(kLanguage, QStringLiteral("system")).toString();
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
    // Default 0 (sender and message). Encrypted bodies only appear once the SDK
    // has decrypted them locally, and stricter modes remain available.
    const int mode =
        m_store->value(QStringLiteral("notifications/preview"), 0).toInt();
    return (mode < 0 || mode > 2) ? 0 : mode;
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

int SettingsManager::notificationPreviewEncrypted() const
{
    // 3 = follow the general setting, so an upgrade changes nothing.
    const int mode = m_store->value(
        QStringLiteral("notifications/previewEncrypted"), 3).toInt();
    return (mode < 0 || mode > 3) ? 3 : mode;
}

void SettingsManager::setNotificationPreviewEncrypted(int mode)
{
    if (mode < 0 || mode > 3)
        mode = 3;
    if (notificationPreviewEncrypted() == mode)
        return;
    m_store->setValue(QStringLiteral("notifications/previewEncrypted"), mode);
    Q_EMIT notificationPreviewEncryptedChanged();
}

int SettingsManager::effectiveNotificationPreview(bool encrypted,
                                                 bool encryptionKnown) const
{
    const int general = notificationPreview();
    const int forEncrypted = notificationPreviewEncrypted();
    if (forEncrypted == 3)
        return general;
    if (encrypted && encryptionKnown)
        return forEncrypted;
    if (!encryptionKnown) {
        // Unknown encryption: a higher mode discloses less, so take the larger.
        return std::max(general, forEncrypted);
    }
    return general;
}

bool SettingsManager::callPictureInPicture() const
{
    // Off by default: an automatic pop-out on minimise was unwanted. The call
    // bar still offers it manually.
    return m_store->value(QStringLiteral("calls/pictureInPicture"), false)
        .toBool();
}

void SettingsManager::setCallPictureInPicture(bool v)
{
    if (callPictureInPicture() == v)
        return;
    m_store->setValue(QStringLiteral("calls/pictureInPicture"), v);
    Q_EMIT callPictureInPictureChanged();
}

bool SettingsManager::strictDeviceTrust() const
{
    return m_store->value(QStringLiteral("privacy/strictDeviceTrust"), false)
        .toBool();
}

void SettingsManager::setStrictDeviceTrust(bool v)
{
    if (strictDeviceTrust() == v)
        return;
    m_store->setValue(QStringLiteral("privacy/strictDeviceTrust"), v);
    Q_EMIT strictDeviceTrustChanged();
}

int SettingsManager::readReceiptMode() const
{
    // 0 = public.
    const int mode =
        m_store->value(QStringLiteral("privacy/readReceiptMode"), 0).toInt();
    return (mode < 0 || mode > 2) ? 0 : mode;
}

void SettingsManager::setReadReceiptMode(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 0;
    if (readReceiptMode() == mode)
        return;
    m_store->setValue(QStringLiteral("privacy/readReceiptMode"), mode);
    Q_EMIT readReceiptModeChanged();
}

bool SettingsManager::sendTypingNotifications() const
{
    return m_store->value(QStringLiteral("privacy/sendTyping"), true).toBool();
}

void SettingsManager::setSendTypingNotifications(bool v)
{
    if (sendTypingNotifications() == v)
        return;
    m_store->setValue(QStringLiteral("privacy/sendTyping"), v);
    Q_EMIT sendTypingNotificationsChanged();
}

int SettingsManager::notificationSound() const
{
    // 1 = mentions and direct messages.
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

bool SettingsManager::ringForCalls() const
{
    return m_store->value(QStringLiteral("notifications/ringForCalls"), true)
        .toBool();
}

void SettingsManager::setRingForCalls(bool enabled)
{
    if (ringForCalls() == enabled)
        return;
    m_store->setValue(QStringLiteral("notifications/ringForCalls"), enabled);
    Q_EMIT ringForCallsChanged();
}

// ── Call sounds ────────────────────────────────────────────────────────────
// All default on. Every cue is local and never mixed into what others hear.
namespace {
constexpr auto kCallSoundsEnabled = "calls/sounds/enabled";
constexpr auto kCallSoundsPresence = "calls/sounds/presence";
constexpr auto kCallSoundsControls = "calls/sounds/controls";
constexpr auto kCallSoundsShareAndHand = "calls/sounds/shareAndHand";
constexpr auto kCallSoundVolume = "calls/sounds/volume";
constexpr auto kRingerVolume = "calls/sounds/ringerVolume";
} // namespace

bool SettingsManager::callSoundsEnabled() const
{
    return m_store->value(QLatin1String(kCallSoundsEnabled), true).toBool();
}

void SettingsManager::setCallSoundsEnabled(bool v)
{
    if (callSoundsEnabled() == v)
        return;
    m_store->setValue(QLatin1String(kCallSoundsEnabled), v);
    Q_EMIT callSoundSettingsChanged();
}

bool SettingsManager::callSoundsPresence() const
{
    return m_store->value(QLatin1String(kCallSoundsPresence), true).toBool();
}

void SettingsManager::setCallSoundsPresence(bool v)
{
    if (callSoundsPresence() == v)
        return;
    m_store->setValue(QLatin1String(kCallSoundsPresence), v);
    Q_EMIT callSoundSettingsChanged();
}

bool SettingsManager::callSoundsControls() const
{
    return m_store->value(QLatin1String(kCallSoundsControls), true).toBool();
}

void SettingsManager::setCallSoundsControls(bool v)
{
    if (callSoundsControls() == v)
        return;
    m_store->setValue(QLatin1String(kCallSoundsControls), v);
    Q_EMIT callSoundSettingsChanged();
}

bool SettingsManager::callSoundsShareAndHand() const
{
    return m_store->value(QLatin1String(kCallSoundsShareAndHand), true)
        .toBool();
}

void SettingsManager::setCallSoundsShareAndHand(bool v)
{
    if (callSoundsShareAndHand() == v)
        return;
    m_store->setValue(QLatin1String(kCallSoundsShareAndHand), v);
    Q_EMIT callSoundSettingsChanged();
}

namespace {
// Non-numeric or out-of-range volumes read as the default.
int readPercent(const QVariant &stored, int fallback)
{
    bool ok = false;
    const int v = stored.toInt(&ok);
    if (!ok || v < 0 || v > 100)
        return fallback;
    return v;
}
} // namespace

int SettingsManager::callSoundVolume() const
{
    return readPercent(m_store->value(QLatin1String(kCallSoundVolume),
                                      kDefaultCallSoundVolume),
                       kDefaultCallSoundVolume);
}

void SettingsManager::setCallSoundVolume(int percent)
{
    const int clamped = std::clamp(percent, 0, 100);
    if (callSoundVolume() == clamped)
        return;
    m_store->setValue(QLatin1String(kCallSoundVolume), clamped);
    Q_EMIT callSoundSettingsChanged();
}

int SettingsManager::ringerVolume() const
{
    return readPercent(m_store->value(QLatin1String(kRingerVolume),
                                      kDefaultRingerVolume),
                       kDefaultRingerVolume);
}

void SettingsManager::setRingerVolume(int percent)
{
    const int clamped = std::clamp(percent, 0, 100);
    if (ringerVolume() == clamped)
        return;
    m_store->setValue(QLatin1String(kRingerVolume), clamped);
    Q_EMIT callSoundSettingsChanged();
}

// Call device preferences, device-scoped. The id is bounded and checked for
// control characters because the config is hand-editable. Backslashes are
// allowed: Windows device ids are paths like `\\?\usb#...`. Quoting for
// pipelines is handled at the point of use, and no consumer of these
// accessors interpolates them into a pipeline description.
namespace {
QString sanitizedDeviceId(const QString &id)
{
    if (id.size() > 256)
        return QString();
    for (const QChar c : id) {
        if (c.isNull() || c.category() == QChar::Other_Control)
            return QString();
    }
    return id;
}
} // namespace

QString SettingsManager::preferredMicrophoneId() const
{
    return sanitizedDeviceId(
        m_store->value(QStringLiteral("calls/microphoneId")).toString());
}

void SettingsManager::setPreferredMicrophoneId(const QString &id)
{
    const QString clean = sanitizedDeviceId(id);
    if (preferredMicrophoneId() == clean)
        return;
    m_store->setValue(QStringLiteral("calls/microphoneId"), clean);
    Q_EMIT callDevicePreferenceChanged();
}

QString SettingsManager::preferredSpeakerId() const
{
    return sanitizedDeviceId(
        m_store->value(QStringLiteral("calls/speakerId")).toString());
}

void SettingsManager::setPreferredSpeakerId(const QString &id)
{
    const QString clean = sanitizedDeviceId(id);
    if (preferredSpeakerId() == clean)
        return;
    m_store->setValue(QStringLiteral("calls/speakerId"), clean);
    Q_EMIT callDevicePreferenceChanged();
}

QString SettingsManager::preferredCameraId() const
{
    return sanitizedDeviceId(
        m_store->value(QStringLiteral("calls/cameraId")).toString());
}

void SettingsManager::setPreferredCameraId(const QString &id)
{
    const QString clean = sanitizedDeviceId(id);
    if (preferredCameraId() == clean)
        return;
    m_store->setValue(QStringLiteral("calls/cameraId"), clean);
    Q_EMIT callDevicePreferenceChanged();
}

// Per-room modes are stored per account
// (accounts/<slug>/notifications/room-mode/<roomId>). Reads fall back to the
// legacy global key, which account writes never delete because other
// accounts still fall back to it; forgetDeviceGlobalAccountResidue() removes
// it once no account remains.
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
// Hashed media key (raw event ids never become settings keys) under the
// active account, bounded by an LRU index.
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

namespace {
constexpr int kDraftCap = 256;
} // namespace

QVariantMap SettingsManager::roomDraft(const QString &draftKey) const
{
    if (draftKey.isEmpty())
        return {};
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return {};
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/')
        + slug + QLatin1String("/drafts/") + videoDimsHash(draftKey);
    return m_store->value(key).toMap();
}

void SettingsManager::setRoomDraft(const QString &draftKey,
                                   const QVariantMap &draft)
{
    if (draftKey.isEmpty())
        return;
    const QString slug = activeAccountSlugCached();
    if (slug.isEmpty())
        return; // no account, write NOTHING (verification-dismissal rule)
    const QString hash = videoDimsHash(draftKey);
    const QString base = QLatin1String(kAccountsGroup) + QLatin1Char('/')
        + slug + QLatin1String("/drafts/");
    const QString indexKey = base + QLatin1String("index");
    QStringList index = m_store->value(indexKey).toStringList();
    if (draft.isEmpty()) {
        m_store->remove(base + hash);
        if (index.removeAll(hash) > 0)
            m_store->setValue(indexKey, index);
        return;
    }
    m_store->setValue(base + hash, draft);
    // LRU bound.
    index.removeOne(hash);
    index.append(hash);
    while (index.size() > kDraftCap) {
        const QString victim = index.takeFirst();
        m_store->remove(base + victim);
    }
    m_store->setValue(indexKey, index);
}

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

// Dimensions and size share one LRU index, so eviction drops both keys.
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
    // Remote events only; a local echo's key is transient.
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
// Whitelist of picker ids that may reach the store, so QML cannot compose
// arbitrary keys. "picker" is the id both overlay pickers share.
bool isKnownPickerId(const QString &id)
{
    return id == QLatin1String("picker") || id == QLatin1String("gif")
        || id == QLatin1String("emoji");
}

QString pickerShareKey(const QString &id, const char *dimension)
{
    return QStringLiteral("pickers/%1/%2Share").arg(id, QLatin1String(dimension));
}

// Per mille of the available space. Values outside the range are treated as
// absent; QML also clamps to the live window.
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
    // Out-of-range forgets the value rather than storing it.
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
    // 3 = follow the account default, stored explicitly because an absent key
    // reads as 0 (all messages).
    if (mode < 0 || mode > 3)
        mode = 0;
    if (roomNotificationMode(roomId) == mode)
        return;
    const QString globalKey = roomNotificationModeGlobalKey(roomId);
    const QString scopedKey = roomNotificationModeScopedKey(roomId);
    if (scopedKey.isEmpty()) {
        // No active account: device-local semantics.
        if (mode == 0)
            m_store->remove(globalKey);  // default: keep the file compact
        else
            m_store->setValue(globalKey, mode);
    } else if (mode == 0 && !m_store->contains(globalKey)) {
        m_store->remove(scopedKey);      // default: keep the file compact
    } else {
        // While a legacy global value exists, store even mode 0 explicitly:
        // removing the scoped key would resurrect the legacy mode.
        m_store->setValue(scopedKey, mode);
    }
    Q_EMIT roomNotificationModeChanged(roomId);
}

bool SettingsManager::notificationsEnabled() const
{
    return m_store->value(kNotifications, true).toBool();
}

namespace {
/// Snap to the offered ceilings rather than trusting a stored value.
int snapShareHeight(int v)
{
    if (v <= 900)
        return 720;
    if (v <= 1260)
        return 1080;
    if (v <= 1800)
        return 1440;
    return 2160;
}
int snapShareFps(int v)
{
    if (v <= 22)
        return 15;
    if (v <= 45)
        return 30;
    return 60;
}
} // namespace

int SettingsManager::shareMaxHeight() const
{
    // Default 1080.
    return snapShareHeight(m_store->value(kShareMaxHeight, 1080).toInt());
}

void SettingsManager::setShareMaxHeight(int v)
{
    const int snapped = snapShareHeight(v);
    if (shareMaxHeight() == snapped)
        return;
    m_store->setValue(kShareMaxHeight, snapped);
    Q_EMIT shareQualityChanged();
}

bool SettingsManager::shareQualityDemanding() const
{
    // Software VP8 cannot sustain 4K at 30+ fps; 4K at 15 is left unmarked for
    // text-heavy desktop shares. Kept here so every menu agrees.
    return shareQualityDemandingAt(shareMaxHeight(), shareFps());
}

bool SettingsManager::shareQualityDemandingAt(int maxHeight, int fps) const
{
    // Windows GDI capture sustains 30 fps but not 60 at high resolutions (the
    // rest is duplicated frames). Warn for 2160p at any rate and 1440p at 60.
    if (maxHeight >= 2160)
        return true;
    return maxHeight >= 1440 && fps >= 60;
}

int SettingsManager::shareFps() const
{
    return snapShareFps(m_store->value(kShareFps, 30).toInt());
}

void SettingsManager::setShareFps(int v)
{
    const int snapped = snapShareFps(v);
    if (shareFps() == snapped)
        return;
    m_store->setValue(kShareFps, snapped);
    Q_EMIT shareQualityChanged();
}

bool SettingsManager::autoLoadLinkPreviews() const
{
    // Off by default. Previews are fetched directly from the linked site, so
    // auto-loading would leak the reader's IP and timing to any host a sender
    // links. docs/privacy.md documents this default; change both together.
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
    // Off by default; in encrypted rooms even the fact of a fetch leaks
    // information.
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

bool SettingsManager::sharePresence() const
{
    // Default on, as is the Matrix norm. Disclosed under Privacy & security.
    return m_store->value(kSharePresence, true).toBool();
}

void SettingsManager::setSharePresence(bool v)
{
    if (sharePresence() == v)
        return;
    m_store->setValue(kSharePresence, v);
    Q_EMIT sharePresenceChanged();
}

bool SettingsManager::spacesRailVisible() const
{
    return m_store->value(kSpacesRailVisible, true).toBool();
}

bool SettingsManager::spaceBannersVisible() const
{
    // Shown by default.
    return m_store->value(kSpaceBannersVisible, true).toBool();
}

void SettingsManager::setSpaceBannersVisible(bool v)
{
    if (spaceBannersVisible() == v)
        return;
    m_store->setValue(kSpaceBannersVisible, v);
    Q_EMIT spaceBannersVisibleChanged();
}

bool SettingsManager::spaceBannerExpanded() const
{
    // Cropped to a strip by default.
    return m_store->value(kSpaceBannerExpanded, false).toBool();
}

void SettingsManager::setSpaceBannerExpanded(bool v)
{
    if (spaceBannerExpanded() == v)
        return;
    m_store->setValue(kSpaceBannerExpanded, v);
    Q_EMIT spaceBannerExpandedChanged();
}

void SettingsManager::setSpacesRailVisible(bool v)
{
    if (spacesRailVisible() == v)
        return;
    m_store->setValue(kSpacesRailVisible, v);
    Q_EMIT spacesRailVisibleChanged();
}

bool SettingsManager::roomListVisible() const
{
    return m_store->value(kRoomListVisible, true).toBool();
}

void SettingsManager::setRoomListVisible(bool v)
{
    if (roomListVisible() == v)
        return;
    m_store->setValue(kRoomListVisible, v);
    Q_EMIT roomListVisibleChanged();
}

int SettingsManager::roomListWidth() const
{
    // Clamped on read: a hand-edited value must not break the layout.
    const int stored = m_store->value(kRoomListWidth, 300).toInt();
    return std::clamp(stored, kRoomListMinWidth, kRoomListMaxWidth);
}

void SettingsManager::setRoomListWidth(int px)
{
    const int clamped = std::clamp(px, kRoomListMinWidth, kRoomListMaxWidth);
    if (roomListWidth() == clamped)
        return;
    m_store->setValue(kRoomListWidth, clamped);
    Q_EMIT roomListWidthChanged();
}

int SettingsManager::spacesRailWidth() const
{
    // Defaults to the minimum. Clamped on read like roomListWidth.
    const int stored = m_store->value(kSpacesRailWidth,
                                      kSpacesRailMinWidth).toInt();
    return std::clamp(stored, kSpacesRailMinWidth, kSpacesRailMaxWidth);
}

void SettingsManager::setSpacesRailWidth(int px)
{
    const int clamped =
        std::clamp(px, kSpacesRailMinWidth, kSpacesRailMaxWidth);
    if (spacesRailWidth() == clamped)
        return;
    m_store->setValue(kSpacesRailWidth, clamped);
    Q_EMIT spacesRailWidthChanged();
}

int SettingsManager::spacesRailDepthStyle() const
{
    const int stored =
        m_store->value(kSpacesRailDepthStyle, kRailDepthRegions).toInt();
    // Fall back rather than clamp: an unknown style (e.g. from a newer build)
    // gets the default, not the nearest enum value.
    if (stored < 0 || stored > kMaxSpacesRailDepthStyle)
        return kRailDepthRegions;
    return stored;
}

void SettingsManager::setSpacesRailDepthStyle(int style)
{
    const int valid = (style < 0 || style > kMaxSpacesRailDepthStyle)
                          ? kRailDepthRegions : style;
    if (spacesRailDepthStyle() == valid)
        return;
    m_store->setValue(kSpacesRailDepthStyle, valid);
    Q_EMIT spacesRailDepthStyleChanged();
}

int SettingsManager::sidePanelWidth() const
{
    const int stored = m_store->value(kSidePanelWidth, 320).toInt();
    return std::clamp(stored, kSidePanelMinWidth, kSidePanelMaxWidth);
}

void SettingsManager::setSidePanelWidth(int px)
{
    const int clamped = std::clamp(px, kSidePanelMinWidth, kSidePanelMaxWidth);
    if (sidePanelWidth() == clamped)
        return;
    m_store->setValue(kSidePanelWidth, clamped);
    Q_EMIT sidePanelWidthChanged();
}

bool SettingsManager::closeToTray() const
{
    return m_store->value(kCloseToTray, false).toBool();
}

void SettingsManager::setCloseToTray(bool v)
{
    if (closeToTray() == v)
        return;
    m_store->setValue(kCloseToTray, v);
    Q_EMIT closeToTrayChanged();
    // startInTray() derives from this value, so announce it too.
    Q_EMIT startInTrayChanged();
}

bool SettingsManager::startInTray() const
{
    // Only meaningful with closeToTray; otherwise the app would start
    // invisible.
    return closeToTray() && m_store->value(kStartInTray, false).toBool();
}

void SettingsManager::setStartInTray(bool v)
{
    if (m_store->value(kStartInTray, false).toBool() == v)
        return;
    m_store->setValue(kStartInTray, v);
    Q_EMIT startInTrayChanged();
}

// Window geometry: four ints under one group, so a half-written value
// degrades to "never saved". Read once and size-validated; whether the
// position is on a connected screen is decided by
// AppController::restorableWindowGeometry (keeps QScreen out of this
// Qt6::Core-only class).
void SettingsManager::loadWindowGeometry()
{
    m_initialWindowMaximized = m_store->value(kWindowMaximized, false).toBool();

    const QString group = QLatin1String(kWindowGeometry);
    const int w = m_store->value(group + QLatin1String("/width"), 0).toInt();
    const int h = m_store->value(group + QLatin1String("/height"), 0).toInt();
    // Main.qml's minimumWidth/minimumHeight.
    if (w < kWindowMinWidth || h < kWindowMinHeight)
        return;
    m_initialWindowGeometry =
        QRect(m_store->value(group + QLatin1String("/x"), 0).toInt(),
              m_store->value(group + QLatin1String("/y"), 0).toInt(), w, h);
}

void SettingsManager::saveWindowGeometry(int x, int y, int width, int height)
{
    // Refuse unrestorable sizes: Qt reports transient 0x0 geometry around
    // show/hide/restore.
    if (width < kWindowMinWidth || height < kWindowMinHeight)
        return;
    const QString group = QLatin1String(kWindowGeometry);
    m_store->setValue(group + QLatin1String("/x"), x);
    m_store->setValue(group + QLatin1String("/y"), y);
    m_store->setValue(group + QLatin1String("/width"), width);
    m_store->setValue(group + QLatin1String("/height"), height);
}

void SettingsManager::saveWindowMaximized(bool maximized)
{
    if (m_store->value(kWindowMaximized, false).toBool() == maximized)
        return;
    m_store->setValue(kWindowMaximized, maximized);
}

bool SettingsManager::verificationWarningDismissed() const
{
    // Account-scoped with no global fallback.
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return false;
    return m_store->value(accountKey(slug, kVerifyWarningDismissed), false)
        .toBool();
}

void SettingsManager::setVerificationWarningDismissed(bool v)
{
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty())
        return;
    if (verificationWarningDismissed() == v)
        return;
    m_store->setValue(accountKey(slug, kVerifyWarningDismissed), v);
    Q_EMIT verificationWarningDismissedChanged();
}

qreal SettingsManager::mediaVolume() const
{
    bool ok = false;
    const qreal v = m_store->value(kMediaVolume, 0.8).toDouble(&ok);
    if (!ok || !(v >= 0.0) || !(v <= 1.0))
        return 0.8;
    return v;
}

void SettingsManager::setMediaVolume(qreal v)
{
    const qreal clamped = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    // Slider drags emit continuously; only write real changes.
    if (qFuzzyCompare(mediaVolume() + 1.0, clamped + 1.0))
        return;
    m_store->setValue(kMediaVolume, clamped);
    Q_EMIT mediaVolumeChanged();
}

qreal SettingsManager::mediaPlaybackRate() const
{
    bool ok = false;
    const qreal v = m_store->value(kMediaPlaybackRate, 1.0).toDouble(&ok);
    if (!ok || !(v >= 0.25) || !(v <= 4.0))
        return 1.0;
    return v;
}

void SettingsManager::setMediaPlaybackRate(qreal v)
{
    const qreal clamped = v < 0.25 ? 0.25 : (v > 4.0 ? 4.0 : v);
    if (qFuzzyCompare(mediaPlaybackRate() + 1.0, clamped + 1.0))
        return;
    m_store->setValue(kMediaPlaybackRate, clamped);
    Q_EMIT mediaPlaybackRateChanged();
}

int SettingsManager::gifAutoplay() const
{
    // Default follows the legacy animateGifPreviews boolean.
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
    // Default PG-13 (id 2).
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

bool SettingsManager::showMembershipEvents() const
{
    // Default true: master on + both halves on is the old behaviour.
    return m_store->value(kShowMembership, true).toBool();
}

void SettingsManager::setShowMembershipEvents(bool v)
{
    if (showMembershipEvents() == v)
        return;
    m_store->setValue(kShowMembership, v);
    Q_EMIT showMembershipEventsChanged();
}

bool SettingsManager::showProfileChangeEvents() const
{
    return m_store->value(kShowProfileChanges, true).toBool();
}

void SettingsManager::setShowProfileChangeEvents(bool v)
{
    if (showProfileChangeEvents() == v)
        return;
    m_store->setValue(kShowProfileChanges, v);
    Q_EMIT showProfileChangeEventsChanged();
}

bool SettingsManager::collapseEmbeds() const
{
    // Off by default.
    return appearanceValue(kCollapseEmbeds, false).toBool();
}

void SettingsManager::setCollapseEmbeds(bool v)
{
    if (collapseEmbeds() == v)
        return;
    setAppearanceValue(kCollapseEmbeds, v);
    Q_EMIT collapseEmbedsChanged();
}

bool SettingsManager::reducedMotion() const
{
    return appearanceValue(kReducedMotion, false).toBool();
}

void SettingsManager::setReducedMotion(bool v)
{
    if (reducedMotion() == v)
        return;
    setAppearanceValue(kReducedMotion, v);
    Q_EMIT reducedMotionChanged();
}

bool SettingsManager::smoothScrolling() const
{
    // Default true.
    return appearanceValue(kSmoothScrolling, true).toBool();
}

void SettingsManager::setSmoothScrolling(bool v)
{
    if (smoothScrolling() == v)
        return;
    setAppearanceValue(kSmoothScrolling, v);
    Q_EMIT smoothScrollingChanged();
}

// See the header for why this stores hidden keys.
QStringList SettingsManager::hiddenComposerButtons() const
{
    return appearanceValue(kHiddenComposerButtons, QStringList{}).toStringList();
}

void SettingsManager::setHiddenComposerButtons(const QStringList &keys)
{
    // Normalised so an order or duplicate change does not fire the notify.
    QStringList next = keys;
    next.removeAll(QString());
    next.removeDuplicates();
    next.sort();
    if (next == hiddenComposerButtons())
        return;
    setAppearanceValue(kHiddenComposerButtons, next);
    Q_EMIT hiddenComposerButtonsChanged();
}

void SettingsManager::setComposerButtonShown(const QString &key, bool shown)
{
    if (key.isEmpty())
        return;
    QStringList next = hiddenComposerButtons();
    if (shown)
        next.removeAll(key);
    else if (!next.contains(key))
        next.append(key);
    setHiddenComposerButtons(next);
}

int SettingsManager::clockFormat() const
{
    const int stored = appearanceValue(kClockFormat, kClockFormatSystem).toInt();
    // Out of range follows the system.
    if (stored < kClockFormatSystem || stored > kClockFormat24Hour)
        return kClockFormatSystem;
    return stored;
}

void SettingsManager::setClockFormat(int mode)
{
    if (mode < kClockFormatSystem || mode > kClockFormat24Hour)
        mode = kClockFormatSystem;
    if (clockFormat() == mode)
        return;
    setAppearanceValue(kClockFormat, mode);
    Q_EMIT clockFormatChanged();
}

QString SettingsManager::clockTimeFormat() const
{
    switch (clockFormat()) {
    case kClockFormat12Hour:
        // "AP": the locale's upper-case designators.
        return QStringLiteral("h:mm AP");
    case kClockFormat24Hour:
        return QStringLiteral("HH:mm");
    default:
        break;
    }
    // The locale's short format, queried fresh so locale changes apply.
    return QLocale().timeFormat(QLocale::ShortFormat);
}

bool SettingsManager::enterInsertsNewline() const
{
    return m_store->value(kEnterNewline, false).toBool();
}

void SettingsManager::setEnterInsertsNewline(bool v)
{
    if (enterInsertsNewline() == v)
        return;
    m_store->setValue(kEnterNewline, v);
    Q_EMIT enterInsertsNewlineChanged();
}

QString SettingsManager::composerMode() const
{
    // Anything but "rich" reads as markdown, so a downgrade cannot strand the
    // composer.
    const QString stored =
        m_store->value(kComposerMode, QStringLiteral("markdown")).toString();
    return stored == QLatin1String("rich") ? stored
                                           : QStringLiteral("markdown");
}

void SettingsManager::setComposerMode(const QString &mode)
{
    const QString normalized = mode == QLatin1String("rich")
        ? QStringLiteral("rich")
        : QStringLiteral("markdown");
    if (composerMode() == normalized)
        return;
    m_store->setValue(kComposerMode, normalized);
    Q_EMIT composerModeChanged();
}

bool SettingsManager::spellCheckEnabled() const
{
    return m_store->value(kSpellCheckEnabled, true).toBool();
}

void SettingsManager::setSpellCheckEnabled(bool enabled)
{
    if (spellCheckEnabled() == enabled)
        return;
    m_store->setValue(kSpellCheckEnabled, enabled);
    Q_EMIT spellCheckEnabledChanged();
}

QString SettingsManager::spellCheckLanguage() const
{
    return m_store->value(kSpellCheckLanguage, QString()).toString().trimmed();
}

void SettingsManager::setSpellCheckLanguage(const QString &tag)
{
    const QString normalized = tag.trimmed();
    if (spellCheckLanguage() == normalized)
        return;
    if (normalized.isEmpty())
        m_store->remove(kSpellCheckLanguage);
    else
        m_store->setValue(kSpellCheckLanguage, normalized);
    Q_EMIT spellCheckLanguageChanged();
}

bool SettingsManager::sendTextAsCaption() const
{
    return m_store->value(kTextAsCaption, false).toBool();
}

void SettingsManager::setSendTextAsCaption(bool v)
{
    if (sendTextAsCaption() == v)
        return;
    m_store->setValue(kTextAsCaption, v);
    Q_EMIT sendTextAsCaptionChanged();
}

namespace {
// Action ids become part of a key path; unsafe ids (e.g. containing '/') are
// refused, not sanitised, since every id comes from the registry.
bool shortcutIdIsSafe(const QString &actionId)
{
    if (actionId.isEmpty() || actionId.size() > 64)
        return false;
    for (const QChar c : actionId) {
        if (c.isLetterOrNumber() && c.unicode() < 128)
            continue;
        if (c == QLatin1Char('.') || c == QLatin1Char('_')
            || c == QLatin1Char('-'))
            continue;
        return false;
    }
    return true;
}
} // namespace

QString SettingsManager::shortcutSequence(const QString &actionId) const
{
    if (!shortcutIdIsSafe(actionId))
        return {};
    const QString leaf =
        QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionId;
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty()) {
        const QString key =
            QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
            + QLatin1Char('/') + leaf;
        if (m_store->contains(key))
            return m_store->value(key).toString();
    }
    return m_store->value(leaf).toString();
}

void SettingsManager::setShortcutSequence(const QString &actionId,
                                          const QString &portable)
{
    if (!shortcutIdIsSafe(actionId))
        return;
    const QString leaf =
        QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionId;
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty()) {
        m_store->setValue(QLatin1String(kAccountsGroup) + QLatin1Char('/')
                              + slug + QLatin1Char('/') + leaf,
                          portable);
    }
    // The global copy is the logged-out default and seeds new accounts.
    m_store->setValue(leaf, portable);
}

void SettingsManager::clearShortcutSequence(const QString &actionId)
{
    if (!shortcutIdIsSafe(actionId))
        return;
    const QString leaf =
        QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionId;
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (!slug.isEmpty()) {
        // Remove both copies, or the global override would come back.
        m_store->remove(QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
                        + QLatin1Char('/') + leaf);
    }
    m_store->remove(leaf);
}

int SettingsManager::interfaceZoom() const
{
    const int stored = m_store->value(kInterfaceZoom, 100).toInt();
    return (stored < kMinInterfaceZoom || stored > kMaxInterfaceZoom)
        ? 100
        : stored;
}

void SettingsManager::setInterfaceZoom(int percent)
{
    percent = std::clamp(percent, kMinInterfaceZoom, kMaxInterfaceZoom);
    if (interfaceZoom() == percent)
        return;
    m_store->setValue(kInterfaceZoom, percent);
    Q_EMIT interfaceZoomChanged();
}

int SettingsManager::timelineWheelSpeed() const
{
    const int stored = m_store->value(kTimelineWheelSpeed,
                                      kDefaultTimelineWheelSpeed).toInt();
    // Unknown values fall back to Fast.
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
    // No SecretStore wired: legacy plaintext key. Unreachable in normal runs.
    return m_store->value(kAccessTokenLegacy).toString();
}

bool SettingsManager::updateSessionTokens(const QString &userId,
                                          const QString &accessToken,
                                          const QString &refreshToken)
{
    // Only the two credentials change; saveSession() would also clear the sync
    // token and re-assert the active account.
    const QString uid = userId.trimmed();
    if (uid.isEmpty() || accessToken.isEmpty() || !m_secretStore)
        return false;
    bool ok = m_secretStore->storeSecret(uid, QLatin1String(kSecretAccessToken),
                                         accessToken);
    // Written even when empty so a stale refresh token cannot be replayed.
    ok = m_secretStore->storeSecret(uid, QLatin1String(kSecretRefreshToken),
                                    refreshToken)
         && ok;
    if (!ok) {
        // Never echoes either value.
        qCWarning(lcSettings) << "failed to persist refreshed session tokens";
    }
    return ok;
}

QString SettingsManager::refreshToken() const
{
    return refreshTokenFor(userId());
}

QString SettingsManager::refreshTokenFor(const QString &userId) const
{
    const QString uid = userId.trimmed();
    if (uid.isEmpty() || !m_secretStore)
        return {};
    // Empty is normal for sessions without refresh tokens.
    return m_secretStore->readSecret(uid, QLatin1String(kSecretRefreshToken));
}

QString SettingsManager::oauthClientIdFor(const QString &userId) const
{
    const QString uid = userId.trimmed();
    if (uid.isEmpty() || !m_secretStore)
        return {};
    return m_secretStore->readSecret(uid, QLatin1String(kSecretOAuthClientId));
}

QString SettingsManager::authTypeFor(const QString &userId) const
{
    // Routing decision for restore ("oauth" -> oauth().restore_session(),
    // otherwise matrix_auth()). Not a secret, so it is readable with a locked
    // keyring.
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return QStringLiteral("password");
    const QString value =
        m_store->value(accountKey(slug, kAccountAuthType)).toString().trimmed();
    return value.isEmpty() ? QStringLiteral("password") : value;
}

bool SettingsManager::isOAuthAccount(const QString &userId) const
{
    return authTypeFor(userId) == QLatin1String("oauth");
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
                                  const QString &accessToken_,
                                  const QString &refreshToken_,
                                  const QString &authType_,
                                  const QString &oauthClientId_)
{
    const bool hsChanged = homeserverUrl() != homeserverUrl_;

    // Canonicalise so records, secrets and store paths agree (matches the Rust
    // store-path resolution).
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
    // A fresh login is a new device; drop the old sync position.
    const QString slug = matrix::app_data::safeUserSlug(uidCanonical);
    m_store->remove(accountKey(slug, kAccountSyncToken));
    m_store->setValue(kActiveAccount, uidCanonical);
    // Keep the login prefill on the most recently used homeserver.
    m_store->setValue(kHomeserver, hsCanonical);

    // In QSettings so restore can route correctly with a locked keyring.
    const QString authType = authType_.trimmed().isEmpty()
                                 ? QStringLiteral("password")
                                 : authType_.trimmed();
    m_store->setValue(accountKey(slug, kAccountAuthType), authType);

    if (m_secretStore) {
        if (!m_secretStore->storeSecret(uidCanonical, QLatin1String(kSecretAccessToken), accessToken_)) {
            qCWarning(lcSettings)
                << "failed to persist access token to SecretStore:"
                << m_secretStore->lastError();
        }
        // Rewritten on every save, even when empty, so a previous session's
        // refresh token cannot be replayed.
        if (!m_secretStore->storeSecret(uidCanonical,
                                        QLatin1String(kSecretRefreshToken),
                                        refreshToken_)) {
            // Do not echo the value or error detail.
            qCWarning(lcSettings) << "failed to persist refresh token to SecretStore";
        }
        if (!m_secretStore->storeSecret(uidCanonical,
                                        QLatin1String(kSecretOAuthClientId),
                                        oauthClientId_)) {
            qCWarning(lcSettings) << "failed to persist OAuth client id to SecretStore";
        }
        // Make sure a stale legacy plaintext token is not left behind.
        m_store->remove(kAccessTokenLegacy);
    } else {
        // Unwired store: see accessToken().
        m_store->setValue(kAccessTokenLegacy, accessToken_);
    }

    // Other accounts keep their records and tokens. Flush now: the SDK store
    // directory is created before the server is contacted, and a store without
    // a durable record would be treated as an orphan and deleted.
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

    // Accept the exact saved id, or resolve a localpart/mixed-case form.
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
            // resolveAccountIdentity preserves localpart case; fall back to the
            // case-insensitive lookup so the canonical record is found.
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
        // Last account removed: drop the device-global room/Space keys.
        if (savedAccountUserIds().isEmpty())
            forgetDeviceGlobalAccountResidue();
        Q_EMIT accountsChanged();
    }
    if (activeAccount) {
        m_store->remove(kActiveAccount);
        m_store->remove(kAccessTokenLegacy);
    }

    bool secretsCleared = true;
    if (m_secretStore) {
        // Use the exact persisted key so a mixed-case id cannot orphan its
        // entry.
        secretsCleared = m_secretStore->clearAccountSecrets(recordUserId);
        if (!secretsCleared) {
            qCWarning(lcSettings)
                << "failed to clear account secrets from SecretStore:"
                << m_secretStore->lastError();
        }
    }

    // Flush: the secrets are already gone, and a crash before QSettings writes
    // would leave a record that restores to nothing.
    m_store->sync();

    if (activeAccount)
        Q_EMIT sessionChanged();
    return secretsCleared;
}
