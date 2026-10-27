#include "media/ImageMetadata.h"
#include "matrix/RoomUnreadRule.h"
#include "app/SettingsManager.h"

#include "i18n/TimeLocale.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include "storage/SecretStore.h"
// Header-only use: the mode keys and their parser, no audio code.
#include "calls/noise/NoiseSuppressor.h"
#include "storage/AppDataPaths.h"
#include "matrix/MediaStoreKey.h"
#include "models/ConversationOrder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEvent>
#include <QHash>
#include <QLocale>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

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
constexpr auto kPreviewsInlineMedia = "previews/inlineMedia";
// Screen-share quality. Device-global. Clamped on read and write because
// the value reaches a GStreamer caps string.
constexpr auto kShareMaxHeight      = "calls/shareMaxHeight";
constexpr auto kShareFps            = "calls/shareFps";
constexpr auto kShareBitrateCap     = "calls/shareBitrateCapKbps";
// Camera ceiling and webrtcdsp switches. Device-global; 0 is Auto.
constexpr auto kCameraResolution    = "calls/cameraResolution";
constexpr auto kEchoCancellation    = "calls/echoCancellation";
constexpr auto kAutoGainControl     = "calls/autoGainControl";
constexpr auto kSharePresence = "presence/shareOwn";
constexpr auto kSpacesRailVisible = "shell/spacesRailVisible";
constexpr auto kSpaceBannersVisible = "shell/spaceBannersVisible";
constexpr auto kSpaceBannerExpanded = "shell/spaceBannerExpanded";
constexpr auto kKeepRoomListOrderStill = "shell/keepRoomListOrderStill";
constexpr auto kRoomListSort = "shell/roomListSort";
constexpr auto kRailSort = "shell/railSort";
constexpr auto kRoomListVisible   = "shell/roomListVisible";
constexpr auto kRoomListWidth     = "shell/roomListWidth";
constexpr auto kRoomListExpandedWidth = "shell/roomListExpandedWidth";
constexpr auto kMemberPanelOpen   = "shell/memberPanelOpen";
constexpr auto kLastAttachFolder = "shell/lastAttachFolder";
constexpr auto kSpacesRailWidth   = "shell/spacesRailWidth";
constexpr auto kSpacesRailDepthStyle = "shell/spacesRailDepthStyle";
constexpr auto kSidePanelWidth    = "shell/sidePanelWidth";
constexpr auto kCloseToTray       = "shell/closeToTray";
constexpr auto kGlobalShortcuts   = "shell/globalShortcuts";
constexpr auto kWindowGeometry    = "shell/windowGeometry";
constexpr auto kWindowMaximized   = "shell/windowMaximized";
constexpr auto kStartInTray       = "shell/startInTray";
// Account-scoped only; no global fallback key.
constexpr auto kVerifyWarningDismissed = "security/verifyWarningDismissed";
constexpr auto kMediaVolume         = "media/volume";       // 0..1
constexpr auto kMediaPlaybackRate   = "media/playbackRate";  // 0.25..4.0
constexpr auto kVoiceMiniPlayerCorner = "media/miniPlayerCorner"; // 0..3
constexpr auto kVoicePlayerDocked   = "media/playerDocked";   // bool
constexpr auto kLargeAlbumArt       = "media/largeAlbumArt";    // bool, default off
// QRectF: x/y fractions of the free space, width/height pixels.
constexpr auto kVoiceMiniPlayerGeometry = "media/miniPlayerGeometry";
constexpr auto kVoicePlaybackRate   = "media/voicePlaybackRate"; // 0.25..4.0
// GIF browser policy.
constexpr auto kGifAutoplay         = "gif/autoplay";       // 0/1/2
constexpr auto kGifSafeSearch       = "gif/safeSearch";     // gif::Rating id
constexpr auto kGifStoreRecent      = "gif/storeRecent";    // bool
constexpr auto kVideoPrefetchEnabled = "media/videoPrefetchEnabled"; // bool
// Set once migratePrefetchFromAnimation() has run (see there).
constexpr auto kPrefetchDecoupledMarker = "media/prefetchDecoupledFromAnimation";
constexpr auto kVideoPrefetchMaxMb  = "media/videoPrefetchMaxMb";   // int, MB
constexpr auto kGifProvider         = "gif/provider";       // "giphy"/"klipy"
// Presentation-only timeline preferences; the model keeps every event, so
// changing them never requires a resync.
constexpr auto kShowRoomActivity    = "timeline/showRoomActivity";
constexpr auto kHideThreadReplies   = "timeline/hideThreadReplies";
constexpr auto kShowMembership      = "timeline/showMembershipEvents";
constexpr auto kShowProfileChanges  = "timeline/showProfileChangeEvents";
constexpr auto kCollapseEmbeds      = "timeline/collapseEmbeds";
constexpr auto kReducedMotion       = "ui/reducedMotion";
constexpr auto kHighContrast        = "ui/highContrast";
constexpr auto kUnderlineLinks      = "ui/underlineLinks";
constexpr auto kNameColorCorrection = "ui/nameColorCorrection";
constexpr auto kMessageSpacing      = "ui/messageSpacing";
constexpr auto kThreadStyle         = "ui/threadStyle";
constexpr auto kThreadPlacement     = "ui/threadPlacement";
constexpr auto kLayoutPreset        = "ui/layoutPreset";
// The Nheko compact profile's own overrides (account-scoped, never synced):
// written only while that preset is selected, so the ordinary keys above
// stay the Lightning configuration.
constexpr auto kNhekoMessageLayout  = "ui/nhekoCompact/messageLayout";
constexpr auto kNhekoMessageSpacing = "ui/nhekoCompact/messageSpacing";
constexpr auto kNhekoThreadPlacement = "ui/nhekoCompact/threadPlacement";
constexpr auto kSmoothScrolling     = "ui/smoothScrolling";
constexpr auto kConversationReturn  = "ui/conversationReturnMode";
constexpr auto kShowSelectionReturn = "ui/showSelectionOnReturn";
constexpr auto kDisableCursorBlink  = "ui/disableCursorBlink";
constexpr auto kShowBridgeBadges    = "ui/showBridgeBadges";
constexpr auto kCompactRoomList     = "ui/compactRoomList";
constexpr auto kHiddenComposerButtons = "ui/hiddenComposerButtons";
constexpr auto kClockFormat         = "ui/clockFormat";
constexpr auto kEnterNewline        = "composer/enterInsertsNewline";
constexpr auto kComposerMode        = "composer/mode";
// Set once migrateComposerModeToMarkdown() has run (see there).
constexpr auto kComposerMarkdownMarker = "composer/markdownDefaultMigrated";
constexpr auto kSpellCheckEnabled   = "composer/spellCheck";
constexpr auto kSpellCheckLanguage  = "composer/spellLanguage";
constexpr auto kSpellExtraLanguages = "composer/spellExtraLanguages";
constexpr auto kAutocorrectMode     = "composer/autocorrect";
constexpr auto kTextAsCaption       = "composer/textAsCaption";
constexpr auto kImageMetadataMode   = "composer/imageMetadataMode";
constexpr auto kEmojiShortcodeAutoConvert = "composer/emojiShortcodeAutoConvert";
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
// Sign-outs begun and not finished (B044), one group per identity slug.
constexpr auto kPendingSignOutGroup = "pendingSignOut";
// Sign-ins begun and not finished (R6), one group per identity slug: the
// store a fresh password sign-in creates before the server has answered.
constexpr auto kPendingSignInGroup = "pendingSignIn";
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
// The one-time "index all messages now?" answer. See indexAllOfferAnswer.
constexpr auto kAccountIndexAllOffer = "indexAllOffer";
// Labs (concurrent live accounts). See setKeepRunningFor / setLastLiveAtFor.
constexpr auto kAccountKeepRunning  = "keepRunning";
constexpr auto kAccountLastLiveAt   = "lastLiveAt";
// The account's network route, device-local (docs/network-profiles.md). The
// non-secret half only; a proxy password is in the SecretStore
// (matrix/NetworkRoute.h). Never synced, never exported.
constexpr auto kAccountNetworkRoute = "networkRoute";
constexpr auto kLabsConcurrentAccounts = "labs/concurrentAccounts";
constexpr auto kLabsConcurrentAccountsWarmSlot = "labs/concurrentAccountsWarmSlot";

// A Labs switch reads on only from a stored bool true: QSettings hands a bool
// back as a real bool in-process and as the string "true" after a reload (the
// INI and registry formats), and nothing else counts. QVariant::toBool() would
// also take "1", 1 and "yes", so a hand edit or another program's value could
// start background network activity for every pinned account.
bool storedTrue(const QVariant &v)
{
    if (v.metaType().id() == QMetaType::Bool)
        return v.toBool();
    return v.metaType().id() == QMetaType::QString
        && v.toString() == QLatin1String("true");
}

// SecretStore keys.
constexpr auto kSecretAccessToken   = "accessToken";
// OAuth credentials, stored in the SecretStore beside the access token and
// never exposed to QML.
constexpr auto kSecretRefreshToken  = "refreshToken";
constexpr auto kSecretOAuthClientId = "oauthClientId";

// Names this install's items in a secret store shared with other installs
// (SecretStore::setInstallScope). One per settings file, the file holding the
// account records those items belong to. Created once, never removed.
constexpr auto kKeyringInstallId    = "keyring/installId";
// Per account, shared stores only: this install's items for the session.
//   keyringItems  "own"                - signed in by this build.
//                 "adopted-unverified" - copied from an item written before
//                                        install scoping, which another install
//                                        on this computer may also use.
//   keyringDevice  the record's deviceId when they were written. An older build
//                  that signs in again changes deviceId and not this, so items
//                  left from an earlier device are never read for a newer one.
// Both absent: recorded before scoping; adopted on first use.
constexpr auto kAccountKeyringItems  = "keyringItems";
constexpr auto kAccountKeyringDevice = "keyringDevice";
// Per account: where its sign-in was last written, or found
// (SecretStore::backendKindFor(), "file" or "service"), for a store with more
// than one backend. That store reads the account there whatever this launch's
// own backend is (TwoBackendSecretStore); a launch on the other one also says
// so (keyringBackendChanged()).
constexpr auto kAccountKeyringBackend = "keyringBackend";
constexpr auto kKeyringItemsOwn      = "own";
constexpr auto kKeyringItemsAdopted  = "adopted-unverified";
//                 "declined-other-install" - the adoption check found the copied
//                                        token is another install's device:
//                                        this install's copy is gone and the
//                                        shared item is never adopted again.
constexpr auto kKeyringItemsDeclined = "declined-other-install";

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
    // The flush throttle (see kFlushDelayMs): QSettings posts itself an
    // UpdateRequest after a change and rewrites the file when it arrives. The
    // filter holds that back and a single-shot timer writes once instead. Not
    // restarted by later writes, so a steady stream of them still reaches the
    // disk within kFlushDelayMs.
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushDelayMs);
    m_flushTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_flushTimer, &QTimer::timeout, this,
            [this] { m_store->sync(); });
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(kDragHoldMaxMs);
    connect(&m_holdTimer, &QTimer::timeout, this,
            [this] { holdWritesWhileDragging(false); });
    m_store->installEventFilter(this);
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this,
                &SettingsManager::flushPendingWrites);
    }
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
    migratePrefetchFromAnimation();
    migrateComposerModeToMarkdown();
    // B118: a damaged settings file used to look exactly like a fresh install.
    // QSettings reports a parse failure only at the first sync (in this
    // constructor), and
    // most garbage parses "fine" as no keys, so the account stores on disk are
    // the other witness. Count only; nothing is rebuilt from them.
    // R6: stores of sign-ins that never finished. Read (and markers whose
    // store is gone dropped, one stat each) at every start.
    const QSettings::Status readStatus = m_store->status();
    const QSet<QString> unfinished = readStatus == QSettings::NoError
        ? pendingSignInStorePaths() : QSet<QString>{};
    if (readStatus != QSettings::NoError) {
        m_settingsReadProblem = SettingsUnreadable;
        qCWarning(lcSettings) << "the settings file could not be read status="
                              << int(readStatus);
    } else if (savedAccountUserIds().isEmpty()) {
        QSet<QString> pending;
        m_store->beginGroup(QLatin1String(kPendingSignOutGroup));
        for (const QString &slug : m_store->childGroups())
            pending.insert(QDir::cleanPath(
                m_store->value(slug + QLatin1String("/storePath")).toString()));
        m_store->endGroup();
        // R6: the store of a sign-in that never finished (cancelled, or the
        // process quit or was killed before the server answered) was never an
        // account on this device, so it is no witness that the file lost one.
        int unlisted = 0;
        int unfinishedStores = 0;
        for (const QString &store :
             matrix::app_data::findRustStoresIn(matrix::app_data::primaryRoot())) {
            const QString clean = QDir::cleanPath(store);
            if (unfinished.contains(clean))
                ++unfinishedStores;
            else if (!pending.contains(clean))
                ++unlisted;
        }
        if (unfinishedStores > 0)
            qCInfo(lcSettings) << unfinishedStores
                               << "store(s) left by a sign-in that did not finish;"
                                  " not counted as a missing account";
        if (unlisted > 0) {
            m_settingsReadProblem = AccountsMissing;
            qCWarning(lcSettings) << "the settings file lists no account but"
                                  << unlisted
                                  << "account store(s) exist on this device";
        }
    }
    loadWindowGeometry();
}

SettingsManager::~SettingsManager()
{
    flushPendingWrites();
}

void SettingsManager::flushPendingWrites()
{
    m_flushTimer.stop();
    // A sync with nothing changed reads and writes nothing.
    m_store->sync();
}

void SettingsManager::holdWritesWhileDragging(bool held)
{
    if (held == m_writesHeld)
        return;
    m_writesHeld = held;
    if (held) {
        // Nothing is written mid-drag, not even what the timer was already
        // waiting to write.
        m_flushTimer.stop();
        m_holdTimer.start();
        return;
    }
    m_holdTimer.stop();
    flushPendingWrites();
}

