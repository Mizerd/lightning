#include "app/AppController.h"

#include "matrix/BridgeNetwork.h"

#include "app/FontManager.h"

#include "app/RichComposerBridge.h"
#include "crypto/BackupController.h"
#include "crypto/E2eeDiagnostics.h"
#include "models/ScheduledSendController.h"
#include "models/ActivityModel.h"
#include "models/MediaHistoryModel.h"

#include <algorithm>

#include "app/CustomAppIcon.h"
#include "app/SessionDiagnostics.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#ifndef LIGHTNING_RUST_ONLY
#include "matrix/CppHttpMatrixClient.h"
#include "matrix/MockMatrixClient.h"
#endif
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/EmojiCatalog.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "models/ReverseListProxyModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "notifications/NotificationManager.h"
#include "spaces/SpaceManager.h"
#include "storage/SecretStore.h"
#include "storage/InMemorySecretStore.h"
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include "app/ScreenshotDemoController.h"
#endif
#include "calls/CallController.h"
#include "calls/CallSoundPlayer.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/CameraPortal.h"
#include "calls/GstCallMediaBackend.h"
#include "calls/ScreenCastPortal.h"
#include "calls/SfuMediaEngine.h"
#endif
#include "presence/PresenceManager.h"
#include "threads/ThreadManager.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

// Pure failure classification with no Rust dependency; compiled everywhere.
#include "matrix/RustSessionPolicy.h"

#include "matrix/MatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QClipboard>
#include <QMimeData>

#include <cstring>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QSaveFile>
#include <QScreen>
#include <QSysInfo>
#include <QTimer>
#include <QPalette>
#include <QStyleHints>
#include <QUuid>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApp, "matrix.app")

namespace {
// Delay before the optional automatic update check. Derived from
// UpdateManager's startup quiet period plus a margin: QTimer::singleShot may
// fire early at this scale, and with equal values the check would be refused
// for the whole session.
constexpr int kAutomaticUpdateCheckDelayMs =
    int(lightning::update::UpdateManager::kStartupQuietPeriodMs) + 5 * 1000;
} // namespace

bool AppController::isBackendCompiled(Backend backend)
{
    switch (backend) {
    case MockBackend:
    case HttpBackend:
#ifdef LIGHTNING_RUST_ONLY
        // Not compiled into a Rust-only release; preflight rejects these.
        return false;
#else
        return true;
#endif
    case RustBackend:
#ifdef ENABLE_RUST_SDK_BACKEND
        return true;
#else
        return false;
#endif
    }
    return false;
}

std::unique_ptr<MatrixClient> AppController::makeClient(Backend backend,
                                                        SettingsManager *settings,
                                                        QObject *parent)
{
#ifdef LIGHTNING_RUST_ONLY
    // Rust-only release: preflight rejects any non-Rust backend.
    Q_UNUSED(backend);
    return std::make_unique<RustSdkMatrixClient>(settings, parent);
#else
    switch (backend) {
    case MockBackend: {
        auto mock = std::make_unique<MockMatrixClient>(parent);
        mock->setSettings(settings);
        return mock;
    }
    case RustBackend:
#ifdef ENABLE_RUST_SDK_BACKEND
        return std::make_unique<RustSdkMatrixClient>(settings, parent);
#else
        // Rejected upstream by main.cpp; fall back to HTTP defensively.
        qCCritical(lcApp)
            << "RustBackend selected but not compiled in; falling back to HTTP";
        return std::make_unique<CppHttpMatrixClient>(settings, parent);
#endif
    case HttpBackend:
    default:
        return std::make_unique<CppHttpMatrixClient>(settings, parent);
    }
#endif
}

