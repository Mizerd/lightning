#include "app/AppController.h"

#include "app/FontManager.h"

#include "app/RichComposerBridge.h"
#include "crypto/BackupController.h"
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
#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstCallMediaBackend.h"
#include "calls/ScreenCastPortal.h"
#include "calls/SfuMediaEngine.h"
#endif
#include "presence/PresenceManager.h"
#include "threads/ThreadManager.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

// Pure failure classification, no Rust dependency: compiled into every
// configuration so the repair policy answers the same way in all of them.
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
// How long after startup the optional automatic update check may run. The
// privacy documentation states nothing is contacted in the first 30 seconds,
// and startup must never wait on the network; the rate limit itself lives in
// UpdateManager::maybeCheckAutomatically().
//
// DERIVED from the manager's own quiet period, with a margin, and not simply
// written as 30s again. maybeCheckAutomatically() refuses while the process is
// younger than kStartupQuietPeriodMs, and QTimer::singleShot uses a coarse
// timer at this scale, which is allowed to fire EARLY. With the two values
// equal, one early millisecond made this one-shot a no-op and the user's
// enabled preference did nothing for the whole session -- silently, since a
// refusal is not an error. The margin also keeps the two from drifting apart
// if the quiet period is ever changed.
constexpr int kAutomaticUpdateCheckDelayMs =
    int(lightning::update::UpdateManager::kStartupQuietPeriodMs) + 5 * 1000;
} // namespace