bool SettingsManager::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_store.get() && event->type() == QEvent::UpdateRequest) {
        // QSettings keeps the change pending (and posts no further request)
        // until the timer's sync() clears it.
        // Held while a slider is being dragged: the release writes once.
        if (!m_writesHeld && !m_flushTimer.isActive())
            m_flushTimer.start();
        return true;
    }
    return QObject::eventFilter(watched, event);
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
        if (m_store->value(key).toString() != storeSlug)
            m_store->setValue(key, storeSlug);
        // R6: the store is an account's now, whatever identity the sign-in
        // that created it was typed as (a bare username, other casing).
        matrix::app_data::AccountIdentity recorded;
        if (resolveSavedIdentity(userId, &recorded))
            dropPendingSignInsNaming(recorded.rustStorePath);
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

bool SettingsManager::secretsWrittenHereForRecordedDevice(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId.trimmed());
    if (slug.isEmpty())
        return false;
    const QString device =
        m_store->value(accountKey(slug, kAccountDeviceId)).toString();
    return !device.trimmed().isEmpty() && keyringItemsAreCurrent(slug)
        && m_store->value(accountKey(slug, kAccountKeyringItems)).toString()
               == QLatin1String(kKeyringItemsOwn);
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

void SettingsManager::migratePrefetchFromAnimation()
{
    // Until 2026-10-20 gif/autoplay "Never" (2) also refused every speculative
    // video/audio download; since then that is "Preload videos and audio"
    // (media/videoPrefetchEnabled, default on) alone. A user who chose Never
    // had preloading OFF in effect, so carry that over once instead of
    // silently starting downloads after the upgrade. Both keys are global, the
    // scope they are read in; gifAutoplay() includes the legacy
    // animateGifPreviews=false fallback. Runs once: a later choice to turn
    // preloading back on is the user's.
    if (m_store->value(kPrefetchDecoupledMarker).toBool())
        return;
    if (gifAutoplay() == 2)
        m_store->setValue(kVideoPrefetchEnabled, false);
    m_store->setValue(kPrefetchDecoupledMarker, true);
}

void SettingsManager::migrateComposerModeToMarkdown()
{
    // Markdown is the default composer mode: users type "# a" and "* a" and
    // expect them to format. The store cannot tell a stored "rich" that was
    // chosen from one left by an old build, so every profile is moved to
    // Markdown ONCE; a "rich" chosen afterwards is the user's and sticks,
    // because the marker is then set.
    if (m_store->value(kComposerMarkdownMarker).toBool())
        return;
    m_store->setValue(kComposerMode, QStringLiteral("markdown"));
    m_store->setValue(kComposerMarkdownMarker, true);
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

QString SettingsManager::indexAllOfferAnswer(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return {};
    return m_store->value(accountKey(slug, kAccountIndexAllOffer)).toString();
}

void SettingsManager::setIndexAllOfferAnswer(const QString &userId,
                                             const QString &answer)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return;
    m_store->setValue(accountKey(slug, kAccountIndexAllOffer), answer);
    m_store->sync();
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
    return readAccountSecret(uid, kSecretAccessToken);
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
    Q_EMIT highContrastChanged();
    Q_EMIT underlineLinksChanged();
    Q_EMIT nameColorCorrectionChanged();
    Q_EMIT messageSpacingChanged();
    Q_EMIT threadStyleChanged();
    Q_EMIT threadPlacementChanged();
    Q_EMIT layoutPresetChanged();
    // Pane widths follow the account's preset (paneWidthKey).
    Q_EMIT roomListWidthChanged();
    Q_EMIT spacesRailWidthChanged();
    Q_EMIT sidePanelWidthChanged();
    Q_EMIT smoothScrollingChanged();
    Q_EMIT conversationReturnModeChanged();
    Q_EMIT showSelectionOnReturnChanged();
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

// ---- Labs: concurrent live accounts, per account ---------------------------
// None of these syncs the file or emits accountsChanged() (design R2-L3): a
// pin or a timestamp rewritten every ten minutes for every live account must
// not flush the settings file or rebuild the switcher and the session
// lifecycle that listen to accountsChanged(). QSettings writes them out on its
// own deferred sync and at exit.

SettingsManager::KeepRunningResult
SettingsManager::setKeepRunningFor(const QString &userId, bool on)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return KeepRunningResult::RefusedNoRecord;
    if (keepRunningFor(userId) == on)
        return KeepRunningResult::Unchanged;
    // Pins are never evicted for capacity, so the cap refuses the new one.
    // keepRunningUserIds() lists pins with a record only: a pin cannot outlive
    // its record (it lives inside it).
    if (on && keepRunningUserIds().size() >= kMaxPinnedAccounts)
        return KeepRunningResult::RefusedCap;
    const QString key = accountKey(slug, kAccountKeepRunning);
    if (on)
        m_store->setValue(key, true);
    else
        m_store->remove(key); // default: keep the file compact
    Q_EMIT keepRunningChanged(userId);
    return KeepRunningResult::Ok;
}

QVariantMap SettingsManager::networkRouteRecord(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return {};
    const QString group = accountKey(slug, kAccountNetworkRoute) + QLatin1Char('/');
    const QString kind = m_store->value(group + QLatin1String("kind")).toString();
    if (kind.isEmpty())
        return {};
    return {
        {QStringLiteral("kind"), kind},
        {QStringLiteral("host"), m_store->value(group + QLatin1String("host")).toString()},
        {QStringLiteral("port"), m_store->value(group + QLatin1String("port")).toInt()},
        {QStringLiteral("username"),
         m_store->value(group + QLatin1String("username")).toString()},
    };
}

bool SettingsManager::setNetworkRouteRecord(const QString &userId, const QVariantMap &record)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return false;
    const QString groupKey = accountKey(slug, kAccountNetworkRoute);
    m_store->remove(groupKey);
    const QString kind = record.value(QStringLiteral("kind")).toString();
    if (!kind.isEmpty()) {
        const QString group = groupKey + QLatin1Char('/');
        m_store->setValue(group + QLatin1String("kind"), kind);
        m_store->setValue(group + QLatin1String("host"),
                          record.value(QStringLiteral("host")).toString());
        m_store->setValue(group + QLatin1String("port"),
                          record.value(QStringLiteral("port")).toInt());
        const QString username = record.value(QStringLiteral("username")).toString();
        if (!username.isEmpty())
            m_store->setValue(group + QLatin1String("username"), username);
    }
    // Flushed now: a route the user just set must still be there if the
    // process dies before the deferred sync, or the next start goes direct.
    m_store->sync();
    Q_EMIT networkRouteChanged(userId);
    return true;
}

bool SettingsManager::networkRouteConfigured(const QString &userId) const
{
    return !networkRouteRecord(userId).isEmpty();
}

bool SettingsManager::keepRunningFor(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    return !slug.isEmpty()
        && storedTrue(m_store->value(accountKey(slug, kAccountKeepRunning)));
}

QStringList SettingsManager::keepRunningUserIds() const
{
    QStringList out;
    for (const QString &uid : savedAccountUserIds()) {
        if (keepRunningFor(uid))
            out.append(uid);
    }
    return out;
}

qint64 SettingsManager::lastLiveAtFor(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return 0;
    bool ok = false;
    const qint64 ms =
        m_store->value(accountKey(slug, kAccountLastLiveAt)).toLongLong(&ok);
    return ok && ms > 0 ? ms : 0;
}

void SettingsManager::setLastLiveAtFor(const QString &userId, qint64 unixMs)
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return;
    const QString key = accountKey(slug, kAccountLastLiveAt);
    if (unixMs <= 0)
        m_store->remove(key);
    else
        m_store->setValue(key, unixMs);
}

QStringList SettingsManager::locallyDecidedNotificationRoomsFor(
    const QString &userId) const
{
    if (slugForSavedAccount(userId).isEmpty())
        return {};
    // The scope roomNotificationModeDecidesLocallyFor() reads, so the set and
    // the predicate cannot disagree for the active account either.
    const QString prefix = scopePrefixFor(slugForAccountScope(userId));
    if (prefix.isEmpty())
        return {};
    QSet<QString> candidates;
    // The account's own unsynced choices: listed means deciding locally.
    m_store->beginGroup(prefix + QStringLiteral("notifications/room-mode-unsynced"));
    const QStringList unsynced = m_store->childKeys();
    m_store->endGroup();
    for (const QString &room : unsynced)
        candidates.insert(room);
    // Legacy device-global modes (chosen before modes were saved to the
    // server) decide on this device for every account without its own key.
    m_store->beginGroup(QStringLiteral("notifications/room-mode"));
    const QStringList legacy = m_store->childKeys();
    m_store->endGroup();
    for (const QString &room : legacy)
        candidates.insert(room);
    QStringList out;
    for (const QString &room : std::as_const(candidates)) {
        if (roomNotificationModeDecidesLocallyFor(userId, room))
            out.append(room);
    }
    out.sort();
    return out;
}

bool SettingsManager::keyringItemsDirectlyReadableFor(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty() || !m_secretStore)
        return false;
    // Every term is a settings-file read or a structural property of the
    // store; none of them reads an item.
    if (keyringItemsAdoptedUnverified(userId))
        return false;
    // This install's copy was removed as another install's sign-in: the
    // settings file already knows a read would come back empty, and the
    // foreground owes the user the keyring_other_install card for it.
    if (keyringItemsDeclinedForOtherInstall(userId))
        return false;
    return !m_secretStore->isSharedBetweenInstalls()
        || keyringItemsAreCurrent(slug);
}

void SettingsManager::noteSessionTokensWrittenElsewhere(const QString &userId)
{
    noteKeyringBackend(userId.trimmed());
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
    // Before any read or write: an unscoped shared store refuses both.
    if (m_secretStore && m_secretStore->isSharedBetweenInstalls())
        m_secretStore->setInstallScope(keyringInstallId());
    // Before any read or write too: a store with two backends reads each
    // account in the one its record names. Pulled on every use, so a record
    // written later is what the next read sees; a store that outlives this
    // object reads as if nothing were recorded.
    if (m_secretStore) {
        m_secretStore->setRecordedBackendLookup(
            [self = QPointer<SettingsManager>(this)](const QString &userId) {
                return self ? self->keyringBackendForStore(userId) : QString();
            });
    }
    migratePlaintextTokenIfPresent();
    // After the plaintext migration: an account it moved has its own items.
    adoptLegacyKeyringSessions();
    Q_EMIT secretBackendChanged();
    Q_EMIT sessionChanged();
}

QString SettingsManager::keyringInstallId()
{
    const QString existing =
        m_store->value(QLatin1String(kKeyringInstallId)).toString().trimmed();
    if (!existing.isEmpty())
        return existing;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_store->setValue(QLatin1String(kKeyringInstallId), id);
    // On disk before anything is stored under it: an id that did not survive
    // the restart would strand every item written with it.
    m_store->sync();
    if (m_store->status() != QSettings::NoError) {
        qCWarning(lcSettings)
            << "could not save this install's keyring id; secure storage "
               "stays unavailable until the settings file is writable";
        m_store->remove(QLatin1String(kKeyringInstallId));
        return {};
    }
    qCInfo(lcSettings) << "created this install's keyring id";
    return id;
}

bool SettingsManager::keyringItemsAreCurrent(const QString &slug) const
{
    const QVariant forDevice =
        m_store->value(accountKey(slug, kAccountKeyringDevice));
    return forDevice.isValid()
        && forDevice.toString()
               == m_store->value(accountKey(slug, kAccountDeviceId)).toString();
}

void SettingsManager::markKeyringItems(const QString &slug,
                                       const char *how) const
{
    m_store->setValue(accountKey(slug, kAccountKeyringItems),
                      QLatin1String(how));
    m_store->setValue(
        accountKey(slug, kAccountKeyringDevice),
        m_store->value(accountKey(slug, kAccountDeviceId)).toString());
    m_store->sync();
}

void SettingsManager::noteKeyringBackend(const QString &userId) const
{
    // Where this account's items went, which is not this process's default
    // when the store reached the recorded backend past it.
    const QString kind =
        m_secretStore ? m_secretStore->backendKindFor(userId) : QString();
    const QString slug = slugForSavedAccount(userId);
    if (kind.isEmpty() || slug.isEmpty())
        return;
    if (m_store->value(accountKey(slug, kAccountKeyringBackend)).toString() != kind)
        m_store->setValue(accountKey(slug, kAccountKeyringBackend), kind);
}

bool SettingsManager::keyringItemsAdoptedUnverified(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId.trimmed());
    return !slug.isEmpty() && m_secretStore && m_secretStore->isSharedBetweenInstalls()
        && keyringItemsAreCurrent(slug)
        && m_store->value(accountKey(slug, kAccountKeyringItems)).toString()
               == QLatin1String(kKeyringItemsAdopted);
}

bool SettingsManager::keyringItemsDeclinedForOtherInstall(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId.trimmed());
    return !slug.isEmpty() && keyringItemsAreCurrent(slug)
        && m_store->value(accountKey(slug, kAccountKeyringItems)).toString()
               == QLatin1String(kKeyringItemsDeclined);
}

void SettingsManager::confirmAdoptedKeyringItems(const QString &userId)
{
    const QString slug = slugForSavedAccount(userId.trimmed());
    if (!slug.isEmpty() && keyringItemsAdoptedUnverified(userId))
        markKeyringItems(slug, kKeyringItemsOwn);
}

bool SettingsManager::declineAdoptedKeyringItems(const QString &userId,
                                                 const QString &checkedDeviceId)
{
    const QString uid = userId.trimmed();
    const QString slug = slugForSavedAccount(uid);
    if (slug.isEmpty() || !m_secretStore)
        return false;
    // Only the copy that was checked: a sign-in written since (marked "own",
    // or for another device) is never removed by a late answer.
    if (!keyringItemsAdoptedUnverified(uid) || checkedDeviceId.isEmpty()
        || m_store->value(accountKey(slug, kAccountDeviceId)).toString()
               != checkedDeviceId)
        return false;
    // Marked first, so a crash between the two never adopts it again.
    markKeyringItems(slug, kKeyringItemsDeclined);
    // This install's items only: in a store shared between installs that is
    // what clearAccountSecrets() reaches. The shared item is another install's
    // live sign-in and stays exactly as it is.
    const bool cleared = m_secretStore->clearAccountSecrets(uid);
    if (!cleared)
        qCWarning(lcSettings) << "keyring: could not remove this install's copy "
                                 "of another install's sign-in"
                              << "slug=" << slug;
    return cleared;
}