AppController::AppController(Backend backend, bool screenshotDemo,
                             QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_screenshotDemo(screenshotDemo)
    // Screenshot-demo mode must never construct a production secure store,
    // whose constructor probes the real Secret Service.
    , m_secretStore(screenshotDemo
                        ? std::unique_ptr<SecretStore>(
                              std::make_unique<InMemorySecretStore>(this))
                        : SecretStore::createDefault(this))
    , m_settings(std::make_unique<SettingsManager>(this))
{
#ifdef LIGHTNING_RUST_ONLY
    // Fail closed: a release binary must never run a non-Rust backend.
    if (backend != RustBackend)
        qFatal("LIGHTNING_RUST_ONLY: refusing to start a non-Rust backend");
#endif
    // Wire the SecretStore before anything reads accessToken(); this also
    // migrates a legacy plaintext token.
    m_settings->setSecretStore(m_secretStore.get());

    // Installs no catalog until main.cpp calls applyStoredLanguage() before
    // the QML engine loads.
    m_localization = std::make_unique<LocalizationManager>(m_settings.get(), this);
    m_customTheme = std::make_unique<CustomThemeStore>(m_settings.get(), this);
    // Constructed before any QML exists so the first frame already carries
    // the user's bindings.
    m_shortcuts = std::make_unique<ShortcutRegistry>(m_settings.get(), this);
    m_railLayout = std::make_unique<RailLayoutStore>(m_settings.get(), this);
    m_railEntries = std::make_unique<RailEntryModel>(this);
    m_mediaVisibility = std::make_unique<MediaVisibilityStore>(this);
    // Persistence is account-scoped; this loads the active account's list.
    m_mediaVisibility->setSettings(m_settings.get());
    m_banners = std::make_unique<ProfileBannerManager>(this);
    m_nameColors = std::make_unique<NameColorManager>(this);
    m_bio = std::make_unique<ProfileBioManager>(this);
    m_userProfiles = std::make_unique<UserProfileResolver>(this);
    // A fixed local table: decoration, not Matrix state.
    m_badges = std::make_unique<ProfileBadges>(this);
    // The cropper previews through the in-memory staged-image store, so QML
    // never loads a user-chosen file:// path (which could render an SVG).
    m_imageCrop.setStagedImages(&m_stagedImages);

    m_client       = makeClient(backend, m_settings.get(), this);
    m_accounts     = std::make_unique<AccountManager>(m_settings.get(), this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_allRooms     = std::make_unique<RoomListModel>(this);
    m_quickSwitcher = std::make_unique<QuickSwitcherModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    m_timelineView = std::make_unique<ReverseListProxyModel>(this);
    m_timelineView->setSourceModel(m_timeline.get());
    m_composer     = std::make_unique<MessageComposer>(this);
    // MSC4108 QR login shares the verification code store: both show one
    // code at a time and a stale token renders nothing.
    m_policy       = std::make_unique<PolicyListController>(this);
    m_qrLogin      = std::make_unique<QrLoginController>(this);
    m_qrLogin->setQrStore(&m_qrCodeStore);
    m_richComposer = std::make_unique<RichComposerBridge>(this);
    m_richComposer->setComposer(m_composer.get());
    // Clipboard images never become files; one store serves both composers.
    m_composer->attachments()->setStagedImages(&m_stagedImages);

    // Spell checking defaults to the system locale rather than the UI
    // language, since the keyboard decides the dictionary. Reports
    // "unavailable" when no dictionary exists; see `--spell-status`.
    m_spell.initialize(m_settings->spellCheckLanguage());
    m_spell.setEnabled(m_settings->spellCheckEnabled());
    connect(m_settings.get(), &SettingsManager::spellCheckEnabledChanged, this,
            [this] { m_spell.setEnabled(m_settings->spellCheckEnabled()); });
    connect(m_settings.get(), &SettingsManager::spellCheckLanguageChanged, this,
            [this] { m_spell.setPreferredLanguage(m_settings->spellCheckLanguage()); });

    // Privacy settings (read receipts, typing) are applied once here so a
    // stored choice holds from the first receipt, and again on change.
    applyPrivacyPreferences();
    connect(m_settings.get(), &SettingsManager::readReceiptModeChanged, this,
            [this] { applyPrivacyPreferences(); });
    // MSC4153 is applied at client build time, so a change affects the next
    // client only.
    connect(m_settings.get(), &SettingsManager::strictDeviceTrustChanged, this,
            [this] { applyStrictDeviceTrust(); });
    applyStrictDeviceTrust();
    connect(m_settings.get(), &SettingsManager::sendTypingNotificationsChanged,
            this, [this] { applyPrivacyPreferences(); });

    // The tray icon exists only while the user asked for it and the platform
    // has a tray.
    connect(&m_tray, &TrayIcon::showRequested,
            this, &AppController::trayShowRequested);
    connect(m_settings.get(), &SettingsManager::closeToTrayChanged,
            this, &AppController::refreshTrayState);
    connect(m_settings.get(), &SettingsManager::notificationsEnabledChanged,
            this, &AppController::refreshTrayState);
    refreshTrayState();

    // Decide once, against the current display layout, whether the stored
    // position is still reachable (SettingsManager already enforced the
    // minimum size). With no screens at all the geometry is dropped.
    if (const QRect stored = m_settings->initialWindowGeometry();
            !stored.isEmpty()) {
        if (windowGeometryIsReachable(stored))
            m_restorableWindowGeometry = stored;
        else
            qCInfo(lcApp, "stored window geometry ignored: off-screen");
    }
    // One shared draft store for the room and thread composers; drafts
    // persist only for unencrypted rooms.
    m_draftStore   = std::make_unique<DraftStore>(this);
    m_draftStore->setSettings(m_settings.get());
    m_composer->setDraftStore(m_draftStore.get());
    m_mentionSuggestions = std::make_unique<MentionSuggestionModel>(this);
    m_emojiCatalog = std::make_unique<EmojiCatalog>(m_settings.get(), this);
    m_notifications= std::make_unique<NotificationManager>(this);
    m_media        = std::make_unique<MediaManager>(this);
    m_crypto       = std::make_unique<CryptoManager>(this);
    m_cryptoHealth = std::make_unique<CryptoHealthModel>(this);
    m_backup       = std::make_unique<BackupController>(this);
    m_scheduledSends = std::make_unique<ScheduledSendController>(this);
    m_widgets = std::make_unique<WidgetController>(this);
    m_activity = std::make_unique<ActivityModel>(this);
    m_mediaHistory = std::make_unique<MediaHistoryModel>(this);
    m_cryptoBootstrap = std::make_unique<CryptoBootstrapModel>(this);
    m_spaces       = std::make_unique<SpaceManager>(this);
    m_threads      = std::make_unique<ThreadManager>(this);
    m_presence     = std::make_unique<PresenceManager>(this);
    m_presence->setSettings(m_settings.get());
    m_scheduledSends->setSettings(m_settings.get());
    // Only the seen marker and the keywords persist, account-scoped.
    m_activity->setStore({ [this] { return m_settings->activityState(); },
                           [this](const QVariantMap &state) {
                               m_settings->setActivityState(state);
                           } });
    // Voice calls. The media backend is attached later, only when the build
    // has the GStreamer engine and its elements resolve at runtime;
    // LIGHTNING_DISABLE_WEBRTC=1 is the kill switch.
    m_calls        = std::make_unique<CallController>(this);
    m_rtc          = std::make_unique<RtcController>(this);
    m_groupCall    = std::make_unique<SfuCallController>(this);
    m_callDevices  = std::make_unique<CallDeviceController>(this);
    m_callSounds   = std::make_unique<CallSoundController>(this);
    // Application updates: not account-scoped, so sign-in, sign-out and
    // account switches never disturb a check or download.
    m_updateManager = std::make_unique<lightning::update::UpdateManager>(this);
    // The automatic check is delayed so it never stands between the user and
    // a usable app (the privacy notes promise nothing in the first 30 s).
    // maybeCheckAutomatically() enforces the preference and the 24 h rate
    // limit; this is a one-shot trigger.
    QTimer::singleShot(kAutomaticUpdateCheckDelayMs, m_updateManager.get(), [this] {
        m_updateManager->maybeCheckAutomatically();
    });
    connect(m_updateManager.get(), &lightning::update::UpdateManager::quitRequested,
            this, [this] {
                // The helper waits for this process to exit before installing.
                // Announce the intent first: close-to-tray would otherwise
                // refuse the window close and abort the quit, leaving the
                // helper to time out.
                Q_EMIT applicationQuitIntended();
                QCoreApplication::quit();
            });
    m_pinned       = std::make_unique<PinnedMessagesController>(this);
    m_roomUpgrade  = std::make_unique<RoomUpgradeController>(this);
    m_thread       = std::make_unique<ThreadController>(this);
    m_thread->attachments()->setStagedImages(&m_stagedImages);
    // The rich composer bridge serves both composers.
    m_richComposer->setThread(m_thread.get());
    m_conversations= std::make_unique<ConversationController>(this);
    m_discovery = std::make_unique<RoomDiscoveryController>(this);
    m_messageSearch = std::make_unique<MessageSearchController>(this);
    m_uia = std::make_unique<UiaController>(this);
    m_moderation = std::make_unique<ModerationController>(this);
    m_forward      = std::make_unique<ForwardController>(this);
    m_roomInfo     = std::make_unique<RoomInfoController>(this);
    m_mediaBridge  = std::make_unique<MediaBridge>(this);
    m_accountAvatars = std::make_unique<AccountAvatarStore>(this);
    // Persist the active account's own avatar when its bytes pass through
    // MediaBridge: that is the only time they exist, since media is fetched
    // through the active client, so the switcher needs a disk copy to draw
    // other accounts. Only the mxc matching the recorded avatarUrl is stored;
    // the store refuses non-raster data and oversized files.
    connect(m_mediaBridge.get(), &MediaBridge::mediaCached, this,
            [this](const QString &cacheKey) {
                if (!m_settings || !m_accountAvatars)
                    return;
                const QString uid = m_settings->activeAccountUserId();
                if (uid.isEmpty())
                    return;
                const QString mxc =
                    m_settings->accountRecord(uid)
                        .value(QStringLiteral("avatarUrl")).toString();
                if (mxc.isEmpty() || !cacheKey.endsWith(mxc))
                    return;
                const QByteArray bytes = m_mediaBridge->cachedBytes(cacheKey);
                if (!bytes.isEmpty())
                    m_accountAvatars->store(uid, bytes);
            });
    // The tray balloon is the delivery where there is no freedesktop daemon
    // (Windows, macOS); see refreshTrayState.
    m_notifications->setFallbackTray(&m_tray);
    m_notifications->setAvatarProvider(
        [this](const QString &mxc, bool request) {
            if (request)
                m_mediaBridge->avatarSource(mxc, 64);
            return m_mediaBridge->cachedAvatarImage(mxc);
        },
        [this](const QString &mxc) {
            return !m_mediaBridge->avatarFailureCategory(mxc).isEmpty();
        });
    connect(m_mediaBridge.get(), &MediaBridge::mediaCached,
            m_notifications.get(),
            [this](const QString &cacheKey) {
                if (cacheKey.startsWith(QLatin1String("mxc:")))
                    m_notifications->avatarCacheChanged();
            });

    // Inline custom emoji (MSC2545). The sanitizer keeps the `mxc:` form so an
    // edit cannot carry a local source back to the room; the resolver maps it
    // through the authenticated media path at 64 px (20 px rendered, doubled
    // for HiDPI). The composer turns an installed `:shortcode:` into an inline
    // image on send.
    m_composer->setEmoticonSearch(
        [this](const QString &prefix, int limit) {
            return m_stickers->findEmoticons(prefix, limit);
        });
    m_composer->setEmoticonResolver([this](const QString &shortcode) {
        const QVariantMap found = m_stickers->emoticon(shortcode);
        return found.value(QStringLiteral("url")).toString();
    });
    m_timeline->setInlineImageResolver(
        [this](const QString &mxc) {
            return m_mediaBridge->mxcImageSource(mxc, 64);
        });
    connect(m_mediaBridge.get(), &MediaBridge::mediaCached,
            m_timeline.get(),
            [this](const QString &) {
                // An emoji resolves to "" until its bytes arrive; re-read on
                // arrival.
                m_timeline->notifyInlineImagesChanged();
            });
    connect(m_mediaBridge.get(), &MediaBridge::mediaFetchFailed,
            m_notifications.get(),
            [this](const QString &cacheKey, const QString &) {
                if (cacheKey.startsWith(QLatin1String("mxc:")))
                    m_notifications->avatarCacheChanged();
            });
    m_playback     = std::make_unique<MediaPlaybackController>(this);
    m_pagination   = std::make_unique<PaginationController>(this);
    m_readReceipts = std::make_unique<ReadReceiptCoordinator>(this);
    m_linkPreviews = std::make_unique<LinkPreviewController>(this);
    m_gifTransport = std::make_unique<MatrixGifTransport>(this);
    m_gif          = std::make_unique<GifSearchController>(this);
    m_gif->setTransport(m_gifTransport.get());
    m_gifSend      = std::make_unique<GifSendController>(this);
    m_stickers     = std::make_unique<StickerPackManager>(this);
    m_gifSend->setRecentModel(m_gif->recent());
    // A starred-GIF send reads the stored bytes from disk by content hash.
    m_gifSend->setLocalGifReader([this](const QString &hash) {
        return m_gif->starredStore()->readBytes(hash);
    });
    // MediaBridge's star fetch stays GIF-agnostic; its result is routed into
    // the starred store only here, keeping src/media/ and src/gif/ decoupled.
    connect(m_mediaBridge.get(), &MediaBridge::mediaBytesForStar, this,
            [this](const QString &mediaKey, bool ok, const QByteArray &bytes,
                   const QString &category) {
        // Only keys this account asked to star or copy. The fetch is shared
        // with ForwardController, and writing every forwarded image into the
        // on-disk saved-media store would persist decrypted media nobody
        // chose to keep. The bridge dedups in-flight fetches by key, so one
        // signal may have to service both a copy and a star claim: no early
        // return between the two branches.
        const bool wasCopy = m_pendingCopyKeys.remove(mediaKey);
        const bool wasStar = m_pendingStarKeys.remove(mediaKey);
        if (wasCopy)
            copyImageBytesToClipboard(mediaKey, ok, bytes, category);
        if (!wasStar)
            return;
        if (ok)
            m_gif->starredStore()->starBytes(mediaKey, bytes);
        else
            m_gif->starredStore()->reportFetchFailed(mediaKey, category);
    });
    // Learned video dimensions persist per account so a metadata-less card
    // has its true shape on later visits. Dimensions only, never content.
    connect(m_mediaBridge.get(), &MediaBridge::videoDimensionsLearned, this,
            [this](const QString &mediaKey, int width, int height) {
        if (m_settings)
            m_settings->setKnownVideoDimensions(mediaKey, width, height);
    });
    connect(m_mediaBridge.get(), &MediaBridge::playableSizeLearned, this,
            [this](const QString &mediaKey, qint64 bytes) {
        if (m_settings)
            m_settings->setKnownMediaSizeBytes(mediaKey, bytes);
    });

    // GIF policy follows settings live.
    m_gif->setRating(m_settings->gifSafeSearch());
    m_gif->setActiveProvider(m_settings->gifPreferredProvider());
    m_gif->recent()->setRecordingEnabled(m_settings->storeRecentGifs());
    connect(m_settings.get(), &SettingsManager::gifSafeSearchChanged, this,
            [this] { m_gif->setRating(m_settings->gifSafeSearch()); });
    connect(m_settings.get(), &SettingsManager::gifPreferredProviderChanged, this,
            [this] { m_gif->setActiveProvider(m_settings->gifPreferredProvider()); });
    connect(m_settings.get(), &SettingsManager::storeRecentGifsChanged, this,
            [this] {
                m_gif->recent()->setRecordingEnabled(m_settings->storeRecentGifs());
            });
    // The verification warning depends on both trust state and the
    // per-account dismissal, so it re-notifies on either.
    connect(m_settings.get(),
            &SettingsManager::verificationWarningDismissedChanged, this,
            [this] { Q_EMIT sessionVerificationWarningChanged(); });
    connect(this, &AppController::securityStateChanged, this,
            [this] { Q_EMIT sessionVerificationWarningChanged(); });
    m_timelineScroll = std::make_unique<TimelineScrollController>(this);
    m_threadScroll   = std::make_unique<TimelineScrollController>(this);

    m_crypto->setBackendName(backendName());

    // A typing notification contradicts a cached "offline" (servers with
    // presence disabled answer "offline" for everyone). PresenceManager only
    // withdraws the contradicted claim; it never promotes anyone to online.
    connect(m_client.get(), &MatrixClient::typingChanged, this,
            [this](const QString &roomId) {
        if (!m_presence || MatrixClient::isThreadTimelineId(roomId))
            return;
        const QStringList typists = m_client->typingUsersFor(roomId);
        for (const QString &userId : typists)
            m_presence->noteTyping(userId);
    });

    // Native notifications: every appended remote event runs the pure
    // decision. Bodies are never logged or persisted.
    connect(m_client.get(), &MatrixClient::eventAppended, this,
            [this](const QString &composedRoomId, const TimelineEvent &event) {
        // Map a thread copy to its real room rather than dropping it: the
        // live room timeline hides threaded events, so for the open room the
        // thread copy is the only producer. The composite id never leaves
        // this scope; duplicates are prevented by event id.
        const bool fromThread = MatrixClient::isThreadTimelineId(composedRoomId);
        const QString roomId = fromThread
            ? MatrixClient::threadTimelineRoomId(composedRoomId)
            : composedRoomId;
        if (roomId.isEmpty())
            return;
        // Dedup on both branches: any event with a thread root may arrive
        // from both producers.
        if (fromThread || !event.threadRootId.isEmpty()) {
            if (event.eventId.isEmpty()
                || m_notifiedThreadEventIds.contains(event.eventId))
                return;
            m_notifiedThreadEventIds.insert(event.eventId);
            // Only needs to outlive the window where two producers overlap.
            if (m_notifiedThreadEventIds.size() > 512)
                m_notifiedThreadEventIds.clear();
        }
        // Activity prompts a presence refresh; it is never evidence of
        // "online".
        if (m_presence && event.sender != m_client->currentUserId())
            m_presence->noteActivity(event.sender);
        NotificationManager::Context context;
        context.selfUserId = m_client->currentUserId();
        // Targeted lookup; rooms() deep-copies the whole list per event.
        const RoomInfo info = m_client->roomInfo(roomId);
        context.roomName = info.name.isEmpty() ? roomId : info.name;
        context.roomIsDirect = info.isDirect;
        const QVariantMap notifyRoomRow = m_roomList->findRoom(roomId);
        context.avatarMxc =
            notifyRoomRow.value(QStringLiteral("avatarUrl")).toString();
        // Initials colour and theme from the same row the room list uses, so
        // the two never disagree about an identity.
        context.avatarColorKey =
            notifyRoomRow.value(QStringLiteral("identityColorKey")).toString();
        context.themeId = int(m_settings->theme());
        context.roomMode = static_cast<NotificationManager::RoomMode>(
            m_settings->roomNotificationMode(roomId));
        // Encrypted rooms may withhold more. `encryptionKnown` is a real third
        // state during hydration; see effectiveNotificationPreview.
        context.previewMode = static_cast<NotificationManager::PreviewMode>(
            m_settings->effectiveNotificationPreview(
                notifyRoomRow.value(QStringLiteral("encrypted")).toBool(),
                notifyRoomRow.value(QStringLiteral("encryptionKnown")).toBool()));
        context.notificationsEnabled = m_settings->notificationsEnabled();
        // "On screen" is thread-aware: the room timeline hides threaded
        // events, so a thread reply is visible only if its panel is open.
        const bool threadPanelShowingThis =
            fromThread && m_thread
            && m_thread->rootEventId() == event.threadRootId;
        context.roomVisibleAtLatest =
            roomId == m_currentRoomId && m_activeRoomAtLatest
            && (!fromThread || threadPanelShowingThis);
        // The open room's backlog arrives as live appends while its view is
        // still hydrating.
        context.roomHydrating =
            roomId == m_currentRoomId && m_activeRoomHydrating;
        // Initial-sync backlog is history and must not re-notify each launch.
        context.initialSyncComplete = m_client->initialSyncDone();
        context.soundMode = static_cast<NotificationManager::SoundMode>(
            m_settings->notificationSound());
        // Events from an ignored user already in flight must not notify.
        context.senderIsIgnored =
            m_moderation && m_moderation->isIgnored(event.sender);
        m_notifications->processEvent(event, context);
        // The Activity Center classifies independently (a muted room's
        // mention is still activity).
        m_activity->ingest(event, context.roomName);
    });
    connect(m_notifications.get(), &NotificationManager::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        routeNotificationOpen(roomId, eventId, threadRootId);
    });
    // A notification card outlives the account that raised it. Acting under
    // whichever account is current could mark another account's room read
    // or send a reply from the wrong identity, so both actions are refused on
    // a mismatch and the user is told.
    connect(m_notifications.get(), &NotificationManager::markReadRequested,
            this, [this](const QString &accountUserId, const QString &roomId,
                         const QString &eventId) {
        Q_UNUSED(eventId);
        if (!notificationActionIsForCurrentAccount(accountUserId))
            return;
        if (m_roomList)
            m_roomList->markRoomRead(roomId);
        m_notifications->closeRoomNotifications(roomId);
    });
    connect(m_notifications.get(), &NotificationManager::replyRequested, this,
            [this](const QString &accountUserId, const QString &roomId,
                   const QString &threadRootId, const QString &text) {
        if (!notificationActionIsForCurrentAccount(accountUserId))
            return;
        if (!m_client || roomId.isEmpty() || text.isEmpty())
            return;
        // A reply to a threaded message belongs in that thread.
        if (!threadRootId.isEmpty())
            m_client->sendThreadReply(roomId, threadRootId, text);
        else
            m_client->sendTextMessage(roomId, text);
        // Replying is reading.
        if (m_roomList)
            m_roomList->markRoomRead(roomId);
        m_notifications->closeRoomNotifications(roomId);
        qCInfo(lcApp) << "notification reply sent"
                      << "thread=" << !threadRootId.isEmpty();
    });
    // Server-reported per-room notification mode. Only an explicit
    // user-defined rule reconciles the device-local cache; a resolved account
    // default is never persisted, since it could overwrite a local choice with
    // a guess. Server writes are issued only from setRoomNotificationMode(),
    // so an echo cannot loop back into another write. Stale-generation events
    // are already rejected in RustSdkMatrixClient.
    connect(m_client.get(), &MatrixClient::roomNotificationModeChanged, this,
            [this](const QString &roomId, int mode, bool userDefined) {
        if (!userDefined) {
            qCDebug(lcApp) << "room notification default report (not persisted)";
            return;
        }
        // Also reachable directly from tests/backends; drop rather than let
        // SettingsManager clamp to the least conservative mode.
        if (mode < 0 || mode > 2)
            return;
        // While a room has a failed write pending, the local value is
        // authoritative: a poll can still report the old rule. Only a report
        // equal to the cached value acknowledges the write.
        if (m_notificationModeSyncFailures.contains(roomId)) {
            if (mode != m_settings->roomNotificationMode(roomId)) {
                qCDebug(lcApp) << "room notification report differs while"
                               << "unsynced (kept local value)";
                return;
            }
            m_settings->setRoomNotificationMode(roomId, mode);
            m_notificationModeSyncFailures.remove(roomId);
            Q_EMIT roomNotificationModeSyncStateChanged(roomId);
            return;
        }
        m_settings->setRoomNotificationMode(roomId, mode);
    });
    // A successful rule removal is the only acknowledgement a "follow account
    // default" choice can receive, so it retires the failure state.
    connect(m_client.get(), &MatrixClient::roomNotificationModeCleared, this,
            [this](const QString &roomId) {
        // Ignore a clear acknowledged after the user chose an explicit mode.
        if (m_settings->roomNotificationMode(roomId) != 3)
            return;
        if (!m_notificationModeSyncFailures.remove(roomId))
            return;
        Q_EMIT roomNotificationModeSyncStateChanged(roomId);
    });
    connect(m_client.get(), &MatrixClient::roomNotificationModeWriteFailed,
            this, [this](const QString &roomId) {
        if (m_notificationModeSyncFailures.contains(roomId))
            return;
        m_notificationModeSyncFailures.insert(roomId);
        Q_EMIT roomNotificationModeSyncStateChanged(roomId);
    });
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            [this] {
                // Close the starred-GIF store on every session detach,
                // including a plain switch: onLoggedOut() returns early while
                // switching, so the picker would otherwise show the outgoing
                // account's rows until the next login. Deletion is separate
                // (onLoggedOut() and removeAccount()).
                m_gif->closeStarredStore();
                m_notifications->clearPending();
                m_knownInvites.clear();
                // Session-scoped failure state; the pickers re-query on open.
                m_notificationModeSyncFailures.clear();
                // A rename in flight must not lock renaming for the next
                // account.
                if (m_sessionDeviceRenameOp != 0
                    || !m_sessionDeviceRenameError.isEmpty()) {
                    m_sessionDeviceRenameOp = 0;
                    m_sessionDeviceRenameError.clear();
                    Q_EMIT sessionDevicesChanged();
                }
                m_activitySeeded = false;
            });
    // Hydration marks a room before its fetch resolves; a failed fetch
    // un-marks it so the next open retries.
    connect(m_client.get(), &MatrixClient::roomMembersReceived, this,
            [this](quint64, const QString &roomId, const QVariantMap &snapshot) {
                if (!snapshot.value(QStringLiteral("ok")).toBool())
                    m_memberHydratedRooms.remove(roomId);
            });
    // MSC2346: reduce a room's advertised bridges to one badge. A protocol id
    // in the curated table wins regardless of order (so it cannot lose to
    // attacker-chosen text); otherwise the first nameable entry; otherwise
    // nothing, leaving the ghost-mxid/alias inference in charge.
    connect(m_client.get(), &MatrixClient::roomBridgesReceived, this,
            [this](quint64, const QString &roomId, bool ok,
                   const QVariantList &bridges) {
                if (!ok) {
                    // A failed read must not fail closed for the session.
                    m_bridgeReadRooms.remove(roomId);
                    return;
                }
                matrix::bridge::AdvertisedBridgeLabel best;
                for (const QVariant &entry : bridges) {
                    const QVariantMap row = entry.toMap();
                    const auto resolved =
                        matrix::bridge::labelForAdvertisedBridge(
                            row.value(QStringLiteral("protocol")).toString(),
                            row.value(QStringLiteral("protocolName")).toString(),
                            row.value(QStringLiteral("network")).toString());
                    if (resolved.label.isEmpty())
                        continue;
                    if (!matrix::bridge::labelForNetworkId(resolved.networkId)
                             .isEmpty()) {
                        best = resolved;
                        break;
                    }
                    if (best.label.isEmpty())
                        best = resolved;
                }
                if (m_roomList)
                    m_roomList->setAdvertisedBridge(roomId, best.networkId,
                                                    best.label);
                // Persist the answer so the next launch paints the badge
                // without a request. An empty advertisement is recorded as a
                // negative; an unnameable one is not, so it is asked again
                // once its protocol may be in the curated table. Failed reads
                // never reach here.
                if (!best.label.isEmpty() || bridges.isEmpty())
                    m_bridgeLabels.remember(roomId, best.networkId, best.label);
            });
    // Notify once per newly seen invite; invites present before initial sync
    // are seeded silently so a restart never re-announces them.
    connect(m_client.get(), &MatrixClient::roomsChanged, this, [this] {
        const auto rooms = m_client->rooms();
        QSet<QString> current;
        for (const auto &room : rooms) {
            if (room.membership != RoomInfo::Invited)
                continue;
            current.insert(room.id);
            m_activity->noteInvite(room);
            if (NotificationManager::shouldNotifyInvite(
                    m_client->initialSyncDone(),
                    m_knownInvites.contains(room.id),
                    m_settings->notificationsEnabled())) {
                m_notifications->showGeneric(
                    tr("Room invitation"),
                    m_settings->notificationPreview() == 2
                        ? tr("New Matrix notification")
                        : tr("You were invited to %1")
                              .arg(room.name.isEmpty() ? room.id : room.name),
                    room.id,
                    m_settings->notificationPreview() == 2
                        ? QString()
                        : m_roomList->findRoom(room.id)
                              .value(QStringLiteral("avatarUrl")).toString());
            }
        }
        for (const QString &gone : m_knownInvites)
            if (!current.contains(gone))
                m_activity->inviteResolved(gone);
        m_knownInvites = current;
    });
    // The tray badge follows the same two signals the room list does. The
    // walk is local and the icon is only re-rasterised when the badge changes.
    m_trayUnreadCoalesce.setSingleShot(true);
    m_trayUnreadCoalesce.setInterval(0);
    connect(&m_trayUnreadCoalesce, &QTimer::timeout, this,
            &AppController::refreshTrayUnread);
    connect(m_client.get(), &MatrixClient::roomsChanged, this,
            [this] { m_trayUnreadCoalesce.start(); });
    connect(m_client.get(), &MatrixClient::roomUpdated, this,
            [this](const QString &) { m_trayUnreadCoalesce.start(); });

    m_mediaHistory->setClient(m_client.get());
    m_qrLogin->setClient(m_client.get());
    m_policy->setClient(m_client.get());
    // A fresh client starts with public receipts, so push the stored choice
    // on every attachment.
    applyPrivacyPreferences();
    m_spaces->setClient(m_client.get());
    m_threads->setClient(m_client.get());
    m_presence->setClient(m_client.get());
    m_calls->setClient(m_client.get());
    m_rtc->setClient(m_client.get());
    m_groupCall->setClient(m_client.get());
    m_groupCall->setRtcController(m_rtc.get());
    // SfuCallController reads the microphone gain and per-participant volumes
    // through the settings; without this every volume control is inert.
    m_groupCall->setSettings(m_settings.get());
    // A refused join is reported in the status strip, and an empty reason is
    // forwarded too: it withdraws a refusal once a retry succeeds.
    //
    // A call answered by joining the room (the MatrixRTC lane) must also end
    // the ring on the notification lane. Hooked to the state change so every
    // entry point is covered.
    connect(m_groupCall.get(), &SfuCallController::callFailed, this,
            [this](const QString &reason) {
                Q_EMIT errorReported(reason);
            });
    connect(m_groupCall.get(), &SfuCallController::stateChanged, this,
            [this] {
                if (m_groupCall->active())
                    m_calls->noteAnsweredByOtherLane(m_groupCall->roomId());
            });
    m_callDevices->setSettings(m_settings.get());
    // Call sounds watch both lanes; the player is installed separately
    // (enableCallSounds) so tests never open audio.
    m_callSounds->setSettings(m_settings.get());
    m_callSounds->setGroupCall(m_groupCall.get());
    m_callSounds->setLegacyCalls(m_calls.get());
    // Ring policy: state stays in CallController, these close the gates
    // shouldRing() consults. The functors read live state, so account
    // switches need no rewiring.
    m_calls->setSenderIgnoredCheck([this](const QString &userId) {
        return m_moderation && m_moderation->isIgnored(userId);
    });
    m_calls->setRoomMutedCheck([this](const QString &roomId) {
        return m_settings->roomNotificationMode(roomId)
            == static_cast<int>(NotificationManager::Muted);
    });
    // Never ring for cold-start backlog, like messages.
    m_calls->setBacklogSuppressed(!m_client->initialSyncDone());
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, this,
            [this] {
                m_calls->setBacklogSuppressed(!m_client->initialSyncDone());
                // MatrixRTC transport discovery is account-scoped and needs a
                // live session, so it runs on the sync edge.
                if (m_client->initialSyncDone())
                    m_rtc->discover(m_currentRoomId);
            });
    // Retry server transport discovery on room change, so a room with no call
    // yet can start one after an earlier failure. RtcController::discover
    // refuses while one is in flight or once the server has answered.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        if (!m_client || !m_client->initialSyncDone())
            return;
        if (!m_rtc->discoveryWorthRetrying())
            return;
        m_rtc->discover(m_currentRoomId);
    });

    // A message selection belongs to the room it was started in.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        if (m_forward && m_forward->selecting())
            m_forward->cancelSelecting();
    });

    // ── Incoming-call notification + ring ──
    connect(m_calls.get(), &CallController::incomingCallStarted, this,
            [this](const QString &roomId, const QString &callId,
                   const QString &senderId, qint64 remainingMs) {
                if (!m_settings->notificationsEnabled())
                    return;
                if (!m_calls->shouldRing())
                    return; // backlog, ignored sender, or muted room
                // Per-sender cooldown so a room member cannot pump
                // critical-urgency notifications with fresh call ids. Only
                // the OS announcement is bounded.
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const qint64 lastRing = m_lastCallRingBySender.value(
                    senderId, 0);
                if (now - lastRing < 30000)
                    return;
                m_lastCallRingBySender.insert(senderId, now);
                while (m_lastCallRingBySender.size() > 64) {
                    // Bounded; the cooldown is a heuristic, so any entry goes.
                    m_lastCallRingBySender.erase(
                        m_lastCallRingBySender.begin());
                }
                const bool privatePreview =
                    m_settings->notificationPreview() == 2;
                const QVariantMap room = m_roomList->findRoom(roomId);
                // Not escaped here: NotificationManager escapes once, at the
                // D-Bus call, only when the daemon advertises body-markup.
                const QString roomName = room.value(QStringLiteral("name"))
                                             .toString();
                // Localpart only, never the full MXID.
                const QString caller = senderId.mid(1)
                                           .section(QLatin1Char(':'), 0, 0);
                const QString body = privatePreview
                    ? tr("Incoming voice call")
                    : (roomName.isEmpty()
                           ? tr("%1 is calling").arg(caller)
                           : tr("%1 is calling in %2")
                                 .arg(caller, roomName));
                // Ring as long as the invite stays valid; sound is gated on
                // the user's switches.
                const bool sound = m_settings->ringForCalls()
                    && m_settings->notificationSound()
                        != 0 /* SoundOff */;
                // Lightning's own ringer when it has loaded, else the
                // desktop's themed call sound, never both. A call the user
                // silenced stays silenced on re-announcement; a new call id
                // rings normally.
                const bool ringing =
                    sound && !m_callSounds->isRingSilenced(callId);
                const bool ownRinger =
                    ringing && m_callSounds->ringerAvailable();
                m_announcedCallId = callId;
                // Whether the card offers Answer is decided here, from the
                // same sources IncomingCallPrompt reads.
                const bool rtcLane = m_calls->rtcRing();
                // The RTC join gate is usually not known yet: kick the session
                // read and let the sessionChanged handler below open the
                // button.
                if (rtcLane)
                    m_rtc->refresh(roomId);
                const bool acceptOffered = callAcceptOffered(roomId, rtcLane);
                m_notifications->showIncomingCall(
                    roomId, callId, tr("Incoming call"), body,
                    ringing && !ownRinger,
                    static_cast<int>(qBound<qint64>(
                        qint64(5), remainingMs / 1000, qint64(300))),
                    acceptOffered, rtcLane, /*silenceOffered=*/ringing);
                if (ownRinger)
                    m_callSounds->startIncomingRing(callId);
            });
    // Silence has one entry point: CallSoundController::silenceRing decides
    // whether the id is still ringing, then the card drops its sound and
    // button.
    connect(m_notifications.get(),
            &NotificationManager::callSilenceRequested, this,
            [this](const QString &callId) {
                m_callSounds->silenceRing(callId);
            });
    connect(m_callSounds.get(), &CallSoundController::ringSilenced, this,
            [this](const QString &callId) {
                m_notifications->silenceIncomingCall(callId);
            });
    connect(m_calls.get(), &CallController::incomingCallEnded, this,
            [this](const QString &roomId, const QString &callId,
                   int reason, bool missed) {
                Q_UNUSED(reason);
                m_notifications->stopIncomingCall(callId);
                const bool wasAnnounced = m_announcedCallId == callId;
                if (m_announcedCallId == callId)
                    m_announcedCallId.clear();
                // Missed-call notice only for a call classified missed from
                // its pre-end state and actually announced; a ring the gates
                // suppressed must not resurface as missed.
                if (!missed || !wasAnnounced)
                    return;
                if (!m_settings->notificationsEnabled())
                    return;
                const bool privatePreview =
                    m_settings->notificationPreview() == 2;
                const QVariantMap room = m_roomList->findRoom(roomId);
                // Escaping belongs to NotificationManager alone.
                const QString roomName = room.value(QStringLiteral("name"))
                                             .toString();
                m_notifications->showGeneric(
                    tr("Missed call"),
                    privatePreview || roomName.isEmpty()
                        ? tr("You missed a voice call")
                        : tr("You missed a voice call in %1").arg(roomName),
                    roomId,
                    privatePreview
                        ? QString()
                        : room.value(QStringLiteral("avatarUrl")).toString());
            });
    connect(m_notifications.get(),
            &NotificationManager::callDeclineRequested, this,
            [this](const QString &callId) {
                if (m_calls->activeCallId() == callId)
                    m_calls->rejectIncoming();
            });
    // The join gate opens late, so redraw the still-ringing card when the
    // session read lands. setCallAcceptOffered never extends the ring
    // deadline.
    connect(m_rtc.get(), &RtcController::sessionChanged, this,
            [this](const QString &roomId) {
                const QString callId = m_calls->activeCallId();
                if (callId.isEmpty() || callId != m_announcedCallId)
                    return;
                if (roomId.isEmpty() || roomId != m_calls->activeRoomId())
                    return;
                const bool rtcLane = m_calls->rtcRing();
                m_notifications->setCallAcceptOffered(
                    callId, callAcceptOffered(roomId, rtcLane), rtcLane);
            });
    // Answering from the notification must pick the lane: answer() refuses
    // an RTC ring with `rtc_unsupported`. The room opens first so a refusal
    // is explained where the user is looking.
    connect(m_notifications.get(),
            &NotificationManager::callAcceptRequested, this,
            [this](const QString &callId) {
                if (m_calls->activeCallId() != callId)
                    return;
                const QString roomId = m_calls->activeRoomId();
                routeNotificationOpen(roomId, QString(), QString());
                // Read both results: the card is already retired, so a silent
                // refusal would leave nothing to press. acceptOffered does not
                // promise success (`no_remote_offer` is a timing condition).
                const bool rtcLane = m_calls->rtcRing();
                const bool ok = rtcLane
                    ? m_groupCall->join(roomId, /*withVideo=*/false)
                    : m_calls->answer();
                if (ok) {
                    qCInfo(lcApp) << "notification accept lane="
                                  << (rtcLane ? "matrixrtc" : "legacy")
                                  << "ok= true";
                } else {
                    // Logged so "I pressed Answer and nothing happened" can be
                    // diagnosed.
                    qCWarning(lcApp) << "notification accept REFUSED lane="
                                     << (rtcLane ? "matrixrtc" : "legacy")
                                     << "refusal="
                                     << (rtcLane
                                             ? m_rtc->joinBlockReason(roomId)
                                             : m_calls->lastRefusal());
                }
            });
    m_pinned->setClient(m_client.get());
    m_roomUpgrade->setClient(m_client.get());
    m_backup->setClient(m_client.get());
    // A backup action invalidates the crypto-health snapshot that gates two
    // destructive buttons; see BackupController::cryptoHealthStale.
    connect(m_backup.get(), &BackupController::cryptoHealthStale, this,
            &AppController::refreshCryptoHealth, Qt::UniqueConnection);
    m_scheduledSends->setClient(m_client.get());
    m_widgets->setClient(m_client.get());
    // Theme and language are widget URL template variables.
    if (m_settings && m_localization) {
        const auto pushPresentation = [this] {
            // The Qt enum key, lower-cased without the suffix ("storm"), the
            // shape Element's client_theme carries.
            QString name = QString::fromLatin1(
                QMetaEnum::fromType<SettingsManager::Theme>()
                    .valueToKey(m_settings->theme()));
            if (name.endsWith(QLatin1String("Theme")))
                name.chop(5);
            m_widgets->setPresentation(
                name.toLower(),
                m_localization ? m_localization->effectiveLanguage() : QString());
        };
        pushPresentation();
        connect(m_settings.get(), &SettingsManager::themeChanged, this,
                pushPresentation);
    }
    // A widget list belongs to one room.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        m_widgets->setRoomId(m_currentRoomId);
    });
    m_activity->setClient(m_client.get());
    // An Activity row click routes exactly like a notification click.
    connect(m_activity.get(), &ActivityModel::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        routeNotificationOpen(roomId, eventId, threadRootId);
    });
    connect(m_client.get(), &MatrixClient::reactionEventReceived, this,
            [this](const QString &roomId, const QString &reactionEventId,
                   const QString &targetEventId, const QString &senderId,
                   const QString &key, qint64 timestampMs) {
        const RoomInfo info = m_client->roomInfo(roomId);
        m_activity->noteReaction(roomId, info.name.isEmpty() ? roomId : info.name,
                                 reactionEventId, targetEventId, senderId,
                                 m_client->displayNameFor(roomId, senderId), key,
                                 timestampMs);
    });
    connect(m_client.get(), &MatrixClient::eventAtTimestampReceived, this,
            [this](quint64 opId, const QString &roomId, bool ok,
                   const QString &eventId, qint64 timestampMs,
                   const QString &category) {
        Q_UNUSED(timestampMs);
        const QString expected = m_pendingDateJumps.take(opId);
        if (expected.isEmpty())
            return;   // not ours, or already answered
        // The room moved under the answer; do not drag another room.
        if (expected != roomId || roomId != m_currentRoomId) {
            Q_EMIT jumpToDateFinished(opId, false, QStringLiteral("stale"));
            return;
        }
        if (!ok || eventId.isEmpty()) {
            Q_EMIT jumpToDateFinished(opId, false, category);
            return;
        }
        // Same landing a reply jump uses: paginates toward an unloaded target
        // and holds it by stable id.
        if (m_pagination)
            m_pagination->jumpToEvent(eventId);
        Q_EMIT jumpToDateFinished(opId, true, QString());
    });
    connect(m_client.get(), &MatrixClient::activitySeedReceived, this,
            [this](const QVariantList &entries) {
        QVariantList named;
        for (const QVariant &v : entries) {
            QVariantMap m = v.toMap();
            const QString roomId = m.value(QStringLiteral("roomId")).toString();
            const QString senderId = m.value(QStringLiteral("senderId")).toString();
            m.insert(QStringLiteral("senderName"),
                     m_client->displayNameFor(roomId, senderId));
            m.insert(QStringLiteral("roomName"), m_client->roomInfo(roomId).name);
            named.append(m);
        }
        m_activity->seed(named);
    });
    connect(m_client.get(), &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState state) {
        // One bounded seed per session, once the account is syncing.
        if (state == MatrixClient::Syncing && !m_activitySeeded) {
            m_activitySeeded = true;
            m_client->requestActivitySeed(60);
        }
        // Keep the local search index filled automatically. The sweep only
        // writes what is not already indexed, so it is cheap on a quiet
        // account.
        if (state == MatrixClient::Syncing) {
            m_client->sweepSearchIndex();
            m_client->searchIndexStats();
            if (!m_searchIndexTimer.isActive())
                m_searchIndexTimer.start();
        } else {
            m_searchIndexTimer.stop();
        }
    });
    // New messages become searchable within five minutes.
    m_searchIndexTimer.setInterval(5 * 60 * 1000);
    m_searchIndexTimer.setSingleShot(false);
    connect(&m_searchIndexTimer, &QTimer::timeout, this, [this] {
        if (m_client)
            m_client->sweepSearchIndex();
    });
    // Backstop for an identity-key mismatch that appears after login (the
    // event-driven checks cluster around sign-in). requestOwnDeviceKeyCheck()
    // holds the rate limit; the timer stops once the fault is latched.
    m_ownDeviceKeyTimer.setInterval(
        static_cast<int>(matrix::crypto::OwnDeviceKeyWatch::kRecheckIntervalMs));
    m_ownDeviceKeyTimer.setSingleShot(false);
    connect(&m_ownDeviceKeyTimer, &QTimer::timeout, this,
            [this] { requestOwnDeviceKeyCheck(); });
    // A redacted message must not stay findable in the local index.
    connect(m_client.get(), &MatrixClient::eventRedacted, this,
            [this](const QString &roomId, const QString &eventId) {
        Q_UNUSED(roomId);
        if (m_client && !eventId.isEmpty())
            m_client->forgetIndexedEvent(eventId);
    });
    m_thread->setClient(m_client.get());
    m_thread->setDraftStore(m_draftStore.get());
    m_draftStore->setClient(m_client.get());
    m_roomList->setClient(m_client.get());
    m_roomList->setSpaceManager(m_spaces.get());
    // No setSpaceManager: this is the unfiltered list the forward pickers use.
    m_allRooms->setClient(m_client.get());
    // Rail rows: the user's arrangement over the hierarchy, with the drag
    // preview in the model rather than QML.
    m_railEntries->setSources(m_spaces.get(), m_railLayout.get());
    // Classic rail (depth style 1) flattens the model, not the paint, so the
    // drag arithmetic never counts hidden rows. Connected and applied once.
    const auto applyRailStyle = [this] {
        m_railEntries->setFlat(m_settings->spacesRailDepthStyle()
                               == SettingsManager::kRailDepthClassic);
    };
    connect(m_settings.get(), &SettingsManager::spacesRailDepthStyleChanged,
            this, applyRailStyle);
    applyRailStyle();
    // The rail arrangement is per-account behind a process-lifetime cache, so
    // the store must be told about sign-out and switches.
    m_railLayout->setClient(m_client.get());
    // The Channels layout is global: it needs the account's rooms and the
    // rail order, not the selected Space.
    m_spaceChannels = std::make_unique<SpaceChannelModel>(this);
    m_spaceChannels->setSources(m_client.get(), m_spaces.get(),
                                m_railLayout.get());
    m_spaceChannels->setSettings(m_settings.get());
    m_quickSwitcher->setClient(m_client.get());
    m_quickSwitcher->setSpaceManager(m_spaces.get());
    m_timeline->setClient(m_client.get());
    m_timeline->setProfileResolver(m_userProfiles.get());
    m_composer->setClient(m_client.get());
    m_mentionSuggestions->setClient(m_client.get());
    m_banners->setClient(m_client.get());
    m_nameColors->setClient(m_client.get());
    m_bio->setClient(m_client.get());
    m_userProfiles->setClient(m_client.get());
    m_media->setClient(m_client.get());
    m_conversations->setClient(m_client.get());
    m_discovery->setClient(m_client.get());
    m_messageSearch->setClient(m_client.get());
    m_uia->setClient(m_client.get());
    m_moderation->setClient(m_client.get());
    m_forward->setClient(m_client.get());
    m_forward->setMediaBridge(m_mediaBridge.get());
    m_roomInfo->setClient(m_client.get());
    m_mediaBridge->setClient(m_client.get());
    m_pagination->setClient(m_client.get());
    m_pagination->setTimelineModel(m_timeline.get());
    m_readReceipts->setClient(m_client.get());
    m_readReceipts->setTimelineModel(m_timeline.get());
    // Reading a room clears its rows from the Activity Center bell. The read
    // receipt is the trigger because it is the moment the client tells the
    // server what was read; the server's read flag then seeds seenMark on the
    // next start.
    connect(m_readReceipts.get(), &ReadReceiptCoordinator::receiptSent, this,
            [this](const QString &roomId, const QString &, qint64 timestampMs) {
        if (m_activity)
            m_activity->markRoomReadUpTo(roomId, timestampMs);
    });

    // Push the room-activity visibility into both timeline models (the thread
    // panel uses the same delegate) so a date divider over a fully hidden run
    // is not rendered; QML cannot answer that without a per-row scan.
    const auto applyRoomActivityVisibility = [this]() {
        const bool shown = m_settings->showRoomActivity();
        m_timeline->setShowRoomActivity(shown);
        // The membership and profile sub-toggles must be pushed too, or the
        // dividers keep counting rows nobody can see.
        const bool members = m_settings->showMembershipEvents();
        const bool profiles = m_settings->showProfileChangeEvents();
        m_timeline->setShowMembershipEvents(members);
        m_timeline->setShowProfileChangeEvents(profiles);
        if (m_thread) {
            m_thread->model()->setShowRoomActivity(shown);
            m_thread->model()->setShowMembershipEvents(members);
            m_thread->model()->setShowProfileChangeEvents(profiles);
        }
    };
    applyRoomActivityVisibility();
    connect(m_settings.get(), &SettingsManager::showRoomActivityChanged,
            this, applyRoomActivityVisibility);
    connect(m_settings.get(), &SettingsManager::showMembershipEventsChanged,
            this, applyRoomActivityVisibility);
    connect(m_settings.get(), &SettingsManager::showProfileChangeEventsChanged,
            this, applyRoomActivityVisibility);
    m_linkPreviews->setClient(m_client.get());
    m_gifTransport->setClient(m_client.get());
    m_gifSend->setClient(m_client.get());
    m_stickers->setClient(m_client.get());

    // Link-preview policy follows settings live; encrypted rooms default OFF.
    m_linkPreviews->setAutoLoadUnencrypted(m_settings->autoLoadLinkPreviews());
    m_linkPreviews->setAllowEncrypted(m_settings->loadPreviewsInEncryptedRooms());
    connect(m_settings.get(), &SettingsManager::autoLoadLinkPreviewsChanged,
            this, [this]() {
        m_linkPreviews->setAutoLoadUnencrypted(m_settings->autoLoadLinkPreviews());
    });
    connect(m_settings.get(), &SettingsManager::loadPreviewsInEncryptedRoomsChanged,
            this, [this]() {
        m_linkPreviews->setAllowEncrypted(
            m_settings->loadPreviewsInEncryptedRooms());
    });

    // Wheel speed affects discrete mouse-wheel distance only, not touchpad or
    // programmatic scrolling.
    m_timelineScroll->setWheelSpeedValue(m_settings->timelineWheelSpeed());
    m_threadScroll->setWheelSpeedValue(m_settings->timelineWheelSpeed());
    connect(m_settings.get(), &SettingsManager::timelineWheelSpeedChanged,
            this, [this]() {
        m_timelineScroll->setWheelSpeedValue(m_settings->timelineWheelSpeed());
        m_threadScroll->setWheelSpeedValue(m_settings->timelineWheelSpeed());
    });

    // The read-receipt coordinator needs the real application activation
    // state; QML reports only timeline visibility and scroll position.
    if (auto *guiApp =
            qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        m_readReceipts->setWindowActive(guiApp->applicationState()
                                        == Qt::ApplicationActive);
        connect(guiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            m_readReceipts->setWindowActive(state == Qt::ApplicationActive);
        });
        // Orderly shutdown while the window and event dispatcher are still
        // valid, before the QML engine is destroyed.
        connect(guiApp, &QGuiApplication::aboutToQuit, this,
                &AppController::prepareForShutdown);
        // Repaint the "System" theme live when the platform preference
        // changes.
        if (auto *hints = guiApp->styleHints()) {
            connect(hints, &QStyleHints::colorSchemeChanged, this,
                    [this](Qt::ColorScheme) { Q_EMIT systemDarkModeChanged(); });
        }
        // Apply the persisted custom icon over the default main.cpp installed.
        applyAppIcon();
    }

    // A created or reused conversation opens once it is in the authoritative
    // room list.
    connect(m_conversations.get(), &ConversationController::conversationReady,
            this, &AppController::openRoom);
    // A created Space is selected, never opened as a timeline; clearing the
    // current room shows the Space Home.
    connect(m_conversations.get(), &ConversationController::spaceReady,
            this, [this](const QString &spaceId) {
        if (m_spaces)
            m_spaces->setActiveSpaceId(spaceId);
        setCurrentRoomId(QString());
    });
    connect(m_conversations.get(), &ConversationController::spacePlacementFailed,
            this, [this](const QString &) {
        Q_EMIT errorReported(
            tr("The room was created, but adding it to the Space failed."));
    });
    connect(m_conversations.get(), &ConversationController::avatarUploadFailed,
            this, [this](const QString &) {
        Q_EMIT errorReported(
            tr("The room was created, but setting its picture failed."));
    });
    // Any terminal sign-out outcome re-reads the device list; a tile
    // disappears only when the server says so.
    connect(m_uia.get(), &UiaController::signOutFinished, this,
            [this](bool, const QString &) { refreshSessionDevices(); });
    // A joined room opens once listed; a joined Space is selected like a
    // created one.
    connect(m_discovery.get(), &RoomDiscoveryController::roomJoined,
            this, [this](const QString &roomId) {
        // A join started from the upgrade banner and then abandoned must not
        // navigate the user back.
        if (m_roomUpgrade && m_roomUpgrade->consumeAbandonedJoin(roomId))
            return;
        openRoom(roomId);
    });
    connect(m_discovery.get(), &RoomDiscoveryController::spaceJoined,
            this, [this](const QString &spaceId) {
        if (m_spaces)
            m_spaces->setActiveSpaceId(spaceId);
        setCurrentRoomId(QString());
    });
    // The upgrade banner reuses Discover's join path; navigateRequested covers
    // an existing successor membership and the "Previous room" link. It fires
    // only from a user action; nothing moves the user on a tombstone alone.
    m_roomUpgrade->setDiscovery(m_discovery.get());
    // The upgrade flow can re-parent the replacement into the same Spaces.
    m_roomUpgrade->setSpaces(m_spaces.get());
    connect(m_roomUpgrade.get(), &RoomUpgradeController::navigateRequested,
            this, &AppController::openRoom);
    // `forwarded` fires only once the send was dispatched.
    connect(m_forward.get(), &ForwardController::forwarded,
            this, &AppController::openRoom);
    // The join gate learns from the member snapshot whether the server would
    // accept a call membership. It starts permitted (an unknown capability
    // must not disable Join) and refuses once power levels are known.
    connect(m_roomInfo.get(), &RoomInfoController::membersChanged, this,
            [this] {
                const QString roomId = m_roomInfo->roomId();
                if (!roomId.isEmpty()) {
                    m_rtc->setCanPublishMembership(
                        roomId, m_roomInfo->canPublishCallMembership());
                }
            });
    // canStartCall() is a Q_INVOKABLE reading asynchronous state, so the call
    // button's binding needs a revision to re-evaluate.
    //
    // The encryption resolver lets the call lane ask about any room, not just
    // one that was opened (the incoming-call card opens none). Unknown still
    // fails closed. Installed once and never cleared: it captures `this`, and
    // RtcController resets its own per-room record. roomInfo() is an indexed
    // lookup, unlike findRoom().
    connect(m_rtc.get(), &RtcController::availabilityChanged, this,
            [this] {
                ++m_callGateRevision;
                Q_EMIT callGateRevisionChanged();
            });
    connect(m_rtc.get(), &RtcController::sessionChanged, this,
            [this](const QString &) {
                ++m_callGateRevision;
                Q_EMIT callGateRevisionChanged();
            });
    m_rtc->setEncryptionResolver([this](const QString &roomId) {
        if (roomId.isEmpty() || !m_client)
            return RtcController::RoomEncryption::Unknown;
        const RoomInfo room = m_client->roomInfo(roomId);
        if (room.id.isEmpty() || !room.encryptionKnown)
            return RtcController::RoomEncryption::Unknown;
        return room.encrypted ? RtcController::RoomEncryption::Yes
                              : RtcController::RoomEncryption::No;
    });
    connect(m_roomInfo.get(), &RoomInfoController::roomLeft,
            this, [this](const QString &roomId) {
        if (m_currentRoomId == roomId) {
#ifdef ENABLE_RUST_SDK_BACKEND
            if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
                rust->closeRoomTimeline();
#endif
            setCurrentRoomId(QString());
        }
        if (m_roomInfo->roomId() == roomId)
            m_roomInfo->setRoomId(QString());
    });

    connect(m_auth.get(), &AuthManager::loginSucceeded,
            this, &AppController::onLoginSucceeded);
    connect(m_auth.get(), &AuthManager::loggedOut,
            this, &AppController::onLoggedOut);
    // A failed switch falls back to the previous account once, else the login
    // screen. A failed add-account restores the previous account in the
    // background, since the attempt released the shared client's session.
    connect(m_auth.get(), &AuthManager::loginFailed, this,
            [this](const QString &) {
        if (m_accountSwitching) {
            if (!m_switchFallbackUserId.isEmpty()) {
                failAccountSwitch(tr("Could not switch accounts — returning "
                                     "to the previous account."));
            } else {
                setAccountSwitching(false);
                setCurrentScreen(LoginScreen);
                Q_EMIT loggedInChanged();
            }
            return;
        }
        if (!m_addAccountReturnTo.isEmpty() && !m_client->isLoggedIn()
            && m_settings->hasSavedAccount(m_addAccountReturnTo)) {
            qCInfo(lcApp) << "add-account attempt failed — restoring"
                          << "slug=" << matrix::app_data::safeUserSlug(
                                 m_addAccountReturnTo);
            m_backgroundRestore = true;
            m_settings->setActiveAccountUserId(m_addAccountReturnTo);
            if (!m_client->restoreSession())
                m_backgroundRestore = false;
        }
        // A failed startup restore is the only case that shows the login form.
        if (m_currentScreen == BootScreen) {
            qCInfo(lcApp) << "startup restore failed — showing login";
            setCurrentScreen(LoginScreen);
            Q_EMIT loggedInChanged();
        }
    });
    // Cache the account's own profile for the switcher UI.
    connect(m_client.get(), &MatrixClient::userProfileFinished, this,
            [this](quint64, bool ok, const QString &userId,
                   const QString &displayName, const QString &avatarUrl,
                   const QString &) {
        if (!ok || userId.isEmpty() || userId != m_client->currentUserId())
            return;
        m_accounts->updateProfile(userId, displayName, avatarUrl);
    });
    // Terminal answer for one own-avatar write.
    connect(m_client.get(), &MatrixClient::ownAvatarChanged, this,
            [this](quint64 opId, bool ok, const QString &error) {
        // The op id is the only thing distinguishing this answer from a
        // previous account's.
        if (opId == 0 || opId != m_avatarOp)
            return;
        m_avatarOp = 0;
        if (!ok) {
            m_avatarError = error.isEmpty()
                ? tr("The picture could not be saved. Please try again.")
                : error;
            Q_EMIT ownAvatarStateChanged();
            return;
        }
        m_avatarError.clear();
        Q_EMIT ownAvatarStateChanged();
        // Re-fetch rather than writing locally: sync does not carry the own
        // profile and the server is the authority.
        const QString uid = m_client ? m_client->currentUserId() : QString{};
        if (!uid.isEmpty())
            m_client->fetchUserProfile(uid);
        Q_EMIT ownAvatarSaved();
    });

    connect(m_client.get(), &MatrixClient::ownDisplayNameChanged, this,
            [this](quint64 opId, bool ok, const QString &error) {
        // Drop answers for a previous account or a retired attempt; the op id
        // is the only guard.
        if (opId == 0 || opId != m_displayNameOp)
            return;
        m_displayNameOp = 0;
        if (!ok) {
            // Empty `error`: no usable server message, so use our own wording.
            m_displayNameError = error.isEmpty()
                ? tr("The display name could not be saved. Please try again.")
                : error;
            Q_EMIT ownDisplayNameStateChanged();
            return;
        }
        m_displayNameError.clear();
        Q_EMIT ownDisplayNameStateChanged();
        // Re-fetch rather than caching the submitted string: the server may
        // normalise or bound what it stored.
        const QString uid = m_client ? m_client->currentUserId() : QString{};
        if (!uid.isEmpty())
            m_client->fetchUserProfile(uid);
        Q_EMIT ownDisplayNameSaved();
    });
    // A plain sign-out bypasses clearCrossAccountCaches(), so retire the
    // writes here; the switch emits this too, which is harmless.
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            &AppController::retireOwnDisplayNameWrite);
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            &AppController::retireOwnAvatarWrite);
    // Hidden images are per account. resetForSession(), not clear(): clear()
    // writes and would erase the persisted list on sign-out.
    connect(m_client.get(), &MatrixClient::loggedOut, this, [this] {
        m_mediaVisibility->resetForSession();
    });
    connect(m_client.get(), &MatrixClient::errorOccurred,
            this, &AppController::errorReported);
    auto refreshConnectionStatus = [this]() {
        const MatrixClient::ConnectionState state = m_client->connectionState();
        switch (state) {
        case MatrixClient::Disconnected:
            setConnectionStatus(m_client->isLoggedIn()
                ? tr("Idle")
                : tr("Not connected"));
            break;
        case MatrixClient::Connecting:
            setConnectionStatus(tr("Connecting…"));
            break;
        case MatrixClient::Syncing:
            // "Loading rooms…" until initial sync completes, then the healthy
            // long-poll reads as "Connected" rather than "Syncing".
            setConnectionStatus(m_client->initialSyncDone()
                ? tr("Connected")
                : tr("Loading rooms…"));
            break;
        case MatrixClient::Error:
            setConnectionStatus(tr("Error"));
            break;
        case MatrixClient::Offline:
            setConnectionStatus(tr("Offline — retrying"));
            break;
        }
    };
    connect(m_client.get(), &MatrixClient::connectionStateChanged,
            this, refreshConnectionStatus);
    // Retry failed rule writes once per reconnection, on the edge into
    // Syncing: the signal can re-announce the same state. The state comes
    // from the signal argument, not the client's getter.
    connect(m_client.get(), &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState state) {
                if (state == MatrixClient::Syncing
                    && m_lastConnectionState
                           != static_cast<int>(MatrixClient::Syncing))
                    retryFailedNotificationModes();
                m_lastConnectionState = static_cast<int>(state);
            });
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, this, [this, refreshConnectionStatus] {
        refreshConnectionStatus();
        Q_EMIT initialSyncDoneChanged();
    });
    connect(m_client.get(), &MatrixClient::syncModeChanged,
            this, &AppController::syncModeChanged);
    setConnectionStatus(tr("Not connected"));

    // Recovery-key restore results and the redacted device id, bridged so
    // QML does not depend on the concrete backend.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
        connect(rust, &RustSdkMatrixClient::keyBackupResult,
                this, [this](const QString &state, const QString &message) {
            Q_EMIT recoveryStateChanged(state, message);
            // Retry decryption in place rather than rebuilding the room: a
            // reopen clears media, closes the open thread panel and
            // re-paginates, and its backup pass is capped where
            // retryDecryption()'s is not. keyBackupResult fires only for an
            // explicit user recovery.
            if (state == QLatin1String("ok") && !m_currentRoomId.isEmpty())
                retryDecryptionInCurrentRoom();
        });
        // The device id becomes available after login/restore; also snapshot
        // the SDK trust state so Settings shows the right label.
        connect(rust, &MatrixClient::loginSucceeded,
                this, [this, rust](const QString &) {
            Q_EMIT rustDeviceIdChanged();
            rust->refreshOwnDeviceStatus();
            // Check once at sign-in and keep the backstop running: the fault
            // can appear after login.
            requestOwnDeviceKeyCheck();
            if (!m_ownDeviceKeyTimer.isActive())
                m_ownDeviceKeyTimer.start();
            // Start a fresh crypto epoch for the new session, then capture it
            // at dispatch so a logout before the answer arrives rejects it.
            m_cryptoHealth->resetForNewGeneration();
            m_cryptoQueryGeneration = m_cryptoHealth->generation();
            rust->queryCryptoHealth();
        });
        // Re-read once sync has run: loginSucceeded fires before the first
        // /keys/query, so on a first sign-in the own identity is not known
        // yet and would latch as "Cross-signing unavailable", which never
        // prompts for verification.
        connect(rust, &MatrixClient::initialSyncDoneChanged, this, [this, rust] {
            if (!rust->initialSyncDone())
                return;
            rust->refreshOwnDeviceStatus();
            requestOwnDeviceKeyCheck();
            m_cryptoQueryGeneration = m_cryptoHealth->generation();
            rust->queryCryptoHealth();
        });
        // Sanitized health snapshots feed the read-only model; the generation
        // stamp drops answers from a previous session.
        m_cryptoHealth->setSupported(true);
        connect(rust, &RustSdkMatrixClient::cryptoHealthUpdated,
                this, [this](const QVariantMap &snapshot) {
            // Compare against the generation captured at dispatch, so a
            // session change in flight rejects the stale answer.
            m_cryptoHealth->applySnapshot(snapshot, m_cryptoQueryGeneration);
        });
        // Verified-session bootstrap status. The model also resets on
        // login/logout, so a previous account's bootstrap never describes
        // the current one.
        connect(rust, &RustSdkMatrixClient::cryptoBootstrapEvent,
                this, [this](const QString &kind, const QString &state,
                             quint64 count, quint64 inconclusive) {
            m_cryptoBootstrap->applyEvent(kind, state, count, inconclusive);
        });
        connect(rust, &MatrixClient::loginSucceeded, this,
                [this](const QString &) { m_cryptoBootstrap->reset(); });
        connect(rust, &MatrixClient::connectionStateChanged,
                this, [this](MatrixClient::ConnectionState state) {
            m_cryptoHealth->setSyncing(state == MatrixClient::Syncing);
        });
        connect(rust, &MatrixClient::loggedOut, this, [this] {
            m_cryptoHealth->resetForNewGeneration();
            m_cryptoBootstrap->reset();
            m_sessionDevices.clear();
            m_sessionDevicesLoading = false;
            m_sessionDevicesFailed = false;
            Q_EMIT sessionDevicesChanged();
        });
        // Device/session list: current session first, then most recently
        // seen.
        connect(rust, &RustSdkMatrixClient::deviceListUpdated,
                this, [this](bool ok, const QVariantList &devices) {
            QVariantList sorted = devices;
            std::sort(sorted.begin(), sorted.end(),
                      [](const QVariant &a, const QVariant &b) {
                const QVariantMap ma = a.toMap();
                const QVariantMap mb = b.toMap();
                const bool ca = ma.value(QStringLiteral("isCurrent")).toBool();
                const bool cb = mb.value(QStringLiteral("isCurrent")).toBool();
                if (ca != cb)
                    return ca;
                return ma.value(QStringLiteral("lastSeen")).toDateTime()
                       > mb.value(QStringLiteral("lastSeen")).toDateTime();
            });
            m_sessionDevices = sorted;
            m_sessionDevicesLoading = false;
            m_sessionDevicesFailed = !ok;
            Q_EMIT sessionDevicesChanged();
        });
        connect(rust, &MatrixClient::deviceRenamed, this,
                [this](quint64 opId, bool ok, const QString &category) {
            if (opId == 0 || opId != m_sessionDeviceRenameOp)
                return;
            m_sessionDeviceRenameOp = 0;
            m_sessionDeviceRenameError = ok
                ? QString()
                : (category == QLatin1String("forbidden")
                       ? tr("The server refused to rename this session.")
                       : tr("The session could not be renamed."));
            Q_EMIT sessionDevicesChanged();
            if (ok)
                refreshSessionDevices();
        });
        connect(this, &AppController::verificationStateChanged,
                this, [this] {
            m_cryptoHealth->setPendingVerificationCount(
                verificationActive() ? 1 : 0);
            // Surface incoming verification requests natively (identity only,
            // never message content or SAS data).
            static QString lastNotifiedFlow;
            if (m_verificationState == QLatin1String("requested")
                && !m_verificationFlowId.isEmpty()
                && m_verificationFlowId != lastNotifiedFlow
                && m_settings->notificationsEnabled()) {
                lastNotifiedFlow = m_verificationFlowId;
                m_notifications->showGeneric(
                    tr("Verification request"),
                    tr("%1 wants to verify a session. Open Lightning to "
                       "review it.").arg(m_verificationOtherUser));
            }
        });
        connect(rust, &MatrixClient::loggedOut,
                this, [this] {
            // The login screen must not inherit a stale verification or
            // import result.
            m_verificationFlowId.clear();
            m_verificationOtherUser.clear();
            m_verificationOtherDevice.clear();
            m_verificationIsSelf = false;
            m_verificationState.clear();
            m_verificationEmojis.clear();
            m_verificationDecimals.clear();
            // A displayed code belongs to the session that is going away.
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
            m_sessionTrustState = QStringLiteral("Unknown");
            m_sessionDeviceId.clear();
            m_ownIdentityAvailable = false;
            m_crossSigningAvailable = false;
            // Signing out and in again is the repair, so the fault does not
            // carry over.
            m_ownDeviceKeyTimer.stop();
            const bool wasBroken = m_ownDeviceKeyWatch.broken();
            m_ownDeviceKeyWatch.reset();
            if (wasBroken)
                Q_EMIT encryptionIdentityBrokenChanged();
            m_roomKeyImportState.clear();
            m_roomKeyImportImported = 0;
            m_roomKeyImportTotal = 0;
            m_roomKeyImportAffected = 0;
            m_roomKeyImportMessage.clear();
            m_roomKeyImportRunning = false;
            m_roomKeyImportAffectedRoomIds.clear();
            Q_EMIT securityStateChanged();
            Q_EMIT roomKeyImportStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::roomTimelineReloaded,
                this, [this](const QString &roomId, int t, int d, int u) {
            if (roomId == m_currentRoomId)
                Q_EMIT currentRoomTimelineReloaded(t, d, u);
        });
        // SAS verification bridge: all state lives here so QML never reaches
        // into backend types.
        connect(rust, &RustSdkMatrixClient::verificationRequestReceived,
                this, [this](const QString &flowId, const QString &otherUser,
                             const QString &otherDevice, bool isSelf) {
            m_verificationFlowId = flowId;
            m_verificationOtherUser = otherUser;
            m_verificationOtherDevice = otherDevice;
            m_verificationIsSelf = isSelf;
            m_verificationState = QStringLiteral("requested");
            m_verificationEmojis.clear();
            m_verificationDecimals.clear();
            // A different flow's code must never survive into this card.
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });
        // Both sides are ready. Progress only: it may advance the pre-emoji
        // states but never pull a flow back from a later one.
        connect(rust, &RustSdkMatrixClient::verificationReady,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState != QLatin1String("requested")
                && m_verificationState != QLatin1String("starting")
                && m_verificationState
                       != QLatin1String("waiting_for_other_session"))
                return;
            m_verificationState = QStringLiteral("ready");
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationSasReady,
                this, [this](const QString &flowId,
                             const QVariantList &emojis,
                             const QVariantList &decimals) {
            if (flowId != m_verificationFlowId) return;
            m_verificationEmojis = emojis;
            m_verificationDecimals = decimals;
            m_verificationState = QStringLiteral("sas_ready");
            Q_EMIT verificationStateChanged();
        });
        // The SDK registered our "They match"; Done still needs the peer.
        // Only "confirming" advances, so a late poll cannot resurrect a
        // finished flow.
        connect(rust, &RustSdkMatrixClient::verificationSasConfirmed,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState != QLatin1String("confirming")) return;
            m_verificationState = QStringLiteral("waiting_for_peer");
            Q_EMIT verificationStateChanged();
        });
        // Show-QR leg, orthogonal to m_verificationState: the QR is another
        // presentation of the same flow. All handlers are flow-scoped, and a
        // late grid never repaints a finished verification.
        connect(rust, &RustSdkMatrixClient::verificationQrReady,
                this, [this](const QString &flowId, int modules,
                             const QByteArray &bits) {
            if (flowId.isEmpty() || flowId != m_verificationFlowId) return;
            if (m_verificationState == QLatin1String("done")
                || m_verificationState == QLatin1String("cancelled")
                || m_verificationState.startsWith(QLatin1String("failed")))
                return;
            // Opaque, per-code, not derived from the flow id.
            const QString token =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!m_qrCodeStore.setCode(token, modules, bits)) {
                // Unrenderable geometry: show no QR; the flow continues on SAS.
                return;
            }
            m_verificationQrToken = token;
            m_verificationQrScanned = false;
            m_verificationQrConfirming = false;
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationQrScanned,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationQrToken.isEmpty()) return;
            m_verificationQrScanned = true;
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationQrConfirmed,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationQrToken.isEmpty()) return;
            // Progress, not success: only verificationDone reports that.
            m_verificationQrConfirming = true;
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationQrDismissed,
                this, [this](const QString &flowId, const QString &) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationQrToken.isEmpty()) return;
            // The peer chose emoji or the display window elapsed: fall back
            // to SAS.
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationDone,
                this, [this, rust](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("done");
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
            // Re-query SDK trust; only the SDK snapshot may promote to
            // "Verified".
            rust->refreshOwnDeviceStatus();
            requestOwnDeviceKeyCheck();
            // Keys shared by verified peers may now decrypt earlier events.
            if (!m_currentRoomId.isEmpty()) {
                qCInfo(lcApp) << "verification=done; retrying decryption in"
                              << matrix::e2ee::redactId(m_currentRoomId);
                // In place, as in the keyBackupResult handler. Fires once per
                // verification flow.
                retryDecryptionInCurrentRoom();
            }
            // Keep the security pane's health card coherent with the
            // just-verified state.
            refreshCryptoHealth();
        });
        // A completed verification is final for its flow: a late cancel or
        // failure with the same flow id must not repaint it. Current ordering
        // prevents that already, but this does not rely on it. Trust itself
        // always comes from SDK state, never from this string.
        connect(rust, &RustSdkMatrixClient::verificationCancelled,
                this, [this](const QString &flowId, const QString &) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState == QLatin1String("done")) return;
            m_verificationState = QStringLiteral("cancelled");
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationFailed,
                this, [this](const QString &flowId, const QString &msg) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState == QLatin1String("done")) return;
            m_verificationState = QStringLiteral("failed:%1").arg(msg);
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });

        // Outbound-initiated verification: same state cache as the
        // receive-first handler, with the direction flag flipped.
        connect(rust, &RustSdkMatrixClient::verificationRequestStarted,
                this, [this](const QString &flowId,
                             const QString &otherUser,
                             bool isSelf) {
            m_verificationFlowId = flowId;
            m_verificationOtherUser = otherUser;
            m_verificationOtherDevice.clear();
            m_verificationIsSelf = isSelf;
            m_verificationState = QStringLiteral("waiting_for_other_session");
            m_verificationEmojis.clear();
            m_verificationDecimals.clear();
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });

        // Cross-signing snapshot and room-key import lifecycle.
        //
        // The label must read `deviceCrossSigned`, not Device::is_verified():
        // matrix-sdk marks our own device locally trusted when it creates it,
        // so is_verified() is always true here and would also suppress the
        // verify-this-session prompt (sessionVerificationNeeded()).
        connect(rust, &RustSdkMatrixClient::ownDeviceStatusUpdated,
                this, [this](const QString &deviceId,
                             bool ownIdentityAvailable,
                             bool /*ownIdentityVerified*/,
                             bool deviceCrossSigned,
                             bool hasMaster,
                             bool hasSelf,
                             bool hasUser) {
            m_sessionDeviceId = deviceId;
            m_ownIdentityAvailable = ownIdentityAvailable;
            m_crossSigningAvailable = hasMaster || hasSelf || hasUser
                                      || ownIdentityAvailable;
            if (!ownIdentityAvailable) {
                m_sessionTrustState = QStringLiteral("Cross-signing unavailable");
            } else if (deviceCrossSigned) {
                m_sessionTrustState = QStringLiteral("Verified");
                // A dismissal applied to an unverified session; reset it so
                // a future unverified session warns again.
                if (m_settings)
                    m_settings->setVerificationWarningDismissed(false);
            } else {
                m_sessionTrustState = QStringLiteral("Not verified");
            }
            Q_EMIT securityStateChanged();
            Q_EMIT rustDeviceIdChanged();
        });
        // Surface a device whose identity key cannot decrypt anything. The
        // latch lives in OwnDeviceKeyWatch so "could not be established" is
        // never presented as "your encryption is destroyed".
        connect(rust, &RustSdkMatrixClient::ownDeviceIdentityKeyChecked,
                this, [this](bool established, bool matchesServer) {
            if (!established) {
                applyOwnDeviceKeyAgreement(
                    matrix::crypto::KeyAgreement::Unknown);
                return;
            }
            applyOwnDeviceKeyAgreement(
                matchesServer ? matrix::crypto::KeyAgreement::Matches
                              : matrix::crypto::KeyAgreement::Mismatch);
        });
        connect(rust, &RustSdkMatrixClient::roomKeyImportStarted,
                this, [this] {
            m_roomKeyImportState = QStringLiteral("importing");
            m_roomKeyImportImported = 0;
            m_roomKeyImportTotal = 0;
            m_roomKeyImportAffected = 0;
            m_roomKeyImportMessage.clear();
            m_roomKeyImportRunning = true;
            m_roomKeyImportAffectedRoomIds.clear();
            Q_EMIT roomKeyImportStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::roomKeyImportProgress,
                this, [this](int imported, int total) {
            m_roomKeyImportImported = imported;
            m_roomKeyImportTotal = total;
            Q_EMIT roomKeyImportStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::roomKeyImportDone,
                this, [this, rust](int imported, int total, int affected,
                                   const QStringList &roomIds) {
            m_roomKeyImportImported = imported;
            m_roomKeyImportTotal = total;
            m_roomKeyImportAffected = affected;
            m_roomKeyImportAffectedRoomIds = roomIds;
            m_roomKeyImportMessage.clear();
            m_roomKeyImportRunning = false;
            m_roomKeyImportState = QStringLiteral("done");
            Q_EMIT roomKeyImportStateChanged();
            Q_EMIT roomKeyImportCompleted(imported, total, affected);
            // No reload needed: Rust retries decryption on the SDK timeline
            // and rows update in place; roomKeysApplied() reports it. Import
            // never affects trust state.
            Q_UNUSED(rust);
        });
        // Post-import retry finished on the open timeline. The message claims
        // the keys were applied, not that every event decrypted.
        connect(rust, &RustSdkMatrixClient::roomKeysApplied,
                this, [this](const QString &roomId, int sessionCount) {
            Q_UNUSED(sessionCount);
            if (roomId != m_currentRoomId)
                return;
            m_roomKeyImportMessage =
                tr("Imported room keys applied to the open timeline.");
            Q_EMIT roomKeyImportStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::roomKeyImportFailed,
                this, [this](const QString &category, const QString &message) {
            m_roomKeyImportRunning = false;
            m_roomKeyImportState = QStringLiteral("failed");
            // Categorized safe message; never the raw passphrase.
            if (category == QLatin1String("bad_passphrase")) {
                m_roomKeyImportMessage = tr(
                    "The passphrase is incorrect or the key export is corrupted.");
            } else if (category == QLatin1String("invalid_file")) {
                m_roomKeyImportMessage = tr(
                    "The selected file is not a supported encrypted Matrix "
                    "room-key export.");
            } else if (category == QLatin1String("read_failed")) {
                m_roomKeyImportMessage = tr(
                    "Lightning could not read the selected file.");
            } else if (category == QLatin1String("already_running")) {
                m_roomKeyImportMessage = tr(
                    "A room-key import is already in progress.");
            } else if (category == QLatin1String("not_signed_in")) {
                m_roomKeyImportMessage = tr("Not signed in.");
            } else {
                m_roomKeyImportMessage = tr("Room-key import failed.");
            }
            Q_UNUSED(message);
            Q_EMIT roomKeyImportStateChanged();
        });

        connect(rust, &RustSdkMatrixClient::localSessionResetRequired,
                this, [this](const QString &reasonCode, const QString &userId,
                             const QString &homeserver) {
            // Record which account failed and why; QML chooses the copy and
            // the valid actions.
            setLocalSessionFailure(reasonCode, userId, homeserver);
            setLocalRustResetRequired(true);
        });
        // Same payload for conditions a local reset cannot repair (missing
        // store, revoked token, contested ownership): the destructive action
        // is never armed, since deleting local data would not help and could
        // throw away the only copy of room keys.
        connect(rust, &RustSdkMatrixClient::localSessionBlocked,
                this, [this](const QString &reasonCode, const QString &userId,
                             const QString &homeserver) {
            setLocalSessionFailure(reasonCode, userId, homeserver);
            setLocalRustResetRequired(false);
        });
        connect(rust, &RustSdkMatrixClient::localSessionCleanupFinished,
                this, [this](bool ok, const QString &message) {
            setLocalRustResetRequired(!ok);
            if (ok)
                clearLocalSessionFailure();
            if (m_resetResultPending) {
                m_resetResultPending = false;
                Q_EMIT localRustStoreResetResult(ok, message);
            }
        });
    }
#endif

    // Startup with a saved account is a restoration state: the login form
    // must not instantiate until the outcome is known. Success reaches
    // MainScreen via loginSucceeded, every failure via loginFailed. The mock
    // restores from its account registry. Screenshot-demo mode runs its own
    // restore from beginScreenshotDemo, so this one must not race it.
    const bool hasRestorableSession = !m_screenshotDemo
        && (m_settings->hasSession()
            || (m_backend == MockBackend
                && !m_settings->activeAccountUserId().isEmpty()));
    if (hasRestorableSession) {
        setCurrentScreen(BootScreen);
        if (!m_client->restoreSession())
            setCurrentScreen(LoginScreen);
        Q_EMIT rustDeviceIdChanged();
    }
}

AppController::~AppController() = default;

bool AppController::notificationActionIsForCurrentAccount(
    const QString &accountUserId)
{
    const QString current = m_client ? m_client->currentUserId() : QString();
    if (!current.isEmpty() && accountUserId == current)
        return true;
    // Not silent: the user believes they have replied. The notice names no
    // room or message, since we are no longer signed in as the account that
    // could see them.
    qCWarning(lcApp) << "notification action refused: account changed";
    // Gated like every other generic notice; the refusal itself still
    // happens.
    if (m_notifications && m_settings->notificationsEnabled()) {
        m_notifications->showGeneric(
            tr("Lightning"),
            tr("That notification was for a different account, so nothing "
               "was sent. Switch back to that account and try again."));
    }
    return false;
}

void AppController::applyStrictDeviceTrust()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    // The symbol exists only in a Rust-enabled build.
    RustSdkMatrixClient::setStrictDeviceTrust(m_settings->strictDeviceTrust());
#endif
}