bool AppController::isBackendCompiled(Backend backend)
{
    switch (backend) {
    case MockBackend:
    case HttpBackend:
#ifdef LIGHTNING_RUST_ONLY
        // Rust-only release: mock/http are not compiled in, so preflight
        // (main.cpp) rejects --backend=mock/http/--mock with the standard
        // "backend not compiled into this build" error.
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
    // Rust-only release build: the HTTP/mock backends are not compiled in and
    // preflight (main.cpp) rejects any non-Rust --backend before construction.
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
        // Construction should be rejected upstream (main.cpp checks
        // isBackendCompiled). Fall back to HTTP defensively so the app does
        // not crash if we ever reach this path.
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
    // Screenshot-demo mode must never construct a production secure store: a
    // libsecret/keychain store's constructor probes the real Secret Service and
    // is not isolated by the demo applicationName. An in-memory store touches no
    // libsecret, no keychain, and no file (see beginScreenshotDemo's assertion).
    , m_secretStore(screenshotDemo
                        ? std::unique_ptr<SecretStore>(
                              std::make_unique<InMemorySecretStore>(this))
                        : SecretStore::createDefault(this))
    , m_settings(std::make_unique<SettingsManager>(this))
{
#ifdef LIGHTNING_RUST_ONLY
    // Fail closed: a release binary must never run a non-Rust backend. Preflight
    // already rejects non-Rust --backend values (they are not compiled in), so
    // this only fires if that invariant is ever bypassed.
    if (backend != RustBackend)
        qFatal("LIGHTNING_RUST_ONLY: refusing to start a non-Rust backend");
#endif
    // Wire the SecretStore before anything reads accessToken(). Migrates any
    // legacy plaintext token into the SecretStore on first entry.
    m_settings->setSecretStore(m_secretStore.get());

    // Built here rather than in main.cpp so QML reaches it as app.localization
    // alongside every other controller. It installs no catalog until
    // applyStoredLanguage() runs — main.cpp does that before the QML engine
    // loads, so the first frame is already in the user's language.
    m_localization = std::make_unique<LocalizationManager>(m_settings.get(), this);
    m_customTheme = std::make_unique<CustomThemeStore>(m_settings.get(), this);
    // Constructed with the settings, before any QML exists: the first frame
    // must already carry the user's bindings, not the defaults followed by
    // a correction the eye can catch.
    m_shortcuts = std::make_unique<ShortcutRegistry>(m_settings.get(), this);
    m_railLayout = std::make_unique<RailLayoutStore>(m_settings.get(), this);
    m_railEntries = std::make_unique<RailEntryModel>(this);
    m_mediaVisibility = std::make_unique<MediaVisibilityStore>(this);
    // Persistence is account-scoped, so this also loads whatever the account
    // that is active right now had hidden.
    m_mediaVisibility->setSettings(m_settings.get());
    m_banners = std::make_unique<ProfileBannerManager>(this);
    m_nameColors = std::make_unique<NameColorManager>(this);
    m_bio = std::make_unique<ProfileBioManager>(this);
    m_userProfiles = std::make_unique<UserProfileResolver>(this);
    // A fixed local table, so it needs no client and no session: it is
    // decoration, not Matrix state (see ProfileBadges).
    m_badges = std::make_unique<ProfileBadges>(this);
    // The cropper stages its preview through the SAME in-memory token store
    // the composer uses, so QML never points an Image at a user-chosen
    // file:// path — which is what would let an .svg render (CLAUDE.md §6).
    m_imageCrop.setStagedImages(&m_stagedImages);

    m_client       = makeClient(backend, m_settings.get(), this);
    m_accounts     = std::make_unique<AccountManager>(m_settings.get(), this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_quickSwitcher = std::make_unique<QuickSwitcherModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    m_timelineView = std::make_unique<ReverseListProxyModel>(this);
    m_timelineView->setSourceModel(m_timeline.get());
    m_composer     = std::make_unique<MessageComposer>(this);
    // MSC4108. Given the SAME code store verification uses: both DISPLAY one
    // code at a time, and a stale token renders nothing, so one slot is
    // correct rather than merely convenient.
    m_policy       = std::make_unique<PolicyListController>(this);
    m_qrLogin      = std::make_unique<QrLoginController>(this);
    m_qrLogin->setQrStore(&m_qrCodeStore);
    m_richComposer = std::make_unique<RichComposerBridge>(this);
    m_richComposer->setComposer(m_composer.get());
    // Clipboard images never become files, so their bytes are registered
    // here for the composer chip to preview. One store, both composers.
    m_composer->attachments()->setStagedImages(&m_stagedImages);

    // The composer's spell checker, resolved against the user's spelling
    // preference ("" = the SYSTEM locale rather than the UI language: a user
    // reading Lightning in English still types Lithuanian, and it is the
    // keyboard that decides which dictionary is the right one). Resolving
    // costs one dlopen (Linux), one CoCreateInstance (Windows) or one AppKit
    // singleton (macOS) and answers "unavailable" honestly when the machine
    // has no dictionary; `--spell-status` prints what happened. Both knobs
    // are application settings: Settings writes them, this pushes them in.
    m_spell.initialize(m_settings->spellCheckLanguage());
    m_spell.setEnabled(m_settings->spellCheckEnabled());
    connect(m_settings.get(), &SettingsManager::spellCheckEnabledChanged, this,
            [this] { m_spell.setEnabled(m_settings->spellCheckEnabled()); });
    connect(m_settings.get(), &SettingsManager::spellCheckLanguageChanged, this,
            [this] { m_spell.setPreferredLanguage(m_settings->spellCheckLanguage()); });

    // Privacy: the two settings that decide what this device DISCLOSES while
    // the user is simply reading and typing. Pushed in the same shape as the
    // spell knobs — Settings writes, this applies — and applied once here so
    // a stored choice is in force from the first receipt, not from the first
    // time the user reopens the settings page.
    applyPrivacyPreferences();
    connect(m_settings.get(), &SettingsManager::readReceiptModeChanged, this,
            [this] { applyPrivacyPreferences(); });
    // MSC4153 is applied at CLIENT BUILD time, so it is pushed here — before
    // any sign-in — and again whenever it changes, which affects the NEXT
    // client. The UI says so; nothing here pretends it is live.
    connect(m_settings.get(), &SettingsManager::strictDeviceTrustChanged, this,
            [this] { applyStrictDeviceTrust(); });
    applyStrictDeviceTrust();
    connect(m_settings.get(), &SettingsManager::sendTypingNotificationsChanged,
            this, [this] { applyPrivacyPreferences(); });

    // System tray. Created only while the user has asked for it — an icon in
    // somebody's tray for a feature they never turned on is noise — and only
    // where the platform actually has one.
    connect(&m_tray, &TrayIcon::showRequested,
            this, &AppController::trayShowRequested);
    connect(m_settings.get(), &SettingsManager::closeToTrayChanged,
            this, &AppController::refreshTrayState);
    connect(m_settings.get(), &SettingsManager::notificationsEnabledChanged,
            this, &AppController::refreshTrayState);
    refreshTrayState();

    // Window geometry: settle the "can this still be restored?" question once,
    // here, while the display layout is the one the window is about to open
    // onto. SettingsManager already refused a size below the window's own
    // minimum, so what is left is the position.
    //
    // The test is a BAND along the top of the frame — the part the user has to
    // be able to grab. Requiring the whole rect to sit on one screen would
    // refuse a window legitimately spanned across two monitors. No screens at
    // all (a guiless run) is not an invitation to guess: the geometry is
    // dropped and the window uses its own default placement.
    if (const QRect stored = m_settings->initialWindowGeometry();
            !stored.isEmpty()) {
        if (windowGeometryIsReachable(stored))
            m_restorableWindowGeometry = stored;
        else
            qCInfo(lcApp, "stored window geometry ignored: off-screen");
    }
    // v0.7.x drafts: one shared store (room + thread composers), policy in
    // DraftStore — persisted only for unencrypted rooms.
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
    // Voice calls (2026-08-18 rounds 1-3): the signaling state machine,
    // and — when the build carries the GStreamer webrtcbin engine AND its
    // element factories resolve at runtime — the real media backend that
    // makes placing/answering calls possible. Absent engine = the honest
    // refusal path, exactly as before. LIGHTNING_DISABLE_WEBRTC=1 is the
    // kill switch (diagnosis, or a machine whose plugins misbehave).
    m_calls        = std::make_unique<CallController>(this);
    m_rtc          = std::make_unique<RtcController>(this);
    m_groupCall    = std::make_unique<SfuCallController>(this);
    m_callDevices  = std::make_unique<CallDeviceController>(this);
    // Application updates. Constructed once and never rebuilt: it holds no
    // Matrix state, is not account-scoped, and signing in, signing out or
    // switching account must not disturb an update check or download.
    m_updateManager = std::make_unique<lightning::update::UpdateManager>(this);
    // The automatic check, if the user enabled it. Deliberately delayed: an
    // update check must never sit between the user and a usable application,
    // and the privacy documentation promises nothing happens in the first 30
    // seconds. maybeCheckAutomatically() itself enforces the preference and
    // the once-per-24h rate limit, so this is only the trigger -- and it is a
    // ONE-SHOT, never re-armed on room or account changes.
    QTimer::singleShot(kAutomaticUpdateCheckDelayMs, m_updateManager.get(), [this] {
        m_updateManager->maybeCheckAutomatically();
    });
    connect(m_updateManager.get(), &lightning::update::UpdateManager::quitRequested,
            this, [] {
                // installAndRestart() has staged a verified artifact and handed
                // it to the helper, which waits for this process to exit before
                // touching anything. Quit through the event loop so normal
                // shutdown still runs; the helper relaunches us afterwards.
                QCoreApplication::quit();
            });
    m_pinned       = std::make_unique<PinnedMessagesController>(this);
    m_roomUpgrade  = std::make_unique<RoomUpgradeController>(this);
    m_thread       = std::make_unique<ThreadController>(this);
    m_thread->attachments()->setStagedImages(&m_stagedImages);
    // The rich composer bridge serves both composers; the thread one exists
    // only from here on.
    m_richComposer->setThread(m_thread.get());
    m_conversations= std::make_unique<ConversationController>(this);
    m_discovery = std::make_unique<RoomDiscoveryController>(this);
    m_messageSearch = std::make_unique<MessageSearchController>(this);
    m_uia = std::make_unique<UiaController>(this);
    m_moderation = std::make_unique<ModerationController>(this);
    m_forward      = std::make_unique<ForwardController>(this);
    m_roomInfo     = std::make_unique<RoomInfoController>(this);
    m_mediaBridge  = std::make_unique<MediaBridge>(this);
    // The tray balloon is the notification delivery where there is no
    // freedesktop daemon (Windows, macOS) — see refreshTrayState for why the
    // icon shows there.
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

    // Inline custom emoji (MSC2545). The sanitizer keeps the `mxc:` form —
    // deliberately, so an edited message cannot carry a local source back to
    // the room — and this is what turns it into something the view can draw,
    // through the SAME authenticated media path every attachment uses.
    // 64px because an emoticon renders at 20 and a HiDPI screen doubles it.
    // The composer's half of MSC2545: a `:shortcode:` the user has installed
    // becomes an inline image on send, and stays literal text when it is not
    // one of theirs.
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
                // An emoji resolves to "" until its bytes arrive; this is the
                // re-read that replaces the shortcode with the image. It
                // costs nothing in a timeline with no emoji in it.
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
    // v0.6.6: a local-favorite send reads the stored bytes straight off
    // disk by content hash — no network, no MatrixClient::gifDownload().
    m_gifSend->setLocalGifReader([this](const QString &hash) {
        return m_gif->starredStore()->readBytes(hash);
    });
    // The star-fetch trigger (starChatGif) lives on MediaBridge and stays
    // GIF-agnostic; this is the one place its result is routed into the
    // local-starred store, keeping src/media/ and src/gif/ decoupled from
    // each other.
    connect(m_mediaBridge.get(), &MediaBridge::mediaBytesForStar, this,
            [this](const QString &mediaKey, bool ok, const QByteArray &bytes,
                   const QString &category) {
        // ONLY keys this account actually asked to star. The fetch trigger
        // is media-generic and now has a second caller (ForwardController),
        // and an unconditional handler here would write the decrypted bytes
        // of every forwarded image into the account's on-disk saved-media
        // store — which §6 forbids and §7 permits only as an explicit
        // export the user chose. Forwarding is not that choice: it would
        // persist decrypted media nobody asked to keep, consume the store's
        // 200-item budget, and render the row's star as filled.
        // Copy-to-clipboard consumer (2026-08-18 tester report #2): a
        // TRANSIENT export on explicit user action — nothing persists, so
        // the saved-GIF store's deletion machinery does not apply; the
        // pending-key discipline (the same one that keeps forwards out of
        // the star store) still does.
        // The bridge dedups in-flight fetches purely by key, so when a
        // star and a copy race on the SAME image exactly ONE signal
        // arrives — it must service BOTH claims, or the loser is left
        // stuck pending with no result and no feedback (review find,
        // 2026-08-18). Never an early return between the two branches.
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
    // v0.7: dimensions learned by the poster extractor persist per account,
    // so a metadata-less video's card takes its true shape from the first
    // render on every later visit (only the first-ever encounter can still
    // start on the 16:9 guess). Dimensions only — never content.
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

    // GIF policy follows the persisted settings live.
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
    // v0.7.x verification badges: the warning is a function of BOTH the
    // trust state and the per-account dismissal, so it has to re-notify on
    // either. securityStateChanged also drives it (see below).
    connect(m_settings.get(),
            &SettingsManager::verificationWarningDismissedChanged, this,
            [this] { Q_EMIT sessionVerificationWarningChanged(); });
    connect(this, &AppController::securityStateChanged, this,
            [this] { Q_EMIT sessionVerificationWarningChanged(); });
    m_timelineScroll = std::make_unique<TimelineScrollController>(this);
    m_threadScroll   = std::make_unique<TimelineScrollController>(this);

    m_crypto->setBackendName(backendName());

    // A typing notification is the one live, present-tense fact the server
    // forwards about somebody else, and it CONTRADICTS a cached "offline"
    // (a homeserver with presence switched off answers 200 "offline" for
    // everyone, so the refusal latch never fires). PresenceManager
    // WITHDRAWS the contradicted claim; it never promotes anyone to online.
    connect(m_client.get(), &MatrixClient::typingChanged, this,
            [this](const QString &roomId) {
        if (!m_presence || MatrixClient::isThreadTimelineId(roomId))
            return;
        const QStringList typists = m_client->typingUsersFor(roomId);
        for (const QString &userId : typists)
            m_presence->noteTyping(userId);
    });

    // v0.6.0 checkpoint 11: native notifications. Every appended remote
    // event (any room, incl. thread-timeline copies which are filtered by
    // their composite id) runs the pure decision; the manager delivers via
    // freedesktop DBus. Bodies are never logged or persisted.
    connect(m_client.get(), &MatrixClient::eventAppended, this,
            [this](const QString &composedRoomId, const TimelineEvent &event) {
        // A THREAD COPY IS MAPPED, NOT DROPPED, and the comment that used to
        // sit here — "the room copy of the same event already notifies" — was
        // false on the Rust backend. The live room timeline is built with
        // `hide_threaded_events: true`, so there IS no room copy of a thread
        // reply; and the raw-sync mirror, the only other producer, early
        // returns for the room that is currently OPEN. So for the open room a
        // thread reply reached this handler from nowhere at all: someone
        // @-mentioning you in a thread of the room on your screen produced no
        // notification, no sound and no Activity Center row, while the same
        // mention in a BACKGROUND room worked. Leaving a room open made you
        // less likely to be told about a mention in it.
        //
        // The composite never leaves this scope (§8): everything downstream
        // sees the real room id, and the payload's threadRootId still routes
        // the click. Double notification is prevented by event id rather than
        // by discarding the copy, which is what the old comment was reaching
        // for.
        const bool fromThread = MatrixClient::isThreadTimelineId(composedRoomId);
        const QString roomId = fromThread
            ? MatrixClient::threadTimelineRoomId(composedRoomId)
            : composedRoomId;
        if (roomId.isEmpty())
            return;
        // Dedup on BOTH branches. Checking only the thread copy made the
        // claim half true: if a room copy of the same event ever arrives
        // (a backend without hide_threaded_events, or the mock), it would
        // still notify a second time. Any event carrying a thread root is a
        // candidate for both producers, so both consult the same set.
        if (fromThread || !event.threadRootId.isEmpty()) {
            if (event.eventId.isEmpty()
                || m_notifiedThreadEventIds.contains(event.eventId))
                return;
            m_notifiedThreadEventIds.insert(event.eventId);
            // Bounded: this only has to outlive the moment two producers
            // could both deliver one event, not the session.
            if (m_notifiedThreadEventIds.size() > 512)
                m_notifiedThreadEventIds.clear();
        }
        // Receiving activity is a reason to refresh a visible sender's
        // presence promptly, but never evidence for fabricating "online".
        // PresenceManager applies only the homeserver's subsequent answer.
        if (m_presence && event.sender != m_client->currentUserId())
            m_presence->noteActivity(event.sender);
        NotificationManager::Context context;
        context.selfUserId = m_client->currentUserId();
        // Targeted lookup — the previous rooms() call deep-copied the whole
        // room list (every QString and member hash) once per appended event,
        // which is O(events x rooms) across a sync burst.
        const RoomInfo info = m_client->roomInfo(roomId);
        context.roomName = info.name.isEmpty() ? roomId : info.name;
        context.roomIsDirect = info.isDirect;
        const QVariantMap notifyRoomRow = m_roomList->findRoom(roomId);
        context.avatarMxc =
            notifyRoomRow.value(QStringLiteral("avatarUrl")).toString();
        // Drives the initials disc when there is no avatar to fetch. Taken
        // from the same row the interface colours its own avatar from, so a
        // notification and the room list never disagree about an identity.
        context.avatarColorKey =
            notifyRoomRow.value(QStringLiteral("identityColorKey")).toString();
        // ...and the theme the disc is coloured from, for the same reason.
        context.themeId = int(m_settings->theme());
        context.roomMode = static_cast<NotificationManager::RoomMode>(
            m_settings->roomNotificationMode(roomId));
        // An encrypted room may withhold more than the rest. The room row is
        // the only place this layer knows the room's encryption from, and it
        // carries `encryptionKnown` separately because "not known yet" is a
        // real third state during hydration — see effectiveNotificationPreview.
        context.previewMode = static_cast<NotificationManager::PreviewMode>(
            m_settings->effectiveNotificationPreview(
                notifyRoomRow.value(QStringLiteral("encrypted")).toBool(),
                notifyRoomRow.value(QStringLiteral("encryptionKnown")).toBool()));
        context.notificationsEnabled = m_settings->notificationsEnabled();
        // "ON SCREEN" IS THREAD-AWARE. A reply in a thread whose panel is not
        // open is not visible just because the room behind it is: the room
        // timeline hides threaded events, so there is nothing on screen for
        // the user to have read. Suppressing on the room's visibility alone
        // is what kept an @-mention in a thread of the OPEN room silent, and
        // remapping the event to its real room id (above) did not by itself
        // change that — it only restored the Activity Center row and the
        // scrolled-away case.
        const bool threadPanelShowingThis =
            fromThread && m_thread
            && m_thread->rootEventId() == event.threadRootId;
        context.roomVisibleAtLatest =
            roomId == m_currentRoomId && m_activeRoomAtLatest
            && (!fromThread || threadPanelShowingThis);
        // The open room's own backlog arrives as live appends the moment
        // sliding sync subscribes it, while its view is still hydrating and
        // therefore not yet "at latest".
        context.roomHydrating =
            roomId == m_currentRoomId && m_activeRoomHydrating;
        // Suppress the initial-sync backlog: those events are pre-existing
        // history, not fresh activity, and must not re-notify on each launch.
        context.initialSyncComplete = m_client->initialSyncDone();
        context.soundMode = static_cast<NotificationManager::SoundMode>(
            m_settings->notificationSound());
        // v0.7.x: belt-and-braces for the ignore race window — the server
        // stops sending an ignored user's events, but ones already in
        // flight must not notify.
        context.senderIsIgnored =
            m_moderation && m_moderation->isIgnored(event.sender);
        m_notifications->processEvent(event, context);
        // v0.9 (phase 2): the same event feeds the Activity Center — its
        // classifier is independent of the notification decision (a muted
        // room's mention is still activity).
        m_activity->ingest(event, context.roomName);
    });
    connect(m_notifications.get(), &NotificationManager::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        Q_EMIT notificationOpenRequested(roomId, eventId, threadRootId);
    });
    // ── Notification actions, and the account check they both need ──────
    //
    // A notification card outlives the account that raised it. The user can
    // switch accounts, or sign out, while it is still on screen — and the
    // desktop will happily deliver the action minutes later. Acting on it
    // under whichever account is current would mark ANOTHER account's room
    // read, or worse, send a reply from the wrong identity into a room the
    // current account may not even be in. Nothing would report it: the send
    // would succeed.
    //
    // So both actions are refused on a mismatch and the user is told, rather
    // than silently switching the account for them — a notification button
    // is not an instruction to change who you are signed in as.
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
        // A reply to a THREADED message belongs in that thread. Sending it
        // to the room instead would be a visible mistake — §8's first
        // invariant — and the payload has carried the root all along.
        if (!threadRootId.isEmpty())
            m_client->sendThreadReply(roomId, threadRootId, text);
        else
            m_client->sendTextMessage(roomId, text);
        // Replying IS reading. Leaving the room unread after the user has
        // answered it is the kind of small wrongness that makes a feature
        // feel broken.
        if (m_roomList)
            m_roomList->markRoomRead(roomId);
        m_notifications->closeRoomNotifications(roomId);
        qCInfo(lcApp) << "notification reply sent"
                      << "thread=" << !threadRootId.isEmpty();
    });
    // Server-reported per-room notification mode. ONLY an explicit
    // user-defined room rule reconciles the device-local cache (server
    // wins for real rules): a resolved account DEFAULT must never mutate
    // persisted state — it would silently destroy a device-local choice an
    // upgrading user made before server sync existed (and, resolved from
    // an unloaded ruleset, could rewrite policy with a guess). The display
    // consequence is accepted and deliberate: a room following an account
    // default that differs from the local value keeps showing the local
    // value in the pickers; reflecting defaults without persisting them is
    // a follow-up. SettingsManager::setRoomNotificationMode is idempotent,
    // and server writes are issued only from the UI entry point
    // (AppController::setRoomNotificationMode), so an echo of our own
    // write can never loop back into another server write.
    // Stale-generation events are already rejected inside
    // RustSdkMatrixClient, so a report from a previous account cannot
    // reach the next account's settings.
    connect(m_client.get(), &MatrixClient::roomNotificationModeChanged, this,
            [this](const QString &roomId, int mode, bool userDefined) {
        if (!userDefined) {
            qCDebug(lcApp) << "room notification default report (not persisted)";
            return;
        }
        // Defence-in-depth: the dispatcher range-guards too, but this
        // handler is also reachable from tests/backends directly. Dropping
        // is the conservative choice (SettingsManager would clamp to 0 —
        // the LEAST conservative mode).
        if (mode < 0 || mode > 2)
            return;
        // While a room carries kept-on-this-device failure state, the
        // local value is authoritative: a failed write never reached the
        // SDK's rules, so a poll can report the room's OLD explicit rule
        // as user-defined. Applying it would silently revert the user's
        // choice and erase the honest failure chip in the same stroke.
        // Only a report that EQUALS the cached value is a real write
        // acknowledgement; a differing one is dropped.
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
    // A successful rule REMOVAL. This is the only acknowledgement a
    // "follow account default" choice can ever receive, so it must retire
    // the room's kept-on-this-device state — otherwise a clear that failed
    // once and then succeeded on retry would keep claiming it had failed,
    // and Lightning would re-issue the deletion on every later reconnect.
    connect(m_client.get(), &MatrixClient::roomNotificationModeCleared, this,
            [this](const QString &roomId) {
        // Only meaningful while the local value actually is "follow
        // default": a clear acknowledged after the user has since chosen an
        // explicit mode belongs to a superseded choice and must not retire
        // that newer choice's pending state.
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
                // Close the local-starred-GIF store's live handle (if any)
                // on EVERY session detach — a genuine sign-out, an
                // account-removal-via-logout, AND a plain account SWITCH
                // (MatrixClient::detachSession() also emits this). This
                // matters most for the SWITCH case: AuthManager's own
                // handler on this same signal runs first (registered
                // earlier, in its own constructor) and synchronously
                // triggers AppController::onLoggedOut(), which returns
                // immediately while m_accountSwitching is true — WITHOUT
                // touching the store — so without this line the store would
                // keep showing the outgoing account's rows in the picker's
                // Starred tab until the new account's login succeeds
                // and calls openStarredStoreFor(). Idempotent/harmless to
                // call again here for a genuine sign-out too, where
                // onLoggedOut() (which ran just before this) already closed
                // it as part of its own deletion. The actual on-disk
                // deletion (sign-out/removal only, never a plain switch) is
                // separate — see onLoggedOut() and removeAccount().
                m_gif->closeStarredStore();
                m_notifications->clearPending();
                m_knownInvites.clear();
                // Session-scoped sync-failure state must not leak into the
                // next account. No per-room signals: the pickers re-query
                // when they (re)open.
                m_notificationModeSyncFailures.clear();
                // A session rename in flight at sign-out must not lock
                // renaming for the next account (its answer, if it ever
                // comes, belongs to nobody now).
                if (m_sessionDeviceRenameOp != 0
                    || !m_sessionDeviceRenameError.isEmpty()) {
                    m_sessionDeviceRenameOp = 0;
                    m_sessionDeviceRenameError.clear();
                    Q_EMIT sessionDevicesChanged();
                }
                m_activitySeeded = false;
            });
    // Room-open roster hydration marks a room BEFORE its fetch resolves;
    // a failed fetch must un-mark it or the room's mention chips and reply
    // headers stay localparts for the whole session (review: a silent
    // one-shot must not fail closed). The next open retries.
    connect(m_client.get(), &MatrixClient::roomMembersReceived, this,
            [this](quint64, const QString &roomId, const QVariantMap &snapshot) {
                if (!snapshot.value(QStringLiteral("ok")).toBool())
                    m_memberHydratedRooms.remove(roomId);
            });
    // Invites: notify once per newly seen invited room. Invites present
    // before the initial sync completes are seeded silently (see
    // shouldNotifyInvite) so a restart never re-announces existing invites.
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
    // The tray badge follows the SAME TWO signals the room list does, so it
    // can never disagree with what the window shows. Recomputing is a walk
    // over a local snapshot with no I/O in it, and the icon is only
    // rasterised when the displayed badge actually changes.
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
    // A fresh client starts with PUBLIC receipts, so the stored privacy
    // choice has to be pushed at every attachment, not only when it changes.
    applyPrivacyPreferences();
    m_spaces->setClient(m_client.get());
    m_threads->setClient(m_client.get());
    m_presence->setClient(m_client.get());
    m_calls->setClient(m_client.get());
    m_rtc->setClient(m_client.get());
    m_groupCall->setClient(m_client.get());
    m_groupCall->setRtcController(m_rtc.get());
    // WITHOUT THIS THE WHOLE VOLUME FEATURE IS INERT, and silently so.
    // SfuCallController reads the stored microphone gain and every stored
    // per-participant level through m_settings, and subscribes to their
    // change signals through it too. It was never handed one — so the
    // pointer stayed null, the connections were never made, applyAudioState()
    // fell back to unity on every join, and nothing was ever persisted or
    // restored. Reported as all three at once: "sound amplifier does
    // nothing", "i cant make myself louder and i cant make other louder",
    // "it doesnt remeber my volumnes set on user". One missing wire.
    m_groupCall->setSettings(m_settings.get());
    // ONE conversation, two doors. A ring arrives on the notification lane
    // (CallController); the user can answer it by pressing Accept on the
    // ring card, or by opening the room and pressing Join — which goes to
    // the MatrixRTC lane and told the ringing lane nothing. So the ring card
    // and its desktop notification stayed up over a call the user was
    // already in, and had to be dismissed by hand.
    //
    // Connected once here rather than called from inside join(): every path
    // that makes a group call live passes through this state change, so a
    // future entry point cannot forget it.
    // A REFUSED JOIN IS SAID OUT LOUD. The controller has carried the reason
    // ("You don't have permission to join this call.") since the MatrixRTC
    // round and emitted callFailed with it, and nothing was connected to
    // that signal — so a member without the power level for the membership
    // state event pressed Join, the prompt vanished, and the banner went on
    // offering Join with no explanation (seen on the 2026-09-06 GUI pass in
    // a room whose state_default was 50). The status bar shows it, exactly
    // like every other reported error.
    connect(m_groupCall.get(), &SfuCallController::callFailed, this,
            [this](const QString &reason) {
                if (!reason.isEmpty())
                    Q_EMIT errorReported(reason);
            });
    connect(m_groupCall.get(), &SfuCallController::stateChanged, this,
            [this] {
                if (m_groupCall->active())
                    m_calls->noteAnsweredByOtherLane(m_groupCall->roomId());
            });
    m_callDevices->setSettings(m_settings.get());
    // ── Voice-call ring policy, wired to its real owners (round 2) ──
    // State truth stays in CallController; these close the policy gates
    // shouldRing() consults. The functors capture `this` and read live
    // state, so account switches need no rewiring.
    m_calls->setSenderIgnoredCheck([this](const QString &userId) {
        return m_moderation && m_moderation->isIgnored(userId);
    });
    m_calls->setRoomMutedCheck([this](const QString &roomId) {
        return m_settings->roomNotificationMode(roomId)
            == static_cast<int>(NotificationManager::Muted);
    });
    // Cold-start backlog: never ring for history. Mirrors the
    // initialSyncComplete gate NotificationManager applies to messages.
    m_calls->setBacklogSuppressed(!m_client->initialSyncDone());
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, this,
            [this] {
                m_calls->setBacklogSuppressed(!m_client->initialSyncDone());
                // MatrixRTC transport discovery is ACCOUNT-scoped and needs a
                // live session, so it runs on the sync edge rather than at
                // construction. Until it answers, the call banner honestly
                // says it is still checking instead of claiming calling is
                // unavailable.
                if (m_client->initialSyncDone())
                    m_rtc->discover(m_currentRoomId);
            });
    // Discovery used to run ONLY on the line above — once, for whatever room
    // was open at the initial-sync edge, which is none. The account-scoped
    // server transports were therefore never retried after a failure, and
    // the per-room participant fallback was never fetched for any room the
    // user actually opened.
    //
    // The room's own session now carries its focus (RtcController::
    // sessionFocusFor), so this is no longer load-bearing for joining. It
    // stays as the retry for the SERVER transports, which is what a room
    // with no call yet needs in order to START one. Bounded by
    // RtcController::discover itself, which refuses while one is in flight
    // and does nothing once the server has answered.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        if (!m_client || !m_client->initialSyncDone())
            return;
        if (!m_rtc->discoveryWorthRetrying())
            return;
        m_rtc->discover(m_currentRoomId);
    });

    // A selection belongs to the room it was started in: leaving that room
    // leaves the mode too (2026-09-05: "when i went into another room the
    // message select was still active" — the circles followed the reader
    // into a room whose messages were not the ones counted).
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
                // Per-sender cooldown: an untrusted room member must not
                // be able to pump critical-urgency ring notifications by
                // minting fresh call ids. State/banner are unaffected —
                // this bounds only the OS-level announcement.
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const qint64 lastRing = m_lastCallRingBySender.value(
                    senderId, 0);
                if (now - lastRing < 30000)
                    return;
                m_lastCallRingBySender.insert(senderId, now);
                while (m_lastCallRingBySender.size() > 64) {
                    // Bounded: drop an arbitrary entry (cooldown is a
                    // heuristic, not bookkeeping worth an LRU).
                    m_lastCallRingBySender.erase(
                        m_lastCallRingBySender.begin());
                }
                const bool privatePreview =
                    m_settings->notificationPreview() == 2;
                const QVariantMap room = m_roomList->findRoom(roomId);
                // NOT escaped here. NotificationManager escapes exactly once,
                // at the D-Bus call, and only when the daemon advertises
                // body-markup — it is the only layer that knows. Escaping
                // here as well produced "Ben &amp; Jerry&#39;s" on GNOME and
                // KDE.
                const QString roomName = room.value(QStringLiteral("name"))
                                             .toString();
                // Localpart only — same restraint as the timeline's
                // unresolved-profile fallback; never the bare full MXID.
                const QString caller = senderId.mid(1)
                                           .section(QLatin1Char(':'), 0, 0);
                const QString body = privatePreview
                    ? tr("Incoming voice call")
                    : (roomName.isEmpty()
                           ? tr("%1 is calling").arg(caller)
                           : tr("%1 is calling in %2")
                                 .arg(caller, roomName));
                // Ring exactly as long as the invite stays valid; the
                // sound repeat is additionally gated on the user's
                // switches.
                const bool sound = m_settings->ringForCalls()
                    && m_settings->notificationSound()
                        != 0 /* SoundOff */;
                m_announcedCallId = callId;
                m_notifications->showIncomingCall(
                    roomId, callId, tr("Incoming call"), body, sound,
                    static_cast<int>(qBound<qint64>(
                        qint64(5), remainingMs / 1000, qint64(300))));
            });
    connect(m_calls.get(), &CallController::incomingCallEnded, this,
            [this](const QString &roomId, const QString &callId,
                   int reason, bool missed) {
                Q_UNUSED(reason);
                m_notifications->stopIncomingCall(callId);
                const bool wasAnnounced = m_announcedCallId == callId;
                if (m_announcedCallId == callId)
                    m_announcedCallId.clear();
                // Missed-call notice: only for a call that (a) the
                // controller classified missed from its pre-end state —
                // an answered call the peer hung up is COMPLETED — and
                // (b) was actually announced to the user; a ring the
                // backlog/mute/ignore gates suppressed must not resurface
                // as "missed" later (review round 2).
                if (!missed || !wasAnnounced)
                    return;
                if (!m_settings->notificationsEnabled())
                    return;
                const bool privatePreview =
                    m_settings->notificationPreview() == 2;
                const QVariantMap room = m_roomList->findRoom(roomId);
                // See above: escaping belongs to NotificationManager alone.
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
    m_pinned->setClient(m_client.get());
    m_roomUpgrade->setClient(m_client.get());
    m_backup->setClient(m_client.get());
    m_scheduledSends->setClient(m_client.get());
    m_widgets->setClient(m_client.get());
    // Theme and language are template variables a widget URL may carry, so a
    // widget can match the client's look. Pushed in from the one place that
    // owns both, exactly like the composer's caption setting.
    if (m_settings && m_localization) {
        const auto pushPresentation = [this] {
            // The Qt enum key IS the stable name a widget would key on
            // ("StormTheme"), and it never needs translating. Lower-cased and
            // stripped of the suffix so a widget sees "storm", which is the
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
    // A widget list belongs to ONE room; re-reading on every room change is
    // what keeps a stale list from being shown under a new room's name.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        m_widgets->setRoomId(m_currentRoomId);
    });
    m_activity->setClient(m_client.get());
    // An Activity row click is exactly a notification click: same window
    // raise, same room, same thread, same exact-event landing.
    connect(m_activity.get(), &ActivityModel::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        Q_EMIT notificationOpenRequested(roomId, eventId, threadRootId);
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
        // The room moved under the answer. Jumping now would drag whatever
        // room is open to an event it does not contain.
        if (expected != roomId || roomId != m_currentRoomId) {
            Q_EMIT jumpToDateFinished(opId, false, QStringLiteral("stale"));
            return;
        }
        if (!ok || eventId.isEmpty()) {
            Q_EMIT jumpToDateFinished(opId, false, category);
            return;
        }
        // The same landing a reply jump uses: it paginates toward a target
        // that is not loaded and holds it by stable id while it waits.
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
        // THE INDEX HAS TO FILL ITSELF. A search feature whose index only
        // grows when the user goes and asks for it is a search feature that
        // returns nothing the first time anybody tries it — and nobody tries
        // twice. The sweep is cheap by construction (one query per room to
        // find what is already indexed, then writes only for what is not), so
        // running it on a timer costs almost nothing on a quiet account.
        if (state == MatrixClient::Syncing) {
            m_client->sweepSearchIndex();
            m_client->searchIndexStats();
            if (!m_searchIndexTimer.isActive())
                m_searchIndexTimer.start();
        } else {
            m_searchIndexTimer.stop();
        }
    });
    // Five minutes: new messages become searchable within one interval, and
    // the interval is long enough that a sweep is never competing with the
    // user's own typing for the runtime.
    m_searchIndexTimer.setInterval(5 * 60 * 1000);
    m_searchIndexTimer.setSingleShot(false);
    connect(&m_searchIndexTimer, &QTimer::timeout, this, [this] {
        if (m_client)
            m_client->sweepSearchIndex();
    });
    // A redaction must reach the index, or a message somebody asked to be
    // unsayable stays findable by its own text — the single worst thing a
    // local index can do.
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
    // The rail's rows: the user's arrangement applied to the hierarchy, with
    // the transient drag preview living in the model rather than in QML.
    m_railEntries->setSources(m_spaces.get(), m_railLayout.get());
    // The Channels layout is GLOBAL — every joined Space is a flat folder, so
    // it needs the account's rooms and the rail's order, and nothing about
    // which Space happens to be selected.
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
    // READING A ROOM CLEARS ITS ROWS FROM THE BELL.
    //
    // The Activity Center keeps its own seen marker deliberately — a row you
    // never looked at should survive a glance at the room list — but the
    // marker was advanced by nothing except the panel's own "mark all seen"
    // button. So reading the very message that produced a row left the bell
    // showing a count for it, which is what a user reads as the badge being
    // broken. Reported from real use on 0.8.4.
    //
    // The read RECEIPT is the right trigger rather than opening a room: it is
    // the moment this client tells the server the user has read up to a
    // specific event, which is precisely the claim being mirrored. It also
    // makes the fix durable for free — the server marks those notifications
    // read, and seed() takes seenMark from that flag on the next start.
    connect(m_readReceipts.get(), &ReadReceiptCoordinator::receiptSent, this,
            [this](const QString &roomId, const QString &, qint64 timestampMs) {
        if (m_activity)
            m_activity->markRoomReadUpTo(roomId, timestampMs);
    });

    // 2026-08-20 (C4): the models need to know whether routine activity is
    // being SHOWN, because a date divider whose entire run is hidden must not
    // render — that is the orphan-date-label defect. QML cannot answer it
    // (a per-row scan of the model on every contentY change is exactly the
    // cost this file has already paid twice), so the setting is pushed into
    // both timeline models and kept live. Both, not just the room's: the
    // thread panel renders the same delegate against its own model.
    const auto applyRoomActivityVisibility = [this]() {
        const bool shown = m_settings->showRoomActivity();
        m_timeline->setShowRoomActivity(shown);
        // The master switch has two sub-toggles since 2026-08-26: membership
        // changes and profile changes are separate annotations and Sable lets
        // them be hidden independently. Both halves have to be PUSHED — the
        // model mirrors default to true, so without this only the QML row
        // filter would honour them and the date dividers would keep counting
        // rows nobody can see.
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

    // Link-preview policy follows the persisted settings live. The
    // encrypted-room setting defaults to OFF (privacy) in SettingsManager.
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

    // v0.5.19: the timeline discrete-wheel speed follows the persisted setting
    // live. Only discrete mouse-wheel distance is affected; touchpad pixel
    // scrolling and all programmatic navigation are independent of it.
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
        // Orderly shutdown: stop media players and the sync loop while the
        // window / event dispatcher are still valid, before the QML engine and
        // windows are destroyed. Fires from inside app.exec().
        connect(guiApp, &QGuiApplication::aboutToQuit, this,
                &AppController::prepareForShutdown);
        // v0.5.11: re-emit when the platform light/dark preference changes so
        // the "System" theme repaints live.
        if (auto *hints = guiApp->styleHints()) {
            connect(hints, &QStyleHints::colorSchemeChanged, this,
                    [this](Qt::ColorScheme) { Q_EMIT systemDarkModeChanged(); });
        }
        // Apply the persisted custom application icon (if any) over the
        // packaged default main.cpp installed before construction.
        applyAppIcon();
    }

    // v0.5.9: a created (or reused) conversation opens once the room is
    // present in the authoritative room list. Leaving the open room closes
    // its timeline and returns to the no-room state; the list entry is
    // removed by the authoritative room-list update, not locally.
    connect(m_conversations.get(), &ConversationController::conversationReady,
            this, &AppController::openRoom);
    // A created Space is SELECTED (rail + Space Home), never opened as a
    // message timeline: an m.space room has no conversation of its own.
    // Clearing the current room is what makes the Space Home surface show.
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
    // v0.7.x sessions: any terminal sign-out outcome re-reads the
    // authoritative device list — a tile disappears only when the server
    // says so, and a failed attempt repaints the truth as well.
    connect(m_uia.get(), &UiaController::signOutFinished, this,
            [this](bool, const QString &) { refreshSessionDevices(); });
    // v0.7.x Discover / Join: a joined room opens once present in the
    // authoritative list; a joined Space is selected in the rail exactly
    // like a created one — never given a message timeline.
    connect(m_discovery.get(), &RoomDiscoveryController::roomJoined,
            this, [this](const QString &roomId) {
        // v0.7.x room upgrades: a join the upgrade banner started and the
        // user then walked away from must not navigate them back. Pressing
        // Continue is consent to switch rooms NOW, not whenever the join
        // happens to settle — the wait is bounded but not instant, and
        // being yanked out of a room you have since opened and started
        // typing in is exactly the silent switch the feature avoids.
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
    // v0.7.x room upgrades. The banner's join reuses Discover's machinery
    // above — so a successful join navigates through the SAME settled
    // roomJoined path, with the same error categories — and this connection
    // covers the case where no join is needed because the user is already a
    // member of the successor, plus the "Previous room" link.
    //
    // navigateRequested is emitted ONLY from a user action on the banner.
    // Nothing observes a tombstone and moves the user by itself.
    m_roomUpgrade->setDiscovery(m_discovery.get());
    // v0.9: the upgrade flow re-parents the replacement into the same
    // Spaces on request, through the Space manager's own child write.
    m_roomUpgrade->setSpaces(m_spaces.get());
    connect(m_roomUpgrade.get(), &RoomUpgradeController::navigateRequested,
            this, &AppController::openRoom);
    // v0.7.x message forwarding: `forwarded` fires
    // only once the send was actually dispatched, so the target room is
    // never opened optimistically ahead of that.
    connect(m_forward.get(), &ForwardController::forwarded,
            this, &AppController::openRoom);
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
    // v0.7: a failed account-switch activation falls back to the previous
    // account once; with no fallback it lands on the login screen. A failed
    // ADD-ACCOUNT login restores the previous account in the background —
    // the shared client's session was released when the attempt started, so
    // without this the footer shows a false "Error/disconnected" state even
    // though the active account is fine.
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
        // v0.7: a failed STARTUP restoration is the genuine
        // unauthenticated state — only now may the login form appear.
        if (m_currentScreen == BootScreen) {
            qCInfo(lcApp) << "startup restore failed — showing login";
            setCurrentScreen(LoginScreen);
            Q_EMIT loggedInChanged();
        }
    });
    // v0.7: cache the signed-in account's own profile (display name and
    // avatar) in its account record for the switcher UI.
    connect(m_client.get(), &MatrixClient::userProfileFinished, this,
            [this](quint64, bool ok, const QString &userId,
                   const QString &displayName, const QString &avatarUrl,
                   const QString &) {
        if (!ok || userId.isEmpty() || userId != m_client->currentUserId())
            return;
        m_accounts->updateProfile(userId, displayName, avatarUrl);
    });
    // v0.7.4: the terminal answer for one own-display-name write.
    connect(m_client.get(), &MatrixClient::ownAvatarChanged, this,
            [this](quint64 opId, bool ok, const QString &error) {
        // Same op-id guard as the display name: the payload carries no
        // path, so the id is the only thing distinguishing this answer
        // from a previous account's.
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
        // Re-fetch rather than writing the new mxc locally: sync does not
        // carry the account's own profile, and the SERVER is the authority
        // on what it stored.
        const QString uid = m_client ? m_client->currentUserId() : QString{};
        if (!uid.isEmpty())
            m_client->fetchUserProfile(uid);
        Q_EMIT ownAvatarSaved();
    });

    connect(m_client.get(), &MatrixClient::ownDisplayNameChanged, this,
            [this](quint64 opId, bool ok, const QString &error) {
        // Drop anything that is not the write this controller is waiting
        // for: a previous account's answer, or an attempt already retired
        // by a sign-out. Matching by op id is the whole guard — nothing
        // else distinguishes them, because the payload carries no name.
        if (opId == 0 || opId != m_displayNameOp)
            return;
        m_displayNameOp = 0;
        if (!ok) {
            // An empty `error` means the server said nothing usable (or
            // there was no server answer at all — a timeout, a transport
            // failure, a synchronous refusal). Supply our own wording
            // rather than showing an empty red line, and never invent a
            // server message.
            m_displayNameError = error.isEmpty()
                ? tr("The display name could not be saved. Please try again.")
                : error;
            Q_EMIT ownDisplayNameStateChanged();
            return;
        }
        m_displayNameError.clear();
        Q_EMIT ownDisplayNameStateChanged();
        // NOTHING else refreshes the cached name — sync does not carry the
        // account's own profile, and the one fetch in the tree runs once
        // per login. Re-issue it, and take the answer from the SERVER
        // rather than writing the submitted string into the registry: the
        // server is free to normalise or bound what it stored, and a local
        // write would cache a value it never held.
        const QString uid = m_client ? m_client->currentUserId() : QString{};
        if (!uid.isEmpty())
            m_client->fetchUserProfile(uid);
        Q_EMIT ownDisplayNameSaved();
    });
    // A plain sign-out does not go through clearCrossAccountCaches(), so
    // retire the write here too — detachSession() (the account switch)
    // emits this signal as well, which makes the reset idempotent rather
    // than duplicated.
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            &AppController::retireOwnDisplayNameWrite);
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            &AppController::retireOwnAvatarWrite);
    // Hidden-image state is per ACCOUNT: what one account's reader hid says
    // nothing about the next account's rooms, and detachSession() (the
    // account switch) emits this too, which makes the reset idempotent
    // rather than duplicated.
    //
    // resetForSession(), NOT clear(): the list is persisted now, and clear()
    // writes. Using it here would erase the account's saved hidden images on
    // its own sign-out — the opposite of what persisting them is for.
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
            // v0.4.6: distinguish the initial-sync wait from steady-state
            // long-poll so a user with no rooms yet loaded doesn't stare
            // at "Syncing" and assume the app is frozen.
            //
            // v0.4.8: after initial sync completes, the long-poll is the
            // normal healthy state; label it "Connected" instead of
            // "Syncing" so users don't think Lightning is still catching
            // up when it is just waiting for new events. Real ongoing
            // work (initial catch-up, /messages backfill) has its own
            // labels ("Loading rooms…", "Refreshing…").
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
    // v0.7: a rule write that failed while offline is retried on the EDGE
    // into Syncing, not on every status change — the signal can re-announce
    // the same state, and retrying each time would hammer the server for a
    // room that keeps failing. One attempt per genuine reconnection.
    //
    // Reads the state from the SIGNAL rather than re-querying the client:
    // the argument is the authoritative "what just changed to", and it
    // keeps the edge observable without depending on when the client's
    // internal getter settles.
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

    // v0.5.0-prep+10: recovery-key restore wiring. The Rust backend
    // exposes keyBackupResult(state, message); we bridge it into
    // AppController::recoveryStateChanged so QML can bind without
    // caring which concrete backend is active. Also proxy the redacted
    // device id so the Settings screen can show which Lightning
    // session is running.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
        connect(rust, &RustSdkMatrixClient::keyBackupResult,
                this, [this](const QString &state, const QString &message) {
            Q_EMIT recoveryStateChanged(state, message);
            // v0.5.0-prep+11: on successful key backup recovery, ask
            // the SDK to reload the current room so previously
            // undecryptable events get another chance.
            if (state == QLatin1String("ok") && !m_currentRoomId.isEmpty()) {
                reloadCurrentRoomTimeline(30);
            }
        });
        // The device id becomes available once login/restore
        // completes; propagate on both. Also snapshot the SDK trust
        // state so Settings can show the right label immediately.
        connect(rust, &MatrixClient::loginSucceeded,
                this, [this, rust](const QString &) {
            Q_EMIT rustDeviceIdChanged();
            rust->refreshOwnDeviceStatus();
            // Start a fresh crypto epoch for the new session, then capture it
            // at dispatch so a logout before the answer arrives rejects it.
            m_cryptoHealth->resetForNewGeneration();
            m_cryptoQueryGeneration = m_cryptoHealth->generation();
            rust->queryCryptoHealth();
        });
        // AND AGAIN ONCE SYNC HAS ACTUALLY RUN. `loginSucceeded` fires inside
        // the login_ok handler, BEFORE sync starts and therefore before the
        // first /keys/query — so on a first sign-in the crypto store has no
        // own identity yet and `own_identity_available` comes back false.
        // AppController latches that as "Cross-signing unavailable", which
        // `sessionVerificationNeeded` deliberately does not prompt for, and
        // nothing re-read it for the rest of the session: the only other
        // callers are the verificationDone handler and a manual Refresh link.
        //
        // The result was that signing in to an account that DOES have
        // cross-signing told the user there was nothing to verify against and
        // never asked again — on exactly the session where verification
        // matters most, since other devices withhold room keys from an
        // unverified one.
        connect(rust, &MatrixClient::initialSyncDoneChanged, this, [this, rust] {
            if (!rust->initialSyncDone())
                return;
            rust->refreshOwnDeviceStatus();
            m_cryptoQueryGeneration = m_cryptoHealth->generation();
            rust->queryCryptoHealth();
        });
        // v0.6.0 checkpoint 7: sanitized health snapshots feed the read-only
        // model; the generation stamp drops answers from a previous session,
        // and any verification-state transition updates the pending counter.
        m_cryptoHealth->setSupported(true);
        connect(rust, &RustSdkMatrixClient::cryptoHealthUpdated,
                this, [this](const QVariantMap &snapshot) {
            // Compare against the generation captured at query DISPATCH, not
            // the model's live generation, so a session change in flight
            // rejects the stale answer.
            m_cryptoHealth->applySnapshot(snapshot, m_cryptoQueryGeneration);
        });
        // v0.7: verified-session bootstrap status. The bridge observer only
        // reports the ACTIVE session handle; the model additionally resets
        // on login/logout (which account switching passes through), so a
        // previous account's bootstrap can never describe the current one.
        connect(rust, &RustSdkMatrixClient::cryptoBootstrapEvent,
                this, [this](const QString &kind, const QString &state,
                             quint64 count) {
            m_cryptoBootstrap->applyEvent(kind, state, count);
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
        // v0.6.0 checkpoint 9: device/session list — current session first,
        // then most recently seen. Stale answers after logout are cleared by
        // the reset above (the handle generation already drops post-destroy
        // events).
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
            // v0.6.0 checkpoint 11: surface INCOMING verification requests
            // natively (identity only; a verification prompt never carries
            // message content or SAS data).
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
            // Clear the caches so the Login screen never inherits a
            // stale verification / import result from a signed-out
            // session.
            m_verificationFlowId.clear();
            m_verificationOtherUser.clear();
            m_verificationOtherDevice.clear();
            m_verificationIsSelf = false;
            m_verificationState.clear();
            m_verificationEmojis.clear();
            m_verificationDecimals.clear();
            // A displayed code must never survive a sign-out or an account
            // switch: it belongs to the session that is going away.
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
            m_sessionTrustState = QStringLiteral("Unknown");
            m_sessionDeviceId.clear();
            m_ownIdentityAvailable = false;
            m_crossSigningAvailable = false;
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
        // v0.5.0-prep+11: bubble timeline reload results.
        connect(rust, &RustSdkMatrixClient::roomTimelineReloaded,
                this, [this](const QString &roomId, int t, int d, int u) {
            if (roomId == m_currentRoomId)
                Q_EMIT currentRoomTimelineReloaded(t, d, u);
        });
        // v0.5.0 SAS verification signal bridge. QML binds
        // verificationStateChanged() and reads the seven derived
        // properties. All state lives in AppController so QML doesn't
        // reach into concrete backend types.
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
        // Both sides are Ready and the SAS handshake is in flight. This is
        // strictly a progress report: it may only advance the pre-emoji
        // states, so a late or duplicated ready can never pull a flow back
        // out of sas_ready/confirming/done/cancelled/failed.
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
        // v0.7.1: the SDK registered OUR "They match" (SasState::Confirmed).
        // Done still needs the peer's confirmation — surface the honest
        // intermediate state. Only the local "confirming" state advances so
        // a stray/late Confirmed poll can never resurrect a flow that
        // already finished, failed, or was cancelled.
        connect(rust, &RustSdkMatrixClient::verificationSasConfirmed,
                this, [this](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState != QLatin1String("confirming")) return;
            m_verificationState = QStringLiteral("waiting_for_peer");
            Q_EMIT verificationStateChanged();
        });
        // Show-QR leg. Deliberately orthogonal to m_verificationState: the
        // QR is an alternative presentation of the SAME flow, so none of
        // these handlers moves the state machine. Every one of them is
        // flow-scoped, and a code is never shown for a flow that already
        // finished — a late grid must not repaint a completed verification
        // as something still awaiting a scan.
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
                // Geometry the renderer cannot honour. Show no QR rather
                // than an unscannable picture; the flow continues on SAS.
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
            // The SDK registered our confirmation. This is progress, NOT
            // success: only verificationDone may report that.
            m_verificationQrConfirming = true;
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationQrDismissed,
                this, [this](const QString &flowId, const QString &) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationQrToken.isEmpty()) return;
            // The peer chose emoji, or the display window elapsed. Drop the
            // panel and let the card fall back to the SAS presentation.
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationDone,
                this, [this, rust](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("done");
            clearVerificationQr();
            Q_EMIT verificationStateChanged();
            // v0.5.6: after a successful flow, re-query the SDK trust
            // state so the Settings pane doesn't fall back to the local
            // "confirmed" guess. Do NOT set the trust state from here —
            // only the SDK snapshot may promote to "Verified".
            rust->refreshOwnDeviceStatus();
            // v0.5.1: post-verification retry. matrix-sdk 0.18 does not
            // expose an explicit per-event "request room key" API on
            // Client — internal event_cache/redecryptor.rs re-runs
            // automatically as keys arrive. Best surface action is a
            // Room::messages reload; if verified peers have shared
            // keys since we first saw the events, decryption succeeds
            // this time. Idempotent by event_id.
            if (!m_currentRoomId.isEmpty()) {
                qCInfo(lcApp) << "verification=done; reloading current room"
                              << m_currentRoomId.right(12);
                reloadCurrentRoomTimeline(50);
            }
            // v0.7: the security pane's backup/recovery snapshot must
            // reflect the just-verified state without a manual refresh —
            // the recovery supervisor's download pass reports through the
            // bootstrap events, and this keeps the health card coherent
            // with it.
            refreshCryptoHealth();
        });
        // A completed verification is FINAL for its flow. A late cancellation
        // or failure carrying the same flow id must not repaint a successful
        // verification as cancelled — the user confirmed matching emoji and
        // the SDK reported Done, and telling them otherwise afterwards is
        // simply false. Ordering happens to prevent this today (the Rust
        // driver returns on SasState::Done, and cancelVerification() clears
        // the flow id first), but relying on two remote invariants for a
        // user-visible correctness property is how it silently breaks.
        //
        // Only `done` is sticky. Trust itself is never taken from this
        // string — it comes from SDK state via refreshOwnDeviceStatus().
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

        // v0.5.6 outbound-initiated verification: reuses the same UI
        // state cache. `verification_request_started` mirrors the
        // receive-first `verification_request_received` handler above
        // but flips the direction flag.
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

        // v0.5.6 Security & Recovery: aggregate cross-signing snapshot
        // and room-key import lifecycle. Only the SDK-provided
        // "device_cross_signed" flag promotes the label to Verified.
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
                // v0.7.x: a dismissal answered "I know this session is
                // unverified". Once it IS verified that answer is spent —
                // clearing it here means a future unverified session warns
                // again instead of inheriting silence from an old dismissal.
                if (m_settings)
                    m_settings->setVerificationWarningDismissed(false);
            } else {
                m_sessionTrustState = QStringLiteral("Not verified");
            }
            Q_EMIT securityStateChanged();
            Q_EMIT rustDeviceIdChanged();
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
            // v0.5.7: no timeline reload needed here. Rust keeps the
            // imported Megolm session IDs and immediately calls the SDK
            // timeline's retry-decryption; already-visible undecryptable
            // rows update in place via Set diffs. roomKeysApplied()
            // below reports the retry back to the UI.
            // Room-key import never affects verification/trust; do NOT
            // touch m_sessionTrustState here.
            Q_UNUSED(rust);
        });
        // v0.5.7: post-import decryption retry completed on the open
        // timeline. Surface a safe, honest status line — it claims the
        // keys were applied, not that every event decrypted.
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
            // Categorized safe user message; never the raw passphrase.
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
            // Record WHICH account failed and WHY, then let QML choose the
            // copy and the valid actions. Five distinct causes used to share
            // one sentence, and the repair could not run at all because it
            // was driven from a login form the user had not filled in.
            setLocalSessionFailure(reasonCode, userId, homeserver);
            setLocalRustResetRequired(true);
        });
        // Same payload, but for conditions a local reset cannot repair: a
        // missing store, a revoked token, contestable ownership. The repair
        // card still explains what happened and offers the actions that are
        // valid for that reason — it just never arms the destructive one,
        // because deleting local data would not fix any of them and, for a
        // revoked token, would throw away the only local copy of room keys.
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

    // v0.7: startup with a saved account is an explicit restoration state,
    // never an unauthenticated one — the login form must not render (or
    // even instantiate) while the outcome is still unknown. Restore
    // success lands on MainScreen via loginSucceeded; every restore
    // failure path funnels through loginFailed, which routes BootScreen to
    // the genuine login form. The mock backend restores from its account
    // registry (it has no real tokens), so the startup lifecycle is the
    // same on every backend and testable end to end.
    // Screenshot-demo mode drives its own deterministic restore from
    // beginScreenshotDemo (which first registers the fictional accounts and
    // enables the rich scene); the normal startup restore must not fire first
    // and race it — especially on a persisted isolated demo profile whose
    // account records already exist from a previous launch.
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
    // Deliberately NOT silent. A button the user pressed that does nothing
    // is worse than one that explains itself, and this is the one case where
    // the explanation genuinely matters: they believe they have replied.
    //
    // The notice names no room and no message — it is raised through the
    // generic path, whose body must already be safe — because the whole
    // point is that we are no longer signed in as the account that could
    // legitimately see either.
    qCWarning(lcApp) << "notification action refused: account changed";
    // Gated like every other generic notice: a user who has turned desktop
    // notifications off in the meantime should not get one back, even to
    // explain a refusal. The refusal still happens — this is only whether we
    // say so on the desktop.
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
    // Guarded: the symbol exists only in a Rust-enabled build, and an
    // unguarded call is exactly what broke the non-Rust tree earlier in this
    // round (§16's standing lesson).
    RustSdkMatrixClient::setStrictDeviceTrust(m_settings->strictDeviceTrust());
