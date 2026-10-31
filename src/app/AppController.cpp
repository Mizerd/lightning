#include "app/AppController.h"

#include "matrix/BridgeNetwork.h"

#include "app/FontManager.h"
#include "app/LogExport.h"
#include "app/GuiStallTracer.h"
#include "app/MouseNavigationFilter.h"

#include "app/RichComposerBridge.h"
#include "app/RendererNotice.h"
#include "app/MembershipLostNotice.h"
#include "update/InstallType.h"
#include <QSettings>
#include <QKeyEvent>
#include <QKeySequence>
#include <QCoreApplication>
#include "crypto/BackupController.h"
#include "privacy/AccountPrefsController.h"
#include "crypto/E2eeDiagnostics.h"
#include "models/ScheduledSendController.h"
#include "models/ActivityModel.h"
#include "models/MediaHistoryModel.h"

#include <algorithm>

#include "app/CustomAppIcon.h"
#include "app/ApplicationBadge.h"
#include "app/SessionDiagnostics.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#ifndef LIGHTNING_RUST_ONLY
#include "matrix/CppHttpMatrixClient.h"
#include "matrix/MockMatrixClient.h"
#endif
#include "matrix/RoomUnreadRule.h"
#include "media/MediaManager.h"
#include "media/MprisServer.h"
#include "models/MessageComposer.h"
#include "models/EmojiCatalog.h"
#include "models/MessageHtml.h"
#include "models/RoomListModel.h"
#include "models/SpaceChannelModel.h"
#include "models/ReverseListProxyModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "notifications/NotificationManager.h"
#include "notifications/NotificationContextBuilder.h"
#include "app/SwitchTrace.h"
#include "accounts/RustBridgeApi.h"
#include "accounts/SessionPool.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "spaces/SpaceManager.h"
#include "storage/SecretStore.h"
#include "storage/InMemorySecretStore.h"
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include "app/ScreenshotDemoController.h"
#endif
#include "calls/CallController.h"
#include "calls/OneToOneDm.h"
#include "calls/CallSoundPlayer.h"
#ifdef HAVE_LIGHTNING_WEBRTC
#include "app/SandboxEnvironment.h"
#include "calls/CameraPortal.h"
#include "calls/GstBootstrap.h"
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
#include <QDate>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QImageReader>
#include <QStandardPaths>
#include <QSysInfo>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <optional>
#include <utility>
#include <QPalette>
#include <QStyleHints>
#include <QUuid>
#include <QLoggingCategory>
#include <memory>
#include <type_traits>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif
#ifdef Q_OS_MACOS
#include <climits> // OPEN_MAX
#endif

Q_LOGGING_CATEGORY(lcApp, "matrix.app")