void AppController::applyPrivacyPreferences()
{
    // Both settings govern what leaves the device while reading and typing,
    // and must be applied on change and on client attachment (a fresh bridge
    // starts permissive).
    if (m_client)
        m_client->setReadReceiptPrivacy(m_settings->readReceiptMode());
    if (m_composer)
        m_composer->setTypingNotificationsEnabled(
            m_settings->sendTypingNotifications());
}

void AppController::prepareForShutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;

    // Stop the Qt Multimedia players first: on Windows their Media Foundation
    // threads deliver state through queued signals that must not run into
    // teardown.
    if (m_playback)
        m_playback->stopAll();

    // Leave the call before stopSync(): the membership retraction and the SFU
    // Leave both need the client, and otherwise we linger in the call as a
    // ghost participant. Not left to ~SfuCallController, whose timing depends
    // on member destruction order. leave() is safe in any state, including
    // mid-join; the client's bounded Rust task join gives the sends time to
    // land.
    if (m_groupCall)
        m_groupCall->leave();

    // Stop the sync loop so no further backend callback is scheduled during
    // teardown.
    if (m_client)
        m_client->stopSync();
}

bool AppController::loggedIn() const
{
    return m_auth && m_auth->isLoggedIn();
}

QString AppController::backendName() const
{
    switch (m_backend) {
    case MockBackend: return QStringLiteral("mock");
    case RustBackend: return QStringLiteral("rust");
    case HttpBackend:
    default:          return QStringLiteral("http");
    }
}

