#include "app/AppController.h"

#include <algorithm>

#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "crypto/CryptoManager.h"
#include "matrix/CppHttpMatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaManager.h"
#include "models/MessageComposer.h"
#include "models/EmojiCatalog.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "notifications/NotificationManager.h"
#include "spaces/SpaceManager.h"
#include "storage/SecretStore.h"
#include "threads/ThreadManager.h"

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

#include "matrix/MatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QDir>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApp, "matrix.app")

bool AppController::isBackendCompiled(Backend backend)
{
    switch (backend) {
    case MockBackend:
    case HttpBackend:
        return true;
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
}

AppController::AppController(Backend backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_secretStore(SecretStore::createDefault(this))
    , m_settings(std::make_unique<SettingsManager>(this))
{
    // Wire the SecretStore before anything reads accessToken(). Migrates any
    // legacy plaintext token into the SecretStore on first entry.
    m_settings->setSecretStore(m_secretStore.get());

    m_client       = makeClient(backend, m_settings.get(), this);
    m_accounts     = std::make_unique<AccountManager>(m_settings.get(), this);
    m_auth         = std::make_unique<AuthManager>(m_client.get(), this);
    m_roomList     = std::make_unique<RoomListModel>(this);
    m_quickSwitcher = std::make_unique<QuickSwitcherModel>(this);
    m_timeline     = std::make_unique<TimelineModel>(this);
    m_composer     = std::make_unique<MessageComposer>(this);
    m_emojiCatalog = std::make_unique<EmojiCatalog>(m_settings.get(), this);
    m_notifications= std::make_unique<NotificationManager>(this);
    m_media        = std::make_unique<MediaManager>(this);
    m_crypto       = std::make_unique<CryptoManager>(this);
    m_cryptoHealth = std::make_unique<CryptoHealthModel>(this);
    m_spaces       = std::make_unique<SpaceManager>(this);
    m_threads      = std::make_unique<ThreadManager>(this);
    m_thread       = std::make_unique<ThreadController>(this);
    m_conversations= std::make_unique<ConversationController>(this);
    m_roomInfo     = std::make_unique<RoomInfoController>(this);
    m_mediaBridge  = std::make_unique<MediaBridge>(this);
    m_pagination   = std::make_unique<PaginationController>(this);
    m_readReceipts = std::make_unique<ReadReceiptCoordinator>(this);
    m_linkPreviews = std::make_unique<LinkPreviewController>(this);
    m_gifTransport = std::make_unique<MatrixGifTransport>(this);
    m_gif          = std::make_unique<GifSearchController>(this);
    m_gif->setTransport(m_gifTransport.get());
    m_gifSend      = std::make_unique<GifSendController>(this);
    m_gifSend->setRecentModel(m_gif->recent());

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
        RoomInfo info;
        const auto rooms = m_client->rooms();
        for (const auto &candidate : rooms) {
            if (candidate.id == roomId) {
                info = candidate;
                break;
            }
        }
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
    connect(m_client.get(), &MatrixClient::loggedOut, this,
            [this] { m_notifications->clearPending(); m_knownInvites.clear(); });
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
        // v0.5.11: re-emit when the platform light/dark preference changes so
        // the "System" theme repaints live.
        if (auto *hints = guiApp->styleHints()) {
            connect(hints, &QStyleHints::colorSchemeChanged, this,
                    [this](Qt::ColorScheme) { Q_EMIT systemDarkModeChanged(); });
        }
    }

    // v0.5.9: a created (or reused) conversation opens once the room is
    // present in the authoritative room list. Leaving the open room closes
    // its timeline and returns to the no-room state; the list entry is
    // removed by the authoritative room-list update, not locally.
    connect(m_conversations.get(), &ConversationController::conversationReady,
            this, &AppController::openRoom);
    connect(m_conversations.get(), &ConversationController::spacePlacementFailed,
            this, [this](const QString &) {
        Q_EMIT errorReported(
            tr("The room was created, but adding it to the Space failed."));
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
        switch (m_client->connectionState()) {
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
        connect(rust, &MatrixClient::connectionStateChanged,
                this, [this](MatrixClient::ConnectionState state) {
            m_cryptoHealth->setSyncing(state == MatrixClient::Syncing);
        });
        connect(rust, &MatrixClient::loggedOut, this, [this] {
            m_cryptoHealth->resetForNewGeneration();
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
        connect(rust, &RustSdkMatrixClient::verificationDone,
                this, [this, rust](const QString &flowId) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("done");
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
        });
        connect(rust, &RustSdkMatrixClient::verificationCancelled,
                this, [this](const QString &flowId, const QString &) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("cancelled");
            Q_EMIT verificationStateChanged();
        });
        connect(rust, &RustSdkMatrixClient::verificationFailed,
                this, [this](const QString &flowId, const QString &msg) {
            if (flowId != m_verificationFlowId) return;
            m_verificationState = QStringLiteral("failed:%1").arg(msg);
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
                this, [this](const QString &) {
            setLocalRustResetRequired(true);
            Q_EMIT storeDeviceMismatchDetected(tr(
                "This local Lightning Rust SDK store belongs to a different "
                "Matrix session or device. Reset the local Lightning session "
                "for this account, then sign in again. This does not delete "
                "server messages or Element data."));
        });
        connect(rust, &RustSdkMatrixClient::localSessionCleanupFinished,
                this, [this](bool ok, const QString &message) {
            setLocalRustResetRequired(!ok);
            if (m_resetResultPending) {
                m_resetResultPending = false;
                Q_EMIT localRustStoreResetResult(ok, message);
            }
        });
    }
#endif

    // Session restore is only meaningful for backends that can actually talk
    // to a homeserver. The mock backend synthesizes its own state.
    if ((m_backend == HttpBackend || m_backend == RustBackend)
        && m_settings->hasSession()) {
        m_client->restoreSession();
        Q_EMIT rustDeviceIdChanged();
    }
}

AppController::~AppController() = default;

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

SettingsManager *AppController::settings() const { return m_settings.get(); }
AuthManager *AppController::auth() const { return m_auth.get(); }
AccountManager *AppController::accounts() const { return m_accounts.get(); }
RoomListModel *AppController::roomList() const { return m_roomList.get(); }
QuickSwitcherModel *AppController::quickSwitcher() const
{ return m_quickSwitcher.get(); }
TimelineModel *AppController::timeline() const { return m_timeline.get(); }
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
    m_timeline->setRoomId(roomId);
    m_composer->setRoomId(roomId);
    m_pagination->setRoomId(roomId);
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
    if (m_backend != RustBackend || !m_client || m_verificationFlowId.isEmpty())
        return;
    if (auto *rust = qobject_cast<RustSdkMatrixClient *>(m_client.get()))
        rust->cancelVerification(m_verificationFlowId);
    m_verificationFlowId.clear();
    m_verificationState.clear();
    m_verificationEmojis.clear();
    m_verificationDecimals.clear();
    Q_EMIT verificationStateChanged();
#endif
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
    setLocalRustResetRequired(!ok);
    if (ok) {
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

void AppController::setLocalRustResetRequired(bool required)
{
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
                  << "(0=Login, 1=Main, 2=Settings)";
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
    m_accounts->setActiveUser(uid);
    setLocalRustResetRequired(false);
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
    Q_EMIT currentRoomIdChanged();
    if (m_accountSwitching) {
        // The old session was detached locally as part of a switch; stay on
        // the main screen while the target account activates.
        return;
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
        // Real (server) logout. Local store/record cleanup and switching to
        // a remaining account follow from the logout flow.
        m_auth->logout();
        return;
    }

    // Background (or signed-out) account: delete its local state without
    // touching the active session.
    const QString hs = m_settings->accountRecord(target)
                           .value(QStringLiteral("homeserver")).toString();
    matrix::app_data::AccountIdentity identity;
    if (matrix::app_data::resolveAccountIdentity(hs, target, &identity)) {
        const auto removed = matrix::app_data::removeAccountRustState(identity);
        QDir accountDir(identity.accountRoot);
        if (accountDir.exists())
            accountDir.removeRecursively();
        qCInfo(lcApp) << "removed account local state"
                      << "slug=" << identity.slug
                      << "rust_deleted=" << removed.deleted
                      << "failed=" << removed.failed;
    }
    m_accounts->removeAccount(target); // record + secrets
    if (isActive)
        m_lastSessionUserId.clear();
}