#endif
}

void AppController::applyPrivacyPreferences()
{
    // Two settings, one place. Both are about what leaves this device while
    // the user is only reading and typing, and both have to be applied on
    // change AND on client attachment — a fresh bridge starts permissive.
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
    // worker threads deliver state through queued signals, a prime candidate
    // for the "Invalid window handle" wake-up if they run into teardown. This
    // also happens before the QML engine destroys the per-card players.
    if (m_playback)
        m_playback->stopAll();

    // LEAVE THE CALL BEFORE ANYTHING THAT CARRIES THE LEAVE IS TORN DOWN.
    //
    // This lane was missing entirely, and the cost was visible to everyone
    // else in the room: closing Lightning left our `m.call.member` state
    // event published and our SFU participant connected, so we sat in the
    // call as a ghost — and every later join added another copy, each one
    // labelled waiting for media because a stale publisher has no tracks.
    //
    // It must run BEFORE `stopSync()` below: the retraction is a state event
    // and a Leave is an SFU command, and both need the client that stopSync()
    // is about to quiesce. Relying on ~SfuCallController instead is not good
    // enough — member destruction order would decide whether the send still
    // had a client to reach, which is exactly the kind of dependency that
    // silently inverts when a member is added.
    //
    // `leave()` is documented safe in any state including mid-join, so this
    // is unconditional rather than gated on `active()`: a join still in
    // flight is precisely the case that would otherwise strand a membership
    // published moments earlier. The bounded Rust task join in the client's
    // destructor is what gives the dispatched sends their window to land.
    if (m_groupCall)
        m_groupCall->leave();

    // Stop the sync loop / poll timer so no further backend callback is
    // scheduled onto the main loop during teardown. The client's own
    // destructor still performs the bounded Rust task join.
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
    // Defence-in-depth: the composite thread-timeline id must never reach
    // settings keys or a protocol call (the pickers only ever pass real
    // room ids; this guards against any future caller slipping one in).
    if (roomId.isEmpty() || mode < 0 || mode > 3
        || MatrixClient::isThreadTimelineId(roomId))
        return;
    // Device-local value first: NotificationManager reads it (see the
    // eventAppended context wiring), so the choice takes effect instantly
    // and keeps working offline. On server-capable backends it doubles as
    // the cache of the account's push-rule mode.
    m_settings->setRoomNotificationMode(roomId, mode);
    if (m_client && m_client->supportsServerNotificationModes()) {
        // Mode 3 is a rule REMOVAL, not a rule with value 3 — Matrix has no
        // follow-default rule, only the absence of a room override.
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
    // Re-issue the user's PERSISTED choice for every room whose write did
    // not reach the server. The local value is authoritative here precisely
    // because the write failed — nothing on the server has since contradicted
    // it (a differing user-defined report is rejected while a room is in this
    // set; see the roomNotificationModeChanged handler).
    //
    // The failure entries are deliberately NOT cleared here. A room leaves
    // the set only when the server ACKNOWLEDGES the value, which happens in
    // that same handler. Clearing on attempt would report success for a
    // retry that is still in flight — or that fails again — which is exactly
    // the "pretend the rule changed" outcome this whole path exists to
    // avoid. A still-failing room simply stays disclosed as
    // kept-on-this-device and is retried on the next reconnect.
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

bool AppController::roomNotificationModeSyncFailed(const QString &roomId) const
{
    return m_notificationModeSyncFailures.contains(roomId);
}

bool AppController::startVoiceRecording(const QString &owner)
{
    // Only the two known composers may own the recorder. An unrecognised
    // owner is refused rather than stored: ownership is a send authorisation,
    // so an unknown value must never end up holding it.
    if (owner != QLatin1String("room") && owner != QLatin1String("thread"))
        return false;
    // Ownership is taken ONLY after a successful start, and never stolen
    // from a live recording.
    //
    // VoiceRecorder::start() REFUSES while Recording or Processing and
    // returns false WITHOUT emitting failed() (see VoiceRecorder::start).
    // An earlier version of this function moved ownership first and cleared
    // it when start() failed, which disowned the still-running recorder:
    // the microphone stayed open with no pill, no cancel button and no
    // owner to deliver ready() to — up to the recorder's 15-minute cap, and
    // across sign-out. Refusing the transfer is what keeps the live
    // recording reachable by the composer that actually owns it.
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
    // True when a recording is in progress ANYWHERE — used by a composer to
    // tell "no microphone available" apart from "the other composer is
    // already recording", which are very different messages to show.
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
    // Never construct the recorder just to cancel: with no owner there is
    // nothing recording, and touching the getter would spin up the audio
    // backend for a session that never recorded.
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
// Map a launcher/CLI account hint ("personal"/"work"/"community", a display
// alias, or a full user id) onto one of the three fictional demo user ids.
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
    // Only ever runs on the in-memory mock backend (preflight forces it). Fail
    // closed on any other backend so this can never touch a real session.
    if (m_backend != MockBackend) {
        qCWarning(lcApp)
            << "beginScreenshotDemo ignored: active backend is not the mock";
        return;
    }
    // Hard safety assertion: the demo must NOT have initialized a production
    // secure secret store. The constructor injects an in-memory store for the
    // demo (isSecure() == false); if a libsecret/keychain store ever reached
    // here, fail closed rather than risk touching the real keychain.
    if (m_secretStore && m_secretStore->isSecure())
        qFatal("screenshot-demo: refusing to run on a production secure "
               "SecretStore (libsecret/keychain must not be initialized)");
    m_screenshotDemoActive = true;

    // Enable the rich, deterministic three-account scene on the mock. Tests
    // never call this, so the shared mock fixtures they assert on are unchanged.
    MockMatrixClient *mock = qobject_cast<MockMatrixClient *>(m_client.get());
    if (mock) {
        mock->setScreenshotDemoMode(true);
        // The demo scenario / control-panel controller (app.demo), owned here.
        if (!m_demoController)
            m_demoController = new ScreenshotDemoController(this, mock, this);
    }

    // Register the three fictional accounts as NON-SECRET metadata only — no
    // token, no SecretStore write — under the isolated demo QSettings profile.
    // clearDemoAccounts first so a persisted profile re-registers deterministically.
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

    // Select the requested initial account (default Alex).
    QString activeUid = resolveDemoAccountId(initialAccount);
    if (!m_settings->hasSavedAccount(activeUid))
        activeUid = QStringLiteral("@alex:lightning.example");
    m_settings->setActiveAccountUserId(activeUid);

    // Restoration state, not an unauthenticated one: show the boot surface (not
    // the login form) while the mock "restores", then land on MainScreen via
    // the normal loginSucceeded path. No network; no real credentials. The mock
    // restore reads the active account from settings and activates its scene.
    setCurrentScreen(BootScreen);
    if (!m_client->restoreSession()) {
        // Extremely unlikely (settings has an active account) — fall back to a
        // direct mock login into the active account so the demo still boots.
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
    // MatrixRTC first. It is what current Element speaks, it carries video
    // and screen share, and it works in a group — the legacy lane does none
    // of those.
    if (m_rtc && m_rtc->joinBlock(roomId) == RtcController::JoinBlock::None)
        return QStringLiteral("matrixrtc");
    // Legacy fallback, and only where it is protocol-safe: a legacy
    // m.call.invite rings EVERY member of a room, so it is 1:1 DMs only.
    if (m_calls && m_calls->mediaBackendAvailable()
        && !legacyCallPeer(roomId).isEmpty()) {
        return QStringLiteral("legacy");
    }
    return QString();
}

QString AppController::legacyCallPeer(const QString &roomId) const
{
    // `isDirect` is not "1:1": it is "m.direct lists at least one target",
    // and a DM a third person was invited into is still direct. The legacy
    // lane must name exactly one peer -- the invite carries them as
    // `invitee` so only they ring, and every later signal on the session is
    // bound to them -- so the room must map to exactly one direct target.
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
    const QString lane = preferredCallLane(roomId);
    // The call path had NO logging at all, which is why "pressing call does
    // nothing / the app goes away" could not be diagnosed from a user's
    // console at all. Room ids are structural, not content.
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
            // The legacy lane is audio-only by design. Saying so beats
            // starting an audio call the user asked to be a video one.
            Q_EMIT callStartRefused(
                tr("Video calls need a MatrixRTC service, which isn't "
                   "available here yet."));
            return false;
        }
        const bool ok = m_calls->placeCall(roomId, legacyCallPeer(roomId));
        qCInfo(lcApp) << "legacy call dispatched ok=" << ok;
        return ok;
    }

    // Neither lane. The reason matters: "this homeserver has no calling" and
    // "this room is encrypted and encrypted calls are not ready" are
    // different problems with different answers.
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

void AppController::enableCallMediaEngine()
{
    // Called from main.cpp for the REAL application run only — never from
    // the AppController constructor, so the offscreen test fleet is not
    // at the mercy of ambient GStreamer plugin availability (review round
    // 3), and gst_init never runs under a test that didn't ask for it.
#ifdef HAVE_LIGHTNING_WEBRTC
    if (qEnvironmentVariableIsSet("LIGHTNING_DISABLE_WEBRTC")) {
        qCInfo(lcApp) << "voice-call media engine disabled by environment";
        return;
    }
    QString whyNot;
    if (GstCallMediaBackend::runtimeAvailable(&whyNot)) {
        auto *engine = new GstCallMediaBackend(this);
        m_calls->setMediaBackend(engine);
        // Apply the user's chosen capture/playback devices, and follow a
        // hotplug that actually moves the ACTIVE device. Applied per session
        // by the engine, so a change lands on the next call rather than
        // relinking a live pipeline.
        const auto applyDevices = [this, engine] {
            engine->setAudioDevices(m_callDevices->microphoneElement(),
                                    m_callDevices->speakerElement());
        };
        applyDevices();
        connect(m_callDevices.get(),
                &CallDeviceController::activeDevicesChanged, engine,
                applyDevices);
        qCInfo(lcApp) << "voice-call media engine active (webrtcbin)";
    }
    // The SFU engine probes a WIDER element set (video and screen capture on
    // top of audio), so it can legitimately be unavailable where the 1:1
    // engine is fine. Probed separately for exactly that reason.
    QString sfuWhyNot;
    if (SfuMediaEngine::runtimeAvailable(&sfuWhyNot)) {
        auto *sfu = new SfuMediaEngine(this);
        m_groupCall->setMediaEngine(sfu);
        // SDP transport is opt-in at the Rust edge, and the SFU lane shares
        // ONE flag with the legacy 1:1 lane — so until now the group call's
        // ability to carry media depended on whether the OTHER lane's engine
        // happened to register. It always did, because the SFU engine probes
        // a strict superset of its elements, which is precisely why this
        // never showed: the coupling is invisible while it holds and silent
        // when it breaks. If it ever broke, every offer, answer and ICE
        // candidate from the SFU would be discarded in Rust while
        // participants, speakers and mute kept updating — a call that looks
        // connected and can never carry a packet.
        //
        // Asserted here so this lane states its own requirement.
        if (m_client)
            m_client->setCallMediaCapable(true);
        // The join gate can now say "joinable": until an engine exists it
        // reports NoMediaTransport, because publishing a membership nobody
        // can connect to is worse than refusing.
        m_rtc->setMediaAvailable(true);
        // Frame encryption exists in this engine (CallFrameCryptor on pad
        // probes between the encoder and the RTP payloader, which is where
        // LiveKit and Element Call encrypt), so an ENCRYPTED room is no
        // longer refused. This flag says the capability is present; whether
        // a given call is actually encrypting is SfuCallController's
        // `mediaEncrypted`, which reads the engine rather than this.
        m_rtc->setMediaEncryptionAvailable(true);
        // Screen sharing goes through the desktop portal, so the picker is
        // the compositor's and Lightning never enumerates windows itself.
        // Registered only when a portal actually answers on this session
        // bus; without one the control refuses honestly.
        if (ScreenCastPortal::available()) {
            m_groupCall->setScreenCastPortal(new ScreenCastPortal(this));
            qCInfo(lcApp) << "screen-share portal available";
        } else {
            qCInfo(lcApp) << "screen-share portal unavailable";
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
    // Exactly one actionable state. "Unknown" is not yet determined, and
    // "Cross-signing unavailable" means there is no identity to verify
    // against — prompting there would be advice the user cannot follow.
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
    // Emit "attempted" first so the button flips to "Running…" the
    // moment the user clicks. matrix-sdk will subsequently emit its
    // own attempted event over the FFI; the QML side is idempotent.
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
    // A thread panel never survives into another room; close it BEFORE the
    // room timeline switches so no stale thread work targets the new room.
    m_thread->handleCurrentRoomChanged(roomId);
    // v0.7: inline media playback never survives a room switch either.
    m_playback->stopAll();
    // Queued speculative fetches (full-GIF autoplay prefetch) belong to the
    // delegates that just got destroyed with the room switch; dropping them
    // stops a heavy GIF room from starving the next room's media. In-flight
    // work is untouched and a revisit re-requests naturally.
    m_mediaBridge->dropQueuedSpeculative();
    m_timeline->setRoomId(roomId);
    m_composer->setRoomId(roomId);
    m_pagination->setRoomId(roomId);
    // v0.7.x pinned messages follow the ACTIVE room: the message-action menu
    // needs the answer for the room the user is reading, and the previous
    // room's list must not survive the switch.
    m_pinned->setRoomId(roomId);
    // MSC2545 packs: the ROOM's own packs belong in the snapshot, so the
    // sticker picker has to know which room it is opening over. This MARKS
    // the snapshot stale and issues NO request — a refresh costs a
    // /state read per room pack, and CLAUDE.md §16's room-list lesson is
    // that a surface which refreshes itself on navigation issues one
    // request per room. The picker asks when it opens.
    m_stickers->setActiveRoomId(roomId);
    // v0.7.x room upgrades follow the ACTIVE room too: the banner sits above
    // the open timeline, and a failed Continue from the previous room must
    // not follow the user into this one.
    m_roomUpgrade->setRoomId(roomId);
    // 2026-08-23 MatrixRTC: read the newly opened room's call session so the
    // banner is right on arrival rather than only after the next membership
    // change. A read is cheap (state store, no request) and coalesced.
    if (!roomId.isEmpty()) {
        m_rtc->refresh(roomId);
        // Feed the room's REAL encryption state to the join gate. The
        // tri-state matters: `encryptionKnown` false means we do not yet
        // know, and an unknown room must fail CLOSED — treating it as
        // unencrypted would be exactly the silent downgrade §6 forbids.
        const QVariantMap room = m_roomList->findRoom(roomId);
        const bool known =
            room.value(QStringLiteral("encryptionKnown")).toBool();
        const bool encrypted =
            room.value(QStringLiteral("encrypted")).toBool();
        m_rtc->setRoomEncrypted(roomId, !known || encrypted);
    }
    // v0.7.x: drop QUEUED thread-participant fetches for the room we just
    // left. Those summary cards are gone; letting their fetches run would
    // make the new room's facepiles wait behind answers nothing will read.
    m_threads->setActiveRoom(roomId);
    // The Unreads list filter keeps the open room visible (reading it
    // must not remove the row the selection sits on).
    m_roomList->setPinnedRoomId(roomId);
    // Mention suggestions arm on the room the user last typed "@" in and
    // never disarmed on switch, leaving that room's roster refetching on
    // every membership event indefinitely (review M2).
    m_mentionSuggestions->setRoomId(QString());
    // v0.6.5: hydrate the member roster on first open — mention chips,
    // reply headers and thread summaries resolve display names through the
    // roster-fed cache behind displayNameFor(), and before this the fetch
    // only ever fired from the member panel or an @-composition, so plain
    // reading kept bare localparts (live-feedback screenshot: the sender
    // showed "Grok AI" while the mention chip showed "@brotato"). Once per
    // room per session; the response merges into the cache and emits
    // membersChanged, which refreshes every consumer. A FAILED fetch
    // un-marks the room (see the roomMembersReceived connection) so the
    // next open retries instead of failing closed for the whole session.
    if (!roomId.isEmpty() && m_client
        && !m_memberHydratedRooms.contains(roomId)) {
        // Record the room only when the dispatch actually went out
        // (MentionSuggestionModel precedent): a synchronous rejection —
        // no SDK handle yet, room not (yet) joined — returns 0 WITHOUT
        // ever emitting roomMembersReceived, and marking it here would
        // fail closed for the whole session.
        if (m_client->requestRoomMembers(roomId) != 0)
            m_memberHydratedRooms.insert(roomId);
    }
    Q_EMIT currentRoomIdChanged();
}

void AppController::showLogin()
{
    // Entering the login screen while a session is active is the
    // add-account flow; remember where to return so a failed attempt or
    // Back never strands the user on a dead client.
    if (m_client->isLoggedIn())
        m_addAccountReturnTo = m_settings->activeAccountUserId();
    m_composer->setRoomId({});
    setCurrentScreen(LoginScreen);
}
void AppController::showMain()
{
    // Self-heal on return from add-account: a failed attempt released the
    // shared client's session, so restore the active account and clear the
    // spurious error before showing the shell again.
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
    // The full-view Settings screen owns the whole content area: every
    // transient room-side surface (thread, thread list — and the QML-side
    // info/member panel, which reacts to the screen change) closes now and
    // is NOT restored when Settings exits.
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
        // Enabled but the normalized copy is unreadable (deleted app data,
        // disk corruption): fall back visually without silently rewriting
        // the user's stored preference.
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
    // Bounded read even when size() lies (a FIFO reports 0): one byte past
    // the cap proves the overrun without an unbounded readAll().
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
    // The GRAB BAND along the top of the frame — the part the user has to be
    // able to reach with a pointer. Requiring the whole rect to sit on one
    // screen would refuse a window legitimately spanned across two monitors.
    const QRect grabBand(geometry.x(), geometry.y(), geometry.width(), 32);
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (screen && screen->availableGeometry().intersects(grabBand))
            return true;
    }
    return false;
}

// WHY THIS IS IN C++ AND WHY IT VALIDATES ITSELF.
//
// The QML this replaces centred against `Screen.desktopAvailableWidth`, which
// is the width of the WHOLE VIRTUAL DESKTOP and not of the screen the window
// is opening on. With two monitors that puts a fresh window at the middle of
// the PAIR — the seam between them — and on this maintainer's layout it put
// it past the right-hand edge entirely: measured
//
//   window placement "centred" applied=[6490,360 1100x720]
//   screens="DP-3[3840,0 2560x1440] DP-1[0,0 2560x1440]"
//
// with the desktop ending at 6400. The window opened where it could not be
// seen, and the only reason that was ever survivable is that most launches
// restore a stored geometry instead.
//
// It compounds with a Qt quirk this repo has already recorded once (see the
// screen-capture note in CLAUDE.md §16): under fractional scaling
// `QScreen` reports an origin in NATIVE pixels and a size in LOGICAL ones —
// DP-3 above is at 3840 = 2560 * 1.5 but 2560 wide. Any arithmetic mixing the
// two is wrong, and a phantom gap appears between the monitors, which is also
// why a perfectly good stored x=2560 was judged off-screen.
//
// So: centre inside ONE screen's availableGeometry — origin and size from the
// SAME rect, which is the only combination that cannot mix spaces — and then
// CHECK the answer against the real screens. When the check fails the caller
// gets an empty rect and leaves the placement to the window manager, which is
// better at this than we are and cannot put the window where it is invisible.
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

void AppController::openRoom(const QString &roomId)
{
    // v0.5.8: skip reopening the room that is already open. Clicking the
    // active room in the list previously stopped and restarted its SDK
    // subscription, forcing an avoidable full timeline reset (a source of
    // spurious DelegateModel churn on rapid clicks). An explicit refresh
    // still goes through reloadCurrentRoomTimeline().
    const bool alreadyOpen = (m_currentRoomId == roomId);
    setCurrentRoomId(roomId);
    // Opening a room from anywhere (room list, quick switcher, links) while
    // the in-shell Settings view is showing returns to the chat view — the
    // user asked for a room, not for Settings over it.
    if (m_currentScreen == SettingsScreen)
        setCurrentScreen(MainScreen);
    // v0.5.7: the Rust backend opens a persistent matrix-sdk-ui timeline
    // for the room. Rust cancels the previous room's subscription, sends
    // one snapshot, and then streams incremental diffs — including
    // in-place decryption updates after key import.
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
    // Teardown FIRST, activation LAST (2026-08-19): the Space Home
    // loader instantiates SYNCHRONOUSLY the moment "no open room" and
    // "real active space" both hold, and its own handlers point
    // RoomInfoController at the space — the old order cleared roomInfo
    // AFTER that, wiping the canInvite/canManageSpaceChildren gates the
    // Home's controls read, so they rendered permission-less.
    if (!m_currentRoomId.isEmpty()) {
        // Mirror the roomLeft path: the Rust backend's SDK timeline for
        // the open room is closed before the room selection clears, so
        // no live subscription outlives the visible timeline.
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
    // The Space's own membership, which SpaceManager already resolves
    // transitively — muting a Space the user thinks of as one thing has to
    // cover the rooms a subspace brought into it, or the mute is a half-mute.
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
    // Captured, so an answer that arrives after the user has moved on cannot
    // yank a DIFFERENT room's timeline to a date they asked about in this
    // one. The op id alone would not catch it: it is unique per request, not
    // per room.
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
    // An UNKNOWN encryption state fails CLOSED, exactly as the draft store
    // does: treated as encrypted, so its text is withheld unless the user
    // explicitly asked for it. Guessing "not encrypted" from a state the
    // client has not learned yet would write plaintext on a hunch.
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
    // NewOnly is deliberately NOT used: the save dialog already asked about
    // overwriting, and refusing here would contradict the answer the user
    // just gave. Truncate is what "save as this file" means.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return tr("Could not write that file.");
    const QByteArray bytes = text.toUtf8();
    const qint64 written = file.write(bytes);
    // Close BEFORE judging: a buffered write can still fail at flush, and a
    // file reported as saved that is short is worse than an honest failure.
    file.close();
    if (written != bytes.size() || file.error() != QFileDevice::NoError)
        return tr("Could not finish writing that file.");

    // Counts and flags only. Never the path (it can carry a real name), never
    // the room name, never a body.
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
    // "Lobby" is the head of whatever the column is currently showing. With a
    // Space selected that is THAT SPACE'S overview — its rooms and subspaces,
    // its People, its settings — which is the page Sable's Lobby row opens and
    // the page a single tap on the rail tile already opens. Clearing the
    // selection instead made Lobby a "leave this Space" control wearing the
    // wrong name: the column jumped back to the whole account and there was no
    // way back to the Space's own overview from inside it.
    //
    // A pseudo rail selection ("" for Home, "@orphans" for the unparented
    // rooms) is not a Space, so those still open the account's Home. Either
    // way this is openSpaceHome's teardown, not a second copy of it: the
    // ordering (close the timeline before the selection clears) matters, and
    // reusing it is what keeps the two paths from drifting.
    const QString active = m_spaces ? m_spaces->activeSpaceId() : QString();
    openSpaceHome(active.startsWith(QLatin1Char('!')) ? active : QString());
}

bool AppController::trimHistoryAndJumpToLive()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    // Gather the state; the POLICY lives in historyTrimAllowed() so each
    // clause is testable on its own (see that predicate's note).
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
    // Report what actually happened. A swallowed dispatch failure would
    // leave the caller in "trim succeeded" state — follow-latest persisted
    // and stickToBottom true — while no reset ever arrives, so the next
    // live message would teleport a reader who is still mid-history
    // (review finding, 2026-08-19).
    return rust->reloadRoomTimelineAtLive(m_currentRoomId);
#else
    return false;
#endif
}

void AppController::reloadCurrentRoomTimeline(int limit)
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_currentRoomId.isEmpty())
        return;
    // v0.5.7: "Refresh current room" re-opens the SDK timeline (fresh
    // snapshot + new subscription generation) instead of the old
    // Room::messages snapshot path. `limit` is retained for API
    // compatibility; the SDK snapshot covers the cached history.
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
    // Claim BEFORE dispatching: the bridge answers synchronously from its
    // RAM cache in the common already-rendered case.
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
    // Identify by MAGIC BYTES (never a claimed MIME — the forward path's
    // rule): the same signatures rooms::sniff_image_mime accepts, so a
    // mislabelled or SVG payload never reaches the clipboard as "image".
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
    // JPEG XL. The ISOBMFF CONTAINER is tested before the bare codestream:
    // a container's own payload begins with the codestream signature, so the
    // short test first would mis-report a container as a bare stream.
    // Verified against real cjxl 0.12.0 output.
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
    // Both representations: a decoded raster (universal paste) AND the
    // original bytes under their true MIME (byte-exact paste for targets
    // that accept the format, e.g. an animated GIF stays animated).
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
    // A star fetch still in flight when the user signs out simply produces
    // no starFinished()/banner: the QML Connections that would show it are
    // gone with the rest of the UI, and MediaBridge::clear() (called from
    // clearCrossAccountCaches on the next login) drops the in-flight
    // request. Deliberate, not a bug — there is no surface left to report
    // to by the time it would resolve.
    // Claim this answer BEFORE dispatching: MediaBridge can answer
    // synchronously from its RAM cache.
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
    // v0.6.6 perf fix (review H1a): nothing to possibly match — skip the
    // MediaBridge round trip (and the SHA-256 it would otherwise compute)
    // entirely rather than hashing a row's bytes just to compare against an
    // empty store. This removes 100% of the hashing cost for anyone who has
    // never starred anything, which is most GIF-viewing traffic in the app.
    if (!m_gif->starredStore()->isOpen() || m_gif->starredStore()->count() == 0)
        return false;
    // Durable path: content-addressed, using only bytes MediaBridge's
    // ordinary display cache already fetched for this row (never a fresh
    // fetch just to answer this question, and memoized per cache key — see
    // MediaBridge::cachedFullContentHash) — see GifStarredStore's class
    // comment for the full rationale.
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
    // v0.6.6 perf fix (review H1a): see isChatGifStarred above.
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
    // v0.7.1: confirming is only meaningful while the emoji list is on
    // screen. Repeated clicks and stray invocations in any other state
    // (confirming, waiting_for_peer, done, cancelled, failed…) are no-ops.
    if (m_verificationState != QLatin1String("sas_ready"))
        return;
    // Flip to "confirming" SYNCHRONOUSLY so the button press always has
    // immediate visible feedback; the SDK's SasState::Confirmed then moves
    // it to "waiting_for_peer", and a synchronous FFI failure lands in the
    // existing verificationFailed path ("failed:…").
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
    // A failure raised BEFORE any flow id exists (no cross-signing identity,
    // request send failed, not signed in) still puts the card into a
    // "failed:…" state. Returning early on an empty flow id left Dismiss and
    // Cancel doing nothing at all, so that card could never be closed. Only
    // the SDK cancel needs a real flow; clearing local presentation state is
    // always safe and is what lets the user start over.
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
    // Closing the card must not leave a zombie code on screen. The wire
    // cancel above already told the peer; this drops the picture and the
    // grid bytes with it.
    clearVerificationQr();
    Q_EMIT verificationStateChanged();
#endif
}

// The user confirmed the OTHER device reported a successful scan. This is
// a request to the SDK, never a trust promotion: `confirming` is local
// progress feedback, and only verificationDone (SDK QrVerificationState
// ::Done) may report success.
void AppController::confirmQrVerification()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    // Only meaningful once the SDK has actually reported the scan. Repeat
    // clicks, and clicks before the peer scanned, are no-ops — the Rust FFI
    // refuses the latter outright rather than letting it be swallowed.
    if (!m_verificationQrScanned || m_verificationQrConfirming)
        return;
    m_verificationQrConfirming = true;
    Q_EMIT verificationStateChanged();
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->confirmQrVerification(m_verificationFlowId);
#endif
}