bool AppController::serverRoomNotificationModes() const
{
    return m_client && m_client->supportsServerNotificationModes();
}

void AppController::setRoomNotificationMode(const QString &roomId, int mode)
{
    // The composite thread-timeline id must never reach settings keys or a
    // protocol call.
    if (roomId.isEmpty() || mode < 0 || mode > 3
        || MatrixClient::isThreadTimelineId(roomId))
        return;
    // Device-local value first: NotificationManager reads it, so the choice
    // applies instantly and offline. On server-capable backends it also
    // caches the push-rule mode.
    m_settings->setRoomNotificationMode(roomId, mode);
    if (m_client && m_client->supportsServerNotificationModes()) {
        // Mode 3 removes the room override; Matrix has no follow-default rule.
        if (mode == 3)
            m_client->clearRoomNotificationMode(roomId);
        else
            m_client->setRoomNotificationMode(roomId, mode);
    }
}

void AppController::retryFailedNotificationModes()
{
    if (!m_client || !m_client->supportsServerNotificationModes()
        || m_notificationModeSyncFailures.isEmpty())
        return;
    // Re-issue the persisted choice for every room whose write failed; the
    // local value is authoritative because nothing on the server has
    // contradicted it. Entries are cleared only when the server acknowledges
    // the value (roomNotificationModeChanged), never on attempt.
    const QList<QString> pending = m_notificationModeSyncFailures.values();
    for (const QString &roomId : pending) {
        const int mode = m_settings->roomNotificationMode(roomId);
        if (mode == 3)
            m_client->clearRoomNotificationMode(roomId);
        else
            m_client->setRoomNotificationMode(roomId, mode);
    }
    qCDebug(lcApp) << "retried room notification rules:" << pending.size();
    Q_EMIT roomNotificationModesRetried(static_cast<int>(pending.size()));
}

void AppController::requestRoomNotificationMode(const QString &roomId)
{
    if (roomId.isEmpty() || MatrixClient::isThreadTimelineId(roomId)
        || !m_client || !m_client->supportsServerNotificationModes())
        return;
    m_client->requestRoomNotificationMode(roomId);
}

void AppController::requestRoomBridgeInfo(const QString &roomId,
                                          bool allowNetwork)
{
    if (roomId.isEmpty() || MatrixClient::isThreadTimelineId(roomId)
        || !m_client || !m_client->supportsRoomBridges())
        return;
    // A remembered answer of either sign is an answer: m_bridgeReadRooms only
    // tracks this session, while the store survives restarts. A remembered
    // "no bridge" expires on its own shorter clock.
    if (m_bridgeLabels.knows(roomId))
        return;
    const auto seen = m_bridgeReadRooms.constFind(roomId);
    // A network read is final; a store-only read upgrades once when a caller
    // allows the network.
    if (seen != m_bridgeReadRooms.constEnd() && (*seen || !allowNetwork))
        return;
    // Record only a dispatched request: a synchronous rejection never answers.
    if (m_client->roomBridges(roomId, allowNetwork) != 0)
        m_bridgeReadRooms.insert(roomId, allowNetwork);
}

bool AppController::roomNotificationModeSyncFailed(const QString &roomId) const
{
    return m_notificationModeSyncFailures.contains(roomId);
}

bool AppController::startVoiceRecording(const QString &owner)
{
    // Only the two known composers may own the recorder; ownership is a send
    // authorisation.
    if (owner != QLatin1String("room") && owner != QLatin1String("thread"))
        return false;
    // Take ownership only after a successful start, and never from a live
    // recording: start() refuses while busy without emitting failed(), so
    // transferring first would orphan a running microphone.
    if (m_voiceRecorder
        && (m_voiceRecorder->recording() || m_voiceRecorder->processing()))
        return false;
    if (!m_voiceOwner.isEmpty() && m_voiceOwner != owner)
        return false;
    if (!voiceRecorder()->start())
        return false;
    if (m_voiceOwner != owner) {
        m_voiceOwner = owner;
        Q_EMIT voiceOwnerChanged();
    }
    return true;
}