QString SettingsManager::keyringBackendFor(const QString &userId) const
{
    const QString slug = slugForSavedAccount(userId.trimmed());
    if (slug.isEmpty())
        return {};
    return m_store->value(accountKey(slug, kAccountKeyringBackend)).toString();
}

QString SettingsManager::keyringBackendForStore(const QString &userId) const
{
    const QString recorded = keyringBackendFor(userId);
    if (!recorded.isEmpty() || m_keyringBackendBeingCleared.first.isEmpty())
        return recorded;
    return userId.trimmed() == m_keyringBackendBeingCleared.first
               ? m_keyringBackendBeingCleared.second
               : QString();
}

bool SettingsManager::keyringBackendChanged(const QString &userId) const
{
    const QString recorded = keyringBackendFor(userId);
    const QString current = m_secretStore ? m_secretStore->backendKind() : QString();
    return !recorded.isEmpty() && !current.isEmpty() && recorded != current;
}

void SettingsManager::adoptLegacyKeyringSessions()
{
    if (!m_secretStore || !m_secretStore->isSharedBetweenInstalls())
        return;
    for (const QString &uid : savedAccountUserIds()) {
        if (keyringItemsAreCurrent(slugForSavedAccount(uid))) {
            const QString own = m_secretStore->readSecret(
                uid, QLatin1String(kSecretAccessToken));
            // Unreadable (locked keyring): the first read that succeeds adopts.
            if (m_secretStore->lastReadFailed() || !own.isEmpty())
                continue;
        }
        adoptLegacyKeyringSession(uid, nullptr);
    }
}

SettingsManager::LegacyAdoption SettingsManager::adoptLegacyKeyringSession(
    const QString &userId, QHash<QString, QString> *legacyOut) const
{
    // Only an account this install records, and never one this build signed
    // in for the device the record names: that session is its own, whatever
    // an older build left behind.
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return LegacyAdoption::NotAdoptable;
    const bool current = keyringItemsAreCurrent(slug);
    const QString how =
        m_store->value(accountKey(slug, kAccountKeyringItems)).toString();
    if (current
        && (how == QLatin1String(kKeyringItemsOwn)
            || how == QLatin1String(kKeyringItemsDeclined)))
        return LegacyAdoption::NotAdoptable;

    // The access token is copied last, so its presence among this install's
    // items means the whole session is there.
    const QLatin1String keys[] = {
        QLatin1String(kSecretRefreshToken),
        QLatin1String(kSecretOAuthClientId),
        QLatin1String(kSecretAccessToken),
    };
    QHash<QString, QString> legacy;
    const QString access = m_secretStore->readLegacySecret(
        userId, QLatin1String(kSecretAccessToken));
    if (m_secretStore->lastReadFailed())
        return LegacyAdoption::Unreadable;
    if (access.isEmpty())
        return LegacyAdoption::NotAdoptable;
    for (const QLatin1String &key : keys) {
        if (key == QLatin1String(kSecretAccessToken)) {
            legacy.insert(key, access);
            continue;
        }
        const QString value = m_secretStore->readLegacySecret(userId, key);
        if (m_secretStore->lastReadFailed())
            return LegacyAdoption::Unreadable;
        legacy.insert(key, value);
    }
    if (legacyOut)
        *legacyOut = legacy;

    // No other install's items are ever read, and the shared item is left
    // exactly as it is: an older build, or another install, may be using it.
    // This install was already reading it on every start; the copy only stops
    // later writes by others from reaching it. Nothing here tells whose device
    // the token belongs to, hence "unverified". Marked first, so a crash
    // mid-copy leaves the mark and no access token, and the next read copies
    // again.
    if (!current || how != QLatin1String(kKeyringItemsAdopted))
        markKeyringItems(slug, kKeyringItemsAdopted);
    for (const QLatin1String &key : keys) {
        if (!m_secretStore->storeSecret(userId, key, legacy.value(key))) {
            qCWarning(lcSettings)
                << "keyring: could not copy a sign-in saved before install "
                   "scoping; reading the shared item as before"
                << "slug=" << slug;
            return LegacyAdoption::CopyFailed;
        }
    }
    if (m_secretStore->readSecret(userId, QLatin1String(kSecretAccessToken))
        != access) {
        qCWarning(lcSettings)
            << "keyring: a copied sign-in did not read back; reading the "
               "shared item as before"
            << "slug=" << slug;
        return LegacyAdoption::CopyFailed;
    }
    noteKeyringBackend(userId);
    qCInfo(lcSettings)
        << "keyring: adopted a sign-in saved before install scoping"
        << "(unverified: another install on this computer may use the same "
           "item)"
        << "slug=" << slug;
    return LegacyAdoption::Adopted;
}

QString SettingsManager::readAccountSecret(const QString &userId,
                                           const char *key) const
{
    if (!m_secretStore)
        return {};
    const QLatin1String name(key);
    // Said once per account and process: this launch landed on another
    // keyring than the one that last held this sign-in (a Flatpak or snap
    // whose Secret portal came or went). The store reads the recorded one
    // when it can reach it, and otherwise cannot answer; it never reads the
    // other one as if the sign-in were gone.
    if (name == QLatin1String(kSecretAccessToken) && keyringBackendChanged(userId)
        && !m_keyringBackendChangeLogged.contains(userId)) {
        m_keyringBackendChangeLogged.insert(userId);
        const bool reachable =
            m_secretStore->backendKindFor(userId) == keyringBackendFor(userId);
        qCWarning(lcSettings)
            << "keyring: this account's sign-in was last saved in another keyring"
            << "than this launch reads"
            << "slug=" << matrix::app_data::safeUserSlug(userId)
            << "saved_in=" << keyringBackendFor(userId)
            << "reading=" << m_secretStore->backendKind()
            << (reachable ? "-- reading it where it was saved"
                          : "-- that keyring cannot be read in this launch; "
                            "the sign-in is kept, not lost");
    }
    if (!m_secretStore->isSharedBetweenInstalls())
        return m_secretStore->readSecret(userId, name);
    // No record here: nothing to restore, and no device to check against.
    const QString slug = slugForSavedAccount(userId);
    if (slug.isEmpty())
        return m_secretStore->readSecret(userId, name);

    // This install's items, when written for the device the record names. The
    // access token decides for every key.
    if (keyringItemsAreCurrent(slug)) {
        const QString access = m_secretStore->readSecret(
            userId, QLatin1String(kSecretAccessToken));
        if (m_secretStore->lastReadFailed())
            return {};
        if (!access.isEmpty()) {
            // Found where this launch reads it: record that, for a sign-in
            // last written before the backend was recorded (a password
            // account never rewrites it), so a later launch landing on the
            // other backend can still tell where it is.
            noteKeyringBackend(userId);
            return name == QLatin1String(kSecretAccessToken)
                       ? access
                       : m_secretStore->readSecret(userId, name);
        }
    }
    // None: the session, if any, is the pre-scoping one. Never for a session
    // this build signed in (see adoptLegacyKeyringSession()).
    QHash<QString, QString> legacy;
    switch (adoptLegacyKeyringSession(userId, &legacy)) {
    case LegacyAdoption::Adopted:
        return m_secretStore->readSecret(userId, name);
    case LegacyAdoption::CopyFailed:
        // A sign-in that is still there must not read as gone.
        return legacy.value(name);
    case LegacyAdoption::Unreadable:
        // lastReadFailed() is set: "cannot tell", never "no account".
    case LegacyAdoption::NotAdoptable:
        return {};
    }
    return {};
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
            // This install's own token, now in its own items.
            const QString slug = slugForSavedAccount(uid);
            if (m_secretStore->isSharedBetweenInstalls() && !slug.isEmpty())
                markKeyringItems(slug, kKeyringItemsOwn);
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
    // strand an OAuth account without its refresh token and client id, and a
    // group holding a key not listed here is kept whole, in plaintext. The
    // media store key (MediaStoreKey.h) moves too, unchanged, so the store it
    // encrypts still opens.
    const QLatin1String secretKeys[] = {
        QLatin1String(kSecretAccessToken),
        QLatin1String(kSecretRefreshToken),
        QLatin1String(kSecretOAuthClientId),
        QLatin1String(matrix::media_store_key::kSecretName),
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

        // Unknown keys (e.g. from a newer build) are not ours to delete, so
        // such a group is kept and offered again at every start.
        const bool groupKept =
            std::any_of(presentKeys.cbegin(), presentKeys.cend(),
                        [&secretKeys](const QString &present) {
                            return std::none_of(
                                std::begin(secretKeys), std::end(secretKeys),
                                [&present](const QLatin1String &key) {
                                    return present == key;
                                });
                        });
        bool allMoved = !groupKept;
        int movedHere = 0;
        for (const QLatin1String &key : secretKeys) {
            const QString plainKey = groupKey + QLatin1Char('/') + key;
            if (!m_store->contains(plainKey))
                continue;
            const QString value = m_store->value(plainKey).toString();
            if (value.isEmpty())
                continue;   // nothing to move; removing it loses nothing
            // A kept group was copied at an earlier start, so a secure value
            // that differs now was written since: a refresh token the server
            // rotated. Copying the stale one over it would end the session.
            // A group that goes after this pass was written while no secure
            // store answered, after anything that store holds, and still wins.
            if (groupKept) {
                const QString existing = m_secretStore->readSecret(uid, key);
                if (m_secretStore->lastReadFailed()) {
                    continue;
                }
                if (existing == value) {
                    ++movedHere;
                    continue;
                }
                if (!existing.isEmpty()) {
                    qCWarning(lcSettings)
                        << "Secure credential migration: a kept plaintext copy "
                           "differs from the secure store's; the secure one "
                           "stays"
                        << "key=" << key << "slug="
                        << matrix::app_data::safeUserSlug(uid);
                    continue;
                }
            }
            // Verify the read-back under the real id before deleting the
            // plaintext.
            if (m_secretStore->storeSecret(uid, key, value)
                && m_secretStore->readSecret(uid, key) == value) {
                ++movedHere;
            } else {
                allMoved = false;
            }
        }

        if (allMoved) {
            m_store->remove(groupKey);
            if (movedHere > 0)
                ++migrated;
            // This install's own session, written while no keyring answered.
            if (movedHere > 0 && m_secretStore->isSharedBetweenInstalls())
                markKeyringItems(slugForSavedAccount(uid), kKeyringItemsOwn);
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
    // Written when the slider is released (holdWritesWhileDragging) or by the
    // throttled flush, never once per step of a drag.
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
    // As setCallParticipantVolume(): one write on release.
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
    const QString global = QLatin1String(globalKey);
    const QString slug = slugForSavedAccount(activeAccountUserId());
    if (slug.isEmpty()) {
        // The signed-out shell: the global key is its own value.
        m_store->setValue(global, value);
        return;
    }
    m_store->setValue(accountKey(slug, globalKey), value);
    // The global key is also the fallback of every account without its own
    // value, so moving it restyled them all (B004: a theme picked in one
    // account showed in two others). Those accounts first keep, as their own,
    // the value they show now; then the global follows this choice, for the
    // signed-out shell and for an account added later. An account showing its
    // DEFAULT (no global value yet) cannot be pinned from here, which does not
    // know the getter's default, so the global stays unwritten while one does.
    const bool hasGlobal = m_store->contains(global);
    const QVariant inherited = hasGlobal ? m_store->value(global) : QVariant();
    bool anotherShowsTheDefault = false;
    for (const QString &userId : savedAccountUserIds()) {
        const QString other = slugForSavedAccount(userId);
        if (other.isEmpty() || other == slug)
            continue;
        const QString otherKey = accountKey(other, globalKey);
        if (m_store->contains(otherKey))
            continue;
        if (hasGlobal)
            m_store->setValue(otherKey, inherited);
        else
            anotherShowsTheDefault = true;
    }
    if (!anotherShowsTheDefault)
        m_store->setValue(global, value);
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
    m_store->remove(QStringLiteral("roomList/keepImportantInCollapsed"));
    // Raw room ids; with no accounts left there is nothing to fall back for.
    m_store->remove(QStringLiteral("notifications/room-mode"));
    m_store->remove(QStringLiteral("notifications/room-mode-unsynced"));
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
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        const int stored =
            accountScopedValue(kNhekoMessageLayout,
                               kMessageLayoutSmallAvatars).toInt();
        return (stored < 0 || stored > kMaxMessageLayout)
            ? kMessageLayoutSmallAvatars : stored;
    }
    const int stored = appearanceValue(kMessageLayout, 0).toInt();
    return (stored < 0 || stored > kMaxMessageLayout) ? 0 : stored;
}

int SettingsManager::layoutPreset() const
{
    // An unknown value (a newer build's preset) reads as Lightning.
    const int stored =
        appearanceValue(kLayoutPreset, kLayoutPresetLightning).toInt();
    return stored == kLayoutPresetNhekoCompact ? kLayoutPresetNhekoCompact
                                               : kLayoutPresetLightning;
}

void SettingsManager::setLayoutPreset(int preset)
{
    if (preset != kLayoutPresetNhekoCompact)
        preset = kLayoutPresetLightning;
    if (layoutPreset() == preset)
        return;
    // One transaction: read what every presentation key answers now, switch,
    // and announce only the keys whose answer moved.
    const int layoutBefore = messageLayout();
    const int spacingBefore = messageSpacing();
    const int placementBefore = threadPlacement();
    const int roomListBefore = roomListWidth();
    const int railBefore = spacesRailWidth();
    const int sidePanelBefore = sidePanelWidth();
    setAppearanceValue(kLayoutPreset, preset);
    Q_EMIT layoutPresetChanged();
    announcePaneWidthsIfMoved(roomListBefore, railBefore, sidePanelBefore);
    if (messageLayout() != layoutBefore)
        Q_EMIT messageLayoutChanged();
    if (messageSpacing() != spacingBefore)
        Q_EMIT messageSpacingChanged();
    if (threadPlacement() != placementBefore)
        Q_EMIT threadPlacementChanged();
}

namespace {
const char *const kNhekoOverrideKeys[] = {
    kNhekoMessageLayout, kNhekoMessageSpacing, kNhekoThreadPlacement,
};
} // namespace

bool SettingsManager::layoutPresetCustomized() const
{
    if (layoutPreset() != kLayoutPresetNhekoCompact)
        return false;
    for (const char *key : kNhekoOverrideKeys) {
        if (accountScopedValue(key, QVariant()).isValid())
            return true;
    }
    return false;
}

bool SettingsManager::presetSuppliesValue(const QString &property) const
{
    if (layoutPreset() != kLayoutPresetNhekoCompact)
        return false;
    const char *key = property == QLatin1String("messageLayout")
        ? kNhekoMessageLayout
        : property == QLatin1String("messageSpacing") ? kNhekoMessageSpacing
        : property == QLatin1String("threadPlacement") ? kNhekoThreadPlacement
        : nullptr;
    return key && !accountScopedValue(key, QVariant()).isValid();
}

QVariant SettingsManager::lightningPresentationValue(
    const QString &property) const
{
    if (property == QLatin1String("messageLayout")) {
        const int stored = appearanceValueUnannounced(kMessageLayout, 0).toInt();
        return (stored < 0 || stored > kMaxMessageLayout) ? 0 : stored;
    }
    if (property == QLatin1String("messageSpacing")) {
        const int stored =
            appearanceValueUnannounced(kMessageSpacing, kSpacingCozy).toInt();
        return (stored < kSpacingCompact || stored > kSpacingRoomy)
            ? kSpacingCozy : stored;
    }
    if (property == QLatin1String("threadPlacement")) {
        // The Lightning placement: its stored choice, else derived from the
        // thread style exactly as threadPlacement() does outside a preset.
        const QVariant stored =
            appearanceValueUnannounced(kThreadPlacement, QVariant());
        bool ok = false;
        const int value = stored.isValid() ? stored.toInt(&ok) : -1;
        if (ok && (value == kThreadPlacementSide
                   || value == kThreadPlacementMain))
            return value;
        return threadStyle() == kThreadStyleInline ? kThreadPlacementMain
                                                   : kThreadPlacementSide;
    }
    // Pane widths: the Lightning preset's (device-local) width.
    if (property == QLatin1String("roomListWidth"))
        return std::clamp(m_store->value(kRoomListWidth, 300).toInt(),
                          kRoomListMinWidth, kRoomListMaxWidth);
    if (property == QLatin1String("spacesRailWidth"))
        return std::clamp(m_store->value(kSpacesRailWidth,
                                         kSpacesRailMinWidth).toInt(),
                          kSpacesRailMinWidth, kSpacesRailMaxWidth);
    if (property == QLatin1String("sidePanelWidth"))
        return std::clamp(m_store->value(kSidePanelWidth, 320).toInt(),
                          kSidePanelMinWidth, kSidePanelMaxWidth);
    return {};
}

bool SettingsManager::setLightningPresentationValue(const QString &property,
                                                    const QVariant &value)
{
    if (layoutPreset() != kLayoutPresetNhekoCompact) {
        // The Lightning values ARE the ordinary setters' values.
        if (property == QLatin1String("messageLayout")) {
            setMessageLayout(value.toInt());
            return true;
        }
        if (property == QLatin1String("messageSpacing")) {
            setMessageSpacing(value.toInt());
            return true;
        }
        if (property == QLatin1String("threadPlacement")) {
            setThreadPlacement(value.toInt());
            return true;
        }
        if (property == QLatin1String("roomListWidth")) {
            setRoomListWidth(value.toInt());
            return true;
        }
        if (property == QLatin1String("spacesRailWidth")) {
            setSpacesRailWidth(value.toInt());
            return true;
        }
        if (property == QLatin1String("sidePanelWidth")) {
            setSidePanelWidth(value.toInt());
            return true;
        }
        return false;
    }
    // Nheko compact is selected: store the Lightning value for later and
    // leave the visible (preset) value alone.
    if (property == QLatin1String("messageLayout")) {
        int layout = value.toInt();
        if (layout < 0 || layout > kMaxMessageLayout)
            layout = 0;
        setAppearanceValueUnannounced(kMessageLayout, layout);
        return true;
    }
    if (property == QLatin1String("messageSpacing")) {
        int mode = value.toInt();
        if (mode < kSpacingCompact || mode > kSpacingRoomy)
            mode = kSpacingCozy;
        setAppearanceValueUnannounced(kMessageSpacing, mode);
        return true;
    }
    if (property == QLatin1String("threadPlacement")) {
        const int placement = value.toInt() == kThreadPlacementMain
            ? kThreadPlacementMain : kThreadPlacementSide;
        setAppearanceValueUnannounced(kThreadPlacement, placement);
        return true;
    }
    if (property == QLatin1String("roomListWidth")) {
        m_store->setValue(kRoomListWidth,
                          std::clamp(value.toInt(), kRoomListMinWidth,
                                     kRoomListMaxWidth));
        return true;
    }
    if (property == QLatin1String("spacesRailWidth")) {
        m_store->setValue(kSpacesRailWidth,
                          std::clamp(value.toInt(), kSpacesRailMinWidth,
                                     kSpacesRailMaxWidth));
        return true;
    }
    if (property == QLatin1String("sidePanelWidth")) {
        m_store->setValue(kSidePanelWidth,
                          std::clamp(value.toInt(), kSidePanelMinWidth,
                                     kSidePanelMaxWidth));
        return true;
    }
    return false;
}

void SettingsManager::resetLayoutPresetOverrides()
{
    if (layoutPreset() != kLayoutPresetNhekoCompact)
        return;
    const int layoutBefore = messageLayout();
    const int spacingBefore = messageSpacing();
    const int placementBefore = threadPlacement();
    const bool wasCustomized = layoutPresetCustomized();
    const QString slug = slugForSavedAccount(activeAccountUserId());
    for (const char *key : kNhekoOverrideKeys) {
        m_store->remove(slug.isEmpty() ? QString::fromLatin1(key)
                                       : accountKey(slug, key));
    }
    if (wasCustomized)
        Q_EMIT layoutPresetChanged();
    if (messageLayout() != layoutBefore)
        Q_EMIT messageLayoutChanged();
    if (messageSpacing() != spacingBefore)
        Q_EMIT messageSpacingChanged();
    if (threadPlacement() != placementBefore)
        Q_EMIT threadPlacementChanged();
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
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        // The profile's override; the Lightning layout stays as it was.
        const bool wasCustomized = layoutPresetCustomized();
        const bool moved = messageLayout() != layout;
        setAccountScopedValue(kNhekoMessageLayout, layout);
        if (!wasCustomized)
            Q_EMIT layoutPresetChanged();
        if (moved)
            Q_EMIT messageLayoutChanged();
        return;
    }
    if (messageLayout() == layout)
        return;
    setAppearanceValue(kMessageLayout, layout);
    Q_EMIT messageLayoutChanged();
}