// Drop the displayed code and the grid backing it. Called from every path
// that ends, replaces, or resets a flow, so a code can never outlive the
// verification it belongs to or leak into the next account's UI. Does not
// emit — the caller owns the notification.
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
        m_cryptoQueryGeneration = m_cryptoHealth->generation();
        rust->queryCryptoHealth();
    }
#endif
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
        // Model first, so the UI honestly re-enters the waiting state the
        // coordinator's events will refine (or re-escalate).
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
    // Non-Rust backends have no crypto machine; the model already reports
    // unsupported.
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
    // Only accept local files. Reject arbitrary URL schemes and
    // directories at the boundary.
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
    // Note: `passphrase` is passed by const-ref and goes out of scope on
    // return. It is never copied to a member field.
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
        // The normal Rust sign-out lifecycle performs the same account-scoped
        // deletion after server logout and handle release.
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
    // A failed reset re-arms the destructive action only when the backend
    // still believes one would help. The "nothing matched, nothing deleted"
    // outcome deliberately does NOT re-arm it — offering the same no-op
    // again is how the old dead end kept the user in a loop — so let the
    // backend's own signal drive the flag rather than inverting `ok` here.
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
    // Deliberately NOT gated on ENABLE_RUST_SDK_BACKEND. RustSessionPolicy is
    // pure classification with no Rust dependency and is compiled into every
    // configuration; gating it made the same reason code answer differently
    // per build, so the repair UI silently lost all its actions on the
    // non-Rust tree. Whether a local reset can repair a failure is a property
    // of the failure, not of which backend is compiled in.
    return matrix::rust_session::suggestsLocalResetForCode(reasonCode);
}