void AppController::setVoiceRecorderForTest(VoiceRecorder *recorder)
{
    m_voiceOwner.clear();
    m_voiceRecorder.reset(recorder);
    Q_EMIT voiceOwnerChanged();
}

bool AppController::voiceRecordingBusy() const
{
    // True when a recording is in progress anywhere, so a composer can tell
    // "no microphone" from "the other composer is recording".
    return !m_voiceOwner.isEmpty()
        || (m_voiceRecorder
            && (m_voiceRecorder->recording() || m_voiceRecorder->processing()));
}

void AppController::endVoiceRecording()
{
    if (m_voiceOwner.isEmpty())
        return;
    m_voiceOwner.clear();
    Q_EMIT voiceOwnerChanged();
}

bool AppController::discardPreparedVoice(const QString &localPath)
{
    if (localPath.isEmpty() || !m_voiceRecorder)
        return false;
    if (!m_voiceRecorder->ownsPath(localPath))
        return false;
    return QFile::remove(localPath);
}

void AppController::cancelVoiceRecording()
{
    // Do not construct the recorder just to cancel: that starts the audio
    // backend.
    if (m_voiceOwner.isEmpty())
        return;
    if (m_voiceRecorder)
        m_voiceRecorder->cancel();
    m_voiceOwner.clear();
    Q_EMIT voiceOwnerChanged();
}

SettingsManager *AppController::settings() const { return m_settings.get(); }
ShortcutRegistry *AppController::shortcuts() const
{ return m_shortcuts.get(); }
LocalizationManager *AppController::localization() const
{ return m_localization.get(); }
CustomThemeStore *AppController::customTheme() const
{ return m_customTheme.get(); }
RailLayoutStore *AppController::railLayout() const
{ return m_railLayout.get(); }

RailEntryModel *AppController::railEntries() const
{ return m_railEntries.get(); }
ProfileBannerManager *AppController::banners() const
{ return m_banners.get(); }
NameColorManager *AppController::nameColors() const
{ return m_nameColors.get(); }
ProfileBioManager *AppController::bio() const
{ return m_bio.get(); }
ProfileBadges *AppController::badges() const
{ return m_badges.get(); }
AuthManager *AppController::auth() const { return m_auth.get(); }

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
// Map an account hint (preset name, alias or user id) onto one of the three
// fictional demo user ids.
static QString resolveDemoAccountId(const QString &hint)
{
    const QString h = hint.trimmed().toLower();
    if (h.isEmpty() || h == QLatin1String("personal") || h == QLatin1String("alex")
        || h == QLatin1String("@alex:lightning.example"))
        return QStringLiteral("@alex:lightning.example");
    if (h == QLatin1String("work") || h == QLatin1String("taylor")
        || h == QLatin1String("@taylor:workplace.example"))
        return QStringLiteral("@taylor:workplace.example");
    if (h == QLatin1String("community") || h == QLatin1String("nova")
        || h == QLatin1String("@nova:community.example"))
        return QStringLiteral("@nova:community.example");
    return QStringLiteral("@alex:lightning.example");
}
#endif

void AppController::beginScreenshotDemo(const QString &initialAccount)
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Mock backend only; fail closed on anything else.
    if (m_backend != MockBackend) {
        qCWarning(lcApp)
            << "beginScreenshotDemo ignored: active backend is not the mock";
        return;
    }
    // The demo must never run on a production secure secret store.
    if (m_secretStore && m_secretStore->isSecure())
        qFatal("screenshot-demo: refusing to run on a production secure "
               "SecretStore (libsecret/keychain must not be initialized)");
    m_screenshotDemoActive = true;

    // Tests never call this, so the shared mock fixtures stay unchanged.
    MockMatrixClient *mock = qobject_cast<MockMatrixClient *>(m_client.get());
    if (mock) {
        mock->setScreenshotDemoMode(true);
        if (!m_demoController)
            m_demoController = new ScreenshotDemoController(this, mock, this);
    }

    // Register the demo accounts as non-secret metadata only, under the
    // isolated demo profile; clear first so registration is deterministic.
    m_settings->clearDemoAccounts();
    struct DemoAcct { const char *hs; const char *uid; const char *name; const char *avatar; };
    static const DemoAcct kAccounts[] = {
        { "https://lightning.example", "@alex:lightning.example",
          "Alex Morgan", "mxc://lightning.example/avatar-alex" },
        { "https://workplace.example", "@taylor:workplace.example",
          "Taylor Reed", "mxc://lightning.example/avatar-taylor" },
        { "https://community.example", "@nova:community.example",
          "Nova", "mxc://lightning.example/avatar-nova" },
    };
    int order = 1;
    for (const auto &a : kAccounts) {
        m_settings->registerDemoAccount(
            QString::fromLatin1(a.hs), QString::fromLatin1(a.uid),
            QString::fromLatin1(a.name), QString::fromLatin1(a.avatar), order++);
    }

    QString activeUid = resolveDemoAccountId(initialAccount);
    if (!m_settings->hasSavedAccount(activeUid))
        activeUid = QStringLiteral("@alex:lightning.example");
    m_settings->setActiveAccountUserId(activeUid);

    // Show the boot surface while the mock restores, then reach MainScreen via
    // the normal loginSucceeded path.
    setCurrentScreen(BootScreen);
    if (!m_client->restoreSession()) {
        // Fall back to a direct mock login so the demo still boots.
        const QVariantMap rec = m_settings->accountRecord(activeUid);
        m_auth->login(rec.value(QStringLiteral("homeserver")).toString(),
                      activeUid.section(QLatin1Char(':'), 0, 0).mid(1),
                      QStringLiteral("demo"));
    }
#endif
}
void AppController::applyDemoLaunchOptions(const QString &scenario,
                                           const QString &theme,
                                           const QString &appearance,
                                           const QString &size,
                                           bool hideControls)
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    if (auto *d = qobject_cast<ScreenshotDemoController *>(m_demoController))
        d->applyLaunchOptions(scenario, theme, appearance, size, hideControls);
#else
    Q_UNUSED(scenario); Q_UNUSED(theme); Q_UNUSED(appearance);
    Q_UNUSED(size); Q_UNUSED(hideControls);
#endif
}

AccountManager *AppController::accounts() const { return m_accounts.get(); }
RoomListModel *AppController::roomList() const { return m_roomList.get(); }
RoomListModel *AppController::allRooms() const { return m_allRooms.get(); }
SpaceChannelModel *AppController::spaceChannels() const
{ return m_spaceChannels.get(); }
QuickSwitcherModel *AppController::quickSwitcher() const
{ return m_quickSwitcher.get(); }
TimelineModel *AppController::timeline() const { return m_timeline.get(); }
QAbstractItemModel *AppController::timelineView() const
{ return m_timelineView.get(); }
MessageComposer *AppController::composer() const { return m_composer.get(); }
QObject *AppController::richComposer() const { return m_richComposer.get(); }
QObject *AppController::backup() const { return m_backup.get(); }
QObject *AppController::scheduledSends() const { return m_scheduledSends.get(); }
QObject *AppController::activity() const { return m_activity.get(); }

QObject *AppController::mediaHistory() const
{
    return m_mediaHistory.get();
}
MediaManager *AppController::media() const { return m_media.get(); }
CryptoManager *AppController::crypto() const { return m_crypto.get(); }
SpaceManager *AppController::spaces() const { return m_spaces.get(); }
ThreadManager *AppController::threads() const { return m_threads.get(); }
PresenceManager *AppController::presence() const { return m_presence.get(); }
CallController *AppController::calls() const { return m_calls.get(); }

QString AppController::preferredCallLane(const QString &roomId) const
{
    if (roomId.isEmpty())
        return QString();
    // MatrixRTC first: it is what current Element speaks, and it carries
    // video, screen share and groups.
    if (m_rtc && m_rtc->joinBlock(roomId) == RtcController::JoinBlock::None)
        return QStringLiteral("matrixrtc");
    // Legacy fallback, 1:1 DMs only: m.call.invite rings every room member.
    if (m_calls && m_calls->mediaBackendAvailable()
        && !legacyCallPeer(roomId).isEmpty()) {
        return QStringLiteral("legacy");
    }
    return QString();
}

QString AppController::legacyCallPeer(const QString &roomId) const
{
    // `isDirect` only means m.direct lists a target; the legacy lane needs
    // exactly one peer, since the invite and every later signal are bound to
    // them.
    if (!m_roomList)
        return QString();
    const QVariantMap room = m_roomList->findRoom(roomId);
    if (!room.value(QStringLiteral("isDirect")).toBool())
        return QString();
    const QStringList targets =
        room.value(QStringLiteral("directUserIds")).toStringList();
    if (targets.size() != 1 || targets.first().isEmpty())
        return QString();
    return targets.first();
}

bool AppController::canStartCall(const QString &roomId) const
{
    return !preferredCallLane(roomId).isEmpty();
}

bool AppController::startCall(const QString &roomId, bool withVideo)
{
    // Re-read the room's encryption now rather than trusting the value
    // recorded on navigation, which may predate `m.room.encryption` arriving
    // and would let the call join in cleartext. Unknown still fails closed.
    if (!roomId.isEmpty() && m_rtc && m_roomList) {
        const QVariantMap room = m_roomList->findRoom(roomId);
        // Record only a known answer; see setCurrentRoomId().
        const bool known =
            room.value(QStringLiteral("encryptionKnown")).toBool();
        if (known) {
            m_rtc->setRoomEncrypted(
                roomId, room.value(QStringLiteral("encrypted")).toBool());
        }
    }
    const QString lane = preferredCallLane(roomId);
    // Room ids are structural, not content, and are safe to log.
    qCInfo(lcApp) << "call start requested lane=" << lane
                  << "video=" << withVideo
                  << "rtcBlock="
                  << (m_rtc ? m_rtc->joinBlockReason(roomId)
                            : QStringLiteral("<no controller>"));
    if (lane == QLatin1String("matrixrtc")) {
        const bool ok = m_groupCall->join(roomId, withVideo);
        qCInfo(lcApp) << "matrixrtc join dispatched ok=" << ok;
        return ok;
    }
    if (lane == QLatin1String("legacy")) {
        if (withVideo) {
            // The legacy lane is audio-only.
            Q_EMIT callStartRefused(
                tr("Video calls need a MatrixRTC service, which isn't "
                   "available here yet."));
            return false;
        }
        const bool ok = m_calls->placeCall(roomId, legacyCallPeer(roomId));
        qCInfo(lcApp) << "legacy call dispatched ok=" << ok;
        return ok;
    }

    // Neither lane: report the specific reason, since each has a different
    // remedy.
    if (m_rtc) {
        const QString reason = m_rtc->joinBlockReason(roomId);
        if (reason == QLatin1String("media_encryption_unavailable")) {
            Q_EMIT callStartRefused(
                tr("This room is encrypted, and encrypted calls aren't "
                   "available yet on this build."));
            return false;
        }
        if (reason == QLatin1String("no_transport")) {
            Q_EMIT callStartRefused(
                tr("Calling isn't available on this homeserver because no "
                   "MatrixRTC service is configured."));
            return false;
        }
        if (reason == QLatin1String("undiscovered")) {
            Q_EMIT callStartRefused(
                tr("Still checking whether calling is available…"));
            return false;
        }
    }
    Q_EMIT callStartRefused(tr("Calling isn't available here."));
    return false;
}
RtcController *AppController::rtc() const { return m_rtc.get(); }
SfuCallController *AppController::groupCall() const
{
    return m_groupCall.get();
}

CallDeviceController *AppController::callDevices() const
{
    return m_callDevices.get();
}

CallSoundController *AppController::callSounds() const
{
    return m_callSounds.get();
}

void AppController::enableCallSounds()
{
    if (m_callSounds->sink())
        return;
    // Cues follow the call's chosen speaker, read live on every cue.
    QPointer<SettingsManager> settings = m_settings.get();
    m_callSounds->setSink(std::make_unique<CallSoundPlayer>([settings] {
        return settings ? settings->preferredSpeakerId() : QString();
    }));
}

void AppController::enableCallMediaEngine()
{
    // Called from main.cpp for the real application only, never from the
    // constructor, so tests never run gst_init or depend on plugin
    // availability.
#ifdef HAVE_LIGHTNING_WEBRTC
    if (qEnvironmentVariableIsSet("LIGHTNING_DISABLE_WEBRTC")) {
        qCInfo(lcApp) << "voice-call media engine disabled by environment";
        return;
    }
    QString whyNot;
    if (GstCallMediaBackend::runtimeAvailable(&whyNot)) {
        auto *engine = new GstCallMediaBackend(this);
        m_calls->setMediaBackend(engine);
        // Apply the chosen devices and follow hotplugs of the active device.
        // The engine applies them per session, so a change lands on the next
        // call.
        const auto applyDevices = [this, engine] {
            engine->setAudioDevices(m_callDevices->microphoneElement(),
                                    m_callDevices->speakerElement());
        };
        // Only when a preference exists: microphoneElement() initialises Qt
        // Multimedia, which costs startup time and logs a SPA parse error per
        // device on PipeWire. Without a preference the engine's empty strings
        // already mean the platform default.
        const bool hasDevicePreference =
            m_settings
            && (!m_settings->preferredMicrophoneId().isEmpty()
                || !m_settings->preferredSpeakerId().isEmpty());
        if (hasDevicePreference)
            applyDevices();
        connect(m_callDevices.get(),
                &CallDeviceController::activeDevicesChanged, engine,
                applyDevices);
        qCInfo(lcApp) << "voice-call media engine active (webrtcbin)";
    }
    // The SFU engine probes a wider element set (video, screen capture), so
    // it can be unavailable where the 1:1 engine is fine.
    QString sfuWhyNot;
    if (SfuMediaEngine::runtimeAvailable(&sfuWhyNot)) {
        auto *sfu = new SfuMediaEngine(this);
        m_groupCall->setMediaEngine(sfu);
        // Chosen camera, microphone and speaker for the MatrixRTC lane,
        // applied per publish so a change lands on the next capture.
        const auto applySfuDevices = [this, sfu] {
            const auto camera = m_callDevices->cameraSelection();
            const auto microphone = m_callDevices->microphoneSelection();
            const auto speaker = m_callDevices->speakerSelection();
            sfu->setPreferredDevices({camera.id, camera.description},
                                     {microphone.id, microphone.description},
                                     {speaker.id, speaker.description});
        };
        applySfuDevices();
        connect(m_callDevices.get(),
                &CallDeviceController::activeDevicesChanged, sfu,
                applySfuDevices);
        // SDP transport is opt-in at the Rust edge and shared with the 1:1
        // lane. Set it here so this lane does not silently depend on the other
        // engine registering: without it every SFU offer, answer and candidate
        // would be discarded while the call looked connected.
        if (m_client)
            m_client->setCallMediaCapable(true);
        // Until an engine exists the join gate reports NoMediaTransport.
        m_rtc->setMediaAvailable(true);
        // Frame encryption is available (CallFrameCryptor between encoder and
        // payloader), so encrypted rooms are allowed. Whether a given call
        // encrypts is SfuCallController's `mediaEncrypted`.
        m_rtc->setMediaEncryptionAvailable(true);
        // Screen sharing goes through the desktop portal, so the compositor
        // owns the picker. Registered only when a portal answers.
        if (ScreenCastPortal::available()) {
            m_groupCall->setScreenCastPortal(new ScreenCastPortal(this));
            qCInfo(lcApp) << "screen-share portal available";
        } else {
            qCInfo(lcApp) << "screen-share portal unavailable";
        }
        // The camera portal is registered whenever it answers.
        // SfuCallController::linuxCameraRoute() prefers a visible direct
        // device, so this changes nothing on a desktop; inside a Flatpak it is
        // the only camera.
        if (CameraPortal::available()) {
            m_groupCall->setCameraPortal(new CameraPortal(this));
            qCInfo(lcApp) << "camera portal available present="
                          << CameraPortal::cameraPresent();
        } else {
            qCInfo(lcApp) << "camera portal unavailable";
        }
        qCInfo(lcApp) << "group-call media engine active (webrtcbin/SFU)";
    } else {
        qCInfo(lcApp) << "group-call media engine unavailable:" << sfuWhyNot;
    }
    if (!GstCallMediaBackend::runtimeAvailable(&whyNot)) {
        // Coarse reason only (element name), safe to log.
        qCInfo(lcApp) << "voice-call media engine unavailable:" << whyNot;
    }
#else
    qCInfo(lcApp) << "voice-call media engine not built into this binary";
#endif
}

bool AppController::sessionVerificationNeeded() const
{
    // Only "Not verified" is actionable: "Unknown" is undetermined, and
    // without cross-signing there is nothing to verify against.
    return m_client && m_client->isLoggedIn()
        && m_cryptoHealth && m_cryptoHealth->cryptoSupported()
        && m_sessionTrustState == QLatin1String("Not verified");
}

bool AppController::sessionVerificationWarning() const
{
    return sessionVerificationNeeded() && m_settings
        && !m_settings->verificationWarningDismissed();
}

void AppController::dismissVerificationWarning()
{
    if (m_settings)
        m_settings->setVerificationWarningDismissed(true);
}

bool AppController::initialSyncDone() const
{
    return m_client && m_client->initialSyncDone();
}

bool AppController::systemDarkMode() const
{
    if (auto *hints = QGuiApplication::styleHints())
        return hints->colorScheme() == Qt::ColorScheme::Dark;
    return false;
}

QString AppController::syncModeLabel() const
{
    if (!m_client || m_backend != RustBackend) return {};
    const QString mode = m_client->syncMode();
    if (mode == QLatin1String("sliding_sync")) return tr("Modern room list");
    if (mode == QLatin1String("classic_fallback")) return tr("Compatibility mode");
    if (mode == QLatin1String("probing")) return tr("Checking server support…");
    return {};
}

QString AppController::rustDeviceIdRedacted() const
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client)
        return {};
    auto *rust = qobject_cast<const RustSdkMatrixClient *>(m_client.get());
    if (!rust) return {};
    const QString id = rust->currentDeviceId().trimmed();
    if (id.isEmpty()) return {};
    if (id.size() <= 8) return id;
    return id.left(4) + QLatin1String("...") + id.right(4);
#else
    return {};
#endif
}

void AppController::requestRecoverFromBackup(const QString &recoveryKey)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Recovery is only available on the Rust backend."));
        return;
    }
    if (recoveryKey.trimmed().isEmpty()) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Recovery key is empty."));
        return;
    }
    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    if (!rust) {
        Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
            tr("Rust backend not available."));
        return;
    }
    // Emit "attempted" immediately for button feedback; the SDK's own
    // attempted event follows and QML handles it idempotently.
    Q_EMIT recoveryStateChanged(QStringLiteral("attempted"), QString());
    rust->recoverFromBackup(recoveryKey);
#else
    Q_UNUSED(recoveryKey);
    Q_EMIT recoveryStateChanged(QStringLiteral("failed"),
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::setCurrentRoomId(const QString &roomId)
{
    if (m_currentRoomId == roomId)
        return;
    m_currentRoomId = roomId;
    // Close the thread panel before the timeline switches rooms.
    m_thread->handleCurrentRoomChanged(roomId);
    // Inline media playback never survives a room switch.
    m_playback->stopAll();
    // Queued speculative fetches belong to the destroyed delegates; dropping
    // them keeps a heavy GIF room from starving the next room's media.
    m_mediaBridge->dropQueuedSpeculative();
    m_timeline->setRoomId(roomId);
    m_composer->setRoomId(roomId);
    m_pagination->setRoomId(roomId);
    // Pinned messages, stickers and upgrades follow the active room.
    m_pinned->setRoomId(roomId);
    // Marks the sticker snapshot stale without a request; the picker
    // refreshes when it opens, so navigation costs no /state reads.
    m_stickers->setActiveRoomId(roomId);
    m_roomUpgrade->setRoomId(roomId);
    // Read the room's call session so the banner is right on arrival.
    if (!roomId.isEmpty()) {
        m_rtc->refresh(roomId);
        // Feed the room's encryption state to the join gate.
        const QVariantMap room = m_roomList->findRoom(roomId);
        // Record only a known answer. An unknown room already fails closed
        // (an absent entry reads as encrypted); recording it as encrypted
        // would latch and block the real answer via the downgrade guard.
        const bool known =
            room.value(QStringLiteral("encryptionKnown")).toBool();
        if (known) {
            m_rtc->setRoomEncrypted(
                roomId, room.value(QStringLiteral("encrypted")).toBool());
        }
    }
    // Drop queued thread-participant fetches for the room just left.
    m_threads->setActiveRoom(roomId);
    // Keep the open room visible under the Unreads filter.
    m_roomList->setPinnedRoomId(roomId);
    // Disarm mention suggestions, or the previous room's roster keeps
    // refetching on every membership event.
    m_mentionSuggestions->setRoomId(QString());
    // Hydrate the member roster on first open so display names resolve in
    // mentions, replies and thread summaries. Once per room per session; a
    // failed fetch un-marks the room so the next open retries.
    if (!roomId.isEmpty() && m_client
        && !m_memberHydratedRooms.contains(roomId)) {
        // Record only a dispatched request: a synchronous rejection returns 0
        // without ever emitting roomMembersReceived.
        if (m_client->requestRoomMembers(roomId) != 0)
            m_memberHydratedRooms.insert(roomId);
    }
    // MSC2346 bridge state, once per room per session, only for a room the
    // user opened (never from the room list, whose badge is computed in
    // data()). /state grows with membership, so above 500 loaded members the
    // eager read is store-only and the room info panel fetches on demand.
    // Spaces are skipped: they cannot be bridged.
    if (!roomId.isEmpty() && m_client) {
        constexpr int kEagerBridgeMemberCeiling = 500;
        bool isSpace = false;
        int loadedMembers = 0;
        for (const auto &info : m_client->rooms()) {
            if (info.id != roomId)
                continue;
            isSpace = info.isSpace;
            loadedMembers = static_cast<int>(info.members.size());
            break;
        }
        if (!isSpace) {
            requestRoomBridgeInfo(roomId,
                                  loadedMembers <= kEagerBridgeMemberCeiling);
        }
    }
    Q_EMIT currentRoomIdChanged();
}

void AppController::showLogin()
{
    // Entering login while signed in is the add-account flow; remember where
    // to return if it fails or the user goes back.
    if (m_client->isLoggedIn())
        m_addAccountReturnTo = m_settings->activeAccountUserId();
    // Leave a live call while its client is still attached: the retraction
    // cannot be sent once the client is released, which happens before
    // onLoginSucceeded() runs.
    if (m_groupCall && m_client->isLoggedIn())
        m_groupCall->leave();
    m_composer->setRoomId({});
    setCurrentScreen(LoginScreen);
}
void AppController::showMain()
{
    // Returning from add-account: a failed attempt released the shared
    // client's session, so restore the active account first.
    if (!m_client->isLoggedIn() && !m_accountSwitching) {
        if (!m_addAccountReturnTo.isEmpty()
            && m_settings->hasSavedAccount(m_addAccountReturnTo)) {
            m_settings->setActiveAccountUserId(m_addAccountReturnTo);
        }
        if (m_settings->hasSession())
            m_client->restoreSession();
    }
    m_addAccountReturnTo.clear();
    m_backgroundRestore = false;
    Q_EMIT errorReported(QString{});
    m_composer->setRoomId(m_currentRoomId);
    setCurrentScreen(MainScreen);
}
void AppController::showSettings()
{
    m_composer->setRoomId({});
    // Settings owns the whole content area: transient room-side surfaces
    // close and are not restored when it exits.
    if (m_thread) {
        m_thread->close();
        m_thread->closeList();
    }
    setCurrentScreen(SettingsScreen);
}

void AppController::showSettingsSection(const QString &section)
{
    m_requestedSettingsSection = section;
    showSettings();
    Q_EMIT settingsSectionRequested(section);
}

QString AppController::takeRequestedSettingsSection()
{
    const QString section = m_requestedSettingsSection;
    m_requestedSettingsSection.clear();
    return section;
}

namespace {
// The embedded default logo, identical to main.cpp's startup fallback.
const auto kDefaultIconResource =
    ":/qt/qml/MatrixClient/data/icons/hicolor/256x256/apps/lightning.png";
}

QString AppController::appIconSource() const
{
    if (m_settings && m_settings->customAppIconEnabled()) {
        const QString file = matrix::app_data::customAppIconFile();
        if (!file.isEmpty() && QFileInfo::exists(file)) {
            return QUrl::fromLocalFile(file).toString()
                   + QStringLiteral("?v=")
                   + QString::number(m_appIconRevision);
        }
    }
    return QLatin1String("qrc") + QLatin1String(kDefaultIconResource);
}

void AppController::applyAppIcon()
{
    if (m_settings && m_settings->customAppIconEnabled()) {
        const QString file = matrix::app_data::customAppIconFile();
        if (!file.isEmpty() && QFileInfo::exists(file)) {
            const QIcon icon(file);
            if (!icon.isNull()) {
                QGuiApplication::setWindowIcon(icon);
                return;
            }
        }
        // Unreadable copy: fall back visually without rewriting the stored
        // preference.
        qCWarning(lcApp) << "custom app icon enabled but unreadable;"
                         << "showing the default icon";
    }
    QGuiApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("lightning"),
        QIcon(QLatin1String(kDefaultIconResource))));
}