int SettingsManager::roomFilterMode() const
{
    // 0 All, 1 People, 2 Rooms, 3 Unreads, 4 Favourites (RoomListModel::filterMode).
    const int stored = appearanceValue(kRoomFilterMode, 0).toInt();
    return (stored < 0 || stored > 4) ? 0 : stored;
}

void SettingsManager::setRoomFilterMode(int mode)
{
    if (mode < 0 || mode > 4)
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
    bool ok = false;
    const int mode =
        m_store->value(QStringLiteral("notifications/preview"), 0).toInt(&ok);
    if (!ok)
        return 1;   // not a number: least disclosure, like any unknown mode
    // An unrecognised stored value most likely names a mode a newer build
    // offered; we cannot honour it, so err toward LESS disclosure (1, sender
    // only), matching the setter. The absent-key default above stays 0.
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

QString SettingsManager::noiseSuppressionMode() const
{
    const QByteArray stored =
        m_store->value(QStringLiteral("calls/noiseSuppression"))
            .toString()
            .toUtf8();
    // A fallback, never a clamp: an unknown key is not "the nearest mode".
    const calls::noise::Mode mode = calls::noise::modeFromKey(
        std::string_view(stored.constData(), size_t(stored.size())),
        calls::noise::kDefaultMode);
    return QString::fromLatin1(calls::noise::modeKey(mode));
}

void SettingsManager::setNoiseSuppressionMode(const QString &mode)
{
    const QByteArray key = mode.toUtf8();
    if (!calls::noise::isModeKey(
            std::string_view(key.constData(), size_t(key.size()))))
        return;
    if (noiseSuppressionMode() == mode)
        return;
    m_store->setValue(QStringLiteral("calls/noiseSuppression"), mode);
    Q_EMIT noiseSuppressionModeChanged();
}

int SettingsManager::callShareAudioMode() const
{
    bool ok = false;
    const int stored =
        m_store->value(QStringLiteral("calls/shareAudioMode"), 1).toInt(&ok);
    // A fallback, never a clamp (2 is not "the nearest" of 7).
    return (ok && stored >= 0 && stored <= 2) ? stored : 1;
}

void SettingsManager::setCallShareAudioMode(int mode)
{
    if (mode < 0 || mode > 2)
        return;
    if (callShareAudioMode() == mode)
        return;
    m_store->setValue(QStringLiteral("calls/shareAudioMode"), mode);
}

QStringList SettingsManager::callShareAudioApps() const
{
    QStringList out;
    for (const QString &key :
         m_store->value(QStringLiteral("calls/shareAudioApps")).toStringList()) {
        const QString k = key.trimmed().toLower();
        if (!k.isEmpty() && !out.contains(k))
            out.append(k);
    }
    return out;
}

void SettingsManager::setCallShareAudioApps(const QStringList &keys)
{
    QStringList clean;
    for (const QString &key : keys) {
        const QString k = key.trimmed().toLower();
        if (!k.isEmpty() && !clean.contains(k))
            clean.append(k);
    }
    if (callShareAudioApps() == clean)
        return;
    m_store->setValue(QStringLiteral("calls/shareAudioApps"), clean);
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

bool SettingsManager::labsConcurrentAccounts() const
{
    return storedTrue(m_store->value(QLatin1String(kLabsConcurrentAccounts)));
}

void SettingsManager::setLabsConcurrentAccounts(bool on)
{
    if (labsConcurrentAccounts() == on)
        return;
    m_store->setValue(QLatin1String(kLabsConcurrentAccounts), on);
    Q_EMIT labsConcurrentAccountsChanged();
}

bool SettingsManager::labsConcurrentAccountsWarmSlot() const
{
    // On by default (DECISION 3); only a stored bool false turns it off, read
    // with the same strictness as the switch above.
    const QVariant v =
        m_store->value(QLatin1String(kLabsConcurrentAccountsWarmSlot));
    if (!v.isValid())
        return true;
    if (v.metaType().id() == QMetaType::Bool)
        return v.toBool();
    return !(v.metaType().id() == QMetaType::QString
             && v.toString() == QLatin1String("false"));
}

void SettingsManager::setLabsConcurrentAccountsWarmSlot(bool on)
{
    if (labsConcurrentAccountsWarmSlot() == on)
        return;
    m_store->setValue(QLatin1String(kLabsConcurrentAccountsWarmSlot), on);
    Q_EMIT labsConcurrentAccountsWarmSlotChanged();
}

bool SettingsManager::developerTools() const
{
    return m_store->value(QStringLiteral("about/developerTools"), false).toBool();
}

void SettingsManager::setDeveloperTools(bool v)
{
    if (developerTools() == v)
        return;
    m_store->setValue(QStringLiteral("about/developerTools"), v);
    Q_EMIT developerToolsChanged();
}

bool SettingsManager::keepMediaOnDevice() const
{
    return m_store->value(QStringLiteral("privacy/keepMediaOnDevice"), true)
        .toBool();
}

void SettingsManager::setKeepMediaOnDevice(bool v)
{
    if (keepMediaOnDevice() == v)
        return;
    m_store->setValue(QStringLiteral("privacy/keepMediaOnDevice"), v);
    Q_EMIT keepMediaOnDeviceChanged();
}

int SettingsManager::readReceiptMode() const
{
    // 0 = public.
    const int mode =
        m_store->value(QStringLiteral("privacy/readReceiptMode"), 0).toInt();
    if (mode == 2)
        return 1;   // the old "Do not send" mode: identical on the wire
    return (mode < 0 || mode > 1) ? 0 : mode;
}

void SettingsManager::setReadReceiptMode(int mode)
{
    // Decided 2026-10-18: two modes, as Element, Element X and Signal offer
    // (one "send read receipts" switch whose off state still sends private
    // receipts). The former third mode sent m.read.private exactly like
    // Private, so it folds into it.
    if (mode == 2)
        mode = 1;
    if (mode < 0 || mode > 1)
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

int SettingsManager::notificationSoundSource() const
{
    // 0 = Lightning's chime. Anything unknown (a newer build's third source)
    // reads as the default, not as the last one.
    return m_store->value(QStringLiteral("notifications/soundSource"), 0)
                   .toInt() == 1 ? 1 : 0;
}

void SettingsManager::setNotificationSoundSource(int source)
{
    source = source == 1 ? 1 : 0;
    if (notificationSoundSource() == source)
        return;
    m_store->setValue(QStringLiteral("notifications/soundSource"), source);
    Q_EMIT notificationSoundSourceChanged();
}

int SettingsManager::notificationSoundVolume() const
{
    bool ok = false;
    const int v = m_store->value(QStringLiteral("notifications/soundVolume"),
                                 kDefaultNotificationSoundVolume).toInt(&ok);
    return (!ok || v < 0 || v > 100) ? kDefaultNotificationSoundVolume : v;
}

void SettingsManager::setNotificationSoundVolume(int percent)
{
    const int clamped = std::clamp(percent, 0, 100);
    if (notificationSoundVolume() == clamped)
        return;
    m_store->setValue(QStringLiteral("notifications/soundVolume"), clamped);
    Q_EMIT notificationSoundVolumeChanged();
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

QString SettingsManager::preferredCameraDescription() const
{
    return sanitizedDeviceId(
        m_store->value(QStringLiteral("calls/cameraDescription")).toString());
}

void SettingsManager::setPreferredCameraDescription(const QString &description)
{
    // No change signal: the id setter that follows announces the pair.
    m_store->setValue(QStringLiteral("calls/cameraDescription"),
                      sanitizedDeviceId(description.simplified()));
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

// The *For(uid, ...) reads resolve the foreground account through the same
// cache as the active accessors, so for the active uid both answer from one
// slug by construction; any other uid reads its saved record.
QString SettingsManager::slugForAccountScope(const QString &userId) const
{
    if (!userId.isEmpty() && userId == activeAccountUserId())
        return activeAccountSlugCached();
    return slugForSavedAccount(userId);
}

QString SettingsManager::scopedKeyFor(const QString &slug, const QString &roomId)
{
    if (slug.isEmpty())
        return {};
    return QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1Char('/') + roomNotificationModeGlobalKey(roomId);
}

QString SettingsManager::scopePrefixFor(const QString &slug)
{
    if (slug.isEmpty())
        return {};
    return QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1Char('/');
}

QString SettingsManager::roomNotificationModeScopedKey(const QString &roomId) const
{
    return scopedKeyFor(activeAccountSlugCached(), roomId);
}

QString SettingsManager::roomNotificationScopePrefix() const
{
    return scopePrefixFor(activeAccountSlugCached());
}

// The readers below exist twice: the active accessors (thin wrappers with the
// cached active slug, the hot path) and the *For(userId) versions a host for
// a non-active account reads through (slugForSavedAccount, uncached). Both
// call one *InScope core, so they cannot disagree for the active account.
bool SettingsManager::roomNotificationModeUnsyncedInScope(
    const QString &slug, const QString &roomId) const
{
    if (roomId.isEmpty())
        return false;
    return m_store->contains(
        scopePrefixFor(slug)
        + QStringLiteral("notifications/room-mode-unsynced/") + roomId);
}

bool SettingsManager::roomNotificationModeUnsynced(const QString &roomId) const
{
    return roomNotificationModeUnsyncedInScope(activeAccountSlugCached(), roomId);
}

bool SettingsManager::roomNotificationModeUnsyncedFor(const QString &userId,
                                                      const QString &roomId) const
{
    return roomNotificationModeUnsyncedInScope(slugForAccountScope(userId),
                                               roomId);
}

int SettingsManager::roomNotificationModeUnsyncedBaseInScope(
    const QString &slug, const QString &roomId) const
{
    if (roomId.isEmpty())
        return -1;
    bool ok = false;
    const int base = m_store
        ->value(scopePrefixFor(slug)
                    + QStringLiteral("notifications/room-mode-unsynced/")
                    + roomId)
        .toInt(&ok);
    return (ok && base >= 0 && base <= 3) ? base : -1;
}

int SettingsManager::roomNotificationModeUnsyncedBase(const QString &roomId) const
{
    return roomNotificationModeUnsyncedBaseInScope(activeAccountSlugCached(),
                                                   roomId);
}

int SettingsManager::roomNotificationModeUnsyncedBaseFor(
    const QString &userId, const QString &roomId) const
{
    return roomNotificationModeUnsyncedBaseInScope(slugForAccountScope(userId),
                                                   roomId);
}

void SettingsManager::setRoomNotificationModeUnsynced(const QString &roomId,
                                                      bool unsynced,
                                                      int serverBase)
{
    if (roomId.isEmpty())
        return;
    const QString key = roomNotificationScopePrefix()
        + QStringLiteral("notifications/room-mode-unsynced/") + roomId;
    const bool wasUnsynced = m_store->contains(key);
    const int oldBase = roomNotificationModeUnsyncedBase(roomId);
    if (unsynced)
        m_store->setValue(key, (serverBase >= 0 && serverBase <= 3) ? serverBase : -1);
    else
        m_store->remove(key);
    // roomUnreadMode() reads this flag, so a surface that counts unread
    // (the tray) must hear about a change in it.
    if (wasUnsynced != unsynced
        || (unsynced && oldBase != ((serverBase >= 0 && serverBase <= 3) ? serverBase : -1)))
        Q_EMIT roomNotificationModeChanged(roomId);
}

QStringList SettingsManager::unsyncedRoomNotificationModes() const
{
    m_store->beginGroup(roomNotificationScopePrefix()
                        + QStringLiteral("notifications/room-mode-unsynced"));
    const QStringList rooms = m_store->childKeys();
    m_store->endGroup();
    return rooms;
}

bool SettingsManager::roomNotificationModeDeviceOnlyInScope(
    const QString &slug, const QString &roomId) const
{
    if (roomId.isEmpty())
        return false;
    // The account's own key, even mode 0 shadowing a legacy value, is a
    // choice made since modes were saved to the server.
    const QString scopedKey = scopedKeyFor(slug, roomId);
    if (!scopedKey.isEmpty() && m_store->contains(scopedKey))
        return false;
    const int legacy =
        m_store->value(roomNotificationModeGlobalKey(roomId), 0).toInt();
    return legacy == 1 || legacy == 2;
}

bool SettingsManager::roomNotificationModeDeviceOnly(const QString &roomId) const
{
    return roomNotificationModeDeviceOnlyInScope(activeAccountSlugCached(),
                                                 roomId);
}

bool SettingsManager::roomNotificationModeDeviceOnlyFor(
    const QString &userId, const QString &roomId) const
{
    return roomNotificationModeDeviceOnlyInScope(slugForAccountScope(userId),
                                                 roomId);
}

bool SettingsManager::roomNotificationModeDecidesLocallyFor(
    const QString &userId, const QString &roomId) const
{
    // AppController::roomNotificationModeDecidesLocally's rule, in its order.
    const QString slug = slugForAccountScope(userId);
    return roomNotificationModeUnsyncedInScope(slug, roomId)
        || roomNotificationModeDeviceOnlyInScope(slug, roomId);
}

QStringList SettingsManager::accountRoomNotificationModeRooms() const
{
    const QString prefix = roomNotificationScopePrefix();
    if (prefix.isEmpty())
        return {};
    m_store->beginGroup(prefix + QStringLiteral("notifications/room-mode"));
    const QStringList rooms = m_store->childKeys();
    m_store->endGroup();
    return rooms;
}

bool SettingsManager::hasStoredRoomNotificationModeInScope(
    const QString &slug, const QString &roomId) const
{
    if (roomId.isEmpty())
        return false;
    const QString scopedKey = scopedKeyFor(slug, roomId);
    return (!scopedKey.isEmpty() && m_store->contains(scopedKey))
        || m_store->contains(roomNotificationModeGlobalKey(roomId));
}

bool SettingsManager::hasStoredRoomNotificationMode(const QString &roomId) const
{
    return hasStoredRoomNotificationModeInScope(activeAccountSlugCached(),
                                                roomId);
}

bool SettingsManager::hasStoredRoomNotificationModeFor(
    const QString &userId, const QString &roomId) const
{
    return hasStoredRoomNotificationModeInScope(slugForAccountScope(userId),
                                                roomId);
}

int SettingsManager::roomNotificationModeInScope(const QString &slug,
                                                 const QString &roomId) const
{
    if (roomId.isEmpty())
        return 0;
    const QString scopedKey = scopedKeyFor(slug, roomId);
    const QString readKey = (!scopedKey.isEmpty() && m_store->contains(scopedKey))
        ? scopedKey
        : roomNotificationModeGlobalKey(roomId);
    const int mode = m_store->value(readKey, 0).toInt();
    return (mode < 0 || mode > 3) ? 0 : mode;
}

int SettingsManager::roomNotificationMode(const QString &roomId) const
{
    return roomNotificationModeInScope(activeAccountSlugCached(), roomId);
}

int SettingsManager::roomNotificationModeFor(const QString &userId,
                                             const QString &roomId) const
{
    return roomNotificationModeInScope(slugForAccountScope(userId), roomId);
}

int SettingsManager::roomUnreadModeInScope(const QString &slug,
                                           const QString &roomId) const
{
    const int mode = roomNotificationModeInScope(slug, roomId);
    if (mode == roomunread::kModeMentions
        && (roomNotificationModeDeviceOnlyInScope(slug, roomId)
            || roomNotificationModeUnsyncedInScope(slug, roomId)))
        return roomunread::kModeMentionsUnverified;
    return mode;
}

int SettingsManager::roomUnreadMode(const QString &roomId) const
{
    return roomUnreadModeInScope(activeAccountSlugCached(), roomId);
}

int SettingsManager::roomUnreadModeFor(const QString &userId,
                                       const QString &roomId) const
{
    return roomUnreadModeInScope(slugForAccountScope(userId), roomId);
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
    // Saved on every room switch and after every typing pause: the same draft
    // that is already the most recent entry is not a change, and any setValue
    // rewrites the whole settings file.
    if (!draft.isEmpty() && !index.isEmpty() && index.constLast() == hash
        && m_store->value(base + hash).toMap() == draft)
        return;
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
    // Already the most recent: nothing to reorder or evict.
    if (!index.isEmpty() && index.constLast() == hash)
        return;
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
    // Learned again on every fetch of the same media; a value already stored
    // is not written again (any setValue rewrites the whole settings file).
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/media/payload-size/") + hash;
    if (m_store->value(key).toLongLong() != bytes)
        m_store->setValue(key, bytes);
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
    // Learned again from every poster of the same video; see
    // setKnownMediaSizeBytes.
    const QString key = QLatin1String(kAccountsGroup) + QLatin1Char('/') + slug
        + QLatin1String("/media/video-dims/") + hash;
    if (m_store->value(key).toSize() != QSize(width, height))
        m_store->setValue(key, QSize(width, height));
    touchMediaInfoIndex(slug, hash);
}

namespace {
// Whitelist of picker ids that may reach the store, so QML cannot compose
// arbitrary keys. "picker" is the id the composer's GIF and emoji pickers
// share; "reaction" is the reaction picker's own (a share of the whole window,
// not of the composer card, so it cannot share theirs).
bool isKnownPickerId(const QString &id)
{
    return id == QLatin1String("picker") || id == QLatin1String("gif")
        || id == QLatin1String("emoji") || id == QLatin1String("reaction");
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
    const QString globalKey = roomNotificationModeGlobalKey(roomId);
    const QString scopedKey = roomNotificationModeScopedKey(roomId);
    // A value equal to a legacy device-global one still gets the account's
    // own key, or the room would stay device-only after an explicit choice.
    const bool onlyLegacyHoldsIt = !scopedKey.isEmpty()
        && !m_store->contains(scopedKey) && m_store->contains(globalKey);
    if (roomNotificationMode(roomId) == mode && !onlyLegacyHoldsIt)
        return;
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
    if (v == 0) // Auto
        return 0;
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
    if (v == 0) // Auto
        return 0;
    if (v <= 22)
        return 15;
    if (v <= 45)
        return 30;
    return 60;
}
} // namespace

int SettingsManager::shareMaxHeight() const
{
    // Default Auto, which resolves to 1080.
    return snapShareHeight(m_store->value(kShareMaxHeight, 0).toInt());
}

int SettingsManager::effectiveShareMaxHeight() const
{
    const int h = shareMaxHeight();
    return h == 0 ? 1080 : h;
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
    return shareQualityDemandingAt(effectiveShareMaxHeight(),
                                   effectiveShareFps());
}

bool SettingsManager::shareQualityDemandingAt(int maxHeight, int fps) const
{
    // Windows GDI capture sustains 30 fps but not 60 at high resolutions (the
    // rest is duplicated frames). Warn for 2160p at any rate and 1440p at 60.
    // Auto (0) is 1080p30 and never warns.
    if (maxHeight >= 2160)
        return true;
    return maxHeight >= 1440 && fps >= 60;
}

int SettingsManager::shareFps() const
{
    return snapShareFps(m_store->value(kShareFps, 0).toInt());
}

int SettingsManager::effectiveShareFps() const
{
    const int f = shareFps();
    return f == 0 ? 30 : f;
}

namespace {
int snapShareBitrateCapKbps(int v)
{
    if (v <= 0)
        return 0; // Auto
    static const int offered[] = { 500, 1000, 2000, 3000, 4500 };
    int best = offered[0];
    for (int o : offered) {
        if (qAbs(o - v) < qAbs(best - v))
            best = o;
    }
    return best;
}
int snapCameraResolution(int v)
{
    // Anything that is not an offered ceiling is Auto, never "the nearest":
    // the stored value reaches a caps string.
    return (v == 360 || v == 480 || v == 720 || v == 1080) ? v : 0;
}
} // namespace

int SettingsManager::shareBitrateCapKbps() const
{
    return snapShareBitrateCapKbps(m_store->value(kShareBitrateCap, 0).toInt());
}

void SettingsManager::setShareBitrateCapKbps(int v)
{
    const int snapped = snapShareBitrateCapKbps(v);
    if (shareBitrateCapKbps() == snapped)
        return;
    m_store->setValue(kShareBitrateCap, snapped);
    Q_EMIT shareQualityChanged();
}

int SettingsManager::cameraResolution() const
{
    return snapCameraResolution(m_store->value(kCameraResolution, 0).toInt());
}

void SettingsManager::setCameraResolution(int v)
{
    const int snapped = snapCameraResolution(v);
    if (cameraResolution() == snapped)
        return;
    m_store->setValue(kCameraResolution, snapped);
    Q_EMIT cameraQualityChanged();
}

int SettingsManager::effectiveCameraResolution() const
{
    const int r = cameraResolution();
    return r == 0 ? 720 : r;
}

bool SettingsManager::echoCancellation() const
{
    return m_store->value(kEchoCancellation, false).toBool();
}

void SettingsManager::setEchoCancellation(bool v)
{
    if (echoCancellation() == v)
        return;
    m_store->setValue(kEchoCancellation, v);
    Q_EMIT voiceProcessingChanged();
}

bool SettingsManager::autoGainControl() const
{
    return m_store->value(kAutoGainControl, true).toBool();
}

void SettingsManager::setAutoGainControl(bool v)
{
    if (autoGainControl() == v)
        return;
    m_store->setValue(kAutoGainControl, v);
    Q_EMIT voiceProcessingChanged();
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

bool SettingsManager::showLinkMediaInline() const
{
    // On: it only changes how a preview the reader already allowed is shown,
    // and what Play or the viewer may then fetch. Previews themselves stay off
    // by default (docs/privacy.md).
    return m_store->value(kPreviewsInlineMedia, true).toBool();
}

void SettingsManager::setShowLinkMediaInline(bool v)
{
    if (showLinkMediaInline() == v)
        return;
    m_store->setValue(kPreviewsInlineMedia, v);
    Q_EMIT showLinkMediaInlineChanged();
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

namespace {
// Trimmed, de-duplicated (case-insensitively), bounded: a list a human edits.
QStringList cleanGameList(const QStringList &in)
{
    QStringList out;
    for (const QString &raw : in) {
        const QString v = raw.simplified().left(120);
        if (v.isEmpty() || out.size() >= 200)
            continue;
        bool dup = false;
        for (const QString &o : out)
            dup = dup || o.compare(v, Qt::CaseInsensitive) == 0;
        if (!dup)
            out.append(v);
    }
    return out;
}
}   // namespace

bool SettingsManager::gameActivityEnabled() const
{
    // Experimental and off: a game name is something the user must choose to
    // tell other people.
    return m_store->value(QStringLiteral("labs/gameActivity"), false).toBool();
}

void SettingsManager::setGameActivityEnabled(bool v)
{
    if (gameActivityEnabled() == v)
        return;
    m_store->setValue(QStringLiteral("labs/gameActivity"), v);
    Q_EMIT gameActivityChanged();
}

QStringList SettingsManager::gameActivityHidden() const
{
    return cleanGameList(
        m_store->value(QStringLiteral("labs/gameActivityHidden")).toStringList());
}

void SettingsManager::setGameActivityHidden(const QStringList &names)
{
    const QStringList clean = cleanGameList(names);
    if (gameActivityHidden() == clean)
        return;
    m_store->setValue(QStringLiteral("labs/gameActivityHidden"), clean);
    Q_EMIT gameActivityChanged();
}

QStringList SettingsManager::gameActivityCustom() const
{
    return cleanGameList(
        m_store->value(QStringLiteral("labs/gameActivityCustom")).toStringList());
}

void SettingsManager::setGameActivityCustom(const QStringList &entries)
{
    const QStringList clean = cleanGameList(entries);
    if (gameActivityCustom() == clean)
        return;
    m_store->setValue(QStringLiteral("labs/gameActivityCustom"), clean);
    Q_EMIT gameActivityChanged();
}

bool SettingsManager::musicActivityEnabled() const
{
    // Experimental and off, like the game: what someone listens to is theirs
    // to choose to tell.
    return m_store->value(QStringLiteral("labs/musicActivity"), false).toBool();
}

void SettingsManager::setMusicActivityEnabled(bool v)
{
    if (musicActivityEnabled() == v)
        return;
    m_store->setValue(QStringLiteral("labs/musicActivity"), v);
    Q_EMIT musicActivityChanged();
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

bool SettingsManager::keepRoomListOrderStill() const
{
    // On by default, so a row does not slide away from under the pointer.
    return m_store->value(kKeepRoomListOrderStill, true).toBool();
}

void SettingsManager::setKeepRoomListOrderStill(bool v)
{
    if (keepRoomListOrderStill() == v)
        return;
    m_store->setValue(kKeepRoomListOrderStill, v);
    Q_EMIT keepRoomListOrderStillChanged();
}

int SettingsManager::roomListSort() const
{
    // An unknown value (hand-edited, or a mode a newer build added) is the
    // default, never the nearest mode.
    return conversation::normalizedSortMode(
        m_store->value(kRoomListSort, conversation::SortByActivity).toInt());
}

bool SettingsManager::roomListSortChosen() const
{
    return m_store->contains(kRoomListSort);
}

int SettingsManager::roomListSortForModels() const
{
    return roomListSortChosen() ? roomListSort() : -1;
}

void SettingsManager::setRoomListSort(int mode)
{
    const int valid = conversation::normalizedSortMode(mode);
    // An explicit choice is stored even when it equals the default: picking
    // Activity inside a Space must keep Activity there.
    if (roomListSortChosen() && roomListSort() == valid)
        return;
    m_store->setValue(kRoomListSort, valid);
    Q_EMIT roomListSortChanged();
}

int SettingsManager::railSort() const
{
    return conversation::normalizedRailSort(
        m_store->value(kRailSort, conversation::RailSortManual).toInt());
}

void SettingsManager::setRailSort(int mode)
{
    const int valid = conversation::normalizedRailSort(mode);
    if (railSort() == valid)
        return;
    m_store->setValue(kRailSort, valid);
    Q_EMIT railSortChanged();
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

bool SettingsManager::memberPanelOpen() const
{
    return m_store->value(kMemberPanelOpen, false).toBool();
}

void SettingsManager::setMemberPanelOpen(bool v)
{
    if (memberPanelOpen() == v)
        return;
    m_store->setValue(kMemberPanelOpen, v);
    Q_EMIT memberPanelOpenChanged();
}

QString SettingsManager::paneWidthKey(const char *key) const
{
    // Pane widths are device-local and remembered PER LAYOUT PRESET: a width
    // dragged under Nheko compact is that preset's, and the Lightning width
    // comes back with the Lightning preset. A preset with no width of its own
    // yet starts from the Lightning one, except the compact rail's own narrow
    // default (see paneWidthValue).
    if (layoutPreset() == kLayoutPresetNhekoCompact)
        return QStringLiteral("nhekoCompactWidths/") + QLatin1String(key);
    return QLatin1String(key);
}

QVariant SettingsManager::paneWidthValue(const char *key, int fallback) const
{
    const QVariant lightning = m_store->value(QLatin1String(key), fallback);
    if (layoutPreset() == kLayoutPresetNhekoCompact
        && QLatin1String(key) == QLatin1String(kSpacesRailWidth))
        return m_store->value(paneWidthKey(key), 66);
    return m_store->value(paneWidthKey(key), lightning);
}

void SettingsManager::announcePaneWidthsIfMoved(int roomList, int rail,
                                                int sidePanel)
{
    if (roomListWidth() != roomList)
        Q_EMIT roomListWidthChanged();
    if (spacesRailWidth() != rail)
        Q_EMIT spacesRailWidthChanged();
    if (sidePanelWidth() != sidePanel)
        Q_EMIT sidePanelWidthChanged();
}

int SettingsManager::roomListWidth() const
{
    // Keep imported/hand-edited values out of the unusable gap between the
    // icon column and the invite row's measured 212 px floor.
    const int stored = paneWidthValue(kRoomListWidth, 300).toInt();
    return stored < kRoomListSnapThreshold ? kRoomListMinWidth
                        : std::clamp(stored, kRoomListExpandedMinWidth,
                                     kRoomListMaxWidth);
}

void SettingsManager::setRoomListWidth(int px)
{
    const int clamped = px < kRoomListSnapThreshold ? kRoomListMinWidth
                        : std::clamp(px, kRoomListExpandedMinWidth,
                                     kRoomListMaxWidth);
    if (roomListWidth() == clamped)
        return;
    m_store->setValue(paneWidthKey(kRoomListWidth), clamped);
    Q_EMIT roomListWidthChanged();
}

int SettingsManager::roomListExpandedWidth() const
{
    return std::clamp(paneWidthValue(kRoomListExpandedWidth, 300).toInt(),
                      kRoomListExpandedMinWidth, kRoomListMaxWidth);
}

void SettingsManager::setRoomListExpandedWidth(int px)
{
    m_store->setValue(paneWidthKey(kRoomListExpandedWidth),
                      std::clamp(px, kRoomListExpandedMinWidth,
                                 kRoomListMaxWidth));
}

QUrl SettingsManager::lastAttachFolder() const
{
    const QString dir = m_store->value(kLastAttachFolder).toString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        return {};
    return QUrl::fromLocalFile(dir);
}

void SettingsManager::rememberAttachFolder(const QUrl &file)
{
    if (!file.isLocalFile())
        return;
    const QString dir = QFileInfo(file.toLocalFile()).absolutePath();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()
        || m_store->value(kLastAttachFolder).toString() == dir)
        return;
    m_store->setValue(kLastAttachFolder, dir);
}

QUrl SettingsManager::lastFileFolder(const QString &purpose) const
{
    if (purpose.isEmpty())
        return {};
    if (purpose == QLatin1String("attach"))
        return lastAttachFolder();
    const QString dir =
        m_store->value(QStringLiteral("shell/lastFolder/") + purpose).toString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        return {};
    return QUrl::fromLocalFile(dir);
}

void SettingsManager::rememberFileFolder(const QString &purpose,
                                         const QUrl &folder)
{
    if (purpose.isEmpty() || !folder.isLocalFile())
        return;
    const QString dir = QFileInfo(folder.toLocalFile()).absoluteFilePath();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        return;
    const QString key = purpose == QLatin1String("attach")
        ? QString::fromLatin1(kLastAttachFolder)
        : QStringLiteral("shell/lastFolder/") + purpose;
    if (m_store->value(key).toString() == dir)
        return;
    m_store->setValue(key, dir);
}

bool SettingsManager::alwaysAskWhereToSave() const
{
    return m_store->value(QStringLiteral("downloads/alwaysAsk"), false)
        .toBool();
}

void SettingsManager::setAlwaysAskWhereToSave(bool v)
{
    if (alwaysAskWhereToSave() == v)
        return;
    m_store->setValue(QStringLiteral("downloads/alwaysAsk"), v);
    Q_EMIT alwaysAskWhereToSaveChanged();
}

QString SettingsManager::downloadFolder() const
{
    return m_store->value(QStringLiteral("downloads/folder")).toString();
}

void SettingsManager::setDownloadFolder(const QString &path)
{
    if (downloadFolder() == path)
        return;
    if (path.isEmpty())
        m_store->remove(QStringLiteral("downloads/folder"));
    else
        m_store->setValue(QStringLiteral("downloads/folder"), path);
    Q_EMIT downloadFolderChanged();
}

int SettingsManager::spacesRailWidth() const
{
    // Defaults to the minimum. Clamped on read like roomListWidth.
    const int stored = paneWidthValue(kSpacesRailWidth,
                                      kSpacesRailMinWidth).toInt();
    const int minimum = layoutPreset() == kLayoutPresetNhekoCompact
        ? kCompactSpacesRailMinWidth : kSpacesRailMinWidth;
    return std::clamp(stored, minimum, kSpacesRailMaxWidth);
}

void SettingsManager::setSpacesRailWidth(int px)
{
    const int clamped =
        std::clamp(px, layoutPreset() == kLayoutPresetNhekoCompact
                           ? kCompactSpacesRailMinWidth : kSpacesRailMinWidth,
                   kSpacesRailMaxWidth);
    if (spacesRailWidth() == clamped)
        return;
    m_store->setValue(paneWidthKey(kSpacesRailWidth), clamped);
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
    const int stored = paneWidthValue(kSidePanelWidth, 320).toInt();
    return std::clamp(stored, kSidePanelMinWidth, kSidePanelMaxWidth);
}

void SettingsManager::setSidePanelWidth(int px)
{
    const int clamped = std::clamp(px, kSidePanelMinWidth, kSidePanelMaxWidth);
    if (sidePanelWidth() == clamped)
        return;
    m_store->setValue(paneWidthKey(kSidePanelWidth), clamped);
    Q_EMIT sidePanelWidthChanged();
}

bool SettingsManager::globalShortcutsEnabled() const
{
    return m_store->value(kGlobalShortcuts, false).toBool();
}

void SettingsManager::setGlobalShortcutsEnabled(bool v)
{
    if (globalShortcutsEnabled() == v)
        return;
    m_store->setValue(kGlobalShortcuts, v);
    Q_EMIT globalShortcutsEnabledChanged();
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
    // A move that ends where it started, or a restore to the saved size, is
    // not a change; any setValue rewrites the whole settings file.
    // Compared as ints: after a restart the file hands back strings.
    const auto same = [&](const char *key, int value) {
        const QString k = group + QLatin1String(key);
        return m_store->contains(k) && m_store->value(k).toInt() == value;
    };
    if (same("/x", x) && same("/y", y) && same("/width", width)
        && same("/height", height))
        return;
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
    // The voice speed inherits this key until it is first written: freeze
    // the inherited value first, so a speed chosen for other media from here
    // on does not silently change how voice messages play.
    if (!m_store->contains(kVoicePlaybackRate))
        m_store->setValue(kVoicePlaybackRate, mediaPlaybackRate());
    m_store->setValue(kMediaPlaybackRate, clamped);
    Q_EMIT mediaPlaybackRateChanged();
}

int SettingsManager::voiceMiniPlayerCorner() const
{
    bool ok = false;
    const int v = m_store->value(kVoiceMiniPlayerCorner, 0).toInt(&ok);
    // An enum, not a quantity: an unknown value falls back to the default
    // corner rather than clamping to the nearest one (CLAUDE.md §16).
    return (ok && v >= 0 && v <= 3) ? v : 0;
}

void SettingsManager::setVoiceMiniPlayerCorner(int corner)
{
    const int valid = (corner >= 0 && corner <= 3) ? corner : 0;
    if (voiceMiniPlayerCorner() == valid)
        return;
    m_store->setValue(kVoiceMiniPlayerCorner, valid);
    Q_EMIT voiceMiniPlayerCornerChanged();
}

bool SettingsManager::voicePlayerDocked() const
{
    return m_store->value(kVoicePlayerDocked, true).toBool();
}

void SettingsManager::setVoicePlayerDocked(bool docked)
{
    if (voicePlayerDocked() == docked)
        return;
    m_store->setValue(kVoicePlayerDocked, docked);
    Q_EMIT voicePlayerDockedChanged();
}

bool SettingsManager::largeAlbumArt() const
{
    return m_store->value(kLargeAlbumArt, false).toBool();
}

void SettingsManager::setLargeAlbumArt(bool large)
{
    if (largeAlbumArt() == large)
        return;
    m_store->setValue(kLargeAlbumArt, large);
    Q_EMIT largeAlbumArtChanged();
}

QRectF SettingsManager::voiceMiniPlayerGeometry() const
{
    const QVariant raw = m_store->value(kVoiceMiniPlayerGeometry);
    if (!raw.isValid() || !raw.canConvert<QRectF>())
        return {};
    const QRectF r = raw.toRectF();
    // Anything a newer build, a hand edit or a corrupt file could leave: a
    // non-finite or non-positive size is "never placed", and the fractions
    // are bounded so the card cannot be restored outside its area.
    if (!qIsFinite(r.x()) || !qIsFinite(r.y()) || !qIsFinite(r.width())
        || !qIsFinite(r.height()) || r.width() <= 0 || r.height() <= 0)
        return {};
    return QRectF(qBound(0.0, r.x(), 1.0), qBound(0.0, r.y(), 1.0),
                  qMin(r.width(), 4000.0), qMin(r.height(), 4000.0));
}

void SettingsManager::setVoiceMiniPlayerGeometry(const QRectF &geometry)
{
    QRectF valid;
    if (qIsFinite(geometry.x()) && qIsFinite(geometry.y())
        && qIsFinite(geometry.width()) && qIsFinite(geometry.height())
        && geometry.width() > 0 && geometry.height() > 0) {
        valid = QRectF(qBound(0.0, geometry.x(), 1.0),
                       qBound(0.0, geometry.y(), 1.0),
                       qMin(geometry.width(), 4000.0),
                       qMin(geometry.height(), 4000.0));
    }
    if (voiceMiniPlayerGeometry() == valid)
        return;
    if (valid.isValid())
        m_store->setValue(kVoiceMiniPlayerGeometry, valid);
    else
        m_store->remove(kVoiceMiniPlayerGeometry);
    Q_EMIT voiceMiniPlayerGeometryChanged();
}

qreal SettingsManager::voicePlaybackRate() const
{
    if (!m_store->contains(kVoicePlaybackRate))
        return mediaPlaybackRate();
    bool ok = false;
    const qreal v = m_store->value(kVoicePlaybackRate, 1.0).toDouble(&ok);
    if (!ok || !(v >= 0.25) || !(v <= 4.0))
        return 1.0;
    return v;
}

void SettingsManager::setVoicePlaybackRate(qreal v)
{
    const qreal clamped = v < 0.25 ? 0.25 : (v > 4.0 ? 4.0 : v);
    // Written even when equal to the inherited value, so the voice speed
    // stops following the shared key from here on.
    const bool changed =
        !qFuzzyCompare(voicePlaybackRate() + 1.0, clamped + 1.0);
    m_store->setValue(kVoicePlaybackRate, clamped);
    if (changed)
        Q_EMIT voicePlaybackRateChanged();
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

bool SettingsManager::videoPrefetchEnabled() const
{
    return m_store->value(kVideoPrefetchEnabled, true).toBool();
}

void SettingsManager::setVideoPrefetchEnabled(bool v)
{
    if (videoPrefetchEnabled() == v)
        return;
    m_store->setValue(kVideoPrefetchEnabled, v);
    Q_EMIT videoPrefetchEnabledChanged();
}

int SettingsManager::videoPrefetchMaxMb() const
{
    // A stored value that is not a whole number in range (hand-edited file,
    // newer build) reads as the default rather than as an arbitrary end of
    // the range.
    bool ok = false;
    const int v = m_store->value(kVideoPrefetchMaxMb,
                                 videoPrefetchDefaultMb()).toInt(&ok);
    if (!ok || v < videoPrefetchMinMb() || v > videoPrefetchLimitMb())
        return videoPrefetchDefaultMb();
    return v;
}

void SettingsManager::setVideoPrefetchMaxMb(int mb)
{
    // A quantity: 4000 and 100 are the same intent at different magnitudes.
    const int clamped = std::clamp(mb, videoPrefetchMinMb(),
                                   videoPrefetchLimitMb());
    if (videoPrefetchMaxMb() == clamped)
        return;
    m_store->setValue(kVideoPrefetchMaxMb, clamped);
    Q_EMIT videoPrefetchMaxMbChanged();
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

bool SettingsManager::hideThreadReplies() const
{
    return m_store->value(kHideThreadReplies, false).toBool();
}

void SettingsManager::setHideThreadReplies(bool hide)
{
    if (hideThreadReplies() == hide)
        return;
    m_store->setValue(kHideThreadReplies, hide);
    Q_EMIT hideThreadRepliesChanged();
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

bool SettingsManager::highContrast() const
{
    return appearanceValue(kHighContrast, false).toBool();
}

void SettingsManager::setHighContrast(bool v)
{
    if (highContrast() == v)
        return;
    setAppearanceValue(kHighContrast, v);
    Q_EMIT highContrastChanged();
}

bool SettingsManager::underlineLinks() const
{
    return appearanceValue(kUnderlineLinks, false).toBool();
}

void SettingsManager::setUnderlineLinks(bool v)
{
    if (underlineLinks() == v)
        return;
    setAppearanceValue(kUnderlineLinks, v);
    Q_EMIT underlineLinksChanged();
}

int SettingsManager::nameColorCorrection() const
{
    // An enum, not a quantity: an unknown value (a newer build wrote a
    // fourth mode) falls back to the default instead of clamping to the last.
    const int stored = appearanceValue(kNameColorCorrection,
                                       kNameColorStrong).toInt();
    return (stored < kNameColorStrong || stored > kNameColorOff)
        ? kNameColorStrong : stored;
}

void SettingsManager::setNameColorCorrection(int mode)
{
    if (mode < kNameColorStrong || mode > kNameColorOff)
        mode = kNameColorStrong;
    if (nameColorCorrection() == mode)
        return;
    setAppearanceValue(kNameColorCorrection, mode);
    Q_EMIT nameColorCorrectionChanged();
}

int SettingsManager::messageSpacing() const
{
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        const int stored =
            accountScopedValue(kNhekoMessageSpacing, kSpacingCozy).toInt();
        // Cozy adds nothing to the Small avatars rhythm (2 px between
        // continuations, 6 px between senders), which is Nheko's own.
        return (stored < kSpacingCompact || stored > kSpacingRoomy)
            ? kSpacingCozy : stored;
    }
    const int stored = appearanceValue(kMessageSpacing, kSpacingCozy).toInt();
    return (stored < kSpacingCompact || stored > kSpacingRoomy)
        ? kSpacingCozy : stored;
}

void SettingsManager::setMessageSpacing(int mode)
{
    if (mode < kSpacingCompact || mode > kSpacingRoomy)
        mode = kSpacingCozy;
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        const bool wasCustomized = layoutPresetCustomized();
        const bool moved = messageSpacing() != mode;
        setAccountScopedValue(kNhekoMessageSpacing, mode);
        if (!wasCustomized)
            Q_EMIT layoutPresetChanged();
        if (moved)
            Q_EMIT messageSpacingChanged();
        return;
    }
    if (messageSpacing() == mode)
        return;
    setAppearanceValue(kMessageSpacing, mode);
    Q_EMIT messageSpacingChanged();
}

int SettingsManager::threadStyle() const
{
    // An unknown value (a newer build's third style) falls back to the
    // default, never to the nearest end of the range.
    const int stored =
        appearanceValue(kThreadStyle, kThreadStylePanel).toInt();
    return (stored == kThreadStyleInline) ? kThreadStyleInline
                                          : kThreadStylePanel;
}

void SettingsManager::setThreadStyle(int style)
{
    if (style != kThreadStyleInline)
        style = kThreadStylePanel;
    if (threadStyle() == style)
        return;
    const int placementBefore = threadPlacement();
    setAppearanceValue(kThreadStyle, style);
    Q_EMIT threadStyleChanged();
    // A derived placement follows the style.
    if (threadPlacement() != placementBefore)
        Q_EMIT threadPlacementChanged();
}

int SettingsManager::threadPlacement() const
{
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        // The compact profile's focused thread is the main column.
        const int stored = accountScopedValue(kNhekoThreadPlacement,
                                              kThreadPlacementMain).toInt();
        return stored == kThreadPlacementSide ? kThreadPlacementSide
                                              : kThreadPlacementMain;
    }
    const QVariant stored = appearanceValue(kThreadPlacement, QVariant());
    if (stored.isValid()) {
        bool ok = false;
        const int value = stored.toInt(&ok);
        if (ok && (value == kThreadPlacementSide
                   || value == kThreadPlacementMain))
            return value;
    }
    // Migration default: the placement each style already had (the panel
    // beside the room; inline focus in the conversation column).
    return threadStyle() == kThreadStyleInline ? kThreadPlacementMain
                                               : kThreadPlacementSide;
}

bool SettingsManager::threadPlacementIsExplicit() const
{
    if (layoutPreset() == kLayoutPresetNhekoCompact)
        return accountScopedValue(kNhekoThreadPlacement, QVariant()).isValid();
    const QVariant stored = appearanceValueUnannounced(kThreadPlacement, QVariant());
    bool ok = false;
    const int value = stored.isValid() ? stored.toInt(&ok) : -1;
    return ok && (value == kThreadPlacementSide
                  || value == kThreadPlacementMain);
}

void SettingsManager::setThreadPlacement(int placement)
{
    if (placement != kThreadPlacementMain)
        placement = kThreadPlacementSide;
    if (layoutPreset() == kLayoutPresetNhekoCompact) {
        const bool wasCustomized = layoutPresetCustomized();
        const bool moved = threadPlacement() != placement;
        setAccountScopedValue(kNhekoThreadPlacement, placement);
        if (!wasCustomized)
            Q_EMIT layoutPresetChanged();
        if (moved)
            Q_EMIT threadPlacementChanged();
        return;
    }
    if (threadPlacementIsExplicit() && threadPlacement() == placement)
        return;
    const int before = threadPlacement();
    setAppearanceValue(kThreadPlacement, placement);
    if (before != placement)
        Q_EMIT threadPlacementChanged();
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

int SettingsManager::conversationReturnMode() const
{
    // An unknown value (a newer build's fourth choice) is the default, never
    // the nearest end of the range.
    const int stored =
        appearanceValue(kConversationReturn, kReturnPrevious).toInt();
    return (stored == kReturnLatest || stored == kReturnSelected)
               ? stored : kReturnPrevious;
}

void SettingsManager::setConversationReturnMode(int mode)
{
    if (mode != kReturnLatest && mode != kReturnSelected)
        mode = kReturnPrevious;
    if (conversationReturnMode() == mode)
        return;
    setAppearanceValue(kConversationReturn, mode);
    Q_EMIT conversationReturnModeChanged();
}

bool SettingsManager::showSelectionOnReturn() const
{
    return appearanceValue(kShowSelectionReturn, false).toBool();
}

void SettingsManager::setShowSelectionOnReturn(bool v)
{
    if (showSelectionOnReturn() == v)
        return;
    setAppearanceValue(kShowSelectionReturn, v);
    Q_EMIT showSelectionOnReturnChanged();
}

bool SettingsManager::showBridgeBadges() const
{
    return m_store->value(kShowBridgeBadges, true).toBool();
}

void SettingsManager::setShowBridgeBadges(bool show)
{
    if (showBridgeBadges() == show)
        return;
    m_store->setValue(kShowBridgeBadges, show);
    Q_EMIT showBridgeBadgesChanged();
}

bool SettingsManager::compactRoomList() const
{
    return m_store->value(kCompactRoomList, false).toBool();
}

void SettingsManager::setCompactRoomList(bool compact)
{
    if (compactRoomList() == compact)
        return;
    m_store->setValue(kCompactRoomList, compact);
    Q_EMIT compactRoomListChanged();
}

bool SettingsManager::disableCursorBlink() const
{
    return m_store->value(kDisableCursorBlink, false).toBool();
}

void SettingsManager::setDisableCursorBlink(bool disabled)
{
    if (disableCursorBlink() == disabled)
        return;
    m_store->setValue(kDisableCursorBlink, disabled);
    Q_EMIT disableCursorBlinkChanged();
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
    // "Follow the system", queried fresh so a language switch applies: the
    // desktop's own pattern when its time category speaks the UI language,
    // else its 12/24-hour cycle in the UI language's pattern (see
    // i18n::followSystemClockFormat). QLocale() is the formatting locale
    // LocalizationManager installs.
    return i18n::followSystemClockFormat(
        QLocale(), i18n::timeCategoryLocale(),
        QLocale::system().timeFormat(QLocale::ShortFormat));
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

QStringList SettingsManager::spellCheckExtraLanguages() const
{
    QStringList out;
    const QStringList stored = m_store->value(kSpellExtraLanguages).toStringList();
    for (const QString &tag : stored) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty() && !out.contains(trimmed) && out.size() < 16)
            out << trimmed;
    }
    return out;
}

void SettingsManager::setSpellCheckExtraLanguages(const QStringList &tags)
{
    QStringList normalized;
    for (const QString &tag : tags) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty() && !normalized.contains(trimmed) && normalized.size() < 16)
            normalized << trimmed;
    }
    if (spellCheckExtraLanguages() == normalized)
        return;
    if (normalized.isEmpty())
        m_store->remove(kSpellExtraLanguages);
    else
        m_store->setValue(kSpellExtraLanguages, normalized);
    Q_EMIT spellCheckExtraLanguagesChanged();
}

int SettingsManager::autocorrectMode() const
{
    // An enum, not a quantity: a value a newer build wrote falls back to the
    // default rather than being clamped onto a neighbour.
    const int mode = m_store->value(kAutocorrectMode, 0).toInt();
    return (mode == 1 || mode == 2) ? mode : 0;
}

void SettingsManager::setAutocorrectMode(int mode)
{
    const int normalized = (mode == 1 || mode == 2) ? mode : 0;
    if (autocorrectMode() == normalized)
        return;
    if (normalized == 0)
        m_store->remove(kAutocorrectMode);
    else
        m_store->setValue(kAutocorrectMode, normalized);
    Q_EMIT autocorrectModeChanged();
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

QString SettingsManager::imageMetadataMode() const
{
    return imagemeta::modeToString(imagemeta::modeFromString(
        m_store->value(kImageMetadataMode).toString()));
}

void SettingsManager::setImageMetadataMode(const QString &mode)
{
    const QString normalized =
        imagemeta::modeToString(imagemeta::modeFromString(mode));
    if (imageMetadataMode() == normalized
        && m_store->contains(kImageMetadataMode))
        return;
    m_store->setValue(kImageMetadataMode, normalized);
    Q_EMIT imageMetadataModeChanged();
}

bool SettingsManager::emojiShortcodeAutoConvert() const
{
    return m_store->value(kEmojiShortcodeAutoConvert, true).toBool();
}

void SettingsManager::setEmojiShortcodeAutoConvert(bool v)
{
    if (emojiShortcodeAutoConvert() == v)
        return;
    m_store->setValue(kEmojiShortcodeAutoConvert, v);
    Q_EMIT emojiShortcodeAutoConvertChanged();
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

int SettingsManager::textRenderingFromValue(const QVariant &stored)
{
    bool ok = false;
    const int v = stored.toInt(&ok);
    if (ok && v == kTextRenderingNative)
        return kTextRenderingNative;
    return kTextRenderingSmooth;
}

int SettingsManager::textRendering() const
{
    return textRenderingFromValue(m_store->value(kTextRenderingKey));
}

void SettingsManager::setTextRendering(int mode)
{
    mode = textRenderingFromValue(mode);
    if (textRendering() == mode)
        return;
    m_store->setValue(kTextRenderingKey, mode);
    Q_EMIT textRenderingChanged();
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

void SettingsManager::setRecentEmoji(const QStringList &mru)
{
    QStringList recent = mru;
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
    if (m_secretStore)
        return readAccountSecret(uid, kSecretAccessToken);
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
    // A session still read from an item saved before install scoping is copied
    // first, so the rotated tokens complete a whole copy, client id included,
    // rather than start one without it.
    const QString slug = slugForSavedAccount(uid);
    if (m_secretStore->isSharedBetweenInstalls() && !slug.isEmpty()) {
        bool copyFirst = !keyringItemsAreCurrent(slug);
        if (!copyFirst) {
            const QString own = m_secretStore->readSecret(
                uid, QLatin1String(kSecretAccessToken));
            copyFirst = !m_secretStore->lastReadFailed() && own.isEmpty();
        }
        QHash<QString, QString> legacy;
        if (copyFirst
            && adoptLegacyKeyringSession(uid, &legacy)
                   == LegacyAdoption::CopyFailed) {
            m_secretStore->storeSecret(
                uid, QLatin1String(kSecretOAuthClientId),
                legacy.value(QLatin1String(kSecretOAuthClientId)));
        }
    }
    // The refresh token first, the access token last and only after it: the
    // access token names the pair (the retiring handle's write-back compares
    // against it), so it must never be newer than the refresh token beside
    // it. In the old order a refused refresh write left the NEW access token
    // over the consumed refresh token, and the write-back then took that
    // keyring for "a different sign-in" and kept the consumed one for good.
    // Written even when empty so a stale refresh token cannot be replayed.
    bool ok = m_secretStore->storeSecret(uid, QLatin1String(kSecretRefreshToken),
                                         refreshToken);
    ok = ok
         && m_secretStore->storeSecret(uid, QLatin1String(kSecretAccessToken),
                                       accessToken);
    if (!ok) {
        // Never echoes either value.
        qCWarning(lcSettings) << "failed to persist refreshed session tokens";
    } else {
        noteKeyringBackend(uid);
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
    return readAccountSecret(uid, kSecretRefreshToken);
}

QString SettingsManager::oauthClientIdFor(const QString &userId) const
{
    const QString uid = userId.trimmed();
    if (uid.isEmpty() || !m_secretStore)
        return {};
    return readAccountSecret(uid, kSecretOAuthClientId);
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

// The portable store (Windows portable zip) encrypts the tokens but keeps its
// key in the same folder by design, so it is not "secure", and it is not the
// plaintext fallback either; Settings words the two differently.
bool SettingsManager::secretsArePortable() const
{
    return m_secretStore && m_secretStore->isAvailable()
        && m_secretStore->backendName().startsWith(QLatin1String("portable file"));
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
    // Signed in again: an unfinished sign-out of this account must not delete
    // the store this sign-in uses at the next start.
    clearPendingSignOut(uidCanonical);
    // The sign-in finished: its store now has a record.
    clearPendingSignIn(uidCanonical);
    m_store->setValue(kActiveAccount, uidCanonical);
    // Keep the login prefill on the most recently used homeserver.
    m_store->setValue(kHomeserver, hsCanonical);

    // In QSettings so restore can route correctly with a locked keyring.
    const QString authType = authType_.trimmed().isEmpty()
                                 ? QStringLiteral("password")
                                 : authType_.trimmed();
    m_store->setValue(accountKey(slug, kAccountAuthType), authType);
    // A sign-in by this build: its items are this install's own, for this
    // device, and it never reads an item saved before install scoping again.
    markKeyringItems(slug, kKeyringItemsOwn);

    if (m_secretStore) {
        if (!m_secretStore->storeSecret(uidCanonical, QLatin1String(kSecretAccessToken), accessToken_)) {
            qCWarning(lcSettings)
                << "failed to persist access token to SecretStore:"
                << m_secretStore->lastError();
        } else {
            noteKeyringBackend(uidCanonical);
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

void SettingsManager::recordPendingSignOut(
    const matrix::app_data::AccountIdentity &identity)
{
    if (!identity.isValid())
        return;
    const QString base = QLatin1String(kPendingSignOutGroup) + QLatin1Char('/')
        + identity.slug + QLatin1Char('/');
    m_store->setValue(base + QLatin1String("userId"), identity.userId);
    m_store->setValue(base + QLatin1String("homeserver"), identity.homeserver);
    m_store->setValue(base + QLatin1String("storeSlug"), identity.effectiveStoreSlug());
    m_store->setValue(base + QLatin1String("storePath"),
                      QDir::cleanPath(QFileInfo(identity.rustStorePath).absoluteFilePath()));
    // Durable before the server logout starts: that is the window it covers.
    m_store->sync();
}

void SettingsManager::clearPendingSignOut(const QString &userId)
{
    const QString slug = matrix::app_data::safeUserSlug(userId.trimmed());
    if (slug.isEmpty())
        return;
    const QString group = QLatin1String(kPendingSignOutGroup) + QLatin1Char('/') + slug;
    if (m_store->value(group + QLatin1String("/userId")).toString() != userId.trimmed())
        return;
    m_store->remove(group);
    m_store->sync();
}

void SettingsManager::recordPendingSignIn(
    const matrix::app_data::AccountIdentity &identity)
{
    if (!identity.isValid())
        return;
    const QString base = QLatin1String(kPendingSignInGroup) + QLatin1Char('/')
        + identity.slug + QLatin1Char('/');
    m_store->setValue(base + QLatin1String("userId"), identity.userId);
    m_store->setValue(base + QLatin1String("storePath"),
                      QDir::cleanPath(QFileInfo(identity.rustStorePath).absoluteFilePath()));
    // Durable before the SDK creates the store: that is the window it covers.
    m_store->sync();
}

void SettingsManager::clearPendingSignIn(const QString &userId)
{
    const QString slug = matrix::app_data::safeUserSlug(userId.trimmed());
    if (slug.isEmpty())
        return;
    const QString group = QLatin1String(kPendingSignInGroup) + QLatin1Char('/') + slug;
    if (!m_store->contains(group + QLatin1String("/storePath")))
        return;
    // The slug is not injective; only the record of this very user id goes.
    if (m_store->value(group + QLatin1String("/userId")).toString() != userId.trimmed())
        return;
    m_store->remove(group);
    m_store->sync();
}

void SettingsManager::clearPendingSignIn(
    const matrix::app_data::AccountIdentity &attempt)
{
    if (!attempt.isValid())
        return;
    // The attempt's own marker, under the identity it was typed as: the
    // record (saveSession) may be under the server's canonical id instead.
    dropPendingSignInsNaming(attempt.rustStorePath);
}

void SettingsManager::dropPendingSignInsNaming(const QString &storePath)
{
    if (storePath.isEmpty())
        return;
    const QString target = QDir::cleanPath(QFileInfo(storePath).absoluteFilePath());
    m_store->beginGroup(QLatin1String(kPendingSignInGroup));
    const QStringList slugs = m_store->childGroups();
    m_store->endGroup();
    bool dropped = false;
    for (const QString &slug : slugs) {
        const QString group = QLatin1String(kPendingSignInGroup) + QLatin1Char('/') + slug;
        if (QDir::cleanPath(m_store->value(group + QLatin1String("/storePath")).toString())
            == target) {
            m_store->remove(group);
            dropped = true;
        }
    }
    if (dropped)
        m_store->sync();
}

QSet<QString> SettingsManager::pendingSignInStorePaths()
{
    QSet<QString> out;
    m_store->beginGroup(QLatin1String(kPendingSignInGroup));
    const QStringList slugs = m_store->childGroups();
    m_store->endGroup();
    bool pruned = false;
    for (const QString &slug : slugs) {
        const QString group = QLatin1String(kPendingSignInGroup) + QLatin1Char('/') + slug;
        const QString storePath = QDir::cleanPath(
            m_store->value(group + QLatin1String("/storePath")).toString());
        // Gone (a failed attempt rolled it back, a later sign-in quarantined
        // it): nothing left to explain, so the marker goes too.
        if (storePath.isEmpty() || storePath == QLatin1String(".")
            || !QFileInfo(storePath).isDir()) {
            m_store->remove(group);
            pruned = true;
            continue;
        }
        out.insert(storePath);
    }
    if (pruned)
        m_store->sync();
    return out;
}

QList<matrix::app_data::AccountIdentity> SettingsManager::pendingSignOuts()
{
    QList<matrix::app_data::AccountIdentity> out;
    m_store->beginGroup(QLatin1String(kPendingSignOutGroup));
    const QStringList slugs = m_store->childGroups();
    m_store->endGroup();
    for (const QString &slug : slugs) {
        const QString base = QLatin1String(kPendingSignOutGroup) + QLatin1Char('/')
            + slug + QLatin1Char('/');
        const QString userId = m_store->value(base + QLatin1String("userId")).toString();
        const QString storePath =
            m_store->value(base + QLatin1String("storePath")).toString();
        matrix::app_data::AccountIdentity identity;
        const char *refused = nullptr;
        if (!matrix::app_data::resolveAccountIdentity(
                m_store->value(base + QLatin1String("homeserver")).toString(), userId,
                &identity)
            || identity.slug != slug
            || !matrix::app_data::bindStoreSlug(
                &identity, m_store->value(base + QLatin1String("storeSlug")).toString())) {
            refused = "unresolvable";
        } else if (storePath.isEmpty()
                   || QDir::cleanPath(QFileInfo(identity.rustStorePath).absoluteFilePath())
                          != storePath) {
            // Another data root (portable mode, a moved home) or an edited file.
            refused = "store_path_moved";
        } else {
            const QString owner = accountOwningStoreSlug(identity.effectiveStoreSlug());
            if (!owner.isEmpty() && owner != identity.userId)
                refused = "store_owned_by_another_account";
        }
        if (refused) {
            qCWarning(lcSettings) << "pending sign-out dropped, nothing removed"
                                  << "slug=" << slug << "reason=" << refused;
            m_store->remove(QLatin1String(kPendingSignOutGroup) + QLatin1Char('/') + slug);
            continue;
        }
        out.append(identity);
    }
    m_store->sync();
    return out;
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
    // Read before the record goes: it names the keyring that holds this
    // account's items, and the clear below must reach that one.
    const QString recordedBackend = keyringBackendFor(recordUserId);

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
        m_keyringBackendBeingCleared = {recordUserId, recordedBackend};
        // A session copied from an item saved before install scoping: while the
        // shared item still holds the very token this sign-out revokes, it goes
        // too, as it did before scoping, or a live token stays at rest after a
        // failed server logout or a local reset (CLAUDE.md §6). Any other value
        // belongs to another session and stays.
        bool sharedCopyCleared = true;
        if (m_secretStore->isSharedBetweenInstalls()) {
            const QString own = m_secretStore->readSecret(
                recordUserId, QLatin1String(kSecretAccessToken));
            const bool ownRead = !m_secretStore->lastReadFailed();
            const QString shared = m_secretStore->readLegacySecret(
                recordUserId, QLatin1String(kSecretAccessToken));
            if (ownRead && !m_secretStore->lastReadFailed() && !own.isEmpty()
                && shared == own) {
                for (const char *key : {kSecretRefreshToken,
                                        kSecretOAuthClientId,
                                        kSecretAccessToken}) {
                    if (!m_secretStore->deleteLegacySecret(recordUserId,
                                                           QLatin1String(key)))
                        sharedCopyCleared = false;
                }
            }
        }
        // Use the exact persisted key so a mixed-case id cannot orphan its
        // entry.
        secretsCleared = m_secretStore->clearAccountSecrets(recordUserId)
                         && sharedCopyCleared;
        m_keyringBackendBeingCleared = {};
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