void AppController::repairLocalSession()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    // The account captured when the failure was DETECTED wins over anything
    // derived from settings. During an add-account attempt the settings'
    // active account is still the previously signed-in one, so a
    // settings-derived repair would quarantine the wrong account's store.
    // The invariant "no destructive local reset for a reason a reset cannot
    // repair" lives HERE, not in a QML label binding. QML decides which
    // actions to *show*; this decides what may actually run. Without it the
    // guarantee depended on a per-reason list maintained by hand in QML, and
    // on the dialog still describing the same failure it was opened for — a
    // failure can change underneath an open confirmation.
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
    // Deliberately does NOT clear the classified failure. The two are
    // independent: `localSessionBlocked` reports a real failure whose remedy
    // is NOT a local reset, so it sets a reason code while leaving this flag
    // false. Coupling them meant that call cleared the very failure it had
    // just recorded. Callers that genuinely resolve a failure clear it
    // explicitly via clearLocalSessionFailure().
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
    if (accountChanged)
        clearCrossAccountCaches();
    // Every notification raised from now on is stamped with THIS account, so
    // a reply or mark-as-read taken after the next switch can be refused
    // instead of acting under the wrong identity. Set after the cache clear,
    // which drops the previous account's still-pending payloads.
    if (m_notifications)
        m_notifications->setAccountUserId(uid);
    // v0.6.6: the local-starred-GIF store is account-scoped storage (see
    // GifStarredStore's header) — point it at this account's own directory
    // on every login, including the first one and a same-account restore
    // (openFor() is cheap and idempotent; an empty root leaves it closed).
    // Path from the ONE shared helper — the open path and both delete
    // paths must never derive this independently (silent divergence here
    // is how cleanup reports "absent" while decrypted bytes survive).
    m_gif->openStarredStoreFor(matrix::app_data::starredGifsDir(uid));
    m_accounts->setActiveUser(uid);
    setLocalRustResetRequired(false);
    clearLocalSessionFailure();
    m_switchFallbackUserId.clear();
    setAccountSwitching(false);
    m_client->startSync();
    // Cache the account's own display name / avatar for the switcher UI.
    m_client->fetchUserProfile(uid);
    // The background restore after a failed add-account attempt must not
    // yank the user off the login screen: the footer regains the real
    // connection state while the error/retry form stays visible.
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
    // WHERE THE TRAY CARRIES THE NOTIFICATIONS, IT SHOWS WHILE THEY ARE ON.
    // A build without QtDBus (Windows, macOS) has no freedesktop daemon to
    // talk to, and Qt's only other delivery is the tray icon's balloon —
    // which needs a visible icon. Until 2026-09-05 those platforms showed
    // no notification at all unless "keep running in the tray" happened to
    // be on. So there the icon follows the notifications setting as well as
    // the close-to-tray one; on a D-Bus desktop nothing changes.
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
    // NOT gated on the tray. This walk also withdraws the desktop
    // notifications of every room that is no longer unread, and until
    // 2026-09-06 the early return above the loop skipped it whenever the
    // tray icon was off — which is the default — so a read room's KDE
    // notification never went away ("if message is read, in client can you
    // make kde notification go away too"). Only the badge write needs the
    // icon.
    if (!m_client)
        return;
    // ONE derivation, from the snapshot the client already holds. There is no
    // account-wide unread total anywhere else in the application to reuse
    // (RoomListModel exposes per-row values, SpaceManager sums a Space's own
    // children), and the fields read here — RoomInfo::unreadCount,
    // hasUnreadMessages, markedUnread — are the very fields every one of
    // those surfaces reads. Nothing is fetched: rooms() is a local snapshot.
    int total = 0;
    bool anyUnread = false;
    for (const RoomInfo &room : m_client->rooms()) {
        // Invites are not messages. They already have their own notification
        // and their own row; counting one as an unread message would be a
        // claim the state does not make.
        if (room.membership != RoomInfo::Joined)
            continue;
        total += qMax(0, room.unreadCount);
        const bool roomUnread = room.unreadCount > 0 || room.hasUnreadMessages
                                || room.markedUnread;
        if (roomUnread)
            anyUnread = true;
        // A ROOM THAT IS NO LONGER UNREAD WITHDRAWS ITS NOTIFICATIONS.
        //
        // Level-triggered on purpose: this runs on every room change, so it
        // cannot miss the transition the way an edge-triggered "the user just
        // read this" hook would — and the read may not have happened here at
        // all. Reading the room in another client clears the unread through
        // sync, and the desktop notification was still sitting there
        // afterwards, asserting that something was waiting when nothing was.
        //
        // Costs nothing for the overwhelming majority of rooms: the payload
        // map is small and bounded, and a room with no live notification
        // returns immediately.
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
    m_playback->stopAll(); // no playback (or decrypted-media handle) survives
    // Unsent clipboard images belong to the session that staged them. Both
    // composers clear their queues on the way out, which releases each token
    // individually; this is the belt-and-braces sweep, so a queue that failed
    // to clear cannot leave image bytes in memory across a sign-out or an
    // account switch.
    m_stagedImages.clear();
    // A cropped picture is derived from a file the OUTGOING account's user
    // chose. Its temp copies go with the session, exactly like the staged
    // bytes above.
    m_imageCrop.clearSession();
    Q_EMIT currentRoomIdChanged();
    if (m_accountSwitching) {
        // The old session was detached locally as part of a switch; stay on
        // the main screen while the target account activates. A plain
        // switch never deletes the outgoing account's starred-GIF store —
        // only a genuine sign-out (below) does.
        return;
    }
    // v0.6.6: a genuine sign-out (never a switch — that returned above,
    // never merely "removing the account record" as such, since removing
    // the ACTIVE logged-in account (removeAccount()) delegates to
    // AuthManager::logout() and lands here too). The local-starred-GIF
    // store for the account that WAS active must not survive: real
    // sign-out already deletes the Rust crypto store this same way
    // (RustSdkMatrixClient::finishSignOut -> removeAccountRustState), and
    // this app-level store follows the identical CLAUDE.md §6 rule ("never
    // leave decrypted material behind after the user asked to sign out").
    // Read m_lastSessionUserId BEFORE it is cleared just below — it is the
    // identity that was actually active, never re-derived for a different
    // account. This handler runs BEFORE m_client.get()'s own
    // MatrixClient::loggedOut connection (registered LATER, so it fires
    // later in the same dispatch — see that lambda's own comment), so the
    // close happens explicitly here rather than being assumed already done.
    if (!m_lastSessionUserId.isEmpty()) {
        m_gif->closeStarredStore(); // never delete a directory still "open"
        const QString starredDir =
            matrix::app_data::starredGifsDir(m_lastSessionUserId);
        const auto outcome = matrix::app_data::removeAppDataDir(starredDir);
        // A FAILED delete leaves decrypted material behind — warn, so it
        // survives a normal log filter; deleted/absent stay informational.
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
    m_accounts->clearActiveUser();
    m_lastSessionUserId.clear();
    m_addAccountReturnTo.clear();
    m_backgroundRestore = false;
    Q_EMIT errorReported(QString{});
    // v0.7: when other accounts remain signed in, continue with the most
    // recently added one instead of dropping to the login screen.
    const QStringList remaining = m_settings->savedAccountUserIds();
    for (auto it = remaining.crbegin(); it != remaining.crend(); ++it) {
        if (m_backend == MockBackend
            || !m_settings->accessTokenFor(*it).isEmpty()) {
            switchToAccount(*it);
            return;
        }
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
    // MediaBridge::clear() drops in-flight requests with NO terminal
    // emission, so a star fetch outstanding at sign-out would otherwise
    // strand its key here for the process lifetime — and a later FORWARD of
    // that same media would then have its answer claimed and written to the
    // saved-media store, which is exactly what the claim set prevents.
    m_pendingStarKeys.clear();
    m_pendingCopyKeys.clear();
    m_notifications->clearPending();
    // No account: anything still on screen belongs to nobody, and an action
    // on it must not be attributed to whoever signs in next.
    m_notifications->setAccountUserId(QString());
    m_knownInvites.clear();
    // Encrypted-room drafts are memory-only and account-scoped; the next
    // account must never see them. (Persisted drafts live under the
    // previous account's own settings group.)
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
    m_roomKeyImportState.clear();
    m_roomKeyImportImported = 0;
    m_roomKeyImportTotal = 0;
    m_roomKeyImportAffected = 0;
    m_roomKeyImportMessage.clear();
    m_roomKeyImportRunning = false;
    m_roomKeyImportAffectedRoomIds.clear();
    Q_EMIT securityStateChanged();
    Q_EMIT roomKeyImportStateChanged();
    // Drop DM profile lookups resolved under the previous account's
    // authority.
    m_roomList->clearProfileCaches();
}

// ── v0.7.4 own display name ─────────────────────────────────────────────

bool AppController::canEditOwnDisplayName() const
{
    return m_client && m_client->isLoggedIn()
           && m_client->supportsOwnProfileEditing();
}

int AppController::displayNameLength(const QString &name) const
{
    // Unicode code points, not UTF-16 code units. QString stores an emoji
    // as a surrogate PAIR, so `name.size()` would count it twice and the
    // editor would refuse a 200-emoji name the server accepts — and a
    // truncation at 255 units could cut one in half. Combining marks and
    // ZWJ joiners count as their own code points here, deliberately: that
    // is the same unit the Rust bound and the server use, so the number
    // the user is shown is the number that is enforced.
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
    // Single-flight, for the same reason the display name is: two writes
    // racing for one control means the loser's answer lands last.
    if (m_avatarOp != 0)
        return false;
    if (!canEditOwnAvatar()) {
        m_avatarError = tr("This account cannot change its picture here.");
        Q_EMIT ownAvatarStateChanged();
        return false;
    }
    // A LOCAL file only. The crop dialog hands back a file:// URL; anything
    // else (an http URL, an empty value) is refused here rather than handed
    // to the FFI to interpret.
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
    // The op id is claimed BEFORE the backend call: a backend is allowed
    // to answer a synchronous refusal from inside setOwnDisplayName, and
    // an answer for an id this controller has not stored yet would be
    // dropped as stale — the editor would then spin forever.
    m_displayNameOp = ++m_displayNameOpCounter;
    m_displayNameError.clear();
    Q_EMIT ownDisplayNameStateChanged();
    m_client->setOwnDisplayName(name, m_displayNameOp);
    return true;
}

bool AppController::submitOwnDisplayName(const QString &name)
{
    // Single-flight: a second Save while one is in flight would leave two
    // ops racing for one editor, and the loser's answer would be reported
    // over the winner's.
    if (m_displayNameOp != 0)
        return false;
    const QString unavailable = ownDisplayNameUnavailableReason();
    if (!unavailable.isEmpty()) {
        m_displayNameError = unavailable;
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    // Trimmed for the comparison and for the wire — a name of spaces is
    // not a name. The INTERIOR of the string is untouched: no case
    // folding, no ASCII filter, no normalisation. Emoji, ZWJ sequences,
    // combining marks and mixed scripts go out exactly as typed.
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        // Clearing is a separate, deliberate action. An editor emptied by
        // a stray select-all must never silently erase the name.
        m_displayNameError =
            tr("Enter a name, or use Clear to remove your display name.");
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    if (displayNameLength(wanted) > ownDisplayNameMaxLength()) {
        // Refused rather than truncated: a silent cut would send something
        // the user did not type and then report it as saved.
        m_displayNameError = tr("Display names are limited to %1 characters.")
                                 .arg(ownDisplayNameMaxLength());
        Q_EMIT ownDisplayNameStateChanged();
        return false;
    }
    if (wanted == cachedOwnDisplayName()) {
        // Belt and braces — the editor disables Save in this state and
        // never reaches here. No error: nothing went wrong, there is
        // simply nothing to send, and claiming a save for a request that
        // was never made is a claim we cannot support.
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
    // Deliberately NOT refused when the cached name is already empty: the
    // cache can be stale or simply never fetched, and asking the server to
    // remove a field it does not have is harmless — whereas refusing here
    // would leave a user who really does have a name stuck with it.
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
    // Retired alongside the display-name write for the same reason: an
    // in-flight op that outlives its account would otherwise leave the next
    // account's control disabled with nothing coming to re-enable it.
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
    if (m_backend != MockBackend
        && m_settings->accessTokenFor(target).isEmpty()) {
        Q_EMIT errorReported(
            tr("That account's sign-in has expired. Sign in to it again."));
        return;
    }

    qCInfo(lcApp) << "account switch begin"
                  << "from=" << matrix::app_data::safeUserSlug(
                         m_settings->activeAccountUserId())
                  << "to=" << matrix::app_data::safeUserSlug(target);
    setAccountSwitching(true);
    m_switchFallbackUserId =
        m_client->isLoggedIn() ? m_settings->activeAccountUserId() : QString{};

    // LEAVE THE CALL BEFORE THE SESSION GOES, for the same reason
    // prepareForShutdown() does it: a membership RETRACTION is a Matrix send
    // and a Leave is an SFU command, and both need the client that
    // detachSession() is about to release.
    //
    // This path did not do it, and a live log said so exactly:
    //
    //   account switch begin from= … to= …
    //   detaching local session …
    //   rust client released …
    //   teardown state= 6
    //   retraction could not be dispatched — this device will remain in the
    //   room's call membership until it expires
    //
    // The teardown ran AFTER the release, so rtcRetractMembership had no
    // handle and returned 0. With no MSC4140 delayed retraction on this
    // homeserver either, the membership then sat in the room until `expires`
    // — a phantom participant every other client in the call had to see, and
    // the exact failure the retraction retry machinery exists to prevent.
    //
    // Unconditional, and `leave()` is documented safe in any state including
    // mid-join: a join still in flight is precisely the case that would
    // otherwise strand a membership published moments earlier.
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
    // Shortcut bindings are per account. The registry caches the resolved
    // sequences (data() runs once per role per row per repaint, so a
    // QSettings read per miss would be a real cost), so the switch has to
    // be ANNOUNCED — it cannot be discovered.
    if (m_shortcuts)
        m_shortcuts->reload();
    // Hidden images are per account and cached in memory for the same reason
    // shortcuts are, so the switch has to be ANNOUNCED here too — it cannot
    // be discovered.
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

void AppController::removeAccount(const QString &userId)
{
    const QString target = userId.trimmed();
    if (target.isEmpty() || m_accountSwitching)
        return;
    if (!m_settings->hasSavedAccount(target))
        return;

    const bool isActive = target == m_settings->activeAccountUserId();
    if (isActive && m_client->isLoggedIn()) {
        // Real (server) logout. Local store/record cleanup (Rust crypto
        // store AND the local-starred-GIF store — see
        // AppController::onLoggedOut) and switching to a remaining account
        // follow from the logout flow.
        m_auth->logout();
        return;
    }

    // Background (or signed-out) account: delete its local state without
    // touching the active session.
    //
    // Resolve from the SAVED record, which binds the store slug this account
    // actually uses. Re-deriving the identity from the user id would delete
    // the canonical path and leave a store written under a divergent slug
    // sitting on disk — Megolm and device keys surviving an explicit
    // "remove account", while the cleanup reports success.
    matrix::app_data::AccountIdentity identity;
    bool resolved = m_settings->resolveSavedIdentity(target, &identity);
    if (!resolved) {
        // Never leave everything behind because the record was unreadable:
        // fall back to the canonical layout so a removal still removes
        // something rather than silently succeeding.
        const QString hs = m_settings->accountRecord(target)
                               .value(QStringLiteral("homeserver")).toString();
        resolved = matrix::app_data::resolveAccountIdentity(hs, target, &identity);
    }
    if (resolved) {
        // Retiring a Rust client is asynchronous (RustSdkMatrixClient::
        // releaseRustHandle), so the account being removed may still have an
        // open SQLite store. Deleting the directory out from under it is the
        // one race that leaves key material on disk while reporting success —
        // exactly the data-at-rest defect §6 has a rule against. Wait for the
        // close first; removal is a deliberate, rare action and can afford it.
#ifdef ENABLE_RUST_SDK_BACKEND
        RustSdkMatrixClient::waitForRustRetirement(
            RustSdkMatrixClient::kStoreCloseBudgetMs);
#endif
        const auto removed = matrix::app_data::removeAccountRustState(identity);

        // v0.6.6: the local-starred-GIF store lives under the CANONICAL
        // account root (matrix::app_data::accountRoot(userId) — see
        // GifStarredStore's header), which can differ from
        // identity.accountRoot for an account with a recorded divergent
        // store slug — the roots-sweep loop below already handles that
        // divergence for the Rust store/cache.sqlite, but this directory
        // gets its own explicit, distinctly-reported deletion here rather
        // than relying on being incidentally swept. Close it first if it
        // happens to be the store this process currently has open (e.g.
        // this was the last signed-in account before being fully signed
        // out, and nothing has opened a different account's store since —
        // MatrixClient::loggedOut's own close only fires on a session
        // detach, which this explicit-removal path is not).
        const QString starredDir =
            matrix::app_data::starredGifsDir(identity.userId);
        if (m_gif->starredStore()->currentDirectory() == starredDir)
            m_gif->closeStarredStore();
        const auto starredOutcome = matrix::app_data::removeAppDataDir(starredDir);
        // FAILED leaves decrypted material behind — warn (normal filters).
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

        // A divergent store slug means this account owns TWO directories: the
        // recorded one holding the SDK store, and the canonical one, which is
        // where CacheStore::openFor() unconditionally puts cache.sqlite
        // (derived from accountRoot(userId), never from the recording).
        // Removing only one leaves the other behind — room ids, unencrypted
        // bodies and display names surviving a removal that told the user
        // its local data was deleted from this computer.
        QStringList roots{identity.accountRoot};
        const QString canonical = matrix::app_data::accountRoot(identity.userId);
        if (!canonical.isEmpty() && !roots.contains(canonical))
            roots.append(canonical);
        for (const QString &root : roots) {
            QDir accountDir(root);
            if (accountDir.exists())
                accountDir.removeRecursively();
        }
        qCInfo(lcApp) << "removed account local state"
                      << "slug=" << identity.slug
                      << "roots=" << roots.size()
                      << "rust_deleted=" << removed.deleted
                      << "failed=" << removed.failed;
    } else {
        qCWarning(lcApp) << "removal could not resolve an account layout"
                         << "slug=" << matrix::app_data::safeUserSlug(target);
    }
    m_accounts->removeAccount(target); // record + secrets
    if (isActive)
        m_lastSessionUserId.clear();
}

// WHY THIS IS RESOLVED IN C++ RATHER THAN LEFT TO Qt'S FALLBACK.
//
// Qt's automatic per-character font fallback is VERSION-DEPENDENT, measured
// with an identical QPainter probe, the same fonts and the same string: Qt
// 6.8.2 (Debian's, which the Linux AppImage bundles) drew U+1F600 with
// colouredPx=0 -- it prefers a MONOCHROME font that claims the codepoint --
// while Qt 6.11.1 (the dev shell, and every from-source build here) drew
// colouredPx=2580. Naming the family explicitly gave 4400 on BOTH. So emoji
// looked correct in a local build and came out monochrome-or-tofu in the
// packaged one, on the same machine with the same host fonts.
//
// It cannot be a QML token: the QML font value type exposes `family` (one
// string) and NOT `families`, so assigning a list is a LOAD-TIME error -- the
// first attempt at this fix did exactly that and took four QML suites down.
// And picking the first family the host actually HAS beats a hard-coded name,
// because the right face differs per platform and an absent one would degrade
// silently to the behaviour being fixed.
QString AppController::emojiFontFamily() const
{
    // One resolver for the whole process: FontManager::emojiFamily() is
    // also what the application default font carries as its fallback face.
    return FontManager::emojiFamily();
}

// The composer's font: the UI face first, the colour emoji face behind it.
//
// setFamilies() is REAL Qt fallback — the shaper picks per character, so words
// render in Manrope and emoji in the colour face, in one text run. That is what
// QML cannot express: its font value type has `family` and no `families`, so a
// mixed-text surface either names one face for everything or is left to Qt's
// automatic fallback, which on Qt 6.8 prefers a MONOCHROME font that claims the
// codepoint. Reported as "emojis look good in catalog but bad when in text box":
// the picker is single-purpose and a plain family binding fixed it, the composer
// is not.
//
// A QSyntaxHighlighter format was tried first and is kept for its mention ink,
// but a presentation-only format run is the wrong lever for a FACE change.
QFont AppController::textFontWithEmoji(const QString &family, int pixelSize,
                                       bool italic) const
{
    QFont font;
    if (pixelSize > 0)
        font.setPixelSize(pixelSize);
    font.setItalic(italic);
    // The base face comes from the CALLER, so the theme keeps deciding it --
    // hardcoding one here would silently ignore a themed UI font.
    QStringList families;
    if (!family.isEmpty())
        families << family;
    const QString emoji = emojiFontFamily();
    // Empty when the host has no emoji font at all: then this is just the UI
    // face and behaves exactly as before rather than naming something absent.
    if (!emoji.isEmpty())
        families << emoji;
    if (families.isEmpty())
        return font;
    font.setFamilies(families);
    return font;
}