QString AppController::setCustomAppIconFromFile(const QUrl &fileUrl)
{
    if (!m_settings || !fileUrl.isLocalFile())
        return tr("Choose a local image file.");
    QFile in(fileUrl.toLocalFile());
    if (!in.open(QIODevice::ReadOnly))
        return tr("The image could not be read.");
    if (in.size() > appicon::kMaxInputBytes)
        return tr("The image is too large — 32 MiB at most.");
    // Bounded read even when size() lies (a FIFO reports 0).
    const QByteArray bytes = in.read(appicon::kMaxInputBytes + 1);
    if (bytes.size() > appicon::kMaxInputBytes)
        return tr("The image is too large — 32 MiB at most.");
    const appicon::NormalizeResult normalized = appicon::normalizeIconBytes(bytes);
    if (!normalized.ok) {
        if (normalized.category == QLatin1String("too_large_bytes"))
            return tr("The image is too large — 32 MiB at most.");
        if (normalized.category == QLatin1String("too_large_dimensions"))
            return tr("The image is too large — 8192×8192 at most.");
        if (normalized.category == QLatin1String("too_small"))
            return tr("The image is too small — 16×16 at least.");
        // svg_rejected / unsupported_format / decode_failed / empty
        return tr("Choose a PNG, JPEG, WebP, BMP or GIF image.");
    }
    const QString target = matrix::app_data::customAppIconFile();
    if (target.isEmpty())
        return tr("Application data storage is unavailable.");
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return tr("The icon could not be saved.");
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly))
        return tr("The icon could not be saved.");
    if (!normalized.image.save(&out, "PNG") || !out.commit())
        return tr("The icon could not be saved.");
    QFile::setPermissions(target,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    ++m_appIconRevision;
    m_settings->setCustomAppIconEnabled(true);
    applyAppIcon();
    Q_EMIT appIconChanged();
    qCInfo(lcApp) << "custom app icon set";
    return {};
}

bool AppController::windowGeometryIsReachable(const QRect &geometry)
{
    if (geometry.isEmpty())
        return false;
    // Only the top grab band must be reachable, so a window spanning two
    // monitors is still accepted.
    const QRect grabBand(geometry.x(), geometry.y(), geometry.width(), 32);
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (screen && screen->availableGeometry().intersects(grabBand))
            return true;
    }
    return false;
}

// Centres inside one screen's availableGeometry, taking origin and size from
// the same rect: under fractional scaling QScreen reports origins in native
// pixels and sizes in logical ones, and Screen.desktopAvailableWidth spans the
// whole virtual desktop. The result is checked against the real screens; an
// empty rect leaves placement to the window manager.
QRect AppController::centredWindowRect(int width, int height)
{
    if (width <= 0 || height <= 0)
        return {};
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return {};
    const QRect available = screen->availableGeometry();
    if (available.isEmpty())
        return {};
    const QRect candidate(available.x() + (available.width() - width) / 2,
                          available.y() + (available.height() - height) / 2,
                          width, height);
    return windowGeometryIsReachable(candidate) ? candidate : QRect{};
}

void AppController::noteWindowPlacement(const QString &how, int x, int y,
                                        int width, int height) const
{
    QStringList screens;
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (!screen)
            continue;
        const QRect available = screen->availableGeometry();
        screens << QStringLiteral("%1[%2,%3 %4x%5]")
                       .arg(screen->name())
                       .arg(available.x()).arg(available.y())
                       .arg(available.width()).arg(available.height());
    }
    const QRect stored =
        m_settings ? m_settings->initialWindowGeometry() : QRect{};
    qCInfo(lcApp).nospace()
        << "window placement " << how
        << " applied=[" << x << "," << y << " " << width << "x" << height
        << "] stored=[" << stored.x() << "," << stored.y() << " "
        << stored.width() << "x" << stored.height()
        << "] restorable=" << !m_restorableWindowGeometry.isEmpty()
        << " screens=" << screens.join(QLatin1Char(' '));
}

void AppController::resetCustomAppIcon()
{
    const QString target = matrix::app_data::customAppIconFile();
    if (!target.isEmpty() && QFileInfo::exists(target))
        QFile::remove(target);
    if (m_settings)
        m_settings->setCustomAppIconEnabled(false);
    ++m_appIconRevision;
    applyAppIcon();
    Q_EMIT appIconChanged();
    qCInfo(lcApp) << "custom app icon reset to default";
}

void AppController::applyControlPalette(const QVariantMap &roles)
{
    QPalette pal = QGuiApplication::palette();
    const auto set = [&](QPalette::ColorRole role, const char *key) {
        const QVariant v = roles.value(QString::fromLatin1(key));
        if (v.isValid())
            pal.setColor(role, v.value<QColor>());
    };
    const auto setDisabled = [&](QPalette::ColorRole role, const char *key) {
        const QVariant v = roles.value(QString::fromLatin1(key));
        if (v.isValid())
            pal.setColor(QPalette::Disabled, role, v.value<QColor>());
    };
    set(QPalette::Window, "window");
    set(QPalette::WindowText, "windowText");
    set(QPalette::Base, "base");
    set(QPalette::AlternateBase, "alternateBase");
    set(QPalette::Text, "text");
    set(QPalette::Button, "button");
    set(QPalette::ButtonText, "buttonText");
    set(QPalette::Highlight, "highlight");
    set(QPalette::HighlightedText, "highlightedText");
    set(QPalette::PlaceholderText, "placeholderText");
    set(QPalette::ToolTipBase, "toolTipBase");
    set(QPalette::ToolTipText, "toolTipText");
    set(QPalette::Light, "light");
    set(QPalette::Midlight, "midlight");
    set(QPalette::Mid, "mid");
    set(QPalette::Dark, "dark");
    set(QPalette::BrightText, "brightText");
    set(QPalette::Link, "link");
    setDisabled(QPalette::Text, "disabledText");
    setDisabled(QPalette::ButtonText, "disabledButtonText");
    setDisabled(QPalette::WindowText, "disabledWindowText");
    QGuiApplication::setPalette(pal);
}

bool AppController::callAcceptOffered(const QString &roomId,
                                      bool rtcLane) const
{
    if (roomId.isEmpty())
        return false;
    if (!rtcLane) {
        // The legacy lane's gate, matching IncomingCallPrompt. Necessary but
        // not sufficient: answer() can still refuse with `no_remote_offer`.
        return m_calls && m_calls->mediaBackendAvailable();
    }
    // The MatrixRTC gate, matching the card's `canJoinRtc`: a reachable
    // transport, and this device not already in the call.
    if (!m_rtc || !m_groupCall)
        return false;
    if (!m_rtc->joinBlockReason(roomId).isEmpty())
        return false;
    return !(m_groupCall->active() && m_groupCall->roomId() == roomId);
}

void AppController::routeNotificationOpen(const QString &roomId,
                                          const QString &eventId,
                                          const QString &threadRootId)
{
    // A notification click must open the room, not merely select it: only
    // openRoom() starts the live SDK timeline, and openRoom() skips that once
    // currentRoomId already matches.
    //
    // Reduce a thread composite id rather than refusing it: the Activity
    // Centre emits unreduced composites, which must never reach a protocol
    // call, and openRoomTimeline() closes the current timeline before it
    // discovers an unparsable id.
    const bool composite = MatrixClient::isThreadTimelineId(roomId);
    const QString target =
        composite ? MatrixClient::threadTimelineRoomId(roomId) : roomId;
    const QString root =
        (composite && threadRootId.isEmpty())
            ? MatrixClient::threadTimelineRootId(roomId)
            : threadRootId;
    if (!target.isEmpty())
        openRoom(target);
    Q_EMIT notificationOpenRequested(target, eventId, root);
}

void AppController::openRoom(const QString &roomId)
{
    // Do not reopen the room that is already open: that restarts the SDK
    // subscription and forces a full timeline reset. Explicit refresh goes
    // through reloadCurrentRoomTimeline().
    const bool alreadyOpen = (m_currentRoomId == roomId);
    setCurrentRoomId(roomId);
    // Opening a room from anywhere returns from Settings to the chat view.
    if (m_currentScreen == SettingsScreen)
        setCurrentScreen(MainScreen);
    // The Rust backend opens a persistent SDK timeline for the room: one
    // snapshot, then incremental diffs, including in-place decryption updates.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend == RustBackend && !roomId.isEmpty() && !alreadyOpen) {
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
            rust->openRoomTimeline(roomId);
    }
#else
    Q_UNUSED(alreadyOpen);
#endif
}

void AppController::openSpaceHome(const QString &spaceId)
{
    // Teardown first, activation last: the Space Home loader instantiates
    // synchronously once there is no open room and a real active space, and
    // clearing roomInfo after that would wipe the permission gates it reads.
    if (!m_currentRoomId.isEmpty()) {
        // Close the SDK timeline before the selection clears, as roomLeft does.
#ifdef ENABLE_RUST_SDK_BACKEND
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
            rust->closeRoomTimeline();
#endif
        m_roomInfo->setRoomId(QString());
    }
    if (m_spaces)
        m_spaces->setActiveSpaceId(spaceId);
    if (!m_currentRoomId.isEmpty())
        setCurrentRoomId(QString());
}

void AppController::setSpaceMuted(const QString &spaceId, bool mute)
{
    if (!m_spaces || spaceId.isEmpty() || !spaceId.startsWith(QLatin1Char('!')))
        return;
    // roomsInSpace() is transitive, so subspace rooms are muted too.
    const QStringList rooms = m_spaces->roomsInSpace(spaceId);
    if (rooms.isEmpty())
        return;
    const int mode = mute ? 2 : 3;
    for (const QString &roomId : rooms) {
        if (m_settings->roomNotificationMode(roomId) == mode)
            continue;   // idempotent: no write, no push-rule call
        setRoomNotificationMode(roomId, mode);
    }
    qCInfo(lcApp) << "space notification mode applied rooms=" << rooms.size()
                  << "muted=" << mute;
}

bool AppController::spaceIsMuted(const QString &spaceId) const
{
    if (!m_spaces || spaceId.isEmpty())
        return false;
    const QStringList rooms = m_spaces->roomsInSpace(spaceId);
    if (rooms.isEmpty())
        return false;
    for (const QString &roomId : rooms) {
        if (m_settings->roomNotificationMode(roomId) != 2)
            return false;
    }
    return true;
}

void AppController::markSpaceRead(const QString &spaceId)
{
    if (!m_spaces || !m_roomList || spaceId.isEmpty()
        || !spaceId.startsWith(QLatin1Char('!'))) {
        return;
    }
    const QStringList rooms = m_spaces->roomsInSpace(spaceId);
    for (const QString &roomId : rooms)
        m_roomList->markRoomRead(roomId);
    qCInfo(lcApp) << "space marked read rooms=" << rooms.size();
}

quint64 AppController::jumpToDate(qint64 timestampMs)
{
    if (!m_client || m_currentRoomId.isEmpty() || timestampMs <= 0)
        return 0;
    // Capture the room so a late answer cannot move a different room's
    // timeline; the op id is unique per request, not per room.
    const QString roomId = m_currentRoomId;
    const quint64 opId = m_client->eventAtTimestamp(roomId, timestampMs);
    if (opId == 0)
        return 0;
    m_pendingDateJumps.insert(opId, roomId);
    return opId;
}

int AppController::exportableMessageCount() const
{
    if (!m_timeline)
        return 0;
    return roomexport::exportableCount(m_timeline->events());
}

namespace {
roomexport::Format exportFormatFor(const QString &format)
{
    return format.compare(QLatin1String("json"), Qt::CaseInsensitive) == 0
        ? roomexport::Format::Json
        : roomexport::Format::PlainText;
}
} // namespace

roomexport::Options AppController::exportOptions() const
{
    roomexport::Options options;
    options.roomId = m_currentRoomId;
    const RoomInfo info = m_client ? m_client->roomInfo(m_currentRoomId)
                                   : RoomInfo{};
    options.roomName = info.name;
    options.exportedBy = m_client ? m_client->currentUserId() : QString();
    // Unknown encryption fails closed: treated as encrypted, so text is
    // withheld unless the user explicitly asked for it.
    options.encrypted = !info.encryptionKnown || info.encrypted;
    options.use24HourClock =
        m_settings && m_settings->clockFormat() == 2;
    return options;
}

QString AppController::suggestedExportFileName(const QString &format) const
{
    return roomexport::suggestedFileName(exportOptions(),
                                         exportFormatFor(format));
}

QString AppController::exportCurrentRoom(const QUrl &fileUrl,
                                         const QString &format,
                                         bool includeEncryptedText)
{
    if (m_currentRoomId.isEmpty() || !m_timeline)
        return tr("No room is open.");
    if (!fileUrl.isValid() || !fileUrl.isLocalFile())
        return tr("Choose a file on this computer.");

    roomexport::Options options = exportOptions();
    options.allowEncryptedPlaintext = includeEncryptedText;

    const QString text = roomexport::render(
        m_timeline->events(), options, exportFormatFor(format));

    QFile file(fileUrl.toLocalFile());
    // Truncate, not NewOnly: the save dialog already asked about overwriting.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return tr("Could not write that file.");
    const QByteArray bytes = text.toUtf8();
    const qint64 written = file.write(bytes);
    // Close before judging: a buffered write can still fail at flush.
    file.close();
    if (written != bytes.size() || file.error() != QFileDevice::NoError)
        return tr("Could not finish writing that file.");

    // Counts and flags only: never the path, room name or a body.
    qCInfo(lcApp) << "room exported messages="
                  << roomexport::exportableCount(m_timeline->events())
                  << "format="
                  << (exportFormatFor(format) == roomexport::Format::Json
                          ? "json" : "text")
                  << "encrypted=" << options.encrypted
                  << "text_included="
                  << (!options.encrypted || includeEncryptedText);
    return QString();
}

void AppController::openLobby()
{
    // Lobby is the head of whatever the column shows: with a Space selected,
    // that Space's overview; for pseudo selections ("" Home, "@orphans"), the
    // account Home. Reuses openSpaceHome's teardown ordering rather than
    // copying it.
    const QString active = m_spaces ? m_spaces->activeSpaceId() : QString();
    openSpaceHome(active.startsWith(QLatin1Char('!')) ? active : QString());
}

bool AppController::trimHistoryAndJumpToLive()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    // The policy lives in historyTrimAllowed() so each clause is testable.
    if (!historyTrimAllowed(
            m_backend == RustBackend, m_client && !m_currentRoomId.isEmpty(),
            m_pagination && m_pagination->busy(),
            m_thread && (m_thread->active() || m_thread->listOpen()),
            m_timeline ? m_timeline->rowCount() : 0,
            historyTrimRowThreshold())) {
        return false;
    }
    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    if (!rust)
        return false;
    qCInfo(lcApp) << "jump-to-live history trim rows="
                  << m_timeline->rowCount();
    // Report the real outcome: a swallowed dispatch failure would leave the
    // caller in follow-latest mode with no reset coming, and the next live
    // message would teleport a reader still mid-history.
    return rust->reloadRoomTimelineAtLive(m_currentRoomId);
#else
    return false;
#endif
}

// Retries decryption in place: unlike reloadCurrentRoomTimeline() it keeps the
// subscription, loaded history and delegates, and the SDK updates the events
// that decrypt. All automatic retries use this; the reload is only the
// explicit user Refresh.
void AppController::retryDecryptionInCurrentRoom()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_currentRoomId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->retryDecryption(m_currentRoomId);
#endif
}

void AppController::reloadCurrentRoomTimeline(int limit)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_currentRoomId.isEmpty())
        return;
    // Re-opens the SDK timeline (fresh snapshot and subscription generation).
    // `limit` is kept for API compatibility.
    Q_UNUSED(limit);
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->openRoomTimeline(m_currentRoomId);
#else
    Q_UNUSED(limit);
#endif
}

void AppController::copyImageToClipboard(const QString &mediaKey)
{
    if (mediaKey.isEmpty() || !m_mediaBridge)
        return;
    // Claim before dispatching: the bridge may answer synchronously from RAM.
    m_pendingCopyKeys.insert(mediaKey);
    m_mediaBridge->fetchFullForStar(mediaKey);
}

void AppController::copyImageBytesToClipboard(const QString &mediaKey,
                                              bool ok,
                                              const QByteArray &bytes,
                                              const QString &category)
{
    Q_UNUSED(mediaKey); // never logged — the key names the media
    if (!ok || bytes.isEmpty()) {
        Q_EMIT copyImageFinished(false, tr("Couldn't load the image (%1).")
                                            .arg(category));
        return;
    }
    // Identify by magic bytes, never a claimed MIME, using the signatures
    // rooms::sniff_image_mime accepts, so SVG never reaches the clipboard.
    const auto starts = [&bytes](const char *magic, int len) {
        return bytes.size() >= len
            && std::memcmp(bytes.constData(), magic, len) == 0;
    };
    QString identified;
    if (starts("\x89PNG\r\n\x1a\n", 8))
        identified = QStringLiteral("image/png");
    else if (starts("\xff\xd8\xff", 3))
        identified = QStringLiteral("image/jpeg");
    else if (starts("GIF87a", 6) || starts("GIF89a", 6))
        identified = QStringLiteral("image/gif");
    else if (bytes.size() >= 12
             && std::memcmp(bytes.constData(), "RIFF", 4) == 0
             && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0)
        identified = QStringLiteral("image/webp");
    else if (starts("BM", 2))
        identified = QStringLiteral("image/bmp");
    // JPEG XL: test the ISOBMFF container before the bare codestream, since a
    // container's payload begins with the codestream signature.
    else if (starts("\x00\x00\x00\x0CJXL \r\n\x87\n", 12)
             || starts("\xff\x0a", 2))
        identified = QStringLiteral("image/jxl");
    if (identified.isEmpty()) {
        Q_EMIT copyImageFinished(false, tr("This isn't a copyable image."));
        return;
    }
    QImage image;
    if (!image.loadFromData(bytes) || image.isNull()) {
        Q_EMIT copyImageFinished(false, tr("Couldn't decode the image."));
        return;
    }
    // Both a decoded raster and the original bytes under their true MIME, so
    // targets that accept the format get an exact copy (animated GIFs stay
    // animated).
    auto *guiApp =
        qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!guiApp) {
        // Guiless harnesses have no clipboard; production always does.
        Q_EMIT copyImageFinished(false, tr("Clipboard unavailable."));
        return;
    }
    auto *mime = new QMimeData;
    mime->setImageData(image);
    mime->setData(identified, bytes);
    guiApp->clipboard()->setMimeData(mime); // clipboard takes ownership
    Q_EMIT copyImageFinished(true, QString());
}

void AppController::starChatGif(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    // A star fetch in flight at sign-out produces no result; there is no UI
    // left to report to. Claim before dispatching: MediaBridge may answer
    // synchronously from RAM.
    m_pendingStarKeys.insert(mediaKey);
    m_mediaBridge->fetchFullForStar(mediaKey);
}

bool AppController::isChatGifStarred(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return false;
    // Fast path: exact for a GIF starred earlier in this session.
    if (m_gif->starredStore()->isStarredThisSession(mediaKey))
        return true;
    // Nothing starred: skip the MediaBridge round trip and its SHA-256.
    if (!m_gif->starredStore()->isOpen() || m_gif->starredStore()->count() == 0)
        return false;
    // Durable path: content-addressed, using only bytes the display cache
    // already fetched (memoized per cache key), never a fresh fetch.
    const QString hash = m_mediaBridge->cachedFullContentHash(mediaKey);
    if (hash.isEmpty())
        return false;
    return m_gif->starredStore()->hasHash(hash);
}

void AppController::unstarChatGif(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    if (m_gif->starredStore()->isStarredThisSession(mediaKey)) {
        m_gif->starredStore()->unstarByMediaKey(mediaKey);
        return;
    }
    // Nothing starred: see isChatGifStarred.
    if (!m_gif->starredStore()->isOpen() || m_gif->starredStore()->count() == 0)
        return;
    const QString hash = m_mediaBridge->cachedFullContentHash(mediaKey);
    if (!hash.isEmpty())
        m_gif->starredStore()->unstar(hash);
}

