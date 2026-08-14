#include "app/AppController.h"

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
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSaveFile>
#include <QSysInfo>
#include <QPalette>
#include <QStyleHints>
#include <QUuid>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApp, "matrix.app")

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

    m_client       = makeClient(backend, m_settings.get(), this);
    m_accounts     = std::make_unique<AccountManager>(m_settings.get(), this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_quickSwitcher = std::make_unique<QuickSwitcherModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    m_timelineView = std::make_unique<ReverseListProxyModel>(this);
    m_timelineView->setSourceModel(m_timeline.get());
    m_composer     = std::make_unique<MessageComposer>(this);
    m_mentionSuggestions = std::make_unique<MentionSuggestionModel>(this);
    m_emojiCatalog = std::make_unique<EmojiCatalog>(m_settings.get(), this);
    m_notifications= std::make_unique<NotificationManager>(this);
    m_media        = std::make_unique<MediaManager>(this);
    m_crypto       = std::make_unique<CryptoManager>(this);
    m_cryptoHealth = std::make_unique<CryptoHealthModel>(this);
    m_cryptoBootstrap = std::make_unique<CryptoBootstrapModel>(this);
    m_spaces       = std::make_unique<SpaceManager>(this);
    m_threads      = std::make_unique<ThreadManager>(this);
    m_thread       = std::make_unique<ThreadController>(this);
    m_conversations= std::make_unique<ConversationController>(this);
    m_roomInfo     = std::make_unique<RoomInfoController>(this);
    m_mediaBridge  = std::make_unique<MediaBridge>(this);
    m_playback     = std::make_unique<MediaPlaybackController>(this);
    m_pagination   = std::make_unique<PaginationController>(this);
    m_readReceipts = std::make_unique<ReadReceiptCoordinator>(this);
    m_linkPreviews = std::make_unique<LinkPreviewController>(this);
    m_gifTransport = std::make_unique<MatrixGifTransport>(this);
    m_gif          = std::make_unique<GifSearchController>(this);
    m_gif->setTransport(m_gifTransport.get());
    m_gifSend      = std::make_unique<GifSendController>(this);
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
    m_timelineScroll = std::make_unique<TimelineScrollController>(this);
    m_threadScroll   = std::make_unique<TimelineScrollController>(this);

    m_crypto->setBackendName(backendName());

    // v0.6.0 checkpoint 11: native notifications. Every appended remote
    // event (any room, incl. thread-timeline copies which are filtered by
    // their composite id) runs the pure decision; the manager delivers via
    // freedesktop DBus. Bodies are never logged or persisted.
    connect(m_client.get(), &MatrixClient::eventAppended, this,
            [this](const QString &roomId, const TimelineEvent &event) {
        if (MatrixClient::isThreadTimelineId(roomId))
            return;   // the room copy of the same event already notifies
        NotificationManager::Context context;
        context.selfUserId = m_client->currentUserId();
        // Targeted lookup — the previous rooms() call deep-copied the whole
        // room list (every QString and member hash) once per appended event,
        // which is O(events x rooms) across a sync burst.
        const RoomInfo info = m_client->roomInfo(roomId);
        context.roomName = info.name.isEmpty() ? roomId : info.name;
        context.roomIsDirect = info.isDirect;
        context.roomMode = static_cast<NotificationManager::RoomMode>(
            m_settings->roomNotificationMode(roomId));
        context.previewMode = static_cast<NotificationManager::PreviewMode>(
            m_settings->notificationPreview());
        context.notificationsEnabled = m_settings->notificationsEnabled();
        context.roomVisibleAtLatest =
            roomId == m_currentRoomId && m_activeRoomAtLatest;
        // Suppress the initial-sync backlog: those events are pre-existing
        // history, not fresh activity, and must not re-notify on each launch.
        context.initialSyncComplete = m_client->initialSyncDone();
        context.soundMode = static_cast<NotificationManager::SoundMode>(
            m_settings->notificationSound());
        m_notifications->processEvent(event, context);
    });
    connect(m_notifications.get(), &NotificationManager::openRequested, this,
            [this](const QString &roomId, const QString &eventId,
                   const QString &threadRootId) {
        Q_EMIT notificationOpenRequested(roomId, eventId, threadRootId);
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
                    room.id);
            }
        }
        m_knownInvites = current;
    });

    m_spaces->setClient(m_client.get());
    m_threads->setClient(m_client.get());
    m_thread->setClient(m_client.get());
    m_roomList->setClient(m_client.get());
    m_roomList->setSpaceManager(m_spaces.get());
    m_quickSwitcher->setClient(m_client.get());
    m_quickSwitcher->setSpaceManager(m_spaces.get());
    m_timeline->setClient(m_client.get());
    m_composer->setClient(m_client.get());
    m_mentionSuggestions->setClient(m_client.get());
    m_media->setClient(m_client.get());
    m_conversations->setClient(m_client.get());
    m_roomInfo->setClient(m_client.get());
    m_mediaBridge->setClient(m_client.get());
    m_pagination->setClient(m_client.get());
    m_pagination->setTimelineModel(m_timeline.get());
    m_readReceipts->setClient(m_client.get());
    m_readReceipts->setTimelineModel(m_timeline.get());
    m_linkPreviews->setClient(m_client.get());
    m_gifTransport->setClient(m_client.get());
    m_gifSend->setClient(m_client.get());

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
QuickSwitcherModel *AppController::quickSwitcher() const
{ return m_quickSwitcher.get(); }
TimelineModel *AppController::timeline() const { return m_timeline.get(); }
QAbstractItemModel *AppController::timelineView() const
{ return m_timelineView.get(); }
MessageComposer *AppController::composer() const { return m_composer.get(); }
MediaManager *AppController::media() const { return m_media.get(); }
CryptoManager *AppController::crypto() const { return m_crypto.get(); }
SpaceManager *AppController::spaces() const { return m_spaces.get(); }
ThreadManager *AppController::threads() const { return m_threads.get(); }

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
    if (m_spaces)
        m_spaces->setActiveSpaceId(spaceId);
    if (m_currentRoomId.isEmpty())
        return;
    // Mirror the roomLeft path: the Rust backend's SDK timeline for the
    // open room is closed before the room selection clears, so no live
    // subscription outlives the visible timeline, and the room-info
    // controller stops pointing at a room that is no longer shown.
#ifdef ENABLE_RUST_SDK_BACKEND
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->closeRoomTimeline();
#endif
    setCurrentRoomId(QString());
    m_roomInfo->setRoomId(QString());
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

void AppController::onLoggedOut()
{
    m_currentRoomId.clear();
    // The roster cache died with the session (detach or logout); the next
    // account — or a re-login — must hydrate rooms afresh.
    m_memberHydratedRooms.clear();
    m_playback->stopAll(); // no playback (or decrypted-media handle) survives
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
    m_notifications->clearPending();
    m_knownInvites.clear();
    m_sessionDevices.clear();
    m_sessionDevicesLoading = false;
    m_sessionDevicesFailed = false;
    Q_EMIT sessionDevicesChanged();
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