namespace {
// Delay before the optional automatic update check. Derived from
// UpdateManager's startup quiet period plus a margin: QTimer::singleShot may
// fire early at this scale, and with equal values the check would be refused
// for the whole session.
constexpr int kAutomaticUpdateCheckDelayMs =
    int(lightning::update::UpdateManager::kStartupQuietPeriodMs) + 5 * 1000;

std::unique_ptr<SecretStore> makeSecretStore(
    const AppController::SecretStoreFactory &factory, QObject *parent)
{
    return factory ? factory(parent) : SecretStore::createDefault(parent);
}
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
    : AppController(backend, screenshotDemo, SecretStoreFactory{}, parent)
{
}

AppController::AppController(Backend backend, bool screenshotDemo,
                             SecretStoreFactory secretStoreFactory,
                             QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_screenshotDemo(screenshotDemo)
    // Screenshot-demo mode must never construct a production secure store,
    // whose constructor probes the real Secret Service.
    , m_secretStore(screenshotDemo
                        ? std::unique_ptr<SecretStore>(
                              std::make_unique<InMemorySecretStore>(this))
                        : makeSecretStore(secretStoreFactory, this))
    , m_secretStoreFactory(std::move(secretStoreFactory))
    , m_settings(std::make_unique<SettingsManager>(this))
{
    // Live accounts (Labs): the base of a background account's backlog
    // floor. Read nowhere while live accounts are off.
    m_appStartWallMs = QDateTime::currentMSecsSinceEpoch();
#ifdef LIGHTNING_RUST_ONLY
    // Fail closed: a release binary must never run a non-Rust backend.
    if (backend != RustBackend)
        qFatal("LIGHTNING_RUST_ONLY: refusing to start a non-Rust backend");
#endif
    // Wire the SecretStore before anything reads accessToken(); this also
    // migrates a legacy plaintext token.
    m_settings->setSecretStore(m_secretStore.get());

    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        auto *hints = QGuiApplication::styleHints();
        if (m_settings->disableCursorBlink())
            hints->setCursorFlashTime(0);
        connect(m_settings.get(), &SettingsManager::disableCursorBlinkChanged,
                this, [this, hints] {
                    // Negative removes the override, re-reading Qt's platform
                    // hint rather than imposing our own blink interval.
                    hints->setCursorFlashTime(
                        m_settings->disableCursorBlink() ? 0 : -1);
                });
    }

    // Installs no catalog until main.cpp calls applyStoredLanguage() before
    // the QML engine loads.
    m_localization = std::make_unique<LocalizationManager>(m_settings.get(), this);
    // The default QLocale follows the language, but QML timestamp bindings
    // only re-run on a notify: clockTimeFormat reads the locale fresh, so
    // announce it once the language change is complete (AppLocale::changed
    // comes after the engine has retranslated; bindings that call
    // Qt.locale(AppLocale.name) depend on it as well).
    connect(AppLocale::instance(), &AppLocale::changed,
            m_settings.get(), &SettingsManager::clockFormatChanged);
    m_customTheme = std::make_unique<CustomThemeStore>(m_settings.get(), this);
    m_networkRoute = std::make_unique<NetworkRouteController>(m_settings.get(), this);
    // Constructed before any QML exists so the first frame already carries
    // the user's bindings.
    m_shortcuts = std::make_unique<ShortcutRegistry>(m_settings.get(), this);
    // Its labels are translated when the tables are built, which is before
    // the stored language is applied, so rebuild them on every change.
    connect(m_localization.get(), &LocalizationManager::languageChanged,
            m_shortcuts.get(), &ShortcutRegistry::retranslate);
    // Touches no bus until the user turns it on; the portal sits idle (Off)
    // otherwise.
    m_globalShortcuts =
        std::make_unique<GlobalShortcutsPortal>(m_shortcuts.get(), this);
    connect(m_settings.get(), &SettingsManager::globalShortcutsEnabledChanged,
            this, [this] {
                m_globalShortcuts->setEnabled(
                    m_settings->globalShortcutsEnabled());
            });
    m_globalShortcuts->setEnabled(m_settings->globalShortcutsEnabled());
    m_railLayout = std::make_unique<RailLayoutStore>(m_settings.get(), this);
    m_railEntries = std::make_unique<RailEntryModel>(this);
    m_mediaVisibility = std::make_unique<MediaVisibilityStore>(this);
    // Persistence is account-scoped; this loads the active account's list.
    m_mediaVisibility->setSettings(m_settings.get());
    m_banners = std::make_unique<ProfileBannerManager>(this);
    m_backdrops = std::make_unique<ChatBackdropController>(this);
    // Personal backgrounds and the depth preference are account-scoped.
    m_backdrops->setSettings(m_settings.get());
    m_settingsSync = std::make_unique<SettingsSyncController>(this);
    m_settingsSync->setSettings(m_settings.get());
    m_settingsSync->setShortcuts(m_shortcuts.get());
    m_settingsSync->setCustomThemes(m_customTheme.get());
    m_nameColors = std::make_unique<NameColorManager>(this);
    m_bio = std::make_unique<ProfileBioManager>(this);
    m_userProfiles = std::make_unique<UserProfileResolver>(this);
    // A fixed local table: decoration, not Matrix state.
    m_badges = std::make_unique<ProfileBadges>(this);
    // The cropper previews through the in-memory staged-image store, so QML
    // never loads a user-chosen file:// path (which could render an SVG).
    m_imageCrop.setStagedImages(&m_stagedImages);

    m_client       = makeClient(backend, m_settings.get(), this);
#ifdef ENABLE_RUST_SDK_BACKEND
    // The client mirrors its revocation proofs into the per-account map
    // (live accounts, Phase 0). Nothing consults the map yet: login and the
    // move-aside still read the client's own fields.
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->setRevocationProofs(&m_revocationProofs);
#endif
    m_accounts     = std::make_unique<AccountManager>(m_settings.get(), this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_directAvatars = std::make_unique<DirectAvatarResolver>(this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_allRooms     = std::make_unique<RoomListModel>(this);
    m_roomList->setDirectAvatarResolver(m_directAvatars.get());
    m_allRooms->setDirectAvatarResolver(m_directAvatars.get());
    m_quickSwitcher = std::make_unique<QuickSwitcherModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    // State rows ("X joined the room") are worded in the app language.
    connect(AppLocale::instance(), &AppLocale::changed,
            m_timeline.get(), &TimelineModel::retranslate);
    m_timelineView = std::make_unique<ReverseListProxyModel>(this);
    m_timelineView->setSourceModel(m_timeline.get());
    m_composer     = std::make_unique<MessageComposer>(this);
    // MSC4108 QR login shares the verification code store: both show one
    // code at a time and a stale token renders nothing.
    m_policy       = std::make_unique<PolicyListController>(this);
    m_about        = std::make_unique<AboutController>(this);
    // versionSummary() builds translated labels on demand; QML only re-reads
    // it on this signal.
    connect(m_localization.get(), &LocalizationManager::languageChanged,
            m_about.get(), &AboutController::versionSummaryChanged);
    m_qrLogin      = std::make_unique<QrLoginController>(this);
    m_qrLogin->setQrStore(&m_qrCodeStore);
    m_richComposer = std::make_unique<RichComposerBridge>(this);
    m_richComposer->setComposer(m_composer.get());
    // Clipboard images never become files; one store serves both composers.
    m_composer->attachments()->setStagedImages(&m_stagedImages);

    // Spell checking. Automatic is the system locale (the keyboard decides
    // the dictionary) plus the UI language when it has a dictionary; the
    // user's own languages are honoured exactly. Reports "unavailable" when
    // no dictionary exists; see `--spell-status` / `--spell-selftest`. The
    // personal dictionary is per operating-system user, not per account, and
    // lives under the app-data root like every other file (FontManager's
    // imported fonts): inside the portable folder on a portable Windows
    // install, %LOCALAPPDATA% on an installed one. No root, no file: the
    // list is then kept for the session only.
    const QString spellRoot = matrix::app_data::primaryRoot();
    m_spell.setPersonalDictionaryPath(
        spellRoot.isEmpty()
            ? QString()
            : spellRoot + QStringLiteral("/spelling/personal-dictionary.txt"));
    m_spell.configure(m_settings->spellCheckLanguage(),
                      m_settings->spellCheckExtraLanguages(),
                      m_localization->effectiveLanguage());
    m_spell.setEnabled(m_settings->spellCheckEnabled());
    auto applyAutocorrect = [this] {
        const int mode = m_settings->autocorrectMode();
        m_spell.setAutocorrectEnabled(mode == 0 ? SpellChecker::autocorrectDefault()
                                                : mode == 1);
    };
    applyAutocorrect();
    connect(m_settings.get(), &SettingsManager::autocorrectModeChanged, this,
            applyAutocorrect);
    connect(m_settings.get(), &SettingsManager::spellCheckEnabledChanged, this,
            [this] { m_spell.setEnabled(m_settings->spellCheckEnabled()); });
    connect(m_settings.get(), &SettingsManager::spellCheckLanguageChanged, this,
            [this] { m_spell.setPreferredLanguage(m_settings->spellCheckLanguage()); });
    connect(m_settings.get(), &SettingsManager::spellCheckExtraLanguagesChanged, this,
            [this] { m_spell.setExtraLanguages(m_settings->spellCheckExtraLanguages()); });
    connect(m_localization.get(), &LocalizationManager::languageChanged, this,
            [this] { m_spell.setInterfaceLanguage(m_localization->effectiveLanguage()); });

    // Privacy settings (read receipts, typing) are applied once here so a
    // stored choice holds from the first receipt, and again on change.
    applyPrivacyPreferences();
    connect(m_settings.get(), &SettingsManager::readReceiptModeChanged, this,
            [this] { applyPrivacyPreferences(); });
    // Thread style. The composer follows the timeline's thread filter: while
    // the room is narrowed to one thread, the room composer sends into it.
    applyThreadStyle(false);
    connect(m_settings.get(), &SettingsManager::threadStyleChanged, this,
            [this] { applyThreadStyle(true); });
    connect(m_timeline.get(), &TimelineModel::threadPresentationChanged, this,
            [this] {
                m_composer->setThreadTargetRootId(
                    m_timeline->threadFilterRootId());
                // The model's toggle (room header, filter bar) is the setting.
                m_settings->setHideThreadReplies(
                    m_timeline->hideThreadReplies());
                applyReceiptThreading();
            });
    m_timeline->setHideThreadReplies(m_settings->hideThreadReplies());
    connect(m_settings.get(), &SettingsManager::hideThreadRepliesChanged, this,
            [this] {
                m_timeline->setHideThreadReplies(
                    m_settings->hideThreadReplies());
            });
    // MSC4153 is applied at client build time, so a change affects the next
    // client only.
    connect(m_settings.get(), &SettingsManager::strictDeviceTrustChanged, this,
            [this] { applyStrictDeviceTrust(); });
    applyStrictDeviceTrust();
    connect(m_settings.get(), &SettingsManager::keepMediaOnDeviceChanged, this,
            [this] { applyKeepMediaOnDevice(); });
    applyKeepMediaOnDevice();
    connect(m_settings.get(), &SettingsManager::sendTypingNotificationsChanged,
            this, [this] { applyPrivacyPreferences(); });

    // The tray icon exists only while the user asked for it and the platform
    // has a tray.
    connect(&m_tray, &TrayIcon::showRequested,
            this, &AppController::trayShowRequested);
    // "Quit Lightning" in the tray menu. Announce the intent first, as the
    // updater does: close-to-tray would otherwise refuse the window close and
    // abort the quit.
    connect(&m_tray, &TrayIcon::quitRequested, this, [this] {
        Q_EMIT applicationQuitIntended();
        QCoreApplication::quit();
    });
    connect(m_settings.get(), &SettingsManager::closeToTrayChanged,
            this, &AppController::refreshTrayState);
    connect(m_settings.get(), &SettingsManager::notificationsEnabledChanged,
            this, &AppController::refreshTrayState);
    // The tooltip's account name follows the cached profile.
    connect(m_accounts.get(), &AccountManager::accountsChanged,
            this, &AppController::refreshTrayAccountLabel);
    // A tray host that appears after launch (autostart racing the panel's
    // StatusNotifierWatcher, a bar started later) gets its icon then, and one
    // that goes away takes it with it.
    connect(this, &AppController::trayAvailableChanged,
            this, &AppController::applyTrayState);
    refreshTrayState();
#ifdef Q_OS_MACOS
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
    // Like Element Desktop: closing the window (red button, Cmd+W) hides it
    // and Lightning keeps running in the Dock, whether or not the menu-bar
    // item is enabled; the Dock icon brings the window back. Qt would
    // otherwise treat the hidden last window as "closed" and quit when there
    // is no tray icon to keep it alive. Every real quit is explicit (Cmd+Q,
    // the app or Dock menu's Quit and the tray Quit all end in
    // QCoreApplication::quit() or a spontaneous QEvent::Quit), so nothing
    // relies on this default.
    QGuiApplication::setQuitOnLastWindowClosed(false);
#endif
    // Qt has no signal for a StatusNotifierWatcher (or, on Windows, the
    // shell's tray host) appearing or leaving the session — the only way to
    // learn availability changed is to ask again. Cheap (one call into
    // QSystemTrayIcon::isSystemTrayAvailable()), so a plain poll rather than
    // a platform-specific D-Bus watcher.
    m_trayAvailabilityTimer.setInterval(4000);
    connect(&m_trayAvailabilityTimer, &QTimer::timeout,
            this, &AppController::refreshTrayAvailability);
    m_trayAvailabilityTimer.start();

    // Decide once, against the current display layout, whether the stored
    // position is still reachable (SettingsManager already enforced the
    // minimum size). With no screens at all the geometry is dropped.
    if (const QRect stored = m_settings->initialWindowGeometry();
            !stored.isEmpty()) {
        if (windowGeometryIsReachable(stored)) {
            // Fitted to the screen it lands on: a size saved on a larger
            // screen, or before the interface zoom went up, opened past the
            // screen's edges.
            m_restorableWindowGeometry = fitWindowGeometry(stored);
            if (m_restorableWindowGeometry != stored)
                qCInfo(lcApp, "stored window geometry fitted to its screen");
        } else {
            qCInfo(lcApp, "stored window geometry ignored: off-screen");
        }
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
    m_accountPrefs = std::make_unique<AccountPrefsController>(this);
    m_scheduledSends = std::make_unique<ScheduledSendController>(this);
    m_widgets = std::make_unique<WidgetController>(this);
    m_activity = std::make_unique<ActivityModel>(this);
    m_mediaHistory = std::make_unique<MediaHistoryModel>(this);
    // Its date-group headers are worded in the formatting locale.
    connect(AppLocale::instance(), &AppLocale::changed,
            m_mediaHistory.get(), &MediaHistoryModel::retranslate);
    m_cryptoBootstrap = std::make_unique<CryptoBootstrapModel>(this);
    m_spaces       = std::make_unique<SpaceManager>(this);
    m_threads      = std::make_unique<ThreadManager>(this);
    m_presence     = std::make_unique<PresenceManager>(this);
    m_presence->setSettings(m_settings.get());
    m_gameActivity = std::make_unique<GameActivity>(this);
    m_gameActivity->setPresence(m_presence.get());
    m_gameActivity->setSettings(m_settings.get());
    m_musicActivity = std::make_unique<MusicActivity>(this);
    m_musicActivity->setPresence(m_presence.get());
    m_musicActivity->setSettings(m_settings.get());
    m_scheduledSends->setSettings(m_settings.get());
    // A muted room's rows are hidden from the bell and its badge (unmuting
    // brings them back), the same rule as the room list.
    m_activity->setNotificationModeSource([this](const QString &id) {
        return m_settings ? m_settings->roomNotificationMode(id) : 0;
    });
    // Only the seen marker and the keywords persist, account-scoped.
    m_activity->setStore({ [this] { return m_settings->activityState(); },
                           [this](const QVariantMap &state) {
                               m_settings->setActivityState(state);
                           } });
    // The Activity opening list's read-row horizon is a device setting; the
    // panel's menu writes the model and the model writes it back here. A
    // widened horizon needs the server's opening page again to list what it
    // had skipped (ids already present are skipped by seed()).
    m_activity->setReadHorizonDays(m_settings->activityReadHorizonDays());
    connect(m_settings.get(), &SettingsManager::activityReadHorizonDaysChanged,
            this, [this] {
        m_activity->setReadHorizonDays(m_settings->activityReadHorizonDays());
    });
    connect(m_activity.get(), &ActivityModel::readHorizonDaysChanged, this,
            [this] {
        m_settings->setActivityReadHorizonDays(m_activity->readHorizonDays());
        if (m_client && m_activitySeeded
            && m_client->connectionState() == MatrixClient::Syncing)
            m_client->requestActivitySeed(60);
    });
    // Voice calls. The media backend is attached later, only when the build
    // has the GStreamer engine and its elements resolve at runtime;
    // LIGHTNING_DISABLE_WEBRTC=1 is the kill switch.
    m_calls        = std::make_unique<CallController>(this);
    m_rtc          = std::make_unique<RtcController>(this);
    m_groupCall    = std::make_unique<SfuCallController>(this);
    m_callDevices  = std::make_unique<CallDeviceController>(this);
    m_callSounds   = std::make_unique<CallSoundController>(this);
    m_audioTester  = std::make_unique<AudioDeviceTester>(this);
    // Application updates: not account-scoped, so sign-in, sign-out and
    // account switches never disturb a check or download.
    m_updateManager = std::make_unique<lightning::update::UpdateManager>(this);
    m_about->setPackageKind(m_updateManager->installTypeLabel());
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
    m_savedMessages = std::make_unique<SavedMessagesController>(this);
    m_roomUpgrade  = std::make_unique<RoomUpgradeController>(this);
    m_openRoomPermissions = std::make_unique<OpenRoomPermissions>(this);
    m_thread       = std::make_unique<ThreadController>(this);
    connect(m_thread.get(), &ThreadController::stateChanged, this, [this] {
        if (m_roomHistoryNavigating || m_currentRoomId.isEmpty())
            return;
        if (m_thread->active()) {
            if (m_thread->roomId() == m_currentRoomId)
                m_roomHistory.visitThread(m_currentRoomId,
                                          m_thread->rootEventId());
            return;
        }
        const auto places = m_roomHistory.places();
        const int cursor = m_roomHistory.cursor();
        if (cursor >= 0 && cursor < places.size()
            && places.at(cursor).kind == lightning::RoomHistory::Place::Thread
            && places.at(cursor).roomId == m_currentRoomId)
            m_roomHistory.visit(m_currentRoomId);
    });
    m_eventContext = std::make_unique<ContextController>(this);
    // Their timelines carry state rows too.
    connect(AppLocale::instance(), &AppLocale::changed,
            m_thread->model(), &TimelineModel::retranslate);
    connect(AppLocale::instance(), &AppLocale::changed,
            m_eventContext->model(), &TimelineModel::retranslate);
    m_thread->attachments()->setStagedImages(&m_stagedImages);
    // Sender grouping window: a presentation policy of the layout preset
    // (Nheko compact repeats a sender's header only after an hour, as Nheko
    // does; Lightning keeps five minutes). Both conversation models follow.
    const auto applySenderGrouping = [this] {
        const int window =
            m_settings->layoutPreset() == SettingsManager::kLayoutPresetNhekoCompact
            ? 60 * 60 : TimelineModel::kDefaultSenderGroupWindowSeconds;
        m_timeline->setSenderGroupWindowSeconds(window);
        if (m_thread && m_thread->model())
            m_thread->model()->setSenderGroupWindowSeconds(window);
    };
    applySenderGrouping();
    connect(m_settings.get(), &SettingsManager::layoutPresetChanged, this,
            applySenderGrouping);
    // The rich composer bridge serves both composers.
    m_richComposer->setThread(m_thread.get());
    m_conversations= std::make_unique<ConversationController>(this);
    m_discovery = std::make_unique<RoomDiscoveryController>(this);
    m_messageSearch = std::make_unique<MessageSearchController>(this);
    m_about->setSearch(m_messageSearch.get());
    m_uia = std::make_unique<UiaController>(this);
    m_passwordChange = std::make_unique<PasswordChangeController>(this);
    // OAuth (MAS) accounts change their password on the account page.
    m_passwordChange->setOAuthAccountCheck(
        [this] { return activeAccountIsOAuth(); });
    m_roomKeyExport = std::make_unique<RoomKeyExportController>(this);
    m_roomExport = std::make_unique<RoomExportController>(this);
    m_moderation = std::make_unique<ModerationController>(this);
    m_pushRules = std::make_unique<PushRulesController>(this);
    connect(m_pushRules.get(), &PushRulesController::stateChanged,
            this, [this] {
        // An unavailable rule cannot hide activity. An explicit Off applies
        // to both All and Reactions, including existing rows and the badge.
        m_activity->setReactionsVisible(!m_pushRules->loaded()
                                        || m_pushRules->eventReactions() != 0);
    });
    // An account-wide default alert level changed. Rooms that follow it now
    // behave differently, so re-read the per-room modes at once (every room
    // with a rule of its own, plus the open room, which may have none) instead
    // of leaving them stale until the next open. The refresh announces itself
    // on roomNotificationModesRefreshed, which the room pickers listen to.
    connect(m_pushRules.get(), &PushRulesController::accountDefaultChanged,
            this, [this] {
        refreshStoredNotificationModes();
        requestRoomNotificationMode(m_currentRoomId);
    });
    m_notificationTroubleshooter =
        std::make_unique<NotificationTroubleshooter>(this);
    m_notificationTroubleshooter->setInputsProvider(
        [this](const QString &roomId) {
            NotificationTroubleshooter::Inputs in;
            in.delivery = NotificationTroubleshooter::probeDeliveryPath();
            in.platform = NotificationTroubleshooter::currentPlatform();
            in.appEnabled = m_settings && m_settings->notificationsEnabled();
            in.rulesKnown = m_pushRules && m_pushRules->loaded();
            in.masterMute = m_pushRules && m_pushRules->masterMute();
            in.roomId = roomId;
            // Whether the balloon fallback has an icon to come from.
            in.trayIconShown = m_tray.enabled();
            // -1 when nothing is stored for the room on this device: a room
            // never configured here follows the account default, which the
            // check resolves below rather than reading "all messages".
            if (!roomId.isEmpty() && m_settings
                && m_settings->hasStoredRoomNotificationMode(roomId))
                in.roomMode = m_settings->roomNotificationMode(roomId);
            if (!roomId.isEmpty() && m_pushRules) {
                const bool direct =
                    m_client && m_client->roomInfo(roomId).isDirect;
                in.accountDefaultLevel = notices::accountDefaultLevel(
                    m_pushRules->loaded(), direct, m_pushRules->dmAll(),
                    m_pushRules->roomsAll());
            }
            return in;
        });
    m_notificationTroubleshooter->setTestSender([this] {
        if (!m_notifications)
            return;
        // A real notification through the normal path. It names nothing of
        // the account: a body is written to the desktop's log in plaintext.
        m_notifications->showGeneric(
            tr("Lightning"),
            tr("This is a test notification. If you can read it, desktop "
               "notifications work."));
    });
    m_forward      = std::make_unique<ForwardController>(this);
    m_bulkRedaction = std::make_unique<BulkRedactionController>(this);
    m_roomInfo     = std::make_unique<RoomInfoController>(this);
    m_roomBatch    = std::make_unique<RoomBatchController>(this);
    m_spaceModeration = std::make_unique<SpaceModerationController>(this);
    m_spaceModeration->setScopeResolver([this](const QString &spaceId) {
        return m_spaces ? m_spaces->moderationScopeRoomIds(spaceId)
                        : QStringList{};
    });
    // Close a room or Space (Matrix has no client-side delete), or delete one
    // from the homeserver as its administrator.
    m_roomClosure = std::make_unique<RoomClosureController>(this);
    m_roomClosure->setScopeResolver([this](const QString &spaceId) {
        return m_spaces ? m_spaces->moderationScopeRoomIds(spaceId)
                        : QStringList{};
    });
    m_roomClosure->setParentResolver([this](const QString &roomId) {
        // The joined Spaces that list the room as a direct child.
        QStringList parents;
        if (!m_client)
            return parents;
        for (const RoomInfo &room : m_client->rooms()) {
            if (room.isSpace && room.membership == RoomInfo::Joined
                && room.childRoomIds.contains(roomId)) {
                parents.append(room.id);
            }
        }
        return parents;
    });
    m_roomClosure->setRoomInfoResolver([this](const QString &roomId) {
        if (m_client) {
            for (const RoomInfo &room : m_client->rooms()) {
                if (room.id == roomId)
                    return QVariantMap{ { QStringLiteral("name"), room.name },
                                        { QStringLiteral("isSpace"),
                                          room.isSpace } };
            }
        }
        return QVariantMap{};
    });
    m_mediaBridge  = std::make_unique<MediaBridge>(this);
    m_fileChooser = std::make_unique<FileChooser>(this);
    m_fileChooser->setFolderMemory(
        [this](const QString &purpose) {
            return m_settings ? m_settings->lastFileFolder(purpose) : QUrl();
        },
        [this](const QString &purpose, const QUrl &folder) {
            if (m_settings)
                m_settings->rememberFileFolder(purpose, folder);
        });
    m_fileChooser->setDefaultFolder([this] { return defaultFileDialogFolder(); });
    m_logExport = std::make_unique<LogExportController>(this);
    m_logExport->setDiagnosticsProvider(
        [this] { return logExportDiagnostics(); });
    m_logExport->setLiteralsProvider([this] { return logExportLiterals(); });
    m_fileLauncher = std::make_unique<FileLauncher>(this);
    m_downloads = std::make_unique<DownloadsController>(
        m_mediaBridge.get(), m_fileChooser.get(), m_fileLauncher.get(), this);
    m_downloads->setSettings(m_settings.get());
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
    // Lightning's own notification chime, through the call sound player so
    // the call state (in a call, ringing, share capturing the mix) gates it.
    // Whose sound plays follows the setting live.
    m_notifications->setSoundSource(m_settings->notificationSoundSource());
    connect(m_settings.get(), &SettingsManager::notificationSoundSourceChanged,
            this, [this] {
        m_notifications->setSoundSource(m_settings->notificationSoundSource());
    });
    m_notifications->setOwnSoundPlayer([this](bool mention) {
        return m_callSounds->playNotificationSound(mention);
    });
    // The same call-state policy gates the platform's sound ("System
    // default"): no desktop sound into a call, a ring, or a share capturing
    // the output mix.
    m_notifications->setSoundAllowedCheck([this] {
        return !m_callSounds || m_callSounds->policy().allowsNotificationSound();
    });
    // A delivered message card while the main window is not in front asks for
    // attention (taskbar flash, Dock bounce), as other clients do.
    connect(m_notifications.get(), &NotificationManager::messageDelivered,
            this, [this](const QString &, bool) { alertMainWindow(); });
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
            // Typing a `:token` is opening a pack surface too: the first one in
            // a room reads that room's (and its Spaces') packs, which plain
            // navigation does not (live GUI run: completion offered no room
            // pack until a picker had been opened). A no-op once covered.
            m_stickers->refreshIfStale();
            return m_stickers->findEmoticons(prefix, limit);
        });
    // A typed (not chosen) `:shortcode:` resolves only when unambiguous; a
    // chosen one carries its exact image (MessageComposer selections).
    m_composer->setEmoticonResolver([this](const QString &shortcode) {
        return m_stickers->uniqueEmoticonUrl(shortcode);
    });
    // Standard Unicode emoji in the same `:query` popup (":thumbs" -> 👍),
    // merged with the custom packs above by MessageComposer::emojiCompletionsAt.
    // Accepting one, or typing out a complete `:shortcode:`, inserts the
    // glyph itself rather than literal shortcode text, so it never touches
    // the MSC2545 resolver above.
    m_composer->setUnicodeEmojiSearch(
        [this](const QString &prefix, int limit) {
            return m_emojiCatalog->completionsForPrefix(prefix, limit);
        });
    m_composer->setUnicodeShortcodeResolver([this](const QString &code) {
        return m_emojiCatalog->emojiForShortcode(code);
    });
    m_composer->setUnicodeEmojiUseRecorder([this](const QString &emoji) {
        m_emojiCatalog->recordUse(emoji);
    });
    connect(m_settings.get(), &SettingsManager::emojiShortcodeAutoConvertChanged,
            this, [this] {
                m_composer->setEmojiAutoConvertEnabled(
                    m_settings->emojiShortcodeAutoConvert());
            });
    m_composer->setEmojiAutoConvertEnabled(
        m_settings->emojiShortcodeAutoConvert());
    // The thread composer sends through the same emoji pass (one rule set).
    m_thread->setOutgoingEmoji([this](QString &body, QVariantMap &spec) {
        m_composer->applyOutgoingEmoji(body, spec);
    });
    // An inline emoji's proportions, from the cached file's header (never a
    // pixel decode), remembered per source. Only a KNOWN aspect is
    // remembered: an emoji rendered before its bytes arrived used to cache 0
    // for good, so a 2:1 image stayed a square for the rest of the session
    // (live run 2026-10-26).
    const auto inlineAspect = MessageHtml::rememberingAspect(
        [this](const QString &source) { return m_mediaBridge->cachedImageSize(source); });
    const auto inlineResolver = [this](const QString &mxc) {
        return m_mediaBridge->mxcImageSource(mxc, 64);
    };
    // The thread panel's model renders the same bodies: without these it
    // showed "in thread" with the chosen emoji missing (live run).
    for (TimelineModel *model : { m_timeline.get(), m_thread->model() }) {
        model->setInlineImageAspect(inlineAspect);
        model->setInlineImageResolver(inlineResolver);
    }
    connect(m_mediaBridge.get(), &MediaBridge::mediaCached,
            m_timeline.get(),
            [this](const QString &) {
                // An emoji resolves to "" until its bytes arrive; re-read on
                // arrival.
                m_timeline->notifyInlineImagesChanged();
                m_thread->model()->notifyInlineImagesChanged();
            });
    connect(m_mediaBridge.get(), &MediaBridge::mediaFetchFailed,
            m_notifications.get(),
            [this](const QString &cacheKey, const QString &) {
                if (cacheKey.startsWith(QLatin1String("mxc:")))
                    m_notifications->avatarCacheChanged();
            });
    m_playback     = std::make_unique<MediaPlaybackController>(this);
    // The voice/audio player outlives the timeline rows that start it.
    m_voicePlayback = std::make_unique<VoicePlaybackController>(this);
    m_voicePlayback->setMediaBridge(m_mediaBridge.get());
    m_voicePlayback->setAudibility(m_playback.get());
    m_voicePlayback->setSettings(m_settings.get());
    m_voicePlayback->setRoomNameResolver([this](const QString &roomId) {
        return m_allRooms
            ? m_allRooms->findRoom(roomId).value(QStringLiteral("name"))
                  .toString()
            : QString();
    });
    // The desktop's media keys and "now playing" widgets reach that player
    // through MPRIS while a clip is loaded (D-Bus desktops; inert elsewhere).
    m_mpris = std::make_unique<MprisServer>(m_voicePlayback.get());
    connect(m_mpris.get(), &MprisServer::raiseRequested, this,
            &AppController::trayShowRequested);
    // A desktop "now playing" widget and the lock screen are as public as a
    // notification: the metadata follows the notification preview privacy
    // of the clip's room (sender and title / sender only / nothing).
    m_mpris->setPreviewPolicy([this](const QString &roomId) {
        return noticePreviewFor(roomId);
    });
    connect(m_settings.get(), &SettingsManager::notificationPreviewChanged,
            m_mpris.get(), &MprisServer::refreshMetadata);
    connect(m_settings.get(),
            &SettingsManager::notificationPreviewEncryptedChanged,
            m_mpris.get(), &MprisServer::refreshMetadata);
    m_pagination   = std::make_unique<PaginationController>(this);
    m_readReceipts = std::make_unique<ReadReceiptCoordinator>(this);
    m_linkPreviews = std::make_unique<LinkPreviewController>(this);
    // A link's own media reaches MediaBridge only through a preview that
    // loaded under the preview policy (LinkPreviewController::resolveLinkMedia).
    m_mediaBridge->setLinkMediaResolver([this](const QString &mediaKey) {
        return m_linkPreviews ? m_linkPreviews->resolveLinkMedia(mediaKey)
                              : QVariantMap{};
    });
    m_gifTransport = std::make_unique<MatrixGifTransport>(this);
    m_gif          = std::make_unique<GifSearchController>(this);
    m_gif->setTransport(m_gifTransport.get());
    m_gifSend      = std::make_unique<GifSendController>(this);
    m_stickers     = std::make_unique<StickerPackManager>(this);
    // Wired here, after the manager exists: these used to run ~130 lines
    // earlier, on a null m_stickers, and every start crashed in
    // setLocalStatePath (QString::operator=, wave-2 master build gui-2).
    // Hidden packs, custom-image recents and favourites: this device only.
    // Beside the app's own settings file, so portable mode (which redirects
    // QSettings into the portable tree) keeps it there too; the config dir
    // when settings are not a file (the Windows registry).
    m_stickers->setLocalStatePath([] {
        const QSettings settings;
        const QFileInfo file(settings.fileName());
        const QString dir = file.isAbsolute() && QFileInfo(file.absolutePath()).isDir()
            ? file.absolutePath()
            : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        return dir + QStringLiteral("/image-packs-local.ini");
    }());
    // A confirmed "add this private image to my stickers": the decrypted bytes
    // come through the bridge's keyed full-payload fetch.
    m_stickers->setPrivateMediaFetch([this](const QString &mediaKey) {
        m_mediaBridge->fetchFullForStar(mediaKey);
    });
    connect(m_mediaBridge.get(), &MediaBridge::mediaBytesForStar, m_stickers.get(),
            [this](const QString &mediaKey, bool ok, const QByteArray &bytes,
                   const QString &) {
                m_stickers->onPrivateMediaBytes(mediaKey, ok, bytes);
            });
    // A chosen custom emoji that was actually sent counts as a use.
    m_composer->setCustomEmojiUseRecorder([this](const QVariantMap &row) {
        m_stickers->recordCustomUse(row);
    });
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
            &AppController::handleLiveEvent);
    // Live events the main timeline never appends: a thread reply in the open
    // room (its live timeline hides threaded events, and only the one thread
    // open in the panel has a timeline of its own), and a remote event the
    // SDK inserts before a local echo still at the bottom (a send pending,
    // uploading or failed). For notifications and the Activity Center only.
    connect(m_client.get(), &MatrixClient::liveEventObserved, this,
            &AppController::handleLiveEvent);
    // A MatrixRTC ring's declared intent ("video"), remembered per ring so the
    // card and the missed-call notice say what kind of call it was. Connected
    // before CallController attaches to the client, so it is recorded before
    // incomingCallStarted is emitted from the same signal. Bounded: a ring
    // that never ends here is forgotten once newer ones push it out.
    connect(m_client.get(), &MatrixClient::callSignalReceived, this,
            [this](const CallSignal &signal) {
        if (signal.kind != CallSignal::Kind::RtcNotification || signal.own
            || signal.eventId.isEmpty())
            return;
        if (m_callVideoIntent.size() >= 16)
            m_callVideoIntent.clear();
        m_callVideoIntent.insert(
            signal.eventId,
            signal.callIntent.compare(QLatin1String("video"),
                                      Qt::CaseInsensitive) == 0);
    });
    // An event first seen undecryptable, now readable: the manager re-runs
    // its decision on the plaintext (and replaces or withholds the earlier
    // "Encrypted message" card itself).
    connect(m_client.get(), &MatrixClient::eventDecrypted, this,
            [this](const QString &composedRoomId, const TimelineEvent &event) {
        if (MatrixClient::isContextTimelineId(composedRoomId))
            return;
        const bool fromThread = MatrixClient::isThreadTimelineId(composedRoomId);
        const QString roomId = fromThread
            ? MatrixClient::threadTimelineRoomId(composedRoomId)
            : composedRoomId;
        if (roomId.isEmpty() || event.eventId.isEmpty())
            return;
        NotificationManager::Context context =
            notificationContextFor(roomId, event, fromThread);
        // Live accounts (Labs): as handleLiveEvent(), the title's label.
        if (m_sessionPool)
            context.accountLabel =
                liveAccountLabel(m_settings->activeAccountUserId());
        m_notifications->processDecrypted(event, context);
    });
    // Aggregated edits (and removal of edits) arrive as an in-place SDK Set.
    // Refresh a card already showing this event without announcing it again.
    connect(m_client.get(), &MatrixClient::eventChangedAt, this,
            [this](const QString &timelineId, int, TimelineEvent event) {
        if (MatrixClient::isContextTimelineId(timelineId))
            return;
        const bool fromThread = MatrixClient::isThreadTimelineId(timelineId);
        const QString roomId = MatrixClient::threadTimelineRoomId(timelineId);
        event.roomId = roomId;
        if (fromThread && event.threadRootId.isEmpty())
            event.threadRootId = MatrixClient::threadTimelineRootId(timelineId);
        m_notifications->processUpdated(
            event, notificationContextFor(roomId, event, fromThread));
    });
    // A redacted message's card goes; its text must not outlive it on the
    // desktop.
    connect(m_client.get(), &MatrixClient::eventRedacted, this,
            [this](const QString &, const QString &eventId) {
        if (!eventId.isEmpty())
            m_notifications->withdrawEvent(eventId);
    });
    // A mention the sync never delivered (an unopened room receives only its
    // newest event per response) still deserves a card: one summary per
    // room, replacing the previous one.
    connect(m_client.get(), &MatrixClient::roomHighlightsUnnotified, this,
            [this](const QString &roomId, int count) {
        if (roomId.isEmpty() || MatrixClient::isThreadTimelineId(roomId))
            return;
        const bool muted = m_settings->roomNotificationMode(roomId)
            == static_cast<int>(NotificationManager::Muted);
        const bool visible = roomId == m_currentRoomId && m_activeRoomAtLatest;
        if (!notices::highlightSummaryAllowed(
                m_settings->notificationsEnabled(), m_client->initialSyncDone(),
                muted, visible, masterMuteIsOn(), count))
            return;
        const int preview = noticePreviewFor(roomId);
        const RoomInfo info = m_client->roomInfo(roomId);
        m_notifications->showGeneric(
            tr("New mentions"),
            notices::highlightSummaryBody(preview, count, info.name),
            roomId,
            notices::showsRoomAvatar(preview)
                ? m_roomList->findRoom(roomId)
                      .value(QStringLiteral("avatarUrl")).toString()
                : QString(),
            m_settings->notificationSound() != 0 /* SoundOff */,
            notices::highlightSummaryKey(roomId));
    });
    connect(m_notifications.get(), &NotificationManager::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        // Live accounts (Labs): the account-naming signal, emitted first for
        // the same click, claimed this open for another account, which opens
        // it once that account is in front. Never set otherwise.
        if (m_claimedOpen) {
            const ClaimedOpen claimed = *std::exchange(m_claimedOpen,
                                                       std::nullopt);
            if (claimed.roomId == roomId && claimed.eventId == eventId
                && claimed.threadRootId == threadRootId)
                return;
        }
        routeNotificationOpen(roomId, eventId, threadRootId);
    });
    // The same click, naming the account the card was raised for. Without
    // live accounts it routes nothing: the three-argument signal above opens
    // the room, and an open for an account that is not the active one is
    // only recorded. With them, that open is claimed and switches accounts.
    connect(m_notifications.get(), &NotificationManager::openRequestedForAccount,
            this, [this](const QString &accountUserId, const QString &roomId,
                         const QString &eventId, const QString &threadRootId) {
        if (!accountUserId.isEmpty()
            && accountUserId != m_settings->activeAccountUserId()) {
            qCInfo(lcApp) << "notification open for another account acct="
                          << matrix::app_data::safeUserSlug(accountUserId);
            if (m_sessionPool)
                claimCrossAccountOpen(accountUserId, roomId, eventId,
                                      threadRootId);
        }
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
    // Server-reported per-room notification mode, from a picker opening, the
    // refresh after the first sync, or the retry's read. The server's state
    // wins for the local cache (desktop notifications follow the server's
    // rules anyway), except where this device holds something the server
    // does not: a failed write (unsynced), a device-only legacy mode, or its
    // own write still in flight. Server writes are issued only from
    // setRoomNotificationMode() and the retry, never from a report.
    // Stale-generation events are already rejected in RustSdkMatrixClient.
    connect(m_client.get(), &MatrixClient::roomNotificationModeChanged, this,
            [this](const QString &roomId, int mode, bool userDefined) {
        const bool retrying = m_notificationModeRetryReads.remove(roomId);
        const bool writing = m_notificationModeWritesInFlight.contains(roomId);
        const bool unsynced = m_settings->roomNotificationModeUnsynced(roomId);
        const int local = m_settings->roomNotificationMode(roomId);
        // Before the first sync the SDK may still hold its fallback rule set,
        // which reads as "no rule" everywhere.
        const bool rulesKnown = m_client->initialSyncDone();
        // What the server holds, for the base of a later write. A read racing
        // this device's own write says nothing new unless it is the ack.
        // Not before the account's rules are known: a rule reported then
        // may be the SDK's fallback set, and recorded as "what the server
        // held" it could become a failed write's base. Unknown makes the
        // retry send again, which is safe.
        if (userDefined && rulesKnown && mode >= 0 && mode <= 2
            && (!writing || mode == local))
            m_notificationModeServerSeen.insert(roomId, mode);
        else if (!userDefined && rulesKnown && !writing)
            m_notificationModeServerSeen.insert(roomId, 3);
        // The user's latest choice wins. An earlier choice of this device that
        // the server acknowledged after a later one was sent is what the
        // server holds now, so it is the base of a later failure of the
        // newer write: the retry then sees its own earlier write and sends
        // the latest choice again rather than adopting the earlier one.
        const int sentModes = m_notificationModeSentModes.value(roomId);
        if (userDefined && rulesKnown && mode >= 0 && mode <= 2 && mode != local
            && (sentModes & (1 << mode))) {
            m_notificationModeServerSeen.insert(roomId, mode);
            if (writing)
                m_notificationModeWritesInFlight.insert(roomId, mode);
            else if (unsynced)
                m_settings->setRoomNotificationModeUnsynced(roomId, true, mode);
        }
        if (retrying)
            m_notificationModeSentModes.remove(roomId);
        if (!userDefined) {
            if (unsynced) {
                // A newer choice is still in flight: its acknowledgement or
                // failure decides, and this read may predate it.
                if (!retrying || !rulesKnown || writing)
                    return;
                if (local == 3) {
                    // "No rule" is what this device asked for.
                    m_settings->setRoomNotificationModeUnsynced(roomId, false);
                    Q_EMIT roomNotificationModeSyncStateChanged(roomId);
                    return;
                }
                const int base = m_settings->roomNotificationModeUnsyncedBase(roomId);
                if (base == 1 || base == 2) {
                    // The rule this device's choice replaced is gone: removed
                    // on another client since, which is newer.
                    adoptServerNotificationMode(roomId, 3);
                    return;
                }
                sendRoomNotificationMode(roomId, local);
                return;
            }
            if (writing || !rulesKnown
                || m_settings->roomNotificationModeDeviceOnly(roomId))
                return;
            // No rule: the account default applies, and the picker says so.
            m_settings->setRoomNotificationMode(roomId, 3);
            return;
        }
        // Also reachable directly from tests/backends; drop rather than let
        // SettingsManager clamp to the least conservative mode.
        if (mode < 0 || mode > 2)
            return;
        if (mode == local) {
            // This device's value, acknowledged.
            m_notificationModeWritesInFlight.remove(roomId);
            m_notificationModeSentModes.remove(roomId);
            if (unsynced) {
                m_settings->setRoomNotificationModeUnsynced(roomId, false);
                Q_EMIT roomNotificationModeSyncStateChanged(roomId);
            }
            return;
        }
        if (unsynced) {
            // Outside the retry's own read this is the room's old rule
            // surfacing through a poll; the local value stays.
            if (!retrying || !rulesKnown || writing) {
                qCDebug(lcApp) << "room notification report differs while"
                               << "unsynced (kept local value)";
                return;
            }
            const int base = m_settings->roomNotificationModeUnsyncedBase(roomId);
            if (base < 0 || mode == base) {
                // The server still holds the rule this device's choice
                // replaces: send the choice again.
                sendRoomNotificationMode(roomId, local);
                return;
            }
            // A rule set on another client since the failed write: newer.
            adoptServerNotificationMode(roomId, mode);
            return;
        }
        // A read that started before this device's own write, landing after.
        if (writing)
            return;
        m_settings->setRoomNotificationMode(roomId, mode);
    });
    // A successful rule removal is the only acknowledgement a "follow account
    // default" choice can receive, so it retires the failure state.
    connect(m_client.get(), &MatrixClient::roomNotificationModeCleared, this,
            [this](const QString &roomId) {
        // A clear acknowledged after the user chose an explicit mode: the
        // server holds no rule now, which is the base of a failure of the
        // newer write (see the rule report handler).
        if (m_settings->roomNotificationMode(roomId) != 3) {
            if (m_notificationModeSentModes.value(roomId) & (1 << 3)) {
                m_notificationModeServerSeen.insert(roomId, 3);
                if (m_notificationModeWritesInFlight.contains(roomId))
                    m_notificationModeWritesInFlight.insert(roomId, 3);
                else if (m_settings->roomNotificationModeUnsynced(roomId))
                    m_settings->setRoomNotificationModeUnsynced(roomId, true, 3);
            }
            return;
        }
        m_notificationModeServerSeen.insert(roomId, 3);
        m_notificationModeWritesInFlight.remove(roomId);
        m_notificationModeSentModes.remove(roomId);
        if (!m_settings->roomNotificationModeUnsynced(roomId))
            return;
        m_settings->setRoomNotificationModeUnsynced(roomId, false);
        Q_EMIT roomNotificationModeSyncStateChanged(roomId);
    });
    connect(m_client.get(), &MatrixClient::roomNotificationModeWriteFailed,
            this, [this](const QString &roomId) {
        // What the server held before this device's first unacknowledged
        // write; the retry compares with it.
        const int base = m_notificationModeWritesInFlight.value(roomId, -1);
        m_notificationModeWritesInFlight.remove(roomId);
        if (m_settings->roomNotificationModeUnsynced(roomId))
            return;
        m_settings->setRoomNotificationModeUnsynced(roomId, true, base);
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
                m_notifications->clearPendingFor(QString());
                m_inviteNotices.clear();
                m_inviteNoticeTimer.stop();
                // Unsynced rooms are kept per account in settings and survive
                // this; the session's reads and writes do not. Shared with
                // the add-account path, which sees no loggedOut.
                resetSessionNotificationState();
                // Room history is the outgoing session's; never carried over.
                m_roomHistory.clear();
                // A rename in flight must not lock renaming for the next
                // account.
                if (m_sessionDeviceRenameOp != 0
                    || !m_sessionDeviceRenameError.isEmpty()) {
                    m_sessionDeviceRenameOp = 0;
                    m_sessionDeviceRenameError.clear();
                    Q_EMIT sessionDevicesChanged();
                }
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
    // are seeded silently so a restart never re-announces them. The room list
    // can drop and re-add an invite within one burst (a re-sort, a resync),
    // so the tracker, not this handler, decides what is new and what is gone.
    m_inviteNoticeTimer.setSingleShot(true);
    connect(&m_inviteNoticeTimer, &QTimer::timeout, this,
            &AppController::processInviteNotices);
    connect(m_client.get(), &MatrixClient::roomsChanged, this, [this] {
        const auto rooms = m_client->rooms();
        QSet<QString> current;
        QSet<QString> joined;
        for (const auto &room : rooms) {
            if (room.membership == RoomInfo::Joined)
                joined.insert(room.id);
            if (room.membership != RoomInfo::Invited) {
                // Accepted, or left: settled now. Only a room missing from
                // the list waits out the grace. Its card goes either way: a
                // declined invite's card would open a room the user is no
                // longer invited to, and an accepted one has been acted on.
                if (m_inviteNotices.forget(room.id)) {
                    m_activity->inviteResolved(room.id);
                    m_notifications->closeRoomNotifications(room.id);
                }
                // Left, kicked or banned (here or on another device): its
                // cards are withdrawn. Only a room LISTED as left counts; one
                // briefly missing from the list (a re-sort, a resync) has not
                // gone anywhere.
                if (room.membership == RoomInfo::Left
                    && m_joinedRoomIds.contains(room.id)) {
                    m_notifications->closeRoomNotifications(room.id);
                }
                continue;
            }
            current.insert(room.id);
            m_activity->noteInvite(room);
        }
        // Keep the ids of rooms still joined or merely absent for now, so a
        // room that reappears as left after a resync is still recognised.
        for (const auto &room : rooms) {
            if (room.membership != RoomInfo::Joined)
                m_joinedRoomIds.remove(room.id);
        }
        m_joinedRoomIds.unite(joined);
        m_inviteNotices.observe(current, m_client->initialSyncDone(),
                                QDateTime::currentMSecsSinceEpoch());
        processInviteNotices();
        // The open room is no longer one the user is in or invited to: left
        // or kicked elsewhere, or an invite declined from the room list,
        // which unlike Leave never reaches RoomInfoController::roomLeft. The
        // pane stayed on it, its name degraded to the raw id and every send
        // failed. A room LISTED as left goes now; one merely absent (the SDK
        // list filters left rooms out) waits out the invite grace, since a
        // re-sort or resync drops a room for one emission. A room never seen
        // listed (a restore before the list has loaded) is left alone.
        if (m_currentRoomId.isEmpty())
            return;
        const auto listed = std::find_if(
            rooms.cbegin(), rooms.cend(),
            [this](const RoomInfo &room) { return room.id == m_currentRoomId; });
        if (listed != rooms.cend() && listed->membership != RoomInfo::Left) {
            m_currentRoomListed = true;
        } else if (listed != rooms.cend()) {
            closeCurrentRoomNoLongerIn(m_currentRoomId);
        } else if (m_currentRoomListed) {
            // Keyed to this opening and this account: after a switch or a
            // sign-out the next session may open the same id before its own
            // list has loaded, and that is not a room that went away.
            const QString gone = m_currentRoomId;
            const quint64 serial = m_currentRoomOpenSerial;
            const QString account = m_client->currentUserId();
            QTimer::singleShot(InviteNoticeTracker::kGoneGraceMs, this,
                               [this, gone, serial, account] {
                if (m_currentRoomId != gone || !m_client
                    || m_currentRoomOpenSerial != serial
                    || m_client->currentUserId() != account)
                    return;
                const RoomInfo info = m_client->roomInfo(gone);
                if (info.id.isEmpty() || info.membership == RoomInfo::Left)
                    closeCurrentRoomNoLongerIn(gone);
            });
        }
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
    // Muting or unmuting a room changes what the badge counts without any
    // room update (GitHub #18).
    if (m_settings)
        connect(m_settings.get(), &SettingsManager::roomNotificationModeChanged, this,
                [this](const QString &) { m_trayUnreadCoalesce.start(); });

    m_mediaHistory->setClient(m_client.get());
    m_qrLogin->setClient(m_client.get());
    m_policy->setClient(m_client.get());
    m_about->setClient(m_client.get());
    // A fresh client starts with public receipts, so push the stored choice
    // on every attachment.
    applyPrivacyPreferences();
    applyThreadStyle(false);
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
    //
    // The strip shows the last errorReported(), so this mirror says whether a
    // call failure is still what it shows; withdrawCallFailure() must never
    // clear somebody else's message.
    connect(this, &AppController::errorReported, this,
            [this](const QString &message) {
                m_callNotice.reported(message);
                // The strip's message used to stay until another replaced it,
                // minutes later and across every screen. It now expires, goes
                // when the user moves on or starts a new create/join, and can
                // be dismissed; a running call's notice stays (see
                // clearStatusError).
                m_statusError = message;
                const quint64 seq = ++m_statusErrorSeq;
                if (!message.isEmpty())
                    armStatusErrorExpiry(seq);
            });
    // An empty reason withdraws, and only our own notice.
    connect(m_groupCall.get(), &SfuCallController::callFailed, this,
            [this](const QString &reason) {
                if (const auto report = m_callNotice.failed(reason)) {
                    // What the strip's call exemption recognises as ours.
                    m_callStatusNotice = *report;
                    Q_EMIT errorReported(*report);
                }
            });
    // A call that carries on another lane makes the failure stale; it stayed
    // up through a whole working legacy call.
    connect(m_calls.get(), &CallController::stateChanged, this, [this] {
        if (m_calls->state() == CallController::State::Active)
            withdrawCallFailure();
    });
    // The server refused our call membership. The gate now says so, and a
    // join the call button started in a 1:1 DM is re-placed on the legacy
    // lane rather than left failed: the user asked for a call. A join from
    // the call banner is not, since that asks for THIS call.
    // Nothing in QML listened to callStartRefused, so every refused call
    // start (no lane, a network route, video on the legacy lane) did nothing
    // visible. The status strip shows it, as it shows other errors.
    connect(this, &AppController::callStartRefused, this,
            [this](const QString &message) { Q_EMIT errorReported(message); });
    connect(m_rtc.get(), &RtcController::membershipRefused, this,
            [this](const QString &roomId) {
                const auto armed = m_legacyFallback.take(roomId);
                if (!armed.fallBack)
                    return;
                const bool withVideo = armed.video;
                // A user who chose MatrixRTC did not ask for the other format.
                if (roomId != m_currentRoomId
                    || (m_settings
                        && m_settings->callFormat()
                            == QLatin1String("matrixrtc"))
                    || (withVideo && !m_calls->videoSupported())
                    || preferredCallLane(roomId) != QLatin1String("legacy")
                    || m_groupCall->active()
                    || (m_calls->state() != CallController::State::Idle
                        && m_calls->state() != CallController::State::Ended)) {
                    return;
                }
                qCInfo(lcApp) << "matrixrtc membership refused; placing the "
                                 "call on the legacy 1:1 lane instead";
                const bool ok = m_calls->placeCall(
                    roomId, legacyCallPeer(roomId), withVideo);
                qCInfo(lcApp) << "legacy call dispatched ok=" << ok;
                if (ok)
                    withdrawCallFailure();
            });
    // A join failure belongs to the room it happened in; a running call's
    // notices stay while the user looks at other rooms.
    connect(this, &AppController::currentRoomIdChanged, this, [this] {
        m_legacyFallback.roomChanged();
        const bool callRunning =
            m_groupCall->active() || m_calls->sessionLive();
        if (const auto report = m_callNotice.roomChanged(callRunning))
            Q_EMIT errorReported(*report);
        // Moving to another room is moving on, unless the operation that
        // reported the message is the one opening the room.
        if (!m_statusErrorOwnNavigation)
            clearStatusError(/*keepCallNotice=*/true);
    });
    connect(m_spaces.get(), &SpaceManager::activeSpaceIdChanged, this, [this] {
        if (!m_statusErrorOwnNavigation)
            clearStatusError(/*keepCallNotice=*/true);
    });
    // A new create or join is a new attempt: the last one's message goes.
    // Only on the idle -> busy EDGE: a create that succeeds goes busy again
    // to wait for its room (beginWaitForRoom) right AFTER reporting "created,
    // but adding it to the Space failed", and that message is the one the
    // user must see in the room it opens.
    m_conversationsBusySeen = m_conversations->busy();
    connect(m_conversations.get(), &ConversationController::busyChanged, this,
            [this] {
                const bool busy = m_conversations->busy();
                const bool started = busy && !m_conversationsBusySeen;
                m_conversationsBusySeen = busy;
                if (started)
                    clearStatusError(/*keepCallNotice=*/true);
            });
    m_discoveryBusySeen = m_discovery->busy();
    connect(m_discovery.get(), &RoomDiscoveryController::busyChanged, this,
            [this] {
                const bool busy = m_discovery->busy();
                const bool started = busy && !m_discoveryBusySeen;
                m_discoveryBusySeen = busy;
                if (started)
                    clearStatusError(/*keepCallNotice=*/true);
            });
    connect(m_groupCall.get(), &SfuCallController::stateChanged, this,
            [this] {
                if (m_groupCall->active())
                    m_calls->noteAnsweredByOtherLane(m_groupCall->roomId());
                // Any new join, the gate passed, or the call over: nothing
                // left for the button's fallback (CallLanePolicy.h).
                using Phase = lightning::calls::LegacyFallbackArm::Phase;
                switch (m_groupCall->state()) {
                case SfuCallController::State::Idle:
                    m_legacyFallback.phaseChanged(Phase::Idle);
                    break;
                case SfuCallController::State::Preparing:
                    m_legacyFallback.phaseChanged(Phase::Preparing);
                    break;
                case SfuCallController::State::Failed:
                    m_legacyFallback.phaseChanged(Phase::Failed);
                    break;
                case SfuCallController::State::Ended:
                    m_legacyFallback.phaseChanged(Phase::Ended);
                    break;
                default:
                    m_legacyFallback.phaseChanged(Phase::PastTheGate);
                    break;
                }
            });
    // The room of an ongoing call (either lane) leads both room-list layouts,
    // so it does not move as other rooms get activity (GitHub #44). Both models
    // are declared before the call controllers, so they outlive the
    // teardown signal the controllers emit from their destructors.
    {
        const auto applyCallRoom = [this] {
            QString room;
            if (m_groupCall->active()) {
                room = m_groupCall->roomId();
            } else {
                const CallController::State legacy = m_calls->state();
                // Ringing is an offer not yet taken, not an ongoing call.
                if (legacy == CallController::State::Inviting
                    || legacy == CallController::State::Connecting
                    || legacy == CallController::State::Active)
                    room = m_calls->activeRoomId();
            }
            m_roomList->setCallRoomId(room);
            m_spaceChannels->setCallRoomId(room);
        };
        connect(m_groupCall.get(), &SfuCallController::stateChanged,
                m_roomList.get(), applyCallRoom);
        connect(m_calls.get(), &CallController::stateChanged,
                m_roomList.get(), applyCallRoom);
    }
    m_callDevices->setSettings(m_settings.get());
    // The device test opens the chosen devices with the chosen gain, and
    // never while a call (either lane) is ringing, starting or live: the call
    // owns the microphone.
    m_audioTester->setDeviceController(m_callDevices.get());
    m_audioTester->setSettings(m_settings.get());
    {
        const auto updateTesterCallState = [this] {
            const CallController::State legacy = m_calls->state();
            const bool legacyBusy = legacy != CallController::State::Idle
                && legacy != CallController::State::Ended;
            m_audioTester->setCallActive(m_groupCall->active() || legacyBusy);
        };
        // The tester is the CONTEXT object, not `this`: it is declared after
        // the call controllers, so it is destroyed BEFORE them, and the group
        // call's destructor emits stateChanged (teardown -> Ended) into a
        // handler that would call setCallActive on a freed tester. A context
        // object disconnects the handler when it dies.
        connect(m_groupCall.get(), &SfuCallController::stateChanged,
                m_audioTester.get(), updateTesterCallState);
        connect(m_calls.get(), &CallController::stateChanged,
                m_audioTester.get(), updateTesterCallState);
    }
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
    // The account's master rule ("Mute all notifications", which promises to
    // silence every room on every device) silences rings too. Unknown rules
    // keep the ring.
    m_calls->setRingPolicyCheck([this](const QString &) {
        return !masterMuteIsOn();
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
        if (m_bulkRedaction)
            m_bulkRedaction->cancel();
    });

    // A MatrixRTC announcement that does not ring this account (a group
    // call's `notification`, or a `ring` addressed to others): one quiet card
    // per room, never a ringer, withdrawn when the announcement's lifetime
    // runs out. Element shows the same quiet notice.
    connect(m_calls.get(), &CallController::callStartedNotice, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &senderId, qint64 remainingMs) {
        Q_UNUSED(eventId);
        if (roomId.isEmpty() || remainingMs <= 0)
            return;
        const bool muted = m_settings->roomNotificationMode(roomId)
            == static_cast<int>(NotificationManager::Muted);
        const bool visible = roomId == m_currentRoomId && m_activeRoomAtLatest;
        const bool ignored = m_moderation && m_moderation->isIgnored(senderId);
        const bool own = !senderId.isEmpty()
            && senderId == m_client->currentUserId();
        // CallController's own gates first (backlog, ignored sender, muted
        // room, the account's ring policy), so the notice can never speak
        // where a ring would have been silent.
        if (!m_calls->noticeAllowed(roomId, senderId))
            return;
        if (!notices::callStartedNoticeAllowed(
                m_settings->notificationsEnabled(), m_client->initialSyncDone(),
                muted, visible, masterMuteIsOn(), ignored, own))
            return;
        const int preview = noticePreviewFor(roomId);
        const RoomInfo info = m_client->roomInfo(roomId);
        const QString caller =
            CallController::callerNameIn(m_client.get(), roomId, senderId);
        const QString key = notices::callStartedKey(roomId);
        m_notifications->showGeneric(
            tr("Call started"),
            notices::callStartedBody(preview, caller,
                                     info.isDirect ? QString() : info.name),
            roomId,
            notices::showsRoomAvatar(preview)
                ? m_roomList->findRoom(roomId)
                      .value(QStringLiteral("avatarUrl")).toString()
                : QString(),
            false, key);
        // The announcement says how long it stays valid; past that the call
        // may be long over. Only the newest announcement's timer withdraws:
        // a newer one replaced the card and owns it now.
        // Never reset, so a timer from an earlier card or session can
        // never match a later one.
        const quint64 generation = ++m_callNoticeSerial;
        m_callNoticeGeneration.insert(roomId, generation);
        QTimer::singleShot(
            int(qBound<qint64>(qint64(1000), remainingMs,
                               qint64(10 * 60 * 1000))), this,
            [this, roomId, key, generation] {
                if (m_callNoticeGeneration.value(roomId) != generation)
                    return;
                m_callNoticeGeneration.remove(roomId);
                if (m_notifications)
                    m_notifications->withdrawGeneric(key);
            });
    });

    // ── Incoming-call notification + ring ──
    connect(m_calls.get(), &CallController::incomingCallStarted, this,
            [this](const QString &roomId, const QString &callId,
                   const QString &senderId, qint64 remainingMs) {
                // An RTC ring may already be settled by the room's session:
                // picked up on another of our devices, or a session known
                // before the ring that the caller has left. Feeding what is
                // known now lets CallController end it before it announces
                // (shouldRing() is then false), and marks a non-empty
                // session as seen so a later emptying ends the ring.
                if (m_calls->rtcRing() && m_calls->activeRoomId() == roomId)
                    noteRtcSessionForRing(roomId);
                if (!m_settings->notificationsEnabled())
                    return;
                if (!m_calls->shouldRing())
                    return; // backlog, ignored sender, muted room, master mute
                // A room member must not be able to pump critical-urgency
                // cards with fresh call ids, but a redial a few seconds after
                // giving up is a real call and is announced in full (ring,
                // card, and a missed-call notice if it is missed). Only a
                // burst beyond the cap is refused.
                const auto verdict = m_ringLimiter.admit(
                    senderId, callId, QDateTime::currentMSecsSinceEpoch());
                if (verdict == notices::RingAnnounceLimiter::Verdict::Refuse) {
                    qCInfo(lcApp) << "incoming call announcement refused:"
                                  << "sender over the per-sender ring cap";
                    return;
                }
                const int preview = noticePreviewFor(roomId);
                const RoomInfo info = m_client->roomInfo(roomId);
                // Not escaped here: NotificationManager escapes once, at the
                // D-Bus call, only when the daemon advertises body-markup.
                const QString roomName = info.name;
                // The room member's display name when known (with the
                // localpart when it could be mistaken for someone else), else
                // the localpart; never the full MXID.
                const QString caller =
                    CallController::callerNameIn(m_client.get(), roomId,
                                                 senderId);
                const bool video = m_callVideoIntent.value(callId, false);
                const QString title = notices::incomingCallTitle(video);
                const QString body = notices::incomingCallBody(
                    preview, caller, roomName, info.isDirect, video);
                // The ring follows "Ring for incoming calls" alone; the
                // message sound setting is about chat chimes. Lightning's own
                // ringer when it has loaded, else the desktop's themed call
                // sound, never both; without a notification daemon the
                // desktop cannot loop one, so the own ringer always runs. A
                // call the user silenced stays silenced on re-announcement; a
                // new call id rings normally.
#ifdef HAVE_QT_DBUS
                constexpr bool kPlatformLoopsCallSound = true;
#else
                constexpr bool kPlatformLoopsCallSound = false;
#endif
                const notices::RingPlan plan = notices::ringPlan(
                    m_settings->ringForCalls(),
                    m_callSounds->isRingSilenced(callId),
                    m_callSounds->ringerAvailable(), kPlatformLoopsCallSound,
                    m_notifications->desktopInhibited());
                m_announcedCallId = callId;
                // Remembered for the missed-call notice, which is raised from
                // a signal that no longer carries the caller.
                m_announcedCaller = caller;
                m_announcedVideo = video;
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
                    roomId, callId, title, body, plan.cardSound,
                    static_cast<int>(qBound<qint64>(
                        qint64(5), remainingMs / 1000, qint64(300))),
                    acceptOffered, rtcLane, /*silenceOffered=*/plan.ring);
                // A minimised or hidden window asks for attention (taskbar
                // flash, Dock bounce) as other clients do.
                alertMainWindow();
                if (plan.ownRinger) {
                    m_callSounds->startIncomingRing(callId);
                    // Our ringer plays on a thread a stalled sound server can
                    // wedge (Qt < 6.10; CallSoundPlayer). Its loop is then
                    // never acknowledged and ringerAvailable() turns false
                    // within ~2 s.
                    const int ringSeconds = static_cast<int>(qBound<qint64>(
                        qint64(5), remainingMs / 1000 - 3, qint64(300)));
                    QTimer::singleShot(
                        2500, this,
                        [this, roomId, callId, title, body, ringSeconds] {
                            if (m_announcedCallId != callId
                                || m_callSounds->ringingCallId() != callId
                                || m_callSounds->isRingSilenced(callId)
                                || m_callSounds->ringerAvailable())
                                return;
                            // Read the gate again: the session read kicked at
                            // the announce has usually opened Join by now, and
                            // a stale answer would take the button away.
                            const bool rtcLane = m_calls->rtcRing();
                            const bool acceptOffered =
                                callAcceptOffered(roomId, rtcLane);
#ifdef HAVE_QT_DBUS
                            // The desktop re-posts its call sound until the
                            // call ends: hand the ring over.
                            qCWarning(lcApp)
                                << "own ringer not playing (sound thread "
                                   "stuck); falling back to the desktop's "
                                   "call sound";
                            m_callSounds->stopIncomingRing();
                            m_notifications->showIncomingCall(
                                roomId, callId, title, body,
                                /*sound=*/true, ringSeconds, acceptOffered,
                                rtcLane, /*silenceOffered=*/true);
#else
                            // The tray balloon gives one cue at most and
                            // cannot loop, so handing the ring over would
                            // ring once (Windows) or not at all (macOS).
                            // Restart our own ringer instead, and let the card
                            // carry one cue meanwhile.
                            qCWarning(lcApp)
                                << "own ringer not playing (sound thread "
                                   "stuck); restarting it (this platform "
                                   "cannot loop a desktop call sound)";
                            m_callSounds->stopIncomingRing();
                            m_callSounds->startIncomingRing(callId);
                            m_notifications->showIncomingCall(
                                roomId, callId, title, body,
                                /*sound=*/true, ringSeconds, acceptOffered,
                                rtcLane, /*silenceOffered=*/true);
#endif
                        });
                }
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
    // Dismiss on the in-app card: the desktop card for that call goes too
    // (local only; the missed-call notice still follows if it is missed).
    connect(m_callSounds.get(), &CallSoundController::ringDismissed, this,
            [this](const QString &callId) {
                m_notifications->stopIncomingCall(callId);
            });
    // The reverse: the user closed the desktop call card. Lightning's own
    // ringer loops independently of the card, so it stops here exactly as
    // the in-app Dismiss would (local only; the call itself is not declined
    // and a missed-call notice still follows if it is missed).
    connect(m_notifications.get(), &NotificationManager::callCardDismissed,
            this, [this](const QString &callId) {
                if (!callId.isEmpty())
                    m_callSounds->dismissRing(callId);
            });
    // Sign-out leaves a live group call FIRST, so its MatrixRTC membership is
    // retracted while the session can still send it. Live 2026-10-07 the
    // logout ran first: "retraction could not be dispatched", and the deleted
    // device stayed in the room's call as a ghost until its membership
    // expired (~5 min). Bounded: a server that does not answer cannot hold
    // the sign-out; the delayed leave (MSC4140) or expiry then covers it.
    // A deferred sign-out whose session is no longer current was dropped:
    // the removal or reset that asked for it did not happen either.
    connect(m_auth.get(), &AuthManager::logoutAbandoned, this, [this] {
        m_pendingRemovalUserId.clear();
        m_pendingRemovalIdentity = {};
        m_pendingRemovalResolved = false;
        if (m_resetResultPending) {
            m_resetResultPending = false;
            Q_EMIT localRustStoreResetResult(
                false, tr("The sign-out was cancelled because the account "
                          "changed. Nothing was reset."));
        }
    });
    // Named so it can run itself again once the activity-line withdrawal
    // below has settled (the second run finds nothing left to withdraw).
    auto beforeLogout = std::make_shared<std::function<void(std::function<void()>)>>();
    *beforeLogout = [this, again = std::weak_ptr(beforeLogout)](
                        std::function<void()> proceed) {
        // The reader's place in the open room (m.fully_read) moves when they
        // leave it; signing out is leaving it, and once loggedOut arrives the
        // session can no longer write. Every sign-out path (menu, removal,
        // local reset) passes through here. Idempotent, as is the call leave
        // below, so the second run after the presence wait repeats nothing.
        if (m_readReceipts)
            m_readReceipts->flushFullyReadMarker();
        SfuCallController *call = m_groupCall.get();
        if (call && call->active()) {
            qCInfo(lcApp) << "sign-out: leaving the call first";
            call->leave();
        }
        // A game or song in the status outlives the session on the server:
        // withdraw both in ONE publish while the session can still write, and
        // wait (bounded, long enough for one rate-limit retry) for the
        // server's answer before the logout revokes the session.
        if (m_presence && m_presence->withdrawActivityLine()) {
            m_presence->whenFarewellSettled(
                PresenceManager::kSignOutFarewellBudgetMs, this, [again, proceed] {
                    if (const auto self = again.lock())
                        (*self)(proceed);
                });
            return;
        }
        if (!call || !call->membershipWritesPending()) {
            proceed();
            return;
        }
        constexpr int kSignOutCallLeaveBoundMs = 4000;
        auto *waiter = new QObject(this);
        auto done = std::make_shared<bool>(false);
        QPointer<SfuCallController> callGuard(call);
        auto finish = [this, waiter, done, proceed, callGuard](bool settled) {
            if (*done)
                return;
            *done = true;
            if (!settled)
                qCWarning(lcApp)
                    << "sign-out: the call's membership retraction did not "
                       "settle within" << kSignOutCallLeaveBoundMs
                    << "ms; signing out anyway";
            // Nothing of this account's may be retried once its session is
            // gone: the next account would send it.
            if (callGuard)
                callGuard->abandonOutstandingMembershipWrites();
            waiter->deleteLater();
            // Never inside the stack that delivered the answer (the backend's
            // event drain): the logout tears that machinery down.
            QTimer::singleShot(0, this, proceed);
        };
        connect(call, &SfuCallController::membershipWritesSettled, waiter,
                [finish] { finish(true); });
        QTimer::singleShot(kSignOutCallLeaveBoundMs, waiter,
                           [finish] { finish(false); });
    };
    m_auth->setBeforeLogout([beforeLogout](std::function<void()> proceed) {
        (*beforeLogout)(std::move(proceed));
    });
    connect(m_calls.get(), &CallController::incomingCallEnded, this,
            [this](const QString &roomId, const QString &callId,
                   int reason, bool missed) {
                Q_UNUSED(reason);
                m_notifications->stopIncomingCall(callId);
                const bool wasAnnounced = m_announcedCallId == callId;
                const QString caller = wasAnnounced ? m_announcedCaller
                                                    : QString();
                const bool video = wasAnnounced && m_announcedVideo;
                if (wasAnnounced) {
                    m_announcedCallId.clear();
                    m_announcedCaller.clear();
                    m_announcedVideo = false;
                }
                m_callVideoIntent.remove(callId);
                // Missed-call notice only for a call classified missed from
                // its pre-end state and actually announced; a ring the gates
                // suppressed must not resurface as missed.
                if (!missed || !wasAnnounced)
                    return;
                if (!m_settings->notificationsEnabled())
                    return;
                const int preview = noticePreviewFor(roomId);
                const RoomInfo info = m_client->roomInfo(roomId);
                // Escaping belongs to NotificationManager alone.
                m_notifications->showGeneric(
                    tr("Missed call"),
                    notices::missedCallBody(preview, caller, info.name,
                                            info.isDirect, video),
                    roomId,
                    notices::showsRoomAvatar(preview)
                        ? m_roomList->findRoom(roomId)
                              .value(QStringLiteral("avatarUrl")).toString()
                        : QString());
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
    // A MatrixRTC ring has no answer or hangup events (MSC4075): whether it
    // was picked up on another of our devices, or abandoned by everyone, is
    // read from the room's memberships. Announced or not, the ring state is
    // CallController's to settle.
    connect(m_rtc.get(), &RtcController::sessionChanged, this,
            [this](const QString &roomId) { noteRtcSessionForRing(roomId); });
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
    m_savedMessages->setClient(m_client.get());
    m_roomUpgrade->setClient(m_client.get());
    m_openRoomPermissions->setClient(m_client.get());
    m_backup->setClient(m_client.get());
    m_accountPrefs->setEmojiCatalog(m_emojiCatalog.get());
    m_accountPrefs->setClient(m_client.get());
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
    connect(m_savedMessages.get(), &SavedMessagesController::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        routeNotificationOpen(roomId, eventId, threadRootId);
    }, Qt::QueuedConnection);
    // Deliver after TimelineModel and PaginationController have handled the
    // backend reset; setRoomId() also resets the model before Rust is ready.
    connect(m_client.get(), &MatrixClient::timelineReset, this,
            [this](const QString &roomId) { Q_EMIT roomTimelineReady(roomId); },
            Qt::QueuedConnection);
    connect(m_client.get(), &MatrixClient::reactionEventReceived, this,
            [this](const QString &roomId, const QString &reactionEventId,
                   const QString &targetEventId, const QString &senderId,
                   const QString &key, qint64 timestampMs, bool targetIsOwn) {
        // An ignored user's reaction in flight never becomes a row.
        if (m_moderation && m_moderation->isIgnored(senderId))
            return;
        const RoomInfo info = m_client->roomInfo(roomId);
        // `targetIsOwn`: the backend confirmed the reacted-to message is ours,
        // so a reaction to a message sent before this launch (or from another
        // device) counts too.
        m_activity->noteReaction(roomId, info.name.isEmpty() ? roomId : info.name,
                                 reactionEventId, targetEventId, senderId,
                                 m_client->displayNameFor(roomId, senderId), key,
                                 timestampMs, targetIsOwn);
    });
    // A thread read here or acknowledged by the server withdraws that
    // thread's cards. Encrypted rooms' unread counters never count thread
    // replies, so the room-level walk cannot be what withdraws them.
    const auto closeThreadCards = [this](const QString &roomId,
                                         const QString &rootEventId) {
        const QString realRoom = MatrixClient::isThreadTimelineId(roomId)
            ? MatrixClient::threadTimelineRoomId(roomId)
            : roomId;
        if (realRoom.isEmpty() || rootEventId.isEmpty())
            return;
        m_notifications->closeThreadNotifications(realRoom, rootEventId);
    };
    connect(m_thread.get(), &ThreadController::threadReadSent, this,
            [closeThreadCards](const QString &roomId, const QString &rootEventId,
                               qint64) { closeThreadCards(roomId, rootEventId); });
    connect(m_client.get(), &MatrixClient::threadReadMarkerAdvanced, this,
            [closeThreadCards](const QString &roomId, const QString &rootEventId,
                               quint64) { closeThreadCards(roomId, rootEventId); });
    // Ignoring someone removes what they already put in the bell, not only
    // what they send next.
    m_activity->setIgnoredCheck([this](const QString &userId) {
        return m_moderation && m_moderation->isIgnored(userId);
    });
    connect(m_client.get(), &MatrixClient::ignoredUsersChanged, this,
            [this](const QStringList &users) {
        for (const QString &userId : users)
            m_activity->dropSender(userId);
        // matrix-sdk clears EVERY room's event cache on an ignore-list change
        // (event_cache/tasks.rs, ignore_user_list_update_task), which empties
        // the open room and thread timelines, and nothing refilled them: the
        // room read "No messages here yet" until it was reopened. Reopen them
        // as Refresh does, a beat later so the SDK's own clear lands first,
        // and only what is still the same room and thread by then.
        if (m_backend != RustBackend)
            return;
        const QString room = m_currentRoomId;
        const QString threadRoom = m_thread ? m_thread->roomId() : QString();
        const QString threadRoot =
            m_thread ? m_thread->rootEventId() : QString();
        // The 250 ms is a bet that the SDK's clear_all_rooms() has landed;
        // it is not ordered with this. The line below says how long it was
        // and whether the open timeline had been emptied by then (items=0),
        // so a recurrence can be read from a log rather than guessed at.
        QElapsedTimer sinceChange;
        sinceChange.start();
        QTimer::singleShot(250, this, [this, room, threadRoom, threadRoot,
                                       sinceChange] {
            const bool reloadRoom =
                !room.isEmpty() && m_currentRoomId == room;
            if (reloadRoom) {
                qCInfo(lcApp) << "ignore-list change: reopening the open room"
                              << matrix::e2ee::redactId(room)
                              << "after_ms=" << sinceChange.elapsed()
                              << "items=" << m_client->timeline(room).size();
                reloadCurrentRoomTimeline();
            }
            // After the room: opening a room timeline drops the thread's.
            const bool reopenThread = m_thread && !threadRoot.isEmpty()
                && m_thread->roomId() == threadRoom
                && m_thread->rootEventId() == threadRoot;
            if (reopenThread)
                m_thread->reopen();
            Q_EMIT timelinesReloadedAfterIgnoreChange(reloadRoom,
                                                      reopenThread);
        });
    });
    // GitHub #48: the classic-sync repair emptied room caches while a view
    // opened; reopen what is open, as About -> Reset local cache does
    // (TimelinePane.qml onEventCacheWasCleared). Rust sends this only after
    // its clear finished, so no delay is needed here.
    connect(m_client.get(), &MatrixClient::eventCacheRepaired, this, [this] {
        if (m_backend != RustBackend)
            return;
        if (!m_currentRoomId.isEmpty())
            reloadCurrentRoomTimeline();
        // After the room: opening a room timeline drops the thread's.
        if (m_thread && m_thread->active())
            m_thread->reopen();
    });
    if (m_moderation) {
        connect(m_moderation.get(), &ModerationController::ignoreActionFinished,
                this, [this](const QString &userId, bool ignored, bool ok,
                             const QString &) {
            if (ignored && ok && !userId.isEmpty())
                m_activity->dropSender(userId);
        });
    }
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
    // A failed seed (a dropped connection, a timeout) is asked again with a
    // bounded backoff, and on the next Syncing edge, instead of leaving the
    // bell without history for the whole session.
    m_activitySeedRetryTimer.setSingleShot(true);
    connect(&m_activitySeedRetryTimer, &QTimer::timeout, this, [this] {
        if (!m_client || m_activitySeeded
            || m_client->connectionState() != MatrixClient::Syncing)
            return;
        m_activitySeeded = true;
        m_client->requestActivitySeed(60);
    });
    connect(m_client.get(), &MatrixClient::activitySeedFailed, this,
            [this](const QString &category) {
        // A server without GET /notifications answers the same way every
        // time: no retry, and no second ask on the next Syncing edge. The
        // panel says history is unavailable (ActivityModel). A restart asks
        // once again.
        if (ActivityModel::seedFailureIsPermanent(category)) {
            qCInfo(lcApp) << "activity seed unsupported by this server; not retrying";
            m_activitySeedRetryTimer.stop();
            return;
        }
        m_activitySeeded = false;
        const int delay =
            notices::activitySeedRetryDelayMs(m_activitySeedAttempts);
        qCInfo(lcApp) << "activity seed failed; retry in ms=" << delay;
        if (delay < 0)
            return;
        ++m_activitySeedAttempts;
        m_activitySeedRetryTimer.start(delay);
    });
    connect(m_client.get(), &MatrixClient::activitySeedReceived, this,
            [this](const QVariantList &entries) {
        m_activitySeedAttempts = 0;
        m_activitySeedRetryTimer.stop();
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
        m_activity->seed(named, true);
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
            // An "index all rooms" pass the last session left unfinished
            // continues; a paused one stays paused. Idempotent: a running
            // pass just answers with its status.
            m_messageSearch->resumeIndexAllIfPending();
            if (!m_searchIndexTimer.isActive())
                m_searchIndexTimer.start();
        } else {
            m_searchIndexTimer.stop();
        }
    });
    connect(m_client.get(), &MatrixClient::storedMediaCleared, this,
            [this](quint64, bool ok, qint64 files, qint64 bytes) {
        Q_EMIT storedMediaCleared(ok, files, bytes);
    });
    connect(m_client.get(), &MatrixClient::mediaKeepStateChanged, this,
            &AppController::mediaKeepStateChanged);
    // New messages become searchable within five minutes.
    m_searchIndexTimer.setInterval(5 * 60 * 1000);
    m_searchIndexTimer.setSingleShot(false);
    connect(&m_searchIndexTimer, &QTimer::timeout, this, [this] {
        if (m_client)
            m_client->sweepSearchIndex();
    });
    // "Index all rooms" steps aside for a call and for a reader scrolling a
    // timeline: the pass waits between pages and loses nothing.
    const auto refreshIndexAllCallHold = [this] {
        using CallState = CallController::State;
        const int oneToOne = m_calls ? m_calls->stateInt()
                                     : static_cast<int>(CallState::Idle);
        const bool inCall =
            (oneToOne != static_cast<int>(CallState::Idle)
             && oneToOne != static_cast<int>(CallState::Ended))
            || (m_groupCall && m_groupCall->active());
        m_messageSearch->setIndexAllHold(MatrixClient::IndexAllHoldCall,
                                         inCall);
    };
    connect(m_calls.get(), &CallController::stateChanged, this,
            refreshIndexAllCallHold);
    connect(m_groupCall.get(), &SfuCallController::stateChanged, this,
            refreshIndexAllCallHold);
    connect(m_timelineScroll.get(),
            &TimelineScrollController::motionActiveChanged, this, [this] {
        m_messageSearch->setIndexAllHold(MatrixClient::IndexAllHoldScroll,
                                         m_timelineScroll->motionActive());
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
    // Nor may a redacted voice message keep playing. Backends that signal the
    // redaction (HTTP, mock) say so here. The Rust SDK never does: it rewrites
    // the event in a timeline instead, so the clip is also re-checked against
    // the open room timeline and the thread timeline whenever either changes
    // (a row rewritten, rows inserted, a reload) and on every room switch.
    // That covers a redaction seen live, one that landed while another room
    // was open, and a clip whose event was already redacted when its room is
    // reopened. A redaction in a room that is open in neither timeline is
    // caught when that room is next opened.
    connect(m_client.get(), &MatrixClient::eventRedacted, this,
            [this](const QString &roomId, const QString &eventId) {
        m_voicePlayback->handleRedaction(roomId, eventId);
        // Nor may a redacted voice message still waiting in the run play.
        if (m_voicePlayback->active() && m_voicePlayback->roomId() == roomId) {
            QVariantList queue = m_voicePlayback->upNext();
            const auto before = queue.size();
            queue.removeIf([&eventId](const QVariant &item) {
                return item.toMap().value(QStringLiteral("eventId"))
                           .toString() == eventId;
            });
            if (queue.size() != before)
                m_voicePlayback->setUpNext(queue);
        }
    });
    // Auto-play next: a voice message that ends on its own starts the next
    // voice message of the same timeline (the room's main timeline, or the
    // thread it was played from), if it directly follows (see
    // TimelineModel::voiceRunAfter). Voice only, never audio files. A clip
    // the reader stopped or paused never gets here. The run comes from the
    // clip's OWN timeline: read live while that timeline is loaded, else from
    // the snapshot the player kept (upNext), so leaving the room does not end
    // the chain (design-media B3; it used to stop after the first clip).
    connect(m_voicePlayback.get(), &VoicePlaybackController::clipFinished, this,
            [this](const QString &eventId, const QString &roomId,
                   const QString &threadRootId, bool isVoice,
                   const QVariantList &upNext) {
        if (!isVoice || m_voicePlayback->active())
            return;
        // A clip that played to its end resets the skip budget below.
        m_voiceAutoplaySkips = 0;
        playNextVoiceMessage(roomId, eventId, threadRootId, upNext);
    });
    // A clip the chain started that cannot be played (fetch refused, invalid
    // media) used to end the run silently, leaving the rest unheard. It is
    // skipped like a row that draws nothing, at most kMaxVoiceAutoplaySkips
    // in a row so a run of broken clips cannot spin; only clips the CHAIN
    // started, never one the reader chose. Queued: the failure can be
    // reported from inside the player's own emission or play() itself.
    connect(m_voicePlayback.get(), &VoicePlaybackController::currentChanged,
            this, [this] {
        // Any change of clip but the chain's own play() is the reader's: a
        // stop, or another clip (or the same one again) chosen. The chain's
        // play() stops the failed clip before starting the next, so that
        // stop must keep the mark it has just set.
        if (m_voiceAutoplayStarting)
            return;
        if (m_voicePlayback->eventId() != m_voiceAutoplayEventId
            || m_voiceAutoplayEventId.isEmpty()) {
            m_voiceAutoplayEventId.clear();
            m_voiceAutoplaySkips = 0;
        }
    });
    connect(m_voicePlayback.get(), &VoicePlaybackController::fetchStateChanged,
            this, [this] {
        const QString failed = m_voicePlayback->eventId();
        if (failed.isEmpty() || failed != m_voiceAutoplayEventId
            || m_voicePlayback->fetchState() != QLatin1String("failed"))
            return;
        const QString room = m_voicePlayback->roomId();
        QMetaObject::invokeMethod(this, [this, failed, room] {
            if (m_voicePlayback->eventId() != failed
                || m_voicePlayback->fetchState() != QLatin1String("failed"))
                return;
            const QString thread = m_voicePlayback->threadRootId();
            const QVariantList queue = m_voicePlayback->upNext();
            if (m_voiceAutoplaySkips >= kMaxVoiceAutoplaySkips) {
                qCInfo(lcApp) << "voice auto-play stopped after"
                              << m_voiceAutoplaySkips << "unplayable clips";
                // The run is over: the clip left failed is the reader's now.
                m_voiceAutoplayEventId.clear();
                return;
            }
            ++m_voiceAutoplaySkips;
            qCInfo(lcApp) << "voice auto-play skipped an unplayable clip";
            playNextVoiceMessage(room, failed, thread, queue);
        }, Qt::QueuedConnection);
    });
    // Keep the run after the playing voice message (its queue, upNext) in
    // step with its own timeline while that is loaded, and prefetch the clip
    // auto-play-next would start, so the chain has no download gap. One at a
    // time: it follows whatever is playing, moves when the clip, the run or
    // the timeline changes, and is dropped on stop. A room switch keeps both:
    // the run plays on from the snapshot.
    // Deferred to the event loop: play() stops the previous clip (emitting
    // currentChanged with nothing loaded) BEFORE it registers interest in the
    // new clip's bytes, so deciding right there would cancel the very prefetch
    // that clip is about to use.
    connect(m_voicePlayback.get(), &VoicePlaybackController::currentChanged,
            this, [this] { scheduleVoicePrefetchUpdate(); });
    connect(m_timeline.get(), &QAbstractItemModel::rowsInserted, this,
            [this] { scheduleVoicePrefetchUpdate(); });
    connect(m_timeline.get(), &QAbstractItemModel::modelReset, this,
            [this] { scheduleVoicePrefetchUpdate(); });
    connect(m_thread->model(), &QAbstractItemModel::rowsInserted, this,
            [this] { scheduleVoicePrefetchUpdate(); });
    connect(m_thread->model(), &QAbstractItemModel::modelReset, this,
            [this] { scheduleVoicePrefetchUpdate(); });
    connect(m_voicePlayback.get(), &VoicePlaybackController::upNextChanged,
            this, [this] { scheduleVoicePrefetchUpdate(); });
    const auto checkVoiceRedaction = [this] {
        if (!m_voicePlayback->active())
            return;
        const QString eventId = m_voicePlayback->eventId();
        const QString roomId = m_voicePlayback->roomId();
        if (m_timeline->isEventRedacted(roomId, eventId)
            || m_thread->model()->isEventRedacted(roomId, eventId))
            m_voicePlayback->handleRedaction(roomId, eventId);
    };
    for (TimelineModel *model : { m_timeline.get(), m_thread->model() }) {
        connect(model, &QAbstractItemModel::dataChanged, this,
                [checkVoiceRedaction](const QModelIndex &, const QModelIndex &,
                                      const QList<int> &roles) {
            if (roles.isEmpty() || roles.contains(TimelineModel::RedactedRole))
                checkVoiceRedaction();
        });
        connect(model, &QAbstractItemModel::rowsInserted, this,
                checkVoiceRedaction);
        connect(model, &QAbstractItemModel::modelReset, this,
                checkVoiceRedaction);
    }
    connect(this, &AppController::currentRoomIdChanged, this,
            checkVoiceRedaction);
    // A call going live pauses the clip, once, on the edge into the call: a
    // clip the user resumes mid-call is not paused again by the call's later
    // state changes. Not stopped, so its row or the player bar can resume it;
    // and on the edge OUT of the call, a clip the call paused and nobody
    // touched since resumes by itself, as in Discord and Telegram (the bar
    // says "Paused for call" meanwhile).
    const auto voiceCallWasLive = std::make_shared<bool>(false);
    const auto pauseVoiceForCall = [this, voiceCallWasLive] {
        using CallState = CallController::State;
        const auto oneToOne = m_calls->state();
        const bool callLive = oneToOne == CallState::Inviting
            || oneToOne == CallState::Connecting
            || oneToOne == CallState::Active
            || m_groupCall->active();
        if (callLive && !*voiceCallWasLive)
            m_voicePlayback->pauseForCall();
        else if (!callLive && *voiceCallWasLive)
            m_voicePlayback->resumeAfterCall();
        *voiceCallWasLive = callLive;
    };
    connect(m_calls.get(), &CallController::stateChanged, this,
            pauseVoiceForCall);
    connect(m_groupCall.get(), &SfuCallController::stateChanged, this,
            pauseVoiceForCall);
    m_thread->setClient(m_client.get());
    m_eventContext->setClient(m_client.get());
    m_thread->setDraftStore(m_draftStore.get());
    m_draftStore->setClient(m_client.get());
    m_roomList->setClient(m_client.get());
    m_roomList->setSpaceManager(m_spaces.get());
    // Muted rooms are silent in the room list's badges too (one rule with the
    // rail and the tray).
    m_roomList->setNotificationModeSource([this](const QString &id) {
        return m_settings->roomUnreadMode(id);
    });
    // Every rollup follows the same rule: the Space badges, Home and People
    // totals and the lobby's counts.
    m_spaces->setNotificationModeSource([this](const QString &id) {
        return m_settings->roomUnreadMode(id);
    });
    m_quickSwitcher->setNotificationModeSource([this](const QString &id) {
        return m_settings->roomUnreadMode(id);
    });
    connect(m_settings.get(), &SettingsManager::roomNotificationModeChanged,
            m_spaces.get(),
            [this](const QString &) { m_spaces->notificationModesChanged(); });
    connect(m_settings.get(), &SettingsManager::roomNotificationModeChanged,
            m_activity.get(),
            [this](const QString &) { m_activity->notificationModesChanged(); });
    connect(m_settings.get(), &SettingsManager::roomNotificationModeChanged,
            m_roomList.get(),
            [this](const QString &) { m_roomList->notificationModeChanged(); });
    // No setSpaceManager: this is the unfiltered list the forward pickers use.
    m_allRooms->setClient(m_client.get());
    // Rail rows: the user's arrangement over the hierarchy, with the drag
    // preview in the model rather than QML.
    m_railEntries->setSources(m_spaces.get(), m_railLayout.get());
    m_railEntries->setRoomSources(m_client.get(), m_settings.get());
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
    // A-Z / Recent activity sort a copy of the arrangement at read time, so
    // Manual restores the dragged order untouched.
    const auto applyRailSort = [this] {
        m_railLayout->setSortMode(m_settings->railSort());
    };
    connect(m_settings.get(), &SettingsManager::railSortChanged, this,
            applyRailSort);
    applyRailSort();
    // The Channels layout is global: it needs the account's rooms and the
    // rail order, not the selected Space.
    m_spaceChannels = std::make_unique<SpaceChannelModel>(this);
    m_spaceChannels->setSources(m_client.get(), m_spaces.get(),
                                m_railLayout.get());
    m_spaceChannels->setSettings(m_settings.get());
    m_spaceChannels->setDirectAvatarResolver(m_directAvatars.get());
    // Hold the room order still while the list is in use (a setting, on by
    // default). Both layouts follow it, so switching layout keeps the rule.
    const auto applyRoomOrderHold = [this] {
        const bool still = m_settings->keepRoomListOrderStill();
        m_roomList->setDeferReordering(still);
        m_spaceChannels->setDeferReordering(still);
    };
    connect(m_settings.get(), &SettingsManager::keepRoomListOrderStillChanged,
            this, applyRoomOrderHold);
    applyRoomOrderHold();
    // The sort mode (Activity / A-Z) reaches both layouts the same way.
    const auto applyRoomListSort = [this] {
        // -1 until the user picks a sort (C04): Space order in a Space.
        const int mode = m_settings->roomListSortForModels();
        m_roomList->setSortMode(mode);
        m_spaceChannels->setSortMode(mode);
    };
    connect(m_settings.get(), &SettingsManager::roomListSortChanged, this,
            applyRoomListSort);
    applyRoomListSort();
    m_quickSwitcher->setClient(m_client.get());
    m_quickSwitcher->setSpaceManager(m_spaces.get());
    m_timeline->setClient(m_client.get());
    m_timeline->setProfileResolver(m_userProfiles.get());
    m_composer->setClient(m_client.get());
    m_mentionSuggestions->setClient(m_client.get());
    m_banners->setClient(m_client.get());
    m_backdrops->setClient(m_client.get());
    m_settingsSync->setClient(m_client.get());
    // Shared pictures are fetched and measured through the media bridge only;
    // personal ones are served from memory via the staged-image store.
    m_backdrops->setMediaBridge(m_mediaBridge.get());
    m_backdrops->setSpaces(m_spaces.get());
    m_backdrops->setStagedImages(&m_stagedImages);
    // Only pictures the backdrop controller staged itself: the store is shared
    // with composer/thread attachments and the cropper, whose bytes stay
    // memory-only and must never reach a scratch file.
    m_mediaBridge->setStagedBytesProvider([this](const QString &token) {
        return m_backdrops && m_backdrops->ownsStagedToken(token)
            ? m_stagedImages.bytes(token) : QByteArray();
    });
    m_nameColors->setClient(m_client.get());
    m_bio->setClient(m_client.get());
    m_userProfiles->setClient(m_client.get());
    m_media->setClient(m_client.get());
    m_conversations->setClient(m_client.get());
    m_discovery->setClient(m_client.get());
    m_messageSearch->setClient(m_client.get());
    m_messageSearch->setSettings(m_settings.get());
    m_messageSearch->setProfileResolver(m_userProfiles.get());
    m_uia->setClient(m_client.get());
    m_passwordChange->setClient(m_client.get());
    m_roomKeyExport->setClient(m_client.get());
    m_roomExport->setClient(m_client.get());
    m_moderation->setClient(m_client.get());
    m_pushRules->setClient(m_client.get());
    m_forward->setClient(m_client.get());
    m_bulkRedaction->setClient(m_client.get());
    m_forward->setMediaBridge(m_mediaBridge.get());
    m_roomInfo->setClient(m_client.get());
    m_roomBatch->configure(m_client.get(), m_roomList.get(), m_roomInfo.get(),
                           [this](const QString &id, int mode) {
        setRoomNotificationMode(id, mode);
    }, [this](const QString &id) {
        return m_settings->roomNotificationMode(id);
    });
    connect(m_client.get(), &MatrixClient::loginSucceeded, m_roomBatch.get(),
            [this](const QString &id) { m_roomBatch->setAccount(id); });
    connect(this, &AppController::accountSwitchingChanged, m_roomBatch.get(),
            [this] { if (m_accountSwitching) m_roomBatch->setAccount({}); });
    m_spaceModeration->setClient(m_client.get());
    m_roomClosure->setClient(m_client.get());
    m_mediaBridge->setClient(m_client.get());
    m_pagination->setClient(m_client.get());
    m_pagination->setTimelineModel(m_timeline.get());
    // Routing: a jump target that is not loaded opens the context view; if
    // /context fails the old history walk runs as the fallback.
    m_pagination->setContextOpener([this](const QString &eventId) {
        return m_eventContext->open(m_currentRoomId, eventId);
    });
    m_pagination->setContextCloser([this] { m_eventContext->close(); });
    connect(m_eventContext.get(), &ContextController::openFailed, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &) {
        if (roomId == m_currentRoomId && m_pagination)
            m_pagination->walkToEvent(eventId);
    });
    connect(m_eventContext.get(), &ContextController::threadReplyHit, this,
            [this](const QString &roomId, const QString &rootId,
                   const QString &eventId) {
        if (roomId != m_currentRoomId || !m_thread)
            return;
        // Opens (or reuses) the thread and lands on the reply once it is
        // Ready, after the panel's own scroll restore; dropped if the thread
        // fails, closes or is replaced first.
        m_thread->openThreadAt(roomId, rootId, eventId);
    });
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
    // The same for a thread's own receipt, and for a room marked read without
    // a timeline (room menu, notification action, Mark space read).
    connect(m_thread.get(), &ThreadController::threadReadSent, this,
            [this](const QString &roomId, const QString &rootId, qint64 ms) {
        if (m_activity)
            m_activity->markRoomReadUpTo(roomId, ms, rootId);
    });
    connect(m_roomList.get(), &RoomListModel::roomMarkedRead, this,
            [this](const QString &roomId) {
        if (m_activity)
            m_activity->markRoomReadNow(roomId);
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
        if (m_eventContext) {
            m_eventContext->model()->setShowRoomActivity(shown);
            m_eventContext->model()->setShowMembershipEvents(members);
            m_eventContext->model()->setShowProfileChangeEvents(profiles);
        }
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
    m_linkPreviews->setInlineMedia(m_settings->showLinkMediaInline());
    connect(m_settings.get(), &SettingsManager::showLinkMediaInlineChanged,
            this, [this]() {
        m_linkPreviews->setInlineMedia(m_settings->showLinkMediaInline());
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

    // The read-receipt coordinator needs the MAIN window's own activation
    // and exposure; QML reports only timeline visibility and scroll position.
    // Application activation is not that: the picture-in-picture and the
    // full-screen call windows are top-level windows of this application,
    // and a focused one would mark the hidden room read. The main window is
    // found when it first shows up (or registered by QML), and until then the
    // application state stands in.
    if (auto *guiApp =
            qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        refreshMainWindowState();
        refreshAnimationsSuspended();
        connect(guiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    refreshMainWindowState();
                    refreshAnimationsSuspended();
                    // Coming back to the app: the own profile may have been
                    // changed on another device meanwhile (refreshOwnProfile).
                    if (state == Qt::ApplicationActive)
                        refreshOwnProfile(kOwnProfileActivationRefreshMs);
                });
        connect(guiApp, &QGuiApplication::focusWindowChanged, this,
                [this](QWindow *) { refreshMainWindowState(); });
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
    // Opening it is the create's own landing, so a message it reported on
    // the way ("created, but adding it to the Space failed") stays.
    connect(m_conversations.get(), &ConversationController::conversationReady,
            this, [this](const QString &roomId) {
        const QScopedValueRollback keep(m_statusErrorOwnNavigation, true);
        openRoom(roomId);
    });
    // A created Space is selected, never opened as a timeline; clearing the
    // current room shows the Space Home.
    connect(m_conversations.get(), &ConversationController::spaceReady,
            this, [this](const QString &spaceId) {
        const QScopedValueRollback keep(m_statusErrorOwnNavigation, true);
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
    // "Sign out of all other devices" removes sessions the list still shows.
    connect(m_passwordChange.get(), &PasswordChangeController::finished, this,
            [this](bool ok, const QString &) {
                if (ok)
                    refreshSessionDevices();
            });
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
    // Whether the server would accept a call membership is read by
    // RtcController from every member snapshot itself (see setCurrentRoom).
    // It is not pushed from the room info panel: that reached only rooms the
    // panel had shown, and its reset wrote "permitted" over a known refusal.
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
            this, &AppController::closeCurrentRoomNoLongerIn);

    connect(m_auth.get(), &AuthManager::loginSucceeded,
            this, &AppController::onLoginSucceeded);
    connect(m_auth.get(), &AuthManager::loggedOut,
            this, &AppController::onLoggedOut);
    // LIGHTNING_SWITCH_TRACE probe: every loggedIn transition QML can see,
    // so a switch that drops to the login screen for a frame shows up.
    // SwitchTrace ignores a repeat of the last value.
    if (lightning::switchtrace::enabled()) {
        connect(this, &AppController::loggedInChanged, this, [this] {
            lightning::switchtrace::noteLoggedIn(loggedIn());
        });
    }
    // A failed switch falls back to the previous account once, else the login
    // screen. A failed add-account restores the previous account in the
    // background, since the attempt released the shared client's session.
    connect(m_auth.get(), &AuthManager::lastErrorChanged, this,
            &AppController::signInErrorExplainedByCardChanged);
    // A sign-in the user starts (password, browser, add-account) ends any
    // claim the return-to restore had on the next failure: that failure is
    // the attempt's, and gets the normal handling below (the return restore
    // again), never the "return restore failed, do not retry" branch.
    connect(m_auth.get(), &AuthManager::isLoggingInChanged, this, [this] {
        if (m_auth->isLoggingIn())
            m_backgroundRestore = false;
    });
    connect(m_auth.get(), &AuthManager::loginFailed, this,
            [this](const QString &reason) {
        ++m_loginFailures;
        // Did this failure raise the repair card? Then its text is the card's
        // and the login screen keeps it behind the card; otherwise (the
        // "already signed in here" redirect, a refused address) it is shown
        // beside it. First, before anything below can fail again inside this
        // call and answer for itself.
        m_cardExplainedError =
            std::exchange(m_localSessionRaiseInFlight, false) ? reason : QString{};
        Q_EMIT signInErrorExplainedByCardChanged();
        // showMain() cancelling a browser sign-in left behind: it restores the
        // account to return to itself, so this must not start a second one.
        if (m_abandoningBrowserSignIn)
            return;
        // A traced switch or launch restore ends here, whichever branch
        // follows (a fallback restore is not part of it). A failure after the
        // traced login (an add-account attempt) is not that switch's.
        if (lightning::switchtrace::awaitingLogin())
            lightning::switchtrace::finish(lightning::switchtrace::Outcome::Failed);
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
        if (m_backgroundRestore) {
            // The return-to restore below failed, synchronously (re-entering
            // this handler from inside its own restoreSession()) or later.
            // Starting it again would fail the same way: a keyring that
            // cannot answer or a store that cannot open recursed until the
            // stack ran out, and a later failure looped for ever. The card it
            // raised, or its error, stays on the sign-in screen.
            qCWarning(lcApp) << "add-account return restore failed — not "
                                "retrying it";
            m_backgroundRestore = false;
        } else if (!m_addAccountReturnTo.isEmpty() && !m_client->isLoggedIn()
            && m_settings->hasSavedAccount(m_addAccountReturnTo)) {
            qCInfo(lcApp) << "add-account attempt failed — restoring"
                          << "slug=" << matrix::app_data::safeUserSlug(
                                 m_addAccountReturnTo);
            // The background restore below finishes almost immediately and
            // its own loginSucceeded unconditionally clears
            // AuthManager::lastError (the same as any successful login), so
            // the login screen's error label is blank again within a beat —
            // onLoginSucceeded() deliberately keeps the login screen open
            // for this exact case, but leaves nothing on it to read. This
            // (e.g. "login redirected to switch": the account just typed is
            // already signed in here) is the one channel that survives that
            // return, since onLoginSucceeded() only clears it on the normal
            // exit path, not the background-restore one.
            if (!reason.isEmpty())
                Q_EMIT errorReported(reason);
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
    // Sync does not carry the global profile, so a name or picture changed on
    // another device never reached the account menu or Settings > Account
    // until the next sign-in. The profile change does show up as our own
    // m.room.member event in each room (in the open timeline here); that is
    // the cue to ask /profile again, as Element's OwnProfileStore does.
    connect(m_client.get(), &MatrixClient::eventAppended, this,
            [this](const QString &roomId, const TimelineEvent &event) {
        if (event.stateKind != QLatin1String("member_profile")
            || MatrixClient::isThreadTimelineId(roomId) || !m_client
            || event.sender.isEmpty()
            || event.sender != m_client->currentUserId())
            return;
        refreshOwnProfile(kOwnProfileEventRefreshMs);
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
    // A plain sign-out bypasses clearCrossAccountCaches(): the download
    // notices (some with a risky-file warning) are the old account's.
    connect(m_client.get(), &MatrixClient::loggedOut, this, [this] {
        if (m_downloads)
            m_downloads->clear();
    });
    connect(m_client.get(), &MatrixClient::errorOccurred,
            this, &AppController::errorReported);
    // The server confirmed we are out of a room (left elsewhere, kicked,
    // banned; rust/src/membership_lost.rs). The open room closes now, with no
    // invite grace: this is the server's word, not a re-sort. The notice comes
    // after the close, which clears the strip.
    connect(m_client.get(), &MatrixClient::roomMembershipLost, this,
            [this](const QString &roomId, const QString &loss,
                   const QString &roomName) {
                const bool wasOpen = !roomId.isEmpty() && roomId == m_currentRoomId;
                if (wasOpen)
                    closeCurrentRoomNoLongerIn(roomId);
                else if (m_voicePlayback->active()
                         && m_voicePlayback->roomId() == roomId)
                    m_voicePlayback->stop();
                const QString notice =
                    lightning::rooms::membershipLostNotice(loss, roomName, wasOpen);
                if (!notice.isEmpty())
                    Q_EMIT errorReported(notice);
            });
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
    // A rule written elsewhere (a room muted in Element) updates the local
    // modes without a restart. Read-only; own writes in flight are skipped.
    connect(m_client.get(), &MatrixClient::pushRulesChanged, this,
            [this] { refreshStoredNotificationModes(); });
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, this, [this, refreshConnectionStatus] {
        refreshConnectionStatus();
        Q_EMIT initialSyncDoneChanged();
        // After the first sync, when the account's push rules are known.
        if (m_client->initialSyncDone()) {
            refreshStoredNotificationModes();
            if (m_retryNotificationModesWhenSynced)
                retryFailedNotificationModes();
        }
    });
    connect(m_client.get(), &MatrixClient::syncModeChanged,
            this, &AppController::syncModeChanged);
    // The status text is stored already translated, so a language change
    // would leave it in the old language until the next state edge. QML
    // compares it against qsTr() of the same words, so both must move.
    if (m_localization)
        connect(m_localization.get(), &LocalizationManager::languageChanged,
                m_client.get(), refreshConnectionStatus);
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
            // A request card stays only while the request does: once the
            // flow leaves "requested" (accepted, cancelled or finished, here
            // or on another device) or another flow replaces it, it goes.
            const QString wantedKey =
                (m_verificationState == QLatin1String("requested")
                 && !m_verificationFlowId.isEmpty())
                ? notices::verificationNoticeKey(m_verificationFlowId)
                : QString();
            if (!m_verificationNoticeKey.isEmpty()
                && m_verificationNoticeKey != wantedKey) {
                m_notifications->withdrawGeneric(m_verificationNoticeKey);
                m_verificationNoticeKey.clear();
            }
            // Surface incoming verification requests natively (identity only,
            // never message content or SAS data), once per flow.
            if (!wantedKey.isEmpty()
                && m_verificationFlowId != m_verificationNotifiedFlow) {
                m_verificationNotifiedFlow = m_verificationFlowId;
                if (notices::verificationNoticeAllowed(
                        m_settings->notificationsEnabled(),
                        m_pushRules && m_pushRules->loaded(),
                        m_pushRules && m_pushRules->masterMute())) {
                    // A display name or localpart, never the full MXID: the
                    // body sits in the desktop's history in plaintext. No
                    // room, so the stricter preview level applies.
                    const QString other = m_verificationIsSelf
                        ? QString()
                        : CallController::presentableCallerName(
                              m_verificationOtherUser, QString());
                    m_verificationNoticeKey = wantedKey;
                    m_notifications->showGeneric(
                        tr("Verification request"),
                        notices::verificationBody(noticePreviewFor(QString()),
                                                  m_verificationIsSelf, other),
                        QString(), QString(), /*sound=*/false, wantedKey);
                }
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
            m_verificationRole.clear();
            m_verificationCancelReason.clear();
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
            m_verificationRole = verificationRoleAtStart(isSelf);
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
                this, [this](const QString &flowId, const QString &message) {
            if (flowId != m_verificationFlowId) return;
            if (m_verificationState == QLatin1String("done")) return;
            // Why, so the card can say it: a mismatch is a security warning,
            // not a plain "cancelled". The SDK's own word, never ours.
            m_verificationCancelReason = verificationCancelReasonFor(message);
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
            m_verificationRole = verificationRoleAtStart(isSelf);
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
            // Only the SDK's definitive answer carries these codes, never a
            // network error: revoked (hard) or expired (soft, the device
            // kept). The switcher must not show the account as healthy,
            // whether or not it is the one running.
            if (reasonCode == QLatin1String("access_token_revoked")
                || reasonCode == QLatin1String("access_token_expired")) {
                m_accounts->markSessionRevoked(userId);
                // A revoked credential while a session runs: the restore
                // reached the main screen first (it reads only the store), so
                // the failed-startup path never sees this. Queued: this runs
                // inside the client's event drain, and ending the session
                // releases the handle that drain is reading.
                if (m_client->isLoggedIn()) {
                    QMetaObject::invokeMethod(this, [this, userId] {
                        endRevokedSession(userId);
                    }, Qt::QueuedConnection);
                }
            } else if (reasonCode == QLatin1String("secret_backend_unavailable")
                       && m_client->isLoggedIn()
                       && userId == m_client->currentUserId()) {
                // The RUNNING session lost its credential without the server
                // refusing anything (it never held its refresh token): ended
                // here, locally and without marking the account revoked, so
                // the card's "Try again" restores it with the keyring's.
                // endRevokedSession() ends only that account's session; a
                // restore of ANOTHER account refused this way leaves the
                // running one alone.
                QMetaObject::invokeMethod(this, [this, userId] {
                    endRevokedSession(userId);
                }, Qt::QueuedConnection);
            }
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

    // Concurrent live accounts (Labs; lead decision 2026-10-20): offered in
    // Settings > Labs and OFF by default; the stored labsConcurrentAccounts
    // alone turns it on (with the Rust bridge built in). While it is off,
    // applyLiveAccountsLabs() returns before asking anything and nothing is
    // constructed, so the rest of this constructor is today's.
    // LIGHTNING_LABS_LIVE_ACCOUNTS is a test override read once here: set to
    // 0/false/off/no (or empty) it forces Labs off and hides the card;
    // unset or any other value changes nothing.
    m_liveAccountsLabsAvailable =
        !qEnvironmentVariableIsSet("LIGHTNING_LABS_LIVE_ACCOUNTS")
        || lightning::switchtrace::enabledFromEnvValue(
            qgetenv("LIGHTNING_LABS_LIVE_ACCOUNTS"));
    if (m_liveAccountsLabsAvailable) {
        connect(m_settings.get(),
                &SettingsManager::labsConcurrentAccountsChanged, this,
                [this] { applyLiveAccountsLabs(); });
        applyLiveAccountsLabs();
    } else {
        qCInfo(lcApp) << "live accounts (Labs) forced off by LIGHTNING_LABS_LIVE_ACCOUNTS";
    }

    // Startup with a saved account is a restoration state: the login form
    // must not instantiate until the outcome is known. Success reaches
    // MainScreen via loginSucceeded, every failure via loginFailed. The mock
    // restores from its account registry. Screenshot-demo mode runs its own
    // restore from beginScreenshotDemo, so this one must not race it.
    const bool sessionReadable = !m_screenshotDemo && m_settings->hasSession();
    // Headed for the login screen: a saved sign-in the keyring cannot answer
    // for, or no longer returns, must not look signed out there, so the
    // screen says which and offers a way on. Asked only then, and from the
    // read hasSession() already made, so a launch reads no more of the keyring
    // than before. Re-checked when an account is added or removed, but only
    // while the notice shows.
    if (!m_screenshotDemo && !sessionReadable)
        explainUnrestoredLaunch();
    connect(m_settings.get(), &SettingsManager::accountsChanged, this, [this] {
        if (m_keyringUnavailable)
            refreshKeyringUnavailable();
    });
    const bool hasRestorableSession = !m_screenshotDemo
        && (sessionReadable
            || (m_backend == MockBackend
                && !m_settings->activeAccountUserId().isEmpty()));
    if (hasRestorableSession) {
        setCurrentScreen(BootScreen);
        // LIGHTNING_SWITCH_TRACE: the launch restore is traced like a switch
        // from no account, so a cold start has a baseline too.
        if (lightning::switchtrace::enabled()) {
            lightning::switchtrace::begin(QString(),
                                          m_settings->activeAccountUserId(),
                                          lightning::switchtrace::Path::Startup);
        }
        if (!m_client->restoreSession()) {
            lightning::switchtrace::finish(
                lightning::switchtrace::Outcome::Failed);
            setCurrentScreen(LoginScreen);
        }
        Q_EMIT rustDeviceIdChanged();
    }
}

void AppController::scheduleVoicePrefetchUpdate()
{
    if (m_voicePrefetchQueued)
        return;
    m_voicePrefetchQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_voicePrefetchQueued = false;
        updateVoicePrefetch();
    }, Qt::QueuedConnection);
}

void AppController::resetVoicePrefetch()
{
    // Sign-out / account switch: the bridge's own tables are wiped, so there
    // is nothing to cancel, and a key wanted again afterwards must re-issue.
    m_voicePrefetchKey.clear();
    m_voicePrefetchOwned = false;
}

void AppController::updateVoicePrefetch()
{
    QString wanted;
    QVariantMap next;
    if (m_voicePlayback->active() && m_voicePlayback->isVoice()) {
        // The run as the clip's own timeline shows it now, while that is
        // loaded; otherwise the snapshot stands.
        bool known = false;
        const QVariantList run = liveVoiceRun(m_voicePlayback->roomId(),
                                              m_voicePlayback->eventId(),
                                              m_voicePlayback->threadRootId(),
                                              &known);
        if (known)
            m_voicePlayback->setUpNext(run);
        const QVariantList queue = m_voicePlayback->upNext();
        if (!queue.isEmpty()) {
            next = queue.constFirst().toMap();
            wanted = next.value(QStringLiteral("mediaKey")).toString();
        }
    }
    if (wanted == m_voicePrefetchKey)
        return;
    const QString old = m_voicePrefetchKey;
    const bool ownedOld = m_voicePrefetchOwned;
    m_voicePrefetchKey = wanted;
    m_voicePrefetchOwned = false;
    // The prefetched clip may be the one playing now: its Play owns the fetch
    // from here, so it is never cancelled out from under it.
    if (!old.isEmpty() && old != m_voicePlayback->mediaKey()) {
        ++m_voicePrefetchDrops;
        // Only interest this controller took: a row's own prefetch of the same
        // key (AudioPlayerCard) is not ours to drop.
        if (ownedOld && m_mediaBridge)
            m_mediaBridge->cancelPrefetch(old);
    }
    // "Preload videos and audio" off means no speculative downloads.
    if (wanted.isEmpty() || !m_mediaBridge || !m_mediaBridge->supported()
        || !m_settings->videoPrefetchEnabled())
        return;
    double size = next.value(QStringLiteral("fileSize")).toDouble();
    if (size <= 0)
        size = m_settings->knownMediaSizeBytes(wanted);
    const bool alreadyPrefetching = m_mediaBridge->hasPrefetch(wanted);
    m_mediaBridge->prefetchPlayable(wanted, size);
    m_voicePrefetchOwned =
        !alreadyPrefetching && m_mediaBridge->hasPrefetch(wanted);
}

void AppController::refreshAnimationsSuspended()
{
    // Application state, not the main window's: a focused call or
    // picture-in-picture window is still Lightning in front of the user.
    const bool suspended =
        QGuiApplication::applicationState() != Qt::ApplicationActive;
    if (suspended == m_animationsSuspended)
        return;
    m_animationsSuspended = suspended;
    Q_EMIT animationsSuspendedChanged();
}

AppController::~AppController()
{
    // Live accounts (Labs): the pool may be destroyed only after its hosts
    // were retired and awaited (SessionPool's contract), and it points at
    // members destroyed before m_client. A quit has done both already
    // (prepareForShutdown); a controller torn down without one (tests) does
    // them here, while everything the pool points at is alive.
    if (m_sessionPool) {
        m_sessionPool->disconnect(this);
        if (!m_liveAccountsQuitBegun) {
            m_liveAccountsQuitBegun = true;
            m_liveAccountsQuitTimer.start();
            m_sessionPool->beginQuit();
        }
        const qint64 remaining = std::max<qint64>(
            0, m_liveAccountsQuitBudgetMs - m_liveAccountsQuitTimer.elapsed());
        m_sessionPool->waitForQuit(int(remaining));
        m_parkedOpenTimer.reset();
        // A runtime off finishing inside that wait has released it already
        // (and, m_liveAccountsQuitBegun being set, started nothing again).
        m_sessionPool.reset();
        m_accountList.reset();
        m_accountActivity.reset();
    }
    // What a runtime off released with deleteLater(): a quit on the same
    // event-loop turn never ran it, and ~QObject would delete them only
    // after the settings, the notifications and the accounts they point at.
    // The pool first, as it points at the activity model.
    delete m_retiredLivePool.data();
    delete m_retiredAccountList.data();
    delete m_retiredAccountActivity.data();
    // Members die in reverse declaration order. ~SfuCallController always
    // emits stateChanged (Idle -> Ended, even with no call), and our lambdas
    // on it touch m_messageSearch and m_voicePlayback, already destroyed: a
    // use-after-free at exit (timeline-pane-qml, 2026-10-07). m_rtc's lambdas
    // read members destroyed before it too. Cut these senders off first.
    if (m_groupCall)
        m_groupCall->disconnect(this);
    if (m_calls)
        m_calls->disconnect(this);
    if (m_timelineScroll)
        m_timelineScroll->disconnect(this);
    if (m_rtc)
        m_rtc->disconnect(this);
}

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

void AppController::applyKeepMediaOnDevice()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    RustSdkMatrixClient::setKeepMediaOnDevice(m_settings->keepMediaOnDevice());
#endif
}

QString AppController::mediaKeepState() const
{
    return m_client ? m_client->mediaKeepState() : QString();
}

bool AppController::clearStoredMedia()
{
    return m_client && m_client->clearStoredMedia() != 0;
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

void AppController::showThread(const QString &roomId,
                               const QString &rootEventId)
{
    if (roomId.isEmpty() || rootEventId.isEmpty())
        return;
    // Every entry point opens the SDK-backed thread session (ThreadController:
    // TimelineFocus::Thread history, its own drafts, thread receipts), in both
    // thread styles. Where it is shown -- beside the room or in the main
    // column -- is SettingsManager::threadPlacement, a presentation choice the
    // conversation view applies. Keep any room timeline filter intact so
    // closing the independently loaded thread returns to the same room view.
    m_thread->openThread(roomId, rootEventId);
}

void AppController::applyReceiptThreading()
{
    // Unthreaded only while the room is shown whole with its thread replies:
    // that is the one view in which the reader saw everything a receipt at the
    // newest row covers (TimelineModel::latestReadableEventId picks the row).
    if (!m_client || !m_timeline)
        return;
    m_client->setUnthreadedReadReceipts(
        m_timeline->threadRepliesInline()
        && m_timeline->threadFilterRootId().isEmpty()
        && !m_timeline->hideThreadReplies());
}

void AppController::applyThreadStyle(bool reopen)
{
    const bool inlineThreads =
        m_settings
        && m_settings->threadStyle() == SettingsManager::kThreadStyleInline;
    const bool changed =
        m_timeline && m_timeline->threadRepliesInline() != inlineThreads;
    if (m_client)
        m_client->setThreadRepliesInLiveTimeline(inlineThreads);
    if (m_timeline)
        m_timeline->setThreadRepliesInline(inlineThreads);
    applyReceiptThreading();
    if (!reopen || !changed || !m_client || m_currentRoomId.isEmpty())
        return;
#ifdef ENABLE_RUST_SDK_BACKEND
    if (m_backend == RustBackend) {
        // A fresh SDK timeline with the new focus (hide_threaded_events): the
        // reader changed what the room timeline contains, so this is the one
        // rebuild a setting may ask for (never a decryption retry).
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
            rust->openRoomTimeline(m_currentRoomId);
        return;
    }
#endif
    // Backends without a persistent SDK timeline answer timeline() anew.
    m_timeline->reloadFromBackend();
}

void AppController::prepareForShutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;

    // Every step below runs synchronously on the GUI thread while the window
    // is still closing, and each is independently bounded further down (the
    // call engine's own drain, the Rust sync/bootstrap thread join). A 14 s
    // quit after using the tray was reported once and never reproduced;
    // nothing here recorded which step, if any, actually ran long, so a
    // repeat would be just as unattributable. Timed and traced the same way
    // src/media/MediaBridge.cpp instruments its own heavy sections
    // (stalltrace::Scope), so a live capture (LIGHTNING_GUI_STALL_TRACE=1)
    // or this log line can tell which phase to look at next time.
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    qCInfo(lcApp) << "quit: prepareForShutdown begin";
    if (QQuickWindow *window = mainWindow())
        window->hide();

    // Live accounts (Labs): every background account starts retiring now,
    // on its own pool, so it retires in parallel with the foreground's stop
    // below; both share one budget (waitForQuit after the stop).
    if (m_sessionPool) {
        m_liveAccountsQuitBegun = true;
        m_liveAccountsQuitTimer.start();
        // The foreground was live until now: its backlog floor next time.
        const QString foreground = m_settings->activeAccountUserId();
        if (!foreground.isEmpty() && m_client && m_client->isLoggedIn())
            m_settings->setLastLiveAtFor(foreground,
                                         QDateTime::currentMSecsSinceEpoch());
        noteLiveAccountsStep(QStringLiteral("quit:begin"), QString());
        m_sessionPool->beginQuit();
    }

    // Stop the Qt Multimedia players first: on Windows their Media Foundation
    // threads deliver state through queued signals that must not run into
    // teardown.
    if (m_playback) {
        stalltrace::Scope stallScope("quit-stop-playback");
        if (m_voicePlayback)
            m_voicePlayback->stop();
        m_playback->stopAll();
    }

    // Leave the call before stopSync(): the membership retraction and the SFU
    // Leave both need the client, and otherwise we linger in the call as a
    // ghost participant. Not left to ~SfuCallController, whose timing depends
    // on member destruction order. leave() is safe in any state, including
    // mid-join; the client's bounded Rust task join gives the sends time to
    // land. teardown() also drains the media engine (webrtcbin/ICE), which
    // docs/open-items.md records as bounded at up to 2 s on its own.
    if (m_groupCall) {
        stalltrace::Scope stallScope("quit-leave-call");
        m_groupCall->leave();
    }

    // Take the "Playing X" / "Listening to" line back while the client can
    // still send: a status_msg outlives the session on the server. Both
    // overlays go in ONE publish (two would race on separate backend tasks,
    // and rc_presence refuses the second), then the pollers stop without
    // publishing again. Best effort with a bounded wait for the answer (one
    // retry on the server's retry_after if it fits); after a crash or a forced
    // close the line stays until the next start publishes.
    const bool farewell = m_presence && m_presence->withdrawActivityLine();
    if (m_gameActivity)
        m_gameActivity->shutdown();
    if (m_musicActivity)
        m_musicActivity->shutdown();
    if (farewell) {
        stalltrace::Scope stallScope("quit-presence-farewell");
        m_presence->settleFarewell(PresenceManager::kFarewellBudgetMs);
    }

    // Stop the sync loop so no further backend callback is scheduled during
    // teardown. Blocking: the Rust side joins its sync (and, on the first
    // account of a session, bootstrap) thread within a bounded budget
    // (SYNC_TASK_JOIN_BUDGET_MS x 2, rust/src/lib.rs) before detaching it.
    if (m_client) {
        stalltrace::Scope stallScope("quit-stop-sync");
        m_client->stopSync();
    }

    // Live accounts (Labs): what is left of the one budget, for the
    // background accounts' retirements (started above) and their last token
    // writes. A host still restoring past it has written no rotation since
    // its last persist (the pool names each one in its log).
    if (m_sessionPool) {
        stalltrace::Scope stallScope("quit-live-accounts");
        noteLiveAccountsStep(QStringLiteral("quit:foreground-stopped"),
                             QString());
#ifdef ENABLE_RUST_SDK_BACKEND
        // The foreground's own store close starts now, BEFORE the wait below,
        // so the two retire side by side within one budget (contract §6.6).
        // Otherwise it starts only in ~RustSdkMatrixClient, after this wait,
        // and a quit can take the background budget plus the foreground's.
        // Asked by name: the hook is WP1-S's (RustSdkMatrixClient), and a
        // build without it falls back, loudly, to waiting one after the other.
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get())) {
            noteLiveAccountsStep(QStringLiteral("quit:foreground-retiring"),
                                 QString());
            if (!QMetaObject::invokeMethod(rust, "beginQuitRetirement",
                                           Qt::DirectConnection)) {
                qCWarning(lcApp)
                    << "live accounts: the foreground store cannot start "
                       "closing before the background accounts are awaited; "
                       "the quit waits for them one after the other";
            }
        }
#endif
        const qint64 remaining = std::max<qint64>(
            0, m_liveAccountsQuitBudgetMs - m_liveAccountsQuitTimer.elapsed());
        const bool drained = m_sessionPool->waitForQuit(int(remaining));
        // A host still restoring at the deadline does not make the pool's
        // answer false (its retirement is deferred to a login result that
        // never came), so it is counted here, whatever that answer. A runtime
        // off finishing inside the wait may have released the pool.
        int stillRestoring = 0;
        if (m_sessionPool) {
            for (const QString &uid : m_settings->keepRunningUserIds()) {
                if (m_sessionPool->liveState(uid)
                    == AccountActivityModel::Starting)
                    ++stillRestoring;
            }
        }
        const bool overBudget = !drained || stillRestoring > 0;
        noteLiveAccountsStep(overBudget ? QStringLiteral("quit:over-budget")
                                        : QStringLiteral("quit:drained"),
                             QString());
        if (overBudget) {
            qCWarning(lcApp) << "background accounts did not all retire within"
                             << remaining << "ms at quit; still restoring:"
                             << stillRestoring;
        }
    }

    const qint64 elapsed = shutdownTimer.elapsed();
    qCInfo(lcApp) << "quit: prepareForShutdown end elapsed_ms=" << elapsed;
    if (elapsed >= 1000) {
        qCInfo(lcApp) << "prepareForShutdown took" << elapsed
                      << "ms before the window can finish closing";
    }
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
        // What the server holds until this write lands, for a retry to
        // compare with: a failed write's recorded base, else the server's last
        // answer this session, else unknown (a retry then sends again). Never
        // the local value: a device-only legacy mode was never on the server.
        if (!m_notificationModeWritesInFlight.contains(roomId)) {
            m_notificationModeWritesInFlight.insert(
                roomId, m_settings->roomNotificationModeUnsynced(roomId)
                            ? m_settings->roomNotificationModeUnsyncedBase(roomId)
                            : m_notificationModeServerSeen.value(roomId, -1));
        }
        sendRoomNotificationMode(roomId, mode);
    }
}

void AppController::sendRoomNotificationMode(const QString &roomId, int mode)
{
    m_notificationModeSentModes[roomId] |= 1 << mode;
    // Mode 3 removes the room override; Matrix has no follow-default rule.
    if (mode == 3)
        m_client->clearRoomNotificationMode(roomId);
    else
        m_client->setRoomNotificationMode(roomId, mode);
    Q_EMIT roomNotificationModeSent(roomId, mode);
}

void AppController::adoptServerNotificationMode(const QString &roomId, int mode)
{
    m_notificationModeSentModes.remove(roomId);
    m_settings->setRoomNotificationMode(roomId, mode);
    m_settings->setRoomNotificationModeUnsynced(roomId, false);
    Q_EMIT roomNotificationModeSyncStateChanged(roomId);
    qCInfo(lcApp) << "room notification mode changed elsewhere since a failed"
                  << "write; adopted mode=" << mode;
}

void AppController::retryFailedNotificationModes()
{
    if (!m_client || !m_client->supportsServerNotificationModes())
        return;
    const QStringList pending = m_settings->unsyncedRoomNotificationModes();
    if (pending.isEmpty())
        return;
    // Read before writing: the report decides whether the choice is sent
    // again or a newer one made elsewhere is adopted. Only once the account's
    // rules are known, or every room would read as having no rule.
    if (!m_client->initialSyncDone()) {
        m_retryNotificationModesWhenSynced = true;
        return;
    }
    m_retryNotificationModesWhenSynced = false;
    for (const QString &roomId : pending) {
        m_notificationModeRetryReads.insert(roomId);
        m_client->requestRoomNotificationMode(roomId);
    }
    qCDebug(lcApp) << "retrying room notification rules:" << pending.size();
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
    return m_settings->roomNotificationModeUnsynced(roomId);
}

bool AppController::roomNotificationModeDecidesLocally(const QString &roomId) const
{
    return m_settings->roomNotificationModeUnsynced(roomId)
        || m_settings->roomNotificationModeDeviceOnly(roomId);
}

void AppController::refreshStoredNotificationModes()
{
    if (!m_client || !m_client->supportsServerNotificationModes()
        || !m_client->initialSyncDone())
        return;
    // Read-only: each answer updates the local cache (see the report handler),
    // so a mute lifted on another client stops silencing this device. Only
    // this account's own keys; the legacy device-global ones are device-only
    // choices and never asked about or sent.
    int asked = 0;
    // Rooms muted (or set) from another client are not in the local record at
    // all, so ask the account's rules for every room that has one.
    m_client->requestAllRoomNotificationModes();
    const QStringList rooms = m_settings->accountRoomNotificationModeRooms();
    for (const QString &roomId : rooms) {
        // The retry owns an unsynced room.
        if (m_settings->roomNotificationModeUnsynced(roomId))
            continue;
        m_client->requestRoomNotificationMode(roomId);
        ++asked;
    }
    qCDebug(lcApp) << "room notification modes refreshed rooms=" << asked;
    Q_EMIT roomNotificationModesRefreshed(asked);
}

void AppController::refreshOwnProfile(qint64 minIntervalMs)
{
    if (!m_client || !m_client->isLoggedIn())
        return;
    const QString uid = m_client->currentUserId();
    if (uid.isEmpty())
        return;
    // Rate-limited, never polled: one request per cue at most, and the cues
    // are human-paced (a window activation, a profile event).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_ownProfileRefreshMs > 0 && now - m_ownProfileRefreshMs >= 0
        && now - m_ownProfileRefreshMs < minIntervalMs)
        return;
    m_ownProfileRefreshMs = now;
    m_client->fetchUserProfile(uid);
}

QVariantList AppController::liveVoiceRun(const QString &roomId,
                                         const QString &afterEventId,
                                         const QString &threadRootId,
                                         bool *known) const
{
    // Whichever loaded timeline is the clip's own: the room's main timeline,
    // or the thread panel's for a thread clip. "Known" only when that
    // timeline holds the clip itself, so an unloaded room never reads as "no
    // run" and wipes the snapshot.
    // A thread ROOT is a main-timeline message (some backends stamp it with
    // its own id as the root): its run is the room's.
    const QString thread =
        threadRootId == afterEventId ? QString() : threadRootId;
    TimelineModel *model = thread.isEmpty() ? m_timeline.get()
                                            : m_thread->model();
    const bool holds = model && !afterEventId.isEmpty()
        && model->realRoomIdForEvent(afterEventId) == roomId
        && (thread.isEmpty()
            || MatrixClient::threadTimelineRootId(model->roomId()) == thread);
    if (known)
        *known = holds;
    if (!holds)
        return {};
    return model->voiceRunAfter(roomId, afterEventId, thread);
}

void AppController::playNextVoiceMessage(const QString &roomId,
                                         const QString &afterEventId,
                                         const QString &threadRootId,
                                         const QVariantList &snapshot)
{
    // The clip's own timeline when it is loaded (it may have grown since the
    // snapshot), else the snapshot the player kept when the clip ended.
    bool known = false;
    const QVariantList live =
        liveVoiceRun(roomId, afterEventId, threadRootId, &known);
    const QVariantList run = known ? live : snapshot;
    // The run ends here: no clip stays marked as the chain's, or the reader
    // replaying the last one would have a failure skipped forward.
    m_voiceAutoplayEventId.clear();
    if (run.isEmpty())
        return;
    QVariantMap next = run.constFirst().toMap();
    if (next.value(QStringLiteral("roomName")).toString().isEmpty()
        && m_allRooms)
        next.insert(QStringLiteral("roomName"),
                    m_allRooms->findRoom(roomId).value(QStringLiteral("name")));
    // Marked before play(), which may report a failure synchronously, and
    // its currentChanged emissions are the chain's own, not the reader's.
    m_voiceAutoplayEventId = next.value(QStringLiteral("eventId")).toString();
    {
        const QScopedValueRollback starting(m_voiceAutoplayStarting, true);
        m_voicePlayback->play(m_voiceAutoplayEventId, roomId,
                              next.value(QStringLiteral("mediaKey")).toString(),
                              next);
    }
    // The rest of the run goes with it, so it plays on wherever the reader
    // is by the time this one ends.
    if (m_voicePlayback->eventId() == m_voiceAutoplayEventId)
        m_voicePlayback->setUpNext(run.mid(1));
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
    // A clip playing on through the recording is picked up by the
    // microphone. Paused, not stopped, as a call pauses it: the reader can
    // resume it afterwards.
    if (m_voicePlayback)
        m_voicePlayback->pause();
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

NetworkRouteController *AppController::networkRoute() const
{ return m_networkRoute.get(); }
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
int AppController::currentKeyboardModifiers() const { return int(QGuiApplication::keyboardModifiers()); }
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
QObject *AppController::accountPrefs() const { return m_accountPrefs.get(); }
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
GameActivity *AppController::gameActivity() const { return m_gameActivity.get(); }
MusicActivity *AppController::musicActivity() const { return m_musicActivity.get(); }
CallController *AppController::calls() const { return m_calls.get(); }

QString AppController::preferredCallLane(const QString &roomId) const
{
    if (roomId.isEmpty())
        return QString();
    const bool rtc =
        m_rtc && m_rtc->joinBlock(roomId) == RtcController::JoinBlock::None;
    // 1:1 DMs only: m.call.invite rings every room member.
    const bool legacy = m_calls && m_calls->mediaBackendAvailable()
        && !legacyCallPeer(roomId).isEmpty();
    // The user's call format, where this room can use it.
    const QString format =
        m_settings ? m_settings->callFormat() : QStringLiteral("auto");
    if (format == QLatin1String("legacy") && legacy)
        return QStringLiteral("legacy");
    // MatrixRTC first: it is what current Element speaks, and it carries
    // video, screen share and groups.
    if (rtc)
        return QStringLiteral("matrixrtc");
    return legacy ? QStringLiteral("legacy") : QString();
}

QVariantMap AppController::callFormats(const QString &roomId) const
{
    const bool rtc = !roomId.isEmpty() && m_rtc
        && m_rtc->joinBlock(roomId) == RtcController::JoinBlock::None;
    const bool legacy = !roomId.isEmpty() && m_calls
        && m_calls->mediaBackendAvailable()
        && !legacyCallPeer(roomId).isEmpty();
    const QString lane = preferredCallLane(roomId);
    QVariantMap out;
    out.insert(QStringLiteral("matrixrtc"), rtc);
    out.insert(QStringLiteral("legacy"), legacy);
    out.insert(QStringLiteral("lane"), lane);
    out.insert(QStringLiteral("autoLane"),
               rtc ? QStringLiteral("matrixrtc")
                   : (legacy ? QStringLiteral("legacy") : QString()));
    out.insert(QStringLiteral("video"),
               lane == QLatin1String("matrixrtc")
                   || (lane == QLatin1String("legacy")
                       && m_calls->videoSupported()));
    return out;
}

QString AppController::legacyCallPeer(const QString &roomId) const
{
    // m.direct can survive inviting a third member. An unavailable count
    // cannot establish that a legacy invite will reach only one peer.
    if (!m_client)
        return QString();
    return calls::oneToOneDmPeer(m_client->roomInfo(roomId), /*requireKnownCount=*/true);
}

void AppController::withdrawCallFailure()
{
    // The strip may show something newer; that is not ours to clear.
    if (const auto report = m_callNotice.withdraw())
        Q_EMIT errorReported(*report);
}

bool AppController::canStartCall(const QString &roomId) const
{
    return !preferredCallLane(roomId).isEmpty();
}

bool AppController::startCall(const QString &roomId, bool withVideo)
{
    // A routed account (docs/network-profiles.md) cannot call on either
    // lane; both refuse silently below, so say why here.
    if (m_client && m_client->networkRouteActive()) {
        qCInfo(lcApp) << "call start refused: the account uses a network route";
        Q_EMIT callStartRefused(
            tr("Calls aren't available while this account uses a proxy."));
        return false;
    }
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
        // Armed so a membership the server refuses can fall back to the
        // legacy lane (see the membershipRefused connection). After join():
        // its own Preparing dropped any older arm.
        m_legacyFallback.buttonJoinDispatched(roomId, withVideo, ok);
        return ok;
    }
    if (lane == QLatin1String("legacy")) {
        if (withVideo && !m_calls->videoSupported()) {
            Q_EMIT callStartRefused(
                tr("Video calls need a MatrixRTC service, which isn't "
                   "available here yet."));
            return false;
        }
        const bool ok =
            m_calls->placeCall(roomId, legacyCallPeer(roomId), withVideo);
        qCInfo(lcApp) << "legacy call dispatched ok=" << ok;
        // An earlier MatrixRTC refusal no longer describes this call.
        if (ok)
            withdrawCallFailure();
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

AudioDeviceTester *AppController::audioTester() const
{
    return m_audioTester.get();
}

void AppController::enableCallSounds()
{
    if (m_callSounds->sink())
        return;
    // Cues follow the call's chosen speaker, read live on every cue.
    QPointer<SettingsManager> settings = m_settings.get();
    auto player = std::make_unique<CallSoundPlayer>([settings] {
        return settings ? settings->preferredSpeakerId() : QString();
    });
    // Cues whose output went away are reloaded when the output list changes,
    // told by the device controller: its signal is the one measured to fire
    // on Windows 2026-10-07 (the player's own path never reloaded). Asking it
    // anything also starts its watcher, which only a call engine did before;
    // Qt Multimedia is already up, the player just listed the outputs.
    CallDeviceController *devices = m_callDevices.get();
    devices->hasSpeaker();
    player->followOutputChanges(devices,
                                &CallDeviceController::audioOutputsChanged,
                                [devices] { return devices->hasSpeaker(); });
    m_callSounds->setSink(std::move(player));
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
        // The chosen microphone and speaker, resolved exactly as the MatrixRTC
        // lane resolves them (device monitor binding, multi-input matrix), at
        // the start of each call and again when the choice changes.
        engine->setAudioDeviceResolver([this] {
            GstCallMediaBackend::AudioDevicePlan plan;
            // Nothing to resolve without a preference, and asking would
            // initialise Qt Multimedia (startup cost, and a SPA parse error
            // per device on PipeWire): the engine's defaults follow the
            // system.
            if (!m_settings || !m_callDevices)
                return plan;
            if (!m_settings->preferredMicrophoneId().isEmpty()) {
                const auto mic = m_callDevices->microphoneSelection();
                const SfuMediaEngine::MicrophoneCapture capture =
                    SfuMediaEngine::resolveMicrophoneCapture(
                        {mic.id, mic.description});
                plan.microphoneFront =
                    SfuMediaEngine::microphoneFrontDescription(capture);
                plan.microphoneBinding = capture.binding;
            }
            if (!m_settings->preferredSpeakerId().isEmpty()) {
                const auto speaker = m_callDevices->speakerSelection();
                const SfuMediaEngine::OutputSink sink =
                    SfuMediaEngine::resolveSpeakerSink(
                        {speaker.id, speaker.description});
                plan.speakerSink = sink.sink;
                plan.speakerBinding = sink.binding;
            }
            return plan;
        });
        // The camera, composed as the MatrixRTC lane composes it. A diagnostic
        // pattern replaces it on a machine with no camera; documented in
        // docs/feature-contracts.md beside the other call diagnostics.
        engine->setCameraResolver([this](int portalFd) {
            GstCallMediaBackend::CameraPlan plan;
            if (qEnvironmentVariableIsSet("LIGHTNING_CALL_TEST_CAMERA")) {
                plan.front = QStringLiteral(
                    "videotestsrc is-live=true pattern=ball name=capsrc "
                    "! video/x-raw,width=640,height=360,"
                    "framerate=(fraction)30/1");
                return plan;
            }
            const bool direct = portalFd < 0;
#ifdef Q_OS_LINUX
            // A sandbox has no /dev/video*: the portal is its only camera.
            if (direct && sandboxenv::isSandboxed()) {
                if (CameraPortal::available())
                    plan.needsPortal = true;
                else
                    plan.refusal = QStringLiteral("camera_unavailable");
                return plan;
            }
#endif
            const QString source = SfuMediaEngine::cameraSource(portalFd);
            if (direct && m_callDevices) {
                const auto camera = m_callDevices->cameraSelection();
                // Never another camera in place of the chosen one.
                if (camera.id.isEmpty() && camera.preferredMissing) {
                    plan.refusal = QStringLiteral("camera_unavailable");
                    return plan;
                }
                if (!camera.id.isEmpty()) {
                    plan.binding = lightning::calls::resolveDeviceBinding(
                        lightning::calls::CaptureKind::Camera,
                        source.section(QLatin1Char(' '), 0, 0), camera.id,
                        camera.description, {});
                    if (plan.binding.refused) {
                        plan.refusal = QStringLiteral("camera_unavailable");
                        return plan;
                    }
                }
            }
            plan.front =
                QStringLiteral("%1 name=capsrc ! %2 "
                               "! queue max-size-buffers=4 leaky=downstream "
                               "! videoconvert ! videoscale ! %3 "
                               "! capsfilter caps=\"%4\"")
                    .arg(source,
                         direct ? QStringLiteral("@ENTRY@")
                                : SfuMediaEngine::portalCameraEntry(),
                         SfuMediaEngine::cameraRateStage(!direct),
                         SfuMediaEngine::cameraLimitsCaps(!direct));
            if (direct) {
                plan.probe =
                    QStringLiteral("%1 name=capsrc ! fakesink").arg(source);
                plan.chooseEntry = [](GstElement *probe) {
                    QString ioMode = QStringLiteral("auto");
                    return SfuMediaEngine::captureCameraEntry(
                        probe, SfuMediaEngine::jpegCameraChainAvailable(),
                        &ioMode);
                };
            }
            return plan;
        });
        if (CameraPortal::available()) {
            auto *portal = new CameraPortal(engine);
            auto callIdOnGrant = std::make_shared<QString>();
            connect(engine, &GstCallMediaBackend::cameraAccessNeeded, portal,
                    [portal, callIdOnGrant](const QString &callId) {
                        *callIdOnGrant = callId;
                        portal->requestAccess();
                    });
            connect(portal, &CameraPortal::ready, engine,
                    [engine, callIdOnGrant](int fd) {
                        engine->providePortalCamera(*callIdOnGrant, fd);
                    });
            const auto refused = [engine, callIdOnGrant] {
                engine->providePortalCamera(*callIdOnGrant, -1);
            };
            connect(portal, &CameraPortal::cancelled, engine, refused);
            connect(portal, &CameraPortal::failed, engine, refused);
        }
        connect(m_calls.get(), &CallController::cameraFailed, this,
                [this](const QString &category) {
                    qCWarning(lcApp) << "call camera failed category="
                                     << category;
                    Q_EMIT errorReported(tr("Your camera couldn't be started. "
                                            "The call goes on without it."));
                });
        // A pick in the call's device menu (or a hotplug that moves the
        // active device) reaches a live 1:1 call now, not the next one.
        connect(m_callDevices.get(),
                &CallDeviceController::activeDevicesChanged, engine,
                [engine] { engine->audioDevicesChanged(); });
        qCInfo(lcApp) << "voice-call media engine active (webrtcbin)";
    }
    // The SFU engine probes a wider element set (video, screen capture), so
    // it can be unavailable where the 1:1 engine is fine.
    QString sfuWhyNot;
    if (SfuMediaEngine::runtimeAvailable(&sfuWhyNot)) {
        auto *sfu = new SfuMediaEngine(this);
        m_groupCall->setMediaEngine(sfu);
        // The Settings device test builds on the same chain and elements.
        m_audioTester->setRuntimeAvailable(true);
        // Chosen camera, microphone and speaker for the MatrixRTC lane. The
        // camera lands on the next capture; a microphone or speaker pick also
        // reaches a running call (SfuMediaEngine::setPreferredDevices()).
        const auto applySfuDevices = [this, sfu] {
            const auto camera = m_callDevices->cameraSelection();
            const auto microphone = m_callDevices->microphoneSelection();
            const auto speaker = m_callDevices->speakerSelection();
            // `preferredMissing`: a chosen camera that is gone opens NO
            // camera, never the default (CaptureDeviceSelection.h).
            sfu->setPreferredDevices({camera.id, camera.description,
                                      camera.preferredMissing},
                                     {microphone.id, microphone.description},
                                     {speaker.id, speaker.description});
        };
        applySfuDevices();
        connect(m_callDevices.get(),
                &CallDeviceController::activeDevicesChanged, sfu,
                applySfuDevices);
        // A received track whose output failed (headset unplugged, RDP audio
        // dropped) and gave up is rebuilt when an output comes back. The
        // engine debounces; the notification is Qt's, nothing is polled.
        connect(m_callDevices.get(),
                &CallDeviceController::audioOutputsChanged, sfu,
                [this, sfu] {
                    // Outside a call there is nothing to re-arm; ask for the
                    // device list only when one is running (an argument is
                    // evaluated before the callee's early return).
                    if (!sfu->active())
                        return;
                    sfu->notifyAudioOutputsChanged(
                        m_callDevices->hasSpeaker());
                });
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
        // SfuCallController::linuxCameraRoute() prefers a usable portal camera
        // unless a camera was chosen in Settings (PipeWire shares the device
        // with other apps); inside a Flatpak it is the only camera.
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

QString AppController::verificationRoleAtStart(bool isSelf) const
{
    // Read when the flow starts: once it is done this session's own trust
    // state may have changed. Wording only; trust is the SDK's to report.
    if (!isSelf)
        return QStringLiteral("other_user");
    if (sessionVerificationNeeded())
        return QStringLiteral("this_session");
    if (m_sessionTrustState == QLatin1String("Verified"))
        return QStringLiteral("other_session");
    return {};
}

QString AppController::verificationCancelReasonFor(const QString &message)
{
    // rust/src/lib.rs reports a SAS or QR cancel as the Debug form of
    // matrix-sdk-crypto's CancelInfo::reason() ("\"The SAS did not
    // match.\""), its own request paths as "cancelled" or
    // "timed_out_waiting_for_peer"; the cancel-code names are accepted too.
    // A mismatch is any check of the other side's keys failing: the SAS
    // (emoji or decimal), the QR code's key, or the commitment the SAS
    // start carried, which a substituted key breaks just the same.
    const QString m = message.toLower();
    if (m.contains(QLatin1String("mismatchedsas"))
        || m.contains(QLatin1String("keymismatch"))
        || m.contains(QLatin1String("mismatchedcommitment"))
        || m.contains(QLatin1String("the sas did not match"))
        || m.contains(QLatin1String("expected key did not match"))
        || m.contains(QLatin1String("hash commitment did not match")))
        return QStringLiteral("mismatch");
    if (m.contains(QLatin1String("timeout"))
        || m.contains(QLatin1String("timed out"))
        || m.contains(QLatin1String("timed_out")))
        return QStringLiteral("timeout");
    if (m.contains(QLatin1String("user cancelled"))
        || m.trimmed() == QLatin1String("user")
        || m.trimmed() == QLatin1String("cancelled"))
        return QStringLiteral("user");
    return {};
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

int AppController::statusErrorExpiryMs(int length)
{
    // A 4 s glance plus about 17 characters a second, so the long keyring
    // and recovery messages are not gone before they are read; never under
    // 8 s, and never over 30 s (the strip can be dismissed sooner).
    return std::clamp(4000 + 60 * length, 8000, 30000);
}

void AppController::setStatusErrorExpiryMsForTest(int ms)
{
    m_statusErrorExpiryForTestMs = ms;
}

bool AppController::statusErrorIsRunningCallNotice() const
{
    const bool callRunning = m_groupCall->active() || m_calls->sessionLive();
    return callRunning && !m_callStatusNotice.isEmpty()
        && m_statusError == m_callStatusNotice;
}

void AppController::armStatusErrorExpiry(quint64 seq)
{
    const int ms = m_statusErrorExpiryForTestMs > 0
        ? m_statusErrorExpiryForTestMs
        : statusErrorExpiryMs(int(m_statusError.size()));
    QTimer::singleShot(ms, this, [this, seq] {
        if (seq != m_statusErrorSeq)
            return;   // replaced, cleared or dismissed since
        // A running call's own notice ("nobody hears you") outlives the
        // timer while the call runs, then expires. Anything else shown
        // during a call expires like any other message.
        if (statusErrorIsRunningCallNotice())
            armStatusErrorExpiry(seq);
        else
            clearStatusError(/*keepCallNotice=*/false);
    });
}

void AppController::clearStatusError(bool keepCallNotice)
{
    if (m_statusError.isEmpty())
        return;
    if (keepCallNotice && statusErrorIsRunningCallNotice())
        return;
    Q_EMIT errorReported(QString());
}

void AppController::dismissStatusError()
{
    clearStatusError(/*keepCallNotice=*/false);
}

void AppController::closeCurrentRoomNoLongerIn(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    // A clip from a room the user is no longer in stops (and its run with
    // it), whether or not that room is the one on screen.
    if (m_voicePlayback && m_voicePlayback->active()
        && m_voicePlayback->roomId() == roomId)
        m_voicePlayback->stop();
    if (m_currentRoomId == roomId) {
        qCInfo(lcApp) << "open room closed: no longer joined or invited";
#ifdef ENABLE_RUST_SDK_BACKEND
        if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
            rust->closeRoomTimeline();
#endif
        setCurrentRoomId(QString());
    }
    if (m_roomInfo->roomId() == roomId)
        m_roomInfo->setRoomId(QString());
}

void AppController::setCurrentRoomId(const QString &roomId)
{
    if (m_currentRoomId == roomId)
        return;
    m_currentRoomId = roomId;
    ++m_currentRoomOpenSerial;
    // In-memory Back/Forward list; a history move itself is not a new visit.
    if (!m_roomHistoryNavigating)
        m_roomHistory.visit(roomId);
    // Whether the room list holds it now; the roomsChanged handler closes a
    // listed room that then leaves the list (closeCurrentRoomNoLongerIn).
    if (!roomId.isEmpty() && m_client) {
        const RoomInfo info = m_client->roomInfo(roomId);
        m_currentRoomListed =
            !info.id.isEmpty() && info.membership != RoomInfo::Left;
    } else {
        m_currentRoomListed = false;
    }
    // LIGHTNING_SWITCH_TRACE: leaving the room S11 measures before it filled
    // gives target 2 up; the first room opened after a traced login starts
    // S11 (both no-ops otherwise, and S11 begins only once per trace).
    lightning::switchtrace::abandonRoomInteractive();
    if (!roomId.isEmpty())
        lightning::switchtrace::stepBegin(lightning::switchtrace::Step::S11);
    // Close the thread panel before the timeline switches rooms.
    m_thread->handleCurrentRoomChanged(roomId);
    m_eventContext->handleCurrentRoomChanged(roomId);
    // Inline video never survives a room switch: its row is destroyed. The
    // voice/audio player is app-owned and keeps playing (the floating
    // mini-player carries it while its room is elsewhere), so it takes
    // audibility back. Video positions are NOT forgotten here: the cover
    // offers to resume when the reader comes back.
    m_playback->stopAll();
    m_voicePlayback->reclaimAudibility();
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
    // The posting block and moderator Delete read these, so they act in a
    // room whose info panel was never opened.
    m_openRoomPermissions->setRoomId(roomId);
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
    // The same for the Channels column: opening a room must not move it.
    if (m_spaceChannels)
        m_spaceChannels->setOpenRoomId(roomId);
    // Disarm mention suggestions, or the previous room's roster keeps
    // refetching on every membership event.
    m_mentionSuggestions->setRoomId(QString());
    // Hydrate the member roster on first open so display names resolve in
    // mentions, replies and thread summaries. Once per room per session; a
    // failed fetch un-marks the room so the next open retries.
    bool membersRequested = false;
    if (!roomId.isEmpty() && m_client
        && !m_memberHydratedRooms.contains(roomId)) {
        // Record only a dispatched request: a synchronous rejection returns 0
        // without ever emitting roomMembersReceived.
        if (m_client->requestRoomMembers(roomId) != 0) {
            m_memberHydratedRooms.insert(roomId);
            membersRequested = true;
        }
    }
    // The call-membership permission comes from that same snapshot; the gate
    // asks for its own only when none is on its way and it has no fresh
    // answer. Known before the call button is pressed.
    m_rtc->setCurrentRoom(roomId, membersRequested);
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
    // Add account would replace the session a waiting sign-out is for.
    if (m_auth->logoutPending() && m_client->isLoggedIn()) {
        qCInfo(lcApp) << "add account refused: a sign-out is in progress";
        return;
    }
    // Entering login while signed in is the add-account flow; remember where
    // to return if it fails or the user goes back.
    if (m_client->isLoggedIn())
        m_addAccountReturnTo = m_settings->activeAccountUserId();
    clearQueuedAccountSwitch("add-account");
    // Leave a live call while its client is still attached: the retraction
    // cannot be sent once the client is released, which happens before
    // onLoginSucceeded() runs.
    if (m_groupCall && m_client->isLoggedIn())
        m_groupCall->leave();
    m_composer->setRoomId({});
    setCurrentScreen(LoginScreen);
}
bool AppController::showMain()
{
    // An add-account browser sign-in left behind: its loopback listener and
    // the server's page stay live for up to fifteen minutes, and finishing it
    // later would replace the session the user went back to. Cancelled first;
    // the backend answers synchronously with loginFailed, whose handler is
    // told not to start the return-to restore this function makes below.
    if (m_auth && m_auth->browserLoginInProgress()) {
        qCInfo(lcApp) << "leaving the sign-in screen: cancelling the browser "
                         "sign-in still waiting";
        m_abandoningBrowserSignIn = true;
        m_auth->cancelBrowserLogin();
        m_abandoningBrowserSignIn = false;
    }
    // Returning from add-account: a failed attempt released the shared
    // client's session, so restore the active account first.
    if (!m_client->isLoggedIn() && !m_accountSwitching) {
        // Cleared before the restore: its failure is answered here, never by
        // the failure handler's own return-to restore.
        const QString returnTo = std::exchange(m_addAccountReturnTo, QString{});
        if (!returnTo.isEmpty() && m_settings->hasSavedAccount(returnTo))
            m_settings->setActiveAccountUserId(returnTo);
        if (m_settings->hasSession()) {
            const quint64 failuresBefore = m_loginFailures;
            m_client->restoreSession();
            if (m_loginFailures != failuresBefore) {
                // Refused before it returned (a keyring that cannot answer,
                // a store that is gone): the shell would show a session that
                // does not exist. Stay with the card, or the error, it left.
                qCInfo(lcApp) << "back to the app: the restore failed at once "
                                 "— staying on the sign-in screen";
                m_backgroundRestore = false;
                return false;
            }
        }
    }
    m_addAccountReturnTo.clear();
    m_backgroundRestore = false;
    Q_EMIT errorReported(QString{});
    m_composer->setRoomId(m_currentRoomId);
    setCurrentScreen(MainScreen);
    return true;
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
    if (m_eventContext)
        m_eventContext->close();
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

void AppController::requestSettingsSearchFocus()
{
    m_settingsSearchFocusRequested = true;
    showSettings();
    Q_EMIT settingsSearchFocusRequested();
}

bool AppController::takeSettingsSearchFocusRequested()
{
    const bool requested = m_settingsSearchFocusRequested;
    m_settingsSearchFocusRequested = false;
    return requested;
}

QUrl AppController::defaultFileDialogFolder() const
{
    for (const auto location : { QStandardPaths::PicturesLocation,
                                 QStandardPaths::DocumentsLocation,
                                 QStandardPaths::HomeLocation }) {
        const QString dir = QStandardPaths::writableLocation(location);
        if (!dir.isEmpty() && QFileInfo::exists(dir))
            return QUrl::fromLocalFile(dir);
    }
    return {};
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

QRect AppController::fitWindowGeometry(const QRect &geometry)
{
    QList<QRect> areas;
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (screen)
            areas.append(screen->availableGeometry());
    }
    return fitWindowGeometry(geometry, areas);
}

QRect AppController::fitWindowGeometry(const QRect &geometry,
                                       const QList<QRect> &screenAreas)
{
    if (geometry.isEmpty())
        return {};
    // The screen the window lands on: the one holding most of its top grab
    // band (the band windowGeometryIsReachable() requires).
    const QRect grabBand(geometry.x(), geometry.y(), geometry.width(), 32);
    QRect area;
    qint64 best = 0;
    for (const QRect &candidate : screenAreas) {
        const QRect overlap = candidate.intersected(grabBand);
        const qint64 size = qint64(overlap.width()) * overlap.height();
        if (!overlap.isEmpty() && size > best) {
            best = size;
            area = candidate;
        }
    }
    if (area.isEmpty())
        return geometry;
    // No larger than that screen's available area, and never below the
    // window's own minimum (Main.qml's minimumWidth/Height).
    constexpr int kMinWidth = 640;
    constexpr int kMinHeight = 420;
    const int width =
        std::max(std::min(geometry.width(), area.width()), kMinWidth);
    const int height =
        std::max(std::min(geometry.height(), area.height()), kMinHeight);
    // Then moved inside it; a window larger than the screen (only below the
    // minimum) keeps the screen's top-left corner.
    const auto fit = [](int pos, int length, int start, int span) {
        if (length >= span)
            return start;
        return std::clamp(pos, start, start + span - length);
    };
    return QRect(fit(geometry.x(), width, area.x(), area.width()),
                 fit(geometry.y(), height, area.y(), area.height()),
                 width, height);
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

void AppController::handleLiveEvent(const QString &composedRoomId,
                                    const TimelineEvent &event)
{
    // The read-only context view shows history; what it appends is never
    // news, so it must not notify or feed the Activity Center.
    if (MatrixClient::isContextTimelineId(composedRoomId))
        return;
    // Map a thread copy to its real room rather than dropping it: the live
    // room timeline hides threaded events. The composite id never leaves
    // this scope.
    const bool fromThread = MatrixClient::isThreadTimelineId(composedRoomId);
    const QString roomId = fromThread
        ? MatrixClient::threadTimelineRoomId(composedRoomId)
        : composedRoomId;
    if (roomId.isEmpty())
        return;
    // One event can reach here twice: a thread copy and the room's sync, an
    // SDK insert observed live and a later append, a sliding-sync reset
    // re-delivering. Deduplicated by event id, forgetting the oldest first.
    if (!event.eventId.isEmpty() && !m_notifiedEventIds.insert(event.eventId))
        return;
    // Activity prompts a presence refresh; it is never evidence of "online".
    if (m_presence && event.sender != m_client->currentUserId())
        m_presence->noteActivity(event.sender);
    NotificationManager::Context context =
        notificationContextFor(roomId, event, fromThread);
    // Live accounts (Labs): the foreground's name, for a title that says
    // which account a card is for. The account stays empty, so the card is
    // still the foreground's ("" table, today's stamping).
    if (m_sessionPool)
        context.accountLabel =
            liveAccountLabel(m_settings->activeAccountUserId());
    // A card is about to name a sender nothing on hand names (no member
    // name, no profile cached): ask for their profile, once per session, so
    // later cards use it rather than the localpart. Only for an event that
    // notifies (decide() is pure), never for the initial-sync backlog.
    const auto bare = [&event](const QString &name) {
        return name.isEmpty() || name == event.sender;
    };
    if (m_userProfiles && context.initialSyncComplete
        && context.notificationsEnabled
        && bare(event.senderDisplayName) && bare(context.senderMemberName)
        && !m_userProfiles->profile(event.sender).known
        && NotificationManager::decide(event, context).notify)
        m_userProfiles->request(event.sender);
    m_notifications->processEvent(event, context);
    // The Activity Center classifies independently (a muted room's mention is
    // still activity), but an ignored user's event in flight never becomes a
    // row.
    if (!context.senderIsIgnored)
        m_activity->ingest(event, context.roomName);
}

NotificationManager::Context AppController::notificationContextFor(
    const QString &roomId, const TimelineEvent &event, bool fromThread) const
{
    // The rules live in NotificationContextBuilder, which a background
    // account's host will share; this only gathers the foreground's inputs.
    // The account stays empty: the foreground's stamping is today's.
    lightning::notifications::NotificationContextInputs in;
    const QString uid = m_settings->activeAccountUserId();
    in.selfUserId = m_client->currentUserId();
    in.roomId = roomId;
    // Targeted lookup; rooms() deep-copies the whole list per event.
    const RoomInfo info = m_client->roomInfo(roomId);
    in.roomInfoName = info.name;
    in.roomIsDirect = info.isDirect;
    const QVariantMap notifyRoomRow = m_roomList->findRoom(roomId);
    in.avatarMxc = notifyRoomRow.value(QStringLiteral("avatarUrl")).toString();
    in.avatarColorKey =
        notifyRoomRow.value(QStringLiteral("identityColorKey")).toString();
    in.themeId = int(m_settings->theme());
    // The *For(uid) reads answer exactly as the active-account accessors do
    // for the active account, without depending on which account is active.
    in.roomMode = m_settings->roomNotificationModeFor(uid, roomId);
    in.localModeUnsynced =
        m_settings->roomNotificationModeDecidesLocallyFor(uid, roomId);
    in.previewMode = noticePreviewFor(roomId);
    in.notificationsEnabled = m_settings->notificationsEnabled();
    in.eventId = event.eventId;
    in.eventThreadRootId = event.threadRootId;
    in.fromThread = fromThread;
    in.currentRoomId = m_currentRoomId;
    in.activeRoomAtLatest = m_activeRoomAtLatest;
    in.activeRoomHydrating = m_activeRoomHydrating;
    // Const getters, so reading them eagerly changes nothing.
    in.threadPanelPresent = m_thread != nullptr;
    in.threadPanelRootId = m_thread ? m_thread->rootEventId() : QString();
    in.threadPanelAtLatest = m_thread && m_thread->viewerAtLatest();
    in.initialSyncComplete = m_client->initialSyncDone();
    in.soundMode = m_settings->notificationSound();
    in.senderIsIgnored = m_moderation && m_moderation->isIgnored(event.sender);
    // The sender label's names (NotificationManager::senderLabel): the member
    // list's (the bare user id when it does not know them) and the cached
    // global profile's. Never asks; handleLiveEvent() does.
    in.senderMemberName = m_client->displayNameFor(roomId, event.sender);
    in.senderProfileName =
        m_userProfiles ? m_userProfiles->profile(event.sender).displayName
                       : QString();
    return lightning::notifications::NotificationContextBuilder::build(in);
}

int AppController::noticePreviewFor(const QString &roomId) const
{
    if (!m_settings)
        return notices::kPreviewPrivate;
    // RoomInfo defaults encryptionKnown to true, so a room that is not listed
    // yet (or no room at all, as for a verification request) must be passed
    // as unknown explicitly: the stricter of the two levels then wins.
    const RoomInfo info =
        (m_client && !roomId.isEmpty()) ? m_client->roomInfo(roomId)
                                        : RoomInfo{};
    const bool known = !info.id.isEmpty();
    return m_settings->effectiveNotificationPreview(
        known && info.encrypted, known && info.encryptionKnown);
}

bool AppController::masterMuteIsOn() const
{
    return m_pushRules && m_pushRules->masterMuteKnown()
        && m_pushRules->masterMute();
}

void AppController::registerMainWindow(QObject *window)
{
    auto *quick = qobject_cast<QQuickWindow *>(window);
    if (!quick || quick == m_mainWindow)
        return;
    if (m_mainWindow)
        disconnect(m_mainWindow, nullptr, this, nullptr);
    m_mainWindow = quick;
    connect(quick, &QWindow::activeChanged, this,
            &AppController::refreshMainWindowState);
    connect(quick, &QWindow::visibilityChanged, this,
            [this](QWindow::Visibility) { refreshMainWindowState(); });
    refreshMainWindowState();
}

QQuickWindow *AppController::mainWindow()
{
    if (!m_mainWindow) {
        // Main.qml's ApplicationWindow is the only one in the application;
        // the call windows are plain Windows.
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *candidate : windows) {
            if (candidate->inherits("QQuickApplicationWindow")) {
                registerMainWindow(candidate);
                break;
            }
        }
    }
    return m_mainWindow.data();
}

bool AppController::focusBelongsToMainWindow(const QWindow *main,
                                             const QWindow *focus)
{
    // Not QWindow::isActive(): for a window with no parent that is
    // isAncestorOf(focusWindow, IncludeTransients), and Qt makes the main
    // window the TRANSIENT PARENT of a QML Window declared inside it (the
    // full-screen call window in CallStage.qml; only the picture-in-picture
    // window opts out with transientParent: null). So a focused full-screen
    // call counted as the user reading the room hidden behind it.
    //
    // Walk from the focus window towards its ancestors instead. Reaching the
    // main window means focus is on it or on something it opened: a Popup or
    // Menu with popupType Window (QQuickPopupWindow), a native dialog or
    // menu. Meeting any other Qt Quick window first means focus is in a
    // window of its own, transient child of the main window or not.
    for (const QWindow *w = focus; w; w = w->parent(QWindow::IncludeTransients)) {
        if (w == main)
            return true;
        if (qobject_cast<const QQuickWindow *>(w)
            && !w->inherits("QQuickPopupWindow"))
            return false;
    }
    return false;
}

void AppController::refreshMainWindowState()
{
    if (!m_readReceipts)
        return;
    QQuickWindow *window = mainWindow();
    bool active = false;
    if (window) {
        const QWindow::Visibility visibility = window->visibility();
        active = notices::mainWindowCountsAsActive(
            focusBelongsToMainWindow(window, QGuiApplication::focusWindow()),
            visibility == QWindow::Minimized,
            visibility == QWindow::Hidden || !window->isVisible());
    } else if (auto *guiApp =
                   qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        active = guiApp->applicationState() == Qt::ApplicationActive;
    }
    m_readReceipts->setWindowActive(active);
}

void AppController::alertMainWindow()
{
    QQuickWindow *window = mainWindow();
    // Only when the user is not already looking at it: a focused window
    // flashing for every message would be noise.
    if (!window
        || (focusBelongsToMainWindow(window, QGuiApplication::focusWindow())
            && window->visibility() != QWindow::Minimized))
        return;
    // Not on Wayland: Qt turns alert() into an xdg-activation request there,
    // so every message would raise and focus the window (GitHub #49).
    if (!notices::windowAlertAllowed(QGuiApplication::platformName()))
        return;
    window->alert(0);
}

void AppController::activateMainWindowForNotificationClick()
{
    // A deliberate click only. Without the daemon's ActivationToken, Wayland
    // refuses requestActivate() (stale input serial); alert() asks for a
    // token with no serial and so can bring the window forward.
    if (!notices::clickNeedsAlertActivation(QGuiApplication::platformName(),
                                            m_notifications
                                                && m_notifications->lastClickHadActivationToken()))
        return;
    if (QQuickWindow *window = mainWindow())
        window->alert(0);
}

void AppController::noteRtcSessionForRing(const QString &roomId)
{
    if (!m_calls || !m_rtc || roomId.isEmpty())
        return;
    if (m_calls->state() != CallController::State::Ringing
        || !m_calls->rtcRing() || m_calls->activeRoomId() != roomId)
        return;
    // The newest of OUR user's memberships from another device: answered
    // there, if it appeared after the ring began (CallController judges
    // that). This device's own membership is the local join, which the group
    // call's stateChanged already reports (noteAnsweredByOtherLane).
    qint64 ownElsewhereJoinedAtMs = 0;
    const QVariantList participants = m_rtc->participants(roomId, 0);
    for (const QVariant &entry : participants) {
        const QVariantMap p = entry.toMap();
        if (p.value(QStringLiteral("ownUser")).toBool()
            && !p.value(QStringLiteral("ownDevice")).toBool()) {
            // A membership with no creation time counts as old: it cannot
            // show that it answered this ring.
            ownElsewhereJoinedAtMs = std::max(
                ownElsewhereJoinedAtMs,
                std::max<qint64>(
                    1, p.value(QStringLiteral("joinedAtMs")).toLongLong()));
        }
    }
    // Never a 0 from a read that cannot know (failed, store-only, not read
    // yet): that would end a live ring as missed.
    m_calls->noteRtcSession(roomId, ownElsewhereJoinedAtMs,
                            m_rtc->authoritativeParticipantCount(roomId));
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
    // A card or row can outlive the membership it was raised under (left,
    // kicked or banned, here or elsewhere; an invite declined elsewhere).
    // Opening it would land on a room the user is no longer in, or on
    // nothing at all for one the client no longer knows, so the window is
    // raised and the user told instead. An invite still pending opens its
    // preview as before.
    if (!target.isEmpty() && m_client) {
        const RoomInfo info = m_client->roomInfo(target);
        if (info.id.isEmpty()
            || (info.membership != RoomInfo::Joined
                && info.membership != RoomInfo::Invited)) {
            qCInfo(lcApp) << "notification open refused: not a joined room"
                          << "known=" << !info.id.isEmpty();
            Q_EMIT notificationOpenRequested(QString(), QString(), QString());
            Q_EMIT errorReported(
                tr("You are no longer in that room, so it cannot be opened."));
            return;
        }
    }
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
    const int requested = mute ? 2 : 3;
    QSettings store;
    int restored = 0;
    for (const QString &roomId : rooms) {
        const int current = m_settings->roomNotificationMode(roomId);
        const int mode = mute ? requested
                              : spaceUnmuteMode(store, roomId, current,
                                                &restored);
        if (mute)
            recordSpaceMutePrior(store, roomId, current);
        if (mode < 0 || current == mode)
            continue;   // idempotent: no write, no push-rule call
        setRoomNotificationMode(roomId, mode);
    }
    qCInfo(lcApp) << "space notification mode applied rooms=" << rooms.size()
                  << "muted=" << mute << "restored=" << restored;
}

// Muting a Space replaces each room's own push rules with one mute override
// (matrix-sdk deletes the room's other custom rules), so the mode the room
// held is recorded before and put back on unmute: a "Mentions & keywords"
// room used to come back as the account default. Kept under the account's
// own settings group, which sign-out and account removal drop whole; without
// a saved account nothing is recorded and unmute follows the default.
QString AppController::spaceMutePriorKey(const QString &roomId) const
{
    const QString uid = m_settings->activeAccountUserId();
    if (uid.isEmpty() || !m_settings->hasSavedAccount(uid))
        return {};
    const QString slug = matrix::app_data::safeUserSlug(uid);
    if (slug.isEmpty())
        return {};
    return QStringLiteral("accounts/") + slug
        + QStringLiteral("/notifications/space-mute-prior/") + roomId;
}

void AppController::recordSpaceMutePrior(QSettings &store,
                                         const QString &roomId,
                                         int current) const
{
    const QString key = spaceMutePriorKey(roomId);
    if (key.isEmpty())
        return;
    // A room already muted may be so from an earlier Space mute, whose record
    // holds the real prior mode: that one is never overwritten. A room in any
    // other mode records it (a record left from before is stale). No stored
    // key reads as 0 but is "no rule" unless the server reported an explicit
    // all-messages rule this session.
    if (current == 2 && store.contains(key))
        return;
    int prior = current;
    if (!m_settings->hasStoredRoomNotificationMode(roomId))
        prior = m_notificationModeServerSeen.value(roomId, -1) == 0 ? 0 : 3;
    store.setValue(key, prior);
}

int AppController::spaceUnmuteMode(QSettings &store, const QString &roomId,
                                   int current, int *restored) const
{
    const QString key = spaceMutePriorKey(roomId);
    const QVariant prior = key.isEmpty() ? QVariant() : store.value(key);
    if (!key.isEmpty())
        store.remove(key);
    // Changed on its own while the Space was muted (here or on another
    // device): that later choice stands.
    if (current != 2)
        return -1;
    bool ok = false;
    const int mode = prior.toInt(&ok);
    if (!prior.isValid() || !ok || mode < 0 || mode > 3)
        return 3;   // no record: follow the account default, as before
    ++*restored;
    return mode;
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
    // Home's filter (RoomListModel::markAllRoomsRead): a room the user marked
    // unread stays unread, and a room with nothing unread gets no receipt.
    // Collected first: marking read can republish the room list.
    const QStringList rooms = m_spaces->roomsInSpace(spaceId);
    QStringList targets;
    if (m_client) {
        for (const QString &roomId : rooms) {
            if (notices::roomWantsMarkRead(m_client->roomInfo(roomId)))
                targets.append(roomId);
        }
    }
    for (const QString &roomId : targets)
        m_roomList->markRoomRead(roomId);
    qCInfo(lcApp) << "space marked read rooms=" << targets.size()
                  << "of" << rooms.size();
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
    if (format.compare(QLatin1String("json"), Qt::CaseInsensitive) == 0)
        return roomexport::Format::Json;
    if (format.compare(QLatin1String("html"), Qt::CaseInsensitive) == 0)
        return roomexport::Format::Html;
    return roomexport::Format::PlainText;
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

QString AppController::startRoomExport(const QUrl &fileUrl,
                                       const QString &format,
                                       const QString &range, int count,
                                       const QString &fromDate,
                                       const QString &toDate,
                                       bool includeAttachments,
                                       int sizeLimitMb,
                                       bool includeEncryptedText)
{
    if (m_currentRoomId.isEmpty())
        return tr("No room is open.");
    if (!fileUrl.isValid() || !fileUrl.isLocalFile())
        return tr("Choose a file on this computer.");

    RoomExportController::Job job;
    job.roomId = m_currentRoomId;
    job.filePath = fileUrl.toLocalFile();
    job.format = exportFormatFor(format);
    job.options = exportOptions();
    // The explicit opt-in, re-asked on every open of the dialog.
    job.options.allowEncryptedPlaintext = includeEncryptedText;

    if (job.format == roomexport::Format::Html && includeAttachments) {
        if (sizeLimitMb < 1 || sizeLimitMb > 2000)
            return tr("Enter a size limit from 1 to 2000 MB.");
        job.options.includeAttachments = true;
        job.options.attachmentBudgetBytes =
            qint64(sizeLimitMb) * 1024 * 1024;
    }

    if (range == QLatin1String("latest")) {
        if (count < 1 || count > 100000)
            return tr("Enter a number of messages from 1 to 100,000.");
        job.range = RoomExportController::Range::Latest;
        job.limit = count;
    } else if (range == QLatin1String("all")) {
        job.range = RoomExportController::Range::All;
    } else if (range == QLatin1String("dates")) {
        const QString from = fromDate.trimmed();
        const QString to = toDate.trimmed();
        const QDate fromDay =
            QDate::fromString(from, QStringLiteral("yyyy-MM-dd"));
        const QDate toDay = QDate::fromString(to, QStringLiteral("yyyy-MM-dd"));
        const bool fromOk = from.isEmpty() || fromDay.isValid();
        const bool toOk = to.isEmpty() || toDay.isValid();
        if ((from.isEmpty() && to.isEmpty()) || !fromOk || !toOk
            || (fromDay.isValid() && toDay.isValid() && fromDay > toDay)) {
            return tr("Use real dates in YYYY-MM-DD format, with From not "
                      "after To.");
        }
        job.range = RoomExportController::Range::Between;
        // Local days, whole: From is the start of its day, To the end of its.
        job.fromMs = fromDay.isValid()
            ? fromDay.startOfDay().toMSecsSinceEpoch() : 0;
        job.toMs = toDay.isValid()
            ? toDay.endOfDay().toMSecsSinceEpoch() : 0;
        job.fromText = from;
        job.toText = to;
    } else {
        if (!m_timeline)
            return tr("No room is open.");
        job.range = RoomExportController::Range::Loaded;
        job.loadedEvents = m_timeline->events();
    }
    return m_roomExport->start(job);
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

bool AppController::navigateBack()
{
    if (m_currentScreen != MainScreen)
        return false;
    if (!m_currentRoomId.isEmpty()) {
        openLobby();
        return true;
    }
    if (m_spaces && !m_spaces->activeSpaceId().isEmpty()) {
        openSpaceHome(QString());
        return true;
    }
    return false;
}

bool AppController::roomHistoryNavigationAllowed() const
{
    // Nothing navigates under the Settings screen, a switch, or a sign-out,
    // including one still waiting (bounded) for its call to be left: the
    // shell is covered by "Signing out…" then, and the session is going.
    return m_currentScreen == MainScreen && !m_accountSwitching && m_client
           && m_client->isLoggedIn() && !(m_auth && m_auth->logoutPending());
}

bool AppController::stepRoomHistory(bool forward)
{
    if (!roomHistoryNavigationAllowed())
        return false;
    // Left and forgotten rooms are skipped; an invite is still a place to go.
    const auto openable = [this](const QString &roomId) {
        const RoomInfo info = m_client->roomInfo(roomId);
        return !info.id.isEmpty() && info.membership != RoomInfo::Left;
    };
    const auto place = forward ? m_roomHistory.forwardPlace(openable)
                               : m_roomHistory.backPlace(openable);
    if (place.roomId.isEmpty())
        return false;
    // A scoped list (space overview) would otherwise stay scoped around a
    // room that is not in it, with no row highlighted. A room the active
    // Space contains keeps the Space (stepping between two of its rooms must
    // not drop the list to Home).
    if (m_spaces && !m_spaces->activeSpaceId().isEmpty()
        && !m_spaces->includesRoom(m_spaces->activeSpaceId(), place.roomId))
        m_spaces->setActiveSpaceId(QString());
    const QScopedValueRollback moving(m_roomHistoryNavigating, true);
    if (m_thread->active())
        m_thread->close();
    openRoom(place.roomId);
    if (place.kind == lightning::RoomHistory::Place::Thread)
        showThread(place.roomId, place.rootEventId);
    else if (place.kind == lightning::RoomHistory::Place::SearchResult)
        m_pagination->jumpToEvent(place.eventId);
    return true;
}

void AppController::recordSearchResult(const QString &roomId,
                                       const QString &eventId)
{
    if (!m_roomHistoryNavigating && roomId == m_currentRoomId
        && !roomId.isEmpty() && !eventId.isEmpty())
        m_roomHistory.visitSearchResult(roomId, eventId);
}

bool AppController::navigateRoomBack()
{
    return stepRoomHistory(false);
}

bool AppController::navigateRoomForward()
{
    return stepRoomHistory(true);
}

bool AppController::navigateRoomHistoryFromKeyboard(QQuickItem *within,
                                                    bool forward)
{
    if (!roomHistoryNavigationAllowed() || hasOpenPopup(within))
        return false;
    return stepRoomHistory(forward);
}

bool AppController::hasOpenPopup(QQuickItem *within) const
{
    return lightning::MouseNavigationFilter::windowHasOpenPopup(
        within ? within->window() : nullptr);
}

void AppController::installSettingsNavigation(QQuickItem *within)
{
    m_settingsNavigation = within;
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
}

void AppController::installMouseNavigation(QQuickItem *within)
{
    if (!within)
        return;
    QQuickWindow *window = within->window();
    if (!window) {
        // Not in a window yet (a Loader can complete an item before parenting
        // it): install when it gets one.
        QPointer<QQuickItem> item(within);
        auto pending = std::make_shared<QMetaObject::Connection>();
        *pending = connect(within, &QQuickItem::windowChanged, this,
                           [this, item, pending](QQuickWindow *w) {
            if (!w || !item)
                return;
            disconnect(*pending);
            installMouseNavigation(item);
        });
        return;
    }
    if (m_mouseNavigation && m_mouseNavigation->window() == window)
        return;
    delete m_mouseNavigation;
    // Owned by this controller (the lambdas use it); the window drops a
    // deleted filter on its own, and the filter forgets a deleted window.
    m_mouseNavigation = new lightning::MouseNavigationFilter(
        window,
        [this] { return roomHistoryNavigationAllowed(); },
        [this](lightning::MouseNavigationFilter::Direction direction) {
            return stepRoomHistory(
                direction == lightning::MouseNavigationFilter::Direction::Forward);
        },
        this);
}

bool AppController::dismissPopupForEscape(QQuickItem *within)
{
    QQuickWindow *window = within ? within->window() : nullptr;
    if (!window || !window->contentItem())
        return false;
    // Every in-scene popup's item is a child of the window's overlay, and its
    // QObject parent is the QQuickPopup itself (QQuickPopupItem's
    // constructor). Class names and properties only: no private Qt API.
    QQuickItem *overlay = nullptr;
    const auto top = window->contentItem()->childItems();
    for (QQuickItem *child : top) {
        if (child->inherits("QQuickOverlay")) {
            overlay = child;
            break;
        }
    }
    if (!overlay)
        return false;
    // Topmost first: higher z, then later in the overlay's child order.
    QList<QQuickItem *> stack = overlay->childItems();
    std::reverse(stack.begin(), stack.end());
    std::stable_sort(stack.begin(), stack.end(),
                     [](const QQuickItem *a, const QQuickItem *b) {
                         return a->z() > b->z();
                     });
    constexpr int kCloseOnEscape = 0x10; // QQuickPopup::CloseOnEscape
    for (QQuickItem *item : std::as_const(stack)) {
        QObject *popup = item->parent();
        if (!item->isVisible() || !popup || !popup->inherits("QQuickPopup")
            || popup->inherits("QQuickToolTip")
            || !popup->property("visible").toBool())
            continue;
        const bool escapable =
            (popup->property("closePolicy").toInt() & kCloseOnEscape) != 0;
        if (escapable) {
            if (popup->inherits("QQuickDialog"))
                QMetaObject::invokeMethod(popup, "reject");
            else
                QMetaObject::invokeMethod(popup, "close");
            return true;
        }
        if (popup->property("modal").toBool())
            return true;
    }
    return false;
}

void AppController::requestComposerFocus(const QString &roomId)
{
    // Only for the room that is actually open: a late request for a room the
    // user has already left must not pull the keyboard anywhere.
    if (roomId.isEmpty() || roomId != m_currentRoomId)
        return;
    Q_EMIT composerFocusRequested(roomId);
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

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
namespace {
const QString kDemoVerificationFlowId = QStringLiteral("demo-verification");
} // namespace

void AppController::stageDemoVerification(const QString &otherDevice,
                                          const QVariantList &emojis)
{
    if (!m_screenshotDemoActive)
        return;
    if (emojis.isEmpty()) {
        // Only ever clears the staged flow, never a real one.
        if (m_verificationFlowId != kDemoVerificationFlowId)
            return;
        m_verificationFlowId.clear();
        m_verificationState.clear();
        m_verificationEmojis.clear();
        Q_EMIT verificationStateChanged();
        return;
    }
    m_verificationFlowId = kDemoVerificationFlowId;
    m_verificationOtherUser = m_client ? m_client->currentUserId() : QString();
    m_verificationOtherDevice = otherDevice;
    m_verificationIsSelf = true;
    m_verificationEmojis = emojis;
    m_verificationDecimals.clear();
    m_verificationState = QStringLiteral("sas_ready");
    Q_EMIT verificationStateChanged();
}
#endif

void AppController::cancelVerification()
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // The staged demo flow has no SDK side; Cancel just clears it.
    if (m_verificationFlowId == kDemoVerificationFlowId) {
        stageDemoVerification(QString(), {});
        return;
    }
#endif
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
    // LIGHTNING_SWITCH_TRACE target 2: the open room's timeline has filled.
    // QML's value also drops when the window loses focus or the chat shell is
    // hidden, which is not a fill: only an edge seen with the main window
    // active on the main screen counts (SwitchTrace.h, target 2). The trace
    // keeps the first one after its login only.
    if (!hydrating && !m_currentRoomId.isEmpty()
        && lightning::switchtrace::active() && m_currentScreen == MainScreen) {
        // isActive() is the very predicate QML's Window.active reads.
        // mainWindow() finds the window if focus tracking has not yet.
        QQuickWindow *window = mainWindow();
        if (window && window->isActive())
            lightning::switchtrace::markRoomInteractive();
    }
}

void AppController::armSwitchTraceFirstFrame()
{
    disarmSwitchTraceFirstFrame();
    if (!m_roomList)
        return;
    // The frame after the room list first has rows. frameSwapped is emitted
    // on the render thread; `this` as the context queues it here, so the
    // stamp is when the GUI thread learns of the swap.
    const auto onRows = [this] {
        if (!lightning::switchtrace::active()) {
            disarmSwitchTraceFirstFrame();
            return;
        }
        if (m_roomList->rowCount() <= 0 || m_switchTraceFrameConnection)
            return;
        // mainWindow(), not m_mainWindow: the member is filled lazily by
        // focus tracking and may still be empty this early in a launch.
        QQuickWindow *window = mainWindow();
        if (!window)
            return;
        QObject::disconnect(m_switchTraceRowsConnection);
        QObject::disconnect(m_switchTraceResetConnection);
        m_switchTraceFrameConnection = connect(
            window, &QQuickWindow::frameSwapped, this, [this] {
                disarmSwitchTraceFirstFrame();
                lightning::switchtrace::markFirstFrame();
            });
    };
    m_switchTraceRowsConnection =
        connect(m_roomList.get(), &QAbstractItemModel::rowsInserted, this, onRows);
    m_switchTraceResetConnection =
        connect(m_roomList.get(), &QAbstractItemModel::modelReset, this, onRows);
}

void AppController::disarmSwitchTraceFirstFrame()
{
    QObject::disconnect(m_switchTraceRowsConnection);
    QObject::disconnect(m_switchTraceResetConnection);
    QObject::disconnect(m_switchTraceFrameConnection);
    m_switchTraceRowsConnection = {};
    m_switchTraceResetConnection = {};
    m_switchTraceFrameConnection = {};
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
                                           const QString &homeserver_)
{
    // The repair form signs in to the account that failed, so its server is
    // that account's RECORDED one, never what the emitter had at hand: a
    // running handle's server, or nothing, which left the form on the
    // last-used server (a matrix.org OAuth account was offered a password
    // form for another homeserver, 2026-10-01). Only without a record does
    // the given server stand.
    QString homeserver = homeserver_;
    if (!userId.isEmpty() && m_settings) {
        const QString recorded = m_settings->accountRecord(userId)
                                     .value(QStringLiteral("homeserver"))
                                     .toString();
        if (!recorded.isEmpty())
            homeserver = recorded;
    }
    // Before the no-change return: a failure that re-raises the card already
    // showing still came with the card, and the error it sends next in the
    // same call is that card's own text (see the loginFailed handler).
    if (!reasonCode.isEmpty()) {
        m_localSessionRaiseInFlight = true;
        const quint64 serial = ++m_localSessionRaiseSerial;
        QTimer::singleShot(0, this, [this, serial] {
            if (serial == m_localSessionRaiseSerial)
                m_localSessionRaiseInFlight = false;
        });
    }
    if (m_localSessionFailureReason == reasonCode
        && m_localSessionFailureUserId == userId
        && m_localSessionFailureHomeserver == homeserver) {
        return;
    }
    // A retry belongs to the removal_incomplete card only; any other outcome
    // (a sign-in that succeeded, another failure) ends it.
    if (reasonCode != QLatin1String("removal_incomplete")) {
        m_removalRetryIdentity = {};
        m_removalLeftovers.clear();
    }
    m_localSessionFailureReason = reasonCode;
    m_localSessionFailureUserId = userId;
    m_localSessionFailureHomeserver = homeserver;
    Q_EMIT localSessionFailureChanged();
}

bool AppController::signInErrorExplainedByCard() const
{
    return !m_cardExplainedError.isEmpty() && m_auth
           && m_auth->lastError() == m_cardExplainedError;
}

bool AppController::localResetHelpsFor(const QString &reasonCode) const
{
    // Not gated on ENABLE_RUST_SDK_BACKEND: RustSessionPolicy is pure
    // classification compiled into every configuration, and whether a reset
    // can repair a failure does not depend on the backend.
    return matrix::rust_session::suggestsLocalResetForCode(reasonCode);
}

void AppController::refreshKeyringUnavailable()
{
    bool unavailable = false;
    // The demo store is in memory and cannot fail. Where no native store
    // exists, the plaintext store answers for itself and a miss is a fact, so
    // this stays false there.
    if (!m_screenshotDemo && m_settings) {
        QStringList ask = m_settings->savedAccountUserIds();
        // A native store's answer is the backend's, and every lookup in a
        // locked collection can raise an unlock prompt (a dismissed one reads
        // as "no such item", so a loop would never stop early): one read.
        if (!m_settings->secretMissesAreInconclusive() && !ask.isEmpty()) {
            const QString active = m_settings->activeAccountUserId();
            ask = QStringList{active.isEmpty() ? ask.first() : active};
        }
        for (const QString &uid : std::as_const(ask)) {
            // Read first: the unavailable flag reflects the most recent read.
            if (m_settings->accessTokenFor(uid).isEmpty()
                && m_settings->secretBackendUnavailable()) {
                unavailable = true;
                break;
            }
        }
    }
    setKeyringUnavailable(unavailable);
}

void AppController::setKeyringUnavailable(bool unavailable)
{
    if (unavailable == m_keyringUnavailable)
        return;
    m_keyringUnavailable = unavailable;
    qCInfo(lcApp) << (unavailable
                          ? "keyring cannot answer for a saved sign-in"
                          : "keyring answers for every saved sign-in")
                  << "backend=" << m_settings->secretBackendName();
    Q_EMIT keyringUnavailableChanged();
}

void AppController::explainUnrestoredLaunch()
{
    const QString active = m_settings->activeAccountUserId();
    if (active.isEmpty()) {
        refreshKeyringUnavailable();
        return;
    }
    // hasSession() has just read the active account's token and found none;
    // whether the store could answer that read is what this launch needs.
    setKeyringUnavailable(m_settings->secretBackendUnavailable());
    if (!m_keyringUnavailable)
        noteKeyringLostSession(active);
}

void AppController::noteKeyringLostSession(const QString &userId)
{
    // The adoption check found this install's copied sign-in to be another
    // install's device and removed it: say that, not "the keyring lost it".
    if (m_settings->keyringItemsDeclinedForOtherInstall(userId)) {
        setLocalSessionFailure(
            QStringLiteral("keyring_other_install"), userId,
            m_settings->accountRecord(userId).value(QStringLiteral("homeserver")).toString());
        return;
    }
    // Only a sign-in this install saved for the recorded device, and never
    // removed: anything else missing may be a real sign-out, or someone
    // else's, and keeps the card it had.
    if (!m_settings->secretsWrittenHereForRecordedDevice(userId))
        return;
    qCWarning(lcApp) << "the secret store no longer returns a sign-in this "
                        "install saved; the store stays, nothing is reset"
                     << "slug=" << matrix::app_data::safeUserSlug(userId)
                     << "backend=" << m_settings->secretBackendName();
    setLocalSessionFailure(
        QStringLiteral("keyring_lost_session"), userId,
        m_settings->accountRecord(userId).value(QStringLiteral("homeserver")).toString());
}

bool AppController::retryKeyring()
{
    if (m_screenshotDemo || !m_settings)
        return false;
    // Replaced only when it is a stand-in: the plaintext store substituted
    // for a native one that did not answer at startup, or a native store that
    // could not open. A native store that opened and is merely locked stays;
    // reading through it again below is the retry, and may prompt to unlock.
    const bool standIn = !m_secretStore
        || m_secretStore->missesAreInconclusive()
        || !m_secretStore->isAvailable();
    if (standIn) {
        // Probes the Secret Service synchronously, as the startup did.
        std::unique_ptr<SecretStore> candidate =
            makeSecretStore(m_secretStoreFactory, this);
        if (candidate && candidate->isAvailable()
            && !candidate->missesAreInconclusive()) {
            qCInfo(lcApp) << "secret store answers again; replacing the stand-in"
                          << "backend=" << candidate->backendName();
            // Wired before it is current, as at startup: setSecretStore()
            // scopes it and moves into it any sign-in the stand-in took
            // meanwhile. The stand-in is retired, not destroyed.
            m_settings->setSecretStore(candidate.get());
            if (m_secretStore)
                m_retiredSecretStores.push_back(std::move(m_secretStore));
            m_secretStore = std::move(candidate);
        }
        // Otherwise the candidate was never wired to anything and goes here.
    }
    refreshKeyringUnavailable();
    if (m_keyringUnavailable) {
        Q_EMIT errorReported(
            tr("Lightning can't read this device's saved sign-ins right now — "
               "the system keyring is locked or unavailable. Unlock it and "
               "try again."));
        return false;
    }
    Q_EMIT errorReported(QString{});
    // Restore as a launch with a saved session does. Only from the login
    // screen and with nothing else in flight: a signed-in session (an
    // account added while the keyring was away) is left alone.
    if (m_currentScreen == LoginScreen && !m_client->isLoggedIn()
        && !m_accountSwitching && !(m_auth && m_auth->isLoggingIn())) {
        if (m_settings->hasSession()) {
            setCurrentScreen(BootScreen);
            if (!m_client->restoreSession())
                setCurrentScreen(LoginScreen);
            Q_EMIT rustDeviceIdChanged();
        } else if (m_localSessionFailureReason
                   == QLatin1String("keyring_lost_session")) {
            Q_EMIT errorReported(
                tr("The keyring still does not return this account's saved "
                   "sign-in."));
        }
    }
    return true;
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

QString AppController::logExportDiagnostics() const
{
    QString out;
    out += QStringLiteral("Lightning log export: diagnostics\n");
    out += QStringLiteral("generated_utc: %1\n")
               .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    // version, source commit, target, build type, backends, artifact kind.
    out += lightning::logexport::buildInfo();
    out += QStringLiteral("package: %1\n")
               .arg(lightning::update::installTypeId(
                   lightning::update::detectInstallType()));
    out += QStringLiteral("os: %1\n").arg(QSysInfo::prettyProductName());
    out += QStringLiteral("kernel: %1 %2\n")
               .arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());
    out += QStringLiteral("cpu_arch: %1\n").arg(QSysInfo::currentCpuArchitecture());
    out += QStringLiteral("desktop: %1\n")
               .arg(qEnvironmentVariable("XDG_CURRENT_DESKTOP"));
    out += QStringLiteral("session_type: %1\n")
               .arg(qEnvironmentVariable("XDG_SESSION_TYPE"));
    out += QStringLiteral("qt_platform_plugin: %1\n")
               .arg(QGuiApplication::platformName());
    out += QStringLiteral("qt_runtime: %1\nqt_build: %2\n")
               .arg(QString::fromLatin1(qVersion()),
                    QStringLiteral(QT_VERSION_STR));
#ifdef HAVE_LIGHTNING_WEBRTC
    out += QStringLiteral("call_media_engine_built_in: yes\n");
    if (qEnvironmentVariableIsSet("LIGHTNING_DISABLE_WEBRTC")) {
        out += QStringLiteral("call_media_engine: disabled by environment\n");
    } else {
        QString whyOneToOne;
        const bool oneToOne = GstCallMediaBackend::runtimeAvailable(&whyOneToOne);
        QString whySfu;
        const bool sfu = SfuMediaEngine::runtimeAvailable(&whySfu);
        out += QStringLiteral("gstreamer: %1\n").arg(lightning::gst::versionString());
        out += QStringLiteral("call_engine_1to1: %1\n")
                   .arg(oneToOne ? QStringLiteral("available")
                                 : QStringLiteral("unavailable (%1)").arg(whyOneToOne));
        out += QStringLiteral("call_engine_group: %1\n")
                   .arg(sfu ? QStringLiteral("available")
                            : QStringLiteral("unavailable (%1)").arg(whySfu));
        out += QStringLiteral("camera_compressed_chain: %1\n")
                   .arg(SfuMediaEngine::jpegCameraChainAvailable()
                            ? QStringLiteral("available")
                            : QStringLiteral("unavailable"));
    }
#else
    out += QStringLiteral("call_media_engine_built_in: no\n");
#endif
    out += QStringLiteral("\n");
    out += sessionDiagnosticsText();
    return out;
}

QStringList AppController::logExportLiterals() const
{
    QStringList out;
    out << qEnvironmentVariable("USER") << qEnvironmentVariable("USERNAME")
        << qEnvironmentVariable("LOGNAME") << QDir::home().dirName()
        << QDir::homePath() << QSysInfo::machineHostName();
    if (m_settings) {
        for (const QString &uid : m_settings->savedAccountUserIds()) {
            out << uid << matrix::app_data::safeUserSlug(uid);
            // "@local:server" -> "local"
            const int colon = uid.indexOf(QLatin1Char(':'));
            if (uid.startsWith(QLatin1Char('@')) && colon > 1)
                out << uid.mid(1, colon - 1);
        }
        out << m_settings->activeAccountUserId();
    }
    out.removeAll(QString());
    return out;
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
    // LIGHTNING_SWITCH_TRACE: the 30 s deadline for the two targets starts
    // here, and S10 is this whole body.
    lightning::switchtrace::markLoginSucceeded();
    const lightning::switchtrace::StepScope traceS10(
        lightning::switchtrace::Step::S10);
    if (lightning::switchtrace::active())
        armSwitchTraceFirstFrame();
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
    refreshTrayAccountLabel();
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
        if (m_pushRules)
            m_pushRules->resetForAccountChange();
        // A running close, delete or Space moderation, and the server-admin
        // answer, belong to the previous account.
        if (m_roomClosure)
            m_roomClosure->resetForAccountChange();
        if (m_spaceModeration)
            m_spaceModeration->resetForAccountChange();
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
    m_accounts->clearSessionRevoked(uid);
    setLocalRustResetRequired(false);
    clearLocalSessionFailure();
    m_switchFallbackUserId.clear();
    setAccountSwitching(false);
    m_client->startSync();
    // Cache the account's own display name / avatar for the switcher UI.
    m_client->fetchUserProfile(uid);
    m_ownProfileRefreshMs = QDateTime::currentMSecsSinceEpoch();
    // Offer "index all messages now?" once per account, after a sign-in made
    // here — never for a restore, which also disarms an offer armed for the
    // account signed in before it.
    // Not in the screenshot demo, whose fictional accounts are staged.
    m_messageSearch->offerIndexAllAfterSignIn(
        uid, m_auth->lastSignInWasInteractive() && !m_screenshotDemo);
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

bool AppController::closeHidesWindow(bool quitIntended) const
{
    // The icon that exists, not the platform's word that one could: a window
    // hidden into a tray icon that was never created has no way back.
    return closebehavior::closeHidesWindow(
        closebehavior::kIsMac, m_settings && m_settings->closeToTray(),
        m_trayAvailable && m_tray.enabled(), quitIntended);
}

bool AppController::activationRestoresWindow(bool hiddenByClose,
                                             bool windowHidden) const
{
    return closebehavior::activationRestoresWindow(
        closebehavior::kIsMac, hiddenByClose, windowHidden);
}

bool AppController::eventFilter(QObject *watched, QEvent *event)
{
    if (m_settingsNavigation && m_settingsNavigation->isVisible()
        && watched == m_settingsNavigation->window()
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)
        && m_settingsNavigation->property("dialogShortcutsEnabled").toBool()
        && !hasOpenPopup(m_settingsNavigation)) {
        auto *focused = m_settingsNavigation->window()->activeFocusItem();
        bool inside = false;
        for (auto *item = focused; item; item = item->parentItem()) {
            if (item == m_settingsNavigation) {
                inside = true;
                break;
            }
        }
        if (inside && focused
            && !focused->objectName().startsWith(QStringLiteral("shortcutCaptureSink_"))) {
            const auto *key = static_cast<QKeyEvent *>(event);
            const bool copying = key->matches(QKeySequence::Copy)
                && !focused->property("selectedText").toString().isEmpty();
            const QString action = m_shortcuts->settingsActionForKey(
                key->key(), int(key->modifiers()), copying);
            if (!action.isEmpty()) {
                event->accept();
                if (event->type() == QEvent::KeyPress)
                    Q_EMIT settingsDialogActionRequested(action);
                return true;
            }
        }
    }
#ifdef Q_OS_MACOS
    if (event->type() == QEvent::Quit && watched == QCoreApplication::instance()
        && event->spontaneous()) {
        // The platform asked us to terminate; the window's close handler runs
        // next and must let it through.
        Q_EMIT applicationQuitIntended();
    }
#endif
    return QObject::eventFilter(watched, event);
}

void AppController::refreshTrayAvailability()
{
    const bool available = TrayIcon::platformSupportsTray();
    if (available == m_trayAvailable)
        return;
    m_trayAvailable = available;
    Q_EMIT trayAvailableChanged();
}

void AppController::refreshTrayState()
{
    // A flip emits trayAvailableChanged, which applies the state itself.
    refreshTrayAvailability();
    applyTrayState();
}

void AppController::applyTrayState()
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
    m_tray.setEnabled(wanted && m_trayAvailable);
    if (m_tray.enabled()) {
        refreshTrayAccountLabel();
        // A tray turned on while messages are already waiting must open with
        // its badge, so the state is pushed here as well as on every change.
        refreshTrayUnread();
    }
}

void AppController::refreshTrayAccountLabel()
{
    // Follows sign-in, switch and sign-out; empty while signed out. A display
    // name or localpart: the tooltip is on screen for anyone to read.
    if (!m_tray.enabled())
        return;
    const QString displayName = m_accounts && !m_lastSessionUserId.isEmpty()
        ? m_accounts->account(m_lastSessionUserId)
              .value(QStringLiteral("displayName")).toString()
        : QString();
    m_tray.setAccountLabel(
        notices::trayAccountLabel(m_lastSessionUserId, displayName));
}

void AppController::refreshTrayUnread()
{
    // Not gated on the tray: this walk also withdraws notifications for rooms
    // that are no longer unread, and feeds the Dock / taskbar badge. Only the
    // icon write needs the icon.
    if (!m_client)
        return;
    // Derived from the local rooms() snapshot with the rail's rule (GitHub
    // #18: a muted room is neither a dot nor a count), through the one
    // function every unread surface reads (a "Mentions" mode this device
    // cannot verify on the server is a dot, never a count).
    // Live accounts (Labs): cards this account raised while it ran in the
    // background live in its own table, and only this walk can still
    // withdraw them now that it is in front. Empty while live accounts are
    // off.
    const QString liveForeground =
        m_sessionPool ? m_settings->activeAccountUserId() : QString();
    const roomunread::Summary unread = roomunread::summarize(
        m_client->rooms(),
        [this](const QString &id) {
            return m_settings ? m_settings->roomUnreadMode(id)
                              : roomunread::kModeAllMessages;
        },
        [this, &liveForeground](const RoomInfo &room, bool roomUnread) {
            // A room that is no longer unread withdraws its notifications, a
            // read in another client (cleared via sync) included. The manager
            // waits until the room has read as unread since its latest card:
            // a card can come before these fields catch up. Cheap for rooms
            // without a card.
            if (m_notifications)
                m_notifications->observeRoomUnread(room.id, roomUnread);
            if (m_notifications && !liveForeground.isEmpty())
                m_notifications->observeRoomUnreadFor(liveForeground, room.id,
                                                      roomUnread);
        });
    int total = unread.total;
    bool anyUnread = unread.any;
    // Live accounts (Labs): the background accounts' counts tables, by the
    // same rule (RoomUnreadRule, applied by each host), the active one
    // excluded. Their mentions add to the number; any activity lights it.
    if (m_accountActivity) {
        const AccountActivityModel::Totals others =
            m_accountActivity->totalsExcluding(
                m_settings->activeAccountUserId());
        total += others.mentions;
        anyUnread = anyUnread || others.accountsWithActivity > 0;
    }
    if (m_tray.enabled())
        m_tray.setUnread(total, anyUnread);
    // The Dock (macOS), taskbar (Windows) and launcher (KDE, Unity) badge,
    // whether or not a tray icon exists. Written only on change.
    if (total != m_lastBadgeNumber) {
        m_lastBadgeNumber = total;
        if (auto *guiApp =
                qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
            lightning::setApplicationBadge(*guiApp, qint64(std::max(0, total)));
    }
}

void AppController::onLoggedOut()
{
    m_currentRoomId.clear();
    ++m_currentRoomOpenSerial;
    m_currentRoomListed = false;
    // The roster cache died with the session (detach or logout); the next
    // account — or a re-login — must hydrate rooms afresh.
    m_memberHydratedRooms.clear();
    // Same scope, same reason: the next account's rooms are not these rooms.
    m_bridgeReadRooms.clear();
    // Closed on both paths, switch included, so a late answer cannot write
    // into the next account's file.
    m_bridgeLabels.close();
    // No playback (or decrypted-media handle) survives sign-out, nor what
    // the session learned about its files (tags, cover art).
    m_voicePlayback->clearSession();
    resetVoicePrefetch();
    m_playback->stopAll();
    // Where each video stopped belongs to this account. AFTER stopAll():
    // a playing card remembers its position synchronously on that stop.
    m_playback->forgetResumePositions();
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
    if (m_revokedDetach) {
        // The server ended this session, not the user: nothing of the
        // account is deleted and no other account is switched to.
        // endRevokedSession() shows the login screen once the detach returns.
        return;
    }
    clearQueuedAccountSwitch("sign-out");
    // The signed-out account's saved room positions and selections go with
    // it (they are kept per account across switches).
    if (m_pagination && !m_lastSessionUserId.isEmpty())
        m_pagination->forgetAccount(m_lastSessionUserId);
    // Its hidden packs, custom-emoji recents and favourites too.
    if (m_stickers && !m_lastSessionUserId.isEmpty())
        m_stickers->forgetAccount(m_lastSessionUserId);
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
        // Personal chat backgrounds under the canonical root, where the
        // controller writes them. The client removed them under the recorded
        // root too (finishSignOut); this one does not depend on that identity
        // being valid.
        const QString backgroundsDir = matrix::app_data::personalBackgroundsDir(
            matrix::app_data::accountRoot(m_lastSessionUserId));
        if (matrix::app_data::removeAppDataDir(backgroundsDir)
            == matrix::app_data::DirRemoval::Failed) {
            qCWarning(lcApp) << "personal backgrounds sign-out cleanup FAILED slug="
                             << matrix::app_data::safeUserSlug(m_lastSessionUserId);
        }
        signOutAccountResidue(m_lastSessionUserId);
    }
    // Second half of removeAccount() for the active account. Runs before
    // clearActiveUser() and the fallback loop, so the account being removed
    // can never be chosen to continue with.
    // Live accounts (Labs) only: the account whose store the registry would
    // not let go (its retirement overran the budget). Its record stays, it
    // is never continued with, and the user is told below. Always empty
    // without live accounts, where the registry refuses nothing.
    QString removalRefusedUserId;
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
            bool refused = false;
            if (resolved)
                removeAccountLocalState(identity, &refused);
            else
                qCWarning(lcApp) << "account removal completed without a "
                                    "resolvable local layout — record and "
                                    "secrets only";
            if (refused) {
                // Nothing was deleted: the record that names the store stays
                // with it (§6), and a later removal finishes the job.
                removalRefusedUserId = target;
                if (m_sessionPool) {
                    noteLiveAccountsStep(QStringLiteral("removal:refused"),
                                         target);
                    reopenLiveAccountActivity(target);
                }
            } else {
                m_accounts->removeAccount(target); // record + secrets
            }
        } else {
            qCWarning(lcApp)
                << "pending account removal abandoned: the session that "
                   "ended is not the account that was being removed";
        }
    }
    m_accounts->clearActiveUser();
    m_lastSessionUserId.clear();
    refreshTrayAccountLabel();
    m_addAccountReturnTo.clear();
    m_backgroundRestore = false;
    Q_EMIT errorReported(QString{});
    // After the blanket clear, so it is what the user reads.
    if (!removalRefusedUserId.isEmpty()) {
        Q_EMIT errorReported(
            tr("Removal did not finish: the account's data is still in use. "
               "Nothing was deleted."));
    }
    // A genuine sign-out/removal ends whatever the last connection state
    // was; nothing else here re-derives it (the client's own
    // connectionStateChanged may not fire again for a session that is
    // already gone), so a stale "Error" from before the sign-out would
    // otherwise sit in the status strip indefinitely. switchToAccount()
    // below reports its own state as soon as it starts.
    setConnectionStatus(tr("Not connected"));
    // When other accounts remain, continue with the most recently added one.
    const QStringList remaining = m_settings->savedAccountUserIds();
    // An empty token read is not evidence an account is gone: with a locked
    // keyring every account reads tokenless.
    bool anyUnreadable = false;
    for (auto it = remaining.crbegin(); it != remaining.crend(); ++it) {
        if (*it == removalRefusedUserId)
            continue;
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
        case SignInState::KeyringLost:
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
    // A click that arrived while this switch was running is the user's
    // latest intent: run it now, on the next turn, so the completion that
    // cleared the flag finishes its own fan-out first. switchToAccount()
    // re-applies every guard (already active, sign-out pending, unknown).
    if (!switching && !m_queuedSwitchTarget.isEmpty()) {
        const QString next = std::exchange(m_queuedSwitchTarget, QString{});
        // A sign-out, removal or add-account between now and the next turn
        // retires it (clearQueuedAccountSwitch bumps the epoch).
        const quint64 epoch = m_queuedSwitchEpoch;
        QTimer::singleShot(0, this, [this, next, epoch] {
            if (epoch == m_queuedSwitchEpoch)
                switchToAccount(next);
        });
    }
}

void AppController::clearQueuedAccountSwitch(const char *reason)
{
    // A click queued behind a running switch was intent for the session list
    // as it was; a sign-out, a removal or a new account changes that list,
    // and the stale click would switch away from where the user now is.
    ++m_queuedSwitchEpoch;
    if (m_queuedSwitchTarget.isEmpty())
        return;
    qCInfo(lcApp) << "queued account switch dropped reason=" << reason;
    m_queuedSwitchTarget.clear();
}

void AppController::resetSessionNotificationState()
{
    // Pending "call started" withdrawals of the old session do nothing now.
    m_callNoticeGeneration.clear();
    m_notificationModeRetryReads.clear();
    m_notificationModeWritesInFlight.clear();
    m_notificationModeServerSeen.clear();
    m_notificationModeSentModes.clear();
    m_retryNotificationModesWhenSynced = false;
    m_notifiedEventIds.clear();
    m_ringLimiter.clear();
    m_joinedRoomIds.clear();
    m_callVideoIntent.clear();
    m_verificationNoticeKey.clear();
    m_verificationNotifiedFlow.clear();
    m_activitySeeded = false;
    m_activitySeedAttempts = 0;
    m_activitySeedRetryTimer.stop();
    // Checks run for the previous account must not read as this one's.
    if (m_notificationTroubleshooter)
        m_notificationTroubleshooter->reset();
}

void AppController::clearCrossAccountCaches()
{
    // Runs twice per switch (here and from onLoginSucceeded); the trace adds
    // both runs and counts them.
    const lightning::switchtrace::StepScope traceS6(
        lightning::switchtrace::Step::S6);
    // Playback stops before the decrypted media files are wiped, so no
    // player holds an open handle into the previous account's cache; the
    // previous account's file tags and cover art are forgotten with it.
    m_voicePlayback->clearSession();
    m_playback->stopAll();
    // The previous account's video positions go with it; after stopAll(),
    // which makes a playing card remember its position synchronously.
    m_playback->forgetResumePositions();
    resetVoicePrefetch();
    m_mediaBridge->clear();
    // Finished and failed download notices belong to the previous account.
    if (m_downloads)
        m_downloads->clear();
    // MediaBridge::clear() drops in-flight requests without a terminal
    // emission, so the claim sets would otherwise strand their keys.
    m_pendingStarKeys.clear();
    m_pendingCopyKeys.clear();
    m_notifications->clearPendingFor(QString());
    // Anything still on screen must not be attributed to the next account.
    m_notifications->setAccountUserId(QString());
    m_inviteNotices.clear();
    m_inviteNoticeTimer.stop();
    // The bell, its seed and the notification-mode bookkeeping are the
    // previous account's. A switch also clears the bell through loggedOut;
    // an add-account sign-in sees no loggedOut, so it is cleared here, or the
    // new account would show (and later save) the previous one's mentions,
    // seen marker and keywords, and never be seeded.
    m_activity->clear();
    resetSessionNotificationState();
    m_roomHistory.clear();
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
    m_verificationRole.clear();
    m_verificationCancelReason.clear();
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

void AppController::endRevokedSession(const QString &userId)
{
    // The client names the running session's own account, so this is exact.
    // Anything else (a switch or a sign-out got there first) ends nothing and
    // moves no screen; the handler has already marked the account.
    const QString running = m_client->currentUserId();
    if (!m_client->isLoggedIn() || userId.isEmpty() || userId != running)
        return;
    qCInfo(lcApp) << "session revoked by the server — ending it locally"
                  << "slug=" << matrix::app_data::safeUserSlug(running)
                  << "screen=" << int(m_currentScreen);
    // Leave the call and the room first, as a switch does: the retraction
    // needs the client, and no composer target may outlive the session.
    if (m_groupCall)
        m_groupCall->leave();
    setCurrentRoomId(QString{});
    m_roomHistory.clear();
    m_roomInfo->setRoomId(QString{});
    m_composer->setRoomId({});
    // A local end only: no server logout (the server already ended it), and
    // the record, token, crypto store and every per-account file stay.
    m_revokedDetach = true;
    const bool detached = m_client->detachSession();
    m_revokedDetach = false;
    if (!detached) {
        // A sign-out is finishing; its completion owns what happens next.
        qCWarning(lcApp) << "revoked session not detached: a sign-out is "
                            "still in flight";
        return;
    }
    // A later failed add-account must not restore this session behind the
    // card: the server has already refused it.
    if (m_addAccountReturnTo == running)
        m_addAccountReturnTo.clear();
    // Sync's own "no longer authorized" error is what the card now says.
    Q_EMIT errorReported(QString{});
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

void AppController::processInviteNotices()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString &roomId : m_inviteNotices.takeResolved(now)) {
        m_activity->inviteResolved(roomId);
        // Declined, rescinded or rejected elsewhere: the card must not stay
        // behind to open a room the user is no longer invited to.
        m_notifications->closeRoomNotifications(roomId);
    }
    // The account's push rules: master mute and "Invitations to you: Off"
    // silence this card as they do on every other client. Unknown rules keep
    // the local behaviour.
    const bool rulesKnown = m_pushRules && m_pushRules->loaded();
    const int inviteRule = m_pushRules ? m_pushRules->eventInvites() : -1;
    const bool allowed = notices::inviteNoticeAllowed(
        m_settings->notificationsEnabled(), rulesKnown,
        m_pushRules && m_pushRules->masterMute(), inviteRule);
    for (const QString &roomId : m_inviteNotices.takeDue(now)) {
        // Read now, not when first seen: the settle is what lets the room's
        // real name replace the SDK's "Empty Room".
        const RoomInfo room = m_client->roomInfo(roomId);
        if (room.membership != RoomInfo::Invited
            || !NotificationManager::shouldNotifyInvite(true, false, allowed))
            continue;
        const int preview = noticePreviewFor(room.id);
        // Sender-chosen text, flattened and bounded like a caller's name; the
        // localpart when no name is known, never the full MXID.
        const QString inviter = room.inviterUserId.isEmpty()
            ? QString()
            : CallController::presentableCallerName(room.inviterUserId,
                                                    room.inviterDisplayName);
        m_notifications->showGeneric(
            tr("Room invitation"),
            notices::inviteBody(preview, inviter, room.name, room.isDirect),
            room.id,
            notices::showsInviteAvatar(preview,
                                       m_accountPrefs->inviteAvatarsAllowed())
                ? m_roomList->findRoom(room.id)
                      .value(QStringLiteral("avatarUrl")).toString()
                : QString(),
            notices::inviteSound(m_settings->notificationSound(), rulesKnown,
                                 inviteRule));
    }
    const qint64 wait = m_inviteNotices.nextDueInMs(now);
    if (wait < 0)
        m_inviteNoticeTimer.stop();
    else
        m_inviteNoticeTimer.start(int(std::min<qint64>(wait, 60000)));
}

void AppController::switchToAccount(const QString &userId)
{
    const QString target = userId.trimmed();
    if (target.isEmpty())
        return;
    // LIGHTNING_SWITCH_TRACE T0: the accepted request, before any release,
    // queueing or keyring read (a no-op unless the trace is enabled).
    lightning::switchtrace::noteIntent(target);
    // The newest click wins, as it already does while a live (Labs) account
    // is being released: a target chosen while a switch is running is
    // remembered (replacing any earlier one) and runs when it completes.
    // It used to be dropped, so A -> B -> C during B's restore ended at B.
    if (m_accountSwitching) {
        m_queuedSwitchTarget = target;
        qCInfo(lcApp) << "account switch queued behind the running one"
                      << "to=" << matrix::app_data::safeUserSlug(target);
        return;
    }
    // A sign-out waiting for its call to be left acts on the client's
    // current session when it runs: switching now would sign out, and delete
    // the store of, the account switched to.
    if (m_auth->logoutPending()) {
        qCInfo(lcApp) << "account switch refused: a sign-out is in progress";
        return;
    }
    if (target == m_settings->activeAccountUserId() && m_client->isLoggedIn())
        return;
    if (!m_settings->hasSavedAccount(target)) {
        Q_EMIT errorReported(tr("That account is not signed in on this device."));
        return;
    }
    // Live accounts (Labs): an account running in the background holds its
    // own store. It is freed first, asynchronously, while this account stays
    // in front and usable; today's switch below follows once it is free.
    if (m_sessionPool && beginLiveAccountSwitch(target))
        return;
    // Warm account adoption (master prompt Part 4 §4): the target runs as a
    // background host that can hand its live session over. The foreground is
    // unbound below exactly as for a cold switch; the keyring is not read (the
    // host's custody is settled, so the session it runs IS the saved sign-in)
    // and the handle is adopted instead of a store being restored.
    const bool adopting = std::exchange(m_adoptTarget, QString{}) == target
        && m_sessionPool && m_client->supportsLiveAdoption()
        && m_sessionPool->canAdopt(target);
    // LIGHTNING_SWITCH_TRACE: every stamp below is a no-op unless it is set.
    namespace trace = lightning::switchtrace;
    if (trace::enabled())
        trace::begin(m_settings->activeAccountUserId(), target,
                     adopting ? trace::Path::Adopt : trace::Path::Cold);
    const SignInState signIn = adopting ? SignInState::Usable : [&] {
        const trace::StepScope traceS1(trace::Step::S1);
        return signInStateFor(target);
    }();
    if (signIn != SignInState::Usable)
        trace::finish(trace::Outcome::Refused);
    switch (signIn) {
    case SignInState::Usable:
        break;
    case SignInState::Gone:
        Q_EMIT errorReported(
            tr("That account's sign-in has expired. Sign in to it again."));
        return;
    case SignInState::KeyringLost:
        // Not "expired": the server session is most likely alive. The login
        // screen's card explains, and a password sign-in continues as the
        // same device (RustSdkMatrixClient::login()), keeping its keys.
        if (!m_client->isLoggedIn())
            noteKeyringLostSession(target);
        Q_EMIT errorReported(
            tr("Your system keyring no longer returns that account's saved "
               "sign-in. Nothing was deleted: sign in to it again to continue "
               "on this device."));
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
    trace::stepBegin(trace::Step::S2);
    if (m_groupCall)
        m_groupCall->leave();

    // Leave the room and thread before detaching so no composer target,
    // pending send, or open thread survives into the next account.
    setCurrentRoomId(QString{});
    m_roomInfo->setRoomId(QString{});
    trace::stepEnd(trace::Step::S2);

    // The game/music line belongs to this account's status: withdraw it in
    // one publish before the client is released. Best effort and no wait:
    // the switch is synchronous, and the PUT still goes out on the backend.
    // Restored below only if THIS call withdrew it: a sign-out already
    // waiting on its own withdrawal must not see the song come back.
    bool withdrewForSwitch = false;
    if (m_presence)
        m_presence->withdrawActivityLine(&withdrewForSwitch);

    trace::stepBegin(trace::Step::S3);
    const bool detachRefused =
        m_client->isLoggedIn() && !m_client->detachSession();
    trace::stepEnd(trace::Step::S3);
    if (detachRefused) {
        if (m_presence && withdrewForSwitch)
            m_presence->restoreActivityLine();
        trace::finish(trace::Outcome::Refused);
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
    // The previous account's session is no longer the client's: a
    // retraction still being retried for it would go out under the next one.
    if (m_groupCall)
        m_groupCall->abandonOutstandingMembershipWrites();

    // Warm adoption: the live handle is taken from the pool HERE, after the
    // outgoing session detached (nothing can refuse the switch any more) and
    // BEFORE the active account changes. That change makes the target the
    // pool's foreground account, and the pool retires a host for the
    // foreground account; taken after it, the ticket was refused
    // ("not-live", live 2026-10-26) and the cold restore then met the
    // retiring store. The same turn that checked eligibility.
    std::optional<lightning::accounts::LiveSessionTicket> adoption;
    if (m_sessionPool && adopting)
        adoption = m_sessionPool->takeForAdoption(target);

    // S5 (accountsChanged -> accounts() keyring reads) runs inside this
    // call's signal fan-out, so the trace folds it into S4.
    trace::stepBegin(trace::Step::S4);
    m_settings->setActiveAccountUserId(target);
    trace::stepEnd(trace::Step::S4);
    clearCrossAccountCaches();
    // Shortcut bindings are per account and cached by the registry, so the
    // switch must be announced.
    if (m_shortcuts)
        m_shortcuts->reload();
    // Hidden images are per account and cached too.
    if (m_mediaVisibility)
        m_mediaVisibility->reloadForAccount();
    bool adopted = false;
    if (m_sessionPool && adoption) {
        noteLiveAccountsStep(QStringLiteral("switch:adopt"), target);
        adopted = m_client->adoptLiveSession(*adoption);
        // Refused by the handle: the client retired it (tokens handed back)
        // and the cold restore below waits for that close.
        if (!adopted)
            noteLiveAccountsStep(QStringLiteral("switch:adopt-fallback"), target);
    } else if (m_sessionPool && adopting) {
        noteLiveAccountsStep(QStringLiteral("switch:adopt-refused"), target);
    }
    if (!adopted && !m_client->restoreSession()) {
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
    if (m_accounts && m_accounts->needsSignIn(userId)) {
        // The store answered "no such item". For a sign-in this install saved
        // for the recorded device and never removed, that says the store lost
        // it, not that the account signed out (settings file only, no read).
        return m_settings->secretsWrittenHereForRecordedDevice(userId)
                   ? SignInState::KeyringLost
                   : SignInState::Gone;
    }
    // Empty read and the backend cannot vouch for it: genuinely unknown.
    return SignInState::Unreadable;
}

void AppController::failAccountSwitch(const QString &message)
{
    lightning::switchtrace::finish(lightning::switchtrace::Outcome::Failed);
    const QString fallback = m_switchFallbackUserId;
    m_switchFallbackUserId.clear();
    if (!message.isEmpty())
        Q_EMIT errorReported(message);
    if (!fallback.isEmpty() && m_settings->hasSavedAccount(fallback)) {
        qCInfo(lcApp) << "account switch falling back"
                      << "slug=" << matrix::app_data::safeUserSlug(fallback);
        // Live accounts (Labs): the account switched away from may have
        // started running in the background since. Its store is freed first
        // (asynchronously; the switch stays in progress), or the restore
        // below would meet a store its host still holds.
        if (m_sessionPool && m_sessionPool->holdsSignIn(fallback)) {
            noteLiveAccountsStep(QStringLiteral("switch:fallback-release"),
                                 fallback);
            m_liveFallbackTarget = fallback;
            QPointer<AppController> self(this);
            m_liveFallbackTicket = m_sessionPool->releaseForForeground(
                fallback,
                [self, fallback](
                    lightning::accounts::SessionPool::ReleaseResult result) {
                    if (self)
                        self->finishFallbackRelease(fallback, int(result));
                });
            return;
        }
        m_settings->setActiveAccountUserId(fallback);
        clearCrossAccountCaches();
        if (m_client->restoreSession())
            return; // completion (or a second failure) arrives async
    }
    setAccountSwitching(false);
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

void AppController::signOutAccountResidue(const QString &userId)
{
    // The account's last known avatar. App-level, so no directory sweep
    // covers it, and sign-out removes the account record the switcher would
    // draw it for: it outlived every sign-out (VM test, 2026-10-07). Keyed on
    // the user id, as AccountAvatarStore names its files.
    if (m_accountAvatars) {
        const bool existed = m_accountAvatars->hasStored(userId);
        const bool removed = existed && m_accountAvatars->forget(userId);
        if (existed && !removed) {
            qCWarning(lcApp) << "account avatar sign-out cleanup FAILED slug="
                             << matrix::app_data::safeUserSlug(userId);
        } else {
            qCInfo(lcApp) << "account avatar sign-out cleanup"
                          << "slug=" << matrix::app_data::safeUserSlug(userId)
                          << "outcome=" << (removed ? "deleted" : "absent");
        }
    }
    // The canonical account directory, which held the starred-GIF store and
    // bridge badges deleted just above, ONLY if nothing is left in it. The
    // client already tried the RECORDED store root (finishSignOut); with a
    // divergent store slug this is the second directory, and with none it is
    // the same one, now emptied. Never recursive: any other file keeps it.
    // (Personal chat backgrounds are not such a file: finishSignOut removed
    // them under both the recorded and the canonical root.)
    const QString root = matrix::app_data::accountRoot(userId);
    if (root.isEmpty())
        return;
    const auto outcome = matrix::app_data::removeAccountRootIfEmpty(root);
    if (outcome == matrix::app_data::EmptyDirRemoval::Failed) {
        qCWarning(lcApp) << "account directory sign-out cleanup FAILED slug="
                         << matrix::app_data::safeUserSlug(userId);
    } else {
        qCInfo(lcApp) << "account directory sign-out cleanup"
                      << "slug=" << matrix::app_data::safeUserSlug(userId)
                      << "outcome="
                      << matrix::app_data::emptyDirRemovalName(outcome);
    }
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

bool AppController::removeAccountLocalState(
    const matrix::app_data::AccountIdentity &identity, bool *refused)
{
    // Retiring a Rust client is asynchronous, so its SQLite store may still be
    // open. Wait for the close before deleting, or key material can survive a
    // removal that reports success.
#ifdef ENABLE_RUST_SDK_BACKEND
    RustSdkMatrixClient::waitForRustRetirement(
        RustSdkMatrixClient::kStoreCloseBudgetMs);
#endif
    return removeAccountLocalStateFiles(identity, refused);
}

bool AppController::removeAccountLocalStateFiles(
    const matrix::app_data::AccountIdentity &identity, bool *refused)
{
    if (refused)
        *refused = false;
    // The store registry decides whether anything may touch this store on
    // disk. Without live accounts it records only and always grants; with
    // them, a store still open, starting or retiring is refused and nothing
    // is deleted (§6: never report a removal that removed nothing).
    if (!identity.rustStorePath.isEmpty()) {
        const auto verdict =
            lightning::accounts::StoreOwnershipRegistry::process()
                .claimForDeletion(identity.rustStorePath);
        using Verdict = std::remove_cv_t<decltype(verdict)>;
        if (verdict != Verdict::Granted) {
            qCWarning(lcApp) << "removing account local state REFUSED: its store"
                             << "is still in use slug=" << identity.slug
                             << "verdict="
                             << lightning::accounts::StoreOwnershipRegistry::
                                    verdictName(verdict);
            if (refused)
                *refused = true;
            return false;
        }
    }
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
    return rootsFailed == 0 && removed.failed == 0;
}

void AppController::removeAccount(const QString &userId)
{
    const QString target = userId.trimmed();
    // Removing the account a queued click names retires that click, even
    // when the removal itself is refused below (it may be retried later).
    if (!target.isEmpty() && target == m_queuedSwitchTarget)
        clearQueuedAccountSwitch("removed-target");
    if (target.isEmpty() || m_accountSwitching)
        return;
    // Arming a removal now would attach it to a sign-out already waiting, or
    // (another account) change sessions under it.
    if (m_auth->logoutPending()) {
        qCInfo(lcApp) << "removal refused: a sign-out is in progress";
        return;
    }
    if (!m_settings->hasSavedAccount(target))
        return;
    // Its saved room positions and selections (kept per account) go now; a
    // removal that has to be retried loses nothing that matters.
    if (m_pagination)
        m_pagination->forgetAccount(target);
    if (m_stickers)
        m_stickers->forgetAccount(target);
    // Live accounts (Labs): an account running in the background is signed
    // out on the server and its store released before anything is deleted
    // (DECISION 7). Never the active account, whose removal is today's.
    // A keyring write of its sign-in still running (no host: a parked pair
    // being written, a live write a retired host left stuck) counts too: the
    // pool forbids further writes and waits for that one to return before
    // anything is deleted, or a token would land with no record (§6, §18
    // round 4, R4-1).
    if (m_sessionPool && target != m_settings->activeAccountUserId()
        && (m_sessionPool->holdsSignIn(target) || m_liveRemovals.contains(target))) {
        removeLiveBackgroundAccount(target);
        return;
    }
    // Its sign-in is in a keyring that cannot be read now: removing the
    // account would delete the record and leave a live token in the keyring,
    // with nothing left that could ever remove it. A signed-in account reads
    // its token, so this never blocks the ordinary sign-out. Only while a
    // store exists and says it cannot answer: with no secret store wired at
    // all (never in production) there is no keyring to leave anything in.
    if (m_settings->secretStore()
        && m_settings->accessTokenFor(target).isEmpty()
        && m_settings->secretBackendUnavailable()) {
        qCWarning(lcApp) << "removal refused: the secret store cannot be read"
                         << "slug=" << matrix::app_data::safeUserSlug(target);
        Q_EMIT errorReported(
            tr("This account can't be removed while the system keyring can't "
               "be read: its saved sign-in is there and would be left behind. "
               "Unlock the keyring, or start it, and try again."));
        return;
    }

    // Live accounts (Labs): what the account left from a background run (its
    // cards, its badge row, an open waiting for it) goes with it.
    if (m_sessionPool) {
        noteLiveAccountsStep(QStringLiteral("removal:not-live"), target);
        cancelParkedOpenFor(target);
        m_notifications->clearPendingFor(target);
        m_accountActivity->removeAccount(target);
    }
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
    bool filesRemoved = false;
    if (resolveRemovalIdentity(target, &identity)) {
        bool refused = false;
        filesRemoved = removeAccountLocalState(identity, &refused);
        // Live accounts (Labs) only: the store registry would not let its
        // store go (a retirement that overran its budget still holds it).
        // Nothing was deleted, so the record that names the store stays
        // with it (§6), and the user is told; a later removal finishes.
        if (refused) {
            if (m_sessionPool) {
                noteLiveAccountsStep(QStringLiteral("removal:refused"), target);
                reopenLiveAccountActivity(target);
            }
            Q_EMIT errorReported(
                tr("Removal did not finish: the account's data is still in "
                   "use. Nothing was deleted."));
            return;
        }
    } else {
        qCWarning(lcApp) << "removal could not resolve an account layout"
                         << "slug=" << matrix::app_data::safeUserSlug(target);
    }
    m_accounts->removeAccount(target); // record + secrets
    if (isActive) {
        m_lastSessionUserId.clear();
        refreshTrayAccountLabel();
        // This account was never actually connected this session (a failed
        // restore is exactly how it ends up here, from the login screen's
        // repair card) but connectionStatus() can still read a stale
        // "Error" from that failed attempt. Nothing else touches it once
        // the account is gone, since onLoggedOut() never runs on this
        // branch.
        setConnectionStatus(tr("Not connected"));
    }
    // The login screen's card named this account. A complete removal ends
    // it; otherwise it becomes a card that says what is still here and
    // tries again, never one whose buttons no longer do anything.
    if (m_localSessionFailureUserId == target && !m_settings->hasSavedAccount(target)) {
        setLocalRustResetRequired(false);
        m_auth->clearLastError();
        if (filesRemoved) {
            m_removalRetryIdentity = {};
            m_removalLeftovers.clear();
            clearLocalSessionFailure();
        } else if (identity.isValid()) {
            m_removalRetryIdentity = identity;
            m_removalLeftovers = accountRemovalLeftoversFor(identity);
            setLocalSessionFailure(QStringLiteral("removal_incomplete"), target,
                                   identity.homeserver);
            Q_EMIT localSessionFailureChanged();
        }
    }
}

QStringList AppController::accountRemovalLeftoversFor(
    const matrix::app_data::AccountIdentity &identity) const
{
    // The directories removeAccountLocalState() deletes, as recorded.
    QStringList out;
    for (const QString &path : { identity.rustStorePath, identity.accountRoot,
                                 matrix::app_data::accountRoot(identity.userId) }) {
        if (!path.isEmpty() && !out.contains(path) && QFileInfo::exists(path))
            out.append(path);
    }
    return out;
}

void AppController::retryAccountRemoval()
{
    if (m_localSessionFailureReason != QLatin1String("removal_incomplete")
        || !m_removalRetryIdentity.isValid()) {
        return;
    }
    const auto identity = m_removalRetryIdentity;
    // The card tells the user they can sign in again. If they did, or are
    // doing so now, those files are the NEW session's: never delete them.
    if (m_settings->hasSavedAccount(identity.userId) || m_auth->isLoggingIn()) {
        qCInfo(lcApp) << "removal retry skipped: the account is signing in or saved again"
                      << "slug=" << matrix::app_data::safeUserSlug(identity.userId);
        return;
    }
    const bool complete = removeAccountLocalState(identity);
    m_removalLeftovers = accountRemovalLeftoversFor(identity);
    if (complete && m_removalLeftovers.isEmpty()) {
        m_removalRetryIdentity = {};
        clearLocalSessionFailure();
        m_auth->clearLastError();
        return;
    }
    // The same card; its list says what is still here.
    Q_EMIT localSessionFailureChanged();
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

namespace {
const QString kSoftwareNoticeDismissedKey =
    QStringLiteral("ui/softwareRendererNoticeDismissedVersion");
} // namespace

void AppController::setGlRenderer(const QString &glRenderer)
{
    QSettings store;
    const bool show = lightning::shouldShowSoftwareRendererNotice(
        glRenderer, qEnvironmentVariable("QT_QUICK_BACKEND"),
        qEnvironmentVariable("QSG_RHI_BACKEND"),
        store.value(kSoftwareNoticeDismissedKey).toString(),
        QCoreApplication::applicationVersion());
    if (show) {
        // Decided once, when the notice is first shown: which package this
        // is (the installer's own detection, AppImage only for a real
        // AppImage) and whether the host is NixOS (os-release ID). The
        // notice must not name either when it does not apply.
        const bool appImage = lightning::update::detectInstallType()
            == lightning::update::InstallType::LinuxAppImage;
        const bool nixos =
            lightning::isNixOsProductType(QSysInfo::productType());
        m_softwareRendererNoticeContext = lightning::softwareRendererContextId(
            lightning::softwareRendererContext(appImage, nixos));
    }
    if (m_softwareRendererNoticeVisible == show)
        return;
    m_softwareRendererNoticeVisible = show;
    Q_EMIT softwareRendererNoticeVisibleChanged();
}

void AppController::dismissSoftwareRendererNotice()
{
    QSettings store;
    store.setValue(kSoftwareNoticeDismissedKey,
                   QCoreApplication::applicationVersion());
    store.sync();
    if (!m_softwareRendererNoticeVisible)
        return;
    m_softwareRendererNoticeVisible = false;
    Q_EMIT softwareRendererNoticeVisibleChanged();
}

// ── Concurrent live accounts (Labs) ─────────────────────────────────────
//
// Everything below runs only while m_sessionPool exists (live accounts on:
// the setting, the bridge, and LIGHTNING_LABS_LIVE_ACCOUNTS not forcing it off). Every call
// into it from the shared paths above is behind `if (m_sessionPool)`, so with
// live accounts off this controller is today's (FlagOffParityTest).

namespace {
#ifdef ENABLE_RUST_SDK_BACKEND
// The background accounts share the foreground's quit budget.
static_assert(RustSdkMatrixClient::kStoreCloseBudgetMs == 15000,
              "keep m_liveAccountsQuitBudgetMs equal to the store-close budget");
#endif

// RLIMIT_NOFILE soft raised to the hard limit (Unix), so the configured
// maximum of every live store (~56 fds per background account) fits. An
// unlimited hard limit is capped, as the kernel refuses "infinity" for open
// files; macOS caps at OPEN_MAX. Returns what happened, for the log.
QString raiseOpenFileLimit()
{
#ifdef Q_OS_UNIX
    struct rlimit limit {};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return QStringLiteral("unreadable");
    rlim_t wanted = limit.rlim_max;
    if (wanted == RLIM_INFINITY)
        wanted = 65536;
#ifdef Q_OS_MACOS
    if (wanted > rlim_t(OPEN_MAX))
        wanted = rlim_t(OPEN_MAX);
#endif
    if (limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur >= wanted)
        return QStringLiteral("unchanged soft=%1")
            .arg(qulonglong(limit.rlim_cur));
    if (limit.rlim_cur == RLIM_INFINITY)
        return QStringLiteral("unchanged soft=unlimited");
    const rlim_t before = limit.rlim_cur;
    limit.rlim_cur = wanted;
    if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
        return QStringLiteral("refused soft=%1").arg(qulonglong(before));
    return QStringLiteral("raised from=%1 to=%2")
        .arg(qulonglong(before))
        .arg(qulonglong(wanted));
#else
    return QStringLiteral("not applicable");
#endif
}

constexpr int kMaxLiveAccountsSteps = 256;
} // namespace

int AppController::maxLiveAccounts() const
{
    return SettingsManager::kMaxLiveAccounts;
}

int AppController::maxPinnedAccounts() const
{
    return SettingsManager::kMaxPinnedAccounts;
}

QString AppController::setAccountKeepRunning(const QString &userId, bool on)
{
    const QString uid = userId.trimmed();
    const QString capRefusal =
        tr("Up to %1 accounts can keep running. Turn one off first.")
            .arg(SettingsManager::kMaxPinnedAccounts);
    const QString noRecord = tr("That account is not signed in on this device.");
    // Live: the pool writes the pin and reschedules. Off: the choice is
    // only stored, for when live accounts are turned on.
    if (m_sessionPool) {
        using Pin = lightning::accounts::SessionPool::PinResult;
        switch (m_sessionPool->setKeepRunning(uid, on)) {
        case Pin::Ok:
            return QString();
        case Pin::RefusedCap:
            return capRefusal;
        case Pin::RefusedNoRecord:
            return noRecord;
        }
        return QString();
    }
    using Keep = SettingsManager::KeepRunningResult;
    switch (m_settings->setKeepRunningFor(uid, on)) {
    case Keep::Ok:
    case Keep::Unchanged:
        return QString();
    case Keep::RefusedCap:
        return capRefusal;
    case Keep::RefusedNoRecord:
        return noRecord;
    }
    return QString();
}

void AppController::noteLiveAccountsStep(const QString &step,
                                         const QString &userId)
{
    m_liveAccountsSteps.append(userId.isEmpty()
                                   ? step
                                   : step + QLatin1Char(' ') + userId);
    while (m_liveAccountsSteps.size() > kMaxLiveAccountsSteps)
        m_liveAccountsSteps.removeFirst();
    qCInfo(lcApp) << "live accounts" << step << "acct="
                  << matrix::app_data::safeUserSlug(userId);
}

QString AppController::liveAccountLabel(const QString &userId) const
{
    // The record's display name (a settings-file read; never the keyring),
    // else the localpart. Never the full MXID.
    const QString name = m_settings->accountRecord(userId)
                             .value(QStringLiteral("displayName"))
                             .toString()
                             .trimmed();
    if (!name.isEmpty())
        return name;
    if (userId.startsWith(QLatin1Char('@'))) {
        const qsizetype colon = userId.indexOf(QLatin1Char(':'));
        const QString local =
            colon > 1 ? userId.mid(1, colon - 1) : userId.mid(1);
        if (!local.isEmpty())
            return local;
    }
    return userId;
}

void AppController::applyLiveAccountsLabs()
{
    // A runtime off still retiring the hosts re-applies when it is done; a
    // quit (or a controller being destroyed) never starts anything.
    if (m_liveAccountsStopping || m_shuttingDown || m_liveAccountsQuitBegun)
        return;
    // Short-circuits in this order, so a stored setting without the hidden
    // switch never asks the bridge anything.
    const bool wanted = m_liveAccountsLabsAvailable
        && m_settings->labsConcurrentAccounts()
        && lightning::accounts::rustBridgeApi().available();
    if (wanted && !m_sessionPool)
        startLiveAccounts();
    else if (!wanted && m_sessionPool)
        stopLiveAccounts();
}

void AppController::startLiveAccounts()
{
    namespace la = lightning::accounts;
    noteLiveAccountsStep(QStringLiteral("labs:on"), QString());
    // The GUI's own secret store, resolved here and only handed over: the
    // pool reads it for the keyring mode and never for a secret.
    SecretStore *const guiSecretStore = m_settings->secretStore();
    // live-accounts host wiring: begin
    // (HostSettingsScanTest: nothing between these markers may reach the
    // active account's state, the foreground client or a keyring.)
    m_accountActivity = std::make_unique<AccountActivityModel>(this);
    m_accountList = std::make_unique<AccountListModel>(
        m_settings.get(), m_accounts.get(), m_accountActivity.get(),
        [this](const QString &uid) {
            return m_revocationProofs.hardFor(uid).has_value()
                || m_revocationProofs.softFor(uid).has_value();
        },
        this);
    la::SessionPool::Deps deps;
    deps.settings = m_settings.get();
    deps.accounts = m_accounts.get();
    deps.notifications = m_notifications.get();
    deps.activity = m_accountActivity.get();
    // The same per-account proofs the foreground client mirrors into, so a
    // background revocation is what a later sign-in consults.
    deps.proofs = &m_revocationProofs;
    deps.guiSecretStore = guiSecretStore;
    // The worker thread's own instance, made the way this controller makes
    // its own (a test's factory included). Called on that thread.
    deps.secretStoreFactory = [factory = m_secretStoreFactory] {
        return makeSecretStore(factory, nullptr);
    };
    deps.appStartMs = m_appStartWallMs;
    m_sessionPool = std::make_unique<la::SessionPool>(std::move(deps), this);
    la::SessionPool *pool = m_sessionPool.get();
    // live-accounts host wiring: end
    // The switcher's "Needs sign-in" for an account whose keyring lost its
    // token: asked on the pool's keyring worker, never on this thread
    // (la1-M L2). The pool outlives no runtime off, so it is guarded.
    m_accountList->setSignInProbe(
        [guard = QPointer<la::SessionPool>(pool)](const QString &uid,
                                                  std::function<void(bool)> done) {
            if (guard)
                guard->probeSignIn(uid, std::move(done));
        });
#ifdef ENABLE_RUST_SDK_BACKEND
    // One-way for the process: store claims enforced, revocation proofs
    // consulted by a sign-in, host retirements awaited by the settle.
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->setLiveAccountsLabs(true);
#endif
    m_notifications->setLiveAccountsMode(true);
    qCInfo(lcApp) << "live accounts open-file limit:" << raiseOpenFileLimit();

    // live-accounts host wiring: begin
    connect(pool, &la::SessionPool::liveHostCountChanged, this,
            [this](int) { refreshLiveAccountCount(); });
    connect(pool, &la::SessionPool::hostSignInExpired, this,
            &AppController::onHostSignInExpired);
    connect(pool, &la::SessionPool::hostNeedsAttention, this,
            [](const QString &uid, int liveState) {
        qCInfo(lcApp) << "background account needs attention acct="
                      << matrix::app_data::safeUserSlug(uid)
                      << "state=" << liveState;
    });
    connect(m_accountActivity.get(), &AccountActivityModel::totalsChanged, this,
            [this] { m_trayUnreadCoalesce.start(); });
    // An account removed (its badge row tombstoned) and saved again in the
    // same session: its row may be fed again. Never while a removal of it
    // is still running, live or waiting for the server sign-out.
    connect(m_settings.get(), &SettingsManager::accountsChanged, pool, [this] {
        if (!m_accountActivity)
            return;
        for (const QString &uid : m_settings->savedAccountUserIds()) {
            if (m_accountActivity->isRemoved(uid) && !m_liveRemovals.contains(uid)
                && uid != m_pendingRemovalUserId)
                reopenLiveAccountActivity(uid);
        }
    });
    // live-accounts host wiring: end
    // Foreground facts. Their context is the pool, so a runtime off drops
    // every one of these with it.
    connect(m_settings.get(), &SettingsManager::sessionChanged, pool,
            [this] { feedLiveAccountsForeground(); });
    connect(this, &AppController::loggedInChanged, pool,
            [this] { refreshLiveAccountCount(); });
    connect(m_client.get(), &MatrixClient::initialSyncDoneChanged, pool,
            [this] {
        if (m_sessionPool && m_client->isLoggedIn()
            && m_client->initialSyncDone())
            m_sessionPool->noteForegroundInitialSyncDone();
    });
    connect(m_client.get(), &MatrixClient::connectionStateChanged, pool,
            [this](MatrixClient::ConnectionState state) {
        if (m_sessionPool && state == MatrixClient::Syncing)
            m_sessionPool->noteForegroundHomeserverReachable(
                m_client->homeserverUrl());
    });
    // A notification open parked for this account fires once its room is
    // known, always from a later event-loop turn: never inside
    // onLoginSucceeded (§16), which runs before these handlers.
    const auto tryParkedOpenLater = [this] {
        if (m_parkedOpen) {
            QMetaObject::invokeMethod(this, [this] { tryFireParkedOpen(); },
                                      Qt::QueuedConnection);
        }
    };
    connect(m_client.get(), &MatrixClient::roomsChanged, pool,
            tryParkedOpenLater);
    connect(m_auth.get(), &AuthManager::loginSucceeded, pool,
            tryParkedOpenLater);

    feedLiveAccountsForeground();
    if (m_client->isLoggedIn() && m_client->initialSyncDone())
        pool->noteForegroundInitialSyncDone();
    pool->start();
    refreshLiveAccountCount();
    Q_EMIT liveAccountsActiveChanged();
}

void AppController::stopLiveAccounts()
{
    if (!m_sessionPool || m_liveAccountsStopping)
        return;
    m_liveAccountsStopping = true;
    noteLiveAccountsStep(QStringLiteral("labs:off"), QString());
    // Nothing waits for another account any more.
    m_parkedOpen.reset();
    m_claimedOpen.reset();
    if (m_parkedOpenTimer)
        m_parkedOpenTimer->stop();
    cancelLiveSwitchRelease();
    m_liveSwitchReleased.clear();
    // A failed switch's fallback waiting for its store: it ends on the login
    // screen (finishFallbackRelease(), Cancelled).
    if (m_liveFallbackTicket != 0)
        m_sessionPool->cancelRelease(m_liveFallbackTicket);
    m_liveFallbackTicket = 0;
    m_liveFallbackTarget.clear();
    // Every host retires (tokens persisted) and is awaited off the GUI
    // thread; the foreground is kept. The store claims stay enforced until
    // the next start (one-way), which the card says.
    QPointer<AppController> self(this);
    m_sessionPool->shutdownAsync([self] {
        if (!self || !self->m_sessionPool)
            return;
        AppController *app = self.data();
        // Called by the pool: it is deleted on a later turn, never inside
        // its own callback. The models go after it (it points at one).
        auto *pool = app->m_sessionPool.release();
        pool->disconnect(app);
        pool->deleteLater();
        app->m_retiredLivePool = pool;
        auto *list = app->m_accountList.release();
        auto *activity = app->m_accountActivity.release();
        app->m_retiredAccountList = list;
        app->m_retiredAccountActivity = activity;
        app->m_notifications->setLiveAccountsMode(false);
        app->m_liveRemovals.clear();
        app->m_liveAccountsForeground.clear();
        app->m_liveAccountsStopping = false;
        app->m_liveAccountCount = 0;
        app->noteLiveAccountsStep(QStringLiteral("labs:off-done"), QString());
        Q_EMIT app->liveAccountsActiveChanged();
        Q_EMIT app->liveAccountCountChanged();
        list->deleteLater();
        activity->deleteLater();
        app->m_trayUnreadCoalesce.start();
        // Turned back on while the hosts retired.
        app->applyLiveAccountsLabs();
    });
}

void AppController::feedLiveAccountsForeground()
{
    if (!m_sessionPool)
        return;
    // The account in front, or restoring to be: never started as a host.
    // Empty only while no account is.
    const QString active = m_settings->activeAccountUserId();
    if (active == m_liveAccountsForeground)
        return;
    const QString previous = std::exchange(m_liveAccountsForeground, active);
    // Switch-away: the account that was in front was live until now, which
    // is its backlog floor when it next runs in the background.
    if (!previous.isEmpty() && m_settings->hasSavedAccount(previous))
        m_settings->setLastLiveAtFor(previous,
                                     QDateTime::currentMSecsSinceEpoch());
    noteLiveAccountsStep(QStringLiteral("foreground"), active);
    m_sessionPool->setForegroundUserId(active);
    m_accountActivity->setActiveUserId(active);
    m_trayUnreadCoalesce.start();
    refreshLiveAccountCount();
}

void AppController::refreshLiveAccountCount()
{
    if (!m_sessionPool)
        return;
    const int count = (m_client && m_client->isLoggedIn() ? 1 : 0)
        + m_sessionPool->liveHostCount();
    // "Room · account" only while a card could be for more than one.
    m_notifications->setAccountLabelsShown(count > 1);
    if (count == m_liveAccountCount)
        return;
    m_liveAccountCount = count;
    Q_EMIT liveAccountCountChanged();
}

bool AppController::beginLiveAccountSwitch(const QString &target)
{
    namespace la = lightning::accounts;
    // Its store was just freed for this switch: today's switch runs now.
    if (m_liveSwitchReleased == target) {
        m_liveSwitchReleased.clear();
        return false;
    }
    m_liveSwitchReleased.clear();
    if (!m_liveSwitchTarget.isEmpty()) {
        // A second click on the account already being freed.
        if (m_liveSwitchTarget == target)
            return true;
        // Another account was clicked meanwhile: the newest click wins.
        cancelLiveSwitchRelease();
    }
    // Being removed: its host is retiring for a removal that deletes its
    // store once free, so it is never freed for the foreground instead.
    if (m_liveRemovals.contains(target)) {
        noteLiveAccountsStep(QStringLiteral("switch:refused-removing"), target);
        Q_EMIT errorReported(
            tr("That account is being removed from this device."));
        endLiveSwitchWithoutSession();
        return true;
    }
    // A running host whose session can be handed over as it is: no release,
    // no store close, no keyring read. switchToAccount() takes it once the
    // foreground is unbound (warm account adoption, Part 4 §4).
    if (m_client->supportsLiveAdoption() && m_sessionPool->canAdopt(target)) {
        noteLiveAccountsStep(QStringLiteral("switch:warm"), target);
        m_adoptTarget = target;
        return false;
    }
    // A host, or a keyring write of its sign-in still running: freed first,
    // so today's switch never reads the keyring under that write (§18 round
    // 3, R3-1).
    if (!m_sessionPool->holdsSignIn(target))
        return false;
    // The background account is retired (its tokens handed back) and its
    // store awaited, all off this thread; the account in front stays in
    // front and usable meanwhile.
    noteLiveAccountsStep(QStringLiteral("switch:release"), target);
    m_liveSwitchTarget = target;
    QPointer<AppController> self(this);
    m_liveSwitchTicket = m_sessionPool->releaseForForeground(
        target, [self, target](la::SessionPool::ReleaseResult result) {
            if (self)
                self->onLiveSwitchReleased(target, int(result));
        });
    return true;
}

void AppController::onLiveSwitchReleased(const QString &target, int result)
{
    using Release = lightning::accounts::SessionPool::ReleaseResult;
    // Superseded by another click, a removal or a runtime off.
    if (!m_sessionPool || m_liveSwitchTarget != target)
        return;
    m_liveSwitchTicket = 0;
    m_liveSwitchTarget.clear();
    // Answered while quitting (inside the quit's wait, or a destructor's):
    // nothing switches any more.
    if (m_shuttingDown || m_liveAccountsQuitBegun)
        return;
    switch (Release(result)) {
    case Release::Free:
        noteLiveAccountsStep(QStringLiteral("switch:free"), target);
        // The release retired its host: an open parked for it now runs
        // under the epoch that release left.
        if (m_parkedOpen && m_parkedOpen->userId == target)
            m_parkedOpen->epoch = m_sessionPool->epochFor(target);
        m_liveSwitchReleased = target;
        switchToAccount(target);
        m_liveSwitchReleased.clear();
        // Today's switch refused on the spot (an expired sign-in, a locked
        // keyring, a sign-out in progress): the account was freed for
        // nothing, and the pool would otherwise never start it again.
        if (m_sessionPool && !m_accountSwitching
            && m_settings->activeAccountUserId() != target) {
            abandonLiveRelease(target);
            endLiveSwitchWithoutSession();
        }
        return;
    case Release::StoreBusy:
        noteLiveAccountsStep(QStringLiteral("switch:store-busy"), target);
        cancelParkedOpenFor(target);
        // Not switched: the account is the pool's again (it restarts only
        // once its store is free, and never while busy until restart).
        m_sessionPool->abandonRelease(target);
        // A store that never confirmed its close stays busy until restart
        // ("Store busy (restart to reopen)"): another try cannot help.
        if (m_sessionPool->liveState(target) == AccountActivityModel::StoreBusy) {
            Q_EMIT errorReported(
                tr("That account's data did not close. Restart Lightning to "
                   "open it."));
        } else {
            Q_EMIT errorReported(
                tr("That account is still closing. Try again in a moment."));
        }
        endLiveSwitchWithoutSession();
        return;
    case Release::Cancelled:
        noteLiveAccountsStep(QStringLiteral("switch:cancelled"), target);
        endLiveSwitchWithoutSession();
        return;
    }
}

void AppController::cancelLiveSwitchRelease()
{
    // The ticket and target are forgotten BEFORE the cancel: the pool answers
    // Cancelled synchronously, and onLiveSwitchReleased() must read that as
    // superseded (a newer click, a removal, Labs off) rather than as this
    // switch failing, which ended on the login screen while logged out and
    // flashed it under the newer click (§18 round 2, L-b).
    const quint64 ticket = std::exchange(m_liveSwitchTicket, 0);
    m_liveSwitchTarget.clear();
    if (ticket != 0 && m_sessionPool)
        m_sessionPool->cancelRelease(ticket);
}

void AppController::endLiveSwitchWithoutSession()
{
    // A live switch that did not switch, started when no session was in front
    // (onLoggedOut's fallback to an account running in the background):
    // nothing else will take the user anywhere, so it ends as onLoggedOut's
    // own tail does, on the login screen (§18 round 1, MUST 3). With a
    // session in front (a click from it), or a switch still in progress, the
    // user stays where they are.
    if (m_shuttingDown || m_liveAccountsQuitBegun || m_accountSwitching
        || m_client->isLoggedIn())
        return;
    noteLiveAccountsStep(QStringLiteral("switch:to-login"), QString());
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

void AppController::abandonLiveRelease(const QString &userId)
{
    if (!m_sessionPool)
        return;
    noteLiveAccountsStep(QStringLiteral("switch:abandoned"), userId);
    // The pool forgets a release when the foreground changes to it, or on a
    // cancelRelease() made before it answered; a release that answered and
    // whose switch never landed is handed back explicitly. (It used to fake a
    // foreground change, which also cleared the account's failure marks and
    // could lift a removal's write ban.)
    m_sessionPool->abandonRelease(userId);
}

void AppController::finishFallbackRelease(const QString &fallback, int result)
{
    using Release = lightning::accounts::SessionPool::ReleaseResult;
    if (m_liveFallbackTarget != fallback)
        return;
    m_liveFallbackTicket = 0;
    m_liveFallbackTarget.clear();
    if (m_shuttingDown || m_liveAccountsQuitBegun)
        return;
    if (Release(result) == Release::Free && m_settings->hasSavedAccount(fallback)) {
        noteLiveAccountsStep(QStringLiteral("switch:fallback-free"), fallback);
        // failAccountSwitch()'s own fallback, now that nothing else holds
        // the store.
        m_settings->setActiveAccountUserId(fallback);
        clearCrossAccountCaches();
        if (m_client->restoreSession())
            return; // completion (or a second failure) arrives async
    } else {
        noteLiveAccountsStep(QStringLiteral("switch:fallback-refused"),
                             fallback);
        if (Release(result) == Release::StoreBusy) {
            Q_EMIT errorReported(
                tr("That account is still closing. Try again in a moment."));
        }
    }
    setAccountSwitching(false);
    setCurrentScreen(LoginScreen);
    Q_EMIT loggedInChanged();
}

bool AppController::claimCrossAccountOpen(const QString &accountUserId,
                                          const QString &roomId,
                                          const QString &eventId,
                                          const QString &threadRootId)
{
    // An account no longer on this device: today's routing (the room-only
    // signal) decides, which refuses a room this account is not in.
    if (!m_sessionPool || !m_settings->hasSavedAccount(accountUserId))
        return false;
    m_claimedOpen = ClaimedOpen{ roomId, eventId, threadRootId };
    // The newest click wins: one open waits at a time.
    if (m_parkedOpen)
        cancelParkedOpenFor(m_parkedOpen->userId);
    if (!roomId.isEmpty()) {
        m_parkedOpen = ParkedOpen{ accountUserId, roomId, eventId,
                                   threadRootId,
                                   m_sessionPool->epochFor(accountUserId) };
        if (!m_parkedOpenTimer) {
            m_parkedOpenTimer = std::make_unique<QTimer>();
            m_parkedOpenTimer->setSingleShot(true);
            connect(m_parkedOpenTimer.get(), &QTimer::timeout, this, [this] {
                if (!m_parkedOpen)
                    return;
                qCInfo(lcApp) << "notification open for another account "
                                 "expired before its rooms arrived acct="
                              << matrix::app_data::safeUserSlug(
                                     m_parkedOpen->userId);
                noteLiveAccountsStep(QStringLiteral("open:expired"),
                                     m_parkedOpen->userId);
                m_parkedOpen.reset();
            });
        }
        m_parkedOpenTimer->start(m_parkedOpenTimeoutMs);
        noteLiveAccountsStep(QStringLiteral("open:parked"), accountUserId);
    }
    // The window comes forward now, naming no room; the room opens once its
    // account is in front.
    Q_EMIT notificationOpenRequested(QString(), QString(), QString());
    // The switch starts on the next turn: this runs inside the manager's
    // emission of the click, and a switch clears the manager's cards.
    QMetaObject::invokeMethod(this, [this, accountUserId] {
        if (!m_sessionPool)
            return;
        switchToAccount(accountUserId);
        // Refused on the spot (an expired sign-in, a locked keyring, a
        // sign-out in progress): nothing will ever open it.
        if (!m_accountSwitching && m_liveSwitchTarget != accountUserId
            && m_settings->activeAccountUserId() != accountUserId)
            cancelParkedOpenFor(accountUserId);
        else
            tryFireParkedOpen();
    }, Qt::QueuedConnection);
    return true;
}

void AppController::tryFireParkedOpen()
{
    if (!m_parkedOpen || !m_sessionPool)
        return;
    const ParkedOpen open = *m_parkedOpen;
    // Only once that account is the one in front, signed in, and done
    // switching.
    if (m_accountSwitching || !m_client->isLoggedIn()
        || m_client->currentUserId() != open.userId
        || m_settings->activeAccountUserId() != open.userId)
        return;
    // Retired or removed since the click (the pool bumps the epoch): not the
    // session the click was for.
    if (m_sessionPool->epochFor(open.userId) != open.epoch) {
        noteLiveAccountsStep(QStringLiteral("open:stale"), open.userId);
        cancelParkedOpenFor(open.userId);
        return;
    }
    if (m_client->roomInfo(open.roomId).id.isEmpty())
        return; // its rooms are not here yet
    m_parkedOpen.reset();
    if (m_parkedOpenTimer)
        m_parkedOpenTimer->stop();
    noteLiveAccountsStep(QStringLiteral("open:fired"), open.userId);
    routeNotificationOpen(open.roomId, open.eventId, open.threadRootId);
}

void AppController::cancelParkedOpenFor(const QString &accountUserId)
{
    if (!m_parkedOpen || m_parkedOpen->userId != accountUserId)
        return;
    m_parkedOpen.reset();
    if (m_parkedOpenTimer)
        m_parkedOpenTimer->stop();
    noteLiveAccountsStep(QStringLiteral("open:cancelled"), accountUserId);
}

void AppController::removeLiveBackgroundAccount(const QString &userId)
{
    // Already being removed: its completion reports.
    if (m_liveRemovals.contains(userId))
        return;
    // As today's removal: a keyring that cannot answer would keep the
    // sign-in it holds after the record goes. Asked of the settings file
    // only; a live background account is never read from here.
    if (m_settings->secretStore() && m_settings->secretBackendUnavailable()) {
        qCWarning(lcApp) << "removal refused: the secret store cannot be read"
                         << "slug=" << matrix::app_data::safeUserSlug(userId);
        Q_EMIT errorReported(
            tr("This account can't be removed while the system keyring can't "
               "be read: its saved sign-in is there and would be left behind. "
               "Unlock the keyring, or start it, and try again."));
        return;
    }
    m_liveRemovals.insert(userId);
    // (1) Nothing of it is acted on any more: a release for a switch, an
    // open waiting for it, its cards and its badge row. Its epoch is bumped
    // by the retirement below (the pool owns epochs).
    noteLiveAccountsStep(QStringLiteral("removal:1-forget"), userId);
    if (m_liveSwitchTarget == userId)
        cancelLiveSwitchRelease();
    cancelParkedOpenFor(userId);
    m_notifications->clearPendingFor(userId);
    m_accountActivity->removeAccount(userId);
    // (2) The identity, while the record that binds its store still exists.
    matrix::app_data::AccountIdentity identity;
    const bool resolved = resolveRemovalIdentity(userId, &identity);
    noteLiveAccountsStep(QStringLiteral("removal:2-identity"), userId);
    if (!resolved) {
        qCWarning(lcApp) << "removal could not resolve an account layout"
                         << "slug=" << matrix::app_data::safeUserSlug(userId);
    }
    // (3)+(4) Signed out on the server (DECISION 7) and retired without
    // writing anything back, then THIS store awaited, never the foreground
    // pool's wait whose result today's removal ignores. Deletes nothing.
    noteLiveAccountsStep(QStringLiteral("removal:3-retire"), userId);
    QPointer<AppController> self(this);
    m_sessionPool->retireForRemoval(
        userId,
        [self, userId, identity,
         resolved](lightning::accounts::SessionPool::RemovalResult result) {
            if (self)
                self->finishLiveBackgroundRemoval(userId, identity, resolved,
                                                  result.storeFree,
                                                  int(result.serverSignOut));
        });
}

void AppController::finishLiveBackgroundRemoval(
    const QString &userId, const matrix::app_data::AccountIdentity &identity,
    bool resolved, bool storeFree, int serverSignOut)
{
    using SignOut = lightning::accounts::SessionPool::ServerSignOut;
    m_liveRemovals.remove(userId);
    const SignOut signOut = SignOut(serverSignOut);
    const char *signOutName = "not-live";
    switch (signOut) {
    case SignOut::Ok: signOutName = "ok"; break;
    case SignOut::AlreadyInvalid: signOutName = "already-invalid"; break;
    case SignOut::Failed: signOutName = "failed"; break;
    case SignOut::TimedOut: signOutName = "timed-out"; break;
    case SignOut::NotLive: signOutName = "not-live"; break;
    }
    qCInfo(lcApp) << "live account removal: released store_free=" << storeFree
                  << "server_sign_out=" << signOutName
                  << "slug=" << matrix::app_data::safeUserSlug(userId);
    // (4) Its store did not close within budget (or never will): nothing is
    // deleted, and the record stays so the user can try again.
    if (!storeFree) {
        noteLiveAccountsStep(QStringLiteral("removal:4-still-held"), userId);
        // The account stays: its badge row may be fed again.
        reopenLiveAccountActivity(userId);
        Q_EMIT errorReported(
            tr("Removal did not finish: the account's data is still in use. "
               "Nothing was deleted."));
        return;
    }
    noteLiveAccountsStep(QStringLiteral("removal:4-released"), userId);
    // (5) Its local state, only if the store registry agrees the store is
    // free (removeAccountLocalStateFiles() asks claimForDeletion() before it
    // deletes anything); a refusal deletes nothing and keeps the record.
    if (resolved) {
        bool refused = false;
        removeAccountLocalStateFiles(identity, &refused);
        if (refused) {
            noteLiveAccountsStep(QStringLiteral("removal:5-refused"), userId);
            reopenLiveAccountActivity(userId);
            Q_EMIT errorReported(
                tr("Removal did not finish: the account's data is still in "
                   "use. Nothing was deleted."));
            return;
        }
        noteLiveAccountsStep(QStringLiteral("removal:5-delete"), userId);
    }
    // (6) The record and its secrets, last.
    noteLiveAccountsStep(QStringLiteral("removal:6-record"), userId);
    m_accounts->removeAccount(userId);
    if (signOut == SignOut::Failed || signOut == SignOut::TimedOut) {
        Q_EMIT errorReported(
            tr("The account was removed from this device, but signing it out "
               "on the server did not finish. It may still be listed among "
               "your sessions on other devices."));
    }
}

void AppController::reopenLiveAccountActivity(const QString &userId)
{
    if (!m_sessionPool || !m_accountActivity
        || !m_accountActivity->isRemoved(userId))
        return;
    // At the pool's current epoch, so a late summary from the host that was
    // retiring is still refused.
    m_accountActivity->reopenAccount(userId, m_sessionPool->epochFor(userId));
    noteLiveAccountsStep(QStringLiteral("activity:reopened"), userId);
}

void AppController::onHostSignInExpired(const QString &userId)
{
    // The pool has classified it (into the shared revocation proofs, so a
    // later sign-in takes today's move-aside or resume path), marked the
    // account and retired its host. Here: one notice, and nothing deleted.
    noteLiveAccountsStep(QStringLiteral("revoked"), userId);
    cancelParkedOpenFor(userId);
    if (!m_settings->notificationsEnabled())
        return;
    // The label is a server-controlled display name: isolated (FSI ... PDI),
    // as every account label in a card title is, so its own direction
    // cannot reorder the title around it.
    const QString label =
        QChar(0x2068) + liveAccountLabel(userId) + QChar(0x2069);
    // Stamped for that account: a click switches to it (§18 round 1, L5).
    m_notifications->showGeneric(
        tr("Sign-in expired · %1").arg(label),
        tr("Sign in to this account again to keep receiving its messages."),
        QString(), QString(), /*sound=*/false,
        QStringLiteral("signin-expired:") + userId, userId);
}