void AppController::acceptVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->acceptVerification(m_verificationFlowId);
#endif
}

void AppController::confirmVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    // Confirming only means something while the emoji list is on screen.
    if (m_verificationState != QLatin1String("sas_ready"))
        return;
    // Flip to "confirming" synchronously for immediate feedback; the SDK then
    // moves it to "waiting_for_peer", and FFI failures arrive as "failed:…".
    m_verificationState = QStringLiteral("confirming");
    Q_EMIT verificationStateChanged();
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->confirmVerification(m_verificationFlowId);
#endif
}

void AppController::mismatchVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->mismatchVerification(m_verificationFlowId);
#endif
}

void AppController::cancelVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client)
        return;
    // A failure raised before any flow id exists still shows a "failed:…"
    // card, so local state is always cleared; only the SDK cancel needs a
    // flow id.
    if (!m_verificationFlowId.isEmpty()) {
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
            rust->cancelVerification(m_verificationFlowId);
    } else if (m_verificationState.isEmpty()) {
        return; // nothing in flight and nothing displayed
    }
    m_verificationFlowId.clear();
    m_verificationState.clear();
    m_verificationEmojis.clear();
    m_verificationDecimals.clear();
    // Do not leave a stale code on screen once the card closes.
    clearVerificationQr();
    Q_EMIT verificationStateChanged();
#endif
}

// The user confirmed the other device reported a successful scan. This is a
// request to the SDK, never a trust promotion: only verificationDone may
// report success.
void AppController::confirmQrVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    // Only meaningful once the SDK has reported the scan; the FFI refuses an
    // early confirm outright.
    if (!m_verificationQrScanned || m_verificationQrConfirming)
        return;
    m_verificationQrConfirming = true;
    Q_EMIT verificationStateChanged();
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->confirmQrVerification(m_verificationFlowId);
#endif
}

// Drop the displayed code and its grid. Called from every path that ends,
// replaces or resets a flow. Does not emit; the caller does.
void AppController::clearVerificationQr()
{
    m_qrCodeStore.clear();
    m_verificationQrToken.clear();
    m_verificationQrScanned = false;
    m_verificationQrConfirming = false;
}

void AppController::startOwnVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT errorReported(tr(
            "Verification is only available on the Rust backend."));
        return;
    }
    // One flow at a time.
    if (!m_verificationFlowId.isEmpty()
        && m_verificationState != QLatin1String("done")
        && m_verificationState != QLatin1String("cancelled")
        && !m_verificationState.startsWith(QLatin1String("failed"))) {
        Q_EMIT errorReported(tr("A verification is already in progress."));
        return;
    }
    // Clear stale state from any previous flow so QML re-renders cleanly.
    m_verificationFlowId.clear();
    m_verificationEmojis.clear();
    m_verificationDecimals.clear();
    clearVerificationQr();
    m_verificationState = QStringLiteral("starting");
    Q_EMIT verificationStateChanged();
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->startOwnVerification();
#endif
}

void AppController::refreshSessionTrustState()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
        rust->refreshOwnDeviceStatus();
        requestOwnDeviceKeyCheck();
        m_cryptoQueryGeneration = m_cryptoHealth->generation();
        rust->queryCryptoHealth();
    }
#endif
}

// Single rate-limited gate for the /keys/query check: the event-driven
// callers and the periodic backstop can all fire within a second of sign-in.
void AppController::requestOwnDeviceKeyCheck()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client)
        return;
    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    if (!rust)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_ownDeviceKeyWatch.checkDue(now))
        return;
    m_ownDeviceKeyWatch.noteDispatched(now);
    rust->checkOwnIdentityKey();
#endif
}

// Fold one answer in and announce only a real transition.
void AppController::applyOwnDeviceKeyAgreement(
    matrix::crypto::KeyAgreement agreement)
{
    if (!m_ownDeviceKeyWatch.apply(agreement))
        return;
    if (m_ownDeviceKeyWatch.broken()) {
        // Permanent until the account signs in again, so the backstop stops;
        // event-driven callers still run and can clear it.
        m_ownDeviceKeyTimer.stop();
    } else if (m_client && m_client->isLoggedIn()
               && !m_ownDeviceKeyTimer.isActive()) {
        // Cleared by a verification or refresh: restart the backstop so a
        // later fault is still noticed this session.
        m_ownDeviceKeyTimer.start();
    }
    Q_EMIT encryptionIdentityBrokenChanged();
}

void AppController::requestEncryptionKeys()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT errorReported(tr(
            "Key requests are only available on the Rust backend."));
        return;
    }
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
        // Update the model first so the UI re-enters the waiting state.
        m_cryptoBootstrap->rearmAfterManualRequest();
        rust->requestMissingSecrets();
    }
#else
    Q_EMIT errorReported(tr(
        "Key requests are only available on the Rust backend."));
#endif
}

void AppController::setActiveRoomAtLatest(bool atLatest)
{
    if (m_activeRoomAtLatest == atLatest)
        return;
    m_activeRoomAtLatest = atLatest;
    Q_EMIT activeRoomAtLatestChanged();
}

void AppController::setActiveRoomHydrating(bool hydrating)
{
    if (m_activeRoomHydrating == hydrating)
        return;
    m_activeRoomHydrating = hydrating;
    Q_EMIT activeRoomHydratingChanged();
}

void AppController::renameSessionDevice(const QString &deviceId,
                                        const QString &name)
{
    if (!m_client || deviceId.isEmpty() || m_sessionDeviceRenameOp != 0)
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    const quint64 opId = m_client->renameDevice(deviceId, trimmed);
    m_sessionDeviceRenameError =
        opId == 0 ? tr("Renaming sessions is not available here.") : QString();
    m_sessionDeviceRenameOp = opId;
    Q_EMIT sessionDevicesChanged();
}

void AppController::refreshSessionDevices()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend == RustBackend && m_client) {
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
            m_sessionDevicesLoading = true;
            m_sessionDevicesFailed = false;
            Q_EMIT sessionDevicesChanged();
            rust->requestDeviceList();
            return;
        }
    }
#endif
}

void AppController::refreshCryptoHealth()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend == RustBackend && m_client) {
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
            m_cryptoQueryGeneration = m_cryptoHealth->generation();
            rust->queryCryptoHealth();
            return;
        }
    }
#endif
    // Non-Rust backends have no crypto machine; the model reports unsupported.
}

void AppController::importRoomKeys(const QUrl &fileUrl, const QString &passphrase)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        m_roomKeyImportState = QStringLiteral("failed");
        m_roomKeyImportMessage = tr(
            "Room-key import is only available on the Rust backend.");
        m_roomKeyImportRunning = false;
        Q_EMIT roomKeyImportStateChanged();
        return;
    }
    // Only local files are accepted.
    if (!fileUrl.isValid() || !fileUrl.isLocalFile() || fileUrl.isEmpty()) {
        m_roomKeyImportState = QStringLiteral("failed");
        m_roomKeyImportMessage = tr(
            "Lightning could not read the selected file.");
        m_roomKeyImportRunning = false;
        Q_EMIT roomKeyImportStateChanged();
        return;
    }
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        m_roomKeyImportState = QStringLiteral("failed");
        m_roomKeyImportMessage = tr(
            "Lightning could not read the selected file.");
        m_roomKeyImportRunning = false;
        Q_EMIT roomKeyImportStateChanged();
        return;
    }
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->importRoomKeys(path, passphrase);
    // The passphrase is never copied to a member.
    Q_UNUSED(passphrase);
#else
    Q_UNUSED(fileUrl);
    Q_UNUSED(passphrase);
#endif
}

void AppController::resetLocalRustStore()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Reset is only available on the Rust backend."));
        return;
    }
    const QString homeserver = m_client && !m_client->homeserverUrl().isEmpty()
        ? m_client->homeserverUrl()
        : (m_settings ? m_settings->homeserverUrl() : QString{});
    const QString userId = m_client && !m_client->currentUserId().isEmpty()
        ? m_client->currentUserId()
        : (m_settings ? m_settings->userId() : QString{});
    if (homeserver.isEmpty() || userId.isEmpty()) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Enter a valid homeserver and Matrix user ID before resetting "
               "the local Lightning session."));
        return;
    }

    if (m_client && m_client->isLoggedIn()) {
        // The Rust sign-out lifecycle performs the same account-scoped
        // deletion after server logout.
        m_resetResultPending = true;
        m_auth->logout();
    } else {
        resetLocalRustSession(homeserver, userId);
    }
#else
    Q_EMIT localRustStoreResetResult(false,
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::resetLocalRustSession(const QString &homeserver,
                                          const QString &user)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Reset is only available on the Rust backend."));
        return;
    }

    matrix::app_data::AccountIdentity identity;
    if (!matrix::app_data::resolveAccountIdentity(
            homeserver, user, &identity)) {
        Q_EMIT localRustStoreResetResult(false,
            tr("Enter a valid homeserver and Matrix user ID before resetting "
               "the local Lightning session."));
        return;
    }

    auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get());
    QString message;
    const bool ok = rust && rust->resetLocalSession(identity, &message);
    // The backend's own signal re-arms the reset flag, and only when a reset
    // could still help; a "nothing matched" outcome must not re-offer the
    // same no-op, so do not derive the flag from `ok`.
    if (ok) {
        setLocalRustResetRequired(false);
        clearLocalSessionFailure();
        m_auth->clearLastError();
        Q_EMIT errorReported(QString{});
    }
    Q_EMIT localRustStoreResetResult(ok, message.isEmpty()
        ? tr("Lightning could not completely reset the local session for this "
             "account. Check the application logs and filesystem permissions, "
             "then try again.")
        : message);
#else
    Q_UNUSED(homeserver);
    Q_UNUSED(user);
    Q_EMIT localRustStoreResetResult(false,
        tr("This build has no Rust SDK backend."));
#endif
}

void AppController::setLocalSessionFailure(const QString &reasonCode,
                                           const QString &userId,
                                           const QString &homeserver)
{
    if (m_localSessionFailureReason == reasonCode
        && m_localSessionFailureUserId == userId
        && m_localSessionFailureHomeserver == homeserver) {
        return;
    }
    m_localSessionFailureReason = reasonCode;
    m_localSessionFailureUserId = userId;
    m_localSessionFailureHomeserver = homeserver;
    Q_EMIT localSessionFailureChanged();
}

bool AppController::localResetHelpsFor(const QString &reasonCode) const
{
    // Not gated on ENABLE_RUST_SDK_BACKEND: RustSessionPolicy is pure
    // classification compiled into every configuration, and whether a reset
    // can repair a failure does not depend on the backend.
    return matrix::rust_session::suggestsLocalResetForCode(reasonCode);
}

void AppController::repairLocalSession()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    // The account captured when the failure was detected wins over settings:
    // during an add-account attempt the settings' active account is still the
    // previous one. The refusal of a destructive reset that cannot help lives
    // here, not in QML, because the failure can change under an open dialog.
    if (!m_localSessionFailureReason.isEmpty()
        && !matrix::rust_session::suggestsLocalResetForCode(
               m_localSessionFailureReason)) {
        qCWarning(lcApp) << "refusing destructive repair for a reason a reset "
                            "cannot fix"
                         << "reason=" << m_localSessionFailureReason;
        Q_EMIT localRustStoreResetResult(false, tr(
            "Clearing this device's local data would not fix this, and it "
            "would destroy encryption keys you still need."));
        return;
    }

    QString homeserver = m_localSessionFailureHomeserver;
    QString user = m_localSessionFailureUserId;
    if (user.isEmpty()) {
        // No failure in flight: the Settings danger-zone case, where the
        // signed-in account is the right target.
        resetLocalRustStore();
        return;
    }
    if (homeserver.isEmpty())
        homeserver = m_settings->homeserverUrl();
    resetLocalRustSession(homeserver, user);
#else
    Q_EMIT localRustStoreResetResult(false,
        tr("This build has no Rust SDK backend."));
#endif
}

QString AppController::sessionDiagnosticsText() const
{
    // One fresh salt per report: identifiers stay correlatable inside this
    // bundle and useless outside it.
    const QByteArray salt = matrix::app_diagnostics::newReportSalt();
    const auto hashIdentifier = [&salt](const QString &value) {
        return matrix::app_diagnostics::hashIdentifier(value, salt);
    };
    matrix::app_diagnostics::Report r;

    r.appVersion = QCoreApplication::applicationVersion();
    r.qtVersion = QString::fromLatin1(qVersion());
    r.backendName = backendName();
#ifdef QT_DEBUG
    r.buildType = QStringLiteral("debug");
#else
    r.buildType = QStringLiteral("release");
#endif
#ifdef ENABLE_RUST_SDK_BACKEND
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        r.rustSdkVersion = rust->rustBackendVersion();
#endif

    r.osProduct = QSysInfo::prettyProductName();
    r.kernelVersion = QSysInfo::kernelVersion();
    r.desktopSession = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    r.sessionType = qEnvironmentVariable("XDG_SESSION_TYPE");

    if (m_settings) {
        const QStringList ids = m_settings->savedAccountUserIds();
        r.accountCount = int(ids.size());
        for (const QString &uid : ids)
            r.accountHashes.append(hashIdentifier(uid));
        r.activeAccountHash = hashIdentifier(m_settings->activeAccountUserId());
        if (auto *secrets = m_settings->secretStore()) {
            r.secretStoreBackend = secrets->backendName();
            r.secretStoreSecure = secrets->isSecure();
        }
    }
    // The on-disk layout is per-account roots under primaryRoot(); the path
    // itself is deliberately not reported because the slug is the localpart.
    r.storeLayoutVersion = QStringLiteral("per-account-root/v1");

    r.connectionStatus = m_connectionStatus;
    r.syncMode = syncModeLabel();
    r.loginStage = m_auth ? m_auth->loginStage() : QString{};
    r.initialSyncDone = initialSyncDone();
    r.localSessionFailureReason = m_localSessionFailureReason;
    r.localSessionFailureAccountHash =
        hashIdentifier(m_localSessionFailureUserId);

    r.sessionTrustState = m_sessionTrustState;
    r.verificationState = m_verificationState;
    if (m_cryptoHealth) {
        r.crossSigningAvailable = m_cryptoHealth->crossSigningAvailable();
        r.keyBackupUsable = m_cryptoHealth->keyBackupUsable();
        r.cryptoStatusSummary = m_cryptoHealth->statusSummary();
    }

    return matrix::app_diagnostics::renderReport(r);
}

void AppController::copySessionDiagnostics()
{
    if (auto *clipboard = QGuiApplication::clipboard())
        clipboard->setText(sessionDiagnosticsText());
}

void AppController::setLocalRustResetRequired(bool required)
{
    // Does not clear the classified failure: `localSessionBlocked` records a
    // reason whose remedy is not a reset while leaving this flag false.
    // Callers that resolve a failure call clearLocalSessionFailure().
    if (m_localRustResetRequired == required)
        return;
    m_localRustResetRequired = required;
    Q_EMIT localRustResetRequiredChanged();
}

void AppController::setCurrentScreen(Screen s)
{
    if (m_currentScreen == s)
        return;
    qCInfo(lcApp) << "screen change" << int(m_currentScreen) << "->" << int(s)
                  << "(0=Login, 1=Main, 2=Settings, 3=Boot)";
    m_currentScreen = s;
    Q_EMIT currentScreenChanged();
}

void AppController::setConnectionStatus(const QString &s)
{
    if (m_connectionStatus == s)
        return;
    m_connectionStatus = s;
    Q_EMIT connectionStatusChanged();
}

void AppController::onLoginSucceeded()
{
    const QString uid = m_auth->currentUserId();
    qCInfo(lcApp) << "login succeeded slug="
                  << matrix::app_data::safeUserSlug(uid)
                  << "switching=" << m_accountSwitching
                  << "— switching to main + starting sync";
    // An add-account login while another account was active is a
    // cross-account transition too — clear caches exactly like a switch.
    const bool accountChanged =
        !m_lastSessionUserId.isEmpty() && m_lastSessionUserId != uid;
    m_lastSessionUserId = uid;
    if (accountChanged) {
        clearCrossAccountCaches();
        // Add-account never signs the previous account out, so nothing
        // downstream sees loggedOut; announce the per-account state a switch
        // announces, or hidden images and ignore lists leak between accounts.
        setCurrentRoomId(QString{});
        if (m_roomInfo)
            m_roomInfo->setRoomId(QString{});
        if (m_shortcuts)
            m_shortcuts->reload();
        if (m_mediaVisibility)
            m_mediaVisibility->reloadForAccount();
        if (m_moderation)
            m_moderation->resetForAccountChange();
    }
    // Stamp notifications with this account so actions taken after a later
    // switch can be refused. Set after the cache clear.
    if (m_notifications)
        m_notifications->setAccountUserId(uid);
    // Point the account-scoped starred-GIF store at this account on every
    // login. The path comes from the shared helper the delete paths also use,
    // so cleanup can never target a different directory.
    m_gif->openStarredStoreFor(matrix::app_data::starredGifsDir(uid));
    // Seed network badges from disk: the read that produces them costs a raw
    // /state per room, so without this they only appear for rooms opened this
    // session. The model keys them by room id, so seeding before the room list
    // is populated is fine.
    m_bridgeLabels.openFor(matrix::app_data::bridgeLabelsFile(uid));
    if (m_roomList) {
        const auto remembered = m_bridgeLabels.positiveLabels();
        for (auto it = remembered.constBegin(); it != remembered.constEnd();
             ++it) {
            m_roomList->setAdvertisedBridge(it.key(), it.value().networkId,
                                            it.value().label);
        }
        qCInfo(lcApp) << "bridge badges restored from disk count="
                      << remembered.size();
    }
    m_accounts->setActiveUser(uid);
    setLocalRustResetRequired(false);
    clearLocalSessionFailure();
    m_switchFallbackUserId.clear();
    setAccountSwitching(false);
    m_client->startSync();
    // Cache the account's own display name / avatar for the switcher UI.
    m_client->fetchUserProfile(uid);
    // A background restore after a failed add-account keeps the user on the
    // login screen with the error form visible.
    if (m_backgroundRestore && uid == m_addAccountReturnTo) {
        m_backgroundRestore = false;
        Q_EMIT loggedInChanged();
        return;
    }
    m_backgroundRestore = false;
    m_addAccountReturnTo.clear();
    Q_EMIT errorReported(QString{});
    setCurrentScreen(MainScreen);
    Q_EMIT loggedInChanged();
}

void AppController::refreshTrayState()
{
    // Without QtDBus (Windows, macOS) the tray balloon is the only notification
    // delivery and it needs a visible icon, so the icon also follows the
    // notifications setting there.
#ifdef HAVE_QT_DBUS
    constexpr bool kTrayCarriesNotifications = false;
#else
    constexpr bool kTrayCarriesNotifications = true;
#endif
    const bool wanted = m_settings
        && (m_settings->closeToTray()
            || (kTrayCarriesNotifications && m_settings->notificationsEnabled()));
    m_tray.setEnabled(wanted && TrayIcon::platformSupportsTray());
    if (m_tray.enabled()) {
        m_tray.setAccountLabel(m_lastSessionUserId);
        // A tray turned on while messages are already waiting must open with
        // its badge, so the state is pushed here as well as on every change.
        refreshTrayUnread();
    }
}

void AppController::refreshTrayUnread()
{
    // Not gated on the tray: this walk also withdraws notifications for rooms
    // that are no longer unread. Only the badge write needs the icon.
    if (!m_client)
        return;
    // Derived from the local rooms() snapshot, reading the same fields every
    // other unread surface reads.
    int total = 0;
    bool anyUnread = false;
    for (const RoomInfo &room : m_client->rooms()) {
        // Invites have their own notification and are not unread messages.
        if (room.membership != RoomInfo::Joined)
            continue;
        total += qMax(0, room.unreadCount);
        const bool roomUnread = room.unreadCount > 0 || room.hasUnreadMessages
                                || room.markedUnread;
        if (roomUnread)
            anyUnread = true;
        // A room that is no longer unread withdraws its notifications.
        // Level-triggered so a read in another client (cleared via sync) is
        // caught too; cheap, since rooms without a live notification return
        // immediately.
        if (!roomUnread && m_notifications)
            m_notifications->closeRoomNotifications(room.id);
    }
    if (m_tray.enabled())
        m_tray.setUnread(total, anyUnread);
}

void AppController::onLoggedOut()
{
    m_currentRoomId.clear();
    // The roster cache died with the session (detach or logout); the next
    // account — or a re-login — must hydrate rooms afresh.
    m_memberHydratedRooms.clear();
    // Same scope, same reason: the next account's rooms are not these rooms.
    m_bridgeReadRooms.clear();
    // Closed on both paths, switch included, so a late answer cannot write
    // into the next account's file.
    m_bridgeLabels.close();
    m_playback->stopAll(); // no playback (or decrypted-media handle) survives
    // Unsent clipboard images belong to the session. The composers release
    // them individually; this sweep catches anything left behind.
    m_stagedImages.clear();
    // Cropped-image temp copies go with the session too.
    m_imageCrop.clearSession();
    Q_EMIT currentRoomIdChanged();
    if (m_accountSwitching) {
        // Detached locally as part of a switch: stay on the main screen and
        // keep the outgoing account's starred-GIF store. A pending removal is
        // dropped: removeAccount() refuses to arm one during a switch, and
        // carrying it forward could let a later sign-out delete it.
        m_pendingRemovalUserId.clear();
        m_pendingRemovalIdentity = {};
        m_pendingRemovalResolved = false;
        return;
    }
    // A genuine sign-out (including removal of the active account). The
    // starred-GIF store of the account that was active must not survive, like
    // the Rust crypto store. m_lastSessionUserId is read before it is cleared
    // below, and this handler runs before the client's own loggedOut
    // connection, so the store is closed explicitly here.
    if (!m_lastSessionUserId.isEmpty()) {
        // Bridge badges go with the account: no message content, but still a
        // per-account record of which rooms the user is in.
        const QString bridgeFile =
            matrix::app_data::bridgeLabelsFile(m_lastSessionUserId);
        const bool bridgeExisted = !bridgeFile.isEmpty()
                                   && QFile::exists(bridgeFile);
        if (!BridgeLabelStore::removeStore(bridgeFile)) {
            qCWarning(lcApp)
                << "bridge badge store sign-out cleanup FAILED slug="
                << matrix::app_data::safeUserSlug(m_lastSessionUserId);
        } else {
            qCInfo(lcApp) << "bridge badge store sign-out cleanup"
                          << "slug="
                          << matrix::app_data::safeUserSlug(m_lastSessionUserId)
                          << "outcome="
                          << (bridgeExisted ? "deleted" : "absent");
        }
        m_gif->closeStarredStore(); // never delete a directory still "open"
        const QString starredDir =
            matrix::app_data::starredGifsDir(m_lastSessionUserId);
        const auto outcome = matrix::app_data::removeAppDataDir(starredDir);
        // A failed delete leaves decrypted material behind, so it warns.
        if (outcome == matrix::app_data::DirRemoval::Failed) {
            qCWarning(lcApp)
                << "starred-GIF store sign-out cleanup FAILED slug="
                << matrix::app_data::safeUserSlug(m_lastSessionUserId);
        } else {
            qCInfo(lcApp) << "starred-GIF store sign-out cleanup"
                          << "slug="
                          << matrix::app_data::safeUserSlug(m_lastSessionUserId)
                          << "outcome="
                          << (outcome == matrix::app_data::DirRemoval::Deleted
                                  ? "deleted"
                                  : "absent");
        }
    }
    // Second half of removeAccount() for the active account. Runs before
    // clearActiveUser() and the fallback loop, so the account being removed
    // can never be chosen to continue with.
    if (!m_pendingRemovalUserId.isEmpty()) {
        const QString target = m_pendingRemovalUserId;
        const bool resolved = m_pendingRemovalResolved;
        const matrix::app_data::AccountIdentity identity =
            m_pendingRemovalIdentity;
        m_pendingRemovalUserId.clear();
        m_pendingRemovalIdentity = {};
        m_pendingRemovalResolved = false;
        // Only for the session that actually ended. An unattributed session
        // still proceeds with the captured identity; a session belonging to
        // a different account abandons the removal.
        if (m_lastSessionUserId.isEmpty() || m_lastSessionUserId == target) {
            if (resolved)
                removeAccountLocalState(identity);
            else
                qCWarning(lcApp) << "account removal completed without a "
                                    "resolvable local layout — record and "
                                    "secrets only";
            m_accounts->removeAccount(target); // record + secrets
        } else {
            qCWarning(lcApp)
                << "pending account removal abandoned: the session that "
                   "ended is not the account that was being removed";
        }
    }
    m_accounts->clearActiveUser();
    m_lastSessionUserId.clear();
    m_addAccountReturnTo.clear();
    m_backgroundRestore = false;
    Q_EMIT errorReported(QString{});
    // When other accounts remain, continue with the most recently added one.
    const QStringList remaining = m_settings->savedAccountUserIds();
    // An empty token read is not evidence an account is gone: with a locked
    // keyring every account reads tokenless.
    bool anyUnreadable = false;
    for (auto it = remaining.crbegin(); it != remaining.crend(); ++it) {
        switch (signInStateFor(*it)) {
        case SignInState::Usable:
            switchToAccount(*it);
            return;
        case SignInState::Unreadable:
            // Not switched to: restoreSession() needs the unreadable token.
            // The user is told why they are on the login screen instead.
            anyUnreadable = true;
            break;
        case SignInState::Gone:
            break;
        }
    }
    if (anyUnreadable) {
        // Emitted after the blanket clear so it reaches the login screen.
        Q_EMIT errorReported(
            tr("Lightning can't read this device's saved sign-ins right now — "
               "the system keyring is locked or unavailable. Your other "
               "accounts are still on this device; unlock the keyring and "
               "restart Lightning to continue with them."));
    }
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

void AppController::setAccountSwitching(bool switching)
{
    if (m_accountSwitching == switching)
        return;
    m_accountSwitching = switching;
    Q_EMIT accountSwitchingChanged();
}

void AppController::clearCrossAccountCaches()
{
    // Playback stops before the decrypted media files are wiped, so no
    // player holds an open handle into the previous account's cache.
    m_playback->stopAll();
    m_mediaBridge->clear();
    // MediaBridge::clear() drops in-flight requests without a terminal
    // emission, so the claim sets would otherwise strand their keys.
    m_pendingStarKeys.clear();
    m_pendingCopyKeys.clear();
    m_notifications->clearPending();
    // Anything still on screen must not be attributed to the next account.
    m_notifications->setAccountUserId(QString());
    m_knownInvites.clear();
    // Encrypted-room drafts are memory-only and account-scoped.
    if (m_draftStore)
        m_draftStore->clearMemoryDrafts();
    m_sessionDevices.clear();
    m_sessionDevicesLoading = false;
    m_sessionDevicesFailed = false;
    Q_EMIT sessionDevicesChanged();
    retireOwnDisplayNameWrite();
    retireOwnAvatarWrite();
    m_verificationFlowId.clear();
    m_verificationOtherUser.clear();
    m_verificationOtherDevice.clear();
    m_verificationIsSelf = false;
    m_verificationState.clear();
    m_verificationEmojis.clear();
    m_verificationDecimals.clear();
    clearVerificationQr();
    Q_EMIT verificationStateChanged();
    m_sessionTrustState = QStringLiteral("Unknown");
    m_sessionDeviceId.clear();
    m_ownIdentityAvailable = false;
    m_crossSigningAvailable = false;
    // Do not carry the previous account's undecryptable-device fault over.
    m_ownDeviceKeyTimer.stop();
    const bool wasKeyFaultLatched = m_ownDeviceKeyWatch.broken();
    m_ownDeviceKeyWatch.reset();
    if (wasKeyFaultLatched)
        Q_EMIT encryptionIdentityBrokenChanged();
    m_roomKeyImportState.clear();
    m_roomKeyImportImported = 0;
    m_roomKeyImportTotal = 0;
    m_roomKeyImportAffected = 0;
    m_roomKeyImportMessage.clear();
    m_roomKeyImportRunning = false;
    m_roomKeyImportAffectedRoomIds.clear();
    Q_EMIT securityStateChanged();
    Q_EMIT roomKeyImportStateChanged();
    // Drop DM profile lookups resolved under the previous account.
    m_roomList->clearProfileCaches();
}

// ── Own display name ────────────────────────────────────────────────────

bool AppController::canEditOwnDisplayName() const
{
    return m_client && m_client->isLoggedIn()
           && m_client->supportsOwnProfileEditing();
}

int AppController::displayNameLength(const QString &name) const
{
    // Counts Unicode code points, the unit the Rust bound and the server use;
    // QString::size() would count an emoji's surrogate pair twice.
    int points = 0;
    for (qsizetype i = 0; i < name.size();) {
        const bool pair = name.at(i).isHighSurrogate() && i + 1 < name.size()
                          && name.at(i + 1).isLowSurrogate();
        i += pair ? 2 : 1;
        ++points;
    }
    return points;
}

QString AppController::cachedOwnDisplayName() const
{
    if (!m_accounts || !m_client)
        return {};
    const QString uid = m_client->currentUserId();
    if (uid.isEmpty())
        return {};
    return m_accounts->account(uid)
        .value(QStringLiteral("displayName"))
        .toString();
}

QString AppController::ownDisplayNameUnavailableReason() const
{
    if (!m_client || !m_client->isLoggedIn())
        return tr("Not signed in.");
    if (!m_client->supportsOwnProfileEditing())
        return tr("This backend cannot change your display name.");
    return {};
}

bool AppController::canEditOwnAvatar() const
{
    return m_client && m_client->supportsOwnProfileEditing();
}

bool AppController::submitOwnAvatar(const QUrl &fileUrl)
{
    // Single-flight: two racing writes would let the loser's answer land last.
    if (m_avatarOp != 0)
        return false;
    if (!canEditOwnAvatar()) {
        m_avatarError = tr("This account cannot change its picture here.");
        Q_EMIT ownAvatarStateChanged();
        return false;
    }
    // Local files only; anything else is refused rather than passed to the FFI.
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                               : QString{};
    if (path.isEmpty()) {
        m_avatarError = tr("That image could not be read.");
        Q_EMIT ownAvatarStateChanged();
        return false;
    }
    m_avatarOp = ++m_avatarOpCounter;
    m_avatarError.clear();
    Q_EMIT ownAvatarStateChanged();
    m_client->setOwnAvatar(path, m_avatarOp);
    return true;
}

bool AppController::clearOwnAvatar()
{
    if (m_avatarOp != 0)
        return false;
    if (!canEditOwnAvatar()) {
        m_avatarError = tr("This account cannot change its picture here.");
        Q_EMIT ownAvatarStateChanged();
        return false;
    }
    m_avatarOp = ++m_avatarOpCounter;
    m_avatarError.clear();
    Q_EMIT ownAvatarStateChanged();
    m_client->clearOwnAvatar(m_avatarOp);
    return true;
}

void AppController::dismissOwnAvatarError()
{
    if (m_avatarError.isEmpty())
        return;
    m_avatarError.clear();
    Q_EMIT ownAvatarStateChanged();
}

bool AppController::dispatchOwnDisplayName(const QString &name)
{
    // Claim the op id before the backend call: a synchronous refusal from
    // inside setOwnDisplayName would otherwise be dropped as stale.
    m_displayNameOp = ++m_displayNameOpCounter;
    m_displayNameError.clear();
    Q_EMIT ownDisplayNameStateChanged();
    m_client->setOwnDisplayName(name, m_displayNameOp);
    return true;
}

bool AppController::submitOwnDisplayName(const QString &name)
{
    // Single-flight: the loser of two racing ops would report over the winner.
    if (m_displayNameOp != 0)
        return false;
    const QString unavailable = ownDisplayNameUnavailableReason();
    if (!unavailable.isEmpty()) {
        m_displayNameError = unavailable;
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    // Trimmed only; the interior is sent exactly as typed.
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        // Clearing is a separate action, so a stray select-all cannot erase
        // the name.
        m_displayNameError =
            tr("Enter a name, or use Clear to remove your display name.");
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    if (displayNameLength(wanted) > ownDisplayNameMaxLength()) {
        // Refused rather than truncated, which would send something untyped.
        m_displayNameError = tr("Display names are limited to %1 characters.")
                                 .arg(ownDisplayNameMaxLength());
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    if (wanted == cachedOwnDisplayName()) {
        // The editor disables Save here; nothing to send and no error.
        if (!m_displayNameError.isEmpty()) {
            m_displayNameError.clear();
            Q_EMIT ownDisplayNameStateChanged();
        }
        return false;
    }
    return dispatchOwnDisplayName(wanted);
}

bool AppController::clearOwnDisplayName()
{
    if (m_displayNameOp != 0)
        return false;
    const QString unavailable = ownDisplayNameUnavailableReason();
    if (!unavailable.isEmpty()) {
        m_displayNameError = unavailable;
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    // Not refused when the cached name is empty: the cache may be stale, and
    // clearing an absent field is harmless.
    return dispatchOwnDisplayName(QString{});
}

void AppController::retireOwnDisplayNameWrite()
{
    if (m_displayNameOp == 0 && m_displayNameError.isEmpty())
        return;
    m_displayNameOp = 0;
    m_displayNameError.clear();
    Q_EMIT ownDisplayNameStateChanged();
}

void AppController::retireOwnAvatarWrite()
{
    // An in-flight op outliving its account would leave the next account's
    // control disabled.
    if (m_avatarOp == 0 && m_avatarError.isEmpty())
        return;
    m_avatarOp = 0;
    m_avatarError.clear();
    Q_EMIT ownAvatarStateChanged();
}

void AppController::dismissOwnDisplayNameError()
{
    if (m_displayNameError.isEmpty())
        return;
    m_displayNameError.clear();
    Q_EMIT ownDisplayNameStateChanged();
}

void AppController::switchToAccount(const QString &userId)
{
    const QString target = userId.trimmed();
    if (m_accountSwitching || target.isEmpty())
        return;
    if (target == m_settings->activeAccountUserId() && m_client->isLoggedIn())
        return;
    if (!m_settings->hasSavedAccount(target)) {
        Q_EMIT errorReported(tr("That account is not signed in on this device."));
        return;
    }
    switch (signInStateFor(target)) {
    case SignInState::Usable:
        break;
    case SignInState::Gone:
        Q_EMIT errorReported(
            tr("That account's sign-in has expired. Sign in to it again."));
        return;
    case SignInState::Unreadable:
        // Refuse with the real reason: switching would detach the current
        // session and then fail to restore the new one without its token,
        // and "sign in again" cannot help because a password login on an
        // existing store is bounced back here as ExistingStoreNeedsRestore.
        Q_EMIT errorReported(
            tr("Lightning can't read this device's saved sign-ins right now — "
               "the system keyring is locked or unavailable. Unlock it and "
               "try again."));
        return;
    }

    qCInfo(lcApp) << "account switch begin"
                  << "from=" << matrix::app_data::safeUserSlug(
                         m_settings->activeAccountUserId())
                  << "to=" << matrix::app_data::safeUserSlug(target);
    setAccountSwitching(true);
    m_switchFallbackUserId =
        m_client->isLoggedIn() ? m_settings->activeAccountUserId() : QString{};

    // Leave the call before detachSession() releases the client: the
    // membership retraction and the SFU Leave both need it, otherwise the
    // membership lingers in the room until it expires. leave() is safe in any
    // state, including mid-join.
    if (m_groupCall)
        m_groupCall->leave();

    // Leave the room and thread before detaching so no composer target,
    // pending send, or open thread survives into the next account.
    setCurrentRoomId(QString{});
    m_roomInfo->setRoomId(QString{});

    if (m_client->isLoggedIn() && !m_client->detachSession()) {
        m_switchFallbackUserId.clear();
        setAccountSwitching(false);
        // Either the backend cannot detach, or a real sign-out is still
        // finishing (its completion deletes that account's local data and
        // must not be discarded).
        Q_EMIT errorReported(tr("Could not switch accounts right now. If a "
                                "sign-out is in progress, try again in a "
                                "moment."));
        return;
    }

    m_settings->setActiveAccountUserId(target);
    clearCrossAccountCaches();
    // Shortcut bindings are per account and cached by the registry, so the
    // switch must be announced.
    if (m_shortcuts)
        m_shortcuts->reload();
    // Hidden images are per account and cached too.
    if (m_mediaVisibility)
        m_mediaVisibility->reloadForAccount();
    if (!m_client->restoreSession()) {
        qCWarning(lcApp) << "account switch restore failed"
                         << "slug=" << matrix::app_data::safeUserSlug(target);
        failAccountSwitch(tr("Could not activate the selected account."));
        return;
    }
    // Outcome arrives asynchronously: loginSucceeded clears the switching
    // state; loginFailed falls back through failAccountSwitch.
}

AppController::SignInState
AppController::signInStateFor(const QString &userId) const
{
    // The mock backend holds no real credentials — its accounts restore from
    // the registry — so classifying one by a token read would report every
    // mock account as signed out.
    if (m_backend == MockBackend)
        return SignInState::Usable;
    // A successful read is the only evidence of Usable, and it is tested
    // first: with the insecure fallback store substituted for an unavailable
    // native backend, tokens read back fine and the user must not be told to
    // unlock a keyring that does not exist.
    if (m_settings && !m_settings->accessTokenFor(userId).isEmpty())
        return SignInState::Usable;
    // No token in hand. AccountManager::needsSignIn() owns the read-then-ask
    // ordering for "is this account genuinely signed out?"; do not duplicate it.
    if (m_accounts && m_accounts->needsSignIn(userId))
        return SignInState::Gone;
    // Empty read and the backend cannot vouch for it: genuinely unknown.
    return SignInState::Unreadable;
}

void AppController::failAccountSwitch(const QString &message)
{
    const QString fallback = m_switchFallbackUserId;
    m_switchFallbackUserId.clear();
    if (!message.isEmpty())
        Q_EMIT errorReported(message);
    if (!fallback.isEmpty() && m_settings->hasSavedAccount(fallback)) {
        qCInfo(lcApp) << "account switch falling back"
                      << "slug=" << matrix::app_data::safeUserSlug(fallback);
        m_settings->setActiveAccountUserId(fallback);
        clearCrossAccountCaches();
        if (m_client->restoreSession())
            return; // completion (or a second failure) arrives async
    }
    setAccountSwitching(false);
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

bool AppController::resolveRemovalIdentity(
    const QString &userId, matrix::app_data::AccountIdentity *identity) const
{
    if (!identity)
        return false;
    *identity = {};
    // Resolve from the saved record, which binds the store slug this account
    // actually uses; re-deriving it from the user id could miss a divergent
    // store and leave keys on disk while reporting success.
    if (m_settings->resolveSavedIdentity(userId, identity))
        return true;
    // Fall back to the canonical layout so a removal still removes something.
    const QString hs = m_settings->accountRecord(userId)
                           .value(QStringLiteral("homeserver")).toString();
    return matrix::app_data::resolveAccountIdentity(hs, userId, identity);
}

void AppController::removeAccountLocalState(
    const matrix::app_data::AccountIdentity &identity)
{
    // Retiring a Rust client is asynchronous, so its SQLite store may still be
    // open. Wait for the close before deleting, or key material can survive a
    // removal that reports success.
#ifdef ENABLE_RUST_SDK_BACKEND
    RustSdkMatrixClient::waitForRustRetirement(
        RustSdkMatrixClient::kStoreCloseBudgetMs);
#endif
    const auto removed = matrix::app_data::removeAccountRustState(identity);
    // The avatar store is app-level, not account-scoped, so the directory
    // sweep below does not cover it.
    if (m_accountAvatars)
        m_accountAvatars->forget(identity.userId);

    // The starred-GIF store lives under the canonical account root, which can
    // differ from identity.accountRoot for a divergent store slug, so it gets
    // its own explicit deletion. Close it first if this process has it open
    // (the background-removal path does not trigger MatrixClient::loggedOut).
    const QString starredDir =
        matrix::app_data::starredGifsDir(identity.userId);
    if (m_gif->starredStore()->currentDirectory() == starredDir)
        m_gif->closeStarredStore();
    // The bridge badge file is swept with the canonical root below, but an
    // open store would write it back on its next answer, so close it first.
    const QString bridgeFile =
        matrix::app_data::bridgeLabelsFile(identity.userId);
    if (!bridgeFile.isEmpty() && m_bridgeLabels.filePath() == bridgeFile)
        m_bridgeLabels.close();
    const auto starredOutcome = matrix::app_data::removeAppDataDir(starredDir);
    // A failure leaves decrypted material behind, so it warns.
    if (starredOutcome == matrix::app_data::DirRemoval::Failed) {
        qCWarning(lcApp) << "removing account starred-GIF store FAILED"
                         << "slug=" << identity.slug;
    } else {
        qCInfo(lcApp) << "removed account starred-GIF store"
                      << "slug=" << identity.slug
                      << "outcome="
                      << (starredOutcome
                                  == matrix::app_data::DirRemoval::Deleted
                              ? "deleted"
                              : "absent");
    }

    // A divergent store slug means two directories: the recorded one holding
    // the SDK store and the canonical one where CacheStore::openFor() puts
    // cache.sqlite. Both must go.
    QStringList roots{identity.accountRoot};
    const QString canonical = matrix::app_data::accountRoot(identity.userId);
    if (!canonical.isEmpty() && !roots.contains(canonical))
        roots.append(canonical);
    int rootsDeleted = 0;
    int rootsFailed = 0;
    for (const QString &root : roots) {
        QDir accountDir(root);
        if (!accountDir.exists())
            continue;
        if (accountDir.removeRecursively())
            ++rootsDeleted;
        else
            ++rootsFailed;
    }
    // Absent, deleted and failed are distinct outcomes; a failure leaves the
    // account's data on disk after the user asked for it to be gone.
    if (rootsFailed > 0) {
        qCWarning(lcApp) << "removing account local state FAILED for"
                         << rootsFailed << "root(s)"
                         << "slug=" << identity.slug;
    }
    qCInfo(lcApp) << "removed account local state"
                  << "slug=" << identity.slug
                  << "roots=" << roots.size()
                  << "roots_deleted=" << rootsDeleted
                  << "roots_failed=" << rootsFailed
                  << "rust_removed_anything=" << removed.removedAnything()
                  << "rust_deleted=" << removed.deleted
                  << "failed=" << removed.failed;
}

void AppController::removeAccount(const QString &userId)
{
    const QString target = userId.trimmed();
    if (target.isEmpty() || m_accountSwitching)
        return;
    if (!m_settings->hasSavedAccount(target))
        return;

    const bool isActive = target == m_settings->activeAccountUserId();
    if (isActive && m_client->isLoggedIn()) {
        // The active account's store is open and needs a real server logout
        // first, so record the intent and finish in onLoggedOut. Resolve the
        // identity now, while the saved record still exists: the sign-out
        // removes it, and deletion must key on the record.
        matrix::app_data::AccountIdentity identity;
        m_pendingRemovalResolved = resolveRemovalIdentity(target, &identity);
        m_pendingRemovalIdentity = identity;
        m_pendingRemovalUserId = target;
        if (!m_pendingRemovalResolved) {
            qCWarning(lcApp) << "removal could not resolve an account layout"
                             << "slug=" << matrix::app_data::safeUserSlug(target);
        }
        m_auth->logout();
        return;
    }

    // Background (or signed-out) account: delete its local state without
    // touching the active session.
    matrix::app_data::AccountIdentity identity;
    if (resolveRemovalIdentity(target, &identity)) {
        removeAccountLocalState(identity);
    } else {
        qCWarning(lcApp) << "removal could not resolve an account layout"
                         << "slug=" << matrix::app_data::safeUserSlug(target);
    }
    m_accounts->removeAccount(target); // record + secrets
    if (isActive)
        m_lastSessionUserId.clear();
}

// Resolved in C++ because Qt's automatic per-character font fallback is
// version-dependent: Qt 6.8 prefers a monochrome font that claims the emoji
// codepoint, Qt 6.11 does not, while naming the family works on both. QML's
// font type has `family` but no `families`, and the first family the host
// actually has beats a hard-coded name.
QString AppController::emojiFontFamily() const
{
    // One resolver for the whole process, shared with the default font fallback.
    return FontManager::emojiFamily();
}

// The composer's font: the UI face first, the colour emoji face behind it.
// setFamilies() gives real per-character fallback in one text run, which QML
// cannot express (no `families`), and relying on Qt's automatic fallback gets
// monochrome emoji on Qt 6.8.
QFont AppController::textFontWithEmoji(const QString &family, int pixelSize,
                                       bool italic) const
{
    QFont font;
    if (pixelSize > 0)
        font.setPixelSize(pixelSize);
    font.setItalic(italic);
    // The base face comes from the caller so the theme keeps deciding it.
    QStringList families;
    if (!family.isEmpty())
        families << family;
    const QString emoji = emojiFontFamily();
    // Empty when the host has no emoji font: then this is just the UI face.
    if (!emoji.isEmpty())
        families << emoji;
    if (families.isEmpty())
        return font;
    font.setFamilies(families);
    return font;
}

void AppController::setSoftwareRenderer(bool software)
{
    if (m_softwareRenderer == software)
        return;
    m_softwareRenderer = software;
    Q_EMIT softwareRendererChanged();
}
